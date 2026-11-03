#!/usr/bin/env python3
"""Stage the engine headers used by player builds and overlay self-healing."""

import argparse
from pathlib import Path
import shutil


HEADERS = {
    "builder/engine/include": (
        "src/armv4t/runtime_arm.h",
        "src/armv4t/runtime_arm_types.h",
    ),
    "overlay_toolchain/include": (
        "src/runtime/overlay_runtime_arm.h",
        "src/runtime/overlay_abi.h",
        "src/armv4t/runtime_arm_types.h",
    ),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("engine", type=Path)
    parser.add_argument("game", type=Path)
    args = parser.parse_args()
    for folder, sources in HEADERS.items():
        destination = args.game / folder
        destination.mkdir(parents=True, exist_ok=True)
        for source in sources:
            shutil.copyfile(args.engine / source, destination / Path(source).name)


if __name__ == "__main__":
    main()
