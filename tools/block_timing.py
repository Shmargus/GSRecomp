#!/usr/bin/env python3
"""Make the block-timing copy of the game code (ROADMAP.md, "block bookkeeping").

    python tools/block_timing.py local/gs011 local/gs011_block

Copies every file of the generated game code from the first folder into the
second and rewrites the .cpp files so the per-instruction bookkeeping happens
once per block (gbarecomp/src/armv4t/runtime_arm.h, "Block timing"):

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

The player build does the same rewrite in C++ (tools/gsr_builder/
block_timing.cpp), which must give byte-for-byte the same output; change
both together (checked 2026-09-30: all 845 generated .cpp files identical).

A file is only written when its content changes, so a rebuild recompiles only
what moved. Files in the destination that no longer exist in the source are
removed. Prints what it did; exits non-zero if nothing matched.
"""
import os
import re
import sys

BEGIN = re.compile(
    r'^(\s*)uint32_t (_cyc_\w+) = runtime_insn_begin\((0x[0-9A-Fa-f]+u), (\d+u)\);$')
LABEL = re.compile(r'^\s*L_[0-9A-Fa-f]+:\s*$')
BOUNDARY = re.compile(r'^\s*if \(runtime_insn_boundary\(\)\) return;$')
FUNC = re.compile(r'^\S.*\)\s*\{\s*$')


GOTO = re.compile(r'\bgoto (L_[0-9A-Fa-f]+);')

MEM_CYCLES = 'runtime_mem_cycles('

# Put after `#include "recompiled.h"` in every file that uses it.
PRELUDE = '''
// Block timing (tools/block_timing.py): inline memory timing.
extern "C" uint8_t g_runtime_mem_cost[16][2][2];
extern "C" uint32_t g_runtime_mem_cost_key;
extern "C" const uint8_t* g_runtime_waitcnt_live;
// always_inline: plain `static inline` was left out of line in the big
// generated functions and was 20% of the heaviest Nereid frames
// (logs/session_20260930_190323.hostprof.txt).
// Below the cart (BIOS, EWRAM, IWRAM, IO, PAL, VRAM, OAM) the cost does not
// depend on WAITCNT or S/N: GbaBus::access_cycles (gba_bus.cpp) gives EWRAM
// 3 / 6, PAL and VRAM 1 / 2, everything else 1, for 16- / 32-bit. With a
// constant address that folds to a constant.
static inline __attribute__((always_inline))
uint32_t gsr_bt_mem_cycles(uint32_t a, uint32_t w, uint32_t s) {
    const uint32_t region = (a >> 24) & 0xFu;
    if (region < 0x8u) {
        if (region == 0x2u) return w == 4u ? 6u : 3u;
        if (region == 0x5u || region == 0x6u) return w == 4u ? 2u : 1u;
        return 1u;
    }
    const uint8_t* wc = g_runtime_waitcnt_live;
    if (wc && (uint32_t)(wc[0] | (wc[1] << 8)) == g_runtime_mem_cost_key)
        return g_runtime_mem_cost[region][w == 4u][s != 0u];
    return runtime_mem_cycles(a, w, s);
}
static inline __attribute__((always_inline))
uint32_t gsr_bt_insn_fetch(uint32_t pc, uint32_t w) {
    g_cpu.R[15] = pc;
    return gsr_bt_mem_cycles(pc, w, 1u);
}
// runtime_arm.cpp's arm_cond_passes, inline.
static inline __attribute__((always_inline))
int gsr_bt_cond_passes(unsigned cond) {
    const uint32_t n = cpsr_n();
    const uint32_t z = cpsr_z();
    const uint32_t c = cpsr_c();
    const uint32_t v = cpsr_v();
    switch (cond & 0xFu) {
        case 0x0: return z != 0;
        case 0x1: return z == 0;
        case 0x2: return c != 0;
        case 0x3: return c == 0;
        case 0x4: return n != 0;
        case 0x5: return n == 0;
        case 0x6: return v != 0;
        case 0x7: return v == 0;
        case 0x8: return (c != 0) && (z == 0);
        case 0x9: return (c == 0) || (z != 0);
        case 0xA: return n == v;
        case 0xB: return n != v;
        case 0xC: return (z == 0) && (n == v);
        case 0xD: return (z != 0) || (n != v);
        case 0xE: return 1;
        default:  return 0;
    }
}
'''


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
    wanted = set()
    for root, _dirs, files in os.walk(src):
        rel_root = os.path.relpath(root, src)
        for name in files:
            rel = os.path.normpath(os.path.join(rel_root, name))
            wanted.add(rel)
            s = os.path.join(src, rel)
            d = os.path.join(dst, rel)
            with open(s, 'rb') as f:
                data = f.read()
            if name.endswith('.cpp'):
                text = data.decode('utf-8')
                data = transform(text, stats).encode('utf-8')
            stats['files'] += 1
            old = None
            if os.path.exists(d):
                with open(d, 'rb') as f:
                    old = f.read()
            if old != data:
                os.makedirs(os.path.dirname(d), exist_ok=True)
                with open(d, 'wb') as f:
                    f.write(data)
                stats['written'] += 1
    for root, _dirs, files in os.walk(dst):
        for name in files:
            rel = os.path.normpath(os.path.relpath(os.path.join(root, name), dst))
            if rel not in wanted:
                os.remove(os.path.join(dst, rel))
                stats['removed'] += 1
    print('block timing: %(files)d files, %(written)d written, %(removed)d removed; '
          'instructions merged %(merged)d, kept %(kept)d; conditional checks '
          'dropped %(dropped_boundary)d, kept %(kept_boundary)d; '
          'ticks deferred %(ticks)d; entry-only labels %(entry_labels)d; '
          'memory timing inline %(mem_cycles_inline)d, kept '
          '%(mem_cycles_kept)d, in %(preludes)d files; conditions inline '
          '%(cond_inline)d'
          % stats)
    return 0 if stats['merged'] else 1


if __name__ == '__main__':
    sys.exit(main())
