"""Device side of the bench tool (tools/bench_provision.py): the release UF2
check, the serial connection and the hidden menu commands benchinfo and pwlock.

The device prints its data between BENCHINFO BEGIN and BENCHINFO END, one
key=value per line (lib/bench_info). The UF2 check reads the slot A header
(lib/fw_image/include/fw_image.h) to learn the version and the key id the
firmware was signed with; the boot program verifies the signature itself.
"""

import os
import re
import struct
import sys
import time
from dataclasses import dataclass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import fwtool  # noqa: E402
import uf2  # noqa: E402

DEV_HEADER = os.path.join(ROOT, "include", "fw_dev_public_key.h")
FLASH_START = 0x10000000
SLOT_A = FLASH_START + 0x10000  # FW_SLOT_OFFSET[0], lib/fw_image/include/fw_layout.h
SLOT_MAGIC = 0x57464652          # "RFFW"
BENCH_KEYS = ("hostname", "mac", "serial", "firmware", "fwkey", "password", "locked", "lost")
BAUD = 115200
PROMPT = "> "
PICO_VID = 0x2E8A


class BenchError(Exception):
    """A refusal or failure, with a message for the person at the bench."""


@dataclass
class UF2Firmware:
    """A release UF2 file: its path, firmware version and signing key id (16 hex)."""
    path: str
    version: str
    key_id: str


def parse_bench_info(text: str) -> dict:
    """The last complete benchinfo block in text as a dict; BenchError if there is none or a key is missing."""
    blocks = re.findall(r"BENCHINFO BEGIN\r?\n((?:(?!BENCHINFO BEGIN)[\s\S])*?)BENCHINFO END", text)
    if not blocks:
        raise BenchError("no complete device data block (benchinfo) received")
    info = {}
    for line in blocks[-1].splitlines():
        key, sep, value = line.strip("\r").partition("=")
        if sep:
            info[key] = value
    missing = [k for k in BENCH_KEYS if k not in info]
    if missing:
        raise BenchError("device data block lacks %s" % ", ".join(missing))
    return info


def _uf2_pages(data: bytes) -> dict:
    """flash address -> 256 byte payload of every UF2 block; BenchError if data is not UF2."""
    if not data or len(data) % 512:
        raise BenchError("not a UF2 file")
    pages = {}
    for off in range(0, len(data), 512):
        m0, m1, _flags, addr, size = struct.unpack_from("<5I", data, off)
        end = struct.unpack_from("<I", data, off + 508)[0]
        if m0 != uf2.UF2_MAGIC_START0 or m1 != uf2.UF2_MAGIC_START1 or end != uf2.UF2_MAGIC_END or size > 476:
            raise BenchError("not a UF2 file (bad block at byte %d)" % off)
        pages[addr] = data[off + 32:off + 32 + size]
    return pages


def read_release_uf2(path: str) -> UF2Firmware:
    """Check a release UF2 (boot program + signed slot A) and read its version and key id.

    Refuses firmware.uf2 and any name not ending in -usb.uf2, a file without
    the boot program at the start of flash, and a slot A without a valid header.
    """
    name = os.path.basename(path)
    if not name.endswith("-usb.uf2"):
        raise BenchError("%s is not a release -usb.uf2 (never use firmware.uf2: it has no boot program and "
                         "the device will not start); build one with tools/fw_release.py --field" % name)
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as e:
        raise BenchError("cannot read %s: %s" % (path, e))
    pages = _uf2_pages(data)
    if FLASH_START not in pages:
        raise BenchError("%s has no boot program at the start of flash" % name)
    header = pages.get(SLOT_A, b"")
    if len(header) < 104:
        raise BenchError("%s has no firmware in slot A" % name)
    magic = struct.unpack_from("<I", header, 64)[0]
    if magic != SLOT_MAGIC:
        raise BenchError("%s: slot A has no valid firmware header" % name)
    raw = header[80:96].split(b"\0", 1)[0]
    try:
        version = raw.decode("ascii")
    except UnicodeDecodeError:
        raise BenchError("%s: slot A version is not text" % name)
    return UF2Firmware(path=path, version=version, key_id=header[96:104].hex())


def field_key_id() -> str:
    """Key id of the field key (public header include/fw_public_key.h)."""
    return fwtool.key_fingerprint(fwtool.header_public_key(fwtool.FIELD_HEADER))


def dev_key_id() -> str:
    """Key id of the development key (include/fw_dev_public_key.h)."""
    return fwtool.key_fingerprint(fwtool.header_public_key(DEV_HEADER))


def check_key(key_id: str, dev: bool, what: str) -> None:
    """BenchError unless key_id is the field key, or the development key when dev."""
    if key_id == field_key_id():
        return
    if key_id == dev_key_id():
        if dev:
            return
        raise BenchError("%s uses the development key: devices built this way refuse field-signed updates. "
                         "Use tools/fw_release.py --field (or --dev to test the bench tool)" % what)
    raise BenchError("%s uses an unknown key (id %s)" % (what, key_id))


def check_signed_for(fw: UF2Firmware, dev: bool) -> None:
    """BenchError unless the UF2 is signed with the field key (or the development key when dev)."""
    check_key(fw.key_id, dev, os.path.basename(fw.path))


class BenchSerial:
    """The device's USB serial menu (DTR set after opening, as test/hil/device.py)."""

    def __init__(self, port: str):
        self.port = port
        self.ser = None

    def open(self, timeout: float = 10.0) -> None:
        """Open the port, retrying while Windows still holds it after the device appeared."""
        import serial
        deadline = time.time() + timeout
        while True:
            try:
                ser = serial.Serial(None, BAUD, timeout=0.1, dsrdtr=False)
                ser.port = self.port
                ser.dtr = False
                ser.open()
                ser.dtr = True  # the firmware prints only once DTR is set
                self.ser = ser
                return
            except serial.SerialException as e:
                if time.time() > deadline:
                    raise BenchError("cannot open %s (is PuTTY using it?): %s" % (self.port, e))
                time.sleep(0.5)

    def close(self) -> None:
        """Close the port (safe to call twice)."""
        if self.ser is not None:
            self.ser.close()
            self.ser = None

    def _read_until(self, pattern: str, timeout: float) -> str:
        regex = re.compile(pattern)
        buf = ""
        deadline = time.time() + timeout
        while time.time() < deadline:
            chunk = self.ser.read(256)
            if chunk:
                buf += chunk.decode("utf-8", errors="replace")
                if regex.search(buf):
                    return buf
        raise BenchError("no answer from the device on %s (waited %.0f s)" % (self.port, timeout))

    def command(self, line: str, until: str, timeout: float = 5.0) -> str:
        """Send one menu line and return what came back up to the regex until."""
        self.ser.write((line + "\r").encode())
        return self._read_until(until, timeout)

    def wait_for_menu(self, timeout: float = 40.0) -> None:
        """Wait for the menu prompt, nudging with '?' while the device boots."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.ser.reset_input_buffer()
            try:
                self.command("?", re.escape(PROMPT) + r"$", timeout=3.0)
                return
            except BenchError:
                continue
        raise BenchError("the device menu did not appear on %s within %.0f s" % (self.port, timeout))


def read_info(ser) -> dict:
    """Send benchinfo and return the parsed block.

    Firmware before 3.5.0 does not know the command and prints its menu instead.
    """
    text = ser.command("benchinfo", r"BENCHINFO END\r?\n|Show this menu[\s\S]*> ")
    if "BENCHINFO BEGIN" not in text:
        raise BenchError("the device firmware is older than 3.5.0 (no benchinfo command): update it first "
                         "(web page Firmware, or tools/fw_update.py), then run again")
    return parse_bench_info(text)


def lock(ser) -> None:
    """Lock the default password (pwlock) and check that the device reports it locked."""
    ser.command("pwlock", r"(Default web password locked|Failed to lock).*\r?\n")
    if read_info(ser)["locked"] != "1":
        raise BenchError("the device did not lock its default web password")


def pico_ports() -> list:
    """Serial ports of connected RP2040 boards (USB VID 0x2E8A)."""
    from serial.tools import list_ports
    return sorted(p.device for p in list_ports.comports() if p.vid == PICO_VID)
