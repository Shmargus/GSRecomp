#pragma once

// The games' own options, game_options.ini next to the program ("Key=value"
// lines; the engine's config.ini is its own). Shared by both games: each
// setter rewrites the file with every key it holds, so one feature's save
// keeps the others'. Callers check a loaded value's range themselves.

#include <string>
#include <span>

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
inline constexpr const char* kOptionAreaMusic = "AreaMusic";
inline constexpr const char* kOptionConfirmQuit = "ConfirmQuit";

void game_options_load(const std::string& path);
int game_option_get(const char* key, int fallback);
void game_option_set(const char* key, int value);
// Remove one feature's saved choices in a single file replacement.
void game_option_reset_prefix(const char* prefix);
struct GameOptionValue { std::string key; int value; };
// One locked transaction and file replacement for related edits.
void game_options_update(std::span<const GameOptionValue> values,
                         std::span<const char* const> reset_prefixes = {});

}  // namespace gbarecomp
