"""The check-in wire format, site side (the firmware's is lib/update_client/update_protocol).

Check-in body: LF-terminated key=value lines in a fixed order, authenticated by
the X-RFPower-Auth header, a keyed BLAKE2b MAC (32 bytes, hex) over the body.
Reply: lines, then "auth=" MAC(key, nonce + "\\n" + every byte before the auth line).
Both sides are tested against tests/vectors.json.
"""

import hashlib
import hmac
import re

BODY_MAX = 1024
IPV6_MAX = 4
_NONCE = re.compile(r"[0-9a-f]{32}")
_HEX64 = re.compile(r"[0-9a-fA-F]{64}")

# Fixed field order; "ipv6" (0-4 lines) comes between ipv4 and mac
_BEFORE = ("serial", "hostname", "ipv4")
_AFTER = ("mac", "version", "slot", "trial", "update", "fail", "last", "nonce")


def mac_hex(key: bytes, data: bytes) -> str:
    """BLAKE2b keyed with key, 32-byte output, lowercase hex."""
    return hashlib.blake2b(data, digest_size=32, key=key).hexdigest()


def verify_mac(key: bytes, data: bytes, got) -> bool:
    """True if got (hex, either case) is the MAC of data; constant time."""
    if not isinstance(got, str) or not _HEX64.fullmatch(got):
        return False
    return hmac.compare_digest(mac_hex(key, data), got.lower())


def parse_checkin(body: bytes) -> dict:
    """Fields of a check-in body; raises ValueError for anything but the exact format.

    The result has the keys of the format, "ipv6" as a list and "trial" as a bool.
    """
    if len(body) > BODY_MAX:
        raise ValueError("check-in too long")
    try:
        text = body.decode("ascii")
    except UnicodeDecodeError:
        raise ValueError("not ASCII") from None
    if not text.endswith("\n"):
        raise ValueError("truncated")
    lines = text[:-1].split("\n")
    for line in lines:
        if any(ord(c) < 0x20 or ord(c) == 0x7F for c in line):
            raise ValueError("control character")
    pairs = []
    for line in lines:
        key, sep, value = line.partition("=")
        if not sep:
            raise ValueError("not key=value: %r" % line)
        pairs.append((key, value))

    fields = {"ipv6": []}
    i = 0
    for name in _BEFORE:
        if i >= len(pairs) or pairs[i][0] != name:
            raise ValueError("expected %s" % name)
        fields[name] = pairs[i][1]
        i += 1
    while i < len(pairs) and pairs[i][0] == "ipv6":
        fields["ipv6"].append(pairs[i][1])
        i += 1
    if len(fields["ipv6"]) > IPV6_MAX:
        raise ValueError("too many ipv6 lines")
    for name in _AFTER:
        if i >= len(pairs) or pairs[i][0] != name:
            raise ValueError("expected %s" % name)
        fields[name] = pairs[i][1]
        i += 1
    if i != len(pairs):
        raise ValueError("extra lines")

    if not fields["serial"] or not fields["version"]:
        raise ValueError("serial and version are required")
    if fields["slot"] not in ("A", "B"):
        raise ValueError("bad slot")
    if fields["trial"] not in ("0", "1"):
        raise ValueError("bad trial")
    fields["trial"] = fields["trial"] == "1"
    if fields["update"] not in ("idle", "downloading"):
        raise ValueError("bad update state")
    if not _NONCE.fullmatch(fields["nonce"]):
        raise ValueError("bad nonce")
    return fields


def build_reply(key: bytes, nonce: str, lines: list) -> bytes:
    """A reply: the lines, then the auth line over the device's nonce and the lines."""
    pre = "".join(line + "\n" for line in lines).encode("ascii")
    auth = mac_hex(key, nonce.encode("ascii") + b"\n" + pre)
    return pre + b"auth=" + auth.encode("ascii") + b"\n"
