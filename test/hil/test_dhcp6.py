"""HIL tests for the DHCPv6 address (stateful DHCPv6, one IA_NA) on the test network.

Opt-in: RFPOWER_DHCP6_TEST=1. The test network (dhcp6_testnet.py) sends router
advertisements with the M flag only to the device and runs Kea for the device only,
in WSL as root; nothing else on the LAN is affected. Each test reboots the device
first (menu option 2, DHCP unchanged), because a started DHCPv6 client keeps running
until reboot. Needs IPv6 from this PC to the device (a VPN such as Cisco AnyConnect
blocks it) and the SNMP community "public".

Run:
    $env:RFPOWER_DHCP6_TEST = "1"
    %USERPROFILE%\\.platformio\\penv\\Scripts\\python.exe -m unittest discover -s test/hil -p "test_dhcp6.py" -v
"""

import ipaddress
import os
import re
import socket
import subprocess
import time
import unittest

from device import Device
from dhcp6_testnet import TestNet, device_duid_ll
from snmp_tools import GET_SYSDESCR

SNMP_PORT = 161


def ipv6_lines(raw):
    """{address: flags text} from the status screen's IPv6 lines."""
    return {m.group(1).lower(): m.group(2) for m in re.finditer(r"^IPv6: ([0-9A-Fa-f:]+)(.*?)\r?$", raw, re.MULTILINE)}


def default_routes_v6():
    out = subprocess.run(["powershell", "-NoProfile", "-Command",
                          "Get-NetRoute -AddressFamily IPv6 -DestinationPrefix ::/0 -ErrorAction SilentlyContinue | "
                          "ForEach-Object { \"$($_.ifIndex) $($_.NextHop)\" } | Sort-Object"],
                         capture_output=True, text=True).stdout
    return out.split()


@unittest.skipUnless(os.environ.get("RFPOWER_DHCP6_TEST") == "1", "set RFPOWER_DHCP6_TEST=1 to run the DHCPv6 tests")
class Dhcp6Test(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.dev = Device()
        cls.dev.open()
        cls.dev.wait_for_menu()
        st = cls.dev.wait_connected()
        if st["community"] != "public" or st["mode"] != "DHCP":
            cls.dev.close()
            raise unittest.SkipTest("device must be in DHCP mode with community public")
        cls.mac = st["mac"]

    @classmethod
    def tearDownClass(cls):
        cls.dev.close()

    def setUp(self):
        # A fresh client: the device reboots (option 2 keeps DHCP) and gets its SLAAC address back
        self.dev.wait_for_menu(timeout=40)
        self.dev.send("2", expect=r"Save and reboot\? \(y/n\): $")
        self.dev.send("y", expect=r"Rebooting", timeout=10)
        self.dev.reconnect_after_reboot()
        self.ll = self.glob = None
        deadline = time.time() + 30
        while time.time() < deadline:
            lines = ipv6_lines(self.dev.status()["raw"])
            self.ll = next((a for a, f in lines.items() if a.startswith("fe80:") and "tentative" not in f), None)
            self.glob = next((a for a, f in lines.items() if not a.startswith("fe80:") and "tentative" not in f), None)
            if self.ll and self.glob:
                break
            time.sleep(1)
        if not (self.ll and self.glob):
            self.skipTest("the device needs a link-local and a SLAAC address (IPv6 router on the LAN)")
        self.net = TestNet(self.mac, self.ll, self.glob)

    def tearDown(self):
        self.net.stop()

    def wait_for(self, predicate, timeout, what):
        """Poll the status screen until predicate(status) is true; return that status."""
        deadline = time.time() + timeout
        st = None
        while time.time() < deadline:
            st = self.dev.status()
            if predicate(st):
                return st
            time.sleep(1)
        self.fail("%s not within %d s; status:\n%s\nKea:\n%s" % (what, timeout, st and st["raw"][-1500:],
                                                                  self.net.kea_log()[-2000:]))

    def wait_bound(self, timeout=40):
        """Status once the DHCPv6 address is in use (out of duplicate address detection)."""
        def bound(st):
            a = st["dhcp6Address"]
            flags = ipv6_lines(st["raw"]).get(a or "", "")
            return a is not None and "(dhcpv6)" in flags and "tentative" not in flags
        return self.wait_for(bound, timeout, "a DHCPv6 address")

    def in_pool(self, address):
        base = ipaddress.IPv6Network(self.net.subnet).network_address
        a = ipaddress.IPv6Address(address)
        return base + 0xd6000001 <= a <= base + 0xd60000ff

    # ---- tests ----

    def test_managed_flag_gets_an_address_from_kea(self):
        self.net.start(managed=True, other=False, valid_s=120, preferred_s=60)
        st = self.wait_bound()
        addr = st["dhcp6Address"]
        self.assertTrue(self.in_pool(addr), addr)
        self.assertIn(device_duid_ll(self.mac), self.net.kea_log().lower())
        # The address works like the others: ping6 and SNMP over IPv6
        out = subprocess.run(["ping", "-6", "-n", "2", addr], capture_output=True, text=True).stdout
        self.assertIn("Reply from", out, out)
        with socket.socket(socket.AF_INET6, socket.SOCK_DGRAM) as s:
            s.settimeout(2)
            s.sendto(GET_SYSDESCR, (addr, SNMP_PORT, 0, 0))
            data, _ = s.recvfrom(2048)
        self.assertIn(b"RF.Power-Monitor", data)

    def test_lease_is_renewed(self):
        self.net.start(managed=True, other=False, valid_s=120, preferred_s=60)  # T1 30 s, T2 48 s
        addr = self.wait_bound()["dhcp6Address"]
        deadline = time.time() + 50
        while time.time() < deadline and "DHCP6_LEASE_RENEW" not in self.net.kea_log():
            time.sleep(2)
        self.assertIn("DHCP6_LEASE_RENEW", self.net.kea_log(), "no Renew reached Kea")
        st = self.dev.status()
        self.assertEqual(addr, st["dhcp6Address"])
        self.assertEqual("bound", st["dhcp6State"])

    def test_address_removed_when_server_stops(self):
        self.net.start(managed=True, other=False, valid_s=40, preferred_s=20)
        addr = self.wait_bound()["dhcp6Address"]
        self.net.stop()  # no more Renew/Rebind answers: the lease runs out after 40 s
        st = self.wait_for(lambda s: s["dhcp6Address"] is None and addr not in ipv6_lines(s["raw"]), 60,
                           "removal of %s" % addr)
        self.assertEqual("soliciting", st["dhcp6State"])

    def test_duplicate_address_is_declined(self):
        # Kea hands out an address this PC already uses: duplicate address detection must catch it
        pc = subprocess.run(["powershell", "-NoProfile", "-Command",
                             "(Get-NetIPAddress -AddressFamily IPv6 | Where-Object { $_.IPAddress -like '%s*' "
                             "-and $_.AddressState -eq 'Preferred' } | Select-Object -First 1).IPAddress"
                             % self.net.subnet.split("::")[0]], capture_output=True, text=True).stdout.strip()
        if not pc:
            self.skipTest("this PC has no address in the device's /64")
        self.net.start(managed=True, other=False, valid_s=120, preferred_s=60, pool="%s-%s" % (pc, pc))
        deadline = time.time() + 40
        while time.time() < deadline and "DHCP6_DECLINE" not in self.net.kea_log():
            time.sleep(1)
        self.assertRegex(self.net.kea_log(), r"DHCP6_DECLINE", "no Decline reached Kea")
        self.assertNotIn(pc.lower(), ipv6_lines(self.dev.status()["raw"]))

    def test_stack_headroom_after_dhcpv6(self):
        self.net.start(managed=True, other=False, valid_s=120, preferred_s=60)
        self.wait_bound()
        self.assertGreater(self.dev.status()["stackNeverUsed"], 1024)

    def test_lan_untouched(self):
        before = default_routes_v6()
        self.net.start(managed=True, other=False, valid_s=120, preferred_s=60)
        self.wait_bound()
        self.assertEqual(before, default_routes_v6(), "this PC's IPv6 default routes changed")


if __name__ == "__main__":
    unittest.main(verbosity=2)
