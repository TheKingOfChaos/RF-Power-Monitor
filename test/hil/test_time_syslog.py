"""HIL tests for NTP time and syslog (menu option 11).

Both sets are opt-in, because they need a network resource:

    RFPOWER_NTP_SERVER   NTP server the device can reach (address or name, e.g. pool.ntp.org)
    RFPOWER_SYSLOG_TEST  1: the PC receives the device's syslog on UDP RFPOWER_SYSLOG_PORT
    RFPOWER_SYSLOG_PORT  default 5514 (Windows may ask to allow Python through its firewall)
    RFPOWER_MAINS_SWITCH Mains switch (mains_switch.py) for the mains event test

Each test puts option 11 back to what it found.
"""
import datetime
import os
import re
import socket
import time
import unittest

from device import Device
from mains_switch import get_switch, skip_without_switch
from snmp_tools import GET_SYSDESCR, garbage_packets, local_ip_towards, net_snmp, raw_get, send_raw

NTP_SERVER = os.environ.get("RFPOWER_NTP_SERVER")
SYSLOG_TEST = os.environ.get("RFPOWER_SYSLOG_TEST") == "1"
SYSLOG_PORT = int(os.environ.get("RFPOWER_SYSLOG_PORT", "5514"))
COMMUNITY = os.environ.get("RFPOWER_COMMUNITY")  # the device's community when it is not "public"

# <PRI>1 TIMESTAMP HOST rfpower - MSGID - TEXT
RFC5424 = re.compile(r"^<(\d{1,3})>1 (-|\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z) (\S+) rfpower - (\S+) - (.*)$")


def wait_synced(dev, timeout=90):
    """Status "Time:" line once NTP has answered; fails after timeout."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        dev.wait_for_menu(timeout=10)
        t = dev.status()["time"] or ""
        if not t.startswith("not synced"):
            return t
        time.sleep(5)
    raise AssertionError("NTP did not answer within %d s: %s" % (timeout, t))


@unittest.skipUnless(NTP_SERVER, "set RFPOWER_NTP_SERVER to an NTP server the device can reach")
class TimeTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.dev = Device()
        cls.dev.open()
        cls.dev.wait_for_menu()
        cls.ip = cls.dev.wait_connected()["ip"]

    @classmethod
    def tearDownClass(cls):
        cls.dev.close()

    def setUp(self):
        self.dev.wait_for_menu(timeout=10)
        self.dev.keep_time_syslog(self)

    def test_typed_ntp_server_syncs_within_60_s(self):
        self.dev.set_time_syslog(NTP_SERVER, "")
        line = wait_synced(self.dev, 60)
        m = re.match(r"(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d) (CES?T) \(NTP ", line)
        self.assertIsNotNone(m, line)
        shown = datetime.datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S")
        offset = 2 if m.group(2) == "CEST" else 1
        utc_now = datetime.datetime.now(datetime.timezone.utc).replace(tzinfo=None)
        self.assertLess(abs((shown - datetime.timedelta(hours=offset) - utc_now).total_seconds()), 5, line)

    def test_hr_system_date_matches_pc_clock(self):
        self.dev.set_time_syslog(NTP_SERVER, "")
        wait_synced(self.dev, 60)
        community = COMMUNITY or self.dev.status()["community"]
        rc, out = net_snmp("snmpget", self.ip, "1.3.6.1.2.1.25.1.2.0", community=community, version="2c")
        self.assertEqual(0, rc, out)
        # Without MIBs net-snmp prints the 11 DateAndTime bytes as hex:
        # year (2), month, day, hour, minute, second, deci-seconds, '+', hours, minutes from UTC
        m = re.search(r"Hex-STRING: ((?:[0-9A-F]{2} ?){11})", out)
        self.assertIsNotNone(m, out)
        b = bytes.fromhex(m.group(1))
        self.assertEqual(ord("+"), b[8], out)
        y, mo, d, h, mi, s, off = (b[0] << 8 | b[1]), b[2], b[3], b[4], b[5], b[6], b[9]
        self.assertIn(off, (1, 2), out)  # CET or CEST
        shown = datetime.datetime(y, mo, d, h, mi, s) - datetime.timedelta(hours=off)
        utc_now = datetime.datetime.now(datetime.timezone.utc).replace(tzinfo=None)
        self.assertLess(abs((shown - utc_now).total_seconds()), 5, out)

    def test_switching_typed_to_dhcp_forgets_the_typed_server(self):
        # lwIP keeps the typed server's resolved address; dhcp must not keep polling it
        # (this network's DHCP offers no option 42, so nothing may be left)
        self.dev.set_time_syslog(NTP_SERVER, "")
        wait_synced(self.dev, 60)
        self.dev.set_time_syslog("dhcp", "")
        self.dev.wait_for_menu(timeout=10)
        line = self.dev.status()["time"]
        self.assertIn("NTP DHCP, none offered", line)

    def test_unresolvable_ntp_name_keeps_running_unsynced(self):
        self.dev.set_time_syslog("no-such-host.invalid", "")
        self.dev.send("2", expect=r"\(y/n\): $")  # restart: forget the time set by earlier tests
        self.dev.send("y", expect=r"Rebooting", timeout=10)
        self.dev.reconnect_after_reboot()
        self.dev.wait_connected()
        time.sleep(20)
        self.dev.wait_for_menu(timeout=10)
        st = self.dev.status()
        self.assertTrue(st["time"].startswith("not synced (NTP no-such-host.invalid)"), st["time"])
        rc, out = net_snmp("snmpget", self.ip, "1.3.6.1.2.1.1.1.0", community=COMMUNITY or st["community"])
        self.assertEqual(0, rc, "SNMP stopped answering: %s" % out)


@unittest.skipUnless(SYSLOG_TEST, "set RFPOWER_SYSLOG_TEST=1 to receive the device's syslog on this PC")
class SyslogTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.dev = Device()
        cls.dev.open()
        cls.dev.wait_for_menu()
        st = cls.dev.wait_connected()
        cls.ip = st["ip"]
        cls.community = st["community"]
        cls.pc = local_ip_towards(cls.ip)
        cls.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        cls.sock.bind(("", SYSLOG_PORT))

    @classmethod
    def tearDownClass(cls):
        cls.sock.close()
        cls.dev.close()

    def setUp(self):
        self.dev.wait_for_menu(timeout=10)
        self.dev.keep_time_syslog(self)
        self.dev.set_time_syslog(NTP_SERVER or "", "%s:%d" % (self.pc, SYSLOG_PORT))
        if NTP_SERVER:
            wait_synced(self.dev, 60)
        self.drain()

    def drain(self):
        self.sock.settimeout(0.2)
        try:
            while True:
                self.sock.recvfrom(2048)
        except socket.timeout:
            pass

    def receive(self, seconds, until=None):
        """Messages (parsed RFC 5424 fields) received for `seconds`, or until one has MSGID `until`."""
        got = []
        deadline = time.time() + seconds
        while time.time() < deadline:
            self.sock.settimeout(max(0.1, deadline - time.time()))
            try:
                data, src = self.sock.recvfrom(2048)
            except socket.timeout:
                break
            if src[0] != self.ip:
                continue
            text = data.decode("utf-8", "replace")
            m = RFC5424.match(text)
            self.assertIsNotNone(m, "not RFC 5424: %r" % text)
            got.append({"pri": int(m.group(1)), "time": m.group(2), "host": m.group(3), "msgid": m.group(4),
                        "text": m.group(5), "raw": text})
            if until and m.group(4) == until:
                break
        return got

    def test_settings_change_is_logged_without_secrets(self):
        st = self.dev.status()
        location = st["sysLocation"]
        try:
            self.dev.set_snmp(st["community"], st["sysName"], st["sysContact"], "syslog test")
            msgs = self.receive(10, until="SETTINGS")
        finally:
            self.dev.wait_for_menu(timeout=10)
            self.dev.set_snmp(st["community"], st["sysName"], st["sysContact"], location)
        settings = [m for m in msgs if m["msgid"] == "SETTINGS"]
        self.assertTrue(settings, msgs)
        self.assertIn("serial menu: SNMP settings changed", settings[0]["text"])
        self.assertEqual(3 * 8 + 5, settings[0]["pri"])  # daemon.notice
        for m in msgs:
            self.assertNotIn(self.community, m["raw"].split(" - ", 2)[-1], "community in a syslog message")

    def test_option_11_change_says_where_it_came_from(self):
        self.dev.set_time_syslog("", "")  # saves the same values: still a settings event
        msgs = [m for m in self.receive(10, until="SETTINGS") if m["msgid"] == "SETTINGS"]
        self.assertTrue(msgs, "no SETTINGS line")
        self.assertTrue(msgs[0]["text"].startswith("serial menu: NTP server "), msgs[0]["text"])

    def test_bad_community_burst_is_throttled(self):
        # The access log folds repeats of one event from one source into one entry (one syslog line),
        # so alternate two kinds of bad request: every packet is then a new entry
        wrong = GET_SYSDESCR.replace(b"public", b"wrongx")
        garbage = garbage_packets(25)
        for i in range(25):
            raw_get(self.ip, 9000 + i, timeout=0.05, packet=wrong)
            send_raw(self.ip, [garbage[i]])
            time.sleep(0.3)
        msgs = [m for m in self.receive(75, until=None) if m["msgid"] == "SNMP"]
        firsts = [m for m in msgs if "more access events" not in m["text"]]
        self.assertGreaterEqual(len(firsts), 1, msgs)
        self.assertLessEqual(len(firsts), 5, msgs)
        self.assertTrue(any("more access events not sent" in m["text"] for m in msgs), msgs)

    def test_reboot_sends_boot_line_dated(self):
        self.dev.send("2", expect=r"\(y/n\): $")
        self.dev.send("y", expect=r"Rebooting", timeout=10)
        self.dev.reconnect_after_reboot()
        msgs = self.receive(180 if not NTP_SERVER else 90, until="BOOT")
        boots = [m for m in msgs if m["msgid"] == "BOOT"]
        self.assertTrue(boots, msgs)
        self.assertIn("started", boots[0]["text"])
        if NTP_SERVER:
            sent = datetime.datetime.strptime(boots[0]["time"], "%Y-%m-%dT%H:%M:%S.%fZ")
            utc_now = datetime.datetime.now(datetime.timezone.utc).replace(tzinfo=None)
            self.assertLess(abs((sent - utc_now).total_seconds()), 120, boots[0]["raw"])  # boot, dated back
        else:
            self.assertEqual("-", boots[0]["time"])
            self.assertIn("[uptime", boots[0]["text"])

    @skip_without_switch
    def test_mains_loss_is_logged(self):
        switch = get_switch()
        try:
            switch.off()
            lost = self.receive(60, until="MAINS")
            switch.on()
            restored = self.receive(60, until="MAINS")
        finally:
            switch.restore()
        self.assertTrue(any(m["msgid"] == "MAINS" and "lost" in m["text"] and m["pri"] == 3 * 8 + 4 for m in lost),
                        lost)
        self.assertTrue(any(m["msgid"] == "MAINS" and "restored" in m["text"] for m in restored), restored)


if __name__ == "__main__":
    unittest.main()
