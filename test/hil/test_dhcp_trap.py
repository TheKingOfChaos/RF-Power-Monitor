"""Hardware-in-the-loop tests for the SNMP trap destination from DHCP option 224.

The first test works with any DHCP server: with the trap destination set to
"dhcp", the device asks for option 224 in its DHCP messages; the server must
still answer (lease kept), and the status screen reports the ACK.

The second needs a DHCP server that has option 224 configured (see
docs/MIB.md, "Trap destination from DHCP") and is opt-in:

    RFPOWER_DHCP_TRAP_TARGET  The address the server hands out in option 224.
                            If it is this PC, the test also receives a trap.

Run:
    %USERPROFILE%\\.platformio\\penv\\Scripts\\python.exe -m unittest discover -s test/hil -p "test_dhcp_trap.py" -v
"""

import os
import re
import socket
import time
import unittest

from device import Device
from snmp_tools import GET_SYSDESCR, TrapListener, local_ip_towards, raw_get, send_raw

EXPECTED = os.environ.get("RFPOWER_DHCP_TRAP_TARGET")


def bad_community_request():
    pkt = bytearray(GET_SYSDESCR)
    pkt[7:13] = b"wrongc"
    return bytes(pkt)


class DhcpTrapTargetTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.dev = Device()
        cls.dev.open()
        cls.dev.wait_for_menu()
        st = cls.dev.wait_connected()
        if st["mode"] != "DHCP" or st["community"] != "public":
            cls.dev.close()
            raise unittest.SkipTest("device must be in DHCP mode with community public")
        cls.ip = st["ip"]
        cls.original = (st["trapTarget"] if st["trapSource"].startswith("fixed") else "dhcp", st["trapVersion"])

    @classmethod
    def tearDownClass(cls):
        try:
            cls.dev.wait_for_menu(timeout=10)
            cls.dev.set_trap_target(*cls.original)
        finally:
            cls.dev.close()

    def setUp(self):
        self.dev.wait_for_menu(timeout=10)

    def wait_for_ack(self, timeout=15):
        """Status after the renew triggered by choosing dhcp has been answered."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            st = self.dev.status()
            if "no DHCP ACK yet" not in (st["trapSource"] or ""):
                return st
            time.sleep(1)
        self.fail("no DHCP ACK within %d s: %s" % (timeout, st["trapSource"]))

    def test_choosing_dhcp_asks_for_option_224_and_keeps_the_lease(self):
        d = self.dev
        before = d.status()
        d.set_trap_target("dhcp")
        st = self.wait_for_ack()
        m = re.match(r"DHCP option 224 \(asked in (\d+) DHCP messages; (.*)\)", st["trapSource"])
        self.assertIsNotNone(m, st["trapSource"])
        self.assertGreaterEqual(int(m.group(1)), 1, "option 224 was not added to the DHCP request")
        self.assertRegex(m.group(2), r"last ACK \d+ s ago")
        # The server accepted the edited REQUEST: same lease, still reachable
        self.assertEqual("connected", st["state"])
        self.assertEqual(before["ip"], st["ip"])
        self.assertIsNotNone(raw_get(self.ip, 8800)[0])
        if not EXPECTED:
            self.assertIn("had no option 224", m.group(2), "server sent option 224: set RFPOWER_DHCP_TRAP_TARGET")
            self.assertEqual("", st["trapTarget"], "no trap destination without option 224")
        # Back to fixed
        d.set_trap_target("-")
        st = d.status()
        self.assertTrue(st["trapSource"].startswith("fixed"), st["trapSource"])

    def test_forged_dhcp_ack_with_option_224_is_ignored(self):
        """Any LAN host can send an "ACK"; only one the DHCP client accepted may set the destination."""
        d = self.dev
        d.set_trap_target("dhcp")
        before = self.wait_for_ack()
        self.assertNotIn("10.10.6.65", before["trapSource"])
        # Kea's captured ACK with option 224 = 10.10.6.65, re-addressed to the device, wrong transaction id
        with open(os.path.join(os.path.dirname(__file__), "..", "native", "test_dhcp_option", "frames.h")) as f:
            frames = f.read()
        block = frames[frames.index("KEA_ACK_WITH_224[]"):]
        block = block[:block.index("};")]
        eth = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-f]{2})", block))
        bootp = bytearray(eth[14 + 20 + 8:])
        bootp[4:8] = b"\x12\x34\x56\x78"                                  # xid lwIP never used
        bootp[16:20] = bytes(int(x) for x in self.ip.split("."))          # yiaddr = the device's address
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.bind(("", 67))
            for _ in range(3):
                s.sendto(bytes(bootp), (self.ip, 68))
                time.sleep(0.2)
        time.sleep(1)
        st = d.status()
        self.assertNotIn("10.10.6.65", st["trapSource"], "a forged ACK changed the trap destination")
        self.assertNotEqual("10.10.6.65", st["trapTarget"])
        self.assertRegex(st["raw"], r"DHCP: [1-9]\d* ACK-like frames ignored")
        self.assertEqual(before["ip"], st["ip"])
        d.set_trap_target("-")

    @unittest.skipUnless(EXPECTED,"set RFPOWER_DHCP_TRAP_TARGET to the address your DHCP server sends in option 224")
    def test_trap_destination_comes_from_option_224(self):
        d = self.dev
        d.set_trap_target("dhcp", "2c")
        st = self.wait_for_ack()
        self.assertIn("last ACK", st["trapSource"])
        self.assertIn(EXPECTED, st["trapSource"])
        self.assertEqual(EXPECTED, st["trapTarget"], "the destination in use must be the DHCP one")
        if EXPECTED == local_ip_towards(self.ip):
            listener = TrapListener()
            try:
                time.sleep(10.5)  # let an earlier authenticationFailure throttle expire
                send_raw(self.ip, [bad_community_request()])
                trap = listener.wait_for(lambda t: t["kind"] == "authenticationFailure", 3)
                self.assertIsNotNone(trap, "no trap arrived at the DHCP-provided destination")
            finally:
                listener.close()


if __name__ == "__main__":
    unittest.main(verbosity=2)
