// Check a complete affine map against the console's visible streaming ring.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gbarecomp {

template <typename Scene>
int world_map_agreement(const std::vector<std::uint8_t>& tiles, int side,
                        const std::vector<std::uint8_t>& vram,
                        const Scene& scene, int minimum_tiles,
                        int* compared = nullptr) {
    if (compared) *compared = 0;
    if (tiles.size() != std::size_t(side) * side * 2 ||
        vram.size() < 0x10000u) return -1;
    int total = 0, same = 0;
    for (int y = 4; y < 160; y += 8) {
        if (!scene.row_affine_valid[y]) continue;
        const auto* io = scene.row_io_valid[y]
            ? scene.row_io[y].data() : scene.io_bytes.data();
        auto rd = [&](std::size_t at) { return int(io[at] | (io[at + 1] << 8)); };
        // Both supported world maps use BG3/BG2 rings in blocks 8/10.
        if ((rd(0) & 7) != 2 || ((rd(0x0E) >> 8) & 31) != 8 ||
            ((rd(0x0C) >> 8) & 31) != 10) continue;
        for (int layer = 2; layer <= 3; ++layer) {
            const int cnt = rd(0x08 + layer * 2);
            if ((cnt & 0x2000) == 0) continue;
            const int ring_side = 16 << ((cnt >> 14) & 3);
            const std::size_t screen = std::size_t((cnt >> 8) & 31) * 2048;
            const int pa = static_cast<std::int16_t>(rd(0x20 + (layer - 2) * 16));
            const int pc = static_cast<std::int16_t>(rd(0x24 + (layer - 2) * 16));
            for (int x = 4; x < 240; x += 8) {
                const int tx = (scene.row_affine[y][(layer - 2) * 2] + pa * x) >> 8;
                const int ty = (scene.row_affine[y][(layer - 2) * 2 + 1] + pc * x) >> 8;
                const auto at = screen + std::size_t(((ty >> 3) & (ring_side - 1)) *
                    ring_side + ((tx >> 3) & (ring_side - 1)));
                if (at >= vram.size()) continue;
                const auto wt = std::size_t(((ty >> 3) & (side - 1)) +
                    (layer == 2 ? side : 0)) * side + ((tx >> 3) & (side - 1));
                ++total;
                if (tiles[wt] == vram[at]) ++same;
            }
        }
    }
    if (compared) *compared = total;
    return total < minimum_tiles ? -1 : same * 100 / total;
}

}  // namespace gbarecomp
