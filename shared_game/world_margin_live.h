// The world map's live margins, both games (2026-11-08): the margins draw the
// settled world map again EVERY frame, with this frame's sprites, sprite art
// and sprite colours, instead of one held photo that froze everything out
// there during menus, dialogue and Psynergy (The Broken Seal gpu_rewind_0096:
// sparkles stopped at the old top edge; 0097: caravan dialogue). The photo
// stays the fallback. Each game passes its own capture type (The Broken
// Seal's GpuFieldCapture, The Lost Age's Capture); both carry these fields.
#pragma once

#include <algorithm>
#include <new>

namespace gsr {

// `mix` becomes the settled capture with this frame's sprite state, and the
// same guest memory the main draw just used, so state read from it sees one
// input per frame. False (nothing to draw) when either capture is short, or
// on an allocation failure.
template <class Capture>
bool live_world_margin_capture(const Capture& settled, const Capture& live, Capture& mix) {
    if (!settled.valid || live.oam.size() < 1024u || live.pal.size() < 1024u ||
        live.vram.size() < 0x18000u || settled.vram.size() < 0x18000u)
        return false;
    try {
        mix = settled;
        mix.effects = {};
        mix.ewram = live.ewram;
        mix.iwram = live.iwram;
        mix.scene.objects = live.scene.objects;
        mix.scene.object_transforms = live.scene.object_transforms;
        mix.scene.one_dimensional_objects = live.scene.one_dimensional_objects;
        // Sprite colours (the palette's second half) and sprite art.
        std::copy(live.scene.palette.begin() + 256, live.scene.palette.end(),
                  mix.scene.palette.begin() + 256);
        mix.oam = live.oam;
        if (mix.pal.size() >= 1024u)
            std::copy(live.pal.begin() + 0x200, live.pal.begin() + 0x400, mix.pal.begin() + 0x200);
        std::copy(live.vram.begin() + 0x10000, live.vram.begin() + 0x18000, mix.vram.begin() + 0x10000);
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

}  // namespace gsr
