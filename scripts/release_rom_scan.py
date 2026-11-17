#!/usr/bin/env python3
"""Fail when a release folder holds bytes copied from a game ROM.

The release's file-name checks (no .gba, no game library, no generated
source) cannot see game code or data compiled into a program. This scans
the CONTENT of every shipped file for runs of the player's ROM:

    python scripts/release_rom_scan.py --rom A.gba --rom B.gba \
        --skip mingw64 --skip toolchain Releases/...

A run is reported when 32 bytes at a 16-byte-aligned ROM offset (so every
copied run of 47 bytes or more is seen) appear anywhere in a file. Windows
with fewer than 8 different byte values (padding, fills, simple tables) are
left out: they say nothing about where bytes came from. --skip names folders
left unscanned: the unmodified third-party compilers the release ships.
A zip or APK is read through: each file inside it is scanned as it is after
unpacking, and so is a zip inside that (the Android app's payload); --skip
applies to the folders inside as well.
Nothing is written; the ROMs are only read.
"""

from __future__ import annotations

import argparse
import io
import sys
import zipfile
from pathlib import Path

import numpy as np

WINDOW = 32         # bytes compared per window
ROM_STEP = 16       # ROM windows start every ROM_STEP bytes
MIN_DISTINCT = 8    # windows with fewer different byte values are skipped
CHUNK = 16 << 20    # file bytes hashed at a time
FILTER_BITS = 24    # RomIndex.filter: one flag per value of a hash's top bits

_MIX = np.array([0x9E3779B97F4A7C15, 0xC2B2AE3D27D4EB4F,
                 0x165667B19E3779F9, 0x27D4EB2F165667C5], dtype=np.uint64)


def window_hashes(data: np.ndarray) -> np.ndarray:
    """A 64-bit hash of the WINDOW bytes starting at every offset."""
    n = data.size - WINDOW + 1
    if n <= 0:
        return np.empty(0, dtype=np.uint64)
    words = np.zeros(data.size - 7, dtype=np.uint64)
    for j in range(8):
        words |= data[j:data.size - 7 + j].astype(np.uint64) << np.uint64(8 * j)
    h = np.zeros(n, dtype=np.uint64)
    with np.errstate(over="ignore"):
        for k in range(4):
            h ^= words[8 * k:8 * k + n] * _MIX[k]
            h = (h << np.uint64(31)) | (h >> np.uint64(33))
    return h


def counting(rows: np.ndarray) -> np.ndarray:
    """Windows that are a counter in 8-, 16- or 32-bit steps (0, 1, 2, ...,
    possibly wrapping once, as 12..15 then 0..11): at most two different
    steps between neighbours. Our own programs hold such index tables too;
    they were the only matches on the 14 October 2026 engine, launcher and
    builder binaries."""
    result = np.zeros(rows.shape[0], dtype=bool)
    for unit in (np.uint8, np.dtype("<u2"), np.dtype("<u4")):
        values = np.ascontiguousarray(rows).view(unit).astype(np.int64)
        steps = np.sort(np.diff(values, axis=1), axis=1)
        result |= np.count_nonzero(np.diff(steps, axis=1), axis=1) + 1 <= 2
    return result


class RomIndex:
    def __init__(self, path: Path):
        self.path = path
        self.data = np.frombuffer(path.read_bytes(), dtype=np.uint8)
        hashes = window_hashes(self.data)[::ROM_STEP]
        offsets = np.arange(hashes.size, dtype=np.int64) * ROM_STEP
        rows = np.lib.stride_tricks.sliding_window_view(self.data, WINDOW)[::ROM_STEP]
        ordered = np.sort(rows, axis=1)
        distinct = np.count_nonzero(np.diff(ordered, axis=1), axis=1) + 1
        keep = (distinct >= MIN_DISTINCT) & ~counting(rows)
        order = np.argsort(hashes[keep], kind="stable")
        self.hashes = hashes[keep][order]
        self.offsets = offsets[keep][order]
        # Use the high bits as a quick filter. If a prefix occurs in the ROM hash
        # list, check the full hash in the sorted list. On 16 November 2026,
        # np.isin re-sorted the ROM list for each file and took 306 s on a 112 MB
        # release.
        self.filter = np.zeros(1 << FILTER_BITS, dtype=bool)
        self.filter[(self.hashes >> np.uint64(64 - FILTER_BITS)).astype(np.intp)] = True

    def matches(self, data: np.ndarray, base: int, hashes: np.ndarray | None = None):
        """(file offset, ROM offset) of every verified window match.
        `hashes` is window_hashes(data) when the caller already has it."""
        if hashes is None:
            hashes = window_hashes(data)
        flagged = np.nonzero(
            self.filter[(hashes >> np.uint64(64 - FILTER_BITS)).astype(np.intp)])[0]
        if flagged.size == 0 or self.hashes.size == 0:
            return
        first = np.searchsorted(self.hashes, hashes[flagged])
        found = first < self.hashes.size
        found[found] = self.hashes[first[found]] == hashes[flagged[found]]
        for i, at in zip(flagged[found], first[found]):
            while at < self.hashes.size and self.hashes[at] == hashes[i]:
                rom_at = int(self.offsets[at])
                if np.array_equal(data[i:i + WINDOW],
                                  self.data[rom_at:rom_at + WINDOW]):
                    yield base + int(i), rom_at
                at += 1


ARCHIVES = (".zip", ".apk")


def scan_bytes(name: str, data: bytes, roms: list[RomIndex],
               skip: frozenset = frozenset(), counted: list | None = None) -> list[str]:
    """Matches in one file's bytes; for a zip or APK, in every file inside.
    `counted` collects [files, bytes] actually compared."""
    if name.lower().endswith(ARCHIVES) and zipfile.is_zipfile(io.BytesIO(data)):
        found = []
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            for member in archive.infolist():
                if member.is_dir() or any(
                        part.lower() in skip for part in member.filename.split("/")):
                    continue
                found += scan_bytes(f"{name}!{member.filename}", archive.read(member),
                                    roms, skip, counted)
                if len(found) >= 5:
                    break
        return found
    if counted is not None:
        counted[0] += 1
        counted[1] += len(data)
    raw = np.frombuffer(data, dtype=np.uint8)
    found = []
    for start in range(0, max(raw.size, 1), CHUNK):
        chunk = raw[start:start + CHUNK + WINDOW - 1]
        hashes = window_hashes(chunk)  # once, for every ROM
        for rom in roms:
            for file_at, rom_at in rom.matches(chunk, start, hashes):
                found.append(f"{name}: offset 0x{file_at:X} = {rom.path.name} "
                             f"0x{0x08000000 + rom_at:08X}")
                if len(found) >= 5:
                    return found
    return found


def scan_file(path: Path, roms: list[RomIndex], skip: frozenset = frozenset(),
              counted: list | None = None) -> list[str]:
    return scan_bytes(str(path), path.read_bytes(), roms, skip, counted)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--rom", action="append", required=True, type=Path)
    parser.add_argument("--skip", action="append", default=[],
                        help="folder name left unscanned (third-party toolchain)")
    parser.add_argument("folders", nargs="+", type=Path)
    args = parser.parse_args()

    roms = [RomIndex(path) for path in args.rom]
    skip = frozenset(name.lower() for name in args.skip)
    files = []
    for folder in args.folders:
        if folder.is_file():
            files.append(folder)
            continue
        for path in sorted(folder.rglob("*")):
            if path.is_file() and not any(part.lower() in skip
                                          for part in path.relative_to(folder).parts):
                files.append(path)

    problems = []
    counted = [0, 0]
    for path in files:
        problems += scan_file(path, roms, skip, counted)
    print(f"ROM scan: {counted[0]} files, {counted[1] / 2**20:.0f} MB checked against "
          f"{len(roms)} ROM(s).")
    if problems:
        print("ROM bytes found in the release:")
        for line in problems:
            print("  " + line)
        return 3  # not 1, which a Python crash also exits with
    print("No ROM bytes found.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
