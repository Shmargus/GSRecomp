// field_scene_renderer.cpp -- see field_scene_renderer.h.
//
// The shader below is the GBA's own background rule, expressed once and run on
// the card instead of per pixel on the processor:
//
//   screen pixel -> add the layer's scroll -> find the map entry for that tile
//   -> read the tile number, flips and palette bank -> find that pixel inside
//   the tile -> read its colour index -> look the colour up -> emit it
//
// Colour index 0 is the console's transparency and is discarded, which is what
// lets the layers stack.
//
// Alpha blending and semi-transparent objects need to know what lies directly
// BENEATH the winning pixel, and whether that is itself a blend target --
// painter's order throws that information away as soon as the next quad
// overwrites it. So every quad is drawn twice into two scratch textures with
// the GPU's own depth test doing the sorting (smaller sort_key wins, exactly
// the reference's rule): once recording the frontmost candidate ("top") and
// once recording the runner-up ("second", by discarding anything that is not
// better than what "top" already recorded at that pixel). A final full-screen
// pass then applies the console's compose rule -- alpha blend, brightness, or
// the top colour unchanged -- reading only those two records
// (`gba_ppu.cpp`, `render_scanline_internal`, the code right after the object
// loop). Each record is the RAW, unfaded, unblended 15-bit colour plus the
// sort key and a semi-transparent flag, packed into an RGBA8 texture so the
// two write passes stay simple colour output and the resolve pass does all of
// the arithmetic in one place, matching the reference exactly.
#include "field_scene_renderer.h"


#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

// The battle arena's side-margin rule, shared with the emulated path so both
// renderers extend the same layers over the same rows.
#include "battle_view.h"

namespace gsr {
namespace {

// Video memory is uploaded as a single-byte texture this wide. 96 KB divides
// exactly, and the shader turns an address into a coordinate with one divide.
constexpr int kVramTextureWidth = 1024;
constexpr int kVramBytes = 96 * 1024;
constexpr int kPaletteEntries = 512;

// Room buffer: the same four measured addresses room_buffer.h documents
// (evidence in FACTS.md), restated here rather than shared, because
// room_buffer.cpp's other work pulls in the live bus and the host config UI,
// which this renderer and its offline check tool deliberately do not link.
// How big our own effect canvas is. Wider and taller than the console's, so a
// widened view has room for the sparks the game would have clipped; the origin
// is where console (0,0) sits inside it.
constexpr int kEffectCanvasWidth = 384;
constexpr int kEffectCanvasHeight = 256;

constexpr std::uint32_t kEwramBase = 0x02000000u;
constexpr std::uint32_t kRoomRectAddr = 0x02030DC0u;  // min_x,max_x,min_y,max_y
// Integer halves of the camera clamp's 16.16 min x / min y (see upload_room).
constexpr std::uint32_t kClampMinXAddr = 0x02030DBAu;
constexpr std::uint32_t kClampMinYAddr = 0x02030DBEu;
constexpr std::uint32_t kCameraAddr = 0x02030DB0u;    // x,y, 16.16 fixed point
// The game's own scroll record per field layer, 0x30 bytes each, BG3 then BG2
// then BG1 (measured on rewinds 0052/0074/0090/0092/0093/0097, 2026-10-01):
// +0x00/+0x04 position (16.16), +0x08/+0x0C the layer's region of the grid
// (pixels, 16.16), +0x10/+0x14 speed ratio against the camera (0x10000 = 1),
// +0x18/+0x1C auto-scroll speed, +0x28/+0x2A wrap mask in 8-px tiles.
constexpr std::uint32_t kLayerRecordAddr = 0x02030DD0u;
constexpr std::uint32_t kLayerRecordBytes = 0x30u;
constexpr std::uint32_t kIdGridAddr = 0x02010000u;    // 128x128 u32, low 12 bits
constexpr std::uint32_t kAtlasAddr = 0x02020000u;     // 8 bytes per id
// The current map number (tools/find_map_id.cpp; ROADMAP.md). It is 0 until
// the game has loaded its first room and at the mode changes between rooms,
// which is the one thing the room rect below cannot tell us -- see
// room_rect_is_room's comment.
constexpr std::uint32_t kMapIdAddr = 0x02000408u;
constexpr int kGridSide = 128;
constexpr int kAtlasIds = 4096;
constexpr std::size_t kEwramBytes = 256 * 1024;
// widescreen_policy.h's measured "not a real tile" sentinels: a raw 0xffff,
// or a raw 0xF200 atlas entry on any metatile id. 0xF200 is the un-authored
// fill; seen at id 0x017 (earlier) and id 0x000 (Bilibin, gpu_frame_0084,
// 2026-09-29). Applied here too so a room the room buffer cannot really
// answer for doesn't draw the yellow/red checker bug fixed in
// room_buffer.cpp (FACTS.md, 2026-09-13).
constexpr std::uint16_t kRoomUnavailableTile = 0xFFFFu;
constexpr std::uint16_t kRoomNoMapTile = 0xF200u;

std::uint16_t ewram_u16(const std::uint8_t* ewram, std::size_t bytes,
                        std::uint32_t address) {
    const std::uint32_t off = (address - kEwramBase) & 0x3FFFFu;
    if (static_cast<std::size_t>(off) + 1 >= bytes) return 0;
    return static_cast<std::uint16_t>(ewram[off] |
                                      (static_cast<unsigned>(ewram[off + 1]) << 8));
}

// Same "is this a real room" rule as room_buffer.cpp's rect_is_room: reject
// the all-zero between-rooms marker (falls out of max_x<=min_x below) and the
// world map's constant (0,1,8,256); accept any rect with real 16px extent.
//
// This rule tells one room from another. It CANNOT tell a real room from
// memory that no room has been written to yet, because a rect of
// (0,512,0,512) passes every test here. That is what EWRAM holds on the
// name-entry screen, 24 seconds into a new game (logs/gpu_frame_0014.bin,
// frame 1451, reported 2026-09-17): the room source then answered with
// whatever was in the tables and drew yellow stripes over a screen the
// hardware draws black -- 10,038 of 38,400 pixels, INSIDE the authentic
// 240x160, not only in the margins. The map number at kMapIdAddr is the
// missing discriminator; upload_room checks it.
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
// red|green (as a 16-bit pair) is the raw palette colour exactly as the
// console stored it, unfaded and unblended; blue plus the low two bits of
// alpha is the console's own sort key (priority*256 + 128+layer for a
// background, priority*256 + slot for an object); bit 4 of alpha marks a
// semi-transparent object. The resolve pass (see kResolveFragment) is the only
// place that turns this into a colour, exactly as the reference compositor
// does it in one place rather than per layer.
//
// `u_compare_top` selects which of the two peel passes this draw is: 0 for the
// first (nothing to compare against yet), 1 for the second, where a fragment
// discards itself unless its own key beats whatever the first pass already
// recorded at this pixel -- which is what turns the second draw into the
// runner-up rather than a duplicate of the first.
const char* kBackgroundFragment =
    "#version 130\n"
    "in vec2 v_screen;\n"
    "uniform usampler2D u_vram;\n"      // raw video memory, one byte per texel
    "uniform usampler2D u_palette;\n"   // 512 fifteen-bit colours
    "uniform sampler2D u_window;\n"     // this pixel's 6-bit window control
    "uniform usampler2D u_id_grid;\n"   // room buffer: 128x128 metatile ids
    "uniform usampler2D u_atlas;\n"     // room buffer: 4 tile entries per id
    "uniform usampler2D u_row_io;\n"    // this frame's per-row registers
    // Each row's own affine reference point, the console's internal
    // accumulator rather than the register file: eight 16-bit halves per row,
    // BG2 x, BG2 y, BG3 x, BG3 y, low half then high half, each biased by
    // 2^29 so the stored number is never negative (see upload_row_affine).
    "uniform usampler2D u_row_affine;\n"
    "uniform sampler2D u_top_record;\n" // pass 1's result; read only in pass 2
    "uniform int u_affine;\n"           // 1: this layer is an affine background
    "uniform int u_palette_half;\n"     // 0 for backgrounds, 256 for objects
    "uniform vec2 u_camera;\n"          // world offset of the output's origin
    "uniform int u_world_loaded;\n"
    "uniform int u_layer;\n"            // 0-3: this background's window bit
    "uniform int u_layer_striped;\n"    // 1: enable bit varies line by line
    "uniform int u_rest_dy;\n"          // rows from now to the rest position
    "uniform int u_compare_top;\n"      // 0 = first pass, 1 = second pass
    "uniform int u_use_room;\n"         // 1: source this layer from the room
    // The layer's own position from the game's record (room_reference_for):
    // when on, each row's scroll is read as that position plus its offset
    // from it in -512..511.
    "uniform int u_room_ref_on;\n"
    "uniform int u_haze_room;\n"
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
    // Our own spell-effect canvas, drawn host-side from the game's particles
    // because the game clips every spark to a canvas as wide as the console
    // screen, and that limit cannot be widened in place (effect_sources.h).
    // u_use_effect is 0 on every frame we have not verified, and the path
    // below is then exactly what it was.
    "uniform usampler2D u_effect;\n"
    "uniform int u_effect_extra;\n"   // our sparks fill where the game drew nothing
    "uniform vec2 u_effect_size;\n"
    "uniform vec2 u_effect_origin;\n"
    // Console size divided by output size: the factor that turns a position
    // in our view into the console position whose canvas pixel belongs
    // there. Zero disables the remap entirely.
    "uniform vec2 u_effect_span;\n"
    // 1 when this effect layer WRAPS and its map shows the game's 128x128
    // canvas at map (0,0), tile i at (i % 16, i / 16) -- checked on the CPU
    // (FieldSceneRenderer::analyse_effect_canvas). Ray draws this way
    // (gpu_rewind_0046). u_effect_fill is the one value the game's canvas
    // holds wherever no spark landed (its flashing tint), 0 when there is
    // no single such value.
    "uniform int u_effect_wrap_canvas;\n"
    "uniform int u_effect_fill;\n"
    // 1 when this effect layer is a non-wrapping canvas stretched over the
    // console's whole width (Thor, Djinn: 128x128 at PA 128, screen x 0..239
    // on canvas x 0..119, gpu_rewind_0088): past the canvas, out in the
    // margins, the picture is the canvas's fill, so the game's flashing tint
    // reaches the edges. Set per layer by draw().
    "uniform int u_effect_fill_beyond;\n"
    // 1 when this effect layer is a wrapping canvas the console itself shows
    // repeating within one row (Torch, gpu_rewind_0124): the margins keep
    // drawing the wrapped map, sparks or not. Set per layer by draw().
    "uniform int u_effect_repeats;\n"
    // 1 when this effect layer is a REGULAR background whose map shows the
    // game's 128x128 canvas as one 16x16 block of consecutive tiles, whose
    // top-left map pixel is u_effect_canvas_map (Ivan's hit sparks on BG1,
    // gpu_rewind_0047). Checked on the CPU like u_effect_wrap_canvas.
    "uniform int u_effect_text_canvas;\n"
    // Hide the game's own effect canvas on this layer (set_effect_hidden):
    // 1 = text layer, the 128x128 block at u_effect_canvas_map draws
    // transparent; 2 = wrapping affine layer, canvas at the map origin.
    "uniform int u_effect_hide;\n"
    "uniform vec2 u_effect_canvas_map;\n"
    "out vec4 o_colour;\n"
    "\n"
    "uint vram_byte(int address) {\n"
    "  ivec2 at = ivec2(address % 1024, address / 1024);\n"
    "  return texelFetch(u_vram, at, 0).r;\n"
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
    // (gpu_rewind_0025, FACTS.md 2026-09-24).
    "uniform usampler2D u_room_vram;\n"
    // Golden Sun's whole world map as tile indices (world_map_source.h):
    // 512 tiles wide, BG3 in rows 0..511 and BG2 in rows 512..1023. The
    // game's own map is a 512-pixel ring refilled around the camera, and
    // the widened zoom-out reaches past it (FACTS.md, 2026-09-25).
    "uniform usampler2D u_world_map;\n"
    "uniform int u_use_world;\n"
    "uint room_vram_byte(int address) {\n"
    "  return texelFetch(u_room_vram, ivec2(address % 1024, address / 1024), 0).r;\n"
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
    "  uint id = texelFetch(u_id_grid, ivec2(stx >> 1, sty >> 1), 0).r;\n"
    "  uint e = texelFetch(u_atlas, ivec2((sty & 1) * 2 + (stx & 1), int(id)),\n"
    "                      0).r;\n"
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
    // Whether a text background draws anything at this map pixel (already
    // scrolled): the console's own wrapped tilemap fetch, colour 0 or not.
    "bool text_lit(ivec2 pixel, int screen_base, int char_base, int size_code,\n"
    "              int eight_bit) {\n"
    "  int wt = ((size_code & 1) != 0) ? 64 : 32;\n"
    "  int ht = ((size_code & 2) != 0) ? 64 : 32;\n"
    "  int tile_x = (pixel.x >> 3) & (wt - 1);\n"
    "  int tile_y = (pixel.y >> 3) & (ht - 1);\n"
    "  int block = 0;\n"
    "  if (wt == 64 && tile_x >= 32) block += 1;\n"
    "  if (ht == 64 && tile_y >= 32) block += (wt == 64) ? 2 : 1;\n"
    "  uint entry = vram_half(screen_base + block * 2048\n"
    "                        + (((tile_y & 31) * 32) + (tile_x & 31)) * 2);\n"
    "  int tile = int(entry & 0x3FFu);\n"
    "  int tx = pixel.x & 7;\n"
    "  int ty = pixel.y & 7;\n"
    "  if ((entry & 0x400u) != 0u) tx = 7 - tx;\n"
    "  if ((entry & 0x800u) != 0u) ty = 7 - ty;\n"
    "  if (eight_bit != 0)\n"
    "    return vram_byte(char_base + tile * 64 + ty * 8 + tx) != 0u;\n"
    "  uint two = vram_byte(char_base + tile * 32 + ty * 4 + (tx >> 1));\n"
    "  return (((tx & 1) != 0) ? (two >> 4) : (two & 0xFu)) != 0u;\n"
    "}\n"
    "\n"
    // The same question for a 256x256 affine background (one tile byte per
    // entry, 8-bit pixels), at a map pixel that wraps.
    "bool affine_lit(int mx, int my, int screen_base, int char_base) {\n"
    "  mx &= 255;\n"
    "  my &= 255;\n"
    "  int tile = int(vram_byte(screen_base + (my >> 3) * 32 + (mx >> 3)));\n"
    "  return vram_byte(char_base + tile * 64 + (my & 7) * 8 + (mx & 7)) != 0u;\n"
    "}\n"
    "\n"
    // This layer's own HOFS/VOFS, as THIS row's copy of the registers held
    // them -- the same per-scanline read the console does, which is what
    // lets a frame that rewrites scroll mid-frame draw correctly. A row the
    // game never captured its own copy of falls back to the frame's
    // end-of-frame registers (see field_scene.h, `io_bytes`), which is
    // exactly the single value every row used before this existed.
    "uint row_u16(int row, int offset) {\n"
    "  ivec2 lo = ivec2(offset, row);\n"
    "  ivec2 hi = ivec2(offset + 1, row);\n"
    "  return texelFetch(u_row_io, lo, 0).r | (texelFetch(u_row_io, hi, 0).r << 8);\n"
    "}\n"
    "\n"
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
    "  int lo = int(texelFetch(u_row_affine, ivec2(which * 2, row), 0).r);\n"
    "  int hi = int(texelFetch(u_row_affine, ivec2(which * 2 + 1, row), 0).r);\n"
    "  return (hi << 16) + lo - 536870912;\n"
    "}\n"
    "\n"
    "void main() {\n"
    "  ivec2 console_px = ivec2(floor(v_screen + u_camera));\n"
    // Rows outside the console's own 0..159 have no real per-row register
    // state (there was no scanline there); the nearest real row's own
    // registers extend into that margin, same as the frame-level uniform
    // this replaced did for the whole picture.
    "  int reg_row = clamp(console_px.y, 0, 159);\n"
    // This layer's own control register, as THIS row's copy of it held it,
    // never the frame's. Golden Sun rewrites BGxCNT mid-frame: on the live
    // battle capture logs/gpu_frame_0001.bin, BG2CNT changes at row 26 (tile
    // block 2 -> 3) and again at row 136 (map block 7 -> 31, size 1 -> 0),
    // and BG1CNT changes priority 3 -> 0 at row 136. A frame-level read is
    // only ever the LAST row's value, which is exactly why rows 136..159 of
    // that frame were already pixel-exact while all 136 rows above it were
    // drawn from the wrong tile and map blocks (FACTS.md, 2026-09-16). The
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
    // file menu (gpu_rewind_0035; the emulated reference drew the logo).
    // Every field row in the captures has BG2 and BG3 on (DISPCNT 0x0F00 of
    // 222 signature rows); the rest were the title and BG0-only fades.
    "  bool room_row_authorized = u_use_room != 0 &&\n"
    "      (row_dispcnt & 7u) == 0u &&\n"
    "      (row_dispcnt & 0x0C00u) == 0x0C00u &&\n"
    "      (((row_u16(reg_row, 0x0E) >> 8) & 0x1Fu) == 5u) &&\n"
    "      (((row_u16(reg_row, 0x0C) >> 8) & 0x1Fu) == 6u) &&\n"
    "      (((row_u16(reg_row, 0x0A) >> 8) & 0x1Fu) == 7u);\n"
    "  int bg_char_base = int((bgcnt >> 2) & 3u) * 16384;\n"
    "  int bg_screen_base = int((bgcnt >> 8) & 31u) * 2048;\n"
    "  int bg_size_code = int((bgcnt >> 14) & 3u);\n"
    // An affine background is 8-bit whatever bit 7 says: gba_ppu.cpp's
    // render_affine_bg reads one byte per pixel unconditionally.
    "  int bg_eight_bit = (u_affine != 0 || (bgcnt & 128u) != 0u) ? 1 : 0;\n"
    "  int bg_wraps = ((bgcnt & 8192u) != 0u) ? 1 : 0;\n"
    // The same sort key the C++ side used to compute once per frame:
    // priority first, then background index, smaller wins (GL_LESS).
    "  int bg_key = int(bgcnt & 3u) * 256 + 128 + u_layer;\n"
    // ...and this row's own DISPCNT enable bit. The draw is issued whenever
    // ANY row turns this layer on; a row that does not is dropped here,
    // rather than the whole layer standing or falling on the frame's last
    // row -- the same read the reference makes, which takes each row's
    // DISPCNT from that row's own registers.
    // A layer the game stripes line by line (draw_background's
    // layer_striped) carries the stripes into the margins: rows outside
    // 0..159 take the enable bit of row (y mod 160).
    "  int enable_row = reg_row;\n"
    "  if (u_layer_striped != 0 && (console_px.y < 0 || console_px.y > 159))\n"
    "    enable_row = ((console_px.y % 160) + 160) % 160;\n"
    "  if (((row_u16(enable_row, 0x00) >> uint(8 + u_layer)) & 1u) == 0u)\n"
    "    discard;\n"
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
    "    if (first_hofs <= -2048 || first_hofs >= 2048 ||\n"
    "        first_vofs <= -2048 || first_vofs >= 2048) {\n"
    "      scroll_row = 1;\n"
    "      continue_bg3_sky = true;\n"
    "    }\n"
    "  }\n"
    // Lamakan's heat haze is each row's small offset from the layer's
  // record; rows above and below the console held row 0's, so the shimmer
  // stopped at the screen edge (Jimmy, gpu_frame_0127/0128). A layer read
  // through its record takes the offset of the console row mirrored across
  // the edge (row 0 itself is the desert's odd one, so the top starts at 1).
  "  if (u_room_ref_on != 0 && !continue_bg3_sky && console_px.y < 0)\n"
  "    scroll_row = min(-console_px.y, 159);\n"
  "  if (u_room_ref_on != 0 && !continue_bg3_sky && console_px.y > 159)\n"
  "    scroll_row = max(318 - console_px.y, 0);\n"
  "  uint hofs_raw = row_u16(scroll_row, 0x10 + u_layer * 4);\n"
    "  uint vofs_raw = row_u16(scroll_row, 0x10 + u_layer * 4 + 2);\n"
    // The accumulator's high bits name no room position; its low nine bits
    // are the actual BG3 sample coordinate. Use those for the room-backed
    // top extension so it keeps the sky texture while moving with the band.
    "  if (continue_bg3_sky) { hofs_raw &= 0x1FFu; vofs_raw &= 0x1FFu; }\n"
    "  bool outside_native = console_px.x < 0 || console_px.x >= 240 ||\n"
    "                        console_px.y < 0 || console_px.y >= 160;\n"
    // Above the console first row or below its last. The arena widens
    // SIDEWAYS only -- src/battle_view.h states that model: "enabled arena
    // layers continue into the left and right margins only". Extending it up
    // and down was added to fill the black bands there, and measurement says
    // it does not: on logs/gpu_frame_0017.bin the backdrop draws black on 36
    // of the 40 rows above the arena and, on the other 4, a stripe of its own
    // artwork from a place the window never shows -- the yellow line across
    // the top of a battle reported 2026-09-17. See FACTS.md.
    "  bool outside_native_v = console_px.y < 0 || console_px.y >= 160;\n"
    // May this layer continue into the left and right margins here?
    //
    //   u_margin_mode 1 -- always. Outside battle, a rotated or scaled
    //     background is the overworld's own map and its transform is defined
    //     for every column, so widening it simply shows more world.
    //   u_margin_mode 2 -- this is BG1 or BG2 and the frame may be a battle;
    //     the arena continues sideways only down the rows of the game's own
    //     live scene band, and only while this layer is carrying the arena
    //     rather than a spell effect. That is the rule the emulated path
    //     already uses (src/battle_view.h and runner_main.cpp's
    //     golden_sun_battle_bg_sample_provider), and it is evaluated HERE,
    //     from this row's own registers, for the same reason the affine walk
    //     above is: the emulated path asks its margin policy once per
    //     scanline, and a battle rewrites WIN0V and BLDCNT during the frame.
    //     Deciding it once from the end-of-frame register file answers for
    //     the last row and blacks out the margins on all the others.
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
    // its own, so it asks row 0 or row 159 -- the same extension `reg_row`
    // already makes for the registers. Without this the arena stopped dead
    // at the console's top and bottom edges and the widened view showed a
    // black band above and below it, which is what the player sees while a
    // battle menu is open.
    "  } else if (u_margin_mode == 2) {\n"
    "    int band_y = clamp(console_px.y, 0, 159);\n"
    "    uint rdisp = row_u16(reg_row, 0x00);\n"
    "    uint win0v = row_u16(reg_row, 0x44);\n"
    "    int y1 = int((win0v >> 8) & 0xFFu);\n"
    "    int y2 = int(win0v & 0xFFu);\n"
    // Window 0 switched off: the band no longer limits the rows. The camera's
    // push-in to the command view does this (DISPCNT 0x1741, WIN0V left at
    // 0x1088), and the console then shows the arena from row 0, so its first
    // 16 rows belong to the band too (bug_report_20261005_094058, rewinds
    // 0002/0006: the left margin stopped at row 16).
    "    if ((rdisp & 0x2000u) == 0u) y1 = 0;\n"
    // ABOVE the console's own screen, ask the band's FIRST row, not row 0.
    // The band does not always start at the top: Golden Sun opens a battle
    // with WIN0V y1 = 0 and moves it to y1 = 16 once the first turn begins
    // (measured on gpu_frame_0001 vs _0009, 2026-09-16). Clamping to row 0
    // therefore landed outside the band from the first turn onwards, and the
    // arena that had been reaching the top of the widened view dropped back
    // to a black band -- exactly what the player saw. Below the screen the
    // clamp to row 159 stands: down there the nearest real row is the
    // command menu, and the arena genuinely does not continue into it.
    "    if (console_px.y < 0) band_y = y1;\n"
    "    bool band = y1 < 160 && y1 < y2 && y2 >= 80 && y2 <= 160;\n"
    "    if ((rdisp & 7u) == 1u &&\n"
    "        (rdisp & 0x0600u) != 0u && band &&\n"
    "        band_y >= y1 && band_y < y2) {\n"
    "      uint bgcnt = row_u16(reg_row, 0x08 + u_layer * 2);\n"
    // An effect is a first target of an alpha blend sitting at priority 1;
    // it is drawn where the battlers are. Two things have been tried and
    // both were wrong:
    //
    //   * excluding it from the margins -- the effect stopped dead at the
    //     console's old screen edge, a hard vertical seam down a widened
    //     battle (reported on a Venus summon);
    //   * widening it like the arena -- it then repeated at the layer's map
    //     period, wrapping into the margins off the wrong edges, which is
    //     what that exclusion had been written to prevent.
    //
    // So it reaches the margin, but is sampled with its source column HELD at
    // the console's own edge instead of wrapped (see clamp_effect below).
    // Nothing out there is invented and nothing repeats: the boundary column
    // extends outward, so the effect stretches off the side of the picture
    // rather than being cut off square or starting over.
    "      uint bldcnt = row_u16(reg_row, 0x50);\n"
    "      bool effect = ((bldcnt >> 6) & 3u) == 1u &&\n"
    "                    (bldcnt & (1u << uint(u_layer))) != 0u &&\n"
    "                    (bgcnt & 3u) == 1u;\n"
    "      bool self_limiting = u_affine != 0 && (bgcnt & 0x2000u) == 0u;\n"
    "      bool size_ok = (u_layer != 2) || (((bgcnt >> 14) & 3u) == 1u)\n"
    "                     || self_limiting;\n"
    // Above the screen only the rotated arena carries on, and only when the
    // band starts at row 0: its transform is defined there and a map without
    // wraparound ends in transparency, not in a stripe of unrelated artwork
    // (the regular backdrop's stripe, gpu_frame_0017, stays excluded).
    "      arena_margin = size_ok && (!outside_native_v ||\n"
    "          (console_px.y < 0 && y1 == 0 && u_affine != 0));\n"
    "      hold_edge = effect;\n"
    "    }\n"
    "  }\n"
    "  bool room_supplied = false;\n"
    // Set together with room_copy below: this pixel also takes its colour
    // from the saved field palette (palette row 1, upload_room_palette).
    "  bool room_colours = false;\n"
    // Whether this pixel lies inside the room at all, kept separate from
    // whether the room buffer had an answer for it. A margin pixel past the
    // room's own edge is outside the world, and the only honest thing out
    // there is black -- see the discard below.
    "  bool room_covers = false;\n"
    "  ivec2 room_source = ivec2(-1);\n"
    // True when the GAMEs own canvas has no pixel for this position: the
    // affine sample fell outside its map. That is precisely where a spark
    // it clipped belongs, and the only place we add one.
    "  bool beyond_game_canvas = false;\n"
    "  ivec2 effect_px = ivec2(0);\n"
    // A scroll larger than the whole shared map is not a position in it. The
    // grid is 128 metatiles of 16 pixels, so 2048 is the entire world on that
    // axis; Tret's sky sits at 33503, sixteen times that, because the game
    // scrolls it by adding to a running total and lets the console mask it to
    // 9 bits. Such a layer is a repeating pattern with no place in the room,
    // and the console's own wrapped tilemap is the honest answer for it in
    // the margins -- which is already proven, because that is exactly what
    // draws it INSIDE the window on frames that are pixel-identical to the
    // hardware.
    //
    // This is deliberately narrower than the test tried and reverted earlier
    // today ("the room could not address this pixel"), which also caught
    // ordinary room layers and wrapped a cave room's own scenery into its
    // margins. Measured over the 865 mode 0 field frames of the corpus, NO
    // layer scrolls past the grid, so this fires on none of them (FACTS.md,
    // 2026-09-16).
    // The register is 16 bits and the game also writes NEGATIVE scrolls into
    // it. Golden Sun nudges a battle backdrop left by five pixels by storing
    // 65531, and reading that as an unsigned 65531 made it look like a scroll
    // sixteen times past the whole grid -- so the Tret rule below held the
    // boundary column and smeared the arena across both margins. That is the
    // horizontal stretch reported on Granite, 2026-09-17
    // (logs/gpu_frame_0015.bin: BG1 scroll x=65531, i.e. -5; the same
    // backdrop at x=204 on gpu_frame_0017.bin never smeared).
    //
    // It is MAGNITUDE that separates the two cases, not sign. Tret's sky
    // accumulator reads 33503/33651/33448 (gpu_frame_0002/0011/0003), which
    // as a signed 16-bit value is about -32000 -- still enormous, still with
    // no place in the map, and the Tret rule must keep firing on it. A -5
    // nudge is a position. So resolve the sign, then compare the distance
    // from zero against the whole grid.
    "  int hofs_s = int(hofs_raw); if (hofs_s >= 32768) hofs_s -= 65536;\n"
    "  int vofs_s = int(vofs_raw); if (vofs_s >= 32768) vofs_s -= 65536;\n"
    "  bool scroll_beyond_map = abs(hofs_s) >= 2048 || abs(vofs_s) >= 2048;\n"
    "  int index;\n"
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
    // reference extrapolated to it on the way in (see upload_row_affine), so
    // this reads the same number the old frame-level walk produced.
    //
    // Whole pixel indices only (console_px, reg_row are already integers,
    // floored before this point) -- an interpolated coordinate would sample
    // the neighbouring texel once the transform rotates it.
    // THE EXPANDED VIEW AS THE CANVAS.
    //
    // The effect canvas is as wide as the CONSOLE screen and the game
    // cannot be made to build a wider one: its stamp generator takes one
    // size for both axes, emits a corrupt instruction above 128, and the
    // next size up does not fit its heap. Nor is it holding anything back
    // -- across 295 recorded stamp calls it never once asks to draw outside
    // its canvas, so there are no clipped sparks to recover (FACTS.md,
    // 2026-09-18).
    //
    // What CAN be done is decide where that canvas lands. Sampling it
    // through the widened view instead of the console window makes its
    // columns span our whole screen, so the effect reaches both edges
    // instead of stopping where the console used to end. Nothing is
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
    // Above row 0 and below row 159 there was no scanline, so there is no
    // latched reference -- `reg_row` has clamped to the nearest real row.
    // Repeating that row draws the same line of the map over and over, which
    // is the vertical banding the expanded world map showed above and below
    // the authentic 160 rows. Keep walking instead, one step per row, where
    // the step is the trend over the last 16 rows, for the reference and
    // for PA/PC alike. PB/PD are NOT the step to use: the game reloads the
    // reference itself every scanline on the world map, and leaves PB at
    // zero while doing it, so a PB walk would not move at all. Nor is the
    // edge row's single step: the world map's PA drops one unit every few
    // rows and the reference jumps 120/256 px sideways with it to keep the
    // centre column still, so a step taken across such a row, repeated
    // with PA held, sheared the margin sideways (gpu_rewind_0151/0152,
    // FACTS.md 2026-10-07). Rows 0..159 keep the exact integer walk.
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
    "      if (epa * float(pa) < 0.0) epa = 0.0;\n"
    "      tex_x = int(floor((ex + float(aff_px.x) * epa) / 256.0));\n"
    "      tex_y = int(floor((ey + float(aff_px.x) * epc) / 256.0));\n"
    "      ref_x = int(floor(ex));\n"
    "    } else {\n"
    "      tex_x = (ref_x + aff_px.x * pa) >> 8;\n"
    "      tex_y = (ref_y + aff_px.x * pc) >> 8;\n"
    "    }\n"
    // A battle spell drawn on a WRAPPING 256-pixel affine layer at 1:1
    // (Ray: BG2CNT 0x6785, PA 256, PC 0, gpu_rewind_0046) repeats its
    // left-hand bolts in the right margin, because past map column 255 the
    // layer starts over. Same rule as the text-layer effect strip below:
    // each margin continues into the 16 columns the console never shows
    // (240..255), shared out by what runs into them from each edge, and
    // never past them. A row lit across the whole strip (Ray's full-screen
    // tint) holds the strip's far column outward on BOTH sides, so the tint
    // reaches the edges without the bolts coming round again (FACTS.md,
    // 2026-09-25).
    "    if (hold_edge && outside_native && bg_wraps != 0 &&\n"
    "        u_effect_wrap_canvas == 0 &&\n"
    "        bg_size_code == 1 && pa == 256 && pc == 0) {\n"
    "      int base_x = ref_x >> 8;\n"
    "      int row_y = tex_y & 255;\n"
    "      int run_right = 0;\n"
    "      int run_left = 0;\n"
    "      for (int i = 0; i < 16; ++i) {\n"
    "        if (!affine_lit(base_x + 240 + i, row_y, bg_screen_base,\n"
    "                        bg_char_base)) break;\n"
    "        run_right = i + 1;\n"
    "      }\n"
    "      for (int i = 0; i < 16 - run_right; ++i) {\n"
    "        if (!affine_lit(base_x - 1 - i, row_y, bg_screen_base,\n"
    "                        bg_char_base)) break;\n"
    "        run_left = i + 1;\n"
    "      }\n"
    "      bool right = aff_px.x >= 240;\n"
    "      int step = right ? aff_px.x - 240 : -1 - aff_px.x;\n"
    "      if (run_right == 16) {\n"
    "        step = min(step, 15);\n"
    "      } else {\n"
    "        int gap = 16 - run_right - run_left;\n"
    "        int share = right ? run_right + gap / 2\n"
    "                          : 16 - (run_right + gap / 2);\n"
    "        if (step >= share) {\n"
    "          if (gap > 0 || share <= 0) discard;\n"
    "          step = share - 1;\n"
    "        }\n"
    "      }\n"
    "      tex_x = base_x + (right ? 240 + step : -1 - step);\n"
    "    }\n"
    "    effect_px = ivec2(tex_x, tex_y);\n"
    // World-map pixels outside the console's screen come from the whole
    // world map rather than the game's ring. Only on this row's world-map
    // layout: mode 2, BG3 map block 8, BG2 map block 10 (the rings at VRAM
    // 0x4000 and 0x5000). The map coordinate is the world pixel; the world
    // repeats every 4096.
    "    bool world_row = u_use_world != 0 && outside_native &&\n"
    "        (row_dispcnt & 7u) == 2u &&\n"
    "        (((row_u16(reg_row, 0x0E) >> 8) & 0x1Fu) == 8u) &&\n"
    "        (((row_u16(reg_row, 0x0C) >> 8) & 0x1Fu) == 10u);\n"
    "    if (world_row) {\n"
    "      ivec2 wt = ivec2((tex_x >> 3) & 511,\n"
    "                       ((tex_y >> 3) & 511) + (u_layer == 2 ? 512 : 0));\n"
    "      int tile = int(texelFetch(u_world_map, wt, 0).r);\n"
    "      index = int(vram_byte(bg_char_base + tile * 64"
    " + (tex_y & 7) * 8 + (tex_x & 7)));\n"
    "    } else {\n"
    "    int bg_pixels = 128 << bg_size_code;\n"
    // A wrapping canvas layer: past the game's 128x128 canvas nothing is
    // taken from the wrapped map in the margins. There the picture is the
    // canvas's own fill with our captured sparks over it, the ones the game
    // clipped at its canvas edge; inside the console's screen the game's
    // pixel stays and only those clipped sparks are added (see below).
    // The fill alone also counts: Meteor's tint on its wrapping 128 canvas
    // has no sparks on some frames (gpu_rewind_0106).
    "    bool wrap_beyond = ((u_effect_wrap_canvas != 0 && u_effect_extra != 0) ||\n"
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
    "        u_effect_fill_beyond == 0 && u_effect_repeats == 0) discard;\n"
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
    // so their density changed at a hard line 16 px into the margin
    // (gpu_rewind_0101). Out here our sparks join the game's by the larger
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
    "      if (at.x >= 0.0 && at.y >= 0.0 &&\n"
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
    // smears the scrolling pattern across the margin (gpu_frame_0052).
    //
    // Sideways, each margin continues into the map's own off-screen strip:
    // the columns between x=240 and the point where the map wraps back onto
    // the screen's left edge (16 pixels on a 256-wide map). Those are real
    // canvas pixels the console would show on a wider screen. Both margins
    // reach that strip from opposite ends, so per row the strip is shared
    // out by what is in it: an effect running unbroken from the right edge
    // belongs to the right margin, one running from the left edge to the
    // left margin, and any gap between is split in half. Past its share a
    // margin draws nothing -- the effect has genuinely ended there. Only a
    // row lit across the whole strip (a full-width effect, which would
    // otherwise stop square) still holds its last column outward.
    //
    // Holding column 239 stretched whatever sat on the edge into a bar
    // across the margin (gpu_rewind_0011, frame 59); holding the middle of
    // the strip did the same to a spark 9 pixels wide (gpu_rewind_0020,
    // frame 102). FACTS.md, 2026-09-24. A 512-wide map's strip (272) is
    // wider than both margins, so only its first 16 columns are examined.
    "    ivec2 src_px = console_px;\n"
    "    if (hold_edge && outside_native && !text_canvas) {\n"
    "      src_px.y = clamp(src_px.y, 0, 159);\n"
    "      int strip = (((bg_size_code & 1) != 0) ? 512 : 256) - 240;\n"
    "      ivec2 scroll = ivec2(int(hofs_raw) & 0x1FF, int(vofs_raw) & 0x1FF);\n"
    "      int run_right = 0;\n"
    "      int run_left = 0;\n"
    "      for (int i = 0; i < 16; ++i) {\n"
    "        if (!text_lit(ivec2(240 + i, src_px.y) + scroll, bg_screen_base,\n"
    "                      bg_char_base, bg_size_code, bg_eight_bit)) break;\n"
    "        run_right = i + 1;\n"
    "      }\n"
    "      for (int i = 0; i < 16 - run_right; ++i) {\n"
    "        if (!text_lit(ivec2(-1 - i, src_px.y) + scroll, bg_screen_base,\n"
    "                      bg_char_base, bg_size_code, bg_eight_bit)) break;\n"
    "        run_left = i + 1;\n"
    "      }\n"
    "      bool right = src_px.x >= 240;\n"
    "      int step = right ? src_px.x - 240 : -1 - src_px.x;\n"
    "      if (strip == 16) {\n"
    "        int gap = strip - run_right - run_left;\n"
    "        int share = right ? run_right + gap / 2 : strip - (run_right + gap / 2);\n"
    "        if (step >= share) {\n"
    "          if (gap > 0 || share <= 0) discard;\n"
    "          step = share - 1;\n"
    "        }\n"
    "      } else if (step >= strip / 2) {\n"
    "        discard;\n"
    "      }\n"
    "      src_px.x = right ? 240 + step : -1 - step;\n"
    "    }\n"
    "    ivec2 pixel = src_px + ivec2(int(hofs_raw) & 0x1FF, int(vofs_raw) & 0x1FF);\n"
    // Text backgrounds always wrap, over 256 or 512 pixels per axis.
    "    int width_tiles  = ((bg_size_code & 1) != 0) ? 64 : 32;\n"
    "    int height_tiles = ((bg_size_code & 2) != 0) ? 64 : 32;\n"
    "    int tile_x = (pixel.x >> 3) & (width_tiles - 1);\n"
    "    int tile_y = (pixel.y >> 3) & (height_tiles - 1);\n"
    // Each 32x32 block of entries is its own 2 KB screenblock.
    "    int block = 0;\n"
    "    if (width_tiles == 64 && tile_x >= 32) block += 1;\n"
    "    if (height_tiles == 64 && tile_y >= 32) block += (width_tiles == 64) ? 2 : 1;\n"
    "    int entry_address = bg_screen_base + block * 2048\n"
    "                      + (((tile_y & 31) * 32) + (tile_x & 31)) * 2;\n"
    // The console's own wrapped tilemap: the honest fallback whenever the
    // room buffer does not know this pixel, and the whole answer when
    // u_use_room is 0.
    "    uint entry = vram_half(entry_address);\n"
    "\n"
    // Room buffer: world pixel -> tile -> metatile id -> quadrant -> grid
    // cell -> atlas entry -> tile entry. The room rect/camera bound which
    // pixels are inside the room at all; the layer's own UNMASKED scroll
    // (not the game's camera) gives this layer's source tile, exactly the
    // rule room_buffer.cpp's render path uses and Goma Cave/Tret confirmed
    // in play (FACTS.md, 2026-09-11 and 2026-09-13). u_use_room is always 0
    // for an affine layer (never reaches this branch), so this stays the
    // field-only path it was measured on.
    // Only OUTSIDE the console's own 240x160 window. Inside it the console's
    // tilemap is the exact answer by definition; letting the room rule
    // replace it there made the native picture wrong wherever the rule reads
    // the wrong grid region -- Lamakan Desert, whose BG2/BG3 scroll 1024 px
    // off the camera, drew 29,544 of 38,400 native pixels wrong
    // (logs/gpu_frame_0106.bin, 2026-09-30).
    // One exception inside the window: Lamakan's haze lifts the top rows up
    // to 3 px above the layer's own row 0, into map rows above our raised
    // camera (the inset) that the game never streamed into VRAM, which drew
    // rows 1-2 black (gpu_frame_0127/0131/0132, the room's top edge). In a
    // frame read through the layer records, those pixels come from the room.
    "    bool haze_above = false;\n"
    "    if (room_row_authorized && !outside_native && u_haze_room != 0) {\n"
    "      int haze_dy = ((int(vofs_raw) - int(u_room_ref.y) + 512) & 1023) - 512;\n"
    "      haze_above = console_px.y + haze_dy < 0;\n"
    "    }\n"
    "    if (room_row_authorized && (outside_native || haze_above)) {\n"
    "      ivec2 room_px = console_px + ivec2(u_ewram_camera);\n"
    "      room_covers = room_px.x >= int(u_room_min.x) &&\n"
    "                    room_px.y >= int(u_room_min.y) &&\n"
    "                    room_px.x < int(u_room_max.x) &&\n"
    "                    room_px.y < int(u_room_max.y);\n"
    "      if (room_covers) {\n"
    "        ivec2 room_scroll = ivec2(int(hofs_raw), int(vofs_raw));\n"
    // Kolima's flood shows the flooded copy of the area, kept 176 rows
    // lower in the map, on a growing set of rows (VOFS 312 -> 488). Beside
    // that copy the map holds unrelated tiles, so those rows read the
    // layer's own position in the margins (bug_report_20261004_155305).
    // While it drains or refills, the other state of the whole room is
    // elsewhere in the grid (drain: 512 px left, 1024 down; refill: 512
    // left, 512 down; the bottom layer does not change), found by
    // split_row_offset. Then a jumping row reads it there, and rows above
    // and below the console take the jump of the console row mirrored
    // across the edge, so the margins stripe like the rows beside them
    // (the pond sits at the top of the screen; FACTS.md 2026-10-07).
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
    // The crow's nest sky (gpu_rewind_0093): above the sky art its region
    // holds one filler tile (the brown band); carry the art's top row up.
    "        if (u_hold_top != 0 && !continue_bg3_sky)\n"
    "          source.y = max(source.y, u_hold_floor);\n"
    // An auto-scrolling layer wraps inside its own region, as the game
    // streams it (the sandstorm: 256 x 256 at x 1024, gpu_rewind_0097).
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
    "        room_source = source;\n"
    "        if (source.x >= 0 && source.y >= 0) {\n"
    "          int stx = source.x >> 3;\n"
    "          int sty = source.y >> 3;\n"
    "          int gx = stx >> 1;\n"
    "          int gy = sty >> 1;\n"
    "          if (gx >= 0 && gy >= 0 && gx < 128 && gy < 128) {\n"
    "            uint id = texelFetch(u_id_grid, ivec2(gx, gy), 0).r;\n"
    "            int sub = (sty & 1) * 2 + (stx & 1);\n"
    "            uint candidate = texelFetch(u_atlas, ivec2(sub, int(id)), 0).r;\n"
    "            bool unavailable = candidate == 0xFFFFu ||\n"
    "                               candidate == 0xF200u;\n"
    "            if (!unavailable) { entry = candidate; room_supplied = true; }\n"
    "          }\n"
    "        }\n"
    "      }\n"
    "    }\n"
    "\n"
    // Outside the console's own 240x160 window, the console's wrapped
    // tilemap is not a real answer for this pixel -- there is no screen row
    // there to wrap from, only whatever VRAM happens to hold at that
    // wrapped address (menu/font art included). Drawing it is noise, not
    // information, so an unanswered pixel out here discards instead,
    // leaving the backdrop -- exactly the native path's own default for
    // "nothing drawn here" (see the two-pass clear colour above). Inside the
    // native window this changes nothing: the video-memory fallback is
    // untouched, which is what keeps the native picture pixel-identical.
    //
    // The scroll-beyond-map exception is also bounded by the room. A layer
    // with no place in the map may fill the margin where the margin is still
    // INSIDE the room; past the room's own edge there is no world at all, and
    // painting a held boundary column out there is the "smear at the top
    // left" reported on Tret's branches. Measured on
    // logs/gpu_frame_0027.bin, 2026-09-18: camera x = 0 against a room rect
    // of x 0..1024, so the whole left margin is outside the room and draws
    // black on 195 of its 240 rows -- while on the 45 sky-accumulator rows
    // this exception held BG3's boundary column and hung a flat cyan bar in
    // that black (one colour per row, 60 px wide, y 41..85 of the output).
    // The Tret captures whose margins must stay filled are not affected:
    // camera x is 157/116/97/121/89 on gpu_frame_0002/_0011/_0003/_0019/
    // _0021, every one at least 60 px past the room's left edge, so the room
    // covers their whole margin.
    "    if (outside_native && !room_supplied && !arena_margin &&\n"
    "        !(room_row_authorized && scroll_beyond_map && room_covers)) discard;\n"
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
    // (gpu_rewind_0147/0148).
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
    // Our canvas replaces the index outright when this layer is one we have
    // verified. Everything after it -- windows, blending, the two-pass peel
    // -- is untouched, so an effect we draw composes exactly as the game's
    // own does.
    // Only where the game drew nothing. Inside its canvas its picture
    // stands untouched: we are ADDING the sparks it threw away, not
    // reproducing the effect. A partial reproduction would lose
    // everything the capture does not see, which is what replacing
    // the whole layer did on session_20260918_172002.
    "  if ((u_effect_extra != 0 || u_effect_fill_beyond != 0) &&\n"
    "      beyond_game_canvas) {\n"
    "    vec2 at = vec2(effect_px) + u_effect_origin;\n"
    "    index = (outside_native && u_effect_fill_beyond != 0) ? u_effect_fill : 0;\n"
    "    if (u_effect_extra != 0 && at.x >= 0.0 && at.y >= 0.0 &&\n"
    "        at.x < u_effect_size.x && at.y < u_effect_size.y) {\n"
    // Joined by the larger value, as the game composes its canvas.
    "      index = max(index, int(texelFetch(u_effect, ivec2(at), 0).r));\n"
    "    }\n"
    "  }\n"
    // Index 0 is the console's transparent colour, in every path.
    "  if (index == 0) discard;\n"
    "\n"
    // Bits 0-3 of the window control are BG0-3's own enable. An arena pixel
    // out in the side margin skips this: the game's windows are written in
    // console coordinates and say nothing about a column that does not exist
    // on the console, so applying them out here would clip the arena away
    // again. The emulated path makes the same exemption, and only for these
    // remapped margin samples (g_ws_bg_sample_provider_ignore_window_layers).
    "  if (!(outside_native && arena_margin)) {\n"
    "    int wc = int(round(texelFetch(u_window, ivec2(gl_FragCoord.xy), 0).r"
    " * 255.0));\n"
    "    if (((wc >> u_layer) & 1) == 0) discard;\n"
    "  }\n"
    "\n"
    "  if (u_compare_top != 0) {\n"
    "    vec4 top = texelFetch(u_top_record, ivec2(gl_FragCoord.xy), 0);\n"
    "    int top_key = int(round(top.b * 255.0))\n"
    "                | ((int(round(top.a * 255.0)) & 3) << 8);\n"
    "    if (bg_key <= top_key) discard;\n"
    "  }\n"
    "\n"
    "  uint c = texelFetch(u_palette, ivec2(u_palette_half + index,\n"
    "                                        room_colours ? 1 : 0), 0).r;\n"
    // Priority is a per-row value now, so the depth the two-layer peel sorts
    // by cannot come from the quad any more. This reproduces exactly what the
    // vertex path produced for a quad of depth key/1024: ortho() leaves clip
    // z equal to that value with w = 1, and the default depth range maps
    // [-1,1] onto [0,1].
    "  gl_FragDepth = (float(bg_key) / 1024.0 + 1.0) * 0.5;\n"
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
const char* kObjectVertex =
    "#version 130\n"
    "in vec2 a_pos;\n"
    "in vec2 a_uv;\n"
    "in float a_depth;\n"
    "in vec4 a_tint;\n"
    "uniform mat4 u_transform;\n"
    "out vec2 v_local;\n"
    "out vec2 v_screen;\n"
    "void main() {\n"
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

const char* kObjectFragment =
    "#version 130\n"
    "in vec2 v_local;\n"
    "in vec2 v_screen;\n"
    "uniform usampler2D u_vram;\n"
    "uniform usampler2D u_palette;\n"
    "uniform sampler2D u_window;\n"     // this pixel's 6-bit window control
    "uniform usampler2D u_row_io;\n"    // this frame's per-row registers
    "uniform sampler2D u_top_record;\n" // pass 1's result; read only in pass 2
    "uniform vec2 u_size;\n"         // sprite size in pixels
    "uniform int u_tile;\n"          // first tile number
    "uniform int u_bank;\n"          // 4-bit palette bank
    "uniform int u_eight_bit;\n"
    "uniform int u_hflip;\n"
    "uniform int u_vflip;\n"
    "uniform int u_affine;\n"        // 0 = plain, 1 = rotated or scaled
    "uniform vec4 u_transform_pd;\n" // PA, PB, PC, PD, already unscaled
    "uniform vec2 u_box;\n"          // drawn box, twice the sprite if doubled
    "uniform int u_key;\n"              // this object's sort key
    "uniform int u_semi_transparent;\n" // object mode 1 (OBJ blend flag)
    "uniform int u_compare_top;\n"      // 0 = first pass, 1 = second pass
    "uniform vec2 u_camera;\n"          // world offset of the output's origin
    // 0: this object's position came from the widescreen provenance
    // providers on both axes, so it may be drawn anywhere, margins
    // included. 1: at least one axis is the hardware sign-wrap fallback,
    // so gba_ppu.cpp's emit_obj only ever draws it inside the authentic
    // 240x160 window -- reproduced below by discarding outside it.
    "uniform int u_clip_native;\n"
    // 1: this draw is the object's lifted copy (draw_object's `lifter`),
    // kept inside the lifting object's box, in console pixels.
    "uniform int u_lift;\n"
    "uniform vec4 u_lift_box;\n"
    "out vec4 o_colour;\n"
    "\n"
    "uint vram_byte(int address) {\n"
    "  return texelFetch(u_vram, ivec2(address % 1024, address / 1024), 0).r;\n"
    "}\n"
    "\n"
    "uint row_u16(int row, int offset) {\n"
    "  return texelFetch(u_row_io, ivec2(offset, row), 0).r |\n"
    "         (texelFetch(u_row_io, ivec2(offset + 1, row), 0).r << 8);\n"
    "}\n"
    "\n"
    "void main() {\n"
    "  ivec2 console_px = ivec2(floor(v_screen + u_camera));\n"
    "  int reg_row = clamp(console_px.y, 0, 159);\n"
    "  uint dispcnt = row_u16(reg_row, 0x00);\n"
    "  if (((dispcnt >> 12u) & 1u) == 0u) discard;\n"
    "  if (u_lift != 0) {\n"
    "    if (float(console_px.x) < u_lift_box.x ||\n"
    "        float(console_px.y) < u_lift_box.y ||\n"
    "        float(console_px.x) >= u_lift_box.z ||\n"
    "        float(console_px.y) >= u_lift_box.w) discard;\n"
    "  }\n"
    // Same output-pixel-to-console-pixel convention as kBackgroundFragment:
    // v_screen is this fragment's output pixel (the object quad was placed in
    // that same space -- see draw_object's quad.x = O.x - camera_x), so
    // adding u_camera recovers the console pixel the way that shader does.
    "  if (u_clip_native != 0) {\n"
    "    if (console_px.x < 0 || console_px.x >= 240 ||\n"
    "        console_px.y < 0 || console_px.y >= 160) discard;\n"
    "  }\n"
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
    "  tile = tile & 0x3FF;\n"
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
    "    if (index != 0) index += u_bank * 16;\n"
    "  }\n"
    "  if (index == 0) discard;\n"
    "\n"
    // Bit 4 of the window control is the OBJ layer's own enable.
    "  int wc = int(round(texelFetch(u_window, ivec2(gl_FragCoord.xy), 0).r"
    " * 255.0));\n"
    "  if (((wc >> 4) & 1) == 0) discard;\n"
    "\n"
    "  if (u_compare_top != 0) {\n"
    "    vec4 top = texelFetch(u_top_record, ivec2(gl_FragCoord.xy), 0);\n"
    "    int top_key = int(round(top.b * 255.0))\n"
    "                | ((int(round(top.a * 255.0)) & 3) << 8);\n"
    "    if (u_key <= top_key) discard;\n"
    "    if (int(round(top.b * 255.0)) < 128) discard;\n"
    "  }\n"
    "\n"
    // Object colours live in the second half of palette memory.
    "  uint c = texelFetch(u_palette, ivec2(256 + index, 0), 0).r;\n"
    "  int a4 = (u_semi_transparent != 0) ? 4 : 0;\n"
    "  o_colour = vec4(float(int(c) & 0xFF) / 255.0,\n"
    "                  float((int(c) >> 8) & 0xFF) / 255.0,\n"
    "                  float(u_key & 0xFF) / 255.0,\n"
    "                  float(((u_key >> 8) & 3) | a4) / 255.0);\n"
    "}\n";

// The object window's own pass. A mode-2 object never draws a visible pixel
// (gba_ppu.cpp skips obj_mode 2 in its colour loop) -- it only says where the
// object window covers the screen, which the window pass below reads back to
// choose the OBJ-window row of WININ/WINOUT. Shares kObjectVertex, so this is
// the same box-and-tile walk as a real object with the colour lookup removed:
// only whether the tile pixel is the console's transparent index 0 matters.
const char* kObjectWindowFragment =
    "#version 130\n"
    "in vec2 v_local;\n"
    "in vec2 v_screen;\n"
    "uniform usampler2D u_vram;\n"
    "uniform usampler2D u_row_io;\n"    // this frame's per-row registers
    "uniform vec2 u_size;\n"
    "uniform int u_tile;\n"
    "uniform int u_eight_bit;\n"
    "uniform int u_hflip;\n"
    "uniform int u_vflip;\n"
    "uniform int u_affine;\n"
    "uniform vec4 u_transform_pd;\n"
    "uniform vec2 u_box;\n"
    "uniform vec2 u_camera;\n"          // world offset of the output's origin
    // See kObjectFragment's u_clip_native: an untrusted object window shape
    // must also stay confined to the authentic window, for the same reason
    // gba_ppu.cpp's emit_obj confines it -- an object window box the game
    // parked off-screen must not carve a hole in the margin.
    "uniform int u_clip_native;\n"
    "out vec4 o_colour;\n"
    "\n"
    "uint vram_byte(int address) {\n"
    "  return texelFetch(u_vram, ivec2(address % 1024, address / 1024), 0).r;\n"
    "}\n"
    "\n"
    "uint row_u16(int row, int offset) {\n"
    "  return texelFetch(u_row_io, ivec2(offset, row), 0).r |\n"
    "         (texelFetch(u_row_io, ivec2(offset + 1, row), 0).r << 8);\n"
    "}\n"
    "\n"
    "void main() {\n"
    "  ivec2 console_px = ivec2(floor(v_screen + u_camera));\n"
    "  int reg_row = clamp(console_px.y, 0, 159);\n"
    "  uint dispcnt = row_u16(reg_row, 0x00);\n"
    "  if (((dispcnt >> 15u) & 1u) == 0u) discard;\n"
    "  if (u_clip_native != 0) {\n"
    "    if (console_px.x < 0 || console_px.x >= 240 ||\n"
    "        console_px.y < 0 || console_px.y >= 160) discard;\n"
    "  }\n"
    "  ivec2 size = ivec2(u_size);\n"
    "  int lx;\n"
    "  int ly;\n"
    "  if (u_affine != 0) {\n"
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
    "  if (u_hflip != 0) lx = size.x - 1 - lx;\n"
    "  if (u_vflip != 0) ly = size.y - 1 - ly;\n"
    "\n"
    "  int tile_x = lx >> 3;\n"
    "  int tile_y = ly >> 3;\n"
    "  int tiles_wide = size.x >> 3;\n"
    "  int step = (u_eight_bit != 0) ? 2 : 1;\n"
    "  int tile;\n"
    "  if (((dispcnt >> 6u) & 1u) != 0u) {\n"
    "    tile = u_tile + (tile_y * tiles_wide + tile_x) * step;\n"
    "  } else {\n"
    "    tile = u_tile + tile_y * 32 + tile_x * step;\n"
    "  }\n"
    "  tile = tile & 0x3FF;\n"
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
    "  o_colour = vec4(1.0, 1.0, 1.0, 1.0);\n"
    "}\n";

// The resolve pass: one full-screen quad reading the two peel records and
// writing the console's compose rule, exactly as the reference does at the
// end of a scanline (gba_ppu.cpp:1044-1074). A record's low byte of the key
// decodes which kind of layer won: 0..127 is an object (kind 4), 128..131 is
// background 0-3 (key - 128), and 255 is the cleared "nothing drawn" sentinel,
// which is read back as the backdrop (kind 5) -- see the clear colour chosen
// in draw().
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
    "out vec4 o_colour;\n"
    "\n"
    "uint row_u16(int row, int offset) {\n"
    "  return texelFetch(u_row_io, ivec2(offset, row), 0).r |\n"
    "         (texelFetch(u_row_io, ivec2(offset + 1, row), 0).r << 8);\n"
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
    // (FACTS.md, 2026-09-15) -- Golden Sun fades the arena without fading the
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
    "  vec4 t = texelFetch(u_top, ivec2(gl_FragCoord.xy), 0);\n"
    "  vec4 s = texelFetch(u_second, ivec2(gl_FragCoord.xy), 0);\n"
    "  int ta = int(round(t.a * 255.0));\n"
    "  int sa = int(round(s.a * 255.0));\n"
    "  int top_color = int(round(t.r * 255.0)) | (int(round(t.g * 255.0)) << 8);\n"
    "  int second_color = int(round(s.r * 255.0)) | (int(round(s.g * 255.0)) << 8);\n"
    "  int top_low = int(round(t.b * 255.0));\n"
    "  int second_low = int(round(s.b * 255.0));\n"
    "  int top_layer = layer_of(top_low);\n"
    "  int second_layer = layer_of(second_low);\n"
    "  bool top_semi = (ta & 4) != 0;\n"
    // Bit 5 of the window control is blend_enabled(x) -- the reference
    // multiplies it into EVERY target1, backdrop included, which is why it is
    // applied once here rather than baked into a peel record.
    "  int wc = int(round(texelFetch(u_window, ivec2(gl_FragCoord.xy), 0).r"
    " * 255.0));\n"
    "  bool blend_ok = ((wc >> 5) & 1) != 0;\n"
    // An OBJ is a first target when semi-transparent or when BLDCNT
    // selects the OBJ layer; only a semi-transparent one forces alpha
    // (gba_ppu.cpp, FACTS.md 2026-09-25).
    "  bool top_target1 = blend_ok && ((top_layer == 4)\n"
    "                    ? (top_semi || ((u_first_targets >> 4) & 1) != 0)\n"
    "                    : (((u_first_targets >> top_layer) & 1) != 0));\n"
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
    "  o_colour = vec4(float(r) / 31.0, float(g6 >> 1) / 31.0,\n"
    "                  float(b) / 31.0, 1.0);\n"
    "}\n";

// The window pass: one full-screen quad computing GBATEK's window_control(x)
// for every pixel and packing it into the red channel (0..63/255), read back
// by the peel and resolve passes above. Priority WIN0 > WIN1 > object window
// > outside, exactly gba_ppu.cpp:668-720; with no window enabled every layer
// and blending are on (0x3F). Window rectangles are the console's own
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
    // The iris wipe's ellipse (fit_iris_window), used outside the console's
    // screen only: centre in console pixels, squared half-axes.
    "uniform int u_iris;\n"
    "uniform vec2 u_iris_centre;\n"
    "uniform vec2 u_iris_axes;\n"
    "out vec4 o_colour;\n"
    "\n"
    "uint row_u16(int row, int offset) {\n"
    "  ivec2 lo = ivec2(offset, row);\n"
    "  ivec2 hi = ivec2(offset + 1, row);\n"
    "  return texelFetch(u_row_io, lo, 0).r | (texelFetch(u_row_io, hi, 0).r << 8);\n"
    "}\n"
    "\n"
// GBATEK: WINnV is [Y1,Y2); Y2>160 or Y1>Y2 forces Y2=160.
    //
    // An edge sitting exactly ON the console's screen boundary means the
    // window continues past it. That is not a guess about intent, it is what
    // the effect is: Golden Sun's curtain wipe alternates WIN0H between
    // x=0..75 on even rows and x=165..240 on odd rows (measured on
    // logs/gpu_frame_0007.bin), so every band has one edge pinned to a screen
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
    // WIN1H reads 0x0000 on Tret (logs/gpu_frame_0019.bin, _0021), which is
    // x1 == x2 == 0: zero pixels wide on hardware. The open-edge rule below
    // saw x1 == 0, called the left edge open, and reduced the test to
    // "x < 0" -- which is exactly the left margin and nothing else. The
    // margin therefore fell INSIDE win1 and took its control byte instead of
    // WINOUT, switching colour blending on out there while the rest of the
    // picture had it off. That is the pale left strip reported 2026-09-17:
    // measured +19.1 brighter than the centre on the 116 ordinary rows and
    // +28.8 on the 44 accumulator rows -- every row, which is what ruled out
    // the scrolling-sky explanation.
    //
    // Inside the console's own 0..239 / 0..159 the old form and this one
    // agree (both reject every real coordinate), which is why all three
    // pixel-identity gates passed throughout.
    // Clamping as mGBA does (gba_ppu.cpp win_h_in): X1 past the screen and
    // past X2 reads as 0, X2 past the screen clamps, and X1 > X2 wraps --
    // the Boreas summon's WIN0H 0xFFF1 is the full width
    // (logs/gpu_frame_0113.bin). A wrapping window reaches both margins.
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
    "  int win0h = int(row_u16(reg_row, 0x40));\n"  // X1 bits 8-15, X2 bits 0-7
    "  int win1h = int(row_u16(reg_row, 0x42));\n"
    "  int win0v = int(row_u16(reg_row, 0x44));\n"
    "  int win1v = int(row_u16(reg_row, 0x46));\n"
    "  int winin = int(row_u16(reg_row, 0x48));\n"
    "  int winout = int(row_u16(reg_row, 0x4A));\n"
    "  int control;\n"
    "  bool any_window = win0_en || win1_en || objwin_en;\n"
    // Reveal: WIN0 carves the circle and WIN1 the Psynergy meter, both
    // showing everything (WININ 0x3F3F), outside only BG0 (WINOUT 0x0001).
    // Among the 6,590 captured frames only Reveal's (gpu_frame_0104,
    // gpu_rewind_0054) use it. The circle lies inside the console's screen,
    // but WIN0V covers the full height, so the open-edge rule below
    // stretched the circle's first row up the top margin and WIN1 (x 0..16)
    // across the left margin. Outside the 240x160 window it is outside the
    // circle; with Better Field Psy the circle covers the whole view.
    "  bool reveal = (dispcnt & 7u) == 0u && win0_en && win1_en &&\n"
    "                winin == 0x3F3F && winout == 0x0001;\n"
    "  bool outside_native = screen.x < 0 || screen.x >= 240 ||\n"
    "                        screen.y < 0 || screen.y >= 160;\n"
    "  if (reveal && u_reveal_full != 0) {\n"
    "    control = winin & 0x3F;\n"
    "  } else if (reveal && outside_native) {\n"
    "    control = winout & 0x3F;\n"
    "  } else if (!any_window) {\n"
    "    control = 0x3F;\n"
    "  } else if (win0_en && (u_iris != 0 && outside_native\n"
    "                 ? iris_inside(screen)\n"
    "                 : (win_v(win0v, screen.y) && win_h(win0h, screen.x)))) {\n"
    "    control = winin & 0x3F;\n"
    "  } else if (win1_en && win_v(win1v, screen.y) &&\n"
    "             win_h(win1h, screen.x)) {\n"
    "    control = (winin >> 8) & 0x3F;\n"
    "  } else if (objwin_en &&\n"
    "             texelFetch(u_obj_window_mask, ivec2(gl_FragCoord.xy), 0).r"
    " > 0.5) {\n"
    "    control = (winout >> 8) & 0x3F;\n"
    "  } else {\n"
    "    control = winout & 0x3F;\n"
    "  }\n"
    "  o_colour = vec4(float(control) / 255.0, 0.0, 0.0, 1.0);\n"
    "}\n";

// Screen-space projection over the output, y downward, column-major.
// The z row must map depth 0..1 (the sort key, scaled) onto a clip-space z
// that INCREASES with depth, because the depth test that makes the two-layer
// peel work relies on GL's default GL_LESS: a smaller stored depth wins,
// which must mean a smaller sort key. Depth is unused elsewhere in this file
// (always the vertex default of 0), so this row was never exercised before.
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
    if (resolve_) surface_->destroy_program(resolve_);
    if (solid_) surface_->destroy_program(solid_);
    if (shake_blit_) surface_->destroy_program(shake_blit_);
    if (shake_target_) surface_->destroy_texture(shake_target_);
    if (obj_window_) surface_->destroy_program(obj_window_);
    if (window_) surface_->destroy_program(window_);
    if (vram_) surface_->destroy_texture(vram_);
    if (palette_) surface_->destroy_texture(palette_);
    if (id_grid_) surface_->destroy_texture(id_grid_);
    if (atlas_) surface_->destroy_texture(atlas_);
    if (row_io_) surface_->destroy_texture(row_io_);
    if (row_affine_) surface_->destroy_texture(row_affine_);
    if (world_map_) surface_->destroy_texture(world_map_);
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

    object_ = surface_->create_program(kObjectVertex, kObjectFragment,
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

    obj_window_ = surface_->create_program(kObjectVertex, kObjectWindowFragment,
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
    palette_ = surface_->create_texture(kPaletteEntries, 2,
                                        gbarecomp::GpuTextureFormat::R16UI,
                                        false);
    id_grid_ = surface_->create_texture(kGridSide, kGridSide,
                                        gbarecomp::GpuTextureFormat::R16UI,
                                        false);
    atlas_ = surface_->create_texture(4, kAtlasIds,
                                      gbarecomp::GpuTextureFormat::R16UI,
                                      false);
    row_io_ = surface_->create_texture(
        static_cast<int>(FieldScene::kLineIoBytes),
        static_cast<int>(FieldScene::kRows),
        gbarecomp::GpuTextureFormat::R8UI, false);
    // Eight 16-bit halves per row: BG2 x, BG2 y, BG3 x, BG3 y, low then high.
    row_affine_ = surface_->create_texture(
        8, static_cast<int>(FieldScene::kRows),
        gbarecomp::GpuTextureFormat::R16UI, false);
    // Our spell-effect canvas. Sized for the widest view we present, so the
    // sparks the game clips at its own canvas edge have somewhere to land.
    effect_pixels_.reset(kEffectCanvasWidth, kEffectCanvasHeight);
    effect_canvas_ = surface_->create_texture(
        kEffectCanvasWidth, kEffectCanvasHeight,
        gbarecomp::GpuTextureFormat::R8UI, false);
    // The world map's whole tile layout, both layers (world_map_source.h).
    world_map_ = surface_->create_texture(512, 1024,
                                          gbarecomp::GpuTextureFormat::R8UI,
                                          false);
    if (!vram_ || !palette_ || !id_grid_ || !atlas_ || !row_io_ ||
        !row_affine_ || !effect_canvas_ || !world_map_) {
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
    output_w_ = width;
    output_h_ = height;
    return true;
}

void FieldSceneRenderer::upload_vram(const std::uint8_t* vram,
                                     std::size_t vram_bytes) {
    if (!ready_ || !vram || vram_bytes == 0) return;
    const int rows = static_cast<int>(
        std::min<std::size_t>(vram_bytes, kVramBytes) / kVramTextureWidth);
    if (rows > 0) {
        surface_->update_texture(vram_, 0, 0, kVramTextureWidth, rows, vram);
        surface_->update_texture(room_vram_, 0, 0, kVramTextureWidth, rows,
                                 vram);
    }
    // Background map blocks only (the first 64 KB): room_layer_agreement()
    // compares the room rule's entries against them.
    vram_maps_.assign(vram, vram + std::min<std::size_t>(vram_bytes, 0x10000));
    // Object tiles (the last 32 KB): object_is_solid() reads them.
    if (vram_bytes > 0x10000)
        vram_objects_.assign(vram + 0x10000,
                             vram + std::min<std::size_t>(vram_bytes, 0x18000));
    else
        vram_objects_.clear();
}

// Whether every pixel of an unrotated object is the same colour index, read
// the way kObjectFragment reads it. A cutscene bar is one solid tile; the
// Sol Sanctum cloud row is not (logs/gpu_rewind_0136).
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
            tile &= 0x3FF;
            const int base = tile * 32;
            const int tx = lx & 7, ty = ly & 7;
            int index;
            if (eight_bit) {
                index = vram_objects_[static_cast<std::size_t>(
                    (base + ty * 8 + tx) & 0x7FFF)];
            } else {
                const int two = vram_objects_[static_cast<std::size_t>(
                    (base + ty * 4 + (tx >> 1)) & 0x7FFF)];
                index = (tx & 1) ? (two >> 4) : (two & 0xF);
            }
            if (first < 0) first = index;
            else if (index != first) return false;
        }
    }
    return true;
}

bool FieldSceneRenderer::room_entry(int screen_x, int screen_y, int scroll_x,
                                    int scroll_y, std::uint16_t* entry) const {
    if (!query_room_known(screen_x, screen_y, scroll_x, scroll_y)) return false;
    const int stx = (scroll_x + screen_x) >> 3;
    const int sty = (scroll_y + screen_y) >> 3;
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
// (logs/gpu_rewind_0052/0053, 2026-09-30), so a check made from it failed
// on most frames and the margins flickered between desert and black.
int FieldSceneRenderer::room_layer_agreement(const FieldScene& scene, int bg,
                                             int* compared,
                                             bool from_record) const {
    if (compared) *compared = 0;
    const SceneLayer& L = scene.layers[bg];
    if (!room_valid_ || L.affine || vram_maps_.empty()) return -1;
    int total = 0, same = 0;
    // Rows showing another copy of the area are not the room's own tiles;
    // the margins read the record for them instead (row_split_vertical).
    const bool split = row_split_vertical(scene, bg);
    for (int y = 4; y < 160; y += 8) {
        if (split && row_jumps_vertically(scene, bg, y)) continue;
        const std::uint8_t* io = scene.row_io_valid[y]
            ? scene.row_io[y].data() : scene.io_bytes.data();
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
        for (int x = 4; x < 240; x += 8) {
            std::uint16_t want = 0;
            if (!room_entry(x, y, room_sx, room_sy, &want)) continue;
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
// its heat-haze rows hand BG1 and BG3 each other's +1024 (logs/
// gpu_frame_0105/0107.bin, 2026-09-30), so the scroll's own region matched
// 0% of the console's tiles. The game's own record of the layer's position
// (kLayerRecordAddr, region included) plus the row's small offset from it
// (the haze, -1 as 0xFFFF at a left edge: gpu_rewind_0055) matches. Until
// 2026-10-01 this searched four fixed 1024-px regions around the camera
// instead, which cannot express regions such as the ship's 960. Try the
// scroll's own region first, so every room it already fits is untouched.
// A row's offset from layer `bg`'s recorded position, folded into
// -512..511 the way the shader folds it.
static void row_offset_from_record(const FieldScene& scene, int bg, int y,
                                   int pos_x, int pos_y, int* dx, int* dy) {
    const std::uint8_t* io = scene.row_io_valid[y]
        ? scene.row_io[y].data() : scene.io_bytes.data();
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

// Measured over every saved capture (3,623 mode 0 frames, 2026-10-05): rows
// sit within 9 px of the record (heat haze, a frame's lag), or the whole
// frame is offset, or -- Tret's sky only -- some rows jump sideways. A
// vertical jump on some rows while others sit on the record is Kolima's
// flood alone (+176, gpu_rewind_0002/0003 of bug_report_20261004_155305).
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

// Kolima's drain and refill (map 43, gpu_rewind_0154-0157, FACTS.md
// 2026-10-07): the jumping rows show the other state of the pond, which the
// room grid holds for the whole room 512 px left and 1024 (drain) or 512
// (refill) down of the layer's own region; the bottom layer is the same in
// both states (offset 0). Match the console's tiles on the jumping rows
// against each 512-px offset and take the best, if it agrees.
bool FieldSceneRenderer::split_row_offset(const FieldScene& scene, int bg,
                                          int* off_x, int* off_y) const {
    const LayerWrap& rec = layer_wrap_[bg];
    if (!room_valid_ || vram_maps_.empty() || !rec.have_record) return false;
    int best = -1, best_x = 0, best_y = 0, best_total = 0;
    for (int oy = -1536; oy <= 1536; oy += 512) {
        for (int ox = -1536; ox <= 1536; ox += 512) {
            int total = 0, same = 0;
            for (int y = 4; y < 160; y += 8) {
                if (!row_jumps_vertically(scene, bg, y)) continue;
                const std::uint8_t* io = scene.row_io_valid[y]
                    ? scene.row_io[y].data() : scene.io_bytes.data();
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
    surface_->update_texture(world_map_, 0, 0, 512, 1024, tiles);
    world_tiles_.assign(tiles, tiles + 512 * 1024);
    world_map_generation_ = generation;
}

// The whole-map decode trusts the game's piece-to-tile table at 0x02010000,
// and the pause menu reuses that memory one frame before it switches the
// screen to mode 0 (logs/gpu_rewind_0058 frame 14, 2026-09-30): the pieces
// still check out, the table does not, and the margins came out as
// scrambled tiles that the menu then held. So compare the decode with what
// the console itself shows, as world_row in the shader samples it.
int FieldSceneRenderer::world_map_agreement(const FieldScene& scene,
                                            int* compared) const {
    if (compared) *compared = 0;
    if (world_tiles_.size() != 512u * 1024u ||
        vram_maps_.size() < 0x10000u) return -1;
    int total = 0, same = 0;
    for (int y = 4; y < 160; y += 8) {
        if (!scene.row_affine_valid[y]) continue;
        const std::uint8_t* io = scene.row_io_valid[y]
            ? scene.row_io[y].data() : scene.io_bytes.data();
        auto rd = [&](std::size_t at) {
            return static_cast<int>(io[at] | (io[at + 1] << 8));
        };
        if ((rd(0x00) & 7) != 2) continue;
        if (((rd(0x0E) >> 8) & 0x1F) != 8 || ((rd(0x0C) >> 8) & 0x1F) != 10)
            continue;
        for (int layer = 2; layer <= 3; ++layer) {
            const int bgcnt = rd(0x08u + static_cast<std::size_t>(layer) * 2u);
            if ((bgcnt & 0x2000) == 0) continue;   // the world rings wrap
            const int side = 16 << ((bgcnt >> 14) & 3);  // map side in tiles
            const std::size_t screen =
                static_cast<std::size_t>((bgcnt >> 8) & 31) * 2048u;
            const std::size_t param = 0x20u + (layer - 2) * 0x10u;
            const int pa = static_cast<std::int16_t>(rd(param));
            const int pc = static_cast<std::int16_t>(rd(param + 4u));
            const int ref_x = scene.row_affine[y][(layer - 2) * 2];
            const int ref_y = scene.row_affine[y][(layer - 2) * 2 + 1];
            for (int x = 4; x < 240; x += 8) {
                const int tx = (ref_x + pa * x) >> 8;
                const int ty = (ref_y + pc * x) >> 8;
                const std::size_t at = screen +
                    static_cast<std::size_t>(((ty >> 3) & (side - 1)) * side +
                                             ((tx >> 3) & (side - 1)));
                if (at >= vram_maps_.size()) continue;
                const std::size_t wt =
                    static_cast<std::size_t>(((ty >> 3) & 511) +
                                             (layer == 2 ? 512 : 0)) * 512u +
                    static_cast<std::size_t>((tx >> 3) & 511);
                ++total;
                if (world_tiles_[wt] == vram_maps_[at]) ++same;
            }
        }
    }
    if (compared) *compared = total;
    if (total < kRoomCheckMinTiles) return -1;
    return (same * 100) / total;
}

void FieldSceneRenderer::upload_room_vram(const std::uint8_t* vram,
                                          std::size_t vram_bytes) {
    if (!ready_ || !vram || vram_bytes == 0) return;
    const int rows = static_cast<int>(
        std::min<std::size_t>(vram_bytes, kVramBytes) / kVramTextureWidth);
    if (rows > 0)
        surface_->update_texture(room_vram_, 0, 0, kVramTextureWidth, rows,
                                 vram);
}

void FieldSceneRenderer::upload_palette(const std::uint16_t* palette,
                                        std::size_t entries) {
    if (!ready_ || !palette || entries == 0) return;
    const int width =
        static_cast<int>(std::min<std::size_t>(entries, kPaletteEntries));
    surface_->update_texture(palette_, 0, 0, width, 1, palette);
    surface_->update_texture(palette_, 0, 1, width, 1, palette);
}

void FieldSceneRenderer::upload_room_palette(const std::uint16_t* palette,
                                             std::size_t entries) {
    if (!ready_ || !palette || entries == 0) return;
    surface_->update_texture(
        palette_, 0, 1,
        static_cast<int>(std::min<std::size_t>(entries, kPaletteEntries)), 1,
        palette);
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
    // from the scene's own registers; nothing is decoded out of guest memory
    // any more, so there is nothing here that can be misread.
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
    const std::int64_t width64 = std::max<std::int64_t>(
        kEffectCanvasWidth, right - left);
    const std::int64_t height64 = std::max<std::int64_t>(
        kEffectCanvasHeight, bottom - top);
    const std::int64_t origin_x64 = -left;
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
    surface_->update_texture(effect_canvas_, 0, 0, width, height,
                             effect_pixels_.pixels.data());
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
//     (Ray, BG2CNT 0x6785, gpu_rewind_0046);
//   * a 256x256 8-bit regular layer with the block anywhere, tiles in order
//     and unflipped (Ivan's hit sparks, BG1CNT 0x1F81: tiles 0x100.. at map
//     column 0, row 11, gpu_rewind_0047).
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
        // stretched over the screen at PA 128: gpu_rewind_0088, 0060).
        // Meteor's is the same 128 canvas with the wrap bit set (BG2CNT
        // 0x2784, gpu_rewind_0106).
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

// The iris wipe (entering and leaving a room, gpu_rewind_0089): WIN0 is an
// ellipse the game writes row by row into WIN0H. The console clips each
// row's edges to 0..240, so past the screen the registers only say "the
// whole row", and the margins drew full rows beside the circle and the
// first row's span above it -- a cross. Rebuilt from the rows whose two
// edges are inside the screen: their half-widths fit w^2 = c0 + c1 y +
// c2 y^2 to under a pixel on every frame of the capture (an ellipse 1.22
// times as wide as tall, centred on the player closing, on the screen
// opening). Too few such rows, a centre that moves, or a worse fit: no
// iris, and the window works as before. Once the centre is known, rows with
// only one edge inside count too (gpu_rewind_0153: the player near the left
// edge leaves 4-7 two-edge rows at the circle's top, too few to place its
// bottom, and the bottom margin flickered).
struct IrisWindow {
    bool found = false;
    float cx = 0.0f, cy = 0.0f;   // console pixels
    float a2 = 0.0f, b2 = 0.0f;   // squared half-width and half-height
};

IrisWindow fit_iris_window(const FieldScene& scene) {
    IrisWindow out;
    double s[5] = {}, t[3] = {};  // sums of y^k, and of w^2 * y^k
    int rows = 0, centre2 = -1;
    std::array<std::pair<int, double>, FieldScene::kRows> seen{};
    auto row_edges = [&](int y, int* x1, int* x2) {
        const std::uint8_t* io = scene.row_io_valid[y]
            ? scene.row_io[y].data() : scene.io_bytes.data();
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
                                               const SceneLayer& layer) {
    effect_wrap_canvas_ = false;
    effect_text_canvas_ = false;
    effect_fill_ = 0;
    // Also run with no sparks: Spark Plasma's pink tint frames carry none,
    // and without a fill the tint stopped at the canvas's edge, 16 px into
    // the right margin, with the left margin untinted (gpu_rewind_0101
    // frame 30). Only the fill can be set then; the spark checks are moot.
    const bool have_sparks = effect_have_sparks_;
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
    // game copies its canvas to the screen, but Thor's canvas was emptied
    // some other way and the last copy's flash stamp (46,416 pixels, 13,654
    // of them inside the canvas, none lit there) drew a blue haze in the
    // margins only (gpu_rewind_0088 frames 5973 and 5976; on every current
    // frame all of ours inside the canvas were lit). So: nothing lit in the
    // game's canvas, nothing of the effect on screen, none of ours either;
    // and inside the canvas, where the game joins sparks by the larger
    // value, fewer than a quarter of ours lit is a stale capture too.
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
        return;
    }
    // The fill: the game's flashing tint, the value the canvas holds wherever
    // no spark landed. Read from the canvas's edge, leaving out our sparks:
    // a tint covers it, and on every other frame the edge is mostly 0. On
    // tint frames one value covers 79-97% of the edge (Thor: 16, 8, 4, 2 in
    // turn, gpu_rewind_0088 frames 6013-6037; Ray: 12, 16, 9, 3,
    // gpu_rewind_0046), the rest being sparks we did not capture. The old
    // rule wanted EVERY non-spark pixel equal and found no fill for Thor.
    std::array<int, 256> edge{};
    int counted = 0;
    auto tally = [&](int x, int y) {
        if (ours(x, y)) return;
        ++edge[static_cast<std::size_t>(canvas(x, y))];
        ++counted;
    };
    for (int x = 0; x < 128; ++x) { tally(x, 0); tally(x, 127); }
    for (int y = 1; y < 127; ++y) { tally(0, y); tally(127, y); }
    const auto top = std::max_element(edge.begin(), edge.end());
    const int fill = static_cast<int>(top - edge.begin());
    effect_fill_ = (fill > 0 && counted > 0 && *top * 4 >= counted * 3) ? fill : 0;
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
// outside a frame (spawn_burst / set_charge); draw() shakes only if this
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
    if (!ready_ || !ewram || ewram_bytes < kEwramBytes) return false;

    // Read this FIRST and keep it even when the rect below is refused: the
    // boot logo and the name-entry screen both have no room, and the margins
    // must know that regardless of what the rect happens to hold.
    world_loaded_ = ewram_u16(ewram, ewram_bytes, kMapIdAddr) != 0;

    const std::uint16_t min_x = ewram_u16(ewram, ewram_bytes, kRoomRectAddr);
    const std::uint16_t max_x = ewram_u16(ewram, ewram_bytes, kRoomRectAddr + 2);
    const std::uint16_t min_y = ewram_u16(ewram, ewram_bytes, kRoomRectAddr + 4);
    const std::uint16_t max_y = ewram_u16(ewram, ewram_bytes, kRoomRectAddr + 6);
    if (!room_rect_is_room(min_x, max_x, min_y, max_y)) return false;

    // No room loaded yet, or between rooms. Measured over all 2,495 recorded
    // snapshots plus the live name-entry dump, 2026-09-17: of the 587 frames
    // the rect above accepts, 583 carry a real map number and every one of
    // them matches the hardware, while the 4 with map number 0 include both
    // of the only two frames in the whole corpus that DIFFER from it
    // (maprec_20260905_161243 snap_00006 and snap_00008, one of them wrong
    // over 31,843 of 38,400 pixels). Gating on this fixes those and the
    // name-entry screen, and leaves all 583 correct frames untouched.
    if (ewram_u16(ewram, ewram_bytes, kMapIdAddr) == 0) return false;

    room_min_x_ = min_x; room_max_x_ = max_x;
    room_min_y_ = min_y; room_max_y_ = max_y;
    // The words at 0x02030DC0 are the camera clamp's 16.16 MAX x and y
    // (Func_10230 reads min x, min y, max x, max y at [0x03001E70] + 0xEC,
    // 0xF0, 0xF4, 0xF8, and [0x03001E70] is 0x02030CCC in every capture), so
    // the "min" halves above are fractions and always 0. The real minimums
    // are the integer halves at 0x02030DBA and 0x02030DBE. Rooms whose map
    // grid holds more than the player can ever scroll to have them above 0,
    // and the strip between is filler (logs/gpu_frame_0099: min 64,48;
    // _0100: 288,240; _0103: 16,32), which the margins drew as patterns.
    {
        const int clamp_min_x = ewram_u16(ewram, ewram_bytes, kClampMinXAddr);
        const int clamp_min_y = ewram_u16(ewram, ewram_bytes, kClampMinYAddr);
        if (clamp_min_x < room_max_x_ - 16) room_min_x_ = clamp_min_x;
        if (clamp_min_y < room_max_y_ - 16) room_min_y_ = clamp_min_y;
    }
    room_camera_x_ = static_cast<int>(ewram_u16(ewram, ewram_bytes, kCameraAddr + 2));
    room_camera_y_ = static_cast<int>(ewram_u16(ewram, ewram_bytes, kCameraAddr + 6));

    // Per-layer scroll records. Only two things are taken from them:
    //  - an auto-scrolling layer (the Suhalla sandstorm, BG1: speed -7.56,
    //    2.19, mask 31x31, region x 1024) wraps inside its own region every
    //    (mask + 1) * 8 pixels, as the game does. Its margins read straight
    //    on past the region instead, into BG2's props (cacti sliding with
    //    the storm, gpu_rewind_0097).
    //  - a layer moving at another speed than the camera (the crow's nest
    //    sky, BG3 ratio 0.375, gpu_rewind_0093) has filler above its art;
    //    Jimmy wants the art's top row carried upward instead.
    for (int k = 0; k < 3; ++k) {
        const int bg = 3 - k;
        const std::uint32_t at = kLayerRecordAddr + kLayerRecordBytes * k;
        auto word = [&](std::uint32_t off) {
            return static_cast<std::uint32_t>(ewram_u16(ewram, ewram_bytes, at + off)) |
                   (static_cast<std::uint32_t>(ewram_u16(ewram, ewram_bytes, at + off + 2)) << 16);
        };
        const bool autoscroll = word(0x18) != 0 || word(0x1C) != 0;
        const int wrap_x = (ewram_u16(ewram, ewram_bytes, at + 0x28) + 1) * 8;
        const int wrap_y = (ewram_u16(ewram, ewram_bytes, at + 0x2A) + 1) * 8;
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

    {
        // A move of more than 32 rows is a new rest position, not a drift:
        // the room's first frames can still hold the last room's records
        // (gpu_rewind_0147: region 0, then 1024 at load, then 1024 -> 1000
        // as the water sinks).
        const int map = ewram_u16(ewram, ewram_bytes, kMapIdAddr);
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
        const std::uint32_t addr = kIdGridAddr + static_cast<std::uint32_t>(i) * 4u;
        // Low 12 bits of the 32-bit grid word are the metatile id; the
        // low 16 bits already contain them whole (FACTS.md, "The upper bits
        // of the metatile word are not unused" -- those live above bit 15).
        room_ids_[i] = ewram_u16(ewram, ewram_bytes, addr) & 0x0FFFu;
    }
    surface_->update_texture(id_grid_, 0, 0, kGridSide, kGridSide,
                             room_ids_.data());

    room_atlas_.resize(static_cast<std::size_t>(kAtlasIds) * 4u);
    for (std::size_t i = 0; i < room_atlas_.size(); ++i) {
        const std::uint32_t addr = kAtlasAddr + static_cast<std::uint32_t>(i) * 2u;
        room_atlas_[i] = ewram_u16(ewram, ewram_bytes, addr);
    }
    surface_->update_texture(atlas_, 0, 0, 4, kAtlasIds, room_atlas_.data());

    room_valid_ = true;
    return true;
}

bool FieldSceneRenderer::query_room_known(int screen_x, int screen_y,
                                          int scroll_x, int scroll_y) const {
    if (!room_valid_) return false;
    const int room_x = room_camera_x_ + screen_x;
    const int room_y = room_camera_y_ + screen_y;
    if (room_x < room_min_x_ || room_y < room_min_y_ ||
        room_x >= room_max_x_ || room_y >= room_max_y_) {
        return false;
    }
    const int source_x = scroll_x + screen_x;
    const int source_y = scroll_y + screen_y;
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
    declined_ = nullptr;
    if (!ready_ || output_w_ <= 0 || output_h_ <= 0) {
        declined_ = "the renderer is not ready";
        return false;
    }
    // Modes 0-2 are tile-based (0 four regular layers, 1 makes BG2 affine, 2
    // makes BG2 and BG3 affine); modes 3-5 are bitmap modes the emulated
    // hardware itself does not render a background for either
    // (gba_ppu.cpp's render_scanline_internal only branches on mode 0/1/2),
    // and the measured 2,429-snapshot corpus never uses them (FACTS.md), so
    // there is nothing to describe yet.
    if (!scene.valid || scene.video_mode > 2) {
        declined_ = "the video mode uses a bitmap background";
        return false;
    }
    // Everything below this line is what the renderer cannot yet reproduce.
    // Each is refused rather than approximated, because a frame drawn almost
    // right is worse than a frame drawn by the old renderer.
    if (scene.forced_blank) { declined_ = "the display is blanked"; return false; }
    // Mosaic is refused only when it would actually change a pixel. MOSAIC
    // (0x4C) holds four sizes, each stored as size-1: backgrounds in bits
    // 0-3 and 4-7, objects in bits 8-11 and 12-15. A stored 0 means a block
    // one pixel wide, which is the picture the game would get anyway -- so a
    // layer or object with the enable bit set but a zero size is not an
    // effect, it is a no-op, and refusing it costs a frame for nothing.
    //
    // This is not a shortcut: measured 2026-09-16 over every recorded frame
    // we have, MOSAIC is 0x0000 in ALL 2,429 snapshots and on all 160 rows of
    // the live battle capture, while the BG enable bit is set on 92 of them
    // and an object enable bit on 2,102. So every mosaic refusal this project
    // has ever counted was for an effect that does nothing (FACTS.md).
    //
    // A NON-zero size is still refused, and deliberately: the emulated
    // hardware in this repository does not implement mosaic either (nothing
    // in gba_ppu.cpp reads 0x4C), so there is no reference to check a real
    // one against and no captured frame that asks for one. When play finally
    // produces one, "Draw only with the graphics card" makes it visible at
    // once and F12 puts it on the bench.
    //
    // Read per row, like every other register: the size can change mid-frame
    // as easily as BGxCNT did.
    bool bg_mosaic_real = false, obj_mosaic_real = false;
    for (int y = 0; y < FieldScene::kRows; ++y) {
        const std::uint8_t* io = scene.row_io_valid[y]
            ? scene.row_io[y].data() : scene.io_bytes.data();
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
    // file otherwise -- the same single value every row used before the
    // background and window shaders read per row.
    {
        std::vector<std::uint8_t> row_bytes(
            static_cast<std::size_t>(FieldScene::kLineIoBytes) *
            static_cast<std::size_t>(FieldScene::kRows));
        for (int y = 0; y < FieldScene::kRows; ++y) {
            const std::uint8_t* src = scene.row_io_valid[y]
                ? scene.row_io[y].data() : scene.io_bytes.data();
            std::memcpy(row_bytes.data() +
                            static_cast<std::size_t>(y) * FieldScene::kLineIoBytes,
                        src, FieldScene::kLineIoBytes);
        }
        // Lamakan Desert's row 0 swaps BG1 and BG3's +1024 (every frame of
        // gpu_rewind_0158-0160; over all captures only maps 89-91), so the
        // console's top line misses the haze: a thin line across the middle
        // of the widened picture. Jimmy, 2026-10-07: draw it with row 1's
        // scroll. This changes native row 0, deliberately.
        if (scene.video_mode == 0) {
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
        surface_->update_texture(row_io_, 0, 0,
                                 static_cast<int>(FieldScene::kLineIoBytes),
                                 static_cast<int>(FieldScene::kRows),
                                 row_bytes.data());
    }

    // Each row's affine reference point for BG2 and BG3 -- the console's own
    // internal accumulator, which it reloads from BG2X/BG2Y and BG3X/BG3Y at
    // VBlank and advances by PB/PD every scanline, and which the game
    // rewrites mid-frame on the overworld and in every battle. The register
    // file cannot answer for it: those registers are write-only, so an
    // end-of-frame read returns the LAST row's value, not this row's.
    //
    // A row the PPU never latched (there was no scanline there) gets the
    // frame's own reference walked out to it here, by the same PB/PD step the
    // hardware uses -- the number the shader's old frame-level walk produced,
    // so a frame that never animates its transform draws exactly as before.
    //
    // Stored biased by 2^29, in a low and a high half, because the only
    // integer texture formats here are unsigned. The bias cannot overflow:
    // a reference point is 28-bit signed and 160 scanlines of PB add at most
    // another 2^23.
    {
        constexpr int kBias = 1 << 29;
        std::vector<std::uint16_t> refs(8u * FieldScene::kRows);
        for (int y = 0; y < FieldScene::kRows; ++y) {
            for (int k = 0; k < 2; ++k) {          // k: 0 = BG2, 1 = BG3
                const SceneLayer& L = scene.layers[2 + k];
                std::int32_t rx, ry;
                if (scene.row_affine_valid[y]) {
                    rx = scene.row_affine[y][k * 2];
                    ry = scene.row_affine[y][k * 2 + 1];
                } else {
                    rx = L.affine_ref_x + y * L.affine_pb;
                    ry = L.affine_ref_y + y * L.affine_pd;
                }
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
        surface_->update_texture(row_affine_, 0, 0, 8,
                                 static_cast<int>(FieldScene::kRows),
                                 refs.data());
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
    // Which layers may reach the side margins at all. The frame-level test is
    // only ever a candidacy test -- whether the margin actually opens on a
    // given row is decided in the shader from that row's own registers.
    //
    // A battle is video mode 1 with an arena layer on, and its arena rides
    // BG1 or BG2 (src/battle_view.h). Anywhere else, a rotated or scaled
    // background is the overworld's own map: its transform is defined for
    // every column, so widening it simply evaluates the same walk further out
    // and shows more world, which is why the world map was coming out
    // pillarboxed at 240 with black beside it. The corpus says the overworld
    // is video mode 2 with only its two affine layers drawn (hardware ignores
    // BG0/BG1 in mode 2), so widening those widens the whole picture, with no
    // wrapped tilemap involved.
    const bool battle_frame = gsr::battle::is_battle_frame(
        static_cast<std::uint16_t>(scene.dispcnt));

    float transform[16];
    ortho(transform, static_cast<float>(output_w_),
          static_cast<float>(output_h_));

    // BLDCNT: which layers are 1st/2nd targets (one bit each for BG0-3, OBJ,
    // backdrop), and which effect -- 1 alpha blend, 2/3 brightness. EVA/EVB/EVY
    // saturate at 16, which is a full effect.
    const unsigned first_targets = scene.effects.blend_control & 0x3Fu;
    const unsigned second_targets = (scene.effects.blend_control >> 8) & 0x3Fu;
    const unsigned effect = (scene.effects.blend_control >> 6) & 0x3u;
    const int eva = std::min<int>(scene.effects.blend_alpha & 0x1Fu, 16);
    const int evb = std::min<int>((scene.effects.blend_alpha >> 8) & 0x1Fu, 16);
    const int bldy = std::min<int>(scene.effects.blend_brightness & 0x1Fu, 16);

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

    // The clear colour argument is never seen: the resolve pass below always
    // covers the whole output.
    surface_->begin_frame(0.0f, 0.0f, 0.0f, 1.0f);

    const float camera_x = static_cast<float>((240 - output_w_) / 2);
    const float camera_y = static_cast<float>((160 - output_h_) / 2);

    surface_->set_uniform_mat4(background_, "u_transform", transform);
    surface_->set_uniform_int(background_, "u_vram", 0);
    surface_->set_uniform_int(background_, "u_palette", 1);
    surface_->set_uniform_int(background_, "u_window", 2);
    surface_->set_uniform_int(background_, "u_id_grid", 3);
    surface_->set_uniform_int(background_, "u_atlas", 4);
    surface_->set_uniform_int(background_, "u_effect", 8);
    surface_->set_uniform_vec2(background_, "u_effect_size",
                               static_cast<float>(effect_pixels_.width),
                               static_cast<float>(effect_pixels_.height));
    surface_->set_uniform_vec2(background_, "u_effect_origin",
                               static_cast<float>(effect_pixels_.origin_x),
                               static_cast<float>(effect_pixels_.origin_y));
    surface_->set_uniform_int(background_, "u_row_io", 5);
    surface_->set_uniform_int(background_, "u_row_affine", 6);
    surface_->set_uniform_int(background_, "u_top_record", 7);
    surface_->set_uniform_int(background_, "u_room_vram", 9);
    surface_->set_uniform_int(background_, "u_world_map", 10);
    // Measured on gpu_rewind_0057/0058 and every single capture: 100% on
    // each good world-map frame, 0% on the two pause-menu frames.
    {
        const int agree = world_map_agreement(scene, nullptr);
        world_map_used_ = world_map_valid_ &&
                          (agree < 0 || agree >= kRoomCheckMinAgreement);
    }
    surface_->set_uniform_int(background_, "u_use_world",
                              world_map_used_ ? 1 : 0);
    surface_->set_uniform_int(background_, "u_palette_half", 0);
    // The native viewport starts at the console's own origin. A wider output
    // is centred on it, so the extra width appears on both sides.
    surface_->set_uniform_vec2(background_, "u_camera", camera_x, camera_y);
    surface_->set_uniform_int(background_, "u_world_loaded",
                              world_loaded_ ? 1 : 0);
    surface_->set_uniform_vec2(background_, "u_room_min",
                               static_cast<float>(room_min_x_),
                               static_cast<float>(room_min_y_));
    surface_->set_uniform_vec2(background_, "u_room_max",
                               static_cast<float>(room_max_x_),
                               static_cast<float>(room_max_y_));
    surface_->set_uniform_vec2(background_, "u_ewram_camera",
                               static_cast<float>(room_camera_x_),
                               static_cast<float>(room_camera_y_));

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

    // WIN0/WIN1/object-window setup for the window pass. The rectangles,
    // WININ/WINOUT and the window-enable bits themselves are read per row
    // from u_row_io inside the shader now (see kWindowFragment), so only the
    // object-window mask and the camera are set here.
    surface_->set_uniform_mat4(window_, "u_transform", transform);
    surface_->set_uniform_int(window_, "u_obj_window_mask", 0);
    surface_->set_uniform_int(window_, "u_row_io", 1);
    surface_->set_uniform_vec2(window_, "u_camera", camera_x, camera_y);
    surface_->set_uniform_int(window_, "u_reveal_full", reveal_full_ ? 1 : 0);
    {
        IrisWindow iris = fit_iris_window(scene);
        surface_->set_uniform_int(window_, "u_iris", iris.found ? 1 : 0);
        surface_->set_uniform_vec2(window_, "u_iris_centre", iris.cx, iris.cy);
        surface_->set_uniform_vec2(window_, "u_iris_axes", iris.a2, iris.b2);
    }

    const gbarecomp::GpuTexture textures2[4] = {
        vram_, palette_, window_control_, row_io_};
    const gbarecomp::GpuTexture textures3[5] = {
        vram_, palette_, window_control_, row_io_, top_};
    const gbarecomp::GpuTexture window_textures[2] = {obj_window_mask_, row_io_};
    // Backgrounds additionally carry the room buffer's id grid and atlas and
    // this frame's per-row registers and affine references, at the fixed
    // units u_id_grid=3/u_atlas=4/u_row_io=5/u_row_affine=6 set above;
    // u_top_record moves to unit 7 only for the background program; objects
    // use row_io at unit 3 and top at unit 4.
    // Unit 8 is our own effect canvas, always bound so the uniform can switch
    // it on per layer without reshuffling the array.
    // Unit 10 is the world map (upload_world_map), bound always for the
    // same reason.
    const gbarecomp::GpuTexture bg_textures2[11] = {
        vram_, palette_, window_control_, id_grid_, atlas_, row_io_,
        row_affine_, effect_canvas_, effect_canvas_, room_vram_, world_map_};
    const gbarecomp::GpuTexture bg_textures3[11] = {
        vram_, palette_, window_control_, id_grid_, atlas_, row_io_,
        row_affine_, top_, effect_canvas_, room_vram_, world_map_};

    // The console picks the winning pixel by a sort key: a background's key is
    // its priority times 256 plus 128 plus its layer number, an object's is its
    // priority times 256 plus its slot number, and the smaller key wins
    // (gba_ppu.cpp). The depth test reproduces that ordering directly, so
    // unlike the old painter's-order draw, these no longer need to be issued
    // back to front -- only every enabled layer and present object drawn once
    // per pass.
    // DISPCNT's layer enables, read the way the reference reads them: once
    // per row. `SceneLayer::enabled` is the frame's LAST row's answer, so a
    // layer the game switches on for part of the frame and off by VBlank
    // would never be drawn at all. The draw below is issued when any row
    // wants the layer; the shader then drops the rows that do not.
    bool objects_on = false;
    bool obj_window_on = false;
    for (int y = 0; y < FieldScene::kRows; ++y) {
        const std::uint8_t* io = scene.row_io_valid[y]
            ? scene.row_io[y].data() : scene.io_bytes.data();
        const unsigned dispcnt =
            io[0] | (static_cast<unsigned>(io[1]) << 8);
        if ((dispcnt & 0x1000u) != 0u) objects_on = true;
        if ((dispcnt & 0x8000u) != 0u) obj_window_on = true;
    }
    // Draw order within a priority: OAM order (lower slot in front), except
    // for a character's shadow that the console never shows. Every field
    // shadow is a 16x8 8-bit sprite on tile 0, centred under its character
    // (all 67 in logs/gpu_frame_0001..0109). On the console's screen the game
    // always lists the character first (58 of 58); above the screen it
    // sometimes lists the shadow first (5 of 9, gpu_frame_0108), which the
    // console hides but the margins showed as shadows over the feet. Such a
    // shadow, wholly outside the 240x160 window, draws just behind its own
    // character; nothing on the console's screen changes order.
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
            for (int b = s + 1; b < FieldScene::kObjects; ++b) {
                const SceneObject& B = scene.objects[b];
                if (!B.present || B.window || is_shadow(B) ||
                    B.priority != S.priority) continue;
                const int dx = (2 * B.x + box_w(B)) - (2 * S.x + sw);
                const int dy = S.y - B.y;
                if (dx >= -2 && dx <= 2 && dy >= 0 && dy <= box_h(B)) {
                    body_of[s] = b;
                    break;
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
    // (24 of them on the ship, gpu_rewind_0129), so in the widened view
    // they stopped short of both edges. A run of identical, abutting,
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
                // margin (y -40, gpu_frame_0123) and stopped short of the
                // sides there.
                const int view_top = -(output_h_ - 160) / 2;
                const int view_bottom = 160 + (output_h_ - 160) / 2;
                if (first.x <= 0 && last.x + last.width >= 240 &&
                    first.y < view_bottom &&
                    first.y + first.height > view_top) {
                    strip[run[i]].left = true;
                    strip[run[j]].right = true;
                    // Up and down only for a solid tile: stacking a patterned
                    // one makes stripes. The Sol Sanctum cloud row slides to
                    // y -1 and was stacked up the whole top margin
                    // (logs/gpu_rewind_0136 frame 90).
                    const bool solid = object_is_solid(
                        first, (scene.dispcnt & 0x40u) != 0u);
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
    bool layer_wanted[4] = {false, false, false, false};
    for (int bg = 0; bg < 4; ++bg) {
        // Which layers exist at all still follows the video mode: mode 0 is
        // four regular layers, mode 1 drops BG3, mode 2 keeps only BG2 and
        // BG3 (field_scene.cpp, layer_exists_in_mode). `enabled` already has
        // that test folded in, but only for the last row, so it cannot be
        // reused on its own.
        const bool exists = (scene.video_mode == 0) ||
                            (scene.video_mode == 1 && bg <= 2) ||
                            (scene.video_mode == 2 && bg >= 2);
        if (!exists) continue;
        for (int y = 0; y < FieldScene::kRows && !layer_wanted[bg]; ++y) {
            const std::uint8_t* io = scene.row_io_valid[y]
                ? scene.row_io[y].data() : scene.io_bytes.data();
            const unsigned dispcnt =
                io[0] | (static_cast<unsigned>(io[1]) << 8);
            if (((dispcnt >> (8 + bg)) & 1u) != 0u) layer_wanted[bg] = true;
        }
    }
    // A layer switched on and off line by line: Altin's water drains as
    // random scanlines of BG1 going off, fewer each frame (gpu_rewind_0145;
    // the same stripes in mGBA). Margin rows have no line of their own, and
    // the nearest line's state left the water solid above and below the
    // console; they take the enable bit of row (y mod 160) instead. A plain
    // split (a dialogue box, a cutscene bar) switches only a few times.
    bool layer_striped[4] = {false, false, false, false};
    for (int bg = 0; bg < 4; ++bg) {
        int switches = 0;
        bool prev = false;
        for (int y = 0; y < FieldScene::kRows; ++y) {
            const std::uint8_t* io = scene.row_io_valid[y]
                ? scene.row_io[y].data() : scene.io_bytes.data();
            const bool on = ((io[1] >> bg) & 1u) != 0u;
            if (y > 0 && on != prev) ++switches;
            prev = on;
        }
        layer_striped[bg] = switches >= 16;
    }
    // A frame where some field layer is read through its record (Lamakan's
    // swapped regions): the haze-above rule then covers every room layer,
    // the sandfall's own-scroll BG2 included (gpu_frame_0131/0132).
    bool haze_room = false;
    if (room_source_enabled_ && room_valid_ && scene.video_mode == 0) {
        for (int bg = 1; bg <= 3; ++bg) {
            bool rec = false;
            if (!scene.layers[bg].affine &&
                room_reference_for(scene, bg, &rec) && rec)
                haze_room = true;
        }
    }

    auto draw_background = [&](int bg, bool second_pass) {
        const SceneLayer& L = scene.layers[bg];
        if (!layer_wanted[bg]) return;
        // Only the quad's own depth, which gl_FragDepth overrides per
        // fragment now; kept so the draw still carries a sane value.
        const int key = L.priority * 256 + 128 + bg;
        surface_->set_uniform_int(background_, "u_layer", bg);
        surface_->set_uniform_int(background_, "u_layer_striped",
                                  layer_striped[bg] ? 1 : 0);
        // Rows from the layer's region now to its rest region (upload_room;
        // at most 32, a larger move resets the rest position there). Field
        // rooms only, and not a wobble under 4 rows: over the capture corpus
        // the rule then fires only on Altin's drain (map 79), not on map
        // 188's BG1 (regions 510..513) or the world map's drifting layers.
        const int rest_dy = rest_region_y_[bg] - layer_wrap_[bg].region_y;
        surface_->set_uniform_int(
            background_, "u_rest_dy",
            scene.video_mode == 0 && std::abs(rest_dy) >= 4 ? rest_dy : 0);
        surface_->set_uniform_int(background_, "u_compare_top",
                                  second_pass ? 1 : 0);
        surface_->set_uniform_int(background_, "u_affine", L.affine ? 1 : 0);
        // The sort key, the tile and map blocks, the size, the colour depth,
        // the affine wrap flag and the layer's own enable bit are NOT set
        // here either: all seven live in BGxCNT and DISPCNT, which Golden Sun
        // rewrites mid-frame, so the shader reads this row's copy of both out
        // of u_row_io (see kBackgroundFragment). Only u_affine stays a
        // frame-level value, because which layer is affine follows the video
        // mode and the scene model carries one mode per frame.
        // PA/PB/PC/PD and the reference point are NOT set here: the shader
        // reads this row's own copy of all six from u_row_io and
        // u_row_affine. A frame-level value is only ever the last row's.
        // Scroll is no longer set here: the shader reads this row's own
        // HOFS/VOFS from u_row_io (see kBackgroundFragment), keyed by
        // u_layer, which is what lets a frame that rewrites scroll mid-frame
        // draw correctly.
        // The room buffer only ever sources BG1-3 -- BG0 is the lighting
        // layer and stays on video memory (FACTS.md, "the lighting layer
        // cannot be reconstructed by the room buffer's method"). It is also
        // never asked to source an affine layer: the room's id grid and
        // atlas describe a field room's regular map, not a battle arena or
        // the overworld's affine geometry, and that combination has not
        // been measured.
        // Video mode 0 only. The room buffer's rect, camera, id grid and
        // atlas all describe a FIELD room, and the game leaves them standing
        // in EWRAM while a battle runs -- so on a battle frame the rect still
        // covers most of the screen and a regular arena layer would be
        // answered with tiles from whichever room the player walked out of.
        // Mode is the honest test: every measurement behind this path was
        // taken on mode 0 field frames (FACTS.md), battles are mode 1 and the
        // overworld mode 2.
        bool use_room =
            room_source_enabled_ && room_valid_ && scene.video_mode == 0 &&
            bg != 0 && !L.affine;
        // A layer whose room rule does not reproduce the console's own
        // tiles inside the window would fill the margins with the wrong
        // tiles (the patterned strips of logs/gpu_frame_0099/0100/0103):
        // leave its margins black instead, the honest answer.
        bool from_record = false;
        if (use_room && !room_reference_for(scene, bg, &from_record))
            use_room = false;
        const LayerWrap& wrap = layer_wrap_[bg];
        surface_->set_uniform_int(background_, "u_use_room", use_room ? 1 : 0);
        surface_->set_uniform_int(background_, "u_haze_room",
                                  haze_room ? 1 : 0);
        surface_->set_uniform_int(background_, "u_room_ref_on",
                                  use_room && from_record ? 1 : 0);
        surface_->set_uniform_vec2(background_, "u_room_ref",
                                   static_cast<float>(wrap.pos_x),
                                   static_cast<float>(wrap.pos_y));
        const bool split_v = use_room && row_split_vertical(scene, bg);
        surface_->set_uniform_int(background_, "u_split_v", split_v ? 1 : 0);
        int split_x = 0, split_y = 0;
        const bool split_found =
            split_v && split_row_offset(scene, bg, &split_x, &split_y);
        surface_->set_uniform_int(background_, "u_split_found",
                                  split_found ? 1 : 0);
        surface_->set_uniform_vec2(background_, "u_split_offset",
                                   static_cast<float>(split_x),
                                   static_cast<float>(split_y));
        surface_->set_uniform_vec4(background_, "u_room_wrap",
                                   static_cast<float>(wrap.region_x),
                                   static_cast<float>(wrap.region_y),
                                   static_cast<float>(use_room ? wrap.wrap_x : 0),
                                   static_cast<float>(use_room ? wrap.wrap_y : 0));
        // Where a parallax layer's art starts: walk up from the console's
        // top row (centre column, row 0's own scroll) through the expanded
        // top margin until the room gives the region's filler entry, the one
        // at the region's origin (the crow's nest: 0x0020 everywhere above
        // y 776, the sky from 776; gpu_rewind_0093). Holding the console's
        // own row 0 instead streaked the clouds upward (gpu_rewind_0083/86).
        int hold_floor = 0;
        bool hold_top = false;
        if (use_room && wrap.parallax && room_valid_) {
            const std::uint8_t* io = scene.row_io_valid[0]
                ? scene.row_io[0].data() : scene.io_bytes.data();
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
            if (entry_at(wrap.region_x, wrap.region_y, &fill)) {
                for (int y = sy & ~7; y >= sy - margin_top - 8; y -= 8) {
                    if (!entry_at(sx, y, &here)) break;
                    if (here == fill) {
                        hold_floor = y + 8;
                        hold_top = hold_floor > sy - margin_top;
                        break;
                    }
                }
            }
        }
        surface_->set_uniform_int(background_, "u_hold_top", hold_top ? 1 : 0);
        surface_->set_uniform_int(background_, "u_hold_floor", hold_floor);
        int margin_mode = 0;
        if (battle_frame) {
            if (bg == static_cast<int>(gsr::battle::kBackdropLayer) ||
                bg == static_cast<int>(gsr::battle::kArenaAffineLayer))
                margin_mode = 2;
        } else if (L.affine) {
            margin_mode = 1;
        }
        surface_->set_uniform_int(background_, "u_margin_mode", margin_mode);
        // The effect layer is sampled through the widened view rather than
        // the console window, so its canvas spans our screen. 240/output is
        // the factor; at the native size it is 1 and the sampling is
        // unchanged, which is what keeps the gates identical.
        // DEFAULT OFF, by the user's call on 2026-09-18: spanning the view
        // means magnifying the same artwork to cover it, and at 360x240 that
        // is 1.5x, which reads as blockier sparks. He wants the effects
        // uncut, not enlarged. The mechanism stays because the choice is a
        // taste one and reversible; `effect_span_` is what selects it.
        const bool remap = effect_layer_ == bg && effect_span_ && !L.wraps &&
                           output_w_ > 0 &&
                           output_h_ > 0 &&
                           (output_w_ != 240 || output_h_ != 160);
        // Two separate things, deliberately: our captured sparks fill where
        // the game drew nothing, and the span remap stretches its canvas over
        // the view. The first adds artwork; the second magnifies it. Only the
        // first is wanted by default.
        // Hiding the game's slash (set_effect_hidden) applies only to a canvas
        // we have located (text block or wrapping affine); it also drops our
        // own stamp overlay and fill, so nothing of the effect is left.
        const bool hide_here = effect_hidden_ && effect_layer_ == bg &&
                               effect_have_sparks_ &&
                               (effect_text_canvas_ || effect_wrap_canvas_);
        surface_->set_uniform_int(background_, "u_effect_hide",
                                  !hide_here ? 0 : (effect_text_canvas_ ? 1 : 2));
        surface_->set_uniform_int(
            background_, "u_effect_extra",
            (effect_layer_ == bg && effect_have_sparks_ && !hide_here) ? 1 : 0);
        // A wrapping 128 canvas stretched by less than 2:1 already repeats
        // inside the console's own screen: Torch's glow at PA 170 spans 158
        // canvas columns per row, so the console shows it starting over
        // (gpu_rewind_0124). There the repeat is the effect as designed, and
        // the margins continue it; swapping in our capture past the canvas
        // drew a hard curved edge on the left and nothing on the right.
        // Decided from the stretch alone, not the offset, so a shaking
        // 2:1 canvas (Meteor, PA 128: 120 columns) is never caught by it.
        bool repeats_on_screen = false;
        if (effect_layer_ == bg && L.affine && L.wraps && L.size_code == 0) {
            const int param = 0x20 + (bg - 2) * 0x10;
            for (int y = 0; y < FieldScene::kRows && !repeats_on_screen; ++y) {
                const std::uint8_t* io = scene.row_io_valid[y]
                    ? scene.row_io[y].data() : scene.io_bytes.data();
                const int pa = static_cast<std::int16_t>(
                    io[param] | (io[param + 1] << 8));
                repeats_on_screen = (239 * std::abs(pa)) >> 8 >= 136;
            }
        }
        surface_->set_uniform_int(
            background_, "u_effect_wrap_canvas",
            (effect_layer_ == bg && effect_have_sparks_ && effect_wrap_canvas_ &&
             !hide_here && !repeats_on_screen)
                ? 1 : 0);
        surface_->set_uniform_int(background_, "u_effect_fill", effect_fill_);
        // The fill reaches the margins past a non-wrapping canvas only when
        // the canvas spans the console's whole width on the first and last
        // rows; at 1:1 a 128-pixel canvas ends inside the screen, where the
        // console shows nothing past it either.
        bool fill_beyond = false;
        if (effect_layer_ == bg && !hide_here && effect_fill_ > 0 &&
            effect_wrap_canvas_ && !repeats_on_screen && L.affine &&
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
        surface_->set_uniform_int(background_, "u_effect_fill_beyond",
                                  fill_beyond ? 1 : 0);
        surface_->set_uniform_int(background_, "u_effect_repeats",
                                  repeats_on_screen && !hide_here ? 1 : 0);
        surface_->set_uniform_int(
            background_, "u_effect_text_canvas",
            (effect_layer_ == bg && effect_have_sparks_ && effect_text_canvas_ &&
             !hide_here)
                ? 1 : 0);
        surface_->set_uniform_vec2(background_, "u_effect_canvas_map",
                                   static_cast<float>(effect_canvas_map_x_),
                                   static_cast<float>(effect_canvas_map_y_));
        surface_->set_uniform_vec2(
            background_, "u_effect_span",
            remap ? 240.0f / static_cast<float>(output_w_) : 0.0f,
            remap ? 160.0f / static_cast<float>(output_h_) : 0.0f);
        gbarecomp::GpuQuad quad;
        quad.x = 0;
        quad.y = 0;
        quad.w = static_cast<float>(output_w_);
        quad.h = static_cast<float>(output_h_);
        // The texture coordinates carry output pixel positions, so the shader
        // can work in whole pixels.
        quad.u0 = 0;
        quad.v0 = 0;
        quad.u1 = static_cast<float>(output_w_);
        quad.v1 = static_cast<float>(output_h_);
        quad.depth = static_cast<float>(key) / 1024.0f;
        surface_->draw_quads(background_, &quad, 1,
                             second_pass ? bg_textures3 : bg_textures2, 11);
    };

    // `lifter` >= 0 draws the object's lifted copy: the console gives an
    // object's pixel the priority of any later, higher-priority object whose
    // box covers it, even where that object is transparent (gba_ppu.cpp's
    // emit_obj). Golden Sun lays an empty 64x64 priority-1 sprite over the
    // ship cabin's rug so whoever stands on it is drawn over the rug.
    auto draw_object = [&](int slot, bool second_pass, int lifter) {
        const SceneObject& O = scene.objects[slot];
        // A mode-2 object never draws a visible pixel; it only shapes the
        // object window, which the mask pass below already used it for.
        if (!O.present || O.window) return;
        const SceneObject* L = lifter >= 0 ? &scene.objects[lifter] : nullptr;
        // The lifted copy sorts behind every object of the lifter's priority
        // (rank 127), so where the lifter itself is opaque it still wins.
        const int key = L ? L->priority * 256 + 127
                          : O.priority * 256 + object_rank[slot];
        surface_->set_uniform_int(object_, "u_key", key);
        surface_->set_uniform_int(object_, "u_semi_transparent",
                                  (L ? L->blended : O.blended) ? 1 : 0);
        surface_->set_uniform_int(object_, "u_lift", L ? 1 : 0);
        if (L)
            surface_->set_uniform_vec4(
                object_, "u_lift_box", static_cast<float>(L->x),
                static_cast<float>(L->y), static_cast<float>(L->x + L->width),
                static_cast<float>(L->y + L->height));
        surface_->set_uniform_int(object_, "u_compare_top",
                                  second_pass ? 1 : 0);
        // Only a PARKED slot is held inside the authentic window -- the same
        // rule gba_ppu.cpp's emit_obj now uses. This used to clip every
        // object the providers could not authenticate, and since they only
        // authenticate field sprites that cut every battle monster and every
        // effect sprite off square at the console's old screen edge: the seam
        // reported on 2026-09-17. Still a fragment test rather than a
        // draw-call skip, because the quad is drawn at the object's real
        // position either way and a parked sprite could otherwise cross back
        // into the authentic window from outside it.
        // A game window (menu, shop, dialogue) is open: its own sprites are
        // priority 0 and the game hides unused ones just off the screen --
        // the shop's arrow parked at x = -32, the icon row at y = 136 whose
        // bottom 8 rows the console cuts (logs/gpu_frame_0092, _0098,
        // 2026-09-30). Keep those inside the console's window. Field
        // characters are priority 1-3, Psynergy sparks outside a window
        // keep drawing into the margins, and battles (mode 1, whose party
        // sprites hang below the screen) are left alone.
        const bool menu_sprite =
            menu_open_ && scene.video_mode == 0 && O.priority == 0;
        surface_->set_uniform_int(object_, "u_clip_native",
                                  (O.parked || menu_sprite) ? 1 : 0);
        surface_->set_uniform_vec2(object_, "u_size",
                                   static_cast<float>(O.width),
                                   static_cast<float>(O.height));
        surface_->set_uniform_int(object_, "u_tile", static_cast<int>(O.tile));
        surface_->set_uniform_int(object_, "u_bank",
                                  O.palette < 0 ? 0 : O.palette);
        surface_->set_uniform_int(object_, "u_eight_bit", O.palette < 0 ? 1 : 0);
        surface_->set_uniform_int(object_, "u_hflip", O.hflip ? 1 : 0);
        surface_->set_uniform_int(object_, "u_vflip", O.vflip ? 1 : 0);

        // A rotated object is drawn over a box, not over the sprite: the same
        // size normally, twice it when the object asks for the room to rotate
        // into. That box is what the position refers to.
        const float box_w = static_cast<float>(
            O.affine && O.double_size ? O.width * 2 : O.width);
        const float box_h = static_cast<float>(
            O.affine && O.double_size ? O.height * 2 : O.height);
        surface_->set_uniform_int(object_, "u_affine", O.affine ? 1 : 0);
        surface_->set_uniform_vec2(object_, "u_box", box_w, box_h);
        if (O.affine) {
            const int g = (O.affine_index >= 0 &&
                           O.affine_index < FieldScene::kTransforms)
                          ? O.affine_index : 0;
            const auto& t = scene.object_transforms[g];
            surface_->set_uniform_vec4(object_, "u_transform_pd",
                                       t[0], t[1], t[2], t[3]);
        }

        gbarecomp::GpuQuad quad;
        // Object positions are in the console's own 240x160 space; a wider
        // output shifts them by the same camera the backgrounds use.
        quad.x = static_cast<float>(O.x) - camera_x;
        quad.y = static_cast<float>(O.y) - camera_y;
        quad.w = box_w;
        quad.h = box_h;
        quad.u0 = 0;
        quad.v0 = 0;
        quad.u1 = box_w;
        quad.v1 = box_h;
        quad.depth = static_cast<float>(key) / 1024.0f;
        surface_->draw_quads(object_, &quad, 1,
                             second_pass ? textures3 : textures2,
                             second_pass ? 5 : 4);
        if (L) return;
        // A full-width strip carries on to the view's edges (see `strip`).
        const StripReach& reach = strip[static_cast<std::size_t>(slot)];
        if (!(reach.left || reach.right || reach.up || reach.down)) return;
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
        for (const float y : ys)
            for (const float x : xs) {
                if (x == x0 && y == y0) continue;
                quad.x = x;
                quad.y = y;
                surface_->draw_quads(object_, &quad, 1,
                                     second_pass ? textures3 : textures2,
                                     second_pass ? 5 : 4);
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
        // Same trust-gated confinement as draw_object above.
        surface_->set_uniform_int(obj_window_, "u_clip_native",
                                  O.parked ? 1 : 0);
        surface_->set_uniform_vec2(obj_window_, "u_size",
                                   static_cast<float>(O.width),
                                   static_cast<float>(O.height));
        surface_->set_uniform_int(obj_window_, "u_tile", static_cast<int>(O.tile));
        surface_->set_uniform_int(obj_window_, "u_eight_bit",
                                  O.palette < 0 ? 1 : 0);
        surface_->set_uniform_int(obj_window_, "u_hflip", O.hflip ? 1 : 0);
        surface_->set_uniform_int(obj_window_, "u_vflip", O.vflip ? 1 : 0);

        const float box_w = static_cast<float>(
            O.affine && O.double_size ? O.width * 2 : O.width);
        const float box_h = static_cast<float>(
            O.affine && O.double_size ? O.height * 2 : O.height);
        surface_->set_uniform_int(obj_window_, "u_affine", O.affine ? 1 : 0);
        surface_->set_uniform_vec2(obj_window_, "u_box", box_w, box_h);
        if (O.affine) {
            const int g = (O.affine_index >= 0 &&
                           O.affine_index < FieldScene::kTransforms)
                          ? O.affine_index : 0;
            const auto& t = scene.object_transforms[g];
            surface_->set_uniform_vec4(obj_window_, "u_transform_pd",
                                       t[0], t[1], t[2], t[3]);
        }

        gbarecomp::GpuQuad quad;
        quad.x = static_cast<float>(O.x) - camera_x;
        quad.y = static_cast<float>(O.y) - camera_y;
        quad.w = box_w;
        quad.h = box_h;
        quad.u0 = 0;
        quad.v0 = 0;
        quad.u1 = box_w;
        quad.v1 = box_h;
        const gbarecomp::GpuTexture mask_textures[2] = {vram_, row_io_};
        surface_->draw_quads(obj_window_, &quad, 1, mask_textures, 2);
    };

    auto draw_pass = [&](bool second_pass) {
        for (int bg = 0; bg < 4; ++bg) draw_background(bg, second_pass);
        if (!objects_on) return;
        for (int slot = 0; slot < FieldScene::kObjects; ++slot) {
            draw_object(slot, second_pass, -1);
            const SceneObject& O = scene.objects[slot];
            if (!O.present || O.window) continue;
            // Unrotated lifters only: a rotated one lifts only where its
            // transform lands inside the sprite, which a box cannot say.
            for (int j = slot + 1; j < FieldScene::kObjects; ++j) {
                const SceneObject& L = scene.objects[j];
                if (!L.present || L.window || L.affine ||
                    L.priority >= O.priority) continue;
                const int ow = O.affine && O.double_size ? O.width * 2 : O.width;
                const int oh = O.affine && O.double_size ? O.height * 2 : O.height;
                if (L.x >= O.x + ow || O.x >= L.x + L.width ||
                    L.y >= O.y + oh || O.y >= L.y + L.height) continue;
                draw_object(slot, second_pass, j);
            }
        }
        surface_->set_uniform_int(object_, "u_lift", 0);
    };

    // The object window mask and the window control texture are both
    // computed once, ahead of the peel passes that read layer/blend
    // visibility from them and the resolve pass that reads blend_enabled(x).
    surface_->set_depth_enabled(false);
    surface_->bind_color_target(obj_window_mask_);
    surface_->clear_color_target(0.0f, 0.0f, 0.0f, 0.0f);
    if (obj_window_on) {
        for (int slot = 0; slot < FieldScene::kObjects; ++slot)
            draw_object_window_mask(slot);
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

    // Pass 1: the frontmost candidate at every pixel.
    surface_->set_depth_enabled(true);
    surface_->bind_color_target(top_);
    surface_->clear_color_target(clear_r, clear_g, clear_b, clear_a);
    surface_->clear_depth(1.0f);
    draw_pass(false);

    // Pass 2: the runner-up. Identical geometry, but each fragment discards
    // itself unless it beats what pass 1 already recorded at its own pixel
    // (the "top" texture, sampled directly -- no depth texture is needed for
    // that comparison, only the depth test that then picks the best of what
    // survives).
    surface_->bind_color_target(second_);
    surface_->clear_color_target(clear_r, clear_g, clear_b, clear_a);
    surface_->clear_depth(1.0f);
    draw_pass(true);

    // Resolve: one full-screen quad turns the two records into the finished
    // picture, exactly as the reference does at the end of a scanline. The
    // depth test is switched off first -- this quad's own depth is the
    // default 0, which could otherwise lose to pass 2's leftover depth buffer
    // at some pixels and leave them undrawn.
    surface_->set_depth_enabled(false);
    // While the screen shakes the resolve output goes to a scratch target
    // (its shader reads pass textures at gl_FragCoord, so moving its quad
    // would not move the picture) and is drawn back offset and scaled.
    const bool shaking = shake_.active() && shake_blit_ && shake_target_ &&
                         shake_target_w_ == output_w_ &&
                         shake_target_h_ == output_h_;
    surface_->bind_color_target(shaking ? shake_target_ : 0);
    surface_->set_uniform_mat4(resolve_, "u_transform", transform);
    surface_->set_uniform_int(resolve_, "u_top", 0);
    surface_->set_uniform_int(resolve_, "u_second", 1);
    surface_->set_uniform_int(resolve_, "u_window", 2);
    // The blend registers are read per row inside the shader now (see
    // kResolveFragment), so nothing about the colour effect is set here.
    surface_->set_uniform_int(resolve_, "u_row_io", 3);
    surface_->set_uniform_vec2(resolve_, "u_camera", camera_x, camera_y);
    const gbarecomp::GpuTexture resolve_textures[4] = {
        top_, second_, window_control_, row_io_};
    gbarecomp::GpuQuad full;
    full.x = 0;
    full.y = 0;
    full.w = static_cast<float>(output_w_);
    full.h = static_cast<float>(output_h_);
    surface_->draw_quads(resolve_, &full, 1, resolve_textures, 4);

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

    // The Earth Surge burst and the jump trail, over the finished picture.
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
        burst_.step();
        trail_.step();
    }
    shake_.step();

    surface_->end_frame();
    return true;
}

}  // namespace gsr
