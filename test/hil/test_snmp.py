"""Hardware-in-the-loop tests for the SNMP v1 agent on the flashed device.

Uses the real net-snmp tools (snmpget, snmpgetnext, snmpwalk, snmpset) in
WSL, and raw UDP from Windows for timing and malformed-packet floods. The
device IP is read from the serial status screen, so the serial port must be
free (close PuTTY).

Run (from the project root, firmware flashed):
    %USERPROFILE%\\.platformio\\penv\\Scripts\\python.exe -m unittest discover -s test/hil -p "test_snmp.py" -v

Environment variables:
    RFPOWER_PORT          Serial port, default COM16
    RFPOWER_WSL_DISTRO    WSL distribution with net-snmp, default Ubuntu-24.04
    RFPOWER_MAINS_SWITCH / RFPOWER_MANUAL_MAINS  Mains tests, see mains_switch.py
    RFPOWER_NO_TMP1075    Set when the device has no TMP1075 (no carrier board):
                        the ambient temperature tests are skipped and walks
                        expect no ambientTemperature

The device must use community "public"; tests that change the SNMP
settings restore them afterwards.
"""

import os
import re
import socket
import statistics
import struct
import subprocess
import time
import unittest

from device import Device
from mains_switch import get_switch, skip_without_switch, wait_until
from snmp_tools import (GET_SYSDESCR, REQUEST_ID_OFFSET, garbage_packets, local_ip_towards, net_snmp,
                        raw_get, response_request_id, send_raw)

ENTERPRISE = "1.3.6.1.4.1.63050"
# Set when the device has no TMP1075 (module without the carrier board)
NO_TMP1075 = bool(os.environ.get("RFPOWER_NO_TMP1075"))
# Enterprise objects in walk order: ambientTemperature (.5) exists only with the TMP1075
ENTERPRISE_OBJECTS = [n for n in range(1, 7) if not (NO_TMP1075 and n == 5)]
LAST_ENTERPRISE = ENTERPRISE_OBJECTS[-1]
# The object that sorts last: snmpSetSerialNo (SNMPv2-MIB)
LAST_OID = ".1.3.6.1.6.3.1.1.6.1.0"
# snmp group (SNMPv2-MIB): 1.3.6.1.2.1.11.1-32, 7 and 23 were never assigned
SNMP_GROUP = [n for n in range(1, 33) if n not in (7, 23)]


def all_oids(v1, neighbour):
    """Every object of the MIB in walk order; v1 does not see the eight Counter64 ifHC* columns.

    LLDP-MIB comes first (1.0.8802 sorts before 1.3.6.1): our port always, the switch's row only
    while the device holds a neighbour (status screen "Switch:").
    """
    lldp = ".1.0.8802.1.1.2.1"
    oids = [lldp + ".3.7.1.%d.1" % i for i in range(2, 5)]
    if neighbour:
        oids += [lldp + ".4.1.1.%d.0.1.1" % i for i in range(4, 13)]
    oids += [".1.3.6.1.2.1.1.%d.0" % i for i in range(1, 9)]
    oids += [".1.3.6.1.2.1.1.9.1.%d.%d" % (c, r) for c in range(2, 5) for r in range(1, 10)]  # sysORTable
    oids += [".1.3.6.1.2.1.2.1.0"]
    oids += [".1.3.6.1.2.1.2.2.1.%d.1" % i for i in range(1, 23)]
    # MIB-II ip (1-19, 23), icmp (1-26), tcp (1-12, 14, 15), udp (1-4): lwIP's counters
    oids += [".1.3.6.1.2.1.4.%d.0" % i for i in list(range(1, 20)) + [23]]
    oids += [".1.3.6.1.2.1.5.%d.0" % i for i in range(1, 27)]
    oids += [".1.3.6.1.2.1.6.%d.0" % i for i in range(1, 16) if i != 13]
    oids += [".1.3.6.1.2.1.7.%d.0" % i for i in range(1, 5)]
    oids += [".1.3.6.1.2.1.11.%d.0" % i for i in SNMP_GROUP]
    # HOST-RESOURCES-MIB: hrSystem, hrMemorySize, hrStorage (RAM, LittleFS), hrDevice and hrProcessor (2 cores)
    hr = ".1.3.6.1.2.1.25"
    oids += [hr + ".1.%d.0" % i for i in (1, 5, 6, 7)] + [hr + ".2.2.0"]
    oids += [hr + ".2.3.1.%d.%d" % (c, r) for c in range(1, 8) for r in (1, 2)]
    oids += [hr + ".3.2.1.%d.%d" % (c, r) for c in range(1, 7) for r in (768, 769)]
    oids += [hr + ".3.3.1.%d.%d" % (c, r) for c in range(1, 3) for r in (768, 769)]
    oids += [".1.3.6.1.2.1.31.1.1.1.%d.1" % i for i in range(1, 20) if not (v1 and 6 <= i <= 13)]
    # ENTITY-MIB: the device's entPhysicalTable row (columns 2-16), entLastChangeTime
    oids += [".1.3.6.1.2.1.47.1.1.1.1.%d.1" % i for i in range(2, 17)] + [".1.3.6.1.2.1.47.1.4.1.0"]
    oids += ["." + ENTERPRISE + ".%d.0" % i for i in ENTERPRISE_OBJECTS]
    oids += [LAST_OID]
    return oids


# IP-MIB address rows (ipAddrTable, ipAddressTable): one per address the device has right now
ADDRESS_ROWS = (".1.3.6.1.2.1.4.20.", ".1.3.6.1.2.1.4.34.")


def static_oids(oids):
    """The walked OIDs without the address rows, which depend on the network (tested on their own)."""
    return [o for o in oids if not o.startswith(ADDRESS_ROWS)]


def v3_packet():
    """The captured v1 GetRequest with its version field set to 3 (SNMPv3)."""
    pkt = bytearray(GET_SYSDESCR)
    pkt[4] = 3
    return bytes(pkt)
NO_SUCH = r"\(noSuchName\) There is no such variable name in this MIB\.\s+Failed object: "


class SnmpAgentTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.dev = Device()
        cls.dev.open()
        cls.dev.wait_for_menu()
        cls.ip = cls.dev.wait_connected()["ip"]
        community = cls.dev.status()["community"]
        if community != "public":
            cls.dev.close()
            raise unittest.SkipTest('device community is "%s"; set it to "public" (menu option 4) '
                                    "before running the SNMP HIL tests" % community)

    @classmethod
    def tearDownClass(cls):
        cls.dev.close()

    def snmp(self, tool, *args, **kw):
        return net_snmp(tool, self.ip, *args, **kw)

    def has_neighbour(self):
        """The device holds an LLDP neighbour, so lldpRemTable has its row"""
        self.dev.wait_for_menu(timeout=10)
        return self.dev.status()["switch"] not in (None, "none seen")

    def snmp_stats(self):
        """SNMP counters from the serial status screen."""
        self.dev.wait_for_menu(timeout=10)
        raw = self.dev.status()["raw"]
        m = re.search(r"SNMP: (\d+) packets, (\d+) responses \(get (\d+), getnext (\d+), set (\d+)\), "
                      r"bad community (\d+), bad version (\d+), malformed (\d+)", raw)
        self.assertIsNotNone(m, raw)
        keys = ("packets", "responses", "get", "getnext", "set", "bad_community", "bad_version", "malformed")
        return dict(zip(keys, map(int, m.groups())))

    # ---- GetRequest ----

    def test_get_sysdescr(self):
        rc, out = self.snmp("snmpget", "1.3.6.1.2.1.1.1.0")
        self.assertEqual(0, rc, out)
        self.assertRegex(out, r'\.1\.3\.6\.1\.2\.1\.1\.1\.0 = STRING: "RF-Power-Monitor \d+\.\d+\.\d+ ')

    def test_get_sysobjectid_is_enterprise(self):
        rc, out = self.snmp("snmpget", "1.3.6.1.2.1.1.2.0")
        self.assertIn("OID: ." + ENTERPRISE, out)

    def test_sysuptime_advances(self):
        def ticks():
            rc, out = self.snmp("snmpget", "1.3.6.1.2.1.1.3.0")
            m = re.search(r"Timeticks: \((\d+)\)", out)
            self.assertIsNotNone(m, out)
            return int(m.group(1))
        t1 = ticks()
        time.sleep(1.0)
        t2 = ticks()
        self.assertGreaterEqual(t2 - t1, 90, "sysUpTime advanced %d ticks in ~1 s" % (t2 - t1))
        self.assertLess(t2 - t1, 300)

    def test_mains_status_matches_serial_status(self):
        rc, out = self.snmp("snmpget", ENTERPRISE + ".1.0", ENTERPRISE + ".2.0", ENTERPRISE + ".3.0")
        self.assertEqual(0, rc, out)
        m = re.search(r"63050\.1\.0 = INTEGER: (\d)", out)
        self.assertIsNotNone(m, out)
        self.assertRegex(out, r"63050\.2\.0 = Counter32: \d+")
        self.assertRegex(out, r"63050\.3\.0 = Timeticks: \(\d+\)")
        serial = self.dev.status()["mains"]
        self.assertEqual("present" if m.group(1) == "1" else "LOST", serial)

    def test_get_several_varbinds_in_request_order(self):
        rc, out = self.snmp("snmpget", ENTERPRISE + ".1.0", "1.3.6.1.2.1.1.5.0", "1.3.6.1.2.1.1.7.0")
        self.assertEqual(0, rc, out)
        lines = [l for l in out.splitlines() if " = " in l]
        self.assertEqual(3, len(lines), out)
        self.assertTrue(lines[0].startswith("." + ENTERPRISE + ".1.0"))
        self.assertIn('STRING: "%s"' % self.dev.status()["sysName"], lines[1])
        self.assertIn("INTEGER: 72", lines[2])

    def test_get_unknown_oid_is_no_such_name(self):
        rc, out = self.snmp("snmpget", "1.3.6.1.2.1.1.1.0", "1.3.6.1.2.1.1.99.0")
        self.assertNotEqual(0, rc)
        self.assertRegex(out, NO_SUCH + r"\.1\.3\.6\.1\.2\.1\.1\.99\.0")

    # ---- GetNextRequest / walk ----

    def test_walk_returns_all_objects_in_order(self):
        rc, out = self.snmp("snmpwalk", ".1")
        self.assertEqual(0, rc, out)
        oids = static_oids([l.split(" = ")[0] for l in out.splitlines() if " = " in l])
        self.assertEqual(all_oids(v1=True, neighbour=self.has_neighbour()), oids)
        self.assertIn("End of MIB", out)

    def test_walk_enterprise_subtree(self):
        rc, out = self.snmp("snmpwalk", ENTERPRISE)
        self.assertEqual(len(ENTERPRISE_OBJECTS), sum(1 for l in out.splitlines() if " = " in l), out)

    def test_walk_system_group(self):
        rc, out = self.snmp("snmpwalk", "1.3.6.1.2.1.1")
        self.assertEqual(8 + 27, sum(1 for l in out.splitlines() if " = " in l), out)  # sysORTable 9 x 3

    def test_snmp_group_counts_this_test_requests(self):
        def counter(n):
            rc, out = self.snmp("snmpget", "1.3.6.1.2.1.11.%d.0" % n)
            m = re.search(r"Counter32: (\d+)", out)
            self.assertIsNotNone(m, out)
            return int(m.group(1))
        before = counter(15)  # snmpInGetRequests
        for _ in range(5):
            self.snmp("snmpget", "1.3.6.1.2.1.1.3.0")
        # The 5 gets, and the get reading the counter itself
        self.assertGreaterEqual(counter(15) - before, 6)
        self.assertGreaterEqual(counter(1), counter(15))  # snmpInPkts covers every request
        rc, out = self.snmp("snmpget", "1.3.6.1.2.1.11.30.0", "1.3.6.1.6.3.1.1.6.1.0")
        self.assertRegex(out, r"11\.30\.0 = INTEGER: 1")  # snmpEnableAuthenTraps
        self.assertRegex(out, r"6\.1\.0 = INTEGER: 0")    # snmpSetSerialNo

    # ---- MIB-II protocol counters (lwIP) ----

    def counters(self, *oids):
        """Counter32/Gauge32 values of the given OIDs, in order."""
        rc, out = self.snmp("snmpget", *oids, version="2c")
        values = re.findall(r"= (?:Counter32|Gauge32): (\d+)", out)
        self.assertEqual(len(oids), len(values), out)
        return [int(v) for v in values]

    def test_icmp_echo_counters_follow_ping(self):
        in_echos, out_reps = "1.3.6.1.2.1.5.8.0", "1.3.6.1.2.1.5.22.0"
        before = self.counters(in_echos, out_reps)
        result = subprocess.run(["ping", "-n", "10", "-w", "1000", self.ip], stdout=subprocess.PIPE, text=True)
        self.assertEqual(0, result.returncode, result.stdout)
        after = self.counters(in_echos, out_reps)
        self.assertGreaterEqual(after[0] - before[0], 10)
        self.assertGreaterEqual(after[1] - before[1], 10)
        self.assertLess(after[0] - before[0], 30)  # nobody else pings it that much

    def test_udp_counters_follow_snmp_requests(self):
        in_dgrams, out_dgrams = "1.3.6.1.2.1.7.1.0", "1.3.6.1.2.1.7.4.0"
        before = self.counters(in_dgrams, out_dgrams)
        for _ in range(10):
            self.snmp("snmpget", "1.3.6.1.2.1.1.3.0")
        after = self.counters(in_dgrams, out_dgrams)
        # The 10 requests and the one reading the counters, each answered
        self.assertGreaterEqual(after[0] - before[0], 11)
        self.assertGreaterEqual(after[1] - before[1], 10)

    def test_udp_no_ports_counts_closed_port(self):
        no_ports = "1.3.6.1.2.1.7.2.0"
        before = self.counters(no_ports)[0]
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            for _ in range(3):
                s.sendto(b"x", (self.ip, 9))  # discard port: nothing listens
                time.sleep(0.1)
        self.assertGreaterEqual(self.counters(no_ports)[0] - before, 3)

    def test_tcp_passive_opens_follow_web_connections(self):
        passive = "1.3.6.1.2.1.6.6.0"
        before = self.counters(passive)[0]
        for _ in range(3):
            with socket.create_connection((self.ip, 80), timeout=5):
                pass
            time.sleep(0.3)
        self.assertGreaterEqual(self.counters(passive)[0] - before, 3)

    def test_tcp_curr_estab_counts_an_open_web_connection(self):
        estab = "1.3.6.1.2.1.6.9.0"
        with socket.create_connection((self.ip, 80), timeout=5):
            time.sleep(0.3)
            self.assertGreaterEqual(self.counters(estab)[0], 1)
        time.sleep(1)
        # Our connection is gone (FIN or the device's 5 s timeout); the web page may still hold others
        self.assertLessEqual(self.counters(estab)[0], 4)

    def test_snmp_ip_addresses_match_status_screen(self):
        st = self.dev.status()
        rc, out = self.snmp("snmpwalk", "1.3.6.1.2.1.4.20.1", version="2c")
        self.assertEqual(0, rc, out)
        # ipAddrTable: exactly the device's IPv4 address, with its netmask
        addrs = re.findall(r"\.4\.20\.1\.1\.[\d.]+ = IpAddress: ([\d.]+)", out)
        self.assertEqual([st["ip"]], addrs)
        self.assertRegex(out, r"\.4\.20\.1\.3\.%s = IpAddress: %s" % (re.escape(st["ip"]), re.escape(st["subnet"])))
        # ipAddressTable: the same address as an ipv4 row, from DHCP or set by hand
        index = "1.4." + st["ip"]
        rc, out = self.snmp("snmpget", "1.3.6.1.2.1.4.34.1.3." + index, "1.3.6.1.2.1.4.34.1.6." + index,
                            version="2c")
        values = re.findall(r"INTEGER: (\d+)", out)
        self.assertEqual(["1", "4" if st["mode"] == "DHCP" else "2"], values, out)

    def test_sysortable_names_the_mibs(self):
        rc, out = self.snmp("snmpwalk", "1.3.6.1.2.1.1.9.1.3")
        descrs = re.findall(r'STRING: "([^"]+)"', out)
        self.assertEqual(["SNMPv2-MIB", "IF-MIB", "IP-MIB", "UDP-MIB", "TCP-MIB", "HOST-RESOURCES-MIB",
                          "ENTITY-MIB", "LLDP-MIB", "RF-POWER-MONITOR-MIB"], descrs)

    def test_getnext_past_end(self):
        last = LAST_OID[1:]
        rc, out = self.snmp("snmpgetnext", last)
        self.assertRegex(out, NO_SUCH + r"\." + last.replace(".", r"\."))

    # ---- rejected requests ----

    def test_set_is_rejected_and_changes_nothing(self):
        rc, out = self.snmp("snmpset", "1.3.6.1.2.1.1.5.0", "s", "hacked")
        self.assertNotEqual(0, rc)
        self.assertRegex(out, NO_SUCH + r"\.1\.3\.6\.1\.2\.1\.1\.5\.0")
        rc, out = self.snmp("snmpget", "1.3.6.1.2.1.1.5.0")
        self.assertIn('STRING: "%s"' % self.dev.status()["sysName"], out)

    def test_wrong_community_gets_no_reply_and_is_counted(self):
        before = self.snmp_stats()["bad_community"]
        rc, out = self.snmp("snmpget", "1.3.6.1.2.1.1.1.0", community="not-public", timeout=1)
        self.assertIn("Timeout", out)
        self.assertEqual(before + 1, self.snmp_stats()["bad_community"])

    def test_v3_gets_no_reply_and_is_counted(self):
        before = self.snmp_stats()["bad_version"]
        resp, _ = raw_get(self.ip, 0, timeout=1.0, packet=v3_packet())
        self.assertIsNone(resp, "SNMPv3 must not be answered")
        self.assertEqual(before + 1, self.snmp_stats()["bad_version"])

    # ---- v2c ----

    def test_v2c_get(self):
        rc, out = self.snmp("snmpget", "1.3.6.1.2.1.1.1.0", ENTERPRISE + ".1.0", version="2c")
        self.assertEqual(0, rc, out)
        self.assertRegex(out, r'1\.1\.0 = STRING: "RF-Power-Monitor \d+\.\d+\.\d+ ')
        self.assertRegex(out, r"63050\.1\.0 = INTEGER: [01]")

    def test_v2c_exceptions_instead_of_errors(self):
        rc, out = self.snmp("snmpget", "1.3.6.1.2.1.1.5.0", "1.3.6.1.2.1.1.99.0", "1.3.6.1.2.1.1.1.5",
                            version="2c")
        self.assertRegex(out, r'1\.5\.0 = STRING: ".*"')
        self.assertIn("1.99.0 = No Such Object available on this agent at this OID", out)
        self.assertIn("1.1.5 = No Such Instance currently exists at this OID", out)

    def test_v2c_walk_ends_with_end_of_mib_view(self):
        rc, out = self.snmp("snmpwalk", ".1", version="2c")
        self.assertEqual(0, rc, out)
        oids = static_oids([l.split(" = ")[0] for l in out.splitlines() if " = " in l and "No more variables" not in l])
        self.assertEqual(all_oids(v1=False, neighbour=self.has_neighbour()), oids)
        self.assertIn("No more variables left in this MIB View", out)

    def test_bulkwalk_returns_the_same_objects_as_walk(self):
        before = self.dev.status()["getBulkRequests"]
        rc, bulk = self.snmp("snmpbulkwalk", ".1", version="2c")
        self.assertEqual(0, rc, bulk)
        rc, walk = self.snmp("snmpwalk", ".1", version="2c")

        def oids(text):
            # Compare names and types only: uptime and traffic counters change between the walks
            return [(l.split(" = ")[0], l.split(" = ")[1].split(":")[0]) for l in text.splitlines()
                    if " = " in l and "No more" not in l]
        self.assertEqual(oids(walk), oids(bulk))
        self.assertEqual(len(all_oids(v1=False, neighbour=self.has_neighbour())), len(static_oids([o for o, _ in oids(bulk)])))
        # 232 or more objects at most 16 per GetBulk: at least 15 requests
        self.assertGreaterEqual(self.dev.status()["getBulkRequests"] - before, 15)

    def test_bulkget_non_repeaters_and_repetitions(self):
        rc, out = self.snmp("snmpbulkget", "-Cn1", "-Cr3", "1.3.6.1.2.1.1.3.0", "1.3.6.1.2.1.1", version="2c")
        self.assertEqual(0, rc, out)
        oids = [l.split(" = ")[0] for l in out.splitlines() if " = " in l]
        self.assertEqual([".1.3.6.1.2.1.1.4.0", ".1.3.6.1.2.1.1.1.0", ".1.3.6.1.2.1.1.2.0",
                          ".1.3.6.1.2.1.1.3.0"], oids)

    def test_bulkget_is_capped_at_16_varbinds(self):
        # Limits reflection: a short request never gets a huge reply
        rc, out = self.snmp("snmpbulkget", "-Cn0", "-Cr1000", ".1", version="2c")
        self.assertEqual(0, rc, out)
        self.assertEqual(16, sum(1 for l in out.splitlines() if " = " in l))

    def test_bulkget_reaches_end_of_mib_view(self):
        rc, out = self.snmp("snmpbulkget", "-Cn0", "-Cr1000", ENTERPRISE, version="2c")
        self.assertEqual(0, rc, out)
        self.assertIn("No more variables left in this MIB View", out)
        # The enterprise objects, then snmpSetSerialNo, then the end
        self.assertEqual(len(ENTERPRISE_OBJECTS) + 1, sum(1 for l in out.splitlines() if " = " in l and "No more" not in l))

    # ---- IF-MIB ----

    IF_OIDS = {"ifDescr": "2.2.1.2", "ifType": "2.2.1.3", "ifMtu": "2.2.1.4", "ifSpeed": "2.2.1.5",
               "ifPhysAddress": "2.2.1.6", "ifAdminStatus": "2.2.1.7", "ifOperStatus": "2.2.1.8",
               "ifInOctets": "2.2.1.10", "ifInDiscards": "2.2.1.13", "ifInErrors": "2.2.1.14",
               "ifInUnknownProtos": "2.2.1.15", "ifOutDiscards": "2.2.1.19", "ifOutErrors": "2.2.1.20",
               "ifName": "31.1.1.1.1", "ifHCInOctets": "31.1.1.1.6",
               "ifHCInUcastPkts": "31.1.1.1.7", "ifHCOutOctets": "31.1.1.1.10",
               "ifHCOutUcastPkts": "31.1.1.1.11", "ifHighSpeed": "31.1.1.1.15"}

    def ifmib(self, *names, version="2c"):
        """One snmpget for IF-MIB columns of interface 1; returns ({name: value text}, output)."""
        oids = {".1.3.6.1.2.1.%s.1" % self.IF_OIDS[n]: n for n in names}
        rc, out = self.snmp("snmpget", *oids, version=version)
        values = {}
        for line in out.splitlines():
            oid, sep, value = line.partition(" = ")
            if sep and oid in oids:
                values[oids[oid]] = value.strip()
        return values, out

    def test_if_table_describes_the_ethernet_port(self):
        v, out = self.ifmib("ifDescr", "ifType", "ifMtu", "ifAdminStatus", "ifOperStatus", "ifName")
        self.assertEqual('STRING: "W5500 10/100 Ethernet"', v.get("ifDescr"), out)
        self.assertEqual("INTEGER: 6", v.get("ifType"))
        self.assertEqual("INTEGER: 1500", v.get("ifMtu"))
        self.assertEqual("INTEGER: 1", v.get("ifAdminStatus"))
        self.assertEqual("INTEGER: 1", v.get("ifOperStatus"))
        self.assertEqual('STRING: "eth0"', v.get("ifName"))

    def test_if_speed_and_mac_match_the_device(self):
        st = self.dev.status()
        v, out = self.ifmib("ifSpeed", "ifHighSpeed", "ifPhysAddress")
        self.assertTrue(st["linkUp"])
        self.assertIn(st["linkMbps"], (10, 100))
        self.assertEqual("Gauge32: %d" % (st["linkMbps"] * 1000000), v.get("ifSpeed"), out)
        self.assertEqual("Gauge32: %d" % st["linkMbps"], v.get("ifHighSpeed"))
        # Without a MIB net-snmp prints the six raw bytes as Hex-STRING: 1E B2 ...
        self.assertEqual("Hex-STRING: " + st["mac"].replace(":", " "), v.get("ifPhysAddress"))

    def test_traffic_counters_count_real_packets(self):
        names = ("ifHCInUcastPkts", "ifHCOutUcastPkts", "ifHCInOctets", "ifHCOutOctets")

        def read():
            v, out = self.ifmib(*names)
            return {n: int(re.search(r"Counter64: (\d+)", v[n]).group(1)) for n in names}
        before = read()
        for i in range(20):
            self.assertIsNotNone(raw_get(self.ip, 9000 + i)[0])
        after = read()
        # 20 requests and 20 replies, plus the second snmpget itself
        self.assertGreaterEqual(after["ifHCInUcastPkts"] - before["ifHCInUcastPkts"], 21)
        self.assertGreaterEqual(after["ifHCOutUcastPkts"] - before["ifHCOutUcastPkts"], 20)
        self.assertGreaterEqual(after["ifHCInOctets"] - before["ifHCInOctets"], 20 * len(GET_SYSDESCR))
        self.assertGreater(after["ifHCOutOctets"], before["ifHCOutOctets"])

    def test_32_bit_counters_follow_64_bit_ones(self):
        v, out = self.ifmib("ifInOctets", "ifHCInOctets")
        c32 = int(re.search(r"Counter32: (\d+)", v["ifInOctets"]).group(1))
        c64 = int(re.search(r"Counter64: (\d+)", v["ifHCInOctets"]).group(1))
        # Read in one request: the 32-bit value is the low half of the 64-bit one
        self.assertEqual(c64 & 0xFFFFFFFF, c32, out)

    def test_receive_drop_counters_match_the_status_screen(self):
        # The values depend on the LAN (e.g. spanning tree frames are unknown protocols), so compare
        # SNMP with the status screen read just before: never lower, and only a little higher
        st = self.dev.status()
        for key in ("inDiscards", "inErrors", "inUnknownProtos", "outErrors"):
            self.assertIsNotNone(st[key], "status screen has no 'Ethernet drops' line:\n" + st["raw"])
        v, out = self.ifmib("ifInDiscards", "ifInErrors", "ifInUnknownProtos", "ifOutDiscards", "ifOutErrors")
        for name, key in (("ifInDiscards", "inDiscards"), ("ifInErrors", "inErrors"),
                          ("ifInUnknownProtos", "inUnknownProtos"), ("ifOutErrors", "outErrors")):
            m = re.match(r"Counter32: (\d+)$", v.get(name, ""))
            self.assertIsNotNone(m, out)
            self.assertGreaterEqual(int(m.group(1)), st[key], name)
            self.assertLessEqual(int(m.group(1)), st[key] + 20, name)
        self.assertEqual("Counter32: 0", v.get("ifOutDiscards"))

    def test_counter64_not_available_in_v1(self):
        rc, out = self.snmp("snmpget", "1.3.6.1.2.1.31.1.1.1.6.1")
        self.assertRegex(out, NO_SUCH + r"\.1\.3\.6\.1\.2\.1\.31\.1\.1\.1\.6\.1")

    # ---- CPU and ambient temperature ----

    def test_cpu_temperature_is_plausible_and_matches_status(self):
        rc, out = self.snmp("snmpget", ENTERPRISE + ".4.0", version="2c")
        deci = int(re.search(r"63050\.4\.0 = INTEGER: (-?\d+)", out).group(1))
        self.assertGreater(deci, 100, "below 10 C")
        self.assertLess(deci, 850, "above 85 C")
        status_c = self.dev.status()["cpuTempC"]
        self.assertAlmostEqual(deci / 10.0, status_c, delta=2.0)

    @unittest.skipIf(NO_TMP1075, "RFPOWER_NO_TMP1075 set: no carrier board")
    def test_ambient_temperature_is_plausible_and_matches_status(self):
        rc, out = self.snmp("snmpget", ENTERPRISE + ".5.0", version="2c")
        m = re.search(r"63050\.5\.0 = INTEGER: (-?\d+)", out)
        self.assertIsNotNone(m, out)
        deci = int(m.group(1))
        self.assertGreater(deci, 50, "below 5 C")
        self.assertLess(deci, 500, "above 50 C")
        status_c = self.dev.status()["ambientTempC"]
        self.assertIsNotNone(status_c, "status screen says no sensor")
        # Read every 2 s: allow for a change between the two reads
        self.assertAlmostEqual(deci / 10.0, status_c, delta=0.5)

    @unittest.skipIf(NO_TMP1075, "RFPOWER_NO_TMP1075 set: no carrier board")
    def test_ambient_temperature_is_below_the_cpu_die(self):
        st = self.dev.status()
        self.assertLess(st["ambientTempC"], st["cpuTempC"])

    def test_v2c_set_is_not_writable(self):
        rc, out = self.snmp("snmpset", "1.3.6.1.2.1.1.5.0", "s", "hacked", version="2c")
        self.assertNotEqual(0, rc)
        self.assertRegex(out, r"notWritable[\s\S]*Failed object: \.1\.3\.6\.1\.2\.1\.1\.5\.0")
        rc, out = self.snmp("snmpget", "1.3.6.1.2.1.1.5.0", version="2c")
        self.assertNotIn("hacked", out)

    def test_v2c_wrong_community_gets_no_reply(self):
        rc, out = self.snmp("snmpget", "1.3.6.1.2.1.1.1.0", community="nope", version="2c", timeout=1)
        self.assertIn("Timeout", out)

    def test_v1_and_v2c_counters(self):
        before = self.dev.status()["v2cRequests"]
        self.snmp("snmpget", "1.3.6.1.2.1.1.1.0", version="2c")
        self.snmp("snmpget", "1.3.6.1.2.1.1.1.0", version="1")
        self.assertEqual(before + 1, self.dev.status()["v2cRequests"])

    # ---- SNMP settings from the serial menu ----

    def test_snmp_settings_from_menu_apply_immediately(self):
        d = self.dev
        before = d.status()
        packets_before = self.snmp_stats()["packets"]
        try:
            out = d.set_snmp("hil-community", "hil-name", "hil@example.com", 'HIL bench "A"')
            self.assertIn("SNMP settings saved and active", out)
            # Rate-limit trust earned with the old community is revoked
            self.assertIn("community changed, rate-limit trust cleared", out)

            rc, out = self.snmp("snmpget", "1.3.6.1.2.1.1.5.0", "1.3.6.1.2.1.1.4.0", "1.3.6.1.2.1.1.6.0",
                                community="hil-community")
            self.assertEqual(0, rc, out)
            self.assertIn('1.5.0 = STRING: "hil-name"', out)
            self.assertIn('1.4.0 = STRING: "hil@example.com"', out)
            self.assertIn('1.6.0 = STRING: "HIL bench \\"A\\""', out)

            rc, out = self.snmp("snmpget", "1.3.6.1.2.1.1.5.0", community=before["community"], timeout=1)
            self.assertIn("Timeout", out, "old community must be refused after the change")

            # No reboot happened: the packet counter kept counting instead of restarting at 0
            self.assertGreater(self.snmp_stats()["packets"], packets_before)
        finally:
            d.wait_for_menu(timeout=10)
            d.set_snmp(before["community"], before["sysName"], before["sysContact"], before["sysLocation"])
        rc, out = self.snmp("snmpget", "1.3.6.1.2.1.1.5.0", community=before["community"])
        self.assertIn('STRING: "%s"' % before["sysName"], out)

    def test_snmp_settings_survive_reboot(self):
        d = self.dev
        before = d.status()
        try:
            d.set_snmp(before["community"], "persist-test", "", "")
            d.send("2", expect=r"\(y/n\): $")                  # DHCP + reboot, keeps other settings
            d.send("y", expect=r"Rebooting", timeout=10)
            d.reconnect_after_reboot()
            self.assertEqual("persist-test", d.status()["sysName"])
            rc, out = self.snmp("snmpget", "1.3.6.1.2.1.1.5.0", community=before["community"])
            self.assertIn('STRING: "persist-test"', out)
        finally:
            d.wait_for_menu(timeout=10)
            d.set_snmp(before["community"], before["sysName"], before["sysContact"], before["sysLocation"])

    # ---- performance and robustness ----

    def test_response_time_under_100ms(self):
        times = []
        for i in range(30):
            resp, secs = raw_get(self.ip, 1000 + i)
            self.assertIsNotNone(resp, "no response to request %d" % i)
            self.assertEqual(1000 + i, response_request_id(resp), "request id not echoed")
            times.append(secs * 1000)
        median, worst = statistics.median(times), max(times)
        print("\n    SNMP round trip: median %.1f ms, max %.1f ms" % (median, worst))
        self.assertLess(worst, 100.0, ".clinerules: SNMP requests must be handled within 100 ms")

    def test_survives_malformed_packet_flood(self):
        # The core's UDP queue holds 5 packets, so most of each burst is dropped
        # before the agent; what matters is that the agent keeps working.
        before = self.snmp_stats()
        packets = garbage_packets(300)
        for i in range(0, len(packets), 20):   # bursts of 20
            send_raw(self.ip, packets[i:i + 20])
            time.sleep(0.05)
        time.sleep(2.5)                         # let the queue drain and the rate limiter refill
        answered = any(raw_get(self.ip, 77 + k, timeout=0.5)[0] is not None for k in range(3))
        self.assertTrue(answered, "agent stopped answering after malformed packets")
        after = self.snmp_stats()
        self.assertGreater(after["malformed"], before["malformed"], "flood never reached the agent")
        self.assertGreater(after["responses"], before["responses"], "probe response not counted")

    # ---- security: rate limiting and access log ----

    def revoke_trust(self):
        """Changing the community revokes all rate-limit trust: switch away and back."""
        st = self.dev.status()
        self.dev.set_snmp("rl-test", st["sysName"], st["sysContact"], st["sysLocation"])
        self.dev.set_snmp("public", st["sysName"], st["sysContact"], st["sysLocation"])
        self.assertEqual("public", self.dev.status()["community"])

    def test_rate_limit_caps_one_source_and_recovers(self):
        # An untrusted source: 50 requests/s with a burst of 100. Requests with a wrong community are
        # never answered, so they keep this PC untrusted; the status screen counts the ones dropped.
        self.revoke_trust()
        time.sleep(2.5)                          # start with a full bucket
        bad = bytearray(GET_SYSDESCR)
        bad[7:13] = b"wrongc"
        before = self.dev.status()["rateLimited"]
        start = time.time()
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            for _ in range(300):
                s.sendto(bytes(bad), (self.ip, 161))
                time.sleep(0.002)                # ~1 s for all, without overflowing the UDP queue
        elapsed = time.time() - start
        time.sleep(0.5)
        dropped = self.dev.status()["rateLimited"] - before
        print("\n    rate limit (untrusted): %d of 300 dropped in %.1f s" % (dropped, elapsed))
        expected = 300 - (100 + 50 * elapsed)    # what the bucket cannot pass
        self.assertGreater(dropped, expected * 0.7, "far fewer dropped than the limit requires")
        self.assertLess(dropped, expected * 1.3 + 10, "more dropped than the limit allows")
        entries = [e for e in self.dev.access_log() if e[1] == "rate limited"]
        self.assertTrue(entries, "rate limiting not in the access log")

        time.sleep(2.5)                          # bucket refills
        self.assertIsNotNone(raw_get(self.ip, 1, timeout=0.5)[0], "source still blocked after refill")

    def test_trusted_manager_walks_back_to_back(self):
        # Once it used the right community, a manager may walk the whole MIB repeatedly at full speed
        self.assertIsNotNone(raw_get(self.ip, 6999, timeout=1.0)[0])  # trusted from here on
        time.sleep(2.5)
        before = self.dev.status()["rateLimited"]
        for _ in range(2):
            rc, out = self.snmp("snmpwalk", ".1", version="2c")
            self.assertEqual(0, rc, out[-300:])
            self.assertIn("No more variables left in this MIB View", out)
        self.assertEqual(before, self.dev.status()["rateLimited"], "a trusted walk was rate limited")

    def test_access_log_lists_rejected_requests_with_source(self):
        pc = local_ip_towards(self.ip)
        bad = bytearray(GET_SYSDESCR)
        bad[7:13] = b"wrongc"
        send_raw(self.ip, [bytes(bad)])
        send_raw(self.ip, [v3_packet()])
        time.sleep(0.3)
        log = self.dev.access_log()
        events = {(ip, ev) for ip, ev, _ in log}
        self.assertIn((pc, "bad community"), events, log)
        self.assertIn((pc, "bad version"), events, log)
        newest = log[0]
        self.assertEqual((pc, "bad version"), newest[:2], "newest entry must come first")

    def test_set_request_is_logged(self):
        self.snmp("snmpset", "1.3.6.1.2.1.1.5.0", "s", "x")
        log = self.dev.access_log()
        self.assertEqual("set rejected", log[0][1], log)

    def test_five_simultaneous_requests_are_all_answered(self):
        # Documented limit: the core queues 5 UDP packets (UdpContext::rxBufMaxDepth + 1).
        # Warm up first: if the device's ARP entry for this PC has expired, lwIP holds
        # only one reply while it resolves the address and drops the others.
        self.assertIsNotNone(raw_get(self.ip, 499)[0], "warm-up request not answered")
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.settimeout(0.5)
            for i in range(5):
                pkt = bytearray(GET_SYSDESCR)
                struct.pack_into(">I", pkt, REQUEST_ID_OFFSET, 500 + i)
                s.sendto(bytes(pkt), (self.ip, 161))
            ids = set()
            try:
                while True:
                    ids.add(response_request_id(s.recvfrom(2048)[0]))
            except socket.timeout:
                pass
        self.assertEqual({500, 501, 502, 503, 504}, ids)

    def test_oversized_packet_is_ignored(self):
        send_raw(self.ip, [b"\x30" + b"\x00" * 1499])
        resp, _ = raw_get(self.ip, 78)
        self.assertIsNotNone(resp)

    # ---- mains (needs a mains switch, see mains_switch.py) ----

    @skip_without_switch
    def test_mains_loss_and_restore_visible_over_snmp(self):
        def mains():
            rc, out = self.snmp("snmpget", ENTERPRISE + ".1.0", ENTERPRISE + ".2.0", ENTERPRISE + ".3.0")
            status = int(re.search(r"63050\.1\.0 = INTEGER: (\d)", out).group(1))
            changes = int(re.search(r"Counter32: (\d+)", out).group(1))
            last = int(re.search(r"63050\.3\.0 = Timeticks: \((\d+)\)", out).group(1))
            return status, changes, last

        status, changes, last = mains()
        self.assertEqual(1, status, "mains must be on when the test starts")

        def wait_for(expected):
            deadline = time.time() + 60
            while time.time() < deadline:
                s = mains()
                if s[0] == expected:
                    return s
                time.sleep(0.5)
            self.fail("mainsPowerStatus never became %d" % expected)

        switch = get_switch()
        try:
            switch.off()
            off = wait_for(0)
            switch.on()
            on = wait_for(1)
        finally:
            switch.restore()
        self.assertEqual(changes + 1, off[1])
        self.assertGreater(off[2], last)
        self.assertEqual(changes + 2, on[1])
        self.assertGreater(on[2], off[2])

    @skip_without_switch
    def test_short_outage_between_polls_is_seen_in_the_change_counter(self):
        # A manager polling every few minutes misses a short outage in mainsPowerStatus;
        # mainsPowerChanges must still show it
        switch = get_switch()
        if not switch.automatic:
            self.skipTest("needs the automatic switch for a short outage")

        def changes():
            rc, out = self.snmp("snmpget", ENTERPRISE + ".1.0", ENTERPRISE + ".2.0")
            return (int(re.search(r"63050\.1\.0 = INTEGER: (\d)", out).group(1)),
                    int(re.search(r"Counter32: (\d+)", out).group(1)))

        status, before = changes()
        self.assertEqual(1, status, "mains must be on when the test starts")
        try:
            switch.off()
            switch.on()        # each switch takes about a second: an outage of ~1 s
        finally:
            switch.restore()
        def counted_twice():
            s = changes()
            return s if s[1] >= before + 2 else None
        status, after = wait_until(counted_twice, 10) or changes()
        self.assertEqual(1, status)
        self.assertEqual(before + 2, after, "the outage must count as one loss and one restore")

    def mains_alarm(self):
        """mainsPowerStatus and mainsPowerAlarm over SNMP."""
        rc, out = self.snmp("snmpget", ENTERPRISE + ".1.0", ENTERPRISE + ".6.0")
        return (int(re.search(r"63050\.1\.0 = INTEGER: (\d)", out).group(1)),
                int(re.search(r"63050\.6\.0 = INTEGER: (\d)", out).group(1)))

    @skip_without_switch
    def test_short_mains_loss_does_not_raise_alarm(self):
        self.assertEqual((1, 0), self.mains_alarm(), "mains must be on and no alarm when the test starts")
        switch = get_switch()
        seen = []
        try:
            switch.off()
            self.assertIsNotNone(wait_until(lambda: self.mains_alarm()[0] == 0, 10), "mains loss not seen")
            end = time.time() + 30
            while time.time() < end:
                seen.append(self.mains_alarm()[1])
                time.sleep(2)
            switch.on()
            self.assertIsNotNone(wait_until(lambda: self.mains_alarm()[0] == 1, 10), "mains restore not seen")
        finally:
            switch.restore()
        self.assertEqual({0}, set(seen), "a 30 s outage must not raise mainsPowerAlarm")
        self.assertEqual((1, 0), self.mains_alarm())

    @skip_without_switch
    def test_mains_loss_over_60_s_raises_alarm_until_restore(self):
        self.assertEqual((1, 0), self.mains_alarm(), "mains must be on and no alarm when the test starts")
        switch = get_switch()
        try:
            switch.off()
            self.assertIsNotNone(wait_until(lambda: self.mains_alarm()[0] == 0, 10), "mains loss not seen")
            lost = time.time()
            time.sleep(50)
            self.assertEqual((0, 0), self.mains_alarm(), "alarm raised before 60 s")
            raised = wait_until(lambda: self.mains_alarm()[1] == 1, 25, interval=0.5)
            raised_after = time.time() - lost
            self.assertIsNotNone(raised, "mainsPowerAlarm not raised within 75 s")
            self.assertEqual("lost over 60 s", self.dev.status()["mainsAlarm"])
            switch.on()
            cleared = wait_until(lambda: self.mains_alarm() == (1, 0), 10)
        finally:
            switch.restore()
        self.assertIsNotNone(cleared, "mainsPowerAlarm not cleared after restore")
        # Counted from the debounced loss, which this test sees a moment after it happens
        self.assertGreater(raised_after, 57)
        self.assertEqual("none", self.dev.status()["mainsAlarm"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
