#!/usr/bin/env python3
"""Rewrite generated game code (local/gs011/**/recompiled*.cpp) so fixed
multi-call sequences become one call to a combined runtime helper.

Every rule matches a sequence gba_recompile writes verbatim and replaces it
with a helper in gbarecomp/src/runtime/runtime_bus_bridge.cpp whose body is
that same sequence, in the same order (declarations and the argument for each
in runtime_arm.h, "Combined per-instruction helpers"). The game does the same
work; its machine code carries one call where it carried two to four.

Anything that does not match a rule exactly is left alone. The rewrite is
idempotent: rewritten text no longer matches any rule.

    python tools/slim_corpus.py local/gs011            # rewrite in place
    python tools/slim_corpus.py local/gs011 --check    # count only
"""

from __future__ import annotations

import argparse
import re
import sys
from collections import Counter
from pathlib import Path

WIDTH = {"32": "4", "16": "2", "8": "1"}

# 1. Yield check + crash-trail check at the top of every instruction.
BOUNDARY = re.compile(
    r"^([ \t]*)if \(runtime_should_yield\(\)\) return;\n"
    r"\1if \(g_runtime_insn_trace\) runtime_insn_fp\(\);$",
    re.M,
)

# 1b. An unconditional instruction's opening: PC store, the boundary from
# rule 1, then its sequential fetch cost. runtime_insn_begin returns that
# cost (>= 1) or 0 where the boundary returned. The fetch cost depends only
# on the region and WAITCNT, which nothing in the boundary changes.
BEGIN = re.compile(
    r"^([ \t]*)g_cpu\.R\[15\] = (0x[0-9A-F]+u);\n"
    r"\1if \(runtime_insn_boundary\(\)\) return;\n"
    r"\1uint32_t (_cyc_\w+) = 1u;\n"
    r"\1\3 = (\d+)u;\n"
    r"\1\3 \+= runtime_mem_cycles\(\2, ([24])u, 1u\) - 1u;$",
    re.M,
)

# 2. N-minus-S fetch correction after a load or store.
NS_DELTA = re.compile(
    r"runtime_mem_cycles\(([^,()\n]+), ([24])u, 0u\) - "
    r"runtime_mem_cycles\(\1, \2u, 1u\)"
)

# 3. Pipeline refill after a taken branch.
REFILL = re.compile(
    r"^([ \t]*)(_cyc_\w+) \+= runtime_mem_cycles\(([^,()\n]+), ([24])u, 0u\) - 1u;\n"
    r"\1\2 \+= runtime_mem_cycles\(\3 \+ \4u, \4u, 1u\) - 1u;$",
    re.M,
)

# 5. Single store: trace event, bus write, data cost.
STORE = re.compile(
    r"^([ \t]*)runtime_trace_event\(RUNTIME_TRACE_MEM_WRITE, ([^,\n]+), ([^,\n]+), ([^,\n]+), ([124])u\);\n"
    r"\1bus_write_u(32|16|8)\(\3, ([^\n]+)\);\n"
    r"\1(_cyc_\w+) \+= runtime_mem_cycles\((_ea_\w+), \5u, 2u\);$",
    re.M,
)

# 6. LDM slot: cost, then the load into a register.
LDM = re.compile(
    r"^([ \t]*)(_cyc_\w+) \+= runtime_mem_cycles\((_a_\w+ & ~3u), 4u, ([01])u\);\n"
    r"\1(g_cpu\.R\[\d+\]) = bus_read_u32\(\3\);$",
    re.M,
)

# 7. STM slot: cost, trace event, bus write.
STM = re.compile(
    r"^([ \t]*)(_cyc_\w+) \+= runtime_mem_cycles\((_a_\w+ & ~3u), 4u, ([01])u\);\n"
    r"\1runtime_trace_event\(RUNTIME_TRACE_MEM_WRITE, ([^,\n]+), \3, ([^,\n]+), 4u\);\n"
    r"\1bus_write_u32\(\3, \6\);$",
    re.M,
)

# 4. Single load: the statement holding exactly one bus read of _ea_X,
# immediately followed by its data cost.
LOAD_COST = re.compile(
    r"^([ \t]*)(_cyc_\w+) \+= runtime_mem_cycles\((_ea_\w+), ([124])u, 2u\);$"
)
READ_CALL = re.compile(r"bus_read_u(32|16|8)\(")


def store_value_matches(bits: str, traced: str, stored: str) -> bool:
    if bits == "32":
        return traced == stored
    inner_t = traced.removeprefix("(uint32_t)")
    inner_s = stored.removeprefix(f"(uint{bits}_t)")
    return traced != inner_t and stored != inner_s and inner_t == inner_s


def rewrite(text: str, counts: Counter) -> str:
    def boundary(m):
        counts["boundary"] += 1
        return f"{m.group(1)}if (runtime_insn_boundary()) return;"

    text = BOUNDARY.sub(boundary, text)

    def begin(m):
        indent, pc, cyc, base, width = m.groups()
        counts["begin"] += 1
        return (f"{indent}uint32_t {cyc} = runtime_insn_begin({pc}, {width}u);\n"
                f"{indent}if ({cyc} == 0u) return;\n"
                f"{indent}{cyc} += {base}u - 1u;")

    text = BEGIN.sub(begin, text)

    def store(m):
        indent, pc, addr, traced, width, bits, stored, cyc, ea = m.groups()
        if WIDTH[bits] != width or not store_value_matches(bits, traced, stored):
            counts["store_skipped"] += 1
            return m.group(0)
        counts["store"] += 1
        return f"{indent}{cyc} += runtime_st_u{bits}({pc}, {addr}, {ea}, {traced});"

    text = STORE.sub(store, text)

    def stm(m):
        indent, cyc, addr, seq, pc, value = m.groups()
        counts["stm"] += 1
        return f"{indent}{cyc} += runtime_stm_u32({pc}, {addr}, {value}, {seq}u);"

    text = STM.sub(stm, text)

    def ldm(m):
        indent, cyc, addr, seq, reg = m.groups()
        counts["ldm"] += 1
        return f"{indent}{cyc} += runtime_ldm_u32({addr}, {seq}u, &{reg});"

    text = LDM.sub(ldm, text)

    def refill(m):
        indent, cyc, target, width = m.groups()
        counts["refill"] += 1
        return f"{indent}{cyc} += runtime_refill_cycles({target}, {width}u);"

    text = REFILL.sub(refill, text)

    def ns_delta(m):
        counts["ns_delta"] += 1
        return f"runtime_fetch_ns_delta({m.group(1)}, {m.group(2)}u)"

    text = NS_DELTA.sub(ns_delta, text)

    # Loads, line by line: the read's statement is the line just before the
    # cost line, or the two lines before it for the translator's two-line
    # rotated 32-bit read.
    lines = text.split("\n")
    for i, line in enumerate(lines):
        m = LOAD_COST.match(line)
        if not m:
            continue
        indent, cyc, ea, width = m.groups()
        prev = [i - 1]
        if lines[i - 1].lstrip().startswith("uint32_t _rot ="):
            prev = [i - 2, i - 1]
        block = "\n".join(lines[j] for j in prev)
        reads = READ_CALL.findall(block)
        before = lines[prev[0] - 1]
        # One read, of this address, not one arm of an if/else pair.
        if len(reads) != 1 or WIDTH[reads[0]] != width or \
                "bus_write" in block or f"({ea}" not in block or \
                block.lstrip().startswith(("else", "if")) or \
                "bus_read" in before:
            counts["load_skipped"] += 1
            continue
        bits = reads[0]
        call = re.compile(rf"bus_read_u{bits}\(({re.escape(ea)}(?: & ~[13]u)?)\)")
        for j in prev:
            new, n = call.subn(rf"runtime_ld_u{bits}(\1, {ea})", lines[j])
            lines[j] = new
            if n:
                break
        else:
            counts["load_skipped"] += 1
            continue
        lines[i] = f"{indent}{cyc} += g_runtime_data_cost;"
        counts["load"] += 1
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("root", type=Path)
    parser.add_argument("--check", action="store_true",
                        help="count what would change; write nothing")
    args = parser.parse_args()
    # Skip backups (main.bak-*, hidden .backup_*/.overlay-stage folders).
    files = sorted(
        p for p in args.root.rglob("recompiled*.cpp")
        if not any(part.startswith(".") or ".bak" in part
                   for part in p.relative_to(args.root).parts[:-1]))
    total = Counter()
    changed = 0
    for path in files:
        raw = path.read_bytes()
        text = raw.decode("utf-8")
        crlf = "\r\n" in text
        if crlf:
            text = text.replace("\r\n", "\n")
        counts = Counter()
        new = rewrite(text, counts)
        total.update(counts)
        if new != text:
            changed += 1
            if not args.check:
                if crlf:
                    new = new.replace("\n", "\r\n")
                path.write_bytes(new.encode("utf-8"))
    print(f"{len(files)} files, {changed} {'would change' if args.check else 'changed'}")
    for key in sorted(total):
        print(f"  {key:14s} {total[key]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
