"""Set up a new board at the bench: flash the first firmware, read the device,
lock its default web password, make its 40 x 40 mm label and log it.

    python tools/bench_provision.py [--uf2 FILE] [--dev] [--revision "rev 2"] [--out bench]
    python tools/bench_provision.py --no-flash [--port COMx] [--dev] [--revision ...] [--out bench]
    python tools/bench_provision.py --reprint <hostname|MAC|serial> [--out bench]

Flash (default): the newest release/rf-power-monitor-<version>-usb.uf2 (or
--uf2), which must be signed with the field key (--dev allows the development
key, for testing this tool). Hold BOOTSEL while plugging in the board; the
script copies the file to the RPI-RP2 drive and waits for the board's serial
port. --no-flash does the same on a board that already runs the firmware.
--reprint makes the label again from bench/devices.csv without a device.

Output in --out (default bench/, gitignored): <hostname>.png to print from
the NIIMBOT app (40 x 40 mm label, white ribbon), <hostname>-preview.png, and
devices.csv, which holds the default web passwords.
"""

import argparse
import datetime
import glob
import os
import re
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench_device  # noqa: E402
import bench_label  # noqa: E402
import bench_log  # noqa: E402
from bench_device import BenchError  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RELEASE_DIR = os.path.join(ROOT, "release")
DEFAULT_OUT = os.path.join(ROOT, "bench")
DEFAULT_REVISION = "rev 2"
BOOTSEL_TIMEOUT_S = 120
PORT_TIMEOUT_S = 60
POLL_S = 0.5
PASSWORD_RE = re.compile(r"^[a-km-np-z2-9]{10}$")  # lib/web_ui default_password.h alphabet


class RealIO:
    """The hardware steps on this PC (tests replace them)."""

    def find_bootsel_drive(self, timeout):
        """Root of the RPI-RP2 drive (INFO_UF2.TXT names it) within timeout seconds."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            for letter in "DEFGHIJKLMNOPQRSTUVWXYZ":
                info = "%s:\\INFO_UF2.TXT" % letter
                try:
                    with open(info, encoding="ascii", errors="replace") as f:
                        if "RPI-RP2" in f.read():
                            return "%s:\\" % letter
                except OSError:
                    continue
            time.sleep(POLL_S)
        raise BenchError("no RPI-RP2 drive within %d s: hold BOOTSEL while plugging in the board" % timeout)

    def copy(self, src, drive_root):
        """Copy the UF2 onto the drive; the board restarts when it has the whole file.

        Windows may report an error while the board restarts at the end of the copy
        (the drive goes away): the board's new serial port is the proof that the
        flash worked, so an error here is only noted.
        """
        dest = os.path.join(drive_root, os.path.basename(src))
        try:
            shutil.copyfile(src, dest)
        except OSError as e:
            print("Note: the copy ended with \"%s\" (normal when the board restarts); waiting for the board" % e)

    def pico_ports(self):
        """Serial ports of connected RP2040 boards."""
        return bench_device.pico_ports()

    def open_serial(self, port):
        """The board's menu on port, opened."""
        ser = bench_device.BenchSerial(port)
        ser.open()
        return ser

    def now(self):
        """Local time."""
        return datetime.datetime.now()

    def sleep(self, seconds):
        """Wait."""
        time.sleep(seconds)


def _version_key(path):
    m = re.search(r"rf-power-monitor-(\d+)\.(\d+)\.(\d+)-usb\.uf2$", os.path.basename(path))
    return tuple(int(x) for x in m.groups()) if m else None


def newest_release(folder):
    """The release -usb.uf2 in folder with the highest version number, or None."""
    files = [p for p in glob.glob(os.path.join(folder, "rf-power-monitor-*-usb.uf2")) if _version_key(p)]
    return max(files, key=_version_key) if files else None


def expected_hostname(mac):
    """The automatic hostname for a MAC: RF-Power-<last 6 hex digits>."""
    return "RF-Power-" + mac.replace(":", "").upper()[-6:]


def _wait_new_port(io, before):
    """The Pico port that appears after the copy."""
    for _ in range(int(PORT_TIMEOUT_S / POLL_S)):
        new = sorted(set(io.pico_ports()) - set(before))
        if new:
            return new[0]
        io.sleep(POLL_S)
    raise BenchError("the board's serial port did not appear within %d s after the copy" % PORT_TIMEOUT_S)


def _single_port(io, port):
    """--port, or the one connected board's port."""
    if port:
        return port
    ports = io.pico_ports()
    if not ports:
        raise BenchError("no board found on USB: connect it, or give --port")
    if len(ports) > 1:
        raise BenchError("several boards connected (%s): use --port" % ", ".join(ports))
    return ports[0]


def _check(info, fw, dev, flashed):
    """Refuse device data that must not go on a label; warn about the rest."""
    bench_device.check_key(info["fwkey"], dev, "the device firmware")
    if fw is not None and info["firmware"] != fw.version:
        raise BenchError("the device runs %s, but %s was flashed" % (info["firmware"], fw.version))
    if not PASSWORD_RE.match(info["password"]):
        raise BenchError("the device reports no valid default web password")
    expected = expected_hostname(info["mac"])
    if info["hostname"] != expected:
        if flashed:
            raise BenchError("hostname %s does not match the MAC (expected %s)" % (info["hostname"], expected))
        print("Note: hostname %s was set by hand (automatic name: %s); it goes on the label" %
              (info["hostname"], expected))
    if info["lost"] == "1":
        print("Warning: the device lost its locked default password and made a new one; "
              "the new label has the new password, replace the old label")


def _label_and_log(info, revision, out, io):
    """Save the label and append the log row; return the label paths."""
    now = io.now()
    data = bench_label.LabelData(hostname=info["hostname"], mac=info["mac"].upper(), serial=info["serial"],
                                 firmware=info["firmware"], password=info["password"], revision=revision,
                                 date=now.date().isoformat())
    ink, preview = bench_label.save_label(data, out)
    bench_log.append(os.path.join(out, "devices.csv"),
                     dict(time=now.isoformat(timespec="seconds"), hostname=data.hostname, mac=data.mac,
                          serial=data.serial, firmware=data.firmware, revision=revision, password=data.password,
                          label=ink))
    return ink, preview


def _device_run(args, io):
    """Flash (unless --no-flash), read, check, lock, label, log."""
    fw = None
    if not args.no_flash:
        path = args.uf2 or newest_release(RELEASE_DIR)
        if path is None:
            raise BenchError("no release -usb.uf2 in release/: run tools/fw_release.py --field")
        fw = bench_device.read_release_uf2(path)
        bench_device.check_signed_for(fw, args.dev)
        print("Firmware %s (%s)" % (fw.version, os.path.basename(fw.path)))
        print("Hold BOOTSEL and plug in the board...")
        drive = io.find_bootsel_drive(BOOTSEL_TIMEOUT_S)
        before = io.pico_ports()
        print("Copying to %s" % drive)
        io.copy(fw.path, drive)
        port = _wait_new_port(io, before)
    else:
        port = _single_port(io, args.port)
    print("Reading the device on %s" % port)
    ser = io.open_serial(port)
    try:
        ser.wait_for_menu()
        info = bench_device.read_info(ser)
        _check(info, fw, args.dev, flashed=fw is not None)
        bench_device.lock(ser)
    finally:
        ser.close()
    ink, preview = _label_and_log(info, args.revision, args.out, io)
    print("%s  MAC %s  firmware %s  password locked" % (info["hostname"], info["mac"], info["firmware"]))
    print("Label:   %s" % ink)
    print("Preview: %s" % preview)
    print("Print the label from the NIIMBOT app (40 x 40 mm, white ribbon on black labels)")


def _reprint(args):
    """Make the label again from the newest log row for the device."""
    row = bench_log.latest(os.path.join(args.out, "devices.csv"), args.reprint)
    if row is None:
        raise BenchError("%s is not in %s" % (args.reprint, os.path.join(args.out, "devices.csv")))
    data = bench_label.LabelData(hostname=row["hostname"], mac=row["mac"], serial=row["serial"],
                                 firmware=row["firmware"], password=row["password"],
                                 revision=row["revision"] or DEFAULT_REVISION, date=row["time"][:10])
    ink, preview = bench_label.save_label(data, args.out)
    print("Label:   %s" % ink)
    print("Preview: %s" % preview)


def main(argv=None, *, io=None) -> int:
    """Command line entry; returns the exit code (0 done, 1 refused or failed)."""
    p = argparse.ArgumentParser(description="Set up a new board at the bench and make its label.")
    mode = p.add_mutually_exclusive_group()
    mode.add_argument("--no-flash", action="store_true", help="board already runs the firmware")
    mode.add_argument("--reprint", metavar="DEVICE", help="label again from the log (hostname, MAC or serial)")
    p.add_argument("--uf2", help="release -usb.uf2 to flash (default: newest in release/)")
    p.add_argument("--dev", action="store_true", help="accept the development key (testing this tool)")
    p.add_argument("--port", help="serial port with --no-flash, when several boards are connected")
    p.add_argument("--revision", default=DEFAULT_REVISION, help='board revision on the label (default "rev 2")')
    p.add_argument("--out", default=DEFAULT_OUT, help="folder for labels and devices.csv (default bench/)")
    args = p.parse_args(argv)
    try:
        if args.reprint:
            _reprint(args)
        else:
            _device_run(args, io or RealIO())
    except BenchError as e:
        print("Refused: %s" % e, file=sys.stderr)
        return 1
    except OSError as e:  # a drive, port or file problem: a message, not a traceback
        print("Failed: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
