"""Hardware-in-the-loop tests for the hardware watchdog.

The hidden menu commands "stall0" and "stall1" hang core 0 or core 1 for good;
the watchdog must then restart the device, and the status screen must say so.
Each test restarts the device once; settings are not touched.

Run:
    %USERPROFILE%\\.platformio\\penv\\Scripts\\python.exe -m unittest discover -s test/hil -p "test_watchdog.py" -v
"""

import time
import unittest

import serial

from device import Device

WATCHDOG_S = 8.0     # WATCHDOG_TIMEOUT_MS
CORE1_STALE_S = 1.0  # CoreHeartbeatCheck::DEFAULT_STALE_MS


class WatchdogTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.dev = Device()
        cls.dev.open()
        cls.dev.wait_for_menu()

    @classmethod
    def tearDownClass(cls):
        cls.dev.close()

    def setUp(self):
        try:
            self.dev.wait_for_menu(timeout=40)
        except (serial.SerialException, OSError):
            self.dev.reconnect_after_reboot()  # an earlier test left the device restarting

    def wait_for_restart(self, timeout):
        """Seconds until the USB port drops (the chip restarted), or None if it stays up."""
        start = time.time()
        while time.time() - start < timeout:
            try:
                self.dev.ser.read(256)
            except (serial.SerialException, OSError):
                return time.time() - start
        return None

    def stall_and_check(self, core):
        before = self.dev.status()["watchdogRestarts"]
        self.assertIsNotNone(before, "status screen has no 'Last restart' line")
        out = self.dev.send("stall%d" % core, expect=r"Stalling core %d" % core)
        self.assertIn("watchdog restarts the device", out)
        took = self.wait_for_restart(timeout=WATCHDOG_S + CORE1_STALE_S + 5)
        self.assertIsNotNone(took, "core %d stalled but the device did not restart" % core)
        # core 0: the watchdog timeout; core 1: core 0 first notices the stale heartbeat
        self.assertLess(took, WATCHDOG_S + (CORE1_STALE_S if core == 1 else 0) + 2)
        self.dev.reconnect_after_reboot()
        st = self.dev.status()
        self.assertEqual("watchdog", st["lastRestart"])
        self.assertEqual(before + 1, st["watchdogRestarts"])

    def test_stalled_core0_is_restarted_by_the_watchdog(self):
        self.stall_and_check(0)

    def test_stalled_core1_is_restarted_by_the_watchdog(self):
        self.stall_and_check(1)

    def test_normal_reboot_is_not_counted_as_watchdog(self):
        st = self.dev.status()
        if st["mode"] != "DHCP":
            self.skipTest("uses option 2 (DHCP, unchanged) to reboot; device is in static mode")
        before = st["watchdogRestarts"]
        self.dev.send("2", expect=r"Save and reboot\? \(y/n\): $")
        self.dev.send("y", expect=r"Rebooting", timeout=10)
        self.dev.reconnect_after_reboot()
        st = self.dev.status()
        self.assertEqual("reboot", st["lastRestart"])
        self.assertEqual(before, st["watchdogRestarts"])

    def test_device_keeps_running_with_the_watchdog_armed(self):
        # Twice the watchdog timeout: normal operation must keep feeding it, so the port never drops
        before = self.dev.status()["watchdogRestarts"]
        self.assertIsNone(self.wait_for_restart(timeout=WATCHDOG_S * 2), "the device restarted on its own")
        self.assertEqual(before, self.dev.status()["watchdogRestarts"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
