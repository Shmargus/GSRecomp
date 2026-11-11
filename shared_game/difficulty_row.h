// The settings screen's Difficulty row, one copy of the rules for both
// runners (Jimmy, 11 November 2026). It is the game's own Auto-Sleep row (each
// game's hard mode file), cycling Normal, Scaled, Hard, Hard Scaled: Hard
// is each game's Hard Mode byte, Scaled the run's Scaled flag
// (random_psynergy_format.h), the same two choices New Game makes.
//
// The game's row holds two values. Left takes one off and Right adds one,
// wrapping, and either redraws the row (TBS 0x0801D800..0x0801D83E, TLA
// 0x08040458..0x0804049E; verified ROMs), so the game's own value only
// flips. Each runner steps the row's choice on the same keys, shows its word
// in the value's place, and on closing writes it back after A/Start only.
#pragma once

namespace gsr::difficulty_row {

constexpr int kCount = 4;
constexpr const char* kLabel = "Difficulty";
constexpr const char* kHelp = "Set enemy strength.";
constexpr const char* kWords[kCount] = {"Normal", "Scaled", "Hard", "Hard Scaled"};

constexpr int index(bool hard, bool scaled) { return (hard ? 2 : 0) + (scaled ? 1 : 0); }
constexpr bool hard(int i) { return i >= 2; }
constexpr bool scaled(int i) { return (i & 1) != 0; }

// The choice while the screen is open; set at its setup.
inline int g_choice = 0;

inline void open(bool is_hard, bool is_scaled) { g_choice = index(is_hard, is_scaled); }
inline const char* word() { return kWords[g_choice >= 0 && g_choice < kCount ? g_choice : 0]; }
// Left and Right on the row, as the game reads them (Left is tested first;
// both together cancel out, as they do for the game's own value).
inline void keys(bool left, bool right) {
    if (left) g_choice = (g_choice + kCount - 1) % kCount;
    if (right) g_choice = (g_choice + 1) % kCount;
}

}  // namespace gsr::difficulty_row
