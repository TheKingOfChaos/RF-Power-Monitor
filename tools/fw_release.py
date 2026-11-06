"""Build the release files for a firmware update.

    python tools/fw_release.py [--field] [--test-variant crash|noconfirm|shorttrial] [--test-version X.Y.Z-tag]

Builds the pico (slot A) and pico_b (slot B) environments, signs both slot
images and writes

    release/rf-power-monitor-<version>.upd        both slots, for the web page update
    release/rf-power-monitor-<version>-usb.uf2    boot program + slot A, for BOOTSEL

--field signs with secret/fw_signing_key (create it with tools/fw_keygen.py);
without it the development key is used. A test variant (firmware that
misbehaves on purpose for the update HIL tests, selected by the build flag
-DRFPOWER_FWTEST_<VARIANT>=1) is written to .pio/fwtest/<variant>.upd, uses
the development key only and keeps the normal version number.

.upd layout, little endian: 64 byte header (magic 0x50554652 "RFUP", format 1,
version[16], slotOffset[2], slotLength[2], rest 0xFF), then the slot A blob and
the slot B blob.
"""

import argparse
import os
import re
import shutil
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fwtool  # noqa: E402

UPD_MAGIC = 0x50554652
UPD_FORMAT = 1
UPD_HEADER_SIZE = 64
VARIANTS = ("crash", "noconfirm", "shorttrial")


def build_upd(version: str, blob_a: bytes, blob_b: bytes) -> bytes:
    """The .upd file: header, then the slot A and slot B blobs."""
    v = version.encode("ascii")
    if not 0 < len(v) <= 15:
        raise ValueError("version must be 1 to 15 characters")
    offset_a = UPD_HEADER_SIZE
    offset_b = offset_a + len(blob_a)
    header = struct.pack("<II16s4I", UPD_MAGIC, UPD_FORMAT, v, offset_a, offset_b,
                         len(blob_a), len(blob_b))
    header += b"\xff" * (UPD_HEADER_SIZE - len(header))
    return header + blob_a + blob_b


def parse_upd_header(data: bytes) -> dict:
    """Fields of a .upd header (for tests and tools). Raises ValueError if it is not one."""
    if len(data) < UPD_HEADER_SIZE:
        raise ValueError("too short for a .upd header")
    magic, fmt, v, oa, ob, la, lb = struct.unpack_from("<II16s4I", data)
    if magic != UPD_MAGIC or fmt != UPD_FORMAT:
        raise ValueError("not a .upd file (magic/format)")
    return {"version": v.rstrip(b"\0").decode("ascii"), "slotOffset": [oa, ob], "slotLength": [la, lb]}


def pio_run(env_name: str, extra_env: dict) -> None:
    """Build one PlatformIO environment."""
    pio = os.path.join(os.path.expanduser("~"), ".platformio", "penv", "Scripts", "pio.exe")
    if not os.path.exists(pio):
        pio = shutil.which("pio") or "pio"
    env = dict(os.environ)
    env.update(extra_env)
    subprocess.run([pio, "run", "-e", env_name], cwd=fwtool.ROOT, env=env, check=True)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--field", action="store_true", help="sign with the field key in secret/")
    ap.add_argument("--test-variant", choices=VARIANTS, help="build a misbehaving test image")
    ap.add_argument("--test-version", metavar="VERSION",
                    help="build the firmware as this version (e.g. 3.4.1-hil), development key, "
                         "written to .pio/fwtest/<VERSION>.upd (update site HIL test)")
    args = ap.parse_args(argv)
    if args.field and (args.test_variant or args.test_version is not None):
        ap.error("test builds use the development key only")
    if args.test_variant and args.test_version is not None:
        ap.error("--test-variant and --test-version cannot be combined")
    if args.test_version is not None and not re.fullmatch(r"\d+\.\d+\.\d+(-[0-9A-Za-z.]+)?", args.test_version):
        ap.error("--test-version must look like 3.4.1 or 3.4.1-hil")
    if args.test_version is not None and len(args.test_version) > 15:
        ap.error("--test-version is at most 15 characters (the .upd header holds 16 bytes with its NUL)")

    try:
        seed = fwtool.signing_seed(args.field)
    except RuntimeError as e:
        print(e, file=sys.stderr)
        return 1
    version = args.test_version or fwtool.firmware_version()
    extra = {}
    if args.field:
        extra["RFPOWER_FW_KEY"] = "field"
    if args.test_variant:
        extra["PLATFORMIO_BUILD_FLAGS"] = "-DRFPOWER_FWTEST_%s=1" % args.test_variant.upper()
    if args.test_version:
        extra["PLATFORMIO_BUILD_FLAGS"] = '-DFIRMWARE_VERSION=\\"%s\\"' % args.test_version

    blobs = []
    for slot, env_name in ((0, "pico"), (1, "pico_b")):
        pio_run(env_name, extra)
        image = os.path.join(fwtool.ROOT, ".pio", "build", env_name, "firmware.bin")
        blobs.append(fwtool.sign_slot(seed, slot, version, image))
    upd = build_upd(version, blobs[0], blobs[1])

    if args.test_variant or args.test_version:
        out_dir = os.path.join(fwtool.ROOT, ".pio", "fwtest")
        os.makedirs(out_dir, exist_ok=True)
        upd_path = os.path.join(out_dir, (args.test_variant or args.test_version) + ".upd")
    else:
        out_dir = os.path.join(fwtool.ROOT, "release")
        os.makedirs(out_dir, exist_ok=True)
        upd_path = os.path.join(out_dir, "rf-power-monitor-%s.upd" % version)
        # pico_b built last; the USB file comes from the pico build
        shutil.copyfile(os.path.join(fwtool.ROOT, ".pio", "build", "pico", "firmware_full.uf2"),
                        os.path.join(out_dir, "rf-power-monitor-%s-usb.uf2" % version))
    with open(upd_path, "wb") as f:
        f.write(upd)
    print("Wrote %s (%d bytes)" % (upd_path, len(upd)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
