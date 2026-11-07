// The Broken Seal's radial Psynergy menu places (shared_game/radial_psynergy.h).
// Evidence: FACTS "Radial Psynergy menu" (4 November 2026).
#pragma once
#include "../../shared_game/radial_psynergy.h"
#include "rom_version.h"

namespace gsr::radial_psynergy::tbs {
// Field button poll 0x0800EAF8 (returns 0 while [0x03001EBC] is 0): masks
// 0x02000450.. (L 0x200 at 0x02000458, R 0x100 at 0x0200045A) pick the shortcut words 0x02000460 / 0x02000462 for
// the cast routine 0x0800EA60, which queues field + 0x17E as the Psynergy
// menu does, for the dispatcher 0x0808E680. Ability records 0x0807EE58 (16 bytes, ids below 0x208, field
// kind +0xC as dispatcher 0x0808E6A8 reads it); icons 0x0802DE88 (160) in
// the common OBJ colours 0x0800779C; names text 0x333 + id.
// Auto Field Psynergy (FACTS "Auto Field Psynergy", 5 November 2026). The
// field loop's A (0x0808CAC4..0x0808CB42): A mask 0x02000450 against pressed
// [0x03001C94]; talk when 0x0808DDEC([0x02000434]) != -1 and 0x0808D5A4 of it
// is nonzero; else check when 0x0808CE74() is nonzero. The dispatcher, after
// PP, searches the room's events ([field] + 0x10, 12 bytes, type 5 = Psynergy)
// with 0x0808E4B4(category, kind, &out) for 0x10000005, 5, 0x50000005
// (0x0808E7F4..0x0808E80E); event bit 0x80 matches anywhere in the room.
inline bool a_acts() {
    const std::uint32_t target = guest(GS_ROM(0x0808DDECu), gbarecomp::active_bus()->read32(0x02000434u));
    if (target != 0xFFFFFFFFu && guest(GS_ROM(0x0808D5A4u), target)) return true;
    return guest(GS_ROM(0x0808CE74u)) != 0;
}
// The search writes one word through its third argument: the word at the
// poll's stack pointer belongs to the poll's caller, so it is put back.
inline std::uint32_t find(std::uint32_t category, unsigned kind) {
    auto* b = gbarecomp::active_bus();
    const std::uint32_t slot = g_cpu.R[13];
    const std::uint32_t saved = b->read32(slot);
    const std::uint32_t event = guest(GS_ROM(0x0808E4B4u), category, kind, slot);
    b->write32(slot, saved);
    return event;
}
inline const AutoCast* auto_cast() {
    static const AutoCast places = {
        0x02000450u, gs::ram(0x03001C94u), 0x10u, a_acts, find,
        {0x10000005u, 0x5u, 0x50000005u}, 3u, 0x80u,
    };
    return &places;
}

inline void install(void (*font)()) {
    static const Game profile = {
        GS_ROM(0x0800EAF8u), gs::ram(0x03001EBCu), 0u,
        GS_ROM(0x0800EA60u), GS_ROM(0x0808E680u), GS_ROM(0x080F9010u),
        GS_ROM(0x0807EE58u), 16u, 0x208u, 0xCu, false,
        GS_ROM(0x0802DE88u), 160u, GS_ROM(0x0800779Cu),
        0x02000500u, 0x02000040u, 0x02000438u, 0x02000460u, 0x02000462u,
        gs::text(0x333u),
        GS_ROM(0x080736B8u) - mods::kRomBase, GS_ROM(0x0803842Cu) - mods::kRomBase,
        font, auto_cast(),
    };
    radial_psynergy::install(profile);
}
}  // namespace gsr::radial_psynergy::tbs
