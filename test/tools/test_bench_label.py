"""Tests for tools/bench_label.py (the 40 x 40 mm device label). No hardware, no printer."""

import os
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import segno  # noqa: E402
from PIL import Image  # noqa: E402

import bench_label  # noqa: E402
from bench_label import LabelData  # noqa: E402

SAMPLE = LabelData(hostname="RF-Power-A1B2C3", mac="2C:CF:67:A1:B2:C3", serial="E6614103E7A1B2C3",
                   firmware="3.5.0", password="k7mq2xr9tp", revision="rev 2", date="2026-11-07")
INK, PAPER = 0, 255


def black_pixels(img, box):
    """Number of ink pixels inside box (x0, y0, x1, y1), end exclusive."""
    region = img.crop(box)
    return sum(1 for p in region.getdata() if p == INK)


def qr_box():
    """(x0, y0, size) of the QR area, quiet zone included, for the sample data."""
    n = len(segno.make(bench_label.qr_text(SAMPLE), error=bench_label.QR_ERROR, boost_error=False).matrix)
    size = (n + 2 * bench_label.QR_BORDER_MODULES) * bench_label.QR_MODULE_DOTS
    x0 = bench_label.SIZE_DOTS - bench_label.MARGIN_DOTS - size
    return x0, x0, size


class LabelTests(unittest.TestCase):
    def test_qr_text_format(self):
        self.assertEqual("RFPM;H=RF-Power-A1B2C3;M=2CCF67A1B2C3;S=E6614103E7A1B2C3;FW=3.5.0",
                         bench_label.qr_text(SAMPLE))

    def test_qr_text_mac_without_colons_upper_case(self):
        d = LabelData(**{**SAMPLE.__dict__, "mac": "2c:cf:67:a1:b2:c3"})
        self.assertIn(";M=2CCF67A1B2C3;", bench_label.qr_text(d))

    def test_password_not_in_qr_text(self):
        self.assertNotIn(SAMPLE.password, bench_label.qr_text(SAMPLE))

    def test_ink_image_is_472_square_bilevel(self):
        img = bench_label.render_ink(SAMPLE)
        self.assertEqual((472, 472), img.size)
        self.assertEqual("1", img.mode)

    def test_ink_stays_inside_margins(self):
        img = bench_label.render_ink(SAMPLE).convert("L")
        m, s = bench_label.MARGIN_DOTS, bench_label.SIZE_DOTS
        self.assertGreater(black_pixels(img, (m, m, s - m, s - m)), 1000)  # something is printed
        self.assertEqual(0, black_pixels(img, (0, 0, s, m)))
        self.assertEqual(0, black_pixels(img, (0, s - m, s, s)))
        self.assertEqual(0, black_pixels(img, (0, 0, m, s)))
        self.assertEqual(0, black_pixels(img, (s - m, 0, s, s)))

    def test_long_hostname_still_inside_margins(self):
        d = LabelData(**{**SAMPLE.__dict__, "hostname": "Rack-17-Building-C-RF-Power-Mon1"})  # 32 characters
        img = bench_label.render_ink(d).convert("L")
        m, s = bench_label.MARGIN_DOTS, bench_label.SIZE_DOTS
        self.assertEqual(0, black_pixels(img, (s - m, 0, s, s)))

    def test_qr_quiet_zone_has_no_ink(self):
        img = bench_label.render_ink(SAMPLE).convert("L")
        x0, y0, size = qr_box()
        q = bench_label.QR_BORDER_MODULES * bench_label.QR_MODULE_DOTS
        self.assertEqual(0, black_pixels(img, (x0, y0, x0 + size, y0 + q)))         # top band
        self.assertEqual(0, black_pixels(img, (x0, y0, x0 + q, y0 + size)))         # left band

    def test_qr_modules_match_segno_matrix(self):
        img = bench_label.render_ink(SAMPLE).convert("L")
        matrix = segno.make(bench_label.qr_text(SAMPLE), error=bench_label.QR_ERROR, boost_error=False).matrix
        x0, y0, _ = qr_box()
        dots, border = bench_label.QR_MODULE_DOTS, bench_label.QR_BORDER_MODULES
        for r, row in enumerate(matrix):
            for c, dark in enumerate(row):
                px = img.getpixel((x0 + (border + c) * dots + dots // 2, y0 + (border + r) * dots + dots // 2))
                self.assertEqual(INK if dark else PAPER, px, "module %d,%d" % (r, c))

    def test_qr_is_about_12_mm(self):
        _, _, size = qr_box()
        self.assertGreaterEqual(size, 118)  # 10 mm
        self.assertLessEqual(size, 165)     # 14 mm

    def test_preview_is_inverted_ink(self):
        ink = bench_label.render_ink(SAMPLE).convert("L")
        preview = bench_label.render_preview(SAMPLE)
        self.assertEqual(PAPER, ink.getpixel((0, 0)))
        self.assertEqual(0, preview.getpixel((0, 0)))  # black label
        x0, y0, _ = qr_box()
        self.assertEqual(255 - ink.getpixel((x0 + 20, y0 + 20)), preview.getpixel((x0 + 20, y0 + 20)))

    def test_save_label_writes_both_files_at_300_dpi(self):
        with tempfile.TemporaryDirectory() as d:
            ink_path, preview_path = bench_label.save_label(SAMPLE, d)
            self.assertEqual(os.path.join(d, "RF-Power-A1B2C3.png"), ink_path)
            self.assertEqual(os.path.join(d, "RF-Power-A1B2C3-preview.png"), preview_path)
            with Image.open(ink_path) as img:
                self.assertEqual((472, 472), img.size)
                self.assertAlmostEqual(300, img.info["dpi"][0], delta=1)
            self.assertTrue(os.path.exists(preview_path))


if __name__ == "__main__":
    unittest.main(verbosity=2)
