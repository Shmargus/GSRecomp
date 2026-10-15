"""Record exact developer inputs and outputs only after a successful build."""
import argparse
import hashlib
import os
from pathlib import Path, PurePosixPath
import stat
import sys
import tempfile

HEADER = "developer_build=1\n"
# On Windows the engine is a plug-in beside the program (GoldenSunEngine.dll,
# TLAEngine.dll; launcher_game.h); on Linux it is the program itself.
GAMES = {"BrokenSeal": ("The Broken Seal", "gs011_opt", "gs011_opt", "GoldenSunRecomp", "GoldenSunGame", "GoldenSunEngine"),
         "LostAge": ("The Lost Age", "tla", "tla-linux", "TLARecomp", "TLAGame", "TLAEngine")}


def safe_path(root, name):
    relative = PurePosixPath(name)
    if (not name or "\\" in name or ":" in name or "\t" in name or "\n" in name
            or "\r" in name or relative.is_absolute() or any(p in (".", "..") for p in name.split("/"))):
        raise ValueError(f"Unsafe collection path: {name!r}")
    path = root.joinpath(*relative.parts)
    current = root
    for component in relative.parts:
        current = current / component
        if current.is_symlink():
            raise ValueError(f"Symlink input is unsupported: {current}")
    path.resolve(strict=True).relative_to(root)
    return path


def excluded(name):
    return name.startswith(".") or ".bak" in name or name == "__pycache__"


def input_name(name):
    return name.replace("{tla_corpus}", "tla" if os.name == "nt" else "tla_linux")


def inventory(root, game):
    files = {}
    spec = root / "scripts/developer_build_inputs.txt"
    for number, line in enumerate(spec.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip() or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) != 2 or fields[0] not in ("Both", *GAMES):
            raise ValueError(f"Invalid input specification line {number}")
        scope, name = fields
        if scope not in ("Both", game):
            continue
        path = safe_path(root, input_name(name))
        if path.is_dir():
            def walk(directory):
                for child in directory.iterdir():
                    if excluded(child.name):
                        continue
                    if child.is_symlink():
                        raise ValueError(f"Symlink input is unsupported: {child}")
                    child.resolve(strict=True).relative_to(root)
                    mode = child.stat().st_mode
                    if stat.S_ISDIR(mode):
                        walk(child)
                    elif stat.S_ISREG(mode):
                        files[child.relative_to(root).as_posix()] = child
                    else:
                        raise ValueError(f"Nonregular input: {child}")
            walk(path)
        elif path.is_file():
            files[path.relative_to(root).as_posix()] = path
        else:
            raise ValueError(f"Nonregular input: {path}")
    return files


def digest(path):
    sha = hashlib.sha1()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            sha.update(block)
    return sha.hexdigest()


def rows(files):
    return {name: digest(path) for name, path in files.items()}


def encode(values):
    return (HEADER + "".join(f"{values[name]}\t{name}\n" for name in sorted(values))).encode("utf-8")


def atomic_write(path, data):
    fd, temporary = tempfile.mkstemp(prefix=path.name + ".", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--game", choices=GAMES, required=True)
    parser.add_argument("--build-dir", type=Path)
    operation = parser.add_mutually_exclusive_group(required=True)
    operation.add_argument("--begin", action="store_true")
    operation.add_argument("--finish", action="store_true")
    parser.add_argument("--snapshot", type=Path, required=True)
    args = parser.parse_args()
    root = args.root.resolve(strict=True)
    folder, windows_dir, linux_dir, program, library, engine = GAMES[args.game]
    build = args.build_dir or root / folder / "build" / (windows_dir if os.name == "nt" else linux_dir)
    if not build.is_absolute():
        build = root / build
    build = build.resolve(strict=True)
    build.relative_to(root)
    stamp = build / "collection-build.txt"
    # Invalidate before hashing: a failed begin cannot leave an old certificate.
    if args.begin:
        stamp.unlink(missing_ok=True)
    current = rows(inventory(root, args.game))
    data = encode(current)
    if args.begin:
        atomic_write(args.snapshot, data)
        return
    if args.snapshot.read_bytes() != data:
        raise ValueError("Developer inputs changed during the build; no build record written.")
    outputs = ((program + ".exe", library + ".dll", engine + ".dll") if os.name == "nt"
               else (program, "lib" + library + ".so"))
    # Build scripts can update CMake settings, so record their final cache after
    # success, rather than requiring it to match the pre-build source snapshot.
    for name in (*outputs, "CMakeCache.txt"):
        binary = build / name
        if binary.is_symlink():
            raise ValueError(f"Symlink output is unsupported: {binary}")
        binary.resolve(strict=True).relative_to(root)
        if not binary.is_file() or binary.stat().st_size == 0:
            raise ValueError(f"Built output is missing or empty: {binary}")
        current[binary.relative_to(root).as_posix()] = digest(binary)
    atomic_write(stamp, encode(current))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as error:
        print(f"Developer build record failed: {error}", file=sys.stderr)
        sys.exit(1)
