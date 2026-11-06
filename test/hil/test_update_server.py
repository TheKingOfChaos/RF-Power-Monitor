"""Hardware-in-the-loop tests for the update site: check-ins and firmware updates
chosen on the site, end to end with the real firmware.

Opt-in: they run the site (update-server/, python -m rfupdate) on this PC with a
temporary data directory and a new shared key, point the device at it, and
install a development-signed test build (tools/fw_release.py --test-version
3.4.1-hil, built on first use into .pio/fwtest/). The device ends on the
firmware version it started with (3.4.0, development-signed, slot A or B), with
no update server set.

Run (firmware already flashed, PuTTY closed):
    set RFPOWER_UPDATE_SERVER_TEST=1
    %USERPROFILE%\\.platformio\\penv\\Scripts\\python.exe -m unittest test/hil/test_update_server.py -v

Environment variables:
    RFPOWER_UPDATE_SERVER_TEST=1    run these tests
    RFPOWER_UPDATE_SERVER_HOST      this PC's address as the device reaches it (default: found from the route)
    RFPOWER_UPDATE_SERVER_PORT      port for the test site (default 8072)
    RFPOWER_SNMP_COMMUNITY          the device's SNMP community (default public)
The device must reach this PC on that TCP port (Windows firewall).
"""

import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest

from device import Device
import snmp_tools

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "update-server"))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from rfupdate.store import Store  # noqa: E402
import fwtool  # noqa: E402

ENABLED = os.environ.get("RFPOWER_UPDATE_SERVER_TEST") == "1"
PORT = int(os.environ.get("RFPOWER_UPDATE_SERVER_PORT", "8072"))
COMMUNITY = os.environ.get("RFPOWER_SNMP_COMMUNITY", "public")
TEST_VERSION = "3.4.1-hil"
SITE_PASSWORD = "hil-only"


def build_release(version):
    """A development-signed .upd built as this version (cached in .pio/fwtest/)."""
    path = os.path.join(ROOT, ".pio", "fwtest", version + ".upd")
    if not os.path.exists(path):
        subprocess.run([sys.executable, os.path.join(ROOT, "tools", "fw_release.py"), "--test-version", version],
                       cwd=ROOT, check=True)
    with open(path, "rb") as f:
        return f.read()


def flip_signatures(data):
    """The same .upd with one byte of each slot's Ed25519 signature changed."""
    import struct
    _, _, _, off_a, off_b, _, _ = struct.unpack_from("<II16s4I", data, 0)
    out = bytearray(data)
    for off in (off_a, off_b):
        out[off + 10] ^= 0x01
    return bytes(out)


@unittest.skipUnless(ENABLED, "set RFPOWER_UPDATE_SERVER_TEST=1 (runs a test site on this PC and updates the device)")
class UpdateServerTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.release_version = fwtool.firmware_version()
        cls.dev = Device()
        cls.dev.open()
        cls.dev.wait_for_menu()
        st = cls.dev.wait_connected()
        if st["fwVersion"] != cls.release_version or st["fwState"] != "kept":
            cls.dev.close()
            raise unittest.SkipTest("the device must run %s, kept (it runs %s, %s)"
                                    % (cls.release_version, st["fwVersion"], st["fwState"]))
        cls.ip = st["ip"]
        cls.good = build_release(TEST_VERSION)
        cls.back = build_release(cls.release_version)
        cls.data = tempfile.mkdtemp(prefix="rfupdate-hil-")
        store = Store(cls.data)
        cls.key = store.new_site_key()
        store.close()
        env = dict(os.environ, RFUPDATE_ADMIN_PASSWORD=SITE_PASSWORD)
        cls.site = subprocess.Popen([sys.executable, "-m", "rfupdate", "--host", "0.0.0.0", "--port", str(PORT),
                                     "--data", cls.data], cwd=os.path.join(ROOT, "update-server"), env=env,
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.time() + 10
        while True:
            try:
                socket.create_connection(("127.0.0.1", PORT), timeout=1).close()
                break
            except OSError:
                if time.time() > deadline:
                    cls.site.kill()
                    raise
                time.sleep(0.2)
        host = os.environ.get("RFPOWER_UPDATE_SERVER_HOST") or snmp_tools.local_ip_towards(cls.ip)
        cls.url = "http://%s:%d" % (host, PORT)
        out = cls.dev.set_update_server(cls.url, cls.key.hex())
        assert "saved" in out, out
        cls.dev.send("checkin", expect=r"checkin: requested")  # the device appears on the site
        deadline = time.time() + 30
        while not re.search(r"last check-in \d+ s ago: ok", cls.dev.update_server_line() or ""):
            if time.time() > deadline:
                raise AssertionError("first check-in failed: %s" % cls.dev.update_server_line())
            time.sleep(1)

    @classmethod
    def tearDownClass(cls):
        try:
            cls.dev.set_update_server("none", "clear")
        finally:
            cls.dev.close()
            cls.site.terminate()
            cls.site.wait(10)
            shutil.rmtree(cls.data, ignore_errors=True)

    def setUp(self):
        self.dev.wait_for_menu(timeout=10)

    # --- helpers ---

    def store(self):
        return Store(self.data)

    def row(self):
        store = self.store()
        try:
            rows = store.device_rows(time.time())
        finally:
            store.close()
        self.assertEqual(1, len(rows), rows)
        return rows[0]

    def put_release(self, data):
        """Upload data as a release (replacing an unassigned one with the same version)."""
        store = self.store()
        try:
            for r in store.releases():
                try:
                    store.delete_release(r["version"])
                except ValueError:
                    pass
            return store.add_release(data, time.time())
        finally:
            store.close()

    def assign(self, version):
        store = self.store()
        try:
            store.assign([self.row()["serial"]], version, time.time())
        finally:
            store.close()

    def checkin(self, expect, timeout=40):
        """Ask for a check-in now (hidden command) and wait until the status line shows
        a check-in that ended after it, matching expect; returns that line."""
        self.dev.send("checkin", expect=r"checkin: requested")
        start = time.time()
        line = ""
        while time.time() - start < timeout:
            line = self.dev.update_server_line() or ""
            m = re.search(r"last check-in (\d+) s ago: (.*)$", line)
            if m and int(m.group(1)) <= time.time() - start + 1 and re.search(expect, m.group(2)):
                return line
            time.sleep(1)
        self.fail("no check-in matching %r within %d s: %s" % (expect, timeout, line))

    def wait_restart_and_keep(self, version):
        """After a verified download: the device restarts into version on trial; a check-in
        confirms it, the next one reports it kept."""
        self.dev.read_until(r"restarting into it on trial", timeout=120)
        st = self.dev.reconnect_after_reboot(timeout=60)
        self.assertEqual(version, st["fwVersion"])
        self.checkin(r"^ok$")  # an authenticated reply: the trial is confirmed
        deadline = time.time() + 20
        while self.dev.status()["fwState"] != "kept":
            self.assertLess(time.time(), deadline, "trial not confirmed")
            time.sleep(1)
        self.checkin(r"^ok$")  # reports the kept firmware

    # --- tests ---

    def test_checkin_appears_on_site(self):
        self.checkin(r"^ok$")
        r = self.row()
        self.assertEqual(self.ip, r["ipv4"])
        self.assertEqual(self.release_version, r["version"])
        self.assertTrue(r["online"])
        self.assertTrue(r["hostname"])
        self.assertRegex(r["serial"], r"^[0-9A-F]{16}$")

    def test_wrong_key_shows_badauth(self):
        try:
            self.assertIn("saved", self.dev.set_update_server("", "00" * 32))
            self.checkin(r"^badauth$")
        finally:
            self.dev.set_update_server("", self.key.hex())
        self.checkin(r"^ok$")

    def test_checkin_unreachable_server_keeps_snmp_answering(self):
        closed = re.sub(r":\d+$", ":8099", self.url)
        try:
            self.dev.set_update_server(closed, "")
            self.dev.send("checkin", expect=r"checkin: requested")
            times = []
            for i in range(20):
                resp, sec = snmp_tools.raw_get(self.ip, 1000 + i, timeout=1.0,
                                               packet=snmp_tools.get_sysdescr_packet(COMMUNITY))
                self.assertIsNotNone(resp, "SNMP request %d not answered" % i)
                times.append(sec)
            self.assertLess(max(times), 0.1, times)
            line = self.checkin(r"connect|reset|closed|answer|timeout")
            self.assertNotIn(": ok", line)
        finally:
            self.dev.set_update_server(self.url, "")

    def test_corrupt_release_is_refused_and_device_keeps_running(self):
        version = self.put_release(flip_signatures(self.good))
        self.assertEqual(TEST_VERSION, version)
        self.assign(version)
        self.dev.send("checkin", expect=r"checkin: requested")
        start = time.time()
        while "refused" not in (self.dev.update_server_line() or ""):  # downloaded, then refused at the check
            self.assertLess(time.time() - start, 120, self.dev.update_server_line())
            time.sleep(2)
        self.checkin(r"^ok$")  # reports the refusal
        r = self.row()
        self.assertEqual("refused", r["update_status"])
        self.assertIn("signature", r["update_detail"])
        st = self.dev.status()
        self.assertEqual((self.release_version, "kept"), (st["fwVersion"], st["fwState"]))

    def test_assigned_release_installs_and_is_kept_then_back_to_release(self):
        self.put_release(self.good)
        self.assign(TEST_VERSION)
        self.dev.send("checkin", expect=r"checkin: requested")  # downloads, checks, restarts
        self.wait_restart_and_keep(TEST_VERSION)
        self.assertEqual("done", self.row()["update_status"])
        self.assertEqual(TEST_VERSION, self.row()["version"])

        # And back to the release through the site (an older version, chosen on purpose)
        self.put_release(self.back)
        self.assign(self.release_version)
        self.dev.send("checkin", expect=r"checkin: requested")
        self.wait_restart_and_keep(self.release_version)
        self.assertEqual("done", self.row()["update_status"])
        st = self.dev.status()
        self.assertEqual((self.release_version, "kept"), (st["fwVersion"], st["fwState"]))


if __name__ == "__main__":
    unittest.main()
