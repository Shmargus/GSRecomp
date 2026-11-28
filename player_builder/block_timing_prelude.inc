// The text block timing puts after `#include "recompiled.h"` in every game
// code file that uses its inline helpers: the one copy for both writers.
// scripts/block_timing.py reads it (developer build); player_builder/
// block_timing.cpp builds it in (player build), so both give the same bytes.
// Only the text inside the raw-string markers below is used; keep them on
// their own lines and never use them in this comment.
R"PRELUDE(
// Block timing (tools/block_timing.py): inline memory timing and conditions.
// g_runtime_mem_cost is rebuilt eagerly whenever a bus is attached, WAITCNT
// (0x04000204) is written or a savestate is restored, so it is always current.
extern "C" uint8_t g_runtime_mem_cost[16][2][2];
// always_inline: plain `static inline` was left out of line in the big
// generated functions and was 20% of the heaviest Nereid frames (one
// profiled run).
// Below the cart (BIOS, EWRAM, IWRAM, IO, PAL, VRAM, OAM) the cost does not
// depend on WAITCNT or S/N: GbaBus::access_cycles (gba_bus.cpp) gives EWRAM
// 3 / 6, PAL and VRAM 1 / 2, everything else 1, for 16- / 32-bit.
static inline __attribute__((always_inline))
uint32_t gsr_bt_mem_cycles_fn(uint32_t a, uint32_t w, uint32_t s) {
    const uint32_t region = (a >> 24) & 0xFu;
    if (region < 0x8u) {
        if (region == 0x2u) return w == 4u ? 6u : 3u;
        if (region == 0x5u || region == 0x6u) return w == 4u ? 2u : 1u;
        return 1u;
    }
    return g_runtime_mem_cost[region][w == 4u][s != 0u];
}
// Constant arguments (every generated call site) fold in the front end to a
// literal below the cart or one table load at or above it, so the compiler
// never builds and deletes the function body per instruction (~32% of an
// Android shard's compile time, measured on the tablet).
#define gsr_bt_mem_cycles(a, w, s) \
    (__builtin_constant_p(a) && __builtin_constant_p(w) && __builtin_constant_p(s) \
        ? ((((uint32_t)(a) >> 24) < 0x8u) \
            ? ((((uint32_t)(a) >> 24) == 0x2u) ? ((w) == 4u ? 6u : 3u) \
               : ((((uint32_t)(a) >> 24) == 0x5u || ((uint32_t)(a) >> 24) == 0x6u) \
                  ? ((w) == 4u ? 2u : 1u) : 1u)) \
            : (uint32_t)g_runtime_mem_cost[((uint32_t)(a) >> 24) & 0xFu][(w) == 4u][(s) != 0u]) \
        : gsr_bt_mem_cycles_fn((a), (w), (s)))
static inline __attribute__((always_inline))
uint32_t gsr_bt_insn_fetch(uint32_t pc, uint32_t w) {
    g_cpu.R[15] = pc;
    return gsr_bt_mem_cycles(pc, w, 1u);
}
// runtime_arm.cpp's arm_cond_passes, inline, on the CPSR bits directly.
// N is bit 31 and V bit 28, so (cpsr ^ (cpsr << 3)) & N is N != V.
static inline __attribute__((always_inline))
int gsr_bt_cond_passes_fn(unsigned cond) {
    const uint32_t cpsr = g_cpu.cpsr;
    switch (cond & 0xFu) {
        case 0x0: return (cpsr & CPSR_Z_BIT) != 0u;
        case 0x1: return (cpsr & CPSR_Z_BIT) == 0u;
        case 0x2: return (cpsr & CPSR_C_BIT) != 0u;
        case 0x3: return (cpsr & CPSR_C_BIT) == 0u;
        case 0x4: return (cpsr & CPSR_N_BIT) != 0u;
        case 0x5: return (cpsr & CPSR_N_BIT) == 0u;
        case 0x6: return (cpsr & CPSR_V_BIT) != 0u;
        case 0x7: return (cpsr & CPSR_V_BIT) == 0u;
        case 0x8: return (cpsr & (CPSR_C_BIT | CPSR_Z_BIT)) == CPSR_C_BIT;
        case 0x9: return (cpsr & (CPSR_C_BIT | CPSR_Z_BIT)) != CPSR_C_BIT;
        case 0xA: return ((cpsr ^ (cpsr << 3)) & CPSR_N_BIT) == 0u;
        case 0xB: return ((cpsr ^ (cpsr << 3)) & CPSR_N_BIT) != 0u;
        case 0xC: return (cpsr & CPSR_Z_BIT) == 0u && ((cpsr ^ (cpsr << 3)) & CPSR_N_BIT) == 0u;
        case 0xD: return (cpsr & CPSR_Z_BIT) != 0u || ((cpsr ^ (cpsr << 3)) & CPSR_N_BIT) != 0u;
        case 0xE: return 1;
        default:  return 0;
    }
}
// Every generated call site passes a constant condition: fold it in the
// front end to the one flag test.
#define gsr_bt_cond_passes(cond) \
    (__builtin_constant_p(cond) ? (int)( \
        ((cond) & 0xFu) == 0x0u ? ((g_cpu.cpsr & CPSR_Z_BIT) != 0u) : \
        ((cond) & 0xFu) == 0x1u ? ((g_cpu.cpsr & CPSR_Z_BIT) == 0u) : \
        ((cond) & 0xFu) == 0x2u ? ((g_cpu.cpsr & CPSR_C_BIT) != 0u) : \
        ((cond) & 0xFu) == 0x3u ? ((g_cpu.cpsr & CPSR_C_BIT) == 0u) : \
        ((cond) & 0xFu) == 0x4u ? ((g_cpu.cpsr & CPSR_N_BIT) != 0u) : \
        ((cond) & 0xFu) == 0x5u ? ((g_cpu.cpsr & CPSR_N_BIT) == 0u) : \
        ((cond) & 0xFu) == 0x6u ? ((g_cpu.cpsr & CPSR_V_BIT) != 0u) : \
        ((cond) & 0xFu) == 0x7u ? ((g_cpu.cpsr & CPSR_V_BIT) == 0u) : \
        ((cond) & 0xFu) == 0x8u ? ((g_cpu.cpsr & (CPSR_C_BIT | CPSR_Z_BIT)) == CPSR_C_BIT) : \
        ((cond) & 0xFu) == 0x9u ? ((g_cpu.cpsr & (CPSR_C_BIT | CPSR_Z_BIT)) != CPSR_C_BIT) : \
        ((cond) & 0xFu) == 0xAu ? (((g_cpu.cpsr ^ (g_cpu.cpsr << 3)) & CPSR_N_BIT) == 0u) : \
        ((cond) & 0xFu) == 0xBu ? (((g_cpu.cpsr ^ (g_cpu.cpsr << 3)) & CPSR_N_BIT) != 0u) : \
        ((cond) & 0xFu) == 0xCu ? ((g_cpu.cpsr & CPSR_Z_BIT) == 0u && (((g_cpu.cpsr ^ (g_cpu.cpsr << 3)) & CPSR_N_BIT) == 0u)) : \
        ((cond) & 0xFu) == 0xDu ? ((g_cpu.cpsr & CPSR_Z_BIT) != 0u || (((g_cpu.cpsr ^ (g_cpu.cpsr << 3)) & CPSR_N_BIT) != 0u)) : \
        ((cond) & 0xFu) == 0xEu ? 1 : 0 \
    ) : gsr_bt_cond_passes_fn(cond))
)PRELUDE"
