"""HIL check of the DHCPv6 test network itself (opt-in: RFPOWER_DHCP6_TEST=1).

Works with any firmware: a router advertisement with only the O flag makes lwIP's
stateless DHCPv6 client on the device send an Information-request to ff02::1:2.
If Kea on this PC (WSL, mirrored networking) receives it and replies, router
advertisements reach the device and the device's DHCPv6 multicast reaches Kea:
everything the DHCPv6 address tests need. Needs root in WSL (`wsl -u root`),
python3-scapy and kea-dhcp6-server there (docs/TESTING.md).

Run:
    $env:RFPOWER_DHCP6_TEST = "1"
    %USERPROFILE%\\.platformio\\penv\\Scripts\\python.exe -m unittest discover -s test/hil -p "test_dhcp6_testnet.py" -v
"""

import os
import re
import time
import unittest

from device import Device
from dhcp6_testnet import TestNet


def ipv6_addresses(raw):
    """(link-local, first global/ULA) from the status screen's IPv6 lines."""
    addrs = [a.lower() for a in re.findall(r"^IPv6: ([0-9A-Fa-f:]+)", raw, re.MULTILINE)]
    ll = next((a for a in addrs if a.startswith("fe80:")), None)
    glob = next((a for a in addrs if not a.startswith("fe80:")), None)
    return ll, glob


@unittest.skipUnless(os.environ.get("RFPOWER_DHCP6_TEST") == "1", "set RFPOWER_DHCP6_TEST=1 to run the DHCPv6 test network")
class Dhcp6TestNetTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.dev = Device()
        cls.dev.open()
        cls.dev.wait_for_menu()
        st = cls.dev.wait_connected()
        cls.mac = st["mac"]
        cls.ll, cls.glob = ipv6_addresses(st["raw"])
        if cls.ll is None or cls.glob is None:
            cls.dev.close()
            raise unittest.SkipTest("the device needs a link-local and a SLAAC address (IPv6 router on the LAN)")

    @classmethod
    def tearDownClass(cls):
        cls.dev.close()

    def test_kea_answers_the_devices_information_request(self):
        before = self.dev.status()["raw"]
        # lwIP's Information-request carries no client id: the device is known by its source address
        received = re.compile(r"DHCP6_BUFFER_RECEIVED received buffer from %s:546 to ff02::1:2" % re.escape(self.ll))
        replied = re.compile(r"(?i)send packet reply \(type 7\) from .* to \[%s\]:546" % re.escape(self.ll))
        with TestNet(self.mac, self.ll, self.glob) as net:
            net.start(managed=False, other=True)
            deadline = time.time() + 30
            log = ""
            while time.time() < deadline:
                log = net.kea_log()
                if received.search(log) and replied.search(log):
                    break
                time.sleep(2)
            tail = "\n".join(l for l in log.splitlines() if "packets" in l or "PACKET" in l)[-3000:]
            self.assertRegex(log, received, "Kea never received the device's Information-request:\n%s\nRA: %s"
                             % (tail, net.ra_log()))
            self.assertRegex(log, replied, "Kea did not reply to the device:\n%s" % tail)
            self.assertNotIn("DHCP6_PACKET_SEND_FAIL", log, "Kea could not send its reply:\n%s" % tail)
        # Router lifetime 0 and no prefix: the device's own addresses are unchanged
        self.assertEqual(ipv6_addresses(before)[1], ipv6_addresses(self.dev.status()["raw"])[1])


    def test_option_31_gives_the_ntp_server_and_comes_back_after_none(self):
        # lwIP's SNTP takes DHCPv6 option 31 (SNTP servers) from the stateless reply; Kea serves
        # 2001:db8::123 (kea-dhcp6.conf.in), which nobody answers: only the source is checked
        self.dev.keep_time_syslog(self)
        self.dev.set_time_syslog("dhcp", "")

        def wait_for_server(timeout):
            deadline = time.time() + timeout
            line = ""
            while time.time() < deadline:
                self.dev.wait_for_menu(timeout=10)
                line = self.dev.status()["time"] or ""
                if re.search(r"NTP 2001:db8::123 from DHCP", line, re.IGNORECASE):  # lwIP prints it in capitals
                    return line
                time.sleep(2)
            self.fail("no NTP server from DHCPv6 option 31: %s" % line)

        with TestNet(self.mac, self.ll, self.glob) as net:
            net.start(managed=False, other=True)
            wait_for_server(40)
            # "none" clears the servers; back to dhcp, the DHCPv6 one returns without a new lease
            self.dev.set_time_syslog("none", "")
            self.assertIn("NTP off", self.dev.status()["time"])
            self.dev.set_time_syslog("dhcp", "")
            wait_for_server(20)


if __name__ == "__main__":
    unittest.main(verbosity=2)
