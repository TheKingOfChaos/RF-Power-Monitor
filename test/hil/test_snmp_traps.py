"""Hardware-in-the-loop tests for the traps sent by the device, in v1 and v2c.

Every test runs twice: once with the trap version set to v1 (Trap-PDU) and
once with v2c (SNMPv2-Trap). The trap destination is set to this PC for the
duration of the tests; destination and version are restored afterwards.
Traps are received on UDP 162 here, so Windows Firewall must allow inbound
UDP for PlatformIO's Python (the "Allow" prompt the first time it listens).
If another receiver holds UDP 162 (the LibreNMS snmptrapd container), the
tests are skipped; stop it while they run.

Run:
    %USERPROFILE%\\.platformio\\penv\\Scripts\\python.exe -m unittest discover -s test/hil -p "test_snmp_traps.py" -v

Environment variables:
    RFPOWER_MAINS_SWITCH / RFPOWER_MANUAL_MAINS  Mains trap test, see mains_switch.py
"""

import time
import unittest

from device import Device
from mains_switch import get_switch, skip_without_switch
from snmp_tools import GET_SYSDESCR, TrapListener, local_ip_towards, raw_get, send_raw

ENTERPRISE = "1.3.6.1.4.1.63050"
AUTH_TRAP_INTERVAL = 10.0


def bad_community_request():
    pkt = bytearray(GET_SYSDESCR)
    pkt[7:13] = b"wrongc"     # same length as "public"
    return bytes(pkt)


class TrapTests:
    """Shared tests; subclasses set TRAP_VERSION ("1" or "2c")."""

    TRAP_VERSION = None

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
        cls.original = (st["trapTarget"], st["trapVersion"])
        cls.pc = local_ip_towards(cls.ip)
        try:
            cls.listener = TrapListener()
        except OSError as e:
            cls.dev.close()
            raise unittest.SkipTest("cannot listen on UDP 162 (LibreNMS snmptrapd in Docker?): %s" % e)
        cls.dev.set_trap_target(cls.pc, cls.TRAP_VERSION)

    @classmethod
    def tearDownClass(cls):
        try:
            cls.dev.wait_for_menu(timeout=10)
            cls.dev.set_trap_target(*cls.original)
        finally:
            cls.listener.close()
            cls.dev.close()

    def setUp(self):
        self.dev.wait_for_menu(timeout=10)
        self.listener.clear()

    @property
    def expected_version(self):
        return 0 if self.TRAP_VERSION == "1" else 1

    def wait_kind(self, kind, timeout):
        return self.listener.wait_for(lambda t: t["kind"] == kind, timeout)

    def assertFromDevice(self, trap):
        self.assertEqual(self.expected_version, trap["version"], "trap sent in the wrong version")
        self.assertEqual("public", trap["community"])
        self.assertEqual(self.ip, trap["source"])
        # v1 agent-addr or v2c snmpTrapAddress.0: managers behind NAT find the device by it
        self.assertEqual(self.ip, trap["agent"])
        if trap["version"] == 0:
            self.assertEqual(ENTERPRISE, trap["enterprise"])

    # ---- coldStart ----

    def test_cold_start_trap_after_reboot(self):
        # After a reboot the ARP table is empty: the very first packet to this PC is the trap
        d = self.dev
        self.assertEqual(self.TRAP_VERSION, d.status()["trapVersion"])
        d.send("2", expect=r"\(y/n\): $")
        d.send("y", expect=r"Rebooting", timeout=10)
        trap = self.wait_kind("coldStart", timeout=45)
        self.assertIsNotNone(trap, "no coldStart trap within 45 s of the reboot")
        self.assertFromDevice(trap)
        self.assertEqual({}, trap["varbinds"])
        self.assertLess(trap["timestamp"], 60 * 100, "coldStart time-stamp should be shortly after boot")
        d.reconnect_after_reboot()
        self.assertEqual(1, len(self.listener.matching(lambda t: t["kind"] == "coldStart")),
                         "exactly one coldStart per boot")
        self.assertEqual(self.TRAP_VERSION, d.status()["trapVersion"], "trap version not kept across reboot")

    # ---- authenticationFailure ----

    def test_wrong_community_sends_one_throttled_auth_failure_trap(self):
        time.sleep(AUTH_TRAP_INTERVAL + 0.5)      # let an earlier auth trap's throttle expire
        send_raw(self.ip, [bad_community_request()])
        trap = self.wait_kind("authenticationFailure", timeout=3)
        self.assertIsNotNone(trap, "no authenticationFailure trap")
        self.assertFromDevice(trap)

        # More bad requests right away are logged but do not cause more traps
        for _ in range(5):
            send_raw(self.ip, [bad_community_request()])
            time.sleep(0.05)
        time.sleep(2)
        self.assertEqual(1, len(self.listener.matching(lambda t: t["kind"] == "authenticationFailure")))
        entries = [e for e in self.dev.access_log() if e[0] == self.pc and e[1] == "bad community"]
        self.assertTrue(entries, "bad community not in the access log")
        self.assertGreaterEqual(entries[0][2], 6)

    def test_v2c_traps_have_increasing_request_ids(self):
        if self.TRAP_VERSION != "2c":
            self.skipTest("v2c only")
        ids = []
        for _ in range(2):
            time.sleep(AUTH_TRAP_INTERVAL + 0.5)
            self.listener.clear()
            send_raw(self.ip, [bad_community_request()])
            trap = self.wait_kind("authenticationFailure", timeout=3)
            self.assertIsNotNone(trap)
            ids.append(trap["requestId"])
        self.assertGreater(ids[1], ids[0])

    def test_no_traps_when_destination_is_off(self):
        d = self.dev
        try:
            d.set_trap_target("")
            self.assertEqual("", d.status()["trapTarget"])
            time.sleep(AUTH_TRAP_INTERVAL + 0.5)
            send_raw(self.ip, [bad_community_request()])
            self.assertIsNone(self.listener.wait_for(lambda t: True, timeout=3), "trap sent although off")
        finally:
            d.wait_for_menu(timeout=10)
            d.set_trap_target(self.pc, self.TRAP_VERSION)

    def test_trap_counter_on_status_screen(self):
        before = self.dev.status()["trapsSent"]
        time.sleep(AUTH_TRAP_INTERVAL + 0.5)
        send_raw(self.ip, [bad_community_request()])
        self.assertIsNotNone(self.wait_kind("authenticationFailure", timeout=3))
        self.assertEqual(before + 1, self.dev.status()["trapsSent"])

    def test_agent_keeps_answering_while_sending_traps(self):
        time.sleep(AUTH_TRAP_INTERVAL + 0.5)
        send_raw(self.ip, [bad_community_request()])
        resp, secs = raw_get(self.ip, 4242)
        self.assertIsNotNone(resp)
        self.assertLess(secs, 1.0)

    # ---- mains (needs a mains switch, see mains_switch.py) ----

    @skip_without_switch
    def test_mains_traps_with_real_mains(self):
        switch = get_switch()
        try:
            print("\n[%s traps]" % self.TRAP_VERSION, end="", flush=True)
            switch.off()
            lost = self.wait_kind("mainsPowerLost", timeout=60)
            switch.on()
            restored = self.wait_kind("mainsPowerRestored", timeout=60)
        finally:
            switch.restore()
        self.assertIsNotNone(lost, "no mainsPowerLost trap")
        self.assertFromDevice(lost)
        vb = lost["varbinds"]
        self.assertEqual((0x02, 0), vb[ENTERPRISE + ".1.0"])      # mainsPowerStatus = lost
        changes = vb[ENTERPRISE + ".2.0"][1]
        self.assertEqual(lost["timestamp"], vb[ENTERPRISE + ".3.0"][1])

        self.assertIsNotNone(restored, "no mainsPowerRestored trap")
        self.assertFromDevice(restored)
        vb = restored["varbinds"]
        self.assertEqual((0x02, 1), vb[ENTERPRISE + ".1.0"])
        self.assertEqual(changes + 1, vb[ENTERPRISE + ".2.0"][1])
        self.assertGreater(restored["timestamp"], lost["timestamp"])

    @skip_without_switch
    def test_mains_alarm_trap_after_60_s(self):
        switch = get_switch()
        try:
            print("\n[%s alarm trap]" % self.TRAP_VERSION, end="", flush=True)
            switch.off()
            lost = self.wait_kind("mainsPowerLost", timeout=30)
            alarm = self.wait_kind("mainsPowerLostTooLong", timeout=75)
            time.sleep(10)
            alarms = len(self.listener.matching(lambda t: t["kind"] == "mainsPowerLostTooLong"))
            switch.on()
            self.wait_kind("mainsPowerRestored", timeout=30)
        finally:
            switch.restore()
        self.assertIsNotNone(lost, "no mainsPowerLost trap")
        self.assertIsNotNone(alarm, "no mainsPowerLostTooLong trap within 75 s of the loss")
        self.assertFromDevice(alarm)
        vb = alarm["varbinds"]
        self.assertEqual((0x02, 0), vb[ENTERPRISE + ".1.0"])      # mainsPowerStatus = lost
        self.assertEqual((0x02, 1), vb[ENTERPRISE + ".6.0"])      # mainsPowerAlarm = lostTooLong
        # Stamped with the loss, like mainsPowerLost
        self.assertEqual(lost["timestamp"], alarm["timestamp"])
        self.assertEqual(alarm["timestamp"], vb[ENTERPRISE + ".3.0"][1])
        self.assertEqual(1, alarms, "one mainsPowerLostTooLong per outage")


class V1TrapTest(TrapTests, unittest.TestCase):
    TRAP_VERSION = "1"


class V2cTrapTest(TrapTests, unittest.TestCase):
    TRAP_VERSION = "2c"


if __name__ == "__main__":
    unittest.main(verbosity=2)
