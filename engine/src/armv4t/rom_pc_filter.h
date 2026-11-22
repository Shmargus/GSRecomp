#pragma once

// A set of game ROM addresses, for skipping hooks at pcs that cannot match.
//
// A game's filtered function-entry and store hooks act only at addresses
// from its ROM address table, so a pc whose bit is clear (any other ROM
// address, and all RAM and BIOS code) matches none of them. Hooks that act
// at RAM addresses are not filtered. One bit per halfword, folded to 64 Ki
// bits (8 KB): a fold collision lets a pc through, never stops one.

#include <array>
#include <cstdint>

namespace gbarecomp {

class RomPcFilter {
public:
    void add(std::uint32_t address) {
        const std::uint32_t k = key(address);
        bits_[k >> 6] |= 1ull << (k & 63u);
    }
    bool may_match(std::uint32_t pc) const {
        const std::uint32_t k = key(pc);
        return ((bits_[k >> 6] >> (k & 63u)) & 1u) != 0;
    }

private:
    static std::uint32_t key(std::uint32_t address) { return (address >> 1) & 0xFFFFu; }
    std::array<std::uint64_t, 1024> bits_{};
};

}  // namespace gbarecomp

// The store hook (g_runtime_mem_write_override) is called only for store pcs
// that `filter` may match. Null (the default) calls it for every store.
extern "C" void runtime_set_mem_write_override_filter(
    const gbarecomp::RomPcFilter* filter);
