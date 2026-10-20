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
// field_scene_renderer.cpp, FACTS.md 2026-10-05). Golden Sun 1's world-map
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
#include "scene_repeat.h"
#include "tla_frame_capture.h"
#include "tla_water_ripple.h"
#include "tla_tornado_margins.h"
#include "tla_screen_effect_margins.h"
#include "tla_ship_water_margins.h"
#include "tla_background_margins.h"
#include "tla_room_palette.h"
#include "tla_room_sprite_clip.h"
#include "effect_capture.h"
#include "field_scene_renderer.h"
#include "world_map_source.h"
#include "gba_bus.h"
#include "gba_io.h"
#include "gba_ppu.h"
#include "gpu_surface.h"
#include "texture_readback_queue.h"
#include "frame_history.h"
#include "runtime.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"
#include "tla.h"

namespace {

bool env_on(const char* name) {
    const char* e = std::getenv(name);
    return e != nullptr && e[0] != '\0' && e[0] != '0';
}

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
    unsigned ripple_phase = 0;
    // The iris wipe the frame shows (iris_window), or none.
    gsr::IrisWindow iris;
    // tla_camera_hold_y at capture time, for screen-fixed layers.
    int camera_hold_y = 0;
    const std::uint8_t* rom = nullptr;
    std::size_t rom_bytes = 0;
};
Capture g_capture;
struct GpuPresentation {
    Capture capture;
    gbarecomp::HistoryFrame frame;
};
gbarecomp::TextureReadbackQueue<GpuPresentation> g_readback;

// The iris wipe, window effect 9 (FACTS.md, 2026-10-16): the effect block
// [0x0300007C] holds the type (s16 +0x528) and size step k (u16 +0x52A, 2 a
// frame; bit 5 set counts down from 32). 0x080CFEF4 centres it on the
// leader, actor [[0x0300006C] + 0x14 + id * 4] with id 8 when s16
// [0x020004B6] != 0, else [0x02000454]: x = actor+8 - camera x, y =
// (actor+0x10 - actor+0xC) - camera y - 16 (16.16, camera whole pixels).
// Row y's half-width is sqrt((5k)^2 - 1.5 (y - cy)^2), clamped to 0..240.
// The rows shown use the k read at the previous VBlank (F12 tla_rewind_0045
// frames 99..111, every row); the centre was below the console's screen,
// so the renderer could not fit it from the rows.
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
// A menu borrows background tiles and colours the room's layers also use; on
// the console it covers them, in the expanded margins they showed as noise
// (F12 0090 Djinn, 0091 Status). While no window is open this keeps the 64 KB
// background area and the 256 background colours; while one is open the
// margins' room tiles read the copy. The copy is refreshed only after the
// windows have stood closed for 30 field frames (Golden Sun 1's measured
// settle: menus free and retake their tiles between screens). Battle frames
// never refresh it and restart the count: a battle has no window map, so the
// copy took the Djinn battle's graphics, and the Djinn's dialogue and Set
// screen right after it never let 30 field frames pass (F12 0102, battle
// 33858-34944, noise behind the Set screen at 35616).
//
// Open: BG0 is on and the window system's map of it (kTlaWindowMapOffset)
// holds any tile but tile 0 in its first 20 rows. It equals BG0's own map
// on that test in all 59 mode 0 F12 captures, and with no window both are
// all tile 0. Text tiles alone missed the Djinn screen's first frame, empty
// boxes before their text (F12 0099 frame 108: one frame of noise and a
// parked Djinn in the margins). The window map, not BG0 itself, because
// Golden Sun 1 also used BG0 for room lighting; and it is written a frame
// before BG0 gets it at VBlank, so the copy also serves the one frame after
// a window closes. The game's tile-use table (window block + 0xE38, 256
// bytes, Golden Sun 1's + 0xDA0) agreed in 87 of 90: 0076, 0080 and 0097
// kept two entries with no window, which would hold the copy into the next
// room.
//
// Colours: Djinn replaces banks 4-7 and 14, Status 0, 1 and 14 (0090/0091
// against 0092), as in Golden Sun 1. A screen-wide change (a fade, a flash)
// changes every used bank of 0-13 and keeps the live colours there. The Lost
// Age fades with the colours themselves (BLDY 0), so the copy taken while
// leaving Madra was black; a bank black in the copy but not live counts as
// used, or the town's name box on return painted the margins black (F12
// 0106, from guest 66080).
//
// For a map's first 30 field frames every closed frame refreshes the copy,
// without the settle: a room's name box can open before 30 frames pass, and
// the copy would still hold the last room's graphics.
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
        }
        if (g_room_copy_closed_frames > 1) return;
    } else {
        g_room_copy_closed_frames = 0;
    }
    if (g_room_char_copy.size() != kBgAreaBytes ||
        g_room_palette_copy.size() != kBgPaletteEntries)
        return;
    *out = vram;
    std::copy(g_room_char_copy.begin(), g_room_char_copy.end(), out->begin());
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

    // When every row agrees on a display mode the end-of-frame registers do
    // not, the rows are the truth (Golden Sun 1, a battle flash).
    std::vector<std::uint8_t> io(bus->io().raw(),
                                 bus->io().raw() + gba::GbaIo::kIoSize);
    const std::uint8_t* rows = ppu->latched_native_line_io();
    const bool* rows_valid = ppu->latched_native_line_io_valid();
    int first = -1;
    bool agree = true;
    for (int y = 0; rows && rows_valid && y < gsr::FieldScene::kRows; ++y) {
        if (!rows_valid[y]) continue;
        const std::uint8_t* row = rows + std::size_t(y) * gsr::FieldScene::kLineIoBytes;
        if (first < 0) first = y;
        else if ((row[0] & 7u) !=
                 (rows[std::size_t(first) * gsr::FieldScene::kLineIoBytes] & 7u))
            agree = false;
    }
    if (first >= 0 && agree) {
        const std::uint8_t* row = rows + std::size_t(first) * gsr::FieldScene::kLineIoBytes;
        if ((row[0] & 7u) != (io[0] & 7u))
            std::memcpy(io.data(), row, gsr::FieldScene::kLineIoBytes);
    }

    // Diagnostic capture alone must not enable placement overrides in the
    // console/Direct3D path; retain the existing renderer-owned hook behavior.
    if (g_enabled) tla_widescreen_frame(game_window_open(bus, io));
    gsr::FieldScene scene{};
    gsr::field_scene_capture(&scene, ppu->frame_count(), io.data(), bus->oam_ptr(),
                             bus->pal_ptr(), ppu->latched_native_line_io(),
                             ppu->latched_native_line_io_valid(),
                             ppu->latched_native_affine_line_refs(),
                             ppu->latched_native_affine_line_ref_valid());
    ++g_captured;
    if (!scene.valid || !scene.supported) {
        g_capture.valid = false;
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
    // rather than borrow the last frame the renderer accepted.
    g_capture.scene = scene;
    g_capture.ripple_phase = bus->read8(0x0300122Cu);
    static int iris_k = -1;
    g_capture.iris = iris_window(bus, &iris_k);
    g_capture.camera_hold_y = tla_camera_hold_y();
    g_capture.rom = bus->rom_ptr();
    g_capture.rom_bytes = bus->rom_size();
    // The console's 96 KB, and the second sprite page while it is in use
    // (src/tla_sprite_memory.cpp).
    g_capture.vram.assign(bus->vram_ptr(), bus->vram_ptr() +
        gba::GbaBus::kNativeVram + (bus->obj_extra() ? gba::GbaBus::kObjExtraVram : 0));
    g_capture.window_open = game_window_open(bus, io);
    const bool field =
        scene.video_mode == 0 && bus->read32(kTlaBattleSlot) == 0u;
    build_room_vram(g_capture.vram, field, bus->read16(kTlaMapIdAddr),
                    g_capture.window_open, scene.palette,
                    &g_capture.room_vram, &g_capture.room_palette);
    g_capture.ewram.assign(bus->ewram_ptr(), bus->ewram_ptr() + 256 * 1024);
    g_capture.iwram.assign(bus->iwram_ptr(), bus->iwram_ptr() + 32 * 1024);
    tla_restore_room_palette(g_capture.window_open,
        g_capture.ewram.data(), g_capture.ewram.size(),
        g_capture.iwram.data(), g_capture.iwram.size(), &g_capture.room_palette);
    g_capture.io = io;
    g_capture.oam.assign(bus->oam_ptr(), bus->oam_ptr() + 1024);
    g_capture.pal.assign(bus->pal_ptr(), bus->pal_ptr() + 1024);
    constexpr std::size_t kRows = gsr::FieldScene::kRows;
    const auto* row_bytes = ppu->latched_native_line_io();
    g_capture.row_io.assign(row_bytes, row_bytes + kRows * gsr::FieldScene::kLineIoBytes);
    const bool* io_valid = ppu->latched_native_line_io_valid();
    g_capture.row_io_valid.assign(io_valid, io_valid + kRows);
    const auto* affine = reinterpret_cast<const std::uint8_t*>(
        ppu->latched_native_affine_line_refs());
    g_capture.row_affine.assign(affine, affine + kRows * 4u * sizeof(std::int32_t));
    const bool* affine_valid = ppu->latched_native_affine_line_ref_valid();
    g_capture.row_affine_valid.assign(affine_valid, affine_valid + kRows);
    // Sparks for the canvas this frame shows, while the game still copies
    // it to the screen; an ended spell's are dropped.
    g_capture.effects = gsr::effect_capture_take();
    if (ppu->frame_count() > gsr::effect_capture_last_copy_frame() + 8u)
        g_capture.effects = {};
    g_capture.valid = scene.valid && scene.supported;
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

bool draw_capture(const Capture& capture, std::uint8_t* rgb,
                  std::uint32_t width, std::uint32_t height,
                  const char** refusal = nullptr,
                  gbarecomp::HistoryFrame* frame = nullptr,
                  bool synchronous = true, Capture* delivered_capture = nullptr) {
    gsr::FieldScene scene = capture.scene;
    gsr::SceneMargins margins;  // margin data lives outside the scene
    margins.iris = capture.iris;
    // Created on first use, at present time, where the window's GL context
    // is current; gpu_surface attaches to it.
    struct Context {
        gbarecomp::GpuSurface surface;
        gsr::FieldSceneRenderer renderer;
    };
    static std::unique_ptr<Context> context;
    static bool init_attempted = false;
    static bool init_ok = false;
    if (!context) {
        context = std::make_unique<Context>();
        gbarecomp::set_frame_present_shutdown_hook([] {
            g_readback.invalidate();
            context.reset();  // renderer, then PBOs/fences, before the GL window
            init_attempted = init_ok = false;
        });
    }
    auto& surface = context->surface;
    auto& renderer = context->renderer;
    if (!init_attempted) {
        init_attempted = true;
        init_ok = surface.init() && renderer.init(&surface);
        if (!init_ok)
            std::fprintf(stderr,
                         "[gpu-field] renderer unavailable (%s); staying on the "
                         "console compositor\n",
                         !surface.ready() ? surface.failure() : renderer.failure());
    }
    if (!init_ok) {
        if (refusal) *refusal = "the renderer could not start";
        return false;
    }
    if (!renderer.set_output_size(int(width), int(height))) {
        if (refusal) *refusal = "the output size was refused";
        return false;
    }

    // The room source only where there is a margin to fill: the native view
    // stays exactly the picture checked against the console compositor.
    const bool expanded = width > 240u || height > 160u;
    if (expanded)
        tla_hide_outside_room(scene,
            capture.ewram.data(), capture.ewram.size(),
            capture.iwram.data(), capture.iwram.size());
    const unsigned map_id = expanded && capture.ewram.size() >= 0x42A
        ? unsigned(capture.ewram[0x428]) |
            (unsigned(capture.ewram[0x429]) << 8) : 0;
    tla_weather_sprite_margins(scene, margins, capture.vram);
    tla_tornado_margins(scene, margins, map_id,
        -(static_cast<int>(height) - 160) / 2, capture.camera_hold_y);
    bool port = false;
    if (expanded && capture.rom && capture.rom_bytes >= 0x20A8u + 1024u * 2u) {
        const auto sine = [&](unsigned index) {
            const auto* p = capture.rom + 0x20A8u + index * 2u;
            return static_cast<std::int16_t>(unsigned(p[0]) | (unsigned(p[1]) << 8));
        };
        const int first_y = -(static_cast<int>(height) - 160) / 2;
        tla_water_ripple(scene, margins, capture.ripple_phase, sine, first_y,
                         static_cast<int>(height));
        port = tla_port_wave(scene, margins, capture.ripple_phase, sine,
                             first_y, static_cast<int>(height));
    } else {
        margins.layers[1].scroll.clear();
        margins.layers[2].scroll.clear();
    }
    static std::vector<std::uint8_t> haze_vram;
    const bool haze_filled =
        port && tla_haze_fill_rows(capture.vram, scene, &haze_vram, 2);
    if (expanded) {
        tla_ship_water_margins(scene, margins, capture.vram);
        tla_background_margins(scene, margins,
            capture.ewram.data(), capture.ewram.size());
    }
    const std::vector<std::uint8_t>& vram = haze_filled ? haze_vram : capture.vram;
    renderer.upload_vram(vram.data(), vram.size());
    if (!capture.room_vram.empty())
        renderer.upload_room_vram(capture.room_vram.data(),
                                  capture.room_vram.size());
    renderer.upload_palette(scene.palette.data(),
                            scene.palette.size());
    if (!capture.room_palette.empty())
        renderer.upload_room_palette(capture.room_palette.data(),
                                     capture.room_palette.size());
    renderer.set_room_source_enabled(expanded);
    renderer.set_menu_open(capture.window_open);
    // Better Field Psy: Reveal covers the whole view, as in The Broken Seal.
    renderer.set_reveal_full(tla_field_psy_fast());
    if (expanded) {
        renderer.upload_room(capture.ewram.data(), capture.ewram.size());
        // Haze, mist, sunlight and any other repeating layer: the shared
        // rule, once the room source can say which layers it fills.
        gsr::continue_repeating_layers(scene, margins, vram.data(), vram.size(),
            [&](int bg) {
                bool from_record = false;
                return bg != 0 && renderer.room_known() &&
                       renderer.room_reference_for(scene, bg, &from_record);
            },
            -(static_cast<int>(height) - 160) / 2, static_cast<int>(height));
    }
    // Complete map tiles are shared code; this game's wrapper supplies its layout.
    static gsr::WorldMapSource world_map;
    if (expanded && scene.video_mode == 2 &&
        world_map.update(capture.ewram.data(), capture.ewram.size(),
            capture.iwram.data(), capture.iwram.size(), capture.rom, capture.rom_bytes)) {
        renderer.upload_world_map(world_map.tiles().data(), world_map.generation());
        const int agreement = renderer.world_map_agreement(scene);
        if (agreement >= 0 && agreement < gsr::FieldSceneRenderer::kRoomCheckMinAgreement && world_map.restore_previous())
            renderer.upload_world_map(world_map.tiles().data(), world_map.generation());
    } else {
        renderer.upload_world_map(nullptr, 0);
    }
    // Spell effects in the margins, Golden Sun 1's way (its
    // gpu_field_present_override): the game paints each effect into a
    // canvas as wide as the console's screen, and nothing exists past it.
    // The margins get the canvas's own fill and the sparks the game stamped
    // outside it, never the layer's wrapped or cut map. Expanded view only.
    renderer.set_effect_renderer_enabled(expanded);
    {
        const gsr::FieldScene& sc = scene;
        int effect_layer = -1;
        if (expanded && sc.video_mode == 1) {
            // A battle's non-wrapping affine BG2 (the stretched canvas), else
            // a layer showing the canvas as a tile block.
            const gsr::SceneLayer& bg2 = sc.layers[2];
            effect_layer = (bg2.enabled && bg2.affine && !bg2.wraps)
                ? 2
                : gsr::FieldSceneRenderer::find_effect_canvas_layer(
                      capture.vram.data(), capture.vram.size(), sc);
        }
        renderer.set_effect_layer(effect_layer);
        if (effect_layer >= 0) {
            const auto& asked = capture.effects.stamps;
            if (!asked.empty())
                renderer.upload_effect_sparks(asked.data(), int(asked.size()),
                                              capture.effects.artwork.data(),
                                              capture.effects.artwork.size(),
                                              effect_layer);
            renderer.analyse_effect_canvas(capture.vram.data(),
                                           capture.vram.size(),
                                           sc.layers[effect_layer]);
        }
    }
    renderer.set_scene_margins(&margins);
    if (!renderer.draw(scene)) {
        if (refusal) *refusal = renderer.declined_reason();
        return false;
    }
    if (frame) {
        const bool battle = frame->battle_2x;
        frame->gpu_drawn = true;
        auto delivered = g_readback.present(surface, renderer.output_texture(),
            int(width), int(height), rgb, runtime_current_frame(),
            GpuPresentation{capture, std::move(*frame)}, synchronous);
        if (!delivered) {
            *frame = gbarecomp::FrameHistory::capture();
            frame->battle_2x = battle;
            frame->scene = tla_gpu_field_snapshot(width, height, &frame->scene_frame);
            if (refusal) *refusal = "GPU readback failed";
            return false;
        }
        *frame = std::move(delivered->frame);
        *delivered_capture = std::move(delivered->capture);
    } else {
        // Redrawing a settled world-map view for a resize is synchronous;
        // it does not participate in the live presentation queue.
        return surface.read_texture_rgb(renderer.output_texture(), int(width), int(height), rgb);
    }
    return true;
}

// ---- World-map margin hold (Golden Sun 1's world_map_margin_hold) --------
//
// A menu over the world map switches the screen from the world map's mode 2
// to mode 0 text layers on the same map blocks (BG2 block 10, BG3 block 8;
// F12 0011, 2026-10-05: DISPCNT 0x1F40, BG2CNT 0xAA0F, BG3CNT 0xA80B, as in
// Golden Sun 1). The margins are only drawn from the world map in mode 2, so
// they went black for as long as the menu was open. The camera cannot move
// while a menu is open, so the margins of the last settled world-map frame
// are still the right picture: keep them and paint them around the live
// picture on menu frames.
std::vector<std::uint8_t> g_world_margin;
Capture g_world_settled;
std::uint32_t g_world_margin_w = 0, g_world_margin_h = 0;

bool world_map_blocks(const std::vector<std::uint8_t>& io) {
    if (io.size() < 0x10) return false;
    const unsigned bg2 = io[0x0C] | (static_cast<unsigned>(io[0x0D]) << 8);
    const unsigned bg3 = io[0x0E] | (static_cast<unsigned>(io[0x0F]) << 8);
    return ((bg3 >> 8) & 0x1Fu) == 8u && ((bg2 >> 8) & 0x1Fu) == 10u;
}

void world_map_margin_hold(std::uint8_t* rgb, std::uint32_t width,
                           std::uint32_t height, const Capture& capture) {
    const std::vector<std::uint8_t>& io = capture.io;
    const bool world_map = capture.ewram.size() >= 0x42Au &&
        (unsigned(capture.ewram[0x428]) |
         (unsigned(capture.ewram[0x429]) << 8)) == 2u;
    if (!world_map || !world_map_blocks(io)) {
        g_world_margin.clear();  // anywhere else: the saved view is stale
        g_world_settled = {};
        return;
    }
    const unsigned mode = io[0] & 7u;
    const std::uint32_t x0 = (width - 240u) / 2u, y0 = (height - 160u) / 2u;
    if (mode == 2u) {
        // Only a settled frame: every row in mode 2, no brightness fade.
        for (int y = 0; y < gsr::FieldScene::kRows; ++y)
            if (capture.scene.row_io_valid[y] &&
                (capture.scene.row_io[y][0] & 7u) != 2u)
                return;
        const unsigned bldcnt = io[0x50] | (static_cast<unsigned>(io[0x51]) << 8);
        const bool fading = ((bldcnt >> 6) & 3u) >= 2u && (io[0x54] & 0x1Fu) != 0u;
        if (!fading) {
            try {
                Capture settled = capture;
                std::vector<std::uint8_t> margin(rgb, rgb + std::size_t{width} * height * 3u);
                g_world_settled = std::move(settled);
                g_world_margin = std::move(margin);
                g_world_margin_w = width;
                g_world_margin_h = height;
            } catch (const std::bad_alloc&) {
                g_world_margin.clear();
                g_world_settled = {};
            }
        }
        return;
    }
    if (mode != 0u) return;
    // A flat picture (a fade to one colour) is no menu: the margins take
    // its colour.
    const std::uint8_t* first = rgb + (std::size_t{y0} * width + x0) * 3u;
    bool flat = true;
    for (std::uint32_t y = y0; y < y0 + 160u && flat; ++y) {
        const std::uint8_t* p = rgb + (std::size_t{y} * width + x0) * 3u;
        for (std::uint32_t x = 0; x < 240u; ++x, p += 3)
            if (std::memcmp(p, first, 3) != 0) { flat = false; break; }
    }
    std::uint8_t colour[3] = {0, 0, 0};
    if (flat) std::memcpy(colour, first, 3);
    if (!flat && g_world_settled.valid &&
        (g_world_margin_w != width || g_world_margin_h != height)) {
        try {
            std::vector<std::uint8_t> resized(std::size_t(width) * height * 3u);
            if (draw_capture(g_world_settled, resized.data(), width, height)) {
                g_world_margin = std::move(resized);
                g_world_margin_w = width;
                g_world_margin_h = height;
            }
        } catch (const std::bad_alloc&) {
            // Existing no-held-frame colour is the allocation fallback.
        }
    }
    const bool held = !flat && g_world_margin_w == width &&
                      g_world_margin_h == height && !g_world_margin.empty();
    for (std::uint32_t y = 0; y < height; ++y) {
        const bool native_row = y >= y0 && y < y0 + 160u;
        std::uint8_t* dst = rgb + std::size_t{y} * width * 3u;
        const std::uint8_t* src =
            held ? g_world_margin.data() + std::size_t{y} * width * 3u : nullptr;
        for (std::uint32_t x = 0; x < width; ++x) {
            if (native_row && x >= x0 && x < x0 + 240u) continue;
            // No settled frame to hold: black rather than the world map's
            // rotation-layer data read as text layers.
            std::memcpy(dst + x * 3u, held ? src + x * 3u : colour, 3);
        }
    }
}

}  // namespace

// Called first by the present hook (src/tla_rewind.cpp). True when `rgb`
// now holds the graphics card's picture (or the refusal colour).
bool tla_gpu_field_present(std::uint8_t* rgb, std::uint32_t width,
                           std::uint32_t height,
                           gbarecomp::HistoryFrame& frame, bool synchronous) {
    if (!g_enabled) { g_readback.invalidate(); return false; }
    ++g_presented;
    if (g_check && g_presented % 300u == 0u)
        std::fprintf(stderr,
                     "[gpu-field] check: %llu captured (%llu not described), "
                     "%llu presented, %llu drawn, %llu differ\n",
                     g_captured, g_undescribed, g_presented, g_tally.drawn,
                     g_tally.differing);
    if (!g_capture.valid) {
        g_readback.invalidate();
        return g_only ? paint_refused(rgb, width, height,
                                      "nothing was captured this frame")
                      : false;
    }

    const bool expanded = width > 240u || height > 160u;
    if (gsr::effect_capture_enabled() != expanded)
        gsr::effect_capture_set_enabled(expanded);
    const char* reason = nullptr;
    bool drawn = false;
    Capture delivered;
    try {
        if (g_check && !g_only) {
            std::vector<std::uint8_t> gpu(std::size_t(width) * height * 3u);
            drawn = draw_capture(g_capture, gpu.data(), width, height, &reason,
                                 &frame, true, &delivered);
            if (drawn) {
                check_against_console(rgb, gpu.data(), width, height, g_capture.scene.frame);
                std::memcpy(rgb, gpu.data(), gpu.size());
            }
        } else {
            drawn = draw_capture(g_capture, rgb, width, height, &reason,
                                 &frame, synchronous || g_check, &delivered);
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
                         static_cast<unsigned long long>(g_capture.scene.frame), reason);
        }
        return g_only ? paint_refused(rgb, width, height, reason) : false;
    }
    static bool announced = false;
    if (!announced) {
        announced = true;
        std::fprintf(stderr, "[gpu-field] drawing at %ux%u\n", width, height);
    }
    world_map_margin_hold(rgb, width, height, delivered);
    return true;
}

// Each F12 history frame owns its rendering inputs, including room/menu
// copies, signed sprite placement and the published spell/triangle history.
std::string tla_gpu_field_snapshot(std::uint32_t width, std::uint32_t height,
                                   std::uint64_t* scene_frame) {
    if (g_capture.io.empty()) return {};
    if (scene_frame) *scene_frame = g_capture.scene.frame;
    std::string objects, stamps = tla_frame::pack_stamps(g_capture.effects.stamps), state,
        camera;
    tla_frame::u32(camera, static_cast<std::uint32_t>(g_capture.camera_hold_y));
    for (const auto& object : g_capture.scene.objects) {
        tla_frame::u32(objects, std::uint32_t(object.x));
        tla_frame::u32(objects, std::uint32_t(object.y));
        // Bit 0 parked; bits 16+ the tile number the sprite was drawn from.
        tla_frame::u32(objects, (object.parked ? 1u : 0u) | (object.tile << 16));
    }
    tla_frame::u32(state, (g_capture.window_open ? 1u : 0u) | (g_capture.valid ? 2u : 0u));
    tla_frame::u32(state, g_capture.ripple_phase);
    std::string iris;
    if (g_capture.iris.found)
        for (float v : {g_capture.iris.cx, g_capture.iris.cy, g_capture.iris.a2,
                        g_capture.iris.b2})
            iris.append(reinterpret_cast<const char*>(&v), sizeof v);
    const auto part = [](const char* name, const auto& data) {
        return tla_frame::Section{name, reinterpret_cast<const std::uint8_t*>(data.data()),
            data.size() * sizeof(*data.data())};
    };
    return tla_frame::encode(g_capture.scene.frame, width, height, {
        part("IO", g_capture.io), part("OAM", g_capture.oam), part("PAL", g_capture.pal),
        tla_frame::Section{"VRAM", g_capture.vram.data(), gba::GbaBus::kNativeVram},
        tla_frame::Section{"OBJX", g_capture.vram.data() + gba::GbaBus::kNativeVram,
                           g_capture.vram.size() - gba::GbaBus::kNativeVram},
        part("ROWIO", g_capture.row_io),
        part("ROWVALID", g_capture.row_io_valid), part("AFFREF", g_capture.row_affine),
        part("AFFVALID", g_capture.row_affine_valid), part("EWRAM", g_capture.ewram),
        part("IWRAM", g_capture.iwram), part("ROOMVRAM", g_capture.room_vram),
        part("ROOMPAL", g_capture.room_palette), part("OBJECTS", objects),
        part("FXSTAMP", stamps), part("FXART", g_capture.effects.artwork), part("STATE", state),
        part("CAMHOLD", camera), part("IRIS", iris)});
}

// Compatibility name for the current single-frame report, now TLAGPUF2.
void tla_gpu_field_dump(const std::string& path) {
    const auto* ppu = gbarecomp::active_ppu();
    if (!ppu) return;
    const std::string bytes = tla_gpu_field_snapshot(
        240u + ppu->view_extra_left() + ppu->view_extra_right(),
        160u + ppu->view_extra_top() + ppu->view_extra_bottom());
    if (bytes.empty()) return;
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

void tla_install_gpu_field() {
    g_enabled = env_on("GSR_GPU_FIELD");
    g_only = g_enabled && env_on("GSR_GPU_FIELD_ONLY");
    g_check = g_enabled && env_on("TLA_GPU_FIELD_CHECK");
    // F12 captures hardware/room state even with the GPU renderer disabled.
    gbarecomp::set_scene_capture_hook(capture);
    gbarecomp::set_savestate_load_hook([] {
        g_readback.invalidate();
        tla_rewind_reset();
        reset_room_copy();
        g_capture = {};
        g_world_settled = {};
        g_world_margin.clear();
        g_world_margin_w = g_world_margin_h = 0;
    });
    if (g_enabled)
        std::fprintf(stderr, "[gpu-field] on%s%s\n", g_only ? ", only" : "",
                     g_check ? ", checking against the console compositor" : "");
    std::fprintf(stderr, "[tla] F12: keeping 120 frames with renderer snapshots\n");
}
