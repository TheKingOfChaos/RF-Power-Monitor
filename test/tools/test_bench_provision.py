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
from bench_fakes import FakeBoard  # noqa: E402
from test_bench_device import write_release  # noqa: E402

NOW = datetime.datetime(2026, 11, 7, 10, 30, 0)
UPDATE_URL = "http://10.132.0.45:8074"
UPDATE_KEY = "e5" * 32


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
        self.settings = os.path.join(self.tmp.name, "bench.ini")  # absent unless a test writes it
        # The dummy releases here are not really signed: test_bench_device checks signatures
        self.real_verify_signature = bench_device.verify_signature
        bench_device.verify_signature = lambda fw: None
        self.addCleanup(setattr, bench_device, "verify_signature", self.real_verify_signature)

    def run_main(self, argv, fake):
        err, out = io.StringIO(), io.StringIO()
        with contextlib.redirect_stderr(err), contextlib.redirect_stdout(out):
            code = bench_provision.main(argv + ["--out", self.out, "--settings", self.settings], io=fake)
        return code, out.getvalue(), err.getvalue()

    def write_settings(self, url=UPDATE_URL, key=UPDATE_KEY):
        with open(self.settings, "w", encoding="utf-8") as f:
            f.write("[update]\nurl = %s\nkey = %s\n" % (url, key))

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

    def test_copy_error_as_the_board_restarts_is_not_fatal(self):
        # Windows may fail closing the file once the board restarts with the whole UF2;
        # the new serial port is the proof that the flash worked
        def failing_copy(src, dest):
            raise OSError(5, "The device is not ready")
        original = bench_provision.shutil.copyfile
        bench_provision.shutil.copyfile = failing_copy
        try:
            bench_provision.RealIO().copy(self.release(), self.tmp.name)  # the "drive" still exists
        finally:
            bench_provision.shutil.copyfile = original

    def test_unexpected_os_error_is_a_refusal_not_a_traceback(self):
        class FullDisk(FakeIO):
            def copy(self, src, drive_root):
                raise OSError(28, "No space left on device")
        code, _, err = self.run_main(["--uf2", self.release()], FullDisk(FakeBoard()))
        self.assertEqual(1, code)
        self.assertIn("No space left", err)

    def test_no_flash_on_firmware_before_3_5_says_so(self):
        class OldBoard(FakeBoard):
            def command(self, line, until, timeout=5.0):
                import re
                menu = "\r\n=== RF-Power-Monitor Menu ===\r\n1. Show status\r\n?. Show this menu\r\n> "
                if not re.search(until, menu):
                    raise bench_device.BenchError("no answer from the device")
                return menu
        code, _, err = self.run_main(["--no-flash"], FakeIO(OldBoard(), ports_before=["COM4"]))
        self.assertEqual(1, code)
        self.assertIn("3.5.0", err)

    def test_settings_file_sets_update_server_default(self):
        self.write_settings()
        board = FakeBoard()
        code, out, err = self.run_main(["--uf2", self.release()], FakeIO(board))
        self.assertEqual(0, code, err)
        self.assertEqual((UPDATE_URL, UPDATE_KEY), (board.update_url, board.update_key))
        self.assertEqual(UPDATE_URL, board.values["updatedefault"])
        self.assertEqual(UPDATE_URL, bench_log.latest(self.log, "RF-Power-A1B2C3")["update"])
        self.assertIn(UPDATE_URL, out)
        self.assertNotIn(UPDATE_KEY, out + err)
        with open(self.log, encoding="utf-8") as f:
            self.assertNotIn(UPDATE_KEY, f.read())

    def test_missing_settings_file_is_noted_and_the_run_continues(self):
        board = FakeBoard()
        code, out, err = self.run_main(["--uf2", self.release()], FakeIO(board))
        self.assertEqual(0, code, err)
        self.assertIn("bench.ini", out)
        self.assertNotIn("10", board.sent)

    def test_settings_file_with_bad_key_refused_before_bootsel(self):
        self.write_settings(key="0011")
        fake = FakeIO(FakeBoard())
        code, _, err = self.run_main(["--uf2", self.release()], fake)
        self.assertEqual(1, code)
        self.assertIn("key", err)
        self.assertFalse(fake.drive_asked)

    def test_settings_file_without_update_section_refused(self):
        with open(self.settings, "w", encoding="utf-8") as f:
            f.write("[other]\nx = 1\n")
        code, _, err = self.run_main(["--uf2", self.release()], FakeIO(FakeBoard()))
        self.assertEqual(1, code)
        self.assertIn("[update]", err)

    def test_unwritable_log_refused_before_bootsel(self):
        os.makedirs(self.log)  # a folder where devices.csv should be: cannot be opened for writing
        fake = FakeIO(FakeBoard())
        code, _, err = self.run_main(["--uf2", self.release()], fake)
        self.assertEqual(1, code)
        self.assertIn("devices.csv", err)
        self.assertFalse(fake.drive_asked)

    def test_two_new_ports_after_the_copy_refused(self):
        fake = FakeIO(FakeBoard(), ports_after=["COM9", "COM10"])
        code, _, err = self.run_main(["--uf2", self.release()], fake)
        self.assertEqual(1, code)
        self.assertIn("COM10", err)
        self.assertIsNone(fake.opened)

    def test_hostname_unsafe_as_file_name_refused(self):
        board = FakeBoard(hostname="..\\..\\evil")
        code, _, err = self.run_main(["--no-flash"], FakeIO(board, ports_before=["COM4"]))
        self.assertEqual(1, code)
        self.assertIn("hostname", err)
        self.assertNotIn("pwlock", board.sent)

    def test_bad_signature_refused_before_bootsel(self):
        bench_device.verify_signature = self.real_verify_signature
        fake = FakeIO(FakeBoard())
        code, _, err = self.run_main(["--uf2", self.release()], fake)  # field key id, dummy signature
        self.assertEqual(1, code)
        self.assertIn("signature", err)
        self.assertFalse(fake.drive_asked)

    def test_default_uf2_is_highest_version(self):
        for v in ("3.9.0", "3.10.0", "3.2.1"):
            write_release(self.tmp.name, name="rf-power-monitor-%s-usb.uf2" % v, version=v)
        self.assertTrue(bench_provision.newest_release(self.tmp.name).endswith("rf-power-monitor-3.10.0-usb.uf2"))

    def test_default_uf2_none_when_folder_empty(self):
        self.assertIsNone(bench_provision.newest_release(self.tmp.name))


if __name__ == "__main__":
    unittest.main(verbosity=2)
