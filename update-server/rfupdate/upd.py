""".upd release files (layout: tools/fw_release.py build_upd) and version order.

The site only reads the header; the device checks the Ed25519 signatures.
"""

import re
import struct

UPD_MAGIC = 0x50554652  # "RFUP"
UPD_FORMAT = 1
HEADER_SIZE = 64
SLOT_SIZE = 0xD7000
MAX_UPLOAD = HEADER_SIZE + 2 * SLOT_SIZE  # FW_UPLOAD_MAX in the firmware: 1 761 344 bytes
MIN_VERSION = "3.4.0"  # the first firmware with check-ins; older ones could never report back

_VERSION = re.compile(r"(\d+)\.(\d+)\.(\d+)(?:-(.+))?")
_SAFE_VERSION = re.compile(r"[0-9A-Za-z.+-]{1,15}")


def parse_header(data: bytes) -> str:
    """The version of a .upd file; raises ValueError when data is not one (bad header or length)."""
    if len(data) < HEADER_SIZE:
        raise ValueError("not a .upd file (too short)")
    magic, fmt, v, off_a, off_b, len_a, len_b = struct.unpack_from("<II16s4I", data, 0)
    if magic != UPD_MAGIC or fmt != UPD_FORMAT:
        raise ValueError("not a .upd file")
    if b"\0" not in v:
        raise ValueError("not a .upd file (version not terminated)")
    version = v.split(b"\0", 1)[0].decode("ascii", "replace")
    if not _SAFE_VERSION.fullmatch(version):
        raise ValueError("not a .upd file (bad version)")
    if off_a != HEADER_SIZE or off_b != off_a + len_a or off_b + len_b != len(data) or len_a == 0 or len_b == 0:
        raise ValueError("not a .upd file (slot offsets do not match its length)")
    return version


def is_safe_version(version: str) -> bool:
    """True for text that can be a version and a file name (no path characters)."""
    return isinstance(version, str) and bool(_SAFE_VERSION.fullmatch(version))


def compare_versions(a: str, b: str) -> int:
    """<0 when a is older, 0 when equal, >0 when newer (as the firmware's compareVersions):
    numeric major.minor.patch; a pre-release ranks below its release; pre-releases compare as text."""
    ma, mb = _VERSION.fullmatch(a), _VERSION.fullmatch(b)
    if not ma or not mb:
        return (a > b) - (a < b)
    na = tuple(int(x) for x in ma.group(1, 2, 3))
    nb = tuple(int(x) for x in mb.group(1, 2, 3))
    if na != nb:
        return -1 if na < nb else 1
    pa, pb = ma.group(4), mb.group(4)
    if pa is None or pb is None:
        return (pa is None) - (pb is None)
    return (pa > pb) - (pa < pb)
