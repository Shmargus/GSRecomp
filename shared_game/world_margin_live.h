// The world map's live margins, both games: the margins draw the
// settled world map again EVERY frame, with this frame's sprites, sprite art
// and sprite colours, instead of one held photo that froze everything out
// there during menus, dialogue and Psynergy (in The Broken Seal, sparkles
// stopped at the old top edge, and so did the caravan dialogue). The photo
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
bool live_world_margin_capture(const Capture& settled, const Capture& live,
                               Capture& mix, bool tint_background) {
    if (!settled.valid || live.oam.size() < 1024u || live.pal.size() < 1024u ||
        live.vram.size() < 0x18000u || settled.vram.size() < 0x18000u)
        return false;
    try {
        mix = settled;
        mix.effects = {};
        mix.ewram = live.ewram;
        mix.iwram = live.iwram;
        mix.scene.objects = live.scene.objects;
        // A game window's own sprites stay out of the margins, as the main
        // draw keeps them in the console's window (FieldSceneRenderer's
        // menu_sprite rule: window open, priority 0). The settled capture is
        // mode 2 with no window, so that rule cannot see them here: The Lost Age's
        // Djinn screen over the world map parks three priority-0 Djinn at
        // y 170 (read as -86), drawn in the top margin.
        if (live.window_open)
            for (auto& o : mix.scene.objects)
                if (o.priority == 0) o.present = false;
        mix.scene.object_transforms = live.scene.object_transforms;
        mix.scene.one_dimensional_objects = live.scene.one_dimensional_objects;
        // A spell can tint the map's palette. Apply that before compositing
        // with the live sprites, whose colours already match the native view.
        const auto first_colour = tint_background ? 0 : 256;
        std::copy(live.scene.palette.begin() + first_colour, live.scene.palette.end(),
                  mix.scene.palette.begin() + first_colour);
        mix.oam = live.oam;
        if (mix.pal.size() >= 1024u)
            std::copy(live.pal.begin() + first_colour * 2, live.pal.begin() + 0x400,
                      mix.pal.begin() + first_colour * 2);
        // Sprite art: the console's, plus the extra sprite page past it when
        // the capture carries one (GbaBus::kObjExtraVram). Retreat's particles
        // on The Lost Age's world map draw from that page (tile 0x4A0..);
        // the settled frame's stale copy garbled them.
        mix.vram.resize(live.vram.size());
        std::copy(live.vram.begin() + 0x10000, live.vram.end(), mix.vram.begin() + 0x10000);
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

}  // namespace gsr
