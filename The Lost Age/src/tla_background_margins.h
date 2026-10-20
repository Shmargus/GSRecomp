#pragma once

#include <cstddef>
#include <cstdint>

#include "field_scene.h"

// Map165's draining water and Gaia Rock's distant scenery (below) need
// game-measured inputs to the shared margin renderer.
// Gaia Rock's distant scenery repeats every eight 16-pixel metatiles
// (0088: three identical copies at grid columns 0..23, rows 89..94).
// Its quarter-speed scroll exposes both ends in the expanded margins.
// Maps 175/176 (0088..0090) use two origins into the same background grid.
// Repeat that artwork there, keeping its scale and the original screen.
inline bool tla_background_margins(const gsr::FieldScene& scene,
                                   gsr::SceneMargins& margins,
                                   const std::uint8_t* ewram,
                                   std::size_t bytes) {
    margins.layers[2].repeat_width = 0;
    margins.layers[2].rest_clip_y = 0;
    if (!ewram || bytes < 0x40000u || scene.video_mode != 0 ||
        !scene.layers[2].enabled || scene.layers[2].affine)
        return false;
    auto word = [&](std::size_t at) {
        return std::uint32_t(ewram[at]) | (std::uint32_t(ewram[at + 1]) << 8) |
               (std::uint32_t(ewram[at + 2]) << 16) |
               (std::uint32_t(ewram[at + 3]) << 24);
    };
    const auto map = word(0x428) & 0xFFFFu;
    if (map == 165u) {
        // F12 0107: BG2's grid origin is (1024,0), independently named by
        // its grid pointer. Its region sinks to y=-32 while the stone and
        // camera stay still. Moving the water also moves its transparent
        // gaps off the pillar/walkway. Keep only pixels present in the
        // stationary water silhouette, using the existing rest-clip rule.
        if (word(0x30E98) != 0x04000000u ||
            word(0x30EC0) != 0x02010100u ||
            std::int16_t(word(0x30E9C) >> 16) >= 0)
            return false;
        const auto reg_y = [&](int row) {
            const auto& io = scene.row_io_valid[row]
                ? scene.row_io[row] : scene.io_bytes;
            return unsigned(io[0x1A]) | (unsigned(io[0x1B]) << 8);
        };
        const auto y = reg_y(0);
        for (int row = 1; row < gsr::FieldScene::kRows; ++row)
            if (reg_y(row) != y) return false;
        // Use the displayed scroll, not the newer live layer record:
        // 0107/f000 has VOFS551 but the record already says550.
        const int camera_y = std::int16_t(word(0x30E38) >> 16);
        const int dy = camera_y - std::int16_t(y);
        if (dy <= 0) return false;
        margins.layers[2].rest_clip_y = dy;
        return true;
    }
    const bool known_origin =
        (word(0x30E98) == 0u && word(0x30E9C) == 0x05200000u) ||
        (word(0x30E98) == 0xFF800000u && word(0x30E9C) == 0x04480000u);
    if ((map != 175u && map != 176u) ||
        word(0x30EA0) != 0x4000u || word(0x30EA4) != 0x4000u ||
        !known_origin)
        return false;
    constexpr int kColumns = 128, kPeriod = 8;
    for (int y = 89; y <= 94; ++y)
        for (int x = kPeriod; x < 3 * kPeriod; ++x)
            if (word(0x10000u + (y * kColumns + x) * 4u) !=
                word(0x10000u + (y * kColumns + x % kPeriod) * 4u))
                return false;
    margins.layers[2].repeat_width = kPeriod * 16;
    return true;
}
