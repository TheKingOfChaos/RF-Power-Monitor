"""Mains soak test: cut the device's mains input at random for days and check every outage.

Not a unittest suite (it runs for days): start it, leave it, read the summary afterwards.

    python test/hil/mains_soak.py                      # 7 days, host and community from the serial menu
    python test/hil/mains_soak.py --days 0.05 --host 10.132.0.77 --community rfit

Needs the Dirigera outlet (RFPOWER_MAINS_SWITCH=dirigera and its variables, see mains_switch.py)
and net-snmp in WSL. The outlet only feeds the mains detector, so the device itself keeps running.

Each cut has a random length (a blip of about a second, short, medium, over the 60 s alarm,
or 10-30 minutes) and a random gap before it. Per cut the script checks over SNMP:
the loss is seen, mainsPowerAlarm stays 0 before 60 s and is raised for cuts over 75 s,
the restore is seen and clears the alarm, mainsPowerChanges grows by exactly 2 and sysUpTime
did not go back (no restart). Between cuts it polls every minute and watches for restarts,
mains changes nobody made, SNMP answers and the web page.

Results go to soak/<start time>/: events.csv (one line per cut or problem), soak.log,
summary.txt and report.html (open it in a browser), both rewritten after every cut so they are
current even if the PC stops; --report <folder> rebuilds report.html from events.csv. The outlet is
always switched back on when the script ends, also on Ctrl+C or an error.
"""

import argparse
import csv
import datetime
import os
import random
import re
import subprocess
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)

ENTERPRISE = "1.3.6.1.4.1.63050"
SYS_UPTIME = "1.3.6.1.2.1.1.3.0"
MAINS_OIDS = (ENTERPRISE + ".1.0", ENTERPRISE + ".2.0", ENTERPRISE + ".6.0", SYS_UPTIME)

# (kind, weight, shortest s, longest s); a blip switches back on at once (about 1 s off)
CUT_KINDS = (
    ("blip", 15, 0, 0),
    ("short", 25, 2, 10),
    ("medium", 30, 10, 55),
    ("long", 25, 65, 300),
    ("very long", 5, 600, 1800),
)
ALARM_AFTER_S = 60      # mainsPowerAlarm is raised after 60 s without mains
ALARM_EXPECTED_S = 75   # cuts at least this long must show the alarm
LOSS_REQUIRED_S = 5     # cuts at least this long must show mainsPowerStatus = 0
DETECT_LIMIT_S = 5      # loss and restore must show within this time after the hub confirms
IDLE_POLL_S = 60
WEB_EVERY_S = 600


# ---- pure parts (tested in test/tools/test_mains_soak.py) ----

def plan_cut(rng):
    """Pick the next cut: (kind, seconds off)."""
    total = sum(w for _, w, _, _ in CUT_KINDS)
    pick = rng.uniform(0, total)
    for kind, weight, low, high in CUT_KINDS:
        if pick < weight:
            return kind, round(rng.uniform(low, high))
        pick -= weight
    kind, _, low, high = CUT_KINDS[-1]
    return kind, round(rng.uniform(low, high))


def plan_gap(rng, min_minutes, max_minutes):
    """Seconds of normal mains before the next cut."""
    return rng.uniform(min_minutes, max_minutes) * 60


def parse_mains(output):
    """snmpget -On output for MAINS_OIDS -> dict(status, changes, alarm, uptime), or None."""
    values = {}
    for name, pattern in (("status", r"63050\.1\.0 = INTEGER: (\d+)"),
                          ("changes", r"63050\.2\.0 = Counter32: (\d+)"),
                          ("alarm", r"63050\.6\.0 = INTEGER: (\d+)"),
                          ("uptime", r"1\.3\.6\.1\.2\.1\.1\.3\.0 = Timeticks: \((\d+)\)")):
        m = re.search(pattern, output)
        if not m:
            return None
        values[name] = int(m.group(1))
    return values


def check_cut(seconds, before, loss_after, alarm_samples, restore_after, after):
    """Problems with one cut (empty list = good).

    seconds        how long mains was off (from the hub confirming off to switching on)
    before, after  parse_mains() values just before the cut and after the restore settled
    loss_after     seconds from the hub confirming off to mainsPowerStatus = 0, None if never seen
    alarm_samples  [(seconds since off, mainsPowerAlarm)] read while mains was off
    restore_after  seconds from the hub confirming on to mainsPowerStatus = 1, None if never seen
    """
    problems = []
    if before["status"] != 1 or before["alarm"] != 0:
        problems.append("not idle before the cut (status %d, alarm %d)" % (before["status"], before["alarm"]))
    if after["uptime"] < before["uptime"]:
        problems.append("device restarted (sysUpTime %d -> %d)" % (before["uptime"], after["uptime"]))
    elif after["changes"] - before["changes"] != 2:
        problems.append("mainsPowerChanges grew by %d, expected 2" % (after["changes"] - before["changes"]))
    if loss_after is None:
        if seconds >= LOSS_REQUIRED_S:
            problems.append("loss never seen in mainsPowerStatus")
    elif loss_after > DETECT_LIMIT_S:
        problems.append("loss seen after %.1f s" % loss_after)
    early = [t for t, alarm in alarm_samples if alarm and t < ALARM_AFTER_S - 1]
    if early:
        problems.append("alarm raised after %.0f s, before %d s" % (early[0], ALARM_AFTER_S))
    if seconds >= ALARM_EXPECTED_S and not any(a for t, a in alarm_samples if t <= ALARM_EXPECTED_S):
        problems.append("alarm not raised within %d s" % ALARM_EXPECTED_S)
    if restore_after is None:
        problems.append("restore never seen in mainsPowerStatus")
    elif restore_after > DETECT_LIMIT_S:
        problems.append("restore seen after %.1f s" % restore_after)
    if after["status"] != 1 or after["alarm"] != 0:
        problems.append("not clean after restore (status %d, alarm %d)" % (after["status"], after["alarm"]))
    return problems


def check_idle(previous, current, cut_made):
    """Problems between two idle polls (no cut in between unless cut_made)."""
    problems = []
    if current["uptime"] < previous["uptime"]:
        problems.append("device restarted (sysUpTime %d -> %d)" % (previous["uptime"], current["uptime"]))
    elif not cut_made and current["changes"] != previous["changes"]:
        problems.append("mains changed %d times without a cut" % (current["changes"] - previous["changes"]))
    if current["status"] != 1 or current["alarm"] != 0:
        problems.append("mains not normal while the outlet is on (status %d, alarm %d)"
                        % (current["status"], current["alarm"]))
    return problems


class Stats:
    """Counters for summary.txt."""

    def __init__(self, started):
        self.started = started
        self.cuts = {kind: 0 for kind, _, _, _ in CUT_KINDS}
        self.bad_cuts = 0
        self.problems = []          # (time, text), every problem seen
        self.restarts = 0
        self.snmp_failures = 0
        self.web_checks = 0
        self.web_failures = 0
        self.hub_errors = 0
        self.loss_times = []
        self.restore_times = []

    def problem(self, when, text):
        self.problems.append((when, text))
        if "restarted" in text:
            self.restarts += 1

    def text(self, now, finished=False):
        cuts = sum(self.cuts.values())
        lines = [
            "Mains soak test, %s" % ("finished" if finished else "running"),
            "Started  %s" % self.started.strftime("%Y-%m-%d %H:%M"),
            "Updated  %s (%.1f days)" % (now.strftime("%Y-%m-%d %H:%M"),
                                          (now - self.started).total_seconds() / 86400),
            "",
            "Cuts: %d, with problems: %d" % (cuts, self.bad_cuts),
            "  " + ", ".join("%s %d" % (k, n) for k, n in self.cuts.items()),
            "Device restarts: %d" % self.restarts,
            "SNMP polls without answer: %d" % self.snmp_failures,
            "Web page checks: %d, failed: %d" % (self.web_checks, self.web_failures),
            "Dirigera hub errors (PC/hub side, not the device): %d" % self.hub_errors,
        ]
        for name, values in (("Loss seen after", self.loss_times), ("Restore seen after", self.restore_times)):
            if values:
                lines.append("%s: median %.1f s, max %.1f s (SNMP poll, includes WSL start)"
                             % (name, sorted(values)[len(values) // 2], max(values)))
        lines.append("")
        lines.append("RESULT: " + ("PASS, no problems" if not self.problems else
                                   "%d problems, see below and events.csv" % len(self.problems)))
        for when, text in self.problems[-50:]:
            lines.append("  %s  %s" % (when.strftime("%Y-%m-%d %H:%M:%S"), text))
        return "\n".join(lines) + "\n"


def _median(values):
    return sorted(values)[len(values) // 2] if values else None


def _fmt_s(value):
    return "-" if value is None else "%.1f s" % value


def build_report(rows, summary_text="", finished=False):
    """report.html from the events.csv rows (dicts) and summary.txt; one self-contained page."""
    from html import escape
    cuts = [r for r in rows if r["event"] == "cut"]
    problems = [r for r in rows if r["result"] != "ok"]
    bad_cuts = [r for r in cuts if r["result"] != "ok"]
    standalone = [r for r in rows if r["event"] == "problem"]
    start = rows[0]["time"] if rows else "-"
    end = rows[-1]["time"] if rows else "-"
    uptimes = [int(r["uptime_s"]) for r in rows if r["uptime_s"]]
    passed = not problems

    def floats(rs, key):
        return [float(r[key]) for r in rs if r[key]]

    kind_rows = []
    for kind, _, low, high in CUT_KINDS:
        rs = [r for r in cuts if r["kind"] == kind]
        loss, restore = floats(rs, "loss_after_s"), floats(rs, "restore_after_s")
        span = "about 1 s" if high == 0 else "%d-%d s" % (low, high)
        kind_rows.append("<tr><td>%s</td><td>%s</td><td>%d</td><td>%d</td><td>%s / %s</td><td>%s / %s</td></tr>" % (
            kind, span, len(rs), sum(1 for r in rs if r["result"] != "ok"),
            _fmt_s(_median(loss)), _fmt_s(max(loss) if loss else None),
            _fmt_s(_median(restore)), _fmt_s(max(restore) if restore else None)))

    days = {}
    for r in rows:
        day = days.setdefault(r["time"][:10], {"cuts": 0, "off": 0.0, "problems": 0})
        if r["event"] == "cut":
            day["cuts"] += 1
            day["off"] += float(r["seconds_off"] or 0)
        if r["result"] != "ok":
            day["problems"] += 1
    most = max([d["cuts"] for d in days.values()] or [1]) or 1
    day_rows = "".join(
        "<tr><td>%s</td><td>%d</td><td><span class=bar style='width:%dpx'></span></td><td>%.0f min</td>"
        "<td class=%s>%d</td></tr>" % (day, d["cuts"], 4 + 200 * d["cuts"] // most, d["off"] / 60,
                                       "bad" if d["problems"] else "", d["problems"])
        for day, d in sorted(days.items()))

    problem_rows = "".join(
        "<tr><td>%s</td><td>%s</td><td>%s</td><td>%s</td></tr>" % (
            escape(r["time"]), escape(r["event"] + (" " + r["kind"] if r["kind"] else "")),
            escape(r["seconds_off"] and r["seconds_off"] + " s"), escape(r["result"]))
        for r in problems) or "<tr><td colspan=4>None</td></tr>"

    longest = max(cuts, key=lambda r: float(r["seconds_off"] or 0), default=None)
    facts = [
        ("Run", "%s to %s%s" % (start, end, "" if finished else " (still running)")),
        ("Cuts", "%d, of which %d with problems" % (len(cuts), len(bad_cuts))),
        ("Mains off in total", "%.1f hours" % (sum(floats(cuts, "seconds_off")) / 3600)),
        ("Longest cut", "%s s (%s)" % (longest["seconds_off"], longest["kind"]) if longest else "-"),
        ("Device uptime at the end", "%.1f days" % (uptimes[-1] / 86400) if uptimes else "-"),
        ("Other problems (between cuts)", str(len(standalone))),
    ]
    fact_rows = "".join("<tr><th>%s</th><td>%s</td></tr>" % (k, escape(v)) for k, v in facts)
    return """<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Mains Soak Report</title>
<style>
:root{--bg:#fff;--fg:#1d2329;--muted:#5d6874;--line:#d9dee3;--ok:#1a7f37;--bad:#c62828;--bar:#3b6ea5}
@media (prefers-color-scheme:dark){:root{--bg:#15191d;--fg:#e4e8ec;--muted:#9aa5b1;--line:#333b43;--ok:#4cc26a;--bad:#ef6b6b;--bar:#6c9bd2}}
body{background:var(--bg);color:var(--fg);font:15px/1.5 system-ui,sans-serif;margin:0 auto;max-width:900px;padding:16px}
h1{margin:0 0 4px}h2{margin-top:32px;border-bottom:1px solid var(--line);padding-bottom:4px}
.result{font-size:20px;font-weight:600;padding:12px 16px;border-radius:8px;border:2px solid}
.pass{color:var(--ok);border-color:var(--ok)}.fail{color:var(--bad);border-color:var(--bad)}
table{border-collapse:collapse;width:100%%;margin:8px 0}td,th{text-align:left;padding:5px 8px;border-bottom:1px solid var(--line);vertical-align:top}
th{color:var(--muted);font-weight:500}.bad{color:var(--bad);font-weight:600}
.bar{display:inline-block;height:10px;background:var(--bar);border-radius:2px}
.wrap{overflow-x:auto}pre{white-space:pre-wrap;color:var(--muted);font-size:13px}p.note{color:var(--muted)}
</style></head><body>
<h1>Mains soak report</h1>
<p class=note>RF-Power-Monitor, mains input cut at random through the Dirigera outlet and checked over SNMP.</p>
<div class="result %s">%s</div>
<h2>Overview</h2><table>%s</table>
<h2>Cuts by length</h2>
<p class=note>Detection times are median / max, measured by SNMP polling from the PC (each poll starts net-snmp in WSL),
so they are upper bounds. A cut passes when the loss and restore are seen within %d s, the alarm stays off before
%d s and is raised by %d s, the change counter grows by exactly 2 and the device did not restart.</p>
<div class=wrap><table><tr><th>Kind</th><th>Length</th><th>Cuts</th><th>Problems</th><th>Loss seen</th><th>Restore seen</th></tr>%s</table></div>
<h2>Day by day</h2>
<div class=wrap><table><tr><th>Day</th><th>Cuts</th><th></th><th>Mains off</th><th>Problems</th></tr>%s</table></div>
<h2>Problems</h2>
<div class=wrap><table><tr><th>Time</th><th>Event</th><th>Off</th><th>What</th></tr>%s</table></div>
<h2>Summary file</h2><pre>%s</pre>
</body></html>
""" % ("pass" if passed else "fail",
       ("PASS: %d cuts, no problems" % len(cuts)) if passed else
       ("%d problems in %d cuts" % (len(problems), len(cuts))),
       fact_rows, DETECT_LIMIT_S, ALARM_AFTER_S, ALARM_EXPECTED_S,
       "".join(kind_rows), day_rows, problem_rows, escape(summary_text))


def write_report(out_dir, finished=False):
    """Rebuild report.html in out_dir from its events.csv and summary.txt."""
    with open(os.path.join(out_dir, "events.csv"), newline="", encoding="utf-8") as f:
        rows = list(csv.DictReader(f))
    summary_path = os.path.join(out_dir, "summary.txt")
    summary = open(summary_path, encoding="utf-8").read() if os.path.exists(summary_path) else ""
    path = os.path.join(out_dir, "report.html")
    with open(path, "w", encoding="utf-8") as f:
        f.write(build_report(rows, summary, finished))
    return path


# ---- hardware ----

class Soak:
    """One soak run against a device."""

    def __init__(self, host, community, switch, out_dir, rng):
        from snmp_tools import net_snmp
        self.net_snmp = net_snmp
        self.host = host
        self.community = community
        self.switch = switch
        self.rng = rng
        self.out_dir = out_dir
        os.makedirs(out_dir, exist_ok=True)
        self.stats = Stats(datetime.datetime.now())
        self.log_file = open(os.path.join(out_dir, "soak.log"), "a", encoding="utf-8")
        new = not os.path.exists(os.path.join(out_dir, "events.csv"))
        self.csv_file = open(os.path.join(out_dir, "events.csv"), "a", newline="", encoding="utf-8")
        self.csv = csv.writer(self.csv_file)
        if new:
            self.csv.writerow(["time", "event", "kind", "seconds_off", "loss_after_s", "restore_after_s",
                               "changes", "uptime_s", "result"])

    def log(self, text):
        line = "%s  %s" % (datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"), text)
        print(line, flush=True)
        self.log_file.write(line + "\n")
        self.log_file.flush()

    def event(self, event, kind="", seconds="", loss="", restore="", values=None, result="ok"):
        self.csv.writerow([datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"), event, kind, seconds,
                           "" if loss in ("", None) else "%.1f" % loss,
                           "" if restore in ("", None) else "%.1f" % restore,
                           values["changes"] if values else "", values["uptime"] // 100 if values else "",
                           result])
        self.csv_file.flush()

    def problem(self, text, values=None):
        self.log("PROBLEM: " + text)
        self.stats.problem(datetime.datetime.now(), text)
        self.event("problem", values=values, result=text)
        self.write_summary()

    def write_summary(self, finished=False):
        with open(os.path.join(self.out_dir, "summary.txt"), "w", encoding="utf-8") as f:
            f.write(self.stats.text(datetime.datetime.now(), finished))
        write_report(self.out_dir, finished)

    def read(self, retries=2):
        """Mains values over SNMP, or None after retries (counted)."""
        for _ in range(retries + 1):
            try:
                rc, out = self.net_snmp("snmpget", self.host, *MAINS_OIDS, community=self.community,
                                        version="2c", timeout=2, retries=0)
            except subprocess.TimeoutExpired:
                rc, out = 1, ""
            values = parse_mains(out) if rc == 0 else None
            if values:
                return values
            time.sleep(1)
        self.stats.snmp_failures += 1
        return None

    def read_until_answer(self, what):
        """Keep reading until the device answers; report how long it was silent."""
        values = self.read()
        if values:
            return values
        silent_since = time.time()
        self.log("no SNMP answer (%s), %s" % (what, "ping ok" if self.ping() else "ping fails too"))
        while values is None:
            time.sleep(10)
            values = self.read()
        self.problem("no SNMP answer for %.0f s (%s)" % (time.time() - silent_since, what), values)
        return values

    def ping(self):
        flag = "-n" if sys.platform == "win32" else "-c"
        return subprocess.run(["ping", flag, "2", self.host], capture_output=True).returncode == 0

    def check_web(self):
        self.stats.web_checks += 1
        try:
            with urllib.request.urlopen("http://%s/" % self.host, timeout=10) as r:
                ok = r.status == 200
        except Exception:
            ok = False
        if not ok:
            self.stats.web_failures += 1
            self.log("web page did not answer")

    def hub(self, action):
        """Switch the outlet, retrying; 'on' never gives up so mains always comes back."""
        attempt = 0
        while True:
            try:
                getattr(self.switch, action)()
                return time.time()
            except Exception as e:
                attempt += 1
                self.stats.hub_errors += 1
                self.log("Dirigera %s failed (attempt %d): %s" % (action, attempt, e))
                if action == "off" and attempt >= 3:
                    return None
                time.sleep(min(30 * attempt, 300))

    def wait_status(self, expected, since, limit):
        """Poll until mainsPowerStatus == expected; seconds after `since`, or None."""
        while time.time() - since < limit:
            values = self.read(retries=0)
            if values and values["status"] == expected:
                return time.time() - since
            time.sleep(0.3)
        return None

    def cut(self, kind, seconds):
        before = self.read_until_answer("before a cut")
        if before["status"] != 1:
            self.problem("mains not present before a cut, skipped", before)
            return before
        off_at = self.hub("off")
        if off_at is None:
            self.log("cut skipped, the hub did not switch off")
            return before
        loss_after, samples = None, []
        while True:
            elapsed = time.time() - off_at
            if elapsed >= seconds:
                break
            values = self.read(retries=0)
            elapsed = time.time() - off_at
            if values:
                if loss_after is None and values["status"] == 0:
                    loss_after = elapsed
                samples.append((elapsed, values["alarm"]))
            time.sleep(0.5 if elapsed < ALARM_EXPECTED_S + 5 else 5)
        on_at = self.hub("on")
        seconds_off = on_at - off_at    # until the hub confirms on: a blip is about a second
        restore_after = self.wait_status(1, on_at, 30)
        time.sleep(2)
        after = self.read_until_answer("after a cut")
        problems = check_cut(seconds_off, before, loss_after, samples, restore_after, after)
        self.stats.cuts[kind] += 1
        if loss_after is not None:
            self.stats.loss_times.append(loss_after)
        if restore_after is not None:
            self.stats.restore_times.append(restore_after)
        result = "; ".join(problems) or "ok"
        self.event("cut", kind, "%.0f" % seconds_off, loss_after, restore_after, after, result)
        self.log("cut %-9s %5.0f s  loss %s  restore %s  %s" % (
            kind, seconds_off, "-" if loss_after is None else "%.1f s" % loss_after,
            "-" if restore_after is None else "%.1f s" % restore_after, result))
        if problems:
            self.stats.bad_cuts += 1
            for p in problems:
                self.stats.problem(datetime.datetime.now(), "%s cut of %.0f s: %s" % (kind, seconds_off, p))
        self.write_summary()
        return after

    def idle(self, seconds, previous):
        """Normal mains for `seconds`, polling; returns the last values."""
        end = time.time() + seconds
        last_web = 0
        while True:
            values = self.read_until_answer("idle poll")
            for p in check_idle(previous, values, cut_made=False):
                self.problem(p, values)
            previous = values
            if time.time() - last_web >= WEB_EVERY_S:
                self.check_web()
                last_web = time.time()
            left = end - time.time()
            if left <= 0:
                return previous
            time.sleep(min(IDLE_POLL_S, left))

    def run(self, days, min_gap, max_gap):
        end = time.time() + days * 86400
        self.log("soak start: %s, %.2f days, gaps %g-%g min, output %s"
                 % (self.host, days, min_gap, max_gap, self.out_dir))
        self.hub("on")
        values = self.read_until_answer("start")
        if values["status"] != 1 or values["alarm"] != 0:
            raise SystemExit("mains must be present and no alarm at the start (status %d, alarm %d)"
                             % (values["status"], values["alarm"]))
        self.write_summary()
        last_summary = time.time()
        while time.time() < end:
            gap = min(plan_gap(self.rng, min_gap, max_gap), max(0, end - time.time()))
            values = self.idle(gap, values)
            if time.time() >= end:
                break
            kind, seconds = plan_cut(self.rng)
            values = self.cut(kind, min(seconds, max(0, end - time.time())))
            if time.time() - last_summary > 6 * 3600:
                self.log("so far: %d cuts, %d problems" % (sum(self.stats.cuts.values()),
                                                            len(self.stats.problems)))
                last_summary = time.time()


def keep_pc_awake():
    """Stop Windows from sleeping while the script runs (released when the process ends)."""
    if sys.platform == "win32":
        import ctypes
        ES_CONTINUOUS, ES_SYSTEM_REQUIRED = 0x80000000, 0x00000001
        ctypes.windll.kernel32.SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED)


def device_settings():
    """IP address and community from the serial menu (closes the port again)."""
    from device import Device
    dev = Device()
    dev.open()
    try:
        status = dev.status()
        return status["ip"], status["community"]
    finally:
        dev.close()


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--days", type=float, default=7.0, help="how long to run (default 7)")
    p.add_argument("--host", help="device address (default: read from the serial menu)")
    p.add_argument("--community", help="SNMP community (default: read from the serial menu)")
    p.add_argument("--min-gap", type=float, default=5, help="shortest gap between cuts, minutes (default 5)")
    p.add_argument("--max-gap", type=float, default=45, help="longest gap between cuts, minutes (default 45)")
    p.add_argument("--seed", type=int, help="random seed, to repeat a run")
    p.add_argument("--out", help="output folder (default soak/<start time>)")
    p.add_argument("--report", metavar="FOLDER", help="only rebuild FOLDER/report.html from its events.csv")
    a = p.parse_args()

    if a.report:
        print(write_report(a.report, finished=True))
        return
    from mains_switch import get_switch
    switch = get_switch()
    if switch is None or not switch.automatic:
        raise SystemExit("set RFPOWER_MAINS_SWITCH=dirigera and its variables (see test/hil/mains_switch.py)")
    host, community = a.host, a.community
    if not host or not community:
        serial_host, serial_community = device_settings()
        host, community = host or serial_host, community or serial_community
    seed = a.seed if a.seed is not None else random.randrange(1 << 30)
    out = a.out or os.path.join(ROOT, "soak", datetime.datetime.now().strftime("%Y-%m-%d_%H%M"))
    soak = Soak(host, community, switch, out, random.Random(seed))
    soak.log("seed %d (repeat with --seed %d)" % (seed, seed))
    keep_pc_awake()
    try:
        soak.run(a.days, a.min_gap, a.max_gap)
        soak.log("soak finished")
    except KeyboardInterrupt:
        soak.log("stopped with Ctrl+C")
    finally:
        soak.hub("on")
        soak.write_summary(finished=True)
        soak.log("outlet on; summary in %s" % os.path.join(out, "summary.txt"))
        print(soak.stats.text(datetime.datetime.now(), finished=True))


if __name__ == "__main__":
    main()
