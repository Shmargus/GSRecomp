"""Write config/tla/reloc/*.toml: one position-independent image per ROM
routine that Golden Sun: The Lost Age copies into RAM and runs.

Every routine in IMAGES starts with a Thumb `bx pc` stub (78 47 00 02) and
runs in ARM; THUMB_IMAGES are plain Thumb. The size of each copy is the game's own number (evidence column):
the allocation size loaded beside the source literal, or the word count in
the DMA3 control word. Where routines sit back to back in ROM, each size ends
exactly at the next routine's stub.

The ARM body (+4 to the end) is made one resume range per run of code, so
every instruction can be entered: these routines jump into themselves with
`add pc, ...` and return to the instruction after. Words read PC-relative
(`ldr rX,[pc,#]`, `add rX,pc,#` + ldm) and words capstone cannot decode are
data ranges.

Usage: python tools/tla_reloc_configs.py   (needs config/local.json's ROM)
"""
import json
import os
import struct

import capstone

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROM_SHA1 = "b500663220cb9bf56b9f8e8c0c544f1d6fa3a824"
ORIGIN = 0x03001300  # generation origin only (label space); any RAM base works

# (ROM source, size in bytes, evidence)
IMAGES = [
    (0x08013060, 0x06C, "0x08013D96: reserves (0x6B+4)&~3 bytes below sp, DMA3s (0x6B+1)>>2 words from literal 0x08013DE0 (size literal 0x08013DDC), calls it with mov lr,sp; bl lr"),
    (0x08017CE8, 0x020, "0x0801553E..0x08015542: DMA3 from literal 0x080156E0, control 0x84000000 + 8 words, called with mov lr,sl; bl lr (ARM fixed-point multiply ending in bx lr at +0x1C)"),
    (0x08001FFC, 0x058, "0x08015830 reserves (0x57+4)&~3 bytes below sp and DMA3s 0x58 bytes (literals 0x08015874/0x08015878), calls it with mov lr,sp; bl lr"),
    (0x080128A8, 0x2C8, "0x0801587C: alloc 0x2C8 (literal 0x080158C4), DMA3 from literal 0x080158C8, call, free"),
    (0x08012B70, 0x4F0, "0x080158CC: alloc 0x4F0 (literal 0x08015914), DMA3 from literal 0x08015918, call, free"),
    (0x08001658, 0x25C, "0x0801591C: alloc 0x25C (literal 0x08015964), DMA3 from literal 0x08015968, call, free"),
    (0x080130CC, 0x080, "0x0801336C: alloc 0x80 (literal 0x080133B8), DMA3 from literal 0x080133BC, call, free"),
    (0x080018B4, 0x140, "DMA3 literal triple: 0x040000D4, source at 0x08001A0C, control 0x84000050 (0x50 words)"),
    (0x08001A14, 0x0C0, "DMA3 literal triple: 0x040000D4, source at 0x08001CA8, control 0x84000030 (0x30 words)"),
    (0x08001AD4, 0x080, "DMA3 literal triple: 0x040000D4, source at 0x08001CC8, control 0x84000020 (0x20 words)"),
    (0x08001B54, 0x0B4, "DMA3 literal triple: 0x040000D4, source at 0x08001D08, control 0x8400002D (0x2D words)"),
    (0x08001C08, 0x088, "DMA3 literal triple: 0x040000D4, source at 0x08001CE8, control 0x84000022 (0x22 words)"),
    (0x08001D10, 0x060, "DMA3 literal triple: 0x040000D4, source at 0x08001F94, control 0x84000018 (0x18 words)"),
    (0x08001D70, 0x060, "DMA3 literal triple: 0x040000D4, source at 0x08001FB4, control 0x84000018 (0x18 words)"),
    (0x08001DD0, 0x0D4, "DMA3 literal triple: 0x040000D4, source at 0x08001FF4, control 0x84000035 (0x35 words)"),
    (0x08001EA4, 0x0D8, "DMA3 literal triple: 0x040000D4, source at 0x08001FD4, control 0x84000036 (0x36 words)"),
    (0x08020DA8, 0x2C8, "size 0x2C8 two words before source literal 0x08021E08 (also 0x08023BFC/0x08023EEC); ends at 0x08021070"),
    (0x08021070, 0x268, "size literal 0x0802DD64 beside source literal 0x0802DD68 (0x0802DD08: DMA3 to 0x0201C000/0x0203C000)"),
    (0x0802146C, 0x298, "size literal 0x0802BCE4 beside source literal 0x0802BCE8"),
    (0x08021704, 0x0A0, "0x0802A5E4: alloc 0xA0, DMA3 from literal 0x0802A648 (size literal 0x0802A644); ends at 0x080217A4"),
    (0x080217A4, 0x080, "size literal 0x08022C70 beside source literal 0x08022C74; ends at 0x08021824"),
    (0x08021824, 0x0F4, "size literal 0x08021F54 beside source literal 0x08021F58"),
    (0x08038438, 0x144, "size literal 0x0803B1E0 beside source literal 0x0803B1E4 (also 0x0803CB04/08); ends at 0x0803857C"),
    (0x0803857C, 0x064, "size literal 0x0803D1BC beside source literal 0x0803D1C0; ends at 0x080385E0"),
    (0x080385E0, 0x31C, "size literal 0x0803BF7C beside source literal 0x0803BF80; ends at 0x080388FC"),
    (0x080388FC, 0x218, "size literal 0x080425F4 beside source literal 0x080425F8; ends at 0x08038B14"),
    (0x08038B14, 0x27C, "size literal 0x0803DC04 beside source literal 0x0803DC08 (also 0x080455A8/AC); ends at 0x08038D90"),
    (0x08038D90, 0x0A0, "size literal 0x0803DC0C beside source literal 0x0803DC10; ends at 0x08038E30"),
    (0x08038E30, 0x080, "size literal 0x0803DC14 beside source literal 0x0803DC18"),
    (0x080C93CC, 0x2C8, "size 0x2C8 two words before source literal 0x080E4460"),
    (0x08118160, 0x234, "size literal 0x08126520 beside source literal 0x08126524"),
    (0x08118394, 0x07C, "DMA3 literal triple: 0x040000D4, source at 0x08118428, control 0x8400001F (0x1F words)"),
    (0x081380AC, 0x3CC, "size literal 0x08196950 beside source literal 0x08196954"),
    (0x08138FC8, 0x200, "(size, source) pair at 0x08196D74; ends at 0x081391C8"),
    (0x081391C8, 0x210, "(size, source) pair at 0x08196D84; ends at 0x081393D8"),
    (0x081393D8, 0x220, "(size, source) pair at 0x08196D8C; ends at 0x081395F8"),
    (0x081395F8, 0x4B4, "(size, source) pair at 0x08196D94; ends at 0x08139AAC"),
    (0x08139AAC, 0x4C4, "(size, source) pair at 0x08196DB8; ends at 0x08139F70"),
    (0x08139F70, 0x4D4, "(size, source) pair at 0x08196DC0; ends at 0x0813A444"),
    (0x0813A444, 0x4E0, "(size, source) pair at 0x08196DC8; ends at 0x0813A924"),
    (0x0813A924, 0x4F0, "(size, source) pair at 0x08196DD0; ends at 0x0813AE14"),
    (0x0813AE14, 0x4D8, "(size, source) pair at 0x08196DA0; ends at 0x0813B2EC"),
    (0x0813B2EC, 0x4B4, "(size, source) pair at 0x08196DAC; ends at 0x0813B7A0"),
    (0x0813B7A0, 0x200, "(size, source) pair at 0x08196D7C; ends at 0x0813B9A0"),
    (0x0813B9A0, 0x0B0, "size literal 0x081969A4 beside source literal 0x081969A8"),
    (0x081A004C, 0x234, "size literal 0x081A05EC beside source literal 0x081A05F0"),
    (0x081A029C, 0x234, "size literal 0x081A17A8 beside source literal 0x081A17AC"),
]

# Areas a routine writes inside its own copy -- data, never executed. They
# are data ranges, and the runner's byte check skips them (offsets from the
# routine start). Each was read from the routine's code (FACTS.md,
# 2026-10-03); such routines are exempt from the self-modification check.
WRITABLE = {
    # cache word (`str r2,[pc,#0x7c]` at +0x158) and a 0xE0-byte lookup table
    # filled by 8 x `stm r3!,{7 regs}` from r3 = +0x1E8 (+0x164); code ends
    # with bx lr at +0x1D8; +0x1E0/+0x1E4 are this copy's table pointers.
    0x08020DA8: [(0x1DC, 0x1E0), (0x1E8, 0x2C8)],
    0x080C93CC: [(0x1DC, 0x1E0), (0x1E8, 0x2C8)],
    # self-relocation: 8 words from +0x104 plus (runtime - link) base stored
    # from r4 = +0xE4 (`add r4,pc,#0xc8` at +0x14 / `#0xc4` at +0x18, loop
    # count `mov r5,#8`).
    0x08118160: [(0x0E4, 0x104)],
    0x081A004C: [(0x0E4, 0x104)],
    0x081A029C: [(0x0E4, 0x104)],
}

# Battle-effect routines (the (size, source) table at 0x08196D74) that keep
# one word of state in their own copy: `ldr fp,[pc,#]` / `str fp,[pc,#]` at
# +0x8 / +0x10 on the literal slot, also read at +0xF0 / +0x120.
for _src, _slot in ((0x08138FC8, 0x1F4), (0x081391C8, 0x204),
                    (0x081393D8, 0x214), (0x0813B7A0, 0x1F4)):
    WRITABLE[_src] = [(_slot, _slot + 4)]

# Battle-effect rasterizers that patch their own code on every call, from
# their setup code (entry `b` to it): `ldr r3,[pc,#-]; orr r3,r3,r5,lsl #7
# [orrne r3,r3,#0x20]; str r3,[pc,#-]` into the shift fields of the words at
# +0x168, +0x170, +0x22C, +0x324, +0x328, and in three of them `strb r4,
# [pc,#-]` into the immediate of `subs sl,sl,#0` at +0x334 (FACTS.md,
# 2026-10-03). Those words are [[dynamic_operand2]]: the translation reads
# the field live. Setup then DMA3-copies n words from +0x324 to +0x324 + 4n
# (`mov r5,#n; orr r5,r5,#0x84000000; sub r3,pc,#; sub r4,pc,#; stm sb`):
# the ROM holds zeros there, so the body is translated from a copy with
# that block filled in ("# post-setup-copy", scripts/tla_recompile.ps1) and
# the block is left out of the byte check. State words: +0x8..+0x10
# (`sub r0,pc,#0xec; stm r0,{r7,r8}`) and five words from the literal slot L
# (`str fp` to L, `stm lr,{r3,r4}` to L+4, `str r5` / `str r3` to L+0xC /
# L+0x10). Entries: (source, n, L, patches +0x334).
SELF_PATCHING = {}
for _src, _n, _slot, _imm in ((0x081395F8, 7, 0x498, False),
                              (0x08139AAC, 9, 0x4A8, False),
                              (0x08139F70, 11, 0x4B8, False),
                              (0x0813A444, 11, 0x4C4, True),
                              (0x0813A924, 13, 0x4D4, True),
                              (0x0813AE14, 10, 0x4BC, True),
                              (0x0813B2EC, 7, 0x498, False)):
    _dynamic = [0x168, 0x170, 0x22C, 0x324, 0x328] + ([0x334] if _imm else [])
    _dst = 0x324 + 4 * _n
    _dynamic += [_dst + (w - 0x324) for w in _dynamic if 0x324 <= w < _dst]
    SELF_PATCHING[_src] = {"copy": (0x324, _dst, _n), "dynamic": sorted(_dynamic)}
    WRITABLE[_src] = [(0x8, 0x10), (_slot, _slot + 0x14)]

# Read-only data inside a routine that does not decode as data on its own
# (compared by the byte check, but never code).
READONLY = {
    # table pointers after the code's final bx lr (+0x1D8)
    0x08020DA8: [(0x1E0, 0x1E8)],
    0x080C93CC: [(0x1E0, 0x1E8)],
}

# Plain Thumb routines (no `bx pc` stub), copied onto the stack by the flash
# save code.
THUMB_IMAGES = [
    (0x08017118, 0x024, "0x08017160: halfword copy from 0x08017118 up to 0x0801713C (literals 0x08017174 = 0x08017119, 0x08017178 = 0x0801713D) to sp, called at sp+1 through 0x08017884"),
    (0x08017050, 0x004, "0x08017054: halfword copy from 0x08017050 up to 0x08017054 (literals 0x08017070 = 0x08017051, 0x08017074 = 0x08017055) to the buffer in r0; `ldrb r0,[r0]; bx lr`"),
]


def name_of(src):
    return "reloc_%x" % (src - 0x08000000)


def self_modifying(rom, src, size):
    """True if the routine stores PC-relative, i.e. patches its own code
    (e.g. 0x08139A08 `ldr r3,[pc,#-0xf0]` / `str r3,[pc,#-0xf8]`). A
    translation of the original bytes would go stale inside one call, so
    such routines are left to the interpreter."""
    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
    pc_regs = set()  # registers last set to a PC-relative address
    for off in range(4, size, 4):
        word = rom[src - 0x08000000 + off:src - 0x08000000 + off + 4]
        ins = next(md.disasm(word, 0), None)
        if ins is None:
            continue
        ops = [o.strip() for o in ins.op_str.split(",")]
        if ins.mnemonic.startswith(("str", "stm")):
            base = ins.op_str.split("[")[1].split(",")[0].rstrip("]!")                 if "[" in ins.op_str else ops[0].rstrip("!")
            if base == "pc" or base in pc_regs:
                return True
        if ins.mnemonic in ("add", "sub") and len(ops) == 3 and ops[1] == "pc":
            pc_regs.add(ops[0])
        elif ops and ops[0] in pc_regs:
            pc_regs.discard(ops[0])
    return False


def classify(rom, src, size):
    """Return a list of (start_off, end_off, is_code) runs over [4, size)."""
    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
    data = set()
    for start, end in WRITABLE.get(src, []) + READONLY.get(src, []):
        data.update(range(start, end, 4))
    regs = {}
    for off in range(4, size, 4):
        word = rom[src - 0x08000000 + off:src - 0x08000000 + off + 4]
        ins = next(md.disasm(word, ORIGIN + off), None)
        if ins is None:
            data.add(off)
            continue
        ops = ins.op_str
        if ins.mnemonic.startswith("ldr") and "[pc, #" in ops:
            data.add(off + 8 + int(ops.split("#")[1].rstrip("]!"), 0))
        elif ins.mnemonic == "add" and ", pc, #" in ops:
            regs[ops.split(",")[0]] = off + 8 + int(ops.split("#")[1], 0)
        elif ins.mnemonic.startswith("ldm"):
            base = ops.split(",")[0].rstrip("!")
            if base in regs:
                for k in range(ops.count(",")):
                    data.add(regs[base] + 4 * k)
    # Instructions a self-patching routine reads PC-relative to patch them.
    data -= set(SELF_PATCHING.get(src, {}).get("dynamic", []))
    runs = []
    for off in range(4, size, 4):
        code = off not in data
        if runs and runs[-1][2] == code and runs[-1][1] == off:
            runs[-1][1] = off + 4
        else:
            runs.append([off, off + 4, code])
    return runs


def classify_thumb(rom, src, size):
    """Runs over [0, size) for a plain Thumb routine; literal-pool words read
    with `ldr rX,[pc,#]` are data."""
    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB)
    data = set()
    for off in range(0, size, 2):
        hw = rom[src - 0x08000000 + off:src - 0x08000000 + off + 2]
        if (hw[1] >> 3) == 0b01001:  # ldr rX,[pc,#imm8*4]
            lit = ((off + 4) & ~3) + hw[0] * 4
            data.update((lit, lit + 2))
    runs = []
    for off in range(0, size, 2):
        code = off not in data
        if runs and runs[-1][2] == code and runs[-1][1] == off:
            runs[-1][1] = off + 2
        else:
            runs.append([off, off + 2, code])
    return runs


def main():
    cfg = json.load(open(os.path.join(ROOT, "config", "local.json")))
    rom = open(cfg["rom"], "rb").read()
    out_dir = os.path.join(ROOT, "config", "tla", "reloc")
    os.makedirs(out_dir, exist_ok=True)
    for old in os.listdir(out_dir):
        if old.endswith(".toml"):
            os.remove(os.path.join(out_dir, old))
    written = 0
    jobs = [(s, n, e, "arm") for s, n, e in IMAGES]
    jobs += [(s, n, e, "thumb") for s, n, e in THUMB_IMAGES]
    for src, size, evidence, body in jobs:
        off = src - 0x08000000
        assert body == "thumb" or rom[off:off + 4] == bytes.fromhex("78470002"), hex(src)
        if body == "arm" and src not in WRITABLE and self_modifying(rom, src, size):
            print("skip 0x%08X: stores PC-relative (patches itself)" % src)
            continue
        written += 1
        name = name_of(src)
        lines = [
            "# Golden Sun: The Lost Age -- position-independent RAM image of ROM 0x%08X." % src,
            "# GENERATED by tools/tla_reloc_configs.py -- edit the list there.",
            "#",
            "# Copy: %s." % evidence,
            "# Starts with a Thumb `bx pc` stub; the body is ARM." if body == "arm"
            else "# Plain Thumb routine.",
            "",
            "[program]",
            'name = "Golden Sun: The Lost Age relocatable image 0x%08X"' % src,
            'id = "golden_sun_tla_%s"' % name,
            "load_address = 0x08000000",
            "size = 0x01000000",
            "entry_pc = 0x%08X" % ORIGIN,
            "speculative_literal_harvest = false",
            "codegen_shards = 1",
            "",
            "[identity]",
            'sha1 = "%s"' % ROM_SHA1,
            "",
            "[[code_copy]]",
            "runtime_start = 0x%08X" % ORIGIN,
            "source_start = 0x%08X" % src,
            "size = 0x%08X" % size,
            'name = "%s"' % name,
            'note = "copied by the game into RAM; see header"',
            "",
        ]
        skips = list(WRITABLE.get(src, []))
        plan = SELF_PATCHING.get(src)
        if plan:
            copy_src, copy_dst, words = plan["copy"]
            skips.append((copy_dst, copy_dst + 4 * words))
            skips += [(w, w + 4) for w in plan["dynamic"]]
            lines.append("# post-setup-copy 0x%X:0x%X:0x%X" % (copy_src, copy_dst, words))
        merged = []
        for start, end in sorted(skips):
            if merged and start <= merged[-1][1]:
                merged[-1][1] = max(merged[-1][1], end)
            else:
                merged.append([start, end])
        for start, end in merged:
            lines.append("# identity-skip 0x%X:0x%X" % (start, end))
        lines += [
            "",
            "[[extra_func]]",
            "addr = 0x%08X" % ORIGIN,
            'mode = "thumb"',
            'name = "%s_entry"' % name,
            'note = "%s"' % ("bx pc stub at offset 0" if body == "arm" else "routine entry"),
            "",
        ]
        for w in (plan["dynamic"] if plan else []):
            lines += ["[[dynamic_operand2]]",
                      "addr = 0x%08X" % (ORIGIN + w),
                      'note = "shift/immediate field patched by the setup code, ROM 0x%08X"' % (src + w),
                      ""]
        runs = classify(rom, src, size) if body == "arm" else classify_thumb(rom, src, size)
        for start, end, code in runs:
            if code and body == "thumb" and start == 0:
                start = 2  # the entry itself is the [[extra_func]] above
                if start >= end:
                    continue
            if code:
                lines += ["[[resume_range]]",
                          "start = 0x%08X" % (ORIGIN + start),
                          "end = 0x%08X" % (ORIGIN + end),
                          'mode = "%s"' % body,
                          'note = "%s code, ROM 0x%08X..0x%08X"' % (body.upper(), src + start, src + end),
                          ""]
            else:
                lines += ["[[data_range]]",
                          "start = 0x%08X" % (ORIGIN + start),
                          "end = 0x%08X" % (ORIGIN + end),
                          'note = "read PC-relative or not ARM-decodable, ROM 0x%08X..0x%08X"' % (src + start, src + end),
                          ""]
        open(os.path.join(out_dir, name + ".toml"), "w", newline="\n").write("\n".join(lines))
    print("wrote %d image configs" % written)


if __name__ == "__main__":
    main()
