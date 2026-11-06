"""Tests for tools/bench_provision.py (the bench command) with fake hardware steps."""

import contextlib
import datetime
import io
import os
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench_device  # noqa: E402
import bench_log  # noqa: E402
import bench_provision  # noqa: E402
from test_bench_device import write_release  # noqa: E402

NOW = datetime.datetime(2026, 11, 7, 10, 30, 0)


class FakeBoard:
    """The device's serial menu: benchinfo answers follow pwlock."""

    def __init__(self, **values):
        self.values = {"hostname": "RF-Power-A1B2C3", "mac": "2C:CF:67:A1:B2:C3", "serial": "E6614103E7A1B2C3",
                       "firmware": "3.5.0",
                       "fwkey": bench_device.field_key_id(), "password": "k7mq2xr9tp", "locked": "0", "lost": "0"}
        self.values.update(values)
        self.sent = []
        self.closed = False

    def wait_for_menu(self, timeout=40.0):
        pass

    def command(self, line, until, timeout=5.0):
        self.sent.append(line)
        if line == "pwlock":
            self.values["locked"] = "1"
            return "Default web password locked (it is on the device label)\r\n"
        if line == "benchinfo":
            lines = "".join("%s=%s\r\n" % (k, self.values[k]) for k in bench_device.BENCH_KEYS)
            return "BENCHINFO BEGIN\r\n" + lines + "BENCHINFO END\r\n> "
        raise AssertionError("unexpected command " + line)

    def close(self):
        self.closed = True


class FakeIO:
    """Hardware steps: a BOOTSEL drive, Pico ports that change after the copy, one board."""

    def __init__(self, board, ports_before=(), ports_after=("COM9",)):
        self.board = board
        self.copied = None
        self.ports_before = list(ports_before)
        self.ports_after = list(ports_after)
        self.drive_asked = False
        self.opened = None

    def find_bootsel_drive(self, timeout):
        self.drive_asked = True
        return "E:\\"

    def copy(self, src, drive_root):
        self.copied = (src, drive_root)

    def pico_ports(self):
        return self.ports_after if self.copied else self.ports_before

    def open_serial(self, port):
        self.opened = port
        return self.board

    def now(self):
        return NOW

    def sleep(self, seconds):
        pass


class ProvisionTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.out = os.path.join(self.tmp.name, "bench")
        self.log = os.path.join(self.out, "devices.csv")

    def run_main(self, argv, fake):
        err, out = io.StringIO(), io.StringIO()
        with contextlib.redirect_stderr(err), contextlib.redirect_stdout(out):
            code = bench_provision.main(argv + ["--out", self.out], io=fake)
        return code, out.getvalue(), err.getvalue()

    def release(self, key_id=None, version="3.5.0"):
        return write_release(self.tmp.name, name="rf-power-monitor-%s-usb.uf2" % version, version=version,
                             key_id=key_id or bench_device.field_key_id())

    def test_flash_run_writes_label_and_log_row(self):
        board = FakeBoard()
        fake = FakeIO(board)
        code, out, err = self.run_main(["--uf2", self.release()], fake)
        self.assertEqual(0, code, err)
        self.assertEqual("COM9", fake.opened)
        self.assertIn("pwlock", board.sent)
        self.assertTrue(board.closed)
        self.assertTrue(os.path.exists(os.path.join(self.out, "RF-Power-A1B2C3.png")))
        self.assertTrue(os.path.exists(os.path.join(self.out, "RF-Power-A1B2C3-preview.png")))
        row = bench_log.latest(self.log, "RF-Power-A1B2C3")
        self.assertEqual("k7mq2xr9tp", row["password"])
        self.assertEqual("rev 2", row["revision"])
        self.assertEqual("3.5.0", row["firmware"])
        self.assertEqual("2026-11-07T10:30:00", row["time"])
        self.assertIn("NIIMBOT", out)

    def test_flash_run_refuses_dev_signed_uf2_before_asking_for_bootsel(self):
        fake = FakeIO(FakeBoard())
        code, _, err = self.run_main(["--uf2", self.release(key_id=bench_device.dev_key_id())], fake)
        self.assertEqual(1, code)
        self.assertIn("development key", err)
        self.assertFalse(fake.drive_asked)

    def test_flash_run_accepts_dev_signed_uf2_with_dev_flag(self):
        board = FakeBoard(fwkey=bench_device.dev_key_id())
        code, _, err = self.run_main(["--uf2", self.release(key_id=bench_device.dev_key_id()), "--dev"],
                                     FakeIO(board))
        self.assertEqual(0, code, err)

    def test_flash_run_refuses_firmware_uf2(self):
        path = write_release(self.tmp.name, name="firmware.uf2", key_id=bench_device.field_key_id())
        fake = FakeIO(FakeBoard())
        code, _, err = self.run_main(["--uf2", path], fake)
        self.assertEqual(1, code)
        self.assertFalse(fake.drive_asked)

    def test_flash_run_refuses_version_mismatch(self):
        code, _, err = self.run_main(["--uf2", self.release()], FakeIO(FakeBoard(firmware="3.4.0")))
        self.assertEqual(1, code)
        self.assertIn("3.4.0", err)
        self.assertIsNone(bench_log.latest(self.log, "RF-Power-A1B2C3"))

    def test_flash_run_refuses_device_trusting_dev_key(self):
        code, _, err = self.run_main(["--uf2", self.release()], FakeIO(FakeBoard(fwkey=bench_device.dev_key_id())))
        self.assertEqual(1, code)
        self.assertIn("development key", err)

    def test_flash_run_refuses_hostname_not_matching_mac(self):
        board = FakeBoard(hostname="Rack-4")
        code, _, err = self.run_main(["--uf2", self.release()], FakeIO(board))
        self.assertEqual(1, code)
        self.assertIn("RF-Power-A1B2C3", err)
        self.assertNotIn("pwlock", board.sent)

    def test_flash_run_refuses_invalid_password(self):
        code, _, err = self.run_main(["--uf2", self.release()], FakeIO(FakeBoard(password="")))
        self.assertEqual(1, code)
        self.assertIn("password", err)

    def test_flash_run_fails_when_no_new_port_appears(self):
        fake = FakeIO(FakeBoard(), ports_before=["COM4"], ports_after=["COM4"])
        code, _, err = self.run_main(["--uf2", self.release()], fake)
        self.assertEqual(1, code)
        self.assertIn("serial port", err)

    def test_no_flash_warns_on_custom_hostname_and_continues(self):
        board = FakeBoard(hostname="Rack-4-UPS-Mon")
        fake = FakeIO(board, ports_before=["COM4"])
        code, out, err = self.run_main(["--no-flash"], fake)
        self.assertEqual(0, code, err)
        self.assertIn("Rack-4-UPS-Mon", out + err)
        self.assertIsNone(fake.copied)
        self.assertEqual("COM4", fake.opened)
        self.assertTrue(os.path.exists(os.path.join(self.out, "Rack-4-UPS-Mon.png")))

    def test_no_flash_refuses_two_ports_without_port_option(self):
        fake = FakeIO(FakeBoard(), ports_before=["COM4", "COM5"])
        code, _, err = self.run_main(["--no-flash"], fake)
        self.assertEqual(1, code)
        self.assertIn("--port", err)
        self.assertIsNone(fake.opened)

    def test_no_flash_uses_port_option(self):
        fake = FakeIO(FakeBoard(), ports_before=["COM4", "COM5"])
        code, _, err = self.run_main(["--no-flash", "--port", "COM5"], fake)
        self.assertEqual(0, code, err)
        self.assertEqual("COM5", fake.opened)

    def test_no_flash_refuses_when_no_board_connected(self):
        code, _, err = self.run_main(["--no-flash"], FakeIO(FakeBoard(), ports_before=[]))
        self.assertEqual(1, code)

    def test_no_flash_new_row_wins_for_reprint(self):
        self.run_main(["--no-flash"], FakeIO(FakeBoard(password="abcdefghij"), ports_before=["COM4"]))
        self.run_main(["--no-flash"], FakeIO(FakeBoard(password="k7mq2xr9tp"), ports_before=["COM4"]))
        self.assertEqual("k7mq2xr9tp", bench_log.latest(self.log, "2CCF67A1B2C3")["password"])

    def test_reprint_uses_newest_row_without_device(self):
        self.run_main(["--no-flash"], FakeIO(FakeBoard(), ports_before=["COM4"]))
        os.remove(os.path.join(self.out, "RF-Power-A1B2C3.png"))
        fake = FakeIO(FakeBoard())
        code, _, err = self.run_main(["--reprint", "rf-power-a1b2c3"], fake)
        self.assertEqual(0, code, err)
        self.assertIsNone(fake.opened)
        self.assertTrue(os.path.exists(os.path.join(self.out, "RF-Power-A1B2C3.png")))

    def test_reprint_unknown_device_exits_1(self):
        code, _, err = self.run_main(["--reprint", "RF-Power-FFFFFF"], FakeIO(FakeBoard()))
        self.assertEqual(1, code)
        self.assertIn("RF-Power-FFFFFF", err)

    def test_lost_password_is_reported_and_labelled(self):
        board = FakeBoard(lost="1", locked="1")
        code, out, err = self.run_main(["--no-flash"], FakeIO(board, ports_before=["COM4"]))
        self.assertEqual(0, code, err)
        self.assertIn("lost", (out + err).lower())
        self.assertTrue(os.path.exists(os.path.join(self.out, "RF-Power-A1B2C3.png")))

    def test_default_uf2_is_highest_version(self):
        for v in ("3.9.0", "3.10.0", "3.2.1"):
            write_release(self.tmp.name, name="rf-power-monitor-%s-usb.uf2" % v, version=v)
        self.assertTrue(bench_provision.newest_release(self.tmp.name).endswith("rf-power-monitor-3.10.0-usb.uf2"))

    def test_default_uf2_none_when_folder_empty(self):
        self.assertIsNone(bench_provision.newest_release(self.tmp.name))


if __name__ == "__main__":
    unittest.main(verbosity=2)
