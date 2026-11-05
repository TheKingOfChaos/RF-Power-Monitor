"""Tests for scripts/build_boot.py: key selection and rebuild inputs. No hardware.

Run:
    python -m unittest discover -s test/tools -p "test_build_boot.py" -v
"""

import importlib.util
import os
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

spec = importlib.util.spec_from_file_location("build_boot", os.path.join(ROOT, "scripts", "build_boot.py"))
build_boot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build_boot)


class KeySelectionTests(unittest.TestCase):
    def test_field_key_build_defines_the_field_key_switch(self):
        self.assertIn("-DRFPOWER_FW_FIELD_KEY", build_boot.compile_flags(True, "core"))

    def test_development_build_does_not_define_it(self):
        self.assertNotIn("-DRFPOWER_FW_FIELD_KEY", build_boot.compile_flags(False, "core"))

    def test_rfpower_fw_key_field_selects_the_field_key(self):
        old = os.environ.get("RFPOWER_FW_KEY")
        try:
            os.environ["RFPOWER_FW_KEY"] = "field"
            self.assertTrue(build_boot.field_key_from_env())
            os.environ["RFPOWER_FW_KEY"] = "dev"
            self.assertFalse(build_boot.field_key_from_env())
            del os.environ["RFPOWER_FW_KEY"]
            self.assertFalse(build_boot.field_key_from_env())
        finally:
            if old is None:
                os.environ.pop("RFPOWER_FW_KEY", None)
            else:
                os.environ["RFPOWER_FW_KEY"] = old

    def test_key_change_changes_the_stamp(self):
        self.assertNotEqual(build_boot.stamp_text("b2", "g++", "core", True),
                            build_boot.stamp_text("b2", "g++", "core", False))


class RebuildInputTests(unittest.TestCase):
    def test_key_headers_are_inputs(self):
        names = [os.path.relpath(p, ROOT).replace(os.sep, "/") for p in build_boot.project_inputs()]
        for header in ("include/fw_key.h", "include/fw_dev_public_key.h", "include/pins.h"):
            self.assertIn(header, names)
        if os.path.exists(os.path.join(ROOT, "include", "fw_public_key.h")):
            self.assertIn("include/fw_public_key.h", names)

    def test_library_headers_and_sources_are_inputs(self):
        names = [os.path.relpath(p, ROOT).replace(os.sep, "/") for p in build_boot.project_inputs()]
        for f in ("lib/fw_update/include/boot_decision.h", "lib/fw_image/include/fw_layout.h",
                  "lib/monocypher/src/monocypher.h", "lib/fw_update/src/boot_decision.cpp",
                  "boot/boot_main.cpp", "boot/startup.S", "boot/boot.ld", "boot/sdk/pico/config_autogen.h",
                  "scripts/build_boot.py"):
            self.assertIn(f, names)

    def test_every_extra_source_exists(self):
        for p in build_boot.EXTRA_SOURCES:
            self.assertTrue(os.path.isfile(os.path.join(ROOT, p)), p)


if __name__ == "__main__":
    unittest.main()
