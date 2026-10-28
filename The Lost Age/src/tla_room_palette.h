#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Djinn portraits replace banks 4-7, and the existing room-palette copy can
// still return those menu colours (F12 0066).
// The game's room-colour allocation at slot 0x80 (0x2A04 bytes, ROM
// 0x080D1684) retains the room colours: its first 224 entries are the
// stored colours (F12 0035/0066; ROM 0x080D1760), and +0x2300 / +0x2680 are
// the two after-effect buffers the game queues for palette RAM (ROM
// 0x080D0D8E; the snow tint of F12 0163 lives only there). Use banks 4-7 of
// the first source whose other non-animated room banks all still agree.
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
    if (ptr < 0x02000000u || ptr - 0x02000000u > ewram_size)
        return false;
    const std::size_t base = ptr - 0x02000000u;
    for (const std::size_t offset : {std::size_t(0), std::size_t(0x2300),
                                     std::size_t(0x2680)}) {
        if (ewram_size - base < offset + 224 * 2) break;
        const auto* source = ewram + base + offset;
        auto colour = [&](int i) {
            return std::uint16_t(source[i * 2]) |
                   (std::uint16_t(source[i * 2 + 1]) << 8);
        };
        // Bank 0 includes the animated water colours (0066 indices 12-15).
        // Banks 14-15 belong to the window system, not these 224-colour buffers.
        bool agree = true;
        for (int bank = 1; bank < 14 && agree; ++bank) {
            if (bank >= 4 && bank < 8) continue;
            for (int i = bank * 16; i < (bank + 1) * 16; ++i)
                if ((*palette)[i] != colour(i)) { agree = false; break; }
        }
        if (!agree) continue;
        bool changed = false;
        for (int i = 4 * 16; i < 8 * 16; ++i) {
            changed |= (*palette)[i] != colour(i);
            (*palette)[i] = colour(i);
        }
        return changed;
    }
    return false;
}
