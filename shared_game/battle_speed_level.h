#pragma once

#include <atomic>
#include <string>

#include "quality_of_life_menu.h"
#include "game_options.h"

// Battle speed-up level, both games: Off, 2x or 4x. Set in Pause > Quality of
// Life > Battle speed, and Select in battle steps through the same three.
// Saved as BattleSpeed (0, 1, 2); 1 was the old On, so old files keep 2x.
//
// Each game's frame wait lets `skips()` passes in a row fall through without
// waiting for VBlank, then waits once: 1 skip runs two battle frames per
// VBlank, 3 skips run four. Field Psynergy's fast cast stays at 1 skip.
namespace gsr::battle_speed_level {

enum Level { kOff = 0, k2x = 1, k4x = 2, kCount = 3 };
constexpr const char* kWords[kCount] = {"Off", "2x", "4x"};
constexpr const char* kIndicators[kCount] = {"", ">>", ">>>"};
constexpr int kSkips[kCount] = {0, 1, 3};

inline std::atomic<int> level{kOff};

inline int clamp(int v) { return v >= 0 && v < kCount ? v : kOff; }
inline void load() {
    level.store(clamp(gbarecomp::game_option_get(gbarecomp::kOptionBattleSpeed, kOff)));
}
inline void set(int v) {
    level.store(clamp(v));
    gbarecomp::game_option_set(gbarecomp::kOptionBattleSpeed, level.load());
}
inline void step(int direction) { set((level.load() + kCount + direction) % kCount); }
inline bool on() { return level.load() != kOff; }
inline int skips() { return kSkips[clamp(level.load())]; }
inline const char* indicator() { return kIndicators[clamp(level.load())]; }

inline qol_menu::Row qol_row() {
    return {"Battle speed", "Run battles faster. Select in battle also changes it.",
        [] { return std::string(kWords[clamp(level.load())]); },
        [](int direction) { step(direction); }};
}

}  // namespace gsr::battle_speed_level
