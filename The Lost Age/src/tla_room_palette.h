#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Djinn portraits replace banks 4-7, and the existing room-palette copy can
// still return those menu colours (F12 0066).
// The game's packed palette at allocation 0x80 retains the room colours
// (F12 0035/0066; ROM 0x080D1760 writes its first 224 entries). Use those
// four banks only while the other non-animated room banks still agree.
inline bool tla_restore_room_palette(bool window_open,
                                     const std::uint8_t* ewram, std::size_t ewram_size,
                                     const std::uint8_t* iwram, std::size_t iwram_size,
                                     std::vector<std::uint16_t>* palette) {
    if (!window_open || !ewram || !iwram || !palette ||
        palette->size() < 256 || iwram_size < 0x84)
        return false;
    const std::uint32_t ptr = std::uint32_t(iwram[0x80]) |
        (std::uint32_t(iwram[0x81]) << 8) |
        (std::uint32_t(iwram[0x82]) << 16) |
        (std::uint32_t(iwram[0x83]) << 24);
    if (ptr < 0x02000000u || ptr - 0x02000000u > ewram_size ||
        ewram_size - (ptr - 0x02000000u) < 224 * 2)
        return false;
    const auto* source = ewram + (ptr - 0x02000000u);
    auto colour = [&](int i) {
        return std::uint16_t(source[i * 2]) |
               (std::uint16_t(source[i * 2 + 1]) << 8);
    };
    // Bank 0 includes the animated water colours (0066 indices 12-15).
    // Banks 14-15 belong to the window system, not this 224-colour buffer.
    for (int bank = 1; bank < 14; ++bank) {
        if (bank >= 4 && bank < 8) continue;
        for (int i = bank * 16; i < (bank + 1) * 16; ++i)
            if ((*palette)[i] != colour(i)) return false;
    }
    bool changed = false;
    for (int i = 4 * 16; i < 8 * 16; ++i) {
        changed |= (*palette)[i] != colour(i);
        (*palette)[i] = colour(i);
    }
    return changed;
}
