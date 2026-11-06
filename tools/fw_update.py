"""Update the firmware of one or more RF-Power-Monitors over the web interface.

    python tools/fw_update.py <file.upd> <host>... [--hosts-file F] [--continue] [--timeout-min 15]

The password of the web user "admin" comes from the environment variable
RFPOWER_WEB_PASSWORD, or is asked for (getpass). It is never an argument and
never printed. Per host: log in, read /firmware/status, compare the file's
signing key id with the device's key (a different key is refused before the
upload), upload, wait for the restart, log in again and poll until the new
firmware runs in the other slot and is "kept" (the device keeps it once it has
been reachable on the network). One line per host:

    <host>: updated | rejected: <reason> | rolled back: <last_result> | not installed: ...
            | unreachable | timeout

"rejected: no reply (<reason>)" means the device refused the file but its
answer was lost; "not installed" that it restarted on its old firmware without
a rollback (for example a power cut before the new slot was recorded).

The script stops at the first host that is not updated unless --continue is
given; the exit code is 0 only when every host is updated. Standard library
only (http.client).
"""

import argparse
import getpass
import http.client
import os
import re
import sys
import time
from urllib.parse import urlencode, urlsplit

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fw_release  # noqa: E402  (.upd layout helpers, shared with the release script)

SLOT_KEY_ID_OFFSET = 96  # key id inside a slot header, see lib/fw_image/include/fw_image.h
SLOT_HEADER_SIZE = 256


class Outcome(Exception):
    """A host's result other than "updated": `text` is the line printed after the host name."""

    def __init__(self, text):
        super().__init__(text)
        self.text = text


def upd_info(data: bytes) -> dict:
    """Version and key id (16 hex characters, the device's `key=`) of a .upd file.

    Raises ValueError when the file is not a valid .upd.
    """
    head = fw_release.parse_upd_header(data)
    off, length = head["slotOffset"][0], head["slotLength"][0]
    if length < SLOT_HEADER_SIZE or off + length > len(data):
        raise ValueError("slot A of the update file is truncated")
    if head["slotOffset"][1] + head["slotLength"][1] > len(data):
        raise ValueError("slot B of the update file is truncated")
    key = data[off + SLOT_KEY_ID_OFFSET:off + SLOT_KEY_ID_OFFSET + 8]
    return {"version": head["version"], "key": key.hex()}


def parse_status(text: str) -> dict:
    """The key=value lines of /firmware/status as a dict."""
    out = {}
    for line in text.splitlines():
        k, sep, v = line.partition("=")
        if sep:
            out[k.strip()] = v.strip()
    return out


class Device:
    """One device's web interface: a session (cookie) over short HTTP/1.1 connections."""

    def __init__(self, host, password, http_timeout):
        self.host = host
        self.password = password
        self.http_timeout = http_timeout
        self.cookie = None
        parts = urlsplit("//" + host)
        self.addr = parts.hostname
        self.port = parts.port or 80

    def request(self, method, path, body=None, headers=None):
        """(status, headers, body text); OSError / http.client.HTTPException when unreachable."""
        conn = http.client.HTTPConnection(self.addr, self.port, timeout=self.http_timeout)
        try:
            h = dict(headers or {})
            if self.cookie:
                h["Cookie"] = self.cookie
            conn.request(method, path, body=body, headers=h)
            r = conn.getresponse()
            data = r.read().decode("utf-8", "replace")
            return r.status, r, data
        finally:
            conn.close()

    def login(self):
        """Log in; raises Outcome("rejected: ...") on a refusal, OSError when unreachable."""
        body = urlencode({"user": "admin", "password": self.password})
        status, resp, text = self.request("POST", "/login", body,
                                          {"Content-Type": "application/x-www-form-urlencoded"})
        cookie = resp.getheader("Set-Cookie") or ""
        m = re.match(r"(sid=[^;]+)", cookie)
        if status in (301, 302, 303) and m:
            self.cookie = m.group(1)
            return
        if status == 401:
            raise Outcome("rejected: wrong password")
        if status == 429:
            raise Outcome("rejected: too many failed logins, try later")
        raise Outcome("rejected: login failed (HTTP %d)" % status)

    def status(self):
        """The parsed /firmware/status; raises OSError when unreachable."""
        code, _, text = self.request("GET", "/firmware/status")
        st = parse_status(text) if code == 200 else {}
        if "state" not in st:
            raise OSError("no firmware status (HTTP %d)" % code)
        return st


def _is_down(exc):
    return isinstance(exc, (OSError, http.client.HTTPException))


def update_host(host, upd, info, password, timeout_s, poll_s, http_timeout, sleep=time.sleep):
    """Update one host; returns normally when updated, raises Outcome otherwise."""
    dev = Device(host, password, http_timeout)
    try:
        dev.login()
        before = dev.status()
    except Outcome:
        raise
    except (OSError, http.client.HTTPException):
        raise Outcome("unreachable")
    if before.get("key", "").lower() != info["key"]:
        raise Outcome("rejected: different key (device %s, file %s)" % (before.get("key", "?"), info["key"]))
    try:
        _, _, page = dev.request("GET", "/firmware")
        m = re.search(r'name="csrf" value="([0-9a-fA-F]+)"', page)
        if not m:
            raise Outcome("rejected: no CSRF token on the firmware page")
        code, _, text = dev.request("POST", "/firmware/upload", upd,
                                    {"Content-Type": "application/octet-stream", "X-CSRF-Token": m.group(1)})
    except Outcome:
        raise
    except (OSError, http.client.HTTPException):
        code, text = None, ""  # the device may have restarted before it answered: wait and see
    if code is not None and code != 200:
        raise Outcome("rejected: %s" % (text.strip() or "HTTP %d" % code))

    # Wait for the restart and the trial. "Went down" = a request failed after the upload.
    before_slot = before.get("running_slot")
    deadline = time.monotonic() + timeout_s
    went_down = code is None
    while time.monotonic() < deadline:
        sleep(poll_s)
        try:
            if not dev.cookie or went_down:
                dev.cookie = None
                dev.login()  # the session did not survive the restart
            st = dev.status()
        except Outcome as e:
            if "wrong password" in e.text:
                raise
            went_down = True
            continue
        except (OSError, http.client.HTTPException):
            went_down = True
            dev.cookie = None
            continue
        verdict = _judge(st, before_slot, info["version"], went_down)
        if verdict == "updated":
            return
        if verdict is not None:
            raise Outcome(verdict)
        # still the old firmware before it restarts, or the new one on trial: keep waiting
    raise Outcome("timeout")


def _judge(st, before_slot, version, went_down):
    """What a status read after the upload means: "updated", another outcome line, or None (wait).

    An update always lands in the other slot, so the new firmware runs only once running_slot
    differs from the slot before the upload (a file of the running version reads "kept" with
    that version before the restart too).
    """
    last = st.get("last_result", "")
    kept = st.get("state") == "kept"
    if st.get("running_slot") != before_slot:
        if kept and st.get("running_version") == version:
            return "updated"
        return None  # the new firmware on trial
    # The slot it ran before the upload
    if last.startswith("rejected"):
        # The refusal's own answer was lost; the device holds the reason until it restarts
        return "rejected: no reply (%s)" % last.partition(":")[2].strip()
    if last.startswith("verified"):
        return None  # accepted, the restart is still to come
    if went_down and kept and last.startswith(("rolled back", "fallback")):
        return "rolled back: %s" % last
    if went_down and kept:
        return "not installed: back on the old firmware (last update: %s)" % (last or "none")
    return None


def read_hosts(args):
    """Hosts from the command line, then the hosts file (one per line, # comments)."""
    hosts = list(args.hosts)
    if args.hosts_file:
        with open(args.hosts_file, encoding="utf-8") as f:
            for line in f:
                line = line.split("#", 1)[0].strip()
                if line:
                    hosts.append(line)
    return hosts


def main(argv=None, environ=None):
    """Command line entry; returns the exit code."""
    environ = os.environ if environ is None else environ
    ap = argparse.ArgumentParser(description="Update RF-Power-Monitor firmware over the web interface.")
    ap.add_argument("upd", help="the .upd file from tools/fw_release.py")
    ap.add_argument("hosts", nargs="*", help="device addresses (host or host:port)")
    ap.add_argument("--hosts-file", help="file with one host per line")
    ap.add_argument("--continue", dest="cont", action="store_true", help="go on with the next host after a failure")
    ap.add_argument("--timeout-min", type=float, default=15.0, help="minutes to wait for the new firmware (15)")
    ap.add_argument("--poll-s", type=float, default=5.0, help=argparse.SUPPRESS)
    ap.add_argument("--http-timeout-s", type=float, default=15.0, help=argparse.SUPPRESS)
    args = ap.parse_args(argv)

    hosts = read_hosts(args)
    if not hosts:
        ap.error("give at least one host or --hosts-file")
    try:
        with open(args.upd, "rb") as f:
            upd = f.read()
        info = upd_info(upd)
    except (OSError, ValueError) as e:
        print("%s: %s" % (args.upd, e), file=sys.stderr)
        return 2
    password = environ.get("RFPOWER_WEB_PASSWORD")
    if password is None:
        password = getpass.getpass("Web password for admin: ")

    failed = False
    for host in hosts:
        try:
            update_host(host, upd, info, password, args.timeout_min * 60, args.poll_s, args.http_timeout_s)
            print("%s: updated" % host, flush=True)
        except Outcome as e:
            print("%s: %s" % (host, e.text), flush=True)
            failed = True
            if not args.cont:
                break
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
