// Random Djinn, The Broken Seal's places (rules: shared_game/random_djinn.h,
// one copy with The Lost Age).
//
// Measured, verified ROM (FACTS.md "Random Djinn", 21 November 2026):
// - join 0x080955B0(r0, element, index): with flag 0x16E (the Djinn lesson)
//   set it only sets the Djinni's flag; otherwise it calls the grant through
//   the veneer 0x08077288 at 0x080955E6 (return 0x080955EA), plays the
//   element's scene and shows the message 0x08015358(recipient, element,
//   index). It is the grant's only caller in the ROM.
// - grant 0x0807A0F4(element, index): flag 0x30 + element*20 + index tested
//   with 0x08079338 at 0x0807A11C (return 0x0807A120), set with 0x08079358
//   at 0x0807A194 (return 0x0807A198).
// - Flags from byte 0x02000040; party records 0x02000500 + id*0x14C, owned
//   bits +0xF8 (FACTS "Djinn Tracker").
// - The look (FACTS "Random Djinn", 22 November): join's caller 0x08095778
//   finds the map Djinni among actors 8..0x41 (0x0808D394) by halfword +2 ==
//   0x30 + element*20 + index. Actor context [0x03001EBC] (+0x14 + 4*id),
//   sprite at actor +0x50, primary layer +0x28; Djinni sprites 90..93,
//   records 0x08185024 + id*20 (layer reload 0x0800B868, animation start
//   0x0800B9F8). Fights: 0x08091F14 stores 0x12C + n as the pending Djinni
//   and calls 0x0808B074(n) at 0x08091F48 (return 0x08091F4C); (set, index)
//   at 0x0809D8B0 + 4n, sets 100..103 by element, one Djinni enemy each.
// The shuffled set: the 28 Djinn but Flint, the lesson's Djinni.
#pragma once
#include "../../shared_game/random_djinn.h"
#include "random_psynergy_tbs.h"
#include "rom_version.h"

namespace gsr::random_djinn::tbs {

inline const Game& game() {
    static const std::vector<Djinni> djinn = [] {
        std::vector<Djinni> out;
        for (std::uint8_t e = 0; e < 4; ++e)
            for (std::uint8_t i = 0; i < 7; ++i)
                if (e != 0 || i != 0) out.push_back({e, i});
        return out;
    }();
    static const Game g{
        &random_psynergy::tbs::game(),
        GS_ROM(0x080955B0u), GS_ROM(0x080955EAu), GS_ROM(0x0807A0F4u),
        GS_ROM(0x08079338u), GS_ROM(0x0807A120u), GS_ROM(0x08079358u), GS_ROM(0x0807A198u),
        0x02000500u, 0x14Cu, 4,
        djinn.data(), static_cast<int>(djinn.size()),
        gs::ram(0x03001EBCu), 8, 0x41, GS_ROM(0x08185024u), {90, 91, 92, 93},
        GS_ROM(0x0808B074u), GS_ROM(0x08091F4Cu),
    };
    return g;
}

inline void on_entry(std::uint32_t pc, std::uint32_t* r) { random_djinn::on_entry(game(), pc, r); }
inline void frame() { random_djinn::frame(game()); }

}  // namespace gsr::random_djinn::tbs
