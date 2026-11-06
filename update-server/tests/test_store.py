"""Tests for the site's storage and release rules (rfupdate.store, rfupdate.upd).
Run: python -m unittest discover -s update-server/tests -v
"""

import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
from helpers import fake_blob, fake_upd  # noqa: E402
from rfupdate import upd  # noqa: E402
from rfupdate.store import Store  # noqa: E402


class UpdTest(unittest.TestCase):
    def test_header_version(self):
        self.assertEqual("3.4.1", upd.parse_header(fake_upd("3.4.1")))

    def test_header_rejects_other_files(self):
        good = fake_upd("3.4.1")
        for bad in (b"", good[:63], b"\0" * 64 + good[64:], good[:4] + b"\2\0\0\0" + good[8:], good[:-1]):
            with self.assertRaises(ValueError):
                upd.parse_header(bad)

    def test_header_rejects_unterminated_or_empty_version(self):
        good = bytearray(fake_upd("3.4.1"))
        good[8:24] = b"x" * 16
        with self.assertRaises(ValueError):
            upd.parse_header(bytes(good))
        good[8:24] = b"\0" * 16
        with self.assertRaises(ValueError):
            upd.parse_header(bytes(good))

    def test_header_rejects_slot_version_mismatch(self):
        # The outer header is not signed: it must name the release the slots hold
        with self.assertRaises(ValueError) as cm:
            upd.parse_header(fake_upd("3.4.2", slot_version="3.4.1"))
        self.assertIn("3.4.1", str(cm.exception))
        with self.assertRaises(ValueError):
            upd.parse_header(fake_upd("3.4.2", blob_b=fake_blob("3.4.1", 400)))

    def test_header_rejects_blob_without_slot_header(self):
        with self.assertRaises(ValueError):
            upd.parse_header(fake_upd("3.4.1", blob_a=b"A" * 300))

    def test_compare_versions(self):
        self.assertGreater(upd.compare_versions("3.10.0", "3.9.0"), 0)
        self.assertLess(upd.compare_versions("3.4.0-hil", "3.4.0"), 0)
        self.assertGreater(upd.compare_versions("3.4.1-hil", "3.4.0"), 0)
        self.assertEqual(0, upd.compare_versions("3.4.0", "3.4.0"))


class StoreTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.store = Store(self.tmp.name)

    def tearDown(self):
        self.store.close()
        self.tmp.cleanup()

    def test_upload_stores_release_file(self):
        data = fake_upd("3.4.1")
        self.assertEqual("3.4.1", self.store.add_release(data, now=1000.0))
        with open(self.store.release_path("3.4.1"), "rb") as f:
            self.assertEqual(data, f.read())
        self.assertEqual(["3.4.1"], [r["version"] for r in self.store.releases()])

    def test_upload_older_than_min_version_is_refused(self):
        with self.assertRaises(ValueError):
            self.store.add_release(fake_upd("3.3.0"), now=1000.0)
        with self.assertRaises(ValueError):
            self.store.add_release(fake_upd("3.4.0-rc1"), now=1000.0)
        self.assertEqual("3.4.0", self.store.add_release(fake_upd("3.4.0"), now=1000.0))

    def test_duplicate_version_is_refused(self):
        self.store.add_release(fake_upd("3.4.1"), now=1000.0)
        with self.assertRaises(ValueError):
            self.store.add_release(fake_upd("3.4.1", blob_a=fake_blob("3.4.1", 500)), now=1001.0)

    def test_upload_non_upd_is_refused(self):
        with self.assertRaises(ValueError):
            self.store.add_release(b"<html>not firmware</html>" * 10, now=1000.0)

    def test_upload_too_large_is_refused(self):
        with self.assertRaises(ValueError):
            self.store.add_release(fake_upd("3.4.1", blob_a=b"A" * (upd.MAX_UPLOAD)), now=1000.0)

    def test_release_with_assignment_cannot_be_deleted(self):
        self.store.add_release(fake_upd("3.4.1"), now=1000.0)
        self.store.upsert_checkin({"serial": "S1", "hostname": "h", "ipv4": "", "ipv6": [], "mac": "", "version": "3.4.0",
                                   "slot": "A", "trial": False, "update": "idle", "fail": "", "last": "none"}, 1000.0)
        self.store.assign(["S1"], "3.4.1", now=1000.0)
        with self.assertRaises(ValueError):
            self.store.delete_release("3.4.1")
        self.assertTrue(os.path.exists(self.store.release_path("3.4.1")))

    def test_release_without_assignment_is_deleted(self):
        self.store.add_release(fake_upd("3.4.1"), now=1000.0)
        path = self.store.release_path("3.4.1")
        self.store.delete_release("3.4.1")
        self.assertEqual([], self.store.releases())
        self.assertFalse(os.path.exists(path))

    def test_release_path_refuses_unknown_and_odd_versions(self):
        self.assertIsNone(self.store.release_path("3.4.9"))
        self.assertIsNone(self.store.release_path("../rfupdate.db"))

    def test_site_key_is_none_until_generated_and_survives_reopen(self):
        self.assertIsNone(self.store.site_key())
        key = self.store.new_site_key()
        self.assertEqual(32, len(key))
        self.store.close()
        self.store = Store(self.tmp.name)
        self.assertEqual(key, self.store.site_key())
        self.assertNotEqual(key, self.store.new_site_key())


if __name__ == "__main__":
    unittest.main()
