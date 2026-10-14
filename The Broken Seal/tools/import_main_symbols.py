#!/usr/bin/env python3
"""Import proven main-image function metadata from an ARM ELF32 executable."""

from __future__ import annotations

import argparse
from bisect import bisect_right
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
from typing import Any

if __package__:
    from .validate_symbol_corpus import validate_document
    from .verify_rom import EXPECTED_SHA1, EXPECTED_SIZE, sha1_file
else:
    from validate_symbol_corpus import validate_document
    from verify_rom import EXPECTED_SHA1, EXPECTED_SIZE, sha1_file


ELF_MAGIC = b"\x7fELF"
ELFCLASS32 = 1
ELFDATA2LSB = 1
EM_ARM = 40
PT_LOAD = 1
SHT_SYMTAB = 2
SHF_EXECINSTR = 0x4
STT_NOTYPE = 0
STT_FUNC = 2
SHN_UNDEF = 0
SHN_ABS = 0xFFF1
MAPPING_RE = re.compile(r"^\$(a|t|d)(?:\.|$)")
REVISION_RE = re.compile(r"^[0-9a-f]{40}$")


class ElfFormatError(ValueError):
    """Raised when an input cannot provide the required ELF evidence."""


@dataclass(frozen=True)
class Section:
    index: int
    name: str
    section_type: int
    flags: int
    address: int
    offset: int
    size: int
    link: int
    entry_size: int


@dataclass(frozen=True)
class ProgramHeader:
    header_type: int
    offset: int
    virtual_address: int
    physical_address: int
    file_size: int
    memory_size: int
    flags: int


@dataclass(frozen=True)
class Symbol:
    index: int
    name: str
    value: int
    size: int
    symbol_type: int
    binding: int
    section_index: int


@dataclass(frozen=True)
class ElfImage:
    sections: tuple[Section, ...]
    program_headers: tuple[ProgramHeader, ...]
    symbols: tuple[Symbol, ...]


def _unpack_from(fmt: str, data: bytes, offset: int, label: str) -> tuple[Any, ...]:
    size = struct.calcsize(fmt)
    if offset < 0 or offset + size > len(data):
        raise ElfFormatError(f"{label} extends beyond the ELF file")
    return struct.unpack_from(fmt, data, offset)


def _read_c_string(table: bytes, offset: int, label: str) -> str:
    if offset < 0 or offset >= len(table):
        raise ElfFormatError(f"{label} string offset is outside its string table")
    end = table.find(b"\0", offset)
    if end < 0:
        raise ElfFormatError(f"{label} string is not NUL terminated")
    try:
        return table[offset:end].decode("utf-8")
    except UnicodeDecodeError as error:
        raise ElfFormatError(f"{label} is not valid UTF-8/ASCII") from error


def _slice(data: bytes, offset: int, size: int, label: str) -> bytes:
    if offset < 0 or size < 0 or offset + size > len(data):
        raise ElfFormatError(f"{label} extends beyond the ELF file")
    return data[offset : offset + size]


def parse_elf32_arm(data: bytes) -> ElfImage:
    """Parse only the ELF metadata required for evidence-backed symbol import."""

    if len(data) < 52 or data[:4] != ELF_MAGIC:
        raise ElfFormatError("input is not an ELF file")
    if data[4] != ELFCLASS32:
        raise ElfFormatError("expected ELF32 input")
    if data[5] != ELFDATA2LSB:
        raise ElfFormatError("expected a little-endian ELF")

    header = _unpack_from("<16sHHIIIIIHHHHHH", data, 0, "ELF header")
    (
        _ident,
        _elf_type,
        machine,
        _version,
        _entry,
        program_offset,
        section_offset,
        _flags,
        header_size,
        program_entry_size,
        program_count,
        section_entry_size,
        section_count,
        section_name_index,
    ) = header
    if machine != EM_ARM:
        raise ElfFormatError(f"expected ARM ELF machine {EM_ARM}, got {machine}")
    if header_size < 52:
        raise ElfFormatError("ELF header is smaller than ELF32 requires")
    if section_count == 0 or section_name_index >= section_count:
        raise ElfFormatError("extended or missing section indexes are unsupported")
    if section_entry_size < 40:
        raise ElfFormatError("section-header entries are too small for ELF32")
    if program_count and program_entry_size < 32:
        raise ElfFormatError("program-header entries are too small for ELF32")

    raw_sections: list[tuple[int, ...]] = []
    for index in range(section_count):
        offset = section_offset + index * section_entry_size
        raw_sections.append(
            _unpack_from("<IIIIIIIIII", data, offset, f"section header {index}")
        )

    shstr = raw_sections[section_name_index]
    section_names = _slice(data, shstr[4], shstr[5], "section-name table")
    sections: list[Section] = []
    for index, raw in enumerate(raw_sections):
        (
            name_offset,
            section_type,
            flags,
            address,
            offset,
            size,
            link,
            _info,
            _alignment,
            entry_size,
        ) = raw
        name = "" if index == 0 and name_offset == 0 else _read_c_string(
            section_names, name_offset, f"section {index} name"
        )
        sections.append(
            Section(
                index=index,
                name=name,
                section_type=section_type,
                flags=flags,
                address=address,
                offset=offset,
                size=size,
                link=link,
                entry_size=entry_size,
            )
        )

    program_headers: list[ProgramHeader] = []
    for index in range(program_count):
        offset = program_offset + index * program_entry_size
        raw = _unpack_from("<IIIIIIII", data, offset, f"program header {index}")
        program_headers.append(
            ProgramHeader(
                header_type=raw[0],
                offset=raw[1],
                virtual_address=raw[2],
                physical_address=raw[3],
                file_size=raw[4],
                memory_size=raw[5],
                flags=raw[6],
            )
        )

    symtabs = [section for section in sections if section.section_type == SHT_SYMTAB]
    if len(symtabs) != 1:
        raise ElfFormatError(f"expected exactly one SHT_SYMTAB, found {len(symtabs)}")
    symtab = symtabs[0]
    if symtab.entry_size != 16 or symtab.size % symtab.entry_size:
        raise ElfFormatError("symbol table does not use complete ELF32 symbol entries")
    if symtab.link <= 0 or symtab.link >= len(sections):
        raise ElfFormatError("symbol table has an invalid linked string table")
    string_section = sections[symtab.link]
    strings = _slice(data, string_section.offset, string_section.size, "symbol strings")

    symbols: list[Symbol] = []
    for index in range(symtab.size // symtab.entry_size):
        offset = symtab.offset + index * symtab.entry_size
        name_offset, value, size, info, _other, section_index = _unpack_from(
            "<IIIBBH", data, offset, f"symbol {index}"
        )
        name = _read_c_string(strings, name_offset, f"symbol {index} name")
        symbols.append(
            Symbol(
                index=index,
                name=name,
                value=value,
                size=size,
                symbol_type=info & 0x0F,
                binding=info >> 4,
                section_index=section_index,
            )
        )

    return ElfImage(
        sections=tuple(sections),
        program_headers=tuple(program_headers),
        symbols=tuple(symbols),
    )


def _mapping_symbols(image: ElfImage) -> dict[int, list[tuple[int, str, int]]]:
    mappings: dict[int, list[tuple[int, str, int]]] = {}
    for symbol in image.symbols:
        if symbol.symbol_type != STT_NOTYPE:
            continue
        match = MAPPING_RE.match(symbol.name)
        if match is None:
            continue
        if symbol.section_index <= SHN_UNDEF or symbol.section_index >= len(image.sections):
            continue
        mappings.setdefault(symbol.section_index, []).append(
            (symbol.value, match.group(1), symbol.index)
        )
    for entries in mappings.values():
        entries.sort(key=lambda item: (item[0], item[2]))
    return mappings


def _source_address(
    image: ElfImage, runtime_address: int, size: int
) -> tuple[int | None, str | None]:
    end = runtime_address + size
    candidates: list[ProgramHeader] = []
    for header in image.program_headers:
        if header.header_type != PT_LOAD:
            continue
        loaded_end = header.virtual_address + header.file_size
        if runtime_address >= header.virtual_address and end <= loaded_end:
            candidates.append(header)
    if not candidates:
        return None, "function bytes are not covered by a file-backed PT_LOAD segment"
    if len(candidates) > 1:
        return None, "function bytes are covered by multiple PT_LOAD segments"
    header = candidates[0]
    return header.physical_address + (runtime_address - header.virtual_address), None


def import_symbols(
    image: ElfImage,
    *,
    binary_id: str,
    overlay_id: str = "",
    evidence_source: str,
    evidence_revision: str,
) -> tuple[dict[str, Any], dict[str, Any]]:
    """Build a proven corpus and a separate report for every excluded symbol."""

    mappings = _mapping_symbols(image)
    records: list[dict[str, Any]] = []
    unresolved: list[dict[str, Any]] = []
    internal_mapping_transitions: list[dict[str, Any]] = []
    external_function_symbols = {"undefined": 0, "absolute": 0}

    for symbol in image.symbols:
        if symbol.symbol_type != STT_FUNC:
            continue
        if symbol.section_index == SHN_UNDEF:
            external_function_symbols["undefined"] += 1
            continue
        if symbol.section_index == SHN_ABS:
            external_function_symbols["absolute"] += 1
            continue
        reasons: list[str] = []
        section: Section | None = None
        if symbol.section_index <= SHN_UNDEF or symbol.section_index >= len(image.sections):
            reasons.append("function is not defined in a normal ELF section")
        else:
            section = image.sections[symbol.section_index]
            if not section.name:
                reasons.append("function section has no stable name")
            if not section.flags & SHF_EXECINSTR:
                reasons.append("function section is not marked executable")

        runtime_address = symbol.value & ~1
        if not symbol.name:
            reasons.append("function has no symbol name")
        if symbol.size <= 0:
            reasons.append("function has zero size")

        if section is not None and symbol.size > 0:
            section_end = section.address + section.size
            if runtime_address < section.address or runtime_address + symbol.size > section_end:
                reasons.append("function range is outside its ELF section")

        mode: str | None = None
        function_transitions: list[dict[str, str]] = []
        section_mappings = mappings.get(symbol.section_index, [])
        if section_mappings:
            addresses = [item[0] for item in section_mappings]
            mapping_index = bisect_right(addresses, runtime_address) - 1
            if mapping_index < 0:
                reasons.append("no mapping symbol precedes the function")
            else:
                current_address = section_mappings[mapping_index][0]
                same_address_modes = {
                    entry[1]
                    for entry in section_mappings
                    if entry[0] == current_address
                }
                if len(same_address_modes) != 1:
                    reasons.append("conflicting mapping symbols define the function mode")
                else:
                    mapping_mode = next(iter(same_address_modes))
                    if mapping_mode == "d":
                        reasons.append("function begins in a mapping-symbol data range")
                    else:
                        mode = "arm" if mapping_mode == "a" else "thumb"
                        symbol_thumb = bool(symbol.value & 1)
                        if (mode == "thumb") != symbol_thumb:
                            reasons.append(
                                "function symbol state bit disagrees with mapping-symbol mode"
                            )

                if symbol.size > 0:
                    function_end = runtime_address + symbol.size
                    transitions = {
                        (entry[0], entry[1])
                        for entry in section_mappings[mapping_index + 1 :]
                        if runtime_address < entry[0] < function_end
                    }
                    function_transitions = [
                        {
                            "runtime_address": f"0x{address:08x}",
                            "mapping_kind": {
                                "a": "arm",
                                "t": "thumb",
                                "d": "data",
                            }[mapping_kind],
                        }
                        for address, mapping_kind in sorted(transitions)
                    ]
        else:
            reasons.append("function section has no ARM mapping-symbol evidence")

        source_address: int | None = None
        if symbol.size > 0:
            source_address, source_error = _source_address(
                image, runtime_address, symbol.size
            )
            if source_error is not None:
                reasons.append(source_error)

        if mode is not None and symbol.size > 0:
            alignment = 4 if mode == "arm" else 2
            if runtime_address % alignment:
                reasons.append(f"{mode} runtime address is not {alignment}-byte aligned")
            if symbol.size % alignment:
                reasons.append(f"{mode} size is not a multiple of {alignment}")
            if source_address is not None and source_address % alignment:
                reasons.append(f"{mode} source address is not {alignment}-byte aligned")

        if reasons:
            unresolved.append(
                {
                    "name": symbol.name,
                    "symbol_value": f"0x{symbol.value:08x}",
                    "size": symbol.size,
                    "section": section.name if section is not None else "",
                    "reasons": sorted(set(reasons)),
                }
            )
            continue

        assert section is not None
        assert mode is not None
        assert source_address is not None
        records.append(
            {
                "binary_id": binary_id,
                "overlay_id": overlay_id,
                "name": symbol.name,
                "runtime_address": f"0x{runtime_address:08x}",
                "source_address": f"0x{source_address:08x}",
                "size": symbol.size,
                "mode": mode,
                "section": section.name,
                "evidence_source": evidence_source,
                "evidence_revision": evidence_revision,
                "confidence": "unresolved" if function_transitions else "proven",
            }
        )
        if function_transitions:
            internal_mapping_transitions.append(
                {
                    "name": symbol.name,
                    "runtime_address": f"0x{runtime_address:08x}",
                    "declared_size": symbol.size,
                    "entry_mode": mode,
                    "transitions": function_transitions,
                }
            )

    overlap_reasons: dict[int, set[str]] = {}
    by_start = sorted(
        range(len(records)),
        key=lambda index: (
            int(records[index]["runtime_address"], 16),
            records[index]["name"],
        ),
    )
    active: list[int] = []
    for index in by_start:
        record = records[index]
        start = int(record["runtime_address"], 16)
        active = [
            other
            for other in active
            if int(records[other]["runtime_address"], 16) + records[other]["size"]
            > start
        ]
        for other in active:
            other_record = records[other]
            other_start = int(other_record["runtime_address"], 16)
            overlap_reasons.setdefault(index, set()).add(
                f"range overlaps STT_FUNC {other_record['name']} at 0x{other_start:08x}"
            )
            overlap_reasons.setdefault(other, set()).add(
                f"range overlaps STT_FUNC {record['name']} at 0x{start:08x}"
            )
        active.append(index)

    if overlap_reasons:
        retained: list[dict[str, Any]] = []
        for index, record in enumerate(records):
            if index not in overlap_reasons:
                retained.append(record)
                continue
            runtime_address = int(record["runtime_address"], 16)
            state_bit = 1 if record["mode"] == "thumb" else 0
            unresolved.append(
                {
                    "name": record["name"],
                    "symbol_value": f"0x{runtime_address | state_bit:08x}",
                    "size": record["size"],
                    "section": record["section"],
                    "reasons": sorted(overlap_reasons[index]),
                }
            )
        records = retained

    retained_keys = {
        (record["name"], record["runtime_address"])
        for record in records
    }
    internal_mapping_transitions = [
        observation
        for observation in internal_mapping_transitions
        if (observation["name"], observation["runtime_address"]) in retained_keys
    ]

    records.sort(
        key=lambda record: (
            record["binary_id"],
            record["overlay_id"],
            int(record["runtime_address"], 16),
            record["name"],
        )
    )
    unresolved.sort(
        key=lambda record: (
            record["section"],
            int(record["symbol_value"], 16),
            record["name"],
        )
    )
    internal_mapping_transitions.sort(
        key=lambda observation: (
            int(observation["runtime_address"], 16),
            observation["name"],
        )
    )
    corpus = {"schema_version": 1, "symbols": records}
    report = {
        "schema_version": 1,
        "binary_id": binary_id,
        "overlay_id": overlay_id,
        "evidence_source": evidence_source,
        "evidence_revision": evidence_revision,
        "unresolved": unresolved,
        "internal_mapping_transitions": internal_mapping_transitions,
        "external_function_symbols": external_function_symbols,
    }
    validation_errors = validate_document(corpus)
    if validation_errors:
        joined = "\n".join(f"- {error}" for error in validation_errors)
        raise ElfFormatError(f"imported corpus failed schema validation:\n{joined}")
    return corpus, report


def _write_json(path: Path, document: dict[str, Any]) -> str:
    payload = (json.dumps(document, indent=2, ensure_ascii=True) + "\n").encode("utf-8")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(payload)
    return hashlib.sha256(payload).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Import proven main-image functions from the verified Golden Sun ELF."
        )
    )
    parser.add_argument("--elf", required=True, type=Path, help="main ARM ELF32 file")
    parser.add_argument("--rom", required=True, type=Path, help="user-owned ROM for hash gate")
    parser.add_argument("--output", required=True, type=Path, help="private corpus JSON")
    parser.add_argument(
        "--unresolved-output", required=True, type=Path, help="private unresolved report"
    )
    parser.add_argument("--binary-id", default="golden_sun_main")
    parser.add_argument(
        "--evidence-revision",
        required=True,
        help="40-character lowercase git revision of the ELF-producing checkout",
    )
    args = parser.parse_args()

    if REVISION_RE.fullmatch(args.evidence_revision) is None:
        print("ERROR: --evidence-revision must be a lowercase 40-hex git commit", file=sys.stderr)
        return 2

    rom_path = args.rom.expanduser().resolve()
    if not rom_path.is_file():
        print(f"ERROR: ROM file not found: {rom_path}", file=sys.stderr)
        return 2
    rom_size = rom_path.stat().st_size
    rom_sha1 = sha1_file(rom_path)
    if rom_size != EXPECTED_SIZE or rom_sha1 != EXPECTED_SHA1:
        print(
            "ERROR: unsupported ROM identity; refusing metadata import",
            file=sys.stderr,
        )
        return 1

    elf_path = args.elf.expanduser().resolve()
    try:
        image = parse_elf32_arm(elf_path.read_bytes())
        evidence_source = f"{elf_path.name}:.symtab+mapping-symbols+PT_LOAD"
        corpus, report = import_symbols(
            image,
            binary_id=args.binary_id,
            evidence_source=evidence_source,
            evidence_revision=args.evidence_revision,
        )
        corpus_sha256 = _write_json(args.output.expanduser().resolve(), corpus)
        report_sha256 = _write_json(
            args.unresolved_output.expanduser().resolve(), report
        )
    except (OSError, ElfFormatError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1

    modes = {"arm": 0, "thumb": 0}
    confidence = {"proven": 0, "unresolved": 0}
    for symbol in corpus["symbols"]:
        modes[symbol["mode"]] += 1
        confidence[symbol["confidence"]] += 1
    print(f"ROM SHA-1: {rom_sha1}")
    print(f"Imported: {len(corpus['symbols'])} functions")
    print(f"Modes: ARM={modes['arm']} THUMB={modes['thumb']}")
    print(
        "Confidence: "
        f"proven={confidence['proven']} unresolved={confidence['unresolved']}"
    )
    print(f"Excluded: {len(report['unresolved'])}")
    print(
        "Internal mapping transitions: "
        f"{len(report['internal_mapping_transitions'])} functions"
    )
    print(f"Corpus SHA-256: {corpus_sha256}")
    print(f"Report SHA-256: {report_sha256}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
