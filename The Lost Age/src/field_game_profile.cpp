// field_game_profile.cpp -- The Lost Age's values for the shared field scene
// and GPU field renderer (engine/src/field/field_game_profile.h).
#include "field_game_profile.h"

#include "tla.h"
#include "widescreen_policy.h"
#include "world_map_source.h"

namespace gsr {

const FieldGameProfile& field_game_profile() {
    static const FieldGameProfile profile = [] {
        FieldGameProfile p;
        // The Lost Age (TLARecomp, FACTS.md 2026-10-05): the same field state
        // block as Golden Sun 1's, at [0x03000020] = 0x02030D50 instead of
        // [0x03001E70] = 0x02030CCC, with the same offsets inside it: camera
        // +0xE4, clamp min +0xEC/+0xF0, max +0xF4/+0xF8; layer records from
        // +0x108, 0x38 apart (GS1's 0x30 plus two pointers). The id grid and
        // atlas are where Golden Sun 1 has them, read the same way (150 of 150
        // on-screen cells on BG1-3 matched).
        p.room_rect_addr = 0x02030E44u;     // min_x,max_x,min_y,max_y
        p.clamp_min_x_addr = 0x02030E3Eu;
        p.clamp_min_y_addr = 0x02030E42u;
        p.camera_addr = 0x02030E34u;        // x,y, 16.16 fixed point
        p.layer_record_addr = 0x02030E58u;
        p.layer_record_bytes = 0x38u;
        p.id_grid_addr = 0x02010000u;
        p.atlas_addr = 0x02020000u;
        // kTlaMapIdAddr: 1 on the title screen, which Golden Sun 1's 0 never
        // was; the layer agreement check still guards it.
        p.map_id_addr = kTlaMapIdAddr;
        p.expanded_extra_x = widescreen::kExpandedExtraX;
        p.obj_park_raw_x = widescreen::kGoldenSunObjParkRawX;
        p.obj_park_raw_y = widescreen::kGoldenSunObjParkRawY;
        p.world_map_side_tiles = WorldMapSource::kSideTiles;
        // The Lost Age stores the y mask times the grid's 128 columns (0x3F80
        // for 127, Madra's rays 0x1F80 for 63: F12 0104/0105); read raw, the
        // rays never wrapped down and stopped at their region's edge.
        p.layer_wrap_y_shift = 7;
        // The room gate returns with the widened view, re-measured on this
        // ROM; until then video memory alone supplies the 240x160.
        p.room_source_ready = nullptr;

        p.margin_scroll_rows = true;
        p.agreement_autoscroll_wrap = true;
        return p;
    }();
    return profile;
}

}  // namespace gsr
