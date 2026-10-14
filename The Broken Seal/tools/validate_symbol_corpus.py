#!/usr/bin/env python3
"""Validate deterministic GoldenSunRecomp symbol-corpus metadata."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys
from typing import Any


SCHEMA_VERSION = 1
ADDRESS_RE = re.compile(r"^0x[0-9a-f]{8}$")
IDENTIFIER_RE = re.compile(r"^[a-z0-9][a-z0-9._-]*$")
OVERLAY_ID_RE = re.compile(r"^[a-z0-9._-]*$")
MODES = {"arm": 4, "thumb": 2}
CONFIDENCE_VALUES = {"proven", "unresolved"}
REQUIRED_FIELDS = {
    "binary_id",
    "overlay_id",
    "name",
    "runtime_address",
    "source_address",
    "size",
    "mode",
    "section",
    "evidence_source",
    "evidence_revision",
    "confidence",
}


def _is_string(value: Any, *, allow_empty: bool = False) -> bool:
    return isinstance(value, str) and (allow_empty or bool(value))


def _parse_address(value: Any) -> int | None:
    if not isinstance(value, str) or ADDRESS_RE.fullmatch(value) is None:
        return None
    return int(value, 16)


def validate_document(document: Any) -> list[str]:
    """Return every validation error in stable, source-order-independent form."""

    errors: list[str] = []
    if not isinstance(document, dict):
        return ["root: expected an object"]

    root_fields = set(document)
    missing_root = {"schema_version", "symbols"} - root_fields
    extra_root = root_fields - {"schema_version", "symbols"}
    for field in sorted(missing_root):
        errors.append(f"root: missing field {field!r}")
    for field in sorted(extra_root):
        errors.append(f"root: unexpected field {field!r}")

    if document.get("schema_version") != SCHEMA_VERSION:
        errors.append(
            f"root.schema_version: expected {SCHEMA_VERSION}, "
            f"got {document.get('schema_version')!r}"
        )

    symbols = document.get("symbols")
    if not isinstance(symbols, list):
        errors.append("root.symbols: expected an array")
        return sorted(errors)

    parsed: list[tuple[int, dict[str, Any], int, int]] = []
    sort_keys: list[tuple[str, str, int, str]] = []
    for index, symbol in enumerate(symbols):
        label = f"symbols[{index}]"
        if not isinstance(symbol, dict):
            errors.append(f"{label}: expected an object")
            continue

        fields = set(symbol)
        for field in sorted(REQUIRED_FIELDS - fields):
            errors.append(f"{label}: missing field {field!r}")
        for field in sorted(fields - REQUIRED_FIELDS):
            errors.append(f"{label}: unexpected field {field!r}")
        if fields != REQUIRED_FIELDS:
            continue

        binary_id = symbol["binary_id"]
        overlay_id = symbol["overlay_id"]
        name = symbol["name"]
        section = symbol["section"]
        evidence_source = symbol["evidence_source"]
        evidence_revision = symbol["evidence_revision"]
        mode = symbol["mode"]
        confidence = symbol["confidence"]
        size = symbol["size"]
        runtime_address = _parse_address(symbol["runtime_address"])
        source_address = _parse_address(symbol["source_address"])

        if not _is_string(binary_id) or IDENTIFIER_RE.fullmatch(binary_id) is None:
            errors.append(f"{label}.binary_id: invalid stable identifier")
        if not _is_string(overlay_id, allow_empty=True) or OVERLAY_ID_RE.fullmatch(overlay_id) is None:
            errors.append(f"{label}.overlay_id: invalid stable identifier")
        for field_name, value in (
            ("name", name),
            ("section", section),
            ("evidence_source", evidence_source),
            ("evidence_revision", evidence_revision),
        ):
            if not _is_string(value):
                errors.append(f"{label}.{field_name}: expected a non-empty string")
        if mode not in MODES:
            errors.append(f"{label}.mode: expected 'arm' or 'thumb', got {mode!r}")
        if confidence not in CONFIDENCE_VALUES:
            errors.append(
                f"{label}.confidence: expected 'proven' or 'unresolved', "
                f"got {confidence!r}"
            )
        if not isinstance(size, int) or isinstance(size, bool) or size <= 0:
            errors.append(f"{label}.size: expected a positive integer")
        if runtime_address is None:
            errors.append(
                f"{label}.runtime_address: expected lowercase 0x plus 8 hex digits"
            )
        if source_address is None:
            errors.append(
                f"{label}.source_address: expected lowercase 0x plus 8 hex digits"
            )

        if mode in MODES:
            alignment = MODES[mode]
            if runtime_address is not None and runtime_address % alignment:
                errors.append(
                    f"{label}.runtime_address: {mode} address is not "
                    f"{alignment}-byte aligned"
                )
            if source_address is not None and source_address % alignment:
                errors.append(
                    f"{label}.source_address: {mode} address is not "
                    f"{alignment}-byte aligned"
                )
            if isinstance(size, int) and not isinstance(size, bool) and size > 0:
                if size % alignment:
                    errors.append(
                        f"{label}.size: {mode} size is not a multiple of {alignment}"
                    )

        if (
            isinstance(binary_id, str)
            and isinstance(overlay_id, str)
            and isinstance(name, str)
            and runtime_address is not None
            and isinstance(size, int)
            and not isinstance(size, bool)
            and size > 0
        ):
            parsed.append((index, symbol, runtime_address, size))
            sort_keys.append((binary_id, overlay_id, runtime_address, name))

    if sort_keys != sorted(sort_keys):
        errors.append(
            "root.symbols: entries are not sorted by "
            "(binary_id, overlay_id, runtime_address, name)"
        )

    seen_addresses: dict[tuple[str, str, int], int] = {}
    seen_names: dict[tuple[str, str, str], int] = {}
    grouped: dict[tuple[str, str], list[tuple[int, int, int]]] = {}
    for index, symbol, start, size in parsed:
        image = (symbol["binary_id"], symbol["overlay_id"])
        address_key = (*image, start)
        name_key = (*image, symbol["name"])
        if address_key in seen_addresses:
            errors.append(
                f"symbols[{index}].runtime_address: duplicates symbols"
                f"[{seen_addresses[address_key]}] within one binary identity"
            )
        else:
            seen_addresses[address_key] = index
        if name_key in seen_names:
            errors.append(
                f"symbols[{index}].name: duplicates symbols[{seen_names[name_key]}] "
                "within one binary identity"
            )
        else:
            seen_names[name_key] = index
        grouped.setdefault(image, []).append((start, start + size, index))

    for intervals in grouped.values():
        intervals.sort()
        previous_start, previous_end, previous_index = intervals[0]
        for start, end, index in intervals[1:]:
            if start < previous_end:
                errors.append(
                    f"symbols[{index}]: runtime range overlaps symbols"
                    f"[{previous_index}] within one binary identity"
                )
            if end > previous_end:
                previous_start, previous_end, previous_index = start, end, index

    return sorted(set(errors))


def validate_path(path: Path) -> list[str]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except OSError as error:
        return [f"unable to read file: {error}"]
    except json.JSONDecodeError as error:
        return [f"invalid JSON at line {error.lineno}, column {error.colno}: {error.msg}"]
    return validate_document(document)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Validate version-1 GoldenSunRecomp symbol corpora."
    )
    parser.add_argument("corpus", type=Path, nargs="+", help="JSON corpus file")
    args = parser.parse_args()

    failed = False
    for raw_path in args.corpus:
        path = raw_path.expanduser().resolve()
        errors = validate_path(path)
        if errors:
            failed = True
            print(f"INVALID: {path}", file=sys.stderr)
            for error in errors:
                print(f"- {error}", file=sys.stderr)
        else:
            print(f"OK: {path}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
