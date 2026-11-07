"""Tests for the linker script generation in scripts/slot_build.py. No hardware.

slot_build.py is a PlatformIO (SCons) script; its pure functions are
imported here without SCons. Uses a copy of the Arduino-Pico core's
memmap_default.ld (skipped when the core is not installed).
"""

import importlib.util
import os
import re
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CORE_LD = os.path.join(os.path.expanduser("~"), ".platformio", "packages", "framework-arduinopico",
                       "lib", "rp2040", "memmap_default.ld")

spec = importlib.util.spec_from_file_location("slot_build", os.path.join(ROOT, "scripts", "slot_build.py"))
slot_build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(slot_build)

SUBS = {
    "__EEPROM_START__": "270528512",
    "__FS_START__": "270024704",
    "__FS_END__": "270524416",
    "__RAM_LENGTH__": "256k",
    "__PSRAM_LENGTH__": "0",
}


@unittest.skipUnless(os.path.exists(CORE_LD), "Arduino-Pico core not installed")
class LinkerScriptTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(CORE_LD, encoding="utf-8") as f:
            cls.core = f.read()

    def test_slot_a_flash_origin_and_length(self):
        text = slot_build.slot_linker_script(self.core, 0, SUBS)
        self.assertIn("FLASH(rx) : ORIGIN = 0x10010100, LENGTH = 0xD6F00", text)

    def test_slot_b_flash_origin_and_length(self):
        text = slot_build.slot_linker_script(self.core, 1, SUBS)
        self.assertIn("FLASH(rx) : ORIGIN = 0x100E7100, LENGTH = 0xD6F00", text)

    def test_boot2_ota_and_partition_are_removed(self):
        text = slot_build.slot_linker_script(self.core, 0, SUBS)
        self.assertNotRegex(text, r"\.boot2 : \{")
        self.assertNotRegex(text, r"\.ota : \{")
        self.assertNotRegex(text, r"\.partition : \{")
        self.assertNotIn("__boot2_end__", text)
        self.assertEqual(text.count("/DISCARD/"), 1)

    def test_no_placeholder_is_left(self):
        text = slot_build.slot_linker_script(self.core, 1, SUBS)
        self.assertIsNone(re.search(r"__[A-Z_]+_(LENGTH|START|END)__", text))

    def test_each_edit_must_match_once(self):
        # A core script where an edit matches twice or not at all is refused
        doubled = self.core + "\n" + self.core
        with self.assertRaises(ValueError):
            slot_build.slot_linker_script(doubled, 0, SUBS)
        missing = self.core.replace("ORIGIN = 0x10000000, LENGTH = __FLASH_LENGTH__", "ORIGIN = 0, LENGTH = 1")
        with self.assertRaises(ValueError):
            slot_build.slot_linker_script(missing, 0, SUBS)

    def test_discard_insertion_must_match_once(self):
        two_sections = self.core.replace("SECTIONS\n{\n", "SECTIONS\n{\n", 1) + "\nSECTIONS\n{\n"
        with self.assertRaises(ValueError):
            slot_build.slot_linker_script(two_sections, 0, SUBS)
        none = self.core.replace("SECTIONS\n{\n", "SECTIONS {\n")
        with self.assertRaises(ValueError):
            slot_build.slot_linker_script(none, 0, SUBS)

    def test_unknown_placeholder_left_over_is_refused(self):
        with self.assertRaises(ValueError):
            slot_build.slot_linker_script(self.core, 0, {k: v for k, v in SUBS.items() if k != "__FS_END__"})


class BootKeyCheckTests(unittest.TestCase):
    KEY = bytes(range(32))

    def test_boot_holding_the_key_passes(self):
        slot_build.check_boot_trusts_key(bytes([0xFF]) * 300 + self.KEY + bytes(40), self.KEY)

    def test_boot_without_the_key_is_refused(self):
        other = bytes(range(1, 33))
        with self.assertRaises(ValueError) as cm:
            slot_build.check_boot_trusts_key(bytes([0xFF]) * 300 + other, self.KEY)
        self.assertIn(slot_build.fwtool.key_fingerprint(self.KEY), str(cm.exception))

    def test_short_key_is_refused(self):
        with self.assertRaises(ValueError):
            slot_build.check_boot_trusts_key(self.KEY, self.KEY[:31])


class VerifyTimeWarningTests(unittest.TestCase):
    def test_today_size_gives_no_warning(self):
        self.assertIsNone(slot_build.verify_time_warning(430796))

    def test_700_kb_exactly_gives_no_warning(self):
        self.assertIsNone(slot_build.verify_time_warning(700 * 1024))

    def test_one_byte_over_700_kb_warns(self):
        w = slot_build.verify_time_warning(700 * 1024 + 1)
        self.assertIsNotNone(w)
        self.assertIn("700 KB", w)

    def test_full_slot_warns_with_its_verify_time(self):
        # 118 ms fixed (Ed25519 equation) + 1.87 us per byte (SHA-512), measured 2026-11-07
        self.assertIn("1.8 s", slot_build.verify_time_warning(880640))

    def test_warning_starts_below_the_budget(self):
        self.assertLess(slot_build.verify_time_estimate_s(700 * 1024), 1.5)
        self.assertGreater(slot_build.verify_time_estimate_s(760 * 1024), 1.5)


def _header_text(*parts):
    with open(os.path.join(ROOT, *parts), encoding="utf-8") as f:
        return f.read()


class LayoutTests(unittest.TestCase):
    FS_START = 0x101BF000  # the core's FS_START for board_build.filesystem_size = 0.25m (platformio.ini)

    def test_slots_end_before_littlefs(self):
        self.assertIsNone(slot_build.slot_layout_problem(self.FS_START))

    def test_slot_b_ending_exactly_at_littlefs_is_allowed(self):
        self.assertIsNone(slot_build.slot_layout_problem(0x10000000 + 0xE7000 + 0xD7000))

    def test_slot_b_overlapping_littlefs_is_refused(self):
        # A larger file system (e.g. 0.5m) would start inside slot B: an upload would erase the settings
        problem = slot_build.slot_layout_problem(0x1017F000)
        self.assertIsNotNone(problem)
        self.assertIn("LittleFS", problem)
        self.assertIsNotNone(slot_build.slot_layout_problem(0x10000000 + 0xE7000 + 0xD7000 - 1))

    def test_python_layout_matches_fw_layout_h(self):
        text = _header_text("lib", "fw_image", "include", "fw_layout.h")
        slots = re.search(r"FW_SLOT_OFFSET\[2\] = \{(0x[0-9A-Fa-f]+)u, (0x[0-9A-Fa-f]+)u\}", text)
        records = re.search(r"FW_BOOT_RECORD_OFFSET\[2\] = \{(0x[0-9A-Fa-f]+)u, (0x[0-9A-Fa-f]+)u\}", text)
        size = re.search(r"FW_SLOT_SIZE = (0x[0-9A-Fa-f]+)u;", text)
        header = re.search(r"FW_SLOT_HEADER_SIZE = (0x[0-9A-Fa-f]+)u;", text)
        self.assertTrue(slots and records and size and header, "fw_layout.h changed shape")
        self.assertEqual({0: int(slots.group(1), 16), 1: int(slots.group(2), 16)}, slot_build.SLOT_OFFSETS)
        self.assertEqual(int(size.group(1), 16), slot_build.SLOT_LENGTH)
        self.assertEqual(int(header.group(1), 16), slot_build.HEADER_SIZE)
        self.assertEqual(int(records.group(1), 16), slot_build.BOOT_RECORD_OFFSET)
        self.assertEqual(int(records.group(2), 16) + 0x1000,
                         slot_build.BOOT_RECORD_OFFSET + slot_build.BOOT_RECORD_SIZE)

    def test_http_stream_max_holds_two_slots_and_the_file_header(self):
        text = _header_text("lib", "web_ui", "include", "http_request.h")
        m = re.search(r"HTTP_STREAM_MAX = (0x[0-9A-Fa-f]+)u \* 2 \+ 64;", text)
        self.assertIsNotNone(m, "http_request.h changed shape")
        self.assertEqual(slot_build.SLOT_LENGTH, int(m.group(1), 16))


if __name__ == "__main__":
    unittest.main()
