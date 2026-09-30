#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "effect_particles.h"

// The rest of the launcher test "Mod test: Earth Surge" (env
// GSR_MOD_FIELD_TEST, not "0"; see src/mod_loader.cpp for the data half):
//
//  * Cast: mod_loader.cpp gives Earth Surge move kind 5, so the game plays its
//    summon prelude and then Isaac's jump attack (effect word 0x4001). The
//    resolver (0x080BE378) then calls 0x08079EF8 via a veneer with LR
//    0x080BF175; that entry arms the cast.
//  * While armed: the game's own slash is hidden, Isaac leaves a translucent
//    white trail during his jump (found in OAM by tile number), and when the
//    slash lands a big earth explosion with a screen shake is drawn by our
//    renderer on top of the picture.
//
// Everything does nothing when the toggle is off.
namespace gsr {

class FieldSceneRenderer;

// Guest function-entry hook (emulation thread).
void earth_surge_on_entry(std::uint32_t entry_pc);

// Present-side hook, once per presented frame of the GPU field path, after
// the frame's effect stamps were uploaded and analysed. Same thread as
// FieldSceneRenderer::draw. `effect_layer` < 0 means no effect layer.
// `oam` is the frame's 1 KB object attribute memory (or null), for the probe.
void earth_surge_on_present(FieldSceneRenderer& renderer, std::uint64_t frame,
                            int effect_layer,
                            const std::vector<EffectSpark>& stamps,
                            const std::uint8_t* oam, std::size_t oam_bytes);

}  // namespace gsr
