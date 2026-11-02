"""HIL tests: the "no link" and "no DHCP server" states, made by changing the device's switch port.

Opt-in, since they change the switch's configuration:
    RFPOWER_SWITCH_CONFIG=1             allow these tests to change the device's switch port
    RFPOWER_LLDP_SWITCH_CONSOLE=COM45   the switch's serial console (Junos); login in secret/switch.md

The port is the one the device names over LLDP, and only used if the switch also lists the device's
MAC on it. Only that port is changed (disabled, or given a filter that drops DHCP) plus the test's own
filter. Every change is committed with "commit confirmed", so Junos undoes it by itself if a test dies;
the test restores the port itself at the end. The device stays powered over USB, so the serial menu
works while the port is down.

Run: python -m unittest discover -s test/hil -p "test_switch_port.py" -v
"""

import os
import re
import time
import unittest

from device import Device
from snmp_tools import net_snmp

CONSOLE = os.environ.get("RFPOWER_LLDP_SWITCH_CONSOLE")
ENABLED = os.environ.get("RFPOWER_SWITCH_CONFIG") == "1" and bool(CONSOLE)
if ENABLED:
    from switch_console import SwitchConsole

CONFIRM_MINUTES = 5  # Junos rolls a change back after this if the test cannot restore it
TEST_FILTER = "rfpower-test-no-dhcp"
SYS_UPTIME = "1.3.6.1.2.1.1.3.0"


@unittest.skipUnless(ENABLED, "set RFPOWER_SWITCH_CONFIG=1 and RFPOWER_LLDP_SWITCH_CONSOLE to change the switch port")
class SwitchPortTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dev = Device()
        cls.dev.open()
        cls.dev.wait_for_menu()
        st = cls.dev.wait_connected(timeout=60)
        m = re.search(r", port (\S+?)(\.\d+)?$", st["switch"] or "")
        if not m:
            raise unittest.SkipTest("the device has not seen its switch port over LLDP: %s" % st["switch"])
        cls.port = m.group(1)
        with SwitchConsole(port=CONSOLE) as sw:
            listing = sw.run("show lldp neighbors")
            if not re.search(r"^%s(\.\d+)?\s.*%s" % (re.escape(cls.port), re.escape(st["mac"].lower())),
                             listing, re.MULTILINE):
                raise RuntimeError("the switch does not list the device on %s, refusing to change it:\n%s"
                                   % (cls.port, listing))
            out = sw.run("show configuration interfaces %s unit 0 family ethernet-switching filter" % cls.port)
        m = re.search(r"^\s*input (\S+);", out, re.MULTILINE)
        cls.filter = m.group(1) if m else None

    @classmethod
    def tearDownClass(cls):
        cls.dev.close()

    def change_port(self, statements):
        with SwitchConsole(port=CONSOLE) as sw:
            sw.configure(statements, confirm_minutes=CONFIRM_MINUTES)

    def restore_port(self, statements):
        """Undo the change with a plain commit, which also confirms the commit confirmed before it"""
        with SwitchConsole(port=CONSOLE) as sw:
            sw.configure(statements)

    def wait_for_state(self, state, timeout, link_up=None):
        """Poll the status screen until State is state (and the Ethernet line's link, if given)"""
        deadline = time.time() + timeout
        while True:
            st = self.dev.status()
            if (st["state"] == state and link_up in (None, st["linkUp"])) or time.time() > deadline:
                return st
            time.sleep(1)

    def assert_answers_snmp(self, st):
        code, out = net_snmp("snmpget", st["ip"], SYS_UPTIME, community=st["community"], retries=2)
        self.assertEqual(0, code, out)

    def test_no_link_shows_on_the_status_screen_and_recovers(self):
        try:
            self.change_port(["set interfaces %s disable" % self.port])
            # State reads the PHY when printed; the Ethernet line shows the 500 ms PHY poll, so it may lag a moment
            st = self.wait_for_state("no Ethernet link, check cable", timeout=10, link_up=False)
            self.assertEqual("no Ethernet link, check cable", st["state"], st["raw"])
            self.assertFalse(st["linkUp"], st["raw"])
        finally:
            self.restore_port(["delete interfaces %s disable" % self.port])
        # Link back: the device keeps its lease and answers again without a reboot
        st = self.dev.wait_connected(timeout=60)
        self.assert_answers_snmp(st)

    def test_no_dhcp_server_keeps_waiting_and_recovers_without_reboot(self):
        if self.dev.status()["mode"] != "DHCP":
            self.skipTest("the device uses a static address")
        prefix = "set firewall family ethernet-switching filter %s " % TEST_FILTER
        restore = ["delete interfaces %s unit 0 family ethernet-switching filter input" % self.port,
                   "delete firewall family ethernet-switching filter %s" % TEST_FILTER]
        if self.filter:
            restore[0] = "set interfaces %s unit 0 family ethernet-switching filter input %s" % (self.port, self.filter)
        try:
            self.change_port([prefix + "term no-dhcp from protocol udp",
                              prefix + "term no-dhcp from destination-port 67",
                              prefix + "term no-dhcp then discard",
                              prefix + "term other then accept",
                              "set interfaces %s unit 0 family ethernet-switching filter input %s"
                              % (self.port, TEST_FILTER)])
            # Reboot (option 2, DHCP unchanged) so the device has to ask for a new lease
            self.dev.send("2", expect=r"Save and reboot\? \(y/n\): $")
            self.dev.send("y", expect=r"Rebooting", timeout=10)
            self.dev.close()
            time.sleep(1.5)
            self.dev.open()
            self.dev.wait_for_menu()
            st = self.wait_for_state("link up, waiting for DHCP lease", timeout=20)
            self.assertEqual("link up, waiting for DHCP lease", st["state"], st["raw"])
            # It keeps waiting: no lease and no fallback address
            deadline = time.time() + 20
            while time.time() < deadline:
                st = self.dev.status()
                self.assertEqual("link up, waiting for DHCP lease", st["state"], st["raw"])
                self.assertRegex(st["raw"], r"(?m)^IP: \(IP unset\)\r?$")
                time.sleep(2)
        finally:
            self.restore_port(restore)
        # DHCP reachable again: lwIP retries at most every 60 s, so the lease comes without a reboot
        st = self.dev.wait_connected(timeout=90)
        self.assert_answers_snmp(st)


if __name__ == "__main__":
    unittest.main()
