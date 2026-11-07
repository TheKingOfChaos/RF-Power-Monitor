"""Tests for tools/bench_device.py (benchinfo parsing, release UF2 check, lock). No hardware."""

import os
import struct
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench_device  # noqa: E402
import fwtool  # noqa: E402
import uf2  # noqa: E402
from bench_device import BenchError  # noqa: E402
from bench_fakes import FakeBoard  # noqa: E402

FLASH = 0x10000000
SLOT_A = FLASH + 0x10000

BLOCK = ("BENCHINFO BEGIN\r\n"
         "hostname=RF-Power-A1B2C3\r\n"
         "mac=2C:CF:67:A1:B2:C3\r\n"
         "serial=E6614103E7A1B2C3\r\n"
         "firmware=3.5.0\r\n"
         "fwkey=0123456789abcdef\r\n"
         "password=k7mq2xr9tp\r\n"
         "locked=1\r\n"
         "lost=0\r\n"
         "updatedefault=http://10.132.0.45:8074\r\n"
         "updatedefaultkey=1\r\n"
         "BENCHINFO END\r\n")


def slot_header(version="3.5.0", key_id="0123456789abcdef", magic=0x57464652, image=b"\x00" * 512):
    """A slot A blob: 256 byte header with a dummy signature + image."""
    head = b"\x11" * 64 + struct.pack("<IIII", magic, 1, 0, len(image))
    head += version.encode().ljust(16, b"\0") + bytes.fromhex(key_id)
    return head + b"\xff" * (256 - len(head)) + image


def write_release(folder, name="rf-power-monitor-3.5.0-usb.uf2", boot=True, blob=None, **header):
    """A release UF2 in folder: boot program (unless boot=False) and slot A (blob, or a dummy header)."""
    chunks = [(SLOT_A, blob if blob is not None else slot_header(**header))]
    if boot:
        chunks.insert(0, (FLASH, b"\x22" * 1024))
    path = os.path.join(folder, name)
    uf2.write_uf2(path, chunks)
    return path


def dev_signed_blob(folder, version="3.5.0"):
    """A slot A blob really signed with the development key (keys/dev), over a small image."""
    image = os.path.join(folder, "image.bin")
    with open(image, "wb") as f:
        f.write(bytes(range(256)) * 4)
    return fwtool.sign_slot(fwtool.DEV_KEY, 0, version, image)


class ParseTests(unittest.TestCase):
    def test_parse_reads_all_keys(self):
        info = bench_device.parse_bench_info(BLOCK)
        self.assertEqual("RF-Power-A1B2C3", info["hostname"])
        self.assertEqual("2C:CF:67:A1:B2:C3", info["mac"])
        self.assertEqual("k7mq2xr9tp", info["password"])
        self.assertEqual("0", info["lost"])

    def test_parse_takes_last_complete_block(self):
        old = BLOCK.replace("k7mq2xr9tp", "abcdefghij")
        text = "Network up after 6344 ms\r\n" + old + "> \r\nState: connected\r\n" + BLOCK + "> "
        self.assertEqual("k7mq2xr9tp", bench_device.parse_bench_info(text)["password"])

    def test_parse_ignores_block_without_end_after_a_complete_one(self):
        text = BLOCK + BLOCK.replace("k7mq2xr9tp", "abcdefghij").split("lost=")[0]
        self.assertEqual("k7mq2xr9tp", bench_device.parse_bench_info(text)["password"])

    def test_parse_rejects_block_without_end(self):
        with self.assertRaises(BenchError):
            bench_device.parse_bench_info(BLOCK.replace("BENCHINFO END\r\n", ""))

    def test_parse_rejects_empty_text(self):
        with self.assertRaises(BenchError):
            bench_device.parse_bench_info("")

    def test_parse_rejects_missing_key(self):
        with self.assertRaises(BenchError):
            bench_device.parse_bench_info(BLOCK.replace("password=k7mq2xr9tp\r\n", ""))

    def test_parse_keeps_equals_sign_in_value(self):
        info = bench_device.parse_bench_info(BLOCK.replace("hostname=RF-Power-A1B2C3", "hostname=a=b"))
        self.assertEqual("a=b", info["hostname"])


class Uf2Tests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def test_read_release_uf2_reads_version_and_key_id(self):
        fw = bench_device.read_release_uf2(write_release(self.tmp.name, version="3.5.0", key_id="00112233aabbccdd"))
        self.assertEqual("3.5.0", fw.version)
        self.assertEqual("00112233aabbccdd", fw.key_id)

    def test_read_release_uf2_refuses_firmware_uf2_name(self):
        with self.assertRaisesRegex(BenchError, "firmware.uf2"):
            bench_device.read_release_uf2(write_release(self.tmp.name, name="firmware.uf2"))

    def test_read_release_uf2_refuses_file_without_boot_program(self):
        with self.assertRaisesRegex(BenchError, "boot program"):
            bench_device.read_release_uf2(write_release(self.tmp.name, boot=False))

    def test_read_release_uf2_refuses_bad_slot_magic(self):
        with self.assertRaises(BenchError):
            bench_device.read_release_uf2(write_release(self.tmp.name, magic=0x12345678))

    def test_read_release_uf2_refuses_file_that_is_not_uf2(self):
        path = os.path.join(self.tmp.name, "rf-power-monitor-3.5.0-usb.uf2")
        with open(path, "wb") as f:
            f.write(b"not a uf2 file" * 100)
        with self.assertRaises(BenchError):
            bench_device.read_release_uf2(path)

    def test_read_release_uf2_refuses_missing_file(self):
        with self.assertRaises(BenchError):
            bench_device.read_release_uf2(os.path.join(self.tmp.name, "rf-power-monitor-9.9.9-usb.uf2"))


class KeyTests(unittest.TestCase):
    def test_key_ids_are_16_hex_and_differ(self):
        self.assertRegex(bench_device.field_key_id(), r"^[0-9a-f]{16}$")
        self.assertRegex(bench_device.dev_key_id(), r"^[0-9a-f]{16}$")
        self.assertNotEqual(bench_device.field_key_id(), bench_device.dev_key_id())

    def test_check_key_refuses_dev_key_without_flag(self):
        with self.assertRaisesRegex(BenchError, "development key"):
            bench_device.check_key(bench_device.dev_key_id(), dev=False, what="x")

    def test_check_key_accepts_dev_key_with_flag(self):
        bench_device.check_key(bench_device.dev_key_id(), dev=True, what="x")

    def test_check_key_accepts_field_key(self):
        bench_device.check_key(bench_device.field_key_id(), dev=False, what="x")

    def test_check_key_refuses_unknown_key(self):
        with self.assertRaises(BenchError):
            bench_device.check_key("ffffffffffffffff", dev=True, what="x")


class SignatureTests(unittest.TestCase):
    """check_signed_for runs the boot program's own check (fwsign verify) on slot A."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.blob = dev_signed_blob(self.tmp.name)

    def test_check_signed_accepts_dev_signed_release_with_flag(self):
        fw = bench_device.read_release_uf2(write_release(self.tmp.name, blob=self.blob))
        bench_device.check_signed_for(fw, dev=True)

    def test_check_signed_refuses_dev_signed_release_without_flag(self):
        fw = bench_device.read_release_uf2(write_release(self.tmp.name, blob=self.blob))
        with self.assertRaisesRegex(BenchError, "development key"):
            bench_device.check_signed_for(fw, dev=False)

    def test_check_signed_refuses_tampered_image(self):
        blob = bytearray(self.blob)
        blob[300] ^= 0x01  # one bit of the image, after the 256 byte header
        fw = bench_device.read_release_uf2(write_release(self.tmp.name, blob=bytes(blob)))
        with self.assertRaisesRegex(BenchError, "signature"):
            bench_device.check_signed_for(fw, dev=True)

    def test_check_signed_refuses_field_key_id_with_wrong_signature(self):
        # A header that claims the field key, but signed by nobody
        fw = bench_device.read_release_uf2(write_release(self.tmp.name, key_id=bench_device.field_key_id()))
        with self.assertRaisesRegex(BenchError, "signature"):
            bench_device.check_signed_for(fw, dev=False)

    def test_read_release_uf2_refuses_truncated_slot(self):
        path = write_release(self.tmp.name, blob=self.blob[:400])  # header says more image than there is
        with self.assertRaisesRegex(BenchError, "incomplete"):
            bench_device.read_release_uf2(path)


class LockTests(unittest.TestCase):
    def test_lock_sends_pwlock_and_checks_it(self):
        board = FakeBoard()
        bench_device.lock(board)
        self.assertIn("pwlock", board.sent)

    def test_lock_raises_when_device_still_unlocked(self):
        with self.assertRaisesRegex(BenchError, "lock"):
            bench_device.lock(FakeBoard(lock_works=False))

    def test_read_info_parses_the_answer(self):
        self.assertEqual("RF-Power-A1B2C3", bench_device.read_info(FakeBoard())["hostname"])


class UpdateDefaultTests(unittest.TestCase):
    KEY = "d4" * 32

    def test_set_update_default_sets_option_10_and_keeps_it(self):
        board = FakeBoard()
        bench_device.set_update_default(board, "http://10.132.0.45:8074", self.KEY)
        self.assertEqual(("http://10.132.0.45:8074", self.KEY), (board.update_url, board.update_key))
        self.assertEqual("http://10.132.0.45:8074", board.values["updatedefault"])
        self.assertEqual("1", board.values["updatedefaultkey"])
        self.assertLess(board.sent.index("10"), board.sent.index("updatedefault"))

    def test_set_update_default_raises_when_device_does_not_keep_it(self):
        class Forgetful(FakeBoard):
            def command(self, line, until, timeout=5.0):
                out = super().command(line, until, timeout)
                if line == "updatedefault":
                    self.values["updatedefault"] = "none"
                return out
        with self.assertRaisesRegex(BenchError, "update server"):
            bench_device.set_update_default(Forgetful(), "http://10.132.0.45:8074", self.KEY)

    def test_set_update_default_refuses_bad_values_before_sending(self):
        for url, key in (("https://x", self.KEY), ("http://x", "00"), ("http://x", "g" * 64)):
            board = FakeBoard()
            with self.assertRaises(BenchError):
                bench_device.set_update_default(board, url, key)
            self.assertEqual([], board.sent)

    def test_errors_never_contain_the_key(self):
        try:
            bench_device.set_update_default(FakeBoard(), "https://x", self.KEY)
        except BenchError as e:
            self.assertNotIn(self.KEY, str(e))


if __name__ == "__main__":
    unittest.main(verbosity=2)
