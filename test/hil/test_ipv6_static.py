"""Hardware-in-the-loop tests for the static IPv6 address (serial menu option 9, web page).

The menu test needs nothing on the network. The test that applies an address
is opt-in, because it needs a free address in the device's /64:

    RFPOWER_STATIC_IPV6          e.g. fd16:d833:4f0e:3ece::249 (in the /64 the router announces)
    RFPOWER_STATIC_IPV6_GATEWAY  optional gateway, e.g. fe80::1 (only shown and stored; routing
                               through it needs an off-link IPv6 host to reach)

The device must start without a static IPv6 address and is left without one.

Run:
    %USERPROFILE%\\.platformio\\penv\\Scripts\\python.exe -m unittest discover -s test/hil -p "test_ipv6_static.py" -v
"""

import http.client
import ipaddress
import os
import socket
import subprocess
import sys
import time
import unittest

from device import Device
from snmp_tools import GET_SYSDESCR

SNMP_PORT = 161


class StaticIpv6Test(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.dev = Device()
        cls.dev.open()
        cls.dev.wait_for_menu()
        st = cls.dev.wait_connected()
        if st["staticIPv6"]:
            cls.dev.close()
            raise unittest.SkipTest("device has static IPv6 %s; remove it (option 9, '-') first" % st["staticIPv6"])

    @classmethod
    def tearDownClass(cls):
        cls.dev.close()

    def setUp(self):
        self.dev.wait_for_menu(timeout=10)

    def test_menu_rejects_unusable_addresses_and_cancel_changes_nothing(self):
        d = self.dev
        d.send("9", expect=r"Static IPv6 address.*: $")
        out = d.send("fe80::10", expect=r"Static IPv6 address.*: $")
        self.assertIn("global or ULA", out)
        out = d.send("fd00::10/64", expect=r"Static IPv6 address.*: $")
        self.assertIn("not a valid IPv6 address", out)
        d.send("FD00:0:0:0:0:0:0:10", expect=r"IPv6 gateway.*: $")
        out = d.send("fd01::1", expect=r"IPv6 gateway.*: $")  # another /64
        self.assertIn("link-local", out)
        out = d.send("fe80::1", expect=r"\(y/n\): $")
        self.assertIn("New static IPv6: fd00::10/64, gateway fe80::1", out)  # shown in canonical form
        out = d.send("n", expect=r"> $")
        self.assertIn("Cancelled", out)
        st = d.status()
        self.assertEqual("", st["staticIPv6"])
        self.assertFalse([a for a, flags in st["ipv6"] if "static" in flags])

    @unittest.skipUnless(os.environ.get("RFPOWER_STATIC_IPV6"), "set RFPOWER_STATIC_IPV6 to a free address in the /64")
    def test_static_ipv6_applies_after_reboot_next_to_slaac(self):
        d = self.dev
        address = str(ipaddress.IPv6Address(os.environ["RFPOWER_STATIC_IPV6"]))  # canonical, as the device stores it
        gateway = os.environ.get("RFPOWER_STATIC_IPV6_GATEWAY", "")
        before = [a for a, _ in d.status()["ipv6"]]
        try:
            d.set_static_ipv6(address, gateway)
            # The router's answer (and so the SLAAC address) can come some seconds after the static one
            st = self.wait_for_address(address, also=before, timeout=60)
            self.assertEqual(address, st["staticIPv6"])
            if gateway:
                self.assertIn("IPv6 gateway: %s" % str(ipaddress.IPv6Address(gateway)), st["raw"])
            # The automatic addresses stay (wait_for_address waited for them)
            now = [a for a, _ in st["ipv6"]]
            for a in before:
                self.assertIn(a, now)

            flag = "-n" if sys.platform == "win32" else "-c"
            out = subprocess.run(["ping", "-6", flag, "3", address], capture_output=True, text=True).stdout
            self.assertIn("Reply from" if sys.platform == "win32" else "bytes from", out, out)

            with socket.socket(socket.AF_INET6, socket.SOCK_DGRAM) as s:
                s.settimeout(2)
                s.sendto(GET_SYSDESCR, (address, SNMP_PORT))
                data, _ = s.recvfrom(2048)
            self.assertIn(b"RF.Power-Monitor", data)

            conn = http.client.HTTPConnection(address, 80, timeout=5)
            conn.request("GET", "/", headers={"Host": "[%s]" % address})
            resp = conn.getresponse()
            body = resp.read()
            conn.close()
            self.assertEqual(200, resp.status)
            self.assertIn(b"Log in", body)
        finally:
            d.wait_for_menu()
            if d.status()["staticIPv6"]:
                d.set_static_ipv6("")
            st = d.status()
            self.assertEqual("", st["staticIPv6"])
            self.assertFalse([a for a, flags in st["ipv6"] if "static" in flags])

    def wait_for_address(self, address, also=(), timeout=20):
        """Status once the static address finished duplicate detection and the addresses in also are back"""
        deadline = time.time() + timeout
        while True:
            st = self.dev.status()
            now = [a for a, _ in st["ipv6"]]
            for a, flags in st["ipv6"]:
                if a == address:
                    self.assertNotIn("DUPLICATE", flags, "address %s is taken on the LAN" % address)
                    if "static" in flags and "tentative" not in flags and all(x in now for x in also):
                        return st
            if time.time() > deadline:
                self.fail("static address %s (and %s) not ready: %s" % (address, list(also), st["ipv6"]))
            time.sleep(1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
