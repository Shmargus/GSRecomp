#pragma once

#include "seamless.h"

namespace gbarecomp::soft_reset {
// Reserve the next session-action code after Seamless's three saved slots.
// The runtime flushes saves and exits; the collection relaunches the same
// game without a savestate or Seamless import, giving it a fresh boot.
inline constexpr int kExitCode = seamless::kExitCodeBase + seamless::kSlotCount;
}
