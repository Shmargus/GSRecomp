#pragma once

// Custom Battle Mode (custom_battle.h): one copy of the option name the game
// and the launcher's switch both use. game_options.ini, 0 = off (the game's
// own random opponent), 1 = the receptionist offers to choose them; on
// when the file has no value (Jimmy, 30 Oct).
namespace gsr::custom_battle {
inline constexpr const char* kOptionKey = "CustomBattle";
inline constexpr int kOptionDefault = 1;
}  // namespace gsr::custom_battle
