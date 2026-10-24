#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "field_scene.h"

namespace gsr {

// FNV-1a over every 4bpp tile one object shows, row by row. It does not
// depend on the object's palette, position, OAM slot or where its tiles sit
// in sprite memory, so a game can recognise measured artwork after the
// sprite memory is rearranged. False when the object is not a whole number
// of tiles or reaches past sprite memory.
inline bool object_artwork_fingerprint_4bpp(
    const FieldScene& scene, const SceneObject& object,
    const std::vector<std::uint8_t>& vram, std::uint64_t* fingerprint) {
    if (!fingerprint || object.width <= 0 || object.height <= 0 ||
        object.width % 8 != 0 || object.height % 8 != 0)
        return false;
    const std::size_t stride = scene.one_dimensional_objects
        ? static_cast<std::size_t>(object.width / 8) : 32u;
    std::uint64_t hash = UINT64_C(14695981039346656037);
    for (int y = 0; y < object.height / 8; ++y)
        for (int x = 0; x < object.width / 8; ++x) {
            const std::size_t tile = object.tile +
                static_cast<std::size_t>(y) * stride +
                static_cast<std::size_t>(x);
            const std::size_t at = 0x10000u + tile * 32u;
            if (at > vram.size() || vram.size() - at < 32u) return false;
            for (int byte = 0; byte < 32; ++byte)
                hash = (hash ^ vram[at + static_cast<std::size_t>(byte)]) *
                       UINT64_C(1099511628211);
        }
    *fingerprint = hash;
    return true;
}

}  // namespace gsr
