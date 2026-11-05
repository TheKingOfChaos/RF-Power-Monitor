"""Tests for the firmware signing tools (tools/fw_keygen.py, fwsign, fw_release .upd layout).

Pure Python plus the host compiler (fwsign is built on first use). No
hardware. Field key paths are only exercised through --out into a temp dir.
Run: python -m unittest discover -s test/tools -v
"""

import os
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import fw_release  # noqa: E402
import fwtool  # noqa: E402

KEYGEN = os.path.join(ROOT, "tools", "fw_keygen.py")


def keygen(out_dir, *extra, stdin=""):
    """Run fw_keygen.py --out out_dir; returns the CompletedProcess."""
    return subprocess.run([sys.executable, KEYGEN, "--out", out_dir, *extra],
                          input=stdin, capture_output=True, text=True)


def verify(pub: bytes, slot: int, blob_path: str):
    """fwsign verify; returns the CompletedProcess."""
    return subprocess.run([fwtool.fwsign_path(), "verify", pub.hex(), str(slot), blob_path],
                          capture_output=True, text=True)


class KeygenTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = self._tmp.name
        self.key = os.path.join(self.dir, "fw_signing_key")
        self.addCleanup(self._tmp.cleanup)

    def read_key(self):
        with open(self.key, "rb") as f:
            return f.read()

    def test_keygen_writes_32_byte_key_and_public_header(self):
        r = keygen(self.dir)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(len(self.read_key()), 32)
        with open(os.path.join(self.dir, "fw_public_key.h"), encoding="utf-8") as f:
            header = f.read()
        pub = fwtool.public_key(self.key)
        for b in pub:
            self.assertIn("0x%02x" % b, header)
        self.assertIn("FW_PUBLIC_KEY[32]", header)

    def test_keygen_refuses_to_overwrite_existing_key(self):
        keygen(self.dir)
        before = self.read_key()
        r = keygen(self.dir)
        self.assertNotEqual(r.returncode, 0)
        self.assertEqual(self.read_key(), before)

    def test_keygen_force_needs_typed_confirmation(self):
        keygen(self.dir)
        before = self.read_key()
        r = keygen(self.dir, "--force", stdin="no\n")
        self.assertNotEqual(r.returncode, 0)
        self.assertEqual(self.read_key(), before)
        r = keygen(self.dir, "--force", stdin="REPLACE\n")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertNotEqual(self.read_key(), before)

    def test_keygen_never_prints_private_key(self):
        r = keygen(self.dir)
        key = self.read_key()
        for text in (r.stdout, r.stderr):
            self.assertNotIn(key.hex(), text.lower())

    def test_keygen_prints_fingerprint(self):
        r = keygen(self.dir)
        fp = fwtool.key_fingerprint(fwtool.public_key(self.key))
        self.assertEqual(len(fp), 16)
        self.assertIn(fp, r.stdout)

    def test_keygen_dev_and_out_cannot_be_combined(self):
        r = keygen(self.dir, "--dev")
        self.assertNotEqual(r.returncode, 0)
        self.assertFalse(os.path.exists(self.key))


class SignVerifyTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.dir = self._tmp.name
        self.key_a = os.path.join(self.dir, "a", "fw_signing_key")
        self.key_b = os.path.join(self.dir, "b", "fw_signing_key")
        for d in ("a", "b"):
            self.assertEqual(keygen(os.path.join(self.dir, d)).returncode, 0)
        self.image = os.path.join(self.dir, "image.bin")
        with open(self.image, "wb") as f:
            f.write(bytes(range(256)) * 20)
        self.blob = os.path.join(self.dir, "slot.blob")

    def sign_to_file(self, key, slot):
        data = fwtool.sign_slot(key, slot, "1.2.3", self.image)
        with open(self.blob, "wb") as f:
            f.write(data)
        return data

    def test_sign_then_verify_ok(self):
        data = self.sign_to_file(self.key_a, 0)
        self.assertEqual(len(data), 256 + 256 * 20)
        r = verify(fwtool.public_key(self.key_a), 0, self.blob)
        self.assertEqual((r.returncode, r.stdout), (0, ""))

    def test_verify_fails_with_other_key(self):
        self.sign_to_file(self.key_a, 0)
        r = verify(fwtool.public_key(self.key_b), 0, self.blob)
        self.assertEqual(r.returncode, 1)
        self.assertEqual(r.stdout.strip(), "wrong key")

    def test_verify_fails_for_other_slot(self):
        self.sign_to_file(self.key_a, 0)
        r = verify(fwtool.public_key(self.key_a), 1, self.blob)
        self.assertEqual(r.returncode, 1)
        self.assertEqual(r.stdout.strip(), "wrong slot")

    def test_verify_fails_when_image_byte_changed(self):
        data = bytearray(self.sign_to_file(self.key_a, 1))
        data[300] ^= 0x01
        with open(self.blob, "wb") as f:
            f.write(data)
        r = verify(fwtool.public_key(self.key_a), 1, self.blob)
        self.assertEqual(r.returncode, 1)
        self.assertEqual(r.stdout.strip(), "bad signature")

    def test_sign_rejects_overlong_version(self):
        with self.assertRaises(RuntimeError):
            fwtool.sign_slot(self.key_a, 0, "1.2.3.4.5.6.7.8.9", self.image)

    def test_public_key_matches_signed_key_id(self):
        data = self.sign_to_file(self.key_a, 0)
        key_id = data[96:104]
        self.assertEqual(key_id.hex(), fwtool.key_fingerprint(fwtool.public_key(self.key_a)))


class UpdLayoutTests(unittest.TestCase):
    def test_upd_layout_offsets_and_lengths(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(keygen(d).returncode, 0)
            key = os.path.join(d, "fw_signing_key")
            images = []
            for n, size in ((0, 1000), (1, 1500)):
                path = os.path.join(d, "img%d.bin" % n)
                with open(path, "wb") as f:
                    f.write(bytes([n + 1]) * size)
                images.append((path, size))
            blobs = [fwtool.sign_slot(key, n, "3.2.0", p) for n, (p, _) in enumerate(images)]
        upd = fw_release.build_upd("3.2.0", blobs[0], blobs[1])
        h = fw_release.parse_upd_header(upd)
        self.assertEqual(h["version"], "3.2.0")
        self.assertEqual(h["slotLength"], [256 + 1000, 256 + 1500])
        for n in (0, 1):
            off, length = h["slotOffset"][n], h["slotLength"][n]
            self.assertEqual(upd[off:off + length], blobs[n])
        self.assertEqual(h["slotOffset"][0], 64)
        self.assertEqual(len(upd), 64 + sum(h["slotLength"]))
        self.assertEqual(upd[:4], b"RFUP")
        self.assertEqual(upd[56:64], b"\xff" * 8)

    def test_parse_upd_header_rejects_other_files(self):
        with self.assertRaises(ValueError):
            fw_release.parse_upd_header(b"\x00" * 64)
        with self.assertRaises(ValueError):
            fw_release.parse_upd_header(b"RFUP")


if __name__ == "__main__":
    unittest.main()
