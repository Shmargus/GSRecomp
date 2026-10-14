#!/usr/bin/env python3
"""Build a private ROM/config pair for Func_dc8's third synthesized slot.

0x080037D4(r0) DMA-copies 0x98 bytes (38 words) from the ROM table
0x08000404 + r0 * 0x98 (r0 clamped to 0..4) over 0x03000BD8..0x03000C70 in
the boot IWRAM copy. Each block computes r1..r4 and falls through or
branches to 0x03000C70; the boot copy holds block 2 (FACTS.md, 2026-09-23).
"""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path


EXPECTED_ROM_SHA1 = "5c4695205413df7db52b9a184815a07783999971"
ROM_BASE = 0x08000000
RUNTIME_BASE = 0x03000000
COPY_SOURCE = 0x08000770
SLOT_START = 0x03000BD8
SLOT_END = 0x03000C70
TABLE = 0x08000404
VARIANTS = (0, 1, 2, 3, 4)


def rom_address(runtime_address: int) -> int:
    return COPY_SOURCE + (runtime_address - RUNTIME_BASE)


def block(rom: bytes, variant: int) -> bytes:
    start = TABLE - ROM_BASE + variant * (SLOT_END - SLOT_START)
    return rom[start : start + (SLOT_END - SLOT_START)]


def build_variant(rom: bytes, variant: int) -> bytes:
    image = bytearray(rom)
    destination = rom_address(SLOT_START) - ROM_BASE
    image[destination : destination + (SLOT_END - SLOT_START)] = block(rom, variant)
    return bytes(image)


def render_config(patched_sha1: str, variant: int, image_sha1: str) -> str:
    return f'''# Private generated input; do not commit.
# 0x080037d4 copies table 0x08000404 + {variant} * 0x98 over 0x03000bd8.
[program]
name = "Golden Sun synthesized Func_dc8 slot 3 variant {variant}"
id = "golden_sun_usa_synth_dc8c_{variant}"
load_address = 0x08000000
size = 0x00800000
entry_pc = 0x{SLOT_START:08x}
speculative_literal_harvest = false
codegen_shards = 1

[identity]
sha1 = "{patched_sha1}"

[[code_copy]]
runtime_start = 0x{SLOT_START:08x}
source_start = 0x{rom_address(SLOT_START):08x}
size = 0x{SLOT_END - SLOT_START:08x}
name = "synthesized_Func_dc8_slot3_{variant}"
note = "ROM block {variant} of table 0x08000404; complete executable SHA-1 {image_sha1}"

[[extra_func]]
addr = 0x{SLOT_START:08x}
mode = "arm"
name = "Func_dc8_slot3_synth_{variant}"
note = "Entered by fall-through from 0x03000bd4; leaves to 0x03000c70"

[[resume_range]]
start = 0x{SLOT_START:08x}
end = 0x{SLOT_END:08x}
mode = "arm"
note = "Whole block; valid until the next 0x080037d4 copy or an IWRAM reset"
'''


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--rom", required=True, type=Path)
    parser.add_argument("--out-dir", required=True, type=Path)
    parser.add_argument("--variant", required=True, type=int, choices=VARIANTS)
    args = parser.parse_args()

    rom = args.rom.read_bytes()
    if hashlib.sha1(rom).hexdigest() != EXPECTED_ROM_SHA1:
        raise SystemExit("unsupported ROM SHA-1")

    patched = build_variant(rom, args.variant)
    start = rom_address(SLOT_START) - ROM_BASE
    image_sha1 = hashlib.sha1(patched[start : start + SLOT_END - SLOT_START]).hexdigest()
    args.out_dir.mkdir(parents=True, exist_ok=True)
    (args.out_dir / "variant.gba").write_bytes(patched)
    (args.out_dir / "variant.toml").write_text(
        render_config(hashlib.sha1(patched).hexdigest(), args.variant, image_sha1),
        encoding="utf-8",
    )
    print(f"image_sha1={image_sha1}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
