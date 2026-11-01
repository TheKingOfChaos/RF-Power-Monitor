"""HIL tests: LLDP, the device announcing itself and showing the switch it is plugged into.

Opt-in, since another network may have no LLDP switch:
    RFUPS_LLDP_SWITCH=1               the device is plugged into a switch that sends LLDP
    RFUPS_LLDP_SWITCH_CONSOLE=COM45   that switch's serial console (Junos), to read its neighbour list;
                                      login in secret/switch.md (see switch_console.py)

Run: python -m unittest discover -s test/hil -p "test_lldp.py" -v
"""

import os
import re
import time
import unittest

from device import Device

LISTEN = os.environ.get("RFUPS_LLDP_SWITCH") == "1"
CONSOLE = os.environ.get("RFUPS_LLDP_SWITCH_CONSOLE")
if CONSOLE:
    from switch_console import SwitchConsole

TEST_HOSTNAME = "rfups-lldp-test"


class LldpTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dev = Device()
        cls.dev.open()
        cls.dev.wait_for_menu()
        # Right after boot the network messages ("Network up ...") would land in the middle of a menu dialog
        cls.dev.wait_connected(timeout=60)

    @classmethod
    def tearDownClass(cls):
        cls.dev.close()

    def neighbour_detail(self, sw, mac):
        """The switch's detailed view of the device: (interface, detail text), or (None, listing)"""
        listing = sw.run("show lldp neighbors")
        for line in listing.split("\n"):
            if mac in line:
                interface = line.split()[0]
                return interface, sw.run("show lldp neighbors interface %s" % interface)
        return None, listing

    @unittest.skipUnless(LISTEN, "set RFUPS_LLDP_SWITCH=1 when the device is plugged into an LLDP switch")
    def test_status_shows_the_switch(self):
        # The switch sends every 30 s; allow one interval and a bit
        deadline = time.time() + 35
        while True:
            st = self.dev.status()
            if st["switch"] not in (None, "none seen") or time.time() > deadline:
                break
            time.sleep(3)
        self.assertRegex(st["switch"] or "", r"^\S.*, port \S+", st["raw"])

    @unittest.skipUnless(CONSOLE, "set RFUPS_LLDP_SWITCH_CONSOLE to the switch's console port")
    def test_switch_lists_the_device(self):
        st = self.dev.status()
        mac = st["mac"].lower()
        with SwitchConsole(port=CONSOLE) as sw:
            interface, detail = self.neighbour_detail(sw, mac)
        self.assertIsNotNone(interface, "device MAC %s not in the switch's LLDP neighbours:\n%s" % (mac, detail))
        self.assertRegex(detail, r"System name\s*: %s\s" % re.escape(st["hostname"]))
        self.assertRegex(detail, r"Port description\s*: W5500 10/100 Ethernet")
        self.assertRegex(detail, r"Address\s*: %s\s" % re.escape(st["ip"]))
        # The device shows the port the switch says it is on
        if LISTEN:
            self.assertIn(", port %s" % interface, self.dev.status()["switch"])

    @unittest.skipUnless(CONSOLE, "set RFUPS_LLDP_SWITCH_CONSOLE to the switch's console port")
    def test_switch_learns_new_hostname_within_5_s(self):
        d = self.dev
        before = d.status()
        mac = before["mac"].lower()
        try:
            with SwitchConsole(port=CONSOLE) as sw:  # log in first: that takes a few seconds itself
                d.set_snmp(before["community"], TEST_HOSTNAME, before["sysContact"], before["sysLocation"])
                changed = time.time()
                while True:
                    _, detail = self.neighbour_detail(sw, mac)
                    if re.search(r"System name\s*: %s\s" % TEST_HOSTNAME, detail):
                        break
                    self.assertLess(time.time() - changed, 5, "switch still shows the old name:\n" + detail)
                    time.sleep(0.5)
        finally:
            d.wait_for_menu(timeout=10)
            d.set_snmp(before["community"], before["sysName"], before["sysContact"], before["sysLocation"])
        self.assertEqual(before["hostname"], d.status()["hostname"])


if __name__ == "__main__":
    unittest.main()
