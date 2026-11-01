"""Switching the device's mains input for the mains HIL tests.

Two ways, chosen with environment variables:

    RFPOWER_MAINS_SWITCH=dirigera   Automatic: an outlet on an IKEA Dirigera hub feeds
                                  the mains detection circuit. Also needs
        RFPOWER_DIRIGERA_IP         hub address, e.g. 10.10.6.124
        RFPOWER_DIRIGERA_OUTLET     outlet name, e.g. "Stikprop 1"
        RFPOWER_DIRIGERA_TOKEN      token file (default ~/dirigera/token, on the WSL side on Windows)
        RFPOWER_DIRIGERA_PYTHON     Python with the dirigera package
                                  (default ~/dirigera/.venv/bin/python, on the WSL side on Windows)
    RFPOWER_MANUAL_MAINS=1          Manual: the test asks you to switch mains off and on.

With neither set, mains tests are skipped. Mains is always switched back on
at the end of a test, also when it fails.
"""

import os
import subprocess
import sys
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
WSL_DISTRO = os.environ.get("RFPOWER_WSL_DISTRO", "Ubuntu-24.04")
SKIP_REASON = "set RFPOWER_MAINS_SWITCH=dirigera (automatic) or RFPOWER_MANUAL_MAINS=1 (manual) to run"


class DirigeraSwitch:
    """Outlet on a Dirigera hub, switched through dirigera_outlet.py."""

    automatic = True

    def __init__(self):
        self.ip = os.environ["RFPOWER_DIRIGERA_IP"]
        self.outlet = os.environ["RFPOWER_DIRIGERA_OUTLET"]
        self.token = os.environ.get("RFPOWER_DIRIGERA_TOKEN", "~/dirigera/token")
        self.python = os.environ.get("RFPOWER_DIRIGERA_PYTHON", "~/dirigera/.venv/bin/python")

    def _run(self, command):
        script = os.path.join(HERE, "dirigera_outlet.py")
        args = ["--ip", self.ip, "--token-file", self.token, "--outlet", self.outlet, command]
        if sys.platform == "win32":
            # The dirigera package lives in WSL. --exec skips WSL's own shell, so only the
            # sh started here expands '~' in the Python path
            script = subprocess.run(["wsl", "-d", WSL_DISTRO, "--exec", "wslpath", "-a", script],
                                    capture_output=True, text=True, check=True).stdout.strip()
            cmd = ["wsl", "-d", WSL_DISTRO, "--exec", "sh", "-c", 'exec %s -I "$@"' % self.python,
                   "sh", script] + args
        else:
            cmd = [os.path.expanduser(self.python), "-I", script] + args
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        if result.returncode != 0:
            raise RuntimeError("Dirigera outlet %s failed: %s" % (command, (result.stderr or result.stdout).strip()))
        return result.stdout.strip()

    def state(self):
        return self._run("status")

    def off(self):
        state = self._run("off")
        if state != "off":
            raise RuntimeError("outlet reports %r after switching off" % state)

    def on(self):
        state = self._run("on")
        if state != "on":
            raise RuntimeError("outlet reports %r after switching on" % state)

    def restore(self):
        """Mains on again; call in a finally block so a failing test never leaves it off."""
        self.on()


class ManualSwitch:
    """A person at the switch."""

    automatic = False

    def off(self):
        print("\n>>> Switch mains power OFF now (60 s)...", flush=True)

    def on(self):
        print(">>> Switch mains power ON again (60 s)...", flush=True)

    def restore(self):
        pass


def get_switch():
    """The configured switch, or None when mains tests should be skipped."""
    if os.environ.get("RFPOWER_MAINS_SWITCH") == "dirigera":
        return DirigeraSwitch()
    if os.environ.get("RFPOWER_MANUAL_MAINS"):
        return ManualSwitch()
    return None


def skip_without_switch(test):
    """Decorator: skip a mains test when no switch is configured."""
    return unittest.skipIf(get_switch() is None, SKIP_REASON)(test)


def wait_until(condition, timeout, interval=0.2):
    """Poll condition() until it returns a truthy value; return it, or None on timeout."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        value = condition()
        if value:
            return value
        time.sleep(interval)
    return None
