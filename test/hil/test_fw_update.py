"""Hardware-in-the-loop tests for the boot program and firmware slots, and for signed updates.

BootProgramTest (always runs) and FirmwareUpdateTest (opt-in, RFPOWER_FW_UPDATE_TEST=1, see
docs/TESTING.md "Firmware update tests") live here.

The boot program (boot/) verifies the firmware slot before it starts it and
tells the firmware what it did; the firmware prints it early in setup():
"Boot: slot A verified in <n> ms (normal|trial|fallback|no record)".
These tests reboot the device once (menu option 2, DHCP unchanged) and read
the boot log; settings are not touched.

Run:
    %USERPROFILE%\\.platformio\\penv\\Scripts\\python.exe -m unittest discover -s test/hil -p "test_fw_update.py" -v
"""

import http.client
import os
import re
import socket
import subprocess
import sys
import tempfile
import time
import unittest

import serial

from device import Device
from snmp_tools import TrapListener, local_ip_towards, net_snmp

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import fw_release  # noqa: E402
import fw_update  # noqa: E402
import fwtool  # noqa: E402

BOOT_LINE = r"Boot: slot ([AB]) verified in (\d+) ms \((normal|trial|fallback|no record)\)"
VERIFY_LIMIT_MS = 1500


class BootProgramTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.dev = Device()
        cls.dev.open()
        cls.dev.wait_for_menu()
        if cls.dev.status()["mode"] != "DHCP":
            cls.dev.close()
            raise unittest.SkipTest("reboots with option 2 (DHCP, unchanged); device is in static mode")
        cls.boot_log = cls.reboot_and_read_boot_log()

    @classmethod
    def tearDownClass(cls):
        cls.dev.wait_connected(timeout=40)
        cls.dev.close()

    @classmethod
    def reboot_and_read_boot_log(cls):
        """Reboot through the menu and return the boot log up to the first prompt."""
        d = cls.dev
        d.send("2", expect=r"\(y/n\): $")
        d.send("y", expect=r"Rebooting", timeout=10)
        d.close()
        time.sleep(1.5)
        d.open()  # the firmware waits for DTR before printing, so the boot log is captured
        return d.read_until(r"> $", timeout=10)

    def boot_line(self):
        m = re.search(BOOT_LINE, self.boot_log)
        self.assertIsNotNone(m, "no boot program line in the boot log:\n%s" % self.boot_log[:600])
        return m

    def test_boot_program_starts_the_running_slot_after_menu_reboot(self):
        m = self.boot_line()
        # The update tests leave the device in either slot: the boot program must start
        # the slot the status screen says runs (the firmware built for it)
        self.assertEqual(self.dev.status()["fwSlot"], m.group(1))
        # USB flashing writes an erased boot record; the firmware does not write one yet
        self.assertIn(m.group(3), ("no record", "normal"))

    def test_menu_reboot_reads_as_planned_restart(self):
        # The boot program arms the watchdog for every start; a menu reboot must
        # still read as a planned restart, not a watchdog (crash) restart
        self.assertRegex(self.boot_log, r"Last restart: reboot \(")

    def test_boot_verification_takes_under_1500_ms(self):
        n = int(self.boot_line().group(2))
        self.assertGreater(n, 0, "verification time not measured")
        self.assertLess(n, VERIFY_LIMIT_MS)


# ---- signed update, rollback and fallback (opt-in) ----

FW_TEST = os.environ.get("RFPOWER_FW_UPDATE_TEST") == "1"
POE_CYCLE = os.environ.get("RFPOWER_POE_CYCLE") == "1"
SWITCH_CONSOLE = os.environ.get("RFPOWER_LLDP_SWITCH_CONSOLE")
COMMUNITY = os.environ.get("RFPOWER_COMMUNITY", "public")
FWTEST_DIR = os.path.join(ROOT, ".pio", "fwtest")
OID_FW = "1.3.6.1.4.1.63050.7."          # fwUpdate group: .1 slot, .4 fwState, .5 fwLastResult
SOURCE_DIRS = ("src", "lib", "include", "boot", "scripts")
SERIAL_ERRORS = (TimeoutError, RuntimeError, OSError, serial.SerialException, AttributeError)
STACK_MIN_NEVER_USED = 1024              # as test_web.py: core 0 keeps 1 KB that was never touched

# Timing of the rollback paths (docs/TESTING.md): crash is 3 x (8.3 s watchdog + boot), an
# unreachable trial is the shorttrial variant's 60 s of network time plus two boots
CRASH_ROLLBACK_LIMIT_S = 120
UNREACHABLE_ROLLBACK_LIMIT_S = 150
TEARDOWN_LIMIT_S = 240                   # a test image rolls back within this (shorttrial: 80 s measured)


def _newest_source():
    """Newest mtime of anything a firmware build depends on."""
    newest = os.path.getmtime(os.path.join(ROOT, "platformio.ini"))
    for d in SOURCE_DIRS:
        for base, _dirs, files in os.walk(os.path.join(ROOT, d)):
            for f in files:
                newest = max(newest, os.path.getmtime(os.path.join(base, f)))
    return newest


def _fresh(path):
    return os.path.exists(path) and os.path.getmtime(path) >= _newest_source() \
        and not os.environ.get("RFPOWER_FW_REBUILD")


def build_test_images():
    """Build (or reuse, when newer than every source) the .upd files the tests upload.

    crash.upd and shorttrial.upd come from tools/fw_release.py --test-variant; normal.upd is the
    current source signed with the development key; otherkey.upd the same images signed with a
    throw-away key the device does not trust. Returns {name: path}.
    """
    os.makedirs(FWTEST_DIR, exist_ok=True)
    paths = {n: os.path.join(FWTEST_DIR, n + ".upd") for n in ("crash", "shorttrial", "normal", "otherkey")}
    for variant in ("crash", "shorttrial"):
        if not _fresh(paths[variant]):
            subprocess.run([sys.executable, os.path.join(ROOT, "tools", "fw_release.py"),
                            "--test-variant", variant], cwd=ROOT, check=True)
    # Last: the build directories then hold the normal images that otherkey.upd is signed from
    if not (_fresh(paths["normal"]) and _fresh(paths["otherkey"])):
        for env_name in ("pico", "pico_b"):
            fw_release.pio_run(env_name, {})
        version = fwtool.firmware_version()
        seed = tempfile.NamedTemporaryFile(delete=False)
        try:
            seed.write(os.urandom(32))
            seed.close()
            for name, seed_path in (("normal", fwtool.DEV_KEY), ("otherkey", seed.name)):
                blobs = [fwtool.sign_slot(seed_path, slot, version,
                                          os.path.join(ROOT, ".pio", "build", env_name, "firmware.bin"))
                         for slot, env_name in ((0, "pico"), (1, "pico_b"))]
                with open(paths[name], "wb") as f:
                    f.write(fw_release.build_upd(version, blobs[0], blobs[1]))
        finally:
            os.unlink(seed.name)
    return paths


def poe_baseline(sw, port, mac):
    """Read-only look at the switch before PoE on `port` may be changed.

    Returns (mac_learned, own_statements): whether the switch lists the device's MAC on that port
    (LLDP neighbours, as test_switch_port.py does), and the port's own PoE statements as
    "set poe interface <port> ..." lines, which the test restores exactly.
    """
    listing = sw.run("show lldp neighbors")
    learned = bool(re.search(r"^%s(\.\d+)?\s.*%s" % (re.escape(port), re.escape(mac.lower())),
                             listing, re.MULTILINE))
    conf = sw.run("show configuration poe | display set")
    own = [line.strip() for line in conf.splitlines()
           if re.match(r"set poe interface %s(\s|$)" % re.escape(port), line.strip())]
    return learned, own


def read_file(path):
    with open(path, "rb") as f:
        return f.read()


def snmp_value(ip, oid):
    """Value of a v2c GET with the device's community (RFPOWER_COMMUNITY), as text."""
    code, out = net_snmp("snmpget", ip, oid, community=COMMUNITY, version="2c", retries=2)
    m = re.search(r'= (?:\w+): "?([^"\r\n]*)"?', out)
    if code != 0 or not m:
        raise AssertionError("snmpget %s failed: %s" % (oid, out.strip()))
    return m.group(1)


@unittest.skipUnless(FW_TEST, "set RFPOWER_FW_UPDATE_TEST=1 (slow: builds, uploads and reboots the device)")
class FirmwareUpdateTest(unittest.TestCase):
    """Uploads, rollbacks and fallbacks on the real device.

    The serial port is the observer: it never counts as a request, so a trial stays a trial until
    the test itself logs in on the web page or reads over SNMP. Every test ends with the device on
    a development-signed build of the current version, kept, in either slot.
    """

    @classmethod
    def setUpClass(cls):
        cls.upd = {n: read_file(p) for n, p in build_test_images().items()}
        cls.dev = Device()
        cls.dev.open()
        cls.dev.wait_for_menu()
        st = cls.dev.wait_connected(timeout=60)
        cls.password = os.environ.get("RFPOWER_WEB_PASSWORD") or st["webDefaultPassword"]
        problem = None
        if st["mode"] != "DHCP":
            problem = "the device must be in DHCP mode"
        elif not cls.password:
            problem = "no web password: set RFPOWER_WEB_PASSWORD (the device has its own password)"
        elif st["fwKey"] != fwtool.key_fingerprint(fwtool.public_key(fwtool.DEV_KEY)):
            problem = "the device does not trust the development key (field firmware)"
        if problem:
            cls.dev.close()
            raise unittest.SkipTest(problem)
        cls.ip = st["ip"]
        cls.version = fwtool.firmware_version()
        # Traps: only when this PC can listen on UDP 162 (LibreNMS's snmptrapd may hold it) and
        # is the device's trap destination; the trap assertions are skipped otherwise
        cls.listener = None
        try:
            if st["trapTarget"] == local_ip_towards(cls.ip):
                cls.listener = TrapListener()
        except OSError as e:
            print("\nno trap receiver (%s): trap assertions are skipped" % e)

    @classmethod
    def tearDownClass(cls):
        """Leave the device on a kept development-signed normal build, or fail loudly.

        A failed test can leave a crash or shorttrial image on trial: a login cannot confirm those,
        they roll back by themselves (80 s measured for shorttrial), so wait for that.
        """
        try:
            deadline = time.time() + TEARDOWN_LIMIT_S
            confirmed = False
            while True:
                st = cls.status()
                # kept means a normal build: the test images (crash, shorttrial) are never confirmed
                if st["fwState"] == "kept" and st["fwVersion"] == cls.version:
                    break
                if not confirmed:
                    confirmed = True
                    try:
                        cls.web()  # confirms a normal build left on trial; test images ignore it
                    except AssertionError:
                        pass
                if time.time() > deadline:
                    raise AssertionError("device not on a kept normal build %d s after the tests: %s" % (
                        TEARDOWN_LIMIT_S, {x: st[x] for x in ("fwSlot", "fwVersion", "fwState", "fwLastUpdate")}))
                time.sleep(3)
        finally:
            if cls.listener:
                cls.listener.close()
            cls.dev.close()

    # ---- observing the device ----

    @classmethod
    def reconnect(cls, timeout=150):
        """Reopen the serial port after a restart and wait for the network."""
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            cls.dev.close()
            try:
                time.sleep(1.5)
                cls.dev.open()
                cls.dev.wait_for_menu(15)
                return cls.dev.wait_connected(40)
            except SERIAL_ERRORS as e:
                last = e
        raise TimeoutError("device not back within %d s: %s" % (timeout, last))

    @classmethod
    def status(cls):
        """The status screen, reconnecting when the port went away."""
        try:
            return cls.dev.status()
        except SERIAL_ERRORS:
            cls.reconnect()
            return cls.dev.status()

    @classmethod
    def await_status(cls, predicate, timeout, poll=2.0):
        """Poll the status screen until predicate(status) holds; returns it, or raises TimeoutError."""
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            try:
                last = cls.dev.status()
                if predicate(last):
                    return last
            except SERIAL_ERRORS:
                try:
                    cls.reconnect(timeout=max(10, deadline - time.time()))
                except TimeoutError:
                    break
            time.sleep(poll)
        keys = ("fwSlot", "fwVersion", "fwState", "fwOther", "fwLastUpdate")
        raise TimeoutError("status condition not met within %d s; last: %s" % (
            timeout, last and {k: last[k] for k in keys}))

    @classmethod
    def web(cls):
        """A logged-in web session (this is a valid request: it confirms a trial)."""
        w = fw_update.Device(cls.ip, cls.password, 60)
        last = None
        for _ in range(5):
            try:
                w.login()
                return w
            except (OSError, http.client.HTTPException) as e:
                last = e
                time.sleep(2)
        raise AssertionError("cannot log in on the web page: %r" % last)

    @staticmethod
    def csrf(w):
        _, _, page = w.request("GET", "/firmware")
        m = re.search(r'name="csrf" value="([0-9a-fA-F]+)"', page)
        assert m, "no CSRF token on the firmware page"
        return m.group(1)

    @classmethod
    def upload(cls, data, w=None):
        """POST /firmware/upload; (HTTP status, text), or (None, "") when the device restarted unanswered."""
        w = w or cls.web()
        token = cls.csrf(w)
        try:
            code, _, text = w.request("POST", "/firmware/upload", data,
                                      {"Content-Type": "application/octet-stream", "X-CSRF-Token": token})
        except (OSError, http.client.HTTPException):
            return None, ""
        return code, text.strip()

    def kept_state(self):
        """Precondition: the device runs a kept firmware (a trial left behind is confirmed first)."""
        st = self.status()
        if st["fwState"] == "trial":
            self.web()
            st = self.await_status(lambda s: s["fwState"] == "kept", 30)
        self.assertEqual("kept", st["fwState"])
        return st

    @staticmethod
    def flipped(before, states=("trial", "kept")):
        """Predicate: the other slot runs. A poller with the device's community (LibreNMS) can confirm
        a trial within seconds of its start, before the slow serial reconnect sees it: "kept" counts too
        unless a test needs the trial itself (states=("trial",))."""
        return lambda s: s["fwSlot"] != before["fwSlot"] and s["fwState"] in states

    @staticmethod
    def other_is_valid(st):
        return bool(re.match(r"\d+\.\d+\.\d+", st["fwOther"] or ""))

    def install(self, name="normal", confirm="web"):
        """Upload an image to the other slot; returns the status once it runs on trial.

        confirm "web" then confirms it with a login and waits for kept; None leaves it on trial.
        """
        before = self.kept_state()
        code, text = self.upload(self.upd[name])
        self.assertIn(code, (200, None), "upload refused: %s %s" % (code, text))
        trial = self.await_status(self.flipped(before), 90)
        self.assertEqual(self.version, trial["fwVersion"])
        if confirm == "web":
            self.web()
            return self.await_status(lambda s: s["fwState"] == "kept", 40)
        return trial

    def ensure_other_valid(self):
        """Precondition for switch and fallback: both slots hold a valid image."""
        st = self.kept_state()
        if not self.other_is_valid(st):
            st = self.install()
            self.assertTrue(self.other_is_valid(st), st["fwOther"])
        return st

    def assert_unchanged(self, before, other_changes=False):
        st = self.status()
        for key in ("fwSlot", "fwVersion", "fwState"):
            self.assertEqual(before[key], st[key], key)
        if not other_changes:
            self.assertEqual(before["fwOther"], st["fwOther"])
        return st

    # ---- traps ----

    def trap_clear(self):
        if self.listener:
            self.listener.clear()

    def expect_trap(self, kind, timeout, last_result=None):
        """Wait for a trap of this kind; skipped (returns None) when no receiver could be started."""
        if not self.listener:
            return None
        trap = self.listener.wait_for(lambda t: t["kind"] == kind, timeout)
        self.assertIsNotNone(trap, "no %s trap within %d s" % (kind, timeout))
        self.assertEqual(self.ip, trap["source"])
        if last_result is not None:
            values = [v for o, (_tag, v) in trap["varbinds"].items() if o.endswith("63050.7.5.0")]
            self.assertEqual([last_result], values, "fwLastResult in the %s trap" % kind)
        return trap

    # ---- the tests ----

    def test_upload_installs_and_is_kept_after_snmp_get(self):
        before = self.kept_state()
        self.trap_clear()
        t0 = time.time()
        code, text = self.upload(self.upd["normal"])
        self.assertIn(code, (200, None), text)
        trial = self.await_status(self.flipped(before), 90)
        self.assertEqual(self.version, trial["fwVersion"])
        # The serial port is not a request; a trial seen here is only confirmed by the SNMP get below
        # (a trial seen as kept was confirmed by LibreNMS's own polling first)
        self.assertEqual("1" if trial["fwSlot"] == "A" else "2", snmp_value(self.ip, OID_FW + "1.0"))
        kept = self.await_status(lambda s: s["fwState"] == "kept", 40)
        self.assertEqual(trial["fwSlot"], kept["fwSlot"])
        self.assertEqual("1", snmp_value(self.ip, OID_FW + "4.0"), "fwState")
        self.assertEqual("1", snmp_value(self.ip, OID_FW + "5.0"), "fwLastResult installed")
        self.assertTrue(kept["fwLastUpdate"].startswith("installed"), kept["fwLastUpdate"])
        self.expect_trap("firmwareUpdateKept", 30, last_result=1)
        print("\nupload to kept: %.0f s" % (time.time() - t0))

    def test_upload_with_other_key_is_rejected(self):
        before = self.kept_state()
        code, text = self.upload(self.upd["otherkey"])
        self.assertEqual(403, code, text)
        self.assertEqual("different key", text)
        self.assert_unchanged(before, other_changes=True)

    def corrupted_image(self, st):
        """The normal image with one byte flipped in the part for the slot the device would write."""
        data = bytearray(self.upd["normal"])
        target = 1 if st["fwSlot"] == "A" else 0
        head = fw_release.parse_upd_header(bytes(data))
        data[head["slotOffset"][target] + fw_update.SLOT_HEADER_SIZE + 1000] ^= 0xFF
        return bytes(data)

    def test_corrupted_upload_is_rejected_and_device_keeps_running(self):
        before = self.ensure_other_valid()
        code, text = self.upload(self.corrupted_image(before))
        self.assertIsNotNone(code, "the device restarted on a corrupted image")
        self.assertNotEqual(200, code, text)
        st = self.assert_unchanged(before, other_changes=True)
        self.assertFalse(self.other_is_valid(st), "the other slot reads as valid: %s" % st["fwOther"])
        self.assertTrue(st["fwLastUpdate"].startswith("rejected"), st["fwLastUpdate"])

    def test_aborted_upload_leaves_device_running(self):
        before = self.ensure_other_valid()
        w = self.web()
        token = self.csrf(w)
        total = len(self.upd["normal"])
        s = socket.create_connection((self.ip, 80), timeout=10)
        s.sendall(("POST /firmware/upload HTTP/1.1\r\nHost: %s\r\nCookie: %s\r\n"
                   "Content-Type: application/octet-stream\r\nX-CSRF-Token: %s\r\n"
                   "Content-Length: %d\r\n\r\n" % (self.ip, w.cookie, token, total)).encode())
        # 100 KB of the part for the slot being written (the .upd starts with the other slot's part,
        # which the device skips: cutting inside it would not have touched the slot)
        head = fw_release.parse_upd_header(self.upd["normal"])
        target = 1 if before["fwSlot"] == "A" else 0
        s.sendall(self.upd["normal"][:head["slotOffset"][target] + 100 * 1024])
        s.close()
        st = self.await_status(lambda x: not self.other_is_valid(x) and x["fwOther"] != "upload in progress", 30)
        self.assert_unchanged(before, other_changes=True)
        self.assertIn("empty", st["fwOther"])
        # A full upload afterwards works
        done = self.install()
        self.assertEqual(self.version, done["fwVersion"])

    def test_upload_during_trial_is_refused(self):
        before = self.kept_state()
        t0 = time.time()
        self.assertIn(self.upload(self.upd["shorttrial"])[0], (200, None))
        self.await_status(lambda s: s["fwSlot"] != before["fwSlot"] and s["fwState"] == "trial", 90)
        # The shorttrial firmware never confirms: logging in and asking is not a valid request for it.
        # A full-size body: the refusal comes before the body, which the device reads and drops so
        # the answer is not lost in a reset connection
        code, text = self.upload(self.upd["normal"])
        self.assertEqual(409, code, text)
        self.assertEqual("trial", self.status()["fwState"])
        back = self.await_status(lambda s: s["fwSlot"] == before["fwSlot"] and s["fwState"] == "kept",
                                 UNREACHABLE_ROLLBACK_LIMIT_S)
        self.assertEqual(before["fwSlot"], back["fwSlot"])
        self.assertEqual("empty", self.await_status(lambda s: s["fwOther"] == "empty", 20)["fwOther"])
        print("\nrefused during trial, rolled back after %.0f s" % (time.time() - t0))

    def test_refused_full_size_upload_gets_its_answer(self):
        # An expired session: refused after the headers while the client still sends 0.8 MB
        before = self.kept_state()
        w = self.web()
        token = self.csrf(w)
        w.cookie = "sid=" + "0" * 32
        code, _, text = w.request("POST", "/firmware/upload", self.upd["normal"],
                                  {"Content-Type": "application/octet-stream", "X-CSRF-Token": token})
        self.assertEqual(401, code, text)
        self.assertEqual("not logged in", text.strip())
        self.assert_unchanged(before)

    def test_trial_crash_rolls_back(self):
        before = self.kept_state()
        self.trap_clear()
        self.assertIn(self.upload(self.upd["crash"])[0], (200, None))
        t0 = time.time()
        st = self.await_status(lambda s: s["fwSlot"] == before["fwSlot"] and "rolled back" in (s["fwLastUpdate"] or ""),
                               CRASH_ROLLBACK_LIMIT_S + 60)
        took = time.time() - t0
        print("\ncrash rollback after %.0f s" % took)
        self.assertLess(took, CRASH_ROLLBACK_LIMIT_S)
        self.assertIn("rolled back (crash)", st["fwLastUpdate"])
        self.assertEqual("kept", st["fwState"])
        # The release that failed is forgotten: a later fallback can never start it
        self.assertEqual("empty", st["fwOther"])
        self.assertEqual("2", snmp_value(self.ip, OID_FW + "5.0"), "fwLastResult rolledBackCrash(2)")
        # The rollback boot sends coldStart, and the firmware trap is held until after it
        self.expect_trap("coldStart", 30)
        self.expect_trap("firmwareRolledBack", 30, last_result=2)

    def test_trial_unreachable_rolls_back(self):
        before = self.kept_state()
        self.trap_clear()
        self.assertIn(self.upload(self.upd["shorttrial"])[0], (200, None))
        t0 = time.time()
        st = self.await_status(lambda s: s["fwSlot"] == before["fwSlot"] and "rolled back" in (s["fwLastUpdate"] or ""),
                               UNREACHABLE_ROLLBACK_LIMIT_S + 60)
        took = time.time() - t0
        print("\nunreachable rollback after %.0f s" % took)
        self.assertLess(took, UNREACHABLE_ROLLBACK_LIMIT_S)
        self.assertIn("rolled back (unreachable)", st["fwLastUpdate"])
        self.assertEqual("kept", st["fwState"])
        self.assertEqual("empty", self.await_status(lambda s: s["fwOther"] == "empty", 20)["fwOther"])
        self.expect_trap("firmwareRolledBack", 30, last_result=3)

    def test_kept_firmware_crash_loop_falls_back(self):
        before = self.ensure_other_valid()
        self.trap_clear()
        # No 5 healthy minutes in between: the crash counter keeps counting
        for _ in range(3):
            self.dev.send("stall0", expect=r"Stalling core 0")
            self.dev.close()
            time.sleep(12)
            self.reconnect()
        st = self.await_status(lambda s: s["fwSlot"] != before["fwSlot"], 60)
        self.assertTrue(st["fwLastUpdate"].startswith("fallback (crash)"), st["fwLastUpdate"])
        self.assertEqual("kept", st["fwState"])
        self.assertEqual("4", snmp_value(self.ip, OID_FW + "5.0"), "fwLastResult fallbackCrash(4)")
        self.expect_trap("firmwareRolledBack", 40, last_result=4)

    def test_manual_switch_runs_other_slot_on_trial(self):
        before = self.ensure_other_valid()
        w = self.web()
        token = self.csrf(w)
        code, _, text = w.request("POST", "/firmware/switch", "confirm=yes&csrf=" + token,
                                  {"Content-Type": "application/x-www-form-urlencoded"})
        self.assertEqual(200, code, text[:200])
        trial = self.await_status(self.flipped(before), 60)
        self.assertEqual(before["fwOther"], trial["fwVersion"])
        self.web()
        kept = self.await_status(lambda s: s["fwState"] == "kept", 40)
        self.assertEqual(trial["fwSlot"], kept["fwSlot"])

    def test_core0_stack_headroom_after_upload(self):
        # A full upload that fails at the end runs the whole receive, flash and verify path without
        # restarting, so the stack high-water mark of this boot still covers it
        before = self.ensure_other_valid()
        self.upload(self.corrupted_image(before))
        free = self.status()["stackNeverUsed"]
        self.assertIsNotNone(free)
        self.assertGreater(free, STACK_MIN_NEVER_USED, "core 0 stack headroom only %d bytes" % free)

    @unittest.skipUnless(POE_CYCLE and SWITCH_CONSOLE,
                         "set RFPOWER_POE_CYCLE=1 and RFPOWER_LLDP_SWITCH_CONSOLE (changes PoE on the switch port)")
    def test_power_cycles_during_trial_do_not_roll_back(self):
        # Needs the switch console (secret/switch.md). Only PoE on the device's own port is changed,
        # with commit confirmed, and restored by the test. With USB attached the Pico may stay
        # powered, so this then exercises PoE and link loss rather than a restart.
        from switch_console import SwitchConsole
        before = self.kept_state()
        m = re.search(r", port (\S+?)(\.\d+)?$", before["switch"] or "")
        if not m:
            self.skipTest("the device has not seen its switch port over LLDP: %s" % before["switch"])
        port = m.group(1)
        with SwitchConsole(port=SWITCH_CONSOLE) as sw:
            learned, own = poe_baseline(sw, port, before["mac"])
        if not learned:
            self.skipTest("the switch does not list the device's MAC on %s, PoE is not changed" % port)
        if any("disable" in line for line in own):
            self.skipTest("PoE is already disabled on %s: %s" % (port, own))
        restore = ["delete poe interface %s" % port] + own  # exactly what the port had before
        self.assertIn(self.upload(self.upd["normal"])[0], (200, None))
        self.await_status(self.flipped(before), 90)
        try:
            for _ in range(4):
                with SwitchConsole(port=SWITCH_CONSOLE) as sw:
                    sw.configure(["set poe interface %s disable" % port], confirm_minutes=5)
                time.sleep(8)
                with SwitchConsole(port=SWITCH_CONSOLE) as sw:
                    sw.configure(restore)
                self.reconnect()
        finally:
            # Also when a step failed: put the port back (plain commit, which ends commit confirmed)
            with SwitchConsole(port=SWITCH_CONSOLE) as sw:
                _, now = poe_baseline(sw, port, before["mac"])
                if now != own:
                    sw.configure(restore)
        st = self.status()
        self.assertNotEqual(before["fwSlot"], st["fwSlot"], "rolled back by the power cycles")
        self.assertNotIn("rolled back", st["fwLastUpdate"])
        self.assertIn(st["fwState"], ("trial", "kept"))
        self.web()
        self.await_status(lambda s: s["fwState"] == "kept", 40)


if __name__ == "__main__":
    unittest.main(verbosity=2)
