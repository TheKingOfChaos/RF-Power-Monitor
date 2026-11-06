"""Serial-port driver for the RF-Power-Monitor menu, used by the HIL tests.

Talks to the device exactly like a person in PuTTY: sends a line terminated
by CR and reads what comes back.
"""

import os
import re
import subprocess
import sys
import time

import serial
from serial.tools import list_ports

PORT = os.environ.get("RFPOWER_PORT", "COM16")
BAUD = 115200
PROMPT = "> "


class Device:
    """One serial session with the device."""

    def __init__(self, port=PORT):
        self.port = port
        self.ser = None
        self.log = ""

    # ---- connection ----

    def wait_for_port(self, timeout=20.0):
        """Wait until the USB serial port is listed (it vanishes during reboot)."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if any(p.device.upper() == self.port.upper() for p in list_ports.comports()):
                return
            time.sleep(0.2)
        raise TimeoutError("%s did not appear within %.0f s" % (self.port, timeout))

    def open(self):
        self.wait_for_port()
        deadline = time.time() + 10
        while True:
            try:
                # DTR must be set, the firmware waits for it before printing. Set it only
                # after opening: pyserial clears the receive buffer while it opens the
                # port, which lost the first boot lines when DTR was already set
                ser = serial.Serial(None, BAUD, timeout=0.1, dsrdtr=False)
                ser.port = self.port
                ser.dtr = False
                ser.open()
                ser.dtr = True
                self.ser = ser
                return
            except serial.SerialException as e:
                if time.time() > deadline:
                    raise RuntimeError(
                        "cannot open %s (is PuTTY or a serial monitor using it?): %s" % (self.port, e))
                time.sleep(0.5)

    def close(self):
        if self.ser is not None:
            try:
                self.ser.close()
            except serial.SerialException:
                pass
            self.ser = None

    # ---- I/O ----

    def read_until(self, pattern, timeout=5.0):
        """Read until the regex pattern appears; return everything read."""
        regex = re.compile(pattern)
        buf = ""
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                chunk = self.ser.read(256)
            except serial.SerialException:
                break
            if chunk:
                buf += chunk.decode("utf-8", errors="replace")
                if regex.search(buf):
                    self.log += buf
                    return buf
        self.log += buf
        raise TimeoutError("pattern %r not seen within %.0f s, got:\n%s" % (pattern, timeout, buf))

    def send(self, line, expect=None, timeout=5.0):
        """Send one line. If expect is given, read until it appears and return the output."""
        self.ser.write((line + "\r").encode())
        if expect is None:
            return ""
        return self.read_until(expect, timeout)

    def wait_for_menu(self, timeout=40.0):
        """Wait for a prompt, nudging with '?' since the menu may have printed before we connected."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.ser.reset_input_buffer()
            self.ser.write(b"?\r")
            try:
                return self.read_until(re.escape(PROMPT) + r"$", timeout=3.0)
            except TimeoutError:
                continue  # Still booting or waiting for DHCP
        raise TimeoutError("menu prompt not seen within %.0f s" % timeout)

    def reconnect_after_reboot(self, timeout=40.0):
        """Close, wait for the port to drop and return, reopen, wait for the menu and network."""
        self.close()
        time.sleep(1.5)
        self.open()
        self.wait_for_menu(timeout)
        return self.wait_connected(timeout)

    def wait_connected(self, timeout=40.0):
        """Poll the status screen until the device reports an address.

        The menu is available right after boot, before the link and DHCP lease,
        so anything that needs the network must wait for this first.
        """
        deadline = time.time() + timeout
        st = None
        while time.time() < deadline:
            st = self.status()
            if st["state"] == "connected" and st["ip"]:
                return st
            time.sleep(1.0)
        raise TimeoutError("device not connected within %.0f s, last state: %s"
                           % (timeout, st and st["state"]))

    # ---- menu helpers ----

    def status(self):
        """Run menu option 1 and return a dict with mode, ip, subnet, gateway."""
        out = self.send("1", expect=r"DNS: .*\r?\n" + re.escape(PROMPT))
        info = {"raw": out}
        m = re.search(r"Mode: (\w+)", out)
        info["mode"] = m.group(1) if m else None
        m = re.search(r"State: (.*?)\r?$", out, re.MULTILINE)
        info["state"] = m.group(1) if m else None
        m = re.search(r"Mains power: (\w+)", out)
        info["mains"] = m.group(1) if m else None
        m = re.search(r"Mains alarm: (.*?)\r?$", out, re.MULTILINE)
        info["mainsAlarm"] = m.group(1) if m else None
        m = re.search(r"^Switch: (.*?)\r?$", out, re.MULTILINE)
        info["switch"] = m.group(1) if m else None
        m = re.search(r"^DHCPv6: (off|[a-z]+)(?:, ([0-9a-f:]+) \(valid (\d+ s|forever)\))?", out, re.MULTILINE)
        info["dhcp6State"] = m.group(1) if m else None
        info["dhcp6Address"] = m.group(2) if m else None
        m = re.search(r"^Last restart: (.+) \((\d+) watchdog restarts since power-on\)\r?$", out, re.MULTILINE)
        info["lastRestart"] = m.group(1) if m else None
        info["watchdogRestarts"] = int(m.group(2)) if m else None
        m = re.search(r"^Firmware: slot ([AB]), (\S+), (kept|trial)\r?$", out, re.MULTILINE)
        info["fwSlot"] = m.group(1) if m else None
        info["fwVersion"] = m.group(2) if m else None
        info["fwState"] = m.group(3) if m else None
        m = re.search(r"^Other slot: ([AB]), (.+?)\r?$", out, re.MULTILINE)
        info["fwOtherSlot"] = m.group(1) if m else None
        info["fwOther"] = m.group(2) if m else None
        m = re.search(r"^Last update: (.+?)\r?$", out, re.MULTILINE)
        info["fwLastUpdate"] = m.group(1) if m else None
        m = re.search(r"^Firmware key: ([0-9a-f]{16})\r?$", out, re.MULTILINE)
        info["fwKey"] = m.group(1) if m else None
        m = re.search(r"^CPU load: core 0 (\d+) %, core 1 (\d+) %", out, re.MULTILINE)
        info["cpuLoad"] = (int(m.group(1)), int(m.group(2))) if m else None
        m = re.search(r"^RAM used: (\d+) of (\d+) bytes", out, re.MULTILINE)
        info["ramUsed"] = (int(m.group(1)), int(m.group(2))) if m else None
        m = re.search(r"^LittleFS used: (\d+) of (\d+) bytes", out, re.MULTILINE)
        info["fsUsed"] = (int(m.group(1)), int(m.group(2))) if m else None
        m = re.search(r"^Serial number: ([0-9A-F]+)\r?$", out, re.MULTILINE)
        info["serialNumber"] = m.group(1) if m else None
        for key, label in (("community", "SNMP community"), ("sysName", "sysName"),
                           ("sysContact", "sysContact"), ("sysLocation", "sysLocation")):
            m = re.search(r'^%s: "(.*)"\r?$' % label, out, re.MULTILINE)
            info[key] = m.group(1) if m else None
        m = re.search(r'^Trap destination: "(.*)" \((\d+) sent, (\d+) failed\)', out, re.MULTILINE)
        info["trapTarget"] = m.group(1) if m else None
        info["trapsSent"] = int(m.group(2)) if m else None
        m = re.search(r"^SNMP security: (\d+) rate limited, (\d+) access log events", out, re.MULTILINE)
        info["rateLimited"] = int(m.group(1)) if m else None
        m = re.search(r"^Trap version: (v1|v2c)", out, re.MULTILINE)
        info["trapVersion"] = {"v1": "1", "v2c": "2c"}[m.group(1)] if m else None
        m = re.search(r"^CPU temperature: (-?\d+\.\d) C", out, re.MULTILINE)
        info["cpuTempC"] = float(m.group(1)) if m else None
        # None without the TMP1075 ("no sensor")
        m = re.search(r"^Ambient temperature: (-?\d+\.\d) C", out, re.MULTILINE)
        info["ambientTempC"] = float(m.group(1)) if m else None
        m = re.search(r"^Ethernet: link (up|down), (\d+) Mbit/s", out, re.MULTILINE)
        info["linkUp"] = (m.group(1) == "up") if m else None
        info["linkMbps"] = int(m.group(2)) if m else None
        m = re.search(r"^Ethernet drops: (\d+) discarded \(no buffer\), (\d+) errors, (\d+) unknown protocol, "
                      r"(\d+) send errors", out, re.MULTILINE)
        for i, key in enumerate(("inDiscards", "inErrors", "inUnknownProtos", "outErrors")):
            info[key] = int(m.group(i + 1)) if m else None
        m = re.search(r'^Web login: user admin, (?:default password "(\w+)"|(own password))', out, re.MULTILINE)
        info["webDefaultPassword"] = m.group(1) if m else None
        info["webOwnPassword"] = bool(m and m.group(2))
        m = re.search(r"^Hostname: ([\w-]+) \(DHCP, mDNS ([\w-]+)\.local\)", out, re.MULTILINE)
        info["hostname"] = m.group(1) if m else None
        m = re.search(r"^Web interface: (on|off)", out, re.MULTILINE)
        info["webEnabled"] = (m.group(1) == "on") if m else None
        m = re.search(r"^Trap destination source: (.*?)\r?$", out, re.MULTILINE)
        info["trapSource"] = m.group(1) if m else None
        m = re.search(r"^Core 0 stack: (\d+) bytes never used", out, re.MULTILINE)
        info["stackNeverUsed"] = int(m.group(1)) if m else None
        m = re.search(r"^MAC: ([0-9A-F:]+)", out, re.MULTILINE)
        info["mac"] = m.group(1) if m else None
        m = re.search(r"^SNMP v2c: (\d+) requests, (\d+) getbulk", out, re.MULTILINE)
        info["v2cRequests"] = int(m.group(1)) if m else None
        info["getBulkRequests"] = int(m.group(2)) if m else None
        m = re.search(r"^Stored static IPv6: (?:([0-9a-f:]+)/64, gateway (.*?)|none.*?)\r?$", out, re.MULTILINE)
        info["staticIPv6"] = m.group(1) if m and m.group(1) else ""
        info["staticIPv6Gateway"] = m.group(2) if m and m.group(1) else ""
        # Live IPv6 addresses: (lower-case address, flags such as "static", "tentative")
        info["ipv6"] = [(a.lower(), flags) for a, flags in
                        re.findall(r"^IPv6: ([0-9A-Fa-f:]+)((?: \([^)]*\))*)\r?$", out, re.MULTILINE)]
        for key, label in (("ip", "IP"), ("subnet", "Subnet"), ("gateway", "Gateway")):
            m = re.search(r"^%s: ([\d.]+)" % label, out, re.MULTILINE)
            info[key] = m.group(1) if m else None
        return info


    def set_snmp(self, community, sys_name, sys_contact, sys_location, trap_target=None, trap_version=None):
        """Set the SNMP settings through menu option 4 and confirm.

        Empty values are cleared with '-'. trap_target / trap_version None keep the current value;
        trap_version is "1" or "2c".
        """
        def entry(value):
            return value if value else "-"
        self.send("4", expect=r"Community \[.*\]: $")
        self.send(entry(community), expect=r"\(sysName\) \[.*\]: $")
        self.send(entry(sys_name), expect=r"sysContact \[.*\]: $")
        self.send(entry(sys_contact), expect=r"sysLocation \[.*\]: $")
        self.send(entry(sys_location), expect=r"Trap destination IP or dhcp \[.*\]: $")
        self.send("" if trap_target is None else entry(trap_target), expect=r"Trap version \(1/2c\) \[.*\]: $")
        self.send("" if trap_version is None else trap_version, expect=r"Save\? \(y/n\): $")
        return self.send("y", expect=r"(SNMP settings saved and active|Failed to save)[\s\S]*> $")

    def set_trap_target(self, target, version=None):
        """Change only the trap destination ('' turns traps off) and optionally the trap version."""
        st = self.status()
        return self.set_snmp(st["community"], st["sysName"], st["sysContact"], st["sysLocation"], target, version)

    def set_web_password(self, password, repeat=None):
        """Menu option 6. password '-' goes back to the device default; returns the menu output."""
        self.send("6", expect=r"Enter to cancel: $")
        if password == "-":
            return self.send("-", expect=r"(reset to the device default|Failed)[\s\S]*> $")
        out = self.send(password, expect=r"(Repeat the password: |Not accepted[\s\S]*Enter to cancel: )$")
        if "Not accepted" in out:
            self.send("", expect=r"Cancelled[\s\S]*> $")
            return out
        return out + self.send(password if repeat is None else repeat,
                               expect=r"(Web password saved|passwords differ|Failed)[\s\S]*> $")

    def regenerate_web_default_password(self, confirm=True):
        """Menu option 7; returns the menu output (with the new password when confirmed)."""
        self.send("7", expect=r"\(y/n\): $")
        return self.send("y" if confirm else "n", expect=r"(New default web password|Cancelled)[\s\S]*> $")

    def bench_info(self):
        """Hidden command benchinfo: the device data block for the bench tool, as a dict."""
        out = self.send("benchinfo", expect=r"BENCHINFO END\r?\n[\s\S]*" + re.escape(PROMPT) + r"$")
        block = out[out.rindex("BENCHINFO BEGIN"):out.rindex("BENCHINFO END")]
        info = {}
        for line in block.splitlines()[1:]:
            key, sep, value = line.partition("=")
            if sep:
                info[key] = value
        return info

    def lock_web_default_password(self, lock):
        """Hidden command pwlock or pwunlock; returns the output up to its result line.

        Only the result line is awaited: after a reboot, network messages can follow the prompt.
        """
        return self.send("pwlock" if lock else "pwunlock",
                         expect=r"(Default web password (un)?locked|Failed to (un)?lock).*\r?\n")

    def set_web_enabled(self, on):
        """Menu option 8: switch the web interface on or off (no-op if already so). Returns the output."""
        st = self.status()
        if st["webEnabled"] == on:
            return ""
        self.send("8", expect=r"Turn it (on|off)\? \(y/n\): $")
        return self.send("y", expect=r"(Web interface (on|off)|Failed)[\s\S]*> $")

    def set_static_ipv6(self, address, gateway=""):
        """Menu option 9: set (or with address "" remove) the static IPv6 address, save and reboot."""
        self.send("9", expect=r"Static IPv6 address.*: $")
        if not address:
            self.send("-", expect=r"\(y/n\): $")
        else:
            self.send(address, expect=r"IPv6 gateway.*: $")
            self.send(gateway or "-", expect=r"\(y/n\): $")
        self.send("y", expect=r"Rebooting", timeout=10)
        self.reconnect_after_reboot()

    def set_update_server(self, url, key=""):
        """Menu option 10: url is a URL, "dhcp", "none" or "" (keep); key is 64 hex, "clear" or "" (keep).
        Returns the output from the key prompt on."""
        # No "$" after the prompts: a log line (e.g. "SNMP v2c trap ... sent") may follow them
        self.send("10", expect=r"Update server URL [^\n]*\]: ")
        out = self.send(url, expect=r"(Shared key [^\n]*\]: |not an http[\s\S]*URL [^\n]*\]: )")
        if "not an http" in out:
            self.send("none", expect=r"Shared key [^\n]*\]: ")
            self.send("", expect=re.escape(PROMPT) + r"$")
            return out
        return self.send(key, expect=r"(saved|failed|must)[\s\S]*" + re.escape(PROMPT) + r"$")

    def update_server_line(self):
        """The status screen's "Update server: ..." line."""
        st = self.status()
        m = re.search(r"^Update server: (.*?)\r?$", st["raw"], re.MULTILINE)
        return m.group(1) if m else None

    def access_log(self):
        """Menu option 5: list of (ip, event, count) tuples, newest first."""
        out = self.send("5", expect=r"Access log: [\s\S]*> $")
        return [(m.group(1), m.group(2).strip(), int(m.group(3)))
                for m in re.finditer(r"^\s+([\d.]+|[0-9a-f:]*:[0-9a-f:]*)\s+(.+?)\s+x(\d+)\s+last", out, re.MULTILINE)]


def ping(host, count=2):
    """Ping from the PC running the tests; True if any reply arrives."""
    flag = "-n" if sys.platform == "win32" else "-c"
    result = subprocess.run(["ping", flag, str(count), host],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return result.returncode == 0 and ("TTL=" in result.stdout or "ttl=" in result.stdout)
