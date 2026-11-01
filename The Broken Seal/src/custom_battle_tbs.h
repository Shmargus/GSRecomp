#pragma once
#include <cstdint>

// The Broken Seal's Custom Battle Mode (custom_battle.h): its measured
// addresses and catalogue, and the battle sprite page that only this game's
// fights need so far (battle_sprite_memory.h).
namespace gsr::custom_battle::tbs {

// Before run_game: installs The Broken Seal's table.
void install();
void on_entry(std::uint32_t pc);
int sprite_tile(int oam_index, std::uint16_t attr0, std::uint16_t attr1,
                std::uint16_t attr2);
// Savestate loaded.
void reset();

}  // namespace gsr::custom_battle::tbs
