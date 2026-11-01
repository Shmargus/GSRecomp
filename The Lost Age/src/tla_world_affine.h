#pragma once
#include "field_scene.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

// World-map row geometry, measured from F12 0158 and the original routines:
// 0802DD70 produces 160 {scale, distance, ...} records, 20 bytes each, at
// [0300001C]. 0802146C's copied ARM writer turns them into the two DMA maps.
// Its integer PA/PC rounding is amplified by the renderer's 16-row endpoint
// trend. Recover the common direction/translation from every native row and
// extend that same trend using the game's unrounded scale/distance instead.
// The writer can update its cached direction before presentation, so cached
// PA/PC are not the captured frame's identity. Validate the captured rows.
inline bool tla_world_affine(gsr::FieldScene& scene, gsr::SceneMargins& margins,
                             const std::uint8_t* iwram, std::size_t bytes) {
    if (!iwram || bytes < 0x11F0u || scene.video_mode != 2) return false;
    auto u32 = [&](std::size_t at) {
        return std::uint32_t(iwram[at]) | (std::uint32_t(iwram[at+1]) << 8) |
               (std::uint32_t(iwram[at+2]) << 16) | (std::uint32_t(iwram[at+3]) << 24);
    };
    const auto address = u32(0x1C);
    if (address < 0x03000000u || address - 0x03000000u > bytes ||
        bytes - (address - 0x03000000u) < 160u * 20u) return false;
    const auto base = address - 0x03000000u;
    const int cx = static_cast<std::int32_t>(u32(0x11EC));
    if (cx < 0 || cx >= 240) return false;
    std::array<double, 160> a{}, b{};
    double mean_b = 0;
    for (int y = 0; y < 160; ++y) {
        if (!scene.row_io_valid[y] || !scene.row_affine_valid[y]) return false;
        a[y] = static_cast<std::int32_t>(u32(base + y * 20)) / 16777216.0;
        b[y] = static_cast<std::int32_t>(u32(base + y * 20 + 4)) / 16777216.0;
        mean_b += b[y] / 160.0;
    }
    double bb = 0;
    for (const auto value : b) bb += (value - mean_b) * (value - mean_b);
    if (bb == 0) return false;
    bool found = false;
    for (int bg = 2; bg < 4; ++bg) {
        const int slot = (bg - 2) * 2, offset = 0x20 + (bg - 2) * 16;
        std::array<std::array<double, 4>, 160> rows{};
        for (int y = 0; y < 160; ++y) {
            const auto& io = scene.row_io[y];
            auto s16 = [&](int at) { return static_cast<std::int16_t>(io[at] | (unsigned(io[at+1]) << 8)); };
            rows[y] = {double(s16(offset)), double(s16(offset+4)),
                       double(scene.row_affine[y][slot]), double(scene.row_affine[y][slot+1])};
        }
        double best = std::numeric_limits<double>::infinity(), residual = best;
        std::array<double, 2> direction{};
        // The original writer alternates a quarter/half-step dither. Its
        // changed-angle branch omits that dither; test both exact formulas.
        for (int phase = 0; phase < 2; ++phase) {
            std::array<std::array<double, 2>, 160> values{};
            std::array<double, 2> means{}, slopes{};
            for (int y = 0; y < 160; ++y) {
                const int pa = int(rows[y][0]), pc = int(rows[y][1]);
                values[y] = {rows[y][2] + cx * pa - (phase ? pc / 4 + pa / 2 : 0),
                             rows[y][3] + cx * pc - (phase ? pa / 4 + pc / 2 : 0)};
                for (int axis = 0; axis < 2; ++axis) means[axis] += values[y][axis] / 160.0;
            }
            for (int y = 0; y < 160; ++y)
                for (int axis = 0; axis < 2; ++axis)
                    slopes[axis] += (b[y] - mean_b) * (values[y][axis] - means[axis]) / bb;
            double square = 0, largest = 0;
            for (int y = 0; y < 160; ++y)
                for (int axis = 0; axis < 2; ++axis) {
                    const double error = values[y][axis] - means[axis] - slopes[axis] * (b[y] - mean_b);
                    square += error * error; largest = std::max(largest, std::abs(error));
                }
            if (square < best) { best = square; residual = largest; direction = slopes; }
        }
        // One unit is the native reference/PA/PC quantization, not a tuned
        // tolerance. Reject unrelated/reused memory or a different camera.
        if (residual > 1) continue;
        bool matched = true;
        for (int y = 0; y < 160; ++y)
            matched &= std::abs(-a[y] * direction[1] - rows[y][0]) <= 1 &&
                       std::abs(a[y] * direction[0] - rows[y][1]) <= 1;
        if (!matched) continue;
        auto& layer = margins.layers[bg];
        layer.affine_step_valid = true; found = true;
        for (int edge = 0; edge < 2; ++edge) {
            // Keep the existing 16-row continuation span on each edge.
            const int end = edge ? 159 : 0, far = edge ? 143 : 16;
            const double da = (a[end] - a[far]) / (end - far);
            const double db = (b[end] - b[far]) / (end - far);
            const double pa = -da * direction[1], pc = da * direction[0];
            layer.affine_step[edge] = {float(db * direction[0] - cx * pa),
                float(db * direction[1] - cx * pc), float(pa), float(pc)};
        }
    }
    return found;
}
