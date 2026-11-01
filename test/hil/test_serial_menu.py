"""Hardware-in-the-loop tests for the serial network menu.

Runs against real firmware on the Pico over USB serial, with the W5500 on a
network that has a DHCP server.

Run (from the project root, firmware already flashed, PuTTY closed):
    %USERPROFILE%\\.platformio\\penv\\Scripts\\python.exe -m unittest discover -s test/hil -v

Environment variables:
    RFPOWER_PORT        Serial port, default COM16
    RFPOWER_STATIC_IP   Free address for the static IP test, e.g. 10.10.6.250
    RFPOWER_STATIC_MASK Subnet mask for that test (default 255.255.0.0)
    RFPOWER_STATIC_GW   Gateway for that test (default: the current DHCP gateway)
    RFPOWER_MAINS_SWITCH / RFPOWER_MANUAL_MAINS  Mains tests, see mains_switch.py

The device must be in DHCP mode before the run, and is left in DHCP mode.
"""

import os
import re
import time
import unittest

from device import Device, ping
from mains_switch import get_switch, skip_without_switch


class SerialMenuTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.dev = Device()
        cls.dev.open()
        cls.dev.wait_for_menu()
        st = cls.dev.wait_connected()
        if st["mode"] != "DHCP":
            cls.dev.close()
            raise unittest.SkipTest("device is in %s mode; switch it to DHCP before running HIL tests" % st["mode"])
        cls.dhcp_status = st

    @classmethod
    def tearDownClass(cls):
        cls.dev.close()

    def setUp(self):
        # Every test starts at the main menu
        self.dev.wait_for_menu(timeout=10)

    # ---- read-only ----

    def test_menu_lists_all_options(self):
        out = self.dev.wait_for_menu()
        for text in ("1. Show status", "2. Use DHCP", "3. Set static IP", "4. SNMP settings",
                     "5. Access log", "6. Web password", "7. New default web password", "8. Web interface on/off", "?. Show this menu"):
            self.assertIn(text, out)

    def test_status_reports_dhcp_lease(self):
        st = self.dev.status()
        self.assertEqual("DHCP", st["mode"])
        self.assertIsNotNone(st["ip"], st["raw"])
        self.assertNotEqual("0.0.0.0", st["ip"], "no DHCP lease")
        self.assertRegex(st["raw"], r"MAC: ([0-9A-F]{2}:){5}[0-9A-F]{2}")

    def test_status_reports_connected_state(self):
        self.assertEqual("connected", self.dev.status()["state"])

    def test_status_reports_mains_power(self):
        st = self.dev.status()
        self.assertIn(st["mains"], ("present", "LOST"), st["raw"])
        self.assertRegex(st["raw"],
                         r"Mains power: \w+ \((no changes since boot|\d+ changes, last change \d+ s ago)\)")

    @skip_without_switch
    def test_mains_loss_and_restore_are_reported(self):
        d = self.dev
        switch = get_switch()
        self.assertEqual("present", d.status()["mains"], "mains must be on when the test starts")
        try:
            switch.off()
            d.read_until(r"Mains power LOST at \d+ ms", timeout=60)
            d.wait_for_menu(timeout=5)
            self.assertEqual("LOST", d.status()["mains"])
            switch.on()
            d.read_until(r"Mains power RESTORED at \d+ ms", timeout=60)
        finally:
            switch.restore()
        d.wait_for_menu(timeout=5)
        st = d.status()
        self.assertEqual("present", st["mains"])
        self.assertRegex(st["raw"], r"Mains power: present \(\d+ changes, last change \d+ s ago\)")

    def test_device_answers_ping(self):
        ip = self.dev.status()["ip"]
        self.assertTrue(ping(ip), "no ping reply from %s" % ip)

    def test_unknown_choice_shows_menu(self):
        out = self.dev.send("x", expect=r"\?\. Show this menu\r?\n> ")
        self.assertIn("=== RF-Power-Monitor Menu ===", out)

    def test_backspace_edits_input(self):
        # "9<BS>1" must be read as "1"
        out = self.dev.send("9\b1", expect=r"DNS: .*\r?\n> ")
        self.assertIn("Mode: DHCP", out)

    # ---- static entry validation, always cancelled ----

    def test_invalid_entries_are_rejected_and_cancel_keeps_settings(self):
        d = self.dev
        d.send("3", expect=r"IP address \[[\d.]+\]: $")
        out = d.send("999.1.1.1", expect=r"IP address \[[\d.]+\]: $")
        self.assertIn("is not a valid IPv4 address", out)

        d.send("10.10.6.200", expect=r"Subnet mask \[[\d.]+\]: $")
        out = d.send("255.0.255.0", expect=r"Subnet mask \[[\d.]+\]: $")
        self.assertIn("Subnet mask must be contiguous", out)
        self.assertNotIn("not a valid IPv4", out, "bad mask must report one error")

        d.send("255.255.0.0", expect=r"Gateway \[[\d.]+\]: $")
        out = d.send("192.168.1.1", expect=r"Gateway \[[\d.]+\]: $")
        self.assertIn("not in the same subnet", out)

        out = d.send("10.10.6.200", expect=r"Gateway \[[\d.]+\]: $")
        self.assertIn("must differ from the IP address", out)

        out = d.send("10.10.0.1", expect=r"Save and reboot\? \(y/n\): $")
        self.assertIn("New mode: Static 10.10.6.200 / 255.255.0.0, gateway 10.10.0.1", out)

        d.send("n", expect=r"Cancelled[\s\S]*> $")
        self.assertEqual("DHCP", d.status()["mode"], "cancel must not change settings")

    def test_network_address_restarts_entry(self):
        d = self.dev
        d.send("3", expect=r"IP address \[[\d.]+\]: $")
        d.send("10.10.0.0", expect=r"Subnet mask \[[\d.]+\]: $")
        d.send("255.255.0.0", expect=r"Gateway \[[\d.]+\]: $")
        out = d.send("10.10.0.1", expect=r"IP address \[[\d.]+\]: $")
        self.assertIn("network or broadcast address", out)
        # Abort by entering a valid config and declining
        d.send("10.10.6.200", expect=r"Subnet mask")
        d.send("255.255.0.0", expect=r"Gateway")
        d.send("10.10.0.1", expect=r"\(y/n\): $")
        d.send("n", expect=r"Cancelled[\s\S]*> $")

    # ---- SNMP settings entry, always cancelled ----

    def test_status_shows_snmp_settings(self):
        st = self.dev.status()
        for key in ("community", "sysName", "sysContact", "sysLocation"):
            self.assertIsNotNone(st[key], "%s missing from status:\n%s" % (key, st["raw"]))
        self.assertNotEqual("", st["community"])

    def test_snmp_settings_are_validated_and_cancel_keeps_them(self):
        d = self.dev
        before = d.status()
        d.send("4", expect=r"Community \[.*\]: $")

        out = d.send("-", expect=r"Community \[.*\]: $")
        self.assertIn("Must not be empty", out)
        out = d.send("c" * 32, expect=r"Community \[.*\]: $")
        self.assertIn("Too long, at most 31 characters", out)
        d.send("c" * 31, expect=r"\(sysName\) \[.*\]: $")             # exactly the maximum

        # The sysName is also the hostname: one DNS label of at most 32 characters
        for bad in ("n" * 33, "ups rack", "-", "ups.local", "-ups", "ups_4"):
            out = d.send(bad, expect=r"\(sysName\) \[.*\]: $")
            self.assertIn("Not a valid hostname", out, bad)
        out = d.send("x" * 100, expect=r"\(sysName\) \[.*\]: $")      # longer than the input line
        self.assertIn("Input too long", out)
        d.send("", expect=r"sysContact \[.*\]: $")                  # Enter keeps sysName
        d.send("-", expect=r"sysLocation \[.*\]: $")                # '-' clears sysContact
        d.send('Rack "4", \\shelf', expect=r"Trap destination IP or dhcp \[.*\]: $")
        out = d.send("10.10.6.300", expect=r"Trap destination IP or dhcp \[.*\]: $")
        self.assertIn("not a valid IPv4 or IPv6 address", out)
        d.send("10.10.6.99", expect=r"Trap version \(1/2c\) \[.*\]: $")
        out = d.send("3", expect=r"Trap version \(1/2c\) \[.*\]: $")
        self.assertIn("Enter 1 or 2c", out)
        out = d.send("1", expect=r"Save\? \(y/n\): $")
        self.assertIn('community "%s", sysName "%s", sysContact "", sysLocation "Rack "4", \\shelf", '
                      'trap destination "10.10.6.99", trap version v1' % ("c" * 31, before["sysName"]), out)

        d.send("n", expect=r"Cancelled[\s\S]*> $")
        after = d.status()
        for key in ("community", "sysName", "sysContact", "sysLocation", "trapTarget", "trapVersion"):
            self.assertEqual(before[key], after[key], "cancel changed %s" % key)

    # ---- changes that reboot the device ----

    def test_dhcp_save_reboots_and_reconnects(self):
        d = self.dev
        d.send("2", expect=r"New mode: DHCP\r?\nSave and reboot\? \(y/n\): $")
        out = d.send("y", expect=r"Rebooting", timeout=10)
        self.assertIn("Settings: saved slot", out)

        d.reconnect_after_reboot()
        st = d.status()
        self.assertEqual("DHCP", st["mode"])
        self.assertNotEqual("0.0.0.0", st["ip"])
        self.assertTrue(ping(st["ip"]), "no ping reply after reboot")

    def test_menu_is_available_before_dhcp_lease(self):
        """Boot must not wait for DHCP: the prompt comes first, the lease is reported later."""
        d = self.dev
        d.send("2", expect=r"\(y/n\): $")
        d.send("y", expect=r"Rebooting", timeout=10)

        d.close()
        time.sleep(1.5)
        d.open()  # Firmware waits for DTR before printing, so the boot log is captured
        opened = time.time()
        boot = d.read_until(r"> $", timeout=8)
        prompt_after = time.time() - opened
        self.assertIn("requesting DHCP lease in background", boot)
        self.assertNotIn("Network up", boot, "lease reported before the menu, boot is still blocking")
        self.assertLess(prompt_after, 5.0, "menu took %.1f s to appear" % prompt_after)

        # Menu works while the lease is pending, and the lease still arrives
        out = d.send("1", expect=r"DNS: .*\r?\n> ")
        self.assertRegex(out, r"State: (link up, waiting for DHCP lease|no Ethernet link|connected)")
        d.read_until(r"Network up after \d+ ms", timeout=30)
        self.assertEqual("connected", d.wait_connected(timeout=5)["state"])

    @unittest.skipUnless(os.environ.get("RFPOWER_STATIC_IP"), "set RFPOWER_STATIC_IP to a free address to run")
    def test_static_ip_applies_after_reboot(self):
        d = self.dev
        ip = os.environ["RFPOWER_STATIC_IP"]
        mask = os.environ.get("RFPOWER_STATIC_MASK", "255.255.0.0")
        gw = os.environ.get("RFPOWER_STATIC_GW", self.dhcp_status["gateway"])
        try:
            d.send("3", expect=r"IP address .*: $")
            d.send(ip, expect=r"Subnet mask .*: $")
            d.send(mask, expect=r"Gateway .*: $")
            d.send(gw, expect=r"\(y/n\): $")
            d.send("y", expect=r"Rebooting", timeout=10)

            d.reconnect_after_reboot()
            st = d.status()
            self.assertEqual("Static", st["mode"])
            self.assertEqual(ip, st["ip"])
            self.assertEqual(mask, st["subnet"])
            self.assertTrue(ping(ip), "no ping reply on static IP %s" % ip)
        finally:
            # Always restore DHCP
            d.wait_for_menu()
            if d.status()["mode"] != "DHCP":
                d.send("2", expect=r"\(y/n\): $")
                d.send("y", expect=r"Rebooting", timeout=10)
                d.reconnect_after_reboot()
            self.assertEqual("DHCP", d.status()["mode"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
