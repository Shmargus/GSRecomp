#!/usr/bin/env python3
"""Replay every saved capture through two renderer builds and compare.

Every margin fix so far was checked the same way by hand: replay the F12
frames (logs/gpu_frame_*.bin) and the rewind rings (logs/gpu_rewind_*/)
through tools/scene_renderer_check.cpp built before and after the change,
and look at what moved. This does that in one command.

  1. Build the replay tool from the working tree (before editing):
       python tools/replay_compare.py --build local/replay_old.exe
  2. Make the change, build again:
       python tools/replay_compare.py --build local/replay_new.exe
  3. Compare:
       python tools/replay_compare.py --old local/replay_old.exe \\
           --new local/replay_new.exe

Add --game tla to do the same for The Lost Age: it builds
"The Lost Age/tools/tla_frame_check.cpp" and replays the tla_rewind_*
rings in "The Lost Age/logs" (wide 360x240 picture).

The report lists each capture whose 360x240 picture changed, how many pixels,
and whether any of them are inside the console's own 240x160 window (which
must normally never change). For each changed capture it writes
old | new | changed-pixels PNGs to the output folder.

Captures hold game memory and stay local (logs/ and local/ are not
published); so do the pictures this writes.
"""

from __future__ import annotations

import argparse
import concurrent.futures as futures
import datetime
import glob
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TLA_ROOT = os.path.join(os.path.dirname(ROOT), "The Lost Age")
MINGW_BIN = "C:/msys64/mingw64/bin"
GAME = "gs"

# The replay tool's own build line (tools/scene_renderer_check.cpp header).
SOURCES = [
    "tools/scene_renderer_check.cpp",
    "../engine/src/field/field_scene.cpp",
    "../engine/src/field/field_scene_renderer.cpp",
    "src/field_game_profile.cpp",
    "src/world_map_source.cpp",
    "../engine/src/field/effect_particles.cpp",
    "../engine/src/field/effect_burst.cpp",
    "../engine/src/runtime/gpu_surface.cpp",
    "../engine/src/gba/gba_ppu.cpp",
]
INCLUDES = ["src", "../engine/src/field", "../engine/src/gba", "../engine/src/runtime",
            "../engine/src/armv4t", "../engine/src/debug",
            "C:/msys64/mingw64/include/SDL2"]
# The Lost Age's frame checker (its CMake target tla_frame_check), built the
# same way. Paths are relative to "The Lost Age".
TLA_SOURCES = [
    "tools/tla_frame_check.cpp",
    "../engine/src/field/field_scene.cpp",
    "../engine/src/field/field_scene_renderer.cpp",
    "src/field_game_profile.cpp",
    "src/world_map_source.cpp",
    "../engine/src/field/effect_particles.cpp",
    "../engine/src/field/effect_burst.cpp",
    "../engine/src/runtime/gpu_surface.cpp",
    "../engine/src/gba/gba_ppu.cpp",
]


def tool_env() -> dict:
    env = dict(os.environ)
    env["PATH"] = MINGW_BIN + os.pathsep + env.get("PATH", "")
    return env


def build(out: str) -> int:
    out = os.path.abspath(out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    cmd = [MINGW_BIN + "/g++.exe", "-O2", "-w", "-std=gnu++20"]
    for inc in INCLUDES:
        cmd += ["-I", inc]
    cmd += ["-o", out] + (TLA_SOURCES if GAME == "tla" else SOURCES)
    cmd += ["-lmingw32", "-lSDL2main", "-lSDL2", "-lopengl32"]
    print("building", out)
    return subprocess.run(cmd, cwd=TLA_ROOT if GAME == "tla" else ROOT,
                          env=tool_env()).returncode


def captures(logs: str, step: int, dense: set[str]) -> list[str]:
    files = sorted(glob.glob(os.path.join(logs, "gpu_frame_*.bin")))
    ring = "tla_rewind_*" if GAME == "tla" else "gpu_rewind_*"
    for d in sorted(glob.glob(os.path.join(logs, ring))):
        name = os.path.basename(d)
        every = 5 if name[-4:] in dense else step
        frames = sorted(glob.glob(os.path.join(d, "frame_*.bin")))
        files += frames[::every]
    return files


def tag_of(path: str) -> str:
    parent = os.path.basename(os.path.dirname(path))
    base = os.path.basename(path)
    return (parent + "_" + base
            if parent.startswith(("gpu_rewind_", "tla_rewind_")) else base)


def replay(exe: str, src: str, work: str) -> str | None:
    """Replays one capture; returns the path of its 360x240 picture."""
    dst = os.path.join(work, tag_of(src))
    if GAME == "tla":
        # tla_frame_check <frame.bin> <out prefix>: <prefix>_wide.ppm.
        subprocess.run([exe, src, dst], env=tool_env(),
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        ppm = dst + "_wide.ppm"
        return ppm if os.path.exists(ppm) else None
    shutil.copyfile(src, dst)
    subprocess.run([exe, "--replay", dst], env=tool_env(),
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    png = dst + ".gpu.png"
    return png if os.path.exists(png) else None


def compare(old_png: str, new_png: str):
    from PIL import Image, ImageChops
    a = Image.open(old_png).convert("RGB")
    b = Image.open(new_png).convert("RGB")
    if a.size != b.size:
        return a, b, None, -1, True
    diff = ImageChops.difference(a, b).convert("L").point(
        lambda v: 255 if v else 0)
    changed = diff.histogram()[255]
    w, h = a.size
    x0, y0 = (w - 240) // 2, (h - 160) // 2
    inside = diff.crop((x0, y0, x0 + 240, y0 + 160)).getbbox() is not None
    return a, b, diff, changed, inside


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--build", metavar="EXE",
                    help="build the replay tool from the working tree")
    ap.add_argument("--old", metavar="EXE")
    ap.add_argument("--new", metavar="EXE")
    ap.add_argument("--game", choices=("gs", "tla"), default="gs")
    ap.add_argument("--logs", default=None,
                    help="capture folder (default: the game's logs/)")
    ap.add_argument("--step", type=int, default=20,
                    help="replay every Nth frame of each rewind (default 20)")
    ap.add_argument("--dense", default="",
                    help="rewind numbers to replay every 5th frame, e.g. 0093,0097")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--out", default=None,
                    help="output folder (default local/replay_compare/<time>)")
    args = ap.parse_args()
    global GAME
    GAME = args.game
    if args.logs is None:
        args.logs = os.path.join(TLA_ROOT if GAME == "tla" else ROOT, "logs")

    if args.build:
        return build(args.build)
    if not (args.old and args.new):
        ap.error("give --build, or both --old and --new")

    stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    out = args.out or os.path.join(ROOT, "local", "replay_compare",
                                   ("tla_" if GAME == "tla" else "") + stamp)
    work = {k: os.path.join(out, "work_" + k) for k in ("old", "new")}
    for d in work.values():
        os.makedirs(d, exist_ok=True)
    old_exe, new_exe = os.path.abspath(args.old), os.path.abspath(args.new)
    files = captures(args.logs, args.step, set(filter(None, args.dense.split(","))))
    print(f"{len(files)} captures, {args.jobs} at a time -> {out}")

    def one(src: str):
        return (src, replay(old_exe, src, work["old"]),
                replay(new_exe, src, work["new"]))

    same, changed, failed = 0, [], []
    with futures.ThreadPoolExecutor(args.jobs) as pool:
        for i, (src, a, b) in enumerate(pool.map(one, files), 1):
            tag = tag_of(src)
            if not a or not b:
                failed.append(tag)
                continue
            old_img, new_img, diff, n, inside = compare(a, b)
            if n == 0:
                same += 1
            else:
                changed.append((tag, n, inside))
                if diff is not None:
                    from PIL import Image
                    w, h = old_img.size
                    sheet = Image.new("RGB", (w * 3, h))
                    sheet.paste(old_img, (0, 0))
                    sheet.paste(new_img, (w, 0))
                    sheet.paste(diff.convert("RGB"), (w * 2, 0))
                    sheet.save(os.path.join(out, tag + ".png"))
            if i % 100 == 0:
                print(f"  {i}/{len(files)}")

    lines = [f"identical {same}, changed {len(changed)}, "
             f"not replayed {len(failed)} (of {len(files)})"]
    inside = [c for c in changed if c[2]]
    lines.append(f"changed INSIDE the console window: {len(inside)}")
    for tag, n, ins in changed:
        lines.append(f"  {tag}: {n if n >= 0 else 'size differs'} px"
                     f"{'  INSIDE WINDOW' if ins else ''}")
    for tag in failed:
        lines.append(f"  not replayed: {tag}")
    report = "\n".join(lines)
    print(report)
    with open(os.path.join(out, "summary.txt"), "w", encoding="utf-8") as f:
        f.write(report + "\n")
    for d in work.values():
        shutil.rmtree(d, ignore_errors=True)
    return 1 if inside else 0


if __name__ == "__main__":
    sys.exit(main())
