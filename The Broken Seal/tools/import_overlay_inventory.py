#!/usr/bin/env python3
"""Import the verified Golden Sun overlay inventory and partitioned symbols."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
from typing import Any

if __package__:
    from .import_main_symbols import PT_LOAD, REVISION_RE, import_symbols, parse_elf32_arm
    from .validate_overlay_manifest import validate_overlay_documents
    from .verify_rom import EXPECTED_SHA1, EXPECTED_SIZE, sha1_file
else:
    from import_main_symbols import PT_LOAD, REVISION_RE, import_symbols, parse_elf32_arm
    from validate_overlay_manifest import validate_overlay_documents
    from verify_rom import EXPECTED_SHA1, EXPECTED_SIZE, sha1_file


OVERLAY_ID_RE = re.compile(r"^rom_([0-9a-f]{6})$")
ROM_BASE = 0x08000000


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _write_json(path: Path, document: dict[str, Any]) -> str:
    payload = (json.dumps(document, indent=2, ensure_ascii=True) + "\n").encode("utf-8")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(payload)
    return _sha256(payload)


def import_overlay_inventory(
    *,
    disasm_root: Path,
    rom_bytes: bytes,
    evidence_revision: str,
    binary_id: str = "golden_sun_overlay",
    expected_count: int = 96,
) -> tuple[dict[str, Any], dict[str, Any], dict[str, Any]]:
    overlay_root = disasm_root / "overlays"
    elf_paths = sorted(overlay_root.glob("rom_*/overlay.elf"))
    if len(elf_paths) != expected_count:
        raise ValueError(
            f"expected {expected_count} overlay ELFs, found {len(elf_paths)}"
        )

    manifest_entries: list[dict[str, Any]] = []
    all_symbols: list[dict[str, Any]] = []
    all_unresolved: list[dict[str, Any]] = []
    all_transitions: list[dict[str, Any]] = []
    external_function_symbols = {"undefined": 0, "absolute": 0}
    for elf_path in elf_paths:
        overlay_id = elf_path.parent.name
        match = OVERLAY_ID_RE.fullmatch(overlay_id)
        if match is None:
            raise ValueError(f"invalid overlay directory identity: {overlay_id}")
        rom_offset = int(match.group(1), 16)
        orig_path = elf_path.parent / "orig.bin"
        built_path = elf_path.parent / "overlay.bin"
        packed_path = elf_path.parent / "overlay.lz"
        for required_path in (orig_path, built_path, packed_path):
            if not required_path.is_file():
                raise ValueError(f"missing verified overlay artifact: {required_path}")

        orig_bytes = orig_path.read_bytes()
        built_bytes = built_path.read_bytes()
        packed_bytes = packed_path.read_bytes()
        if orig_bytes != built_bytes:
            raise ValueError(f"rebuilt overlay differs from extracted bytes: {overlay_id}")
        rom_end = rom_offset + len(packed_bytes)
        if rom_end > len(rom_bytes) or rom_bytes[rom_offset:rom_end] != packed_bytes:
            raise ValueError(
                f"packed overlay does not match the exact ROM at its declared offset: {overlay_id}"
            )

        image = parse_elf32_arm(elf_path.read_bytes())
        load_headers = [
            header for header in image.program_headers if header.header_type == PT_LOAD
        ]
        if len(load_headers) != 1:
            raise ValueError(f"expected one PT_LOAD segment in {overlay_id}")
        load = load_headers[0]
        if load.virtual_address != load.physical_address:
            raise ValueError(f"overlay ELF source/runtime domains differ in {overlay_id}")
        if load.file_size != len(orig_bytes):
            raise ValueError(
                f"PT_LOAD file size differs from decompressed overlay size: {overlay_id}"
            )
        if load.memory_size < load.file_size:
            raise ValueError(f"PT_LOAD memory size is smaller than file size: {overlay_id}")

        evidence_source = (
            f"overlays/{overlay_id}/overlay.elf:.symtab+mapping-symbols+PT_LOAD;"
            f"Makefile:rom_%/orig.bin;overlays/{overlay_id}/overlay.lz"
        )
        corpus, report = import_symbols(
            image,
            binary_id=binary_id,
            overlay_id=overlay_id,
            evidence_source=evidence_source,
            evidence_revision=evidence_revision,
        )
        symbols = corpus["symbols"]
        all_symbols.extend(symbols)
        all_unresolved.extend(
            {"overlay_id": overlay_id, **entry} for entry in report["unresolved"]
        )
        all_transitions.extend(
            {"overlay_id": overlay_id, **entry}
            for entry in report["internal_mapping_transitions"]
        )
        for kind in external_function_symbols:
            external_function_symbols[kind] += report["external_function_symbols"][kind]
        mode_counts = {"arm": 0, "thumb": 0}
        confidence_counts = {"proven": 0, "unresolved": 0}
        for symbol in symbols:
            mode_counts[symbol["mode"]] += 1
            confidence_counts[symbol["confidence"]] += 1

        manifest_entries.append(
            {
                "overlay_id": overlay_id,
                "evidence_source": evidence_source,
                "source": {
                    "rom_offset": f"0x{rom_offset:08x}",
                    "rom_start": f"0x{ROM_BASE + rom_offset:08x}",
                    "compressed_size": len(packed_bytes),
                    "compressed_sha256": _sha256(packed_bytes),
                    "decompressed_size": len(orig_bytes),
                    "decompressed_sha256": _sha256(orig_bytes),
                },
                "runtime": {
                    "start": f"0x{load.virtual_address:08x}",
                    "loaded_size": load.file_size,
                    "memory_size": load.memory_size,
                },
                "symbols": {
                    "count": len(symbols),
                    "arm_count": mode_counts["arm"],
                    "thumb_count": mode_counts["thumb"],
                    "proven_count": confidence_counts["proven"],
                    "unresolved_confidence_count": confidence_counts["unresolved"],
                    "excluded_count": len(report["unresolved"]),
                },
            }
        )

    all_symbols.sort(
        key=lambda record: (
            record["binary_id"],
            record["overlay_id"],
            int(record["runtime_address"], 16),
            record["name"],
        )
    )
    all_unresolved.sort(
        key=lambda record: (
            record["overlay_id"],
            record["section"],
            int(record["symbol_value"], 16),
            record["name"],
        )
    )
    all_transitions.sort(
        key=lambda record: (
            record["overlay_id"],
            int(record["runtime_address"], 16),
            record["name"],
        )
    )
    manifest = {
        "schema_version": 1,
        "rom_sha1": EXPECTED_SHA1,
        "binary_id": binary_id,
        "evidence_revision": evidence_revision,
        "overlays": manifest_entries,
    }
    corpus = {"schema_version": 1, "symbols": all_symbols}
    report = {
        "schema_version": 1,
        "binary_id": binary_id,
        "evidence_revision": evidence_revision,
        "unresolved": all_unresolved,
        "internal_mapping_transitions": all_transitions,
        "external_function_symbols": external_function_symbols,
    }
    validation_errors, _summary = validate_overlay_documents(manifest, corpus)
    if validation_errors:
        joined = "\n".join(f"- {error}" for error in validation_errors)
        raise ValueError(f"overlay inventory failed validation:\n{joined}")
    return manifest, corpus, report


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--disasm-root", required=True, type=Path)
    parser.add_argument("--rom", required=True, type=Path)
    parser.add_argument("--manifest-output", required=True, type=Path)
    parser.add_argument("--symbols-output", required=True, type=Path)
    parser.add_argument("--unresolved-output", required=True, type=Path)
    parser.add_argument("--evidence-revision", required=True)
    parser.add_argument("--binary-id", default="golden_sun_overlay")
    parser.add_argument("--expected-count", type=int, default=96)
    args = parser.parse_args()
    if REVISION_RE.fullmatch(args.evidence_revision) is None:
        print("ERROR: --evidence-revision must be a lowercase 40-hex git commit", file=sys.stderr)
        return 2
    rom_path = args.rom.expanduser().resolve()
    if not rom_path.is_file():
        print(f"ERROR: ROM file not found: {rom_path}", file=sys.stderr)
        return 2
    if rom_path.stat().st_size != EXPECTED_SIZE or sha1_file(rom_path) != EXPECTED_SHA1:
        print("ERROR: unsupported ROM identity; refusing overlay import", file=sys.stderr)
        return 1
    try:
        manifest, corpus, report = import_overlay_inventory(
            disasm_root=args.disasm_root.expanduser().resolve(),
            rom_bytes=rom_path.read_bytes(),
            evidence_revision=args.evidence_revision,
            binary_id=args.binary_id,
            expected_count=args.expected_count,
        )
        manifest_hash = _write_json(args.manifest_output.expanduser().resolve(), manifest)
        corpus_hash = _write_json(args.symbols_output.expanduser().resolve(), corpus)
        report_hash = _write_json(args.unresolved_output.expanduser().resolve(), report)
    except (OSError, ValueError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1
    _errors, summary = validate_overlay_documents(manifest, corpus)
    print(f"ROM SHA-1: {EXPECTED_SHA1}")
    print(f"Overlays: {summary['overlay_count']}")
    print(f"Symbols: {summary['symbol_count']}")
    print(f"Cross-overlay collision addresses: {summary['collision_addresses']}")
    print(f"Excluded: {len(report['unresolved'])}")
    print(
        "External function symbols: "
        f"undefined={report['external_function_symbols']['undefined']} "
        f"absolute={report['external_function_symbols']['absolute']}"
    )
    print(f"Internal mapping transitions: {len(report['internal_mapping_transitions'])}")
    print(f"Manifest SHA-256: {manifest_hash}")
    print(f"Corpus SHA-256: {corpus_hash}")
    print(f"Report SHA-256: {report_hash}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
