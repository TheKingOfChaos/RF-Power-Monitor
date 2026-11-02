"""Hardware-in-the-loop test for the factory reset button (GPIO22). Manual: you press the button.

Run:
    set RFPOWER_MANUAL_BUTTON=1
    %USERPROFILE%\\.platformio\\penv\\Scripts\\python.exe -m unittest discover -s test/hil -p "test_factory_reset.py" -v

The test asks you to press the button twice: a short press that must not
reset, then a 10+ second hold that must. Watch the LED: it blinks slowly
while the button is held and fast once the reset triggers. Your SNMP
settings and stored static IP are restored afterwards.
"""

import os
import re
import time
import unittest

from device import Device
from snmp_tools import GET_SYSDESCR, raw_get

# Community set before the hold; as long as "public", so the captured request only needs the name swapped
RESET_COMMUNITY = "hilrst"


@unittest.skipUnless(os.environ.get("RFPOWER_MANUAL_BUTTON"),
                     "manual: set RFPOWER_MANUAL_BUTTON=1 and press the reset button when asked")
class FactoryResetTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.dev = Device()
        cls.dev.open()
        cls.dev.wait_for_menu()
        cls.before = cls.dev.wait_connected()
        if cls.before["mode"] != "DHCP":
            cls.dev.close()
            raise unittest.SkipTest("device must be in DHCP mode")
        raw = cls.before["raw"]
        m = re.search(r"Stored static IP: ([\d.]+) / ([\d.]+), gateway ([\d.]+)", raw)
        cls.stored_static = m.groups() if m else None

    @classmethod
    def tearDownClass(cls):
        cls.dev.close()

    def _wait_log(self, pattern, timeout, poll_snmp=None):
        """Read the serial log until pattern; meanwhile check SNMP answers with community poll_snmp, if given."""
        deadline = time.time() + timeout
        buf, snmp_ok, snmp_tries = "", 0, 0
        regex = re.compile(pattern)
        while time.time() < deadline:
            try:
                buf += self.dev.read_until(r"[\s\S]+", timeout=0.3)
            except TimeoutError:
                pass
            m = regex.search(buf)
            if m:
                return m, buf, snmp_ok, snmp_tries
            if poll_snmp:
                snmp_tries += 1
                packet = GET_SYSDESCR.replace(b"public", poll_snmp.encode())
                if raw_get(self.before["ip"], snmp_tries, timeout=0.3, packet=packet)[0] is not None:
                    snmp_ok += 1
        self.fail("not seen within %d s: %r\n%s" % (timeout, pattern, buf[-2000:]))

    def test_1_short_press_does_not_reset_and_device_keeps_working(self):
        print("\n>>> Press and HOLD the reset button for about 3 seconds, then release (30 s)...",
              flush=True)
        self._wait_log(r"Factory reset button pressed", 30)
        m, log, snmp_ok, tries = self._wait_log(r"released after (\d+) ms, no reset", 20, poll_snmp="public")
        held = int(m.group(1))
        self.assertGreater(held, 1000)
        self.assertLess(held, 10000)
        self.assertNotIn("Factory reset after", log)
        # Non-blocking: SNMP answered while the button was held
        self.assertGreater(tries, 0)
        self.assertGreaterEqual(snmp_ok, tries - 1, "SNMP stopped answering while the button was held")
        self.assertEqual(self.before["sysName"], self.dev.status()["sysName"], "settings changed")

    def test_2_ten_second_hold_resets_to_defaults(self):
        d = self.dev
        try:
            # Non-default values so the reset is visible
            d.set_snmp(RESET_COMMUNITY, "hil-before-reset", "x@example.com", "bench")
            print("\n>>> Press and HOLD the reset button for 12 seconds; the LED blinks fast at 10 s (40 s)...",
                  flush=True)
            self._wait_log(r"Factory reset button pressed", 40)
            m, log, snmp_ok, tries = self._wait_log(r"Factory reset after (\d+) ms hold", 20,
                                                    poll_snmp=RESET_COMMUNITY)
            held = int(m.group(1))
            self.assertGreaterEqual(held, 10000)
            self.assertLessEqual(held, 10100, "reset fired late")
            self.assertGreaterEqual(snmp_ok, tries - 1, "SNMP stopped answering during the hold")
            m, log, _, _ = self._wait_log(r"Factory defaults saved in (\d+) ms, rebooting", 5)
            self.assertLess(int(m.group(1)), 2000, ".clinerules: factory reset must complete within 2 s")
            print(">>> You can release the button now.", flush=True)

            d.reconnect_after_reboot()
            st = d.status()
            self.assertEqual("DHCP", st["mode"])
            self.assertEqual("public", st["community"])
            self.assertEqual("RF-Power-" + st["mac"].replace(":", "")[-6:], st["sysName"])  # the default name
            self.assertEqual("", st["sysContact"])
            self.assertEqual("", st["sysLocation"])
            self.assertEqual("", st["trapTarget"])
            self.assertEqual("2c", st["trapVersion"])
            self.assertRegex(st["raw"], r"Stored static IP: 192\.168\.1\.100 / 255\.255\.255\.0, gateway 192\.168\.1\.1")
        finally:
            self._restore()

    def _restore(self):
        d, b = self.dev, self.before
        # Also after a failed check: the reset may have rebooted the device and dropped the port
        d.reconnect_after_reboot()
        d.set_snmp(b["community"], b["sysName"], b["sysContact"], b["sysLocation"], b["trapTarget"], b["trapVersion"])
        if self.stored_static and not re.search(
                r"Stored static IP: %s / %s, gateway %s" % tuple(map(re.escape, self.stored_static)),
                d.status()["raw"]):
            # Stored static values can only be set via option 3 (reboots into static), then back to DHCP
            ip, mask, gw = self.stored_static
            d.send("3", expect=r"IP address .*: $")
            d.send(ip, expect=r"Subnet mask .*: $")
            d.send(mask, expect=r"Gateway .*: $")
            d.send(gw, expect=r"\(y/n\): $")
            d.send("y", expect=r"Rebooting", timeout=10)
            d.reconnect_after_reboot()
            d.send("2", expect=r"\(y/n\): $")
            d.send("y", expect=r"Rebooting", timeout=10)
            d.reconnect_after_reboot()


if __name__ == "__main__":
    unittest.main(verbosity=2)
