#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gsr::mods {

// Game-owned ROM patch, installed as RunOptions::rom_patch. Called once after
// the ROM is loaded and verified, before the bus sees it. Data only: tables
// and text the game reads at run time, never code or literal pools.
// May append to the vector (text extensions past the original end).
void patch_rom(std::vector<std::uint8_t>* rom);

}  // namespace gsr::mods
