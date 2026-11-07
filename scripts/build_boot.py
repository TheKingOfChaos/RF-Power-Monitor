"""Build the boot program (boot/) into .pio/boot/boot.bin.

boot.bin is what goes at flash offset 0: the board's boot2 (256 bytes, from
the Arduino-Pico core) followed by our boot program, whose vector table is at
0x10000100 and whose code must end before 0x1000E000 (the boot record). It is
built with the core's own arm-none-eabi toolchain, so it needs no internet.

scripts/slot_build.py calls build() during `pio run -e pico`; it can also be
run by hand:

    python scripts/build_boot.py [--boot2 boot2_w25q080_2_padded_checksum.S] [--force]

Sources: boot/*.S and boot/*.cpp, plus EXTRA_SOURCES: the pure libraries that
verify and pick a slot (lib/fw_image, lib/fw_update) and Monocypher (C, built
with gcc -std=c99). The trusted key comes from include/fw_key.h, the header the
firmware uses too: with RFPOWER_FW_KEY=field (the switch slot_build.py signs
with) the boot program is built with -DRFPOWER_FW_FIELD_KEY and trusts the
field key. slot_build.py then checks that boot.bin holds the public key it
signs slot A with. Register layouts come from the Pico SDK headers in the core
(no SDK code is linked).

It rebuilds when a source, a header under boot/ or the include directories,
boot/boot.ld or this script is newer than boot.bin, or when the key, boot2,
core or toolchain changed.
"""

import argparse
import glob
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BOOT_DIR = os.path.join(ROOT, "boot")
OUT_DIR = os.path.join(ROOT, ".pio", "boot")
OUT_BIN = os.path.join(OUT_DIR, "boot.bin")
# Which boot2, core, toolchain and key boot.bin was built with (a change rebuilds it)
OUT_STAMP = os.path.join(OUT_DIR, "boot.stamp")
LINKER_SCRIPT = os.path.join(BOOT_DIR, "boot.ld")

PIO_PACKAGES = os.path.join(os.path.expanduser("~"), ".platformio", "packages")
DEFAULT_TOOLCHAIN = os.path.join(PIO_PACKAGES, "toolchain-rp2040-earlephilhower")
DEFAULT_FRAMEWORK = os.path.join(PIO_PACKAGES, "framework-arduinopico")
# The core's default when the board JSON names none
DEFAULT_BOOT2 = "boot2_generic_03h_2_padded_checksum.S"

FLASH_BASE = 0x10000000
BOOT_LIMIT = 0xE000  # boot record starts here

# More sources compiled into the boot program (paths relative to the project)
EXTRA_SOURCES = [
    "lib/fw_image/src/fw_image.cpp",
    "lib/fw_image/src/fw_sha512.cpp",
    "lib/fw_update/src/boot_record.cpp",
    "lib/fw_update/src/boot_decision.cpp",
    "lib/monocypher/src/monocypher.c",
    "lib/monocypher/src/monocypher-ed25519.c",
]
# Their include directories (relative to the project); every header below them
# is a rebuild input
EXTRA_INCLUDES = [
    "boot/sdk",  # pico/config_autogen.h for the SDK headers, instead of the core's
    "include",  # fw_key.h, fw_dev_public_key.h / fw_public_key.h, pins.h
    "lib/fw_image/include",
    "lib/fw_update/include",
    "lib/monocypher/src",
]
# Pico SDK headers for the register layouts (relative to the core)
SDK_INCLUDES = [
    "include/rp2040/pico_base",
    "pico-sdk/src/rp2040/hardware_structs/include",
    "pico-sdk/src/rp2040/hardware_regs/include",
    "pico-sdk/src/rp2_common/hardware_base/include",
    "pico-sdk/src/common/pico_base_headers/include",
    "pico-sdk/src/rp2040/pico_platform/include",
    "pico-sdk/src/rp2_common/pico_platform_common/include",
    "pico-sdk/src/rp2_common/pico_platform_compiler/include",
    "pico-sdk/src/rp2_common/pico_platform_sections/include",
    "pico-sdk/src/rp2_common/pico_platform_panic/include",
]
DEFINES = ["-DPICO_RP2040=1", "-DRFPOWER_BOOT_PROGRAM"]
FIELD_KEY_DEFINE = "-DRFPOWER_FW_FIELD_KEY"  # include/fw_key.h then trusts the field key

COMMON_FLAGS = [
    "-mcpu=cortex-m0plus", "-mthumb", "-Os", "-ffreestanding", "-nostdlib",
    "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
    "-ffunction-sections", "-fdata-sections", "-fstack-usage",
    "-Wall", "-Wextra", "-Werror",
]
CFLAGS = COMMON_FLAGS + ["-fno-exceptions", "-fno-rtti", "-fno-threadsafe-statics", "-std=gnu++17"]
C_FLAGS = COMMON_FLAGS + ["-std=c99"]  # Monocypher (lib/monocypher/library.json)
ASFLAGS = ["-mcpu=cortex-m0plus", "-mthumb"]


def field_key_from_env():
    """True when RFPOWER_FW_KEY=field, the switch scripts/slot_build.py signs with."""
    return os.environ.get("RFPOWER_FW_KEY", "") == "field"


def project_sources():
    """The project's compiled sources: boot/*.S, boot/*.cpp and EXTRA_SOURCES."""
    files = sorted(glob.glob(os.path.join(BOOT_DIR, "*.S")))
    files += sorted(glob.glob(os.path.join(BOOT_DIR, "*.cpp")))
    files += [os.path.join(ROOT, p) for p in EXTRA_SOURCES]
    return files


def sources(boot2_path):
    """Every compiled source: boot2, then project_sources()."""
    return [boot2_path] + project_sources()


def headers():
    """Every project header the build can include: under boot/ and EXTRA_INCLUDES.

    Includes the key headers: include/fw_key.h, fw_dev_public_key.h and
    fw_public_key.h when it exists.
    """
    found = set()
    for d in [BOOT_DIR] + [os.path.join(ROOT, p) for p in EXTRA_INCLUDES]:
        for dirpath, _dirs, files in os.walk(d):
            found.update(os.path.join(dirpath, f) for f in files if f.endswith(".h"))
    return sorted(found)


def project_inputs():
    """Every project file boot.bin is built from (all but boot2 and the toolchain)."""
    return project_sources() + headers() + [LINKER_SCRIPT, os.path.abspath(__file__)]


def compile_flags(field_key, framework_dir):
    """Defines and include flags for every C and C++ source."""
    flags = DEFINES + ([FIELD_KEY_DEFINE] if field_key else [])
    flags += ["-I" + os.path.join(ROOT, d) for d in EXTRA_INCLUDES]
    flags += ["-I" + os.path.join(framework_dir, d) for d in SDK_INCLUDES]
    return flags


def stamp_text(boot2_path, gxx, framework_dir, field_key):
    """What boot.bin was built with, besides its sources (a change rebuilds it)."""
    return "%s\n%s\n%s\n%s\n" % (boot2_path, gxx, framework_dir,
                                 "field key" if field_key else "development key")


def up_to_date(inputs, stamp):
    """True when boot.bin exists, was built with `stamp` and is newer than every input."""
    if not os.path.isfile(OUT_BIN) or not os.path.isfile(OUT_STAMP):
        return False
    with open(OUT_STAMP, encoding="utf-8") as f:
        if f.read() != stamp:
            return False
    built = os.path.getmtime(OUT_BIN)
    return all(os.path.getmtime(p) <= built for p in inputs)


def run(cmd):
    """Run a toolchain command; raise RuntimeError with its output on failure."""
    result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if result.returncode != 0:
        raise RuntimeError("%s failed:\n%s" % (os.path.basename(cmd[0]), result.stdout))
    return result.stdout


def build(toolchain_dir=DEFAULT_TOOLCHAIN, framework_dir=DEFAULT_FRAMEWORK,
          boot2=DEFAULT_BOOT2, force=False, field_key=None):
    """Build .pio/boot/boot.bin if needed and return its path.

    toolchain_dir: the toolchain package (with bin/arm-none-eabi-g++).
    framework_dir: the Arduino-Pico core (boot2/rp2040/<boot2>, SDK headers).
    field_key: trust the field key instead of the development key; None reads
    RFPOWER_FW_KEY.
    Raises RuntimeError when compiling or linking fails or the code passes
    0xE000.
    """
    if field_key is None:
        field_key = field_key_from_env()
    bindir = os.path.join(toolchain_dir, "bin")
    gxx = os.path.join(bindir, "arm-none-eabi-g++")
    gcc = os.path.join(bindir, "arm-none-eabi-gcc")
    objcopy = os.path.join(bindir, "arm-none-eabi-objcopy")
    boot2_path = os.path.join(framework_dir, "boot2", "rp2040", boot2)
    if not os.path.isfile(boot2_path):
        raise RuntimeError("boot2 source not found: %s" % boot2_path)

    inputs = [boot2_path] + project_inputs()
    stamp = stamp_text(boot2_path, gxx, framework_dir, field_key)
    if not force and up_to_date(inputs, stamp):
        return OUT_BIN

    os.makedirs(OUT_DIR, exist_ok=True)
    flags = compile_flags(field_key, framework_dir)
    objects = []
    for src in sources(boot2_path):
        obj = os.path.join(OUT_DIR, os.path.basename(src) + ".o")
        if src.endswith(".S"):
            run([gxx] + ASFLAGS + ["-c", src, "-o", obj])
        elif src.endswith(".c"):
            run([gcc] + C_FLAGS + flags + ["-c", src, "-o", obj])
        else:
            run([gxx] + CFLAGS + flags + ["-c", src, "-o", obj])
        objects.append(obj)

    elf = os.path.join(OUT_DIR, "boot.elf")
    # libc only for memcpy/memset/memcmp (nothing in the boot program calls into the system)
    run([gxx] + CFLAGS + ["-T", LINKER_SCRIPT, "-Wl,--gc-sections",
                          "-Wl,--no-warn-rwx-segments",
                          "-Wl,-Map=" + os.path.join(OUT_DIR, "boot.map"),
                          "-o", elf] + objects + ["-lc", "-lgcc"])
    tmp = OUT_BIN + ".tmp"
    run([objcopy, "-O", "binary", elf, tmp])

    size = os.path.getsize(tmp)
    if size > BOOT_LIMIT:
        os.remove(tmp)
        raise RuntimeError("boot program is %d bytes, more than 0x%X" % (size, BOOT_LIMIT))
    with open(tmp, "rb") as f:
        image = f.read()
    if len(image) < 0x108 or image[0x100:0x104] == b"\xff\xff\xff\xff":
        os.remove(tmp)
        raise RuntimeError("boot program has no vector table at 0x100")
    os.replace(tmp, OUT_BIN)
    with open(OUT_STAMP, "w", encoding="utf-8") as f:
        f.write(stamp)
    print("Boot program: %s, %d bytes (limit 0x%X), %s"
          % (OUT_BIN, size, BOOT_LIMIT, "field key" if field_key else "development key"))
    return OUT_BIN


def main():
    """Command line: build (or rebuild with --force) .pio/boot/boot.bin."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--toolchain", default=DEFAULT_TOOLCHAIN)
    parser.add_argument("--framework", default=DEFAULT_FRAMEWORK)
    parser.add_argument("--boot2", default=DEFAULT_BOOT2)
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()
    try:
        build(args.toolchain, args.framework, args.boot2, args.force)
    except RuntimeError as e:
        sys.exit(str(e))


if __name__ == "__main__":
    main()
