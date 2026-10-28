"""Turn the RAM code the game ran into static translations.

Golden Sun 1's way (its docs/features/STATIC_RAM_CODE.md): code the static
build did not know is compiled at runtime by the engine's self-heal tier,
and the engine keeps every such piece in its cache with the exact bytes it
compiled (recomp_cache/<rom sha1>/<compiler>/<host>/*.pic: "GBAPIC5\\0",
then schema, ABI, pc, end, crc, thumb, length, and the bytes). This reads
those pieces, joins the ones that overlap or touch with the same bytes into
clusters, and packs the clusters into families like the battle blitters
(tools/tla_battle_blitters.py): the runner matches a live routine by its
exact bytes at any base (src/tla_ram_images.cpp), so a piece seen at one
address is also found where the game puts the same code next time.

A cluster keeps its address mod 4 inside its slot (PC-relative literal
loads round the PC down to a word), with a per-routine start offset so the
padding is never compared.

For each family this writes, under --out/<name>/:
  variant.toml  gba_recompile config (relocatable image at ORIGIN)
  expected.cpp  the packed bytes, each routine's size and start offset
and --out/harvest.gba, the ROM with every family's bytes at its SOURCE
(only these translations read it). All output is game-derived and belongs
under local/ (gitignored). Prints "<name> 0x<ORIGIN>:<SIZE>" per family.

Usage: python tools/tla_harvest.py --rom <rom> --cache recomp_cache --out local/tla/harvest
"""
import argparse
import glob
import hashlib
import os
import struct

ROM_SHA1 = "b500663220cb9bf56b9f8e8c0c544f1d6fa3a824"
MAGIC = b"GBAPIC5\0"
ORIGIN = 0x02000000          # translation origin of every family
SOURCE = 0x08C00000          # where harvest.gba holds the packed families
# Translation-only image: blitters.gba is separate, and TLA_HARVEST runtime
# matching uses expected bytes / RAM origin, never these SOURCE addresses
# (tla_recompile.py:image; tla_ram_images.cpp:Family/build_family_index).
# 2026-10-17: 13,681 pieces pack into 6,566 routines / 4,190,336 bytes,
# ending at 0x08FFF080, beyond the old 0x08F00000 blitter boundary.
SOURCE_END = 0x0A000000      # exclusive end of the 32 MiB GBA ROM window
STRIDES = (0x40, 0x100, 0x400, 0x1000)
GROUP = 64
# 2026-10-17: harv73's 125,240,222-byte block unit exhausted build memory.
# Existing 32-way function sharding makes the largest large-stride unit
# 5,261,927 bytes; three large units compiled at a 610,156,544-byte job peak.
# Smaller-stride families stay whole; packing and identities do not change.

# F12 tla_rewind_0001/0002: the live Thumb function at 0x0200B6A8
# compares world-object descriptor x/z with the camera focus box.
WORLD_DESCRIPTOR_BOUNDS = (
    (0x0200B768, 0xDC00, "descriptor x above the lower camera focus bound; taken retains descriptor"),
    (0x0200B770, 0xDB00, "descriptor x below the upper camera focus bound; taken retains descriptor"),
    (0x0200B778, 0xDC00, "descriptor z above the lower camera focus bound; taken retains descriptor"),
    (0x0200B780, 0xDB00, "descriptor z below the upper camera focus bound; taken retains descriptor"),
)

# F12 0103/0104: measured pad emitter at 0x0200B0F4; full coordinates
# alone cannot draw sections rejected by these original visibility tests.
JUMP_PAD_BOUNDS = (
    (0x0200B1BE, 0xD845, "pad x + 15 above 270; taken skips cap and stem"),
    (0x0200B1C6, 0xD818, "pad cap y + 47 above 238; taken skips cap"),
    (0x0200B208, 0xD820, "pad stem y + 47 above 238; taken skips stem"),
)

SPARKLE_BOUNDS = (
    (0x0200B5AC, 0xD825, "sparkle x + 15 above 270; taken skips sprite"),
    (0x0200B5B4, 0xD821, "sparkle y + 47 above 238; taken skips sprite"),
)

# F12 tla_rewind_0144/0145, 0147, 0148: the pillar-top routine, loaded at
# 0x0200925C (map 255), 0x0200B77C (map 263) and 0x02009A78 (map 264).
PILLAR_TOP_SITES = (
    (0xB6, 0xD85E, "pillar top x + 7 above 334; taken skips piece"),
    (0xBE, 0xDD5A, "pillar top row -16 or less; taken skips piece"),
    (0xC2, 0xDC58, "pillar top row above 239; taken skips piece"),
    (0xF4, 0xD83F, "tall pillar top x + 0x17 above 350; taken skips piece"),
    (0xFC, 0xDD3B, "tall pillar top row -64 or less; taken skips piece"),
    (0x100, 0xDC39, "tall pillar top row above 175; taken skips piece"),
)
PILLAR_TOP_BOUNDS = tuple((start + offset, opcode, bound)
                          for start in (0x0200925C, 0x0200B77C, 0x02009A78)
                          for offset, opcode, bound in PILLAR_TOP_SITES)
# F12 tla_rewind_0173 (map 292): the emitter timer, loaded at 0x0200A42C
# there and at 0x0200A0EC (self-heal cache, identical test section).
EMITTER_SITES = (
    (0xD8, 0xD840, "emitter x + 7 above 334; taken skips its timer"),
    (0xE0, 0xDD3C, "emitter row -48 or less; taken skips its timer"),
    (0xE4, 0xDC3A, "emitter row above 239; taken skips its timer"),
)
EMITTER_BOUNDS = tuple((start + offset, opcode, bound)
                       for start in (0x0200A42C, 0x0200A0EC)
                       for offset, opcode, bound in EMITTER_SITES)


def read_pieces(cache_root, rom_sha1):
    pieces = set()
    for path in glob.glob(os.path.join(cache_root, rom_sha1, "**", "*.pic"),
                          recursive=True):
        data = open(path, "rb").read()
        if len(data) < 36 or data[:8] != MAGIC:
            continue
        _schema, _abi, pc, end, _crc, thumb, length = struct.unpack_from("<7I", data, 8)
        code = data[36:36 + length]
        if len(code) != length or end - pc != length or length == 0:
            continue
        if not (0x02000000 <= pc < 0x02040000 or 0x03000000 <= pc < 0x03008000):
            continue
        pieces.add((pc, bool(thumb), code))
    return sorted(pieces)


def clusters_of(pieces):
    """Overlapping or touching pieces with the same bytes, by address.

    A piece joins EVERY cluster it agrees with, not only the first: the
    game loads different code at the same addresses, and the world-map
    loader's bound checks (0x0200B752..0x0200B784) once went only into an
    older routine at 0x0200B69C that differs from the live one in 174 bytes,
    so the live loader matched no harvested routine there and its pieces ran
    unhooked (F12 tla_rewind_0004-0007, 2026-10-13)."""
    clusters = []
    for pc, thumb, code in pieces:
        end = pc + len(code)
        homes = []
        for c in clusters:
            if pc > c["end"] or end < c["start"]:
                continue
            if all(c["bytes"].get(pc + i, b) == b for i, b in enumerate(code)):
                homes.append(c)
        if not homes:
            homes = [{"start": pc, "end": end, "bytes": {}, "entries": set()}]
            clusters.append(homes[0])
        for home in homes:
            for i, b in enumerate(code):
                home["bytes"][pc + i] = b
            home["start"] = min(home["start"], pc)
            home["end"] = max(home["end"], end)
            home["entries"].add((pc, thumb))
    # Joining can bridge two clusters; the bytes are contiguous by
    # construction only if every address in [start, end) is known.
    out = []
    for c in clusters:
        if all(a in c["bytes"] for a in range(c["start"], c["end"])):
            out.append(c)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rom", required=True)
    ap.add_argument("--cache", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    rom = open(args.rom, "rb").read()
    if hashlib.sha1(rom).hexdigest() != ROM_SHA1:
        raise SystemExit("unexpected ROM")

    clusters = clusters_of(read_pieces(args.cache, ROM_SHA1))
    routines = []  # (pad, bytes, entries relative to the slot, origin note)
    seen = set()
    for c in clusters:
        pad = c["start"] & 3
        code = bytes(c["bytes"][a] for a in range(c["start"], c["end"]))
        key = (pad, code)
        entries = sorted((pc - c["start"] + pad, thumb) for pc, thumb in c["entries"])
        if key in seen:
            continue
        seen.add(key)
        routines.append((pad, code, entries, c["start"]))

    img = bytearray(rom)
    families = []
    src = SOURCE
    for stride in STRIDES:
        fits = [r for r in routines if r[0] + len(r[1]) <= stride and
                (stride == STRIDES[0] or r[0] + len(r[1]) > STRIDES[STRIDES.index(stride) - 1])]
        for g in range(0, len(fits), GROUP):
            group = fits[g:g + GROUP]
            name = "harv%d" % len(families)
            size = len(group) * stride
            if src + size > SOURCE_END:
                raise SystemExit("harvest exceeds the 32 MiB translation ROM window")
            families.append((name, stride, src, group))
            src += size
    os.makedirs(args.out, exist_ok=True)
    for name, stride, src, group in families:
        packed = bytearray(len(group) * stride)
        for v, (pad, code, _, _) in enumerate(group):
            packed[v * stride + pad:v * stride + pad + len(code)] = code
        o = src - 0x08000000
        img[o:o + len(packed)] = packed
        d = os.path.join(args.out, name)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "expected.cpp"), "w", newline="\n") as f:
            f.write("// GENERATED by tools/tla_harvest.py -- game-derived.\n")
            f.write("#include <cstdint>\n")
            f.write('extern "C" const uint8_t %s_kExpected[] = {\n' % name)
            for i in range(0, len(packed), 16):
                f.write("    " + ", ".join("0x%02X" % b for b in packed[i:i + 16]) + ",\n")
            f.write("};\n")
            f.write('extern "C" const uint32_t %s_kSizes[] = {\n' % name)
            for pad, code, _, at in group:
                f.write("    0x%03Xu,  // seen at 0x%08X\n" % (pad + len(code), at))
            f.write("};\n")
            f.write('extern "C" const uint8_t %s_kStarts[] = {\n' % name)
            f.write("    " + ", ".join("%d" % pad for pad, _, _, _ in group) + ",\n")
            f.write("};\n")
            f.write('extern "C" const unsigned %s_kCount = %du;\n' % (name, len(group)))
            f.write('extern "C" const uint32_t %s_kStride = 0x%Xu;\n' % (name, stride))
    sha = hashlib.sha1(img).hexdigest()
    open(os.path.join(args.out, "harvest.gba"), "wb").write(img)

    for name, stride, src, group in families:
        entries = []
        overrides = []
        first = None
        for v, (_, _, rel_entries, at) in enumerate(group):
            for k, (rel, thumb) in enumerate(rel_entries):
                addr = ORIGIN + v * stride + rel
                first = first if first is not None else addr
                entries.append('''
[[extra_func]]
addr = 0x%08X
mode = "%s"
name = "%s_v%d_e%d"
note = "seen at 0x%08X"
''' % (addr, "thumb" if thumb else "arm", name, v, k, at - (group[v][0]) + rel))
        for v, (pad, code, _, at) in enumerate(group):
            for site, opcode, bound in (WORLD_DESCRIPTOR_BOUNDS + JUMP_PAD_BOUNDS +
                                        SPARKLE_BOUNDS + PILLAR_TOP_BOUNDS +
                                        EMITTER_BOUNDS):
                offset = site - at
                if (offset < 0 or offset + 2 > len(code) or
                        struct.unpack_from("<H", code, offset)[0] != opcode):
                    continue
                addr = ORIGIN + v * stride + pad + offset
                evidence = ("F12 tla_rewind_0144/0145, 0147, 0148: pillar-top Thumb function"
                            if (site, opcode, bound) in PILLAR_TOP_BOUNDS else
                            "F12 tla_rewind_0173: emitter-timer Thumb function"
                            if (site, opcode, bound) in EMITTER_BOUNDS else
                            "F12 tla_rewind_0108/0109: Thumb function 0x0200B4FC"
                            if (site, opcode, bound) in SPARKLE_BOUNDS else
                            "F12 tla_rewind_0103/0104: Thumb function 0x0200B0F4"
                            if (site, opcode, bound) in JUMP_PAD_BOUNDS else
                            "F12 tla_rewind_0001/0002: Thumb function 0x0200B6A8")
                overrides.append('''
[[conditional_branch_override]]
addr = 0x%08X
note = "%s, %s at source 0x%08X"
''' % (addr, evidence, bound, site))
        with open(os.path.join(args.out, name, "variant.toml"), "w", newline="\n") as f:
            f.write('''# GENERATED by tools/tla_harvest.py -- do not edit or commit.
# RAM code the game ran (engine self-heal cache), %d routines at stride 0x%X.
[program]
name = "Golden Sun: The Lost Age harvested RAM code %s"
id = "golden_sun_tla_%s"
load_address = 0x08000000
size = 0x%08X
entry_pc = 0x%08X
speculative_literal_harvest = false
codegen_shards = %d

[identity]
sha1 = "%s"

[[code_copy]]
runtime_start = 0x%08X
source_start = 0x%08X
size = 0x%08X
name = "%s"
note = "packed RAM code"
''' % (len(group), stride, name, name, len(img), first,
       32 if stride >= 0x400 else 1, sha, ORIGIN, src,
       len(group) * stride, name))
            f.write("".join(entries))
            f.write("".join(overrides))
        print("%s 0x%08X:%X" % (name, ORIGIN, len(group) * stride))


if __name__ == "__main__":
    main()
