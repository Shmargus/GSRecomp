#pragma once

#include <cstdint>
#include <vector>

#include "effect_particles.h"
#include "field_scene.h"

// Diagnostic "move probe", riding on the launcher test "Mod test: Earth Surge"
// (env GSR_MOD_FIELD_TEST, not "0"). Logs, for a battle move, the frame it
// starts (guest function 0x080B9D34, R0 = the battle action record) and, for
// the 300 frames after, where the host effect sparks land. Log lines start
// with "[move-probe]". Does nothing when the toggle is off.
namespace gsr {

// Guest function-entry hook (emulation thread).
void move_probe_on_entry(std::uint32_t entry_pc);

// Present-side hook, once per presented frame of the GPU field path. `layer`
// is the scene's effect layer, or null when effect_layer < 0.
void move_probe_on_sparks(std::uint64_t frame, int effect_layer,
                          const std::vector<EffectSpark>& stamps,
                          const SceneLayer* layer);

}  // namespace gsr
