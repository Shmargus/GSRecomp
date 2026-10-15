// field_game_profile.cpp -- The Broken Seal's values for the shared field scene
// and GPU field renderer (engine/src/field/field_game_profile.h).
#include "field_game_profile.h"

#include "effect_capture.h"
#include "room_buffer.h"
#include "widescreen_policy.h"

namespace gsr {

const FieldGameProfile& field_game_profile() {
    static const FieldGameProfile profile = [] {
        FieldGameProfile p;
        // Room buffer: the same four measured addresses room_buffer.h
        // documents (evidence in FACTS.md).
        p.room_rect_addr = 0x02030DC0u;     // min_x,max_x,min_y,max_y
        // Integer halves of the camera clamp's 16.16 min x / min y.
        p.clamp_min_x_addr = 0x02030DBAu;
        p.clamp_min_y_addr = 0x02030DBEu;
        p.camera_addr = 0x02030DB0u;        // x,y, 16.16 fixed point
        // The game's own scroll record per field layer, 0x30 bytes each, BG3
        // then BG2 then BG1 (measured on rewinds 0052/0074/0090/0092/0093/
        // 0097, 2026-10-01).
        p.layer_record_addr = 0x02030DD0u;
        p.layer_record_bytes = 0x30u;
        p.id_grid_addr = 0x02010000u;       // 128x128 u32, low 12 bits
        p.atlas_addr = 0x02020000u;         // 8 bytes per id
        // The current map number (tools/find_map_id.cpp; ROADMAP.md). It is 0
        // until the game has loaded its first room and at the mode changes
        // between rooms.
        p.map_id_addr = 0x02000408u;
        p.expanded_extra_x = widescreen::kExpandedExtraX;
        p.obj_park_raw_x = widescreen::kGoldenSunObjParkRawX;
        p.obj_park_raw_y = widescreen::kGoldenSunObjParkRawY;
        p.world_map_side_tiles = 512;       // WorldMapSource::kSideTiles
        p.layer_wrap_y_shift = 0;
        // A field frame is only drawable when the room buffer actually holds
        // this room (D-008).
        p.room_source_ready = &room_buffer_rendering;

        p.kolima_split_rows = true;
        p.lamakan_haze = true;
        p.lamakan_row0_swap = true;
        // Altin's drain maps (FACTS.md, 2026-10-06: 79 captured, 77 in
        // mGBA). Not every map: map 188's ship rises ~90 rows through its
        // BG1/BG2 regions and the rule cut it into water squares
        // (gpu_rewind_0037-0044, 2026-10-15).
        p.altin_rest_maps[0] = 79;
        p.altin_rest_maps[1] = 77;
        p.altin_striped_layers = true;
        p.solid_object_strips = true;
        p.margin_scroll_rows = false;
        p.agreement_autoscroll_wrap = false;
        return p;
    }();
    return profile;
}

const EffectCaptureProfile& effect_capture_profile() {
    static const EffectCaptureProfile profile = [] {
        EffectCaptureProfile p;
        // Allocator slots and generator template addresses: verified from
        // Func_ed408 and live gpu_frame_0041/0043, FACTS.md 2026-09-19.
        // Template BYTES stay in the user-supplied ROM.
        p.canvas_slot = 0x03001EF0u;
        p.stamp_slot = 0x03001F08u;
        // c1438 publishes the 128x128 canvas through this copy-and-fade
        // wrapper. c1470 clears it with the relocated ROM 0x080008D4 routine
        // (116 bytes).
        p.copy_fade[0] = 0x080054E4u;
        p.copy_halve[0] = 0x08005534u;
        // Func_cd260's other canvas modes (block [0x03001EEC], FACTS.md
        // 2026-10-02): mode 3 copies then subtracts the param from every
        // byte, floor 0 (Func_5490); mode 4 copies then adds it, capped at 63
        // (Func_543c); both take r0 canvas, r1 packed param, r2 the VRAM copy,
        // r3 the size. Mode 1 is the plain copy followed by a fill of the
        // canvas with the param (0x03000168, entered mid-routine). Meteor runs
        // mode 3 then mode 1 (gpu_rewind_0106/0109).
        p.copy_subtract[0] = 0x08005490u;
        p.copy_add[0] = 0x0800543Cu;
        p.canvas_block_slot = 0x03001EECu;
        p.plain_copy = 0x03001388u;
        p.plain_copy_source = 0x08001AF8u;
        p.clear_canvas = 0x03000164u;
        p.clear_source = 0x080008D4u;
        p.clear_bytes = 116;
        p.fill_canvas = 0x03000168u;
        p.fill_source = 0x080008D8u;
        p.fill_bytes = 112;
        p.header_template = 0x080EDCC4u;
        p.saturation_template = 0x080EDCB8u;
        p.flip_x_template = 0x080EDCD0u;
        p.flip_y_template = 0x080EDCDCu;
        p.flip_xy_template = 0x080EDCECu;
        p.no_flip_template = 0x080EDCF8u;
        p.max_template = 0x080EDBE8u;
        p.max_reverse_template = 0x080EDBF8u;
        p.add_template = 0x080EDC88u;
        p.add_reverse_template = 0x080EDCA0u;
        // Func_ed408 mode 1: copy each non-zero source byte (stamp variant
        // 13, flags 3 mode 1, measured 2026-09-23; session_20260923_124739).
        p.replace_template = 0x080EDB00u;
        p.replace_reverse_template = 0x080EDB10u;
        return p;
    }();
    return profile;
}

}  // namespace gsr
