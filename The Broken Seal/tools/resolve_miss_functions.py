#!/usr/bin/env python3
"""Resolve strict-static / self-heal miss PCs to their containing ELF functions.

Every interior miss this project has seen is an asynchronous IRQ return landing
inside a function the corpus already knows. Turning that PC into a reviewed
`REVIEWED_RESUME_FUNCTIONS` entry needs the containing `STT_FUNC`, its size and
its mode, and looking those up by hand is the slow part of the loop.

Reads a runner log on stdin or from a path, extracts every miss PC it reports,
and prints the ELF function containing each one. A PC with no containing sized
`STT_FUNC` is reported as such rather than guessed at — those are the genuinely
new images that need DMA/store attribution instead.

    python tools/resolve_miss_functions.py --elf <goldensun.elf> run.log
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

from import_main_symbols import STT_FUNC, parse_elf32_arm  # noqa: E402


MISS_RE = re.compile(
    r"(?:STRICT_STATIC|SELF-HEAL) dispatch miss for pc=0x([0-9A-Fa-f]+)"
    r"[^(]*\((thumb|arm)\)"
)
TRANSIENT_RE = re.compile(
    r"unknown transient code identity at 0x([0-9A-Fa-f]+)"
)
MODE_MISMATCH_RE = re.compile(
    r"transient code mode mismatch at 0x([0-9A-Fa-f]+)"
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--elf", required=True, type=Path)
    parser.add_argument("log", nargs="?", type=Path)
    args = parser.parse_args()

    text = (
        args.log.read_text(encoding="utf-8", errors="replace")
        if args.log
        else sys.stdin.read()
    )
    image = parse_elf32_arm(args.elf.expanduser().resolve().read_bytes())
    functions = sorted(
        (symbol.value & ~1, symbol.size, symbol.name)
        for symbol in image.symbols
        if symbol.symbol_type == STT_FUNC and symbol.size > 0
    )

    def containing(pc: int) -> tuple[int, int, str] | None:
        for start, size, name in functions:
            if start <= pc < start + size:
                return start, size, name
        return None

    misses = sorted({(int(pc, 16), mode) for pc, mode in MISS_RE.findall(text)})
    if misses:
        print(f"{len(misses)} distinct miss PC(s):")
    for pc, mode in misses:
        hit = containing(pc)
        if hit is None:
            print(f"  0x{pc:08X} ({mode})  NO CONTAINING STT_FUNC "
                  "— needs writer attribution, not a resume entry")
            continue
        start, size, name = hit
        print(f"  0x{pc:08X} ({mode})  {name} 0x{start:08x} size=0x{size:x} "
              f"(+0x{pc - start:x})")
        print(f'    {{"addr": 0x{start:08X}, "mode": "{mode}", '
              f'"note": "observed resume at 0x{pc:08x} ({name}+0x{pc - start:x})"}},')

    for label, pattern in (
        ("unknown transient identity", TRANSIENT_RE),
        ("transient mode mismatch", MODE_MISMATCH_RE),
    ):
        for pc in sorted({int(value, 16) for value in pattern.findall(text)}):
            print(f"  0x{pc:08X}  {label} — match the live image dump against "
                  "the pinned overlay/ROM set before registering")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
