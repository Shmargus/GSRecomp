// Golden Sun: The Lost Age -- draw the field with the graphics card.
//
// The launcher's two boxes, as in Golden Sun Recompiled:
//   GSR_GPU_FIELD       draw each frame with the graphics card; a frame it
//                       cannot draw comes from the console compositor.
//   GSR_GPU_FIELD_ONLY  the console compositor stops drawing (it still
//                       records each row's registers, which the capture
//                       reads) and a refused frame is painted flat magenta,
//                       so what is still missing shows instead of hiding.
//
// The renderer is Golden Sun 1's own (src/field_scene.cpp describes the
// frame, src/field_scene_renderer.cpp draws it through gbarecomp's
// gpu_surface.cpp). At the console's 240x160 it draws from video memory
// alone. In the expanded view (F1 Screen size, 360x240) the field's
// background layers beyond the console's picture come from the game's own
// room tables (the room source; the Lost Age's addresses are in
// field_scene_renderer.cpp). The Broken Seal's world-map
// source, menu-graphics copy and spell-effect margins are not ported yet.
//
// Bench check (TLA_GPU_FIELD_CHECK=1, with GSR_GPU_FIELD=1): every drawn
// frame is compared with the console compositor's picture of the same frame
// and the differing ones are logged; the first few are saved as PNG pairs.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <memory>
#include <string>
#include <vector>

#include "field_scene.h"
#include "tla_frame_capture.h"
#include "tla_field_draw.h"
#include "tla_flat_map.h"
#include "tla_room_palette.h"
#include "effect_capture.h"
#include "env_flag.h"
#include "field_scene_renderer.h"
#include "gba_bus.h"
#include "gba_io.h"
#include "gba_ppu.h"
#include "gpu_present.h"
#include "gpu_surface.h"
#include "texture_readback_queue.h"
#include "frame_history.h"
#include "snapshot_pool.h"
#include "runtime.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"
#include "temporal_blend.h"
#include "tla.h"
#include "../../shared_game/quality_of_life.h"
#include "../../shared_game/world_margin_live.h"
#include "../../shared_game/gpu_field_context.h"
#include "tla_world_map.h"
#include "../../shared_game/autosave.h"
#include "../../shared_game/held_palette_effect.h"
#include "../../shared_game/party_window_fade.h"

namespace {

bool g_enabled = false;
bool g_only = false;
bool g_check = false;

// Filled at VBlank start (the scene capture hook), read at present time.
// Plain copies: the capture fires inside guest dispatch, where the window's
// GL context may not be current.
struct Capture {
    bool valid = false;
    gsr::FieldScene scene;
    std::vector<std::uint8_t> vram;
    // EWRAM, 256 KB: the room tables upload_room reads.
    std::vector<std::uint8_t> ewram;
    std::vector<std::uint8_t> iwram;
    // The spell sparks captured since the canvas was last published
    // (src/effect_capture.cpp), for the expanded view's margins.
    gsr::CapturedEffectFrame effects;
    // Raw copies for F12 (tla_gpu_field_dump): the registers the frame was
    // drawn with, objects, palettes and the PPU's per-row tables.
    std::vector<std::uint8_t> io, oam, pal, row_io, row_io_valid, row_affine,
        row_affine_valid;
    // While a game window is open: video memory with the background area
    // from before it opened, and the field's colours; else empty (the
    // renderer's live copies already agree). See build_room_vram.
    std::vector<std::uint8_t> room_vram;
    std::vector<std::uint16_t> room_palette;
    bool window_open = false;
    gsr::party_window_fade::Fade fade;  // fallen heroes' menu columns
    gsr::save_files::RedRows red_rows;  // red save files (save_files.h)
    unsigned ripple_phase = 0;
    // The iris wipe the frame shows (iris_window), or none.
    gsr::IrisWindow iris;
    // tla_camera_hold_y at capture time, for screen-fixed layers.
    int camera_hold_y = 0;
    // tla_camera_kept_bounds at capture time (x, y).
    bool kept[2] = {false, false};
    int kept_lo[2] = {0, 0}, kept_edge[2] = {0, 0};
    const std::uint8_t* rom = nullptr;
    std::size_t rom_bytes = 0;
};
// This frame's capture, shared with the present queue, the F12 history and
// the world map's held view (snapshot_pool.h); nothing changes one once taken.
gbarecomp::SnapshotPool<Capture> g_capture_pool;
std::shared_ptr<const Capture> g_capture;
bool capture_valid() { return g_capture && g_capture->valid; }
struct GpuPresentation {
    gbarecomp::HistoryFrame frame;
};
gbarecomp::TextureReadbackQueue<GpuPresentation> g_readback;

// The iris wipe, window effect 9 (FACTS.md).
//   - The effect block [0x0300007C] holds the type (s16 +0x528) and the size
//     step k (u16 +0x52A). k grows by 2 a frame; with bit 5 set it counts down
//     from 32.
//   - The routine 0x080CFEF4 centres the wipe on the leader, actor
//     [[0x0300006C] + 0x14 + id * 4]. The id is 8 when s16 [0x020004B6] != 0,
//     otherwise [0x02000454].
//   - The centre is x = actor+8 - camera x and
//     y = (actor+0x10 - actor+0xC) - camera y - 16 (16.16, camera in whole
//     pixels).
//   - Row y's half-width is sqrt((5k)^2 - 1.5 (y - cy)^2), clamped to 0..240.
//   - The rows on screen use the k read at the previous VBlank (checked over
//     every row of a recorded wipe).
// The centre was below the console's screen, so the renderer could not fit it
// from the rows.
gsr::IrisWindow iris_window(gba::GbaBus* bus, int* last_k) {
    gsr::IrisWindow out;
    const std::uint32_t block = bus->read32(0x0300007Cu);
    const int shown_k = *last_k;
    *last_k = -1;
    if ((block >> 24) != 0x02u ||
        static_cast<std::int16_t>(bus->read16(block + 0x528u)) != 9)
        return out;
    *last_k = bus->read16(block + 0x52Au);
    if (shown_k < 0) return out;
    const std::uint32_t field = bus->read32(0x03000020u);
    const unsigned id = static_cast<std::int16_t>(bus->read16(0x020004B6u)) != 0
        ? 8u : bus->read32(0x02000454u);
    if ((field >> 24) != 0x02u || id > 0xBFu) return out;
    const std::uint32_t actor = bus->read32(bus->read32(0x0300006Cu) + 0x14u + id * 4u);
    if ((actor >> 24) != 0x02u && (actor >> 24) != 0x03u) return out;
    const auto whole = [](std::int32_t v) { return v < 0 ? -(-v >> 16) : v >> 16; };
    const std::int32_t cam_x = static_cast<std::int32_t>(bus->read32(field + 0xE4u) & 0xFFFF0000u);
    const std::int32_t cam_y = static_cast<std::int32_t>(bus->read32(field + 0xE8u) & 0xFFFF0000u);
    const std::int32_t x = static_cast<std::int32_t>(bus->read32(actor + 8u));
    const std::int32_t y = static_cast<std::int32_t>(bus->read32(actor + 0x10u)) -
                           static_cast<std::int32_t>(bus->read32(actor + 0xCu));
    const int k = (shown_k & 0x20) ? 32 - (shown_k & 31) : (shown_k & 31);
    if (k <= 0) return out;
    out.found = true;
    out.cx = static_cast<float>(whole(x - cam_x));
    out.cy = static_cast<float>(whole(y - cam_y) - 16);
    out.a2 = static_cast<float>(25 * k * k);
    out.b2 = out.a2 / 1.5f;
    return out;
}

// ---- Room graphics behind menus (Golden Sun 1's build_room_vram) ----------
//
// A menu borrows background tiles and colours that the room's layers also
// use. On the console the menu covers them; in the expanded margins they
// showed as noise (seen on the Djinn and Status screens).
//
// While no window is open, this code keeps a copy of the 64 KB background area
// and the 256 background colours. While a window is open, the margins' room
// tiles read the copy.
//
// The copy is refreshed only after the windows have stood closed for 30 field
// frames (Golden Sun 1's measured settle: menus free and retake their tiles
// between screens).
//   - Battle frames never refresh it and restart the count. A battle has no
//     window map, so the copy took the Djinn battle's graphics. Then the
//     Djinn's dialogue and Set screen right after it never let 30 field
//     frames pass (noise showed behind the Set screen).
//   - For a map's first 30 field frames, every closed frame refreshes the
//     copy, without the settle. A room's name box can open before 30 frames
//     pass, and the copy would still hold the last room's graphics.
//
// When a window is "open". BG0 is on, and the window system's map of it
// (kTlaWindowMapOffset) holds any tile but tile 0 in its first 20 rows.
//   - That map equals BG0's own map on this test in all 59 mode 0 captures
//     measured; with no window both are all tile 0.
//   - Text tiles alone missed the Djinn screen's first frame, empty boxes
//     before their text (one frame of noise and a parked Djinn in the
//     margins).
//   - The window map is used, not BG0 itself, because the Broken Seal also
//     used BG0 for room lighting. It is also written a frame before BG0 gets
//     it at VBlank, so the copy also serves the one frame after a window
//     closes.
//   - The game's tile-use table (window block + 0xE38, 256 bytes; the Broken
//     Seal's is at + 0xDA0) agreed in 87 of 90 captures. Three kept two
//     entries with no window, which would hold the copy into the next room.
//
// Colours. The Djinn screen replaces banks 4-7 and 14, and Status replaces
// banks 0, 1 and 14 (measured against the room's own colours), as in the
// Broken Seal. A screen-wide change (a fade, a flash) changes every used bank
// of 0-13 and keeps the live colours there. The Lost Age fades with the
// colours themselves (BLDY 0), so the copy taken while leaving Madra was
// black. A bank that is black in the copy but not live therefore counts as
// used; otherwise the town's name box on return painted the margins black.
//
// The room's own tile animation keeps running behind a window. Example:
// Alhafra's port water (map 98) rewrites its 32 tiles at 0x0600F800 every
// second frame through the whole "Ahoy, Briggs!" dialogue, and the copy froze
// it in the margins. So:
//   - A tile slot that changes between two trusted closed frames (those that
//     may refresh the copy) is animation. The images it showed then are kept.
//   - While a window is open, the slot takes the live image whenever it is one
//     of those images.
//   - A menu's own tile is never one, so a slot a menu borrows keeps the copy.
//   - Over every closed frame measured, animated slots cycle 2 to 32 images
//     (32: the port water, a complete cycle in 120 frames). A slot showing
//     more is not a looping animation (a field spell's canvas) and stays
//     frozen for the map.
struct RoomTileImages {
    std::vector<std::uint64_t> seen;
    bool not_animation = false;
};
std::vector<RoomTileImages> g_room_tile_images;
std::vector<std::uint8_t> g_room_previous_trusted;
constexpr std::size_t kTileBytes = 32u;
constexpr std::size_t kMaxAnimationImages = 32u;

std::vector<std::uint8_t> g_room_char_copy;
std::vector<std::uint16_t> g_room_palette_copy;
int g_room_copy_closed_frames = 0;
std::uint16_t g_room_copy_map = 0;
int g_room_copy_map_frames = 0;
constexpr int kRoomCopySettleFrames = 30;
constexpr std::size_t kBgAreaBytes = 0x10000u;
constexpr std::size_t kBgPaletteEntries = 256;

void reset_room_copy() {
    g_room_char_copy.clear();
    g_room_palette_copy.clear();
    g_room_copy_closed_frames = 0;
    g_room_copy_map = 0;
    g_room_copy_map_frames = 0;
    g_room_tile_images.clear();
    g_room_previous_trusted.clear();
}

std::uint64_t tile_image_hash(const std::uint8_t* tile) {
    std::uint64_t h = 0xCBF29CE484222325ull;
    for (std::size_t i = 0; i < kTileBytes; ++i)
        h = (h ^ tile[i]) * 0x100000001B3ull;
    return h;
}

void learn_room_animation(const std::vector<std::uint8_t>& vram) {
    if (g_room_tile_images.empty())
        g_room_tile_images.resize(kBgAreaBytes / kTileBytes);
    if (g_room_previous_trusted.size() == kBgAreaBytes) {
        for (std::size_t slot = 0; slot < g_room_tile_images.size(); ++slot) {
            const std::size_t at = slot * kTileBytes;
            RoomTileImages& images = g_room_tile_images[slot];
            if (images.not_animation ||
                std::memcmp(&g_room_previous_trusted[at], &vram[at], kTileBytes) == 0)
                continue;
            const std::uint8_t* both[] = {&g_room_previous_trusted[at], &vram[at]};
            for (const std::uint8_t* tile : both) {
                const std::uint64_t h = tile_image_hash(tile);
                if (std::find(images.seen.begin(), images.seen.end(), h) !=
                    images.seen.end())
                    continue;
                if (images.seen.size() == kMaxAnimationImages) {
                    images.not_animation = true;
                    images.seen.clear();
                    break;
                }
                images.seen.push_back(h);
            }
        }
    }
    g_room_previous_trusted.assign(vram.begin(), vram.begin() + kBgAreaBytes);
}

// The open-window side: animated slots showing a learned image go live, in
// the copy too, so a menu that later borrows the slot keeps the latest frame.
void apply_room_animation(const std::vector<std::uint8_t>& vram,
                          std::vector<std::uint8_t>* out) {
    for (std::size_t slot = 0; slot < g_room_tile_images.size(); ++slot) {
        const RoomTileImages& images = g_room_tile_images[slot];
        if (images.seen.empty()) continue;
        const std::size_t at = slot * kTileBytes;
        if (std::memcmp(&(*out)[at], &vram[at], kTileBytes) == 0) continue;
        const std::uint64_t h = tile_image_hash(&vram[at]);
        if (std::find(images.seen.begin(), images.seen.end(), h) ==
            images.seen.end())
            continue;
        std::memcpy(&(*out)[at], &vram[at], kTileBytes);
        std::memcpy(&g_room_char_copy[at], &vram[at], kTileBytes);
    }
}

bool game_window_open(gba::GbaBus* bus, const std::vector<std::uint8_t>& io) {
    const unsigned dispcnt = io[0] | (static_cast<unsigned>(io[1]) << 8);
    if ((dispcnt & 0x100u) == 0u) return false;
    const std::uint32_t block = bus->read32(kTlaWindowBlockSlot);
    if ((block >> 24) != 0x02u) return false;
    for (std::uint32_t i = 0; i < 32u * 20u; ++i)
        if ((bus->read16(block + kTlaWindowMapOffset + i * 2u) & 0x3FFu) != 0u)
            return true;
    return false;
}

void build_room_vram(const std::vector<std::uint8_t>& vram, bool field,
                     std::uint16_t map, bool window_open,
                     const std::array<std::uint16_t, 512>& palette,
                     std::vector<std::uint8_t>* out,
                     std::vector<std::uint16_t>* out_palette) {
    out->clear();
    out_palette->clear();
    if (vram.size() < kBgAreaBytes) return;
    if (!field) {
        g_room_copy_closed_frames = 0;
        return;
    }
    if (map != g_room_copy_map) {
        reset_room_copy();
        g_room_copy_map = map;
    }
    if (g_room_copy_map_frames < kRoomCopySettleFrames) ++g_room_copy_map_frames;
    if (!window_open) {
        const bool settled = g_room_copy_closed_frames >= kRoomCopySettleFrames;
        if (!settled) ++g_room_copy_closed_frames;
        if (settled || g_room_copy_map_frames < kRoomCopySettleFrames) {
            g_room_char_copy.assign(vram.begin(), vram.begin() + kBgAreaBytes);
            g_room_palette_copy.assign(palette.begin(),
                                       palette.begin() + kBgPaletteEntries);
            learn_room_animation(vram);
        } else {
            g_room_previous_trusted.clear();
        }
        if (g_room_copy_closed_frames > 1) return;
    } else {
        g_room_copy_closed_frames = 0;
        g_room_previous_trusted.clear();
    }
    if (g_room_char_copy.size() != kBgAreaBytes ||
        g_room_palette_copy.size() != kBgPaletteEntries)
        return;
    *out = vram;
    std::copy(g_room_char_copy.begin(), g_room_char_copy.end(), out->begin());
    apply_room_animation(vram, out);
    out_palette->assign(palette.begin(), palette.end());
    bool any_used = false, all_used_changed = true;
    for (int bank = 0; bank < 14; ++bank) {
        bool used = false, changed = false;
        for (int i = bank * 16; i < bank * 16 + 16; ++i) {
            if (g_room_palette_copy[i] != 0u || palette[i] != 0u) used = true;
            if (palette[i] != g_room_palette_copy[i]) changed = true;
        }
        if (!used) continue;
        any_used = true;
        if (!changed) all_used_changed = false;
    }
    const std::size_t restore_from = any_used && all_used_changed ? 14u * 16u : 0u;
    std::copy(g_room_palette_copy.begin() + restore_from,
              g_room_palette_copy.end(), out_palette->begin() + restore_from);
}

unsigned long long g_captured = 0, g_undescribed = 0, g_presented = 0;

void capture() {
    gba::GbaBus* bus = gbarecomp::active_bus();
    gba::GbaPpu* ppu = gbarecomp::active_ppu();
    if (!bus || !ppu) return;
    tla_trackers_sample();
    if (g_only) {
        static bool applied = false;
        if (!applied) {
            applied = true;
            ppu->set_native_raster_enabled(false);
            std::fprintf(stderr,
                         "[gpu-field] console compositor off: the graphics card "
                         "draws every frame, refused frames go magenta\n");
        }
    }

    // The registers this frame was drawn with (field_scene_rows_drawn_mode;
    // Golden Sun 1, a battle flash).
    std::vector<std::uint8_t> io(bus->io().raw(),
                                 bus->io().raw() + gba::GbaIo::kIoSize);
    if (const std::uint8_t* row = gsr::field_scene_rows_drawn_mode(
            io.data(), ppu->latched_native_line_io(),
            ppu->latched_native_line_io_valid()))
        std::memcpy(io.data(), row, gsr::FieldScene::kLineIoBytes);

    // Diagnostic capture alone must not enable placement overrides on the
    // console compositor path; retain the existing renderer-owned hook
    // behaviour.
    const bool window_open = game_window_open(bus, io);
    if (g_enabled) tla_widescreen_frame(window_open);
    gsr::FieldScene scene{};
    gsr::field_scene_capture(&scene, ppu->frame_count(), io.data(), bus->oam_ptr(),
                             bus->pal_ptr(), ppu->latched_native_line_io(),
                             ppu->latched_native_line_io_valid(),
                             ppu->latched_native_affine_line_refs(),
                             ppu->latched_native_affine_line_ref_valid(),
                             int(ppu->view_extra_top()), int(ppu->view_extra_bottom()));
    ++g_captured;
    if (!scene.valid || !scene.supported) {
        ++g_undescribed;
        if ((g_only || g_check) && scene.unsupported_reason) {
            static const char* said = nullptr;
            if (said != scene.unsupported_reason) {
                said = scene.unsupported_reason;
                std::fprintf(stderr, "[gpu-field] frame %llu not described: %s\n",
                             static_cast<unsigned long long>(ppu->frame_count()),
                             said);
            }
        }
    }
    // Retain raw state even for unsupported frames: F12 must show the fault,
    // rather than borrow the last frame the renderer accepted. A recycled
    // capture: every field is set again below.
    const std::shared_ptr<Capture> capture = g_capture_pool.take();
    capture->scene = scene;
    capture->ripple_phase = bus->read8(0x0300122Cu);
    static int iris_k = -1;
    capture->iris = iris_window(bus, &iris_k);
    capture->camera_hold_y = tla_camera_hold_y();
    tla_camera_kept_bounds(capture->kept, capture->kept_lo, capture->kept_edge);
    capture->rom = bus->rom_ptr();
    capture->rom_bytes = bus->rom_size();
    // The console's 96 KB, and the second sprite page while it is in use
    // (src/tla_sprite_memory.cpp).
    capture->vram.assign(bus->vram_ptr(), bus->vram_ptr() +
        gba::GbaBus::kNativeVram + (bus->obj_extra() ? gba::GbaBus::kObjExtraVram : 0));
    capture->window_open = window_open;
    capture->fade = gsr::party_window_fade::find(*bus);
    capture->red_rows = tla_save_files_red_rows();
    const bool field =
        scene.video_mode == 0 && bus->read32(kTlaBattleSlot) == 0u;
    build_room_vram(capture->vram, field, bus->read16(kTlaMapIdAddr),
                    capture->window_open, scene.palette,
                    &capture->room_vram, &capture->room_palette);
    capture->ewram.assign(bus->ewram_ptr(), bus->ewram_ptr() + 256 * 1024);
    capture->iwram.assign(bus->iwram_ptr(), bus->iwram_ptr() + 32 * 1024);
    tla_restore_room_palette(capture->window_open,
        capture->ewram.data(), capture->ewram.size(),
        capture->iwram.data(), capture->iwram.size(), &capture->room_palette);
    capture->io = io;
    capture->oam.assign(bus->oam_ptr(), bus->oam_ptr() + 1024);
    capture->pal.assign(bus->pal_ptr(), bus->pal_ptr() + 1024);
    constexpr std::size_t kRows = gsr::FieldScene::kRows;
    const auto* row_bytes = ppu->latched_native_line_io();
    capture->row_io.assign(row_bytes, row_bytes + kRows * gsr::FieldScene::kLineIoBytes);
    const bool* io_valid = ppu->latched_native_line_io_valid();
    capture->row_io_valid.assign(io_valid, io_valid + kRows);
    const auto* affine = reinterpret_cast<const std::uint8_t*>(
        ppu->latched_native_affine_line_refs());
    capture->row_affine.assign(affine, affine + kRows * 4u * sizeof(std::int32_t));
    const bool* affine_valid = ppu->latched_native_affine_line_ref_valid();
    capture->row_affine_valid.assign(affine_valid, affine_valid + kRows);
    // Sparks for the canvas this frame shows, while the game still copies
    // it to the screen; an ended spell's are dropped.
    capture->effects = gsr::effect_capture_take();
    if (ppu->frame_count() > gsr::effect_capture_last_copy_frame() + 8u)
        capture->effects = {};
    capture->valid = scene.valid && scene.supported;
    g_capture = capture;
}

// Flat magenta: no Golden Sun palette makes it, so it is never mistaken for
// a drawing fault. Refusals are counted and logged each time the run of one
// reason doubles.
bool paint_refused(std::uint8_t* rgb, std::uint32_t width, std::uint32_t height,
                   const char* reason) {
    const std::size_t pixels = std::size_t(width) * height;
    for (std::size_t i = 0; i < pixels; ++i) {
        rgb[i * 3 + 0] = 0xFF;
        rgb[i * 3 + 1] = 0x00;
        rgb[i * 3 + 2] = 0xFF;
    }
    static const char* last = nullptr;
    static unsigned long long count = 0;
    if (reason != last) { last = reason; count = 0; }
    ++count;
    if ((count & (count - 1)) == 0)
        std::fprintf(stderr, "[gpu-field] refused %llu frame(s) in a row: %s\n",
                     count, reason ? reason : "no reason given");
    return true;
}

struct CheckTally {
    unsigned long long drawn = 0, differing = 0, saved = 0;
};
CheckTally g_tally;

void check_against_console(const std::uint8_t* console, const std::uint8_t* gpu,
                           std::uint32_t width, std::uint32_t height,
                           std::uint64_t frame) {
    ++g_tally.drawn;
    const std::size_t pixels = std::size_t(width) * height;
    std::size_t differ = 0;
    int first_x = -1, first_y = -1;
    // Compared as the console's 5-bit colours: the shader writes c / 31 and
    // the compositor (c << 3) | (c >> 2), which can differ by one step of 255.
    auto same = [](const std::uint8_t* p, const std::uint8_t* q) {
        return (p[0] >> 3) == (q[0] >> 3) && (p[1] >> 3) == (q[1] >> 3) &&
               (p[2] >> 3) == (q[2] >> 3);
    };
    for (std::size_t i = 0; i < pixels; ++i) {
        if (same(console + i * 3, gpu + i * 3)) continue;
        if (differ++ == 0) {
            first_x = int(i % width);
            first_y = int(i / width);
        }
    }
    if (!differ) return;
    ++g_tally.differing;
    if (g_tally.differing <= 50 || (g_tally.differing & (g_tally.differing - 1)) == 0)
        std::fprintf(stderr,
                     "[gpu-field] check: frame %llu differs in %zu pixels, first "
                     "at %d,%d\n",
                     static_cast<unsigned long long>(frame), differ, first_x, first_y);
    if (g_tally.saved < 8) {
        ++g_tally.saved;
        const std::string base = "gpu_check_" + std::to_string(frame);
        gbarecomp::write_png(base + "_console.png", console, width, height);
        gbarecomp::write_png(base + "_gpu.png", gpu, width, height);
    }
}

// ---- World-map margin hold ------------------------------------------------
//
// A menu over the world map switches the screen from the world map's mode 2
// to mode 0 text layers on the same map blocks (BG2 block 10, BG3 block 8;
// measured: DISPCNT 0x1F40, BG2CNT 0xAA0F, BG3CNT 0xA80B, as in Golden Sun 1).
// The margins are only drawn from the world map in mode 2, so they went black
// for as long as the menu was open.
// The camera cannot move while a menu is open, so the margins of the last
// settled world-map frame are still the right picture. We keep that capture
// and render it into the renderer's margin target on menu frames.
gsr::WorldMarginHold<Capture> g_world_hold;

bool world_map_blocks(const std::vector<std::uint8_t>& io) {
    if (io.size() < 0x10) return false;
    const unsigned bg2 = io[0x0C] | (static_cast<unsigned>(io[0x0D]) << 8);
    const unsigned bg3 = io[0x0E] | (static_cast<unsigned>(io[0x0F]) << 8);
    return ((bg3 >> 8) & 0x1Fu) == 8u && ((bg2 >> 8) & 0x1Fu) == 10u;
}

bool on_world_map(const Capture& capture) {
    return capture.io.size() >= 0x58u && capture.ewram.size() >= 0x42Au &&
           tla_world_map(unsigned(capture.ewram[0x428]) |
                         (unsigned(capture.ewram[0x429]) << 8)) &&
           world_map_blocks(capture.io);
}

TlaFieldFrame make_tla_field_frame(const Capture& capture, bool side_picture) {
    TlaFieldFrame in;
    in.vram = &capture.vram;
    in.ewram = capture.ewram.data();
    in.ewram_bytes = capture.ewram.size();
    in.iwram = capture.iwram.data();
    in.iwram_bytes = capture.iwram.size();
    if (capture.io.size() >= 0x400u) in.io = capture.io.data();
    in.rom = capture.rom;
    in.rom_bytes = capture.rom_bytes;
    if (!capture.room_vram.empty()) {
        in.room_vram = capture.room_vram.data();
        in.room_vram_bytes = capture.room_vram.size();
    }
    if (!capture.room_palette.empty()) {
        in.room_palette = capture.room_palette.data();
        in.room_palette_entries = capture.room_palette.size();
    }
    in.menu_open = capture.window_open;
    in.fade = capture.fade;
    in.red_rows = capture.red_rows;
    in.reveal_full = tla_field_psy_reveal();
    in.flicker_weight = gbarecomp::temporal_blend_weight_for_index(
        gbarecomp::flicker_reduction_index());
    in.side_picture = side_picture;
    in.ripple_phase = capture.ripple_phase;
    in.camera_hold_y = capture.camera_hold_y;
    for (int n = 0; n < 2; ++n) {
        in.kept[n] = capture.kept[n];
        in.kept_lo[n] = capture.kept_lo[n];
        in.kept_edge[n] = capture.kept_edge[n];
    }
    in.sparks = capture.effects.stamps.data();
    in.spark_count = int(capture.effects.stamps.size());
    in.spark_art = capture.effects.artwork.data();
    in.spark_art_bytes = capture.effects.artwork.size();
    return in;
}

bool draw_world_margin_capture(gsr::FieldSceneRenderer& renderer,
                               TlaFieldDrawState& draw_state,
                               const Capture& m,
                               std::uint32_t width, std::uint32_t height) {
    gsr::FieldScene scene_copy = m.scene;
    gsr::SceneMargins margins;
    margins.iris = m.iris;
    const TlaFieldFrame in = make_tla_field_frame(m, true);
    return tla_field_draw(renderer, scene_copy, margins, in,
                          int(width), int(height), draw_state);
}

bool draw_capture(const std::shared_ptr<const Capture>& owned, std::uint8_t* rgb,
                  std::uint32_t width, std::uint32_t height,
                  const char** refusal = nullptr,
                  gbarecomp::HistoryFrame* frame = nullptr,
                  bool synchronous = true,
                  bool direct = false) {
    const Capture& capture = *owned;
    gsr::FieldScene scene = capture.scene;
    gsr::SceneMargins margins;  // margin data lives outside the scene
    margins.iris = capture.iris;
    // Extra: what the renderer has been handed (TlaFieldDrawState).
    auto* const context = gsr::gpu_field_context<TlaFieldDrawState>(
        [] { g_readback.invalidate(); }, nullptr);
    if (!context) {
        if (refusal) *refusal = "the renderer could not start";
        return false;
    }
    auto& surface = context->surface;
    auto& renderer = context->renderer;
    if (!renderer.set_output_size(int(width), int(height))) {
        if (refusal) *refusal = "the output size was refused";
        return false;
    }

    const bool world = on_world_map(capture);
    const bool expanded = width > 240u || height > 160u;
    gsr::WorldCompose compose{};
    bool mixed = false;
    if (!world) {
        g_world_hold.forget();
        renderer.clear_world_margin();
    } else {
        const unsigned mode = capture.io[0] & 7u;
        mixed = gsr::world_rows_mixed(capture.scene);
        if (expanded && mode == 2u && mixed) {
            compose.mode = gsr::WorldComposeMode::ShutterRows;
        } else if (expanded && mode == 0u &&
                   !tla_flat_map::is_map_screen(capture.io.data(), capture.io.size(),
                                                capture.ewram.data(), capture.ewram.size())) {
            const bool glow_candidate = tla_beam_glow_frame(
                capture.io.data(),
                unsigned(capture.ewram[0x428]) | (unsigned(capture.ewram[0x429]) << 8));
            const bool has_margin = g_world_hold.draw_margin(capture,
                !glow_candidate && g_world_hold.changes_background(capture.pal),
                [&](const Capture& c) {
                    return draw_world_margin_capture(renderer, context->extra, c,
                                                     width, height);
                },
                renderer.has_world_margin(int(width), int(height)));
            int shake_x = 0, shake_y = 0;
            const unsigned dispcnt = capture.io[0] | (unsigned(capture.io[1]) << 8);
            if ((dispcnt & 0x0200u) && (capture.io[0x0A] & 0x80u)) {
                const auto offset = [&](unsigned at) {
                    const int v = int((capture.io[at] | (unsigned(capture.io[at + 1]) << 8)) & 0x1FFu);
                    return v >= 0x100 ? v - 0x200 : v;
                };
                shake_x = offset(0x14);
                shake_y = offset(0x16);
            }
            const int evb = std::min<int>(capture.io[0x53] & 0x1Fu, 16);
            compose.mode = gsr::WorldComposeMode::HoldMargins;
            compose.has_margin = has_margin;
            compose.mixed_modes = mixed;
            compose.glow = has_margin && glow_candidate;
            compose.evb = evb;
            compose.shake_x = shake_x;
            compose.shake_y = shake_y;
        }
    }

    TlaFieldFrame in = make_tla_field_frame(capture, false);
    in.world_compose = compose;
    if (!tla_field_draw(renderer, scene, margins, in, int(width), int(height),
                        context->extra)) {
        if (refusal) *refusal = renderer.declined_reason();
        return false;
    }
    if (world && (capture.io[0] & 7u) == 2u && !mixed && !gsr::world_map_fading(capture.io))
        g_world_hold.remember(owned);
    if (frame && direct) {
        // Phones: the card's picture straight to the window, no copy back
        // (gpu_present.h); `frame` stays this frame's.
        g_readback.invalidate();
        gbarecomp::gpu_present_offer({static_cast<unsigned>(renderer.output_texture()),
                                      int(width), int(height)});
        frame->gpu_drawn = true;
        return true;
    }
    if (frame) {
        const bool battle = frame->battle_2x;
        frame->gpu_drawn = true;
        auto delivered = g_readback.present(surface, renderer.output_texture(),
            int(width), int(height), rgb, runtime_current_frame(),
            GpuPresentation{std::move(*frame)}, synchronous);
        if (!delivered) {
            *frame = gbarecomp::FrameHistory::capture();
            frame->battle_2x = battle;
            frame->scene = tla_gpu_field_snapshot(width, height, &frame->scene_frame);
            if (refusal) *refusal = "GPU readback failed";
            return false;
        }
        *frame = std::move(delivered->frame);
    }
    return true;
}

// The capture inside one of tla_gpu_field_snapshot's snapshots, or null.
std::shared_ptr<const Capture> snapshot_capture(const gbarecomp::SceneSnapshot* snapshot);

}  // namespace

// The present hook calls this first (tla_rewind.cpp). True means `rgb`
// contains the rendered frame or the refusal colour. `held`: an earlier
// frame's snapshot to draw again instead of this frame's capture.
bool tla_gpu_field_present(std::uint8_t* rgb, std::uint32_t width,
                           std::uint32_t height,
                           gbarecomp::HistoryFrame& frame, bool synchronous,
                           bool direct_ok, const gbarecomp::SceneSnapshot* held) {
    gbarecomp::gpu_present_withdraw();
    if (!g_enabled) { g_readback.invalidate(); return false; }
    const std::shared_ptr<const Capture> held_capture = snapshot_capture(held);
    ++g_presented;
    if (g_check && g_presented % 300u == 0u)
        std::fprintf(stderr,
                     "[gpu-field] check: %llu captured (%llu not described), "
                     "%llu presented, %llu drawn, %llu differ\n",
                     g_captured, g_undescribed, g_presented, g_tally.drawn,
                     g_tally.differing);
    if (!held_capture && !capture_valid()) {
        g_readback.invalidate();
        return g_only ? paint_refused(rgb, width, height,
                                      "nothing was captured this frame")
                      : false;
    }
    const std::shared_ptr<const Capture> source = held_capture ? held_capture : g_capture;

    const bool expanded = width > 240u || height > 160u;
    if (gsr::effect_capture_enabled() != expanded)
        gsr::effect_capture_set_enabled(expanded);
    const char* reason = nullptr;
    bool drawn = false;
    try {
        if (g_check && !g_only) {
            std::vector<std::uint8_t> gpu(std::size_t(width) * height * 3u);
            drawn = draw_capture(source, gpu.data(), width, height, &reason,
                                 &frame, true);
            if (drawn) {
                check_against_console(rgb, gpu.data(), width, height, source->scene.frame);
                std::memcpy(rgb, gpu.data(), gpu.size());
            }
        } else {
            // `direct_ok`: the caller paints nothing on the CPU picture.
            const bool direct = direct_ok && !synchronous && !g_check &&
                                gbarecomp::gpu_present_accepting();
            drawn = draw_capture(source, rgb, width, height, &reason,
                                 &frame, synchronous || g_check, direct);
        }
    } catch (const std::bad_alloc&) {
        g_readback.invalidate();
        return g_only ? paint_refused(rgb, width, height, "renderer allocation failed") : false;
    }
    if (!drawn) {
        g_readback.invalidate();
        static const char* said = nullptr;
        if (reason && reason != said) {
            said = reason;
            std::fprintf(stderr, "[gpu-field] declined frame %llu: %s\n",
                         static_cast<unsigned long long>(source->scene.frame), reason);
        }
        return g_only ? paint_refused(rgb, width, height, reason) : false;
    }
    static bool announced = false;
    if (!announced) {
        announced = true;
        std::fprintf(stderr, "[gpu-field] drawing at %ux%u\n", width, height);
    }
    return true;
}

// Each F12 history frame owns its rendering inputs, including room/menu
// copies, signed sprite placement and the published spell/triangle history.
namespace {
std::string snapshot_bytes(const Capture& c, std::uint32_t width, std::uint32_t height) {
    std::string objects, stamps = tla_frame::pack_stamps(c.effects.stamps), state,
        camera;
    tla_frame::u32(camera, static_cast<std::uint32_t>(c.camera_hold_y));
    // tla_camera_kept_bounds, x then y: kept, lo, edge (whole pixels).
    std::string kept;
    for (int n = 0; n < 2; ++n) {
        tla_frame::u32(kept, c.kept[n] ? 1u : 0u);
        tla_frame::u32(kept, static_cast<std::uint32_t>(c.kept_lo[n]));
        tla_frame::u32(kept, static_cast<std::uint32_t>(c.kept_edge[n]));
    }
    for (const auto& object : c.scene.objects) {
        tla_frame::u32(objects, std::uint32_t(object.x));
        tla_frame::u32(objects, std::uint32_t(object.y));
        // Bit 0 parked; bits 16+ the tile number the sprite was drawn from.
        tla_frame::u32(objects, (object.parked ? 1u : 0u) | (object.tile << 16));
    }
    tla_frame::u32(state, (c.window_open ? 1u : 0u) | (c.valid ? 2u : 0u));
    tla_frame::u32(state, c.ripple_phase);
    std::string iris;
    if (c.iris.found)
        for (float v : {c.iris.cx, c.iris.cy, c.iris.a2,
                        c.iris.b2})
            iris.append(reinterpret_cast<const char*>(&v), sizeof v);
    const auto part = [](const char* name, const auto& data) {
        return tla_frame::Section{name, reinterpret_cast<const std::uint8_t*>(data.data()),
            data.size() * sizeof(*data.data())};
    };
    return tla_frame::encode(c.scene.frame, width, height, {
        part("IO", c.io), part("OAM", c.oam), part("PAL", c.pal),
        tla_frame::Section{"VRAM", c.vram.data(), gba::GbaBus::kNativeVram},
        tla_frame::Section{"OBJX", c.vram.data() + gba::GbaBus::kNativeVram,
                           c.vram.size() - gba::GbaBus::kNativeVram},
        part("ROWIO", c.row_io),
        part("ROWVALID", c.row_io_valid), part("AFFREF", c.row_affine),
        part("AFFVALID", c.row_affine_valid), part("EWRAM", c.ewram),
        part("IWRAM", c.iwram), part("ROOMVRAM", c.room_vram),
        part("ROOMPAL", c.room_palette), part("OBJECTS", objects),
        part("FXSTAMP", stamps), part("FXART", c.effects.artwork), part("STATE", state),
        part("CAMHOLD", camera), part("KEPT", kept), part("IRIS", iris)});
}

struct Snapshot final : gbarecomp::SceneSnapshot {
    std::shared_ptr<const Capture> capture;
    std::uint32_t width = 0, height = 0;
    std::string bytes() const override { return snapshot_bytes(*capture, width, height); }
};

std::shared_ptr<const Capture> snapshot_capture(const gbarecomp::SceneSnapshot* snapshot) {
    const auto* own = dynamic_cast<const Snapshot*>(snapshot);
    return own ? own->capture : nullptr;
}
}  // namespace

std::shared_ptr<const gbarecomp::SceneSnapshot> tla_gpu_field_snapshot(
    std::uint32_t width, std::uint32_t height, std::uint64_t* scene_frame) {
    if (!g_capture || g_capture->io.empty()) return nullptr;
    if (scene_frame) *scene_frame = g_capture->scene.frame;
    auto snapshot = std::make_shared<Snapshot>();
    snapshot->capture = g_capture;
    snapshot->width = width;
    snapshot->height = height;
    return snapshot;
}

// Compatibility name for the single-frame report (format TLAGPUF2).
void tla_gpu_field_dump(const std::string& path) {
    const auto* ppu = gbarecomp::active_ppu();
    if (!ppu) return;
    const auto snapshot = tla_gpu_field_snapshot(
        240u + ppu->view_extra_left() + ppu->view_extra_right(),
        160u + ppu->view_extra_top() + ppu->view_extra_bottom());
    if (!snapshot) return;
    const std::string bytes = snapshot->bytes();
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
}

// The engine's function entry observer (src/tla_runner.cpp): the spell
// effect capture watches the game's canvas and stamp routines.
void tla_gpu_field_on_entry(std::uint32_t pc) {
    if (!gsr::effect_capture_enabled()) return;
    tla_spell_mesh_on_entry(pc);
    if (!gsr::effect_capture_observes(pc)) return;
    if (gba::GbaBus* bus = gbarecomp::active_bus())
        gsr::effect_capture_on_entry(pc, &g_cpu.R[0], bus->ewram_ptr(),
                                     bus->iwram_ptr(), bus->rom_ptr(),
                                     bus->rom_size(), runtime_current_frame());
}

bool tla_flat_map_on_entry(std::uint32_t pc) {
    // The unpack wrapper, called from one of the map's two row loaders.
    if (pc != TLA_ROM(0x0801587Cu)) return false;
    const std::uint32_t back = g_cpu.R[14] & ~1u;
    if (back != TLA_ROM(0x080ECF1Cu) && back != TLA_ROM(0x080ECFD4u)) return false;
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus) return false;
    const unsigned file = bus->read16(0x02000000u + tla_flat_map::kFileSwitch) ? 0x23u : 0x24u;
    if (!tla_flat_map::past_table(bus->rom_ptr(), bus->rom_size(), file, g_cpu.R[0]))
        return false;
    // A row past the map's table (tla_flat_map::past_table): unpack nothing.
    gbarecomp::guest_return_now(pc, 0u);
    return true;
}

void tla_install_gpu_field() {
    g_enabled = gbarecomp::env_flag("GSR_GPU_FIELD");
    g_only = g_enabled && gbarecomp::env_flag("GSR_GPU_FIELD_ONLY");
    g_check = g_enabled && gbarecomp::env_flag("TLA_GPU_FIELD_CHECK");
    // F12 captures hardware/room state even with the GPU renderer disabled.
    gbarecomp::set_scene_capture_hook(capture);
    gbarecomp::set_savestate_load_hook([] {
        g_readback.invalidate();
        tla_rewind_reset();
        reset_room_copy();
        g_capture = {};
        g_world_hold.forget();
        tla_local_music_on_savestate_load();
        tla_custom_battle_reset();
        tla_retreat_travel_reset();
        gsr::qol::reset();
        tla_radial_psynergy_reset();
        gsr::autosave::reset();
        tla_new_game_reset();
        tla_sprite_memory_restore();
    });
    if (g_enabled)
        std::fprintf(stderr, "[gpu-field] on%s%s\n", g_only ? ", only" : "",
                     g_check ? ", checking against the console compositor" : "");
    std::fprintf(stderr, "[tla] F12: keeping 120 frames with renderer snapshots\n");
}
