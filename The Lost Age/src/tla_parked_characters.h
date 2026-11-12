// The Lost Age's characters for the renderer's off-room rule
// (shared_game/parked_characters.h). Field state [0x03000020], actor context
// [0x0300006C]; none in a battle ([0x03000024] set).
//
// Actor ids 0..0x4F: 0x080CAD84 returns [[0x0300006C] + 0x14 + 4 * id] for
// id <= 0xBF; the nearest-actor searches walk 0..0x50 through it
// (0x080CDC10..34, 0x080CD942..0x080CDA38), and id 0x50 (S + 0x154) holds a
// small counter in every capture. Interiors stage NPCs off the room (F12
// 0079 Izumo, 0083, 0041-0043, 0045, 0077, 0165, 0178); the Psynergy beam's
// sparks rise past the room's top (0019) and must stay (FACTS.md
// 2026-11-12).
#pragma once

#include <bitset>
#include <cstddef>
#include <cstdint>

#include "../../shared_game/parked_characters.h"

inline std::bitset<gsr::FieldScene::kObjects> tla_character_objects(
        const gsr::FieldScene& scene, const std::uint8_t* ewram,
        std::size_t ewram_bytes, const std::uint8_t* iwram,
        std::size_t iwram_bytes) {
    constexpr int kActorCount = 0x50;
    if (!iwram || iwram_bytes < 0x28u) return {};
    const std::uint8_t* battle = iwram + 0x24u;
    if (battle[0] | battle[1] | battle[2] | battle[3]) return {};
    return gsr::character_objects(scene, ewram, ewram_bytes, iwram,
                                  iwram_bytes, 0x03000020u, 0x0300006Cu,
                                  kActorCount);
}
