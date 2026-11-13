#!/usr/bin/env python3
"""Repair NetKeen 2013-03-26 page flipping; retain the original executable.

Usage: python3 contrib/netkeen/fix_vsync.py /path/to/netkeen.exe
       python3 contrib/netkeen/fix_vsync.py /path/to/netkeen.exe --restore
       python3 contrib/netkeen/fix_vsync.py /path/to/netkeen.exe --output /path/to/NKSTABLE.EXE
"""

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import struct
import tempfile


ORIGINAL_SHA256 = "b395a912f5c2d0b35124a236a7704b93b3f33c472dd7ea8401f0e542c184004a"
PATCHED_SHA256 = "b7b999dd64400a6657407af0d355f5ff9344736287eb0fbac81f4be37dbad1ff"
OFFSET = 0x1044C
# Assembled from vsync.asm with: nasm -f bin vsync.asm -o vsync.bin
# No assembler is needed to apply the repair.
REPLACEMENT = bytes.fromhex(
    "9cfbbada03eca80875fbfa8b4e06bad403b00cee4288e8ee"
    "4ab00dee4288c8eef70608b30100750abac003b033ee8a46"
    "08eefbbada03eca80874fb9dc9cb90909090909090909090"
    "909090909090909090909090909090909090909090909090"
    "909090909090909090909090909090909090909090909090"
    "909090909090909090c8000000e978ff9090909090909090"
    "909090909090909090909090909090909090909090909090"
    "90909090909090909090909090"
)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def patch_executable(data):
    if digest(data) != ORIGINAL_SHA256:
        raise ValueError("Unsupported NetKeen executable; expected the unmodified 2013-03-26 release")
    # A relocation into replaced code would let the DOS loader corrupt the repair.
    count, = struct.unpack_from("<H", data, 6)
    relocations, = struct.unpack_from("<H", data, 24)
    paragraphs, = struct.unpack_from("<H", data, 8)
    for i in range(count):
        offset, segment = struct.unpack_from("<HH", data, relocations + i * 4)
        location = paragraphs * 16 + segment * 16 + offset
        if OFFSET <= location < OFFSET + len(REPLACEMENT):
            raise ValueError("DOS relocation overlaps the repair")
    result = data[:OFFSET] + REPLACEMENT + data[OFFSET + len(REPLACEMENT):]
    if digest(result) != PATCHED_SHA256:
        raise ValueError("Repair checksum mismatch")
    return result


def replace_file(destination, data, metadata_source):
    # Complete the new file before replacing the executable.
    fd, name = tempfile.mkstemp(prefix=".netkeen-vsync-", dir=destination.parent)
    try:
        with os.fdopen(fd, "wb") as out:
            out.write(data)
            out.flush()
            os.fsync(out.fileno())
        shutil.copystat(metadata_source, name)
        os.replace(name, destination)
    finally:
        if os.path.exists(name):
            os.unlink(name)


def repair(path, output=None, restore=False):
    data = path.read_bytes()
    backup = path.with_name(path.name + ".original")
    if restore:
        if digest(data) != PATCHED_SHA256:
            raise ValueError("Restore expects the repaired executable")
        original = backup.read_bytes()
        if digest(original) != ORIGINAL_SHA256:
            raise ValueError("Original backup checksum mismatch")
        replace_file(path, original, backup)
        return "Restored original: " + str(path)
    if digest(data) == PATCHED_SHA256 and output is None:
        return "Already repaired: " + str(path)
    patched = patch_executable(data)
    if output is not None and output.resolve() != path.resolve():
        if output.exists():
            raise ValueError("Output file already exists: " + str(output))
        replace_file(output, patched, path)
        return "Repaired copy: " + str(output)
    if backup.exists():
        if digest(backup.read_bytes()) != ORIGINAL_SHA256:
            raise ValueError("A different backup already exists: " + str(backup))
    else:
        # Never overwrite an existing backup.
        with backup.open("xb") as out:
            out.write(data)
        shutil.copystat(path, backup)
    replace_file(path, patched, path)
    return "Repaired: " + str(path) + " (original: " + str(backup) + ")"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--output", type=Path)
    mode.add_argument("--restore", action="store_true")
    args = parser.parse_args()
    try:
        print(repair(args.executable, args.output, args.restore))
    except (OSError, ValueError) as error:
        parser.exit(1, str(error) + "\n")


if __name__ == "__main__":
    main()
