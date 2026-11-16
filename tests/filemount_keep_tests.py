#!/usr/bin/env python3
"""FILEMOUNT /K integration tests, using a built DOSBox-X executable.

/K maps a file the guest writes itself (a log, a scratch file) to a host path outside the mounted
folder, and keeps the mapping when the guest deletes the file.

Run: python3 tests/filemount_keep_tests.py [--binary /path/to/dosbox-x]
Uses only the Python standard library; DOSBox-X runs with its silent SDL driver.
"""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


BINARY = Path(__file__).resolve().parents[1] / "src" / "dosbox-x"


class FileMountKeepTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="filemount keep ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.drive = self.root / "drive"
        self.drive.mkdir()
        self.outside = self.root / "outside"
        self.outside.mkdir()

    def run_dos(self, commands):
        config = self.root / "test.conf"
        config.write_text(
            "[sdl]\noutput=surface\n[dosbox]\nmemsize=16\n[autoexec]\n"
            f'mount c "{self.drive}"\nc:\n'
            + "\n".join(commands) + "\nexit\n",
            encoding="utf-8",
        )
        result = subprocess.run(
            [str(BINARY), "-silent", "-noconfig", "-conf", str(config),
             "-nomenu", "-nogui"],
            cwd=self.root, env=dict(os.environ), capture_output=True, timeout=30,
        )
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))

    def read(self, name):
        return (self.drive / name).read_bytes().replace(b"\r\n", b"\n")

    def said_exists(self, name):
        """Whether `if exist FILE echo listed > name` found the file (the redirection creates `name` either way)."""
        return b"listed" in self.read(name)

    def test_a_file_the_guest_writes_lands_outside_and_the_mapping_survives_a_delete(self):
        host = self.outside / "game.log"  # not there yet: the guest creates it
        self.run_dos([
            f'filemount /K C:\\GAME.LOG "{host}" > MOUNT.LOG',
            "if exist GAME.LOG echo listed > EARLY.TXT",
            "echo first> GAME.LOG",
            "echo second>> GAME.LOG",
            "copy /b GAME.LOG AFTER1.TXT > NUL",
            "del GAME.LOG",
            "if exist GAME.LOG echo listed > DELETED.TXT",
            "filemount > LIST.LOG",
            "echo third>> GAME.LOG",
            "copy /b GAME.LOG AFTER2.TXT > NUL",
        ])
        self.assertIn(b"Mounted file", self.read("MOUNT.LOG"))
        self.assertFalse(self.said_exists("EARLY.TXT"), "the file does not exist before the guest creates it")
        self.assertEqual(self.read("AFTER1.TXT"), b"first\nsecond\n")
        self.assertFalse(self.said_exists("DELETED.TXT"), "a deleted file is gone for the guest")
        self.assertIn(b"GAME.LOG <=", self.read("LIST.LOG"))
        self.assertIn(b"[/K]", self.read("LIST.LOG"), "the mapping is still there after the delete")
        self.assertEqual(self.read("AFTER2.TXT"), b"third\n", "created again, without the old content")
        self.assertEqual(host.read_bytes().replace(b"\r\n", b"\n"), b"third\n")
        self.assertFalse((self.drive / "GAME.LOG").exists(), "nothing was written into the mounted folder")

    def test_a_real_file_of_that_name_stays_hidden_and_untouched(self):
        (self.drive / "GAME.LOG").write_bytes(b"the player's own file")
        host = self.outside / "game.log"
        self.run_dos([
            f'filemount /K GAME.LOG "{host}"',
            "if exist GAME.LOG echo listed > BEFORE.TXT",
            "del GAME.LOG",
            "echo guest> GAME.LOG",
            "del GAME.LOG",
            "if exist GAME.LOG echo listed > AFTER.TXT",
            "dir /b > DIR.TXT",
        ])
        self.assertFalse(self.said_exists("BEFORE.TXT"), "the real file is hidden while the kept file is not there")
        self.assertFalse(self.said_exists("AFTER.TXT"))
        self.assertNotIn(b"GAME.LOG", self.read("DIR.TXT"))
        self.assertEqual((self.drive / "GAME.LOG").read_bytes(), b"the player's own file")
        self.assertFalse(host.exists(), "the guest's delete removed the host file")

    def test_an_existing_host_file_is_shown_and_unmounting_leaves_it(self):
        host = self.outside / "notes.txt"
        host.write_bytes(b"kept notes\r\n")
        self.run_dos([
            f'filemount /K NOTES.TXT "{host}"',
            "copy /b NOTES.TXT COPY.TXT > NUL",
            "echo more>> NOTES.TXT",
            "filemount /D NOTES.TXT",
            "if exist NOTES.TXT echo listed > AFTER.TXT",
            "if exist COPY.TXT echo listed > CONTROL.TXT",
        ])
        self.assertEqual(self.read("COPY.TXT"), b"kept notes\n")
        self.assertEqual(host.read_bytes().replace(b"\r\n", b"\n"), b"kept notes\nmore\n")
        self.assertFalse(self.said_exists("AFTER.TXT"))
        self.assertTrue(self.said_exists("CONTROL.TXT"), "the probe itself sees a file that is there")

    def test_without_k_a_delete_still_drops_the_mapping(self):
        host = self.outside / "data.bin"
        host.write_bytes(b"host data")
        self.run_dos([
            f'filemount DATA.BIN "{host}"',
            "del DATA.BIN",
            "filemount > LIST.LOG",
            "echo guest> DATA.BIN",
        ])
        self.assertIn(b"No files are currently mounted", self.read("LIST.LOG"))
        self.assertEqual(host.read_bytes(), b"host data", "the host file is left untouched")
        self.assertEqual(self.read("DATA.BIN"), b"guest\n", "the guest's new file is a real one")

    def test_a_folder_that_does_not_exist_or_a_folder_itself_is_refused(self):
        self.run_dos([
            f'filemount /K A.LOG "{self.outside / "missing" / "a.log"}" > NOFOLDER.LOG',
            f'filemount /K B.LOG "{self.outside}" > ISFOLDER.LOG',
            "filemount /K C.LOG > USAGE.LOG",
            "filemount > LIST.LOG",
        ])
        self.assertIn(b"Host file not found", self.read("NOFOLDER.LOG"))
        self.assertIn(b"Host file not found", self.read("ISFOLDER.LOG"))
        self.assertIn(b"Usage: FILEMOUNT", self.read("USAGE.LOG"))
        self.assertIn(b"No files are currently mounted", self.read("LIST.LOG"))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=BINARY)
    args, remaining = parser.parse_known_args()
    BINARY = args.binary.resolve()
    unittest.main(argv=[__file__] + remaining)
