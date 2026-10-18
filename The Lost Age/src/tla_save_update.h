#pragma once

#include "gs_save.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tla_save_update {
struct Slot {
    unsigned slot;  // 1..3
    unsigned level;
    std::string name;
    bool post_reunion;
    std::size_t offset;
};
std::vector<Slot> slots(const std::vector<std::uint8_t>& flash);

// Execute the ROM's importer in a private machine. Nothing in the running
// game's memory or saves is written; the caller commits the result separately.
bool prepare(const std::vector<std::uint8_t>& rom,
             const std::vector<std::uint8_t>& ewram,
             const std::vector<std::uint8_t>& iwram,
             const std::vector<std::uint8_t>& flash, unsigned destination,
             const gs_save::ClearSlot& source,
             std::vector<std::uint8_t>& updated, std::string& error);
}
