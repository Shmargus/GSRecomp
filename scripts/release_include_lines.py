#!/usr/bin/env python3
"""Print every distinct `#include <...>` line in the generated game code.

    python scripts/release_include_lines.py <folder> [<folder> ...]

make_release.ps1 compiles these lines once to learn which standard headers
the trimmed compiler must ship. Reads *.cpp and *.h under each folder; one
line per distinct include, sorted, spaces at the ends removed.
"""
import re
import sys
from pathlib import Path

INCLUDE = re.compile(rb"^[ \t]*#[ \t]*include[ \t]*<[^\r\n]*", re.M)


def main() -> int:
    lines = set()
    for root in sys.argv[1:]:
        for path in Path(root).rglob("*"):
            if path.suffix in (".cpp", ".h") and path.is_file():
                for match in INCLUDE.finditer(path.read_bytes()):
                    lines.add(match.group(0).strip().decode("ascii", "replace"))
    sys.stdout.write("".join(line + "\n" for line in sorted(lines)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
