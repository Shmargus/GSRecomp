#!/usr/bin/env python3
"""Run Golden Sun's particle stamp builder (Func_ed408) under Unicorn.

Func_ed408 (ROM 0x080ED408, THUMB) assembles an ARM routine into a heap block
from template words at 0x080EDC88.., DMA-copying runs and patching fields.
Its output depends on r1 and r2 (log2 canvas sizes), r3 (4 flag bits) and the
stack argument (mode 0..3). This runs the ROM's own code, so the bytes it
returns are the game's, not a re-implementation.

Stand-ins, both measured (FACTS.md, 2026-09-23):
- Func_48b0 (0x080048B0, the slot allocator) returns --dest.
- DMA3 (0x040000D4 source, 0x040000D8 destination, 0x040000DC count and
  control) copies at once when the enable bit is written.

Usage:
  emulate_stamp_builder.py --rom ROM --r1 7 --r2 7 --r3 3 --mode 2
      [--dest 0x03006000] [--r0 SLOT] [--out FILE]
"""

from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path

from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn.arm_const import (
    UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
    UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_CPSR,
)

EXPECTED_ROM_SHA1 = "5c4695205413df7db52b9a184815a07783999971"
BUILDER = 0x080ED408
ALLOCATOR = 0x080048B0
RETURN_TRAP = 0x08FFFFF0
DMA3 = 0x040000D4


def run(rom: bytes, r0: int, r1: int, r2: int, r3: int, mode: int,
        dest: int) -> bytes:
    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
    uc.mem_map(0x02000000, 0x40000)
    uc.mem_map(0x03000000, 0x8000)
    uc.mem_map(0x04000000, 0x1000)
    uc.mem_map(0x08000000, 0x01000000)
    uc.mem_write(0x08000000, rom)
    # A return trap: a THUMB `b .` the run stops on.
    uc.mem_write(RETURN_TRAP, b"\xfe\xe7")

    written = {"lo": None, "hi": None}

    def on_code(uc, address, size, _):
        if address == ALLOCATOR:
            uc.reg_write(UC_ARM_REG_R0, dest)
            lr = uc.reg_read(UC_ARM_REG_LR)
            uc.reg_write(UC_ARM_REG_PC, lr | 1)
        elif address == RETURN_TRAP:
            uc.emu_stop()

    def on_write(uc, access, address, size, value, _):
        if address == DMA3 + 8 and size >= 4 or address == DMA3 + 10:
            if address == DMA3 + 8:
                count = value & 0xFFFF
                control = (value >> 16) & 0xFFFF
            else:
                count = struct.unpack("<H", uc.mem_read(DMA3 + 8, 2))[0]
                control = value & 0xFFFF
            if not control & 0x8000:
                return
            src, dst = struct.unpack("<II", uc.mem_read(DMA3, 8))
            unit = 4 if control & 0x0400 else 2
            count = count or (0x10000 if dst >> 24 else 0x4000)
            data = uc.mem_read(src, count * unit)
            uc.mem_write(dst, bytes(data))
        if 0x03000000 <= address < 0x03008000:
            lo = written["lo"]
            written["lo"] = address if lo is None else min(lo, address)
            hi = written["hi"]
            written["hi"] = address + size if hi is None else max(hi, address + size)

    uc.hook_add(UC_HOOK_CODE, on_code)
    uc.hook_add(UC_HOOK_MEM_WRITE, on_write)

    # The fifth argument is the caller's [sp]; after the builder's pushes and
    # frame it reads it as [sp, #0x30].
    sp = 0x03007E00
    uc.reg_write(UC_ARM_REG_SP, sp)
    uc.mem_write(sp, struct.pack("<I", mode))
    uc.reg_write(UC_ARM_REG_R0, r0)
    uc.reg_write(UC_ARM_REG_R1, r1)
    uc.reg_write(UC_ARM_REG_R2, r2)
    uc.reg_write(UC_ARM_REG_R3, r3)
    uc.reg_write(UC_ARM_REG_LR, RETURN_TRAP | 1)
    try:
        uc.emu_start(BUILDER | 1, 0, count=2_000_000)
    except UcError as error:
        raise SystemExit(f"emulation stopped at 0x{uc.reg_read(UC_ARM_REG_PC):08x}: {error}")
    return bytes(uc.mem_read(0x03000000, 0x8000))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--rom", required=True, type=Path)
    for name in ("r0", "r1", "r2", "r3", "mode"):
        parser.add_argument(f"--{name}", type=lambda v: int(v, 0), default=0)
    parser.add_argument("--dest", type=lambda v: int(v, 0), default=0x03006000)
    parser.add_argument("--length", type=lambda v: int(v, 0), default=0x500)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    rom = args.rom.read_bytes()
    if hashlib.sha1(rom).hexdigest() != EXPECTED_ROM_SHA1:
        raise SystemExit("unsupported ROM")
    iwram = run(rom, args.r0, args.r1, args.r2, args.r3, args.mode, args.dest)
    image = iwram[args.dest - 0x03000000: args.dest - 0x03000000 + args.length]
    if args.out:
        args.out.write_bytes(image)
    print(hashlib.sha1(image).hexdigest())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
