"""Shared test helpers: fake .upd files and signed check-ins from a fake device."""

import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
from rfupdate import protocol  # noqa: E402

KEY = bytes(range(32))
SERIAL = "E6614103E7452D2F"


def fake_upd(version, blob_a=b"A" * 300, blob_b=b"B" * 400):
    """A .upd with the real header layout (tools/fw_release.py build_upd) and dummy blobs.
    The site does not check signatures (the device does), so dummy blobs are enough."""
    v = version.encode("ascii")
    header = struct.pack("<II16s4I", 0x50554652, 1, v, 64, 64 + len(blob_a), len(blob_a), len(blob_b))
    return header + b"\xff" * (64 - len(header)) + blob_a + blob_b


class FakeDevice:
    """Builds signed check-ins the way the firmware does (lib/update_client update_protocol)."""

    def __init__(self, serial=SERIAL, version="3.4.0", key=KEY):
        self.serial = serial
        self.version = version
        self.key = key
        self.hostname = "rfpm-" + serial[-4:].lower()
        self.ipv4 = "10.132.0.58"
        self.ipv6 = ["2001:db8::5"]
        self.slot = "A"
        self.trial = False
        self.update = "idle"
        self.fail = ""
        self.last = "none"
        self.counter = 0

    def body(self, nonce=None):
        self.counter += 1
        nonce = nonce or ("%032x" % (0xABC000 + self.counter + hash(self.serial) % 1000 * 100000))
        lines = ["serial=" + self.serial, "hostname=" + self.hostname, "ipv4=" + self.ipv4]
        lines += ["ipv6=" + a for a in self.ipv6]
        lines += ["mac=28:cd:c1:00:00:01", "version=" + self.version, "slot=" + self.slot,
                  "trial=" + ("1" if self.trial else "0"), "update=" + self.update, "fail=" + self.fail,
                  "last=" + self.last, "nonce=" + nonce]
        return "".join(line + "\n" for line in lines).encode("ascii"), nonce

    def signed(self, nonce=None):
        """(body, auth header, nonce)"""
        body, nonce = self.body(nonce)
        return body, protocol.mac_hex(self.key, body), nonce


def reply_lines(reply, key=KEY, nonce=None):
    """The lines of a reply before its auth line; checks the MAC when nonce is given."""
    text = reply.decode("ascii")
    lines = text.rstrip("\n").split("\n")
    assert lines[-1].startswith("auth="), text
    if nonce is not None:
        pre = "".join(line + "\n" for line in lines[:-1]).encode("ascii")
        assert protocol.mac_hex(key, nonce.encode() + b"\n" + pre) == lines[-1][5:], "reply MAC"
    return lines[:-1]
