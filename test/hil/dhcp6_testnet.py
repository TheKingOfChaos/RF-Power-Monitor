"""DHCPv6 test network, run from this PC for the HIL tests (opt-in, RFPOWER_DHCP6_TEST=1).

In WSL (mirrored networking, as root via `wsl -u root`): router advertisements sent
unicast to the device's link-local address only (M and/or O flag, router lifetime 0,
no prefix), and Kea answering only the device's DUID. Nothing else on the LAN sees an
M flag or gets an answer. Always use it as a context manager so it is stopped.
"""

import ipaddress
import os
import subprocess

DISTRO = os.environ.get("RFPOWER_WSL_DISTRO", "Ubuntu-24.04")
HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = "dhcp6/testnet.sh"


def wsl_path(path):
    """Windows path -> /mnt/<drive>/... for WSL."""
    drive, rest = os.path.splitdrive(os.path.abspath(path))
    return "/mnt/%s%s" % (drive[0].lower(), rest.replace("\\", "/"))


def device_duid_ll(mac):
    """DUID-LL (type 3, hardware type 1 = Ethernet) for a MAC, as Kea prints it."""
    return "00:03:00:01:" + mac.lower().replace("-", ":")


class TestNet:
    """The router advertisement sender and Kea, for one device."""

    def __init__(self, device_mac, device_link_local, device_global, interface="eth0"):
        self.mac = device_mac
        self.link_local = device_link_local.lower()
        net = ipaddress.IPv6Network(device_global.lower() + "/64", strict=False)
        self.subnet = str(net)
        self.interface = interface

    def _wsl(self, *args, timeout=30):
        cmd = ["wsl", "-d", DISTRO, "-u", "root", "--", "bash", wsl_path(os.path.join(HERE, SCRIPT))] + list(args)
        return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)

    def default_pool(self):
        """A small pool in the device's /64 that SLAAC (EUI-64 ids) never uses."""
        base = ipaddress.IPv6Network(self.subnet).network_address
        return "%s-%s" % (base + 0xd6000001, base + 0xd60000ff)

    def start(self, managed, other, valid_s=120, preferred_s=60, pool=None, renew_s=None, rebind_s=None):
        renew_s = renew_s if renew_s is not None else preferred_s // 2
        rebind_s = rebind_s if rebind_s is not None else preferred_s * 4 // 5
        r = self._wsl("start", self.interface, self.mac, self.link_local, "1" if managed else "0",
                      "1" if other else "0", self.subnet, pool or self.default_pool(), str(valid_s),
                      str(preferred_s), str(renew_s), str(rebind_s))
        if r.returncode != 0:
            raise RuntimeError("test network did not start: %s %s" % (r.stdout, r.stderr))
        return r.stdout

    def stop(self):
        return self._wsl("stop").stdout

    def kea_log(self):
        return self._wsl("log").stdout

    def ra_log(self):
        return self._wsl("ralog").stdout

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.stop()
        return False
