#!/usr/bin/env python3
"""Make the block-timing copy of the game code (ROADMAP.md, "block bookkeeping").

    python scripts/block_timing.py local/gs011 local/gs011_block

The one copy for both games (scripts/build_game.ps1 runs it from the game's
folder).

Copies every file of the generated game code from the first folder into the
second and rewrites the .cpp files so the per-instruction bookkeeping happens
once per block (engine/src/armv4t/runtime_arm.h, "Block timing"):

  * `uint32_t _cyc_X = runtime_insn_begin(P, W);` followed by
    `if (_cyc_X == 0u) return;` becomes `uint32_t _cyc_X = runtime_insn_fetch(P, W);`
    except for the first instruction of a function and the first one after a
    `L_xxxxxxxx:` label that a `goto` inside the function jumps to (a loop
    head keeps its yield check, so a tight loop can still be interrupted;
    labels reached only from the resume switch do not).
  * `if (runtime_insn_boundary()) return;` (conditional instructions and
    branches) is dropped, with the same exceptions.
  * `runtime_tick(` becomes `runtime_tick_deferred(`.
  * `runtime_mem_cycles(A, W, 0u|1u)` becomes `gsr_bt_mem_cycles(...)` and
    `runtime_insn_fetch(` `gsr_bt_insn_fetch(`: inline reads of the engine's
    memory timing table (runtime_bus_bridge.cpp, g_runtime_mem_cost), with a
    call to runtime_mem_cycles whenever WAITCNT differs from the value the
    table was built for. The same numbers without a call into the engine;
    after the fixes above these calls were ~23% of the heaviest Ragnarok
    frames (logs/session_20260930_184503.hostprof.txt). Data-access costs
    (third argument 2u) keep calling the engine: they can take the
    prefetch path.
  * `arm_cond_passes(` becomes `gsr_bt_cond_passes(`, the same condition
    table inline (runtime_arm.cpp's is out of line, a call into the engine
    per conditional instruction: 4.2% of the heaviest Ragnarok frames in the
    same profile). Every call site passes a constant, so it folds to a flag
    test.

The player build does the same rewrite in C++ (player_builder/
block_timing.cpp), which must give byte-for-byte the same output; change
both together (checked 2026-09-30: all 845 generated .cpp files identical).
The inserted helpers come from one file both read,
player_builder/block_timing_prelude.inc.

A file is only written when its content changes, so a rebuild recompiles only
what moved. Files in the destination that no longer exist in the source are
removed. Prints what it did; exits non-zero if nothing matched. (The Lost
Age's RAM images set R15 from their base, so slim leaves their instruction
starts unmerged; their checks and ticks are still rewritten, which counts.)

A source file whose size and modification time match the last run, under
this same script, with its output still as written, is not read again: its
counts come from the cache file CACHE_NAME in the destination. Rewriting
every file took 43 s (Broken Seal) and 19 s (Lost Age) per build even with
nothing changed (measured 2026-10-14).
"""
import hashlib
import json
import os
import re
import sys

CACHE_NAME = '.block_timing_cache.json'

BEGIN = re.compile(
    r'^(\s*)uint32_t (_cyc_\w+) = runtime_insn_begin\((0x[0-9A-Fa-f]+u), (\d+u)\);$')
LABEL = re.compile(r'^\s*L_[0-9A-Fa-f]+:\s*$')
BOUNDARY = re.compile(r'^\s*if \(runtime_insn_boundary\(\)\) return;$')
FUNC = re.compile(r'^\S.*\)\s*\{\s*$')


GOTO = re.compile(r'\bgoto (L_[0-9A-Fa-f]+);')

MEM_CYCLES = 'runtime_mem_cycles('

# Put after `#include "recompiled.h"` in every file that uses it. The text is
# shared with the player build (player_builder/block_timing.cpp builds the same
# file in): the part inside its raw-string markers.
PRELUDE_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..',
                            'player_builder', 'block_timing_prelude.inc')
with open(PRELUDE_PATH, encoding='utf-8') as _f:
    PRELUDE = _f.read().split('R"PRELUDE(', 1)[1].split(')PRELUDE"', 1)[0]


def inline_mem_cycles(line, stats):
    """runtime_mem_cycles(A, W, 0u|1u) -> gsr_bt_mem_cycles(A, W, ...)."""
    out = []
    i = 0
    n = len(MEM_CYCLES)
    while True:
        j = line.find(MEM_CYCLES, i)
        if j < 0:
            out.append(line[i:])
            break
        if j > 0 and (line[j - 1].isalnum() or line[j - 1] == '_'):
            out.append(line[i:j + n])
            i = j + n
            continue
        k = j + n
        depth = 1
        start = k
        args = []
        while k < len(line) and depth:
            c = line[k]
            if c == '(':
                depth += 1
            elif c == ')':
                depth -= 1
            elif c == ',' and depth == 1:
                args.append(line[start:k])
                start = k + 1
            k += 1
        if depth:
            stats['mem_cycles_kept'] += 1
            out.append(line[i:])
            break
        args.append(line[start:k - 1])
        out.append(line[i:j])
        if len(args) == 3 and args[2].strip() in ('0u', '1u'):
            out.append('gsr_bt_mem_cycles(')
            stats['mem_cycles_inline'] += 1
        else:
            out.append(MEM_CYCLES)
            stats['mem_cycles_kept'] += 1
        i = j + n
    return ''.join(out)


def transform(text, stats):
    lines = text.split('\n')
    # Labels a jump inside a function returns to: possible loop heads, which
    # keep their check. A label reached only from the resume switch at the
    # top of a function (`case P: goto L_P;`) is an entry point, and the
    # entry has already checked in; the spell-effect stamps have one at every
    # instruction (local/gs011/stamps), which kept every check there.
    loop_labels = set()
    for line in lines:
        if 'goto L_' in line and 'case ' not in line:
            loop_labels.update(GOTO.findall(line))
    out = []
    keep_next = False  # next instruction start keeps its yield check
    i = 0
    n = len(lines)
    while i < n:
        line = lines[i]
        if FUNC.match(line):
            keep_next = True
        elif LABEL.match(line):
            if line.strip()[:-1] in loop_labels:
                keep_next = True
            else:
                stats['entry_labels'] += 1
        m = BEGIN.match(line)
        if m and i + 1 < n and \
                lines[i + 1] == '%sif (%s == 0u) return;' % (m.group(1), m.group(2)):
            if keep_next:
                keep_next = False
                stats['kept'] += 1
                out.append(line)
                out.append(lines[i + 1])
            else:
                stats['merged'] += 1
                out.append('%suint32_t %s = gsr_bt_insn_fetch(%s, %s);'
                           % (m.group(1), m.group(2), m.group(3), m.group(4)))
            i += 2
            continue
        # A conditional instruction (every conditional ARM instruction and
        # every THUMB conditional branch) checks in with
        # `g_cpu.R[15] = P; if (runtime_insn_boundary()) return;`. A loop can
        # only repeat through a label or a function entry, which keep their
        # check, so the check here is dropped too (R15 is still set). The
        # heaviest Ragnarok frames made ~1.1 M such checks against ~140 k
        # instruction starts (logs/session_20260930_165552.phase.csv).
        if BOUNDARY.match(line):
            if keep_next:
                keep_next = False
                stats['kept_boundary'] += 1
            else:
                stats['dropped_boundary'] += 1
                i += 1
                continue
        if 'runtime_insn_boundary()' in line or 'runtime_insn_begin(' in line:
            keep_next = False
        if 'runtime_tick(' in line:
            stats['ticks'] += line.count('runtime_tick(')
            line = line.replace('runtime_tick(', 'runtime_tick_deferred(')
        if MEM_CYCLES in line:
            line = inline_mem_cycles(line, stats)
        if 'arm_cond_passes(' in line:
            stats['cond_inline'] += line.count('arm_cond_passes(')
            line = line.replace('arm_cond_passes(', 'gsr_bt_cond_passes(')
        out.append(line)
        i += 1
    text = '\n'.join(out)
    if 'gsr_bt_' in text:
        anchor = '#include "recompiled.h"\n'
        at = text.find(anchor)
        if at < 0:
            raise SystemExit('block_timing: no #include "recompiled.h" to '
                             'put the inline timing after')
        at += len(anchor)
        text = text[:at] + PRELUDE + text[at:]
        stats['preludes'] += 1
    return text


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    src, dst = sys.argv[1], sys.argv[2]
    if os.path.abspath(src) == os.path.abspath(dst):
        print('source and destination must differ')
        return 2
    stats = {'kept': 0, 'merged': 0, 'ticks': 0, 'written': 0, 'files': 0,
             'removed': 0, 'kept_boundary': 0, 'dropped_boundary': 0,
             'entry_labels': 0, 'mem_cycles_inline': 0,
             'mem_cycles_kept': 0, 'preludes': 0, 'cond_inline': 0}
    # The cache is only good for this script and this prelude.
    digest = hashlib.sha256()
    for path in (os.path.abspath(__file__), PRELUDE_PATH):
        with open(path, 'rb') as f:
            digest.update(f.read())
    script = digest.hexdigest()
    cache_path = os.path.join(dst, CACHE_NAME)
    cache = {}
    try:
        with open(cache_path, encoding='utf-8') as f:
            saved = json.load(f)
        if saved.get('script') == script:
            cache = saved.get('files', {})
    except (OSError, ValueError):
        pass
    fresh = {}

    def stamp(path):
        st = os.stat(path)
        return [st.st_size, st.st_mtime_ns]

    wanted = set()
    for root, _dirs, files in os.walk(src):
        rel_root = os.path.relpath(root, src)
        for name in files:
            rel = os.path.normpath(os.path.join(rel_root, name))
            wanted.add(rel)
            s = os.path.join(src, rel)
            d = os.path.join(dst, rel)
            stats['files'] += 1
            key = rel.replace(os.sep, '/')
            source = stamp(s)
            hit = cache.get(key)
            if hit and hit['src'] == source and os.path.exists(d) and \
                    stamp(d) == hit['dst']:
                for k, v in hit['counts'].items():
                    stats[k] += v
                fresh[key] = hit
                continue
            with open(s, 'rb') as f:
                data = f.read()
            counts = dict.fromkeys(stats, 0)
            if name.endswith('.cpp'):
                text = data.decode('utf-8')
                data = transform(text, counts).encode('utf-8')
            old = None
            if os.path.exists(d):
                with open(d, 'rb') as f:
                    old = f.read()
            if old != data:
                os.makedirs(os.path.dirname(d), exist_ok=True)
                with open(d, 'wb') as f:
                    f.write(data)
                stats['written'] += 1
            counts = {k: v for k, v in counts.items() if v}
            for k, v in counts.items():
                stats[k] += v
            fresh[key] = {'src': source, 'dst': stamp(d), 'counts': counts}
    for root, _dirs, files in os.walk(dst):
        for name in files:
            rel = os.path.normpath(os.path.relpath(os.path.join(root, name), dst))
            if rel == CACHE_NAME:
                continue
            if rel not in wanted:
                os.remove(os.path.join(dst, rel))
                stats['removed'] += 1
    if os.path.isdir(dst):
        with open(cache_path + '.tmp', 'w', encoding='utf-8') as f:
            json.dump({'script': script, 'files': fresh}, f)
        os.replace(cache_path + '.tmp', cache_path)
    print('block timing: %(files)d files, %(written)d written, %(removed)d removed; '
          'instructions merged %(merged)d, kept %(kept)d; conditional checks '
          'dropped %(dropped_boundary)d, kept %(kept_boundary)d; '
          'ticks deferred %(ticks)d; entry-only labels %(entry_labels)d; '
          'memory timing inline %(mem_cycles_inline)d, kept '
          '%(mem_cycles_kept)d, in %(preludes)d files; conditions inline '
          '%(cond_inline)d'
          % stats)
    return 0 if stats['merged'] or stats['ticks'] else 1


if __name__ == '__main__':
    sys.exit(main())
