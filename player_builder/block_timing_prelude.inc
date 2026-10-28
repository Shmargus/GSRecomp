// The text block timing puts after `#include "recompiled.h"` in every game
// code file that uses its inline helpers: the one copy for both writers.
// scripts/block_timing.py reads it (developer build); player_builder/
// block_timing.cpp builds it in (player build), so both give the same bytes.
// Only the text inside the raw-string markers below is used; keep them on
// their own lines and never use them in this comment.
R"PRELUDE(
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
)PRELUDE"
