// Game-supplied IWRAM/EWRAM addresses that engine diagnostics and host
// features need. The engine holds no game addresses of its own: each game's
// runner fills a GameRamLayout once at startup (before the guest runs) and
// calls runtime_set_game_ram_layout().
//
// All-zero (the default) means "feature inert": every probe, journal and
// range test below treats a zero range as empty and records nothing. Fields
// are absolute guest addresses, ranges as documented per field.
//
// Deliberately not included by runtime_arm.h: that header is pulled into every
// translated game file, so touching it recompiles all generated code.
#pragma once
#include <cstdint>

struct GameRamLayout {
    // IWRAM allocation heap the game builds routines in (inclusive range).
    uint32_t iwram_heap_first = 0, iwram_heap_last = 0;

    // Overlay unpacker slot (absolute IWRAM address and byte length) and the
    // game's IWRAM bump-allocator pointer (where the next routine lands).
    // slot_len == 0 disables the unpacker-slot journal and the IWRAM
    // code-write ring derived from it.
    uint32_t unpacker_slot = 0, unpacker_slot_len = 0, iwram_heap_ptr = 0;

    // Measured transient RAM code image [start,end).
    uint32_t ram_image_start = 0, ram_image_end = 0;

    // OAM shadow buffer [start,end).
    uint32_t oam_shadow_start = 0, oam_shadow_end = 0;

    // CRASH-03 pool-LDM probe: probed pc, two fixed slots, poison target and
    // the stack window [base,end) it tracks. The window may hold at most
    // kPoolLdmWindowMaxWords words; a larger window leaves the probe unarmed.
    uint32_t pool_ldm_pc = 0, pool_ldm_slot0 = 0, pool_ldm_slot1 = 0;
    uint32_t pool_ldm_poison_target = 0;
    uint32_t pool_ldm_window_base = 0, pool_ldm_window_end = 0;
    // The two pool dispatch ranges [lo,hi) that arm the probe.
    uint32_t pool_range0_lo = 0, pool_range0_hi = 0;
    uint32_t pool_range1_lo = 0, pool_range1_hi = 0;

    static constexpr uint32_t kPoolLdmWindowMaxWords = 16u;
};

// The active layout. A plain global so hot checks cost one load; written only
// through runtime_set_game_ram_layout() at startup.
extern GameRamLayout g_game_ram_layout;

// Install the game's layout. Call once, before the guest runs.
void runtime_set_game_ram_layout(const GameRamLayout& layout);
