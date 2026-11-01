#pragma once

#include "field_scene.h"

// F12 0150: Jupiter's pillar is two identical 32x64 pieces, 32 pixels apart,
// vertically flipped without vertical scaling. Their overlapping rounded ends
// make the continuous beam; the upper piece alone ends at native y -56.
// Continue that same stack upward, keeping its live horizontal sway, palette
// and transform. Recognise the complete artwork, not its slot/map/tile number
// (the captured copy uses the second sprite page). No native object changes.
inline bool tla_beacon_margins(const gsr::FieldScene& scene,
                               gsr::SceneMargins& margins,
                               const std::vector<std::uint8_t>& vram,
                               int first_y) {
    if (scene.video_mode != 0 || first_y >= 0 ||
        margins.margin_object_period != std::array<int, 2>{}) return false;
    for (const auto& upper : scene.objects) {
        if (!upper.present || upper.parked || upper.window || !upper.affine ||
            !upper.double_size || upper.width != 32 || upper.height != 64 ||
            upper.palette != -1 || upper.affine_index < 0 ||
            upper.affine_index >= gsr::FieldScene::kTransforms) continue;
        const auto& transform = scene.object_transforms[upper.affine_index];
        if (transform[0] <= 0 || transform[1] != 0 || transform[2] != 0 ||
            transform[3] != -1) continue;
        // FNV-1a of all 32x64 8bpp pixels in tile order, allocation-independent.
        const unsigned stride = scene.one_dimensional_objects ? 8u : 32u;
        std::uint64_t hash = UINT64_C(14695981039346656037);
        bool complete = true;
        for (unsigned y = 0; y < 8 && complete; ++y)
            for (unsigned x = 0; x < 4; ++x) {
                const std::size_t at = 0x10000u +
                    (std::size_t(upper.tile) + y * stride + x * 2u) * 32u;
                if (at > vram.size() || vram.size() - at < 64u) {
                    complete = false;
                    break;
                }
                for (unsigned b = 0; b < 64; ++b)
                    hash = (hash ^ vram[at + b]) * UINT64_C(1099511628211);
            }
        if (!complete || hash != UINT64_C(0x9cd8cc9ebf933453)) continue;
        const int step = upper.height / 2;
        for (const auto& lower : scene.objects) {
            if (!lower.present || lower.parked || lower.window || !lower.affine ||
                !lower.double_size || lower.width != upper.width ||
                lower.height != upper.height || lower.x != upper.x ||
                lower.y != upper.y + step || lower.tile != upper.tile ||
                lower.palette != upper.palette || lower.priority != upper.priority ||
                lower.blended != upper.blended || lower.affine_index < 0 ||
                lower.affine_index >= gsr::FieldScene::kTransforms ||
                scene.object_transforms[lower.affine_index] != transform) continue;
            auto copy = upper;
            while (copy.y + copy.height > first_y) {
                copy.y -= step;
                margins.margin_objects.push_back(copy);
            }
            return true;
        }
    }
    return false;
}
