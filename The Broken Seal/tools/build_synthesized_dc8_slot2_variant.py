#!/usr/bin/env python3
"""Build a private ROM/config pair for Func_dc8's second synthesized slot.

The mixer loop at 0x03000A58..0x03000A94 has eight placeholder words (ROM
`mov r0, r0`) at 0x03000A5C/60, 68/6C, 74/78, 80/84. Before the loop,
0x03000A20..0x03000A50 copies eight words from the table at 0x030009C4,
starting (r3 & 3) * 8 bytes in, into those pairs: one variant per sample
byte alignment, four in all (FACTS.md, 2026-09-23).
"""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path


EXPECTED_ROM_SHA1 = "5c4695205413df7db52b9a184815a07783999971"
ROM_BASE = 0x08000000
RUNTIME_BASE = 0x03000000
COPY_SOURCE = 0x08000770
LOOP_START = 0x03000A58
LOOP_END = 0x03000A98
TABLE = 0x030009C4
HOLES = (0x03000A5C, 0x03000A68, 0x03000A74, 0x03000A80)
VARIANTS = (0, 1, 2, 3)


def rom_address(runtime_address: int) -> int:
    return COPY_SOURCE + (runtime_address - RUNTIME_BASE)


def build_variant(rom: bytes, variant: int) -> bytes:
    image = bytearray(rom)
    source = rom_address(TABLE) - ROM_BASE + variant * 8
    words = [rom[source + i : source + i + 4] for i in range(0, 32, 4)]
    for index, hole in enumerate(HOLES):
        destination = rom_address(hole) - ROM_BASE
        image[destination : destination + 8] = b"".join(
            words[index * 2 : index * 2 + 2]
        )
    return bytes(image)


def render_config(patched_sha1: str, variant: int, image_sha1: str) -> str:
    source = rom_address(LOOP_START)
    size = LOOP_END - LOOP_START
    return f'''# Private generated input; do not commit.
# Writer 0x03000a20..0x03000a50 copies table 0x030009c4 + {variant} * 8.
[program]
name = "Golden Sun synthesized Func_dc8 slot 2 variant {variant}"
id = "golden_sun_usa_synth_dc8b_{variant}"
load_address = 0x08000000
size = 0x00800000
entry_pc = 0x{LOOP_START:08x}
speculative_literal_harvest = false
codegen_shards = 1

[identity]
sha1 = "{patched_sha1}"

[[code_copy]]
runtime_start = 0x{LOOP_START:08x}
source_start = 0x{source:08x}
size = 0x{size:08x}
name = "synthesized_Func_dc8_slot2_{variant}"
note = "Eight table words in four holes; complete executable SHA-1 {image_sha1}"

[[extra_func]]
addr = 0x{LOOP_START:08x}
mode = "arm"
name = "Func_dc8_slot2_synth_{variant}"
note = "Mixer loop head; backedge from 0x03000a94, falls through to 0x03000a98"

[[resume_range]]
start = 0x{LOOP_START:08x}
end = 0x{LOOP_END:08x}
mode = "arm"
note = "Whole loop; valid after the writer's pass until its next pass or an IWRAM reset"
'''


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--rom", required=True, type=Path)
    parser.add_argument("--out-dir", required=True, type=Path)
    parser.add_argument("--variant", required=True, type=int, choices=VARIANTS)
    args = parser.parse_args()

    rom = args.rom.read_bytes()
    actual_sha1 = hashlib.sha1(rom).hexdigest()
    if actual_sha1 != EXPECTED_ROM_SHA1:
        raise SystemExit(f"unsupported ROM SHA-1: {actual_sha1}")

    patched = build_variant(rom, args.variant)
    patched_sha1 = hashlib.sha1(patched).hexdigest()
    start = rom_address(LOOP_START) - ROM_BASE
    end = rom_address(LOOP_END) - ROM_BASE
    image_sha1 = hashlib.sha1(patched[start:end]).hexdigest()

    args.out_dir.mkdir(parents=True, exist_ok=True)
    rom_path = args.out_dir / "variant.gba"
    config_path = args.out_dir / "variant.toml"
    rom_path.write_bytes(patched)
    config_path.write_text(
        render_config(patched_sha1, args.variant, image_sha1), encoding="utf-8"
    )
    print(f"variant_rom={rom_path}")
    print(f"variant_config={config_path}")
    print(f"patched_sha1={patched_sha1}")
    print(f"image_sha1={image_sha1}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
