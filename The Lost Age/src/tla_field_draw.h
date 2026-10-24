// tla_field_draw.h -- one frame's margin rules and renderer uploads, from the
// scene to FieldSceneRenderer::draw.
//
// The one copy the game (src/tla_gpu_field.cpp) and the bench
// (tools/tla_frame_check.cpp) both draw with, so a rule added for the game is
// what the bench checks. Before this the bench kept its own list of steps and
// fell behind (it never kept the camera rule's wider bounds, Lemuria's dock
// stairs, F12 0136). Reading back the picture, and anything painted over it
// afterwards (the flat map's margins), stays with the caller.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "effect_capture.h"
#include "field_scene.h"
#include "field_scene_renderer.h"
#include "scene_repeat.h"
#include "tla_alhafra_ship_view.h"
#include "tla_background_margins.h"
#include "tla_room_sprite_clip.h"
#include "tla_screen_effect_margins.h"
#include "tla_ship_water_margins.h"
#include "tla_tornado_margins.h"
#include "tla_water_ripple.h"
#include "tla_world_climates.h"
#include "world_map_source.h"

// What one frame hands the renderer besides its scene. Pointers may be null
// where a saved frame has no such section.
struct TlaFieldFrame {
    // Video memory: the console's 96 KB, then the second sprite page when it
    // is in use.
    const std::vector<std::uint8_t>* vram = nullptr;
    const std::uint8_t* ewram = nullptr;  // 256 KB
    std::size_t ewram_bytes = 0;
    const std::uint8_t* iwram = nullptr;  // 32 KB
    std::size_t iwram_bytes = 0;
    const std::uint8_t* rom = nullptr;
    std::size_t rom_bytes = 0;
    // The background area and colours from before a game window opened
    // (tla_gpu_field.cpp build_room_vram); null while none is open.
    const std::uint8_t* room_vram = nullptr;
    std::size_t room_vram_bytes = 0;
    const std::uint16_t* room_palette = nullptr;
    std::size_t room_palette_entries = 0;
    bool menu_open = false;
    bool reveal_full = false;  // Better Field Psy: Reveal covers the whole view
    unsigned ripple_phase = 0;
    int camera_hold_y = 0;      // tla_camera_hold_y
    // tla_camera_kept_bounds (x, y), whole pixels.
    bool kept[2] = {false, false};
    int kept_lo[2] = {0, 0}, kept_edge[2] = {0, 0};
    // Spell sparks for the margins. `effects` false leaves the effect
    // renderer as it is (frames saved before it existed).
    bool effects = true;
    const gsr::EffectSpark* sparks = nullptr;
    int spark_count = 0;
    const std::uint8_t* spark_art = nullptr;
    std::size_t spark_art_bytes = 0;
};

// Kept from frame to frame by the caller (one per renderer).
struct TlaFieldDrawState {
    gsr::WorldMapSource world_map;
    TlaWorldClimates climates;
    std::vector<std::uint8_t> haze_vram;
    std::vector<std::uint8_t> room_ewram;  // the margins' edited copy
};

// Which rules took part, for the bench's report.
struct TlaFieldDrawReport {
    bool ripple = false, port = false, haze = false;
    bool ship_water = false, background = false;
    bool ship_view = false, kept = false, room_known = false;
    bool world_map = false;
    int world_map_agreement = -1, world_map_compared = 0;
};

// Applies every margin rule to `scene` and `margins` (iris already set),
// uploads the frame and draws it at width x height. False: the renderer
// declined (FieldSceneRenderer::declined_reason).
inline bool tla_field_draw(gsr::FieldSceneRenderer& renderer, gsr::FieldScene& scene,
                           gsr::SceneMargins& margins, const TlaFieldFrame& in,
                           int width, int height, TlaFieldDrawState& state,
                           TlaFieldDrawReport* report = nullptr) {
    TlaFieldDrawReport local;
    TlaFieldDrawReport& r = report ? *report : local;
    r = {};
    const std::vector<std::uint8_t>& frame_vram = *in.vram;
    // The room source only where there is a margin to fill: the native view
    // stays exactly the picture checked against the console compositor.
    const bool expanded = width > 240 || height > 160;
    const int first_y = -(height - 160) / 2;
    if (expanded)
        tla_hide_outside_room(scene, in.ewram, in.ewram_bytes, in.iwram, in.iwram_bytes);
    tla_weather_sprite_margins(scene, margins, frame_vram);
    tla_tornado_margins(scene, margins,
        expanded && frame_vram.size() >= 0x2800u ? frame_vram.data() : nullptr,
        first_y, in.camera_hold_y);
    if (expanded && in.rom && in.rom_bytes >= 0x20A8u + 1024u * 2u) {
        const auto sine = [&](unsigned index) {
            const auto* p = in.rom + 0x20A8u + index * 2u;
            return static_cast<std::int16_t>(unsigned(p[0]) | (unsigned(p[1]) << 8));
        };
        r.ripple = tla_water_ripple(scene, margins, in.ripple_phase, sine, first_y, height);
        r.port = tla_port_wave(scene, margins, in.ripple_phase, sine, first_y, height);
    } else {
        margins.layers[1].scroll.clear();
        margins.layers[2].scroll.clear();
    }
    r.haze = r.port && tla_haze_fill_rows(frame_vram, scene, &state.haze_vram, 2);
    if (expanded) {
        r.ship_water = tla_ship_water_margins(scene, margins, frame_vram,
            in.ewram, in.ewram_bytes, width, height);
        r.background = tla_background_margins(scene, margins, in.ewram, in.ewram_bytes);
    }
    const std::vector<std::uint8_t>& vram = r.haze ? state.haze_vram : frame_vram;
    renderer.upload_vram(vram.data(), vram.size());
    if (in.room_vram) renderer.upload_room_vram(in.room_vram, in.room_vram_bytes);
    renderer.upload_palette(scene.palette.data(), scene.palette.size());
    if (in.room_palette)
        renderer.upload_room_palette(in.room_palette, in.room_palette_entries);
    renderer.set_room_source_enabled(expanded);
    renderer.set_menu_open(in.menu_open);
    renderer.set_reveal_full(in.reveal_full);

    if (expanded) {
        const std::uint8_t* room = in.ewram;
        r.ship_view = tla_alhafra_ship_view(in.ewram, in.ewram_bytes, &state.room_ewram);
        if (r.ship_view) room = state.room_ewram.data();
        // A narrowing the camera rule keeps (tla_camera_kept_bounds): the
        // margins' copy of the clamp (field state +0xEC..+0xF8, integer
        // halves; the state is [0x03000020]) keeps the wider bounds, so the
        // scenery stays drawn.
        const std::uint32_t state_at = in.iwram && in.iwram_bytes >= 0x24u
            ? std::uint32_t(in.iwram[0x20]) | (std::uint32_t(in.iwram[0x21]) << 8) |
              (std::uint32_t(in.iwram[0x22]) << 16) | (std::uint32_t(in.iwram[0x23]) << 24)
            : 0u;
        const std::size_t field = state_at & 0x00FFFFFFu;
        if ((in.kept[0] || in.kept[1]) && in.ewram && (state_at >> 24) == 0x02u &&
            field + 0xFCu <= in.ewram_bytes) {
            if (!r.ship_view) state.room_ewram.assign(in.ewram, in.ewram + in.ewram_bytes);
            auto put = [&](std::size_t at, int value) {
                state.room_ewram[at] = std::uint8_t(value);
                state.room_ewram[at + 1] = std::uint8_t(value >> 8);
            };
            for (int n = 0; n < 2; ++n) {
                if (!in.kept[n]) continue;
                put(field + 0xEEu + 4u * n, in.kept_lo[n]);
                put(field + 0xF6u + 4u * n, in.kept_edge[n]);
            }
            room = state.room_ewram.data();
            r.kept = true;
        }
        r.room_known = renderer.upload_room(room, room ? in.ewram_bytes : 0);
        // Haze, mist, sunlight and any other repeating layer: the shared
        // rule, once the room source can say which layers it fills.
        gsr::continue_repeating_layers(scene, margins, vram.data(), vram.size(),
            [&](int bg) {
                bool from_record = false;
                return bg != 0 && renderer.room_known() &&
                       renderer.room_reference_for(scene, bg, &from_record);
            },
            first_y, height);
    }

    // Complete map tiles are shared code; this game's wrapper supplies its layout.
    if (expanded && scene.video_mode == 2 &&
        state.world_map.update(in.ewram, in.ewram_bytes, in.iwram, in.iwram_bytes,
                               in.rom, in.rom_bytes)) {
        renderer.upload_world_map(state.world_map.tiles().data(), state.world_map.generation());
        r.world_map_agreement = renderer.world_map_agreement(scene, &r.world_map_compared);
        if (r.world_map_agreement >= 0 &&
            r.world_map_agreement < gsr::FieldSceneRenderer::kRoomCheckMinAgreement &&
            state.world_map.restore_previous())
            renderer.upload_world_map(state.world_map.tiles().data(),
                                      state.world_map.generation());
        r.world_map = true;
    } else {
        renderer.upload_world_map(nullptr, 0);
    }
    // Each margin cell in its own climate (tla_world_climates.h).
    state.climates.apply(renderer, r.world_map, state.world_map, scene.palette.data(),
                         in.iwram, in.iwram_bytes, in.rom, in.rom_bytes);

    // Spell effects in the margins, Golden Sun 1's way (its
    // gpu_field_present_override): the game paints each effect into a
    // canvas as wide as the console's screen, and nothing exists past it.
    // The margins get the canvas's own fill and the sparks the game stamped
    // outside it, never the layer's wrapped or cut map. Expanded view only.
    if (in.effects) {
        renderer.set_effect_renderer_enabled(expanded);
        int effect_layer = -1;
        if (expanded && scene.video_mode == 1) {
            // A battle's non-wrapping affine BG2 (the stretched canvas), else
            // a layer showing the canvas as a tile block.
            const gsr::SceneLayer& bg2 = scene.layers[2];
            effect_layer = (bg2.enabled && bg2.affine && !bg2.wraps)
                ? 2
                : gsr::FieldSceneRenderer::find_effect_canvas_layer(
                      frame_vram.data(), frame_vram.size(), scene);
        }
        renderer.set_effect_layer(effect_layer);
        if (effect_layer >= 0) {
            if (in.spark_count > 0)
                renderer.upload_effect_sparks(in.sparks, in.spark_count, in.spark_art,
                                              in.spark_art_bytes, effect_layer);
            renderer.analyse_effect_canvas(frame_vram.data(), frame_vram.size(),
                                           scene.layers[effect_layer]);
        }
    }
    renderer.set_scene_margins(&margins);
    return renderer.draw(scene);
}
