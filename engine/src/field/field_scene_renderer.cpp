// field_scene_renderer.cpp -- see field_scene_renderer.h.
//
// This file draws the field scene on the GPU. The shader below is the GBA's
// own background rule, written once and run on the graphics card instead of
// per pixel on the processor:
//
//   1. screen pixel, plus the layer's scroll
//   2. find the map entry for that tile
//   3. read the tile number, flips and palette bank
//   4. find the pixel inside the tile and read its colour index
//   5. look the colour up and emit it
//
// Colour index 0 is the console's transparency and is discarded. That is what
// lets the layers stack.
//
// Alpha blending and semi-transparent objects need to know what lies directly
// BENEATH the winning pixel, and whether that is itself a blend target.
// Painter's order throws that information away as soon as the next quad
// overwrites it. So every quad is drawn twice, into two scratch textures:
//
//   - The GPU's own depth test does the sorting. The smaller sort_key wins,
//     exactly as in the reference renderer.
//   - Pass 1 records the frontmost candidate ("top").
//   - Pass 2 records the runner-up ("second"). It discards anything that is
//     not better than what "top" already recorded at that pixel.
//
// A final full-screen pass then applies the console's compose rule: alpha
// blend, brightness, or the top colour unchanged. It reads only those two
// records (`gba_ppu.cpp`, `render_scanline_internal`, the code right after
// the object loop).
//
// Each record holds the RAW, unfaded, unblended 15-bit colour, the sort key
// and a semi-transparent flag, packed into an RGBA8 texture. That keeps the
// two write passes simple colour output, and the resolve pass does all of
// the arithmetic in one place, matching the reference exactly.
#include "field_scene_renderer.h"
#include "field_game_profile.h"
#include "gpu_timing.h"
#include "world_map_agreement.h"
#include "world_map_shader.h"


#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

// The battle arena's side-margin rule, shared with the emulated path so both
// renderers extend the same layers over the same rows.
#include "battle_view.h"

namespace gsr {
namespace {

// Video memory is uploaded as a single-byte texture this wide. 96 KB divides
// exactly, and the shader turns an address into a coordinate with one divide.
constexpr int kVramTextureWidth = 1024;
// The console's 96 KB plus the optional second sprite page (gba_bus.h).
constexpr int kVramBytes = 128 * 1024;
constexpr int kPaletteEntries = 512;

// Room buffer addresses: each game's measured field state block, passed in by
// its FieldGameProfile (field_game_profile.h; the evidence stays beside the
// values in the game's own profile file).
// How big our own effect canvas is. Wider and taller than the console's, so a
// widened view has room for the sparks the game would have clipped; the origin
// is where console (0,0) sits inside it.
constexpr int kEffectCanvasWidth = 384;
constexpr int kEffectCanvasHeight = 256;

constexpr std::uint32_t kEwramBase = 0x02000000u;
// The profile's addresses (field_game_profile.h). The current map number is 0
// until the game has loaded its first room and at the mode changes between
// rooms, which is the one thing the room rect below cannot tell us -- see
// room_rect_is_room's comment.
inline const FieldGameProfile& prof() { return field_game_profile(); }
constexpr int kGridSide = 128;
constexpr int kAtlasIds = 4096;
constexpr std::size_t kEwramBytes = 256 * 1024;
// widescreen_policy.h's measured "not a real tile" sentinels: a raw 0xffff,
// or a raw 0xF200 atlas entry on any metatile id. 0xF200 is the un-authored
// fill; seen at id 0x017 and at id 0x000 (Bilibin). Applied here too so a
// room the room buffer cannot really answer for does not draw a yellow/red
// checker pattern.
constexpr std::uint16_t kRoomUnavailableTile = 0xFFFFu;
constexpr std::uint16_t kRoomNoMapTile = 0xF200u;

std::uint16_t ewram_u16(const std::uint8_t* ewram, std::size_t bytes,
                        std::uint32_t address) {
    const std::uint32_t off = (address - kEwramBase) & 0x3FFFFu;
    if (static_cast<std::size_t>(off) + 1 >= bytes) return 0;
    return static_cast<std::uint16_t>(ewram[off] |
                                      (static_cast<unsigned>(ewram[off + 1]) << 8));
}

// Same "is this a real room" rule as the archived room_buffer.cpp (Archive/2026-11-27-retire-direct3d)
// rect_is_room: reject
// the all-zero between-rooms marker (falls out of max_x<=min_x below) and the
// world map's constant (0,1,8,256); accept any rect with real 16px extent.
//
// This rule tells one room from another. It CANNOT tell a real room from
// memory that no room has been written to yet, because a rect of
// (0,512,0,512) passes every test here.
//
// That is what EWRAM holds on the name-entry screen, 24 seconds into a new
// game (measured at frame 1451). The room source then answered with whatever
// was in the tables and drew yellow stripes over a screen the hardware draws
// black. That was 10,038 of 38,400 pixels, INSIDE the authentic 240x160, not
// only in the margins.
//
// The map number (profile map_id_addr) is the missing discriminator.
// upload_room checks it.
bool room_rect_is_room(std::uint16_t min_x, std::uint16_t max_x,
                       std::uint16_t min_y, std::uint16_t max_y) {
    if (max_x == 1 && min_y == 8 && max_y == 256) return false;
    if (max_x <= min_x || max_y <= min_y) return false;
    return (max_x - min_x) >= 16 && (max_y - min_y) >= 16;
}

const char* kBackgroundVertex =
    "#version 130\n"
    "in vec2 a_pos;\n"
    "in vec2 a_uv;\n"
    "in float a_depth;\n"
    "in vec4 a_tint;\n"
    "uniform mat4 u_transform;\n"
    "out vec2 v_screen;\n"
    "void main() {\n"
    // a_uv carries the output pixel this vertex sits at, so the fragment
    // shader works in screen pixels rather than 0..1.
    "  v_screen = a_uv;\n"
    "  gl_Position = u_transform * vec4(a_pos, a_depth, 1.0);\n"
    "}\n";

// Both record shaders below write a PACKED CANDIDATE, not a finished colour:
//   - red|green, as a 16-bit pair, is the raw palette colour exactly as the
//     console stored it, unfaded and unblended.
//   - blue plus the low two bits of alpha is the console's own sort key:
//     priority*256 + 128+layer for a background, priority*256 + slot for an
//     object.
//   - bit 4 of alpha marks a semi-transparent object.
//
// The resolve pass (see kResolveFragment) is the only place that turns this
// into a colour. The reference compositor likewise does it in one place
// rather than per layer.
//
// `u_compare_top` selects which of the two peel passes this draw is:
//   - 0 for the first pass, with nothing to compare against yet.
//   - 1 for the second pass. A fragment discards itself unless its own key
//     beats whatever the first pass already recorded at this pixel. That is
//     what makes the second draw the runner-up rather than a duplicate of
//     the first.
const char* kBackgroundFragment =
    "#version 130\n"
    "in vec2 v_screen;\n"
    "uniform usampler2D u_vram;\n"      // raw video memory, one byte per texel
    "uniform usampler2D u_palette;\n"   // 512 fifteen-bit colours
    "uniform sampler2D u_window;\n"     // this pixel's 6-bit window control
    "uniform usampler2D u_id_grid;\n"   // room buffer: 128x128 metatile ids
    "uniform usampler2D u_atlas;\n"     // room buffer: 4 tile entries per id
    "uniform usampler2D u_row_io;\n"    // this frame's per-row registers
    "uniform int u_row_io_max;\n"       // last valid row index in u_row_io
    "uniform usampler2D u_hold_strip;\n"// precomputed hold_edge strip shares (2 x H)
    "uniform int u_hold_strip_max;\n"   // last valid row index in u_hold_strip
    "#ifndef GSR_BG_REGULAR_ONLY\n"
    // Each row's own affine reference point, the console's internal
    // accumulator rather than the register file: eight 16-bit halves per row,
    // BG2 x, BG2 y, BG3 x, BG3 y, low half then high half, each biased by
    // 2^29 so the stored number is never negative (see upload_row_affine).
    "uniform usampler2D u_row_affine;\n"
    "uniform int u_affine;\n"           // 1: this layer is an affine background
    "uniform int u_affine_step_valid;\n"
    "uniform vec4 u_affine_step_top;\n"
    "uniform vec4 u_affine_step_bottom;\n"
    "uniform vec2 u_effect_span;\n"
    "uniform int u_effect_wrap_canvas;\n"
    "uniform int u_effect_fill_beyond;\n"
    "uniform int u_effect_repeats;\n"
    "#endif\n"
    "uniform sampler2D u_top_record;\n" // pass 1's result; read only in pass 2
    "uniform int u_row_mode_filter;\n"  // 1: keep only rows whose mode agrees
    "uniform int u_palette_half;\n"     // 0 for backgrounds, 256 for objects
    "uniform vec2 u_camera;\n"          // world offset of the output's origin
    "uniform int u_world_loaded;\n"
    "uniform int u_layer;\n"            // 0-3: this background's window bit
    "uniform int u_window_on;\n"        // 1: window masks are active this frame
    "uniform int u_layer_striped;\n"    // 1: enable bit varies line by line
    "uniform int u_rest_dy;\n"          // rows from its current region to the rest position
    "uniform int u_compare_top;\n"      // 0 = first pass, 1 = second pass
    "uniform int u_room_auth;\n"        // 0: never, 1: check per-row, 2: all rows
    "uniform int u_room_grid_wrap;\n"
    // The layer's own position from the game's record (room_reference_for):
    // when on, each row's scroll is read as that position plus its offset
    // from it in -512..511.
    "uniform int u_room_ref_on;\n"
    "uniform int u_haze_room;\n"
    // Lamakan's haze mirrored into the margins (FieldGameProfile::lamakan_haze).
    "uniform int u_haze_mirror;\n"
    // Game-supplied scroll rows for the vertical margins
    // (SceneMarginLayer::scroll); zero rows disables it.
    "uniform int u_margin_scroll_first_y;\n"
    "uniform int u_margin_scroll_rows;\n"
    "uniform int u_margin_scroll_sides;\n"
    "uniform vec2 u_room_ref;\n"
    // Rows that jump vertically off the recorded position read it instead
    // (row_split_vertical).
    "uniform int u_split_v;\n"
    // Where the other state of a split layer lies, from its own position
    // (split_row_offset); u_split_found 0 = not found, read the record.
    "uniform int u_split_found;\n"
    "uniform vec2 u_split_offset;\n"
    // Auto-scrolling layer: region x,y and wrap width,height in pixels
    // (0 = no wrap); see upload_room's layer records.
    "uniform vec4 u_room_wrap;\n"
    // Layer moving at its own speed ratio whose art ends above the console's
    // top: room source rows above u_hold_floor repeat that row (upload_room).
    "uniform int u_hold_top;\n"
    "uniform int u_hold_floor;\n"
    // 0 never widens, 1 always widens, 2 widens under the battle arena rule
    "uniform int u_margin_mode;\n"
    "uniform vec2 u_room_min;\n"        // room rect, pixels (min_x, min_y)
    "uniform vec2 u_room_max;\n"        // room rect, pixels (max_x, max_y)
    "uniform vec2 u_ewram_camera;\n"    // the game's own camera, room pixels
    // The camera's keep-out rectangles (x0, y0, x1, y1), outside the room;
    // the array size is FieldSceneRenderer::kKeepOutMax.
    "uniform int u_keep_out_count;\n"
    "uniform vec4 u_keep_out[2];\n"
    // Our own spell-effect canvas, drawn host-side from the game's particles
    // because the game clips every spark to a canvas as wide as the console
    // screen, and that limit cannot be widened in place (effect_sources.h).
    "uniform usampler2D u_effect;\n"
    "uniform int u_effect_extra;\n"   // our sparks fill where the game drew nothing
    "uniform vec2 u_effect_size;\n"
    "uniform vec2 u_effect_origin;\n"
    // Our canvas's hold columns: past them, out in the margins, the canvas is
    // read at them (a cut band continues; anything else holds nothing).
    "uniform vec2 u_effect_hold;\n"
    "uniform int u_effect_fill;\n"
    // 1 when this effect layer is a REGULAR background whose map shows the
    // game's 128x128 canvas as one 16x16 block of consecutive tiles, whose
    // top-left map pixel is u_effect_canvas_map (Ivan's hit sparks on BG1).
    "uniform int u_effect_text_canvas;\n"
    // Hide the game's own effect canvas on this layer (set_effect_hidden):
    // 1 = text layer, the 128x128 block at u_effect_canvas_map draws
    // transparent; 2 = wrapping affine layer, canvas at the map origin.
    "uniform int u_effect_hide;\n"
    // Regular effects exposing their live tilemap in the margins
    // (SceneMarginLayer::vram_height); zero disables it.
    "uniform int u_margin_vram_height;\n"
    "uniform int u_margin_vram_clamp;\n"
    "uniform vec2 u_effect_canvas_map;\n"
    "out vec4 o_colour;\n"
    "\n"
    "uint vram_byte(int address) {\n"
    "  if (uint(address) >= 131072u) return 0u;\n"
    "  return gsr_raw_fetch(u_vram, ivec2(address & 1023, address >> 10), 0).r;\n"
    "}\n"
    "\n"
    "uint vram_half(int address) {\n"
    "  return vram_byte(address) | (vram_byte(address + 1) << 8);\n"
    "}\n"
    "\n"
    // Room tiles outside the console's screen read their pixels from this
    // copy: a menu borrows BG0's character block for its font and window
    // pieces, which the room's own layers share, and on hardware the menu
    // covers the whole screen so the borrowed tiles never show under the
    // room. In the margins they did, as scraps of menu text
    // (seen in the file menu).
    "uniform usampler2D u_room_vram;\n"
    "uniform vec2 u_margin_repeat;\n"
    "uniform vec4 u_art_repeat;\n"   // period down, its shift, period across
    "uniform int u_past_room_edge;\n"
    "#ifndef GSR_BG_REGULAR_ONLY\n"
    // Golden Sun's whole world map as tile indices (world_map_source.h):
    // 512 tiles wide, BG3 in rows 0..511 and BG2 in rows 512..1023. The
    // game's own map is a 512-pixel ring refilled around the camera, and
    // the widened zoom-out reaches past it.
    GBARECOMP_WORLD_MAP_UNIFORMS
    "#endif\n"
    "uint room_vram_byte(int address) {\n"
    "  if (uint(address) >= 131072u) return 0u;\n"
    "  return gsr_raw_fetch(u_room_vram, ivec2(address & 1023, address >> 10), 0).r;\n"
    "}\n"
    "\n"
    // The room buffer's colour index at a room pixel, 0 where it has none:
    // the margin path's lookup in main, for the rest-position test
    // (u_rest_dy).
    "int room_index_at(ivec2 source, int char_base, int eight_bit) {\n"
    "  if (source.x < 0 || source.y < 0) return 0;\n"
    "  int stx = source.x >> 3;\n"
    "  int sty = source.y >> 3;\n"
    "  if ((stx >> 1) >= 128 || (sty >> 1) >= 128) return 0;\n"
    "  uint id = gsr_raw_fetch(u_id_grid, ivec2(stx >> 1, sty >> 1), 0).r;\n"
    "  uint e = gsr_raw_fetch(u_atlas, ivec2((sty & 1) * 2 + (stx & 1), int(id & 4095u)),\n"
    "                         0).r;\n"
    "  if (e == 0xFFFFu || e == 0xF200u) return 0;\n"
    "  int tx = source.x & 7;\n"
    "  int ty = source.y & 7;\n"
    "  if ((e & 0x400u) != 0u) tx = 7 - tx;\n"
    "  if ((e & 0x800u) != 0u) ty = 7 - ty;\n"
    "  int tile = int(e & 0x3FFu);\n"
    "  if (eight_bit != 0)\n"
    "    return int(room_vram_byte(char_base + tile * 64 + ty * 8 + tx));\n"
    "  uint two = room_vram_byte(char_base + tile * 32 + ty * 4 + (tx >> 1));\n"
    "  return int(((tx & 1) != 0) ? (two >> 4) : (two & 0xFu));\n"
    "}\n"
    "\n"
    // This layer's own HOFS/VOFS, as THIS row's copy of the registers held
    // them -- the same per-scanline read the console does, which is what
    // lets a frame that rewrites scroll mid-frame draw correctly.
    "uint row_u16(int row, int offset) {\n"
    "  return gsr_raw_fetch(u_row_io, ivec2(offset >> 1, clamp(row, 0, u_row_io_max)), 0).r;\n"
    "}\n"
    "\n"
    "#ifndef GSR_BG_REGULAR_ONLY\n"
    // The same read, as the signed 16-bit number the hardware stores: the
    // affine parameters PA/PB/PC/PD are signed Q8.8 (gba_ppu.cpp, read_s16).
    "int row_s16(int row, int offset) {\n"
    "  int v = int(row_u16(row, offset));\n"
    "  return (v >= 32768) ? v - 65536 : v;\n"
    "}\n"
    "\n"
    // This row's own affine reference point, `which` being 0 BG2 x, 1 BG2 y,
    // 2 BG3 x, 3 BG3 y. Stored biased rather than as a signed value because
    // there is no signed integer texture format here, and reinterpreting a
    // uint's top bit as a sign is not something GLSL 1.30 guarantees.
    "int row_affine_ref(int row, int which) {\n"
    "  int r = clamp(row, 0, 159);\n"
    "  int lo = int(gsr_raw_fetch(u_row_affine, ivec2(which * 2, r), 0).r);\n"
    "  int hi = int(gsr_raw_fetch(u_row_affine, ivec2(which * 2 + 1, r), 0).r);\n"
    "  return (hi << 16) + lo - 536870912;\n"
    "}\n"
    "#endif\n"
    "\n"
    // A battle window's rows y1..y2 (WINxV) as the arena band: they start on
    // the console's screen and end in its lower half. True when row `y` is in
    // that band.
    "bool in_arena_band(int y1, int y2, int y) {\n"
    "  return y1 < 160 && y1 < y2 && y2 >= 80 && y2 <= 160 && y >= y1 && y < y2;\n"
    "}\n"
    "\n"
    // A signed scroll with no place in the shared 2048-pixel map: the whole
    // 240x160 screen at that scroll lands outside it. Tret's sky is a running
    // total the console masks to 9 bits, so it eventually passes near zero;
    // at -298 a ">= 2048 from zero" test took it for a room position and
    // blacked the side margins of the sky band.
    "bool scroll_has_no_place(int hofs_s, int vofs_s) {\n"
    "  return hofs_s + 240 <= 0 || hofs_s >= 2048 ||\n"
    "         vofs_s + 160 <= 0 || vofs_s >= 2048;\n"
    "}\n"
    "\n"
    "void main() {\n"
    "  ivec2 console_px = ivec2(floor(v_screen + u_camera));\n"
    // Rows outside the console's own 0..159 have no real per-row register
    // state (there is no scanline there); the nearest real row's own
    // registers extend into that margin, as a frame-level value would
    // do for the whole picture.
    "  int reg_row = clamp(console_px.y, 0, 159);\n"
    // This layer's own control register, as THIS row's copy of it held it,
    // never the frame's. Golden Sun rewrites BGxCNT mid-frame: in a
    // measured battle frame, BG2CNT changes at row 26 (tile
    // block 2 -> 3) and again at row 136 (map block 7 -> 31, size 1 -> 0),
    // and BG1CNT changes priority 3 -> 0 at row 136. A frame-level read is
    // only ever the LAST row's value, which would draw every row above from
    // the wrong tile and map blocks. The
    // reference reads the same register the same way: render_captured_scene
    // hands each row its own register file and render_scanline_internal
    // takes BGxCNT out of it (gba_ppu.cpp).
    "  uint row_dispcnt = row_u16(reg_row, 0x00);\n"
    "  uint bgcnt = row_u16(reg_row, 0x08 + u_layer * 2);\n"
    // The frame-level room flag only says that this scene was a room
    // candidate. A transition can retain the room registers while the
    // current row is already another Mode 0 layout, so authorize the room
    // source from the row's own DISPCNT and field screen-base signature.
    // The screen bases alone are not a field: BGxCNT keeps its value while
    // the layer is switched off, and the title screen shows its logo on BG2
    // (BG2CNT 0x0681, map 6) with BG1 and BG3 off but still set from the
    // field, so the room tables replaced the logo with a pattern behind the
    // file menu (the reference draws the logo).
    // Every measured field row has BG2 and BG3 on (DISPCNT 0x0F00 in 222
    // signature rows); the rest were the title and BG0-only fades.
    "  bool room_row_authorized = u_room_auth == 2 || (u_room_auth == 1 &&\n"
    "      (row_dispcnt & 7u) == 0u &&\n"
    "      (row_dispcnt & 0x0C00u) == 0x0C00u &&\n"
    "      (((row_u16(reg_row, 0x0E) >> 8) & 0x1Fu) == 5u) &&\n"
    "      (((row_u16(reg_row, 0x0C) >> 8) & 0x1Fu) == 6u) &&\n"
    "      (((row_u16(reg_row, 0x0A) >> 8) & 0x1Fu) == 7u));\n"
    "  int bg_char_base = int((bgcnt >> 2) & 3u) * 16384;\n"
    "  int bg_screen_base = int((bgcnt >> 8) & 31u) * 2048;\n"
    "  int bg_size_code = int((bgcnt >> 14) & 3u);\n"
    // An affine background is 8-bit whatever bit 7 says: gba_ppu.cpp's
    // render_affine_bg reads one byte per pixel unconditionally.
    "#ifdef GSR_BG_REGULAR_ONLY\n"
    "  int bg_eight_bit = ((bgcnt & 128u) != 0u) ? 1 : 0;\n"
    "#else\n"
    "  int bg_eight_bit = (u_affine != 0 || (bgcnt & 128u) != 0u) ? 1 : 0;\n"
    "  int bg_wraps = ((bgcnt & 8192u) != 0u) ? 1 : 0;\n"
    "#endif\n"
    // The sort key:
    // priority first, then background index, smaller wins (GL_LESS).
    "  int bg_key = int(bgcnt & 3u) * 256 + 128 + u_layer;\n"
    // ...and this row's own DISPCNT enable bit. Striped layers read (y mod 160);
    // plain layers reuse row_dispcnt already fetched above.
    "  if (u_layer_striped != 0) {\n"
    "    int enable_row = reg_row;\n"
    "    if (console_px.y < 0 || console_px.y > 159)\n"
    "      enable_row = ((console_px.y % 160) + 160) % 160;\n"
    "    if (((row_u16(enable_row, 0x00) >> uint(8 + u_layer)) & 1u) == 0u)\n"
    "      discard;\n"
    "  } else if (((row_dispcnt >> uint(8 + u_layer)) & 1u) == 0u) {\n"
    "    discard;\n"
    "  }\n"
    // Pass 2: a layer at or in front of pass 1's winner here is dropped
    // immediately before any tile work (no duplicate check at the bottom).
    "  if (u_compare_top != 0) {\n"
    "    vec4 early_top = gsr_raw_fetch(u_top_record, ivec2(gl_FragCoord.xy), 0);\n"
    "    int early_key = int(round(early_top.b * 255.0))\n"
    "                  | ((int(round(early_top.a * 255.0)) & 3) << 8);\n"
    "    if (bg_key <= early_key) discard;\n"
    "  }\n"
    // A frame that changes video mode part-way down (The Lost Age's
    // world-map wipe: a mode 2 band between mode 0 rows) draws such a layer
    // once per reading, regular and affine (draw_background); each row keeps
    // the draw that matches its own mode, and none where the mode has no
    // such layer, as render_scanline_internal decides per row.
    "  if (u_row_mode_filter != 0) {\n"
    "    uint row_mode = row_dispcnt & 7u;\n"
    "    bool row_has_layer = row_mode == 0u ||\n"
    "        (row_mode == 1u && u_layer <= 2) || (row_mode == 2u && u_layer >= 2);\n"
    "    bool row_affine = (row_mode == 1u && u_layer == 2) ||\n"
    "        (row_mode == 2u && u_layer >= 2);\n"
    "#ifdef GSR_BG_REGULAR_ONLY\n"
    "    if (!row_has_layer || row_affine) discard;\n"
    "#else\n"
    "    if (!row_has_layer || row_affine != (u_affine != 0)) discard;\n"
    "#endif\n"
    "  }\n"
    // BG3's field sky can start its scrolling band on row 1: row 0 still has
    // the room-position scroll, while rows 1 onward carry the sky accumulator.
    // Continue that measured pattern through the expanded top and border row
    // 0 when row 1's scroll is beyond the room map, so the added sky does not
    // end in a one-scanline cutoff. Only HOFS/VOFS follow row 1; BG3 control
    // and window state still come from reg_row.
    "  int scroll_row = reg_row;\n"
    "  bool continue_bg3_sky = false;\n"
    "  if (console_px.y <= 0 && u_layer == 3 && room_row_authorized) {\n"
    "    int first_hofs = int(row_u16(1, 0x1C));\n"
    "    int first_vofs = int(row_u16(1, 0x1E));\n"
    "    if (first_hofs >= 32768) first_hofs -= 65536;\n"
    "    if (first_vofs >= 32768) first_vofs -= 65536;\n"
    "    if (scroll_has_no_place(first_hofs, first_vofs)) {\n"
    "      scroll_row = 1;\n"
    "      continue_bg3_sky = true;\n"
    "    }\n"
    "  }\n"
    // Lamakan's heat haze is each row's small offset from the layer's
  // record; rows above and below the console would hold row 0's, so the
  // shimmer would stop at the screen edge. A layer read
  // through its record takes the offset of the console row mirrored across
  // the edge (row 0 itself is the desert's odd one, so the top starts at 1).
    "  if (u_haze_mirror != 0 && u_room_ref_on != 0 && !continue_bg3_sky &&\n"
    "      console_px.y < 0)\n"
    "    scroll_row = min(-console_px.y, 159);\n"
    "  if (u_haze_mirror != 0 && u_room_ref_on != 0 && !continue_bg3_sky &&\n"
    "      console_px.y > 159)\n"
    "    scroll_row = max(318 - console_px.y, 0);\n"
    // Game-supplied scroll rows for vertical margins and, when requested,
    // side margins. Native pixels keep captured registers. Rows hold HOFS,
    // VOFS after the 160.
    "  if ((console_px.y < 0 || console_px.y >= 160 ||\n"
    "       (u_margin_scroll_sides != 0 && (console_px.x < 0 || console_px.x >= 240))) &&\n"
    "      console_px.y >= u_margin_scroll_first_y &&\n"
    "      console_px.y < u_margin_scroll_first_y + u_margin_scroll_rows)\n"
    "    scroll_row = 160 + console_px.y - u_margin_scroll_first_y;\n"
    "  uint hofs_raw = row_u16(scroll_row, 0x10 + u_layer * 4);\n"
    "  uint vofs_raw = row_u16(scroll_row, 0x10 + u_layer * 4 + 2);\n"
    // The accumulator's high bits name no room position; its low nine bits
    // are the actual BG3 sample coordinate. Use those for the room-backed
    // top extension so it keeps the sky texture while moving with the band.
    "  if (continue_bg3_sky) { hofs_raw &= 0x1FFu; vofs_raw &= 0x1FFu; }\n"
    "  bool outside_native = console_px.x < 0 || console_px.x >= 240 ||\n"
    "                        console_px.y < 0 || console_px.y >= 160;\n"
    // True for rows above the console's first row or below its last.
    // The arena widens SIDEWAYS only. battle_view.h states that model:
    // "enabled arena layers continue into the left and right margins only".
    // Extending it up and down does not fill the black bands. In a measured
    // battle frame the backdrop draws:
    //   - black on 36 of the 40 rows above the arena, and
    //   - on the other 4, a stripe of its own artwork from a place the window
    //     never shows (a yellow line across the top of the battle).
    "  bool outside_native_v = console_px.y < 0 || console_px.y >= 160;\n"
    // May this layer continue into the left and right margins here?
    //
    //   u_margin_mode 1 -- always. Outside battle, a rotated or scaled
    //     background is the overworld's own map. Its transform is defined for
    //     every column, so widening it simply shows more world.
    //   u_margin_mode 2 -- this is BG1 or BG2 and the frame may be a battle.
    //     The arena continues sideways only:
    //       - down the rows of the game's own live scene band, and
    //       - only while this layer carries the arena rather than a spell
    //         effect.
    //
    // Mode 2 is the rule the emulated path already uses (battle_view.h and
    // runner_main.cpp's golden_sun_battle_bg_sample_provider). It is
    // evaluated HERE, from this row's own registers, for the same reason the
    // affine walk above is. The emulated path asks its margin policy once per
    // scanline, and a battle rewrites WIN0V and BLDCNT during the frame.
    // Deciding it once from the end-of-frame register file answers for the
    // last row and blacks out the margins on all the others.
    "  bool arena_margin = false;\n"
    // Hold the console's boundary column for battle effects, which otherwise
    // repeat their map into the margins. Accumulator backgrounds use their
    // normal wrapped tilemap fetch outside the screen: holding a pattern at
    // the edge freezes it into a smear instead of letting it scroll.
    "  bool hold_edge = false;\n"
    "  if (u_margin_mode == 1) {\n"
    "    arena_margin = u_world_loaded != 0;\n"
    // The band test runs on the NEAREST real scanline, not only on the
    // console's own 160 rows. A row above 0 or below 159 has no scanline of
    // its own, so it asks row 0 or row 159. `reg_row` already makes the same
    // extension for the registers.
    //
    // Without this, the arena stopped dead at the console's top and bottom
    // edges. The widened view then showed a black band above and below it,
    // which is what the player sees while a battle menu is open.
    "  } else if (u_margin_mode == 2) {\n"
    "    int band_y = clamp(console_px.y, 0, 159);\n"
    "    uint rdisp = row_u16(reg_row, 0x00);\n"
    "    uint win0v = row_u16(reg_row, 0x44);\n"
    "    int y1 = int((win0v >> 8) & 0xFFu);\n"
    "    int y2 = int(win0v & 0xFFu);\n"
    // Without WIN0 the arena starts at row 0. The camera push-in disables
    // the window (DISPCNT 0x1741, WIN0V still 0x1088). Include rows 0-15 in
    // the margin as on the GBA.
    "    if ((rdisp & 0x2000u) == 0u) y1 = 0;\n"
    // Above the GBA screen, extend the first row of the arena. WIN0V starts
    // at y1 = 0 and moves to 16 on the first turn. Sampling row 0 then gives
    // a black band. Below the screen, clamp to row 159 because the command
    // menu is there.
    "    if (console_px.y < 0) band_y = y1;\n"
    "    bool in_band = in_arena_band(y1, y2, band_y);\n"
    // Window 1 can carry the arena past window 0's band. The Lost Age's
    // Trident narrows WIN0V to 16..124 while window 1 (16..136, WININ's high
    // byte showing BG0..BG2) keeps the console's arena to row 136; the
    // margins would stop 12 rows short.
    "    uint win1v = row_u16(reg_row, 0x46);\n"
    "    int w1y1 = int((win1v >> 8) & 0xFFu);\n"
    "    int w1y2 = int(win1v & 0xFFu);\n"
    "    if (console_px.y < 0 && !in_band) band_y = w1y1;\n"
    "    if ((rdisp & 0x4000u) != 0u &&\n"
    "        ((row_u16(reg_row, 0x48) >> (8u + uint(u_layer))) & 1u) != 0u &&\n"
    "        in_arena_band(w1y1, w1y2, band_y))\n"
    "      in_band = true;\n"
    "    if ((rdisp & 7u) == 1u &&\n"
    "        (rdisp & 0x0600u) != 0u && in_band) {\n"
    "      uint bgcnt = row_u16(reg_row, 0x08 + u_layer * 2);\n"
    // A spell effect is the first target of an alpha blend, sits at priority
    // 1 and is drawn where the battlers are. Two approaches were tried and
    // both were wrong:
    //
    //   * Excluding it from the margins. The effect stopped dead at the
    //     console's old screen edge, leaving a hard vertical seam down a
    //     widened battle (seen on a Venus summon).
    //   * Widening it like the arena. It then repeated at the layer's map
    //     period and wrapped into the margins from the wrong edges, which is
    //     what the exclusion had been written to prevent.
    //
    // So the effect reaches the margin through the map's own unseen strip,
    // shared between the two margins, and through our captured sparks (see
    // below). Nothing out there is invented, nothing repeats and no edge
    // column is held, because holding one stretches particles into streaks.
    "      uint bldcnt = row_u16(reg_row, 0x50);\n"
    // An effect layer is one at priority 1, or one the CPU has found showing
    // the spell canvas. The second case matters because The Lost Age's
    // critical-hit sparkles are on BG1 at priority 0 (BG1CNT 0x1F80), and
    // their 256-wide map would repeat them into both margins.
    "      bool effect = ((bldcnt >> 6) & 3u) == 1u &&\n"
    "                    (bldcnt & (1u << uint(u_layer))) != 0u &&\n"
    "                    ((bgcnt & 3u) == 1u || u_effect_text_canvas != 0);\n"
    "#ifdef GSR_BG_REGULAR_ONLY\n"
    "      bool size_ok = (u_layer != 2) || (((bgcnt >> 14) & 3u) == 1u);\n"
    "      arena_margin = size_ok && !outside_native_v;\n"
    "#else\n"
    "      bool self_limiting = u_affine != 0 && (bgcnt & 0x2000u) == 0u;\n"
    "      bool size_ok = (u_layer != 2) || (((bgcnt >> 14) & 3u) == 1u)\n"
    "                     || self_limiting;\n"
    "      arena_margin = size_ok && (!outside_native_v ||\n"
    "          (console_px.y < 0 && y1 == 0 && u_affine != 0));\n"
    "#endif\n"
    "      hold_edge = effect;\n"
    "    }\n"
    "  }\n"
    "  bool room_supplied = false;\n"
    // Set together with room_copy below: this pixel also takes its colour
    // from the saved field palette (palette row 1, upload_room_palette).
    "  bool room_colours = false;\n"
    // A world-map cell drawn with another terrain set's artwork takes that
    // set's colours (palette row 2 + set, upload_world_cell_art).
    "  int world_colour_row = 0;\n"
    // Whether this pixel lies inside the room at all. This is kept separate
    // from whether the room buffer had an answer for it. A margin pixel past
    // the room's own edge is outside the world, and the only correct thing
    // out there is black (see the discard below).
    "  bool room_covers = false;\n"
    "  ivec2 room_source = ivec2(-1);\n"
    // True when the game's own canvas has no pixel for this position: the
    // affine sample fell outside its map. That is precisely where a spark
    // it clipped belongs, and the only place we add one.
    "  bool beyond_game_canvas = false;\n"
    "  ivec2 effect_px = ivec2(0);\n"
    // A scroll larger than the whole shared map is not a position in it.
    // The grid is 128 metatiles of 16 pixels, so 2048 is the entire world on
    // that axis. Tret's sky sits at 33503, sixteen times that, because the
    // game scrolls it by adding to a running total and lets the console mask
    // it to 9 bits.
    //
    // Such a layer is a repeating pattern with no place in the room. The
    // console's own wrapped tilemap is the correct answer for it in the
    // margins. That is already proven, because it is exactly what draws the
    // layer INSIDE the window on frames that are pixel-identical to the
    // hardware.
    //
    // This test is deliberately narrower than something like "the room could
    // not address this pixel". That test also catches ordinary room layers
    // and wraps a cave room's own scenery into its margins. Measured over 865
    // mode 0 field frames, NO layer scrolls past the grid, so this fires on
    // none of them.
    //
    // The scroll register is 16 bits, and the game also writes NEGATIVE
    // scrolls into it. Golden Sun nudges a battle backdrop left by five
    // pixels by storing 65531. Read as an unsigned 65531, that would look
    // like a scroll sixteen times past the whole grid. The Tret rule below
    // would then hold the boundary column and smear the arena across both
    // margins (seen on Granite: BG1 scroll x=65531, i.e. -5).
    //
    // MAGNITUDE separates the two cases, not sign. Tret's sky accumulator
    // reads 33503/33651/33448, which as a signed 16-bit value is about
    // -32000. That is still enormous and still has no place in the map, so
    // the Tret rule must keep firing on it. A -5 nudge is a position. So
    // resolve the sign first, then compare the distance from zero against the
    // whole grid.
    "  int hofs_s = int(hofs_raw); if (hofs_s >= 32768) hofs_s -= 65536;\n"
    "  int vofs_s = int(vofs_raw); if (vofs_s >= 32768) vofs_s -= 65536;\n"
    // Game-authorized regular effects use their live maps in the margins.
    // Masking a negative source x to the map width repeated Air's Rock's
    // tornado at the opposite edge (the first continuation preview).
    "#ifdef GSR_BG_REGULAR_ONLY\n"
    "  bool vram_margin = outside_native && u_margin_vram_height != 0;\n"
    "#else\n"
    "  bool vram_margin = outside_native && u_affine == 0 &&\n"
    "                     u_margin_vram_height != 0;\n"
    "#endif\n"
    "  if (vram_margin && u_margin_vram_height > 0 && u_margin_vram_clamp == 0) {\n"
    "    ivec2 source = console_px + ivec2(hofs_s, vofs_s);\n"
    "    int map_width = ((bg_size_code & 1) != 0) ? 512 : 256;\n"
    "    if (source.x < 0 || source.x >= map_width || source.y < 0 ||\n"
    "        source.y >= u_margin_vram_height) discard;\n"
    "  }\n"
    "  bool scroll_beyond_map = scroll_has_no_place(hofs_s, vofs_s);\n"
    // A margin pixel this layer leaves empty is dropped by the one discard
    // at the end, not by an early discard inside a branch. With an early
    // discard, radeonsi's LLVM compiler (Mesa 25, Radeon 740M) also killed
    // neighbouring world-map pixels that never took the branch. That showed
    // as 4 px black bars beside the console's screen.
    "  bool drop_pixel = false;\n"
    "  int index;\n"
    "#ifndef GSR_BG_REGULAR_ONLY\n"
    "  if (u_affine != 0) {\n"
    // Affine background (mode 1's BG2, mode 2's BG2/BG3): the console walks
    // a 2x3 transform from its own reference point, one row and column at a
    // time (gba_ppu.cpp, render_affine_bg). Both halves of that walk are read
    // PER ROW, never from one end-of-frame register read:
    //
    //   * The reference point is the console's internal accumulator, which it
    //     reloads at VBlank and advances by PB/PD every scanline -- and which
    //     the game overwrites mid-frame whenever it animates the transform.
    //     Reading the register file at the end of the frame yields only the
    //     last row's value, which is why the overworld drew as noise and a
    //     battle arena drew as a band or not at all: every row sampled the
    //     map through the final row's transform.
    //   * PA and PC likewise come from this row's own copy of the registers,
    //     the same source the reference uses (gba_ppu.cpp, row_io_for).
    //
    // A row whose accumulator was never latched gets the frame's own
    // reference extrapolated to it on the way in (see upload_row_affine).
    //
    // Whole pixel indices only (console_px, reg_row are already integers,
    // floored before this point) -- an interpolated coordinate would sample
    // the neighbouring texel once the transform rotates it.
    // THE EXPANDED VIEW AS THE CANVAS.
    //
    // The effect canvas is as wide as the CONSOLE screen and the game
    // cannot be made to build a wider one: its stamp generator takes one
    // size for both axes, emits a corrupt instruction above 128, and the
    // next size up does not fit its heap. The sparks it clips at that canvas
    // edge are added back further down (the game does request sparks
    // outside its canvas).
    //
    // What CAN be done is decide where that canvas lands. Sampling it
    // through the widened view instead of the console window makes its
    // columns span our whole screen, so the effect reaches both edges
    // instead of stopping where the console's screen ends. Nothing is
    // invented and nothing repeats; the same artwork covers more ground,
    // at a proportionally coarser horizontal step.
    "    ivec2 aff_px = console_px;\n"
    "    if (u_effect_span.x > 0.0) {\n"
    "      vec2 scaled = (vec2(console_px) - u_camera) * u_effect_span;\n"
    "      aff_px = ivec2(floor(scaled));\n"
    "    }\n"
    "    int param_off = 0x20 + (u_layer - 2) * 0x10;\n"
    "    int aff_row = clamp(aff_px.y, 0, 159);\n"
    "    int pa = row_s16(aff_row, param_off);\n"
    "    int pc = row_s16(aff_row, param_off + 4);\n"
    "    int ref_slot = (u_layer - 2) * 2;\n"
    "    int ref_x = row_affine_ref(aff_row, ref_slot);\n"
    "    int ref_y = row_affine_ref(aff_row, ref_slot + 1);\n"
    // Above row 0 and below row 159 there is no scanline, so there is no
    // latched reference -- `reg_row` has clamped to the nearest real row.
    // Repeating that row draws the same line of the map over and over, which
    // is vertical banding of the expanded world map above and below
    // the authentic 160 rows. Keep walking instead, one step per row, where
    // the step is the trend over the last 16 rows, for the reference and
    // for PA/PC alike. PB/PD are NOT the step to use: the game reloads the
    // reference itself every scanline on the world map, and leaves PB at
    // zero while doing it, so a PB walk would not move at all. Nor is the
    // edge row's single step: the world map's PA drops one unit every few
    // rows and the reference jumps 120/256 px sideways with it to keep the
    // centre column still, so a step taken across such a row, repeated
    // with PA held, shears the margin sideways. Rows 0..159 keep the exact integer walk.
    "    int rows_past = aff_px.y - aff_row;\n"
    "    int tex_x;\n"
    "    int tex_y;\n"
    "    if (rows_past != 0) {\n"
    "      int far_row = (aff_px.y < 0) ? 16 : 143;\n"
    "      float run = float(rows_past) / float(aff_row - far_row);\n"
    "      float ex = float(ref_x) + run *\n"
    "          float(ref_x - row_affine_ref(far_row, ref_slot));\n"
    "      float ey = float(ref_y) + run *\n"
    "          float(ref_y - row_affine_ref(far_row, ref_slot + 1));\n"
    "      float epa = float(pa) + run *\n"
    "          float(pa - row_s16(far_row, param_off));\n"
    "      float epc = float(pc) + run *\n"
    "          float(pc - row_s16(far_row, param_off + 4));\n"
    "      if (u_affine_step_valid != 0) {\n"
    "        vec4 step = aff_px.y < 0 ? u_affine_step_top : u_affine_step_bottom;\n"
    "        float past = float(rows_past);\n"
    "        ex = float(ref_x) + past * step.x;\n"
    "        ey = float(ref_y) + past * step.y;\n"
    "        epa = float(pa) + past * step.z;\n"
    "        epc = float(pc) + past * step.w;\n"
    "      }\n"
    "      if (epa * float(pa) < 0.0) epa = 0.0;\n"
    "      tex_x = int(floor((ex + float(aff_px.x) * epa) / 256.0));\n"
    "      tex_y = int(floor((ey + float(aff_px.x) * epc) / 256.0));\n"
    "      ref_x = int(floor(ex));\n"
    "    } else {\n"
    "      tex_x = (ref_x + aff_px.x * pa) >> 8;\n"
    "      tex_y = (ref_y + aff_px.x * pc) >> 8;\n"
    "    }\n"
    // A battle spell drawn on a WRAPPING 256-pixel affine layer at 1:1
    // (Ray: BG2CNT 0x6785, PA 256, PC 0) repeats its
    // left-hand bolts in the right margin, because past map column 255 the
    // layer starts over. Same rule as the text-layer effect strip below:
    // each margin continues into the 16 columns the console never shows
    // (240..255), shared out by what runs into them from each edge, and
    // never past them. Past its share a margin draws nothing: a margin
    // pixel is never a copy of an edge column, which would stretch particles
    // into streaks (Soothing Star). The one continuation is a
    // FLAT row: the whole strip one colour (Ray's full-screen tint) carries
    // that colour to the edges, since a uniform colour has nothing to
    // stretch.
    // Precomputed per-row strip share (see u_hold_strip in draw_background_as):
    // 254 = flat full row (clamp step to 15), 16 = non-flat full row (discard
    // past 15), 0..16 = this margin's share of the 16-column unseen strip.
    "    if (hold_edge && outside_native && bg_wraps != 0 &&\n"
    "        u_effect_wrap_canvas == 0 &&\n"
    "        bg_size_code == 1 && pa == 256 && pc == 0 &&\n"
    "        (aff_px.x < 0 || aff_px.x >= 240)) {\n"
    "      int base_x = ref_x >> 8;\n"
    "      bool right = aff_px.x >= 240;\n"
    "      int step = right ? aff_px.x - 240 : -1 - aff_px.x;\n"
    "      int row_idx = clamp(int(floor(v_screen.y)), 0, u_hold_strip_max);\n"
    "      int share = int(gsr_raw_fetch(u_hold_strip, ivec2(right ? 0 : 1, row_idx), 0).r);\n"
    "      if (share == 254) {\n"
    "        if (step > 15) step = 15;\n"
    "      } else if (step >= share) {\n"
    "        discard;\n"
    "      }\n"
    "      tex_x = base_x + (right ? 240 + step : -1 - step);\n"
    "    }\n"
    "    effect_px = ivec2(tex_x, tex_y);\n"
    // World-map pixels outside the console's screen come from the whole
    // world map rather than the game's ring. Only on this row's world-map
    // layout: mode 2, BG3 map block 8, BG2 map block 10 (the rings at VRAM
    // 0x4000 and 0x5000). The map coordinate is the world pixel; the world
    // repeats every 4096.
    GBARECOMP_WORLD_MAP_SAMPLE_BEGIN
    "    int bg_pixels = 128 << bg_size_code;\n"
    // A wrapping canvas layer: past the game's 128x128 canvas nothing is
    // taken from the wrapped map in the margins. There the picture is the
    // canvas's own fill with our captured sparks over it, the ones the game
    // clipped at its canvas edge; inside the console's screen the game's
    // pixel stays and only those clipped sparks are added (see below).
    // The fill alone also counts: Meteor's tint on its wrapping 128 canvas
    // has no sparks on some frames.
    "    bool wrap_beyond = ((u_effect_wrap_canvas != 0 &&\n"
    "                         (outside_native || u_effect_extra != 0)) ||\n"
    "                        u_effect_fill_beyond != 0) &&\n"
    "        bg_wraps != 0 && (tex_x < 0 || tex_y < 0 ||\n"
    "                          tex_x >= 128 || tex_y >= 128);\n"
    "    if (bg_wraps != 0) {\n"
    "      tex_x &= (bg_pixels - 1);\n"
    "      tex_y &= (bg_pixels - 1);\n"
    "    } else if (tex_x < 0 || tex_x >= bg_pixels ||\n"
    "               tex_y < 0 || tex_y >= bg_pixels) {\n"
    "      if (u_effect_extra == 0 && u_effect_fill_beyond == 0) discard;\n"
    "      beyond_game_canvas = true;\n"
    "    }\n"
    // Outside the console's own 240x160 window there is no real scanline to
    // walk the transform from; same rule the regular-layer path below uses,
    // and the same battle-arena exception.
    "    if (outside_native && !arena_margin && u_effect_extra == 0 &&\n"
    "        u_effect_fill_beyond == 0 && u_effect_repeats == 0)\n"
    "      drop_pixel = true;\n"
    // One byte per map entry: the whole byte is the tile index. Affine maps
    // carry no flip bits and no palette bank (gba_ppu.cpp, render_affine_bg).
    "    if (!beyond_game_canvas) {\n"
    "    int bg_tiles = bg_pixels >> 3;\n"
    "    int map_off = bg_screen_base + (tex_y >> 3) * bg_tiles + (tex_x >> 3);\n"
    "    int tile = int(vram_byte(map_off));\n"
    "    index = int(vram_byte(bg_char_base + tile * 64"
    " + (tex_y & 7) * 8 + (tex_x & 7)));\n"
    "    }\n"
    // A non-wrapping canvas at 2:1 (PA 128) ends 16 columns into the right
    // margin, but Spark Plasma stops drawing at canvas x 119, the console's
    // edge: its last 8 columns hold only spill. Our sparks start past 128,
    // so their density would change at a hard line 16 px into the margin.
    // Out here our sparks join the game's by the larger
    // value too; inside the console's screen nothing changes.
    "    if (!beyond_game_canvas && !wrap_beyond &&\n"
    "        (bg_wraps == 0 || bg_size_code == 0) && outside_native &&\n"
    "        u_effect_extra != 0) {\n"
    "      vec2 at = vec2(effect_px) + u_effect_origin;\n"
    "      if (at.x >= 0.0 && at.y >= 0.0 &&\n"
    "          at.x < u_effect_size.x && at.y < u_effect_size.y)\n"
    "        index = max(index, int(texelFetch(u_effect, ivec2(at), 0).r));\n"
    "    }\n"
    "    if (wrap_beyond) {\n"
    "      int base = outside_native ? u_effect_fill : index;\n"
    "      vec2 at = vec2(effect_px) + u_effect_origin;\n"
    "      int spark = 0;\n"
    "      if (u_effect_extra != 0 && at.x >= 0.0 && at.y >= 0.0 &&\n"
    "          at.x < u_effect_size.x && at.y < u_effect_size.y)\n"
    "        spark = int(texelFetch(u_effect, ivec2(at), 0).r);\n"
    // The game composes its canvas by taking the larger value (the packed
    // canvas stamp is EffectBlend::Maximum), so the same rule joins ours.
    "      index = max(base, spark);\n"
    "    }\n"
    "    if (u_effect_hide == 2 && bg_wraps != 0 && tex_x < 128 && tex_y < 128)\n"
    "      index = 0;\n"
    "    }\n"
    "  } else {\n"
    "#else\n"
    "  {\n"
    "#endif\n"
    // A regular layer showing the spell canvas: the copy of the canvas
    // that reaches the console's screen is the real one; everything past
    // it -- in the margins, where the map would start over -- is our
    // captured sparks (and the canvas fill), never the wrapped map. Inside
    // the screen the game's pixel stays and only the sparks the game
    // clipped at its canvas edge are added.
    "    bool text_canvas = hold_edge && u_effect_text_canvas != 0 &&\n"
    "                       u_effect_extra != 0;\n"
    "    bool text_beyond = false;\n"
    "    ivec2 canvas_px = ivec2(0);\n"
    "    if (text_canvas) {\n"
    "      int map_w = ((bg_size_code & 1) != 0) ? 512 : 256;\n"
    "      int map_h = ((bg_size_code & 2) != 0) ? 512 : 256;\n"
    "      int sx0 = (int(u_effect_canvas_map.x) - int(hofs_raw & 0x1FFu)) & (map_w - 1);\n"
    "      int sy0 = (int(u_effect_canvas_map.y) - int(vofs_raw & 0x1FFu)) & (map_h - 1);\n"
    "      if (sx0 >= 240) sx0 -= map_w;\n"
    "      if (sy0 >= 160) sy0 -= map_h;\n"
    "      canvas_px = console_px - ivec2(sx0, sy0);\n"
    "      text_beyond = canvas_px.x < 0 || canvas_px.y < 0 ||\n"
    "                    canvas_px.x >= 128 || canvas_px.y >= 128;\n"
    "    }\n"
    "    if (text_beyond && outside_native) {\n"
    "      index = u_effect_fill;\n"
    "    } else {\n"
    // Scroll is masked to the hardware's own 9 bits for the actual pixel
    // fetch. Golden Sun writes wider values whose excess selects the
    // layer's region of the shared map grid, but that only matters to the
    // room-buffer path below, which uses the raw value instead.
    // Keep the effect's held edge confined to the expanded margin. A layer
    // with a large scroll accumulator continues from its wrapped tilemap at
    // the expanded coordinates; clamping it here pins the edge texel and
    // smears the scrolling pattern across the margin.
    //
    // Sideways, each margin continues into the map's own off-screen strip:
    // the columns between x=240 and the point where the map wraps back onto
    // the screen's left edge (16 pixels on a 256-wide map). Those are real
    // canvas pixels the console would show on a wider screen. Both margins
    // reach that strip from opposite ends, so per row the strip is shared
    // out by what is in it: an effect running unbroken from the right edge
    // belongs to the right margin, one running from the left edge to the
    // left margin, and any gap between is split in half. Past its share a
    // margin draws nothing -- the effect has really ended there. Only a
    // row lit across the whole strip (a full-width effect, which would
    // otherwise stop square) continues its own wrapped map into both
    // margins. Holding the last column would draw that row's edge pixel as
    // streaks across the right margin and leave the left margin bare: The
    // Lost Age's critical-hit flash is a whole-screen picture on BG1 (all
    // 256 map columns lit, 32 distinct tiles per row) shown for one frame.
    //
    // Holding column 239 stretches whatever sits on the edge into a bar
    // across the margin; holding the middle of the strip does the same to
    // a spark 9 pixels wide. A 512-wide map's strip (272) is
    // wider than both margins, so only its first 16 columns are examined.
    //
    // NO MARGIN PIXEL IS A COPY OF AN EDGE. Past its share a
    // margin draws nothing; holding the share's last column would stretch
    // particles into streaks. Above and below, the same holds for rows:
    // each vertical margin reads the map's own unseen rows (half the
    // hidden strip each way), never row 0 or 159 repeated.
    "    ivec2 src_px = console_px;\n"
    "    if (hold_edge && outside_native && !text_canvas) {\n"
    "      if (src_px.y < 0 || src_px.y > 159) {\n"
    "        int vstrip = (((bg_size_code & 2) != 0) ? 512 : 256) - 160;\n"
    "        int vstep = src_px.y < 0 ? -1 - src_px.y : src_px.y - 160;\n"
    "        if (vstep >= vstrip / 2) discard;\n"
    "      }\n"
    "      if (src_px.x < 0 || src_px.x >= 240) {\n"
    "        int strip = (((bg_size_code & 1) != 0) ? 512 : 256) - 240;\n"
    "        bool right = src_px.x >= 240;\n"
    "        int step = right ? src_px.x - 240 : -1 - src_px.x;\n"
    "        if (strip == 16) {\n"
    "          int row_idx = clamp(int(floor(v_screen.y)), 0, u_hold_strip_max);\n"
    "          int share = int(gsr_raw_fetch(u_hold_strip, ivec2(right ? 0 : 1, row_idx), 0).r);\n"
    "          if (share != 255 && step >= share) discard;\n"
    "        } else if (step >= strip / 2) {\n"
    "          discard;\n"
    "        }\n"
    "      }\n"
    "    }\n"
    "    ivec2 pixel = src_px + ivec2(int(hofs_raw) & 0x1FF, int(vofs_raw) & 0x1FF);\n"
    "    if (vram_margin && u_margin_vram_height > 0 && u_margin_vram_clamp != 0) {\n"
    "      int map_width = ((bg_size_code & 1) != 0) ? 512 : 256;\n"
    "      pixel = clamp(console_px + ivec2(hofs_s, vofs_s), ivec2(0),\n"
    "                    ivec2(map_width - 1, u_margin_vram_height - 1));\n"
    "    }\n"
    "    if (vram_margin && u_margin_repeat.x > 0.0 && u_margin_repeat.y > 0.0)\n"
    "      pixel &= ivec2(u_margin_repeat) - ivec2(1);\n"
    // Text backgrounds always wrap, over 256 or 512 pixels per axis.
    "    int width_tiles  = ((bg_size_code & 1) != 0) ? 64 : 32;\n"
    "    int height_tiles = ((bg_size_code & 2) != 0) ? 64 : 32;\n"
    "    int tile_x = (pixel.x >> 3) & (width_tiles - 1);\n"
    "    int tile_y = (pixel.y >> 3) & (height_tiles - 1);\n"
    "    uint entry = 0u;\n"
    "\n"
    // Room buffer: world pixel -> tile -> metatile id -> quadrant -> grid
    // cell -> atlas entry -> tile entry. The room rect/camera bound which
    // pixels are inside the room at all; the layer's own UNMASKED scroll
    // (not the game's camera) gives this layer's source tile, exactly the
    // rule the retired room-buffer render path used (confirmed in Goma Cave and
    // Tret). u_use_room is always 0 for an affine layer (never reaches this
    // branch), so this stays a field-only path.
    // Only OUTSIDE the console's own 240x160 window. Inside it the console's
    // tilemap is the exact answer by definition; letting the room rule
    // replace it there makes the native picture wrong wherever the rule reads
    // the wrong grid region -- Lamakan Desert, whose BG2/BG3 scroll 1024 px
    // off the camera, drew 29,544 of 38,400 native pixels wrong.
    // One exception inside the window: Lamakan's haze lifts the top rows up
    // to 3 px above the layer's own row 0, into map rows above our raised
    // camera (the inset) that the game never streamed into VRAM, which would
    // draw rows 1-2 black (at the room's top edge). In a
    // frame read through the layer records, those pixels come from the room.
    "    bool haze_above = false;\n"
    "    if (room_row_authorized && !outside_native && u_haze_room != 0) {\n"
    "      int haze_dy = ((int(vofs_raw) - int(u_room_ref.y) + 512) & 1023) - 512;\n"
    "      haze_above = console_px.y + haze_dy < 0;\n"
    "    }\n"
    "    if (room_row_authorized && (outside_native || haze_above)) {\n"
    "      ivec2 room_px = console_px + ivec2(u_ewram_camera);\n"
    // A game-measured distant backdrop (SceneMarginLayer::past_room_edge)
    // continues past the room's own edge; the keep-outs below still hold.
    "      room_covers = u_past_room_edge != 0 ||\n"
    "                    (room_px.x >= int(u_room_min.x) &&\n"
    "                     room_px.y >= int(u_room_min.y) &&\n"
    "                     room_px.x < int(u_room_max.x) &&\n"
    "                     room_px.y < int(u_room_max.y));\n"
    "      for (int k = 0; k < u_keep_out_count; ++k) {\n"
    "        ivec4 r = ivec4(u_keep_out[k]);\n"
    "        if (room_px.x >= r.x && room_px.y >= r.y &&\n"
    "            room_px.x < r.z && room_px.y < r.w)\n"
    "          room_covers = false;\n"
    "      }\n"
    "      if (room_covers) {\n"
    "        ivec2 room_scroll = ivec2(int(hofs_raw), int(vofs_raw));\n"
    // Kolima's flood shows the flooded copy of the area, kept 176 rows
    // lower in the map, on a growing set of rows (VOFS 312 -> 488). Beside
    // that copy the map holds unrelated tiles, so in the margins those rows
    // read the layer's own position instead.
    //
    // While the flood drains or refills, the other state of the whole room
    // is elsewhere in the grid. For a drain it is 512 px left and 1024 down;
    // for a refill it is 512 px left and 512 down; the bottom layer does not
    // change. split_row_offset finds that place. A jumping row then reads
    // from it. Rows above and below the console take the jump of the console
    // row mirrored across the edge, so the margins stripe like the rows
    // beside them (the pond sits at the top of the screen).
    "        bool split_jump = false;\n"
    "        if (u_split_v != 0) {\n"
    "          int jump_row = reg_row;\n"
    "          if (u_split_found != 0 && console_px.y < 0)\n"
    "            jump_row = min(-console_px.y - 1, 159);\n"
    "          if (u_split_found != 0 && console_px.y > 159)\n"
    "            jump_row = max(319 - console_px.y, 0);\n"
    "          int jump_vofs = int(row_u16(jump_row, 0x10 + u_layer * 4 + 2));\n"
    "          split_jump =\n"
    "              abs(((jump_vofs - int(u_room_ref.y) + 512) & 1023) - 512) > 9;\n"
    "          if (split_jump ||\n"
    "              abs(((room_scroll.y - int(u_room_ref.y) + 512) & 1023) - 512) > 9)\n"
    "            room_scroll.y = int(u_room_ref.y);\n"
    "        }\n"
    // Record reference (room_reference_for): the layer's own position +
    // the row's offset from it folded into -512..511, so the haze's -1
    // (0xFFFF) at a room's left edge stays -1.
    "        if (u_room_ref_on != 0 && !continue_bg3_sky) {\n"
    "          ivec2 ref = ivec2(u_room_ref);\n"
    "          room_scroll = ref + (((room_scroll - ref + 512) & 1023) - 512);\n"
    "        }\n"
    "        if (split_jump && u_split_found != 0)\n"
    "          room_scroll += ivec2(u_split_offset);\n"
    "        ivec2 source = console_px + room_scroll;\n"
    // The crow's nest sky: above the sky art its region
    // holds one filler tile (the brown band). Nothing is drawn there:
    // carrying the art's top row up would stretch it into streaks, and no
    // margin pixel is a copy of an edge.
    "        if (u_hold_top != 0 && !continue_bg3_sky &&\n"
    "            source.y < u_hold_floor)\n"
    "          drop_pixel = true;\n"
    // An auto-scrolling layer wraps inside its own region, as the game
    // streams it (the sandstorm: 256 x 256 at x 1024).
    "        if (u_room_wrap.z > 0.0) {\n"
    "          int rx = int(u_room_wrap.x);\n"
    "          source.x = rx + ((source.x - rx) & (int(u_room_wrap.z) - 1));\n"
    "        }\n"
    "        if (u_room_wrap.w > 0.0) {\n"
    "          int ry = int(u_room_wrap.y);\n"
    "          source.y = ry + ((source.y - ry) & (int(u_room_wrap.w) - 1));\n"
    "        }\n"
    "        if (continue_bg3_sky && source.x < 0) {\n"
    "          source.x = source.x & (width_tiles * 8 - 1);\n"
    "        }\n"
    // A game can expose a measured horizontal scenery repeat to fill the
    // margins where a slower layer runs out before the room does. Any width
    // (The Lost Age's map 248 backdrop is 480): a floored modulo, since GLSL leaves
    // % of a negative undefined; for a power of two it equals a mask.
    "        if (!vram_margin && u_margin_repeat.x > 0.0 &&\n"
    "            (source.x < 0 || u_margin_repeat.y == 0.0)) {\n"
    "          int repeat_x = int(u_margin_repeat.x);\n"
    "          source.x -= repeat_x * int(floor(float(source.x) / float(repeat_x)));\n"
    "        }\n"
    // Above or left of the layer's art, its repeat measured on the console's
    // own rows and columns (SceneMarginLayer::art_repeat_y / _x).
    "        if (u_art_repeat.x > 0.0 && source.y < 0) {\n"
    "          ivec2 art = ivec2(u_art_repeat.xy);\n"
    "          int copies = (-source.y + art.x - 1) / art.x;\n"
    "          source += copies * ivec2(art.y, art.x);\n"
    "        }\n"
    "        if (u_art_repeat.z > 0.0 && source.x < 0) {\n"
    "          int art_x = int(u_art_repeat.z);\n"
    "          source.x += ((-source.x + art_x - 1) / art_x) * art_x;\n"
    "        }\n"
    // Past the grid's far edge only (room_grid_pixel).
    "        if (u_room_grid_wrap != 0) {\n"
    "          if (source.x >= 0) source.x &= 2047;\n"
    "          if (source.y >= 0) source.y &= 2047;\n"
    "        }\n"
    "        room_source = source;\n"
    "        if (source.x >= 0 && source.y >= 0) {\n"
    "          int stx = source.x >> 3;\n"
    "          int sty = source.y >> 3;\n"
    "          int gx = stx >> 1;\n"
    "          int gy = sty >> 1;\n"
    "          if (gx >= 0 && gy >= 0 && gx < 128 && gy < 128) {\n"
    "            uint id = gsr_raw_fetch(u_id_grid, ivec2(gx, gy), 0).r;\n"
    "            int sub = (sty & 1) * 2 + (stx & 1);\n"
    "            uint candidate = gsr_raw_fetch(u_atlas, ivec2(sub, int(id & 4095u)), 0).r;\n"
    "            bool unavailable = candidate == 0xFFFFu ||\n"
    "                               candidate == 0xF200u;\n"
    "            if (!unavailable) { entry = candidate; room_supplied = true; }\n"
    "          }\n"
    "        }\n"
    "      }\n"
    "    }\n"
    "\n"
    "    if (outside_native && !room_supplied && !arena_margin && !vram_margin &&\n"
    "        !(room_row_authorized && scroll_beyond_map && room_covers)) discard;\n"
    // Only fetch VRAM map entry when the room buffer did not supply it.
    "    if (!room_supplied) {\n"
    "      int block = 0;\n"
    "      if (width_tiles == 64 && tile_x >= 32) block += 1;\n"
    "      if (height_tiles == 64 && tile_y >= 32) block += (width_tiles == 64) ? 2 : 1;\n"
    "      int entry_address = bg_screen_base + block * 2048\n"
    "                        + (((tile_y & 31) * 32) + (tile_x & 31)) * 2;\n"
    "      entry = vram_half(entry_address);\n"
    "    }\n"
    "\n"
    "    int tile    = int(entry & 0x3FFu);\n"
    "    bool hflip  = (entry & 0x400u) != 0u;\n"
    "    bool vflip  = (entry & 0x800u) != 0u;\n"
    "    int bank    = int((entry >> 12) & 0xFu);\n"
    "\n"
    "    int tx = pixel.x & 7;\n"
    "    int ty = pixel.y & 7;\n"
    "    if (hflip) tx = 7 - tx;\n"
    "    if (vflip) ty = 7 - ty;\n"
    "\n"
    "    bool room_copy = room_supplied && outside_native;\n"
    "    room_colours = room_copy;\n"
    "    if (bg_eight_bit != 0) {\n"
    "      int at = bg_char_base + tile * 64 + ty * 8 + tx;\n"
    "      index = int(room_copy ? room_vram_byte(at) : vram_byte(at));\n"
    "    } else {\n"
    "      int at = bg_char_base + tile * 32 + ty * 4 + (tx >> 1);\n"
    "      uint two = room_copy ? room_vram_byte(at) : vram_byte(at);\n"
    "      index = int(((tx & 1) != 0) ? (two >> 4) : (two & 0xFu));\n"
    "      if (index != 0) index += bank * 16;\n"
    "    }\n"
    // A layer moved a little off its rest position (the Altin drain sinks
    // the water 24 rows, region 1024 -> 1000, before its stripes start): in
    // the margins it keeps a pixel only where the layer had one at rest, so
    // the water never spills onto ground the console's own shoreline hides
    "    if (room_copy && u_rest_dy != 0 && index != 0 &&\n"
    "        room_index_at(room_source + ivec2(0, u_rest_dy), bg_char_base,\n"
    "                      bg_eight_bit) == 0) index = 0;\n"
    // The map pixel just sampled lies in the canvas block (wrapped over the
    // map): draw nothing there. Uses the sampled pixel, so the margins'
    // held-edge strip is hidden too.
    "    if (u_effect_hide == 1) {\n"
    "      ivec2 map_mask = ivec2(width_tiles * 8 - 1, height_tiles * 8 - 1);\n"
    "      ivec2 rel = (pixel - ivec2(u_effect_canvas_map)) & map_mask;\n"
    "      if (rel.x < 128 && rel.y < 128) index = 0;\n"
    "    }\n"
    "    }\n"
    "    if (text_beyond) {\n"
    "      vec2 at = vec2(canvas_px) + u_effect_origin;\n"
    "      int spark = 0;\n"
    "      if (at.x >= 0.0 && at.y >= 0.0 &&\n"
    "          at.x < u_effect_size.x && at.y < u_effect_size.y)\n"
    "        spark = int(texelFetch(u_effect, ivec2(at), 0).r);\n"
    "      index = max(index, spark);\n"
    "    }\n"
    "  }\n"
    "#ifndef GSR_BG_REGULAR_ONLY\n"
    // Add skipped sparks only where the game left the layer empty. The
    // normal window, blend and two-pass peel still apply. Replacing the full
    // layer lost effects that the capture didn't see.
    "  if ((u_effect_extra != 0 || u_effect_fill_beyond != 0) &&\n"
    "      beyond_game_canvas) {\n"
    "    vec2 at = vec2(effect_px) + u_effect_origin;\n"
    "    if (outside_native) at.x = clamp(at.x, u_effect_hold.x, u_effect_hold.y);\n"
    "    index = (outside_native && u_effect_fill_beyond != 0) ? u_effect_fill : 0;\n"
    "    if (u_effect_extra != 0 && at.x >= 0.0 && at.y >= 0.0 &&\n"
    "        at.x < u_effect_size.x && at.y < u_effect_size.y) {\n"
    // Joined by the larger value, as the game composes its canvas.
    "      index = max(index, int(texelFetch(u_effect, ivec2(at), 0).r));\n"
    "    }\n"
    "  }\n"
    "#endif\n"
    // Index 0 is the console's transparent colour, in every path.
    "  if (drop_pixel || index == 0) discard;\n"
    "\n"
    // Bits 0-3 of the window control are BG0-3's own enable. An arena pixel
    // out in the side margin skips this.
    "  if (u_window_on != 0 && !(outside_native && arena_margin)) {\n"
    "    int wc = int(round(gsr_raw_fetch(u_window, ivec2(gl_FragCoord.xy), 0).r"
    " * 255.0));\n"
    "    if (((wc >> u_layer) & 1) == 0) discard;\n"
    "  }\n"
    "\n"
    "  uint c = gsr_raw_fetch(u_palette, ivec2((u_palette_half + index) & 511,\n"
    "                                            room_colours ? 1 : clamp(world_colour_row, 0, 5)), 0).r;\n"
    // Depth comes from the strip quad's vertex depth (float(bg_key)/1024),
    // keeping hardware early-Z rejection active instead of writing gl_FragDepth.
    "  o_colour = vec4(float(int(c) & 0xFF) / 255.0,\n"
    "                  float((int(c) >> 8) & 0xFF) / 255.0,\n"
    "                  float(bg_key & 0xFF) / 255.0,\n"
    "                  float((bg_key >> 8) & 3) / 255.0);\n"
    "}\n";

// Objects. One quad per object; the shader works out which pixel of the
// sprite it is on, then follows the same tile-to-colour path as a background.
//
// Object tile art always starts at 0x10000 in video memory, and object colours
// are the second half of palette memory. The two mapping modes differ only in
// how a sprite's tiles are laid out: one after another, or one row of the
// sprite per 32-tile row of memory.
//
// Sprites are drawn in batches (ObjectBatch in draw_frame): each quad's
// a_tint.x names its record in u_rec, three ivec4 per sprite --
//   [0] sort key, flags, first tile, palette bank
//   [1] sprite width and height, drawn box width and height
//   [2] lift box (console pixels: x0, y0, x1, y1)
// flags: 1 semi-transparent, 2 lifted copy, 4 held inside the console
// screen, 8 8-bit colour, 16 h-flip, 32 v-flip, 64 rotated/scaled,
// 128 margins only, bits 8-12 the transform group (u_transforms).
// kObjectRecords is how many records one draw holds: 48 * 3 ivec4 plus the
// 32 transforms and the 4-row matrix is 180 of the 256 vertex uniform
// vectors OpenGL ES 3.0 guarantees.
constexpr int kObjectRecords = 48;
constexpr int kObjectRecordInts = 12;
const char* kObjectVertex =
    "#version 130\n"
    "in vec2 a_pos;\n"
    "in vec2 a_uv;\n"
    "in float a_depth;\n"
    "in vec4 a_tint;\n"
    "uniform mat4 u_transform;\n"
    "uniform ivec4 u_rec[144];\n"       // kObjectRecords * 3
    "uniform vec4 u_transforms[32];\n"  // PA, PB, PC, PD per transform group
    "out vec2 v_local;\n"
    "out vec2 v_screen;\n"
    "flat out ivec4 v_rec0;\n"
    "flat out ivec4 v_rec1;\n"
    "flat out ivec4 v_rec2;\n"
    "flat out vec4 v_pd;\n"
    "void main() {\n"
    "  int r = int(a_tint.x) * 3;\n"
    "  v_rec0 = u_rec[r];\n"
    "  v_rec1 = u_rec[r + 1];\n"
    "  v_rec2 = u_rec[r + 2];\n"
    "  v_pd = u_transforms[(v_rec0.y >> 8) & 31];\n"
    // a_uv carries the pixel offset inside the sprite.
    "  v_local = a_uv;\n"
    // a_pos is this vertex's OUTPUT pixel, top-down, the same space the
    // background shader's v_screen is in. gl_FragCoord is not a substitute:
    // GL counts its y from the bottom, so a clip written against it lands on
    // the mirrored rows -- and at native size, where the clip covers the
    // whole picture, nothing reveals the mistake.
    "  v_screen = a_pos;\n"
    "  gl_Position = u_transform * vec4(a_pos, a_depth, 1.0);\n"
    "}\n";

// Visible objects and object windows use the same tile pixels. Keep the
// box, native clipping, tile addressing and transparent index in one shader
// source fragment so a change to either pass's shape reaches both.
const char* kObjectSampling =
    "#version 130\n"
    "in vec2 v_local;\n"
    "in vec2 v_screen;\n"
    "uniform usampler2D u_vram;\n"
    "uniform usampler2D u_row_io;\n"    // this frame's per-row registers
    "uniform vec2 u_camera;\n"       // world offset of the output's origin
    // This sprite's record (see kObjectVertex), under the names the code
    // below reads.
    "flat in ivec4 v_rec0;\n"
    "flat in ivec4 v_rec1;\n"
    "flat in ivec4 v_rec2;\n"
    "flat in vec4 v_pd;\n"
    "#define u_size vec2(v_rec1.xy)\n"     // sprite size in pixels
    "#define u_tile v_rec0.z\n"            // first tile number
    "#define u_eight_bit ((v_rec0.y >> 3) & 1)\n"
    "#define u_hflip ((v_rec0.y >> 4) & 1)\n"
    "#define u_vflip ((v_rec0.y >> 5) & 1)\n"
    "#define u_affine ((v_rec0.y >> 6) & 1)\n"  // rotated or scaled
    "#define u_transform_pd v_pd\n"        // PA, PB, PC, PD, already unscaled
    "#define u_box vec2(v_rec1.zw)\n"      // drawn box, twice the sprite if doubled
    // Parked objects and their masks stay inside the native screen, as in
    // gba_ppu.cpp's emit_obj.
    "#define u_clip_native ((v_rec0.y >> 2) & 1)\n"
    "out vec4 o_colour;\n"
    "\n"
    "uint vram_byte(int address) {\n"
    "  if (uint(address) >= 131072u) return 0u;\n"
    "  return gsr_raw_fetch(u_vram, ivec2(address & 1023, address >> 10), 0).r;\n"
    "}\n"
    "\n"
    // v_screen is the output pixel in both passes. Adding the camera gives
    // the corresponding console pixel, as in the background shader.
    "ivec2 object_console_pixel() {\n"
    "  return ivec2(floor(v_screen + u_camera));\n"
    "}\n"
    "\n"
    "bool inside_native(ivec2 p) {\n"
    "  return p.x >= 0 && p.x < 240 && p.y >= 0 && p.y < 160;\n"
    "}\n"
    "\n"
    "uint object_display_control(ivec2 console_px) {\n"
    "  int row = clamp(console_px.y, 0, 159);\n"
    "  return gsr_raw_fetch(u_row_io, ivec2(0, row), 0).r;\n"
    "}\n"
    "\n"
    "int sample_object(ivec2 console_px, uint dispcnt) {\n"
    "  if (u_clip_native != 0 && !inside_native(console_px)) discard;\n"
    "  ivec2 size = ivec2(u_size);\n"
    "  int lx;\n"
    "  int ly;\n"
    "  if (u_affine != 0) {\n"
    // The console walks the sprite's texture with the transform as it fills the
    // box, which is the same as mapping each box pixel back through it. The box
    // centre maps to the sprite centre; a pixel landing outside the sprite is
    // simply not drawn, which is what makes a rotated sprite's corners empty.
    // Take the whole pixel index first. The interpolated coordinate lands at
    // the centre of the pixel, half a step past where the console's integer
    // arithmetic starts, and that half step is enough to sample the neighbouring
    // texel once a rotation is applied.
    "    vec2 from_centre = floor(v_local) - u_box * 0.5;\n"
    "    float sx = u_transform_pd.x * from_centre.x\n"
    "             + u_transform_pd.y * from_centre.y + u_size.x * 0.5;\n"
    "    float sy = u_transform_pd.z * from_centre.x\n"
    "             + u_transform_pd.w * from_centre.y + u_size.y * 0.5;\n"
    "    lx = int(floor(sx));\n"
    "    ly = int(floor(sy));\n"
    "  } else {\n"
    "    lx = int(floor(v_local.x));\n"
    "    ly = int(floor(v_local.y));\n"
    "  }\n"
    "  if (lx < 0 || ly < 0 || lx >= size.x || ly >= size.y) discard;\n"
    // Flips and transforms are alternatives: the bits that hold the flips are
    // the transform group's number on a rotated object.
    "  if (u_hflip != 0) lx = size.x - 1 - lx;\n"
    "  if (u_vflip != 0) ly = size.y - 1 - ly;\n"
    "\n"
    "  int tile_x = lx >> 3;\n"
    "  int tile_y = ly >> 3;\n"
    "  int tiles_wide = size.x >> 3;\n"
    // An 8-bit-colour tile occupies two tile numbers.
    "  int step = (u_eight_bit != 0) ? 2 : 1;\n"
    "  int tile;\n"
    "  if (((dispcnt >> 6u) & 1u) != 0u) {\n"
    "    tile = u_tile + (tile_y * tiles_wide + tile_x) * step;\n"
    "  } else {\n"
    "    tile = u_tile + tile_y * 32 + tile_x * step;\n"
    "  }\n"
    // Tile numbers wrap inside their own 1024-tile page (u_tile 0x400 and up
    // is the second sprite page, gba_bus.h set_obj_extra).
    "  tile = (u_tile & 0x400) | (tile & 0x3FF);\n"
    "\n"
    "  int tx = lx & 7;\n"
    "  int ty = ly & 7;\n"
    "  int base = 0x10000 + tile * 32;\n"
    "  int index;\n"
    "  if (u_eight_bit != 0) {\n"
    "    index = int(vram_byte(base + ty * 8 + tx));\n"
    "  } else {\n"
    "    uint two = vram_byte(base + ty * 4 + (tx >> 1));\n"
    "    index = int(((tx & 1) != 0) ? (two >> 4) : (two & 0xFu));\n"
    "  }\n"
    "  if (index == 0) discard;\n"
    "  return index;\n"
    "}\n"
    "\n";

const char* kObjectFragment =
    "uniform usampler2D u_palette;\n"
    "uniform sampler2D u_window;\n"     // this pixel's 6-bit window control
    "uniform sampler2D u_top_record;\n" // pass 1's result; read only in pass 2
    "uniform int u_compare_top;\n"      // 0 = first pass, 1 = second pass
    "uniform int u_window_on;\n"        // 1: window masks are active this frame
    "#define u_bank v_rec0.w\n"         // 4-bit palette bank
    "#define u_key v_rec0.x\n"          // this object's sort key
    "#define u_semi_transparent (v_rec0.y & 1)\n" // object mode 1 (OBJ blend flag)
    "#define u_margin_only ((v_rec0.y >> 7) & 1)\n"
    // The lifted copy stays inside the lifting object's box in console pixels.
    "#define u_lift ((v_rec0.y >> 1) & 1)\n"
    "#define u_lift_box vec4(v_rec2)\n"
    "\n"
    "void main() {\n"
    "  ivec2 console_px = object_console_pixel();\n"
    "  if (u_margin_only != 0 && inside_native(console_px)) discard;\n"
    // Reject pass-2 and window-masked pixels before sampling VRAM tiles.
    "  if (u_compare_top != 0) {\n"
    "    vec4 top = gsr_raw_fetch(u_top_record, ivec2(gl_FragCoord.xy), 0);\n"
    "    int top_low = int(round(top.b * 255.0));\n"
    "    if (top_low < 128) discard;\n"
    "    int top_key = top_low | ((int(round(top.a * 255.0)) & 3) << 8);\n"
    "    if (u_key <= top_key) discard;\n"
    "  }\n"
    "  if (u_window_on != 0) {\n"
    "    int wc = int(round(gsr_raw_fetch(u_window, ivec2(gl_FragCoord.xy), 0).r"
    " * 255.0));\n"
    "    if (((wc >> 4) & 1) == 0) discard;\n"
    "  }\n"
    "  uint dispcnt = object_display_control(console_px);\n"
    "  if (((dispcnt >> 12u) & 1u) == 0u) discard;\n"
    "  if (u_lift != 0) {\n"
    "    if (float(console_px.x) < u_lift_box.x ||\n"
    "        float(console_px.y) < u_lift_box.y ||\n"
    "        float(console_px.x) >= u_lift_box.z ||\n"
    "        float(console_px.y) >= u_lift_box.w) discard;\n"
    "  }\n"
    "  int index = sample_object(console_px, dispcnt);\n"
    "  if (u_eight_bit == 0) index += u_bank * 16;\n"
    "\n"
    // Object colours live in the second half of palette memory.
    "  uint c = gsr_raw_fetch(u_palette, ivec2((256 + index) & 511, 0), 0).r;\n"
    "  int a4 = (u_semi_transparent != 0) ? 4 : 0;\n"
    "  o_colour = vec4(float(int(c) & 0xFF) / 255.0,\n"
    "                  float((int(c) >> 8) & 0xFF) / 255.0,\n"
    "                  float(u_key & 0xFF) / 255.0,\n"
    "                  float(((u_key >> 8) & 3) | a4) / 255.0);\n"
    "}\n";

// Mode-2 objects mark window coverage for WININ/WINOUT. The pass needs
// only the shape of the object, so it ignores the palette index.
const char* kObjectWindowFragment =
    "void main() {\n"
    "  ivec2 console_px = object_console_pixel();\n"
    "  uint dispcnt = object_display_control(console_px);\n"
    "  if (((dispcnt >> 15u) & 1u) == 0u) discard;\n"
    "  sample_object(console_px, dispcnt);\n"
    "  o_colour = vec4(1.0, 1.0, 1.0, 1.0);\n"
    "}\n";

// The resolve pass: one full-screen quad reading the two peel records and
// writing the console's compose rule, exactly as the reference does at the
// end of a scanline (gba_ppu.cpp, render_scanline_internal).
//
// The low byte of a record's key says which kind of layer won: 0..127 is an
// object (kind 4), 128..131 is background 0-3 (key - 128), and 255 is the
// cleared "nothing drawn" sentinel, which is read back as the backdrop
// (kind 5). See the clear colour chosen in draw().
const char* kResolveVertex =
    "#version 130\n"
    "in vec2 a_pos;\n"
    "in vec2 a_uv;\n"
    "in float a_depth;\n"
    "in vec4 a_tint;\n"
    "uniform mat4 u_transform;\n"
    "out vec2 v_screen;\n"
    "void main() {\n"
    "  v_screen = a_pos;\n"
    "  gl_Position = u_transform * vec4(a_pos, a_depth, 1.0);\n"
    "}\n";

const char* kResolveFragment =
    "#version 130\n"
    "in vec2 v_screen;\n"
    "uniform sampler2D u_top;\n"
    "uniform sampler2D u_second;\n"
    "uniform sampler2D u_window;\n"   // this pixel's 6-bit window control
    "uniform usampler2D u_row_io;\n"  // this frame's per-row registers
    "uniform vec2 u_camera;\n"        // world offset of the output's origin
    "uniform int u_fade_columns;\n"   // set_window_fade: tile columns, 0 = off
    "uniform int u_fade_row0;\n"      // first and last tile row
    "uniform int u_fade_row1;\n"
    "uniform float u_fade_opacity;\n"
    "uniform int u_red_x0;\n"         // set_window_red: the rows' box,
    "uniform int u_red_x1;\n"         // console pixels, end excluded
    "uniform int u_red_y0;\n"
    "uniform int u_red_y1;\n"
    "uniform int u_red_rows;\n"       // 16-pixel rows from y0, 0 = off
    "uniform int u_window_on;\n"      // 1: window masks are active this frame
    "uniform int u_has_second;\n"     // 1: pass 2 ran this frame
    "out vec4 o_colour;\n"
    "\n"
    "uint row_u16(int row, int offset) {\n"
    "  return gsr_raw_fetch(u_row_io, ivec2(offset >> 1, row), 0).r;\n"
    "}\n"
    "\n"
    "int layer_of(int low) {\n"
    "  if (low == 255) return 5;\n"
    "  if (low < 128) return 4;\n"
    "  return low - 128;\n"
    "}\n"
    "\n"
    "void main() {\n"
    // BLDCNT, BLDALPHA and BLDY come from THIS row, not from one
    // end-of-frame read. Measured on 56 recorded battle frames, the alpha
    // weights change part-way down 16 of them and the blend control down one
    // -- Golden Sun fades the arena without fading the
    // menus or the battlers over it. Taking the frame's last value instead
    // applies one row's fade to all 160, which is a battle arena going black
    // behind sprites that look completely normal.
    "  int reg_row = clamp(int(floor(v_screen.y + u_camera.y)), 0, 159);\n"
    "  int bldcnt = int(row_u16(reg_row, 0x50));\n"
    "  int bldalpha = int(row_u16(reg_row, 0x52));\n"
    "  int u_first_targets = bldcnt & 0x3F;\n"
    "  int u_second_targets = (bldcnt >> 8) & 0x3F;\n"
    "  int u_effect = (bldcnt >> 6) & 3;\n"
    "  int u_eva = min(bldalpha & 0x1F, 16);\n"
    "  int u_evb = min((bldalpha >> 8) & 0x1F, 16);\n"
    "  int u_bldy = min(int(row_u16(reg_row, 0x54)) & 0x1F, 16);\n"
    "  vec4 t = gsr_raw_fetch(u_top, ivec2(gl_FragCoord.xy), 0);\n"
    "  int ta = int(round(t.a * 255.0));\n"
    "  int top_color = int(round(t.r * 255.0)) | (int(round(t.g * 255.0)) << 8);\n"
    "  int top_low = int(round(t.b * 255.0));\n"
    "  int top_layer = layer_of(top_low);\n"
    "  bool top_semi = (ta & 4) != 0;\n"
    "  int second_color = 0;\n"
    "  int second_layer = 5;\n"
    "  if (u_has_second != 0) {\n"
    "    vec4 s = gsr_raw_fetch(u_second, ivec2(gl_FragCoord.xy), 0);\n"
    "    second_color = int(round(s.r * 255.0)) | (int(round(s.g * 255.0)) << 8);\n"
    "    second_layer = layer_of(int(round(s.b * 255.0)));\n"
    "  }\n"
    // Bit 5 of the window control is blend_enabled(x) -- the reference
    // multiplies it into EVERY target1, backdrop included, which is why it is
    // applied once here rather than baked into a peel record.
    "  bool blend_ok = true;\n"
    "  if (u_window_on != 0) {\n"
    "    int wc = int(round(gsr_raw_fetch(u_window, ivec2(gl_FragCoord.xy), 0).r"
    " * 255.0));\n"
    "    blend_ok = ((wc >> 5) & 1) != 0;\n"
    "  }\n"
    // An OBJ is a first target when semi-transparent or when BLDCNT
    // selects the OBJ layer; only a semi-transparent one forces alpha
    // (gba_ppu.cpp). The window's effect bit gates
    // only the BLDCNT choice: a semi-transparent OBJ blends even where the
    // window turns effects off (the title's lens flare, WININ 0x1616).
    "  bool top_target1 = (top_layer == 4 && top_semi) ||\n"
    "                    (blend_ok && ((u_first_targets >> top_layer) & 1) != 0);\n"
    "  bool second_target2 = ((u_second_targets >> second_layer) & 1) != 0;\n"
    "\n"
    "  int r; int g6; int b;\n"
    "  if ((u_effect == 1 || (top_layer == 4 && top_semi)) && top_target1 &&\n"
    "      second_target2 &&\n"
    "      !(top_layer == 4 && second_layer == 4)) {\n"
    "    int tr = top_color & 31;\n"
    "    int tg6 = ((top_color >> 4) & 62) | (top_color >> 15);\n"
    "    int tb = (top_color >> 10) & 31;\n"
    "    int br = second_color & 31;\n"
    "    int bg6 = ((second_color >> 4) & 62) | (second_color >> 15);\n"
    "    int bb = (second_color >> 10) & 31;\n"
    "    int eva = min(u_eva, 16);\n"
    "    int evb = min(u_evb, 16);\n"
    "    r = min(31, (tr * eva + br * evb + 8) >> 4);\n"
    "    g6 = min(63, (tg6 * eva + bg6 * evb + 8) >> 4);\n"
    "    b = min(31, (tb * eva + bb * evb + 8) >> 4);\n"
    "  } else if ((u_effect == 2 || u_effect == 3) && u_bldy != 0 && top_target1) {\n"
    "    r = top_color & 31;\n"
    "    g6 = ((top_color >> 4) & 62) | (top_color >> 15);\n"
    "    b = (top_color >> 10) & 31;\n"
    "    int evy = min(u_bldy, 16);\n"
    "    if (u_effect == 2) {\n"
    "      r += ((31 - r) * evy + 8) >> 4;\n"
    "      g6 += ((63 - g6) * evy + 8) >> 4;\n"
    "      b += ((31 - b) * evy + 8) >> 4;\n"
    "    } else {\n"
    "      r -= (r * evy + 7) >> 4;\n"
    "      g6 -= (g6 * evy + 7) >> 4;\n"
    "      b -= (b * evy + 7) >> 4;\n"
    "    }\n"
    "  } else {\n"
    "    r = top_color & 31;\n"
    "    g6 = ((top_color >> 4) & 62) | (top_color >> 15);\n"
    "    b = (top_color >> 10) & 31;\n"
    "  }\n"
    // set_window_fade: a window's BG0 pixels in the chosen tiles mixed with
    // the record beneath (its raw colour) at the chosen opacity.
    "  int cx = int(floor(v_screen.x + u_camera.x));\n"
    "  if (u_fade_columns != 0 && top_layer == 0 && cx >= 0 && cx < 240 &&\n"
    "      reg_row >= u_fade_row0 * 8 && reg_row < (u_fade_row1 + 1) * 8 &&\n"
    "      ((u_fade_columns >> (cx >> 3)) & 1) != 0) {\n"
    "    float a = clamp(u_fade_opacity, 0.0, 1.0);\n"
    "    int sr = second_color & 31;\n"
    "    int sg6 = ((second_color >> 4) & 62) | (second_color >> 15);\n"
    "    int sb = (second_color >> 10) & 31;\n"
    "    r = int(round(mix(float(sr), float(r), a)));\n"
    "    g6 = int(round(mix(float(sg6), float(g6), a)));\n"
    "    b = int(round(mix(float(sb), float(b), a)));\n"
    "  }\n"
    // set_window_red: the CPU rule (save_files::redden) on the 8-bit colour
    // this pixel is stored as.
    "  int cy = int(floor(v_screen.y + u_camera.y));\n"
    "  if (u_red_rows != 0 && cx >= u_red_x0 && cx < u_red_x1 &&\n"
    "      cy >= u_red_y0 && cy < u_red_y1 &&\n"
    "      ((u_red_rows >> ((cy - u_red_y0) / 16)) & 1) != 0) {\n"
    "    int pr = int(round(float(r) * 255.0 / 31.0));\n"
    "    int pg = int(round(float(g6 >> 1) * 255.0 / 31.0));\n"
    "    int pb = int(round(float(b) * 255.0 / 31.0));\n"
    "    if (pb > pr + 16 && pb > pg) {\n"
    "      o_colour = vec4(float(pb) / 255.0, vec2(float(pg / 3) / 255.0), 1.0);\n"
    "      return;\n"
    "    }\n"
    "  }\n"
    "  o_colour = vec4(float(r) / 31.0, float(g6 >> 1) / 31.0,\n"
    "                  float(b) / 31.0, 1.0);\n"
    "}\n";

// The window pass: one full-screen quad computing GBATEK's window_control(x)
// for every pixel and packing it into the red channel (0..63/255), read back
// by the peel and resolve passes above. The priority is WIN0 > WIN1 > object
// window > outside, exactly as gba_ppu.cpp's window_control has it. With no
// window enabled, every layer and blending are on (0x3F). Window rectangles
// are the console's own
// 240x160 space, so this reads the pixel through the same camera offset as
// the background and object passes.
//
// The window registers (and DISPCNT's own window-enable bits) are read per
// row from `u_row_io`, not a frame-level uniform: this is what lets a frame
// that rewrites WIN0H every scanline -- the room-transition iris, carving
// its shrinking circle -- draw correctly. A pixel outside the console's own
// 0..159 rows (a widened output's top/bottom margin) has no real scanline of
// its own; it reads the nearest real row's registers, the same extension
// the background shader's `reg_row` clamp uses. Because this pass covers the
// WHOLE output including the left/right and top/bottom margins, and the
// peel/resolve passes discard or blank a layer wherever this control byte
// says to, the transition's own WINOUT rule -- which during an iris turns
// every layer off outside the circle -- reaches every margin pixel exactly
// as it reaches the console's own 240x160, independent of whether the room
// buffer could otherwise have supplied that pixel.
const char* kWindowFragment =
    "#version 130\n"
    "in vec2 v_screen;\n"
    "uniform sampler2D u_obj_window_mask;\n"
    "uniform usampler2D u_row_io;\n"    // this frame's per-row registers
    "uniform vec2 u_camera;\n"
    "uniform int u_reveal_full;\n"     // Better Field Psy: Reveal fills the view
    // The iris wipe's ellipse: 1 fitted to the console's rows
    // (fit_iris_window), used outside its screen only; 2 the game's own
    // (SceneMargins::iris), used everywhere, since the game's rows break
    // once the expanded camera puts the centre off the console's screen
    // (The Lost Age: WIN0 rows open above it). Centre in
    // console pixels, squared half-axes.
    "uniform int u_iris;\n"
    "uniform vec2 u_iris_centre;\n"
    "uniform vec2 u_iris_axes;\n"
    "uniform int u_has_obj_window;\n"  // 1: obj_window_mask was drawn this frame
    "out vec4 o_colour;\n"
    "\n"
    "uint row_u16(int row, int offset) {\n"
    "  return gsr_raw_fetch(u_row_io, ivec2(offset >> 1, row), 0).r;\n"
    "}\n"
    "\n"
// GBATEK: WINnV is [Y1,Y2); Y2>160 or Y1>Y2 forces Y2=160.
    //
    // An edge sitting exactly ON the console's screen boundary means the
    // window continues past it. That is not a guess about intent, it is what
    // the effect is: Golden Sun's curtain wipe alternates WIN0H between
    // x=0..75 on even rows and x=165..240 on odd rows (measured), so every band has one edge pinned to a screen
    // edge and is visibly sliding OFF-screen. A console cannot express "and
    // onwards" any other way. Clamping the band to 0..239 in a widened view
    // stops the wipe dead at the old screen edge and leaves the margins black.
    //
    // Inside the console's own 0..239 / 0..159 this changes nothing: an
    // opened edge only ever admits coordinates that do not exist on hardware.
    "bool win_v(int reg, int y) {\n"
    "  int y1 = (reg >> 8) & 0xFF;\n"
    "  int y2 = reg & 0xFF;\n"
    "  if (y2 > 160 || y1 > y2) y2 = 160;\n"
    "  if (y1 == y2) return false;\n"
    "  bool top_open = y1 == 0;\n"
    "  bool bottom_open = y2 >= 160;\n"
    "  return (top_open || y >= y1) && (bottom_open || y < y2);\n"
    "}\n"
    "\n"
    // GBATEK: WINnH is [X1,X2); X2>240 or X1>X2 forces X2=240. Same
    // open-edge rule as win_v above.
    // An EMPTY window matches nothing, and must not be opened outward.
    // WIN1H reads 0x0000 on Tret, which is
    // x1 == x2 == 0: zero pixels wide on hardware. An open-edge rule alone
    // would see x1 == 0, call the left edge open, and reduce the test to
    // "x < 0" -- which is exactly the left margin and nothing else. The
    // margin would then fall INSIDE win1 and take its control byte instead of
    // WINOUT, switching colour blending on out there while the rest of the
    // picture has it off (a pale left strip: measured +19.1 brighter than
    // the centre on 116 ordinary rows and +28.8 on 44 accumulator rows).
    //
    // Inside the console's own 0..239 / 0..159 both forms
    // agree (both reject every real coordinate).
    // Clamping as mGBA does (gba_ppu.cpp win_h_in): X1 past the screen and
    // past X2 reads as 0, X2 past the screen clamps, and X1 > X2 wraps --
    // the Boreas summon's WIN0H 0xFFF1 is the full width. A wrapping window reaches both margins.
    "bool win_h(int reg, int x) {\n"
    "  int x1 = (reg >> 8) & 0xFF;\n"
    "  int x2 = reg & 0xFF;\n"
    "  if (x1 > 240 && x1 > x2) x1 = 0;\n"
    "  if (x2 > 240) { x2 = 240; if (x1 > 240) x1 = 240; }\n"
    "  if (x1 > x2) return x >= x1 || x < x2;\n"
    "  if (x1 == x2) return false;\n"
    "  bool left_open = x1 == 0;\n"
    "  bool right_open = x2 >= 240;\n"
    "  return (left_open || x >= x1) && (right_open || x < x2);\n"
    "}\n"
    "\n"
    "bool iris_inside(ivec2 p) {\n"
    "  vec2 d = vec2(p) + 0.5 - u_iris_centre;\n"
    "  return d.x * d.x / u_iris_axes.x + d.y * d.y / u_iris_axes.y <= 1.0;\n"
    "}\n"
    "\n"
    "void main() {\n"
    "  ivec2 screen = ivec2(floor(v_screen + u_camera));\n"
    "  int reg_row = clamp(screen.y, 0, 159);\n"
    "  uint dispcnt = row_u16(reg_row, 0x00);\n"
    "  bool win0_en = ((dispcnt >> 13) & 1u) != 0u;\n"
    "  bool win1_en = ((dispcnt >> 14) & 1u) != 0u;\n"
    "  bool objwin_en = ((dispcnt >> 15) & 1u) != 0u;\n"
    "  if (!win0_en && !win1_en && !objwin_en) {\n"
    "    o_colour = vec4(float(0x3F) / 255.0, 0.0, 0.0, 1.0);\n"
    "    return;\n"
    "  }\n"
    "  bool outside_native = screen.x < 0 || screen.x >= 240 ||\n"
    "                        screen.y < 0 || screen.y >= 160;\n"
    // Reveal: WIN0 carves the circle and WIN1 the Psynergy meter, both
    // showing everything (WININ 0x3F3F), outside only BG0 (WINOUT 0x0001).
    "  if ((dispcnt & 7u) == 0u && win0_en && win1_en && !objwin_en) {\n"
    "    int winin = int(row_u16(reg_row, 0x48));\n"
    "    int winout = int(row_u16(reg_row, 0x4A));\n"
    "    if (winin == 0x3F3F && (winout & 0xFF) == 0x01) {\n"
    "      if (u_reveal_full != 0) {\n"
    "        o_colour = vec4(float(winin & 0x3F) / 255.0, 0.0, 0.0, 1.0);\n"
    "        return;\n"
    "      }\n"
    "      if (outside_native) {\n"
    "        o_colour = vec4(float(winout & 0x3F) / 255.0, 0.0, 0.0, 1.0);\n"
    "        return;\n"
    "      }\n"
    "    }\n"
    "  }\n"
    "  int control;\n"
    "  if (win0_en && (u_iris == 2 || (u_iris != 0 && outside_native)\n"
    "                 ? iris_inside(screen)\n"
    "                 : (win_v(int(row_u16(reg_row, 0x44)), screen.y) &&\n"
    "                    win_h(int(row_u16(reg_row, 0x40)), screen.x)))) {\n"
    "    control = int(row_u16(reg_row, 0x48)) & 0x3F;\n"
    "  } else if (win1_en && win_v(int(row_u16(reg_row, 0x46)), screen.y) &&\n"
    "             win_h(int(row_u16(reg_row, 0x42)), screen.x)) {\n"
    "    control = (int(row_u16(reg_row, 0x48)) >> 8) & 0x3F;\n"
    "  } else if (u_has_obj_window != 0 && objwin_en &&\n"
    "             gsr_raw_fetch(u_obj_window_mask, ivec2(gl_FragCoord.xy), 0).r"
    " > 0.5) {\n"
    "    control = (int(row_u16(reg_row, 0x4A)) >> 8) & 0x3F;\n"
    "  } else {\n"
    "    control = int(row_u16(reg_row, 0x4A)) & 0x3F;\n"
    "  }\n"
    "  o_colour = vec4(float(control) / 255.0, 0.0, 0.0, 1.0);\n"
    "}\n";

// Screen-space projection over the output, y downward, column-major.
// The z row must map depth 0..1 (the sort key, scaled) onto a clip-space z
// that INCREASES with depth, because the depth test that makes the two-layer
// peel work relies on GL's default GL_LESS: a smaller stored depth wins,
// which must mean a smaller sort key. Depth is unused elsewhere in this file
// (always the vertex default of 0), so this row is not exercised elsewhere.
void ortho(float* m, float width, float height) {
    std::memset(m, 0, sizeof(float) * 16);
    m[0]  =  2.0f / width;
    m[5]  = -2.0f / height;
    m[10] =  1.0f;
    m[12] = -1.0f;
    m[13] =  1.0f;
    m[15] =  1.0f;
}

}  // namespace

FieldSceneRenderer::~FieldSceneRenderer() {
    if (!surface_) return;
    if (background_) surface_->destroy_program(background_);
    if (background_regular_) surface_->destroy_program(background_regular_);
    if (object_) surface_->destroy_program(object_);
    if (resolve_) surface_->destroy_program(resolve_);
    if (solid_) surface_->destroy_program(solid_);
    if (shake_blit_) surface_->destroy_program(shake_blit_);
    if (shake_target_) surface_->destroy_texture(shake_target_);
    if (flicker_mix_) surface_->destroy_program(flicker_mix_);
    if (flicker_this_) surface_->destroy_texture(flicker_this_);
    if (flicker_other_) surface_->destroy_texture(flicker_other_);
    if (flat_rows_prog_) surface_->destroy_program(flat_rows_prog_);
    if (flat_all_prog_) surface_->destroy_program(flat_all_prog_);
    if (world_compose_prog_) surface_->destroy_program(world_compose_prog_);
    if (compose_src_) surface_->destroy_texture(compose_src_);
    if (compose_rows_) surface_->destroy_texture(compose_rows_);
    if (compose_flat_) surface_->destroy_texture(compose_flat_);
    if (world_margin_tex_) surface_->destroy_texture(world_margin_tex_);
    if (flat_world_map_) surface_->destroy_texture(flat_world_map_);
    if (obj_window_) surface_->destroy_program(obj_window_);
    if (window_) surface_->destroy_program(window_);
    if (vram_) surface_->destroy_texture(vram_);
    if (room_vram_) surface_->destroy_texture(room_vram_);
    if (effect_canvas_) surface_->destroy_texture(effect_canvas_);
    if (hold_strip_) surface_->destroy_texture(hold_strip_);
    if (palette_) surface_->destroy_texture(palette_);
    if (id_grid_) surface_->destroy_texture(id_grid_);
    if (atlas_) surface_->destroy_texture(atlas_);
    for (auto t : row_io_)
        if (t) surface_->destroy_texture(t);
    for (auto t : row_affine_)
        if (t) surface_->destroy_texture(t);
    if (world_map_) surface_->destroy_texture(world_map_);
    if (world_cell_art_) surface_->destroy_texture(world_cell_art_);
    if (world_art_) surface_->destroy_texture(world_art_);
    if (top_) surface_->destroy_texture(top_);
    if (second_) surface_->destroy_texture(second_);
    if (obj_window_mask_) surface_->destroy_texture(obj_window_mask_);
    if (window_control_) surface_->destroy_texture(window_control_);
}

bool FieldSceneRenderer::init(gbarecomp::GpuSurface* surface) {
    if (ready_) return true;
    if (!surface || !surface->ready()) {
        failure_ = "the graphics surface is not available";
        return false;
    }
    surface_ = surface;

    char log[2048] = {};
    background_ = surface_->create_program(kBackgroundVertex,
                                           kBackgroundFragment,
                                           log, sizeof log);
    if (!background_) {
        std::fprintf(stderr, "field_scene_renderer: background shader: %s\n",
                     log);
        failure_ = "the background shader did not compile on this driver";
        return false;
    }
    // Lean regular-BG variant compiled from the exact same source without
    // affine / world-map code, reducing GPR pressure on mobile GPUs.
    {
        std::string reg_src = kBackgroundFragment;
        static const char kVer[] = "#version 130\n";
        if (reg_src.compare(0, sizeof(kVer) - 1, kVer) == 0)
            reg_src.insert(sizeof(kVer) - 1, "#define GSR_BG_REGULAR_ONLY 1\n");
        background_regular_ = surface_->create_program(
            kBackgroundVertex, reg_src.c_str(), log, sizeof log);
        if (!background_regular_) {
            std::fprintf(stderr, "field_scene_renderer: regular background shader: %s\n",
                         log);
            failure_ = "the regular background shader did not compile on this driver";
            return false;
        }
    }

    const std::string object_fragment = std::string(kObjectSampling) + kObjectFragment;
    object_ = surface_->create_program(kObjectVertex, object_fragment.c_str(),
                                       log, sizeof log);
    if (!object_) {
        std::fprintf(stderr, "field_scene_renderer: object shader: %s\n", log);
        failure_ = "the object shader did not compile on this driver";
        return false;
    }

    resolve_ = surface_->create_program(kResolveVertex, kResolveFragment,
                                        log, sizeof log);
    if (!resolve_) {
        std::fprintf(stderr, "field_scene_renderer: resolve shader: %s\n", log);
        failure_ = "the resolve shader did not compile on this driver";
        return false;
    }

    const std::string object_window_fragment = std::string(kObjectSampling) + kObjectWindowFragment;
    obj_window_ = surface_->create_program(kObjectVertex, object_window_fragment.c_str(),
                                           log, sizeof log);
    if (!obj_window_) {
        std::fprintf(stderr, "field_scene_renderer: object window shader: %s\n",
                     log);
        failure_ = "the object window shader did not compile on this driver";
        return false;
    }

    window_ = surface_->create_program(kBackgroundVertex, kWindowFragment,
                                       log, sizeof log);
    if (!window_) {
        std::fprintf(stderr, "field_scene_renderer: window shader: %s\n", log);
        failure_ = "the window shader did not compile on this driver";
        return false;
    }

    vram_ = surface_->create_texture(kVramTextureWidth,
                                     kVramBytes / kVramTextureWidth,
                                     gbarecomp::GpuTextureFormat::R8UI, false);
    room_vram_ = surface_->create_texture(kVramTextureWidth,
                                          kVramBytes / kVramTextureWidth,
                                          gbarecomp::GpuTextureFormat::R8UI,
                                          false);
    // Row 0 is the live palette; row 1 what room tiles in the margins use,
    // the field's colours from before a menu replaced some of them
    // (upload_room_palette). Row 1 follows row 0 whenever no menu is open.
    // Rows 2 onward are the world map's other artwork sets
    // (upload_world_cell_art).
    palette_ = surface_->create_texture(kPaletteEntries, 2 + kWorldArtSets,
                                        gbarecomp::GpuTextureFormat::R16UI,
                                        false);
    id_grid_ = surface_->create_texture(kGridSide, kGridSide,
                                        gbarecomp::GpuTextureFormat::R16UI,
                                        false);
    atlas_ = surface_->create_texture(4, kAtlasIds,
                                      gbarecomp::GpuTextureFormat::R16UI,
                                      false);
    static_assert(FieldScene::kLineIoBytes % 2 == 0,
                  "kLineIoBytes must be even for R16UI row_io");
    bool row_tex_ok = true;
    for (int i = 0; i < kRowRing; ++i) {
        row_io_[i] = surface_->create_texture(
            static_cast<int>(FieldScene::kLineIoBytes / 2),
            static_cast<int>(FieldScene::kRows),
            gbarecomp::GpuTextureFormat::R16UI, false);
        // Eight 16-bit halves per row: BG2 x, BG2 y, BG3 x, BG3 y, low then high.
        row_affine_[i] = surface_->create_texture(
            8, static_cast<int>(FieldScene::kRows),
            gbarecomp::GpuTextureFormat::R16UI, false);
        if (!row_io_[i] || !row_affine_[i]) row_tex_ok = false;
    }
    // Our spell-effect canvas. Sized for the widest view we present, so the
    // sparks the game clips at its own canvas edge have somewhere to land.
    effect_pixels_.reset(kEffectCanvasWidth, kEffectCanvasHeight);
    effect_canvas_ = surface_->create_texture(
        kEffectCanvasWidth, kEffectCanvasHeight,
        gbarecomp::GpuTextureFormat::R8UI, false);
    if (effect_canvas_) {
        surface_->update_texture(effect_canvas_, 0, 0,
                                 kEffectCanvasWidth, kEffectCanvasHeight,
                                 effect_pixels_.pixels.data());
        uploaded_effect_pixels_ = effect_pixels_.pixels;
    }
    // Per-row off-screen strip shares for battle hold_edge layers (2 x H R8UI).
    hold_strip_h_ = 512;
    hold_strip_ = surface_->create_texture(
        2, hold_strip_h_, gbarecomp::GpuTextureFormat::R8UI, false);
    if (hold_strip_) {
        hold_strip_buf_.assign(static_cast<std::size_t>(hold_strip_h_) * 2u, 0u);
        surface_->update_texture(hold_strip_, 0, 0, 2, hold_strip_h_,
                                 hold_strip_buf_.data());
        uploaded_hold_strip_.clear();
    }
    // The world map's whole tile layout, both layers (world_map_source.h):
    // the game's side in tiles wide, BG3 then BG2 stacked.
    world_map_ = surface_->create_texture(prof().world_map_side_tiles,
                                          2 * prof().world_map_side_tiles,
                                          gbarecomp::GpuTextureFormat::R8UI,
                                          false);
    world_cell_art_ = surface_->create_texture(prof().world_map_side_tiles / 2,
                                               prof().world_map_side_tiles / 2,
                                               gbarecomp::GpuTextureFormat::R8UI,
                                               false);
    world_art_ = surface_->create_texture(kVramTextureWidth,
                                          kWorldArtSets * 0x8000 / kVramTextureWidth,
                                          gbarecomp::GpuTextureFormat::R8UI,
                                          false);
    if (!vram_ || !palette_ || !id_grid_ || !atlas_ || !row_tex_ok ||
        !effect_canvas_ || !hold_strip_ || !world_map_ ||
        !world_cell_art_ || !world_art_) {
        failure_ = "could not create the video memory, palette, room "
                  "buffer or per-row register textures";
        return false;
    }

    ready_ = true;
    failure_ = nullptr;
    return true;
}

bool FieldSceneRenderer::set_output_size(int width, int height) {
    if (!ready_ || !surface_) return false;
    if (!surface_->set_target_size(width, height)) return false;
    if (prof().margin_scroll_rows && row_io_rows_ != FieldScene::kRows + height) {
        gbarecomp::GpuTexture next[kRowRing] = {};
        bool ok = true;
        for (int i = 0; i < kRowRing; ++i) {
            next[i] = surface_->create_texture(
                static_cast<int>(FieldScene::kLineIoBytes / 2),
                FieldScene::kRows + height,
                gbarecomp::GpuTextureFormat::R16UI, false);
            if (!next[i]) ok = false;
        }
        if (!ok) {
            for (auto t : next)
                if (t) surface_->destroy_texture(t);
            return false;
        }
        for (int i = 0; i < kRowRing; ++i) {
            if (row_io_[i]) surface_->destroy_texture(row_io_[i]);
            row_io_[i] = next[i];
        }
        row_io_rows_ = FieldScene::kRows + height;
        uploaded_row_io_.clear();
    }
    if (width != output_w_ || height != output_h_ || !top_ || !second_ ||
        !obj_window_mask_ || !window_control_) {
        if (top_) { surface_->destroy_texture(top_); top_ = 0; }
        if (second_) { surface_->destroy_texture(second_); second_ = 0; }
        if (obj_window_mask_) {
            surface_->destroy_texture(obj_window_mask_);
            obj_window_mask_ = 0;
        }
        if (window_control_) {
            surface_->destroy_texture(window_control_);
            window_control_ = 0;
        }
        top_ = surface_->create_texture(width, height,
                                        gbarecomp::GpuTextureFormat::RGBA8,
                                        false);
        second_ = surface_->create_texture(width, height,
                                           gbarecomp::GpuTextureFormat::RGBA8,
                                           false);
        obj_window_mask_ = surface_->create_texture(
            width, height, gbarecomp::GpuTextureFormat::RGBA8, false);
        window_control_ = surface_->create_texture(
            width, height, gbarecomp::GpuTextureFormat::RGBA8, false);
        if (!top_ || !second_ || !obj_window_mask_ || !window_control_)
            return false;
    }
    if (height > hold_strip_h_) {
        const gbarecomp::GpuTexture next = surface_->create_texture(
            2, height, gbarecomp::GpuTextureFormat::R8UI, false);
        if (!next) return false;
        if (hold_strip_) surface_->destroy_texture(hold_strip_);
        hold_strip_ = next;
        hold_strip_h_ = height;
        hold_strip_buf_.assign(static_cast<std::size_t>(hold_strip_h_) * 2u, 0u);
        surface_->update_texture(hold_strip_, 0, 0, 2, hold_strip_h_,
                                 hold_strip_buf_.data());
        uploaded_hold_strip_.clear();
    }
    output_w_ = width;
    output_h_ = height;
    return true;
}

namespace {
// Uploads only the changed row span of a kVramTextureWidth-wide R8UI texture.
void upload_dirty_vram_rows(gbarecomp::GpuSurface* surface,
                            gbarecomp::GpuTexture tex,
                            const std::uint8_t* src, int rows,
                            std::vector<std::uint8_t>& shadow) {
    const std::size_t bytes =
        static_cast<std::size_t>(rows) * static_cast<std::size_t>(kVramTextureWidth);
    if (shadow.size() != bytes) {
        surface->update_texture(tex, 0, 0, kVramTextureWidth, rows, src);
        shadow.assign(src, src + bytes);
        return;
    }
    int first = -1, last = -1;
    for (int r = 0; r < rows; ++r) {
        const std::size_t off =
            static_cast<std::size_t>(r) * static_cast<std::size_t>(kVramTextureWidth);
        if (std::memcmp(shadow.data() + off, src + off, kVramTextureWidth) != 0) {
            if (first < 0) first = r;
            last = r;
        }
    }
    if (first >= 0) {
        const std::size_t off =
            static_cast<std::size_t>(first) * static_cast<std::size_t>(kVramTextureWidth);
        const int span = last - first + 1;
        surface->update_texture(tex, 0, first, kVramTextureWidth, span, src + off);
        std::memcpy(shadow.data() + off, src + off,
                    static_cast<std::size_t>(span) * kVramTextureWidth);
    }
}
}  // namespace

void FieldSceneRenderer::upload_vram(const std::uint8_t* vram,
                                     std::size_t vram_bytes) {
    if (!ready_ || !vram || vram_bytes == 0) return;
    const int rows = static_cast<int>(
        std::min<std::size_t>(vram_bytes, kVramBytes) / kVramTextureWidth);
    if (rows > 0) {
        upload_dirty_vram_rows(surface_, vram_, vram, rows, uploaded_vram_);
        room_vram_mirrors_vram_ = true;
    }
    // Background map blocks only (the first 64 KB): room_layer_agreement()
    // compares the room rule's entries against them.
    vram_maps_.assign(vram, vram + std::min<std::size_t>(vram_bytes, 0x10000));
    // Object tiles (32 KB, then the second page when sent): object_is_solid()
    // reads them.
    if (vram_bytes > 0x10000)
        vram_objects_.assign(vram + 0x10000,
                             vram + std::min<std::size_t>(vram_bytes, kVramBytes));
    else
        vram_objects_.clear();
}

// Whether every pixel of an unrotated object is the same colour index, read
// the way kObjectFragment reads it. A cutscene bar is one solid tile; the
// Sol Sanctum cloud row is not.
bool FieldSceneRenderer::object_is_solid(const SceneObject& O,
                                         bool one_dimensional) const {
    if (vram_objects_.size() < 0x8000 || O.width <= 0 || O.height <= 0)
        return false;
    const bool eight_bit = O.palette < 0;
    const int step = eight_bit ? 2 : 1;
    const int tiles_wide = O.width >> 3;
    int first = -1;
    for (int ly = 0; ly < O.height; ++ly) {
        for (int lx = 0; lx < O.width; ++lx) {
            const int tile_x = lx >> 3, tile_y = ly >> 3;
            int tile = one_dimensional
                ? static_cast<int>(O.tile) + (tile_y * tiles_wide + tile_x) * step
                : static_cast<int>(O.tile) + tile_y * 32 + tile_x * step;
            tile = static_cast<int>(O.tile & 0x400u) | (tile & 0x3FF);
            const int base = tile * 32;
            const int tx = lx & 7, ty = ly & 7;
            const std::size_t at = static_cast<std::size_t>(
                eight_bit ? base + ty * 8 + tx : base + ty * 4 + (tx >> 1));
            if (at >= vram_objects_.size()) return false;
            int index;
            if (eight_bit) {
                index = vram_objects_[at];
            } else {
                const int two = vram_objects_[at];
                index = (tx & 1) ? (two >> 4) : (two & 0xF);
            }
            if (first < 0) first = index;
            else if (index != first) return false;
        }
    }
    return true;
}

// Only past the grid's far edge: a source left of or above 0 is before a
// layer's art, not around the grid. Wrapped, Jupiter Lighthouse's clouds
// (The Lost Age map 264, BG3 HOFS 90) would read the grid's last
// columns, another layer's art, into the left margin.
static int room_grid_pixel(int pixel) {
    return prof().room_grid_wrap && pixel >= 0 ? (pixel & (kGridSide * 16 - 1)) : pixel;
}

bool FieldSceneRenderer::room_entry(int screen_x, int screen_y, int scroll_x,
                                    int scroll_y, std::uint16_t* entry) const {
    if (!query_room_known(screen_x, screen_y, scroll_x, scroll_y)) return false;
    const int stx = room_grid_pixel(scroll_x + screen_x) >> 3;
    const int sty = room_grid_pixel(scroll_y + screen_y) >> 3;
    const std::uint16_t id =
        room_ids_[static_cast<std::size_t>(sty >> 1) * kGridSide + (stx >> 1)];
    *entry = room_atlas_[static_cast<std::size_t>(id) * 4u +
                         static_cast<std::size_t>((sty & 1) * 2 + (stx & 1))];
    return true;
}

// How often the room rule gives the tile entry the console itself shows,
// over the centre of every tile inside the 240x160 window, for one regular
// layer. Inside the window both answers exist, so this is a direct check of
// the rule for THIS room and layer on THIS frame. -1 when fewer than
// kRoomCheckMinTiles tiles could be compared.
// Each sampled row uses its own scanline's BGxCNT/HOFS/VOFS, as the shader
// does. The end-of-frame register copy is not the picture's: in Lamakan
// Desert the heat haze leaves BG1 at (0, n) with n counting up each frame
// (measured), so a check made from it fails on most frames and the margins
// would flicker between desert and black.
int FieldSceneRenderer::room_layer_agreement(const FieldScene& scene, int bg,
                                             int* compared,
                                             bool from_record) const {
    if (compared) *compared = 0;
    const SceneLayer& L = scene.layers[bg];
    if (!room_valid_ || L.affine || vram_maps_.empty()) return -1;
    int total = 0, same = 0;
    // Rows showing another copy of the area are not the room's own tiles;
    // the margins read the record for them instead (row_split_vertical).
    const bool split = prof().kolima_split_rows && row_split_vertical(scene, bg);
    for (int y = 4; y < 160; y += 8) {
        if (split && row_jumps_vertically(scene, bg, y)) continue;
        const std::uint8_t* io = scene.registers_for_row(y);
        auto rd = [&](std::size_t at) {
            return static_cast<int>(io[at] | (io[at + 1] << 8));
        };
        const int bgcnt = rd(0x08u + static_cast<std::size_t>(bg) * 2u);
        const int scroll_x = rd(0x10u + static_cast<std::size_t>(bg) * 4u);
        const int scroll_y = rd(0x12u + static_cast<std::size_t>(bg) * 4u);
        const int size_code = (bgcnt >> 14) & 3;
        const std::size_t screen_base =
            static_cast<std::size_t>((bgcnt >> 8) & 31) * 2048u;
        const int w = (size_code & 1) ? 64 : 32;   // map size in tiles
        const int h = (size_code & 2) ? 64 : 32;
        // Same fold as the shader: the layer's own position + the row's
        // offset from it in -512..511.
        const LayerWrap& rec = layer_wrap_[bg];
        const int room_sx = !from_record ? scroll_x
            : rec.pos_x + (((scroll_x - rec.pos_x + 512) & 1023) - 512);
        const int room_sy = !from_record ? scroll_y
            : rec.pos_y + (((scroll_y - rec.pos_y + 512) & 1023) - 512);
        // The Lost Age: an auto-scrolling layer wraps inside its region, as in
        // the shader (u_room_wrap). Madra's rays (BG1, region y 1024, scroll y
        // 951) read the grid above their region on the top 73 rows and agreed
        // 55%, so the margins would drop them on such frames.
        const bool wrap = prof().agreement_autoscroll_wrap;
        const int sy = wrap && rec.wrap_y > 0
            ? rec.region_y + ((room_sy + y - rec.region_y) & (rec.wrap_y - 1)) - y
            : room_sy;
        for (int x = 4; x < 240; x += 8) {
            const int sx = wrap && rec.wrap_x > 0
                ? rec.region_x + ((room_sx + x - rec.region_x) & (rec.wrap_x - 1)) - x
                : room_sx;
            std::uint16_t want = 0;
            if (!room_entry(x, y, sx, sy, &want)) continue;
            const int tx = (((x + scroll_x) & 511) >> 3) & (w - 1);
            const int ty = (((y + scroll_y) & 511) >> 3) & (h - 1);
            int block = 0;
            if (w == 64 && tx >= 32) block += 1;
            if (h == 64 && ty >= 32) block += (w == 64) ? 2 : 1;
            const std::size_t at = screen_base + static_cast<std::size_t>(block) * 2048u +
                static_cast<std::size_t>(((ty & 31) * 32 + (tx & 31)) * 2);
            if (at + 1 >= vram_maps_.size()) continue;
            const std::uint16_t have = static_cast<std::uint16_t>(
                vram_maps_[at] | (vram_maps_[at + 1] << 8));
            ++total;
            if (have == want) ++same;
        }
    }
    if (compared) *compared = total;
    if (total < kRoomCheckMinTiles) return -1;
    return (same * 100) / total;
}

// Normally a layer's unmasked scroll names its region of the 128x128 grid
// (BG2 at +1024 x, BG3 at +1024 y, and so on). Lamakan Desert breaks that:
// its heat-haze rows hand BG1 and BG3 each other's +1024, so the scroll's
// own region matches 0% of the console's tiles. The game's own record of the
// layer's position (profile layer_record_addr, region included) plus the
// row's small offset from it (the haze, -1 as 0xFFFF at a left edge) matches.
// A search of four fixed 1024-px regions around the camera cannot express
// regions such as the ship's 960, so the scroll's own region is tried first.
// Every room it already fits is untouched.
// A row's offset from layer `bg`'s recorded position, folded into
// -512..511 the way the shader folds it.
static void row_offset_from_record(const FieldScene& scene, int bg, int y,
                                   int pos_x, int pos_y, int* dx, int* dy) {
    const std::uint8_t* io = scene.registers_for_row(y);
    const std::size_t at = 0x10u + static_cast<std::size_t>(bg) * 4u;
    const int hofs = io[at] | (io[at + 1] << 8);
    const int vofs = io[at + 2] | (io[at + 3] << 8);
    *dx = ((hofs - pos_x + 512) & 1023) - 512;
    *dy = ((vofs - pos_y + 512) & 1023) - 512;
}

bool FieldSceneRenderer::row_jumps_vertically(const FieldScene& scene, int bg,
                                              int y) const {
    const LayerWrap& rec = layer_wrap_[bg];
    if (!rec.have_record) return false;
    int dx = 0, dy = 0;
    row_offset_from_record(scene, bg, y, rec.pos_x, rec.pos_y, &dx, &dy);
    return std::abs(dx) <= kRowNearRecord && std::abs(dy) > kRowNearRecord;
}

// Measured over 3,623 mode 0 frames: rows
// sit within 9 px of the record (heat haze, a frame's lag), or the whole
// frame is offset, or -- Tret's sky only -- some rows jump sideways. A
// vertical jump on some rows while others sit on the record is Kolima's
// flood alone (+176).
bool FieldSceneRenderer::row_split_vertical(const FieldScene& scene,
                                            int bg) const {
    const LayerWrap& rec = layer_wrap_[bg];
    if (!rec.have_record || scene.video_mode != 0) return false;
    bool near = false, jump = false;
    for (int y = 0; y < FieldScene::kRows; ++y) {
        int dx = 0, dy = 0;
        row_offset_from_record(scene, bg, y, rec.pos_x, rec.pos_y, &dx, &dy);
        if (std::abs(dx) > kRowNearRecord) continue;
        if (std::abs(dy) <= kRowNearRecord) near = true;
        else jump = true;
    }
    return near && jump;
}

// Kolima's drain and refill (map 43): the jumping rows show the other state of the pond. The room
// grid holds that state for the whole room 512 px left of the layer's own
// region, and 1024 px down (drain) or 512 px down (refill). The bottom layer
// is the same in both states (offset 0).
//
// At other moments of the same drain, the grid holds that state at the jump's
// own rows instead: the layer's position plus the jump (+176). There all
// three layers agree 95/90/83% on the jumping rows, while none of the 512-px
// offsets agree above 63% for BG2 and BG3. So both families of offsets are tried.
//
// Every jumping row is compared, not every 8th: the drain's stripes are a few
// rows each, and sampling every 8th row saw two of them there.
//
// The function matches the console's tiles on the jumping rows against each
// offset and takes the best one, if it agrees well enough.
bool FieldSceneRenderer::split_row_offset(const FieldScene& scene, int bg,
                                          int* off_x, int* off_y) const {
    const LayerWrap& rec = layer_wrap_[bg];
    if (!room_valid_ || vram_maps_.empty() || !rec.have_record) return false;
    int jump_dy = 0;
    for (int y = 0; y < FieldScene::kRows && jump_dy == 0; ++y) {
        if (!row_jumps_vertically(scene, bg, y)) continue;
        int dx = 0;
        row_offset_from_record(scene, bg, y, rec.pos_x, rec.pos_y, &dx, &jump_dy);
    }
    int best = -1, best_x = 0, best_y = 0, best_total = 0;
    for (int candidate = 0; candidate < 14; ++candidate) {
        const int oy = -1536 + (candidate % 7) * 512 + (candidate >= 7 ? jump_dy : 0);
        if (candidate >= 7 && jump_dy == 0) break;
        for (int ox = -1536; ox <= 1536; ox += 512) {
            int total = 0, same = 0;
            for (int y = 0; y < 160; ++y) {
                if (!row_jumps_vertically(scene, bg, y)) continue;
                const std::uint8_t* io = scene.registers_for_row(y);
                auto rd = [&](std::size_t at) {
                    return static_cast<int>(io[at] | (io[at + 1] << 8));
                };
                const int bgcnt = rd(0x08u + static_cast<std::size_t>(bg) * 2u);
                const int scroll_x = rd(0x10u + static_cast<std::size_t>(bg) * 4u);
                const int scroll_y = rd(0x12u + static_cast<std::size_t>(bg) * 4u);
                const int size_code = (bgcnt >> 14) & 3;
                const std::size_t screen_base =
                    static_cast<std::size_t>((bgcnt >> 8) & 31) * 2048u;
                const int w = (size_code & 1) ? 64 : 32;
                const int h = (size_code & 2) ? 64 : 32;
                const int room_sx = rec.pos_x + ox +
                    (((scroll_x - rec.pos_x + 512) & 1023) - 512);
                const int room_sy = rec.pos_y + oy;
                for (int x = 4; x < 240; x += 8) {
                    std::uint16_t want = 0;
                    if (!room_entry(x, y, room_sx, room_sy, &want)) continue;
                    const int tx = (((x + scroll_x) & 511) >> 3) & (w - 1);
                    const int ty = (((y + scroll_y) & 511) >> 3) & (h - 1);
                    int block = 0;
                    if (w == 64 && tx >= 32) block += 1;
                    if (h == 64 && ty >= 32) block += (w == 64) ? 2 : 1;
                    const std::size_t at = screen_base +
                        static_cast<std::size_t>(block) * 2048u +
                        static_cast<std::size_t>(((ty & 31) * 32 + (tx & 31)) * 2);
                    if (at + 1 >= vram_maps_.size()) continue;
                    const std::uint16_t have = static_cast<std::uint16_t>(
                        vram_maps_[at] | (vram_maps_[at + 1] << 8));
                    ++total;
                    if (have == want) ++same;
                }
            }
            if (total < kRoomCheckMinTiles) continue;
            if (same > best) { best = same; best_x = ox; best_y = oy; best_total = total; }
        }
    }
    if (best < 0 || best * 100 < kRoomCheckMinAgreement * best_total) return false;
    *off_x = best_x;
    *off_y = best_y;
    return true;
}

bool FieldSceneRenderer::room_reference_for(const FieldScene& scene, int bg,
                                            bool* from_record) const {
    *from_record = false;
    const int own = room_layer_agreement(scene, bg, nullptr);
    if (own < 0 || own >= kRoomCheckMinAgreement) return true;
    if (!layer_wrap_[bg].have_record) return false;
    const int agree = room_layer_agreement(scene, bg, nullptr, true);
    if (agree < kRoomCheckMinAgreement) return false;
    *from_record = true;
    return true;
}

void FieldSceneRenderer::upload_world_map(const std::uint8_t* tiles,
                                          std::uint32_t generation) {
    if (!ready_) return;
    world_map_valid_ = tiles != nullptr;
    if (!tiles || generation == world_map_generation_) return;
    const int side = prof().world_map_side_tiles;
    surface_->update_texture(world_map_, 0, 0, side, 2 * side, tiles);
    world_tiles_.assign(tiles, tiles + side * side * 2);
    world_map_generation_ = generation;
}

void FieldSceneRenderer::upload_world_cell_art(const std::uint8_t* cells,
                                               std::uint32_t cells_generation,
                                               const std::uint8_t* art,
                                               const std::uint16_t* palettes,
                                               int sets,
                                               std::uint32_t art_generation) {
    world_art_sets_ = 0;
    if (!ready_ || !cells || !art || !palettes || sets <= 0 || sets > kWorldArtSets)
        return;
    if (art_generation != world_art_generation_) {
        surface_->update_texture(world_art_, 0, 0, kVramTextureWidth,
                                 sets * 0x8000 / kVramTextureWidth, art);
        world_art_generation_ = art_generation;
    }
    if (cells_generation != world_cells_generation_) {
        const int side = prof().world_map_side_tiles / 2;
        surface_->update_texture(world_cell_art_, 0, 0, side, side, cells);
        world_cells_generation_ = cells_generation;
    }
    surface_->update_texture(palette_, 0, 2, 256, sets, palettes);
    world_art_sets_ = sets;
}

// The whole-map decode trusts the game's piece-to-tile table at 0x02010000,
// and the pause menu reuses that memory one frame before it switches the
// screen to mode 0: the pieces still check out, the table does not, and the
// margins would come out as scrambled tiles that the menu then held. So compare the decode with what
// the console itself shows, as world_row in the shader samples it.
int FieldSceneRenderer::world_map_agreement(const FieldScene& scene,
                                            int* compared) const {
    return gbarecomp::world_map_agreement(world_tiles_, prof().world_map_side_tiles, vram_maps_,
        scene, kRoomCheckMinTiles, compared);
}

void FieldSceneRenderer::upload_room_vram(const std::uint8_t* vram,
                                          std::size_t vram_bytes) {
    if (!ready_ || !vram || vram_bytes == 0) return;
    const int rows = static_cast<int>(
        std::min<std::size_t>(vram_bytes, kVramBytes) / kVramTextureWidth);
    if (rows > 0) {
        const std::size_t bytes =
            static_cast<std::size_t>(rows) * static_cast<std::size_t>(kVramTextureWidth);
        pending_room_vram_.assign(vram, vram + bytes);
        room_vram_mirrors_vram_ = false;
    }
}

void FieldSceneRenderer::upload_palette(const std::uint16_t* palette,
                                        std::size_t entries) {
    if (!ready_ || !palette || entries == 0) return;
    const int width =
        static_cast<int>(std::min<std::size_t>(entries, kPaletteEntries));
    if (!uploaded_palette_valid_[0] ||
        std::memcmp(uploaded_palette_[0], palette,
                    static_cast<std::size_t>(width) * sizeof(std::uint16_t)) != 0) {
        surface_->update_texture(palette_, 0, 0, width, 1, palette);
        std::memcpy(uploaded_palette_[0], palette,
                    static_cast<std::size_t>(width) * sizeof(std::uint16_t));
        uploaded_palette_valid_[0] = true;
    }
    room_palette_mirrors_live_ = true;
    pending_room_palette_len_ = width;
}

void FieldSceneRenderer::upload_room_palette(const std::uint16_t* palette,
                                             std::size_t entries) {
    if (!ready_ || !palette || entries == 0) return;
    const int width =
        static_cast<int>(std::min<std::size_t>(entries, kPaletteEntries));
    std::memcpy(pending_room_palette_, palette,
                static_cast<std::size_t>(width) * sizeof(std::uint16_t));
    pending_room_palette_len_ = width;
    room_palette_mirrors_live_ = false;
}

void FieldSceneRenderer::sync_deferred_uploads() {
    if (room_vram_mirrors_vram_) {
        if (!uploaded_vram_.empty()) {
            const int rows = static_cast<int>(uploaded_vram_.size() / kVramTextureWidth);
            upload_dirty_vram_rows(surface_, room_vram_, uploaded_vram_.data(), rows,
                                   uploaded_room_vram_);
        }
    } else if (!pending_room_vram_.empty()) {
        const int rows = static_cast<int>(pending_room_vram_.size() / kVramTextureWidth);
        upload_dirty_vram_rows(surface_, room_vram_, pending_room_vram_.data(), rows,
                               uploaded_room_vram_);
    }
    const std::uint16_t* pal_src =
        room_palette_mirrors_live_ ? uploaded_palette_[0] : pending_room_palette_;
    const int pal_w = pending_room_palette_len_;
    if (pal_w > 0 && (!room_palette_mirrors_live_ || uploaded_palette_valid_[0])) {
        if (!uploaded_palette_valid_[1] ||
            std::memcmp(uploaded_palette_[1], pal_src,
                        static_cast<std::size_t>(pal_w) * sizeof(std::uint16_t)) != 0) {
            surface_->update_texture(palette_, 0, 1, pal_w, 1, pal_src);
            std::memcpy(uploaded_palette_[1], pal_src,
                        static_cast<std::size_t>(pal_w) * sizeof(std::uint16_t));
            uploaded_palette_valid_[1] = true;
        }
    }
}

void FieldSceneRenderer::set_room_source_enabled(bool enabled) {
    room_source_enabled_ = enabled;
}

void FieldSceneRenderer::set_effect_span_enabled(bool enabled) {
    effect_span_ = enabled;
}

void FieldSceneRenderer::set_effect_layer(int layer) {
    effect_have_sparks_ = false;
    effect_wrap_canvas_ = false;
    effect_text_canvas_ = false;
    effect_fill_ = 0;
    // Which layer, if any, is the spell effect this frame. The caller reads it
    // from the scene's own registers; nothing is decoded out of guest memory.
    effect_layer_ = effect_enabled_ ? layer : -1;
}

bool FieldSceneRenderer::upload_effect_sparks(const EffectSpark* sparks,
                                              int count,
                                              const std::uint8_t* art,
                                              std::size_t art_bytes,
                                              int effect_layer) {
    effect_have_sparks_ = false;
    effect_wrap_canvas_ = false;
    effect_text_canvas_ = false;
    effect_fill_ = 0;
    effect_layer_ = -1;
    if (!effect_enabled_ || !ready_ || !sparks || count <= 0 || !art ||
        effect_layer < 0) return false;

    // Keep the stamps in the game's canvas coordinates. The shader samples
    // them through the SAME per-row transform as the original spell, including
    // rotation and scale. Size this frame from its requests while keeping the
    // minimum canvas; an earlier outlier must not enlarge later spell frames.
    std::int64_t left = sparks[0].x, top = sparks[0].y;
    std::int64_t right = left, bottom = top;
    for (int i = 0; i < count; ++i) {
        const std::int64_t stamp_width = effect_stamp_width(sparks[i]);
        const std::int64_t stamp_height = effect_stamp_height(sparks[i]);
        const std::int64_t x = std::int64_t(sparks[i].x) - stamp_width / 2;
        const std::int64_t y = std::int64_t(sparks[i].y) - stamp_height / 2;
        left = std::min(left, x);
        top = std::min(top, y);
        right = std::max(right, x + stamp_width);
        bottom = std::max(bottom, y + stamp_height);
    }
    // One spare column either side of the sparks for the held band below.
    const std::int64_t span = right - left;
    const std::int64_t width64 = std::max<std::int64_t>(
        kEffectCanvasWidth, span + 2);
    const std::int64_t height64 = std::max<std::int64_t>(
        kEffectCanvasHeight, bottom - top);
    const std::int64_t origin_x64 = 1 - left;
    const std::int64_t origin_y64 = -top;
    if (width64 <= 0 || height64 <= 0 ||
        width64 > std::numeric_limits<int>::max() ||
        height64 > std::numeric_limits<int>::max() ||
        origin_x64 < std::numeric_limits<int>::min() ||
        origin_x64 > std::numeric_limits<int>::max() ||
        origin_y64 < std::numeric_limits<int>::min() ||
        origin_y64 > std::numeric_limits<int>::max()) {
        return false;
    }
    const int width = static_cast<int>(width64);
    const int height = static_cast<int>(height64);
    const int origin_x = static_cast<int>(origin_x64);
    const int origin_y = static_cast<int>(origin_y64);
    if (width != effect_pixels_.width || height != effect_pixels_.height) {
        const auto texture = surface_->create_texture(
            width, height, gbarecomp::GpuTextureFormat::R8UI, false);
        if (!texture) return false;
        surface_->destroy_texture(effect_canvas_);
        effect_canvas_ = texture;
        effect_pixels_.reset(width, height);
        uploaded_effect_pixels_.clear();
    }
    effect_pixels_.clear();
    effect_pixels_.origin_x = origin_x;
    effect_pixels_.origin_y = origin_y;
    std::vector<EffectSpark> placed(sparks, sparks + count);
    for (EffectSpark& spark : placed) {
        spark.x += effect_pixels_.origin_x;
        spark.y += effect_pixels_.origin_y;
    }
    const int drawn = stamp_sparks(&effect_pixels_, placed.data(), count,
                                   art, art_bytes);
    if (drawn <= 0) return false;
    // A band the game cut where it stopped drawing: Cycle Beam is one solid
    // run from its picture's left edge (canvas x -50, 2:1, so 100 px left of
    // the console's screen), which Extra Wide's 120 px margin shows ending
    // square. Such a row holds its edge
    // pixel outward through the margin when all of these hold:
    //  * the edge is outside the game's 128-px canvas, where the console
    //    never shows it (the beam's other end, at the caster, is inside);
    //  * it sits where it sat last frame: a clip the game keeps (Cycle
    //    Beam: x -50 on 22 frames running), not artwork still spreading
    //    out (Fireball's streaks run up to 90% of their picture from its
    //    edge but the edge moves every frame);
    //  * the row runs from the edge for at least half the picture (Cycle
    //    Beam 76-99%; other edge-touching rows at most 36%).
    const int content = static_cast<int>(span);
    const bool hold_left = left < 0 && left == effect_last_left_;
    const bool hold_right = right > 128 && right == effect_last_right_;
    effect_last_left_ = left;
    effect_last_right_ = right;
    effect_hold_right_ = content + 1;
    for (int y = 0; y < height && (hold_left || hold_right); ++y) {
        std::uint8_t* row = effect_pixels_.pixels.data() + std::size_t(y) * width;
        // FLAT runs only: the run counts pixels equal to the edge pixel, so
        // only a single-colour band continues. Holding any lit edge pixel
        // stretches particles across the margin (Soothing Star);
        // a uniform colour has nothing to stretch.
        const std::uint8_t first = row[1], last = row[content];
        int run_left = 0, run_right = 0;
        while (first != 0 && run_left < content && row[1 + run_left] == first) ++run_left;
        while (last != 0 && run_right < content && row[content - run_right] == last) ++run_right;
        if (hold_left && run_left * 2 >= content) row[0] = row[1];
        if (hold_right && run_right * 2 >= content) row[content + 1] = row[content];
    }
    if (uploaded_effect_pixels_ != effect_pixels_.pixels) {
        surface_->update_texture(effect_canvas_, 0, 0, width, height,
                                 effect_pixels_.pixels.data());
        uploaded_effect_pixels_ = effect_pixels_.pixels;
    }
    effect_layer_ = effect_layer;
    effect_have_sparks_ = true;
    static bool said = false;
    if (!said) {
        said = true;
        std::fprintf(stderr,
                     "[gsr] host effects: uploaded %d non-transparent spark "
                     "stamps from %d requests; GPU overlay enabled\n",
                     drawn, count);
    }
    return true;
}

namespace {

// Where a layer shows the game's 128x128 spell canvas, if it does: the 256
// consecutive 8-bit tiles laid out 16 x 16 somewhere in its map.
//   * a WRAPPING 256x256 affine layer with the block at the map origin
//     (Ray, BG2CNT 0x6785);
//   * a 256x256 8-bit regular layer with the block anywhere, tiles in order
//     and unflipped (Ivan's hit sparks, BG1CNT 0x1F81: tiles 0x100.. at map
//     column 0, row 11).
struct EffectCanvasLayout {
    bool found = false;
    bool text = false;
    int map_x = 0, map_y = 0;   // top-left, map pixels
    std::size_t chr = 0;        // the canvas's first tile, VRAM byte offset
};

EffectCanvasLayout effect_canvas_layout(const std::uint8_t* vram,
                                        std::size_t vram_bytes,
                                        const SceneLayer& layer) {
    EffectCanvasLayout out;
    if (!vram) return out;
    const std::size_t map = layer.screen_base;
    if (layer.affine) {
        // A wrapping 256x256 map with the canvas in its corner (Ray), or a
        // 128x128 map that is the canvas itself (Thor and the Djinn,
        // stretched over the screen at PA 128).
        // Meteor's is the same 128 canvas with the wrap bit set (BG2CNT
        // 0x2784).
        const bool wrapping_256 = layer.wraps && layer.size_code == 1;
        const bool plain_128 = layer.size_code == 0;
        const int stride = wrapping_256 ? 32 : 16;
        if (!(wrapping_256 || plain_128) ||
            map + 16 * stride > vram_bytes ||
            layer.char_base + 256 * 64 > vram_bytes)
            return out;
        for (int r = 0; r < 16; ++r)
            for (int c = 0; c < 16; ++c)
                if (vram[map + r * stride + c] != r * 16 + c) return out;
        out.found = true;
        out.chr = layer.char_base;
        return out;
    }
    if (layer.size_code != 0 || !layer.eight_bit_colour ||
        map + 32 * 32 * 2 > vram_bytes)
        return out;
    auto entry = [&](int r, int c) {
        const std::size_t at = map + static_cast<std::size_t>(r * 32 + c) * 2u;
        return static_cast<unsigned>(vram[at] | (vram[at + 1] << 8));
    };
    for (int r0 = 0; r0 <= 16; ++r0) {
        for (int c0 = 0; c0 <= 16; ++c0) {
            const unsigned first = entry(r0, c0);
            if ((first & 0xC00u) != 0u) continue;
            const unsigned base = first & 0x3FFu;
            if (base + 255u > 0x3FFu) continue;
            bool block = true;
            for (int r = 0; r < 16 && block; ++r)
                for (int c = 0; c < 16 && block; ++c)
                    block = (entry(r0 + r, c0 + c) & 0xFFFu) ==
                            base + static_cast<unsigned>(r * 16 + c);
            const std::size_t chr =
                layer.char_base + static_cast<std::size_t>(base) * 64u;
            if (!block || chr + 256 * 64 > vram_bytes) continue;
            out.found = true;
            out.text = true;
            out.map_x = c0 * 8;
            out.map_y = r0 * 8;
            out.chr = chr;
            return out;
        }
    }
    return out;
}

// The iris wipe (entering and leaving a room): WIN0 is an
// ellipse the game writes row by row into WIN0H. The console clips each
// row's edges to 0..240, so past the screen the registers only say "the
// whole row", and the margins would draw full rows beside the circle and the
// first row's span above it -- a cross. Rebuilt from the rows whose two
// edges are inside the screen: their half-widths fit w^2 = c0 + c1 y +
// c2 y^2 to under a pixel on every measured frame (an ellipse 1.22
// times as wide as tall, centred on the player closing, on the screen
// opening). Too few such rows, a centre that moves, or a worse fit: no
// iris, and the window works as before. Once the centre is known, rows with
// only one edge inside count too (with the player near the left
// edge only 4-7 two-edge rows remain at the circle's top, too few to place
// its bottom, and the bottom margin would flicker). A game that knows its iris
// supplies it in SceneMargins::iris instead.

IrisWindow fit_iris_window(const FieldScene& scene) {
    IrisWindow out;
    double s[5] = {}, t[3] = {};  // sums of y^k, and of w^2 * y^k
    int rows = 0, centre2 = -1;
    std::array<std::pair<int, double>, FieldScene::kRows> seen{};
    auto row_edges = [&](int y, int* x1, int* x2) {
        const std::uint8_t* io = scene.registers_for_row(y);
        const unsigned dispcnt = io[0] | (io[1] << 8);
        if (((dispcnt >> 13) & 1u) == 0u) return false;
        *x1 = io[0x41];
        *x2 = io[0x40];
        return *x1 < *x2;
    };
    auto add_row = [&](int y, double w) {
        const double w2 = w * w;
        double yk = 1.0;
        for (int k = 0; k < 5; ++k) { s[k] += yk; if (k < 3) t[k] += w2 * yk; yk *= y; }
        seen[static_cast<std::size_t>(rows++)] = {y, w};
    };
    int two_edge_rows = 0;
    for (int y = 0; y < FieldScene::kRows; ++y) {
        int x1 = 0, x2 = 0;
        if (!row_edges(y, &x1, &x2) || x1 <= 0 || x2 >= 240) continue;
        if (centre2 < 0) centre2 = x1 + x2;
        if (std::abs(x1 + x2 - centre2) > 1) return out;
        add_row(y, (x2 - x1) * 0.5);
        ++two_edge_rows;
    }
    if (two_edge_rows < 3) return out;
    for (int y = 0; y < FieldScene::kRows; ++y) {
        int x1 = 0, x2 = 0;
        if (!row_edges(y, &x1, &x2)) continue;
        const bool left_only = x1 > 0 && x2 >= 240;
        const bool right_only = x1 <= 0 && x2 < 240;
        if (!left_only && !right_only) continue;
        const double w = left_only ? centre2 * 0.5 - x1 : x2 - centre2 * 0.5;
        if (w > 0.0) add_row(y, w);
    }
    // Normal equations for (c0, c1, c2), by Cramer's rule.
    const double m[3][3] = {{s[0], s[1], s[2]}, {s[1], s[2], s[3]}, {s[2], s[3], s[4]}};
    auto det3 = [](const double a[3][3]) {
        return a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
               a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
               a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    };
    const double d = det3(m);
    if (std::abs(d) < 1e-9) return out;
    double c[3];
    for (int k = 0; k < 3; ++k) {
        double mk[3][3];
        for (int r = 0; r < 3; ++r)
            for (int col = 0; col < 3; ++col) mk[r][col] = col == k ? t[r] : m[r][col];
        c[k] = det3(mk) / d;
    }
    if (c[2] >= 0.0) return out;
    const double cy = -c[1] / (2.0 * c[2]);
    const double a2 = c[0] - c[1] * c[1] / (4.0 * c[2]);
    if (a2 <= 0.0) return out;
    for (int i = 0; i < rows; ++i) {
        const double y = seen[static_cast<std::size_t>(i)].first;
        const double fit = std::sqrt(std::max(0.0, c[0] + c[1] * y + c[2] * y * y));
        if (std::abs(fit - seen[static_cast<std::size_t>(i)].second) > 1.5) return out;
    }
    out.found = true;
    out.cx = static_cast<float>(centre2 * 0.5);
    out.cy = static_cast<float>(cy);
    out.a2 = static_cast<float>(a2);
    out.b2 = static_cast<float>(-a2 / c[2]);
    return out;
}

// The smallest canvas value the console's own screen shows: each row's
// pixels where its windows let the layer through, sampled through that row's
// registers as the background shader samples them. 0 when a shown pixel is
// clear or nothing is shown. The game joins everything it paints by the
// larger value, so a tint under the whole effect is this floor.
int effect_canvas_floor(const std::uint8_t* vram, const EffectCanvasLayout& at,
                        const FieldScene& scene, int bg) {
    const SceneLayer& layer = scene.layers[bg];
    auto win_h = [](unsigned reg, int x) {
        int x1 = int(reg >> 8), x2 = int(reg & 0xFFu);
        if (x1 > 240 && x1 > x2) x1 = 0;
        if (x2 > 240) { x2 = 240; if (x1 > 240) x1 = 240; }
        if (x1 > x2) return x >= x1 || x < x2;
        return x >= x1 && x < x2;
    };
    auto win_v = [](unsigned reg, int y) {
        const int y1 = int(reg >> 8);
        int y2 = int(reg & 0xFFu);
        if (y2 > 160 || y1 > y2) y2 = 160;
        return y >= y1 && y < y2;
    };
    int floor_value = 256;
    for (int y = 0; y < FieldScene::kRows; ++y) {
        const std::uint8_t* io = scene.registers_for_row(y);
        auto reg = [&](int a) { return unsigned(io[a] | (io[a + 1] << 8)); };
        const unsigned dispcnt = reg(0x00);
        if ((dispcnt & (0x100u << bg)) == 0) continue;
        const bool win0 = (dispcnt & 0x2000u) && win_v(reg(0x44), y);
        const bool win1 = (dispcnt & 0x4000u) && win_v(reg(0x46), y);
        // The object window's shape is not known here: a pixel either half
        // of WINOUT shows counts, which can only lower the floor.
        const unsigned outside = (dispcnt & 0x8000u)
            ? (reg(0x4A) | (reg(0x4A) >> 8)) : reg(0x4A);
        const unsigned bit = 1u << bg;
        std::int32_t rx = 0, ry = 0;
        int pa = 0, pc = 0, hofs = 0, vofs = 0;
        if (layer.affine) {
            const int param = 0x20 + (bg - 2) * 0x10;
            pa = static_cast<std::int16_t>(reg(param));
            pc = static_cast<std::int16_t>(reg(param + 4));
            const auto reference = scene.affine_reference_for_row(y, bg);
            rx = reference[0];
            ry = reference[1];
        } else {
            hofs = int(reg(0x10 + bg * 4) & 0x1FFu);
            vofs = int(reg(0x12 + bg * 4) & 0x1FFu);
        }
        for (int x = 0; x < 240; ++x) {
            if (dispcnt & 0xE000u) {
                const unsigned control =
                    (win0 && win_h(reg(0x40), x)) ? reg(0x48)
                    : (win1 && win_h(reg(0x42), x)) ? reg(0x48) >> 8
                    : outside;
                if ((control & bit) == 0) continue;
            }
            int cx, cy;
            if (layer.affine) {
                cx = (rx + x * pa) >> 8;
                cy = (ry + x * pc) >> 8;
                if (layer.wraps) {
                    cx &= (128 << layer.size_code) - 1;
                    cy &= (128 << layer.size_code) - 1;
                }
            } else {
                cx = (x + hofs - at.map_x) & 255;
                cy = (y + vofs - at.map_y) & 255;
            }
            if (cx < 0 || cy < 0 || cx >= 128 || cy >= 128) continue;
            const int value = vram[at.chr + ((cy >> 3) * 16 + (cx >> 3)) * 64 +
                                   (cy & 7) * 8 + (cx & 7)];
            if (value == 0) return 0;
            floor_value = std::min(floor_value, value);
        }
    }
    return floor_value < 256 ? floor_value : 0;
}

}  // namespace

int FieldSceneRenderer::find_effect_canvas_layer(const std::uint8_t* vram,
                                                 std::size_t vram_bytes,
                                                 const FieldScene& scene) {
    for (const int bg : {2, 1}) {
        const SceneLayer& layer = scene.layers[bg];
        if (layer.enabled && effect_canvas_layout(vram, vram_bytes, layer).found)
            return bg;
    }
    return -1;
}

void FieldSceneRenderer::analyse_effect_canvas(const std::uint8_t* vram,
                                               std::size_t vram_bytes,
                                               const FieldScene& scene,
                                               int bg) {
    const SceneLayer& layer = scene.layers[bg];
    effect_wrap_canvas_ = false;
    effect_text_canvas_ = false;
    effect_fill_ = 0;
    // Also run with no sparks: Spark Plasma's pink tint frames carry none,
    // and without a fill the tint would stop at the canvas's edge, 16 px into
    // the right margin, with the left margin untinted. Only the fill can be set then; the spark checks are moot.
    bool have_sparks = effect_have_sparks_;
    const EffectCanvasLayout at = effect_canvas_layout(vram, vram_bytes, layer);
    if (!at.found) return;
    auto canvas = [&](int x, int y) -> int {
        return vram[at.chr + ((y >> 3) * 16 + (x >> 3)) * 64 +
                    (y & 7) * 8 + (x & 7)];
    };
    auto ours = [&](int x, int y) {
        const int hx = x + effect_pixels_.origin_x;
        const int hy = y + effect_pixels_.origin_y;
        return have_sparks && effect_pixels_.in_bounds(hx, hy) &&
               effect_pixels_.pixels[static_cast<std::size_t>(hy) *
                                     effect_pixels_.width + hx] != 0;
    };
    // Our sparks must be this frame's. The capture is refreshed when the
    // game copies its canvas to the screen, but Thor's canvas is emptied
    // some other way. The flash stamp from the last copy (46,416 pixels,
    // 13,654 of them inside the canvas, none lit there) would then draw a blue
    // haze in the margins only (on a current frame all of ours inside the
    // canvas are lit).
    //
    // So two checks mark a capture as stale. If nothing is lit in the game's
    // canvas, nothing of the effect is on screen, so none of ours may draw
    // either. Inside the canvas the game joins sparks by the larger value, so
    // if fewer than a quarter of ours are lit there, the capture is stale
    // too.
    effect_sparks_inside_ = 0;
    effect_sparks_lit_ = 0;
    int canvas_lit = 0;
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 128; ++x) {
            const bool lit = canvas(x, y) != 0;
            canvas_lit += lit ? 1 : 0;
            if (ours(x, y)) {
                ++effect_sparks_inside_;
                if (lit) ++effect_sparks_lit_;
            }
        }
    if (canvas_lit == 0) {
        effect_have_sparks_ = false;
        return;
    }
    if (have_sparks && effect_sparks_inside_ >= 64 &&
        effect_sparks_lit_ * 4 < effect_sparks_inside_) {
        effect_have_sparks_ = false;
        have_sparks = false;
    }
    // The fill is the game's flashing tint: the value under everything the
    // console shows of the canvas (effect_canvas_floor).
    //  * Thor's tint (16, 8, 4, 2 in turn) and Ray's (12, 16, 9, 3) lie
    //    under sparks we did not capture.
    //  * Odyssey and Eclipse tint only the rows the battle window shows
    //    (canvas rows 0..119 of 128), and Odyssey's sword crosses the
    //    canvas's edge. Looking for one value over three quarters of the
    //    whole 128x128 edge finds no tint there, so the tint would stop at
    //    the console's edge.
    // Over 684 measured Lost Age canvas frames, every tint that edge rule
    // finds is this floor too.
    effect_fill_ = effect_canvas_floor(vram, at, scene, bg);
    effect_wrap_canvas_ = !at.text;
    effect_text_canvas_ = at.text;
    effect_canvas_map_x_ = at.map_x;
    effect_canvas_map_y_ = at.map_y;
    effect_scroll_x_ = layer.scroll_x;
    effect_scroll_y_ = layer.scroll_y;
    effect_size_code_ = layer.size_code;
}

// Canvas -> output pixels, for a text layer showing the canvas. Inverse of
// what the background shader does in its text_canvas branch:
//
//   map_w  = (size_code & 1) ? 512 : 256 ; map_h likewise with bit 1
//   sx0    = (canvas_map_x - (hofs & 0x1FF)) & (map_w - 1)   (same for y)
//   sx0   -= map_w if sx0 >= 240 ; sy0 -= map_h if sy0 >= 160
//   canvas_px = console_px - (sx0, sy0)
//
// so a canvas point sits at console_px = canvas + (sx0, sy0), where the
// canvas's top-left map pixel (effect_canvas_map_x_/y_, found by
// effect_canvas_layout) is scrolled by the layer's own HOFS/VOFS -- the
// hardware rule for a regular layer. The shader then takes
//   console_px = floor(v_screen + u_camera)
// with v_screen the output pixel and u_camera = ((240 - output_w_) / 2,
// (160 - output_h_) / 2) (draw()), so
//   view = console_px - camera.
// Limits: scroll is the frame-level SceneLayer value, while the shader reads
// HOFS/VOFS per scanline; a layer whose scroll changes mid-frame would map
// only approximately. The effect span remap (u_effect_span) is only applied
// by the shader's affine branch, so it does not enter here.
void FieldSceneRenderer::set_effect_hidden(bool hidden) {
    effect_hidden_ = hidden;
}

bool FieldSceneRenderer::effect_canvas_to_view(int canvas_x, int canvas_y,
                                               float* view_x,
                                               float* view_y) const {
    if (!ready_ || !effect_have_sparks_ || !effect_text_canvas_ ||
        output_w_ <= 0 || output_h_ <= 0 || !view_x || !view_y)
        return false;
    const int map_w = (effect_size_code_ & 1u) ? 512 : 256;
    const int map_h = (effect_size_code_ & 2u) ? 512 : 256;
    int sx0 = (effect_canvas_map_x_ - (effect_scroll_x_ & 0x1FF)) & (map_w - 1);
    int sy0 = (effect_canvas_map_y_ - (effect_scroll_y_ & 0x1FF)) & (map_h - 1);
    if (sx0 >= 240) sx0 -= map_w;
    if (sy0 >= 160) sy0 -= map_h;
    const int camera_x = (240 - output_w_) / 2;
    const int camera_y = (160 - output_h_) / 2;
    *view_x = static_cast<float>(canvas_x + sx0 - camera_x);
    *view_y = static_cast<float>(canvas_y + sy0 - camera_y);
    return true;
}

namespace {

const char* kSolidVertex =
    "#version 130\n"
    "in vec2 a_pos;\n"
    "in vec2 a_uv;\n"
    "in float a_depth;\n"
    "in vec4 a_tint;\n"
    "uniform mat4 u_transform;\n"
    "out vec4 v_tint;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "  v_tint = a_tint;\n"
    "  v_uv = a_uv;\n"
    "  gl_Position = u_transform * vec4(a_pos, a_depth, 1.0);\n"
    "}\n";

const char* kSolidFragment =
    "#version 130\n"
    "in vec4 v_tint;\n"
    "in vec2 v_uv;\n"
    "out vec4 o_colour;\n"
    // v_uv runs 0..1 across the quad (GpuQuad's default source rectangle), so
    // d is 0 at the centre and 1 at the edge. u_shape picks the profile:
    //   0 soft glow: alpha falls smoothly to 0 at the edge (dust, grit, impact
    //     flash). With u_core != 0 a near-white core also blends into the
    //     tint colour; the burst leaves it off so colours stay earth-toned;
    //   1 hard blob: mostly opaque, edge at 0.85 (dirt chunks; keep in step
    //     with kBurstChunkQuadScale).
    "uniform int u_shape;\n"
    "uniform int u_core;\n"
    "void main() {\n"
    "  float d = length(v_uv * 2.0 - 1.0);\n"
    "  if (d >= 1.0) discard;\n"
    "  vec3 rgb = v_tint.rgb;\n"
    "  float shape_a;\n"
    "  if (u_shape == 1) {\n"
    "    shape_a = 1.0 - smoothstep(0.72, 0.85, d);\n"
    "  } else {\n"
    "    shape_a = 1.0 - d;\n"
    "    shape_a *= shape_a;\n"
    "    if (u_core != 0) {\n"
    "      float core = 1.0 - smoothstep(0.0, 0.4, d);\n"
    "      rgb = mix(rgb, vec3(1.0), core * 0.7);\n"
    "    }\n"
    "  }\n"
    "  o_colour = vec4(rgb, v_tint.a * shape_a);\n"
    "}\n";

}  // namespace

// Draws the shake target back to the surface, offset and scaled about the
// centre. gl_FragCoord is in GL orientation (y up), so the view's downward
// offset is negated. The source coordinate is clamped; the scale keeps it
// inside the picture anyway.
static const char* kShakeBlitFragment =
    "#version 130\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec2 u_size;\n"
    "uniform vec2 u_offset;\n"
    "uniform float u_scale;\n"
    "out vec4 o_colour;\n"
    "void main() {\n"
    "  vec2 c = u_size * 0.5;\n"
    "  vec2 src = (gl_FragCoord.xy - c - u_offset) / u_scale + c;\n"
    "  ivec2 i = ivec2(clamp(floor(src), vec2(0.0), u_size - 1.0));\n"
    "  o_colour = texelFetch(u_tex, i, 0);\n"
    "}\n";

bool FieldSceneRenderer::ensure_solid() {
    if (!ready_) return false;
    if (!solid_ && !solid_failed_) {
        char log[1024] = {};
        solid_ = surface_->create_program(kSolidVertex, kSolidFragment, log,
                                          sizeof log);
        if (!solid_) {
            solid_failed_ = true;
            std::fprintf(stderr, "field_scene_renderer: burst shader: %s\n",
                         log);
        }
    }
    return solid_ != 0;
}

// Creates the shake program and a target the size of the output. Called
// outside a frame (spawn_burst); draw() shakes only if this
// succeeded and the size still matches.
bool FieldSceneRenderer::ensure_shake_target() {
    if (!ready_ || shake_failed_ || output_w_ <= 0 || output_h_ <= 0)
        return false;
    if (!shake_blit_) {
        char log[1024] = {};
        shake_blit_ = surface_->create_program(kSolidVertex, kShakeBlitFragment,
                                               log, sizeof log);
        if (!shake_blit_) {
            shake_failed_ = true;
            std::fprintf(stderr, "field_scene_renderer: shake shader: %s\n",
                         log);
            return false;
        }
    }
    if (!shake_target_ || shake_target_w_ != output_w_ ||
        shake_target_h_ != output_h_) {
        if (shake_target_) surface_->destroy_texture(shake_target_);
        shake_target_ = surface_->create_texture(
            output_w_, output_h_, gbarecomp::GpuTextureFormat::RGBA8, false);
        shake_target_w_ = output_w_;
        shake_target_h_ = output_h_;
        if (!shake_target_) {
            shake_failed_ = true;
            return false;
        }
    }
    return true;
}

// Mixes this frame's picture with the other sprite phase's picture
// (set_flicker_smoothing). Both were resolved at the same output pixels, so
// they are read at gl_FragCoord like the shake target. A pixel the two agree
// on comes out unchanged.
static const char* kFlickerMixFragment =
    "#version 130\n"
    "uniform sampler2D u_this;\n"
    "uniform sampler2D u_other;\n"
    "uniform float u_weight;\n"
    // Bottom-up GL pixel box [x0, y0, x1, y1) where u_other was rendered.
    // Outside this box both phases are identical, so u_this passes straight through.
    "uniform vec4 u_flicker_box;\n"
    "out vec4 o_colour;\n"
    "void main() {\n"
    "  ivec2 i = ivec2(gl_FragCoord.xy);\n"
    "  vec4 a = gsr_raw_fetch(u_this, i, 0);\n"
    "  if (gl_FragCoord.x < u_flicker_box.x || gl_FragCoord.x >= u_flicker_box.z ||\n"
    "      gl_FragCoord.y < u_flicker_box.y || gl_FragCoord.y >= u_flicker_box.w) {\n"
    "    o_colour = a;\n"
    "    return;\n"
    "  }\n"
    "  vec4 b = gsr_raw_fetch(u_other, i, 0);\n"
    "  o_colour = (a == b) ? a : mix(a, b, u_weight);\n"
    "}\n";

void FieldSceneRenderer::set_flicker_smoothing(float weight) {
    flicker_weight_ = weight > 0.0f ? std::min(weight, 0.5f) : 0.0f;
}

bool FieldSceneRenderer::ensure_flicker_targets() {
    if (!ready_ || flicker_failed_) return false;
    if (!flicker_mix_) {
        char log[1024] = {};
        flicker_mix_ = surface_->create_program(kSolidVertex, kFlickerMixFragment,
                                                log, sizeof log);
        if (!flicker_mix_) {
            flicker_failed_ = true;
            std::fprintf(stderr, "field_scene_renderer: flicker shader: %s\n",
                         log);
            return false;
        }
    }
    if (!flicker_this_ || !flicker_other_ || flicker_target_w_ != output_w_ ||
        flicker_target_h_ != output_h_) {
        if (flicker_this_) surface_->destroy_texture(flicker_this_);
        if (flicker_other_) surface_->destroy_texture(flicker_other_);
        flicker_this_ = surface_->create_texture(
            output_w_, output_h_, gbarecomp::GpuTextureFormat::RGBA8, false);
        flicker_other_ = surface_->create_texture(
            output_w_, output_h_, gbarecomp::GpuTextureFormat::RGBA8, false);
        flicker_target_w_ = output_w_;
        flicker_target_h_ = output_h_;
        if (!flicker_this_ || !flicker_other_) {
            flicker_failed_ = true;
            return false;
        }
    }
    return true;
}

static const char* kFlatRowsFragment =
    "#version 130\n"
    "uniform sampler2D u_src;\n"
    "uniform sampler2D u_top;\n"
    "uniform int u_height;\n"
    "uniform int u_win_x;\n"
    "uniform int u_win_y;\n"
    "out vec4 o_colour;\n"
    "void main() {\n"
    "  int r = int(gl_FragCoord.x);\n"
    "  int row_kind = int(gl_FragCoord.y);\n"
    "  int fyr = u_height - 1 - (u_win_y + r);\n"
    "  if (row_kind == 0) {\n"
    // Row 0: uniform background bar colour & flag for ShutterRows.
    // Ignore object pixels (top.b * 255 < 128) so a world-map sprite on a
    // Mode-0 battle-flash band (GS1 BG1 priority 1) does not break the bar.
    "    vec3 bg_colour = vec3(0.0);\n"
    "    bool have_bg = false;\n"
    "    bool bg_uniform = true;\n"
    "    for (int x = 0; x < 240; ++x) {\n"
    "      ivec2 at = ivec2(u_win_x + x, fyr);\n"
    "      int top_low = int(round(texelFetch(u_top, at, 0).b * 255.0));\n"
    "      if (top_low < 128) continue;\n"
    "      vec3 p = texelFetch(u_src, at, 0).rgb;\n"
    "      if (!have_bg) {\n"
    "        bg_colour = p;\n"
    "        have_bg = true;\n"
    "      } else if (any(notEqual(p, bg_colour))) {\n"
    "        bg_uniform = false;\n"
    "        break;\n"
    "      }\n"
    "    }\n"
    "    o_colour = vec4(bg_colour, (have_bg && bg_uniform) ? 1.0 : 0.0);\n"
    "  } else {\n"
    // Row 1: whether all 240 pixels of row r match console (0, 0).
    "    int fy0 = u_height - 1 - u_win_y;\n"
    "    vec3 first0 = texelFetch(u_src, ivec2(u_win_x, fy0), 0).rgb;\n"
    "    float same_all = 1.0;\n"
    "    for (int x = 0; x < 240; ++x) {\n"
    "      vec3 p = texelFetch(u_src, ivec2(u_win_x + x, fyr), 0).rgb;\n"
    "      if (any(notEqual(p, first0))) {\n"
    "        same_all = 0.0;\n"
    "        break;\n"
    "      }\n"
    "    }\n"
    "    o_colour = vec4(same_all, 0.0, 0.0, 1.0);\n"
    "  }\n"
    "}\n";

static const char* kFlatAllFragment =
    "#version 130\n"
    "uniform sampler2D u_rows;\n"
    "out vec4 o_colour;\n"
    "void main() {\n"
    "  float same = 1.0;\n"
    "  for (int r = 0; r < 160; ++r) {\n"
    "    if (texelFetch(u_rows, ivec2(r, 1), 0).r < 0.5) {\n"
    "      same = 0.0;\n"
    "      break;\n"
    "    }\n"
    "  }\n"
    "  o_colour = vec4(same, 0.0, 0.0, 1.0);\n"
    "}\n";

static const char* kWorldComposeFragment =
    "#version 130\n"
    "in vec2 v_screen;\n"
    "uniform sampler2D u_src;\n"
    "uniform sampler2D u_margin;\n"
    "uniform sampler2D u_flat_rows;\n"
    "uniform sampler2D u_flat_all;\n"
    "uniform sampler2D u_top;\n"
    "uniform usampler2D u_row_io;\n"
    "uniform usampler2D u_palette;\n"
    "uniform usampler2D u_flat_map;\n"
    "uniform int u_mode;\n"
    "uniform int u_width;\n"
    "uniform int u_height;\n"
    "uniform int u_win_x;\n"
    "uniform int u_win_y;\n"
    "uniform int u_has_margin;\n"
    "uniform int u_mixed_modes;\n"
    "uniform int u_glow;\n"
    "uniform int u_evb;\n"
    "uniform int u_shake_x;\n"
    "uniform int u_shake_y;\n"
    "uniform int u_flat_map_trusted;\n"
    "out vec4 o_colour;\n"
    "\n"
    "uint row_u16(int row, int offset) {\n"
    "  return gsr_raw_fetch(u_row_io, ivec2(offset >> 1, row), 0).r;\n"
    "}\n"
    "\n"
    "ivec2 fbo_at(int x, int y) {\n"
    "  return ivec2(x, u_height - 1 - y);\n"
    "}\n"
    "\n"
    "void main() {\n"
    "  int tx = clamp(int(floor(v_screen.x)), 0, u_width - 1);\n"
    "  int ty = clamp(int(floor(v_screen.y)), 0, u_height - 1);\n"
    "  int cx = tx - u_win_x;\n"
    "  int cy = ty - u_win_y;\n"
    "  bool outside = cx < 0 || cx >= 240 || cy < 0 || cy >= 160;\n"
    "  vec4 base = texelFetch(u_src, fbo_at(tx, ty), 0);\n"
    "\n"
    "  if (u_mode == 1) {\n"
    // ShutterRows: extend uniform non-Mode-2 rows across margins.
    "    if (!outside) { o_colour = base; return; }\n"
    "    int r = clamp(cy, 0, 159);\n"
    "    vec4 bar = texelFetch(u_flat_rows, ivec2(r, 0), 0);\n"
    "    if ((row_u16(r, 0x00) & 7u) != 2u && bar.a > 0.5) {\n"
    "      o_colour = vec4(bar.rgb, 1.0);\n"
    "    } else {\n"
    "      o_colour = base;\n"
    "    }\n"
    "    return;\n"
    "  }\n"
    "\n"
    "  if (u_mode == 2) {\n"
    // ZoomNative (GS1 R-button map): integer nearest zoom of 240x160.
    "    bool by_width = u_width * 160 >= u_height * 240;\n"
    "    int num = by_width ? u_width : u_height;\n"
    "    int den = by_width ? 240 : 160;\n"
    "    int ox = (240 * num / den - u_width) / 2;\n"
    "    int oy = (160 * num / den - u_height) / 2;\n"
    "    int sx = min((tx + ox) * den / num, 239);\n"
    "    int sy = min((ty + oy) * den / num, 159);\n"
    "    o_colour = texelFetch(u_src, fbo_at(u_win_x + sx, u_win_y + sy), 0);\n"
    "    return;\n"
    "  }\n"
    "\n"
    "  if (u_mode == 3) {\n"
    // FlatMap (TLA Select world map): 512x384 8bpp map in margins.
    "    if (!outside) { o_colour = base; return; }\n"
    "    int reg_row = clamp(cy, 0, 159);\n"
    "    int hofs = int(row_u16(reg_row, 0x14) & 0x1FFu);\n"
    "    int vofs = int(row_u16(reg_row, 0x16) & 0x1FFu);\n"
    "    int map_x = hofs + cx;\n"
    "    int map_y = vofs + cy;\n"
    "    if (u_flat_map_trusted == 0 || map_x < 0 || map_x >= 512 || map_y < 0 || map_y >= 384) {\n"
    "      o_colour = vec4(0.0, 0.0, 0.0, 1.0);\n"
    "      return;\n"
    "    }\n"
    "    int byte_off = (map_y >> 3) * 4096 + (map_x >> 3) * 64 + (map_y & 7) * 8 + (map_x & 7);\n"
    "    uint index = texelFetch(u_flat_map, ivec2(byte_off & 1023, byte_off >> 10), 0).r;\n"
    "    int colour = int(texelFetch(u_palette, ivec2(int(index), 0), 0).r);\n"
    "    int cr = colour & 31;\n"
    "    int g6 = ((colour >> 4) & 62) | (colour >> 15);\n"
    "    int cb = (colour >> 10) & 31;\n"
    "    uint bldcnt = row_u16(reg_row, 0x50);\n"
    "    uint effect = (bldcnt >> 6u) & 3u;\n"
    "    int evy = min(int(row_u16(reg_row, 0x54) & 0x1Fu), 16);\n"
    "    bool target = ((bldcnt >> (index != 0u ? 1u : 5u)) & 1u) != 0u;\n"
    "    if ((effect == 2u || effect == 3u) && evy != 0 && target) {\n"
    "      if (effect == 2u) {\n"
    "        cr += ((31 - cr) * evy + 8) >> 4;\n"
    "        g6 += ((63 - g6) * evy + 8) >> 4;\n"
    "        cb += ((31 - cb) * evy + 8) >> 4;\n"
    "      } else {\n"
    "        cr -= (cr * evy + 7) >> 4;\n"
    "        g6 -= (g6 * evy + 7) >> 4;\n"
    "        cb -= (cb * evy + 7) >> 4;\n"
    "      }\n"
    "    }\n"
    "    o_colour = vec4(float(cr) / 31.0, float(g6 >> 1) / 31.0, float(cb) / 31.0, 1.0);\n"
    "    return;\n"
    "  }\n"
    "\n"
    // Mode 4: HoldMargins (menus, fades, beam glow, shutter over Mode 0, seamless quakes).
    "  if (outside && u_mixed_modes != 0) {\n"
    "    int r = clamp(cy, 0, 159);\n"
    "    vec4 bar = texelFetch(u_flat_rows, ivec2(r, 0), 0);\n"
    "    if ((row_u16(r, 0x00) & 7u) != 2u && bar.a > 0.5) {\n"
    "      o_colour = vec4(bar.rgb, 1.0);\n"
    "      return;\n"
    "    }\n"
    "  }\n"
    "  if (texelFetch(u_flat_all, ivec2(0, 0), 0).r > 0.5) {\n"
    "    o_colour = outside ? texelFetch(u_src, fbo_at(u_win_x, u_win_y), 0) : base;\n"
    "    return;\n"
    "  }\n"
    "  if (u_has_margin == 0) {\n"
    "    o_colour = outside ? vec4(0.0, 0.0, 0.0, 1.0) : base;\n"
    "    return;\n"
    "  }\n"
    "  int sx = clamp(tx + u_shake_x, 0, u_width - 1);\n"
    "  int sy = clamp(ty + u_shake_y, 0, u_height - 1);\n"
    "  vec3 m = texelFetch(u_margin, fbo_at(sx, sy), 0).rgb;\n"
    "  if (u_glow != 0) {\n"
    "    if (!outside) { o_colour = base; return; }\n"
    "    ivec3 d8 = ivec3(round(base.rgb * 255.0));\n"
    "    ivec3 m8 = ivec3(round(m * 255.0));\n"
    "    ivec3 out8 = min(ivec3(255), d8 + (m8 * u_evb) / 16);\n"
    "    o_colour = vec4(vec3(out8) / 255.0, 1.0);\n"
    "    return;\n"
    "  }\n"
    "  int top_low = int(round(texelFetch(u_top, fbo_at(tx, ty), 0).b * 255.0));\n"
    "  bool fg = top_low <= 128;\n"  // 0..127 = OBJ, 128 = BG0 window/text
    "  bool quake = (u_shake_x != 0 || u_shake_y != 0);\n"
    "  o_colour = (outside || (quake && !fg)) ? vec4(m, 1.0) : base;\n"
    "}\n";

bool FieldSceneRenderer::ensure_world_compose() {
    if (!ready_ || !surface_ || world_compose_failed_ ||
        output_w_ <= 0 || output_h_ <= 0)
        return false;
    char log[1024] = {};
    if (!flat_rows_prog_) {
        flat_rows_prog_ = surface_->create_program(
            kResolveVertex, kFlatRowsFragment, log, sizeof log);
        if (!flat_rows_prog_) {
            world_compose_failed_ = true;
            std::fprintf(stderr, "field_scene_renderer: flat rows shader: %s\n",
                         log);
            return false;
        }
    }
    if (!flat_all_prog_) {
        flat_all_prog_ = surface_->create_program(
            kResolveVertex, kFlatAllFragment, log, sizeof log);
        if (!flat_all_prog_) {
            world_compose_failed_ = true;
            std::fprintf(stderr, "field_scene_renderer: flat all shader: %s\n",
                         log);
            return false;
        }
    }
    if (!world_compose_prog_) {
        world_compose_prog_ = surface_->create_program(
            kResolveVertex, kWorldComposeFragment, log, sizeof log);
        if (!world_compose_prog_) {
            world_compose_failed_ = true;
            std::fprintf(stderr, "field_scene_renderer: world compose shader: %s\n",
                         log);
            return false;
        }
    }
    if (!compose_src_ || compose_src_w_ != output_w_ ||
        compose_src_h_ != output_h_) {
        if (compose_src_) {
            surface_->destroy_texture(compose_src_);
            compose_src_ = 0;
        }
        compose_src_ = surface_->create_texture(
            output_w_, output_h_, gbarecomp::GpuTextureFormat::RGBA8, false);
        compose_src_w_ = output_w_;
        compose_src_h_ = output_h_;
        if (!compose_src_) {
            compose_src_w_ = compose_src_h_ = 0;
            world_compose_failed_ = true;
            return false;
        }
    }
    // Drawn with the output-sized viewport; only their own texels land.
    if (!compose_rows_)
        compose_rows_ = surface_->create_texture(
            160, 2, gbarecomp::GpuTextureFormat::RGBA8, false);
    if (!compose_flat_)
        compose_flat_ = surface_->create_texture(
            1, 1, gbarecomp::GpuTextureFormat::RGBA8, false);
    if (!compose_rows_ || !compose_flat_) {
        world_compose_failed_ = true;
        return false;
    }
    return true;
}

void FieldSceneRenderer::upload_flat_world_map(const std::uint8_t* rows_48x4096,
                                               std::uint32_t generation) {
    if (!ready_ || !surface_) return;
    if (!flat_world_map_) {
        flat_world_map_ = surface_->create_texture(
            1024, 192, gbarecomp::GpuTextureFormat::R8UI, false);
        if (!flat_world_map_) return;
    }
    if (rows_48x4096 != nullptr && generation != flat_world_map_generation_) {
        surface_->update_texture(flat_world_map_, 0, 0, 1024, 192, rows_48x4096);
        flat_world_map_generation_ = generation;
    }
}

bool FieldSceneRenderer::draw_world_margin(const FieldScene& scene) {
    if (!ready_ || !surface_ || output_w_ <= 0 || output_h_ <= 0) {
        world_margin_valid_ = false;
        return false;
    }
    if (!world_margin_tex_ || world_margin_w_ != output_w_ ||
        world_margin_h_ != output_h_) {
        if (world_margin_tex_) {
            surface_->destroy_texture(world_margin_tex_);
            world_margin_tex_ = 0;
        }
        world_margin_tex_ = surface_->create_texture(
            output_w_, output_h_, gbarecomp::GpuTextureFormat::RGBA8, false);
        world_margin_w_ = output_w_;
        world_margin_h_ = output_h_;
        world_margin_valid_ = false;
        if (!world_margin_tex_) {
            world_margin_w_ = world_margin_h_ = 0;
            return false;
        }
    }
    const bool ok = draw_frame(scene, FlickerPhase::WorldMargin);
    world_margin_valid_ = ok;
    world_margin_w_ = output_w_;
    world_margin_h_ = output_h_;
    return ok;
}

void FieldSceneRenderer::spawn_burst(float view_x, float view_y) {
    if (!ensure_solid()) return;
    ensure_shake_target();
    shake_.start();
    burst_.spawn(view_x, view_y);
}

void FieldSceneRenderer::add_trail(float x0, float y0, float x1, float y1) {
    if (!ensure_solid()) return;
    trail_.add(x0, y0, x1, y1);
}

void FieldSceneRenderer::set_effect_renderer_enabled(bool enabled) {
    effect_enabled_ = enabled;
    if (!enabled) {
        effect_layer_ = -1;
        effect_have_sparks_ = false;
    }
}

void FieldSceneRenderer::log_effect_state() const {
    if (!ready_ || !surface_) return;
    const auto nonzero = std::count_if(effect_pixels_.pixels.begin(),
        effect_pixels_.pixels.end(), [](std::uint8_t pixel) { return pixel != 0; });
    std::fprintf(stderr,
        "[gsr] host effects F12: enabled=%d layer=%d have_sparks=%d "
        "cpu=%dx%d origin=%d,%d nonzero=%zu fill=%d sparks in canvas %d, "
        "lit %d\n",
        effect_enabled_, effect_layer_, effect_have_sparks_,
        effect_pixels_.width, effect_pixels_.height,
        effect_pixels_.origin_x, effect_pixels_.origin_y, std::size_t(nonzero),
        effect_fill_, effect_sparks_inside_, effect_sparks_lit_);
    surface_->log_texture_r8ui(effect_canvas_, 8, effect_pixels_.width,
        effect_pixels_.height, effect_pixels_.pixels.data());
    int layer = -1, extra = -1, unit = -1;
    float size[2] = {}, origin[2] = {};
    const bool uniforms =
        surface_->get_uniform_int(background_, "u_layer", &layer) &&
        surface_->get_uniform_int(background_, "u_effect_extra", &extra) &&
        surface_->get_uniform_int(background_, "u_effect", &unit) &&
        surface_->get_uniform_vec2(background_, "u_effect_size", size) &&
        surface_->get_uniform_vec2(background_, "u_effect_origin", origin);
    std::fprintf(stderr,
        "[gsr] host effects GPU uniforms: read=%d layer=%d extra=%d unit=%d "
        "size=%.0f,%.0f origin=%.0f,%.0f\n",
        uniforms, layer, extra, unit, size[0], size[1], origin[0], origin[1]);
}

bool FieldSceneRenderer::upload_room(const std::uint8_t* ewram,
                                     std::size_t ewram_bytes) {
    room_valid_ = false;
    keep_out_count_ = 0;
    if (!ready_ || !ewram || ewram_bytes < kEwramBytes) return false;

    // Read this FIRST and keep it even when the rect below is refused: the
    // boot logo and the name-entry screen both have no room, and the margins
    // must know that regardless of what the rect happens to hold.
    world_loaded_ = ewram_u16(ewram, ewram_bytes, prof().map_id_addr) != 0;

    const std::uint16_t min_x = ewram_u16(ewram, ewram_bytes, prof().room_rect_addr);
    const std::uint16_t max_x = ewram_u16(ewram, ewram_bytes, prof().room_rect_addr + 2);
    const std::uint16_t min_y = ewram_u16(ewram, ewram_bytes, prof().room_rect_addr + 4);
    const std::uint16_t max_y = ewram_u16(ewram, ewram_bytes, prof().room_rect_addr + 6);
    if (!room_rect_is_room(min_x, max_x, min_y, max_y)) return false;

    // No room loaded yet, or between rooms. Measured over 2,495 recorded
    // snapshots plus the live name-entry screen: of the 587 frames the rect
    // above accepts, 583 carry a real map number and every one of them
    // matches the hardware, while the 4 with map number 0 include both of
    // the only two frames that DIFFER from it (one of them wrong over
    // 31,843 of 38,400 pixels). Gating on the map number rejects those and
    // the name-entry screen, and leaves all 583 correct frames untouched.
    if (ewram_u16(ewram, ewram_bytes, prof().map_id_addr) == 0) return false;

    room_min_x_ = min_x; room_max_x_ = max_x;
    room_min_y_ = min_y; room_max_y_ = max_y;
    // The words at 0x02030DC0 are the camera clamp's 16.16 MAX x and y
    // (Func_10230 reads min x, min y, max x, max y at [0x03001E70] + 0xEC,
    // 0xF0, 0xF4, 0xF8, and [0x03001E70] is 0x02030CCC in every measured frame), so
    // the "min" halves above are fractions and always 0. The real minimums
    // are the integer halves at 0x02030DBA and 0x02030DBE. Rooms whose map
    // grid holds more than the player can ever scroll to have them above 0,
    // and the strip between is filler (measured minimums such as 64,48,
    // 288,240 and 16,32), which the margins would draw as patterns.
    {
        const int clamp_min_x = ewram_u16(ewram, ewram_bytes, prof().clamp_min_x_addr);
        const int clamp_min_y = ewram_u16(ewram, ewram_bytes, prof().clamp_min_y_addr);
        if (clamp_min_x < room_max_x_ - 16) room_min_x_ = clamp_min_x;
        if (clamp_min_y < room_max_y_ - 16) room_min_y_ = clamp_min_y;
    }
    room_camera_x_ = static_cast<int>(ewram_u16(ewram, ewram_bytes, prof().camera_addr + 2));
    room_camera_y_ = static_cast<int>(ewram_u16(ewram, ewram_bytes, prof().camera_addr + 6));

    // The camera's keep-out rectangles (FieldGameProfile::keep_out_list_addr):
    // the game never shows them, so they are outside the room.
    if (prof().keep_out_list_addr != 0) {
        const std::uint32_t list =
            ewram_u16(ewram, ewram_bytes, prof().keep_out_list_addr) |
            (static_cast<std::uint32_t>(
                 ewram_u16(ewram, ewram_bytes, prof().keep_out_list_addr + 2)) << 16);
        if (list >= kEwramBase && list - kEwramBase < kEwramBytes) {
            int n = 0;
            for (;; ++n) {
                const std::uint32_t at = list + static_cast<std::uint32_t>(n) * 8u;
                if (ewram_u16(ewram, ewram_bytes, at) == 0xFFFFu) break;
                if (n == kKeepOutMax) { n = 0; break; }
                for (int k = 0; k < 4; ++k)
                    keep_out_[n][k] = static_cast<std::int16_t>(
                        ewram_u16(ewram, ewram_bytes, at + 2u * k));
            }
            keep_out_count_ = n;
        }
    }

    // Per-layer scroll records. Only two things are taken from them:
    //  - an auto-scrolling layer (the Suhalla sandstorm, BG1: speed -7.56,
    //    2.19, mask 31x31, region x 1024) wraps inside its own region every
    //    (mask + 1) * 8 pixels, as the game does. Its margins read straight
    //    on past the region instead, into BG2's props (cacti sliding with
    //    the storm).
    //  - a layer moving at another speed than the camera (the crow's nest
    //    sky, BG3 ratio 0.375) has filler above its art;
    //    the art's top row is carried upward instead.
    // The records sit prof().layer_record_bytes apart, BG3 then BG2 then BG1:
    // +0x00/+0x04 position (16.16), +0x08/+0x0C the layer's region of the grid
    // (pixels, 16.16), +0x10/+0x14 speed ratio against the camera (0x10000 =
    // 1), +0x18/+0x1C auto-scroll speed, +0x28/+0x2A wrap mask in 8-px tiles
    // (the game's profile says how the +0x2A word is stored).
    for (int k = 0; k < 3; ++k) {
        const int bg = 3 - k;
        const std::uint32_t at = prof().layer_record_addr + prof().layer_record_bytes * k;
        auto word = [&](std::uint32_t off) {
            return static_cast<std::uint32_t>(ewram_u16(ewram, ewram_bytes, at + off)) |
                   (static_cast<std::uint32_t>(ewram_u16(ewram, ewram_bytes, at + off + 2)) << 16);
        };
        const bool autoscroll = word(0x18) != 0 || word(0x1C) != 0;
        const int unit = prof().layer_wrap_unit;
        const int wrap_x = (ewram_u16(ewram, ewram_bytes, at + 0x28) + 1) * unit;
        const int wrap_y =
            ((ewram_u16(ewram, ewram_bytes, at + 0x2A) >> prof().layer_wrap_y_shift) + 1) * unit;
        const bool pow2 = (wrap_x & (wrap_x - 1)) == 0 && (wrap_y & (wrap_y - 1)) == 0;
        LayerWrap& w = layer_wrap_[bg];
        w = LayerWrap{};
        w.have_record = true;
        w.pos_x = static_cast<std::int16_t>(ewram_u16(ewram, ewram_bytes, at + 0x02));
        w.pos_y = static_cast<std::int16_t>(ewram_u16(ewram, ewram_bytes, at + 0x06));
        w.region_x = static_cast<int>(ewram_u16(ewram, ewram_bytes, at + 0x0A));
        w.region_y = static_cast<int>(ewram_u16(ewram, ewram_bytes, at + 0x0E));
        if (autoscroll && pow2 && wrap_x < 2048 && wrap_y < 2048) {
            w.wrap_x = wrap_x;
            w.wrap_y = wrap_y;
        }
        // The storm also has a ratio (1.25); it wraps instead, and holding
        // its top row drew vertical streaks.
        w.parallax = !autoscroll &&
                     (word(0x10) != 0x10000u || word(0x14) != 0x10000u);
    }

    // Altin's rest positions, kept only on the maps that use them; any other
    // map forgets them, so a return starts afresh.
    const int map = ewram_u16(ewram, ewram_bytes, prof().map_id_addr);
    const auto& rest_maps = prof().altin_rest_maps;
    rest_rule_on_ = map != 0 &&
        std::find(std::begin(rest_maps), std::end(rest_maps), map) != std::end(rest_maps);
    if (!rest_rule_on_) {
        rest_map_ = -1;
    } else {
        // A move of more than 32 rows is a new rest position, not a drift:
        // the room's first frames can still hold the last room's records
        // (region 0, then 1024 at load, then 1024 -> 1000 as the water
        // sinks).
        for (int bg = 0; bg < 4; ++bg) {
            const int region = layer_wrap_[bg].region_y;
            if (map != rest_map_ || rest_region_y_[bg] == 0 ||
                std::abs(region - rest_region_y_[bg]) > 32)
                rest_region_y_[bg] = region;
        }
        rest_map_ = map;
    }

    room_ids_.resize(static_cast<std::size_t>(kGridSide) * kGridSide);
    for (std::size_t i = 0; i < room_ids_.size(); ++i) {
        const std::uint32_t addr = prof().id_grid_addr + static_cast<std::uint32_t>(i) * 4u;
        // Low 12 bits of the 32-bit grid word are the metatile id; the
        // low 16 bits already contain them whole (FACTS.md, "The upper bits
        // of the metatile word are not unused" -- those live above bit 15).
        room_ids_[i] = ewram_u16(ewram, ewram_bytes, addr) & 0x0FFFu;
    }
    if (uploaded_room_ids_ != room_ids_) {
        surface_->update_texture(id_grid_, 0, 0, kGridSide, kGridSide,
                                 room_ids_.data());
        uploaded_room_ids_ = room_ids_;
    }

    room_atlas_.resize(static_cast<std::size_t>(kAtlasIds) * 4u);
    for (std::size_t i = 0; i < room_atlas_.size(); ++i) {
        const std::uint32_t addr = prof().atlas_addr + static_cast<std::uint32_t>(i) * 2u;
        room_atlas_[i] = ewram_u16(ewram, ewram_bytes, addr);
    }
    if (uploaded_room_atlas_ != room_atlas_) {
        surface_->update_texture(atlas_, 0, 0, 4, kAtlasIds, room_atlas_.data());
        uploaded_room_atlas_ = room_atlas_;
    }

    room_valid_ = true;
    return true;
}

bool FieldSceneRenderer::in_keep_out(int room_x, int room_y) const {
    for (int i = 0; i < keep_out_count_; ++i) {
        const int* r = keep_out_[i];
        if (room_x >= r[0] && room_y >= r[1] && room_x < r[2] && room_y < r[3])
            return true;
    }
    return false;
}

bool FieldSceneRenderer::query_room_known(int screen_x, int screen_y,
                                          int scroll_x, int scroll_y) const {
    if (!room_valid_) return false;
    const int room_x = room_camera_x_ + screen_x;
    const int room_y = room_camera_y_ + screen_y;
    if (room_x < room_min_x_ || room_y < room_min_y_ ||
        room_x >= room_max_x_ || room_y >= room_max_y_ ||
        in_keep_out(room_x, room_y)) {
        return false;
    }
    const int source_x = room_grid_pixel(scroll_x + screen_x);
    const int source_y = room_grid_pixel(scroll_y + screen_y);
    if (source_x < 0 || source_y < 0) return false;
    const int stx = source_x >> 3;
    const int sty = source_y >> 3;
    const int gx = stx >> 1;
    const int gy = sty >> 1;
    if (gx < 0 || gy < 0 || gx >= kGridSide || gy >= kGridSide) return false;
    const std::uint16_t id = room_ids_[static_cast<std::size_t>(gy) * kGridSide + gx];
    const int sub = (sty & 1) * 2 + (stx & 1);
    const std::uint16_t entry =
        room_atlas_[static_cast<std::size_t>(id) * 4u + static_cast<std::size_t>(sub)];
    if (entry == kRoomUnavailableTile || entry == kRoomNoMapTile) {
        return false;
    }
    return true;
}

gbarecomp::GpuTexture FieldSceneRenderer::output_texture() const {
    return surface_ ? surface_->target_texture() : 0;
}

bool FieldSceneRenderer::draw(const FieldScene& scene) {
    // The same frame again (a held picture): as it was drawn, no step.
    const bool repeat = has_last_drawn_ && scene.frame == last_drawn_frame_;
    // Sprites shown on every other frame: draw the other phase first (clipped
    // to the tight bounding box of the alternating sprites when safe), then
    // this frame mixed with it (sprite_flicker.h). Off, or nothing
    // alternating, is the plain single draw.
    bool other_drawn = false;
    flicker_scissor_on_ = false;
    if (flicker_weight_ > 0.0f && ready_ && output_w_ > 0 && output_h_ > 0 &&
        ensure_flicker_targets()) {
        SceneObjectTable other;
        SpriteFlickerBox fbox;
        std::array<bool, FieldScene::kObjects> changed_slots{};
        if (repeat) {
            if (last_other_drawn_ && flicker_scene_) {
                flicker_scissor_on_ = last_flicker_scissor_on_;
                flicker_sx0_ = last_flicker_sx0_;
                flicker_sy0_ = last_flicker_sy0_;
                flicker_sx1_ = last_flicker_sx1_;
                flicker_sy1_ = last_flicker_sy1_;
                other_drawn = draw_frame(*flicker_scene_, FlickerPhase::Other);
            }
        } else if (scene.valid &&
                   sprite_flicker_other_phase(scene.objects, flicker_history_,
                                              &other, &fbox, &changed_slots)) {
            if (!flicker_scene_) flicker_scene_ = std::make_unique<FieldScene>();
            *flicker_scene_ = scene;
            flicker_scene_->objects = other;
            // Tight scissor box is safe when no alternating sprite is repeated
            // into margins or spans the full console width as a strip.
            bool scissor_ok = fbox.valid;
            if (scissor_ok) {
                for (int s = 0; s < FieldScene::kObjects; ++s) {
                    if (!changed_slots[s]) continue;
                    if (margins_ && (margins_->object_repeat[s][0] > 0 ||
                                     margins_->object_repeat[s][1] > 0)) {
                        scissor_ok = false;
                        break;
                    }
                    const SceneObject& a = scene.objects[s];
                    const SceneObject& b = other[s];
                    if ((a.present && (a.x <= 0 || a.x + a.width >= 240)) ||
                        (b.present && (b.x <= 0 || b.x + b.width >= 240))) {
                        // Conservative: edge-touching sprites could anchor a strip.
                        scissor_ok = false;
                        break;
                    }
                }
            }
            if (scissor_ok) {
                const int cam_x = (240 - output_w_) / 2;
                const int cam_y = (160 - output_h_) / 2;
                // Pad bottom by 8 px if box extends outside native 240x160 in
                // case an off-screen character shadow reorders under it.
                const int pad_y = (fbox.x0 < 0 || fbox.y0 < 0 ||
                                   fbox.x1 > 240 || fbox.y1 > 160) ? 8 : 0;
                flicker_sx0_ = std::clamp(fbox.x0 - cam_x, 0, output_w_);
                flicker_sy0_ = std::clamp(fbox.y0 - cam_y, 0, output_h_);
                flicker_sx1_ = std::clamp(fbox.x1 - cam_x, 0, output_w_);
                flicker_sy1_ = std::clamp(fbox.y1 + pad_y - cam_y, 0, output_h_);
                flicker_scissor_on_ =
                    (flicker_sx1_ > flicker_sx0_ && flicker_sy1_ > flicker_sy0_);
            }
            other_drawn = draw_frame(*flicker_scene_, FlickerPhase::Other);
        }
    }
    repeat_draw_ = repeat;
    const bool ok = draw_frame(
        scene, other_drawn ? FlickerPhase::Mix : FlickerPhase::Plain);
    repeat_draw_ = false;
    if (!ok || flicker_weight_ <= 0.0f)
        flicker_history_.reset();
    else if (!repeat)
        flicker_history_.push(scene.objects);
    has_last_drawn_ = ok;
    last_drawn_frame_ = scene.frame;
    last_other_drawn_ = other_drawn;
    last_flicker_scissor_on_ = flicker_scissor_on_;
    last_flicker_sx0_ = flicker_sx0_;
    last_flicker_sy0_ = flicker_sy0_;
    last_flicker_sx1_ = flicker_sx1_;
    last_flicker_sy1_ = flicker_sy1_;
    return ok;
}

bool FieldSceneRenderer::draw_frame(const FieldScene& scene,
                                    FlickerPhase phase) {
    declined_ = nullptr;
    if (!ready_ || output_w_ <= 0 || output_h_ <= 0) {
        declined_ = "the renderer is not ready";
        return false;
    }
    // Modes 0-2 are tile-based (0 four regular layers, 1 makes BG2 affine, 2
    // makes BG2 and BG3 affine); modes 3-5 are bitmap modes the emulated
    // hardware itself does not render a background for either
    // (gba_ppu.cpp's render_scanline_internal only branches on mode 0/1/2),
    // and 2,429 measured snapshots never use them, so there is nothing to
    // describe yet.
    if (!scene.valid || scene.video_mode > 2) {
        declined_ = "the video mode uses a bitmap background";
        return false;
    }
    // Everything below this line is what the renderer cannot yet reproduce.
    // Each is refused rather than approximated, because a frame drawn almost
    // right is worse than a frame drawn by the old renderer.
    if (scene.forced_blank) { declined_ = "the display is blanked"; return false; }
    // Per row too: a row with forced blank (DISPCNT bit 7) is solid white in
    // render_scanline_internal, which no shader draws. (A row in another
    // video mode is drawn: see row_modes below.)
    for (int y = 0; y < FieldScene::kRows; ++y) {
        const std::uint8_t* io = scene.registers_for_row(y);
        if ((io[0] & 0x80u) != 0u) {
            declined_ = "the display is blanked";
            return false;
        }
    }
    // Mosaic is refused only when it would actually change a pixel. MOSAIC
    // (0x4C) holds four sizes, each stored as size-1: backgrounds in bits
    // 0-3 and 4-7, objects in bits 8-11 and 12-15. A stored 0 means a block
    // one pixel wide, which is the picture the game would get anyway -- so a
    // layer or object with the enable bit set but a zero size is not an
    // effect, it is a no-op, and refusing it costs a frame for nothing.
    //
    // This is not a shortcut: measured over every recorded frame, MOSAIC is
    // 0x0000 in ALL 2,429 snapshots and on all 160 rows of a live battle
    // frame, while the BG enable bit is set on 92 of them and an object
    // enable bit on 2,102. So every mosaic refusal ever counted was for an
    // effect that does nothing.
    //
    // A NON-zero size is still refused, and deliberately: the emulated
    // hardware in this repository does not implement mosaic either (nothing
    // in gba_ppu.cpp reads 0x4C), so there is no reference to check a real
    // one against and no measured frame that asks for one. When play finally
    // produces one, "Draw only with the graphics card" makes it visible at
    // once.
    //
    // Read per row, like every other register: the size can change mid-frame
    // as easily as BGxCNT did.
    bool bg_mosaic_real = false, obj_mosaic_real = false;
    for (int y = 0; y < FieldScene::kRows; ++y) {
        const std::uint8_t* io = scene.registers_for_row(y);
        const unsigned m = io[0x4C] | (static_cast<unsigned>(io[0x4D]) << 8);
        if ((m & 0x00FFu) != 0u) bg_mosaic_real = true;
        if ((m & 0xFF00u) != 0u) obj_mosaic_real = true;
    }
    if (obj_mosaic_real) {
        for (const auto& o : scene.objects) {
            if (!o.present) continue;
            if (o.mosaic) {
                declined_ = "mosaic is not drawn yet";
                return false;
            }
        }
    }
    if (bg_mosaic_real) {
        for (const auto& L : scene.layers) {
            if (L.enabled && L.mosaic) {
                declined_ = "mosaic is not drawn yet";
                return false;
            }
        }
    }

    // This frame's per-row registers, one FieldScene::kLineIoBytes record per
    // row: the game's own copy where it captured one (the room-transition
    // iris rewrites WIN0H every scanline), the frame's end-of-frame register
    // file otherwise.
    // Flush any deferred room_vram / room_palette changes once per draw.
    sync_deferred_uploads();

    // This frame's per-row registers, packed as 16-bit words into a 3-slot
    // ring texture (R16UI) so row_u16() needs one fetch instead of two and
    // frame N never stalls waiting on frame N-1's draw.
    gbarecomp::GpuTexture cur_row_io = row_io_[row_io_slot_];
    {
        // Game-supplied margin scroll rows follow the 160 captured rows
        // (SceneMarginLayer::scroll; only for a game whose profile says so).
        int extra_rows = 0;
        if (prof().margin_scroll_rows && margins_) {
            for (const auto& layer : margins_->layers)
                extra_rows = std::max(extra_rows,
                                      static_cast<int>(layer.scroll.size()));
            if (extra_rows > row_io_rows_ - FieldScene::kRows) {
                declined_ = "margin scroll rows exceed the output height";
                return false;
            }
        }
        const int total_rows = FieldScene::kRows + extra_rows;
        std::vector<std::uint8_t> row_bytes(
            static_cast<std::size_t>(FieldScene::kLineIoBytes) *
            static_cast<std::size_t>(total_rows));
        for (int y = 0; y < FieldScene::kRows; ++y) {
            const std::uint8_t* src = scene.registers_for_row(y);
            std::memcpy(row_bytes.data() +
                            static_cast<std::size_t>(y) * FieldScene::kLineIoBytes,
                        src, FieldScene::kLineIoBytes);
        }
        // Lamakan Desert's row 0 swaps BG1 and BG3's +1024 (every frame of
        // measured; only maps 89-91 do it), so the
        // console's top line misses the haze and shows as a thin line across
        // the middle of the widened picture. Row 0 is therefore drawn with
        // row 1's scroll. This changes native row 0 on purpose.
        if (prof().lamakan_row0_swap && scene.video_mode == 0) {
            std::uint8_t* r0 = row_bytes.data();
            const std::uint8_t* r1 = r0 + FieldScene::kLineIoBytes;
            bool swapped = false;
            for (int bg = 1; bg <= 3; ++bg) {
                const int at = 0x12 + 4 * bg;
                const int v0 = r0[at] | (r0[at + 1] << 8);
                const int v1 = r1[at] | (r1[at + 1] << 8);
                const int d = std::abs(((v0 - v1 + 0x8000) & 0xFFFF) - 0x8000);
                if (d >= 1022 && d <= 1026) swapped = true;
            }
            if (swapped) std::memcpy(r0 + 0x14, r1 + 0x14, 12);
        }
        for (int bg = 0; bg < 4; ++bg) {
            if (!prof().margin_scroll_rows || !margins_) break;
            const auto& scroll = margins_->layers[bg].scroll;
            for (std::size_t y = 0; y < scroll.size(); ++y) {
                auto* dst = row_bytes.data() +
                    (FieldScene::kRows + y) * FieldScene::kLineIoBytes + 0x10 + bg * 4;
                for (int axis = 0; axis < 2; ++axis) {
                    dst[axis * 2] = static_cast<std::uint8_t>(scroll[y][axis]);
                    dst[axis * 2 + 1] = static_cast<std::uint8_t>(scroll[y][axis] >> 8);
                }
            }
        }
        constexpr int kRowWords = static_cast<int>(FieldScene::kLineIoBytes / 2);
        std::vector<std::uint16_t> row_words(
            static_cast<std::size_t>(kRowWords) * static_cast<std::size_t>(total_rows));
        for (std::size_t i = 0; i < row_words.size(); ++i) {
            row_words[i] = static_cast<std::uint16_t>(
                row_bytes[i * 2] | (static_cast<unsigned>(row_bytes[i * 2 + 1]) << 8));
        }
        if (uploaded_row_io_ != row_words) {
            row_io_slot_ = (row_io_slot_ + 1) % kRowRing;
            cur_row_io = row_io_[row_io_slot_];
            surface_->update_texture(cur_row_io, 0, 0, kRowWords, total_rows,
                                     row_words.data());
            uploaded_row_io_ = std::move(row_words);
        }
    }

    // Each row's affine reference point for BG2 and BG3 -- the console's own
    // internal accumulator, which it reloads from BG2X/BG2Y and BG3X/BG3Y at
    // VBlank and advances by PB/PD every scanline, and which the game
    // rewrites mid-frame on the overworld and in every battle. The register
    // file cannot answer for it: those registers are write-only, so an
    // end-of-frame read returns the LAST row's value, not this row's.
    //
    // A row the PPU never latched (there is no scanline there) gets the
    // frame's own reference walked out to it here, by the same PB/PD step the
    // hardware uses.
    //
    // Stored biased by 2^29, in a low and a high half, because the only
    // integer texture formats here are unsigned. The bias cannot overflow:
    // a reference point is 28-bit signed and 160 scanlines of PB add at most
    // another 2^23.
    gbarecomp::GpuTexture cur_row_affine = row_affine_[row_affine_slot_];
    {
        constexpr int kBias = 1 << 29;
        std::vector<std::uint16_t> refs(8u * FieldScene::kRows);
        for (int y = 0; y < FieldScene::kRows; ++y) {
            for (int k = 0; k < 2; ++k) {          // k: 0 = BG2, 1 = BG3
                const auto [rx, ry] = scene.affine_reference_for_row(y, 2 + k);
                const std::int64_t ex =
                    static_cast<std::int64_t>(rx) + kBias;
                const std::int64_t ey =
                    static_cast<std::int64_t>(ry) + kBias;
                std::uint16_t* row = refs.data() + static_cast<std::size_t>(y) * 8u;
                row[k * 4 + 0] = static_cast<std::uint16_t>(ex & 0xFFFF);
                row[k * 4 + 1] = static_cast<std::uint16_t>((ex >> 16) & 0xFFFF);
                row[k * 4 + 2] = static_cast<std::uint16_t>(ey & 0xFFFF);
                row[k * 4 + 3] = static_cast<std::uint16_t>((ey >> 16) & 0xFFFF);
            }
        }
        if (uploaded_row_affine_ != refs) {
            row_affine_slot_ = (row_affine_slot_ + 1) % kRowRing;
            cur_row_affine = row_affine_[row_affine_slot_];
            surface_->update_texture(cur_row_affine, 0, 0, 8,
                                     static_cast<int>(FieldScene::kRows),
                                     refs.data());
            uploaded_row_affine_ = std::move(refs);
        }
    }

    // Which layers, if any, may carry the battle arena out into the side
    // margins, and over which rows. This is the same decision the emulated
    // path makes in runner_main.cpp's golden_sun_update_battle_backdrop,
    // made from the same registers, so a battle drawn here fills the width
    // the same way and picks the same layer to do it with:
    //
    //   * only a battle frame (mode 1 with an arena layer on) with a live,
    //     sane WIN0V band;
    //   * BG2 only when its map is the size the arena uses (size code 1);
    //   * and neither layer while it is carrying an EFFECT rather than the
    //     arena -- an alpha blend's first target sitting at priority 1.
    //     An effect is drawn where the battlers are, so continuing it
    //     sideways just repeats it at the layer's map period.
    //
    // The frame-level test is only ever a candidacy test -- whether the
    // margin actually opens on a given row is decided in the shader from that
    // row's own registers.
    //
    // Outside battles, the affine backgrounds are the world map. Its mode-2
    // layers can use the same transform across the wider screen. This extends
    // the whole map without wrapping a regular tilemap.
    const bool battle_frame = gsr::battle::is_battle_frame(
        static_cast<std::uint16_t>(scene.dispcnt));

    float transform[16];
    ortho(transform, static_cast<float>(output_w_),
          static_cast<float>(output_h_));

    // The backdrop's RECORD: palette entry 0, RAW (the resolve pass fades it
    // exactly like any other layer that ends up on top, so it must not be
    // faded twice). Its key is the maximum a real layer or object can never
    // reach, decoding to kind 5 in the resolve shader ("nothing drawn here" ==
    // the backdrop, rather than a special case -- see kResolveFragment).
    const std::uint16_t backdrop = scene.palette[0];
    constexpr int kBackdropKey = 1023;
    const float clear_r = static_cast<float>(backdrop & 0xFFu) / 255.0f;
    const float clear_g = static_cast<float>((backdrop >> 8) & 0xFFu) / 255.0f;
    const float clear_b = static_cast<float>(kBackdropKey & 0xFF) / 255.0f;
    const float clear_a = static_cast<float>((kBackdropKey >> 8) & 3) / 255.0f;

    // The resolve pass always overwrites the whole output; skipping the
    // begin_frame clear avoids an empty clear-only render pass on mobile GPUs.
    surface_->begin_frame(0.0f, 0.0f, 0.0f, 1.0f, /*clear=*/false);

    const float camera_x = static_cast<float>((240 - output_w_) / 2);
    const float camera_y = static_cast<float>((160 - output_h_) / 2);

    // Measured on every recorded frame: 100% on each good world-map frame,
    // 0% on the two pause-menu frames.
    {
        const int agree = world_map_agreement(scene, nullptr);
        world_map_used_ = world_map_valid_ &&
                          (agree < 0 || agree >= kRoomCheckMinAgreement);
    }
    auto set_bg_frame_uniforms = [&](gbarecomp::GpuProgram prog, bool has_affine) {
        surface_->set_uniform_mat4(prog, "u_transform", transform);
        surface_->set_uniform_int(prog, "u_vram", 0);
        surface_->set_uniform_int(prog, "u_palette", 1);
        surface_->set_uniform_int(prog, "u_window", 2);
        surface_->set_uniform_int(prog, "u_id_grid", 3);
        surface_->set_uniform_int(prog, "u_atlas", 4);
        surface_->set_uniform_int(prog, "u_row_io", 5);
        surface_->set_uniform_int(prog, "u_row_io_max", row_io_rows_ - 1);
        surface_->set_uniform_int(prog, "u_top_record", 7);
        surface_->set_uniform_int(prog, "u_effect", 8);
        surface_->set_uniform_vec2(prog, "u_effect_size",
                                   static_cast<float>(effect_pixels_.width),
                                   static_cast<float>(effect_pixels_.height));
        surface_->set_uniform_vec2(prog, "u_effect_origin",
                                   static_cast<float>(effect_pixels_.origin_x),
                                   static_cast<float>(effect_pixels_.origin_y));
        surface_->set_uniform_vec2(prog, "u_effect_hold", 0.0f,
                                   static_cast<float>(effect_hold_right_));
        surface_->set_uniform_int(prog, "u_room_vram", 9);
        surface_->set_uniform_int(prog, "u_hold_strip", 13);
        surface_->set_uniform_int(prog, "u_hold_strip_max",
                                  std::max(0, hold_strip_h_ - 1));
        if (has_affine) {
            surface_->set_uniform_int(prog, "u_row_affine", 6);
            surface_->set_uniform_int(prog, "u_world_map", 10);
            surface_->set_uniform_int(prog, "u_world_side",
                                      prof().world_map_side_tiles);
            surface_->set_uniform_int(prog, "u_use_world",
                                      world_map_used_ ? 1 : 0);
            surface_->set_uniform_int(prog, "u_world_cell_art", 11);
            surface_->set_uniform_int(prog, "u_world_art", 12);
            surface_->set_uniform_int(prog, "u_world_art_sets",
                                      world_map_used_ ? world_art_sets_ : 0);
        }
        surface_->set_uniform_int(prog, "u_palette_half", 0);
        surface_->set_uniform_vec2(prog, "u_camera", camera_x, camera_y);
        surface_->set_uniform_int(prog, "u_world_loaded",
                                  world_loaded_ ? 1 : 0);
        surface_->set_uniform_vec2(prog, "u_room_min",
                                   static_cast<float>(room_min_x_),
                                   static_cast<float>(room_min_y_));
        surface_->set_uniform_vec2(prog, "u_room_max",
                                   static_cast<float>(room_max_x_),
                                   static_cast<float>(room_max_y_));
        surface_->set_uniform_vec2(prog, "u_ewram_camera",
                                   static_cast<float>(room_camera_x_),
                                   static_cast<float>(room_camera_y_));
        static_assert(kKeepOutMax == 2, "u_keep_out's size in the shader");
        surface_->set_uniform_int(prog, "u_keep_out_count", keep_out_count_);
        for (int i = 0; i < keep_out_count_; ++i) {
            const char* name = i == 0 ? "u_keep_out[0]" : "u_keep_out[1]";
            surface_->set_uniform_vec4(prog, name,
                                       static_cast<float>(keep_out_[i][0]),
                                       static_cast<float>(keep_out_[i][1]),
                                       static_cast<float>(keep_out_[i][2]),
                                       static_cast<float>(keep_out_[i][3]));
        }
    };
    set_bg_frame_uniforms(background_regular_, false);
    set_bg_frame_uniforms(background_, true);

    surface_->set_uniform_mat4(object_, "u_transform", transform);
    surface_->set_uniform_int(object_, "u_vram", 0);
    surface_->set_uniform_int(object_, "u_palette", 1);
    surface_->set_uniform_int(object_, "u_window", 2);
    surface_->set_uniform_int(object_, "u_row_io", 3);
    surface_->set_uniform_int(object_, "u_top_record", 4);
    // Same camera the background quad's v_screen uses, so an object
    // fragment's gl_FragCoord recovers the console pixel the same way.
    surface_->set_uniform_vec2(object_, "u_camera", camera_x, camera_y);

    surface_->set_uniform_mat4(obj_window_, "u_transform", transform);
    surface_->set_uniform_int(obj_window_, "u_vram", 0);
    surface_->set_uniform_int(obj_window_, "u_row_io", 1);
    surface_->set_uniform_vec2(obj_window_, "u_camera", camera_x, camera_y);
    // Every transform group once a frame; a record names its group.
    surface_->set_uniform_vec4_array(object_, "u_transforms[0]",
                                     scene.object_transforms[0].data(),
                                     FieldScene::kTransforms);
    surface_->set_uniform_vec4_array(obj_window_, "u_transforms[0]",
                                     scene.object_transforms[0].data(),
                                     FieldScene::kTransforms);

    // WIN0/WIN1/object-window setup for the window pass. The rectangles,
    // WININ/WINOUT and the window-enable bits themselves are read per row
    // from u_row_io inside the shader (see kWindowFragment), so only the
    // object-window mask and the camera are set here.
    surface_->set_uniform_mat4(window_, "u_transform", transform);
    surface_->set_uniform_int(window_, "u_obj_window_mask", 0);
    surface_->set_uniform_int(window_, "u_row_io", 1);
    surface_->set_uniform_vec2(window_, "u_camera", camera_x, camera_y);
    surface_->set_uniform_int(window_, "u_reveal_full", reveal_full_ ? 1 : 0);
    {
        const IrisWindow iris = margins_ && margins_->iris.found
            ? margins_->iris : fit_iris_window(scene);
        surface_->set_uniform_int(window_, "u_iris",
            !iris.found ? 0 : margins_ && margins_->iris.found ? 2 : 1);
        surface_->set_uniform_vec2(window_, "u_iris_centre", iris.cx, iris.cy);
        surface_->set_uniform_vec2(window_, "u_iris_axes", iris.a2, iris.b2);
    }

    // Every sampler2D unit gets a valid RGBA8 texture even on Pass 1 (never
    // an R8UI/R16UI integer texture like effect_canvas_ or atlas_), because
    // ES 3.0 leaves mismatched sampler/format bindings undefined and Adreno
    // faults on them.
    const gbarecomp::GpuTexture textures2[5] = {
        vram_, palette_, window_control_, cur_row_io, window_control_};
    const gbarecomp::GpuTexture textures3[5] = {
        vram_, palette_, window_control_, cur_row_io, top_};
    const gbarecomp::GpuTexture window_textures[2] = {obj_window_mask_, cur_row_io};
    // Backgrounds additionally carry the room buffer's id grid and atlas and
    // this frame's per-row registers and affine references, at the fixed
    // units u_id_grid=3/u_atlas=4/u_row_io=5/u_row_affine=6 set above;
    // u_top_record moves to unit 7 only for the background program; objects
    // use row_io at unit 3 and top at unit 4.
    // Unit 8 is our own effect canvas, always bound so the uniform can switch
    // it on per layer without reshuffling the array.
    // Unit 10 is the world map (upload_world_map), 11 and 12 its cells'
    // other artwork (upload_world_cell_art), bound always for the same
    // reason.
    const gbarecomp::GpuTexture bg_textures2[14] = {
        vram_, palette_, window_control_, id_grid_, atlas_, cur_row_io,
        cur_row_affine, window_control_, effect_canvas_, room_vram_, world_map_,
        world_cell_art_, world_art_, hold_strip_};
    const gbarecomp::GpuTexture bg_textures3[14] = {
        vram_, palette_, window_control_, id_grid_, atlas_, cur_row_io,
        cur_row_affine, top_, effect_canvas_, room_vram_, world_map_,
        world_cell_art_, world_art_, hold_strip_};

    // The console picks the winning pixel by a sort key: a background's key is
    // its priority times 256 plus 128 plus its layer number, an object's is its
    // priority times 256 plus its slot number, and the smaller key wins
    // (gba_ppu.cpp). The depth test reproduces that ordering directly, and
    // drawing front-to-back lets hardware early-Z reject occluded backgrounds.
    // DISPCNT's layer enables, read the way the reference reads them: once
    // per row. `SceneLayer::enabled` is the frame's LAST row's answer, so a
    // layer the game switches on for part of the frame and off by VBlank
    // would never be drawn at all. The draw below is issued when any row
    // wants the layer; the shader then drops the rows that do not.
    bool objects_on = false;
    bool obj_window_on = false;
    bool any_window_on = false;
    bool all_rows_room_sig = true;
    bool any_rows_room_sig = false;
    for (int y = 0; y < FieldScene::kRows; ++y) {
        const std::uint8_t* io = scene.registers_for_row(y);
        const unsigned dispcnt =
            io[0] | (static_cast<unsigned>(io[1]) << 8);
        if ((dispcnt & 0x1000u) != 0u) objects_on = true;
        if ((dispcnt & 0x8000u) != 0u) obj_window_on = true;
        if ((dispcnt & 0xE000u) != 0u) any_window_on = true;
        const bool sig =
            (dispcnt & 7u) == 0u &&
            (dispcnt & 0x0C00u) == 0x0C00u &&
            (io[0x0F] & 0x1Fu) == 5u &&
            (io[0x0D] & 0x1Fu) == 6u &&
            (io[0x0B] & 0x1Fu) == 7u;
        if (sig) any_rows_room_sig = true;
        else all_rows_room_sig = false;
    }
    bool has_obj_window_sprites = false;
    if (obj_window_on) {
        for (int slot = 0; slot < FieldScene::kObjects; ++slot) {
            if (scene.objects[slot].present && scene.objects[slot].window) {
                has_obj_window_sprites = true;
                break;
            }
        }
    }
    surface_->set_uniform_int(background_regular_, "u_window_on", any_window_on ? 1 : 0);
    surface_->set_uniform_int(background_, "u_window_on", any_window_on ? 1 : 0);
    surface_->set_uniform_int(object_, "u_window_on", any_window_on ? 1 : 0);
    surface_->set_uniform_int(window_, "u_has_obj_window",
                              has_obj_window_sprites ? 1 : 0);
    // Draw order within a priority: OAM order (lower slot in front), except
    // for a character's shadow that the console never shows. Every field
    // shadow is a 16x8 8-bit sprite on tile 0, centred under its character
    // (all 67 measured). On the console's screen the game
    // always lists the character first (58 of 58); above the screen it
    // sometimes lists the shadow first (5 of 9), which the
    // console hides but the margins would show as shadows over the feet. Such a
    // shadow, wholly outside the 240x160 window, draws just behind its own
    // character; nothing on the console's screen changes order. Its own
    // character is the candidate whose feet it sits at (bottom 2 rows above
    // the shadow's: Isaac and the statues), not the first
    // listed: a statue a row lower also covers it.
    std::array<int, FieldScene::kObjects> object_rank{};
    {
        auto is_shadow = [&](const SceneObject& O) {
            return O.present && !O.window && O.tile == 0 && O.palette < 0 &&
                   O.width == 16 && O.height == 8;
        };
        auto box_w = [](const SceneObject& O) {
            return O.affine && O.double_size ? O.width * 2 : O.width;
        };
        auto box_h = [](const SceneObject& O) {
            return O.affine && O.double_size ? O.height * 2 : O.height;
        };
        std::array<int, FieldScene::kObjects> body_of{};
        body_of.fill(-1);
        for (int s = 0; s < FieldScene::kObjects; ++s) {
            const SceneObject& S = scene.objects[s];
            if (!is_shadow(S)) continue;
            const int sw = box_w(S), sh = box_h(S);
            if (S.x + sw > 0 && S.x < 240 && S.y + sh > 0 && S.y < 160)
                continue;
            int best_gap = std::numeric_limits<int>::max();
            for (int b = s + 1; b < FieldScene::kObjects; ++b) {
                const SceneObject& B = scene.objects[b];
                if (!B.present || B.window || is_shadow(B) ||
                    B.priority != S.priority) continue;
                const int dx = (2 * B.x + box_w(B)) - (2 * S.x + sw);
                const int dy = S.y - B.y;
                if (dx >= -2 && dx <= 2 && dy >= 0 && dy <= box_h(B)) {
                    const int gap = std::abs((S.y + sh) - (B.y + box_h(B)));
                    if (gap < best_gap) {
                        best_gap = gap;
                        body_of[s] = b;
                    }
                }
            }
        }
        int rank = 0;
        for (int b = 0; b < FieldScene::kObjects; ++b) {
            if (body_of[b] >= 0) continue;
            object_rank[b] = rank++;
            for (int s = 0; s < b; ++s)
                if (body_of[s] == b) object_rank[s] = rank++;
        }
    }
    // Full-width sprite strips: a cutscene's black bars are rows of one
    // repeated 32x16 sprite laid end to end across the console's 0..256
    // (24 of them on the ship), so in the widened view
    // they would stop short of both edges. A run of identical, abutting,
    // unrotated sprites on one row that covers the whole console width is
    // repeated outward to the view's edges, and a run touching the top or
    // bottom of the console is repeated up or down the same way. Nothing
    // new is drawn: the strip is the game's own tile, laid further.
    struct StripReach { bool left = false, right = false, up = false, down = false; };
    std::array<StripReach, FieldScene::kObjects> strip{};
    if (output_w_ != 240 || output_h_ != 160) {
        auto same_kind = [](const SceneObject& a, const SceneObject& b) {
            return a.y == b.y && a.width == b.width && a.height == b.height &&
                   a.tile == b.tile && a.palette == b.palette &&
                   a.priority == b.priority && a.hflip == b.hflip &&
                   a.vflip == b.vflip && a.blended == b.blended;
        };
        auto plain = [](const SceneObject& o) {
            return o.present && !o.window && !o.affine && !o.parked &&
                   o.width > 0;
        };
        std::array<bool, FieldScene::kObjects> seen{};
        for (int a = 0; a < FieldScene::kObjects; ++a) {
            if (seen[a] || !plain(scene.objects[a])) continue;
            std::array<int, FieldScene::kObjects> run{};
            int n = 0;
            for (int b = a; b < FieldScene::kObjects; ++b)
                if (!seen[b] && plain(scene.objects[b]) &&
                    same_kind(scene.objects[a], scene.objects[b])) {
                    seen[b] = true;
                    run[n++] = b;
                }
            std::sort(run.begin(), run.begin() + n, [&](int p, int q) {
                return scene.objects[p].x < scene.objects[q].x;
            });
            // The longest abutting chain in this group.
            for (int i = 0; i < n;) {
                int j = i;
                while (j + 1 < n &&
                       scene.objects[run[j + 1]].x ==
                           scene.objects[run[j]].x + scene.objects[run[j]].width)
                    ++j;
                const SceneObject& first = scene.objects[run[i]];
                const SceneObject& last = scene.objects[run[j]];
                // Any row inside the widened view, not only the console's
                // 160: the Sol Sanctum cloud rows also sit wholly in the top
                // margin (y -40) and would stop short of the
                // sides there.
                // (FieldGameProfile::solid_object_strips; a game without the
                // rule keeps the console's 0..160 and stacks any tile.)
                const bool strip_rules = prof().solid_object_strips;
                const int view_top = strip_rules ? -(output_h_ - 160) / 2 : 0;
                const int view_bottom =
                    strip_rules ? 160 + (output_h_ - 160) / 2 : 160;
                if (first.x <= 0 && last.x + last.width >= 240 &&
                    first.y < view_bottom &&
                    first.y + first.height > view_top) {
                    strip[run[i]].left = true;
                    strip[run[j]].right = true;
                    // Up and down only for a solid tile: stacking a patterned
                    // one makes stripes. The Sol Sanctum cloud row slides to
                    // y -1 and would be stacked up the whole top margin.
                    const bool solid = !strip_rules ||
                        object_is_solid(first, (scene.dispcnt & 0x40u) != 0u);
                    for (int k = i; k <= j; ++k) {
                        strip[run[k]].up = solid && first.y <= 0;
                        strip[run[k]].down =
                            solid && first.y + first.height >= 160;
                    }
                }
                i = j + 1;
            }
        }
    }
    // Which layers exist, and which are affine, follows each row's own video
    // mode: mode 0 is four regular layers, mode 1 drops BG3 and makes BG2
    // affine, mode 2 keeps only BG2 and BG3, both affine (field_scene.cpp,
    // layer_exists_in_mode). `enabled` and `affine` have the frame's mode
    // folded in, but only for the last row, so they cannot be reused here.
    // A layer one row reads as regular and another as affine is drawn once
    // per reading, and the shader keeps each row's own (u_row_mode_filter).
    bool layer_wanted[4] = {false, false, false, false};
    bool wanted_regular[4] = {false, false, false, false};
    bool wanted_affine[4] = {false, false, false, false};
    bool row_modes_mixed = false;
    for (int y = 0; y < FieldScene::kRows; ++y) {
        const std::uint8_t* io = scene.registers_for_row(y);
        const unsigned dispcnt = io[0] | (static_cast<unsigned>(io[1]) << 8);
        const int mode = static_cast<int>(dispcnt & 7u);
        if (mode != scene.video_mode) row_modes_mixed = true;
        for (int bg = 0; bg < 4; ++bg) {
            const bool exists = mode == 0 || (mode == 1 && bg <= 2) ||
                                (mode == 2 && bg >= 2);
            if (!exists || ((dispcnt >> (8 + bg)) & 1u) == 0u) continue;
            layer_wanted[bg] = true;
            const bool affine = (mode == 1 && bg == 2) || (mode == 2 && bg >= 2);
            (affine ? wanted_affine : wanted_regular)[bg] = true;
        }
    }
    // A layer switched on and off line by line: Altin's water drains as
    // random scanlines of BG1 going off, fewer each frame (the same stripes
    // appear in mGBA). Margin rows have no line of their own, and
    // the nearest line's state would leave the water solid above and below the
    // console; they take the enable bit of row (y mod 160) instead. A plain
    // split (a dialogue box, a cutscene bar) switches only a few times.
    bool layer_striped[4] = {false, false, false, false};
    for (int bg = 0; prof().altin_striped_layers && bg < 4; ++bg) {
        int switches = 0;
        bool prev = false;
        for (int y = 0; y < FieldScene::kRows; ++y) {
            const std::uint8_t* io = scene.registers_for_row(y);
            const bool on = ((io[1] >> bg) & 1u) != 0u;
            if (y > 0 && on != prev) ++switches;
            prev = on;
        }
        layer_striped[bg] = switches >= 16;
    }
    // A frame where some field layer is read through its record (Lamakan's
    // swapped regions): the haze-above rule then covers every room layer,
    // the sandfall's own-scroll BG2 included.
    bool haze_room = false;
    if (prof().lamakan_haze && room_source_enabled_ && room_valid_ &&
        scene.video_mode == 0) {
        for (int bg = 1; bg <= 3; ++bg) {
            bool rec = false;
            if (!scene.layers[bg].affine &&
                room_reference_for(scene, bg, &rec) && rec)
                haze_room = true;
        }
    }

    // Partition each enabled background into horizontal strips by (enabled,
    // priority) so every quad carries its exact vertex depth (no gl_FragDepth
    // write, keeping hardware early-Z rejection on) and disabled letterbox
    // bands are not rasterized.
    std::array<std::vector<gbarecomp::GpuQuad>, 4> bg_quads[4][2];
    auto build_bg_strips = [&](int bg, bool affine) {
        const bool striped = layer_striped[bg];
        int run_start = -1;
        int run_prio = -1;
        auto flush_run = [&](int y_end) {
            if (run_start < 0) return;
            const float out_y0 = (run_start == 0) ? 0.0f
                : std::clamp(static_cast<float>(run_start) - camera_y,
                             0.0f, static_cast<float>(output_h_));
            const float out_y1 = (y_end == FieldScene::kRows)
                ? static_cast<float>(output_h_)
                : std::clamp(static_cast<float>(y_end) - camera_y,
                             0.0f, static_cast<float>(output_h_));
            if (out_y1 > out_y0) {
                gbarecomp::GpuQuad q;
                q.x = 0.0f;
                q.y = out_y0;
                q.w = static_cast<float>(output_w_);
                q.h = out_y1 - out_y0;
                q.u0 = 0.0f;
                q.v0 = out_y0;
                q.u1 = static_cast<float>(output_w_);
                q.v1 = out_y1;
                const int key = run_prio * 256 + 128 + bg;
                q.depth = static_cast<float>(key) / 1024.0f;
                bg_quads[bg][affine ? 1 : 0][run_prio].push_back(q);
            }
            run_start = -1;
            run_prio = -1;
        };
        for (int y = 0; y < FieldScene::kRows; ++y) {
            const std::uint8_t* io = scene.registers_for_row(y);
            const unsigned dispcnt = io[0] | (static_cast<unsigned>(io[1]) << 8);
            bool on = striped || (((dispcnt >> (8 + bg)) & 1u) != 0u);
            if (on && row_modes_mixed) {
                const unsigned mode = dispcnt & 7u;
                const bool exists = mode == 0u || (mode == 1u && bg <= 2) ||
                                    (mode == 2u && bg >= 2);
                const bool row_aff = (mode == 1u && bg == 2) ||
                                     (mode == 2u && bg >= 2);
                if (!exists || row_aff != affine) on = false;
            }
            const unsigned bgcnt = io[0x08 + bg * 2] |
                                   (static_cast<unsigned>(io[0x09 + bg * 2]) << 8);
            const int prio = static_cast<int>(bgcnt & 3u);
            if (!on) {
                flush_run(y);
            } else if (run_start < 0) {
                run_start = y;
                run_prio = prio;
            } else if (prio != run_prio) {
                flush_run(y);
                run_start = y;
                run_prio = prio;
            }
        }
        flush_run(FieldScene::kRows);
    };
    for (int bg = 0; bg < 4; ++bg) {
        if (!layer_wanted[bg]) continue;
        if (!row_modes_mixed) {
            build_bg_strips(bg, scene.layers[bg].affine);
        } else {
            if (wanted_regular[bg]) build_bg_strips(bg, false);
            if (wanted_affine[bg]) build_bg_strips(bg, true);
        }
    }

    // `affine` is this draw's reading of the layer: the frame's own unless
    // the rows mix video modes (see layer_wanted above).
    auto draw_background_as = [&](int bg, bool second_pass, bool affine,
                                  const std::vector<gbarecomp::GpuQuad>& quads) {
        const gbarecomp::GpuProgram prog = affine ? background_ : background_regular_;
        const SceneLayer& L = scene.layers[bg];
        static const SceneMarginLayer kNoMargin;
        const SceneMarginLayer& margin = margins_ ? margins_->layers[bg] : kNoMargin;
        surface_->set_uniform_int(prog, "u_layer", bg);
        surface_->set_uniform_int(prog, "u_layer_striped",
                                  layer_striped[bg] ? 1 : 0);
        const int rest_dy = rest_region_y_[bg] - layer_wrap_[bg].region_y;
        surface_->set_uniform_int(
            prog, "u_rest_dy",
            margin.rest_clip_y != 0 ? margin.rest_clip_y :
                (rest_rule_on_ && scene.video_mode == 0 &&
                    std::abs(rest_dy) >= 4 ? rest_dy : 0));
        surface_->set_uniform_int(prog, "u_compare_top",
                                  second_pass ? 1 : 0);
        if (affine) surface_->set_uniform_int(prog, "u_affine", 1);
        surface_->set_uniform_int(prog, "u_row_mode_filter",
                                  row_modes_mixed ? 1 : 0);
        bool use_room =
            room_source_enabled_ && room_valid_ && scene.video_mode == 0 &&
            bg != 0 && !affine && margin.vram_height == 0;
        bool from_record = false;
        if (use_room && !room_reference_for(scene, bg, &from_record))
            use_room = false;
        const LayerWrap& wrap = layer_wrap_[bg];
        const int room_auth = !use_room || !any_rows_room_sig ? 0
                            : all_rows_room_sig ? 2 : 1;
        surface_->set_uniform_int(prog, "u_room_auth", room_auth);
        surface_->set_uniform_int(prog, "u_room_grid_wrap",
                                  prof().room_grid_wrap ? 1 : 0);
        surface_->set_uniform_int(prog, "u_haze_room",
                                  haze_room ? 1 : 0);
        surface_->set_uniform_int(prog, "u_haze_mirror",
                                  prof().lamakan_haze ? 1 : 0);
        surface_->set_uniform_int(prog, "u_margin_scroll_first_y",
                                  margin.scroll_first_y);
        surface_->set_uniform_int(prog, "u_margin_scroll_rows",
                                  static_cast<int>(margin.scroll.size()));
        surface_->set_uniform_int(prog, "u_margin_scroll_sides",
                                  margin.scroll_sides ? 1 : 0);
        if (affine) {
            surface_->set_uniform_int(prog, "u_affine_step_valid", margin.affine_step_valid ? 1 : 0);
            for (int edge = 0; edge < 2; ++edge) {
                const auto& step = margin.affine_step[edge];
                surface_->set_uniform_vec4(prog, edge ? "u_affine_step_bottom" : "u_affine_step_top",
                                           step[0], step[1], step[2], step[3]);
            }
        }
        surface_->set_uniform_int(prog, "u_margin_vram_height",
                                  margin.vram_height);
        surface_->set_uniform_int(prog, "u_margin_vram_clamp",
                                  margin.vram_clamp ? 1 : 0);
        surface_->set_uniform_vec2(prog, "u_margin_repeat",
                                   static_cast<float>(margin.repeat_width),
                                   static_cast<float>(margin.repeat_height));
        surface_->set_uniform_vec4(prog, "u_art_repeat",
                                   static_cast<float>(use_room ? margin.art_repeat_y : 0),
                                   static_cast<float>(margin.art_repeat_shift),
                                   static_cast<float>(use_room ? margin.art_repeat_x : 0),
                                   0.0f);
        surface_->set_uniform_int(prog, "u_past_room_edge",
                                  margin.past_room_edge ? 1 : 0);
        surface_->set_uniform_int(prog, "u_room_ref_on",
                                  use_room && from_record ? 1 : 0);
        surface_->set_uniform_vec2(prog, "u_room_ref",
                                   static_cast<float>(wrap.pos_x),
                                   static_cast<float>(wrap.pos_y));
        const bool split_v = prof().kolima_split_rows && use_room &&
                             row_split_vertical(scene, bg);
        surface_->set_uniform_int(prog, "u_split_v", split_v ? 1 : 0);
        int split_x = 0, split_y = 0;
        const bool split_found =
            split_v && split_row_offset(scene, bg, &split_x, &split_y);
        surface_->set_uniform_int(prog, "u_split_found",
                                  split_found ? 1 : 0);
        surface_->set_uniform_vec2(prog, "u_split_offset",
                                   static_cast<float>(split_x),
                                   static_cast<float>(split_y));
        surface_->set_uniform_vec4(prog, "u_room_wrap",
                                   static_cast<float>(wrap.region_x),
                                   static_cast<float>(wrap.region_y),
                                   static_cast<float>(use_room ? wrap.wrap_x : 0),
                                   static_cast<float>(use_room ? wrap.wrap_y : 0));
        int hold_floor = 0;
        bool hold_top = false;
        if (use_room && wrap.parallax && room_valid_ && margin.scroll.empty()) {
            const std::uint8_t* io = scene.registers_for_row(0);
            const std::size_t at = 0x10u + static_cast<std::size_t>(bg) * 4u;
            const int sx = (io[at] | (io[at + 1] << 8)) + 120;
            const int sy = io[at + 2] | (io[at + 3] << 8);
            auto entry_at = [&](int x, int y, std::uint16_t* e) {
                if (x < 0 || y < 0) return false;
                const int gx = x >> 4, gy = y >> 4;
                if (gx >= kGridSide || gy >= kGridSide) return false;
                const std::uint16_t id =
                    room_ids_[static_cast<std::size_t>(gy) * kGridSide + gx];
                const int sub = ((y >> 3) & 1) * 2 + ((x >> 3) & 1);
                *e = room_atlas_[static_cast<std::size_t>(id) * 4u +
                                 static_cast<std::size_t>(sub)];
                return true;
            };
            std::uint16_t fill = 0, here = 0;
            const int margin_top = std::max(0, (output_h_ - 160) / 2);
            auto filler_row = [&](int y) {
                for (int x = sx - 120; x < sx + 120; x += 8) {
                    std::uint16_t e = 0;
                    if (!entry_at(x, y, &e) || e != fill) return false;
                }
                return true;
            };
            if (entry_at(wrap.region_x, wrap.region_y, &fill)) {
                for (int y = sy & ~7; y >= sy - margin_top - 8; y -= 8) {
                    if (!entry_at(sx, y, &here)) break;
                    if (here == fill && filler_row(y)) {
                        hold_floor = y + 8;
                        hold_top = hold_floor > sy - margin_top;
                        break;
                    }
                }
            }
        }
        surface_->set_uniform_int(prog, "u_hold_top", hold_top ? 1 : 0);
        surface_->set_uniform_int(prog, "u_hold_floor", hold_floor);
        int margin_mode = 0;
        if (battle_frame) {
            if (bg == static_cast<int>(gsr::battle::kBackdropLayer) ||
                bg == static_cast<int>(gsr::battle::kArenaAffineLayer))
                margin_mode = 2;
        } else if (affine) {
            margin_mode = 1;
        }
        surface_->set_uniform_int(prog, "u_margin_mode", margin_mode);
        const bool remap = effect_layer_ == bg && effect_span_ && !L.wraps &&
                           output_w_ > 0 &&
                           output_h_ > 0 &&
                           (output_w_ != 240 || output_h_ != 160);
        const bool hide_here = effect_hidden_ && effect_layer_ == bg &&
                               effect_have_sparks_ &&
                               (effect_text_canvas_ || effect_wrap_canvas_);
        surface_->set_uniform_int(prog, "u_effect_hide",
                                  !hide_here ? 0 : (effect_text_canvas_ ? 1 : 2));
        const bool effect_extra_on =
            effect_layer_ == bg && effect_have_sparks_ && !hide_here;
        surface_->set_uniform_int(prog, "u_effect_extra",
                                  effect_extra_on ? 1 : 0);
        bool repeats_on_screen = false;
        if (effect_layer_ == bg && affine && L.wraps && L.size_code == 0) {
            const int param = 0x20 + (bg - 2) * 0x10;
            for (int y = 0; y < FieldScene::kRows && !repeats_on_screen; ++y) {
                const std::uint8_t* io = scene.registers_for_row(y);
                const int pa = static_cast<std::int16_t>(
                    io[param] | (io[param + 1] << 8));
                repeats_on_screen = std::abs(pa) != 256 &&
                    ((239 * std::abs(pa)) >> 8) >= 136;
            }
        }
        const bool wrap_canvas_on =
            effect_layer_ == bg && effect_wrap_canvas_ &&
            !hide_here && !repeats_on_screen;
        if (affine) {
            surface_->set_uniform_int(prog, "u_effect_wrap_canvas",
                                      wrap_canvas_on ? 1 : 0);
        }
        surface_->set_uniform_int(prog, "u_effect_fill", effect_fill_);
        bool fill_beyond = false;
        if (effect_layer_ == bg && !hide_here && effect_fill_ > 0 &&
            effect_wrap_canvas_ && !repeats_on_screen && affine &&
            (!L.wraps || L.size_code == 0)) {
            fill_beyond = true;
            const int slot = bg == 2 ? 0 : 2;
            for (int y : {0, FieldScene::kRows - 1}) {
                const std::int32_t ref = scene.row_affine_valid[y]
                    ? scene.row_affine[y][slot] : L.affine_ref_x;
                const int left = ref >> 8;
                const int right = (ref + 239 * L.affine_pa) >> 8;
                if (std::min(left, right) < 0 || std::max(left, right) >= 128)
                    fill_beyond = false;
            }
        }
        if (affine) {
            surface_->set_uniform_int(prog, "u_effect_fill_beyond",
                                      fill_beyond ? 1 : 0);
            surface_->set_uniform_int(prog, "u_effect_repeats",
                                      repeats_on_screen && !hide_here ? 1 : 0);
        }
        const bool text_canvas_on =
            effect_layer_ == bg && effect_have_sparks_ && effect_text_canvas_ &&
            !hide_here;
        surface_->set_uniform_int(prog, "u_effect_text_canvas",
                                  text_canvas_on ? 1 : 0);
        surface_->set_uniform_vec2(prog, "u_effect_canvas_map",
                                   static_cast<float>(effect_canvas_map_x_),
                                   static_cast<float>(effect_canvas_map_y_));
        if (affine) {
            surface_->set_uniform_vec2(
                prog, "u_effect_span",
                remap ? 240.0f / static_cast<float>(output_w_) : 0.0f,
                remap ? 160.0f / static_cast<float>(output_h_) : 0.0f);
        }

        // Precompute per-row 16-column off-screen strip shares on CPU whenever
        // a battle hold_edge layer needs them (replaces per-pixel 16-iteration
        // VRAM loops that caused Adreno 619 GPU faults on attack frames).
        if (margin_mode == 2 && (output_w_ > 240 || output_h_ > 160) &&
            !uploaded_vram_.empty() &&
            ((!affine && !text_canvas_on) || (affine && !wrap_canvas_on))) {
            bool need_strip = false;
            for (int y = 0; y < FieldScene::kRows && !need_strip; ++y) {
                const std::uint8_t* io = scene.registers_for_row(y);
                const unsigned bldcnt = io[0x50] | (static_cast<unsigned>(io[0x51]) << 8);
                if (((bldcnt >> 6) & 3u) != 1u || (bldcnt & (1u << bg)) == 0u) continue;
                const unsigned bgcnt = io[0x08 + bg * 2] | (static_cast<unsigned>(io[0x09 + bg * 2]) << 8);
                if ((bgcnt & 3u) != 1u && !text_canvas_on) continue;
                if (!affine) {
                    if ((((bgcnt >> 14) & 3u) & 1u) == 0u) need_strip = true;
                } else {
                    const int param = 0x20 + (bg - 2) * 0x10;
                    const int pa = static_cast<std::int16_t>(io[param] | (io[param + 1] << 8));
                    const int pc = static_cast<std::int16_t>(io[param + 4] | (io[param + 5] << 8));
                    if ((bgcnt & 0x2000u) != 0u && ((bgcnt >> 14) & 3u) == 1u &&
                        pa == 256 && pc == 0)
                        need_strip = true;
                }
            }
            if (need_strip) {
                const std::uint8_t* vram = uploaded_vram_.data();
                const std::size_t vram_sz = uploaded_vram_.size();
                auto vram_u8 = [&](int addr) -> std::uint8_t {
                    return (addr >= 0 && static_cast<std::size_t>(addr) < vram_sz)
                        ? vram[addr] : 0u;
                };
                auto vram_u16 = [&](int addr) -> unsigned {
                    return unsigned(vram_u8(addr)) | (unsigned(vram_u8(addr + 1)) << 8);
                };
                hold_strip_buf_.resize(static_cast<std::size_t>(output_h_) * 2u);
                const int cam_y_i = (160 - output_h_) / 2;
                for (int out_y = 0; out_y < output_h_; ++out_y) {
                    const int console_y = out_y + cam_y_i;
                    const int reg_row = std::clamp(console_y, 0, 159);
                    const std::uint8_t* io = scene.registers_for_row(reg_row);
                    const unsigned bgcnt = io[0x08 + bg * 2] |
                                           (static_cast<unsigned>(io[0x09 + bg * 2]) << 8);
                    const int char_base = static_cast<int>((bgcnt >> 2) & 3u) * 16384;
                    const int screen_base = static_cast<int>((bgcnt >> 8) & 31u) * 2048;
                    const int size_code = static_cast<int>((bgcnt >> 14) & 3u);
                    if (!affine) {
                        if ((size_code & 1) != 0) {
                            hold_strip_buf_[static_cast<std::size_t>(out_y) * 2u + 0] = 255;
                            hold_strip_buf_[static_cast<std::size_t>(out_y) * 2u + 1] = 255;
                            continue;
                        }
                        const bool eight_bit = (bgcnt & 128u) != 0u;
                        const int ht = (size_code & 2) ? 64 : 32;
                        const int hofs = (io[0x10 + bg * 4] | (static_cast<int>(io[0x11 + bg * 4]) << 8)) & 0x1FF;
                        const int vofs = (io[0x12 + bg * 4] | (static_cast<int>(io[0x13 + bg * 4]) << 8)) & 0x1FF;
                        const int py = console_y + vofs;
                        const int tile_y = (py >> 3) & (ht - 1);
                        const int block_y = (ht == 64 && tile_y >= 32) ? 1 : 0;
                        const int row_base = screen_base + block_y * 2048 + ((tile_y & 31) * 32) * 2;
                        auto text_lit_cpu = [&](int px) -> bool {
                            const int tile_x = (px >> 3) & 31;
                            const unsigned entry = vram_u16(row_base + tile_x * 2);
                            const int tile = static_cast<int>(entry & 0x3FFu);
                            int tx = px & 7;
                            int ty = py & 7;
                            if (entry & 0x400u) tx = 7 - tx;
                            if (entry & 0x800u) ty = 7 - ty;
                            if (eight_bit)
                                return vram_u8(char_base + tile * 64 + ty * 8 + tx) != 0u;
                            const std::uint8_t two = vram_u8(char_base + tile * 32 + ty * 4 + (tx >> 1));
                            return ((tx & 1) ? (two >> 4) : (two & 0xFu)) != 0u;
                        };
                        int run_right = 0, run_left = 0;
                        for (int i = 0; i < 16; ++i) {
                            if (!text_lit_cpu(240 + i + hofs)) break;
                            run_right = i + 1;
                        }
                        for (int i = 0; i < 16 - run_right; ++i) {
                            if (!text_lit_cpu(-1 - i + hofs)) break;
                            run_left = i + 1;
                        }
                        if (run_right == 16) {
                            hold_strip_buf_[static_cast<std::size_t>(out_y) * 2u + 0] = 255;
                            hold_strip_buf_[static_cast<std::size_t>(out_y) * 2u + 1] = 255;
                        } else {
                            const int gap = 16 - run_right - run_left;
                            const int sr = run_right + gap / 2;
                            hold_strip_buf_[static_cast<std::size_t>(out_y) * 2u + 0] = static_cast<std::uint8_t>(sr);
                            hold_strip_buf_[static_cast<std::size_t>(out_y) * 2u + 1] = static_cast<std::uint8_t>(16 - sr);
                        }
                    } else {
                        const int param = 0x20 + (bg - 2) * 0x10;
                        const int pa = static_cast<std::int16_t>(io[param] | (io[param + 1] << 8));
                        const int pc = static_cast<std::int16_t>(io[param + 4] | (io[param + 5] << 8));
                        if ((bgcnt & 0x2000u) == 0u || size_code != 1 || pa != 256 || pc != 0) {
                            hold_strip_buf_[static_cast<std::size_t>(out_y) * 2u + 0] = 16;
                            hold_strip_buf_[static_cast<std::size_t>(out_y) * 2u + 1] = 16;
                            continue;
                        }
                        const auto [rx, ry] = scene.affine_reference_for_row(reg_row, bg);
                        const int rows_past = console_y - reg_row;
                        int base_x, row_y;
                        if (rows_past == 0) {
                            base_x = rx >> 8;
                            row_y = (ry >> 8) & 255;
                        } else {
                            const int far_row = console_y < 0 ? 16 : 143;
                            const auto [frx, fry] = scene.affine_reference_for_row(far_row, bg);
                            const float run = static_cast<float>(rows_past) /
                                              static_cast<float>(reg_row - far_row);
                            float ex = static_cast<float>(rx) + run * static_cast<float>(rx - frx);
                            float ey = static_cast<float>(ry) + run * static_cast<float>(ry - fry);
                            if (margin.affine_step_valid) {
                                const auto& st = margin.affine_step[console_y < 0 ? 0 : 1];
                                const float past = static_cast<float>(rows_past);
                                ex = static_cast<float>(rx) + past * st[0];
                                ey = static_cast<float>(ry) + past * st[1];
                            }
                            base_x = static_cast<int>(std::floor(ex)) >> 8;
                            row_y = static_cast<int>(std::floor(ey / 256.0f)) & 255;
                        }
                        auto aff_idx = [&](int mx) -> std::uint8_t {
                            mx &= 255;
                            const int tile = vram_u8(screen_base + (row_y >> 3) * 32 + (mx >> 3));
                            return vram_u8(char_base + tile * 64 + (row_y & 7) * 8 + (mx & 7));
                        };
                        int run_right = 0, run_left = 0;
                        for (int i = 0; i < 16; ++i) {
                            if (aff_idx(base_x + 240 + i) == 0u) break;
                            run_right = i + 1;
                        }
                        for (int i = 0; i < 16 - run_right; ++i) {
                            if (aff_idx(base_x - 1 - i) == 0u) break;
                            run_left = i + 1;
                        }
                        if (run_right == 16) {
                            const std::uint8_t tint = aff_idx(base_x + 240);
                            bool flat = true;
                            for (int i = 1; i < 16; ++i)
                                if (aff_idx(base_x + 240 + i) != tint) { flat = false; break; }
                            const std::uint8_t code = flat ? 254u : 16u;
                            hold_strip_buf_[static_cast<std::size_t>(out_y) * 2u + 0] = code;
                            hold_strip_buf_[static_cast<std::size_t>(out_y) * 2u + 1] = code;
                        } else {
                            const int gap = 16 - run_right - run_left;
                            const int sr = run_right + gap / 2;
                            hold_strip_buf_[static_cast<std::size_t>(out_y) * 2u + 0] = static_cast<std::uint8_t>(sr);
                            hold_strip_buf_[static_cast<std::size_t>(out_y) * 2u + 1] = static_cast<std::uint8_t>(16 - sr);
                        }
                    }
                }
                if (uploaded_hold_strip_ != hold_strip_buf_) {
                    surface_->update_texture(hold_strip_, 0, 0, 2, output_h_,
                                             hold_strip_buf_.data());
                    uploaded_hold_strip_ = hold_strip_buf_;
                }
            }
        }

        surface_->draw_quads(prog, quads.data(), quads.size(),
                             second_pass ? bg_textures3 : bg_textures2, 14);
    };

    // Sprites go to the card in batches (records: kObjectVertex). A record
    // holds what were the sprite's own uniforms; a batch is drawn when a
    // priority's sprites are done (before that priority's backgrounds) or
    // when it holds kObjectRecords records. Quads keep the order they were
    // added in, which is the order the depth test settles equal keys by.
    using ObjectRecord = std::array<int, kObjectRecordInts>;
    gbarecomp::GpuProgram object_batch_program = 0;
    const gbarecomp::GpuTexture* object_batch_textures = nullptr;
    std::size_t object_batch_texture_count = 0;
    object_records_.clear();
    object_quads_.clear();
    auto flush_objects = [&]() {
        if (!object_quads_.empty()) {
            surface_->set_uniform_ivec4_array(
                object_batch_program, "u_rec[0]", object_records_.data(),
                static_cast<int>(object_records_.size() / 4));
            surface_->draw_quads(object_batch_program, object_quads_.data(),
                                 object_quads_.size(), object_batch_textures,
                                 object_batch_texture_count);
        }
        object_records_.clear();
        object_quads_.clear();
    };
    // The record index for a quad's a_tint.x: the last record when it is the
    // same, else a new one (drawing the batch first when it is full).
    auto object_use_record = [&](const ObjectRecord& rec) -> float {
        const std::size_t n = object_records_.size() / kObjectRecordInts;
        if (n > 0 && std::equal(rec.begin(), rec.end(),
                                object_records_.end() - kObjectRecordInts))
            return static_cast<float>(n - 1);
        if (n >= static_cast<std::size_t>(kObjectRecords)) flush_objects();
        object_records_.insert(object_records_.end(), rec.begin(), rec.end());
        return static_cast<float>(object_records_.size() / kObjectRecordInts - 1);
    };
    // The shape part of a record, shared by visible sprites and the object
    // window mask: flags for colour depth, flips, rotation and its group,
    // the native-screen hold, and the sprite and drawn-box sizes.
    auto object_shape = [&](const SceneObject& O, bool clip_native) {
        ObjectRecord rec{};
        const int g = (O.affine_index >= 0 &&
                       O.affine_index < FieldScene::kTransforms)
                      ? O.affine_index : 0;
        rec[1] = (clip_native ? 4 : 0) | (O.palette < 0 ? 8 : 0) |
                 (O.hflip ? 16 : 0) | (O.vflip ? 32 : 0) |
                 (O.affine ? 64 | (g << 8) : 0);
        rec[4] = O.width;
        rec[5] = O.height;
        rec[6] = O.affine && O.double_size ? O.width * 2 : O.width;
        rec[7] = O.affine && O.double_size ? O.height * 2 : O.height;
        return rec;
    };

    // `lifter` >= 0 draws the object's lifted copy: the console gives an
    // object's pixel the priority of any later, higher-priority object whose
    // box covers it, even where that object is transparent (gba_ppu.cpp's
    // emit_obj). Golden Sun lays an empty 64x64 priority-1 sprite over the
    // ship cabin's rug so whoever stands on it is drawn over the rug.
    // `extra`: one of the margins' own sprites (SceneMargins::margin_objects)
    // instead of the scene's slot.
    auto draw_object = [&](int slot, bool second_pass, int lifter,
                           const SceneObject* extra) {
        const SceneObject& O = extra ? *extra : scene.objects[slot];
        // A mode-2 object never draws a visible pixel; it only shapes the
        // object window, which the mask pass below already used it for.
        if (!O.present || O.window) return;
        // A character off the console's screen and off the room the margins
        // draw: no console frame shows it here, and our margins are black
        // there (FieldGameProfile::hide_objects_off_room). Effects stay
        // (set_character_objects). Field rooms only, the same gate the
        // margins' room lookup uses.
        if (!extra && prof().hide_objects_off_room &&
            character_objects_[static_cast<std::size_t>(slot)] &&
            room_source_enabled_ &&
            room_valid_ && scene.video_mode == 0) {
            const int w = O.affine && O.double_size ? O.width * 2 : O.width;
            const int h = O.affine && O.double_size ? O.height * 2 : O.height;
            const bool on_console =
                O.x + w > 0 && O.x < 240 && O.y + h > 0 && O.y < 160;
            const int rx = room_camera_x_ + O.x;
            const int ry = room_camera_y_ + O.y;
            const bool in_room = rx + w > room_min_x_ && rx < room_max_x_ &&
                                 ry + h > room_min_y_ && ry < room_max_y_;
            if (!on_console && !in_room) return;
        }
        const SceneObject* L = lifter >= 0 ? &scene.objects[lifter] : nullptr;
        // The lifted copy sorts behind every object of the lifter's priority
        // (rank 127), so where the lifter itself is opaque it still wins.
        const int key = L ? L->priority * 256 + 127
                      : extra ? O.priority * 256 + 126
                              : O.priority * 256 + object_rank[slot];
        // Only a PARKED slot is held inside the authentic window -- the same
        // rule gba_ppu.cpp's emit_obj uses. Clipping every object the
        // providers could not authenticate would cut every battle monster and
        // effect sprite off square at the console's old screen edge, since
        // the providers only authenticate field sprites. This is a fragment
        // test rather than a draw-call skip, because the quad is drawn at the
        // object's real position either way and a parked sprite could
        // otherwise cross back into the authentic window from outside it.
        // A game window (menu, shop, dialogue) is open: its own sprites are
        // priority 0 and the game hides unused ones just off the screen --
        // the shop's arrow parked at x = -32, the icon row at y = 136 whose
        // bottom 8 rows the console cuts). Keep those inside the console's window. Field
        // characters are priority 1-3, Psynergy sparks outside a window
        // keep drawing into the margins, and battles (mode 1, whose party
        // sprites hang below the screen) are left alone.
        const bool menu_sprite =
            menu_open_ && scene.video_mode == 0 && O.priority == 0;
        if (extra && menu_sprite) return;

        const float box_w = static_cast<float>(
            O.affine && O.double_size ? O.width * 2 : O.width);
        const float box_h = static_cast<float>(
            O.affine && O.double_size ? O.height * 2 : O.height);
        gbarecomp::GpuQuad quad;
        quad.x = static_cast<float>(O.x) - camera_x;
        quad.y = static_cast<float>(O.y) - camera_y;
        quad.w = box_w;
        quad.h = box_h;
        quad.u0 = 0;
        quad.v0 = 0;
        quad.u1 = box_w;
        quad.v1 = box_h;
        // During scissor-clipped FlickerPhase::Other, skip plain objects whose
        // bounding box lies wholly outside the alternating-sprite scissor rect.
        const bool has_repeat = !L && !O.parked && !menu_sprite && margins_ &&
            (output_w_ != 240 || output_h_ != 160) &&
            ((extra ? margins_->margin_object_period[0] : margins_->object_repeat[slot][0]) > 0 ||
             (extra ? margins_->margin_object_period[1] : margins_->object_repeat[slot][1]) > 0);
        const bool has_strip = !L && !extra &&
            (strip[static_cast<std::size_t>(slot)].left ||
             strip[static_cast<std::size_t>(slot)].right ||
             strip[static_cast<std::size_t>(slot)].up ||
             strip[static_cast<std::size_t>(slot)].down);
        if (phase == FlickerPhase::Other && flicker_scissor_on_ &&
            !has_repeat && !has_strip &&
            (quad.x + box_w <= static_cast<float>(flicker_sx0_) ||
             quad.x >= static_cast<float>(flicker_sx1_) ||
             quad.y + box_h <= static_cast<float>(flicker_sy0_) ||
             quad.y >= static_cast<float>(flicker_sy1_))) {
            return;
        }
        ObjectRecord rec = object_shape(O, O.parked || menu_sprite);
        rec[0] = key;
        rec[1] |= ((L ? L->blended : O.blended) ? 1 : 0) | (L ? 2 : 0) |
                  (extra ? 128 : 0);
        rec[2] = static_cast<int>(O.tile);
        rec[3] = O.palette < 0 ? 0 : O.palette;
        if (L) {
            rec[8] = L->x;
            rec[9] = L->y;
            rec[10] = L->x + L->width;
            rec[11] = L->y + L->height;
        }

        quad.depth = static_cast<float>(key) / 1024.0f;
        quad.tint[0] = object_use_record(rec);
        object_quads_.push_back(quad);
        // The game supplies the canvas continuation period, never an inferred
        // sprite kind. Copies keep the same artwork, phase, blend and order;
        // batch all repeated quads into one draw call.
        if (has_repeat) {
            const auto& period = extra ? margins_->margin_object_period
                                       : margins_->object_repeat[slot];
            const float x0 = quad.x, y0 = quad.y;
            const int left = period[0] > 0 ? static_cast<int>(std::floor((-x0 - box_w) / period[0])) + 1 : 0;
            const int right = period[0] > 0 ? static_cast<int>(std::ceil((output_w_ - x0) / period[0])) : 1;
            const int top = period[1] > 0 ? static_cast<int>(std::floor((-y0 - box_h) / period[1])) + 1 : 0;
            const int bottom = period[1] > 0 ? static_cast<int>(std::ceil((output_h_ - y0) / period[1])) : 1;
            const float win_x = -camera_x, win_y = -camera_y;
            repeat_quads_.clear();
            for (int y = top; y < bottom; ++y)
                for (int x = left; x < right; ++x) {
                    if (x == 0 && y == 0) continue;
                    gbarecomp::GpuQuad rq = quad;
                    rq.x = x0 + x * period[0];
                    rq.y = y0 + y * period[1];
                    if (margins_->object_repeat_whole &&
                        rq.x < win_x + 240.0f && rq.x + box_w > win_x &&
                        rq.y < win_y + 160.0f && rq.y + box_h > win_y)
                        continue;
                    repeat_quads_.push_back(rq);
                }
            if (!repeat_quads_.empty()) {
                ObjectRecord margin_rec = rec;
                margin_rec[1] |= 128;
                const float at = object_use_record(margin_rec);
                for (gbarecomp::GpuQuad& rq : repeat_quads_) {
                    rq.tint[0] = at;
                    object_quads_.push_back(rq);
                }
            }
        }
        if (!has_strip) return;
        // A full-width strip carries on to the view's edges (see `strip`),
        // batched into one draw call.
        const StripReach& reach = strip[static_cast<std::size_t>(slot)];
        const float x0 = quad.x, y0 = quad.y;
        const float view_w = static_cast<float>(output_w_);
        const float view_h = static_cast<float>(output_h_);
        std::vector<float> xs{x0}, ys{y0};
        if (reach.left)
            for (float x = x0 - box_w; x + box_w > 0.0f; x -= box_w) xs.push_back(x);
        if (reach.right)
            for (float x = x0 + box_w; x < view_w; x += box_w) xs.push_back(x);
        if (reach.up)
            for (float y = y0 - box_h; y + box_h > 0.0f; y -= box_h) ys.push_back(y);
        if (reach.down)
            for (float y = y0 + box_h; y < view_h; y += box_h) ys.push_back(y);
        repeat_quads_.clear();
        for (const float y : ys)
            for (const float x : xs) {
                if (x == x0 && y == y0) continue;
                gbarecomp::GpuQuad sq = quad;
                sq.x = x;
                sq.y = y;
                repeat_quads_.push_back(sq);
            }
        if (repeat_quads_.empty()) return;
        const float at = object_use_record(rec);
        for (gbarecomp::GpuQuad& sq : repeat_quads_) {
            sq.tint[0] = at;
            object_quads_.push_back(sq);
        }
    };

    // A mode-2 object's box, drawn into the mask texture the window pass
    // reads back -- see kObjectWindowFragment. This runs whenever the object
    // window is enabled (DISPCNT bit 15), regardless of whether objects are
    // even being displayed (DISPCNT bit 12): gba_ppu.cpp builds this mask
    // outside that check.
    auto draw_object_window_mask = [&](int slot) {
        const SceneObject& O = scene.objects[slot];
        if (!O.present || !O.window) return;
        // Same parked-slot confinement as draw_object above.
        ObjectRecord rec = object_shape(O, O.parked);
        rec[2] = static_cast<int>(O.tile);
        const float box_w = static_cast<float>(rec[6]);
        const float box_h = static_cast<float>(rec[7]);

        gbarecomp::GpuQuad quad;
        quad.x = static_cast<float>(O.x) - camera_x;
        quad.y = static_cast<float>(O.y) - camera_y;
        quad.w = box_w;
        quad.h = box_h;
        quad.u0 = 0;
        quad.v0 = 0;
        quad.u1 = box_w;
        quad.v1 = box_h;
        quad.tint[0] = object_use_record(rec);
        object_quads_.push_back(quad);
    };

    // Draw front-to-back by priority (objects first, then backgrounds of that
    // priority) so hardware early-Z rejects occluded lower-priority layers.
    auto draw_pass = [&](bool second_pass) {
        object_batch_program = object_;
        object_batch_textures = second_pass ? textures3 : textures2;
        object_batch_texture_count = 5;
        if (objects_on)
            surface_->set_uniform_int(object_, "u_compare_top", second_pass ? 1 : 0);
        for (int prio = 0; prio < 4; ++prio) {
            if (objects_on) {
                for (int slot = 0; slot < FieldScene::kObjects; ++slot) {
                    const SceneObject& O = scene.objects[slot];
                    if (!O.present || O.window) continue;
                    if (O.priority == prio)
                        draw_object(slot, second_pass, -1, nullptr);
                    // Unrotated lifters only: a rotated one lifts only where its
                    // transform lands inside the sprite, which a box cannot say.
                    for (int j = slot + 1; j < FieldScene::kObjects; ++j) {
                        const SceneObject& L = scene.objects[j];
                        if (!L.present || L.window || L.affine ||
                            L.priority != prio || L.priority >= O.priority) continue;
                        const int ow = O.affine && O.double_size ? O.width * 2 : O.width;
                        const int oh = O.affine && O.double_size ? O.height * 2 : O.height;
                        if (L.x >= O.x + ow || O.x >= L.x + L.width ||
                            L.y >= O.y + oh || O.y >= L.y + L.height) continue;
                        draw_object(slot, second_pass, j, nullptr);
                    }
                }
                if (margins_ && (output_w_ != 240 || output_h_ != 160))
                    for (const SceneObject& O : margins_->margin_objects)
                        if (O.priority == prio)
                            draw_object(-1, second_pass, -1, &O);
                // This priority's sprites go before its backgrounds.
                if (!second_pass && !object_quads_.empty())
                    gbarecomp::gpu_timing_switch(gbarecomp::GpuTimingStep::FrontSprites);
                flush_objects();
            }
            for (int bg = 0; bg < 4; ++bg) {
                if (!second_pass &&
                    (!bg_quads[bg][0][prio].empty() || !bg_quads[bg][1][prio].empty()))
                    gbarecomp::gpu_timing_switch(static_cast<gbarecomp::GpuTimingStep>(
                        static_cast<int>(gbarecomp::GpuTimingStep::FrontBg0) + bg));
                if (!bg_quads[bg][0][prio].empty())
                    draw_background_as(bg, second_pass, false, bg_quads[bg][0][prio]);
                if (!bg_quads[bg][1][prio].empty())
                    draw_background_as(bg, second_pass, true, bg_quads[bg][1][prio]);
            }
        }
    };

    // The object window mask and the window control texture are computed only
    // when at least one row enables a GBA window (WIN0, WIN1, or OBJWIN).
    // Skipping empty clear-only FBO passes also avoids Adreno's tile-timer bug
    // and redundant GMEM flushes.
    struct BackToGame {
        gbarecomp::GpuSurface* surface;
        bool scissor;
        ~BackToGame() {
            if (scissor) surface->clear_scissor();
            gbarecomp::gpu_timing_switch(gbarecomp::GpuTimingStep::Game);
        }
    } back_to_game{surface_, phase == FlickerPhase::Other && flicker_scissor_on_};
    if (back_to_game.scissor) {
        surface_->set_scissor(flicker_sx0_, flicker_sy0_,
                              flicker_sx1_ - flicker_sx0_,
                              flicker_sy1_ - flicker_sy0_);
    }
    if (any_window_on) {
        gbarecomp::gpu_timing_switch(gbarecomp::GpuTimingStep::Masks);
        surface_->set_depth_enabled(false);
        if (has_obj_window_sprites) {
            surface_->bind_color_target(obj_window_mask_);
            surface_->clear_color_target(0.0f, 0.0f, 0.0f, 0.0f);
            const gbarecomp::GpuTexture mask_textures[2] = {vram_, cur_row_io};
            object_batch_program = obj_window_;
            object_batch_textures = mask_textures;
            object_batch_texture_count = 2;
            for (int slot = 0; slot < FieldScene::kObjects; ++slot)
                draw_object_window_mask(slot);
            flush_objects();
        }

        surface_->bind_color_target(window_control_);
        surface_->clear_color_target(0.0f, 0.0f, 0.0f, 0.0f);
        {
            gbarecomp::GpuQuad full_window;
            full_window.x = 0;
            full_window.y = 0;
            full_window.w = static_cast<float>(output_w_);
            full_window.h = static_cast<float>(output_h_);
            full_window.u0 = 0;
            full_window.v0 = 0;
            full_window.u1 = static_cast<float>(output_w_);
            full_window.v1 = static_cast<float>(output_h_);
            surface_->draw_quads(window_, &full_window, 1, window_textures, 2);
        }
    }

    // Pass 1: the frontmost candidate at every pixel.
    gbarecomp::gpu_timing_switch(gbarecomp::GpuTimingStep::FrontPass);
    surface_->set_depth_enabled(true);
    surface_->bind_color_target(top_, /*with_depth=*/true);
    surface_->clear_color_target(clear_r, clear_g, clear_b, clear_a);
    surface_->clear_depth(1.0f);
    draw_pass(false);
    surface_->discard_depth();  // pass 2 clears it; nothing else reads it

    // Pass 2: the runner-up. Identical geometry, but each fragment discards
    // itself unless it beats what pass 1 already recorded at its own pixel.
    // Skipped completely (no target bind, no clear, no resolve read) when no
    // row uses alpha blend, no sprite is semi-transparent, and no window fades.
    bool needs_second = fade_columns_ != 0;
    for (int y = 0; y < FieldScene::kRows && !needs_second; ++y) {
        const std::uint8_t* io = scene.registers_for_row(y);
        needs_second = ((io[0x50] >> 6) & 3) == 1;
    }
    if (!needs_second && objects_on) {
        const auto see_through = [](const SceneObject& O) {
            return O.present && O.blended && !O.window;
        };
        for (const SceneObject& O : scene.objects)
            if (see_through(O)) { needs_second = true; break; }
        if (!needs_second && margins_)
            for (const SceneObject& O : margins_->margin_objects)
                if (see_through(O)) { needs_second = true; break; }
    }
    if (needs_second) {
        gbarecomp::gpu_timing_switch(gbarecomp::GpuTimingStep::SecondPass);
        surface_->bind_color_target(second_, /*with_depth=*/true);
        surface_->clear_color_target(clear_r, clear_g, clear_b, clear_a);
        surface_->clear_depth(1.0f);
        draw_pass(true);
        surface_->discard_depth();
    }

    // Resolve: one full-screen quad turns the two records into the finished
    // picture, exactly as the reference does at the end of a scanline. The
    // depth test is switched off first -- this quad's own depth is the
    // default 0, which could otherwise lose to pass 2's leftover depth buffer
    // at some pixels and leave them undrawn.
    gbarecomp::gpu_timing_switch(gbarecomp::GpuTimingStep::Finish);
    surface_->set_depth_enabled(false);
    // While the screen shakes the resolve output goes to a scratch target
    // (its shader reads pass textures at gl_FragCoord, so moving its quad
    // would not move the picture) and is drawn back offset and scaled.
    const bool shaking = shake_.active() && shake_blit_ && shake_target_ &&
                         shake_target_w_ == output_w_ &&
                         shake_target_h_ == output_h_;
    const gbarecomp::GpuTexture destination = shaking ? shake_target_ : 0;
    const bool composing =
        (phase == FlickerPhase::Plain || phase == FlickerPhase::Mix) &&
        world_compose_.mode != WorldComposeMode::None &&
        (output_w_ > 240 || output_h_ > 160) &&
        ensure_world_compose();
    surface_->bind_color_target(
        phase == FlickerPhase::Other         ? flicker_other_
        : phase == FlickerPhase::WorldMargin ? world_margin_tex_
        : phase == FlickerPhase::Mix         ? flicker_this_
        : composing                          ? compose_src_
                                             : destination);
    surface_->set_uniform_mat4(resolve_, "u_transform", transform);
    surface_->set_uniform_int(resolve_, "u_top", 0);
    surface_->set_uniform_int(resolve_, "u_second", 1);
    surface_->set_uniform_int(resolve_, "u_window", 2);
    // The blend registers are read per row inside the shader (see
    // kResolveFragment), so nothing about the colour effect is set here.
    surface_->set_uniform_int(resolve_, "u_row_io", 3);
    surface_->set_uniform_vec2(resolve_, "u_camera", camera_x, camera_y);
    surface_->set_uniform_int(resolve_, "u_fade_columns", static_cast<int>(fade_columns_ & 0x3FFFFFFFu));
    surface_->set_uniform_int(resolve_, "u_fade_row0", fade_row0_);
    surface_->set_uniform_int(resolve_, "u_fade_row1", fade_row1_);
    surface_->set_uniform_float(resolve_, "u_fade_opacity", fade_opacity_);
    surface_->set_uniform_int(resolve_, "u_red_rows", static_cast<int>(red_rows_ & 0xFFu));
    surface_->set_uniform_int(resolve_, "u_red_x0", red_x0_);
    surface_->set_uniform_int(resolve_, "u_red_x1", red_x1_);
    surface_->set_uniform_int(resolve_, "u_red_y0", red_y0_);
    surface_->set_uniform_int(resolve_, "u_red_y1", red_y1_);
    surface_->set_uniform_int(resolve_, "u_window_on", any_window_on ? 1 : 0);
    surface_->set_uniform_int(resolve_, "u_has_second", needs_second ? 1 : 0);
    const gbarecomp::GpuTexture resolve_textures[4] = {
        top_, second_, window_control_, cur_row_io};
    gbarecomp::GpuQuad full;
    full.x = 0;
    full.y = 0;
    full.w = static_cast<float>(output_w_);
    full.h = static_cast<float>(output_h_);
    surface_->draw_quads(resolve_, &full, 1, resolve_textures, 4);
    if (phase == FlickerPhase::Other) return true;
    if (phase == FlickerPhase::WorldMargin) {
        // Hand the main target back: the next begin_frame binds and clears
        // whatever target is current, which would wipe the margin picture
        // to black just before the world compose reads it.
        surface_->bind_color_target(0);
        surface_->end_frame();
        return true;
    }
    if (phase == FlickerPhase::Mix) {
        surface_->bind_color_target(composing ? compose_src_ : destination);
        surface_->set_uniform_mat4(flicker_mix_, "u_transform", transform);
        surface_->set_uniform_int(flicker_mix_, "u_this", 0);
        surface_->set_uniform_int(flicker_mix_, "u_other", 1);
        surface_->set_uniform_float(flicker_mix_, "u_weight", flicker_weight_);
        if (flicker_scissor_on_) {
            surface_->set_uniform_vec4(
                flicker_mix_, "u_flicker_box",
                static_cast<float>(flicker_sx0_),
                static_cast<float>(output_h_ - flicker_sy1_),
                static_cast<float>(flicker_sx1_),
                static_cast<float>(output_h_ - flicker_sy0_));
        } else {
            surface_->set_uniform_vec4(
                flicker_mix_, "u_flicker_box",
                0.0f, 0.0f,
                static_cast<float>(output_w_),
                static_cast<float>(output_h_));
        }
        const gbarecomp::GpuTexture mix_textures[2] = {flicker_this_,
                                                       flicker_other_};
        surface_->draw_quads(flicker_mix_, &full, 1, mix_textures, 2);
    }
    if (composing) {
        const int win_x = (output_w_ - 240) / 2;
        const int win_y = (output_h_ - 160) / 2;
        if (world_compose_.mode == WorldComposeMode::ShutterRows ||
            world_compose_.mode == WorldComposeMode::HoldMargins) {
            surface_->bind_color_target(compose_rows_);
            surface_->set_uniform_mat4(flat_rows_prog_, "u_transform", transform);
            surface_->set_uniform_int(flat_rows_prog_, "u_src", 0);
            surface_->set_uniform_int(flat_rows_prog_, "u_top", 1);
            surface_->set_uniform_int(flat_rows_prog_, "u_height", output_h_);
            surface_->set_uniform_int(flat_rows_prog_, "u_win_x", win_x);
            surface_->set_uniform_int(flat_rows_prog_, "u_win_y", win_y);
            const gbarecomp::GpuTexture flat_rows_textures[2] = {compose_src_, top_};
            gbarecomp::GpuQuad rows_quad;
            rows_quad.x = 0.0f;
            rows_quad.y = static_cast<float>(output_h_ - 2);
            rows_quad.w = 160.0f;
            rows_quad.h = 2.0f;
            surface_->draw_quads(flat_rows_prog_, &rows_quad, 1, flat_rows_textures, 2);
        }
        if (world_compose_.mode == WorldComposeMode::HoldMargins) {
            surface_->bind_color_target(compose_flat_);
            surface_->set_uniform_mat4(flat_all_prog_, "u_transform", transform);
            surface_->set_uniform_int(flat_all_prog_, "u_rows", 0);
            gbarecomp::GpuQuad all_quad;
            all_quad.x = 0.0f;
            all_quad.y = static_cast<float>(output_h_ - 1);
            all_quad.w = 1.0f;
            all_quad.h = 1.0f;
            surface_->draw_quads(flat_all_prog_, &all_quad, 1, &compose_rows_, 1);
        }
        const bool has_valid_margin =
            world_compose_.has_margin && world_margin_valid_ &&
            world_margin_tex_ != 0 && world_margin_w_ == output_w_ &&
            world_margin_h_ == output_h_;
        surface_->bind_color_target(destination);
        surface_->set_uniform_mat4(world_compose_prog_, "u_transform", transform);
        surface_->set_uniform_int(world_compose_prog_, "u_src", 0);
        surface_->set_uniform_int(world_compose_prog_, "u_margin", 1);
        surface_->set_uniform_int(world_compose_prog_, "u_flat_rows", 2);
        surface_->set_uniform_int(world_compose_prog_, "u_flat_all", 3);
        surface_->set_uniform_int(world_compose_prog_, "u_top", 4);
        surface_->set_uniform_int(world_compose_prog_, "u_row_io", 5);
        surface_->set_uniform_int(world_compose_prog_, "u_palette", 6);
        surface_->set_uniform_int(world_compose_prog_, "u_flat_map", 7);
        surface_->set_uniform_int(world_compose_prog_, "u_mode",
                                  static_cast<int>(world_compose_.mode));
        surface_->set_uniform_int(world_compose_prog_, "u_width", output_w_);
        surface_->set_uniform_int(world_compose_prog_, "u_height", output_h_);
        surface_->set_uniform_int(world_compose_prog_, "u_win_x", win_x);
        surface_->set_uniform_int(world_compose_prog_, "u_win_y", win_y);
        surface_->set_uniform_int(world_compose_prog_, "u_has_margin",
                                  has_valid_margin ? 1 : 0);
        surface_->set_uniform_int(world_compose_prog_, "u_mixed_modes",
                                  world_compose_.mixed_modes ? 1 : 0);
        surface_->set_uniform_int(world_compose_prog_, "u_glow",
                                  world_compose_.glow ? 1 : 0);
        surface_->set_uniform_int(world_compose_prog_, "u_evb",
                                  world_compose_.evb);
        surface_->set_uniform_int(world_compose_prog_, "u_shake_x",
                                  world_compose_.shake_x);
        surface_->set_uniform_int(world_compose_prog_, "u_shake_y",
                                  world_compose_.shake_y);
        surface_->set_uniform_int(world_compose_prog_, "u_flat_map_trusted",
                                  world_compose_.flat_map_trusted ? 1 : 0);
        const gbarecomp::GpuTexture compose_textures[8] = {
            compose_src_,
            has_valid_margin ? world_margin_tex_ : compose_src_,
            compose_rows_,
            compose_flat_,
            top_,
            cur_row_io,
            palette_,
            flat_world_map_ ? flat_world_map_ : vram_,
        };
        surface_->draw_quads(world_compose_prog_, &full, 1, compose_textures, 8);
    }

    // The shake, applied to the burst quads too: about the view centre,
    // scaled just enough that the shifted picture still covers the view.
    const float shake_scale =
        shaking ? shake_.cover_scale(output_w_, output_h_) : 1.0f;
    const float shake_dx = shaking ? shake_.dx() : 0.0f;
    const float shake_dy = shaking ? shake_.dy() : 0.0f;
    if (shaking) {
        surface_->bind_color_target(0);
        surface_->set_uniform_mat4(shake_blit_, "u_transform", transform);
        surface_->set_uniform_int(shake_blit_, "u_tex", 0);
        surface_->set_uniform_vec2(shake_blit_, "u_size",
                                   static_cast<float>(output_w_),
                                   static_cast<float>(output_h_));
        surface_->set_uniform_vec2(shake_blit_, "u_offset", shake_dx, -shake_dy);
        surface_->set_uniform_float(shake_blit_, "u_scale", shake_scale);
        surface_->draw_quads(shake_blit_, &full, 1, &shake_target_, 1);
    }

    // Optional particle bursts and trails, over the finished picture.
    // One simulation step per drawn frame.
    if (solid_ && (burst_.alive() || trail_.alive())) {
        std::vector<BurstQuad> burst_quads;
        trail_.collect(&burst_quads);
        burst_.collect(&burst_quads);
        // One batch per kind, in draw order: dust (behind), dirt chunks,
        // grit, flash. Everything is normally alpha blended (never additive)
        // so the colours stay earth-toned over any background; only the
        // shape differs per batch.
        struct Batch {
            BurstKind kind;
            int shape;
        };
        const Batch batches[] = {
            {BurstKind::Trail, 0},
            {BurstKind::Dust, 0},
            {BurstKind::Chunk, 1},
            {BurstKind::Geyser, 1},
            {BurstKind::Grit, 0},
            {BurstKind::Flash, 0},
        };
        surface_->set_uniform_mat4(solid_, "u_transform", transform);
        std::vector<gbarecomp::GpuQuad> quads;
        for (const Batch& batch : batches) {
            quads.clear();
            for (const BurstQuad& b : burst_quads) {
                if (b.kind != batch.kind) continue;
                gbarecomp::GpuQuad q;
                const float half_w = static_cast<float>(output_w_) * 0.5f;
                const float half_h = static_cast<float>(output_h_) * 0.5f;
                const float cx = (b.x - half_w) * shake_scale + half_w + shake_dx;
                const float cy = (b.y - half_h) * shake_scale + half_h + shake_dy;
                const float size = b.size * shake_scale;
                q.x = cx - size * 0.5f;
                q.y = cy - size * 0.5f;
                q.w = size;
                q.h = size;
                q.tint[0] = b.r;
                q.tint[1] = b.g;
                q.tint[2] = b.b;
                q.tint[3] = b.a;
                quads.push_back(q);
            }
            if (quads.empty()) continue;
            surface_->set_uniform_int(solid_, "u_shape", batch.shape);
            surface_->set_uniform_int(solid_, "u_core", 0);
            surface_->set_blend_mode(gbarecomp::GpuBlendMode::Alpha);
            surface_->draw_quads(solid_, quads.data(), quads.size(), nullptr,
                                 0);
        }
        surface_->set_blend_mode(gbarecomp::GpuBlendMode::Off);
        if (!repeat_draw_) {
            burst_.step();
            trail_.step();
        }
    }
    if (!repeat_draw_) shake_.step();

    surface_->end_frame();
    return true;
}

}  // namespace gsr
