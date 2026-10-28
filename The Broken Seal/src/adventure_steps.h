// Location-aware TBS USA/EU instructions; no guest writes or saved checkboxes.
// Evidence: FACTS.md "Guide mode"; root local/guide-mode and existing recordings.
#pragma once
#include "adventure_rules.h"
#include "adventure_steps_data.h"
namespace gsr::adventure {
// Original character getter08077394:02000500 + character*014C.
// Inventory lookup08078664:15 halfwords at+D8, item word &01FF.
inline bool guide_has_item(const unsigned char* ram, unsigned item) {
    for (unsigned hero = 0; hero < 8; ++hero)
        for (unsigned slot = 0; slot < 15; ++slot) {
            const unsigned p = 0x500 + hero * 0x14c + 0xd8 + slot * 2;
            const unsigned word = ram[p] | (unsigned(ram[p + 1]) << 8);
            if ((word & 0x1ff) == item) return true;
        }
    return false;
}
inline const GuideStep* guide_for(const unsigned char* ram) {
    if (!ram) return nullptr;
    using namespace guide_detail;
    const unsigned room = ram[0x400] | (unsigned(ram[0x401]) << 8);
    const unsigned map = ram[0x408] | (unsigned(ram[0x409]) << 8);
    if (room == 510) return &battle;
    // Room table0809F1A8 + room*8 feeds loader0808AB48; asset table08320000.
    // Original overlays verified for rooms11..19, including shared16/19 code.
    if (room >= 11 && room <= 19 && guide_has_item(ram, 0xde))
        return room == 17 ? &leave_stars : &escape;
    if (room == 19) return &entrance;
    if (room == 14 || room == 15) {
        if (read_flag(ram, 0x821)) return &deeper;
        if (!read_flag(ram, 0x81a))
            return read_flag(ram, 0xf01) || guide_has_item(ram, 0xe1) ? &first_eye : &water_first;
        return read_flag(ram, 0xf02) || guide_has_item(ram, 0xe1) ? &second_eye : &water_second;
    }
    if (room == 16) {
        if (!read_flag(ram, 0x81a))
            return read_flag(ram, 0xf01) || guide_has_item(ram, 0xe1) ? &first_eye : &first_jewel;
        if (!read_flag(ram, 0x804)) return &move_wall;
        if (!read_flag(ram, 0x821))
            return read_flag(ram, 0xf02) || guide_has_item(ram, 0xe1) ? &second_eye : &second_jewel;
        return &deeper;
    }
    if (room == 11 || room == 13) {
        if (read_flag(ram, 0x80f)) return &portal;
        if (!read_flag(ram, 0x809)) return &inspect;
        return read_flag(ram, 0x818) ? &moon_statues : &upstairs;
    }
    if (room == 12) {
        if (read_flag(ram, 0x80f)) return &portal;
        if (!read_flag(ram, 0x818)) {
            if (!read_flag(ram, 0x816)) return &left_outer;
            if (!read_flag(ram, 0x817)) return &right_outer;
            return &center;
        }
        return &moon_statues;
    }
    if (room == 17) {
        if (read_flag(ram, 0x83e)) return &mars;
        if (!guide_has_item(ram, 0xe0)) return &bags;
        if (!read_flag(ram, 0x83c) && !guide_has_item(ram, 0xdc)) return &venus;
        if (!read_flag(ram, 0x83d) && !guide_has_item(ram, 0xdd)) return &mercury;
        if (!guide_has_item(ram, 0xdf)) return &jupiter;
        return &mars;
    }
    if (room == 18) return &escape;
    if (!prologue_complete(ram)) return nullptr;
    if (map == 2 && !read_flag(ram, 0x30)) return &flint;
    return nullptr;
}
} // namespace gsr::adventure
