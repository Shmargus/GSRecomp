#!/usr/bin/env python3
"""Build one evidence-backed Golden Sun overlay TOML proposal."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys
from typing import Any

if __package__:
    from .build_main_toml import ProposalError, _merge_ranges, _toml_string
    from .import_main_symbols import SHF_EXECINSTR, _mapping_symbols, parse_elf32_arm
    from .validate_symbol_corpus import validate_document
else:
    from build_main_toml import ProposalError, _merge_ranges, _toml_string
    from import_main_symbols import SHF_EXECINSTR, _mapping_symbols, parse_elf32_arm
    from validate_symbol_corpus import validate_document


# GNU as emits this `.call_via r10` alignment halfword under a `$d` mapping
# even though THUMB control falls through it before the macro's MOV/BX pair.
# Pinned source macro + ELF STT_FUNC extent + recompiler CFG collision prove it
# is executed alignment, not literal data. Keep this exception exact and loud.
REVIEWED_EXECUTED_ALIGNMENT = {
    "rom_791794": (
        (0x02008FF6, 0x02008FF8),
        (0x0200928E, 0x02009290),
        (0x020092DA, 0x020092DC),
    ),
    "rom_7a5214": ((0x020090AA, 0x020090AC),),
    "rom_7aa430": (
        (0x02009D3E, 0x02009D40),
        (0x02009D4A, 0x02009D4C),
        (0x02009D92, 0x02009D94),
    ),
    "rom_7ac2d8": (
        (0x0200D2CE, 0x0200D2D0),
        (0x0200D2DA, 0x0200D2DC),
        (0x0200D322, 0x0200D324),
    ),
    "rom_7b9cb4": ((0x0200B7C6, 0x0200B7C8),),
    "rom_7fa4ec": ((0x02009066, 0x02009068),),
}

# Reviewed operand overrides inside an overlay, answered at run time by
# src/runner_main.cpp. The world map's OvlFunc_598 hides towns and caves
# outside a box around the camera sized for the 240x160 screen; the expanded
# view grows it (FACTS.md, 2026-09-25).
REVIEWED_OVERLAY_OVERRIDES = {
    "rom_77a7c8": (
        ("thumb_literal_override", 0x020085D2,
         "OvlFunc_598 town box: left x bound -160 << 16"),
        ("thumb_alu_immediate_override", 0x020085D6,
         "OvlFunc_598 town box: right x bound movs #160"),
        ("thumb_alu_immediate_override", 0x020085E6,
         "OvlFunc_598 town box: lower z bound movs #200"),
    ),
    # Sol Sanctum's cloud rows are queued only while on the 240x160 screen
    # (`cmp r3,#0xAF; bhi`, r3 = y + 16); the expanded view admits the top
    # margin (FACTS.md, 2026-10-05).
    "rom_78c76c": (
        ("conditional_branch_override", 0x02008EEC,
         "Sol Sanctum cloud rows: screen-Y BHI, admit the top margin"),
    ),
}


def derive_overlay_data_ranges(
    image: Any, loaded_start: int, loaded_size: int, overlay_id: str | None = None
) -> list[tuple[int, int]]:
    """Return ELF `$d` spans plus loaded bytes outside executable sections."""

    loaded_end = loaded_start + loaded_size
    mappings = _mapping_symbols(image)
    executable: list[tuple[int, int]] = []
    data: list[tuple[int, int]] = []
    for section in image.sections:
        if not section.flags & SHF_EXECINSTR or section.size == 0:
            continue
        section_end = section.address + section.size
        if not (loaded_start <= section.address < section_end <= loaded_end):
            raise ProposalError(
                f"executable section {section.name} is outside the loaded overlay"
            )
        executable.append((section.address, section_end))
        states = sorted(
            (address, kind)
            for address, kind, _index in mappings.get(section.index, [])
            if section.address <= address < section_end
        )
        if not states or states[0][0] != section.address:
            raise ProposalError(
                f"executable section {section.name} has no mapping at its start"
            )
        for index, (start, kind) in enumerate(states):
            end = states[index + 1][0] if index + 1 < len(states) else section_end
            if kind == "d" and start < end:
                data.append((start, end))

    executable.sort()
    cursor = loaded_start
    for start, end in executable:
        if start < cursor:
            raise ProposalError("overlay executable sections overlap")
        if cursor < start:
            data.append((cursor, start))
        cursor = end
    if cursor < loaded_end:
        data.append((cursor, loaded_end))
    merged = _merge_ranges(data)
    for exception_start, exception_end in REVIEWED_EXECUTED_ALIGNMENT.get(
        overlay_id, ()
    ):
        if not (loaded_start <= exception_start < exception_end <= loaded_end):
            continue
        owners = [
            (start, end)
            for start, end in merged
            if start <= exception_start < exception_end <= end
        ]
        if len(owners) != 1:
            raise ProposalError(
                f"reviewed executed alignment 0x{exception_start:08x} is not data"
            )
        revised: list[tuple[int, int]] = []
        for start, end in merged:
            if (start, end) != owners[0]:
                revised.append((start, end))
                continue
            if start < exception_start:
                revised.append((start, exception_start))
            if exception_end < end:
                revised.append((exception_end, end))
        merged = revised
    return merged


def select_overlay_seeds(
    corpus: dict[str, Any], observed: dict[str, Any], overlay_id: str
) -> list[dict[str, Any]]:
    errors = validate_document(corpus)
    if errors:
        raise ProposalError("invalid overlay corpus: " + "; ".join(errors))
    symbols = [s for s in corpus["symbols"] if s["overlay_id"] == overlay_id]
    if not symbols:
        raise ProposalError(f"overlay corpus has no symbols for {overlay_id}")

    selected: dict[tuple[int, str], dict[str, Any]] = {}
    for symbol in symbols:
        # A record only reaches the corpus after passing every entry-level
        # check, so an `unresolved` one differs from a proven one solely in
        # that its declared extent contains further mapping-symbol
        # transitions — literal pools. Its entry address and mode carry the
        # same evidence, and an overlay is dispatched only after its live
        # bytes hash-match, so seeding the whole ELF-bounded set is the same
        # standard applied consistently rather than a new claim.
        if symbol["confidence"] not in ("proven", "unresolved"):
            continue
        addr = int(symbol["runtime_address"], 16)
        selected[(addr, symbol["mode"])] = {
            "addr": addr,
            "mode": symbol["mode"],
            "name": symbol["name"],
            "resume": False,
            "owner_start": addr,
            "owner_end": addr + int(symbol["size"]),
            "note": (
                "proven overlay STT_FUNC entry and mapping-symbol mode"
                if symbol["confidence"] == "proven"
                else "overlay STT_FUNC entry and mapping-symbol mode; corpus "
                "withholds it only for interior mapping transitions"
            ),
        }

    for entry in observed["entries"]:
        addr = int(entry["addr"], 16)
        mode = entry["mode"]
        resume = bool(entry.get("resume", False))
        if not entry.get("evidence"):
            raise ProposalError(f"observed overlay entry 0x{addr:08x} has no evidence")
        if resume:
            owners = [
                symbol
                for symbol in symbols
                if int(symbol["runtime_address"], 16) < addr
                < int(symbol["runtime_address"], 16) + int(symbol["size"])
                and symbol["mode"] == mode
            ]
            if len(owners) != 1:
                raise ProposalError(
                    f"resume 0x{addr:08x} is not inside one {mode} symbol"
                )
            name = f"resume_{overlay_id}_{addr:08x}"
        else:
            owners = [
                symbol
                for symbol in symbols
                if int(symbol["runtime_address"], 16) == addr
                and symbol["mode"] == mode
            ]
            if len(owners) != 1:
                raise ProposalError(
                    f"entry 0x{addr:08x} is not one exact {mode} symbol"
                )
            name = owners[0]["name"]
        owner_start = int(owners[0]["runtime_address"], 16)
        owner_end = owner_start + int(owners[0]["size"])
        selected[(addr, mode)] = {
            "addr": addr,
            "mode": mode,
            "name": name,
            "resume": resume,
            "note": entry["evidence"],
            # Extent of the symbol that owns this entry, so a resume can cover
            # the whole routine instead of only the PC that happened to be
            # observed. Unused for non-resume entries.
            "owner_start": owner_start,
            "owner_end": owner_end,
        }
    return [selected[key] for key in sorted(selected)]


def render_overlay_toml(
    *, overlay_id: str, start: int, size: int, sha1: str,
    sha256: str, seeds: list[dict[str, Any]],
    data_ranges: list[tuple[int, int]],
) -> str:
    entry = next((seed for seed in seeds if not seed["resume"]), None)
    if entry is None:
        raise ProposalError("overlay has no non-resume entry seed")
    lines = [
        "# Generated evidence proposal for GS-011. Do not hand-edit addresses.",
        f"# decompressed overlay SHA-256: {sha256}",
        "",
        "[program]",
        f"name = {_toml_string(f'Golden Sun overlay {overlay_id}')}",
        f"id = {_toml_string(f'golden_sun_overlay_{overlay_id}')}",
        f"load_address = 0x{start:08x}",
        f"size = 0x{size:08x}",
        f"entry_pc = 0x{entry['addr']:08x}",
        "speculative_literal_harvest = false",
        "codegen_shards = 1",
        "",
        "[identity]",
        f"sha1 = {_toml_string(sha1)}",
    ]
    for range_start, range_end in data_ranges:
        lines.extend([
            "", "[[data_range]]",
            f"start = 0x{range_start:08x}",
            f"end = 0x{range_end:08x}",
            'note = "overlay ELF mapping-symbol data or non-executable loaded bytes"',
        ])
    for kind, addr, note in REVIEWED_OVERLAY_OVERRIDES.get(overlay_id, ()):
        lines.extend([
            "", f"[[{kind}]]", f"addr = 0x{addr:08x}",
            f"note = {_toml_string(note)}",
        ])
    for seed in seeds:
        lines.extend([
            "", "[[extra_func]]", f"addr = 0x{seed['addr']:08x}",
            f"mode = {_toml_string(seed['mode'])}",
            f"name = {_toml_string(seed['name'])}",
        ])
        if seed["resume"]:
            lines.append("resume = true")
        lines.append(f"note = {_toml_string(seed['note'])}")

    # Cover each resume's WHOLE owning routine, split around the overlay's own
    # data ranges so no alias can decode declared data as code.
    #
    # Without this an overlay entered mid-body needs one regeneration, rebuild
    # and run per interior PC, which does not converge: a VBlank yield or a
    # computed dispatch re-enters wherever it likes. This mirrors
    # derive_resume_ranges() in build_main_toml.py, which already solved exactly
    # this for the main corpus.
    #
    # Applied to EVERY seeded routine, not only observed ones. The evidence is
    # identical either way - the overlay ELF proves each extent and each `$d`
    # run - and the alternative is one regeneration/rebuild/run cycle per
    # interior PC the game happens to re-enter, of which there are many. This is
    # the same generalisation REVIEWED_SEED_SECTIONS made for the main corpus.
    for seed in seeds:
        cursor, end = seed["owner_start"], seed["owner_end"]
        if cursor is None or end is None or end <= cursor:
            continue
        for data_start, data_end in data_ranges:
            if data_end <= cursor or data_start >= end:
                continue
            if data_start > cursor:
                lines.extend([
                    "", "[[resume_range]]",
                    f"start = 0x{cursor:08x}",
                    f"end = 0x{min(data_start, end):08x}",
                    f"mode = {_toml_string(seed['mode'])}",
                    f"note = {_toml_string('code run inside ' + seed['name'])}",
                ])
            cursor = max(cursor, data_end)
            if cursor >= end:
                break
        if cursor < end:
            lines.extend([
                "", "[[resume_range]]",
                f"start = 0x{cursor:08x}",
                f"end = 0x{end:08x}",
                f"mode = {_toml_string(seed['mode'])}",
                f"note = {_toml_string('code run inside ' + seed['name'])}",
            ])
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--overlay-id", required=True)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--elf", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--corpus", required=True, type=Path)
    review = parser.add_mutually_exclusive_group(required=True)
    review.add_argument("--observed", type=Path)
    review.add_argument(
        "--proactive",
        action="store_true",
        help="use only the pinned manifest, ELF and symbol corpus; add no observed resumes",
    )
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        binary = args.binary.read_bytes()
        manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
        corpus = json.loads(args.corpus.read_text(encoding="utf-8"))
        matches = [x for x in manifest["overlays"] if x["overlay_id"] == args.overlay_id]
        if len(matches) != 1:
            raise ProposalError(f"manifest does not contain one {args.overlay_id}")
        record = matches[0]
        expected_sha256 = record["source"]["decompressed_sha256"]
        actual_sha256 = hashlib.sha256(binary).hexdigest()
        if actual_sha256 != expected_sha256:
            raise ProposalError("decompressed overlay SHA-256 mismatch")
        start = int(record["runtime"]["start"], 16)
        size = int(record["runtime"]["loaded_size"])
        if len(binary) != size:
            raise ProposalError("decompressed overlay size mismatch")
        observed = (
            {
                "overlay_id": args.overlay_id,
                "decompressed_sha256": actual_sha256,
                "entries": [],
            }
            if args.proactive
            else json.loads(args.observed.read_text(encoding="utf-8"))
        )
        if observed.get("overlay_id") != args.overlay_id:
            raise ProposalError("observed-entry review names another overlay")
        if observed.get("decompressed_sha256") != actual_sha256:
            raise ProposalError("observed-entry review has another overlay identity")
        image = parse_elf32_arm(args.elf.read_bytes())
        seeds = select_overlay_seeds(corpus, observed, args.overlay_id)
        ranges = derive_overlay_data_ranges(image, start, size, args.overlay_id)
        output = render_overlay_toml(
            overlay_id=args.overlay_id, start=start, size=size,
            sha1=hashlib.sha1(binary).hexdigest(), sha256=actual_sha256,
            seeds=seeds, data_ranges=ranges,
        )
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(output, encoding="utf-8", newline="\n")
    except (OSError, KeyError, ValueError, json.JSONDecodeError, ProposalError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1
    print(f"Overlay: {args.overlay_id}")
    print(f"Seeds: {len(seeds)}")
    print(f"Data ranges: {len(ranges)}")
    print(f"TOML SHA-256: {hashlib.sha256(output.encode()).hexdigest()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
