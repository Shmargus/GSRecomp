"""Checks that compiled game code turned every guest jump into a host jump.

The game code needs guest jumps (B, computed jumps, fall-throughs) to be host
jumps, or long play runs the stack out. GCC 15 guarantees that; Clang does it
as an optimisation (engine runtime_arm_types.h, GBARECOMP_TAIL_TRANSFER). A
direct call followed only by restoring the stack and returning is a jump the
compiler left as a call.

    python scripts/tail_jump_audit.py --objdump <llvm-objdump> <objects dir>...

Used by scripts/android/build_apk.py --check-games (ARM64).
"""
from __future__ import annotations

import argparse
import concurrent.futures
import os
import re
import subprocess
import sys
from pathlib import Path

INSN = re.compile(r"^\s+[0-9a-f]+:\s+(\S+)\s*(.*)$")

# Per architecture: a direct call, its relocation (on the next line), the
# instructions that may sit between the call and the return (stack restore;
# x86-64 Windows also puts a nop after a call that ends a function, for the
# unwinder), and the return.
ARCHES = {
    "arm64": {
        "call": re.compile(r"^\s+[0-9a-f]+:\s+bl\s"),
        "reloc": re.compile(r"R_AARCH64_CALL26\s+(\S+)"),
        "restore": lambda op, args: op in ("ldp", "ldr", "add") and "sp" in args,
        "ret": ("ret",),
    },
    "x86-64": {
        "call": re.compile(r"^\s+[0-9a-f]+:\s+call\s+0x"),
        "reloc": re.compile(r"(?:IMAGE_REL_AMD64_REL32|R_X86_64_PLT32|R_X86_64_PC32)\s+(\S+)"),
        "restore": lambda op, args: op in ("nop", "pop") or
            (op in ("add", "lea") and args.startswith("rsp")),
        "ret": ("ret",),
    },
}


def missed_in(obj: Path, objdump: str, arch: str) -> list[str]:
    spec = ARCHES[arch]
    flags = {"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {}
    args = [objdump, "-dr", "--no-show-raw-insn"]
    if arch == "x86-64":
        args += ["-M", "intel"]
    text = subprocess.run(args + [str(obj)], capture_output=True, text=True,
                          check=True, **flags).stdout.splitlines()
    found = []
    for i, line in enumerate(text):
        if not spec["call"].match(line) or i + 1 >= len(text):
            continue
        m = spec["reloc"].search(text[i + 1])
        if not m:
            continue
        j = i + 2
        while j < len(text):
            im = INSN.match(text[j])
            if not im:
                j += 1
                continue
            op, rest = im.group(1), im.group(2)
            if op in spec["ret"]:
                found.append(m.group(1))
                break
            if spec["restore"](op, rest):
                j += 1
                continue
            break
    return found


def audit(objects_dirs: list[Path], objdump: str, arch: str) -> dict[str, list[str]]:
    """Every compiled game file's missed jumps, by file name (empty = none)."""
    objects = sorted(p for d in objects_dirs for p in d.rglob("*")
                     if p.suffix in (".o", ".obj"))
    if not objects:
        raise RuntimeError(f"no compiled game-code objects in {', '.join(map(str, objects_dirs))}")
    print(f"Checking {len(objects)} game-code files for guest jumps ...", flush=True)
    bad = {}
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 4) as pool:
        for obj, calls in zip(objects, pool.map(lambda o: missed_in(o, objdump, arch), objects)):
            if calls:
                bad[obj.name] = calls
    return bad


def report(bad: dict[str, list[str]]) -> bool:
    if bad:
        for name, calls in list(bad.items())[:10]:
            print(f"    {name}: {len(calls)} (e.g. {calls[0]})")
        print("Some guest jumps compiled as calls; the game could run out of stack.")
        return False
    print("    all guest jumps are host jumps")
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--objdump", required=True)
    parser.add_argument("--arch", choices=ARCHES, default="x86-64")
    parser.add_argument("objects", nargs="+", type=Path)
    args = parser.parse_args()
    try:
        return 0 if report(audit(args.objects, args.objdump, args.arch)) else 1
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"Jump check failed: {error}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
