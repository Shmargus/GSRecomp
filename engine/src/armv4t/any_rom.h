// The launcher's per-game "Skip the ROM check" (launcher-settings.ini,
// AnyRom). The game's session sets this variable from it for itself and
// everything it starts, so the session, the player builder and the game take
// the player's ROM whatever its SHA-1. The game code is translated for the
// supported ROM; another ROM may not work.
#pragma once
#include "env_flag.h"

namespace gbarecomp {

inline constexpr const char* kAnyRomEnv = "GSR_ANY_ROM";

inline bool any_rom_allowed() { return env_flag(kAnyRomEnv); }

}  // namespace gbarecomp
