"""Hardware-in-the-loop tests for what the bench tool (tools/bench_provision.py) uses:
the hidden menu commands benchinfo, pwlock and pwunlock, and option 7 refusing
while the default web password is locked.

Run:
    %USERPROFILE%\\.platformio\\penv\\Scripts\\python.exe -m unittest discover -s test/hil -p "test_bench.py" -v

Only the serial port is needed. Each test restores the lock state it found.
test_unlock_allows_option_7_again replaces the device default web password
(like test_web.py's test_new_default_password_from_serial_menu).
"""

import re
import time
import unittest

from device import Device


class BenchCommandsTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.dev = Device()
        cls.dev.open()
        cls.dev.wait_for_menu()

    @classmethod
    def tearDownClass(cls):
        cls.dev.close()

    def setUp(self):
        self.dev.wait_for_menu(timeout=10)
        was = self.dev.bench_info()["locked"] == "1"
        self.addCleanup(lambda: self.dev.lock_web_default_password(was))

    def test_benchinfo_reports_hostname_mac_and_default_password(self):
        st = self.dev.status()
        info = self.dev.bench_info()
        self.assertEqual(st["sysName"], info["hostname"])
        self.assertEqual(st["mac"], info["mac"])
        self.assertEqual(st["serialNumber"], info["serial"])
        self.assertEqual(st["fwVersion"], info["firmware"])
        self.assertEqual(st["fwKey"], info["fwkey"])
        self.assertRegex(info["password"], r"^[a-km-np-z2-9]{10}$")
        if st["webDefaultPassword"]:  # shown only while no own password is set
            self.assertEqual(st["webDefaultPassword"], info["password"])
        self.assertIn(info["locked"], ("0", "1"))
        self.assertEqual("0", info["lost"])

    def test_locked_default_password_refuses_option_7(self):
        self.assertIn("Default web password locked", self.dev.lock_web_default_password(True))
        before = self.dev.bench_info()
        self.assertEqual("1", before["locked"])
        out = self.dev.send("7", expect=r"(is locked[\s\S]*> $|\(y/n\): $)")
        self.assertIn("is locked because it is printed on the device label", out)
        self.assertEqual(before["password"], self.dev.bench_info()["password"])
        self.assertRegex(self.dev.status()["raw"], r"Default web password: locked \(on the label\)")

    def test_unlock_allows_option_7_again(self):
        self.dev.lock_web_default_password(True)
        self.assertIn("Default web password unlocked", self.dev.lock_web_default_password(False))
        self.assertEqual("0", self.dev.bench_info()["locked"])
        self.assertRegex(self.dev.status()["raw"], r"Default web password: unlocked")
        old = self.dev.bench_info()["password"]
        out = self.dev.regenerate_web_default_password()
        new = re.search(r'New default web password: "(\w+)"', out).group(1)
        self.assertNotEqual(old, new)
        self.assertEqual(new, self.dev.bench_info()["password"])

    def test_lock_and_password_survive_reboot(self):
        if self.dev.status()["mode"] != "DHCP":
            self.skipTest("reboots with menu option 2 (Use DHCP), which would change a static setup")
        self.dev.lock_web_default_password(True)
        password = self.dev.bench_info()["password"]
        self.dev.send("2", expect=r"\(y/n\): $")  # "Use DHCP" reboots, the device already uses DHCP
        self.dev.send("y", expect=r"Rebooting", timeout=10)
        self.dev.close()
        time.sleep(1.5)
        self.dev.open()
        self.dev.wait_for_menu()
        self.dev.wait_connected()  # let the network messages pass before the next command
        info = self.dev.bench_info()
        self.assertEqual("1", info["locked"])
        self.assertEqual(password, info["password"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
