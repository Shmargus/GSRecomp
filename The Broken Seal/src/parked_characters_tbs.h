// Golden Sun's characters for the renderer's off-room rule
// (shared_game/parked_characters.h). Field state [0x03001E70], actor context
// [0x03001EBC] (German 0x03001E80 / 0x03001ECC: rom_addresses.inc); the
// caller passes the slots for its ROM.
//
// Actor ids 0..0x42: 0x0808BA1C returns [[0x03001EBC] + 0x14 + 4 * id] for
// id <= 0xBF; the loops that walk it stop at 0x41 (0x08095748, 0x0809C384)
// or 0x42 (0x08096D16). Higher entries hold other fields (S + 0x1E0 is the
// focus actor). Over every 10th saved field frame, each sprite hidden off
// the room covered an actor point (0056, 0072, 0092 the Wise One); the
// effects there (0050, 0053, 0080, 0081, 0091, 0098-0100) covered none
// (FACTS.md 2026-11-12).
#pragma once

#include <bitset>
#include <cstddef>
#include <cstdint>

#include "../../shared_game/parked_characters.h"

inline std::bitset<gsr::FieldScene::kObjects> golden_sun_character_objects(
        const gsr::FieldScene& scene, const std::uint8_t* ewram,
        std::size_t ewram_bytes, const std::uint8_t* iwram,
        std::size_t iwram_bytes, std::uint32_t field_slot,
        std::uint32_t actor_slot) {
    constexpr int kActorCount = 0x43;
    return gsr::character_objects(scene, ewram, ewram_bytes, iwram,
                                  iwram_bytes, field_slot, actor_slot,
                                  kActorCount);
}
