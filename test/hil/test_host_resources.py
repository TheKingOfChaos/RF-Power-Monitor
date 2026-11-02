"""Hardware-in-the-loop tests for HOST-RESOURCES-MIB (CPU load, RAM, LittleFS) and ENTITY-MIB.

Values over SNMP are compared with the serial status screen. The device must use community
"public" (as for test_snmp.py). The CPU load tests take about 3 minutes: the load is a
one-minute average.

Run:
    %USERPROFILE%\\.platformio\\penv\\Scripts\\python.exe -m unittest discover -s test/hil -p "test_host_resources.py" -v
"""

import http.client
import re
import threading
import time
import unittest

from device import Device
from snmp_tools import net_snmp

HR = "1.3.6.1.2.1.25"
ENT = "1.3.6.1.2.1.47.1.1.1.1"


class HostResourcesTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.dev = Device()
        cls.dev.open()
        cls.dev.wait_for_menu()
        cls.ip = cls.dev.wait_connected()["ip"]
        if cls.dev.status()["community"] != "public":
            cls.dev.close()
            raise unittest.SkipTest('set the community to "public" (menu option 4) first')

    @classmethod
    def tearDownClass(cls):
        cls.dev.close()

    def snmp(self, tool, *args, **kw):
        return net_snmp(tool, self.ip, *args, version="2c", **kw)

    def integers(self, *oids):
        """INTEGER values of the given OIDs, in order."""
        rc, out = self.snmp("snmpget", *oids)
        self.assertEqual(0, rc, out)
        values = re.findall(r"= INTEGER: (-?\d+)", out)
        self.assertEqual(len(oids), len(values), out)
        return [int(v) for v in values]

    def cpu_load(self):
        return tuple(self.integers(HR + ".3.3.1.2.768", HR + ".3.3.1.2.769"))

    def test_ram_storage_matches_status_screen(self):
        size_kb, used_kb = self.integers(HR + ".2.3.1.5.1", HR + ".2.3.1.6.1")
        self.dev.wait_for_menu(timeout=10)
        used, total = self.dev.status()["ramUsed"]
        self.assertEqual(total // 1024, size_kb)
        self.assertAlmostEqual(used / 1024, used_kb, delta=4)  # the heap moves a little between the two reads
        self.assertGreater(used_kb, 64)
        self.assertLess(used_kb, size_kb)
        rc, out = self.snmp("snmpget", HR + ".2.3.1.2.1", HR + ".2.3.1.3.1", HR + ".2.2.0")
        self.assertIn("OID: .1.3.6.1.2.1.25.2.1.2", out)  # hrStorageRam
        self.assertIn('STRING: "RAM"', out)
        self.assertIn("INTEGER: %d" % size_kb, out)       # hrMemorySize

    def test_littlefs_storage_is_plausible(self):
        size_kb, used_kb = self.integers(HR + ".2.3.1.5.2", HR + ".2.3.1.6.2")
        self.assertAlmostEqual(256, size_kb, delta=8)  # board_build.filesystem_size = 0.25m
        self.assertGreater(used_kb, 0)                  # the settings files
        self.assertLess(used_kb, size_kb)
        self.dev.wait_for_menu(timeout=10)
        used, total = self.dev.status()["fsUsed"]
        self.assertEqual(total // 1024, size_kb)
        rc, out = self.snmp("snmpget", HR + ".2.3.1.2.2", HR + ".2.3.1.3.2")
        self.assertIn("OID: .1.3.6.1.2.1.25.2.1.9", out)  # hrStorageFlashMemory
        self.assertIn('STRING: "LittleFS (settings)"', out)

    def test_processor_rows_describe_both_cores(self):
        rc, out = self.snmp("snmpwalk", HR + ".3.2.1.3")
        self.assertEqual(['"RP2040 core 0 (network, SNMP)"', '"RP2040 core 1 (mains input)"'],
                         re.findall(r'STRING: (".*")', out))

    def test_entity_serial_matches_status_screen(self):
        rc, out = self.snmp("snmpget", ENT + ".11.1", ENT + ".13.1", ENT + ".9.1")
        self.assertEqual(0, rc, out)
        self.dev.wait_for_menu(timeout=10)
        st = self.dev.status()
        self.assertRegex(st["serialNumber"], r"^[0-9A-F]{16}$")
        self.assertIn('STRING: "%s"' % st["serialNumber"], out)
        self.assertIn('STRING: "RF-Power-Monitor"', out)
        rc, descr = self.snmp("snmpget", "1.3.6.1.2.1.1.1.0")
        m = re.search(r"RF-Power-Monitor (\d+\.\d+\.\d+)", descr)
        self.assertIsNotNone(m, descr)
        self.assertIn('STRING: "%s"' % m.group(1), out)  # entPhysicalFirmwareRev = the version in sysDescr

    def test_cpu_load_is_low_when_idle_and_rises_under_load(self):
        # A minute with only these few requests: both cores near idle
        time.sleep(65)
        idle0, idle1 = self.cpu_load()
        print("\n    idle CPU load: core 0 %d %%, core 1 %d %%" % (idle0, idle1))
        self.assertLessEqual(idle0, 5)
        self.assertLessEqual(idle1, 5)
        self.dev.wait_for_menu(timeout=10)
        self.assertEqual((idle0, idle1), self.dev.status()["cpuLoad"])

        # A minute of SNMP walks and web page loads keeps core 0 busy. SNMP alone stays near 5 %:
        # the rate limit allows one source 50 requests/s, each about 1 ms of work
        stop = time.time() + 60
        def walk():
            while time.time() < stop:
                net_snmp("snmpbulkwalk", self.ip, "1.3.6.1.2.1", version="2c")
        def browse():
            while time.time() < stop:
                try:
                    conn = http.client.HTTPConnection(self.ip, 80, timeout=5)
                    conn.request("GET", "/")
                    conn.getresponse().read()
                    conn.close()
                except OSError:
                    time.sleep(0.1)
        workers = [threading.Thread(target=walk), threading.Thread(target=browse)]
        for w in workers:
            w.start()
        for w in workers:
            w.join()
        busy0, busy1 = self.cpu_load()
        print("    under SNMP and web load: core 0 %d %%, core 1 %d %%" % (busy0, busy1))
        self.assertGreaterEqual(busy0, max(10, 2 * idle0))
        self.assertLessEqual(busy0, 100)
        self.assertLessEqual(busy1, 5, "core 1 only samples the mains input")


if __name__ == "__main__":
    unittest.main()
