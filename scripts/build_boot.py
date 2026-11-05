"""Build the boot program (boot/) into .pio/boot/boot.bin.

boot.bin is what goes at flash offset 0: the board's boot2 (256 bytes, from
the Arduino-Pico core) followed by our boot program, whose vector table is at
0x10000100 and whose code must end before 0x1000E000 (the boot record). It is
built with the core's own arm-none-eabi toolchain, so it needs no internet.

scripts/slot_build.py calls build() during `pio run -e pico`; it can also be
run by hand:

    python scripts/build_boot.py [--boot2 boot2_w25q080_2_padded_checksum.S] [--force]

Sources: boot/*.S and boot/*.cpp, plus the files in EXTRA_SOURCES (pure
libraries the boot program uses, added by later tasks). It rebuilds only when
a source, boot/boot.ld or this script is newer than boot.bin.
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
# Which boot2 and toolchain boot.bin was built with (a change rebuilds it)
OUT_STAMP = os.path.join(OUT_DIR, "boot.stamp")
LINKER_SCRIPT = os.path.join(BOOT_DIR, "boot.ld")

PIO_PACKAGES = os.path.join(os.path.expanduser("~"), ".platformio", "packages")
DEFAULT_TOOLCHAIN = os.path.join(PIO_PACKAGES, "toolchain-rp2040-earlephilhower")
DEFAULT_FRAMEWORK = os.path.join(PIO_PACKAGES, "framework-arduinopico")
# The core's default when the board JSON names none
DEFAULT_BOOT2 = "boot2_generic_03h_2_padded_checksum.S"

FLASH_BASE = 0x10000000
BOOT_LIMIT = 0xE000  # boot record starts here

# More sources compiled into the boot program (paths relative to the project),
# with their include directories, for later tasks (e.g. lib/ verifiers)
EXTRA_SOURCES = []
EXTRA_INCLUDES = []

CFLAGS = [
    "-mcpu=cortex-m0plus", "-mthumb", "-Os", "-ffreestanding",
    "-fno-exceptions", "-fno-rtti", "-nostdlib", "-fno-threadsafe-statics",
    "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
    "-ffunction-sections", "-fdata-sections",
    "-Wall", "-Wextra", "-Werror", "-std=gnu++17",
]
ASFLAGS = ["-mcpu=cortex-m0plus", "-mthumb"]


def sources(boot2_path):
    """Every input of the build: boot2, boot/ sources and EXTRA_SOURCES."""
    files = [boot2_path]
    files += sorted(glob.glob(os.path.join(BOOT_DIR, "*.S")))
    files += sorted(glob.glob(os.path.join(BOOT_DIR, "*.cpp")))
    files += [os.path.join(ROOT, p) for p in EXTRA_SOURCES]
    return files


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
          boot2=DEFAULT_BOOT2, force=False):
    """Build .pio/boot/boot.bin if needed and return its path.

    toolchain_dir: the toolchain package (with bin/arm-none-eabi-g++).
    framework_dir: the Arduino-Pico core (for boot2/rp2040/<boot2>).
    Raises RuntimeError when compiling or linking fails or the code passes
    0xE000.
    """
    bindir = os.path.join(toolchain_dir, "bin")
    gxx = os.path.join(bindir, "arm-none-eabi-g++")
    objcopy = os.path.join(bindir, "arm-none-eabi-objcopy")
    boot2_path = os.path.join(framework_dir, "boot2", "rp2040", boot2)
    if not os.path.isfile(boot2_path):
        raise RuntimeError("boot2 source not found: %s" % boot2_path)

    inputs = sources(boot2_path) + [LINKER_SCRIPT, os.path.abspath(__file__)]
    stamp = "%s\n%s\n" % (boot2_path, gxx)
    if not force and up_to_date(inputs, stamp):
        return OUT_BIN

    os.makedirs(OUT_DIR, exist_ok=True)
    includes = ["-I" + os.path.join(ROOT, d) for d in EXTRA_INCLUDES]
    objects = []
    for src in sources(boot2_path):
        obj = os.path.join(OUT_DIR, os.path.basename(src) + ".o")
        if src.endswith(".S"):
            run([gxx] + ASFLAGS + ["-c", src, "-o", obj])
        else:
            run([gxx] + CFLAGS + includes + ["-c", src, "-o", obj])
        objects.append(obj)

    elf = os.path.join(OUT_DIR, "boot.elf")
    run([gxx] + CFLAGS + ["-T", LINKER_SCRIPT, "-Wl,--gc-sections",
                          "-Wl,--no-warn-rwx-segments",
                          "-Wl,-Map=" + os.path.join(OUT_DIR, "boot.map"),
                          "-o", elf] + objects + ["-lgcc"])
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
    print("Boot program: %s, %d bytes (limit 0x%X)" % (OUT_BIN, size, BOOT_LIMIT))
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
