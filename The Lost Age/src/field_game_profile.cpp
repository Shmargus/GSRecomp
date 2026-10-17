// field_game_profile.cpp -- The Lost Age's values for the shared field scene
// and GPU field renderer (engine/src/field/field_game_profile.h).
#include "field_game_profile.h"

#include "effect_capture.h"
#include "tla.h"
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
        // The expanded view adds 60 columns per side (as Golden Sun 1).
        p.expanded_extra_x = 60;
        // Unused sprites are parked at raw OAM (192, 192), as in Golden Sun 1
        // (FACTS.md: OAM shadow 0x03005410).
        p.obj_park_raw_x = 192;
        p.obj_park_raw_y = 192;
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
        p.room_grid_wrap = true;
        return p;
    }();
    return profile;
}

const EffectCaptureProfile& effect_capture_profile() {
    static const EffectCaptureProfile profile = [] {
        EffectCaptureProfile p;
        // FACTS.md 2026-10-05. Golden Sun 1's effect machinery, re-found on
        // this ROM by Golden Sun 1's own bytes:
        //   * generated stamp routines: a Thumb stub, then GS1's stamp
        //     header, in IWRAM allocations under varying ids (F12 0039/0040:
        //     Ray at 26 and 47, Flare at 26, 20 also holding one), so they
        //     are known by their bytes, not by a slot;
        //   * the canvas copy/fade/halve/subtract/add wrappers run from ROM
        //     (0x08001C90..0x08001FDC), each with a pool naming its routine.
        //     Two sets: GS1's (0x08001F7C..) and an unrolled set (0x08001C90..)
        //     that battles use (same results);
        //   * every resident ARM routine (plain copy, clear, fill) and every
        //     generated stamp starts with a 4-byte Thumb stub (`bx pc`).
        // The canvas is not taken from an allocation slot: GS1's slot id read
        // 0 in every capture (a measured run: 0 stamps, 0 copies).
        // It is the 16 KB buffer the game copies to video memory instead.
        p.thumb_stub = 4;
        // The game copies ROM 0x080005B8.. (4 KB) to IWRAM.
        p.iwram_image_rom = 0x080005B8u;
        p.iwram_image_rom_end = 0x080016B8u;
        // Fade: routines 0x08001D10 / unrolled 0x08001A14; halve: 0x08001D70
        // / 0x08001AD4; subtract: 0x08001DD0 / 0x08001B54; add: 0x08001EA4 /
        // 0x08001C08.
        p.copy_fade[0] = 0x08001F7Cu;
        p.copy_fade[1] = 0x08001C90u;
        p.copy_halve[0] = 0x08001F9Cu;
        p.copy_halve[1] = 0x08001CB0u;
        p.copy_subtract[0] = 0x08001FDCu;
        p.copy_subtract[1] = 0x08001CF0u;
        p.copy_add[0] = 0x08001FBCu;
        p.copy_add[1] = 0x08001CD0u;
        p.canvas_block_slot = 0x0300009Cu;
        // GS1's block offsets, not re-measured here (mode 1 detection only).
        p.plain_copy = 0x03000734u;           // ROM 0x08000CEC, 120 bytes
        p.plain_copy_source = 0x08000CECu;
        p.clear_canvas = 0x0300025Cu;         // ROM 0x08000814: mov r2,#0,
        p.clear_source = 0x08000814u;         // a stub, then the fill
        p.clear_bytes = 120;
        p.fill_canvas = 0x03000264u;          // ROM 0x0800081C, 112 bytes
        p.fill_source = 0x0800081Cu;
        p.fill_bytes = 112;
        // The generator's templates are GS1's bytes at 0x0819706C..0x081972D8.
        p.header_template = 0x081972A4u;
        p.saturation_template = 0x08197224u;
        p.flip_x_template = 0x081972B0u;
        p.flip_y_template = 0x081972BCu;
        p.flip_xy_template = 0x081972CCu;
        p.no_flip_template = 0x081972D8u;
        p.max_template = 0x08197154u;
        p.max_reverse_template = 0x08197164u;
        p.add_template = 0x081971F4u;
        p.add_reverse_template = 0x0819720Cu;
        p.replace_template = 0x0819706Cu;
        p.replace_reverse_template = 0x0819707Cu;
        return p;
    }();
    return profile;
}

}  // namespace gsr
