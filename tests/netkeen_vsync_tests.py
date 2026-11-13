#!/usr/bin/env python3
"""Test the reversible NetKeen repair against a supplied original executable.

python3 tests/netkeen_vsync_tests.py --executable /path/to/original/netkeen.exe
The game is not redistributed; tests work on temporary copies only.
"""

import argparse
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("netkeen_vsync", ROOT / "contrib/netkeen/fix_vsync.py")
fix = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fix)
ORIGINAL = None


class NetKeenVsyncTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "netkeen.exe"
        self.path.write_bytes(ORIGINAL)
        self.backup = self.path.with_name("netkeen.exe.original")

    def test_in_place_repair_preserves_original_and_restores_exactly(self):
        fix.repair(self.path)
        self.assertEqual(self.backup.read_bytes(), ORIGINAL)
        self.assertEqual(fix.digest(self.path.read_bytes()), fix.PATCHED_SHA256)
        fix.repair(self.path, restore=True)
        self.assertEqual(self.path.read_bytes(), ORIGINAL)

    def test_repeat_repair_is_idempotent(self):
        fix.repair(self.path)
        first = self.path.read_bytes()
        self.assertIn("Already repaired", fix.repair(self.path))
        self.assertEqual(self.path.read_bytes(), first)
        self.assertEqual(self.backup.read_bytes(), ORIGINAL)

    def test_separate_output_changes_only_the_two_display_routines(self):
        output = self.path.with_name("NKSTABLE.EXE")
        fix.repair(self.path, output=output)
        result = output.read_bytes()
        self.assertEqual(self.path.read_bytes(), ORIGINAL)
        self.assertEqual(len(result), len(ORIGINAL))
        self.assertEqual(result[:fix.OFFSET], ORIGINAL[:fix.OFFSET])
        end = fix.OFFSET + len(fix.REPLACEMENT)
        self.assertEqual(result[end:], ORIGINAL[end:])
        self.assertFalse(self.backup.exists())
        with self.assertRaises(ValueError):
            fix.repair(self.path, output=output)
        self.assertEqual(output.read_bytes(), result)

    def test_unknown_version_and_conflicting_backup_are_untouched(self):
        unknown = b"MZ unsupported version"
        self.path.write_bytes(unknown)
        with self.assertRaises(ValueError):
            fix.repair(self.path)
        self.assertEqual(self.path.read_bytes(), unknown)
        self.assertFalse(self.backup.exists())
        self.path.write_bytes(ORIGINAL)
        self.backup.write_bytes(b"another backup")
        with self.assertRaises(ValueError):
            fix.repair(self.path)
        self.assertEqual(self.path.read_bytes(), ORIGINAL)
        self.assertEqual(self.backup.read_bytes(), b"another backup")

    def test_assembly_matches_the_installed_repair(self):
        if not shutil.which("nasm"):
            self.skipTest("nasm is needed to check assembly reproducibility")
        output = self.path.with_suffix(".bin")
        subprocess.run(["nasm", "-f", "bin", str(ROOT / "contrib/netkeen/vsync.asm"),
                        "-o", str(output)], check=True)
        self.assertEqual(output.read_bytes(), fix.REPLACEMENT)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", required=True, type=Path)
    args, remaining = parser.parse_known_args()
    ORIGINAL = args.executable.read_bytes()
    if fix.digest(ORIGINAL) != fix.ORIGINAL_SHA256:
        parser.error("Supply the original NetKeen 2013-03-26 executable")
    unittest.main(argv=[__file__] + remaining)
