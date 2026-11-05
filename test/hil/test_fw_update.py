"""Hardware-in-the-loop tests for the boot program and firmware slots.

The boot program (boot/) verifies the firmware slot before it starts it and
tells the firmware what it did; the firmware prints it early in setup():
"Boot: slot A verified in <n> ms (normal|trial|fallback|no record)".
These tests reboot the device once (menu option 2, DHCP unchanged) and read
the boot log; settings are not touched.

Run:
    %USERPROFILE%\\.platformio\\penv\\Scripts\\python.exe -m unittest discover -s test/hil -p "test_fw_update.py" -v
"""

import re
import time
import unittest

from device import Device

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

    def test_boot_program_starts_slot_a_after_menu_reboot(self):
        m = self.boot_line()
        self.assertEqual("A", m.group(1))
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


if __name__ == "__main__":
    unittest.main(verbosity=2)
