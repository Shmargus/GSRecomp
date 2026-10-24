#pragma once

// The games' own options, game_options.ini next to the program ("Key=value"
// lines; the engine's config.ini is its own). Shared by both games: each
// setter rewrites the file with every key it holds, so one feature's save
// keeps the others'. Callers check a loaded value's range themselves.

#include <string>

namespace gbarecomp {

// The keys both games use, so their files read the same.
inline constexpr const char* kOptionEncounterRate = "EncounterRate";
inline constexpr const char* kOptionBattleSpeed = "BattleSpeed";
inline constexpr const char* kOptionFieldPsynergy = "FieldPsynergy";
inline constexpr const char* kOptionExpMultiplier = "ExpMultiplier";
inline constexpr const char* kOptionCoinMultiplier = "CoinMultiplier";
inline constexpr const char* kOptionDropMultiplier = "DropMultiplier";
// Music mod (local recordings in place of the game's songs): launcher cog,
// its first-run question and F1 > Audio. Absent until the player decides.
inline constexpr const char* kOptionMusicMod = "MusicMod";

void game_options_load(const std::string& path);
int game_option_get(const char* key, int fallback);
void game_option_set(const char* key, int value);

}  // namespace gbarecomp
