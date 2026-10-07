#!/usr/bin/env python3
"""Fail when obvious protected/private artifacts are present in the repository.

Scope is deliberately "what would end up in a public clone": files git tracks,
plus files that are untracked *and* unignored, since those are one `git add -A`
away from being committed. Ignored paths are out of scope by construction —
`private/`, `roms/`, `build/`, `local/` and friends exist precisely to hold
user-owned ROM, BIOS and trace data, and auditing them produced a permanent
false failure that trained everyone to ignore this gate.
"""

from __future__ import annotations

from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]

FORBIDDEN_SUFFIXES = {
    ".gba", ".bin", ".rom", ".sav", ".srm", ".state", ".trace", ".dump",
    ".bios", ".wav", ".sf2", ".mid",
}

MAX_PUBLIC_FILE = 2 * 1024 * 1024

# Text files worth scanning for private absolute paths. Binary and generated
# content is excluded: it is either unreadable or not human-authored.
TEXT_SUFFIXES = {
    ".md", ".txt", ".py", ".ps1", ".sh", ".toml", ".json", ".yml", ".yaml",
    ".cmake", ".cpp", ".h", ".hpp", ".c", ".cfg", ".ini",
}

# A home directory carrying a real account name. `SECURITY.md` forbids
# committing private filesystem paths, and these leak both the username and
# where the user keeps their ROM.
PRIVATE_PATH_RE = re.compile(
    r"(?:[A-Za-z]:[\\/]Users[\\/]|/home/|/Users/)([A-Za-z0-9._-]+)",
    re.IGNORECASE,
)

# Generic stand-ins that are documentation, not someone's actual home.
PLACEHOLDER_NAMES = {
    "you", "user", "username", "youruser", "yourname", "me", "name",
    "path", "someone", "example",
}


def repo_candidate_files() -> list[Path] | None:
    """Tracked files plus untracked-but-unignored ones, or None without git."""
    try:
        tracked = subprocess.run(
            ["git", "ls-files", "-z"],
            cwd=ROOT, capture_output=True, check=True,
        ).stdout
        untracked = subprocess.run(
            ["git", "ls-files", "-z", "--others", "--exclude-standard"],
            cwd=ROOT, capture_output=True, check=True,
        ).stdout
    except (OSError, subprocess.CalledProcessError):
        return None

    names = [n for n in (tracked + untracked).split(b"\0") if n]
    return [ROOT / n.decode("utf-8") for n in names]


def scan_for_private_paths(path: Path, rel: Path, problems: list[str]) -> None:
    if path.suffix.lower() not in TEXT_SUFFIXES:
        return
    try:
        text = path.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        return
    for line_no, line in enumerate(text.splitlines(), start=1):
        match = PRIVATE_PATH_RE.search(line)
        if match and match.group(1).lower() not in PLACEHOLDER_NAMES:
            problems.append(
                f"private filesystem path: {rel}:{line_no} "
                f"(home directory of '{match.group(1)}')"
            )
            return  # one report per file is enough to act on


def main() -> int:
    problems: list[str] = []

    candidates = repo_candidate_files()
    if candidates is None:
        print(
            "Public repository audit SKIPPED: git is unavailable, so repository "
            "membership cannot be determined.",
            file=sys.stderr,
        )
        return 0

    for path in candidates:
        if not path.is_file():
            continue
        rel = path.relative_to(ROOT)

        if path.suffix.lower() in FORBIDDEN_SUFFIXES:
            problems.append(f"forbidden extension: {rel}")

        size = path.stat().st_size
        if size > MAX_PUBLIC_FILE:
            problems.append(
                f"unexpectedly large public file: {rel} ({size} bytes)"
            )

        scan_for_private_paths(path, rel, problems)

    if problems:
        print("Public repository audit FAILED:", file=sys.stderr)
        for problem in problems:
            print(f"- {problem}", file=sys.stderr)
        return 1

    print("Public repository audit OK: no obvious protected/private artifacts found.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
