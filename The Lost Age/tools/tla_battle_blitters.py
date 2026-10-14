"""Build every battle-effect blitter the game can assemble, for gba_recompile.

Battle and Psynergy effects draw through small ARM routines the game builds
in RAM from template pieces (FACTS.md, 2026-10-03): the builder 0x08196404
(r0 allocation id, r1 and r2 size exponents, r3 flags, [sp] mode; wrapper
0x081963EC passes r1 = r2 = 7, flags = arg & 0xF, mode = arg >> 4) allocates
0x1B4 bytes (0x08014CC0), DMA3-copies pieces from 0x081972A0.. into it,
patches branch offsets and shift amounts, and returns 1. The result is
self-contained (no calls, no literal pools, relative branches only), so it
runs the same at any address.

This runs the builder itself (unicorn) for r1, r2 in 6..8 (values seen at
its call sites), flags 0..15 and mode 0..3 (4 and up build what 3 builds),
keeps each distinct result, and packs them GROUP to an image at a fixed
stride: the runner matches a live routine by its exact bytes at any base.

For each group this writes, under --out/<name>/:
  variant.toml  gba_recompile config (relocatable image at ORIGIN)
  expected.cpp  the packed bytes and each routine's size
and --out/blitters.gba, the ROM with every group's bytes at SOURCE (only
these translations read it). All output is ROM-derived (local/, gitignored).
Prints "<name> 0x<ORIGIN>:<SIZE>" per group.

Usage: python tools/tla_battle_blitters.py --rom <rom> --out local/tla/blitters
"""
import argparse
import hashlib
import os
import struct

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
                               UC_ARM_REG_R3, UC_ARM_REG_SP, UC_ARM_REG_LR,
                               UC_ARM_REG_PC)

ROM_SHA1 = "b500663220cb9bf56b9f8e8c0c544f1d6fa3a824"
BUILDER = 0x08196404
ALLOC = 0x08014CC0
ALLOC_SIZE = 0x1B4
DMA3_CNT = 0x040000DC
BUFFER = 0x03005000          # where the emulated builder writes
STOP = 0x08000100            # return address of the emulated call
ORIGIN = 0x02000000          # translation origin of every group
SOURCE = 0x08F00000          # where blitters.gba holds the packed groups
STRIDE = 0x1C0               # >= ALLOC_SIZE, word aligned
GROUP = 32


def build(rom, r1, r2, flags, mode):
    mu = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
    mu.mem_map(0x08000000, 0x01000000)
    mu.mem_write(0x08000000, rom)
    mu.mem_map(0x02000000, 0x40000)
    mu.mem_map(0x03000000, 0x8000)
    mu.mem_map(0x04000000, 0x1000)
    written = set()
    dma_pending = []

    def dma():
        src, dst, cnt = struct.unpack("<III", mu.mem_read(0x040000D4, 12))
        unit = 4 if cnt & (1 << 26) else 2
        n = (cnt & 0xFFFF) * unit
        mu.mem_write(dst, bytes(mu.mem_read(src, n)))
        written.update(range(dst, dst + n))

    def on_write(uc, access, addr, size, value, data):
        if addr <= DMA3_CNT < addr + size:
            dma_pending.append(1)
        if BUFFER <= addr < BUFFER + 0x400:
            written.update(range(addr, addr + size))

    def on_code(uc, addr, size, data):
        if dma_pending:  # the copy lands before the next instruction
            dma_pending.clear()
            dma()
        if addr == ALLOC:
            uc.reg_write(UC_ARM_REG_R0, BUFFER)
            uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))

    mu.hook_add(UC_HOOK_MEM_WRITE, on_write)
    mu.hook_add(UC_HOOK_CODE, on_code)
    sp = 0x03007E00
    mu.mem_write(sp, struct.pack("<I", mode))
    for reg, val in ((UC_ARM_REG_SP, sp), (UC_ARM_REG_R0, 0x68),
                     (UC_ARM_REG_R1, r1), (UC_ARM_REG_R2, r2),
                     (UC_ARM_REG_R3, flags), (UC_ARM_REG_LR, STOP | 1)):
        mu.reg_write(reg, val)
    mu.emu_start(BUILDER | 1, STOP, count=200000)
    if dma_pending:
        dma()
    if mu.reg_read(UC_ARM_REG_R0) != 1:
        raise SystemExit("builder failed for %r" % ((r1, r2, flags, mode),))
    inside = [a for a in written if BUFFER <= a < BUFFER + ALLOC_SIZE]
    end = max(inside) + 1
    if len(set(range(BUFFER, end)) - written):
        raise SystemExit("gap in builder output for %r" % ((r1, r2, flags, mode),))
    return bytes(mu.mem_read(BUFFER, end - BUFFER))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rom", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    rom = open(args.rom, "rb").read()
    if hashlib.sha1(rom).hexdigest() != ROM_SHA1:
        raise SystemExit("unexpected ROM")

    routines = {}
    for r1 in (6, 7, 8):
        for r2 in (6, 7, 8):
            for flags in range(16):
                for mode in range(4):
                    code = build(rom, r1, r2, flags, mode)
                    routines.setdefault(code, (r1, r2, flags, mode))
    routines = list(routines.items())

    img = bytearray(rom)
    groups = [routines[i:i + GROUP] for i in range(0, len(routines), GROUP)]
    os.makedirs(args.out, exist_ok=True)
    for g, group in enumerate(groups):
        name = "blit%d" % g
        src = SOURCE + g * GROUP * STRIDE
        packed = bytearray(len(group) * STRIDE)
        for v, (code, _) in enumerate(group):
            packed[v * STRIDE:v * STRIDE + len(code)] = code
        o = src - 0x08000000
        img[o:o + len(packed)] = packed
        d = os.path.join(args.out, name)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "expected.cpp"), "w", newline="\n") as f:
            f.write("// GENERATED by tools/tla_battle_blitters.py -- ROM-derived.\n")
            f.write("#include <cstdint>\n")
            f.write('extern "C" const uint8_t %s_kExpected[] = {\n' % name)
            for i in range(0, len(packed), 16):
                f.write("    " + ", ".join("0x%02X" % b for b in packed[i:i + 16]) + ",\n")
            f.write("};\n")
            f.write('extern "C" const uint32_t %s_kSizes[] = {\n' % name)
            for code, params in group:
                f.write("    0x%03Xu,  // r1=%d r2=%d flags=%d mode=%d\n"
                        % ((len(code),) + params))
            f.write("};\n")
            f.write('extern "C" const unsigned %s_kCount = %du;\n' % (name, len(group)))
            f.write('extern "C" const uint32_t %s_kStride = 0x%Xu;\n' % (name, STRIDE))
        groups[g] = (name, src, group)
    sha = hashlib.sha1(img).hexdigest()
    open(os.path.join(args.out, "blitters.gba"), "wb").write(img)

    for name, src, group in groups:
        size = len(group) * STRIDE
        entries = []
        for v, (code, params) in enumerate(group):
            start = ORIGIN + v * STRIDE
            entries.append('''
[[extra_func]]
addr = 0x%08X
mode = "thumb"
name = "%s_v%d"
note = "bx pc stub; r1=%d r2=%d flags=%d mode=%d"

[[resume_range]]
start = 0x%08X
end = 0x%08X
mode = "arm"
note = "routine body, every instruction an entry"
''' % ((start, name, v) + params + (start + 4, start + len(code))))
        with open(os.path.join(args.out, name, "variant.toml"), "w", newline="\n") as f:
            f.write('''# GENERATED by tools/tla_battle_blitters.py -- do not edit or commit.
# Battle-effect blitters built by 0x08196404, %d routines at stride 0x%X.
[program]
name = "Golden Sun: The Lost Age battle blitters %s"
id = "golden_sun_tla_%s"
load_address = 0x08000000
size = 0x01000000
entry_pc = 0x%08X
speculative_literal_harvest = false
codegen_shards = 1

[identity]
sha1 = "%s"

[[code_copy]]
runtime_start = 0x%08X
source_start = 0x%08X
size = 0x%08X
name = "%s"
note = "packed builder outputs"
''' % (len(group), STRIDE, name, name, ORIGIN, sha, ORIGIN, src, size, name))
            f.write("".join(entries))
        print("%s 0x%08X:%X" % (name, ORIGIN, size))


if __name__ == "__main__":
    main()
