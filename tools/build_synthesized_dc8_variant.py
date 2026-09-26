#!/usr/bin/env python3
"""Build a private ROM/config pair for Func_dc8's synthesized ARM image."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path


EXPECTED_ROM_SHA1 = "5c4695205413df7db52b9a184815a07783999971"
ROM_BASE = 0x08000000
RUNTIME_BASE = 0x03000000
COPY_SOURCE = 0x08000770
FUNCTION_START = 0x03000820
ENTRY = 0x03000828
END = 0x030008CC
TEMPLATES = (0x030008D4, 0x030008EC, 0x03000904)


def rom_address(runtime_address: int) -> int:
    return COPY_SOURCE + (runtime_address - RUNTIME_BASE)


def build_variant(rom: bytes, template: int) -> bytes:
    image = bytearray(rom)
    source = rom_address(template) - ROM_BASE
    words = [rom[source + i : source + i + 4] for i in range(0, 24, 4)]
    if any(len(word) != 4 for word in words):
        raise ValueError("template falls outside ROM")

    destination = rom_address(ENTRY) - ROM_BASE
    for iteration in range(4):
        base = destination + iteration * 0x24
        image[base : base + 8] = b"".join(words[:2])
        image[base + 0x0C : base + 0x1C] = b"".join(words[2:])
    return bytes(image)


def render_config(patched_sha1: str, template: int, image_sha1: str) -> str:
    return f'''# Private generated input; do not commit.
# Ring event #3295783 proves ARM writer 0x03000800 and template 0x{template:08x}.
[program]
name = "Golden Sun synthesized Func_dc8 variant 0x{template:08x}"
id = "golden_sun_usa_synth_dc8_{template:08x}"
load_address = 0x08000000
size = 0x00800000
entry_pc = 0x03000820
speculative_literal_harvest = false
codegen_shards = 1

[identity]
sha1 = "{patched_sha1}"

[[code_copy]]
runtime_start = 0x03000820
source_start = 0x08000f90
size = 0x000000ac
name = "synthesized_Func_dc8_0x{template:08x}"
note = "24-byte template repeated into four code holes; complete executable SHA-1 {image_sha1}"

[[extra_func]]
addr = 0x03000820
mode = "arm"
name = "Func_dc8_synth_0x{template:08x}"
note = "ARM loop header immediately preceding the synthesized span; backedge from 0x030008c4"

[[resume_range]]
start = 0x03000820
end = 0x030008cc
mode = "arm"
note = "Whole loop including its 0x03000820 header; valid after writer completes until its next pass or IWRAM reset"
'''


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--rom", required=True, type=Path)
    parser.add_argument("--out-dir", required=True, type=Path)
    parser.add_argument(
        "--template", required=True, type=lambda value: int(value, 0)
    )
    args = parser.parse_args()

    if args.template not in TEMPLATES:
        parser.error("template must be 0x030008d4, 0x030008ec, or 0x03000904")

    rom = args.rom.read_bytes()
    actual_sha1 = hashlib.sha1(rom).hexdigest()
    if actual_sha1 != EXPECTED_ROM_SHA1:
        raise SystemExit(f"unsupported ROM SHA-1: {actual_sha1}")

    patched = build_variant(rom, args.template)
    patched_sha1 = hashlib.sha1(patched).hexdigest()
    start = rom_address(ENTRY) - ROM_BASE
    end = rom_address(END) - ROM_BASE
    image_sha1 = hashlib.sha1(patched[start:end]).hexdigest()

    args.out_dir.mkdir(parents=True, exist_ok=True)
    rom_path = args.out_dir / "variant.gba"
    config_path = args.out_dir / "variant.toml"
    rom_path.write_bytes(patched)
    config_path.write_text(
        render_config(patched_sha1, args.template, image_sha1), encoding="utf-8"
    )
    print(f"variant_rom={rom_path}")
    print(f"variant_config={config_path}")
    print(f"patched_sha1={patched_sha1}")
    print(f"image_sha1={image_sha1}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
