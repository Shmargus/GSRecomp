// Menus and spell casts can recolour the frozen world map. Apply that
// palette change to the margins while ignoring the menu's palette banks.
// The native view comes from the game (measured in captures of both games,
// including The Lost Age's Retreat).
#pragma once
#include <array>
#include <cstdint>
#include <vector>

namespace gsr {
class HeldPaletteEffect {
public:
    void save(const std::vector<std::uint8_t>& palette) {
        valid_ = palette.size() >= 512u;
        if (valid_)
            for (unsigned i = 0; i < saved_.size(); ++i)
                saved_[i] = colour_at(palette, i);
    }

    bool changes_background(const std::vector<std::uint8_t>& palette) const {
        if (!valid_ || palette.size() < 512u) return false;
        std::array<bool, 16> used{}, changed{};
        for (unsigned i = 0; i < saved_.size(); ++i) {
            if (saved_[i]) used[i / 16u] = true;
            if (saved_[i] != colour_at(palette, i)) changed[i / 16u] = true;
        }
        bool whole_palette = false;
        // Both measured cast effects change every used bank 0..13. Menus
        // change only a subset; banks 14/15 also contain window colours.
        for (unsigned bank = 0; bank < 14; ++bank) {
            if (!used[bank]) continue;
            if (!changed[bank]) return false;
            whole_palette = true;
        }
        return whole_palette;
    }

private:
    static std::uint16_t colour_at(const std::vector<std::uint8_t>& palette, unsigned i) {
        return palette[i * 2u] | (unsigned(palette[i * 2u + 1u]) << 8);
    }
    bool valid_ = false;
    std::array<std::uint16_t, 256> saved_{};
};
} // namespace gsr
