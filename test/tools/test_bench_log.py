"""Tests for tools/bench_log.py (the CSV of boards set up at the bench). No hardware."""

import os
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import bench_log  # noqa: E402


def row(**kw):
    base = dict(time="2026-11-07T10:00:00", hostname="RF-Power-A1B2C3", mac="2C:CF:67:A1:B2:C3",
                serial="E6614103E7A1B2C3", firmware="3.5.0", revision="rev 2", password="k7mq2xr9tp",
                label="bench/RF-Power-A1B2C3.png")
    base.update(kw)
    return base


class LogTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = os.path.join(self.tmp.name, "devices.csv")

    def test_append_creates_header_once(self):
        bench_log.append(self.path, row())
        bench_log.append(self.path, row(password="abcdefghij"))
        with open(self.path, encoding="utf-8") as f:
            lines = f.read().splitlines()
        self.assertEqual(",".join(bench_log.FIELDS), lines[0])
        self.assertEqual(3, len(lines))

    def test_append_creates_the_folder(self):
        path = os.path.join(self.tmp.name, "bench", "devices.csv")
        bench_log.append(path, row())
        self.assertTrue(os.path.exists(path))

    def test_latest_newest_row_wins_for_same_mac(self):
        bench_log.append(self.path, row(password="abcdefghij"))
        bench_log.append(self.path, row(password="k7mq2xr9tp"))
        self.assertEqual("k7mq2xr9tp", bench_log.latest(self.path, "2C:CF:67:A1:B2:C3")["password"])

    def test_latest_matches_mac_without_colons_any_case(self):
        bench_log.append(self.path, row())
        self.assertIsNotNone(bench_log.latest(self.path, "2ccf67a1b2c3"))
        self.assertIsNotNone(bench_log.latest(self.path, "2c-cf-67-a1-b2-c3"))

    def test_latest_matches_hostname_case_insensitive(self):
        bench_log.append(self.path, row())
        self.assertIsNotNone(bench_log.latest(self.path, "rf-power-a1b2c3"))

    def test_latest_matches_serial(self):
        bench_log.append(self.path, row())
        self.assertIsNotNone(bench_log.latest(self.path, "e6614103e7a1b2c3"))

    def test_latest_picks_the_right_device(self):
        bench_log.append(self.path, row())
        bench_log.append(self.path, row(hostname="RF-Power-000001", mac="2C:CF:67:00:00:01",
                                        serial="0000000000000001", password="zzzzzzzzzz"))
        self.assertEqual("k7mq2xr9tp", bench_log.latest(self.path, "RF-Power-A1B2C3")["password"])

    def test_latest_none_when_unknown(self):
        bench_log.append(self.path, row())
        self.assertIsNone(bench_log.latest(self.path, "RF-Power-FFFFFF"))

    def test_latest_none_when_file_missing(self):
        self.assertIsNone(bench_log.latest(self.path, "RF-Power-A1B2C3"))

    def test_latest_ignores_partial_hostname(self):
        bench_log.append(self.path, row())
        self.assertIsNone(bench_log.latest(self.path, "RF-Power-A1"))

    def test_latest_ignores_empty_key(self):
        bench_log.append(self.path, row(serial=""))
        self.assertIsNone(bench_log.latest(self.path, ""))


if __name__ == "__main__":
    unittest.main(verbosity=2)
