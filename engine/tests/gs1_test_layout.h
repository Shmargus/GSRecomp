// Test fixture: the Broken Seal RAM layout, for engine tests that exercise the
// layout-driven probes (RAM image window, OAM shadow, pool-LDM probe, unpacker
// slot). The engine itself holds no game addresses; the real runner sets these.
#pragma once
#include "game_ram_layout.h"

inline void set_gs1_test_layout() {
    GameRamLayout l;
    l.iwram_heap_first = 0x03002000u;
    l.iwram_heap_last = 0x030077FFu;
    l.unpacker_slot = 0x03002000u;
    l.unpacker_slot_len = 0x2C4u;
    l.iwram_heap_ptr = 0x03001E54u;
    l.ram_image_start = 0x03005B0Cu;
    l.ram_image_end = 0x03005D5Cu;
    l.oam_shadow_start = 0x0300347Cu;
    l.oam_shadow_end = 0x0300387Cu;
    l.pool_ldm_pc = 0x03006100u;
    l.pool_ldm_slot0 = 0x03007E24u;
    l.pool_ldm_slot1 = 0x03007E28u;
    l.pool_ldm_poison_target = 0x03007E30u;
    l.pool_ldm_window_base = 0x03007E00u;
    l.pool_ldm_window_end = 0x03007E40u;
    l.pool_range0_lo = 0x030057E0u;
    l.pool_range0_hi = 0x03005A64u;
    l.pool_range1_lo = 0x03006000u;
    l.pool_range1_hi = 0x03006500u;
    runtime_set_game_ram_layout(l);
}
