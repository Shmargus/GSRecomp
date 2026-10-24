#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "field_scene.h"
#include "object_artwork.h"

namespace tla_haze {

constexpr int kRows = 32, kRowBytes = 64, kCycle = 8;

inline unsigned reg(const gsr::FieldScene& scene, int y, int at) {
    return unsigned(scene.row_io[y][at]) | (unsigned(scene.row_io[y][at + 1]) << 8);
}

// BG1's map in video memory, or npos when it is not a 32x32 map in range.
inline std::size_t map_base(const std::vector<std::uint8_t>& vram,
                            const gsr::FieldScene& scene) {
    const unsigned bg1cnt = reg(scene, 0, 0x0A);
    const std::size_t base = ((bg1cnt >> 8) & 31u) * 0x800u;
    if ((bg1cnt >> 14) != 0 || vram.size() < base + kRows * kRowBytes)
        return std::size_t(-1);
    return base;
}

// The map rows the console's screen rows from `first` on show (BG1VOFS per
// row).
inline void shown_rows(const gsr::FieldScene& scene, bool (&shown)[kRows],
                       int first = 0) {
    for (int r = 0; r < kRows; ++r) shown[r] = false;
    for (int y = first; y < gsr::FieldScene::kRows; ++y)
        if (scene.row_io_valid[y])
            shown[((reg(scene, y, 0x16) & 0x1FFu) + unsigned(y)) / 8u % kRows] = true;
}

}  // namespace tla_haze

// Recognize rain by its complete 4bpp artwork, not map, palette or tile slot.
// These FNV-1a fingerprints were measured from 0093 and agree in every rain
// object in 0097/0098 despite their different sprite-memory allocations.
inline bool tla_rain_artwork(const gsr::FieldScene& scene,
                              const gsr::SceneObject& object,
                              const std::vector<std::uint8_t>& vram) {
    if (object.palette < 0 || object.width != 16 ||
        (object.height != 16 && object.height != 32)) return false;
    std::uint64_t hash = 0;
    if (!gsr::object_artwork_fingerprint_4bpp(scene, object, vram, &hash))
        return false;
    if (object.height == 32) return hash == UINT64_C(0x78bccfb7740d43e5);
    return hash == UINT64_C(0x9b7f9d206c16534f) ||
           hash == UINT64_C(0x27418cc15f5fe5fe) ||
           hash == UINT64_C(0xba5aa823f24b688a) ||
           hash == UINT64_C(0x9179ca96918010b8);
}

// Apojii's rain is a screen-space sprite layout (0093/0097/0098).
// Continue it at its original density and animation phase. Rainbow Water
// is attached to the stream: repeating its sprites across the screen puts
// copies on unrelated ground and sea (0094/0095). Keep its real placements.
inline void tla_weather_sprite_margins(const gsr::FieldScene& scene,
                                       gsr::SceneMargins& margins,
                                       const std::vector<std::uint8_t>& vram) {
    margins.object_repeat = {};
    if (scene.video_mode != 0) return;
    for (int slot = 0; slot < gsr::FieldScene::kObjects; ++slot) {
        const auto& o = scene.objects[slot];
        if (!o.present || o.parked || o.window || o.affine || !o.blended)
            continue;
        if (tla_rain_artwork(scene, o, vram))
            margins.object_repeat[slot] = {240, 160};
    }
}

// The haze and mist are one 64-pixel tile pattern: eight different map rows,
// row r the same as row r + 8 (32x32 map at 0x3800; Air's Rock map 84, F12
// tla_rewind_0030 frames 41/80/119; Yampi map 69, 0031 f119; map 75, 0036
// visible rows). The game rewrites only the rows the console screen shows as
// the layer drifts. Rows out of its view keep what they held: blank entries
// (0x5180) above Air's Rock's screen, black stripes in the margin; rows one
// step out of the cycle in the Yampi cave (0036 f119: rows 2..11 one row
// behind), seams at the old screen edge. The haze, mist and Champa's
// sunlight are now continued by the shared repeating-layer rule
// (engine/src/field/scene_repeat.h), which draws margins only from shown
// rows. This copy-in of the cycle remains for the port water below, whose
// wave moves rows up and down so the shared rule does not apply.
//
// Alhafra's port water (map 98, tla_port_wave) is the same kind of layer:
// the room's BG1 is the 0x3E0..0x3E7 pattern, four map rows a cycle, and
// the game streams only the rows its camera covers. The wave lifts screen
// rows 0 and 1 by a pixel into a row it never streamed, which still holds
// the last room's tiles: a pale line along the top (F12 tla_rewind_0053:
// map row 25, tiles 0x200/0x220). Over all 720 map-98 frames (0040..0044,
// 0053) only screen rows 0 and 1 ever showed a non-water row, so for it
// `first` = 2: rows 2..159 name the streamed rows, and the rest, row 0's
// included, take the cycle.
inline bool tla_haze_fill_rows(const std::vector<std::uint8_t>& vram,
                               const gsr::FieldScene& scene,
                               std::vector<std::uint8_t>* out,
                               int first = 0) {
    using namespace tla_haze;
    const std::size_t base = map_base(vram, scene);
    if (base == std::size_t(-1)) return false;
    bool shown[kRows];
    shown_rows(scene, shown, first);
    int source[kCycle];
    for (int c = 0; c < kCycle; ++c) {
        source[c] = -1;
        for (int r = c; r < kRows && source[c] < 0; r += kCycle)
            if (shown[r]) source[c] = r;
        if (source[c] < 0) return false;
    }
    bool copied = false;
    for (int r = 0; r < kRows; ++r) {
        if (shown[r]) continue;
        const std::uint8_t* src = vram.data() + base + source[r % kCycle] * kRowBytes;
        const std::size_t at = base + r * kRowBytes;
        if (std::memcmp(vram.data() + at, src, kRowBytes) == 0) continue;
        if (!copied) { *out = vram; copied = true; }
        std::memcpy(out->data() + at, src, kRowBytes);
    }
    return copied;
}
