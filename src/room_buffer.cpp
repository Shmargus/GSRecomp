// room_buffer.cpp — see room_buffer.h.

#include "room_buffer.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gba_bus.h"
#include "gba_ppu.h"
#include "host_config_ui.h"
#include "runtime_bus_bridge.h"
#include "widescreen_policy.h"

#ifdef GBARECOMP_HAVE_IMGUI
#include "imgui.h"
#endif

namespace gsr {
namespace {

bool g_enabled = false;
bool g_rendering = false;

// Why the render path declined to answer, so a black margin can be attributed
// to a reason instead of a guess. Indices below.
enum Refusal { kNoRoom, kNotMode0, kNotFieldSig, kBadOffset, kOutsideRoom,
               kOffGrid, kUnavailableEntry, kRefusalCount };
std::uint64_t g_refuse[kRefusalCount] = {};

// ---- measured addresses and layout (FACTS.md) ---------------------------
constexpr std::uint32_t kEwramBase = 0x02000000u;
constexpr std::uint32_t kRoomRect = 0x02030DC0u;   // min_x, max_x, min_y, max_y (u16)
constexpr std::uint32_t kCamera = 0x02030DB0u;     // x, y as 16.16 fixed point
constexpr std::uint32_t kIdGrid = 0x02010000u;     // 128x128 u32, low 12 bits
constexpr std::uint32_t kAtlas = 0x02020000u;      // 8 bytes per metatile id

constexpr std::size_t kGridSide = 128;
constexpr std::size_t kCellPixels = 16;            // one grid cell is a 2x2 metatile
constexpr std::size_t kTilePixels = 8;             // one tilemap entry
constexpr std::size_t kRingCells = 16;             // 256px screenblock / 16px cell
constexpr std::size_t kScreenblockBytes = 0x800;
constexpr std::size_t kEntriesPerRow = 32;         // text screenblock is 32x32 entries

// The three field layers, in the order their evidence is recorded. BG0 is the
// lighting layer and is not sourced from the map tables, so it is not built.
constexpr unsigned kLayers[3] = {3, 2, 1};

std::uint16_t read_u16(const gba::GbaBus& bus, std::uint32_t address) {
    const std::uint8_t* ew = bus.ewram_ptr();
    const std::uint32_t off = (address - kEwramBase) & 0x3FFFFu;
    return static_cast<std::uint16_t>(ew[off] | (ew[off + 1] << 8));
}

std::uint32_t read_u32(const gba::GbaBus& bus, std::uint32_t address) {
    const std::uint8_t* ew = bus.ewram_ptr();
    const std::uint32_t off = (address - kEwramBase) & 0x3FFFFu;
    return static_cast<std::uint32_t>(ew[off]) |
           (static_cast<std::uint32_t>(ew[off + 1]) << 8) |
           (static_cast<std::uint32_t>(ew[off + 2]) << 16) |
           (static_cast<std::uint32_t>(ew[off + 3]) << 24);
}

std::uint16_t io_u16(const std::uint8_t* io, std::uint32_t offset) {
    return static_cast<std::uint16_t>(io[offset] | (io[offset + 1] << 8));
}

struct Rect {
    std::uint16_t min_x, max_x, min_y, max_y;
};

Rect read_room_rect(const gba::GbaBus& bus) {
    return {read_u16(bus, kRoomRect), read_u16(bus, kRoomRect + 2),
            read_u16(bus, kRoomRect + 4), read_u16(bus, kRoomRect + 6)};
}

// A rect is usable when it describes a real room.
//
// It used to require min_x/min_y == 0, because that held in every room
// measured. It is not a property of the format, and rooms that break it were
// rejected outright -- which blanked the whole margin and left the native
// area sitting in black (Goma Cave Entrance, session_20260905_210710: 3.7M
// samples declined for "no room"). Accept any rect with real extent, and let
// the origin be wherever it says.
//
// Still excluded: the all-zero between-rooms marker, and the (0,1,8,256)
// constant the world map parks here, both measured.
bool rect_is_room(const Rect& r) {
    if (r.max_x == 1 && r.min_y == 8 && r.max_y == 256) return false;
    if (r.max_x <= r.min_x || r.max_y <= r.min_y) return false;
    return (r.max_x - r.min_x) >= kCellPixels &&
           (r.max_y - r.min_y) >= kCellPixels;
}

// ---- observed per-layer grid offsets ------------------------------------
//
// Learned, not computed. The writers below take the grid cell as arguments;
// slot 0/1/2 selects the destination screenblock (BG3/BG2/BG1 in screen-base
// order), and the caller passes a different cell per slot. Recording the
// offset each slot is given, relative to slot 0 at the same ring position,
// gives exactly the per-layer region — with no rule to get wrong.
constexpr std::uint32_t kFieldTilemapWriters[6] = {
    0x0800FEC8u, 0x0800FF54u, 0x08010230u,
    0x08010424u, 0x08010560u, 0x08010788u,
};

// Slot 0/1/2 maps to the screenblock at 0x2800/0x3000/0x3800, i.e. BG3/BG2/BG1
// in the field's screen-base assignment (FACTS.md, "Field register signature").
// The room the last checked frame was in, so the render path knows the
// bounds without re-reading them per pixel. Cleared implicitly by the check
// refusing to run outside a Mode 0 room.
Rect g_current_room{};
bool g_have_room = false;

// Every distinct room rect the check has seen, with how many frames each was
// current. Printed at exit: the room geometry is the input everything else
// depends on, and it has never actually been looked at in the scene that
// fails.
struct SeenRect { Rect r; std::uint64_t frames; };
SeenRect g_seen[24] = {};
std::size_t g_seen_used = 0;

void note_rect(const Rect& r) {
    for (std::size_t i = 0; i < g_seen_used; ++i) {
        if (std::memcmp(&g_seen[i].r, &r, sizeof(Rect)) == 0) {
            ++g_seen[i].frames;
            return;
        }
    }
    if (g_seen_used >= 24) return;
    g_seen[g_seen_used].r = r;
    g_seen[g_seen_used].frames = 1;
    ++g_seen_used;
}

// Each layer's region offset, from its own scroll register.
//
// The registers hold camera + region offset, wider than the hardware's 9 bits;
// the part the hardware discards is the offset. Measured equivalent to reading
// the offsets out of the writers' arguments (91.5/94.3/95.3% against
// 90.7/93.9/95.3% over session_20260905_125143), and preferable for drawing:
// it is live, per layer, needs no observation to warm up, and cannot drift
// from the registers the renderer is using on this very scanline. The
// writer-argument capture in map_recorder.cpp is what established the layout
// and stays as evidence.
//
// Measured in TILES (8px), not cells (16px). Goma Cave, session
// 20260911_005648: the room is 496x464 -- a whole number of cells -- BG3 and
// BG1 matched 99.32% and 99.81%, and BG2 refused every one of the 298 checked
// frames with an offset of exactly 0,8 px. Half a cell. A whole-cell offset
// cannot express that, so the layer declined all session and the margin fell
// back to the hardware's wrapped ring. The atlas record already holds four 8px
// tiles per cell, so tile granularity costs nothing and a whole-cell offset
// still resolves to exactly the tiles it did before.
//
// Returns false if a register is mid-update and not a whole number of tiles,
// rather than drawing a half-shifted row.
bool layer_offset(const std::uint8_t* io, unsigned layer, std::int32_t cam_x,
                  std::int32_t cam_y, std::int32_t& tdx, std::int32_t& tdy) {
    const std::int32_t hofs = static_cast<std::int32_t>(io_u16(io, 0x10u + layer * 4u));
    const std::int32_t vofs = static_cast<std::int32_t>(io_u16(io, 0x12u + layer * 4u));
    const std::int32_t rx = hofs - cam_x;
    const std::int32_t ry = vofs - cam_y;
    if (rx % static_cast<std::int32_t>(kTilePixels) != 0 ||
        ry % static_cast<std::int32_t>(kTilePixels) != 0) {
        return false;
    }
    tdx = rx / static_cast<std::int32_t>(kTilePixels);
    tdy = ry / static_cast<std::int32_t>(kTilePixels);
    return true;
}

// The PPU calls the field source once per pixel, but all of those calls for a
// row share the same live bus, scene registers, camera, room bounds and layer
// scrolls. The PPU-side begin hook resets this state for every render
// invocation, including VBlank-rendered rows; it never substitutes the
// VBlank-latched IO snapshot used by the compositor.
struct PreparedLayer {
    bool scroll_valid = false;
    std::int32_t hofs = 0;
    std::int32_t vofs = 0;
};

struct PreparedTile {
    bool valid = false;
    std::int32_t source_tile_x = 0;
    std::int32_t source_tile_y = 0;
    bool resolved = false;
    Refusal refusal = kOffGrid;
    std::uint16_t entry = 0;
};

struct PreparedRow {
    bool prepared = false;
    const gba::GbaBus* bus = nullptr;
    bool scene_valid = false;
    Refusal scene_refusal = kNoRoom;
    std::uint16_t dispcnt = 0;
    Rect rect{};
    std::int32_t cam_x = 0;
    std::int32_t cam_y = 0;
    std::array<PreparedLayer, 4> layers{};
    std::array<PreparedTile, 4> tiles{};
};

PreparedRow g_prepared_row{};

// The field's screen-base assignment: BG0=4, BG3=5, BG2=6, BG1=7. Checked
// before drawing anything, because a Mode 0 frame during the world-map
// transition still carries the overworld's BGCNT -- that combination was
// garbling the screen on the way in and out.
bool is_field_signature(const std::uint8_t* io) {
    return ((io_u16(io, 0x0Eu) >> 8) & 0x1Fu) == 5u &&
           ((io_u16(io, 0x0Cu) >> 8) & 0x1Fu) == 6u &&
           ((io_u16(io, 0x0Au) >> 8) & 0x1Fu) == 7u;
}

void prepare_row_state(int screen_y, std::uint16_t dispcnt,
                       const std::uint8_t* io) {
    (void)screen_y;
    g_prepared_row = PreparedRow{};
    g_prepared_row.prepared = true;
    if (!g_rendering) return;

    if (io == nullptr) return;

    const gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus) return;

    g_prepared_row.bus = bus;
    g_prepared_row.dispcnt = dispcnt;
    if ((g_prepared_row.dispcnt & 0x7u) != 0u ||
        (g_prepared_row.dispcnt & 0x0E00u) == 0u) {
        g_prepared_row.scene_refusal = kNotMode0;
        return;
    }
    if (!is_field_signature(io)) {
        g_prepared_row.scene_refusal = kNotFieldSig;
        return;
    }

    // The room bounds and camera are in EWRAM room/world space. BG3's scroll
    // register is a layer source coordinate and can differ from that camera.
    g_prepared_row.cam_x = static_cast<std::int32_t>(read_u16(*bus, kCamera + 2));
    g_prepared_row.cam_y = static_cast<std::int32_t>(read_u16(*bus, kCamera + 6));
    g_prepared_row.rect = read_room_rect(*bus);
    if (!rect_is_room(g_prepared_row.rect)) {
        g_prepared_row.scene_refusal = kNoRoom;
        return;
    }

    // Read each layer's scroll registers from the render-row snapshot. The
    // active bus remains the source for camera, room bounds, grid and atlas.
    for (unsigned layer : kLayers) {
        PreparedLayer& prepared = g_prepared_row.layers[layer];
        prepared.scroll_valid = true;
        prepared.hofs = static_cast<std::int32_t>(
            io_u16(io, 0x10u + layer * 4u));
        prepared.vofs = static_cast<std::int32_t>(
            io_u16(io, 0x12u + layer * 4u));
    }
    g_prepared_row.scene_valid = true;
}

bool entry_for_prepared_row(unsigned layer, std::int32_t world_px,
                            std::int32_t world_py, std::int32_t screen_x,
                            std::int32_t screen_y, std::uint16_t& out) {
    if (!g_prepared_row.scene_valid) {
        ++g_refuse[g_prepared_row.scene_refusal];
        return false;
    }
    if (layer >= g_prepared_row.layers.size() ||
        !g_prepared_row.layers[layer].scroll_valid) {
        ++g_refuse[kBadOffset];
        return false;
    }

    // Bounds are in the actual room/world coordinate space, which is
    // camera + screen. The layer's own scroll is a separate source-space
    // coordinate and must not move the room edge.
    const Rect& rect = g_prepared_row.rect;
    if (world_px < static_cast<std::int32_t>(rect.min_x) ||
        world_py < static_cast<std::int32_t>(rect.min_y) ||
        world_px >= static_cast<std::int32_t>(rect.max_x) ||
        world_py >= static_cast<std::int32_t>(rect.max_y)) {
        ++g_refuse[kOutsideRoom];
        return false;
    }

    // Resolve the source tile from this layer's own unmasked HOFS/VOFS and
    // the pixel coordinate before dividing by 8. This preserves each layer's
    // sub-tile phase and does not reject a non-8-aligned layer delta.
    const PreparedLayer& prepared = g_prepared_row.layers[layer];
    const std::int32_t source_px = prepared.hofs + screen_x;
    const std::int32_t source_py = prepared.vofs + screen_y;
    if (source_px < 0 || source_py < 0) {
        ++g_refuse[kOffGrid];
        return false;
    }
    const std::int32_t source_tile_x = source_px >> 3;
    const std::int32_t source_tile_y = source_py >> 3;

    PreparedTile& cache = g_prepared_row.tiles[layer];
    if (cache.valid && cache.source_tile_x == source_tile_x &&
        cache.source_tile_y == source_tile_y) {
        if (cache.resolved) {
            out = cache.entry;
            return true;
        }
        ++g_refuse[cache.refusal];
        return false;
    }

    cache = PreparedTile{};
    cache.valid = true;
    cache.source_tile_x = source_tile_x;
    cache.source_tile_y = source_tile_y;
    const std::int32_t sx = source_tile_x >> 1;
    const std::int32_t sy = source_tile_y >> 1;
    if (sx >= static_cast<std::int32_t>(kGridSide) ||
        sy >= static_cast<std::int32_t>(kGridSide)) {
        cache.refusal = kOffGrid;
        ++g_refuse[cache.refusal];
        return false;
    }

    const std::uint32_t word = read_u32(
        *g_prepared_row.bus,
        kIdGrid + (static_cast<std::uint32_t>(sy) * kGridSide +
                   static_cast<std::uint32_t>(sx)) * 4u);
    const std::uint32_t id = word & 0xFFFu;
    const std::uint32_t sub = static_cast<std::uint32_t>(
        (source_tile_y & 1) * 2 + (source_tile_x & 1));
    cache.entry = read_u16(*g_prepared_row.bus,
                           kAtlas + id * 8u + sub * 2u);
    // The room source runs before the older widescreen provider.  Letting an
    // unavailable atlas entry through here bypasses that provider's sentinel
    // guard and turns raw 0xffff into tile 0x3ff/palette 0xf: the yellow/red
    // checker strip in the Bilibin Barricade capture (2026-09-13).  Apply the
    // same measured fill rules as the presentation lookup so the PPU can fail
    // closed and leave the margin transparent.
    //
    // Metatile 0x01A is NOT rejected here, unlike in the older per-pixel
    // provider. That rule rejects the id on its own -- its paired raw value
    // was never measured -- and it was written for margin content invented
    // from residue OUTSIDE the room, which this source never reads. Measured
    // 2026-09-13 (maprec_20260913_161626, snap_00052, room 400x400): cell
    // (23,1) holds id 0x01A with atlas 0341/13AA/13AA/13AB, ordinary authored
    // tiles, and the blanket rule punched it out as a 16x16 black square in
    // the margin. The raw 0xF200 fill and the 0xffff raw sentinel are both
    // value-based and stay. 0xF200 is the un-authored fill; seen at id 0x017
    // (earlier) and id 0x000 (Bilibin, gpu_frame_0084, 2026-09-29), so it is
    // rejected for any metatile id.
    const bool unavailable =
        cache.entry == widescreen::kGoldenSunFieldUnavailableTile ||
        cache.entry == widescreen::kGoldenSunFieldNoMapTile;
    if (unavailable) {
        cache.refusal = kUnavailableEntry;
        ++g_refuse[cache.refusal];
        return false;
    }
    cache.resolved = true;
    out = cache.entry;
    return true;
}

// ---- why a room fails ---------------------------------------------------
//
// layer_offset() refuses whenever a layer does not sit a whole number of 16px
// cells from the camera, and the refusal alone cannot say whether the layer is
// half a cell out (fixable: the atlas record already holds four 8px tiles) or
// scrolling at its own rate (not fixable by an offset at all). Goma Cave is
// the measured failure (FACTS.md, 2026-09-10: BG2 never resolved in 951
// frames), so record the remainder itself.
//
// Both tables are bounded, hold one row per distinct tuple, and are filled
// once per frame per layer from the self-check -- never from the per-pixel
// render path.
struct OffsetRemainder {
    unsigned layer;
    std::int32_t rx_mod, ry_mod;
    std::int32_t sample_rx, sample_ry;
    std::uint64_t frames;
};
OffsetRemainder g_remainder[24] = {};
std::size_t g_remainder_used = 0;

void note_bad_offset(unsigned layer, std::int32_t rx, std::int32_t ry) {
    const std::int32_t cell = static_cast<std::int32_t>(kCellPixels);
    const std::int32_t rx_mod = ((rx % cell) + cell) % cell;
    const std::int32_t ry_mod = ((ry % cell) + cell) % cell;
    for (std::size_t i = 0; i < g_remainder_used; ++i) {
        if (g_remainder[i].layer == layer && g_remainder[i].rx_mod == rx_mod &&
            g_remainder[i].ry_mod == ry_mod) {
            ++g_remainder[i].frames;
            return;
        }
    }
    if (g_remainder_used >= 24) return;
    g_remainder[g_remainder_used] = {layer, rx_mod, ry_mod, rx, ry, 1};
    ++g_remainder_used;
}

// The draw path takes BG3's scroll register AS the camera (see
// room_buffer_supply). That is measured true in the rooms that work; if it is
// false in Goma the room test is being applied in the wrong space, which is
// what "outside-room" would then be counting. One row per distinct delta.
struct CameraDelta {
    std::int32_t dx, dy;
    std::uint64_t frames;
};
CameraDelta g_camera_delta[16] = {};
std::size_t g_camera_delta_used = 0;

void note_camera_delta(std::int32_t dx, std::int32_t dy) {
    for (std::size_t i = 0; i < g_camera_delta_used; ++i) {
        if (g_camera_delta[i].dx == dx && g_camera_delta[i].dy == dy) {
            ++g_camera_delta[i].frames;
            return;
        }
    }
    if (g_camera_delta_used >= 16) return;
    g_camera_delta[g_camera_delta_used] = {dx, dy, 1};
    ++g_camera_delta_used;
}

// ---- the buffer ---------------------------------------------------------
//
// There is deliberately no copy of the room here. The id grid in EWRAM is
// already a flat 128x128 array covering the whole room, and the atlas beside
// it is already a flat table; once the per-layer region offset is known, any
// cell in the room is two array reads. Copying them into a host buffer would
// add nothing except a second thing to keep in step with event edits — a
// pillar moved by Move patches a handful of grid cells, and reading the grid
// live picks that up for free.
//
// So "the room buffer" is: the room rect, the three region offsets, and this
// resolver. That is what makes a pixel beyond the old screen edge an ordinary
// lookup rather than a question the game cannot answer.
//
// tile_x/tile_y are world tile (8 px) coordinates with the room's top-left at
// 0,0. Returns false when the answer is not known — no room loaded, offsets
// not yet observed, or outside the room — and the caller must then fall back
// to whatever it would have done anyway. Never guesses.
bool entry_for_tile(const gba::GbaBus& bus, const std::uint8_t* io,
                    unsigned layer, std::int32_t tile_x, std::int32_t tile_y,
                    std::int32_t cam_x, std::int32_t cam_y, const Rect& rect,
                    std::uint16_t& out) {
    std::int32_t tdx = 0, tdy = 0;
    if (!layer_offset(io, layer, cam_x, cam_y, tdx, tdy)) {
        ++g_refuse[kBadOffset];
        return false;
    }

    // The room test is in room space: two tiles per cell in each axis.
    const std::int32_t gx = tile_x >> 1;
    const std::int32_t gy = tile_y >> 1;
    const std::int32_t lo_gx = rect.min_x / static_cast<int>(kCellPixels);
    const std::int32_t lo_gy = rect.min_y / static_cast<int>(kCellPixels);
    if (gx < lo_gx || gy < lo_gy ||
        gx >= rect.max_x / static_cast<int>(kCellPixels) ||
        gy >= rect.max_y / static_cast<int>(kCellPixels)) {
        ++g_refuse[kOutsideRoom];
        return false;
    }
    // Into this layer's own space, in tiles, and only then back to a grid cell
    // plus which of the four entries in that cell's atlas record. A whole-cell
    // offset gives exactly the old answer; a half-cell offset takes the two
    // tile rows of a room cell from two different grid cells, which is what
    // Goma Cave's BG2 needs.
    const std::int32_t lx = tile_x + tdx;
    const std::int32_t ly = tile_y + tdy;
    const std::int32_t sx = lx >> 1;
    const std::int32_t sy = ly >> 1;
    if (lx < 0 || ly < 0 || sx >= static_cast<std::int32_t>(kGridSide) ||
        sy >= static_cast<std::int32_t>(kGridSide)) {
        ++g_refuse[kOffGrid];
        return false;
    }
    const std::uint32_t word =
        read_u32(bus, kIdGrid + (static_cast<std::uint32_t>(sy) * kGridSide +
                                 static_cast<std::uint32_t>(sx)) * 4u);
    const std::uint32_t id = word & 0xFFFu;
    const std::uint32_t sub = static_cast<std::uint32_t>((ly & 1) * 2 +
                                                         (lx & 1));
    out = read_u16(bus, kAtlas + id * 8u + sub * 2u);
    return true;
}

// ---- the check ----------------------------------------------------------
//
// Per layer, walk the 16x16 cells the screenblock currently holds, work out
// which grid cell each one is showing, build the four tile entries that cell
// implies, and compare against VRAM. Cells whose grid position falls outside
// the room are skipped, not counted: the ring is one to two cells larger than
// the viewport, so its edge legitimately holds whatever was there before.
struct Counters {
    std::uint64_t checked = 0;
    std::uint64_t matched = 0;
    std::uint64_t frames = 0;
    std::uint64_t skipped_frames = 0;
    std::uint32_t worst_frame_misses = 0;
    // Where the misses are. If they sit on the edge of the resident window
    // they are the game's streaming lag -- it writes each new strip as it
    // scrolls in, so our answer is briefly ahead of its VRAM. If they are in
    // the interior, something is actually wrong. Guessing between those two
    // from a description of shimmer is what this replaces.
    std::uint64_t miss_edge = 0;
    std::uint64_t miss_interior = 0;
};
Counters g_counts[4];  // indexed by layer number, 1..3

void check_layer(const gba::GbaBus& bus, const std::uint8_t* io, unsigned layer,
                 const Rect& rect, std::uint32_t cam_x, std::uint32_t cam_y) {
    const std::uint16_t cnt = io_u16(io, 0x08u + layer * 2u);
    const std::uint32_t screen_base =
        static_cast<std::uint32_t>((cnt >> 8) & 0x1Fu) * kScreenblockBytes;

    // Where this layer reads in the grid, taken from what the writers were
    // actually told. The earlier version derived this from the scroll
    // registers; that matched only in 512x512 rooms, because only there do
    // the offsets happen to equal the room's own dimensions.
    const std::int32_t cam_cx = static_cast<std::int32_t>(io_u16(io, 0x1Cu));
    const std::int32_t cam_cy = static_cast<std::int32_t>(io_u16(io, 0x1Eu));
    note_camera_delta(cam_cx - static_cast<std::int32_t>(cam_x),
                      cam_cy - static_cast<std::int32_t>(cam_y));
    std::int32_t tile_dx = 0, tile_dy = 0;
    if (!layer_offset(io, layer, cam_cx, cam_cy, tile_dx, tile_dy)) {
        ++g_counts[layer].skipped_frames;
        note_bad_offset(
            layer,
            static_cast<std::int32_t>(io_u16(io, 0x10u + layer * 4u)) - cam_cx,
            static_cast<std::int32_t>(io_u16(io, 0x12u + layer * 4u)) - cam_cy);
        return;
    }

    // Which 16 cells the ring is holding. The ring is exactly 16 cells and a
    // cell shows at ring position (cell mod 16), so the resident set is the
    // 16 consecutive cells starting at the camera -- NOT the 16 aligned to a
    // multiple of 16. The first version of this check assumed the aligned
    // window; it agreed with the snapshots only because the offline analysis
    // had quietly skipped every frame where the two differ, and in play it
    // cost roughly 24 points of match rate (46.8% -> 71.0% on the town
    // captures when re-run offline both ways, 2026-09-05).
    const std::int32_t origin_x = static_cast<std::int32_t>(cam_x / kCellPixels);
    const std::int32_t origin_y = static_cast<std::int32_t>(cam_y / kCellPixels);

    const std::int32_t room_cells_x = rect.max_x / kCellPixels;
    const std::int32_t room_cells_y = rect.max_y / kCellPixels;
    const std::uint8_t* vram = bus.vram_ptr();
    std::uint32_t misses = 0;

    for (std::int32_t cy = 0; cy < static_cast<std::int32_t>(kRingCells); ++cy) {
        for (std::int32_t cx = 0; cx < static_cast<std::int32_t>(kRingCells); ++cx) {
            const std::int32_t gx = origin_x + cx;
            const std::int32_t gy = origin_y + cy;
            if (gx < 0 || gy < 0 || gx >= room_cells_x || gy >= room_cells_y) {
                continue;  // outside the room: ring edge, legitimately stale
            }
            // This layer's own top-left tile for that room cell. A whole-cell
            // offset keeps all four tiles inside one grid cell; a half-cell
            // offset straddles two, which is the whole reason the offset is
            // carried in tiles.
            const std::int32_t lx0 = gx * 2 + tile_dx;
            const std::int32_t ly0 = gy * 2 + tile_dy;
            if (lx0 < 0 || ly0 < 0 ||
                lx0 + 1 >= static_cast<std::int32_t>(kGridSide) * 2 ||
                ly0 + 1 >= static_cast<std::int32_t>(kGridSide) * 2) {
                continue;  // this layer's region leaves the grid
            }
            // Ring position wraps every 32 tile entries per axis, and it comes
            // from THIS layer's own tile, not the ground layer's. The writer places
            // the cell it was given at (its col mod 16, its row mod 16), so a
            // layer whose region offset is not a multiple of 16 sits at a
            // different ring position from the ground layer. Offsets of 32
            // hide this (32 mod 16 == 0); offsets of 60 do not (60 mod 16 ==
            // 12), which is why the match rate depended on which room and
            // where the player stood. Using the layer's own cell lifts BG2
            // from 66.9% to 93.9% and BG1 from 62.0% to 95.3% when re-run
            // offline over session_20260905_125143.
            bool cell_ok = true;
            for (unsigned row = 0; row < 2u; ++row) {
                for (unsigned col = 0; col < 2u; ++col) {
                    // Deliberately routed through entry_for_tile, the same
                    // function the renderer will call, so this measures the
                    // thing that will actually draw rather than a parallel
                    // copy of the same arithmetic.
                    std::uint16_t expected = 0;
                    if (!entry_for_tile(bus, io, layer,
                                        gx * 2 + static_cast<int>(col),
                                        gy * 2 + static_cast<int>(row),
                                        cam_cx, cam_cy, rect, expected)) {
                        cell_ok = false;
                        break;
                    }
                    const std::uint32_t ring_tx =
                        static_cast<std::uint32_t>(lx0 + static_cast<int>(col)) %
                        kEntriesPerRow;
                    const std::uint32_t ring_ty =
                        static_cast<std::uint32_t>(ly0 + static_cast<int>(row)) %
                        kEntriesPerRow;
                    const std::uint32_t off =
                        screen_base + (ring_ty * kEntriesPerRow + ring_tx) * 2u;
                    if (off + 1 >= 0x18000u) { cell_ok = false; break; }
                    const std::uint16_t actual =
                        static_cast<std::uint16_t>(vram[off] | (vram[off + 1] << 8));
                    if (actual != expected) { cell_ok = false; break; }
                }
                if (!cell_ok) break;
            }
            ++g_counts[layer].checked;
            if (cell_ok) {
                ++g_counts[layer].matched;
            } else {
                ++misses;
                const bool edge = (cx == 0 || cy == 0 ||
                                   cx == static_cast<std::int32_t>(kRingCells) - 1 ||
                                   cy == static_cast<std::int32_t>(kRingCells) - 1);
                if (edge) ++g_counts[layer].miss_edge;
                else ++g_counts[layer].miss_interior;
            }
        }
    }
    ++g_counts[layer].frames;
    if (misses > g_counts[layer].worst_frame_misses) {
        g_counts[layer].worst_frame_misses = misses;
    }
}

// ---- readout ------------------------------------------------------------
void (*g_prev_extra_draw)() = nullptr;
bool (*g_prev_extra_wants_keyboard)() = nullptr;
bool g_window_focused = false;

#ifdef GBARECOMP_HAVE_IMGUI
void draw_window() {
    if (!g_enabled) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x + 24.0f,
                                    vp->WorkPos.y + 540.0f),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(380.0f, 160.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Room Buffer Check", nullptr, ImGuiWindowFlags_NoDocking)) {
        g_window_focused = false;
        ImGui::End();
        return;
    }
    g_window_focused = ImGui::IsWindowFocused();
    ImGui::TextUnformatted("Rebuilt from EWRAM vs live VRAM:");
    for (std::size_t i = 0; i < g_seen_used; ++i) {
        std::fprintf(stderr,
                     "[room-buffer] room rect seen: min %u,%u max %u,%u "
                     "(%llu frames)\n",
                     g_seen[i].r.min_x, g_seen[i].r.min_y, g_seen[i].r.max_x,
                     g_seen[i].r.max_y,
                     static_cast<unsigned long long>(g_seen[i].frames));
    }
    for (unsigned layer : kLayers) {
        const Counters& c = g_counts[layer];
        if (c.checked == 0) {
            ImGui::Text("BG%u: no cells checked yet", layer);
            continue;
        }
        const double pct = 100.0 * static_cast<double>(c.matched) /
                           static_cast<double>(c.checked);
        ImGui::Text("BG%u: %.3f%%  %llu/%llu  worst frame %u misses", layer, pct,
                    static_cast<unsigned long long>(c.matched),
                    static_cast<unsigned long long>(c.checked),
                    c.worst_frame_misses);
    }
    ImGui::End();
}

bool wants_keyboard() {
    if (!g_enabled) return false;
    return g_window_focused && ImGui::GetIO().WantCaptureKeyboard;
}
#else
void draw_window() {}
bool wants_keyboard() { return false; }
#endif

void combined_extra_draw() {
    if (g_prev_extra_draw) g_prev_extra_draw();
    draw_window();
}

bool combined_extra_wants_keyboard() {
    if (g_prev_extra_wants_keyboard && g_prev_extra_wants_keyboard()) return true;
    return wants_keyboard();
}

extern "C" unsigned long long g_ws_expanded_diag[4];

void report_at_exit() {
    // Report whenever either half is armed: drawing from the buffer without
    // the self-check is a normal way to play, and the counters are how a
    // black margin gets explained.
    if (!g_enabled && !g_rendering) return;
    std::fprintf(stderr,
                 // g_ws_expanded_diag[2] counts objects with an authenticated
                 // position that fell outside the EXPANDED bounds, which is not
                 // the same thing as a parked sprite -- an untrusted sprite is
                 // confined to the native rectangle inside emit_obj and never
                 // reaches this counter. The old "skipped as parked" wording
                 // made a reading of 0 look like the parked-sprite guard was
                 // dead when it simply counts something else.
                 "[room-buffer] expanded view: %llu wide scanlines, %llu "
                 "entries supplied, %llu placed objects culled outside the "
                 "expanded bounds, %llu margin samples blanked\n",
                 g_ws_expanded_diag[0], g_ws_expanded_diag[1],
                 g_ws_expanded_diag[2], g_ws_expanded_diag[3]);
    std::fprintf(stderr,
                 "[room-buffer] declined: no-room %llu, not-mode0 %llu, "
                 "not-field-signature %llu, bad-offset %llu, outside-room "
                 "%llu, off-grid %llu, unavailable-entry %llu\n",
                 static_cast<unsigned long long>(g_refuse[kNoRoom]),
                 static_cast<unsigned long long>(g_refuse[kNotMode0]),
                 static_cast<unsigned long long>(g_refuse[kNotFieldSig]),
                 static_cast<unsigned long long>(g_refuse[kBadOffset]),
                 static_cast<unsigned long long>(g_refuse[kOutsideRoom]),
                 static_cast<unsigned long long>(g_refuse[kOffGrid]),
                 static_cast<unsigned long long>(g_refuse[kUnavailableEntry]));
    // The rects are the input everything else depends on; the comment on
    // note_rect has always claimed these were printed, and they were not.
    for (std::size_t i = 0; i < g_seen_used; ++i) {
        const Rect& r = g_seen[i].r;
        std::fprintf(stderr,
                     "[room-buffer] room rect x %u..%u (%u px, %s), "
                     "y %u..%u (%u px, %s), %llu frames\n",
                     r.min_x, r.max_x,
                     static_cast<unsigned>(r.max_x - r.min_x),
                     ((r.max_x - r.min_x) % kCellPixels) ? "NOT a whole cell"
                                                         : "whole cells",
                     r.min_y, r.max_y,
                     static_cast<unsigned>(r.max_y - r.min_y),
                     ((r.max_y - r.min_y) % kCellPixels) ? "NOT a whole cell"
                                                         : "whole cells",
                     static_cast<unsigned long long>(g_seen[i].frames));
    }
    for (std::size_t i = 0; i < g_camera_delta_used; ++i) {
        std::fprintf(stderr,
                     "[room-buffer] BG3 scroll minus camera: %d,%d "
                     "(%llu frames)%s\n",
                     g_camera_delta[i].dx, g_camera_delta[i].dy,
                     static_cast<unsigned long long>(g_camera_delta[i].frames),
                     (g_camera_delta[i].dx == 0 && g_camera_delta[i].dy == 0)
                         ? "" : "  <-- BG3 is NOT the camera here");
    }
    for (std::size_t i = 0; i < g_remainder_used; ++i) {
        const OffsetRemainder& o = g_remainder[i];
        std::fprintf(stderr,
                     "[room-buffer] BG%u refused: off by %d,%d px within the "
                     "16px cell (raw %d,%d), %llu frames%s\n",
                     o.layer, o.rx_mod, o.ry_mod, o.sample_rx, o.sample_ry,
                     static_cast<unsigned long long>(o.frames),
                     ((o.rx_mod % static_cast<std::int32_t>(kTilePixels)) == 0 &&
                      (o.ry_mod % static_cast<std::int32_t>(kTilePixels)) == 0)
                         ? "  <-- whole tiles; should no longer refuse"
                         : "  <-- not a tile boundary, needs its own answer");
    }
    for (unsigned layer : kLayers) {
        const Counters& c = g_counts[layer];
        if (c.checked == 0) {
            std::fprintf(stderr, "[room-buffer] BG%u: no cells checked\n", layer);
            continue;
        }
        std::fprintf(stderr,
                     "[room-buffer] BG%u: %llu/%llu cells matched (%.4f%%), "
                     "%llu frames, %llu frames skipped mid-scroll, "
                     "worst frame %u misses, misses edge/interior %llu/%llu\n",
                     layer, static_cast<unsigned long long>(c.matched),
                     static_cast<unsigned long long>(c.checked),
                     100.0 * static_cast<double>(c.matched) /
                         static_cast<double>(c.checked),
                     static_cast<unsigned long long>(c.frames),
                     static_cast<unsigned long long>(c.skipped_frames),
                     c.worst_frame_misses,
                     static_cast<unsigned long long>(c.miss_edge),
                     static_cast<unsigned long long>(c.miss_interior));
    }
}

}  // namespace

bool room_buffer_rendering() { return g_rendering; }

void room_buffer_init() {
    const char* e = std::getenv("GSR_ROOM_BUFFER");
    g_enabled = e != nullptr && e[0] != '\0' && e[0] != '0';
    // Rendering from the buffer is a second, separate opt-in. The check runs
    // without it, so the buffer can be measured against the hardware path in
    // the same session that decides whether to trust it -- and drawing runs
    // without the check, which is how it is normally played.
    const char* r = std::getenv("GSR_ROOM_BUFFER_RENDER");
    if (r != nullptr && r[0] != '\0' && r[0] != '0') {
        gba::g_ws_field_tilemap_source = &gsr_field_tilemap_source;
        gba::g_ws_field_tilemap_source_begin =
            &gsr_field_tilemap_source_begin;
        g_rendering = true;
    }
    // Both halves are off: nothing to install and nothing to report.
    if (!g_enabled && !g_rendering) return;
    g_prev_extra_draw = gbarecomp::g_config_ui_extra_draw;
    g_prev_extra_wants_keyboard = gbarecomp::g_config_ui_extra_wants_keyboard;
    gbarecomp::g_config_ui_extra_draw = &combined_extra_draw;
    gbarecomp::g_config_ui_extra_wants_keyboard = &combined_extra_wants_keyboard;
    std::atexit(report_at_exit);
}

// Called by the PPU once before each field-source render invocation/row. The
// row's DISPCNT and IO are supplied by the PPU; only camera, room bounds, grid
// and atlas data come from the active bus. A missing row snapshot refuses the
// source rather than silently substituting live IO.
void room_buffer_prepare_row(int screen_y, std::uint16_t dispcnt,
                             const std::uint8_t* io) {
    prepare_row_state(screen_y, dispcnt, io);
}

// The render path. Screen coordinates in room space via the prepared camera;
// answers only for the three field layers, only in a Mode 0 room, and only
// once the row's live scene state has been prepared. Anything else returns
// false and the PPU's own fetch stands -- so the worst case is today's picture.
bool room_buffer_supply(int bg, int screen_x, int screen_y,
                        std::uint16_t* out_entry) {
    if (!g_rendering || out_entry == nullptr) return false;
    if (bg < 1 || bg > 3) return false;
    if (!g_prepared_row.prepared)
        prepare_row_state(screen_y, 0, nullptr);
    if (!g_prepared_row.scene_valid) {
        if (g_prepared_row.bus != nullptr)
            ++g_refuse[g_prepared_row.scene_refusal];
        return false;
    }

    // Room-space pixels use the measured EWRAM camera, captured once at the
    // row boundary. The source resolver separately applies each layer's own
    // unmasked scroll, so a layer can have a different phase or region.
    const std::int32_t world_px = g_prepared_row.cam_x + screen_x;
    const std::int32_t world_py = g_prepared_row.cam_y + screen_y;
    return entry_for_prepared_row(static_cast<unsigned>(bg), world_px,
                                  world_py, screen_x, screen_y, *out_entry);
}

void room_buffer_on_function_args(std::uint32_t entry_pc, std::uint32_t r0,
                                  std::uint32_t r1, std::uint32_t r2) {
    if (!g_enabled) return;  // OFF: one predictable branch, nothing else.
    for (std::uint32_t pc : kFieldTilemapWriters) {
        if (pc != entry_pc) continue;
        return;
    }
}

void room_buffer_on_entry(std::uint32_t entry_pc) {
    (void)entry_pc;
    if (!g_enabled) return;  // OFF: one predictable branch, nothing else.

    static std::uint64_t s_last_frame = ~std::uint64_t{0};
    const std::uint64_t frame = runtime_current_frame();
    if (frame == s_last_frame) return;
    s_last_frame = frame;

    const gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus) return;
    const std::uint8_t* io = bus->io().raw();
    if (!io) return;

    // Mode 0 field only. The world map works the same way but streams as the
    // camera moves, so its ring holds rows built from earlier table contents;
    // checking it needs a different comparison and is deliberately not done
    // here (ROADMAP.md milestone 2, scope decided 2026-09-05).
    const std::uint16_t dispcnt = io_u16(io, 0x00u);
    if ((dispcnt & 0x7u) != 0u || (dispcnt & 0x0E00u) == 0u) return;

    const Rect rect = read_room_rect(*bus);
    if (!rect_is_room(rect)) return;

    // Counters are per room, not per session. A running total across rooms
    // only drifts toward whatever the current room scores, which reads as the
    // number "winding down" while standing still and hides which room is
    // actually failing.
    g_current_room = rect;
    g_have_room = true;
    note_rect(rect);
    static Rect s_room{};
    if (std::memcmp(&s_room, &rect, sizeof(Rect)) != 0) {
        s_room = rect;
        for (Counters& c : g_counts) c = Counters{};
        // The offsets are per room, so last room's are worse than none.
    }

    // Camera is 16.16; the integer part is the pixel position.
    const std::uint32_t cam_x = read_u16(*bus, kCamera + 2);
    const std::uint32_t cam_y = read_u16(*bus, kCamera + 6);

    for (unsigned layer : kLayers) {
        if ((dispcnt & (0x0100u << layer)) == 0u) continue;
        check_layer(*bus, io, layer, rect, cam_x, cam_y);
    }
}

}  // namespace gsr

extern "C" int gsr_field_tilemap_source(int bg, int screen_x, int screen_y,
                                        std::uint16_t* out_entry) {
    if (!gsr::room_buffer_supply(bg, screen_x, screen_y, out_entry)) return 0;
    return 1;
}

extern "C" void gsr_field_tilemap_source_begin(int screen_y,
                                                std::uint16_t dispcnt,
                                                const std::uint8_t* io) {
    gsr::room_buffer_prepare_row(screen_y, dispcnt, io);
}
