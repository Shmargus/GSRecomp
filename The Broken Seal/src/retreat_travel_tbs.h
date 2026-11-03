// TBS native world-map routes. Evidence: FACTS "Retreat visited-town list"
// and local/retreat-travel-20261102; these are entry aliases, NOT actor IDs.
#pragma once
#include "../../shared_game/retreat_travel.h"
#include "rom_version.h"

namespace gsr::retreat_travel::tbs {
struct Town { const char* name; unsigned area, route; };
inline constexpr Town towns[] = {
    {"Vale", 3, 1}, {"Vault", 5, 3}, {"Bilibin", 8, 5},
    {"Kolima", 11, 7}, {"Imil", 17, 9}, {"Xian", 23, 14},
    {"Altin", 25, 16}, {"Kalay", 32, 20}, {"Lunpa", 34, 34},
    {"Tolbi", 40, 23}, {"Suhalla", 51, 27}, {"Lalivero", 55, 30},
};
inline bool valid_route(unsigned route) {
    return std::any_of(std::begin(towns), std::end(towns),
        [=](const auto& town) { return town.route == route; });
}
inline bool resolve(unsigned route, std::uint16_t& room, std::uint16_t& entrance) {
    auto* b = gbarecomp::active_bus();
    const auto before = b->read32(0x02000400u);
    const bool ok = call(GS_ROM(0x0808A5F8u), route, 0);
    room = b->read16(0x02000400u);
    entrance = b->read16(0x02000402u);
    b->write32(0x02000400u, before);
    return ok && room != 2u;
}
inline std::vector<Destination> list() {
    auto* b = gbarecomp::active_bus();
    std::vector<Destination> result;
    const auto rooms = GS_ROM(0x0809F1A8u);
    for (const auto& town : towns) {
        if (!gsr::read_flag(b->ewram_ptr(), 0x180u + town.area)) continue;
        std::uint16_t room = 0, entrance = 0;
        if (!resolve(town.route, room, entrance) ||
            rooms + room * 8u + 2u - 0x08000000u >= b->rom_size() ||
            b->read8(rooms + room * 8u + 2u) != town.area) continue;
        result.push_back({town.name, 0x180u + town.area, town.route, room, entrance});
    }
    return result;
}
inline bool arrive(const Destination& d) {
    std::uint16_t room = 0, entrance = 0;
    if (!resolve(d.route, room, entrance) || room != d.room || entrance != d.entrance)
        return false;
    return true;
}
inline bool on_entry(std::uint32_t pc) {
    if (cast_entry(pc)) return true;
    auto* b = gbarecomp::active_bus();
    Hold h{};
    if (b && game && pc == game->frame && read_hold(b, h) &&
        h[5] != 2u && phase(h) == 3u) {
        std::uint16_t room = 0, entrance = 0;
        if (resolve(route(h), room, entrance)) {
            b->write16(game->room, room);
            b->write16(game->room + 2u, entrance);
            b->write16(h[4] + game->exit_offset, 999u);
        }
    }
    return retreat_travel::on_entry(pc);
}
// The runner supplies its existing option getter/font preparation.
inline void install(bool (*enabled)(), void (*font)()) {
    static const Cast cast = {GS_ROM(0x080770C0u), GS_ROM(0x08015120u),
        GS_ROM(0x08015040u), GS_ROM(0x08015140u), GS_ROM(0x08091D84u),
        GS_ROM(0x08077080u), GS_ROM(0x0808E7ADu), GS_ROM(0x0809B208u),
        0x2Cu, 0x40u};
    static const Game profile = {GS_ROM(0x0808E680u), GS_ROM(0x0808CC43u),
        GS_ROM(0x080030F8u), GS_ROM(0x080F9010u), 0x02000408u,
        0x02000400u, gs::ram(0x03001EBCu), 0x02000500u, 0x170u, 6u,
        enabled, list, arrive, font, GS_ROM(0x0808E680u), 6u, &cast, valid_route};
    retreat_travel::install(profile);
}
} // namespace gsr::retreat_travel::tbs
