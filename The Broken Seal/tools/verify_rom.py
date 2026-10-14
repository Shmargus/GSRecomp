#!/usr/bin/env python3
"""Verify the exact Golden Sun ROM revision supported by the first target."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import sys

EXPECTED_SHA1 = "5c4695205413df7db52b9a184815a07783999971"
EXPECTED_SIZE = 8_388_608
CHUNK_SIZE = 1024 * 1024


def sha1_file(path: Path) -> str:
    digest = hashlib.sha1()
    with path.open("rb") as handle:
        while chunk := handle.read(CHUNK_SIZE):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Verify the supported Golden Sun USA/Europe GBA ROM."
    )
    parser.add_argument("rom", type=Path, help="Path to the user-owned .gba file")
    args = parser.parse_args()

    path: Path = args.rom.expanduser().resolve()
    if not path.is_file():
        print(f"ERROR: ROM file not found: {path}", file=sys.stderr)
        return 2

    size = path.stat().st_size
    digest = sha1_file(path)

    print(f"Path:   {path}")
    print(f"Size:   {size} bytes")
    print(f"SHA-1:  {digest}")

    if size != EXPECTED_SIZE:
        print(
            f"ERROR: expected {EXPECTED_SIZE} bytes, got {size}.",
            file=sys.stderr,
        )
        return 1

    if digest.lower() != EXPECTED_SHA1:
        print(
            "ERROR: unsupported ROM revision. Do not continue with guessed addresses.",
            file=sys.stderr,
        )
        return 1

    print("OK: exact supported Golden Sun ROM verified.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
