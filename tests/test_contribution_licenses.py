#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Source-license scope regressions against the actual REUSE command."""

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent
CHECK = ROOT / "scripts/check-contribution-licenses.py"


class SourceLicenseScope(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="monado-license-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        subprocess.run(["git", "init", "--quiet", str(self.root)], check=True)
        (self.root / "LICENSES").mkdir()
        shutil.copyfile(ROOT / "LICENSES/BSL-1.0.txt", self.root / "LICENSES/BSL-1.0.txt")
        self.write("src/example.c", self.header() + "int value;\n")

    @staticmethod
    def header(license_id="BSL-1.0"):
        # Assemble fixture tags so they aren't mistaken for this test's license.
        return ("// SPDX-" + "FileCopyrightText: 2026 Example contributor\n"
                "// SPDX-" + f"License-Identifier: {license_id}\n")

    def write(self, name, content):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
        return path

    def check_scope(self):
        return subprocess.run(
            [sys.executable, str(CHECK), "--root", str(self.root)],
            capture_output=True, text=True, timeout=30,
        )

    def test_licensed_source_passes(self):
        result = self.check_scope()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_unlicensed_monado_source_fails(self):
        self.write("src/xrt/new_driver.c", "int unlicensed;\n")
        result = self.check_scope()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("src/xrt/new_driver.c", result.stdout)

    def test_ordinary_log_and_test_fixture_are_not_excluded(self):
        for name in ("tests/fixtures/result.json", "doc/new.log"):
            with self.subTest(name=name):
                path = self.write(name, "unlicensed fixture\n")
                result = self.check_scope()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(name, result.stdout)
                path.unlink()

    def test_historical_artifacts_remain_unchanged_and_outside_scope(self):
        log = self.write("doc/alyx-valve-lua-results/capture.log", "third-party diagnostic output\n")
        patch = self.write("experiments/alyx-vulkan/patches/external.patch", self.header("GPL-3.0-or-later"))
        before = (log.read_bytes(), patch.read_bytes())
        result = self.check_scope()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("2 archived files excluded", result.stdout)
        self.assertEqual(before, (log.read_bytes(), patch.read_bytes()))

    def test_missing_license_text_in_source_still_fails(self):
        self.write("src/example.c", self.header("MIT") + "int value;\n")
        result = self.check_scope()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Missing licenses: MIT", result.stdout)

    def test_ignored_generated_files_do_not_enter_source_scope(self):
        self.write(".gitignore", self.header().replace("//", "#") + "generated/\n")
        self.write("generated/capture.log", "unlicensed generated data\n")
        result = self.check_scope()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
