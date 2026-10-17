// field_game_profile.h -- what the shared field scene and GPU field renderer
// need to know about the game they are drawing.
//
// engine/src/field/ is ONE copy of the field scene model and the GPU field
// renderer, compiled by both Golden Sun: The Broken Seal and The Lost Age.
// Everything that differs between the two games is passed in here, so the
// shared files hold no game address and no game name:
//
//   * addresses and sizes (where the game keeps its camera, room rectangle,
//     layer records, map number; how big its whole-world-map tile grid is);
//   * one flag per rule that exists in only one game today. A rule whose flag
//     is off is skipped, so each game keeps exactly the behaviour it has.
//
// Each game defines field_game_profile() once, in its own src/ (see
// "The Broken Seal/src/field_game_profile.cpp" and "The Lost Age/src/"). It is
// resolved at link time, so every target that links the shared renderer must
// also link that file. Rules that are inert unless the game supplies their
// input (bursts and shake, effect hide/span, Better Field Psy, margin scroll
// rows, margin_vram_height) carry no flag: nothing calls them in the other
// game.
#pragma once

#include <cstdint>

namespace gsr {

struct FieldGameProfile {
    // ---- EWRAM addresses (field state block, evidence in each game's
    // profile file and FACTS.md) ----
    std::uint32_t room_rect_addr = 0;      // min_x,max_x,min_y,max_y (16.16 max)
    std::uint32_t clamp_min_x_addr = 0;    // integer half of the clamp min x
    std::uint32_t clamp_min_y_addr = 0;    // integer half of the clamp min y
    std::uint32_t camera_addr = 0;         // x,y, 16.16 fixed point
    std::uint32_t layer_record_addr = 0;   // scroll record per field layer
    std::uint32_t layer_record_bytes = 0;  // stride of one layer record
    std::uint32_t id_grid_addr = 0;        // 128x128 u32 metatile ids
    std::uint32_t atlas_addr = 0;          // 8 bytes per metatile id
    std::uint32_t map_id_addr = 0;         // current map number (u16)

    // Widescreen object rules (the game's widescreen_policy.h): how many
    // columns the expanded view adds on each side, and the raw OAM X and Y of
    // the game's parked-dummy sprites.
    std::int32_t expanded_extra_x = 0;
    unsigned obj_park_raw_x = 0;
    unsigned obj_park_raw_y = 0;

    // Side of the whole world map, in tiles (WorldMapSource::kSideTiles).
    int world_map_side_tiles = 512;
    // The auto-scroll wrap-y word of a layer record is stored shifted left by
    // this many bits (The Lost Age stores the mask times the grid's 128
    // columns, so 7); 0 reads it raw.
    int layer_wrap_y_shift = 0;

    // Room gate for the scene model: when set and it returns false, the frame
    // is not drawable ("the room buffer has no reconstruction for this
    // room"). Null: no gate.
    bool (*room_source_ready)() = nullptr;

    // ---- rules that exist in one game only ----
    // Kolima's flooded copy of the area on some rows (row_split_vertical,
    // split_row_offset, u_split_*).
    bool kolima_split_rows = false;
    // Lamakan's heat haze: scroll rows mirrored into the margins and the
    // haze_above room lookup (u_haze_room, u_haze_mirror).
    bool lamakan_haze = false;
    // Lamakan's row-0 scroll swap repaired in draw().
    bool lamakan_row0_swap = false;
    // Altin's drain: rest position of each layer and the rest-position test
    // in the margins (rest_region_y_, u_rest_dy), on these map numbers only
    // (0 = unused slot; all 0 = off).
    int altin_rest_maps[2] = {0, 0};
    // Altin's line-striped layer enable bit carried into the margins
    // (u_layer_striped).
    bool altin_striped_layers = false;
    // Solid-colour object strips and view-bounded strip rows
    // (object_is_solid).
    bool solid_object_strips = false;
    // Margin scroll rows (SceneLayer::margin_scroll) need extra rows in the
    // per-row register texture.
    bool margin_scroll_rows = false;
    // room_layer_agreement wraps an auto-scrolling layer inside its region
    // (Madra's rays).
    bool agreement_autoscroll_wrap = false;
    // TLA's layer scroll plus its grid region can cross the 2048-pixel grid.
    // The room lookup wraps there (Gondowan Cliffs, F12 0057).
    bool room_grid_wrap = false;
};

// Defined by the game (link-time hook).
const FieldGameProfile& field_game_profile();

// The one parked-dummy test, for every renderer in both games. A dummy sits on
// the game's exact raw park position and is never affine: over every saved F12
// and rewind capture of both games (11,751 frames, 2026-10-16) the only affine
// object on (192,192) was Lost Age's real Pound pillar passing through it
// (tla_rewind_0032 frame 62), which the bare position test hid for that frame.
inline bool field_obj_is_parked(std::uint16_t attr0, std::uint16_t attr1) {
    const FieldGameProfile& p = field_game_profile();
    return (attr0 & 0x0100u) == 0 && (attr1 & 0x01FFu) == p.obj_park_raw_x &&
           (attr0 & 0x00FFu) == p.obj_park_raw_y;
}

}  // namespace gsr
