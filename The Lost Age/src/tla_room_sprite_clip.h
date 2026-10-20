#pragma once

#include <cstddef>
#include <cstdint>

#include "field_scene.h"

// Verified interiors: F12 0079 (Izumo, map 170) and 0083 (map 142) expose
// actors from outside the active room against black. Share one clipping rule
// and extend this list only with captured evidence. Other maps have legitimate
// airborne actors/effects beyond camera bounds (0019, map 82).
// Keep every sprite touching either the active room or the native screen.
inline void tla_hide_outside_room(gsr::FieldScene& scene,
        const std::uint8_t* ewram, std::size_t ewram_bytes,
        const std::uint8_t* iwram, std::size_t iwram_bytes) {
    if (scene.video_mode != 0 || !ewram || !iwram ||
        ewram_bytes < 0x42Au || iwram_bytes < 0x28u)
        return;
    const unsigned map = unsigned(ewram[0x428]) | (unsigned(ewram[0x429]) << 8);
    if (map != 142u && map != 170u) return;
    const auto rd32 = [](const std::uint8_t* p) {
        return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
               (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
    };
    if (rd32(iwram + 0x24u) != 0u) return;  // battle
    const std::uint32_t pointer = rd32(iwram + 0x20u);
    const std::size_t field = pointer & 0x00FFFFFFu;
    if ((pointer >> 24) != 2u || field > ewram_bytes ||
        ewram_bytes - field < 0xFCu)
        return;
    const auto word = [&](std::size_t offset) {
        return std::int64_t(static_cast<std::int32_t>(rd32(ewram + field + offset)));
    };
    const auto camera_x = word(0xE4u), camera_y = word(0xE8u);
    const auto left = word(0xECu), top = word(0xF0u);
    const auto right = word(0xF4u), bottom = word(0xF8u);
    if (left >= right || top >= bottom) return;
    for (auto& object : scene.objects) {
        if (!object.present || object.parked) continue;
        const int width = object.width * (object.double_size ? 2 : 1);
        const int height = object.height * (object.double_size ? 2 : 1);
        if (object.x < 240 && object.y < 160 &&
            object.x + width > 0 && object.y + height > 0)
            continue;
        const auto x = std::int64_t(object.x) * 65536 + camera_x;
        const auto y = std::int64_t(object.y) * 65536 + camera_y;
        if (x < right && y < bottom &&
            x + std::int64_t(width) * 65536 > left &&
            y + std::int64_t(height) * 65536 > top)
            continue;
        object.present = false;
    }
}
