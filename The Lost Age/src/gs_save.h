// GS1 battery-save parsing. Layout measured from its own save/load routines;
// no cartridge data is embedded here. See FACTS.md (2026-10-11).
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace gs_save {
constexpr std::size_t kFileBytes = 0x10000;
constexpr std::size_t kTransferBytes = 0xF80;
struct ClearSlot {
    unsigned slot;  // 1..3, independent of physical flash sector
    unsigned level;
    std::string name;
    std::array<std::uint8_t, kTransferBytes> transfer{};
};
std::vector<ClearSlot> cleared_slots(const std::vector<std::uint8_t>& bytes);
}  // namespace gs_save
