"""Read the spell-effect canvas trace and say whether a wider canvas is worth building.

The game software-paints each frame's spell effect into an affine background's
character block, used as a 128x128 8-bit canvas, and the hardware stretches that
canvas across the console's 240-pixel screen (FACTS.md, 2026-09-18). The canvas
is therefore only as wide as the old screen, which is why a widened view shows
the effect stopping in a straight line.

The trace (launcher: "Trace spell effect canvas") writes four files per session:

    logs/effect_canvas_writers_NNN.csv   who painted, and how far they reached
    logs/effect_canvas_columns_NNN.csv   a per-frame column histogram
    logs/effect_stage_writers_NNN.csv    who wrote the IWRAM staging buffer
    logs/effect_stage_walk_NNN.csv       those writes in order, for the stride

This reads them and answers three questions:

  1. WHICH ROUTINES PAINT.  The writer PCs are guest ROM addresses, so they can
     be looked up in the symbol corpus and their coordinate arithmetic read.
     A small number of routines shared by every spell is the good case: one
     place to widen.

  2. WHAT THE STAMP ROUTINE'S STRIDE IS.  The effect is composed in a 16 KB
     buffer on the game's IWRAM heap and blitted whole into VRAM, and the
     routine that stamps particles into it is relocated at runtime -- it cannot
     be read out of the ROM.  The ordered walk shows its address steps, which
     is what says whether it writes tile-ordered (8 bytes, then a jump) or
     linearly, and with what row stride.  That stride has to change with the
     canvas width.

  3. WHETHER CONTENT IS BEING LOST AT THE EDGE.  A painter that CLAMPS what
     will not fit piles writes onto the first or last column, which shows here
     as an edge spike against the interior mean.  A painter that DROPS it shows
     no spike -- and then only reading the routine settles it.

Usage:
    python tools/analyze_effect_canvas.py [logs-dir-or-csv ...]

With no argument it reads every effect_canvas_*.csv and effect_stage_*.csv
under logs/.

Not part of the product. Reads only host-written diagnostic files.
"""

import collections
import glob
import os
import sys


def load_writers(path):
    rows = []
    with open(path, "r", encoding="utf-8") as handle:
        header = handle.readline().strip().split(",")
        for line in handle:
            parts = line.strip().split(",")
            if len(parts) != len(header):
                continue
            rows.append(dict(zip(header, parts)))
    return rows


def load_columns(path):
    frames = []
    with open(path, "r", encoding="utf-8") as handle:
        handle.readline()
        for line in handle:
            parts = line.strip().split(",")
            if len(parts) < 3:
                continue
            frame = int(parts[0])
            width = int(parts[1])
            counts = [int(value) for value in parts[2:]]
            frames.append((frame, width, counts))
    return frames


def report_writers(rows):
    if not rows:
        print("no writer rows")
        return

    per_pc = collections.OrderedDict()
    for row in rows:
        key = (row["pc"], row["kind"])
        entry = per_pc.setdefault(
            key,
            {
                "frames": 0,
                "writes": 0,
                "bytes": 0,
                "min_col": 1 << 30,
                "max_col": -1,
                "min_row": 1 << 30,
                "max_row": -1,
                "layers": set(),
            },
        )
        entry["frames"] += 1
        entry["writes"] += int(row["writes"])
        entry["bytes"] += int(row["bytes"])
        entry["min_col"] = min(entry["min_col"], int(row["min_col"]))
        entry["max_col"] = max(entry["max_col"], int(row["max_col"]))
        entry["min_row"] = min(entry["min_row"], int(row["min_row"]))
        entry["max_row"] = max(entry["max_row"], int(row["max_row"]))
        entry["layers"].add(row["layer"])

    frames = {int(row["frame"]) for row in rows}
    widths = {int(row["canvas_px"]) for row in rows}
    bases = {row["canvas_base"] for row in rows}
    print("frames with canvas writes : %d" % len(frames))
    print("canvas widths seen        : %s" % ", ".join(sorted(str(w) for w in widths)))
    print("canvas bases seen         : %s" % ", ".join(sorted(bases)))
    print()
    print("painters, busiest first:")
    print("  %-12s %-4s %-6s %10s %12s  %-12s %-12s" % (
        "pc", "kind", "layer", "frames", "bytes", "columns", "rows"))
    ordered = sorted(per_pc.items(), key=lambda kv: -kv[1]["bytes"])
    for (pc, kind), entry in ordered:
        print("  %-12s %-4s %-6s %10d %12d  %-12s %-12s" % (
            pc,
            kind,
            "/".join(sorted(entry["layers"])),
            entry["frames"],
            entry["bytes"],
            "%d..%d" % (entry["min_col"], entry["max_col"]),
            "%d..%d" % (entry["min_row"], entry["max_row"]),
        ))
    print()
    print("Look each pc up in config/usa/main.toml (or the generated sources) to")
    print("read its coordinate arithmetic. A painter whose columns span the whole")
    print("canvas every frame is the one that decides the effect's width.")


def report_columns(frames):
    if not frames:
        print("\nno column histogram rows")
        return

    width = frames[0][1]
    total = [0] * width
    for _, frame_width, counts in frames:
        if frame_width != width:
            continue
        for index, value in enumerate(counts[:width]):
            total[index] += value

    interior = total[4:width - 4]
    if not interior:
        print("\ncanvas too narrow to judge an edge spike")
        return
    mean = sum(interior) / float(len(interior))
    print()
    print("column histogram over %d frames, canvas %d wide" % (len(frames), width))
    print("  interior mean writes/column : %.1f" % mean)
    print("  column 0                    : %d  (%.2fx interior)" % (
        total[0], total[0] / mean if mean else 0.0))
    print("  column %-21d: %d  (%.2fx interior)" % (
        width - 1, total[width - 1], total[width - 1] / mean if mean else 0.0))
    peak = max(range(width), key=lambda index: total[index])
    print("  busiest column              : %d (%d writes)" % (peak, total[peak]))
    print()
    ratio_lo = total[0] / mean if mean else 0.0
    ratio_hi = total[width - 1] / mean if mean else 0.0
    if max(ratio_lo, ratio_hi) >= 2.0:
        print("  READ: an edge column carries well over its share of the painting,")
        print("  which is what clamping looks like -- the painter has content out")
        print("  there and is folding it onto the boundary. A wider canvas would")
        print("  show that content in its real place.")
    else:
        print("  READ: no edge pile-up. The painter is not clamping, so this alone")
        print("  does not prove content exists past the canvas. Read the routines")
        print("  above before deciding: a painter that simply skips what does not")
        print("  fit looks exactly like this.")


def load_stage_writers(path):
    rows = []
    with open(path, "r", encoding="utf-8") as handle:
        header = handle.readline().strip().split(",")
        for line in handle:
            parts = line.strip().split(",")
            if len(parts) == len(header):
                rows.append(dict(zip(header, parts)))
    return rows


def load_stage_walk(path):
    rows = []
    with open(path, "r", encoding="utf-8") as handle:
        handle.readline()
        for line in handle:
            parts = line.strip().split(",")
            if len(parts) == 5:
                rows.append((int(parts[0]), int(parts[1]), parts[2],
                             int(parts[3], 16), int(parts[4])))
    return rows


def report_stage(writers, walk):
    if not writers and not walk:
        return
    print()
    print("=" * 68)
    print("IWRAM staging buffer -- where the effect is actually composed")
    print("=" * 68)

    per_pc = collections.OrderedDict()
    for row in writers:
        entry = per_pc.setdefault(row["pc"], {
            "frames": 0, "writes": 0, "bytes": 0,
            "low": 1 << 32, "high": 0})
        entry["frames"] += 1
        entry["writes"] += int(row["writes"])
        entry["bytes"] += int(row["bytes"])
        entry["low"] = min(entry["low"], int(row["low"], 16))
        entry["high"] = max(entry["high"], int(row["high"], 16))

    if per_pc:
        print()
        print("  %-12s %8s %12s  %-24s %s" % (
            "pc", "frames", "bytes", "address span", "span size"))
        for pc, e in sorted(per_pc.items(), key=lambda kv: -kv[1]["bytes"]):
            span = e["high"] - e["low"] + 1
            flag = "  <-- 16 KB, this is the canvas" if 0x3800 <= span <= 0x4800 else ""
            print("  %-12s %8d %12d  0x%08X..0x%08X %9d%s" % (
                pc, e["frames"], e["bytes"], e["low"], e["high"], span, flag))

    if not walk:
        return
    by_pc = collections.defaultdict(list)
    for frame, seq, pc, addr, size in walk:
        by_pc[pc].append((frame, seq, addr, size))
    print()
    print("  address steps between consecutive writes, per writer:")
    for pc, rows in sorted(by_pc.items(), key=lambda kv: -len(kv[1]))[:6]:
        deltas = collections.Counter()
        for (f0, s0, a0, z0), (f1, s1, a1, z1) in zip(rows, rows[1:]):
            if f0 != f1 or s1 != s0 + 1:
                continue
            deltas[a1 - a0] += 1
        if not deltas:
            continue
        common = ", ".join("%+d x%d" % (d, n) for d, n in deltas.most_common(6))
        print("    %-12s %5d writes: %s" % (pc, len(rows), common))
    print()
    print("  READ: a run of +1/+2/+4 broken by a jump of about +56 is an 8x8")
    print("  tile-ordered blit, and a step near +1024 is one tile ROW -- 16")
    print("  tiles of 64 bytes. Those two numbers are the stride that has to")
    print("  change when the canvas gets wider.")


def main(argv):
    targets = argv[1:]
    if not targets:
        targets = ["logs"]

    writer_files = []
    column_files = []
    stage_writer_files = []
    stage_walk_files = []
    for target in targets:
        if os.path.isdir(target):
            writer_files += sorted(glob.glob(
                os.path.join(target, "effect_canvas_writers_*.csv")))
            column_files += sorted(glob.glob(
                os.path.join(target, "effect_canvas_columns_*.csv")))
            stage_writer_files += sorted(glob.glob(
                os.path.join(target, "effect_stage_writers_*.csv")))
            stage_walk_files += sorted(glob.glob(
                os.path.join(target, "effect_stage_walk_*.csv")))
        elif "stage_writers" in os.path.basename(target):
            stage_writer_files.append(target)
        elif "stage_walk" in os.path.basename(target):
            stage_walk_files.append(target)
        elif "writers" in os.path.basename(target):
            writer_files.append(target)
        else:
            column_files.append(target)

    if not (writer_files or column_files or stage_writer_files
            or stage_walk_files):
        print("no effect canvas trace files found in: %s" % ", ".join(targets))
        print("Tick 'Trace spell effect canvas' in the launcher and cast a spell.")
        return 1

    rows = []
    for path in writer_files:
        print("reading %s" % path)
        rows += load_writers(path)
    frames = []
    for path in column_files:
        print("reading %s" % path)
        frames += load_columns(path)
    print()

    report_writers(rows)
    report_columns(frames)

    stage_writers = []
    for path in stage_writer_files:
        print("reading %s" % path)
        stage_writers += load_stage_writers(path)
    stage_walk = []
    for path in stage_walk_files:
        print("reading %s" % path)
        stage_walk += load_stage_walk(path)
    report_stage(stage_writers, stage_walk)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
