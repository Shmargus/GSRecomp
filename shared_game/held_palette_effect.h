// Palette effects on a held world-map picture. The game recolours its
// frozen native picture while menus/casts have stopped the affine map.
// Keep the expanded borders in step without following a menu's own banks.
// Original TBS rule (F12 0102/0062/0063); TLA Retreat 0199 has the same
// whole-palette change. No pixels inside the native window are touched.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace gsr {
class HeldPaletteEffect {
public:
    void save(const std::vector<std::uint8_t>& palette) {
        valid_ = palette.size() >= 512u;
        if (valid_)
            for (unsigned i = 0; i < saved_.size(); ++i)
                saved_[i] = palette[i * 2u] | (unsigned(palette[i * 2u + 1u]) << 8);
        reset_indices();
    }
    void reset_indices() { indices_.clear(); }

    bool apply(std::uint8_t* rgb, unsigned width, unsigned height,
               const std::vector<std::uint8_t>& held,
               const std::vector<std::uint8_t>& palette) {
        if (!valid_ || palette.size() < 512u || width < 240u || height < 160u ||
            held.size() != std::size_t(width) * height * 3u) return false;
        int delta[256][3];
        std::array<bool, 16> used{}, changed{};
        for (unsigned i = 0; i < saved_.size(); ++i) {
            const auto live = std::uint16_t(palette[i * 2u] | (unsigned(palette[i * 2u + 1u]) << 8));
            int a[3], b[3];
            rgb_of(saved_[i], a); rgb_of(live, b);
            for (unsigned k = 0; k < 3; ++k) delta[i][k] = b[k] - a[k];
            if (saved_[i]) used[i / 16u] = true;
            if (saved_[i] != live) changed[i / 16u] = true;
        }
        bool whole_palette = false;
        // Both measured cast effects change every used bank 0..13. Menus
        // change only a subset; banks 14/15 also contain window colours.
        for (unsigned bank = 0; bank < 14; ++bank) {
            if (!used[bank]) continue;
            if (!changed[bank]) return false;
            whole_palette = true;
        }
        if (!whole_palette) return false;
        if (indices_.empty()) index_colours(held);
        const auto x0 = (width - 240u) / 2u, y0 = (height - 160u) / 2u;
        for (unsigned y = 0; y < height; ++y) {
            auto* row = rgb + std::size_t(y) * width * 3u;
            for (unsigned x = 0; x < width; ++x) {
                if (y >= y0 && y < y0 + 160u && x == x0) { x += 239u; continue; }
                auto* p = row + std::size_t(x) * 3u;
                const auto it = indices_.find(key(p));
                if (it == indices_.end()) continue;
                for (unsigned k = 0; k < 3; ++k)
                    p[k] = std::uint8_t(std::clamp(p[k] + delta[it->second][k], 0, 255));
            }
        }
        return true;
    }
private:
    static std::uint32_t key(const std::uint8_t* p) {
        return (std::uint32_t(p[0]) << 16) | (std::uint32_t(p[1]) << 8) | p[2];
    }
    static void rgb_of(std::uint16_t c, int out[3]) {
        out[0] = (c & 31) * 255 / 31;
        out[1] = ((c >> 5) & 31) * 255 / 31;
        out[2] = ((c >> 10) & 31) * 255 / 31;
    }
    void index_colours(const std::vector<std::uint8_t>& held) {
        int colours[256][3];
        for (unsigned i = 0; i < saved_.size(); ++i) rgb_of(saved_[i], colours[i]);
        for (std::size_t i = 0; i < held.size(); i += 3u) {
            const auto* p = held.data() + i;
            const auto colour = key(p);
            if (indices_.count(colour)) continue;
            int best = 0, best_distance = 1 << 30;
            for (unsigned k = 0; k < saved_.size(); ++k) {
                const int dr = p[0] - colours[k][0], dg = p[1] - colours[k][1], db = p[2] - colours[k][2];
                const int distance = dr * dr + dg * dg + db * db;
                if (distance < best_distance) { best_distance = distance; best = k; }
            }
            indices_[colour] = std::uint8_t(best);
        }
    }
    bool valid_ = false;
    std::array<std::uint16_t, 256> saved_{};
    std::unordered_map<std::uint32_t, std::uint8_t> indices_;
};
} // namespace gsr
