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
Nothing is written; the ROMs are only read.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np

WINDOW = 32         # bytes compared per window
ROM_STEP = 16       # ROM windows start every ROM_STEP bytes
MIN_DISTINCT = 8    # windows with fewer different byte values are skipped
CHUNK = 16 << 20    # file bytes hashed at a time

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

    def matches(self, data: np.ndarray, base: int):
        """(file offset, ROM offset) of every verified window match."""
        hashes = window_hashes(data)
        hits = np.nonzero(np.isin(hashes, self.hashes))[0]
        for i in hits:
            at = np.searchsorted(self.hashes, hashes[i])
            while at < self.hashes.size and self.hashes[at] == hashes[i]:
                rom_at = int(self.offsets[at])
                if np.array_equal(data[i:i + WINDOW],
                                  self.data[rom_at:rom_at + WINDOW]):
                    yield base + int(i), rom_at
                at += 1


def scan_file(path: Path, roms: list[RomIndex]) -> list[str]:
    raw = np.frombuffer(path.read_bytes(), dtype=np.uint8)
    found = []
    for start in range(0, max(raw.size, 1), CHUNK):
        chunk = raw[start:start + CHUNK + WINDOW - 1]
        for rom in roms:
            for file_at, rom_at in rom.matches(chunk, start):
                found.append(f"{path}: offset 0x{file_at:X} = {rom.path.name} "
                             f"0x{0x08000000 + rom_at:08X}")
                if len(found) >= 5:
                    return found
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--rom", action="append", required=True, type=Path)
    parser.add_argument("--skip", action="append", default=[],
                        help="folder name left unscanned (third-party toolchain)")
    parser.add_argument("folders", nargs="+", type=Path)
    args = parser.parse_args()

    roms = [RomIndex(path) for path in args.rom]
    skip = {name.lower() for name in args.skip}
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
    for path in files:
        problems += scan_file(path, roms)
    total = sum(path.stat().st_size for path in files)
    print(f"ROM scan: {len(files)} files, {total / 2**20:.0f} MB checked against "
          f"{len(roms)} ROM(s).")
    if problems:
        print("ROM bytes found in the release:")
        for line in problems:
            print("  " + line)
        return 1
    print("No ROM bytes found.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
