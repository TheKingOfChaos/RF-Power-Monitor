"""Tests for tools/fw_update.py against a fake device (http.server in a thread).

The fake mimics the device's login, /firmware, /firmware/status and
/firmware/upload, including a restart (connections dropped for a while) and
the trial that follows. No hardware. Timeouts and the poll interval are
passed on the command line, so the suite runs in seconds.
Run: python -m unittest discover -s test/tools -v
"""

import contextlib
import http.server
import io
import os
import socket
import subprocess
import sys
import threading
import unittest
from urllib.parse import parse_qs

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fw_update  # noqa: E402
import golden_upd  # noqa: E402

PASSWORD = "s3cret-Pw-q7"
FAST = ["--timeout-min", "0.03", "--poll-s", "0.02", "--http-timeout-s", "1"]

UPD = None
KEY_ID = None
VERSION = golden_upd.VERSION


def setUpModule():
    global UPD, KEY_ID
    UPD, key_id = golden_upd.build()
    KEY_ID = key_id.hex()


class FakeDevice:
    """One device on 127.0.0.1, running slot A before the upload; the new firmware runs in slot B.

    `after_upload` is "kept", "rolled_back" (back on slot A), "old_firmware" (back on slot A without
    a rollback record) or "dead". `restart_after_polls`: status requests answered by the old
    firmware ("verified ..., restarting on trial") before the restart. `upload_drop`: the upload's
    reply is lost (connection closed); with `reject_reason` the device refused the file and keeps
    running (last_result "rejected: <reason>").
    """

    def __init__(self, key=None, after_upload="kept", old_version="3.3.0", upload_status=200,
                 upload_text="ok 3.4.0", restart_after_polls=0, upload_drop=False, reject_reason=None):
        self.key = key or KEY_ID
        self.after_upload = after_upload
        self.old_version = old_version
        self.upload_status = upload_status
        self.upload_text = upload_text
        self.restart_after_polls = restart_after_polls
        self.upload_drop = upload_drop
        self.reject_reason = reject_reason
        self.pending_polls = 0
        self.requests = []          # (method, path)
        self.login_passwords = []
        self.uploads = []           # (body length, csrf header)
        self.phase = "normal"       # normal -> [pending ->] down -> trial -> kept / rolled_back / old_firmware
        self.down_polls = 0
        self.trial_polls = 0
        self.lock = threading.Lock()
        dev = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.0"

            def log_message(self, *args):
                pass

            def reply(self, code, body, headers=()):
                data = body.encode()
                self.send_response(code)
                self.send_header("Content-Type", "text/plain")
                self.send_header("Content-Length", str(len(data)))
                for k, v in headers:
                    self.send_header(k, v)
                self.end_headers()
                self.wfile.write(data)

            def drop(self):
                self.connection.close()

            def authed(self):
                return "sid=goodsid" in (self.headers.get("Cookie") or "")

            def do_GET(self):
                with dev.lock:
                    dev.requests.append(("GET", self.path))
                    if dev.phase == "down":
                        dev.down_polls += 1
                        if dev.down_polls > 2:
                            dev.phase = dev.next_phase()
                        return self.drop()
                if not self.authed():
                    return self.reply(200, '<form action="/login">')
                if self.path == "/firmware":
                    return self.reply(200, '<input type="hidden" name="csrf" value="c5af0123c5af0123c5af0123c5af0123">')
                if self.path == "/firmware/status":
                    return self.reply(200, dev.status_text())
                self.reply(404, "")

            def do_POST(self):
                n = int(self.headers.get("Content-Length") or 0)
                body = self.rfile.read(n)
                with dev.lock:
                    dev.requests.append(("POST", self.path))
                    if dev.phase == "down":
                        dev.down_polls += 1
                        if dev.down_polls > 2:
                            dev.phase = dev.next_phase()
                        return self.drop()
                if self.path == "/login":
                    form = parse_qs(body.decode())
                    pw = form.get("password", [""])[0]
                    dev.login_passwords.append(pw)
                    if pw == PASSWORD and form.get("user") == ["admin"]:
                        return self.reply(302, "", [("Location", "/"), ("Set-Cookie", "sid=goodsid; Path=/; HttpOnly")])
                    return self.reply(401, "Wrong user name or password.")
                if self.path == "/firmware/upload":
                    if not self.authed():
                        return self.reply(401, "login")
                    dev.uploads.append((len(body), self.headers.get("X-CSRF-Token")))
                    if dev.reject_reason:
                        with dev.lock:
                            dev.phase = "rejected"
                        return self.drop() if dev.upload_drop else self.reply(400, dev.reject_reason)
                    if dev.upload_status == 200:
                        with dev.lock:
                            dev.phase = "pending" if dev.restart_after_polls else "down"
                        return self.drop() if dev.upload_drop else self.reply(200, dev.upload_text)
                    return self.reply(dev.upload_status, dev.upload_text)
                self.reply(404, "")

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    @property
    def host(self):
        return "127.0.0.1:%d" % self.port

    def next_phase(self):
        """The phase after the restart."""
        return {"kept": "trial", "rolled_back": "rolled_back", "old_firmware": "old_firmware"}.get(
            self.after_upload, "down")

    def status_text(self):
        with self.lock:
            phase = self.phase
            if phase == "trial":
                self.trial_polls += 1
                if self.trial_polls > 2:
                    self.phase = phase = "kept"
            elif phase == "pending":
                self.pending_polls += 1
                if self.pending_polls >= self.restart_after_polls:
                    self.phase = "down"  # restarts after this answer
        slot = "A"
        if phase == "kept":
            slot, ver, state, last = "B", VERSION, "kept", "installed " + VERSION
        elif phase == "trial":
            slot, ver, state, last = "B", VERSION, "trial", "installed " + VERSION
        elif phase == "rolled_back":
            ver, state, last = self.old_version, "kept", "rolled back (crash) " + VERSION
        elif phase == "pending":
            ver, state, last = self.old_version, "kept", "verified %s, restarting on trial" % VERSION
        elif phase == "rejected":
            ver, state, last = self.old_version, "kept", "rejected: " + self.reject_reason
        else:
            ver, state, last = self.old_version, "kept", "none"
        a, b = (ver, "") if slot == "A" else ("", ver)
        return ("running_slot=%s\nrunning_version=%s\nstate=%s\nslot_a=%s\nslot_b=%s\nlast_result=%s\nkey=%s\n"
                % (slot, ver, state, a, b, last, self.key))

    def close(self):
        self.server.shutdown()
        self.server.server_close()


def closed_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return "127.0.0.1:%d" % port


class FwUpdateScriptTest(unittest.TestCase):
    def setUp(self):
        self.devices = []
        self.tmp = os.path.join(ROOT, ".pio", "fwsign", "test_upd_%d.upd" % os.getpid())
        os.makedirs(os.path.dirname(self.tmp), exist_ok=True)
        with open(self.tmp, "wb") as f:
            f.write(UPD)
        self.addCleanup(lambda: os.path.exists(self.tmp) and os.remove(self.tmp))

    def device(self, **kw):
        d = FakeDevice(**kw)
        self.devices.append(d)
        self.addCleanup(d.close)
        return d

    def run_script(self, hosts, *extra, password=PASSWORD):
        out = io.StringIO()
        env = {"RFPOWER_WEB_PASSWORD": password}
        with contextlib.redirect_stdout(out):
            code = fw_update.main([self.tmp] + list(hosts) + FAST + list(extra), environ=env)
        return code, out.getvalue()

    def test_script_updates_and_waits_for_kept(self):
        d = self.device()
        code, out = self.run_script([d.host])
        self.assertEqual(0, code, out)
        self.assertIn("%s: updated" % d.host, out)
        self.assertEqual(1, len(d.uploads))
        self.assertEqual("c5af0123c5af0123c5af0123c5af0123", d.uploads[0][1])   # the token read from the /firmware page
        self.assertEqual("kept", d.phase)             # it only returned once the trial was over
        self.assertGreaterEqual(d.trial_polls, 3)

    def test_script_sends_whole_file_as_raw_body(self):
        d = self.device()
        self.run_script([d.host])
        self.assertEqual(len(UPD), d.uploads[0][0])

    def test_script_stops_at_first_failure(self):
        bad = self.device(upload_status=409, upload_text="busy")
        good = self.device()
        code, out = self.run_script([bad.host, good.host])
        self.assertNotEqual(0, code)
        self.assertIn("%s: rejected: busy" % bad.host, out)
        self.assertNotIn(good.host, out)
        self.assertEqual([], good.uploads)

    def test_script_continue_flag_runs_all(self):
        bad = self.device(upload_status=400, upload_text="bad signature")
        good = self.device()
        code, out = self.run_script([bad.host, good.host], "--continue")
        self.assertNotEqual(0, code)
        self.assertIn("%s: rejected: bad signature" % bad.host, out)
        self.assertIn("%s: updated" % good.host, out)
        self.assertEqual(1, len(good.uploads))

    def test_script_reports_rolled_back(self):
        d = self.device(after_upload="rolled_back")
        code, out = self.run_script([d.host])
        self.assertNotEqual(0, code)
        self.assertIn("%s: rolled back: rolled back (crash) %s" % (d.host, VERSION), out)

    def test_script_same_version_waits_for_the_restart(self):
        # A rebuilt release of the running version: "kept" with that version holds before the
        # restart too; only the slot change shows the new firmware runs
        d = self.device(old_version=VERSION, restart_after_polls=3)
        code, out = self.run_script([d.host])
        self.assertEqual(0, code, out)
        self.assertIn("%s: updated" % d.host, out)
        self.assertEqual("kept", d.phase)
        self.assertGreaterEqual(d.pending_polls, 3)
        self.assertGreaterEqual(d.trial_polls, 3)  # it saw the trial through

    def test_script_waits_while_the_old_firmware_still_runs(self):
        d = self.device(restart_after_polls=4)
        code, out = self.run_script([d.host])
        self.assertEqual(0, code, out)
        self.assertIn("%s: updated" % d.host, out)
        self.assertEqual("kept", d.phase)

    def test_script_reports_lost_reply_to_rejected_upload_as_rejected(self):
        d = self.device(upload_drop=True, reject_reason="bad signature")
        code, out = self.run_script([d.host])
        self.assertNotEqual(0, code)
        self.assertIn("%s: rejected: no reply (bad signature)" % d.host, out)
        self.assertNotIn("rolled back", out)

    def test_script_lost_reply_to_accepted_upload_still_updates(self):
        d = self.device(upload_drop=True)
        code, out = self.run_script([d.host])
        self.assertEqual(0, code, out)
        self.assertIn("%s: updated" % d.host, out)

    def test_script_reports_restart_on_old_firmware_without_rollback_as_not_installed(self):
        d = self.device(after_upload="old_firmware")
        code, out = self.run_script([d.host])
        self.assertNotEqual(0, code)
        self.assertIn("%s: not installed" % d.host, out)
        self.assertNotIn("updated", out)

    def test_script_refuses_key_mismatch_before_upload(self):
        d = self.device(key="0123456789abcdef")
        code, out = self.run_script([d.host])
        self.assertNotEqual(0, code)
        self.assertIn("%s: rejected: different key" % d.host, out)
        self.assertEqual([], d.uploads)

    def test_script_reports_the_devices_own_different_key_refusal(self):
        # The key matches the pre-check, yet the device (403, "different key") refuses the image
        d = self.device(upload_status=403, upload_text="different key")
        code, out = self.run_script([d.host])
        self.assertNotEqual(0, code)
        self.assertIn("%s: rejected: different key" % d.host, out)
        self.assertEqual(1, len(d.uploads))

    def test_script_reports_unreachable_host(self):
        host = closed_port()
        code, out = self.run_script([host])
        self.assertNotEqual(0, code)
        self.assertIn("%s: unreachable" % host, out)

    def test_script_reports_timeout_when_device_never_returns(self):
        d = self.device(after_upload="dead")
        code, out = self.run_script([d.host])
        self.assertNotEqual(0, code)
        self.assertIn("%s: timeout" % d.host, out)

    def test_script_rejects_wrong_password_without_upload(self):
        d = self.device()
        code, out = self.run_script([d.host], password="wrong")
        self.assertNotEqual(0, code)
        self.assertIn("%s: rejected: wrong password" % d.host, out)
        self.assertEqual([], d.uploads)

    def test_script_reads_hosts_from_file(self):
        a, b = self.device(), self.device()
        hosts_file = self.tmp + ".hosts"
        with open(hosts_file, "w") as f:
            f.write("# devices\n%s\n\n%s\n" % (a.host, b.host))
        self.addCleanup(os.remove, hosts_file)
        code, out = self.run_script([], "--hosts-file", hosts_file)
        self.assertEqual(0, code, out)
        self.assertEqual(1, len(a.uploads))
        self.assertEqual(1, len(b.uploads))

    def test_script_rejects_a_file_that_is_not_upd(self):
        d = self.device()
        with open(self.tmp, "wb") as f:
            f.write(b"not an update file" * 10)
        code, out = self.run_script([d.host])
        self.assertNotEqual(0, code)
        self.assertEqual([], d.uploads)

    def test_script_never_puts_password_in_argv_or_output(self):
        d = self.device()
        env = dict(os.environ, RFPOWER_WEB_PASSWORD=PASSWORD)
        cmd = [sys.executable, os.path.join(ROOT, "tools", "fw_update.py"), self.tmp, d.host] + FAST
        r = subprocess.run(cmd, env=env, capture_output=True, text=True)
        self.assertEqual(0, r.returncode, r.stdout + r.stderr)
        self.assertNotIn(PASSWORD, " ".join(cmd))
        self.assertNotIn(PASSWORD, r.stdout + r.stderr)
        self.assertEqual([PASSWORD] * len(d.login_passwords), d.login_passwords)  # but it was used
        self.assertTrue(d.login_passwords)

    def test_script_has_no_password_argument(self):
        with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
            fw_update.main([self.tmp, "h", "--password", "x"], environ={"RFPOWER_WEB_PASSWORD": "x"})


if __name__ == "__main__":
    unittest.main()
