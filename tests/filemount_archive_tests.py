#!/usr/bin/env python3
"""FILEMOUNT archive integration tests, using a built DOSBox-X executable.

Run: python3 tests/filemount_archive_tests.py [--binary /path/to/dosbox-x]
Uses only the Python standard library; DOSBox-X runs with its silent SDL driver.
"""

import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import zipfile


BINARY = Path(__file__).resolve().parents[1] / "src" / "dosbox-x"


class FileMountArchiveTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="filemount archives ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.drive = self.root / "drive"
        self.drive.mkdir()
        self.scratch = self.root / "scratch"
        self.scratch.mkdir()

    def run_dos(self, commands):
        config = self.root / "test.conf"
        config.write_text(
            "[sdl]\noutput=surface\n[dosbox]\nmemsize=16\n[autoexec]\n"
            f'mount c "{self.drive}"\nc:\n'
            + "\n".join(commands) + "\nexit\n",
            encoding="utf-8",
        )
        env = dict(os.environ, TMPDIR=str(self.scratch), TEMP=str(self.scratch),
                   TMP=str(self.scratch))
        result = subprocess.run(
            [str(BINARY), "-silent", "-noconfig", "-conf", str(config),
             "-nomenu", "-nogui"],
            cwd=self.root, env=env, capture_output=True, timeout=30,
        )
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
        self.assertEqual(list(self.scratch.iterdir()), [], "archive copies were not cleaned up")

    def make_zip(self, name="helpers.ZIP", entries=None, compression=zipfile.ZIP_DEFLATED):
        archive = self.root / name
        with zipfile.ZipFile(archive, "w", compression=compression) as z:
            for entry, data in (entries or {}).items():
                z.writestr(entry, data)
        return archive

    def test_stored_deflated_empty_and_long_nested_paths(self):
        payload = bytes(range(256)) * 1100  # Binary data spanning several read buffers.
        deep = "/".join(["nested_directory"] * 17) + "/file.bin"
        archive = self.make_zip(entries={
            "res/gameres/helper.bin": payload,
            "res/gameres/empty.bin": b"",
            "res/a file.bin": b"spaces",
            "root.bin": b"root entry",
            deep: b"deep entry",
        })
        stored = self.make_zip("stored.zip", {"res/file.bin": payload}, zipfile.ZIP_STORED)
        sources = [
            (str(archive) + "/res/gameres/helper.bin", payload),
            (str(archive) + r"\res\gameres\helper.bin", payload),
            (str(archive) + "/res/./gameres/../gameres/helper.bin", payload),
            (str(archive) + "/res/gameres/empty.bin", b""),
            (str(archive) + "/res/a file.bin", b"spaces"),
            (str(archive) + "/root.bin", b"root entry"),
            (str(archive) + "/" + deep, b"deep entry"),
            (str(stored) + "/res/file.bin", payload),
        ]
        original = archive.read_bytes()
        (self.drive / "SUB").mkdir()
        commands = []
        for i, (source, _) in enumerate(sources):
            commands.extend([
                f'filemount C:\\SUB\\IN.BIN "{source}" > MOUNT{i}.LOG',
                f"copy /b C:\\SUB\\IN.BIN C:\\OUT{i}.BIN > NUL",
            ])
        self.run_dos(commands)
        for i, (_, expected) in enumerate(sources):
            with self.subTest(source=sources[i][0]):
                self.assertEqual((self.drive / f"OUT{i}.BIN").read_bytes(), expected)
                self.assertIn(b"Mounted file" if i == 0 else b"Updated file",
                              (self.drive / f"MOUNT{i}.LOG").read_bytes())
        self.assertFalse((self.drive / "SUB" / "IN.BIN").exists())
        self.assertEqual(archive.read_bytes(), original)

    def test_invalid_entries_preserve_the_previous_mapping(self):
        archive = self.make_zip(entries={
            "res/good.bin": b"good mapping", "res/": b"", "res/subdir/": b"",
        })
        invalid = self.root / "invalid.zip"
        invalid.write_bytes(b"not a zip archive")
        corrupt = self.make_zip("corrupt.zip", {"bad.bin": b"CRC validation"}, zipfile.ZIP_STORED)
        data = bytearray(corrupt.read_bytes())
        data[data.index(b"CRC validation")] ^= 1
        corrupt.write_bytes(data)
        encrypted = self.make_zip("encrypted.zip", {"bad.bin": b"encrypted"}, zipfile.ZIP_STORED)
        data = bytearray(encrypted.read_bytes())
        for marker, flag_offset in [(b"PK\x03\x04", 6), (b"PK\x01\x02", 8)]:
            data[data.index(marker) + flag_offset] |= 1
        encrypted.write_bytes(data)
        broken = self.make_zip("broken.zip", {"bad.bin": b"broken header"})
        data = bytearray(broken.read_bytes())
        data[:4] = b"FAIL"  # Central directory is readable, entry extraction must fail.
        broken.write_bytes(data)
        failures = [
            str(archive) + "/res/missing.bin", str(archive) + "/res",
            str(archive) + "/res/subdir/", str(archive) + "/",
            str(invalid) + "/bad.bin", str(corrupt) + "/bad.bin",
            str(encrypted) + "/bad.bin", str(broken) + "/bad.bin",
        ]
        commands = [f'filemount IN.BIN "{archive}/res/good.bin"']
        for i, source in enumerate(failures):
            commands.extend([
                f'filemount IN.BIN "{source}" > FAIL{i}.LOG',
                f"copy /b IN.BIN OUT{i}.BIN > NUL",
            ])
        self.run_dos(commands)
        for i, source in enumerate(failures):
            with self.subTest(source=source):
                self.assertIn(b"Host file not found", (self.drive / f"FAIL{i}.LOG").read_bytes())
                self.assertEqual((self.drive / f"OUT{i}.BIN").read_bytes(), b"good mapping")

    def test_unmount_restores_shadowed_host_file_and_leaves_archive_untouched(self):
        archive = self.make_zip(entries={"nested/file.bin": b"archive data"})
        original = archive.read_bytes()
        (self.drive / "IN.BIN").write_bytes(b"host data")
        self.run_dos([
            f'filemount IN.BIN "{archive}/nested/file.bin"',
            "copy /b IN.BIN BEFORE.BIN > NUL",
            "filemount > LIST.LOG",
            "filemount /D IN.BIN",
            "copy /b IN.BIN AFTER.BIN > NUL",
        ])
        self.assertEqual((self.drive / "BEFORE.BIN").read_bytes(), b"archive data")
        self.assertEqual((self.drive / "AFTER.BIN").read_bytes(), b"host data")
        self.assertIn(b"IN.BIN <=", (self.drive / "LIST.LOG").read_bytes())
        self.assertEqual(archive.read_bytes(), original)

    def test_real_zip_named_directories_and_plain_host_files(self):
        folder = self.root / "folder.zip"
        folder.mkdir()
        archive = self.make_zip("folder.zip/inner.zip", {"res/file.bin": b"inner archive"})
        plain = folder / "plain.bin"
        plain.write_bytes(b"plain host file")
        self.run_dos([
            f'filemount IN.BIN "{archive}/res/file.bin"',
            "copy /b IN.BIN ZIP.BIN > NUL",
            f'filemount IN.BIN "{plain}"',
            "copy /b IN.BIN PLAIN.BIN > NUL",
        ])
        self.assertEqual((self.drive / "ZIP.BIN").read_bytes(), b"inner archive")
        self.assertEqual((self.drive / "PLAIN.BIN").read_bytes(), b"plain host file")

    def test_existing_asar_packed_link_and_unpacked_files(self):
        payload = b"ASAR packed file"
        tree = {"files": {
            "res": {"files": {"file.bin": {"size": len(payload), "offset": "0"}}},
            "link.bin": {"link": "res/file.bin"},
            "loose.bin": {"size": 5, "unpacked": True},
        }}
        header = json.dumps(tree).encode()
        header += b" " * (-len(header) % 4)
        archive = self.root / "app.asar"
        archive.write_bytes(struct.pack("<4I", 4, 8 + len(header), 4 + len(header), len(header))
                            + header + payload)
        unpacked = self.root / "app.asar.unpacked"
        unpacked.mkdir()
        (unpacked / "loose.bin").write_bytes(b"loose")
        commands = []
        for i, inner in enumerate(["res/file.bin", "link.bin", "loose.bin"]):
            commands.extend([
                f'filemount IN.BIN "{archive}/{inner}"',
                f"copy /b IN.BIN OUT{i}.BIN > NUL",
            ])
        self.run_dos(commands)
        for i, expected in enumerate([payload, payload, b"loose"]):
            self.assertEqual((self.drive / f"OUT{i}.BIN").read_bytes(), expected)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=BINARY)
    args, remaining = parser.parse_known_args()
    BINARY = args.binary.resolve()
    unittest.main(argv=[__file__] + remaining)
