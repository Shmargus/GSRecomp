#pragma once

#include "gs_save.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tla_save_update {
struct Slot {
    unsigned slot;  // the file the player sees, 1..9
    unsigned level;
    std::string name;
    bool post_reunion;
    std::size_t offset;      // of its group in the image it was read from
    bool extension = false;  // files 4-9: the battery extension, not flash
    // A Challenge Mode, Random Psynergy or Random Weapons file (payload
    // +0x28 / +0x30 / +0x38,
    // src/tla_new_game.cpp): fresh games only, never updated (Jimmy, 5 Nov).
    bool locked = false;
};
// Files 1-3, from the cartridge's flash image.
std::vector<Slot> slots(const std::vector<std::uint8_t>& flash);
// Files 4-9, from the battery extension (tla_save_files.cpp's layout).
std::vector<Slot> extension_slots(const std::vector<std::uint8_t>& extension);

// Execute the ROM's importer in a private machine. Nothing in the running
// game's memory or saves is written; the caller commits the result separately.
// `image` is the flash or extension image `destination` was listed from;
// `updated` is that image with the destination's group replaced.
bool prepare(const std::vector<std::uint8_t>& rom,
             const std::vector<std::uint8_t>& ewram,
             const std::vector<std::uint8_t>& iwram,
             const std::vector<std::uint8_t>& image, unsigned destination,
             const gs_save::ClearSlot& source,
             std::vector<std::uint8_t>& updated, std::string& error);
}
