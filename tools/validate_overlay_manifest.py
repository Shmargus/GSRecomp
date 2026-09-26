#!/usr/bin/env python3
"""Validate a Golden Sun overlay manifest together with its symbol corpus."""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import re
import sys
from typing import Any

if __package__:
    from .validate_symbol_corpus import validate_document
    from .verify_rom import EXPECTED_SHA1, EXPECTED_SIZE
else:
    from validate_symbol_corpus import validate_document
    from verify_rom import EXPECTED_SHA1, EXPECTED_SIZE


ADDRESS_RE = re.compile(r"^0x[0-9a-f]{8}$")
SHA1_RE = re.compile(r"^[0-9a-f]{40}$")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
OVERLAY_RE = re.compile(r"^rom_([0-9a-f]{6})$")
ROOT_FIELDS = {
    "schema_version",
    "rom_sha1",
    "binary_id",
    "evidence_revision",
    "overlays",
}
OVERLAY_FIELDS = {"overlay_id", "evidence_source", "source", "runtime", "symbols"}
SOURCE_FIELDS = {
    "rom_offset",
    "rom_start",
    "compressed_size",
    "compressed_sha256",
    "decompressed_size",
    "decompressed_sha256",
}
RUNTIME_FIELDS = {"start", "loaded_size", "memory_size"}
SYMBOL_COUNT_FIELDS = {
    "count",
    "arm_count",
    "thumb_count",
    "proven_count",
    "unresolved_confidence_count",
    "excluded_count",
}


def _exact_fields(value: Any, fields: set[str], label: str, errors: list[str]) -> bool:
    if not isinstance(value, dict):
        errors.append(f"{label}: expected an object")
        return False
    for field in sorted(fields - set(value)):
        errors.append(f"{label}: missing field {field!r}")
    for field in sorted(set(value) - fields):
        errors.append(f"{label}: unexpected field {field!r}")
    return set(value) == fields


def _address(value: Any, label: str, errors: list[str]) -> int | None:
    if not isinstance(value, str) or ADDRESS_RE.fullmatch(value) is None:
        errors.append(f"{label}: expected lowercase 0x plus 8 hex digits")
        return None
    return int(value, 16)


def _positive_int(value: Any, label: str, errors: list[str]) -> int | None:
    if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
        errors.append(f"{label}: expected a positive integer")
        return None
    return value


def validate_overlay_documents(
    manifest: Any, corpus: Any
) -> tuple[list[str], dict[str, int]]:
    errors = validate_document(corpus)
    summary = {"overlay_count": 0, "symbol_count": 0, "collision_addresses": 0}
    if not _exact_fields(manifest, ROOT_FIELDS, "root", errors):
        return sorted(set(errors)), summary
    if manifest["schema_version"] != 1:
        errors.append("root.schema_version: expected 1")
    if manifest["rom_sha1"] != EXPECTED_SHA1:
        errors.append("root.rom_sha1: unsupported ROM identity")
    if not isinstance(manifest["binary_id"], str) or not manifest["binary_id"]:
        errors.append("root.binary_id: expected a non-empty string")
    if not isinstance(manifest["evidence_revision"], str) or SHA1_RE.fullmatch(
        manifest["evidence_revision"]
    ) is None:
        errors.append("root.evidence_revision: expected 40 lowercase hex digits")
    overlays = manifest["overlays"]
    if not isinstance(overlays, list):
        errors.append("root.overlays: expected an array")
        return sorted(set(errors)), summary

    overlay_by_id: dict[str, dict[str, Any]] = {}
    source_ranges: list[tuple[int, int, str]] = []
    previous_id = ""
    for index, overlay in enumerate(overlays):
        label = f"overlays[{index}]"
        if not _exact_fields(overlay, OVERLAY_FIELDS, label, errors):
            continue
        overlay_id = overlay["overlay_id"]
        match = OVERLAY_RE.fullmatch(overlay_id) if isinstance(overlay_id, str) else None
        if match is None:
            errors.append(f"{label}.overlay_id: expected rom_ plus 6 lowercase hex digits")
            continue
        if overlay_id <= previous_id:
            errors.append("root.overlays: overlay IDs are not strictly sorted")
        previous_id = overlay_id
        if overlay_id in overlay_by_id:
            errors.append(f"{label}.overlay_id: duplicate overlay identity")
        overlay_by_id[overlay_id] = overlay
        if not isinstance(overlay["evidence_source"], str) or not overlay["evidence_source"]:
            errors.append(f"{label}.evidence_source: expected a non-empty string")

        source = overlay["source"]
        runtime = overlay["runtime"]
        counts = overlay["symbols"]
        if not _exact_fields(source, SOURCE_FIELDS, f"{label}.source", errors):
            continue
        if not _exact_fields(runtime, RUNTIME_FIELDS, f"{label}.runtime", errors):
            continue
        if not _exact_fields(counts, SYMBOL_COUNT_FIELDS, f"{label}.symbols", errors):
            continue
        rom_offset = _address(source["rom_offset"], f"{label}.source.rom_offset", errors)
        rom_start = _address(source["rom_start"], f"{label}.source.rom_start", errors)
        compressed_size = _positive_int(
            source["compressed_size"], f"{label}.source.compressed_size", errors
        )
        decompressed_size = _positive_int(
            source["decompressed_size"], f"{label}.source.decompressed_size", errors
        )
        runtime_start = _address(runtime["start"], f"{label}.runtime.start", errors)
        loaded_size = _positive_int(runtime["loaded_size"], f"{label}.runtime.loaded_size", errors)
        memory_size = _positive_int(runtime["memory_size"], f"{label}.runtime.memory_size", errors)
        for hash_field in ("compressed_sha256", "decompressed_sha256"):
            value = source[hash_field]
            if not isinstance(value, str) or SHA256_RE.fullmatch(value) is None:
                errors.append(f"{label}.source.{hash_field}: expected 64 lowercase hex digits")
        if rom_offset is not None:
            declared_offset = int(match.group(1), 16)
            if rom_offset != declared_offset:
                errors.append(f"{label}.source.rom_offset: disagrees with overlay ID")
            if rom_start is not None and rom_start != 0x08000000 + rom_offset:
                errors.append(f"{label}.source.rom_start: does not map the ROM file offset")
            if compressed_size is not None:
                if rom_offset + compressed_size > EXPECTED_SIZE:
                    errors.append(f"{label}.source: compressed range exceeds the ROM")
                source_ranges.append((rom_offset, rom_offset + compressed_size, overlay_id))
        if decompressed_size is not None and loaded_size is not None:
            if decompressed_size != loaded_size:
                errors.append(f"{label}.runtime.loaded_size: differs from decompressed size")
        if loaded_size is not None and memory_size is not None and memory_size < loaded_size:
            errors.append(f"{label}.runtime.memory_size: smaller than loaded size")
        if runtime_start is not None and runtime_start % 4:
            errors.append(f"{label}.runtime.start: not word aligned")
        for field in SYMBOL_COUNT_FIELDS:
            value = counts[field]
            if not isinstance(value, int) or isinstance(value, bool) or value < 0:
                errors.append(f"{label}.symbols.{field}: expected a non-negative integer")
        if all(isinstance(counts[field], int) for field in SYMBOL_COUNT_FIELDS):
            if counts["count"] != counts["arm_count"] + counts["thumb_count"]:
                errors.append(f"{label}.symbols: ARM/THUMB counts do not sum to count")
            if counts["count"] != counts["proven_count"] + counts["unresolved_confidence_count"]:
                errors.append(f"{label}.symbols: confidence counts do not sum to count")

    source_ranges.sort()
    for previous, current in zip(source_ranges, source_ranges[1:]):
        if current[0] < previous[1]:
            errors.append(
                f"compressed ROM ranges overlap: {previous[2]} and {current[2]}"
            )

    symbols = corpus.get("symbols", []) if isinstance(corpus, dict) else []
    actual_counts: dict[str, Counter[str]] = defaultdict(Counter)
    addresses: dict[int, set[str]] = defaultdict(set)
    for index, symbol in enumerate(symbols if isinstance(symbols, list) else []):
        overlay_id = symbol.get("overlay_id") if isinstance(symbol, dict) else None
        if overlay_id not in overlay_by_id:
            errors.append(f"symbols[{index}].overlay_id: absent from overlay manifest")
            continue
        overlay = overlay_by_id[overlay_id]
        if symbol.get("binary_id") != manifest["binary_id"]:
            errors.append(f"symbols[{index}].binary_id: differs from overlay manifest")
        if symbol.get("evidence_revision") != manifest["evidence_revision"]:
            errors.append(f"symbols[{index}].evidence_revision: differs from overlay manifest")
        runtime_address = int(symbol["runtime_address"], 16)
        source_address = int(symbol["source_address"], 16)
        runtime_start = int(overlay["runtime"]["start"], 16)
        runtime_end = runtime_start + overlay["runtime"]["loaded_size"]
        if runtime_address < runtime_start or runtime_address + symbol["size"] > runtime_end:
            errors.append(f"symbols[{index}]: range lies outside its overlay runtime image")
        if source_address != runtime_address:
            errors.append(
                f"symbols[{index}].source_address: overlay ELF source domain must match runtime"
            )
        actual_counts[overlay_id]["count"] += 1
        actual_counts[overlay_id][f"{symbol['mode']}_count"] += 1
        actual_counts[overlay_id][f"{symbol['confidence']}_count"] += 1
        addresses[runtime_address].add(overlay_id)

    for overlay_id, overlay in overlay_by_id.items():
        expected = overlay["symbols"]
        actual = actual_counts[overlay_id]
        comparisons = {
            "count": actual["count"],
            "arm_count": actual["arm_count"],
            "thumb_count": actual["thumb_count"],
            "proven_count": actual["proven_count"],
            "unresolved_confidence_count": actual["unresolved_count"],
        }
        for field, value in comparisons.items():
            if expected[field] != value:
                errors.append(f"overlay {overlay_id}: manifest {field} does not match corpus")

    summary = {
        "overlay_count": len(overlay_by_id),
        "symbol_count": len(symbols) if isinstance(symbols, list) else 0,
        "collision_addresses": sum(1 for ids in addresses.values() if len(ids) > 1),
    }
    return sorted(set(errors)), summary


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("manifest", type=Path)
    parser.add_argument("corpus", type=Path)
    args = parser.parse_args()
    try:
        manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
        corpus = json.loads(args.corpus.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1
    errors, summary = validate_overlay_documents(manifest, corpus)
    if errors:
        print("INVALID overlay manifest/corpus:", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
        return 1
    print(
        f"OK: overlays={summary['overlay_count']} symbols={summary['symbol_count']} "
        f"cross-overlay-collision-addresses={summary['collision_addresses']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
