"""A fake board's serial menu for the bench tool tests (tools/bench_device.py, bench_provision.py).

Answers the commands the bench tool sends like firmware 3.5.0: benchinfo,
pwlock, menu option 10 (URL, then key) and updatedefault. Not a test module.
"""

import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import bench_device  # noqa: E402


class FakeBoard:
    """The device's menu. values holds the benchinfo fields; sent lists every line sent."""

    def __init__(self, lock_works=True, **values):
        self.values = {"hostname": "RF-Power-A1B2C3", "mac": "2C:CF:67:A1:B2:C3", "serial": "E6614103E7A1B2C3",
                       "firmware": "3.5.0", "fwkey": bench_device.field_key_id(), "password": "k7mq2xr9tp",
                       "locked": "0", "lost": "0", "updatedefault": "none", "updatedefaultkey": "0"}
        self.values.update(values)
        self.lock_works = lock_works
        self.sent = []
        self.closed = False
        self.update_url, self.update_key = "none", ""  # option 10 now
        self._menu10 = None  # None, "url" or "key" while option 10 asks
        self._new_url = ""

    def wait_for_menu(self, timeout=40.0):
        pass

    def command(self, line, until, timeout=5.0):
        self.sent.append(line)
        if self._menu10 == "url":
            self._new_url = line or self.update_url
            self._menu10 = "key"
            return line + "\r\nShared key (64 hex characters from the update site), Enter keeps, clear removes [%s]: " % (
                "set" if self.update_key else "not set")
        if self._menu10 == "key":
            self._menu10 = None
            self.update_url = self._new_url
            if line == "clear":
                self.update_key = ""
            elif line:
                self.update_key = line
            return "*" * len(line) + "\r\nUpdate server settings saved\r\nUpdate server: %s (fixed), key set\r\n> " % (
                self.update_url)
        if line == "10":
            self._menu10 = "url"
            return "10\r\nUpdate server URL (http://host[:port][/path]), dhcp or none [%s]: " % self.update_url
        if line == "pwlock":
            if self.lock_works:
                self.values["locked"] = "1"
            return "Default web password locked (it is on the device label)\r\n"
        if line == "updatedefault":
            self.values["updatedefault"] = self.update_url
            self.values["updatedefaultkey"] = "1" if self.update_key else "0"
            return "Update server default saved (a factory reset restores it)\r\n"
        if line == "benchinfo":
            lines = "".join("%s=%s\r\n" % (k, self.values[k]) for k in bench_device.BENCH_KEYS)
            return "BENCHINFO BEGIN\r\n" + lines + "BENCHINFO END\r\n> "
        raise AssertionError("unexpected command " + line)

    def close(self):
        self.closed = True
