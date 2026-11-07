"""Helpers around the fwsign host program, shared by the build and release scripts.

fwsign (tools/fwsign/fwsign.cpp) links lib/monocypher and lib/fw_image, so
the PC signs with the same code the device verifies with. fwsign_path()
builds it into .pio/fwsign/ when it is missing or older than its sources.
"""

import glob
import hashlib
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, ".pio", "fwsign")
EXE = "fwsign.exe" if sys.platform == "win32" else "fwsign"
MONOCYPHER = os.path.join(ROOT, "lib", "monocypher", "src")
FW_IMAGE = os.path.join(ROOT, "lib", "fw_image")
SOURCES = [
    os.path.join(ROOT, "tools", "fwsign", "fwsign.cpp"),
    os.path.join(FW_IMAGE, "src", "fw_image.cpp"),
    os.path.join(FW_IMAGE, "src", "fw_sha512.cpp"),
    os.path.join(MONOCYPHER, "monocypher.c"),
    os.path.join(MONOCYPHER, "monocypher-ed25519.c"),
]
INCLUDE_DIRS = [MONOCYPHER, os.path.join(FW_IMAGE, "include")]

DEV_KEY = os.path.join(ROOT, "keys", "dev", "fw_signing_key")
FIELD_KEY = os.path.join(ROOT, "secret", "fw_signing_key")
FIELD_HEADER = os.path.join(ROOT, "include", "fw_public_key.h")


def _run(cmd):
    """Run a command, raising RuntimeError with its output when it fails."""
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError("%s failed:\n%s%s" % (cmd[0], result.stdout, result.stderr))
    return result


def fwsign_path() -> str:
    """Path of the fwsign program, built first when missing or out of date."""
    exe = os.path.join(OUT_DIR, EXE)
    headers = glob.glob(os.path.join(MONOCYPHER, "*.h")) + glob.glob(os.path.join(FW_IMAGE, "include", "*.h"))
    newest = max(os.path.getmtime(p) for p in SOURCES + headers)
    if os.path.exists(exe) and os.path.getmtime(exe) >= newest:
        return exe
    os.makedirs(OUT_DIR, exist_ok=True)
    includes = ["-I" + d for d in INCLUDE_DIRS]
    objects = []
    for src in SOURCES:
        obj = os.path.join(OUT_DIR, os.path.basename(src) + ".o")
        if src.endswith(".c"):
            cmd = ["gcc", "-O2", "-std=c99", "-Wall", "-Wextra", "-Werror"]
        else:
            cmd = ["g++", "-O2", "-std=gnu++17", "-Wall", "-Wextra", "-Werror"]
        _run(cmd + includes + ["-c", src, "-o", obj])
        objects.append(obj)
    _run(["g++", "-O2"] + objects + ["-o", exe])
    return exe


def public_key(seed_path) -> bytes:
    """The 32 byte public key for the seed file (via fwsign)."""
    out = _run([fwsign_path(), "pubkey", str(seed_path)]).stdout.strip()
    return bytes.fromhex(out)


def key_fingerprint(pub: bytes) -> str:
    """Key id as 16 hex characters: the first 8 bytes of SHA-512 of the public key."""
    return hashlib.sha512(pub).digest()[:8].hex()


def sign_slot(seed_path, slot, version, image_path) -> bytes:
    """Slot blob (256 byte signed header + image) for `image_path`, signed with the seed file."""
    os.makedirs(OUT_DIR, exist_ok=True)
    out = os.path.join(OUT_DIR, "sign_%d.blob" % os.getpid())
    try:
        _run([fwsign_path(), "sign", str(seed_path), str(slot), version, str(image_path), out])
        with open(out, "rb") as f:
            return f.read()
    finally:
        if os.path.exists(out):
            os.remove(out)


def firmware_version() -> str:
    """FIRMWARE_VERSION from include/version.h."""
    with open(os.path.join(ROOT, "include", "version.h"), encoding="utf-8") as f:
        m = re.search(r'#define\s+FIRMWARE_VERSION\s+"([^"]+)"', f.read())
    if not m:
        raise RuntimeError("FIRMWARE_VERSION not found in include/version.h")
    return m.group(1)


def header_public_key(header_path) -> bytes:
    """The 32 bytes of FW_PUBLIC_KEY in a key header written by fw_keygen.py."""
    try:
        with open(header_path, encoding="utf-8") as f:
            text = f.read()
    except OSError:
        raise RuntimeError("%s is missing. Run tools/fw_keygen.py" % header_path)
    body = text.split("FW_PUBLIC_KEY[32]", 1)[-1]
    values = re.findall(r"0x([0-9a-fA-F]{2})", body)
    if len(values) != 32:
        raise RuntimeError("%s does not hold a 32 byte FW_PUBLIC_KEY" % header_path)
    return bytes(int(v, 16) for v in values)


def check_key_matches_header(seed_path, header_path) -> None:
    """Raise RuntimeError unless the seed's public key is the one in the header.

    Devices built with the header's key reject firmware signed with any
    other key. The message names both key ids and never the seed.
    """
    mine = key_fingerprint(public_key(seed_path))
    theirs = key_fingerprint(header_public_key(header_path))
    if mine != theirs:
        raise RuntimeError("%s has key id %s but %s has key id %s: devices would reject the "
                           "firmware" % (os.path.basename(str(seed_path)), mine,
                                         os.path.basename(str(header_path)), theirs))


def signing_seed(field: bool) -> str:
    """Seed file for the development key, or the field key (secret/) when `field`.

    The field key must match include/fw_public_key.h. Raises RuntimeError
    ("... Run tools/fw_keygen.py") when it is missing or does not match.
    """
    if not field:
        return DEV_KEY
    if not os.path.exists(FIELD_KEY):
        raise RuntimeError("secret/fw_signing_key is missing. Run tools/fw_keygen.py")
    check_key_matches_header(FIELD_KEY, FIELD_HEADER)
    return FIELD_KEY
