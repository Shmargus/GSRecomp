#!/usr/bin/env python3
"""Build the evidence-backed GS-007 main-ROM TOML proposal."""

from __future__ import annotations

import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
from typing import Any

if __package__:
    from .import_main_symbols import (
        ElfImage,
        PT_LOAD,
        SHF_EXECINSTR,
        STT_FUNC,
        _mapping_symbols,
        parse_elf32_arm,
    )
    from .validate_symbol_corpus import validate_document
    from .verify_rom import EXPECTED_SHA1, EXPECTED_SIZE, sha1_file
else:
    from import_main_symbols import (
        ElfImage,
        PT_LOAD,
        SHF_EXECINSTR,
        STT_FUNC,
        _mapping_symbols,
        parse_elf32_arm,
    )
    from validate_symbol_corpus import validate_document
    from verify_rom import EXPECTED_SHA1, EXPECTED_SIZE, sha1_file


ROM_BASE = 0x08000000
EXPECTED_ENTRY = 0x080003C0
REVISION_RE = re.compile(r"^[0-9a-f]{40}$")
# `[[resume_range]]` is capped at 0x1000 bytes by the upstream TOML schema.
RESUME_RANGE_MAX_BYTES = 0x1000

# Exact conditional branches proven by the verified USA/Europe ROM to reject
# viewport candidates before the unchanged PPU can clip them. The runner may
# force only these decisions, and only for strict 360x240 geometry.
REVIEWED_CONDITIONAL_BRANCH_OVERRIDES = (
    (0x0800B27E, "Func_b168 screen-Y BGT: force the reviewed writer path"),
    (0x0800B324, "Func_b168 screen-X BGT: force the reviewed writer path"),
    (0x0800B328, "Func_b168 screen-Y BGT: force the reviewed writer path"),
    (0x0800B3D2, "Func_b388 lower-X BGE: bypass the padded reject"),
    (0x0800B3DC, "Func_b388 upper-X BLE: bypass the padded reject"),
    (0x0800B3E6, "Func_b388 lower-Y BGE: bypass the padded reject"),
    (0x0800B3EC, "Func_b388 upper-Y BLE: bypass the padded reject"),
    (0x0800C6FA, "Func_c62c upper-X BLS: bypass the direct reject route"),
    (0x0800C702, "Func_c62c lower-Y BLE: bypass the direct reject route"),
    (0x0800C708, "Func_c62c upper-Y BGT: bypass the direct reject route"),
    (0x0801CFC6, "Settings screen BLE: skip the icon bounce for Message speed Instant"),
    # Three sprite routines that queue with Func_3dec after an x+16 <= 255,
    # y >= -32 test; world-map towns vanish at the old right edge on one of
    # them (FACTS.md, 2026-09-24). X and lower-Y only.
    (0x080948BC, "Func_94820 screen-X BHI: admit the expanded margins"),
    (0x080948C4, "Func_94820 lower-Y BLT: admit the top margin"),
    (0x08094C56, "Func_94bbc screen-X BHI: admit the expanded margins"),
    (0x08094C5E, "Func_94bbc lower-Y BLT: admit the top margin"),
    (0x08094F26, "Func_94e7c screen-X BHI: admit the expanded margins"),
    (0x08094F2E, "Func_94e7c lower-Y BLT: admit the top margin"),
    # The battle reward tally's drop roll (Func_c24f0, FACTS.md 2026-09-25):
    # the cheat menu's Item Drops multiplier decides it on a scaled threshold.
    (0x080C26BE, "Func_c24f0 drop roll BLE: cheat menu drop multiplier"),
)

# Instant text as a fourth Message-speed choice (value 3). The settings screen
# wraps each option by the choice count it stores at setup; the runner raises
# only Message speed's count, and only while the launcher option is on.
REVIEWED_THUMB_ALU_IMMEDIATE_OVERRIDES = (
    (0x0801D072, "Settings screen setup: Message-speed choice count movs #3"),
    (0x080180D0, "Text decoder: adds r7,r0,#0 takes each decoded character"),
    (0x08021732, "Icon loader: adds r2,r5,#0 picture source for the OBJ upload"),
    (0x0801D6B8, "Settings screen: Message-speed caption clear x movs #0xA0"),
    (0x0801D6C6, "Settings screen: Message-speed caption draw x movs #0xA0"),
)

# The ROM speed tables hold three entries; index 3 reads the next table's
# bytes. The runner re-bases these two table loads so value 3 reads Fast's
# entry instead.
REVIEWED_THUMB_LITERAL_OVERRIDES = (
    (0x08016E56, "Dialogue delay table 0x08073808 load, indexed by Message speed"),
    (0x08093332, "Battle text table 0x0809FC28 load, indexed by Message speed"),
    (0x0801D6AE, "Settings screen: Message-speed caption text ID base 0xC0A"),
    (0x0801D39C, "Settings screen setup: Message-speed icon table 0x080367C9"),
    (0x0801D5B6, "Settings screen redraw: Message-speed icon table 0x080367C9"),
    (0x0801D772, "Settings loop: pressed-keys word 0x03001C94 (settings page two)"),
    (0x0801D7CA, "Settings loop: repeat-keys word 0x03001B04 (settings page two)"),
    (0x0808AF84, "Encounter step: base 0x02000240 of the counter 0x02000478"),
    (0x08003318, "Frame wait: VBlank flag clear mask 0xFFFE (battle speed-up)"),
)

# ROM functions that strict-static execution has shown to take an asynchronous
# IRQ return at an interior instruction. Listing individual PCs for these was a
# crawl — each build surfaced the next instruction of the same loop — so the
# whole ELF-bounded function is declared instead and `derive_resume_ranges`
# splits it around its own `$d` literal pools. Membership still requires an
# observed resume: this is not a whole-program policy.
REVIEWED_RESUME_FUNCTIONS = (
    # Session 20260924_142353 healed 0x0800bb6c and 0x080c9d54 (thumb, each
    # bridged once): interior PCs of the ELF functions below.
    {
        "addr": 0x0800BB20,
        "mode": "thumb",
        "note": "observed resume at 0x0800bb6c (Func_bb20+0x4c)",
    },
    {
        "addr": 0x080C9CA8,
        "mode": "thumb",
        "note": "observed resume at 0x080c9d54 (Func_c9ca8+0xac)",
    },
    # Session 20260924_111847 healed 0x08006ff4 (thumb, bridged at top level,
    # LR 0x08006b74): the instruction after `ldrh r0,[r6]` in the polling
    # loop of Func_6f84 (ELF 0xa4 bytes, interior $d pools).
    {
        "addr": 0x08006F84,
        "mode": "thumb",
        "note": "observed resume at 0x08006ff4 (Func_6f84+0x70)",
    },
    # Runtime-compiled at interior PCs in sessions 20260923_101919 and
    # _114927; each miss PC lies inside the sized ELF STT_FUNC listed.
    {
        "addr": 0x080F3078,
        "mode": "thumb",
        "note": "observed resume at 0x080f352c (Func_f3078+0x4b4)",
    },
    {
        "addr": 0x080F2F10,
        "mode": "thumb",
        "note": "observed resume at 0x080f2f4c (Func_f2f10+0x3c)",
    },
    {
        "addr": 0x080B0958,
        "mode": "thumb",
        "note": "observed resume at 0x080b09ee (Func_b0958+0x96)",
    },
    {
        "addr": 0x080B6B40,
        "mode": "thumb",
        "note": "observed resume at 0x080b6ba8 (Func_b6b40+0x68)",
    },
    {
        "addr": 0x080044D0,
        "mode": "thumb",
        "note": "observed resume at 0x0800450a (Func_44d0+0x3a)",
    },
    {
        "addr": 0x080797D4,
        "mode": "thumb",
        "note": "observed resume at 0x080797e0 (Func_797d4+0xc)",
    },
    {
        "addr": 0x080B920C,
        "mode": "thumb",
        "note": "observed resume at 0x080b925a (Func_b920c+0x4e)",
    },
    {
        "addr": 0x080E40A4,
        "mode": "thumb",
        "note": "observed resume at 0x080e4628 and 0x080e4658 (Func_e40a4+0x584/+0x5b4)",
    },
    {
        "addr": 0x080045D4,
        "mode": "thumb",
        "note": "observed resume at 0x080045e0 (Func_45d4+0xc)",
    },
    {
        "addr": 0x080BA918,
        "mode": "thumb",
        "note": "observed resume at 0x080ba974 (Func_ba918+0x5c)",
    },
    # Runtime-compiled at their interior PCs in normal play (sessions
    # 20260923_090349 and _093557, "SELF-HEAL bridge entered at top level");
    # each miss PC lies inside the sized ELF STT_FUNC listed.
    {
        "addr": 0x08005AE0,
        "mode": "thumb",
        "note": "observed resume at 0x08005b16 (Func_5ae0+0x36)",
    },
    {
        "addr": 0x0800C598,
        "mode": "thumb",
        "note": "observed resume at 0x0800c5a4 (Func_c598+0xc)",
    },
    {
        "addr": 0x080174D8,
        "mode": "thumb",
        "note": "observed resume at 0x080174dc (Func_174d8+0x4)",
    },
    {
        "addr": 0x0801E8B0,
        "mode": "thumb",
        "note": "observed resume at 0x0801e8ea (Func_1e8b0+0x3a)",
    },
    {
        "addr": 0x08018EFC,
        "mode": "thumb",
        "note": "observed resume at 0x08018f0c (Func_18efc+0x10)",
    },
    {
        "addr": 0x080022EC,
        "mode": "thumb",
        "note": "observed resume at 0x080022ee (Func_af0_from_thumb+0x2)",
    },
    {
        "addr": 0x08021950,
        "mode": "thumb",
        "note": "observed resume at 0x080219a6 (Func_21950+0x56)",
    },
    # Second crawl iteration on the 2026-08-08 fight route: surfaced once the
    # first pair below was seeded. Each verified against the pinned ELF —
    # containing sized STT_FUNC, miss PC inside that function's $t run.
    {
        "addr": 0x08005268,
        "mode": "thumb",
        "note": "observed resume at 0x080052ae (Func_5268+0x46)",
    },
    {
        "addr": 0x08022768,
        "mode": "thumb",
        "note": "observed resume at 0x080227e4 (Func_22768+0x7c)",
    },
    {
        "addr": 0x080C0A24,
        "mode": "thumb",
        "note": "observed resume at 0x080c0b9a (Func_c0a24+0x176)",
    },
    # Observed on the 2026-08-08 fight route after the corpus dedup fix. Both
    # verified against the pinned ELF: containing sized STT_FUNC, and the miss
    # PC sits inside that function's $t (THUMB) mapping run, not a $d run.
    {
        "addr": 0x080BBB0C,
        "mode": "thumb",
        "note": "observed resume at 0x080bbfbc (Func_bbb0c+0x4b0)",
    },
    {
        "addr": 0x080BD898,
        "mode": "thumb",
        "note": "observed resume at 0x080bda7c (Func_bd898+0x1e4)",
    },
    {
        "addr": 0x08077428,
        "mode": "thumb",
        "note": (
            "observed resume at 0x0807796c inside the 0x08077964 loop "
            "(trace event #4660918)"
        ),
    },
    {
        "addr": 0x0801E41C,
        "mode": "thumb",
        "note": (
            "observed resume at 0x0801e4d0 returning from the BIOS IRQ vector "
            "(lr=0x0000027f, trace event #4701726)"
        ),
    },
    {
        "addr": 0x08017AA4,
        "mode": "thumb",
        "note": "observed resume at 0x08017b74 (trace event #4703841)",
    },
    {
        "addr": 0x08018850,
        "mode": "thumb",
        "note": "observed self-heal resume at 0x080189a2 (Func_18850+0x152)",
    },
    # Reached only once the demo-input driver clears the title/file-select and
    # enters gameplay. A no-input run idles and never executes any of these.
    {
        "addr": 0x08010230,
        "mode": "thumb",
        "note": "observed input-driven resume at 0x0801039c (Func_10230+0x16c)",
    },
    {
        "addr": 0x08078BF0,
        "mode": "thumb",
        "note": "observed input-driven resume at 0x08078c70 (Func_78bf0+0x80)",
    },
    {
        "addr": 0x0808B25C,
        "mode": "thumb",
        "note": "observed input-driven resume at 0x0808b282 (Func_8b25c+0x26)",
    },
    {
        "addr": 0x080901C0,
        "mode": "thumb",
        "note": "observed input-driven resume at 0x080901f4 (Func_901c0+0x34)",
    },
    {
        "addr": 0x080FA9E0,
        "mode": "thumb",
        "note": (
            "observed input-driven resume at 0x080faa40 (Func_fa9e0+0x60) in "
            "the M4A sound driver"
        ),
    },
    {
        "addr": 0x08004144,
        "mode": "thumb",
        "note": (
            "observed input-driven resumes at 0x0800415c and 0x08004180 "
            "(Func_4144+0x18 and +0x3c)"
        ),
    },
    # Second input-driven batch, resolved with tools/resolve_miss_functions.py.
    # Several of these produced three or four interior PCs in a single run,
    # which is the pattern the per-PC list could never keep up with.
    {
        "addr": 0x0800AA0C,
        "mode": "thumb",
        "note": (
            "observed input-driven resumes at 0x0800ab1c, 0x0800ab32, "
            "0x0800acf2 and 0x0800acfa (Func_aa0c)"
        ),
    },
    {
        "addr": 0x080168F4,
        "mode": "thumb",
        "note": "observed input-driven resume at 0x08016c2c (Func_168f4+0x338)",
    },
    {
        "addr": 0x08090A5C,
        "mode": "thumb",
        "note": (
            "observed input-driven resumes at 0x08090c22, 0x08090d4e and "
            "0x0809115c (Func_90a5c)"
        ),
    },
    {
        "addr": 0x08093A6C,
        "mode": "thumb",
        "note": "observed input-driven resume at 0x08093a9c (Func_93a6c+0x30)",
    },
    {
        "addr": 0x080949A8,
        "mode": "thumb",
        "note": (
            "observed input-driven resumes at 0x08094a14, 0x08094a62 and "
            "0x08094a86 (Func_949a8)"
        ),
    },
    {
        "addr": 0x0808E9C0,
        "mode": "thumb",
        "note": "observed input-driven resume at 0x0808ea30 (Func_8e9c0+0x70)",
    },
    {
        "addr": 0x08018A50,
        "mode": "thumb",
        "note": (
            "observed input-driven resumes at 0x08018b60, 0x08018b76 and "
            "0x08018b84 (Func_18a50)"
        ),
    },
    {
        "addr": 0x0808F52C,
        "mode": "thumb",
        "note": (
            "eight observed input-driven resumes between 0x0808fc32 and "
            "0x0808fcda (Func_8f52c); the clearest case for ranges over PCs"
        ),
    },
    # Reached only once the position-independent RAM images cleared the
    # relocatable pool: the campaign run no longer stops at a moving RAM base,
    # so the next boundary is an ordinary ROM-space interior entry. Trace
    # #9814457 dispatches 0x08096a74 with r3 = 0x08096a74, i.e. a computed
    # target inside the ELF-sized Func_96960 (0x08096960, size 0x150).
    {
        "addr": 0x08096960,
        "mode": "thumb",
        "note": (
            "observed input-driven interior entry at 0x08096a74 "
            "(Func_96960+0x114, trace event #9814457)"
        ),
    },
    {
        "addr": 0x0809088C,
        "mode": "thumb",
        "note": (
            "observed input-driven interior entry at 0x080908b0 "
            "(Func_9088c+0x24, trace event #9845087)"
        ),
    },
    {
        "addr": 0x0800BA30,
        "mode": "thumb",
        "note": "observed windowed frame-boundary resume at 0x0800ba32 (Func_ba30+0x2)",
    },
    {
        "addr": 0x0800C62C,
        "mode": "thumb",
        "note": "observed windowed frame-boundary resume at 0x0800c698 (Func_c62c+0x6c)",
    },
    {
        "addr": 0x0808BA1C,
        "mode": "thumb",
        "note": "observed windowed frame-boundary resume at 0x0808ba1e (Func_8ba1c+0x2)",
    },
    {
        "addr": 0x08091174,
        "mode": "thumb",
        "note": "observed windowed frame-boundary resume at 0x08091190 (Func_91174+0x1c)",
    },
    {
        "addr": 0x080923E4,
        "mode": "thumb",
        "note": "observed windowed frame-boundary resume at 0x080923e8 (Func_923e4+0x4)",
    },
    {
        "addr": 0x080A5CC0,
        "mode": "thumb",
        "note": "observed windowed frame-boundary resume at 0x080a5d04 (Func_a5cc0+0x44)",
    },
    {
        "addr": 0x080908E0,
        "mode": "thumb",
        "note": "observed windowed frame-boundary resume at 0x08090936 (Func_908e0+0x56)",
    },
    # Frame-boundary resume in WINDOWED play. Only reachable with
    # GBARECOMP_PRESENT_IN_PLACE=0, i.e. on the unwind-and-redispatch path;
    # present-in-place hid this class by never unwinding, at the cost of an
    # unbounded host stack (see gbarecomp docs/DEBUGGING.md).
    {
        "addr": 0x080040E8,
        "mode": "thumb",
        "note": "observed windowed frame-boundary resume at 0x08004100 (Func_40e8+0x18)",
    },
    # Beyond 5,400 frames: the campaign track is fully static through 5,400, and
    # these are what the 10,800-frame extension of it finds.
    # Seeded now that the upstream finder's BL-as-long-branch fix (gbarecomp
    # src/recompile/function_finder.cpp, direct_call_continuation /
    # stats_.long_branch_calls) is implemented and its test suite passes.
    # Func_a2680's ELF extent (0xc38, ending exactly at Func_a32b8), THUMB
    # mode, the $d pad at 0x080a29fa, the 13-word literal pool at
    # 0x080a29fc and the $t resume at 0x080a2a30 were measured and match the
    # ROM/ELF bytes exactly. See docs/GS011_TRANSIENT_IMAGES.md.
    {
        "addr": 0x080A2680,
        "mode": "thumb",
        "note": "observed resume at 0x080a26f0 (Func_a2680+0x70)",
    },
    {
        "addr": 0x0801C244,
        "mode": "thumb",
        "note": "observed input-driven resume at 0x0801c2a8 (Func_1c244+0x64)",
    },
    {
        "addr": 0x08003E58,
        "mode": "thumb",
        "note": "observed input-driven resume at 0x08003eb8 (Func_3e58+0x60)",
    },
    {
        "addr": 0x08003F04,
        "mode": "thumb",
        "note": "observed input-driven resume at 0x08003f22 (Func_3f04+0x1e)",
    },
    {
        "addr": 0x08010424,
        "mode": "thumb",
        "note": "observed input-driven resume at 0x080104d4 (Func_10424+0xb0)",
    },
    # Found by the 5,400-frame campaign track; the 1,800-frame track never
    # reaches it. Trace #13491210 dispatches THUMB 0x0800fa5a.
    {
        "addr": 0x0800F9F4,
        "mode": "thumb",
        "note": "observed input-driven resume at 0x0800fa5a (Func_f9f4+0x66)",
    },
    {
        "addr": 0x03000380,
        "mode": "arm",
        "note": "observed input-driven resume at 0x03000390 (Func_af0+0x10)",
    },
    # Func_d30 is a third resident of that copy, reached once the boundary got
    # past the flash driver. Trace #10385332 dispatches ARM 0x0300061c.
    {
        "addr": 0x030005C0,
        "mode": "arm",
        "note": "observed input-driven resume at 0x0300061c (Func_d30+0x5c)",
    },
    # Func_b60 is another resident of the 0x08000770 -> 0x03000000 code copy.
    {
        "addr": 0x030003F0,
        "mode": "arm",
        "note": "observed input-driven resume at 0x03000544 (Func_b60+0x154)",
    },
    # Func_1af8 lives at runtime 0x03001388, inside the ELF-declared
    # 0x08000770 -> 0x03000000 code copy that main.toml already emits, so a
    # runtime-addressed resume range is legal here. Its ELF extent carries a
    # single `$a` and no interior `$d`.
    {
        "addr": 0x03001388,
        "mode": "arm",
        "note": "observed input-driven resume at 0x030013d4 (Func_1af8+0x4c)",
    },
    # 10,800-frame campaign strict-static abort boundary. ELF STT_FUNC
    # Func_1bc34 at 0x0801bc34, size 0xa0, with a $t mapping symbol at its
    # entry confirming THUMB mode; a $d/$t split at 0x0801bc4e/0x0801bc74
    # inside its extent puts a literal pool ahead of the resume PC, which
    # still falls inside the declared extent (0x0801bc34..0x0801bcd4).
    {
        "addr": 0x0801BC34,
        "mode": "thumb",
        "note": "observed resume at 0x0801bc84 (Func_1bc34+0x50)",
    },
    # The next boundary after Func_1bc34, and literally the next function: ELF
    # STT_FUNC Func_1bcd4 at 0x0801bcd4 (where Func_1bc34's extent ends), size
    # 0xc4, $t at the entry confirming THUMB. Observed resume 0x0801bd42 falls
    # inside 0x0801bcd4..0x0801bd98.
    {
        "addr": 0x0801BCD4,
        "mode": "thumb",
        "note": "observed resume at 0x0801bd42 (Func_1bcd4+0x6e)",
    },
    # First boundary inside the newly un-excluded rom_a1000. ELF STT_FUNC
    # Func_a112c at 0x080a112c, size 0x3c4, $t at the entry (THUMB).
    {
        "addr": 0x080A112C,
        "mode": "thumb",
        "note": "observed resume at 0x080a12c8 (Func_a112c+0x19c)",
    },
    # ELF STT_FUNC Func_1de5c at 0x0801de5c, size 0x404, $t at the entry.
    {
        "addr": 0x0801DE5C,
        "mode": "thumb",
        "note": "observed resume at 0x0801dfb2 (Func_1de5c+0x156)",
    },
    # Third play-session batch: everything still translated AT RUNTIME
    # during the Mt. Aleph boulder scene, enumerated from a cold-cache
    # replay of the player's own recorded input. This is the stutter the
    # player reported as 'MASSIVE slowdowns' - 70 runtime compiles across
    # 16 containing routines.
    {
        "addr": 0x030003E0,
        "mode": "arm",
        "note": "observed resume at 0x030003e8 (Func_b50+0x8)",
    },
    {
        "addr": 0x08002DD8,
        "mode": "thumb",
        "note": "observed resume at 0x08002dda (Func_2dd8+0x2)",
    },
    {
        "addr": 0x080030F8,
        "mode": "thumb",
        "note": "observed resume at 0x0800316a (Func_30f8+0x72)",
    },
    {
        "addr": 0x08003E10,
        "mode": "thumb",
        "note": "observed resume at 0x08003e22 (Func_3e10+0x12)",
    },
    {
        "addr": 0x08004080,
        "mode": "thumb",
        "note": "observed resume at 0x08004096 (Func_4080+0x16)",
    },
    {
        "addr": 0x08004420,
        "mode": "thumb",
        "note": "observed resume at 0x0800443e (Func_4420+0x1e)",
    },
    {
        "addr": 0x0800A97C,
        "mode": "thumb",
        "note": "observed resume at 0x0800a9b0 (Func_a97c+0x34)",
    },
    {
        "addr": 0x0800B168,
        "mode": "thumb",
        "note": "observed resume at 0x0800b17c (Func_b168+0x14)",
    },
    {
        "addr": 0x0800B8AC,
        "mode": "thumb",
        "note": "observed resume at 0x0800b918 (Func_b8ac+0x6c)",
    },
    {
        "addr": 0x0800BBC0,
        "mode": "thumb",
        "note": "observed resume at 0x0800bbf0 (Func_bbc0+0x30)",
    },
    {
        "addr": 0x0800BC70,
        "mode": "thumb",
        "note": "observed resume at 0x0800bca6 (Func_bc70+0x36)",
    },
    {
        "addr": 0x08016F2C,
        "mode": "thumb",
        "note": "observed resume at 0x08016f44 (Func_16f2c+0x18)",
    },
    {
        "addr": 0x080178B0,
        "mode": "thumb",
        "note": "observed resume at 0x08017a08 (Func_178b0+0x158)",
    },
    {
        "addr": 0x08092054,
        "mode": "thumb",
        "note": "observed resume at 0x0809205c (Func_92054+0x8)",
    },
    # Second play-session batch: the sages' psynergy scene at Mt. Aleph.
    # These were being translated AT RUNTIME mid-scene, which is what made
    # the spell stutter - the player was watching a C compiler run.
    {
        "addr": 0x08003D28,
        "mode": "thumb",
        "note": "observed resume at 0x08003d3e (Func_3d28+0x16)",
    },
    {
        "addr": 0x08003FA4,
        "mode": "thumb",
        "note": "observed resume at 0x08003fbe (Func_3fa4+0x1a)",
    },
    {
        "addr": 0x08004458,
        "mode": "thumb",
        "note": "observed resume at 0x0800446a (Func_4458+0x12)",
    },
    {
        "addr": 0x0800C0CC,
        "mode": "thumb",
        "note": "observed resume at 0x0800c0e6 (Func_c0cc+0x1a)",
    },
    {
        "addr": 0x08011F54,
        "mode": "thumb",
        "note": "observed resume at 0x08011f98 (Func_11f54+0x44)",
    },
    {
        "addr": 0x080770C0,
        "mode": "thumb",
        "note": "observed resume at 0x080770c2 (_Func_79338+0x2)",
    },
    {
        "addr": 0x08079338,
        "mode": "thumb",
        "note": "observed resume at 0x0807934a (Func_79338+0x12)",
    },
    {
        "addr": 0x08094820,
        "mode": "thumb",
        "note": "observed resume at 0x08094858 (Func_94820+0x38)",
    },
    # THE FIRST FAILURE in that play session, and the one everything else
    # followed from. Self-heal could not repair ARM 0x030001F0 ("function
    # finder found no entry at the miss PC") so it ran on the interpreter
    # bridge all session. ELF Func_948 sits at RUNTIME 0x030001d8 size 0x3c
    # with a single $a and no interior $d - the IWRAM image's symbols are
    # listed at their runtime addresses, not their ROM source addresses -
    # and 0x030001F0 is Func_948+0x18.
    {
        "addr": 0x030001D8,
        "mode": "arm",
        "note": "observed play-session resume at 0x030001f0 (Func_948+0x18)",
    },
    # Batch promoted from a REAL play session (Mt. Aleph prologue, the
    # sages' psynergy scene). Each is an interior resume inside an
    # ELF-sized STT_FUNC, resolved from that session's own SELF-HEAL log
    # by tools/resolve_miss_functions.py. Play found these; the scripted
    # campaign track never reached them.
    {
        "addr": 0x08003DEC,
        "mode": "thumb",
        "note": "observed resume at 0x08003e02 (Func_3dec+0x16)",
    },
    {
        "addr": 0x080048B0,
        "mode": "thumb",
        "note": "observed resume at 0x080048b4 (Func_48b0+0x4)",
    },
    {
        "addr": 0x0800C004,
        "mode": "thumb",
        "note": "observed resume at 0x0800c03a (Func_c004+0x36)",
    },
    {
        "addr": 0x08015EC0,
        "mode": "thumb",
        "note": "observed resume at 0x08015ec4 (Func_15ec0+0x4)",
    },
    {
        "addr": 0x080191CC,
        "mode": "thumb",
        "note": "observed resume at 0x08019310 (Func_191cc+0x144)",
    },
    {
        "addr": 0x0801A088,
        "mode": "thumb",
        "note": "observed resume at 0x0801a0a4 (Func_1a088+0x1c)",
    },
    {
        "addr": 0x0801CCC0,
        "mode": "thumb",
        "note": "observed resume at 0x0801ccc8 (Func_1ccc0+0x8)",
    },
    {
        "addr": 0x0801D4CC,
        "mode": "thumb",
        "note": "observed resume at 0x0801d726 (Func_1d4cc+0x25a)",
    },
    {
        "addr": 0x0801E260,
        "mode": "thumb",
        "note": "observed resume at 0x0801e2ae (Func_1e260+0x4e)",
    },
    {
        "addr": 0x0801E7C0,
        "mode": "thumb",
        "note": "observed resume at 0x0801e820 (Func_1e7c0+0x60)",
    },
    {
        "addr": 0x080799B0,
        "mode": "thumb",
        "note": "observed resume at 0x08079a62 (Func_799b0+0xb2)",
    },
    {
        "addr": 0x080A3EF0,
        "mode": "thumb",
        "note": "observed resume at 0x080a3f6c (Func_a3ef0+0x7c)",
    },
    {
        "addr": 0x080A414C,
        "mode": "thumb",
        "note": "observed resume at 0x080a4398 (Func_a414c+0x24c)",
    },
    {
        "addr": 0x080A9F10,
        "mode": "thumb",
        "note": "observed resume at 0x080a9fcc (Func_a9f10+0xbc)",
    },
    {
        "addr": 0x08003D28,
        "mode": "thumb",
        "note": "observed resume at 0x08003d8e (Func_3d28+0x66)",
    },
    {
        "addr": 0x08094820,
        "mode": "thumb",
        "note": "observed resume at 0x08094944 (Func_94820+0x124)",
    },
    # First boundary found AFTER the recompiler learned to resume into a THUMB
    # BL_suffix (which is what unblocked 0x08094944 above, and 716 others).
    # 21,600-frame campaign track, trace event #105,788,455. ELF STT_FUNC
    # Func_2df0 at 0x08002df0, size 16, extent 0x08002df0..0x08002e00, $t at
    # the entry; the observed resume 0x08002df2 is `movs r1,#4`, well clear of
    # the function's single trailing $d literal pool at 0x08002dfc.
    {
        "addr": 0x08002DF0,
        "mode": "thumb",
        "note": "observed resume at 0x08002df2 (Func_2df0+0x2)",
    },
    # First RAM-space entry in this list. Func_8d4 lives at RUNTIME 0x03000164,
    # inside the boot [[code_copy]] 0x08000770 -> 0x03000000 size 0x1400
    # (0x03000164 maps back to ROM 0x080008d4, which is where its name comes
    # from). TOML_SCHEMA.md allows a [[resume_range]] to sit inside a declared
    # [[code_copy]] runtime span, so this needs no transient-image treatment -
    # unlike the RAM images reached through an [[extra_func]] source_addr,
    # which have no declared span and cannot use resume ranges at all.
    # 21,600-frame campaign track. ELF STT_FUNC size 0x74, extent
    # 0x03000164..0x030001d8, $a at the entry; observed resume 0x030001d4.
    {
        "addr": 0x03000164,
        "mode": "arm",
        "note": "observed resume at 0x030001d4 (Func_8d4+0x70)",
    },
    # 0x0808a080 (_Func_92054) removed: it is a linker-emitted interworking
    # veneer, and its +0x2 resume (0x0808a082) is now derived by
    # scan_veneer_stubs/derive_veneer_resume_points instead of hand-seeded
    # here. See THUMB_LDR_R4_PC0/THUMB_BX_R4 above.
    #
    # ROM-space self-heal misses from two real play sessions (HANDOFF_CRASH_
    # SCRIPTED_FIGHT.md seed loop). Each resolved via
    # tools/resolve_miss_functions.py to a sized ELF STT_FUNC with a $t
    # mapping symbol at the containing function's entry matching the observed
    # thumb mode, and the miss PC itself sits strictly between that $t and the
    # next mapping symbol (never inside an intervening $d run) -- verified by
    # hand against goldensun.elf's own mapping symbols. None of the nine is a
    # THUMB BL_suffix resume (checked the halfword pair at PC-2/PC against the
    # 0xF000-F7FF/0xF800-FFFF BL encoding for each; none matched).
    {
        "addr": 0x08015F30,
        "mode": "thumb",
        "note": "observed resume at 0x08015f4e (Func_15f30+0x1e)",
    },
    {
        "addr": 0x080FAA58,
        "mode": "thumb",
        "note": (
            "observed resume at 0x080fab02 and 0x080fab34 "
            "(Func_faa58+0xaa, Func_faa58+0xdc)"
        ),
    },
    {
        "addr": 0x08016868,
        "mode": "thumb",
        "note": (
            "observed resume at 0x0801687a and 0x080168e6 "
            "(Func_16868+0x12, Func_16868+0x7e)"
        ),
    },
    {
        "addr": 0x0800C150,
        "mode": "thumb",
        "note": (
            "observed resume at 0x0800c1ae and 0x0800c242 "
            "(Func_c150+0x5e, Func_c150+0xf2)"
        ),
    },
    {
        "addr": 0x080B6EB4,
        "mode": "thumb",
        "note": (
            "observed resume at 0x080b6f08 and 0x080b6f1a "
            "(Func_b6eb4+0x54, Func_b6eb4+0x66)"
        ),
    },
    {
        "addr": 0x080B6F44,
        "mode": "thumb",
        "note": (
            "observed resume at 0x080b6fe0 and 0x080b6ff4 "
            "(Func_b6f44+0x9c, Func_b6f44+0xb0)"
        ),
    },
    {
        "addr": 0x08185008,
        "mode": "thumb",
        "note": "observed resume at 0x0818500a (Func_185008+0x2)",
    },
    {
        "addr": 0x08003F3C,
        "mode": "thumb",
        "note": "observed resume at 0x08003f3e (Func_3f3c+0x2)",
    },
    {
        "addr": 0x0800CACC,
        "mode": "thumb",
        "note": "observed resume at 0x0800cf40 (Func_cacc+0x474)",
    },
    {
        "addr": 0x08091294,
        "mode": "thumb",
        "note": "observed resume at 0x08091296 (Func_91294+0x2)",
    },
    # First scripted-fight battle-scene session, ROM-space misses only (RAM-
    # space misses from the same session are a separate investigation). 13
    # observed PCs collapse into 4 containing STT_FUNC symbols. Entry mapping
    # symbols verified ($t at each function start, matching the observed
    # THUMB mode) and each PC confirmed to sit in a $t run, never inside an
    # intervening $d literal pool, by hand against goldensun.elf's own
    # mapping symbols. None of the 13 is a THUMB BL_suffix resume (checked
    # the halfword pair at PC-2/PC against the 0xF000-F7FF/0xF800-FFFF BL
    # encoding for each; none matched).
    {
        "addr": 0x08021CB8,
        "mode": "thumb",
        "note": "observed resume at 0x08021d38 (Func_21cb8+0x80)",
    },
    {
        "addr": 0x08027114,
        "mode": "thumb",
        "note": (
            "ten observed resumes between 0x080275fc and 0x08027716 "
            "(Func_27114+0x4e8 .. +0x602)"
        ),
    },
    {
        "addr": 0x080B9B30,
        "mode": "thumb",
        "note": "observed resume at 0x080b9c34 (Func_b9b30+0x104)",
    },
    {
        "addr": 0x080BE378,
        "mode": "thumb",
        "note": "observed resume at 0x080be76c (Func_be378+0x3f4)",
    },
    # User save-state-1 play session at frame 9,066, 2026-08-08. These are the
    # only self-heals from that run whose identities and extents are static:
    # three ARM functions in the boot [[code_copy]], plus fourteen ROM THUMB
    # functions. The observed PCs were checked against their sized ELF
    # STT_FUNC and mapping-symbol runs. Runtime-generated code at 0x03006000+
    # and mutable stack code at 0x03007a90+ are intentionally excluded.
    {
        "addr": 0x03000118,
        "mode": "arm",
        "note": "observed resumes at 0x03000124 and 0x03000128 (Func_888)",
    },
    {
        "addr": 0x03000250,
        "mode": "arm",
        "note": "observed resume at 0x03000278 (Func_9c0+0x28)",
    },
    {
        "addr": 0x030002C0,
        "mode": "arm",
        "note": (
            "five observed resumes between 0x030002e4 and 0x0300032c "
            "inside Func_a30"
        ),
    },
    {
        "addr": 0x08002F40,
        "mode": "thumb",
        "note": "observed resume at 0x08002f42 (Func_2f40+0x2)",
    },
    {
        "addr": 0x08004C1C,
        "mode": "thumb",
        "note": "observed resume at 0x08004c4c (Func_4c1c+0x30)",
    },
    {
        "addr": 0x08004FE4,
        "mode": "thumb",
        "note": "observed resumes at 0x08005146 and 0x0800517e (Func_4fe4)",
    },
    {
        "addr": 0x0800B388,
        "mode": "thumb",
        "note": "observed resumes at 0x0800b444 and 0x0800b4e4 (Func_b388)",
    },
    {
        "addr": 0x0800C880,
        "mode": "thumb",
        "note": (
            "observed resumes at 0x0800c8b2, 0x0800c980 and 0x0800c9de "
            "inside Func_c880"
        ),
    },
    {
        "addr": 0x08015E8C,
        "mode": "thumb",
        "note": "observed resume at 0x08015e92 (Func_15e8c+0x6)",
    },
    {
        "addr": 0x08018038,
        "mode": "thumb",
        "note": "observed resume at 0x08018280 (Func_18038+0x248)",
    },
    {
        "addr": 0x0802281C,
        "mode": "thumb",
        "note": "observed resume at 0x0802289c (Func_2281c+0x80)",
    },
    {
        "addr": 0x0808BB2C,
        "mode": "thumb",
        "note": "observed resume at 0x0808bb8c (Func_8bb2c+0x60)",
    },
    {
        "addr": 0x080B63C8,
        "mode": "thumb",
        "note": "observed resume at 0x080b65fe (Func_b63c8+0x236)",
    },
    {
        "addr": 0x080B7DD0,
        "mode": "thumb",
        "note": "observed resume at 0x080b7dd6 (Func_b7dd0+0x6)",
    },
    {
        "addr": 0x080C1724,
        "mode": "thumb",
        "note": "observed resumes at 0x080c1760 and 0x080c176e (Func_c1724)",
    },
    {
        "addr": 0x080E38B8,
        "mode": "thumb",
        "note": "observed resume at 0x080e38cc (Func_e38b8+0x14)",
    },
    {
        "addr": 0x080E3944,
        "mode": "thumb",
        "note": "observed resume at 0x080e3946 (Func_e3944+0x2)",
    },
    {
        "addr": 0x080F26EC,
        "mode": "thumb",
        "note": (
            "State-8 strict-static IRQ resume at 0x080f29b0 "
            "(Func_f26ec+0x2c4, trace event #5186694)"
        ),
    },
    {
        "addr": 0x080F2EBC,
        "mode": "thumb",
        "note": (
            "State-8 strict-static IRQ resume at Func_f2ebc+0x36; whole "
            "ELF-bounded THUMB run reviewed through the 0x080f2f08 literal pool"
        ),
    },    # Real-play progression into Sol Sanctum, 2026-08-09. The process was
    # terminated with STATUS_APPLICATION_HANG before its shutdown-only miss
    # fragment could be written, so the observed PCs were recovered from the
    # session log and its newly written self-heal cache entries. All ROM PCs
    # resolve to sized ELF STT_FUNC symbols and sit in $t runs; the IWRAM PC is
    # in the declared boot code-copy and resolves to Func_984's $a run. None of
    # the THUMB PCs is a BL suffix.
    {
        "addr": 0x03000214,
        "mode": "arm",
        "note": "observed resume at 0x03000220 (Func_984+0xc)",
    },
    {
        "addr": 0x08003F78,
        "mode": "thumb",
        "note": "observed resumes at 0x08003f8e and 0x08003f9c (Func_3f78)",
    },
    {
        "addr": 0x08004938,
        "mode": "thumb",
        "note": "observed resume at 0x08004946 (Func_4938+0xe)",
    },
    {
        "addr": 0x0800D6D8,
        "mode": "thumb",
        "note": "observed resume at 0x0800d6ea (Func_d6d8+0x12)",
    },
    {
        "addr": 0x0800D710,
        "mode": "thumb",
        "note": "observed resume at 0x0800d722 (Func_d710+0x12)",
    },
    {
        "addr": 0x0800D924,
        "mode": "thumb",
        "note": "observed resumes at 0x0800d93c, 0x0800d950 and 0x0800d97c inside Func_d924",
    },
    {
        "addr": 0x0800EBA0,
        "mode": "thumb",
        "note": "observed resumes at 0x0800eba6 and 0x0800ebd6 (Func_eba0)",
    },
    {
        "addr": 0x0801179C,
        "mode": "thumb",
        "note": "observed resumes at 0x080117ec and 0x08011874 (Func_1179c)",
    },
    {
        "addr": 0x080118D8,
        "mode": "thumb",
        "note": "observed resume at 0x0801194e (Func_118d8+0x76)",
    },
    {
        "addr": 0x080120DC,
        "mode": "thumb",
        "note": "observed resumes at 0x080120fc and 0x0801214c (Func_120dc)",
    },
    {
        "addr": 0x08019BAC,
        "mode": "thumb",
        "note": "observed resume at 0x08019bca (Func_19bac+0x1e)",
    },
    {
        "addr": 0x0801CC50,
        "mode": "thumb",
        "note": "observed resume at 0x0801cc9a (Func_1cc50+0x4a)",
    },
    {
        "addr": 0x0807A1F8,
        "mode": "thumb",
        "note": "observed resume at 0x0807a2b8 (Func_7a1f8+0xc0)",
    },
    {
        "addr": 0x0808ACE0,
        "mode": "thumb",
        "note": "observed resume at 0x0808ad7a (Func_8ace0+0x9a)",
    },
    {
        "addr": 0x08096810,
        "mode": "thumb",
        "note": "observed resumes at 0x08096878 and 0x0809687c (Func_96810)",
    },
    {
        "addr": 0x080A1A40,
        "mode": "thumb",
        "note": "observed resume at 0x080a1a56 (Func_a1a40+0x16)",
    },
    # Real-play progression from state 6 through the Vale bandit sequence
    # (session_20260813_172202).  Each miss below resolves to a sized pinned-
    # ELF STT_FUNC; every observed PC is in a $t run and none is a BL suffix.
    # 0x0800d1c2 and the unsized 0x08002338/0x08002340 are deliberately
    # excluded because the first is in a $d run and the latter two have no
    # containing sized STT_FUNC.
    {"addr": 0x080022FC, "mode": "thumb", "note": "observed resume at 0x080022fe (Func_b1c_from_thumb+0x2)"},
    {"addr": 0x08004620, "mode": "thumb", "note": "observed resume at 0x0800464c (Func_4620+0x2c)"},
    {"addr": 0x080048F4, "mode": "thumb", "note": "observed resume at 0x080048f8 (Func_48f4+0x4)"},
    {"addr": 0x080049AC, "mode": "thumb", "note": "observed resume at 0x080049ae (Func_49ac+0x2)"},
    {"addr": 0x08004BD4, "mode": "thumb", "note": "observed resume at 0x08004bf4 (Func_4bd4+0x20)"},
    {"addr": 0x08005340, "mode": "thumb", "note": "observed resume at 0x08005366 (Func_5340+0x26)"},
    {"addr": 0x08005394, "mode": "thumb", "note": "observed resume at 0x080053ba (Func_5394+0x26)"},
    {"addr": 0x080053E8, "mode": "thumb", "note": "observed resumes at 0x0800540e and 0x0800542c (Func_53e8)"},
    {"addr": 0x0800B6B8, "mode": "thumb", "note": "observed resume at 0x0800b76c (Func_b6b8+0xb4)"},
    {"addr": 0x0800D340, "mode": "thumb", "note": "observed resumes at 0x0800d452 and 0x0800d638 (Func_d340)"},
    {"addr": 0x0800E9A0, "mode": "thumb", "note": "observed resume at 0x0800e9d4 (Func_e9a0+0x34)"},
    {"addr": 0x08010000, "mode": "thumb", "note": "observed resume at 0x080101ac (Func_10000+0x1ac)"},
    {"addr": 0x080108E4, "mode": "thumb", "note": "observed resumes at 0x08010968 and 0x0801099a (Func_108e4)"},
    {"addr": 0x080123F4, "mode": "thumb", "note": "observed resume at 0x08012492 (Func_123f4+0x9e)"},
    {"addr": 0x080170C4, "mode": "thumb", "note": "observed resume at 0x080170de (Func_170c4+0x1a)"},
    {"addr": 0x08017DD4, "mode": "thumb", "note": "observed resume at 0x08017e36 (Func_17dd4+0x62)"},
    {"addr": 0x08018CAC, "mode": "thumb", "note": "observed resume at 0x08018d32 (Func_18cac+0x86)"},
    {"addr": 0x0801A5A4, "mode": "thumb", "note": "observed resume at 0x0801a5ca (Func_1a5a4+0x26)"},
    {"addr": 0x0801F088, "mode": "thumb", "note": "observed resumes at 0x0801f150..0x0801f1b6 (Func_1f088)"},
    {"addr": 0x080218DC, "mode": "thumb", "note": "observed resume at 0x08021942 (Func_218dc+0x66)"},
    {"addr": 0x08021E6C, "mode": "thumb", "note": "observed resume at 0x0802233a (Func_21e6c+0x4ce)"},
    {"addr": 0x08026080, "mode": "thumb", "note": "observed resumes at 0x080266de..0x0802670c (Func_26080)"},
    {"addr": 0x08079D7C, "mode": "thumb", "note": "observed resume at 0x08079e78 (Func_79d7c+0xfc)"},
    {"addr": 0x0808B2B0, "mode": "thumb", "note": "observed resume at 0x0808b2e0 (Func_8b2b0+0x30)"},
    {"addr": 0x080912A8, "mode": "thumb", "note": "observed resumes at 0x080912ae and 0x080912b0 (Func_912a8)"},
    {"addr": 0x080A17C4, "mode": "thumb", "note": "observed resume at 0x080a17c6 (Func_a17c4+0x2)"},
    {"addr": 0x080A1BDC, "mode": "thumb", "note": "observed resume at 0x080a1c08 (Func_a1bdc+0x2c)"},
    {"addr": 0x080A1C2C, "mode": "thumb", "note": "observed resume at 0x080a1c58 (Func_a1c2c+0x2c)"},
    {"addr": 0x080AA768, "mode": "thumb", "note": "observed resumes at 0x080aa7fa and 0x080aa812 (Func_aa768)"},
    {"addr": 0x080B5864, "mode": "thumb", "note": "observed resumes at 0x080b5868 and 0x080b58ce (Func_b5864)"},
    {"addr": 0x080B6C08, "mode": "thumb", "note": "observed resumes at 0x080b6c1e..0x080b6c32 (Func_b6c08)"},
    {"addr": 0x080B7738, "mode": "thumb", "note": "observed resumes at 0x080b7786..0x080b7880 (Func_b7738)"},
    {"addr": 0x080B7F70, "mode": "thumb", "note": "observed resumes at 0x080b7f7a and 0x080b7f7c (Func_b7f70)"},
    {"addr": 0x080B8530, "mode": "thumb", "note": "observed resume at 0x080b854e (Func_b8530+0x1e)"},
    {"addr": 0x080BAE40, "mode": "thumb", "note": "observed resume at 0x080bb380 (Func_bae40+0x540)"},
    {"addr": 0x080BB938, "mode": "thumb", "note": "observed resumes at 0x080bb9b6 and 0x080bba0c (Func_bb938)"},
    {"addr": 0x080BD424, "mode": "thumb", "note": "observed resume at 0x080bd4d6 (Func_bd424+0xb2)"},
    {"addr": 0x080CCAEC, "mode": "thumb", "note": "observed resume at 0x080ccb6c (Func_ccaec+0x80)"},
    {"addr": 0x080CD260, "mode": "thumb", "note": "observed resumes at 0x080cd2f0 and 0x080cd302 (Func_cd260)"},
    {"addr": 0x080CDE90, "mode": "thumb", "note": "observed resumes at 0x080cde94..0x080cdf6e (Func_cde90)"},
    {"addr": 0x080DB264, "mode": "thumb", "note": "observed resumes at 0x080db370..0x080db5ce (Func_db264)"},
    {"addr": 0x080DB6E0, "mode": "thumb", "note": "observed resumes at 0x080db7fc and 0x080db984 (Func_db6e0)"},
    {"addr": 0x080DF9D0, "mode": "thumb", "note": "observed resume at 0x080dfa04 (Func_df9d0+0x34)"},
    {"addr": 0x080E0564, "mode": "thumb", "note": "observed resume at 0x080e07e0 (Func_e0564+0x27c)"},
    {"addr": 0x080E2974, "mode": "thumb", "note": "observed resume at 0x080e2a2a (Func_e2974+0xb6)"},
    {"addr": 0x080E3908, "mode": "thumb", "note": "observed resumes at 0x080e390a..0x080e391c (Func_e3908)"},
    {"addr": 0x080E3AA0, "mode": "thumb", "note": "observed resumes at 0x080e4018 and 0x080e402c (Func_e3aa0)"},
    {"addr": 0x080E47B8, "mode": "thumb", "note": "observed resumes at 0x080e4ab8 and 0x080e4ad0 (Func_e47b8)"},
    # session_20260923_171845; each miss PC lies inside the sized ELF STT_FUNC
    # listed (all thumb). Resolved with tools/resolve_miss_functions.py.
    {"addr": 0x080041D8, "mode": "thumb", "note": "observed resume at 0x08004214 (Func_41d8+0x3c)"},
    {"addr": 0x0800447C, "mode": "thumb", "note": "observed resumes at 0x08004496 and 0x080044c6 (Func_447c)"},
    {"addr": 0x08006A00, "mode": "thumb", "note": "observed resumes at 0x08006a02, 0x08006a08, 0x08006a10, 0x08006a16, 0x08006a18, 0x08006a20, 0x08006a2c, 0x08006a2e, 0x08006a34, 0x08006a40 and 0x08006a54 (Func_6a00)"},
    {"addr": 0x08006A78, "mode": "thumb", "note": "observed resumes at 0x08006a7a, 0x08006a8c, 0x08006a92, 0x08006a96, 0x08006a98, 0x08006a9c, 0x08006aa0 and 0x08006aa2 (Func_6a78)"},
    {"addr": 0x08006AF8, "mode": "thumb", "note": "observed resumes at 0x08006afa, 0x08006b08, 0x08006b0a, 0x08006b16, 0x08006b1e and 0x08006b4a (Func_6af8)"},
    {"addr": 0x08006DEC, "mode": "thumb", "note": "observed resumes at 0x08006dfa, 0x08006dfe, 0x08006e04, 0x08006e08, 0x08006e12 and 0x08006e14 (Func_6dec)"},
    {"addr": 0x0800DAF0, "mode": "thumb", "note": "observed resume at 0x0800db20 (Func_daf0+0x30)"},
    {"addr": 0x0800F9CC, "mode": "thumb", "note": "observed resume at 0x0800f9e6 (Func_f9cc+0x1a)"},
    {"addr": 0x0800FEC8, "mode": "thumb", "note": "observed resume at 0x0800ff2c (Func_fec8+0x64)"},
    {"addr": 0x08016178, "mode": "thumb", "note": "observed resume at 0x080161fc (Func_16178+0x84)"},
    {"addr": 0x0808B868, "mode": "thumb", "note": "observed resume at 0x0808b8a0 (Func_8b868+0x38)"},
    {"addr": 0x080D41A4, "mode": "thumb", "note": "observed resumes at 0x080d4434, 0x080d452e and 0x080d457c (Func_d41a4)"},
    # session_20260923_202001; miss PC lies inside the sized ELF STT_FUNC
    # listed (thumb). Resolved with tools/resolve_miss_functions.py.
    {"addr": 0x080F377C, "mode": "thumb", "note": "observed resume at 0x080f3798 (Func_f377c+0x1c)"},
)

# Explicit mid-function resume entries require runtime evidence. This PC is the
# first GS-010 strict-static boundary: native and mGBA architectural state agree
# through the preceding instruction at 0x080047AC, whose tick reaches VBlank;
# the headless runtime then unwinds before executing 0x080047AE. The containing
# THUMB body begins at 0x080047A4. See docs/history/ORACLE_HANDOFF_BASELINE.md.
REVIEWED_RESUME_POINTS = (
    {
        "addr": 0x03000C70,
        "source_addr": 0x080013E0,
        "mode": "arm",
        "name": "resume_03000c70",
        "note": (
            "Func_dc8 after its third self-modifying slot: every block copied "
            "over 0x03000bd8 by 0x080037d4 ends at 0x03000c70"
        ),
    },
    {
        "addr": 0x03000954,
        "source_addr": 0x080010C4,
        "mode": "arm",
        "name": "resume_03000954",
        "note": (
            "GS-011 strict-static scheduler resume at Func_dc8+0x2fc; "
            "ELF/PT_LOAD maps ARM LDRSBNE to ROM 0x080010c4"
        ),
    },
    {
        "addr": 0x080103A6,
        "mode": "thumb",
        "name": "resume_080103a6",
        "note": (
            "GS-011 strict-static IRQ resume at Func_10230+0x176; ELF "
            "decodes this PC as THUMB ADDS"
        ),
    },
    {
        "addr": 0x0300139C,
        "source_addr": 0x08001B0C,
        "mode": "arm",
        "name": "resume_0300139c",
        "note": (
            "GS-011 strict-static IRQ resume at Func_1af8+0x14; ELF/PT_LOAD "
            "maps ARM STMIA 0x0300139c to ROM 0x08001b0c"
        ),
    },
    # The Func_2544 image DMA-copied to 0x0300347c is the other long-running
    # flash driver, and it produced interior resumes at +0x50, +0x144 and
    # +0x298 across three separate builds before this range replaced them.
    # Like the Func_1dc8 image it is reached through an [[extra_func]]
    # `source_addr` rather than a declared [[code_copy]] span, so
    # [[resume_range]] cannot describe it and the extent is aliased word by
    # word. ELF carries a single `$a` at 0x08002544 and no interior `$d`
    # through 0x08002808, so the whole 0x2c4-byte copy is ARM code.
    *(
        {
            "addr": 0x0300347C + (rom_addr - 0x08002544),
            "source_addr": rom_addr,
            "mode": "arm",
            "name": f"resume_{0x0300347C + (rom_addr - 0x08002544):08x}",
            "note": (
                "GS-011 IRQ/VBlank resume inside the Func_2544 flash driver; "
                "DMA copy 0x0300347c..0x03003740 maps this PC to ROM "
                f"0x{rom_addr:08x}"
            ),
        }
        for rom_addr in range(0x08002548, 0x08002808, 4)
    ),
    {
        "addr": 0x080F2C06,
        "mode": "thumb",
        "name": "resume_080f2c06",
        "note": (
            "GS-011 strict-static IRQ resume at Func_f2b70+0x96 inside "
            "ELF-bounded THUMB code"
        ),
    },
    # Func_6878 spins two identical four-instruction flash-timing delay loops,
    # 0x080068ac..0x080068b6 and 0x080068f8..0x08006902. A VBlank IRQ can land
    # on any instruction of either loop, so the headless runtime re-enters at
    # any of their aligned THUMB boundaries; the crawl produced one wall per
    # build until every boundary was covered. Every PC below is an exact
    # instruction start that ELF disassembly of the bounded function
    # 0x08006878..0x08006910 decodes as THUMB, with no `$d` transition in
    # either loop. The finder already roots host bodies at 0x080068ac,
    # 0x080068b0, 0x080068b4, 0x080068f8, 0x080068fc, 0x080068fe and
    # 0x08006900, so only the remaining five need explicit aliases.
    *(
        {
            "addr": addr,
            "mode": "thumb",
            "name": f"resume_{addr:08x}",
            "note": (
                "GS-011 strict-static IRQ resume inside a Func_6878 "
                f"flash-timing delay loop; ELF decodes 0x{addr:08x} as THUMB "
                f"{mnemonic}"
            ),
        }
        for addr, mnemonic in (
            (0x080068AE, "SUBS r0,#1"),
            (0x080068B2, "LDRH r0,[r1]"),
            (0x080068B4, "CMP r0,#0"),
            (0x080068B6, "BNE 0x080068ac"),
            (0x080068FA, "SUBS r0,#1"),
            (0x08006902, "BNE 0x080068f8"),
        )
    ),
    # The Func_1dc8 image DMA-copied to 0x0300387c is a flash driver that spins
    # for milliseconds, so a VBlank resume can land on any of its ARM words —
    # first inside the `ldmdb` loop at 0x03003890, later inside the subroutine
    # at 0x030038d4. `[[resume_range]]` cannot express this: the image is
    # reached through an [[extra_func]] `source_addr`, not a declared
    # [[code_copy]] span, so the whole code extent is aliased word by word.
    # The copy is verbatim, so the bias 0x0300387c - 0x08001dc8 maps each PC to
    # a ROM word inside the ELF-bounded Func_1dc8 (0x08001dc8, size 0xe0). Its
    # only `$d` span is the trailing literal pool at 0x08001e98, excluded here.
    *(
        {
            "addr": 0x0300387C + (rom_addr - 0x08001DC8),
            "source_addr": rom_addr,
            "mode": "arm",
            "name": f"resume_{0x0300387C + (rom_addr - 0x08001DC8):08x}",
            "note": (
                "GS-011 strict-static VBlank resume inside the Func_1dc8 "
                "flash driver; DMA copy 0x0300387c..0x0300395c maps this PC to "
                f"ROM 0x{rom_addr:08x}"
            ),
        }
        for rom_addr in range(0x08001DCC, 0x08001E98, 4)
    ),
    {
        "addr": 0x080068FE,
        "mode": "thumb",
        "name": "resume_080068fe",
        "note": (
            "GS-011 strict-static IRQ resume at Func_6878+0x86; ELF bounds "
            "0x08006878..0x08006910 and decodes this PC as THUMB LDRH"
        ),
    },
    {
        "addr": 0x08006900,
        "mode": "thumb",
        "name": "resume_08006900",
        "note": (
            "GS-011 strict-static IRQ resume at Func_6878+0x88; ELF bounds "
            "0x08006878..0x08006910 and decodes this PC as THUMB CMP"
        ),
    },
    {
        "addr": 0x080047AE,
        "mode": "thumb",
        "name": "resume_080047ae",
        "note": "GS-010 oracle-proven VBlank resume inside function 0x080047a4",
    },
    {
        "addr": 0x08003650,
        "mode": "thumb",
        "name": "resume_08003650",
        "note": "GS-011 oracle-proven IRQ callback resume inside function 0x0800360e",
    },
    {
        "addr": 0x080FAA3C,
        "mode": "thumb",
        "name": "resume_080faa3c",
        "note": "GS-011 oracle-proven IRQ resume inside function 0x080faa38",
    },
    {
        "addr": 0x0800FCA0,
        "mode": "thumb",
        "name": "resume_0800fca0",
        "note": (
            "GS-011 strict-static Thumb continuation inside Func_fb38 after "
            "interworking through ARM helper Func_888"
        ),
    },
    {
        "addr": 0x0800FCB0,
        "mode": "thumb",
        "name": "resume_0800fcb0",
        "note": (
            "GS-011 strict-static second Thumb continuation inside Func_fb38 "
            "after interworking through ARM helper Func_888"
        ),
    },
    *(
        {
            "addr": addr,
            "mode": "thumb",
            "name": f"resume_{addr:08x}",
            "note": (
                "GS-011 oracle-proven Thumb continuation after Func_1cc50 "
                "interworks through ARM helper Func_888"
            ),
        }
        for addr in (0x0801CC64, 0x0801CC74, 0x0801CC84)
    ),
)

# Static entries omitted by the ELF symbol table require both an observed
# control-flow target/mode and an independently proven immutable source
# mapping. GS-011 reaches the IWRAM IRQ handler through the real BIOS vector:
# native and mGBA agree on PC 0x03000000 with CPSR.T clear. The containing
# executable PT_LOAD maps that runtime address to ROM 0x08000770.
REVIEWED_STATIC_SEEDS = (
    {
        "addr": 0x0800CACC,
        "source_addr": 0x0800CACC,
        "mode": "thumb",
        "name": "Func_cacc",
        "note": (
            "GS-011 strict-static function-pointer target; ELF STT_FUNC/$t "
            "proves the 0x0800cacc THUMB entry"
        ),
    },
    {
        "addr": 0x080908E0,
        "source_addr": 0x080908E0,
        "mode": "thumb",
        "name": "Func_908e0",
        "note": (
            "GS-011 strict-static function-pointer target through "
            "_call_via_r0; ELF STT_FUNC/$t proves the 0x080908e0 THUMB entry"
        ),
    },
    {
        "addr": 0x0808A360,
        "source_addr": 0x0808A360,
        "mode": "thumb",
        "name": "_Func_91dc8",
        "note": (
            "GS-011 strict-static verified-overlay callback target; live "
            "__Func_91dc8 veneer exchanges to 0x0808a361 and ELF proves the "
            "0x0808a360 THUMB entry"
        ),
    },
    {
        "addr": 0x080000D0,
        "source_addr": 0x080000D0,
        "mode": "thumb",
        "name": "_Func_41d8",
        "note": (
            "GS-011 strict-static verified-overlay callback target; live "
            "__Func_41d8 veneer exchanges to 0x080000d1 and ELF proves the "
            "0x080000d0 THUMB entry"
        ),
    },
    {
        "addr": 0x0808A080,
        "source_addr": 0x0808A080,
        "mode": "thumb",
        "name": "_Func_92054",
        "note": (
            "GS-011 strict-static verified-overlay callback target; live "
            "__Func_92054 veneer exchanges to 0x0808a081 and ELF proves the "
            "0x0808a080 THUMB entry"
        ),
    },
    {
        "addr": 0x080001A8,
        "source_addr": 0x080001A8,
        "mode": "thumb",
        "name": "_Func_5340",
        "note": (
            "GS-011 strict-static verified-overlay callback target; ELF $t "
            "entry loads 0x08005341 and branches through r4"
        ),
    },
    {
        "addr": 0x08000290,
        "source_addr": 0x08000290,
        "mode": "thumb",
        "name": "_Func_2f40",
        "note": (
            "GS-011 strict-static verified-overlay callback target; ELF $t "
            "entry loads 0x08002f41 and branches through r4"
        ),
    },
    {
        "addr": 0x03000000,
        "source_addr": 0x08000770,
        "mode": "arm",
        "name": "irq_handler_03000000",
        "note": "GS-011 oracle-proven IRQ target; source mapping from executable PT_LOAD",
    },
    {
        "addr": 0x080F9A18,
        "source_addr": 0x080F9A18,
        "mode": "thumb",
        "name": "Func_f9a18",
        "note": (
            "GS-011 oracle-proven indirect target and ELF STT_FUNC/$t entry; "
            "corpus unresolved only because declared extent includes trailing "
            "$d padding at 0x080f9a2e"
        ),
    },
    {
        "addr": 0x080FAE58,
        "source_addr": 0x080FAE58,
        "mode": "thumb",
        "name": "Func_fae58",
        "note": (
            "GS-011 oracle-proven function-pointer target and ELF STT_FUNC/$t "
            "entry; corpus unresolved only because the function contains "
            "ELF-marked literal pools"
        ),
    },
    {
        "addr": 0x03002000,
        "source_addr": 0x08015430,
        "mode": "arm",
        "name": "copied_Func_15430_03002000",
        "note": (
            "GS-011 oracle-proven transient IWRAM target; DMA3 copies 0x50 "
            "words (0x140 bytes) from ROM 0x08015430 and runner SHA-1 guards "
            "the active image before dispatch"
        ),
    },
    {
        "addr": 0x03002140,
        "source_addr": 0x08015570,
        "mode": "arm",
        "name": "copied_Func_15570_03002140",
        "note": (
            "GS-011 oracle-proven transient IWRAM target; DMA3 copies 0x18 "
            "words (0x60 bytes) from ROM 0x08015570 and runner SHA-1 guards "
            "the active image before dispatch"
        ),
    },
    {
        "addr": 0x03002400,
        "source_addr": 0x08001DC8,
        "mode": "arm",
        "name": "copied_Func_1dc8_03002400",
        "note": (
            "GS-011 oracle-proven transient IWRAM target; DMA3 copies 0x38 "
            "words (0xe0 bytes) from ROM 0x08001dc8 and runner SHA-1 guards "
            "the active image before dispatch"
        ),
    },
    {
        "addr": 0x0300387C,
        "source_addr": 0x08001DC8,
        "mode": "arm",
        "name": "copied_Func_1dc8_0300387c",
        "note": (
            "GS-011 strict-static trace proves DMA3 copies 0x38 words "
            "(0xe0 bytes) from ROM 0x08001dc8 to 0x0300387c; runner SHA-1 "
            "guards the active image before dispatch"
        ),
    },
    {
        "addr": 0x0300347C,
        "source_addr": 0x08002544,
        "mode": "arm",
        "name": "copied_Func_2544_0300347c",
        "note": (
            "GS-011 strict-static trace proves DMA3 copies 0xb1 words "
            "(0x2c4 bytes) from ROM 0x08002544 to 0x0300347c; runner SHA-1 "
            "guards the active image before dispatch"
        ),
    },
    {
        "addr": 0x03000164,
        "source_addr": 0x080008D4,
        "mode": "arm",
        "name": "Func_8d4",
        "note": (
            "GS-011 oracle-proven indirect target and ELF STT_FUNC/$a entry; "
            "corpus unresolved only because Func_8d4 contains alternate entry "
            "Func_8d8 at 0x03000168"
        ),
    },
    {
        "addr": 0x03000168,
        "source_addr": 0x080008D8,
        "mode": "arm",
        "name": "Func_8d8",
        "note": (
            "GS-011 oracle-proven indirect target and ELF STT_FUNC/$a entry; "
            "corpus unresolved only because its range overlaps alternate "
            "entry Func_8d4 at 0x03000164"
        ),
    },
    # Func_1af8 masks r2 to one of 0x00..0xe0 in steps of 0x20, subtracts
    # that value from 0xf0, then executes ``add pc, r12, lsr #2`` at runtime
    # 0x03001394. With the ARM visible PC of 0x0300139c, this produces exactly
    # the eight entries below into its unrolled LDM/STM copy sequence.
    *(
        {
            "addr": addr,
            "source_addr": 0x08000770 + (addr - 0x03000000),
            "mode": "arm",
            "name": f"Func_1af8_computed_entry_{addr:08x}",
            "note": (
                "GS-011 hash-matched Func_1af8 computed-entry target derived "
                "from 0x0300138c..0x03001394 and executable PT_LOAD mapping"
                + (
                    "; independently observed by strict-static execution"
                    if addr in (0x030013A8, 0x030013D8)
                    else ""
                )
            ),
        }
        for addr in range(0x030013A0, 0x030013E0, 8)
    ),
    # Func_8d8 uses ``add pc, pc, r12, lsr #3`` at runtime 0x03000190
    # after reducing r12 to 0xe0 - (r1 & 0xe0).  The decoded, hash-matched
    # instruction therefore has exactly these eight possible ARM targets.
    # They enter the consecutive STMIA sequence in the same immutable PT_LOAD
    # image; 0x030001b4 is also independently observed in the GS-011 oracle.
    *(
        {
            "addr": addr,
            "source_addr": 0x08000770 + (addr - 0x03000000),
            "mode": "arm",
            "name": f"Func_8d8_computed_entry_{addr:08x}",
            "note": (
                "GS-011 hash-matched Func_8d8 computed-entry target derived "
                "from 0x03000188..0x03000190 and executable PT_LOAD mapping"
                + (
                    "; independently reached by mGBA with CPSR.T clear"
                    if addr == 0x030001B4
                    else ""
                )
            ),
        }
        for addr in range(0x03000198, 0x030001B8, 4)
    ),
)

# GS-011 self-heal discovery observed each exact ROM PC as a THUMB dispatch
# miss, then compiled it natively from the hash-verified immutable ROM. These
# are reviewed bootstrap roots for finder gaps; they are not inferred ranges.
REVIEWED_DISPATCH_MISS_SEEDS = tuple(
    {
        "addr": addr,
        "source_addr": addr,
        "mode": "thumb",
        "name": f"observed_thumb_entry_{addr:08x}",
        "note": (
            "GS-011 observed THUMB dispatch miss; on-demand finder compiled "
            "this exact hash-matched ROM entry natively"
        ),
    }
    for addr in (
        0x08000298,
        0x080002A8,
        0x080002D0,
        0x0801011C,
        0x08010128,
        0x080102C8,
        0x080102D8,
        0x08011CE0,
        0x08015318,
        0x080160FC,
        0x0808A010,
        0x0808B28C,
        0x080F9A50,
        0x080F2018,
        0x080F2020,
        0x080F9A6E,
        0x080F9AE0,
        0x080F9B60,
        0x080F9B66,
        0x080F9B74,
        0x080F9B8E,
        0x080F9B96,
        0x080F9B9E,
        0x080F9BA4,
        0x080F9BAA,
        0x080F9BF4,
        0x080F9BFA,
        0x080F9F6C,
        0x080F9FB0,
        0x080F9FB2,
        0x080FA0B2,
        0x080FA0C0,
        0x080FA0DA,
        0x080FA100,
        0x080FA10C,
        0x080FA144,
        0x080FA1D4,
        0x080FA1E8,
        0x080FACF8,
    )
)

# The decompression inner loop constructs one of three ARM instruction images
# in IWRAM. At 0x030007FC it sets the write cursor to 0x03000828; the loop
# through 0x03000818 copies one of the ELF-marked templates at 0x030008D4,
# 0x030008EC, or 0x03000904 across the placeholder span ending at 0x030008B0.
# Control then enters the synthesized image at 0x03000828. The immutable
# 0xFEDCBA98 words remain data; this declaration only creates an honest
# external dispatch boundary for an identity-aware runtime hook.
REVIEWED_RUNTIME_CODE_ENTRIES = (
    {
        "addr": 0x03000828,
        "mode": "arm",
        "note": (
            "GS-011 ELF/ROM-proven self-modifying entry; writer "
            "0x030007fc..0x03000818 selects template 0x030008d4/0x030008ec/"
            "0x03000904 for runtime span 0x03000828..0x030008b0"
        ),
    },
    {
        "addr": 0x03000A5C,
        "mode": "arm",
        "note": (
            "Func_dc8 second self-modifying slot; writer 0x03000a20..0x03000a50 "
            "copies table 0x030009c4 + (r3 & 3) * 8 into 0x03000a5c..0x03000a84 "
            "of the loop 0x03000a58..0x03000a94"
        ),
    },
    {
        "addr": 0x03000BD8,
        "mode": "arm",
        "note": (
            "Func_dc8 third self-modifying slot; 0x080037d4 copies one of five "
            "0x98-byte blocks from 0x08000404 over 0x03000bd8..0x03000c70"
        ),
    },
)

# The second slot's placeholders are ROM `mov r0, r0` words the ELF marks as
# code; the writer above replaces all four pairs before the loop runs, so the
# ROM words are never executed. Declared data so the main corpus stops at the
# slot instead of baking the placeholders in (the recompiler requires a
# runtime_code_entry to begin inside such a range).
REVIEWED_RUNTIME_PLACEHOLDER_RANGES = (
    (0x03000A5C, 0x03000A64),
    (0x03000A68, 0x03000A70),
    (0x03000A74, 0x03000A7C),
    (0x03000A80, 0x03000A88),
    # Third slot: the whole block is replaced (0x080037D4); the boot copy's
    # block 2 is one of the five registered variants, never AOT code.
    (0x03000BD8, 0x03000C70),
)

# Func_77320 bounds the selector to 0..5 before loading one absolute word
# from 0x080779c4 and moving it to PC.  The table is ELF-marked `$d`; every
# decoded target below is ELF-marked `$t`.  GS-011 strict-static execution and
# mGBA both observe the first target, 0x080779dc.
REVIEWED_JUMP_TABLES = (
    {
        "addr": 0x080779C4,
        "stride": 4,
        "count": 6,
        "format": "abs32",
        "entries_mode": "thumb",
        "name": "Func_77320_state_dispatch",
        "targets": (
            0x080779DC,
            0x080779E0,
            0x080779EA,
            0x080779F2,
            0x08077A28,
            0x08077A20,
        ),
        "note": (
            "GS-011 hash-matched six-way switch; selector bound at "
            "0x080779b8 and ELF $d/$t mappings prove table and target modes"
        ),
    },
    {
        "addr": 0x0808FF24,
        "stride": 4,
        "count": 5,
        "format": "abs32",
        "entries_mode": "thumb",
        "name": "Func_8fefc_type_dispatch",
        "targets": (
            0x0808FF38,
            0x0808FF4C,
            0x0808FFA2,
            0x0809003C,
            0x080900C0,
        ),
        "note": (
            "GS-011 hash-matched five-way switch; selector bound at "
            "0x0808ff18 and ELF $d/$t mappings prove table and target modes"
        ),
    },
)


# Golden Sun dispatches object behaviour through two ELF-bounded tables of
# absolute THUMB function pointers. `Data_13624` (0x08013624, 188 bytes, 47
# words) is loaded as a literal by Func_a494 (pool 0x0800a848) and by Func_cacc
# (pool 0x0800cd84); `Data_136e0` (0x080136e0, 164 bytes, 41 words) is loaded
# by Func_e9a0, Func_e9dc and Func_ea18 (pools 0x0800e9d8, 0x0800ea14,
# 0x0800ea50), all three of which are themselves entries in `Data_13624`.
#
# Every word inside both ELF-declared extents is odd, lies in ROM, and lands on
# an exact ELF STT_FUNC entry carrying a `$t` mapping symbol.
# `verify_reviewed_pointer_tables` re-checks the literal words against the
# hash-verified ROM, so a different image aborts instead of seeding silently.
#
# GS-011 strict-static stopped at one of these targets, 0x0800d7e8, in the run
# immediately after Func_cacc was seeded. 49 of the 74 distinct targets are
# absent from the corpus proven set purely because their declared extents carry
# ELF-marked literal pools (`confidence = "unresolved"` in
# import_main_symbols.py), which is a corpus limitation, not missing evidence.
REVIEWED_POINTER_TABLES = (
    {
        "addr": 0x08013624,
        "name": "Data_13624",
        "targets": (
            0x0800D655, 0x0800D675, 0x0800D9F1, 0x0800DA19,
            0x0800DA41, 0x0800DA79, 0x0800DAA1, 0x0800DAC1,
            0x0800DD71, 0x0800DF05, 0x0800DCDD, 0x0800DAF1,
            0x0800D711, 0x0800D761, 0x0800D781, 0x0800D7B5,
            0x0800D7E9, 0x0800D7F9, 0x0800D821, 0x0800D851,
            0x0800D881, 0x0800E9A1, 0x0800E9DD, 0x0800EA19,
            0x0800EBED, 0x0800F7F5, 0x0800F2F9, 0x0800D8E9,
            0x0800D8C5, 0x0800D8F5, 0x0800D901, 0x0800CA2D,
            0x0800CA45, 0x0800CA59, 0x0800D6A5, 0x0800F7DD,
            0x0800D7E9, 0x0800D7E9, 0x0800D7E9, 0x0800D7E9,
            0x0800D7E9, 0x0800D7E9, 0x0800D7E9, 0x0800D7E9,
            0x0800D7E9, 0x0800D7E9, 0x0800D7E9,
        ),
    },
    {
        "addr": 0x080136E0,
        "name": "Data_136e0",
        "targets": (
            0x0800E221, 0x0800E24D, 0x0800E281, 0x0800E281,
            0x0800E281, 0x0800E2B1, 0x0800E2DD, 0x0800E309,
            0x0800E335, 0x0800E365, 0x0800E391, 0x0800E391,
            0x0800E3BD, 0x0800E3E9, 0x0800E415, 0x0800E441,
            0x0800E46D, 0x0800E499, 0x0800E4C5, 0x0800E4F1,
            0x0800E51D, 0x0800E549, 0x0800E575, 0x0800E5A1,
            0x0800E5CD, 0x0800E5F9, 0x0800E635, 0x0800E671,
            0x0800E6AD, 0x0800E6E5, 0x0800E721, 0x0800E75D,
            0x0800E799, 0x0800E7D5, 0x0800E811, 0x0800E851,
            0x0800E891, 0x0800E8D1, 0x0800E8FD, 0x0800E929,
            0x0800E965,
        ),
    },
)


# GS-011 execution is now inside the `rom_9000` cluster (0x08009000..0x0801404c):
# Func_cacc, Func_c62c, the Data_13624/Data_136e0 handler tables and everything
# they reach live there. Every wall since the Func_8fefc jump table has been the
# same shape — an exact ELF STT_FUNC/$t entry that the corpus files as
# `confidence = "unresolved"` purely because its declared extent contains
# ELF-marked literal pools (see `function_transitions` in
# import_main_symbols.py), so the finder never reaches it and nothing seeds it.
#
# That is a corpus conservatism, not missing evidence: those symbols passed
# every entry-level check (symtab value, mapping-symbol mode, PT_LOAD source,
# alignment, non-overlap) and differ from the proven set only in their
# interiors. Seeding the whole cluster at once replaces a one-function-per-build
# crawl.
#
# Scope is every executable ROM section from the reset vector through
# `rom_8a000`, plus `rom_a1000` and `rom_c9000`. `rom_a1000` and `rom_c9000`
# were previously held back by a finder defect: the compiler emits long
# intra-function branches at 0x080ac342 (in `rom_a1000`) and 0x080e578a /
# 0x080e5ac6 (in `rom_c9000`) as `BL`, and the finder treated a `BL` as a
# returning call, so it resumed decoding at literal-pool bytes correctly
# marked `$d`. That defect is now fixed upstream in `gbarecomp`: the finder
# recognises a `BL` whose CONTINUATION address `pc + 4` lands inside a declared
# `data_range` as non-returning and does not enqueue it. (The test is on the
# continuation, not the branch target, and on the declared data_range, not the
# caller's ELF extent - a returning call cannot have a literal pool as its
# return address, which is what makes the collision proof rather than a
# heuristic.) Both sections are therefore seeded here; the acceptance gate is
# the collision-free regeneration this change was made under, not a re-review
# of every symbol in the two sections.
#
# `rom_770` is the IWRAM image already covered by [[code_copy]] and its own
# explicit reviewed entries, so it is not seeded wholesale here.
REVIEWED_SEED_SECTIONS = (
    "rom_c0",
    "rom_1b70",
    "rom_9000",
    "rom_15000",
    "rom_77000",
    "rom_8a000",
    "rom_a1000",
    "rom_c9000",
    "rom_b5000",
    "rom_b0000",
    "rom_f6000",
    "rom_f9000",
    "rom_f2000",
    "rom_f4000",
    "rom_f0000",
    "rom_185000",
)

# THUMB code calls an ARM callee through the two-instruction veneer
# `MOV ip,pc; BX rN`: `ip` reads as the site address plus four, the ARM helper
# (Func_888 at runtime 0x03000118) returns to that address with the T bit set,
# and execution continues at site+4. The continuation is straight-line THUMB
# code inside the same ELF-bounded function, but it is only ever reached by an
# inter-mode dispatch, so it needs an explicit entry — the same situation as the
# already reviewed 0x0800fca0, 0x0800fcb0 and 0x0801cc64/74/84 resumes. GS-011
# strict-static stopped at exactly one of these, 0x0800cc94 (Func_cacc+0x1c8),
# with r12 = 0x0800cc94 on the exchange into Func_888.
#
# Both the continuations and the `0x0000` alignment pads that precede odd-aligned
# veneers are derived by decoding REVIEWED_SEED_SECTIONS out of the hash-verified
# ROM, not by hand-listing addresses, so a seeded function can never arrive
# without the entries its own interworking calls require.

THUMB_MOV_IP_PC = 0x46FC
THUMB_BX_MASK = 0xFF87
THUMB_BX_VALUE = 0x4700

# The linker emits a fixed two-instruction interworking veneer,
# `_Func_<target>`, wherever a call site could not reach its callee's mode
# directly: `ldr r4,[pc,#0]` loads the absolute target from the word that
# follows, then `bx r4` exchanges to it. That word is `$d` data and the
# veneer's own entry (`+0x0`) is an ordinary dispatchable STT_FUNC symbol, but
# `+0x2` — the `bx r4` halfword — is only ever reached by falling out of
# `+0x0`, never by a discovery walk that starts there, so it needs the same
# explicit resume treatment as the `MOV ip,pc; BX rN` continuations above.
# GS-011 self-heal discovery hand-reviewed exactly one of these the expensive
# way (_Func_92054 @ 0x0808a080, resume 0x0808a082, in
# REVIEWED_STATIC_SEEDS); `scan_veneer_stubs` derives the rest of the class
# from the hash-verified ELF/ROM instead of listing them one build at a time.
THUMB_LDR_R4_PC0 = 0x4C00
THUMB_BX_R4 = 0x4720


class ProposalError(ValueError):
    """Raised when the inputs cannot support a conservative proposal."""


def decode_cartridge_entry(rom_prefix: bytes) -> int:
    """Decode the ARM B instruction at the cartridge reset vector."""

    if len(rom_prefix) < 4:
        raise ProposalError("ROM is too short to contain a reset instruction")
    word = struct.unpack_from("<I", rom_prefix)[0]
    if word & 0x0E000000 != 0x0A000000:
        raise ProposalError("cartridge reset instruction is not an ARM branch")
    displacement = word & 0x00FFFFFF
    if displacement & 0x00800000:
        displacement -= 0x01000000
    return (ROM_BASE + 8 + displacement * 4) & 0xFFFFFFFF


def derive_code_copies(image: ElfImage) -> list[dict[str, int]]:
    """Return file-backed PT_LOAD mappings from ROM source to non-ROM runtime."""

    copies: list[dict[str, int]] = []
    rom_end = ROM_BASE + EXPECTED_SIZE
    for header in image.program_headers:
        if header.header_type != PT_LOAD or header.file_size == 0:
            continue
        if ROM_BASE <= header.virtual_address < rom_end:
            continue
        source_end = header.physical_address + header.file_size
        if not (ROM_BASE <= header.physical_address < source_end <= rom_end):
            continue
        if header.memory_size != header.file_size:
            raise ProposalError(
                "relocated executable PT_LOAD has different file and memory sizes"
            )
        copies.append(
            {
                "runtime_start": header.virtual_address,
                "source_start": header.physical_address,
                "size": header.file_size,
            }
        )
    copies.sort(key=lambda item: (item["runtime_start"], item["source_start"]))
    for previous, current in zip(copies, copies[1:]):
        if previous["runtime_start"] + previous["size"] > current["runtime_start"]:
            raise ProposalError("relocated executable PT_LOAD ranges overlap")
    return copies


def _merge_ranges(ranges: list[tuple[int, int]]) -> list[tuple[int, int]]:
    merged: list[tuple[int, int]] = []
    for start, end in sorted(ranges):
        if start >= end:
            raise ProposalError(f"invalid data range 0x{start:08x}..0x{end:08x}")
        if merged and start <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], end))
        else:
            merged.append((start, end))
    return merged


def apply_data_exceptions(
    ranges: list[tuple[int, int]], exceptions: list[dict[str, Any]]
) -> list[tuple[int, int]]:
    """Subtract reviewed executed spans from ELF-declared data ranges."""

    current = list(ranges)
    seen: set[tuple[int, int]] = set()
    for exception in exceptions:
        start = int(exception["start"], 16)
        end = int(exception["end"], 16)
        key = (start, end)
        if key in seen:
            raise ProposalError(f"duplicate data exception 0x{start:08x}")
        seen.add(key)
        if exception.get("mode") != "thumb" or end - start != 2 or start % 2:
            raise ProposalError(
                f"data exception 0x{start:08x} is not one aligned THUMB instruction"
            )
        if not exception.get("evidence"):
            raise ProposalError(f"data exception 0x{start:08x} has no evidence note")
        containing = [item for item in current if item[0] <= start and end <= item[1]]
        if len(containing) != 1:
            raise ProposalError(
                f"data exception 0x{start:08x} is not inside exactly one data range"
            )
        source = containing[0]
        current.remove(source)
        if source[0] < start:
            current.append((source[0], start))
        if end < source[1]:
            current.append((end, source[1]))
        current.sort()
    return current


def subtract_jump_table_ranges(
    ranges: list[tuple[int, int]], tables: tuple[dict[str, Any], ...]
) -> list[tuple[int, int]]:
    """Remove reviewed table bytes that ``[[jump_table]]`` excludes itself."""

    current = list(ranges)
    for table in tables:
        start = table["addr"]
        end = start + table["stride"] * table["count"]
        containing = [item for item in current if item[0] <= start and end <= item[1]]
        if len(containing) != 1:
            raise ProposalError(
                f"jump table 0x{start:08x} is not inside exactly one data range"
            )
        source = containing[0]
        current.remove(source)
        if source[0] < start:
            current.append((source[0], start))
        if end < source[1]:
            current.append((end, source[1]))
        current.sort()
    return current


def verify_reviewed_jump_tables(rom: bytes) -> None:
    """Require the supported ROM to contain every reviewed absolute target."""

    for table in REVIEWED_JUMP_TABLES:
        offset = table["addr"] - ROM_BASE
        actual = struct.unpack_from(f"<{table['count']}I", rom, offset)
        if actual != table["targets"]:
            raise ProposalError(
                f"jump table 0x{table['addr']:08x} target bytes changed"
            )


def verify_reviewed_pointer_tables(rom: bytes) -> None:
    """Require the supported ROM to contain every reviewed pointer-table word."""

    for table in REVIEWED_POINTER_TABLES:
        offset = table["addr"] - ROM_BASE
        count = len(table["targets"])
        actual = struct.unpack_from(f"<{count}I", rom, offset)
        if actual != table["targets"]:
            raise ProposalError(
                f"pointer table 0x{table['addr']:08x} target bytes changed"
            )


def reviewed_section_ranges(image: ElfImage) -> list[tuple[int, int]]:
    """Return the address extents of the reviewed seed sections."""

    ranges = [
        (section.address, section.address + section.size)
        for section in image.sections
        if section.name in REVIEWED_SEED_SECTIONS and section.size
    ]
    if len(ranges) != len(REVIEWED_SEED_SECTIONS):
        raise ProposalError("a reviewed seed section is missing from the ELF")
    return sorted(ranges)


def scan_interwork_sites(rom: bytes, image: ElfImage) -> list[int]:
    """Decode every `MOV ip,pc; BX rN` veneer in the reviewed seed sections."""

    sites: list[int] = []
    for start, end in reviewed_section_ranges(image):
        for site in range(start, end - 4, 2):
            first, second = struct.unpack_from("<2H", rom, site - ROM_BASE)
            if first != THUMB_MOV_IP_PC:
                continue
            if second & THUMB_BX_MASK != THUMB_BX_VALUE:
                continue
            sites.append(site)
    return sites


def derive_interwork_resume_points(sites: list[int]) -> list[dict[str, Any]]:
    """Return THUMB continuations for reviewed `MOV ip,pc; BX rN` veneers."""

    rom_end = ROM_BASE + EXPECTED_SIZE
    resumes: list[dict[str, Any]] = []
    seen: set[int] = set()
    for site in sorted(sites):
        if site % 2 or not ROM_BASE <= site + 4 < rom_end:
            raise ProposalError(f"interworking site 0x{site:08x} is not in THUMB ROM")
        resume = site + 4
        if resume in seen:
            raise ProposalError(f"duplicate interworking resume 0x{resume:08x}")
        seen.add(resume)
        resumes.append(
            {
                "addr": resume,
                "mode": "thumb",
                "name": f"resume_{resume:08x}",
                "note": (
                    "GS-011 derived THUMB continuation after the interworking "
                    f"veneer at 0x{site:08x}; hash-matched ROM decodes "
                    "MOV ip,pc; BX rN and the ARM helper returns to this PC"
                ),
            }
        )
    return resumes


def derive_interwork_pad_exceptions(
    rom: bytes,
    sites: list[int],
    ranges: list[tuple[int, int]],
    reviewed_starts: set[int],
) -> list[dict[str, str]]:
    """Return the `0x0000` alignment pads that precede reviewed veneers.

    A `MOV ip,pc` must sit at a word-aligned address for `ip` to read back
    word-aligned, so the compiler emits a `0x0000` halfword (THUMB
    `MOVS r0,r0`) ahead of an odd-aligned veneer. Several such pads carry a `$d`
    mapping symbol even though straight-line control flow executes them, which
    is the same stale-mapping contradiction the hand-reviewed exceptions in
    `config/usa/main-data-exceptions.json` record. These are derived from the
    scanned veneer sites rather than listed by hand so a site can never be
    seeded without its pad; each one is still re-decoded from the hash-verified
    ROM and must fall inside exactly one ELF `$d` range.
    """

    derived: list[dict[str, str]] = []
    for site in sorted(sites):
        pad = site - 2
        if pad in reviewed_starts:
            continue
        if not any(start <= pad and pad + 2 <= end for start, end in ranges):
            continue
        if struct.unpack_from("<H", rom, pad - ROM_BASE)[0] != 0:
            raise ProposalError(
                f"interworking pad 0x{pad:08x} is not a 0x0000 halfword"
            )
        derived.append(
            {
                "start": f"0x{pad:08x}",
                "end": f"0x{pad + 2:08x}",
                "mode": "thumb",
                "evidence": (
                    f"alignment pad ahead of the veneer at 0x{site:08x}; "
                    "hash-matched ROM bytes 0000 decode as THUMB MOVS r0,r0 "
                    "before MOV ip,pc; BX rN and straight-line control flow "
                    "reaches them"
                ),
            }
        )
    return derived


def scan_veneer_stubs(rom: bytes, image: ElfImage) -> list[dict[str, int]]:
    """Find linker-emitted `_Func_<target>` interworking veneer stubs.

    Every criterion is independently checked against the hash-verified
    ROM/ELF; a symbol that merely has the right name or the right size is not
    enough on its own:

    - STT_FUNC, size exactly 8, THUMB (odd) address
    - a `$t` mapping symbol at the entry and a `$d` mapping symbol at entry+4
    - the actual ROM bytes at the entry are `4c00 4720`
      (`ldr r4,[pc,#0]; bx r4`)
    - the word at entry+4 has bit 0 set (a THUMB target)
    """

    mapping_kind: dict[int, str] = {}
    for entries in _mapping_symbols(image).values():
        for address, kind, _symbol_index in entries:
            mapping_kind[address] = kind

    rom_end = ROM_BASE + EXPECTED_SIZE
    stubs: list[dict[str, int]] = []
    seen_entries: set[int] = set()
    for symbol in image.symbols:
        if symbol.symbol_type != STT_FUNC or symbol.size != 8:
            continue
        if not symbol.value & 1:
            continue
        entry = symbol.value & ~1
        if entry in seen_entries:
            continue
        if not (ROM_BASE <= entry and entry + 8 <= rom_end):
            continue
        if mapping_kind.get(entry) != "t":
            continue
        if mapping_kind.get(entry + 4) != "d":
            continue
        first, second = struct.unpack_from("<2H", rom, entry - ROM_BASE)
        if first != THUMB_LDR_R4_PC0 or second != THUMB_BX_R4:
            continue
        target = struct.unpack_from("<I", rom, entry + 4 - ROM_BASE)[0]
        if not target & 1:
            continue
        seen_entries.add(entry)
        stubs.append({"entry": entry, "target": target})
    stubs.sort(key=lambda stub: stub["entry"])
    return stubs


def derive_veneer_resume_points(stubs: list[dict[str, int]]) -> list[dict[str, Any]]:
    """Return the `+0x2` THUMB resume entry for each derived veneer stub."""

    resumes: list[dict[str, Any]] = []
    seen: set[int] = set()
    for stub in stubs:
        resume = stub["entry"] + 2
        if resume in seen:
            raise ProposalError(f"duplicate veneer resume 0x{resume:08x}")
        seen.add(resume)
        resumes.append(
            {
                "addr": resume,
                "mode": "thumb",
                "name": f"resume_{resume:08x}",
                "note": (
                    "GS-011 derived THUMB continuation inside the "
                    f"linker-emitted interworking veneer at 0x{stub['entry']:08x}; "
                    "hash-matched ROM decodes ldr r4,[pc,#0]; bx r4 with a "
                    f"THUMB target word 0x{stub['target']:08x}"
                ),
            }
        )
    return resumes


def derive_pointer_table_seeds(
    image: ElfImage, excluded: set[int]
) -> list[dict[str, Any]]:
    """Return THUMB seeds for reviewed function-pointer tables.

    Every word must independently satisfy the same standard the corpus applies
    to a proven entry: THUMB (odd) ROM pointer, exact STT_FUNC symbol value and
    a `$t` mapping symbol at the entry. Anything weaker aborts the proposal
    rather than seeding an unproven address.
    """

    functions = {
        symbol.value & ~1
        for symbol in image.symbols
        if symbol.symbol_type == STT_FUNC and symbol.value & 1
    }
    thumb_entries: set[int] = set()
    for entries in _mapping_symbols(image).values():
        for address, kind, _symbol_index in entries:
            if kind == "t":
                thumb_entries.add(address)

    rom_end = ROM_BASE + EXPECTED_SIZE
    seeds: list[dict[str, Any]] = []
    seen: set[int] = set()
    for table in REVIEWED_POINTER_TABLES:
        for index, word in enumerate(table["targets"]):
            entry = word & ~1
            slot = table["addr"] + index * 4
            if not word & 1:
                raise ProposalError(f"pointer table slot 0x{slot:08x} is not THUMB")
            if not ROM_BASE <= entry < rom_end:
                raise ProposalError(f"pointer table slot 0x{slot:08x} leaves the ROM")
            if entry not in functions:
                raise ProposalError(
                    f"pointer table slot 0x{slot:08x} is not an ELF STT_FUNC entry"
                )
            if entry not in thumb_entries:
                raise ProposalError(
                    f"pointer table slot 0x{slot:08x} has no $t mapping symbol"
                )
            if entry in seen or entry in excluded:
                continue
            seen.add(entry)
            seeds.append(
                {
                    "addr": entry,
                    "source_addr": entry,
                    "mode": "thumb",
                    "name": f"Func_{entry - ROM_BASE:x}",
                    "note": (
                        "GS-011 reviewed function-pointer target from "
                        f"ELF-bounded {table['name']} at 0x{table['addr']:08x}; "
                        "hash-matched table word and ELF STT_FUNC/$t prove the "
                        "THUMB entry"
                    ),
                }
            )
    seeds.sort(key=lambda seed: seed["addr"])
    return seeds


def derive_resume_ranges(
    image: ElfImage, data_ranges: list[tuple[int, int]]
) -> list[dict[str, Any]]:
    """Turn each reviewed resume function into per-code-run `[[resume_range]]`s.

    A strict-static interior miss is never really about one PC. The headless
    runtime unwinds on VBlank wherever the guest happened to be, so a function
    that yielded once will yield at other instructions on other runs, and
    listing them one build at a time is a crawl with no end. `[[resume_range]]`
    exists for exactly this: declare the reviewed function's extent and let the
    recompiler alias every aligned instruction inside it.

    The declared extent is the ELF STT_FUNC size minus the `$d` spans already
    derived for `[[data_range]]`, so a literal pool inside the function is never
    offered as an entry point and the ranges cannot overlap a data range. Runs
    are split at the schema's 0x1000 cap on aligned boundaries.
    """

    by_address = {
        symbol.value & ~1: symbol
        for symbol in image.symbols
        if symbol.symbol_type == STT_FUNC and symbol.size > 0
    }
    ranges: list[dict[str, Any]] = []
    for reviewed in REVIEWED_RESUME_FUNCTIONS:
        symbol = by_address.get(reviewed["addr"])
        if symbol is None:
            raise ProposalError(
                f"reviewed resume function 0x{reviewed['addr']:08x} has no sized "
                "ELF STT_FUNC entry"
            )
        start = reviewed["addr"]
        end = start + symbol.size
        if end > ROM_BASE + EXPECTED_SIZE:
            raise ProposalError(
                f"reviewed resume function {symbol.name} exceeds the ROM"
            )
        cursor = start
        for data_start, data_end in data_ranges:
            if data_end <= cursor or data_start >= end:
                continue
            if data_start > cursor:
                ranges.append((cursor, min(data_start, end), symbol.name, reviewed))
            cursor = max(cursor, data_end)
            if cursor >= end:
                break
        if cursor < end:
            ranges.append((cursor, end, symbol.name, reviewed))

    emitted: list[dict[str, Any]] = []
    for run_start, run_end, name, reviewed in ranges:
        step = 2 if reviewed["mode"] == "thumb" else 4
        if run_start % step or run_end % step:
            raise ProposalError(
                f"resume run 0x{run_start:08x}..0x{run_end:08x} in {name} is not "
                f"{step}-byte aligned"
            )
        chunk_start = run_start
        while chunk_start < run_end:
            chunk_end = min(chunk_start + RESUME_RANGE_MAX_BYTES, run_end)
            emitted.append(
                {
                    "start": chunk_start,
                    "end": chunk_end,
                    "mode": reviewed["mode"],
                    "note": (
                        f"GS-011 strict-static IRQ resumes inside ELF-bounded "
                        f"{name}; {reviewed['note']}"
                    ),
                }
            )
            chunk_start = chunk_end
    emitted.sort(key=lambda entry: entry["start"])
    return emitted


def derive_data_ranges(image: ElfImage) -> list[tuple[int, int]]:
    """Derive `$d` spans plus the complement of ROM executable sections."""

    mappings = _mapping_symbols(image)
    data_ranges: list[tuple[int, int]] = []
    executable_rom_sections: list[tuple[int, int]] = []
    rom_end = ROM_BASE + EXPECTED_SIZE

    for section in image.sections:
        if not section.flags & SHF_EXECINSTR or section.size == 0:
            continue
        section_end = section.address + section.size
        if ROM_BASE <= section.address < rom_end:
            if section_end > rom_end:
                raise ProposalError(f"executable section {section.name} exceeds the ROM")
            executable_rom_sections.append((section.address, section_end))

        by_address: dict[int, set[str]] = defaultdict(set)
        for address, kind, _symbol_index in mappings.get(section.index, []):
            # Some linked ELF mapping symbols retain a section index while their
            # value is outside that output section. They cannot describe bytes
            # in this section and are deliberately ignored.
            if section.address <= address < section_end:
                by_address[address].add(kind)
        conflicts = [address for address, kinds in by_address.items() if len(kinds) != 1]
        if conflicts:
            raise ProposalError(
                f"conflicting mapping symbols in {section.name} at "
                f"0x{min(conflicts):08x}"
            )
        states = sorted((address, next(iter(kinds))) for address, kinds in by_address.items())
        for index, (start, kind) in enumerate(states):
            end = states[index + 1][0] if index + 1 < len(states) else section_end
            if kind == "d" and start < end:
                data_ranges.append((start, end))

    executable_rom_sections.sort()
    cursor = ROM_BASE
    for start, end in executable_rom_sections:
        if start < cursor:
            raise ProposalError("ROM executable sections overlap")
        if cursor < start:
            data_ranges.append((cursor, start))
        cursor = end
    if cursor < rom_end:
        data_ranges.append((cursor, rom_end))
    return _merge_ranges(data_ranges)


def select_section_seeds(
    corpus: dict[str, Any], excluded: set[int]
) -> list[dict[str, Any]]:
    """Return corpus entries in REVIEWED_SEED_SECTIONS blocked only by interiors.

    A record only reaches the corpus after passing every entry-level check, so a
    `confidence = "unresolved"` record differs from a proven one solely in that
    its declared extent contains further mapping-symbol transitions. Its entry
    address and mode carry exactly the same evidence.
    """

    seeds: list[dict[str, Any]] = []
    for symbol in corpus["symbols"]:
        if symbol["confidence"] != "unresolved":
            continue
        if symbol["section"] not in REVIEWED_SEED_SECTIONS:
            continue
        runtime_address = int(symbol["runtime_address"], 16)
        if runtime_address in excluded:
            continue
        seeds.append(
            {
                "addr": runtime_address,
                "source_addr": int(symbol["source_address"], 16),
                "mode": symbol["mode"],
                "name": symbol["name"],
                "note": (
                    f"GS-011 reviewed {symbol['section']} entry; ELF STT_FUNC, "
                    "mapping-symbol mode and PT_LOAD source all agree and the "
                    "corpus withholds it only for interior mapping transitions"
                ),
            }
        )
    seeds.sort(key=lambda seed: seed["addr"])
    return seeds


def select_proven_seeds(corpus: dict[str, Any], entry: int) -> list[dict[str, Any]]:
    errors = validate_document(corpus)
    if errors:
        raise ProposalError("invalid main corpus: " + "; ".join(errors))
    seeds = [
        symbol
        for symbol in corpus["symbols"]
        if symbol["confidence"] == "proven"
        and int(symbol["runtime_address"], 16) != entry
    ]
    seeds.sort(
        key=lambda symbol: (
            int(symbol["runtime_address"], 16),
            symbol["mode"],
            symbol["name"],
        )
    )
    return seeds


def _toml_string(value: str) -> str:
    return json.dumps(value, ensure_ascii=True)


def render_toml(
    *,
    reset_target: int,
    disassembly_revision: str,
    corpus_sha256: str,
    seeds: list[dict[str, Any]],
    pointer_seeds: list[dict[str, Any]],
    section_seeds: list[dict[str, Any]],
    interwork_resumes: list[dict[str, Any]],
    veneer_resumes: list[dict[str, Any]],
    resume_ranges: list[dict[str, Any]],
    data_ranges: list[tuple[int, int]],
    code_copies: list[dict[str, int]],
    data_exception_sha256: str,
) -> str:
    lines = [
        "# Generated evidence proposal for GS-007. Do not hand-edit addresses.",
        f"# goldensun-disasm revision: {disassembly_revision}",
        f"# main symbol corpus SHA-256: {corpus_sha256}",
        f"# data exception review SHA-256: {data_exception_sha256}",
        f"# reset vector: 0x{ROM_BASE:08x} ARM branch to 0x{reset_target:08x}",
        f"# proven seeds: {len(seeds)}; data ranges: {len(data_ranges)}; code copies: {len(code_copies)}",
        f"# reviewed jump tables: {len(REVIEWED_JUMP_TABLES)}",
        f"# reviewed pointer-table seeds: {len(pointer_seeds)}",
        f"# reviewed section seeds: {len(section_seeds)}",
        f"# reviewed interworking resumes: {len(interwork_resumes)}",
        f"# derived veneer resumes: {len(veneer_resumes)}",
        f"# reviewed conditional branch overrides: "
        f"{len(REVIEWED_CONDITIONAL_BRANCH_OVERRIDES)}",
        f"# reviewed resume ranges: {len(resume_ranges)}"
        f" from {len(REVIEWED_RESUME_FUNCTIONS)} function(s)",
        "",
        "[program]",
        'name = "Golden Sun (USA/Europe) main image"',
        'id = "golden_sun_usa_main"',
        f"load_address = 0x{ROM_BASE:08x}",
        f"size = 0x{EXPECTED_SIZE:08x}",
        f"entry_pc = 0x{ROM_BASE:08x}",
        "speculative_literal_harvest = false",
        "codegen_shards = 32",
        "",
        "[identity]",
        f'sha1 = "{EXPECTED_SHA1}"',
    ]
    for addr, note in REVIEWED_CONDITIONAL_BRANCH_OVERRIDES:
        lines.extend(
            [
                "",
                "[[conditional_branch_override]]",
                f"addr = 0x{addr:08x}",
                f"note = {_toml_string(note)}",
            ]
        )
    for addr, note in REVIEWED_THUMB_ALU_IMMEDIATE_OVERRIDES:
        lines.extend(
            [
                "",
                "[[thumb_alu_immediate_override]]",
                f"addr = 0x{addr:08x}",
                f"note = {_toml_string(note)}",
            ]
        )
    for addr, note in REVIEWED_THUMB_LITERAL_OVERRIDES:
        lines.extend(
            [
                "",
                "[[thumb_literal_override]]",
                f"addr = 0x{addr:08x}",
                f"note = {_toml_string(note)}",
            ]
        )
    for copy in code_copies:
        lines.extend(
            [
                "",
                "[[code_copy]]",
                f"runtime_start = 0x{copy['runtime_start']:08x}",
                f"source_start = 0x{copy['source_start']:08x}",
                f"size = 0x{copy['size']:08x}",
                f'name = "main_copy_{copy["runtime_start"]:08x}"',
                'note = "ELF file-backed PT_LOAD runtime/source mapping"',
            ]
        )
    for start, end in data_ranges:
        lines.extend(
            [
                "",
                "[[data_range]]",
                f"start = 0x{start:08x}",
                f"end = 0x{end:08x}",
                'note = "ELF mapping-symbol data or non-executable ROM complement"',
            ]
        )
    for table in REVIEWED_JUMP_TABLES:
        lines.extend(
            [
                "",
                "[[jump_table]]",
                f"addr = 0x{table['addr']:08x}",
                f"stride = {table['stride']}",
                f"count = {table['count']}",
                f"format = {_toml_string(table['format'])}",
                f"entries_mode = {_toml_string(table['entries_mode'])}",
                f"name = {_toml_string(table['name'])}",
                f"note = {_toml_string(table['note'])}",
            ]
        )
    for entry in REVIEWED_RUNTIME_CODE_ENTRIES:
        lines.extend(
            [
                "",
                "[[runtime_code_entry]]",
                f"addr = 0x{entry['addr']:08x}",
                f"mode = {_toml_string(entry['mode'])}",
                f"note = {_toml_string(entry['note'])}",
            ]
        )
    for symbol in seeds:
        runtime_address = int(symbol["runtime_address"], 16)
        source_address = int(symbol["source_address"], 16)
        lines.extend(
            [
                "",
                "[[extra_func]]",
                f"addr = 0x{runtime_address:08x}",
            ]
        )
        if source_address != runtime_address:
            lines.append(f"source_addr = 0x{source_address:08x}")
        lines.extend(
            [
                f"mode = {_toml_string(symbol['mode'])}",
                f"name = {_toml_string(symbol['name'])}",
                'note = "proven STT_FUNC entry and ARM mapping-symbol mode"',
            ]
        )
    for seed in (*REVIEWED_STATIC_SEEDS, *REVIEWED_DISPATCH_MISS_SEEDS, *pointer_seeds, *section_seeds):
        lines.extend(
            [
                "",
                "[[extra_func]]",
                f"addr = 0x{seed['addr']:08x}",
                f"source_addr = 0x{seed['source_addr']:08x}",
                f"mode = {_toml_string(seed['mode'])}",
                f"name = {_toml_string(seed['name'])}",
                f"note = {_toml_string(seed['note'])}",
            ]
        )
    for resume in (*REVIEWED_RESUME_POINTS, *interwork_resumes, *veneer_resumes):
        lines.extend(["", "[[extra_func]]", f"addr = 0x{resume['addr']:08x}"])
        if "source_addr" in resume:
            lines.append(f"source_addr = 0x{resume['source_addr']:08x}")
        lines.extend(
            [
                f"mode = {_toml_string(resume['mode'])}",
                f"name = {_toml_string(resume['name'])}",
                "resume = true",
                f"note = {_toml_string(resume['note'])}",
            ]
        )
    for entry in resume_ranges:
        lines.extend(
            [
                "",
                "[[resume_range]]",
                f"start = 0x{entry['start']:08x}",
                f"end = 0x{entry['end']:08x}",
                f"mode = {_toml_string(entry['mode'])}",
                f"note = {_toml_string(entry['note'])}",
            ]
        )
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--rom", required=True, type=Path)
    parser.add_argument("--elf", required=True, type=Path)
    parser.add_argument("--corpus", required=True, type=Path)
    parser.add_argument("--data-exceptions", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--disassembly-revision", required=True)
    args = parser.parse_args()
    if REVISION_RE.fullmatch(args.disassembly_revision) is None:
        print("ERROR: invalid --disassembly-revision", file=sys.stderr)
        return 2

    try:
        rom = args.rom.expanduser().resolve()
        if (
            not rom.is_file()
            or rom.stat().st_size != EXPECTED_SIZE
            or sha1_file(rom) != EXPECTED_SHA1
        ):
            raise ProposalError("unsupported ROM identity")
        rom_bytes = rom.read_bytes()
        entry = decode_cartridge_entry(rom_bytes[:4])
        if entry != EXPECTED_ENTRY:
            raise ProposalError(f"unexpected decoded entry 0x{entry:08x}")
        verify_reviewed_jump_tables(rom_bytes)
        verify_reviewed_pointer_tables(rom_bytes)

        elf_path = args.elf.expanduser().resolve()
        image = parse_elf32_arm(elf_path.read_bytes())
        corpus_path = args.corpus.expanduser().resolve()
        corpus_bytes = corpus_path.read_bytes()
        corpus = json.loads(corpus_bytes)
        revisions = {symbol["evidence_revision"] for symbol in corpus["symbols"]}
        if revisions != {args.disassembly_revision}:
            raise ProposalError("symbol corpus revision does not match the requested ELF pin")
        seeds = select_proven_seeds(corpus, entry)
        exceptions_path = args.data_exceptions.expanduser().resolve()
        exceptions_bytes = exceptions_path.read_bytes()
        exceptions_document = json.loads(exceptions_bytes)
        if exceptions_document.get("schema_version") != 1:
            raise ProposalError("unsupported data-exception schema")
        if exceptions_document.get("rom_sha1") != EXPECTED_SHA1:
            raise ProposalError("data exceptions are not bound to the supported ROM")
        if exceptions_document.get("evidence_revision") != args.disassembly_revision:
            raise ProposalError("data exceptions do not match the requested ELF pin")
        exceptions = exceptions_document.get("exceptions")
        if not isinstance(exceptions, list):
            raise ProposalError("data exceptions must contain an exceptions array")
        already_seeded = {int(symbol["runtime_address"], 16) for symbol in seeds}
        already_seeded.update(seed["addr"] for seed in REVIEWED_STATIC_SEEDS)
        already_seeded.update(seed["addr"] for seed in REVIEWED_DISPATCH_MISS_SEEDS)
        already_seeded.update(point["addr"] for point in REVIEWED_RESUME_POINTS)
        pointer_seeds = derive_pointer_table_seeds(image, already_seeded)
        already_seeded.update(seed["addr"] for seed in pointer_seeds)
        section_seeds = select_section_seeds(corpus, already_seeded)
        already_seeded.update(seed["addr"] for seed in section_seeds)
        interwork_sites = scan_interwork_sites(rom_bytes, image)
        interwork_resumes = [
            resume
            for resume in derive_interwork_resume_points(interwork_sites)
            if resume["addr"] not in already_seeded
        ]
        already_seeded.update(resume["addr"] for resume in interwork_resumes)
        veneer_stubs = scan_veneer_stubs(rom_bytes, image)
        veneer_resumes = [
            resume
            for resume in derive_veneer_resume_points(veneer_stubs)
            if resume["addr"] not in already_seeded
        ]
        base_ranges = _merge_ranges(
            [*derive_data_ranges(image), *REVIEWED_RUNTIME_PLACEHOLDER_RANGES]
        )
        derived_exceptions = derive_interwork_pad_exceptions(
            rom_bytes,
            interwork_sites,
            base_ranges,
            {int(exception["start"], 16) for exception in exceptions},
        )
        excluded_from_resume = apply_data_exceptions(
            base_ranges, [*exceptions, *derived_exceptions]
        )
        ranges = subtract_jump_table_ranges(excluded_from_resume, REVIEWED_JUMP_TABLES)
        # Derived before the jump-table subtraction, so a resume range never
        # claims table words as instructions. It still honours the reviewed data
        # exceptions, which are bytes an ELF `$d` symbol marks wrongly and that
        # straight-line control flow really executes.
        resume_ranges = derive_resume_ranges(image, excluded_from_resume)
        copies = derive_code_copies(image)
        if not copies:
            raise ProposalError("ELF contains no evidenced ROM-to-RAM executable mapping")
        output_text = render_toml(
            reset_target=entry,
            disassembly_revision=args.disassembly_revision,
            corpus_sha256=hashlib.sha256(corpus_bytes).hexdigest(),
            seeds=seeds,
            pointer_seeds=pointer_seeds,
            section_seeds=section_seeds,
            interwork_resumes=interwork_resumes,
            veneer_resumes=veneer_resumes,
            resume_ranges=resume_ranges,
            data_ranges=ranges,
            code_copies=copies,
            data_exception_sha256=hashlib.sha256(exceptions_bytes).hexdigest(),
        )
        output = args.output.expanduser().resolve()
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(output_text, encoding="utf-8", newline="\n")
    except (OSError, KeyError, json.JSONDecodeError, ProposalError, ValueError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1

    print(f"Discovery entry: 0x{ROM_BASE:08x}")
    print(f"Decoded reset target: 0x{entry:08x}")
    print(f"Proven seeds: {len(seeds)}")
    print(f"Reviewed pointer-table seeds: {len(pointer_seeds)}")
    print(f"Reviewed section seeds: {len(section_seeds)}")
    print(f"Reviewed interworking resumes: {len(interwork_resumes)}")
    print(f"Derived interworking pad exceptions: {len(derived_exceptions)}")
    print(f"Derived veneer stubs: {len(veneer_stubs)}")
    print(f"Derived veneer resumes: {len(veneer_resumes)}")
    print(
        f"Reviewed resume ranges: {len(resume_ranges)} from "
        f"{len(REVIEWED_RESUME_FUNCTIONS)} function(s)"
    )
    print(f"Data ranges: {len(ranges)}")
    print(f"Code copies: {len(copies)}")
    for copy in copies:
        print(
            "  "
            f"0x{copy['source_start']:08x} -> 0x{copy['runtime_start']:08x} "
            f"size=0x{copy['size']:x}"
        )
    print(f"TOML SHA-256: {hashlib.sha256(output_text.encode('utf-8')).hexdigest()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
