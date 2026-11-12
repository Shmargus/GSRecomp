// Which sprites are a character's, for hiding characters the game parks off
// the map (FieldSceneRenderer::set_character_objects, the renderer's
// off-room rule). Both games stage cutscene actors and interior NPCs outside
// the room, where the wider view showed them floating in the black; effects
// there (Psynergy sparks rising past the room's top, TLA F12 0019) must stay.
//
// A sprite is a character's when its box covers an actor's body point or
// ground point (its shadow). Same layout in both games (FACTS.md 2026-11-12):
//   field state  [field_slot]: camera x/y at +0xE4/+0xE8 (16.16)
//   actors       [[actor_slot] + 0x14 + 4 * id], id < actor_count; null is
//                unused; x +0x08, height +0x0C, y +0x10 (16.16)
// Every body hidden over the saved captures also matched its actor's sprite
// record (+0x50, attr0/attr1 at +4/+6); every other one was its shadow.
#pragma once

#include <bitset>
#include <cstddef>
#include <cstdint>

#include "field_scene.h"

namespace gsr {

inline std::bitset<FieldScene::kObjects> character_objects(
        const FieldScene& scene, const std::uint8_t* ewram,
        std::size_t ewram_bytes, const std::uint8_t* iwram,
        std::size_t iwram_bytes, std::uint32_t field_slot,
        std::uint32_t actor_slot, int actor_count) {
    std::bitset<FieldScene::kObjects> out;
    const auto rd32 = [](const std::uint8_t* p) {
        return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
               (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
    };
    const auto slot = [&](std::uint32_t address) -> std::uint32_t {
        const std::size_t offset = address & 0x00FFFFFFu;
        if ((address >> 24) != 3u || !iwram || offset + 4u > iwram_bytes)
            return 0;
        return rd32(iwram + offset);
    };
    // An EWRAM block of `bytes` at `pointer`, or nullptr.
    const auto at = [&](std::uint32_t pointer,
                        std::size_t bytes) -> const std::uint8_t* {
        const std::size_t offset = pointer & 0x00FFFFFFu;
        if ((pointer >> 24) != 2u || !ewram || offset > ewram_bytes ||
            ewram_bytes - offset < bytes)
            return nullptr;
        return ewram + offset;
    };
    const std::uint8_t* field = at(slot(field_slot), 0xECu);
    const std::uint8_t* actors =
        at(slot(actor_slot), 0x14u + 4u * static_cast<std::size_t>(actor_count));
    if (!field || !actors) return out;
    const auto word = [&](const std::uint8_t* base, std::size_t offset) {
        return std::int64_t(static_cast<std::int32_t>(rd32(base + offset)));
    };
    const auto camera_x = word(field, 0xE4u), camera_y = word(field, 0xE8u);

    for (int id = 0; id < actor_count; ++id) {
        const std::uint8_t* actor =
            at(rd32(actors + 0x14u + 4u * static_cast<std::size_t>(id)), 0x14u);
        if (!actor) continue;
        const auto x = word(actor, 0x08u) - camera_x;
        const auto ground = word(actor, 0x10u) - camera_y;
        const auto body = ground - word(actor, 0x0Cu);
        for (int i = 0; i < FieldScene::kObjects; ++i) {
            const SceneObject& o = scene.objects[static_cast<std::size_t>(i)];
            if (!o.present || o.window) continue;
            const int scale = o.affine && o.double_size ? 2 : 1;
            const auto x0 = std::int64_t(o.x) * 65536;
            const auto y0 = std::int64_t(o.y) * 65536;
            const auto x1 = x0 + std::int64_t(o.width * scale) * 65536;
            const auto y1 = y0 + std::int64_t(o.height * scale) * 65536;
            if (x < x0 || x >= x1) continue;
            if ((ground >= y0 && ground < y1) || (body >= y0 && body < y1))
                out.set(static_cast<std::size_t>(i));
        }
    }
    return out;
}

}  // namespace gsr
