// The Broken Seal's native world-map routes. Evidence: FACTS.md "Retreat visited-town list";
// these are entry aliases, NOT actor IDs.
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
// Leftover test rooms, in every supported ROM (identical room-table rows).
// Names are the game's own location names. FACTS.md "Debug rooms in Retreat".
inline constexpr DebugRoom debug_rooms[] = {
    {"Test 0", 191}, {"Test 1", 192}, {"Test 2", 193}, {"Test 3", 194},
    {"Test 4", 195}, {"Test 5", 196}, {"Test 6", 197}, {"Test 7", 198},
    {"Increase item and level", 199}, {"Menu test", 200},
};
// The party leader is field actor 0 on the world map (measured, FACTS.md
// "Debug rooms in Retreat"): field state [0x03001EBC] + 0x14.
inline std::uint32_t leader(gba::GbaBus* b) {
    return b->read32(b->read32(game->state_slot) + 0x14u);
}
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
    if (d.debug) return true;
    std::uint16_t room = 0, entrance = 0;
    if (!resolve(d.route, room, entrance) || room != d.room || entrance != d.entrance)
        return false;
    return true;
}
// The overworld adds a terrain layer to the leader's sprite. Retreat's
// dungeon animation drives it too, turning the woodland mask solid black
// (measured in a capture). Native town loading replaces the actor after this effect.
inline void remove_departure_terrain(gba::GbaBus* b) {
    const auto actor = leader(b);
    if (!field_address(actor) || (b->read8(actor + 0x54u) & 0xFu) != 1u) return;
    const auto sprite = b->read32(actor + 0x50u);
    if (!field_address(sprite)) return;
    // The native layer remover searches these four slots and frees the layer.
    for (unsigned slot = 0; slot < 4; ++slot) {
        const auto part = b->read32(sprite + 0x28u + slot * 4u);
        if (!field_address(part) || b->read16(part) != 0x17u) continue;
        if (call(GS_ROM(0x0800B93Cu), sprite, part))
            b->write8(sprite + 0x25u, 1u);  // rebuild the sprite's tiles
        return;
    }
}
inline bool on_entry(std::uint32_t pc) {
    auto* b = gbarecomp::active_bus();
    Hold h{};
    if (b && game && !busy && pc == game->cast->effect &&
        read_hold_at(b, h, g_cpu.R[13] + game->cast->effect_stack) &&
        h[5] != 2u && phase(h) == 1u)
        remove_departure_terrain(b);
    if (cast_entry(pc)) return true;
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
    // Snapshot 0x0808BA38 through the field Save's veneer 0x0808A5B0.
    static const Spot spot = {GS_ROM(0x0808A5B0u), 0x02001104u, 0x02001F84u,
        0x0200041Cu, 0u, leader};
    static const Game profile = {GS_ROM(0x0808E680u), GS_ROM(0x0808CC43u),
        GS_ROM(0x080030F8u), GS_ROM(0x080F9010u), 0x02000408u,
        0x02000400u, gs::ram(0x03001EBCu), 0x02000500u, 0x170u, 6u,
        enabled, list, arrive, font, GS_ROM(0x0808E680u), 6u, &cast, valid_route,
        debug_rooms, 0x02000480u, &spot};
    retreat_travel::install(profile);
}
} // namespace gsr::retreat_travel::tbs
