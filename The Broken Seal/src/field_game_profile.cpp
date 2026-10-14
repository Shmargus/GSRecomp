// field_game_profile.cpp -- The Broken Seal's values for the shared field scene
// and GPU field renderer (engine/src/field/field_game_profile.h).
#include "field_game_profile.h"

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
        p.altin_rest_drift = true;
        p.altin_striped_layers = true;
        p.solid_object_strips = true;
        p.margin_scroll_rows = false;
        p.agreement_autoscroll_wrap = false;
        return p;
    }();
    return profile;
}

}  // namespace gsr
