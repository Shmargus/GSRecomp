// gba_ppu.cpp — see gba_ppu.h.

#include "gba_ppu.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "snapshot.h"

namespace gba {

// Widescreen margin tilemap provider (Step C). When set (by the runtime-side
// sidecar) and the wide path samples a margin column (hardware x outside
// 0..239), the BG text-tilemap entry is sourced from this hook (the true
// extended world) instead of the wrapped 256px ring. Returns nonzero + fills
// *out_entry when data is available. nullptr (default) = vanilla wide behavior.
// Layering-clean: the PPU only calls a nullable pointer; the sidecar owns it.
extern "C" int (*g_ws_tilemap_provider)(int bg, int hw_x, int screen_y,
                                        uint16_t* out_entry) = nullptr;
extern "C" int (*g_ws_field_tilemap_source)(int bg, int screen_x,
                                            int screen_y,
                                            uint16_t* out_entry) = nullptr;
extern "C" void (*g_ws_field_tilemap_source_begin)(int screen_y,
                                                     uint16_t dispcnt,
                                                     const uint8_t* io) = nullptr;
extern "C" unsigned long long g_ws_expanded_diag[4] = {0, 0, 0, 0};
// Per-OBJ trust tally for the expanded-view margin defects: how many OBJ
// entries (per frame, summed here as a running session total) had both axes
// authenticated by a provider versus fell back to raw/wrapped OAM
// coordinates on at least one axis. Read by the game's tracer as a delta
// against the previous frame's totals. Never reset by the renderer.
extern "C" unsigned long long g_ws_obj_trusted_total = 0;
extern "C" unsigned long long g_ws_obj_untrusted_total = 0;
// See the declaration in gba_ppu.h: once-per-frame gate for a per-object
// provenance-reject tally kept on the game side. Defaults off; only
// render_scanline_wide below turns it on, and only while it renders
// logical_y == 0.
extern "C" int g_ws_obj_census_line = 0;
extern "C" int (*g_ws_bg_x_provider)(int bg, int output_x, int screen_y,
                                     int* out_hw_x) = nullptr;
extern "C" unsigned g_ws_bg_x_provider_layers = 0xFu;
extern "C" int (*g_ws_bg_sample_provider)(int bg, int output_x, int screen_y,
                                          int* out_hw_x, int* out_hw_y) =
    nullptr;
extern "C" unsigned g_ws_bg_sample_provider_layers = 0u;
extern "C" unsigned g_ws_bg_sample_provider_ignore_window_layers = 0u;
// Row composition census, armed by the same toggle as the row dump below.
// For every row of the expanded canvas, how many pixels each layer finally
// won. A band on screen that no register explains -- the stripe across the
// battle arena, 2026-09-12 -- shows up here as rows whose winning layer
// differs from their neighbours', which says WHICH layer to go and look at.
namespace {
constexpr std::size_t kRowCensusRows = 512;
constexpr std::size_t kRowCensusLayers = 7;  // BG0-3, OBJ, backdrop, other
std::uint32_t g_row_census[kRowCensusRows][kRowCensusLayers] = {};
}  // namespace
extern "C" int g_ws_row_state_dump_mode = -1;
extern "C" int g_ws_row_state_dump_frames = 0;
extern "C" int g_ws_defer_native_rows = 0;
extern "C" int g_ws_authored_margin_layers = 0;

// Widescreen pillarbox (Step C policy): when nonzero, the wide path renders the
// margin columns (outside the central 240) as solid black instead of extended
// world — used for non-overworld screens (menus, battles, transitions) so they
// are letterboxed rather than garbled. Set per-frame by the runtime policy.
extern "C" int g_ws_pillarbox = 0;
extern "C" int g_ws_pillarbox_left = 0;
extern "C" int g_ws_pillarbox_right = 0;
extern "C" int g_ws_pillarbox_top = 0;
extern "C" int g_ws_pillarbox_bottom = 0;
// Generic, game-owned per-frame world-space pixel shift for the expanded
// (wide) renderer only. When nonzero, every regular-BG sample (native columns
// and margins alike) and every OBJ's screen position moves by this amount in
// hardware pixels before any wrap/native-window math, so the whole 360x240
// canvas pans as one rigid unit instead of the native window and its margins
// drifting apart. Zero (default) leaves rendering byte-for-byte unchanged.
// The narrow (240x160 / non-wide) renderer never reads these.
extern "C" int g_ws_view_shift_x = 0;
extern "C" int g_ws_view_shift_y = 0;
extern "C" int (*g_ws_obj_x_provider)(int, int*) = nullptr;
extern "C" int (*g_ws_obj_y_provider)(int, int*) = nullptr;
extern "C" int (*g_ws_obj_attr_x_provider)(int, std::uint16_t,
                                            std::uint16_t, std::uint16_t,
                                            int*) = nullptr;
extern "C" int (*g_ws_obj_attr_y_provider)(int, std::uint16_t,
                                            std::uint16_t, std::uint16_t,
                                            int*) = nullptr;
extern "C" unsigned (*g_ws_margin_policy)(uint16_t, const uint8_t*) = nullptr;
extern "C" WsMarginDiagnosticsCallback g_ws_margin_diagnostics = nullptr;

// See gba_ppu.h: live BG-layer visibility toggle set by the in-game F1 menu.
extern "C" int (*g_ws_obj_park_provider)(int, std::uint16_t,
                                          std::uint16_t,
                                          std::uint16_t) = nullptr;
extern "C" bool g_hide_bg0 = false;
extern "C" bool g_hide_bg1 = false;
extern "C" bool g_hide_bg2 = false;
extern "C" bool g_hide_bg3 = false;

GbaPpu::GbaPpu()  = default;
GbaPpu::~GbaPpu() = default;

void GbaPpu::serialize(gbarecomp::debug::SnapshotWriter& w) const {
    w.u32(scanline_);
    w.u32(dot_in_scanline_);
    w.u32(cycle_in_dot_);
    w.u16(vcount_);
    w.u64(frame_count_);
    w.boolean(has_latched_fb_);
    // Snapshot layout remains the fixed native 240x160 payload. At native width
    // retain the historical byte-for-byte path. A wide latch is row-major at
    // render_width(), so its first kFramebufferBytes are NOT the native image;
    // explicitly crop the authentic center instead. This also makes a wide save
    // useful when loaded later in faithful mode.
    if (!view_expanded()) {
        w.bytes(latched_fb_.data(), kFramebufferBytes);
    } else {
        std::array<uint8_t, kFramebufferBytes> center{};
        constexpr std::size_t kNativeStride = kScreenWidth * 3u;
        const std::size_t wide_stride = render_width() * 3u;
        const std::size_t center_x = extra_left_ * 3u;
        for (std::size_t y = 0; y < kScreenHeight; ++y) {
            std::memcpy(center.data() + y * kNativeStride,
                        latched_fb_.data() +
                            (y + extra_top_) * wide_stride + center_x,
                        kNativeStride);
        }
        w.bytes(center.data(), center.size());
    }
}

void GbaPpu::deserialize(gbarecomp::debug::SnapshotReader& r) {
    scanline_        = r.u32();
    dot_in_scanline_ = r.u32();
    cycle_in_dot_    = r.u32();
    vcount_          = r.u16();
    frame_count_     = r.u64();
    // Older snapshots do not carry the hidden affine accumulators. Discard
    // any cold-boot value and reconstruct from live BGxX/Y on the first
    // rendered line; subsequent writes and line advances are tracked.
    affine_ref_valid_ = false;
    affine_line_ref_valid_.fill(false);
    line_io_valid_.fill(false);
    latched_native_line_io_valid_.fill(false);
    latched_native_affine_line_ref_valid_.fill(false);
    wide_margin_diagnostics_ = {};
    wide_margin_diagnostics_callback_ = nullptr;
    wide_margin_diagnostics_active_ = false;
    // Native scene memory is a present-time cache, not part of the guest
    // snapshot payload. Never reuse it across a savestate restore.
    has_latched_native_state_ = false;
    const bool serialized_latch = r.boolean();
    if (!view_expanded()) {
        // Historical native-width restore path, deliberately unchanged.
        has_latched_fb_ = serialized_latch;
        r.bytes(latched_fb_.data(), kFramebufferBytes);
    } else {
        // The fixed-size payload has no authored wide margins. Consume it to
        // preserve snapshot framing, then invalidate only the presentation
        // latch; the next present renders a fresh wide frame from restored GBA
        // state. Device timing/frame counters above remain fully restored.
        std::array<uint8_t, kFramebufferBytes> native_latch{};
        r.bytes(native_latch.data(), native_latch.size());
        has_latched_fb_ = false;
        // A host may re-present immediately after load, before a game-owned
        // state-epoch hook has invalidated/rebuilt its margin caches. Fail those
        // unauthored margins closed for that interim render; the game's normal
        // publish policy reauthorizes them once restored-state data is ready.
        g_ws_pillarbox = g_ws_authored_margin_layers ? 0 : 1;
        g_ws_pillarbox_left = 0;
        g_ws_pillarbox_right = 0;
        g_ws_pillarbox_top = 0;
        g_ws_pillarbox_bottom = 0;
    }
}

void GbaPpu::reset() {
    scanline_ = 0;
    dot_in_scanline_ = 0;
    cycle_in_dot_ = 0;
    vcount_ = 0;
    frame_count_ = 0;
    has_latched_fb_ = false;
    has_latched_native_state_ = false;
    affine_ref_valid_ = false;
    affine_line_ref_valid_.fill(false);
    line_io_valid_.fill(false);
    latched_native_line_io_valid_.fill(false);
    latched_native_affine_line_ref_valid_.fill(false);
    wide_margin_diagnostics_ = {};
    wide_margin_diagnostics_callback_ = nullptr;
    wide_margin_diagnostics_active_ = false;
    std::memset(latched_fb_.data(), 0xFF, latched_fb_.size());
}

void GbaPpu::reload_affine_reference(uint32_t layer, bool y_coord,
                                     uint32_t raw_value) {
    if (layer < 2 || layer > 3) return;
    raw_value &= 0x0FFFFFFFu;
    if (raw_value & 0x08000000u) raw_value |= 0xF0000000u;
    affine_ref_[layer - 2][y_coord ? 1 : 0] =
        static_cast<int32_t>(raw_value);
    affine_ref_valid_ = true;
}

void GbaPpu::reload_affine_references(const uint8_t* io) {
    if (!io) return;
    auto read_ref = [](const uint8_t* p) {
        uint32_t value = static_cast<uint32_t>(p[0]) |
                         (static_cast<uint32_t>(p[1]) << 8) |
                         (static_cast<uint32_t>(p[2]) << 16) |
                         (static_cast<uint32_t>(p[3]) << 24);
        value &= 0x0FFFFFFFu;
        if (value & 0x08000000u) value |= 0xF0000000u;
        return static_cast<int32_t>(value);
    };
    for (uint32_t bg = 0; bg < 2; ++bg) {
        const uint32_t off = 0x28u + bg * 0x10u;
        affine_ref_[bg][0] = read_ref(io + off);
        affine_ref_[bg][1] = read_ref(io + off + 4u);
    }
    affine_ref_valid_ = true;
}

void GbaPpu::serialize_affine_state(
        gbarecomp::debug::SnapshotWriter& w) const {
    w.boolean(affine_ref_valid_);
    for (const auto& bg : affine_ref_)
        for (int32_t value : bg) w.u32(static_cast<uint32_t>(value));
    for (uint32_t y = 0; y < kScreenHeight; ++y) {
        w.boolean(affine_line_ref_valid_[y]);
        for (const auto& bg : affine_line_ref_[y])
            for (int32_t value : bg) w.u32(static_cast<uint32_t>(value));
    }
}

void GbaPpu::deserialize_affine_state(
        gbarecomp::debug::SnapshotReader& r) {
    affine_ref_valid_ = r.boolean();
    for (auto& bg : affine_ref_)
        for (int32_t& value : bg) value = static_cast<int32_t>(r.u32());
    for (uint32_t y = 0; y < kScreenHeight; ++y) {
        affine_line_ref_valid_[y] = r.boolean();
        for (auto& bg : affine_line_ref_[y])
            for (int32_t& value : bg) value = static_cast<int32_t>(r.u32());
    }
}

void GbaPpu::set_view_margins(uint32_t left, uint32_t right,
                              uint32_t top, uint32_t bottom) {
    // Clamp each side to its compile-time max. Top/bottom use the same
    // present-time host state as horizontal margins; no guest timing state is
    // changed by resizing the presentation surface.
    const uint32_t next_left = left > kMaxExtraX ? kMaxExtraX : left;
    const uint32_t next_right = right > kMaxExtraX ? kMaxExtraX : right;
    const uint32_t next_top = top > kMaxExtraY ? kMaxExtraY : top;
    const uint32_t next_bottom = bottom > kMaxExtraY ? kMaxExtraY : bottom;
    if (next_left != extra_left_ || next_right != extra_right_ ||
        next_top != extra_top_ || next_bottom != extra_bottom_) {
        // A latched frame is laid out at the old stride. Force one fresh
        // render after a live resize rather than interpreting it at the new
        // dimensions. Timing state and guest memory remain untouched.
        has_latched_fb_ = false;
        wide_margin_diagnostics_ = {};
        wide_margin_diagnostics_callback_ = nullptr;
        wide_margin_diagnostics_active_ = false;
    }
    extra_left_ = next_left;
    extra_right_ = next_right;
    extra_top_ = next_top;
    extra_bottom_ = next_bottom;
}

GbaPpu::TickEvents GbaPpu::tick(uint32_t cycles, uint16_t vcount_compare) {
    // Coarse but correct: walk dot-by-dot, advancing scanline counters
    // as we cross dot boundaries. The cycle_in_dot_ accumulator lets
    // sub-dot ticks (less than 4 cycles) stash residue for the next
    // call.
    TickEvents ev{};
    cycle_in_dot_ += cycles;
    while (cycle_in_dot_ >= kCyclesPerDot) {
        cycle_in_dot_ -= kCyclesPerDot;
        bool was_visible = (dot_in_scanline_ < kDotsVisible);
        ++dot_in_scanline_;
        if (was_visible && dot_in_scanline_ == kDotsVisible) {
            ev.hblank_started = true;
        }
        if (dot_in_scanline_ >= kDotsPerScanline) {
            dot_in_scanline_ = 0;
            bool was_in_visible_window = (scanline_ < kLinesVisible);
            ++scanline_;
            if (scanline_ >= kLinesTotal) {
                scanline_ = 0;
                ++frame_count_;
                ev.frame_completed = true;
            }
            vcount_ = static_cast<uint16_t>(scanline_);
            if (was_in_visible_window && scanline_ == kLinesVisible) {
                ev.vblank_started = true;
            }
            if (vcount_ == vcount_compare) {
                ev.vcount_matched = true;
            }
        }
    }
    return ev;
}

uint32_t GbaPpu::cycles_until_next_event() const {
    uint32_t target_dot = (dot_in_scanline_ < kDotsVisible)
        ? kDotsVisible
        : kDotsPerScanline;
    uint32_t cycles = (target_dot - dot_in_scanline_) * kCyclesPerDot;
    if (cycles > cycle_in_dot_) cycles -= cycle_in_dot_;
    else cycles = 1;
    return cycles ? cycles : 1;
}

// ─────────────────────────────────────────────────────────────────────
// Renderer (Phase 2.4)
// ─────────────────────────────────────────────────────────────────────
//
// Scope: enough to draw the GBA BIOS boot intro. The BIOS intro uses
// DISPCNT = 0x1002 (BG mode 2, OBJ enabled, all BGs disabled), so we
// only need OBJ (sprite) compositing for this milestone.
//
// References: GBATEK § "GBA OBJs - OAM", § "GBA Palettes",
//             § "GBA VRAM Character Data".

namespace {

// Sprite shape × size → (width, height) in pixels.
//   shape: 0=square, 1=horizontal, 2=vertical
//   size:  0..3
constexpr int kSpriteWH[3][4][2] = {
    // square
    {{8, 8}, {16, 16}, {32, 32}, {64, 64}},
    // horizontal
    {{16, 8}, {32, 8}, {32, 16}, {64, 32}},
    // vertical
    {{8, 16}, {8, 32}, {16, 32}, {32, 64}},
};

// Convert 16-bit GBA color (0BBBBBGGGGGRRRRR) to 24-bit RGB888.
inline void to_rgb888(uint16_t c, uint8_t* out) {
    // 5 → 8 bit expansion: ((v << 3) | (v >> 2)) gives full-range 0..255.
    uint8_t r = (c >>  0) & 0x1F;
    uint8_t g = (c >>  5) & 0x1F;
    uint8_t b = (c >> 10) & 0x1F;
    out[0] = static_cast<uint8_t>((r << 3) | (r >> 2));
    out[1] = static_cast<uint8_t>((g << 3) | (g >> 2));
    out[2] = static_cast<uint8_t>((b << 3) | (b >> 2));
}

inline uint16_t blend_alpha_gba555(uint16_t top,
                                   uint16_t bottom,
                                   uint32_t eva,
                                   uint32_t evb) {
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;
    const uint32_t tr = top & 31u;
    const uint32_t tg = ((top >> 4) & 62u) | (top >> 15);
    const uint32_t tb = (top >> 10) & 31u;
    const uint32_t br = bottom & 31u;
    const uint32_t bg = ((bottom >> 4) & 62u) | (bottom >> 15);
    const uint32_t bb = (bottom >> 10) & 31u;

    // The GBA blends in its native color domain, rounds to nearest (+8 before
    // the 4-bit division), and carries a sixth green precision bit in palette
    // bit 15. Only after blending is green reduced to the displayed 5 bits.
    // Doing this on already-expanded RGB888 values produces subtly wrong colors.
    const uint32_t r = std::min(31u, (tr * eva + br * evb + 8u) >> 4);
    const uint32_t g6 = std::min(63u, (tg * eva + bg * evb + 8u) >> 4);
    const uint32_t b = std::min(31u, (tb * eva + bb * evb + 8u) >> 4);
    return static_cast<uint16_t>((b << 10) | ((g6 >> 1) << 5) | r);
}

inline uint16_t brighten_gba555(uint16_t color, uint32_t evy) {
    if (evy > 16) evy = 16;
    uint32_t r = color & 31u;
    uint32_t g6 = ((color >> 4) & 62u) | (color >> 15);
    uint32_t b = (color >> 10) & 31u;
    r += ((31u - r) * evy + 8u) >> 4;
    g6 += ((63u - g6) * evy + 8u) >> 4;
    b += ((31u - b) * evy + 8u) >> 4;
    return static_cast<uint16_t>((b << 10) | ((g6 >> 1) << 5) | r);
}

inline uint16_t darken_gba555(uint16_t color, uint32_t evy) {
    if (evy > 16) evy = 16;
    uint32_t r = color & 31u;
    uint32_t g6 = ((color >> 4) & 62u) | (color >> 15);
    uint32_t b = (color >> 10) & 31u;
    // Hardware rounds the subtractive term with +7 rather than +8.
    r -= (r * evy + 7u) >> 4;
    g6 -= (g6 * evy + 7u) >> 4;
    b -= (b * evy + 7u) >> 4;
    return static_cast<uint16_t>((b << 10) | ((g6 >> 1) << 5) | r);
}

inline uint16_t load_u16_le(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

}  // namespace

// Read a signed 16-bit Q8.8 affine parameter from IO.
static inline int32_t read_s16(const uint8_t* io, uint32_t off) {
    int16_t v = static_cast<int16_t>(
        io[off] | (io[off + 1] << 8));
    return static_cast<int32_t>(v);
}

// Read a signed 28-bit Q19.8 reference coord from IO (BGxX/BGxY).
// Bits 27..0 are the value; the field is sign-extended from bit 27.
static inline int32_t read_s28_ref(const uint8_t* io, uint32_t off) {
    uint32_t v = static_cast<uint32_t>(io[off]) |
                 (static_cast<uint32_t>(io[off + 1]) <<  8) |
                 (static_cast<uint32_t>(io[off + 2]) << 16) |
                 (static_cast<uint32_t>(io[off + 3]) << 24);
    v &= 0x0FFFFFFFu;
    if (v & 0x08000000u) v |= 0xF0000000u;
    return static_cast<int32_t>(v);
}

namespace {

bool obj_texel_opaque(const uint8_t* vram,
                      uint32_t obj_tile_base,
                      bool obj_1d_mapping,
                      uint32_t tile_num,
                      int tiles_w,
                      bool color256,
                      int tex_x,
                      int tex_y) {
    int tile_x_in_sprite = tex_x >> 3;
    int tile_y_in_sprite = tex_y >> 3;
    int px_in_tile       = tex_x & 7;
    int py_in_tile       = tex_y & 7;

    uint32_t this_tile;
    if (obj_1d_mapping) {
        this_tile = tile_num + (tile_y_in_sprite * tiles_w + tile_x_in_sprite) *
                                (color256 ? 2u : 1u);
    } else {
        this_tile = tile_num + (tile_y_in_sprite * 32u) +
                    tile_x_in_sprite * (color256 ? 2u : 1u);
    }
    uint32_t tile_off = obj_tile_base + this_tile * 32u;
    if (color256) {
        uint32_t off = tile_off + py_in_tile * 8 + px_in_tile;
        if (off + 1 > 96u * 1024u) return false;
        return vram[off] != 0;
    }
    uint32_t off = tile_off + py_in_tile * 4 + (px_in_tile / 2);
    if (off + 1 > 96u * 1024u) return false;
    uint8_t b = vram[off];
    uint8_t pal_index = (px_in_tile & 1) ? (b >> 4) : (b & 0x0F);
    return pal_index != 0;
}

void mark_obj_window_scanline(bool* mask,
                              int y,
                              uint16_t dispcnt,
                              const uint8_t* vram,
                              const uint8_t* oam,
                              uint32_t kScreenWidth,
                              uint32_t kScreenHeight) {
    if (y < 0 || y >= static_cast<int>(kScreenHeight)) return;
    if ((dispcnt & 0x8000u) == 0) return;

    uint32_t bg_mode = dispcnt & 0x07u;
    uint32_t obj_tile_base = (bg_mode >= 3) ? 0x14000u : 0x10000u;
    bool obj_1d_mapping = (dispcnt & 0x0040u) != 0;

    for (int idx = 127; idx >= 0; --idx) {
        const uint8_t* entry = oam + idx * 8;
        uint16_t attr0 = load_u16_le(&entry[0]);
        uint16_t attr1 = load_u16_le(&entry[2]);
        uint16_t attr2 = load_u16_le(&entry[4]);

        bool rot_scale = (attr0 & 0x0100u) != 0;
        bool disable_or_double = (attr0 & 0x0200u) != 0;
        if (!rot_scale && disable_or_double) continue;
        uint32_t obj_mode = (attr0 >> 10) & 0x3u;
        if (obj_mode != 2) continue;

        uint32_t shape = (attr0 >> 14) & 0x3u;
        if (shape >= 3) continue;
        uint32_t size  = (attr1 >> 14) & 0x3u;
        int sw = kSpriteWH[shape][size][0];
        int sh = kSpriteWH[shape][size][1];

        int sy = static_cast<int>(attr0 & 0xFFu);
        int sx = static_cast<int>(attr1 & 0x1FFu);
        if (sy >= 160) sy -= 256;
        if (sx & 0x100) sx -= 0x200;

        bool color256 = (attr0 & 0x2000u) != 0;
        uint32_t tile_num = attr2 & 0x3FFu;
        int tiles_w = sw / 8;
        int tiles_h = sh / 8;

        if (rot_scale) {
            int bw = disable_or_double ? sw * 2 : sw;
            int bh = disable_or_double ? sh * 2 : sh;
            int j = static_cast<int>(y) - sy;
            if (j < 0 || j >= bh) continue;

            int affine_group = (attr1 >> 9) & 0x1Fu;
            const uint8_t* ag = oam + affine_group * 0x20u;
            int32_t pa = read_s16(ag, 0x06);
            int32_t pb = read_s16(ag, 0x0E);
            int32_t pc = read_s16(ag, 0x16);
            int32_t pd = read_s16(ag, 0x1E);

            int half_bw = bw >> 1;
            int half_bh = bh >> 1;
            int half_sw = sw >> 1;
            int half_sh = sh >> 1;
            int dy = j - half_bh;
            for (int i = 0; i < bw; ++i) {
                int screen_x = sx + i;
                if (screen_x < 0 || screen_x >= static_cast<int>(kScreenWidth)) continue;
                int dx = i - half_bw;
                int tex_x = ((pa * dx + pb * dy) >> 8) + half_sw;
                int tex_y = ((pc * dx + pd * dy) >> 8) + half_sh;
                if (tex_x < 0 || tex_x >= sw) continue;
                if (tex_y < 0 || tex_y >= sh) continue;
                if (obj_texel_opaque(vram, obj_tile_base, obj_1d_mapping,
                                     tile_num, tiles_w, color256,
                                     tex_x, tex_y)) {
                    mask[screen_x] = true;
                }
            }
            continue;
        }

        int line = static_cast<int>(y) - sy;
        if (line < 0 || line >= sh) continue;
        bool hflip = (attr1 & 0x1000u) != 0;
        bool vflip = (attr1 & 0x2000u) != 0;
        int ty = line >> 3;
        int py = line & 7;
        int src_ty = vflip ? (tiles_h - 1 - ty) : ty;
        int src_py = vflip ? (7 - py) : py;
        for (int tx = 0; tx < tiles_w; ++tx) {
            int src_tx = hflip ? (tiles_w - 1 - tx) : tx;
            for (int px = 0; px < 8; ++px) {
                int screen_x = sx + tx * 8 + px;
                if (screen_x < 0 || screen_x >= static_cast<int>(kScreenWidth)) continue;
                int src_px = hflip ? (7 - px) : px;
                int tex_x = src_tx * 8 + src_px;
                int tex_y = src_ty * 8 + src_py;
                if (obj_texel_opaque(vram, obj_tile_base, obj_1d_mapping,
                                     tile_num, tiles_w, color256,
                                     tex_x, tex_y)) {
                    mask[screen_x] = true;
                }
            }
        }
    }
}

// Debug view aid (in-game F1 menu's "Hide BGn" checkboxes, Enhancements >
// Visual > Layers): render-only suppression of a regular/affine BG layer,
// forced fully transparent so the compositor's backdrop/OBJ/window/blend
// behavior is otherwise untouched. Backed by g_hide_bg0..g_hide_bg3 (see
// gba_ppu.h), which the config UI sets live while the game runs. Purely
// presentational: it never touches guest memory or DISPCNT.
bool hide_bg_layer(uint32_t layer) {
    switch (layer) {
        case 0: return g_hide_bg0;
        case 1: return g_hide_bg1;
        case 2: return g_hide_bg2;
        case 3: return g_hide_bg3;
        default: return false;
    }
}

void render_scanline_internal(uint8_t* rgb,
                              uint32_t y,
                              uint16_t dispcnt,
                              const uint8_t* io,
                              const uint8_t* vram,
                              const uint8_t* oam,
                              const uint8_t* pal,
                              uint32_t kScreenWidth,
                              uint32_t kScreenHeight,
                              const int32_t* affine_refs) {
    if (y >= kScreenHeight) return;

    uint8_t* row = rgb + y * kScreenWidth * 3;
    if (dispcnt & 0x0080u) {
        std::memset(row, 0xFF, kScreenWidth * 3);
        return;
    }
    if (g_ws_field_tilemap_source && g_ws_field_tilemap_source_begin)
        g_ws_field_tilemap_source_begin(static_cast<int>(y), dispcnt, io);

    struct PixelCandidate {
        uint8_t rgb[3] = {0, 0, 0};
        uint16_t color = 0;
        int key = 0x7FFFFFFF;
        uint8_t layer = 5;
        bool target1 = false;
        bool target2 = false;
        bool valid = false;
        bool semi = false;   // a semi-transparent OBJ (OAM mode 1)
    };

    // ── Windows (GBATEK "LCD I/O Windows") ──────────────────────────────
    // window_control(x) returns the 6-bit per-pixel mask (BG0-3, OBJ, and bit5
    // = color-special-effect enable) chosen by region, priority WIN0 > WIN1 >
    // OBJ-window > outside. WIN0/WIN1 are COORDINATE windows: WINnH gives
    // [X1,X2), WINnV gives [Y1,Y2). The per-scanline WIN0H the game rewrites
    // each line is what carves the circular IRIS used by room transitions — it
    // blanks everything outside the circle to the backdrop, hiding the room's
    // VRAM/palette reload (MC-HP-003). With no window enabled, every layer is on.
    const bool win0_en   = (dispcnt & 0x2000u) != 0;
    const bool win1_en   = (dispcnt & 0x4000u) != 0;
    const bool objwin_en = (dispcnt & 0x8000u) != 0;
    const bool any_window = win0_en || win1_en || objwin_en;
    uint16_t winin  = static_cast<uint16_t>(io[0x48] | (io[0x49] << 8));
    uint16_t winout = static_cast<uint16_t>(io[0x4A] | (io[0x4B] << 8));
    // y within WINnV [Y1,Y2)? GBATEK: Y2>height or Y1>Y2 → Y2=height.
    auto win_v_row = [&](uint32_t vreg) -> bool {
        uint32_t v  = static_cast<uint32_t>(io[vreg] | (io[vreg + 1] << 8));
        uint32_t y1 = (v >> 8) & 0xFFu, y2 = v & 0xFFu;
        if (y2 > kScreenHeight || y1 > y2) y2 = kScreenHeight;
        return y >= y1 && y < y2;
    };
    // x within WINnH [X1,X2)? GBATEK: X2>width or X1>X2 → X2=width.
    auto win_h_in = [&](uint32_t hreg, uint32_t x) -> bool {
        uint32_t h  = static_cast<uint32_t>(io[hreg] | (io[hreg + 1] << 8));
        uint32_t x1 = (h >> 8) & 0xFFu, x2 = h & 0xFFu;
        if (x2 > kScreenWidth || x1 > x2) x2 = kScreenWidth;
        return x >= x1 && x < x2;
    };
    const bool win0_row = win0_en && win_v_row(0x44);
    const bool win1_row = win1_en && win_v_row(0x46);
    bool obj_window_storage[GbaPpu::kScreenWidth] = {};
    bool* obj_window_mask = nullptr;
    if (objwin_en) {
        mark_obj_window_scanline(obj_window_storage, y, dispcnt, vram, oam,
                                 kScreenWidth, kScreenHeight);
        obj_window_mask = obj_window_storage;
    }
    auto window_control = [&](uint32_t x) -> uint16_t {
        if (!any_window) return 0x3Fu;
        if (win0_row && win_h_in(0x40, x)) return winin & 0x3Fu;
        if (win1_row && win_h_in(0x42, x)) return (winin >> 8) & 0x3Fu;
        if (obj_window_mask && obj_window_mask[x])
            return static_cast<uint16_t>((winout >> 8) & 0x3Fu);
        return static_cast<uint16_t>(winout & 0x3Fu);
    };
    auto layer_enabled = [&](uint32_t x, uint32_t layer_bit) -> bool {
        return (window_control(x) & (1u << layer_bit)) != 0;
    };
    auto blend_enabled = [&](uint32_t x) -> bool {
        return (window_control(x) & (1u << 5)) != 0;
    };

    uint16_t bldcnt = static_cast<uint16_t>(io[0x50] | (io[0x51] << 8));
    uint16_t bldalpha = static_cast<uint16_t>(io[0x52] | (io[0x53] << 8));
    uint32_t first_targets = bldcnt & 0x3Fu;
    uint32_t second_targets = (bldcnt >> 8) & 0x3Fu;
    uint32_t effect = (bldcnt >> 6) & 0x3u;

    PixelCandidate top[GbaPpu::kScreenWidth];
    PixelCandidate second[GbaPpu::kScreenWidth];
    const uint16_t backdrop_color = load_u16_le(&pal[0]);
    uint8_t backdrop_rgb[3];
    to_rgb888(backdrop_color, backdrop_rgb);
    for (uint32_t x = 0; x < kScreenWidth; ++x) {
        top[x].rgb[0] = backdrop_rgb[0];
        top[x].rgb[1] = backdrop_rgb[1];
        top[x].rgb[2] = backdrop_rgb[2];
        top[x].color = backdrop_color;
        top[x].key = 0x70000000;
        top[x].layer = 5;
        top[x].target1 = blend_enabled(x) && ((first_targets & (1u << 5)) != 0);
        top[x].target2 = (second_targets & (1u << 5)) != 0;
        top[x].valid = true;
    }
    auto submit = [&](uint32_t x,
                      uint16_t color,
                      int key,
                      uint8_t layer,
                      bool target1,
                      bool target2,
                      bool semi = false) {
        PixelCandidate cand;
        cand.color = color;
        to_rgb888(color, cand.rgb);
        cand.key = key;
        cand.layer = layer;
        cand.target1 = target1;
        cand.target2 = target2;
        cand.valid = true;
        cand.semi = semi;
        // Sprites are ONE layer on hardware: at each pixel only the frontmost
        // sprite takes part, and a sprite never becomes the layer below
        // another sprite. Keeping the sprite behind as `second` made a
        // semi-transparent sprite find no blend partner over another sprite
        // and fall through to the brightness effect -- Golden Sun's defend
        // flash (BLDCNT 0x3F90, BLDY 16) then painted every sprite overlap
        // white (gpu_rewind_0050, FACTS.md 2026-09-25).
        if (layer == 4) {
            if (top[x].layer == 4) {
                if (key < top[x].key) top[x] = cand;
                return;
            }
            if (second[x].valid && second[x].layer == 4) {
                if (key >= second[x].key) return;
                if (key < top[x].key) { second[x] = top[x]; top[x] = cand; }
                else second[x] = cand;
                return;
            }
        }
        if (key < top[x].key) {
            second[x] = top[x];
            top[x] = cand;
        } else if (key < second[x].key) {
            second[x] = cand;
        }
    };

    uint32_t bg_mode = dispcnt & 0x07u;
    auto render_regular_bg = [&](uint32_t layer,
                                 uint32_t cnt_off,
                                 uint32_t scroll_off) {
        if ((dispcnt & (0x0100u << layer)) == 0) return;
        if (hide_bg_layer(layer)) return;

        uint16_t bgcnt = static_cast<uint16_t>(
            io[cnt_off] | (io[cnt_off + 1] << 8));
        uint32_t char_base = ((bgcnt >> 2) & 0x3u) * 0x4000u;
        uint32_t screen_base = ((bgcnt >> 8) & 0x1Fu) * 0x800u;
        bool color256 = (bgcnt & 0x0080u) != 0;
        uint32_t size_code = (bgcnt >> 14) & 0x3u;
        uint32_t bg_priority = bgcnt & 0x3u;
        uint32_t hofs = static_cast<uint16_t>(
            io[scroll_off] | (io[scroll_off + 1] << 8)) & 0x01FFu;
        uint32_t vofs = static_cast<uint16_t>(
            io[scroll_off + 2] | (io[scroll_off + 3] << 8)) & 0x01FFu;

        uint32_t width_tiles = (size_code & 1u) ? 64u : 32u;
        uint32_t height_tiles = (size_code & 2u) ? 64u : 32u;
        uint32_t width_px = width_tiles * 8u;
        uint32_t height_px = height_tiles * 8u;
        uint32_t block_cols = width_tiles / 32u;

        for (uint32_t x = 0; x < kScreenWidth; ++x) {
            if (!layer_enabled(x, layer)) continue;
            uint32_t tex_x = (x + hofs) & (width_px - 1u);
            uint32_t tex_y = (y + vofs) & (height_px - 1u);
            uint32_t tile_x = tex_x >> 3;
            uint32_t tile_y = tex_y >> 3;
            uint32_t block = (tile_x >> 5) + (tile_y >> 5) * block_cols;
            uint32_t map_off = screen_base + block * 0x800u +
                ((tile_y & 31u) * 32u + (tile_x & 31u)) * 2u;
            if (map_off + 1 >= 96u * 1024u) continue;

            uint16_t entry = load_u16_le(&vram[map_off]);
            if (g_ws_field_tilemap_source) {
                uint16_t supplied = 0;
                if (g_ws_field_tilemap_source(static_cast<int>(layer),
                                              static_cast<int>(x),
                                              static_cast<int>(y), &supplied)) {
                    entry = supplied;
                }
            }
            uint32_t tile_num = entry & 0x03FFu;
            bool hflip = (entry & 0x0400u) != 0;
            bool vflip = (entry & 0x0800u) != 0;
            uint32_t palette_bank = (entry >> 12) & 0x0Fu;
            uint32_t px = tex_x & 7u;
            uint32_t py = tex_y & 7u;
            if (hflip) px = 7u - px;
            if (vflip) py = 7u - py;

            uint8_t pal_idx = 0;
            if (color256) {
                uint32_t tile_addr = char_base + tile_num * 64u + py * 8u + px;
                if (tile_addr >= 96u * 1024u) continue;
                pal_idx = vram[tile_addr];
            } else {
                uint32_t tile_addr = char_base + tile_num * 32u +
                    py * 4u + (px >> 1);
                if (tile_addr >= 96u * 1024u) continue;
                uint8_t packed = vram[tile_addr];
                pal_idx = (px & 1u) ? (packed >> 4) : (packed & 0x0Fu);
                pal_idx = static_cast<uint8_t>(
                    pal_idx | static_cast<uint8_t>(palette_bank << 4));
            }
            if ((pal_idx & (color256 ? 0xFFu : 0x0Fu)) == 0) continue;

            const uint16_t color = load_u16_le(&pal[pal_idx * 2]);
            submit(x, color,
                   static_cast<int>(bg_priority * 256u + 128u + layer),
                   static_cast<uint8_t>(layer),
                   blend_enabled(x) && ((first_targets & (1u << layer)) != 0),
                   (second_targets & (1u << layer)) != 0);
        }
    };

    auto render_affine_bg = [&](uint32_t layer,
                                uint32_t cnt_off,
                                uint32_t param_off) {
        if ((dispcnt & (0x0100u << layer)) == 0) return;
        if (hide_bg_layer(layer)) return;

        uint16_t bgcnt = static_cast<uint16_t>(io[cnt_off] |
                                               (io[cnt_off + 1] << 8));
        uint32_t char_base   = ((bgcnt >> 2) & 0x3u) * 0x4000u;
        uint32_t screen_base = ((bgcnt >> 8) & 0x1Fu) * 0x800u;
        bool wrap            = (bgcnt & 0x2000u) != 0;
        uint32_t size_code   = (bgcnt >> 14) & 0x3u;
        int bg_pixels = 128 << size_code;
        int bg_tiles  = bg_pixels / 8;
        int bg_priority = static_cast<int>(bgcnt & 0x3u);

        int32_t pa = read_s16(io, param_off + 0x00);
        int32_t pb = read_s16(io, param_off + 0x02);
        int32_t pc = read_s16(io, param_off + 0x04);
        int32_t pd = read_s16(io, param_off + 0x06);
        const uint32_t ref_index = (layer - 2u) * 2u;
        int32_t refx = affine_refs ? affine_refs[ref_index]
                                   : read_s28_ref(io, param_off + 0x08);
        int32_t refy = affine_refs ? affine_refs[ref_index + 1u]
                                   : read_s28_ref(io, param_off + 0x0C);
        int32_t xt = refx + (affine_refs ? 0 : static_cast<int32_t>(y) * pb);
        int32_t yt = refy + (affine_refs ? 0 : static_cast<int32_t>(y) * pd);
        for (uint32_t x = 0; x < kScreenWidth; ++x) {
            int32_t tex_x = xt >> 8;
            int32_t tex_y = yt >> 8;
            xt += pa;
            yt += pc;
            if (!layer_enabled(x, layer)) continue;
            if (wrap) {
                tex_x &= (bg_pixels - 1);
                tex_y &= (bg_pixels - 1);
            } else if (tex_x < 0 || tex_x >= bg_pixels ||
                       tex_y < 0 || tex_y >= bg_pixels) {
                continue;
            }
            uint32_t map_off = screen_base + (tex_y >> 3) * bg_tiles + (tex_x >> 3);
            if (map_off >= 96u * 1024u) continue;
            uint8_t tile_index = vram[map_off];
            uint32_t tile_addr = char_base + tile_index * 64u +
                                 (tex_y & 7) * 8 + (tex_x & 7);
            if (tile_addr >= 96u * 1024u) continue;
            uint8_t pal_idx = vram[tile_addr];
            if (pal_idx == 0) continue;
            const uint16_t color = load_u16_le(&pal[pal_idx * 2]);
            submit(x, color,
                   static_cast<int>(bg_priority * 256 + 128 + layer),
                   static_cast<uint8_t>(layer),
                   blend_enabled(x) && ((first_targets & (1u << layer)) != 0),
                   (second_targets & (1u << layer)) != 0);
        }
    };

    if (bg_mode == 0) {
        render_regular_bg(3, 0x0E, 0x1C);
        render_regular_bg(2, 0x0C, 0x18);
        render_regular_bg(1, 0x0A, 0x14);
        render_regular_bg(0, 0x08, 0x10);
    } else if (bg_mode == 1) {
        render_affine_bg(2, 0x0C, 0x20);
        render_regular_bg(1, 0x0A, 0x14);
        render_regular_bg(0, 0x08, 0x10);
    } else if (bg_mode == 2) {
        render_affine_bg(3, 0x0E, 0x30);
        render_affine_bg(2, 0x0C, 0x20);
    }

    if (dispcnt & 0x1000u) {
        uint32_t obj_tile_base = (bg_mode >= 3) ? 0x14000u : 0x10000u;
        bool obj_1d_mapping = (dispcnt & 0x0040u) != 0;
        const uint8_t* obj_pal = pal + 0x200;
        for (int idx = 127; idx >= 0; --idx) {
            const uint8_t* entry = oam + idx * 8;
            uint16_t attr0 = load_u16_le(&entry[0]);
            uint16_t attr1 = load_u16_le(&entry[2]);
            uint16_t attr2 = load_u16_le(&entry[4]);
            bool rot_scale = (attr0 & 0x0100u) != 0;
            bool disable_or_double = (attr0 & 0x0200u) != 0;
            if (!rot_scale && disable_or_double) continue;
            uint32_t obj_mode = (attr0 >> 10) & 0x3u;
            if (obj_mode == 2 || obj_mode == 3) continue;
            uint32_t shape = (attr0 >> 14) & 0x3u;
            if (shape >= 3) continue;
            uint32_t size  = (attr1 >> 14) & 0x3u;
            int sw = kSpriteWH[shape][size][0];
            int sh = kSpriteWH[shape][size][1];
            const int raw_sy = static_cast<int>(attr0 & 0xFFu);
            const int raw_sx = static_cast<int>(attr1 & 0x1FFu);
            int sy = raw_sy;
            int sx = raw_sx;
            if (sy >= 160) sy -= 256;
            if (sx & 0x100) sx -= 0x200;
            bool color256 = (attr0 & 0x2000u) != 0;
            uint32_t tile_num = attr2 & 0x3FFu;
            uint32_t palette_bank = (attr2 >> 12) & 0xFu;
            int tiles_w = sw / 8;
            int tiles_h = sh / 8;
            int priority = static_cast<int>((attr2 >> 10) & 0x3u);
            // Composite key (lower = front). Per-priority stride 256 with OBJ in
            // [p*256, p*256+127] (tie-break by OAM idx) and BG in [p*256+128,
            // p*256+131] (tie-break by layer) yields the exact GBA order
            // OBJ0<BG0<OBJ1<BG1<OBJ2<BG2<OBJ3<BG3: an OBJ is in front of a same-
            // priority BG, but a BG of priority p sits in front of any OBJ of
            // priority p+1 (this is what lets the player walk BEHIND roof/tree
            // tops). The stride MUST exceed 128 so OBJ idx (0..127) can't bleed
            // into the next priority's BG band.
            int key = priority * 256 + idx;
            bool obj_target2 = (second_targets & (1u << 4)) != 0;
            auto emit_obj = [&](int tex_x, int tex_y, int screen_x) {
                if (screen_x < 0 || screen_x >= static_cast<int>(kScreenWidth)) return;
                if (!layer_enabled(static_cast<uint32_t>(screen_x), 4)) return;
                int tile_x_in_sprite = tex_x >> 3;
                int tile_y_in_sprite = tex_y >> 3;
                int px_in_tile = tex_x & 7;
                int py_in_tile = tex_y & 7;
                uint32_t this_tile;
                if (obj_1d_mapping) {
                    this_tile = tile_num + (tile_y_in_sprite * tiles_w + tile_x_in_sprite) *
                                            (color256 ? 2u : 1u);
                } else {
                    this_tile = tile_num + (tile_y_in_sprite * 32u) +
                                tile_x_in_sprite * (color256 ? 2u : 1u);
                }
                uint32_t tile_off = obj_tile_base + this_tile * 32u;
                uint8_t pal_index;
                if (color256) {
                    uint32_t off = tile_off + py_in_tile * 8 + px_in_tile;
                    if (off + 1 > 96u * 1024u) return;
                    pal_index = vram[off];
                    if (pal_index == 0) return;
                } else {
                    uint32_t off = tile_off + py_in_tile * 4 + (px_in_tile / 2);
                    if (off + 1 > 96u * 1024u) return;
                    uint8_t b = vram[off];
                    pal_index = (px_in_tile & 1) ? (b >> 4) : (b & 0x0F);
                    if (pal_index == 0) return;
                    pal_index = static_cast<uint8_t>(pal_index | (palette_bank << 4));
                }
                const uint16_t color = load_u16_le(&obj_pal[pal_index * 2]);
                uint32_t ux = static_cast<uint32_t>(screen_x);
                // First target: a semi-transparent OBJ always, any other
                // OBJ when BLDCNT selects the OBJ layer (GBATEK, "LCD I/O
                // Color Special Effects"). Golden Sun's defend flash
                // brightens the one non-semi-transparent party member this
                // way (BLDCNT 0x3F90, gpu_rewind_0051, FACTS.md 2026-09-25).
                bool t1 = blend_enabled(ux) &&
                    (obj_mode == 1 || (first_targets & (1u << 4)) != 0);
                submit(ux, color, key, 4, t1, obj_target2, obj_mode == 1);
            };
            if (rot_scale) {
                int bw = disable_or_double ? sw * 2 : sw;
                int bh = disable_or_double ? sh * 2 : sh;
                int j = static_cast<int>(y) - sy;
                if (j < 0 || j >= bh) continue;
                int affine_group = (attr1 >> 9) & 0x1Fu;
                const uint8_t* ag = oam + affine_group * 0x20u;
                int32_t pa = read_s16(ag, 0x06);
                int32_t pb = read_s16(ag, 0x0E);
                int32_t pc = read_s16(ag, 0x16);
                int32_t pd = read_s16(ag, 0x1E);
                int half_bw = bw >> 1;
                int half_bh = bh >> 1;
                int half_sw = sw >> 1;
                int half_sh = sh >> 1;
                int dy = j - half_bh;
                for (int i = 0; i < bw; ++i) {
                    int dx = i - half_bw;
                    int tex_x = ((pa * dx + pb * dy) >> 8) + half_sw;
                    int tex_y = ((pc * dx + pd * dy) >> 8) + half_sh;
                    if (tex_x < 0 || tex_x >= sw) continue;
                    if (tex_y < 0 || tex_y >= sh) continue;
                    emit_obj(tex_x, tex_y, sx + i);
                }
                continue;
            }
            int line = static_cast<int>(y) - sy;
            if (line < 0 || line >= sh) continue;
            bool hflip = (attr1 & 0x1000u) != 0;
            bool vflip = (attr1 & 0x2000u) != 0;
            int ty = line >> 3;
            int py = line & 7;
            int src_ty = vflip ? (tiles_h - 1 - ty) : ty;
            int src_py = vflip ? (7 - py) : py;
            for (int tx = 0; tx < tiles_w; ++tx) {
                int src_tx = hflip ? (tiles_w - 1 - tx) : tx;
                for (int px = 0; px < 8; ++px) {
                    int src_px = hflip ? (7 - px) : px;
                    emit_obj(src_tx * 8 + src_px, src_ty * 8 + src_py,
                             sx + tx * 8 + px);
                }
            }
        }
    }

    // BLDY brightness coefficient (clamped to 16/16 = full).
    uint32_t bldy = static_cast<uint32_t>(io[0x54] | (io[0x55] << 8)) & 0x1Fu;
    if (bldy > 16u) bldy = 16u;
    for (uint32_t x = 0; x < kScreenWidth; ++x) {
        uint8_t* dst = row + x * 3;
        // Alpha blend top (1st target) with the layer below (2nd target). Per
        // GBATEK this occurs when effect==1 OR top is a semi-transparent OBJ
        // (mode 1 forces alpha regardless of BLDCNT). NOT gated on EVA!=0:
        // EVA=0/EVB=16 is a valid blend (1st target fully fades into the 2nd) —
        // the Oak-intro character fade endpoint that previously snapped back to
        // opaque, leaving body/feet (BG2 + semi-transparent OBJ) out of sync.
        if ((effect == 1 || (top[x].layer == 4 && top[x].semi)) &&
            top[x].target1 && second[x].valid && second[x].target2 &&
            !(top[x].layer == 4 && second[x].layer == 4)) {
            const uint16_t blended = blend_alpha_gba555(
                top[x].color, second[x].color,
                bldalpha & 0x1Fu, (bldalpha >> 8) & 0x1Fu);
            to_rgb888(blended, dst);
        } else if ((effect == 2u || effect == 3u) && bldy != 0u && top[x].target1) {
            // Brightness, like alpha, operates on native GBA channels before
            // RGB888 expansion and observes palette bit 15 as green precision.
            const uint16_t adjusted = effect == 2u
                ? brighten_gba555(top[x].color, bldy)
                : darken_gba555(top[x].color, bldy);
            to_rgb888(adjusted, dst);
        } else {
            dst[0] = top[x].rgb[0];
            dst[1] = top[x].rgb[1];
            dst[2] = top[x].rgb[2];
        }
    }
    return;
}

// ── Wide (view-expanded) scanline compositor ────────────────────────────────
// Renders output scanline `y` into an `out_w`-pixel row, where output column x
// maps to hardware column hx = x - ox (ox = left margin). This path is ONLY
// entered when view-area expansion is active; the faithful build always takes
// render_scanline_internal above, so OFF-mode stays byte-identical by construc-
// tion. For central hx in [0,240) the per-pixel logic is the same as vanilla;
// margin columns extend the BG scroll / affine extrapolation / OBJ sampling into
// the surrounding map. WIN0/WIN1 register tests are NOT widened — a margin hx
// falls outside [X1,X2) so it naturally resolves to WINOUT (the conservative
// policy for columns the game never authored). OAM X keeps its 9-bit signed
// decode and is tested against the expanded viewport, so wrapped-negative sprites
// (left) and x>=240 sprites (right) both appear in the margins. `logical_y` is
// the authentic hardware coordinate; output_y includes the top host margin.
void render_scanline_wide(uint8_t* rgb, int logical_y, uint32_t output_y,
                          uint16_t dispcnt,
                          const uint8_t* io, const uint8_t* vram,
                          const uint8_t* oam, const uint8_t* pal,
                          uint32_t out_w, uint32_t ox,
                          uint32_t pixel_scale = 1,
                          uint8_t* row_override = nullptr,
                          const int32_t* affine_refs = nullptr,
                          int affine_ref_line = 0,
                          unsigned margin_policy_flags = 0,
                          WsMarginDiagnostics* diagnostics = nullptr,
                          // Expanded-view vertical geometry, for object
                          // culling. Defaults describe the native view.
                          uint32_t oy = 0,
                          uint32_t out_h = GbaPpu::kScreenHeight,
                          // Every authentic row's latched affine reference,
                          // four int32 per row (BG2 x/y, BG3 x/y), with its
                          // validity. Only a sample-remapped affine layer
                          // reads them: it draws one row's pixels from
                          // ANOTHER row, so it needs that row's own
                          // transform rather than this row's extrapolated.
                          // A game that latches per scanline -- Golden Sun's
                          // battle water does -- is torn into bands without
                          // this.
                          const int32_t* affine_line_refs = nullptr,
                          const bool* affine_line_refs_valid = nullptr,
                          // Every authentic row's latched IO, kLineIoBytes
                          // apart, with its validity. Same reason as the
                          // affine references above: a remapped sample reads
                          // a DIFFERENT row's pixel, so it must read that
                          // row's scroll and transform registers too. Golden
                          // Sun's battle arenas scroll per scanline (the heat
                          // and water shimmer), and using this row's values
                          // for another row's pixel slides each band
                          // sideways.
                          const uint8_t* line_io_table = nullptr,
                          const bool* line_io_valid = nullptr) {
    constexpr uint32_t kVanW = GbaPpu::kScreenWidth;   // 240
    constexpr uint32_t kVanH = GbaPpu::kScreenHeight;  // 160
    if (pixel_scale == 0) pixel_scale = 1;
    // Measurement-only gate (see gba_ppu.h): on for the whole of this call
    // only while it renders the frame's first logical scanline. Cleared on
    // every return path below, including the early return, so a miscounted
    // frame can never leave this stuck on.
    g_ws_obj_census_line = (logical_y == 0) ? 1 : 0;
    ++g_ws_expanded_diag[0];
    const int native_width = static_cast<int>(kVanW * pixel_scale);
    const int native_first = static_cast<int>(ox);
    const int native_last = native_first + native_width;
    auto logical_x = [&](uint32_t x) -> int {
        return static_cast<int>(x / pixel_scale) - static_cast<int>(ox);
    };
    const auto is_margin_x = [&](uint32_t x) {
        return static_cast<int>(x) < native_first ||
               static_cast<int>(x) >= native_last ||
               logical_y < 0 || logical_y >= static_cast<int>(kVanH);
    };
    const auto record_margin_final = [&](uint32_t x, std::size_t layer,
                                         std::size_t source) {
        if (!diagnostics || !is_margin_x(x) ||
            layer >= kWsMarginTraceLayerCount ||
            source >= kWsMarginTraceSourceCount) {
            return;
        }
        ++diagnostics->margin_pixels;
        ++diagnostics->final_selected[layer][source];
        const bool left = static_cast<int>(x) < native_first;
        const bool right = static_cast<int>(x) >= native_last;
        const bool top = logical_y < 0;
        const bool bottom = logical_y >= static_cast<int>(kVanH);
        if (left) {
            ++diagnostics->left_margin_pixels;
        } else if (right) {
            ++diagnostics->right_margin_pixels;
        }
        if (top) ++diagnostics->top_margin_pixels;
        if (bottom) ++diagnostics->bottom_margin_pixels;
        const std::size_t horizontal_side = left ? 0u :
            (right ? 1u : static_cast<std::size_t>(kWsMarginTraceLayerCount));
        if (horizontal_side < 2u) {
            ++diagnostics->horizontal_final_selected[horizontal_side][layer][
                source];
        }
    };

    uint8_t* row = row_override ? row_override :
        rgb + static_cast<std::size_t>(output_y) * out_w * 3u;
    if (dispcnt & 0x0080u) {
        std::memset(row, 0xFF, out_w * 3);
        // The callback is opt-in and the faithful/native paths pass zero, so
        // this branch preserves the historical forced-blank output unless a
        // game explicitly asks for expanded-margin pillarboxing.
        if ((margin_policy_flags != 0 || diagnostics != nullptr) &&
            pixel_scale == 1) {
            for (uint32_t x = 0; x < out_w; ++x) {
                const bool left = x < ox;
                const bool right = x >= ox + kVanW;
                const bool top = logical_y < 0;
                const bool bottom = logical_y >= static_cast<int>(kVanH);
                if ((left && (margin_policy_flags & kWsMarginPillarboxLeft)) ||
                    (right && (margin_policy_flags & kWsMarginPillarboxRight)) ||
                    (top && (margin_policy_flags & kWsMarginPillarboxTop)) ||
                    (bottom && (margin_policy_flags & kWsMarginPillarboxBottom))) {
                    row[x * 3u + 0] = 0;
                    row[x * 3u + 1] = 0;
                    row[x * 3u + 2] = 0;
                }
                if (is_margin_x(x)) {
                    const bool pillarboxed =
                        (left && (margin_policy_flags & kWsMarginPillarboxLeft)) ||
                        (right && (margin_policy_flags & kWsMarginPillarboxRight)) ||
                        (top && (margin_policy_flags & kWsMarginPillarboxTop)) ||
                        (bottom && (margin_policy_flags & kWsMarginPillarboxBottom));
                    record_margin_final(
                        x, pillarboxed ? kWsMarginTracePillarbox
                                        : kWsMarginTraceForcedBlank,
                         pillarboxed ? kWsMarginSourcePillarbox
                                     : kWsMarginSourceForcedBlank);
                }
            }
        }
        g_ws_obj_census_line = 0;
        return;
    }
    if (g_ws_field_tilemap_source && g_ws_field_tilemap_source_begin)
        g_ws_field_tilemap_source_begin(logical_y, dispcnt, io);

    // Deliberately WITHOUT default member initialisers. The two arrays below
    // are sized by kMaxCompositeWidth (2400, the native supersampler's
    // envelope), while an expanded row is 360 wide; an initialiser here makes
    // the compiler construct all 2400 entries on every scanline -- 96 KB of
    // stores per row, 23 MB per expanded frame, measured at roughly half the
    // cost of an expanded frame (FACTS.md, 2026-09-13). Both arrays are
    // written explicitly for [0, out_w) below instead; nothing past out_w is
    // ever read.
    struct PixelCandidate {
        uint16_t color;
        // Real keys are priority * 256 + OBJ index or 128 + BG layer:
        // at most 3 * 256 + 131 = 899. Reserve the top two uint16_t values
        // for backdrop/empty so the original ordering stays unchanged.
        uint16_t key;
        uint8_t layer;
        uint8_t source;
        bool target1 : 1;
        bool target2 : 1;
        bool valid : 1;
        bool semi : 1;   // a semi-transparent OBJ (OAM mode 1)
    };

    const bool win0_en   = (dispcnt & 0x2000u) != 0;
    const bool win1_en   = (dispcnt & 0x4000u) != 0;
    const bool objwin_en = (dispcnt & 0x8000u) != 0;
    const bool any_window = win0_en || win1_en || objwin_en;
    uint16_t winin  = static_cast<uint16_t>(io[0x48] | (io[0x49] << 8));
    uint16_t winout = static_cast<uint16_t>(io[0x4A] | (io[0x4B] << 8));
    auto win_v_row_at = [&](uint32_t vreg, int y) -> bool {
        uint32_t v  = static_cast<uint32_t>(io[vreg] | (io[vreg + 1] << 8));
        uint32_t y1 = (v >> 8) & 0xFFu, y2 = v & 0xFFu;
        if (y2 > kVanH || y1 > y2) y2 = kVanH;
        return y >= static_cast<int>(y1) && y < static_cast<int>(y2);
    };
    auto win_v_row = [&](uint32_t vreg) -> bool {
        return win_v_row_at(vreg, logical_y);
    };
    auto win_h_in = [&](uint32_t hreg, int hx) -> bool {
        uint32_t h = static_cast<uint32_t>(io[hreg] | (io[hreg + 1] << 8));
        uint32_t x1 = (h >> 8) & 0xFFu, x2 = h & 0xFFu;
        if (x2 > kVanW || x1 > x2) x2 = kVanW;
        return hx >= static_cast<int>(x1) && hx < static_cast<int>(x2);
    };
    const bool win0_row = win0_en && win_v_row(0x44);
    const bool win1_row = win1_en && win_v_row(0x46);
    // OBJ-window stencil is built in vanilla 240-space; margin columns (hx
    // outside [0,240)) get no OBJ-window (WINOUT), consistent with WIN0/1.
    bool obj_window_storage[GbaPpu::kScreenWidth] = {};
    bool* obj_window_mask = nullptr;
    if (objwin_en) {
        mark_obj_window_scanline(obj_window_storage, logical_y, dispcnt, vram, oam,
                                 kVanW, kVanH);
        obj_window_mask = obj_window_storage;
    }
    auto window_control_at_hx = [&](int hx) -> uint16_t {
        if (!any_window) return 0x3Fu;
        if (win0_row && win_h_in(0x40, hx)) return winin & 0x3Fu;
        if (win1_row && win_h_in(0x42, hx)) return (winin >> 8) & 0x3Fu;
        if (obj_window_mask && hx >= 0 && hx < static_cast<int>(kVanW) &&
            obj_window_mask[hx])
            return static_cast<uint16_t>((winout >> 8) & 0x3Fu);
        return static_cast<uint16_t>(winout & 0x3Fu);
    };
    // The same question for a pixel on another row, asked only about a sample
    // a game provider remapped (g_ws_bg_sample_provider). The OBJ-window
    // stencil is built for logical_y alone, so a foreign row answers from
    // WIN0/WIN1/WINOUT exactly as a margin column already does.
    auto window_control_at_pixel = [&](int hx, int y) -> uint16_t {
        if (!any_window) return 0x3Fu;
        if (y == logical_y) return window_control_at_hx(hx);
        if (win0_en && win_v_row_at(0x44, y) && win_h_in(0x40, hx))
            return winin & 0x3Fu;
        if (win1_en && win_v_row_at(0x46, y) && win_h_in(0x42, hx))
            return (winin >> 8) & 0x3Fu;
        return static_cast<uint16_t>(winout & 0x3Fu);
    };
    // A guest-authored window has no defined continuation beyond the native
    // 240-pixel scanline. Extend it into the margins only when every authentic
    // pixel selects the same window control. Sampling just an edge and the
    // center misses narrow apertures (including OBJ-window stencils) that lie
    // between those probes and can leak scenery around transitions.
    uint16_t uniform_window_control = window_control_at_hx(0);
    bool window_control_is_uniform = true;
    if (g_ws_tilemap_provider && any_window) {
        for (int hx = 1; hx < static_cast<int>(kVanW); ++hx) {
            if (window_control_at_hx(hx) != uniform_window_control) {
                window_control_is_uniform = false;
                break;
            }
        }
    }
    // The same question for a remapped sample (g_ws_bg_sample_provider). A
    // margin column must be answered the way window_control below answers one
    // -- by extending the row's own uniform control -- and NOT by WINOUT: a
    // guest window has no defined continuation past the native 240, and
    // WINOUT commonly has the world layers off, which would switch the
    // remapped layer off across the whole margin. Golden Sun's battle band is
    // exactly that case: WIN0H covers all 240 authentic columns, so the band
    // continues into the margins instead of ending at them.
    auto window_control_for_sample = [&](int hx, int y) -> uint16_t {
        if ((hx < 0 || hx >= static_cast<int>(kVanW)) &&
            g_ws_tilemap_provider && !g_ws_authored_margin_layers) {
            return window_control_is_uniform ? uniform_window_control : 0u;
        }
        return window_control_at_pixel(hx, y);
    };
    // A layer whose remapped placement the game adapter owns outright asks no
    // window at all. Golden Sun's battle arena is the case: the game's window
    // exists to letterbox the arena into the authentic screen, and the adapter
    // is deliberately spreading that same band across the whole canvas, so the
    // letterbox would cut the very rows it is there to supply.
    const auto sample_window_bypassed = [&](uint32_t layer) {
        return (g_ws_bg_sample_provider_ignore_window_layers &
                (1u << layer)) != 0u;
    };
    const bool black_nonuniform_window_margins =
        g_ws_tilemap_provider && any_window && !window_control_is_uniform &&
        !g_ws_authored_margin_layers;
    const auto margin_is_pillarboxed = [&](uint32_t x) {
        const bool left = static_cast<int>(x) < native_first;
        const bool right = static_cast<int>(x) >= native_last;
        const bool top = logical_y < 0;
        const bool bottom = logical_y >= static_cast<int>(kVanH);
        const bool vertical = top || bottom;
        const bool use_legacy_pillarbox = (g_ws_margin_policy == nullptr);
        return (black_nonuniform_window_margins &&
                (left || right || vertical)) ||
            (use_legacy_pillarbox && g_ws_pillarbox &&
             (left || right || vertical)) ||
            (use_legacy_pillarbox && g_ws_pillarbox_left && left) ||
            (use_legacy_pillarbox && g_ws_pillarbox_right && right) ||
            (use_legacy_pillarbox && g_ws_pillarbox_top && top) ||
            (use_legacy_pillarbox && g_ws_pillarbox_bottom && bottom) ||
            ((margin_policy_flags & kWsMarginPillarboxLeft) && left) ||
            ((margin_policy_flags & kWsMarginPillarboxRight) && right) ||
            ((margin_policy_flags & kWsMarginPillarboxTop) && top) ||
            ((margin_policy_flags & kWsMarginPillarboxBottom) && bottom);
    };
    // A row on which EVERY column is pillarboxed is unconditionally black.
    // The output loop already writes it black -- but only after every enabled
    // BG layer and every sprite has been sampled across the whole canvas and
    // then thrown away. Golden Sun's battle policy pillarboxes the entire
    // vertical margin, so 80 of an expanded battle frame's 240 rows were
    // paying full price for pixels that can only be black.
    //
    // margin_is_pillarboxed depends on x only through left/right/native, so
    // three probes decide the whole row exactly: one column from each class.
    // Diagnostics and the row census both want per-column records, so the
    // shortcut stands down whenever either is armed.
    const bool row_fully_pillarboxed =
        out_w > 0 && margin_is_pillarboxed(0u) &&
        margin_is_pillarboxed(out_w - 1u) &&
        margin_is_pillarboxed(static_cast<uint32_t>(native_first));
    if (row_fully_pillarboxed && diagnostics == nullptr &&
        !(g_ws_row_state_dump_mode >= 0 && output_y < kRowCensusRows)) {
        std::memset(row, 0, static_cast<std::size_t>(out_w) * 3u);
        g_ws_obj_census_line = 0;
        return;
    }
    auto window_control = [&](uint32_t x) -> uint16_t {
        int hx = logical_x(x);
        // A margin clamp used to live here, answering margin columns with
        // the nearest native column's window control. It was meant to stop
        // WINOUT enabling text layers beside the world; instead it switched
        // the background layers OFF across the whole margin, because the
        // edge column is often outside the game's window rectangle. Removed
        // 2026-09-05 -- it cost far more than the stray tiles it fixed, and
        // those need a different answer.
        if (g_ws_tilemap_provider &&
            (hx < 0 || hx >= static_cast<int>(kVanW))) {
            // Minish's room provider treats expanded columns as lying outside
            // every native WIN0/WIN1 rectangle, so WINOUT controls the world
            // there. Other providers retain the established fail-closed rule.
            if (g_ws_authored_margin_layers)
                return window_control_at_hx(hx);
            return window_control_is_uniform ? uniform_window_control : 0u;
        }
        return window_control_at_hx(hx);
    };
    // Every later compositor stage asks the same window questions for the
    // same output column. Resolve them once per scanline instead of repeating
    // WIN0/WIN1/OBJ-window branches for the backdrop, every BG, and every OBJ
    // pixel. The fixed-size stack array covers the renderer's clamped width.
    // The authentic IO of another row, when a remapped sample needs it.
    const auto row_io_for = [&](int row) -> const uint8_t* {
        if (!line_io_table || !line_io_valid) return nullptr;
        if (row < 0 || row >= static_cast<int>(kVanH)) return nullptr;
        if (!line_io_valid[row]) return nullptr;
        return line_io_table +
            static_cast<std::size_t>(row) * GbaPpu::kLineIoBytes;
    };

    uint8_t window_controls[GbaPpu::kMaxCompositeWidth];
    if (any_window) {
        for (uint32_t x = 0; x < out_w; ++x)
            window_controls[x] = static_cast<uint8_t>(window_control(x) & 0x3Fu);
    } else {
        // With no WIN0/WIN1/OBJ-window enabled every layer and the special
        // effect are visible for every output column.
        std::memset(window_controls, 0x3F, out_w);
    }
    auto layer_enabled = [&](uint32_t x, uint32_t layer_bit) -> bool {
        return (window_controls[x] & (1u << layer_bit)) != 0;
    };
    auto blend_enabled = [&](uint32_t x) -> bool {
        return (window_controls[x] & (1u << 5)) != 0;
    };

    uint16_t bldcnt = static_cast<uint16_t>(io[0x50] | (io[0x51] << 8));
    uint16_t bldalpha = static_cast<uint16_t>(io[0x52] | (io[0x53] << 8));
    uint32_t first_targets = bldcnt & 0x3Fu;
    uint32_t second_targets = (bldcnt >> 8) & 0x3Fu;
    uint32_t effect = (bldcnt >> 6) & 0x3u;

    PixelCandidate top[GbaPpu::kMaxCompositeWidth];
    PixelCandidate second[GbaPpu::kMaxCompositeWidth];
    const uint16_t backdrop_color = load_u16_le(&pal[0]);
    // `second` has no backdrop: it is the layer BELOW the winner, and "nothing
    // there" is what an unwritten column means. Only its key/valid matter, but
    // every field is set so no path can read an indeterminate byte.
    for (uint32_t x = 0; x < out_w; ++x) {
        second[x].color = 0;
        second[x].key = UINT16_MAX;
        second[x].layer = 5;
        second[x].source = static_cast<uint8_t>(kWsMarginSourceBackdrop);
        second[x].target1 = false;
        second[x].target2 = false;
        second[x].valid = false;
        second[x].semi = false;
    }
    for (uint32_t x = 0; x < out_w; ++x) {
        top[x].color = backdrop_color;
        top[x].key = UINT16_MAX - 1;
        top[x].layer = 5;
        top[x].source = static_cast<uint8_t>(kWsMarginSourceBackdrop);
        top[x].target1 = blend_enabled(x) && ((first_targets & (1u << 5)) != 0);
        top[x].target2 = (second_targets & (1u << 5)) != 0;
        top[x].valid = true;
        top[x].semi = false;
    }
    auto submit = [&](uint32_t x, uint16_t color, int key, uint8_t layer,
                      uint8_t source, bool target1, bool target2,
                      bool semi = false) {
        PixelCandidate cand;
        // The 555 colour is kept and converted once, at output. Converting
        // here instead converted every BG and OBJ pixel that was later
        // overdrawn -- up to four conversions per output pixel where one is
        // needed (FACTS.md, 2026-09-13).
        cand.color = color;
        cand.key = key;
        cand.layer = layer;
        cand.source = source;
        cand.target1 = target1;
        cand.target2 = target2;
        cand.valid = true;
        cand.semi = semi;
        // Sprites are ONE layer on hardware: at each pixel only the frontmost
        // sprite takes part, and a sprite never becomes the layer below
        // another sprite. Keeping the sprite behind as `second` made a
        // semi-transparent sprite find no blend partner over another sprite
        // and fall through to the brightness effect -- Golden Sun's defend
        // flash (BLDCNT 0x3F90, BLDY 16) then painted every sprite overlap
        // white (gpu_rewind_0050, FACTS.md 2026-09-25).
        if (layer == 4) {
            if (top[x].layer == 4) {
                if (key < top[x].key) top[x] = cand;
                return;
            }
            if (second[x].valid && second[x].layer == 4) {
                if (key >= second[x].key) return;
                if (key < top[x].key) { second[x] = top[x]; top[x] = cand; }
                else second[x] = cand;
                return;
            }
        }
        if (key < top[x].key) { second[x] = top[x]; top[x] = cand; }
        else if (key < second[x].key) { second[x] = cand; }
    };

    uint32_t bg_mode = dispcnt & 0x07u;
    const auto record_provider_result = [&](uint32_t layer, int action) {
        if (!diagnostics || layer >= 4u) return;
        const std::size_t result = action == kWsTilemapReplace
            ? kWsMarginProviderReplace
            : (action == kWsTilemapKeepWrapped
                   ? kWsMarginProviderKeepWrapped
                   : kWsMarginProviderUnavailable);
        ++diagnostics->provider_results[layer][result];
    };
    auto render_regular_bg = [&](uint32_t layer, uint32_t cnt_off,
                                 uint32_t scroll_off) {
        if ((dispcnt & (0x0100u << layer)) == 0) return;
        if (hide_bg_layer(layer)) return;
        uint16_t bgcnt = static_cast<uint16_t>(io[cnt_off] | (io[cnt_off + 1] << 8));
        uint32_t char_base = ((bgcnt >> 2) & 0x3u) * 0x4000u;
        uint32_t screen_base = ((bgcnt >> 8) & 0x1Fu) * 0x800u;
        bool color256 = (bgcnt & 0x0080u) != 0;
        uint32_t size_code = (bgcnt >> 14) & 0x3u;
        uint32_t bg_priority = bgcnt & 0x3u;
        uint32_t hofs = static_cast<uint16_t>(
            io[scroll_off] | (io[scroll_off + 1] << 8)) & 0x01FFu;
        uint32_t vofs = static_cast<uint16_t>(
            io[scroll_off + 2] | (io[scroll_off + 3] << 8)) & 0x01FFu;
        uint32_t width_tiles = (size_code & 1u) ? 64u : 32u;
        uint32_t height_tiles = (size_code & 2u) ? 64u : 32u;
        uint32_t width_px = width_tiles * 8u;
        uint32_t height_px = height_tiles * 8u;
        uint32_t block_cols = width_tiles / 32u;
        for (uint32_t x = 0; x < out_w; ++x) {
            int hx = logical_x(x);
            const bool authored_margin = g_ws_authored_margin_layers &&
                (hx < 0 || hx >= static_cast<int>(kVanW));
            // A game-owned sample remap runs before the window question,
            // because it decides WHICH hardware pixel this output pixel shows
            // and the window must then be asked about that pixel.
            int sample_hx = hx;
            int sample_y = logical_y;
            bool remapped = false;
            if (g_ws_bg_sample_provider &&
                (g_ws_bg_sample_provider_layers & (1u << layer))) {
                int provided_hx = hx;
                int provided_y = logical_y;
                const int action = g_ws_bg_sample_provider(
                    static_cast<int>(layer), static_cast<int>(x), logical_y,
                    &provided_hx, &provided_y);
                if (action < 0) continue;
                if (action > 0) {
                    sample_hx = provided_hx;
                    sample_y = provided_y;
                    remapped = true;
                }
            }
            if (remapped) {
                if (!sample_window_bypassed(layer) &&
                    (window_control_for_sample(sample_hx, sample_y) &
                     (1u << layer)) == 0) {
                    continue;
                }
            } else if (!layer_enabled(x, layer) && !authored_margin) {
                // Minish supplies room-map entries for margins independently
                // of the native 240px WIN0/WIN1 HUD/dialog masks. Ignore only
                // their regular-BG layer gate there; unsupported UI BGs still
                // fail in the provider below, so they cannot repeat.
                continue;
            }
            // Widescreen BG-x providers only ever act on margin columns (see
            // is_margin_x above): for native columns they are proven no-ops
            // (action == 0, sample_hx unchanged), so skip the call entirely
            // there to avoid the per-pixel function-pointer overhead across
            // the whole native 240-column region.
            if (!remapped && is_margin_x(x) && g_ws_bg_x_provider &&
                (g_ws_bg_x_provider_layers & (1u << layer))) {
                int provided_hx = hx;
                const int action = g_ws_bg_x_provider(
                    static_cast<int>(layer), static_cast<int>(x),
                    logical_y, &provided_hx);
                if (action < 0) continue;
                if (action > 0) sample_hx = provided_hx;
            }
            // A game-owned camera clamp (g_ws_view_shift_x/y, zero unless a
            // policy pans the whole expanded canvas) moves the world sample
            // for every column uniformly, native and margin alike -- applied
            // here, after the screen-space authored_margin/bg_x_provider
            // checks above (which must stay in physical column space), and
            // before the wrap/native-window math below (which must reflect
            // the shifted world position).
            const int world_hx = sample_hx + g_ws_view_shift_x;
            const int world_y = sample_y + g_ws_view_shift_y;
            // A remapped sample belongs to another row, so it scrolls by that
            // row's registers. Only the scroll is re-read: a game that also
            // repoints the layer's map or tiles mid-frame would need the rest,
            // and none seen here does.
            uint32_t row_hofs = hofs;
            uint32_t row_vofs = vofs;
            if (remapped) {
                if (const uint8_t* row_io = row_io_for(sample_y)) {
                    row_hofs = static_cast<uint16_t>(
                        row_io[scroll_off] | (row_io[scroll_off + 1] << 8)) &
                        0x01FFu;
                    row_vofs = static_cast<uint16_t>(
                        row_io[scroll_off + 2] |
                        (row_io[scroll_off + 3] << 8)) & 0x01FFu;
                }
            }
            uint32_t tex_x = static_cast<uint32_t>(
                                 world_hx + static_cast<int>(row_hofs)) &
                             (width_px - 1u);
            uint32_t tex_y = static_cast<uint32_t>(
                static_cast<int64_t>(world_y) + row_vofs) & (height_px - 1u);
            uint32_t tile_x = tex_x >> 3;
            uint32_t tile_y = tex_y >> 3;
            uint32_t block = (tile_x >> 5) + (tile_y >> 5) * block_cols;
            uint32_t map_off = screen_base + block * 0x800u +
                ((tile_y & 31u) * 32u + (tile_x & 31u)) * 2u;
            const bool beyond_native =
                world_hx < 0 || world_hx >= static_cast<int>(kVanW) ||
                world_y < 0 || world_y >= static_cast<int>(kVanH);
            const bool wrapped_entry_in_bounds = map_off + 1 < 96u * 1024u;
            uint16_t entry = 0;
            bool entry_supplied = false;
            bool field_source_answered = false;
            // A remapped sample is the game's own answer about which hardware
            // pixel belongs here, so the margin reconstruction paths below --
            // which exist to decide what an off-screen sample should show --
            // have nothing left to decide and are skipped.
            if (!remapped && g_ws_field_tilemap_source) {
                uint16_t supplied = 0;
                if (g_ws_field_tilemap_source(static_cast<int>(layer), world_hx,
                                              world_y, &supplied)) {
                    entry = supplied;
                    entry_supplied = true;
                    field_source_answered = true;
                    ++g_ws_expanded_diag[1];
                } else if (beyond_native) {
                    // Beyond the native area with no answer: draw nothing. The
                    // 256px ring entry sitting here is the wrapped seam, i.e.
                    // some unrelated part of the room repeated, and drawing it
                    // is what produced the garbage margins in the three earlier
                    // widescreen attempts. Transparent leaves black, which is
                    // the agreed behaviour when a room cannot fill the view.
                    ++g_ws_expanded_diag[3];
                    continue;
                }
            }
            uint8_t source = static_cast<uint8_t>(kWsMarginSourceWrapped);
            // Widescreen margin (hardware column outside 0..239): the 256px ring
            // entry here is the wrapped/aliased seam. If the sidecar can supply
            // the true off-screen world tile, use it; if it's armed but has no
            // data, leave the pixel transparent (no seam) rather than draw the
            // wrap. With no sidecar, keep vanilla wide behavior.
            const bool outside_native =
                world_hx < 0 || world_hx >= static_cast<int>(kVanW) ||
                world_y < 0 || world_y >= static_cast<int>(kVanH);
            // ...and only when nothing better already answered. The field
            // tilemap source reconstructs the margin tile from the game's own
            // map tables, so its answer is final. Running the older per-pixel
            // margin provider over the top of it threw that answer away: the
            // provider reports Unavailable for an ordinary field scene, and
            // the `continue` below then dropped every margin pixel the room
            // buffer had just supplied. That is why margins stayed black in
            // sessions whose own counters reported tens of millions of
            // entries supplied (session_20260905_233225: 91.5M supplied,
            // margins black).
            if (outside_native && !field_source_answered && !remapped) {
                if (g_ws_tilemap_provider) {
                    uint16_t ext;
                    const int action = g_ws_tilemap_provider(
                        static_cast<int>(layer), world_hx,
                        world_y, &ext);
                    record_provider_result(layer, action);
                    if (action == kWsTilemapReplace) {
                        entry = ext;
                        entry_supplied = true;
                        source = static_cast<uint8_t>(
                            kWsMarginSourceProviderReplace);
                    } else if (action != kWsTilemapKeepWrapped) {
                        continue;
                    } else {
                        source = static_cast<uint8_t>(
                            kWsMarginSourceProviderKeepWrapped);
                    }
                }
            }
            // Read the wrapped hardware entry only when no game-owned source
            // supplied one. This keeps the bounds check beside the sole read,
            // while preserving KeepWrapped as an explicit request for that
            // bounded hardware entry.
            if (!entry_supplied) {
                if (!wrapped_entry_in_bounds) continue;
                entry = load_u16_le(&vram[map_off]);
            }
            uint32_t tile_num = entry & 0x03FFu;
            bool hflip = (entry & 0x0400u) != 0;
            bool vflip = (entry & 0x0800u) != 0;
            uint32_t palette_bank = (entry >> 12) & 0x0Fu;
            uint32_t px = tex_x & 7u;
            uint32_t py = tex_y & 7u;
            if (hflip) px = 7u - px;
            if (vflip) py = 7u - py;
            uint8_t pal_idx = 0;
            if (color256) {
                uint32_t tile_addr = char_base + tile_num * 64u + py * 8u + px;
                if (tile_addr >= 96u * 1024u) continue;
                pal_idx = vram[tile_addr];
            } else {
                uint32_t tile_addr = char_base + tile_num * 32u + py * 4u + (px >> 1);
                if (tile_addr >= 96u * 1024u) continue;
                uint8_t packed = vram[tile_addr];
                pal_idx = (px & 1u) ? (packed >> 4) : (packed & 0x0Fu);
                pal_idx = static_cast<uint8_t>(
                    pal_idx | static_cast<uint8_t>(palette_bank << 4));
            }
            if ((pal_idx & (color256 ? 0xFFu : 0x0Fu)) == 0) continue;
            const uint16_t color = load_u16_le(&pal[pal_idx * 2]);
            submit(x, color,
                   static_cast<int>(bg_priority * 256u + 128u + layer),
                   static_cast<uint8_t>(layer), source,
                   blend_enabled(x) && ((first_targets & (1u << layer)) != 0),
                   (second_targets & (1u << layer)) != 0);
        }
    };
    auto render_affine_bg = [&](uint32_t layer, uint32_t cnt_off,
                                uint32_t param_off) {
        if ((dispcnt & (0x0100u << layer)) == 0) return;
        if (hide_bg_layer(layer)) return;
        uint16_t bgcnt = static_cast<uint16_t>(io[cnt_off] | (io[cnt_off + 1] << 8));
        uint32_t char_base   = ((bgcnt >> 2) & 0x3u) * 0x4000u;
        uint32_t screen_base = ((bgcnt >> 8) & 0x1Fu) * 0x800u;
        bool wrap            = (bgcnt & 0x2000u) != 0;
        uint32_t size_code   = (bgcnt >> 14) & 0x3u;
        int bg_pixels = 128 << size_code;
        int bg_tiles  = bg_pixels / 8;
        int bg_priority = static_cast<int>(bgcnt & 0x3u);
        int32_t pa = read_s16(io, param_off + 0x00);
        int32_t pb = read_s16(io, param_off + 0x02);
        int32_t pc = read_s16(io, param_off + 0x04);
        int32_t pd = read_s16(io, param_off + 0x06);
        const uint32_t ref_index = (layer - 2u) * 2u;
        int32_t refx = affine_refs ? affine_refs[ref_index]
                                   : read_s28_ref(io, param_off + 0x08);
        int32_t refy = affine_refs ? affine_refs[ref_index + 1u]
                                   : read_s28_ref(io, param_off + 0x0C);
        // Extrapolate from the scanline reference using the signed logical x:
        // output column 0 is hardware x = -ox, so pre-advance by (-ox)*PA/PC.
        int64_t xt = static_cast<int64_t>(refx) +
                      (affine_refs ?
                          static_cast<int64_t>(logical_y - affine_ref_line) * pb :
                          static_cast<int64_t>(logical_y) * pb) +
                     static_cast<int32_t>(-static_cast<int>(ox)) * pa;
        int64_t yt = static_cast<int64_t>(refy) +
                      (affine_refs ?
                          static_cast<int64_t>(logical_y - affine_ref_line) * pd :
                          static_cast<int64_t>(logical_y) * pd) +
                     static_cast<int32_t>(-static_cast<int>(ox)) * pc;
        // A game-owned sample remap (see g_ws_bg_sample_provider) applies to an
        // affine layer as well: the provider names the hardware pixel this
        // output pixel shows, and the transform is then evaluated there. The
        // incremental xt/yt walk below is skipped for those columns, since the
        // remapped source does not advance one pixel per output pixel.
        const bool affine_remap_armed = g_ws_bg_sample_provider &&
            (g_ws_bg_sample_provider_layers & (1u << layer)) &&
            pixel_scale == 1;
        for (uint32_t x = 0; x < out_w; ++x) {
            int32_t tex_x;
            int32_t tex_y;
            if (affine_remap_armed) {
                const int native_hx = logical_x(x);
                int sample_hx = native_hx;
                int sample_y = logical_y;
                const int action = g_ws_bg_sample_provider(
                    static_cast<int>(layer), static_cast<int>(x), logical_y,
                    &sample_hx, &sample_y);
                if (action < 0) {
                    xt += pa;
                    yt += pc;
                    continue;
                }
                if (action > 0) {
                    // A same-row identity remap's source coordinate is
                    // exactly the affine walk's current hardware coordinate.
                    // Reuse that walk and avoid re-reading the row's
                    // IO/reference state. A provider may still return a
                    // different coordinate, so retain the full source-row
                    // path below for that case.
                    if (sample_hx == native_hx && sample_y == logical_y) {
                        tex_x = xt >> 8;
                        tex_y = yt >> 8;
                        xt += pa;
                        yt += pc;
                    } else {
                        // Prefer the source row's OWN latched reference;
                        // fall back to extrapolating this row's when it is
                        // unavailable.
                        int32_t use_refx = refx;
                        int32_t use_refy = refy;
                        int32_t use_pa = pa, use_pb = pb;
                        int32_t use_pc = pc, use_pd = pd;
                        if (const uint8_t* row_io = row_io_for(sample_y)) {
                            use_pa = read_s16(row_io, param_off + 0x00);
                            use_pb = read_s16(row_io, param_off + 0x02);
                            use_pc = read_s16(row_io, param_off + 0x04);
                            use_pd = read_s16(row_io, param_off + 0x06);
                        }
                        int64_t rows_from_ref = affine_refs
                            ? static_cast<int64_t>(sample_y - affine_ref_line)
                            : static_cast<int64_t>(sample_y);
                        if (affine_line_refs && affine_line_refs_valid &&
                            sample_y >= 0 &&
                            sample_y < static_cast<int>(kVanH) &&
                            affine_line_refs_valid[sample_y]) {
                            const int32_t* row_refs =
                                affine_line_refs +
                                static_cast<std::size_t>(sample_y) * 4u;
                            use_refx = row_refs[ref_index];
                            use_refy = row_refs[ref_index + 1u];
                            rows_from_ref = 0;
                        }
                        const int64_t x_fp = static_cast<int64_t>(use_refx) +
                            rows_from_ref * use_pb +
                            static_cast<int64_t>(sample_hx) * use_pa;
                        const int64_t y_fp = static_cast<int64_t>(use_refy) +
                            rows_from_ref * use_pd +
                            static_cast<int64_t>(sample_hx) * use_pc;
                        tex_x = static_cast<int32_t>(x_fp >> 8);
                        tex_y = static_cast<int32_t>(y_fp >> 8);
                        xt += pa;
                        yt += pc;
                    }
                    // The provider is armed for every enabled layer so that a
                    // margin callback can opt into remapping. A no-op is still a
                    // native center sample and must retain the ordinary window
                    // gate; only a positive remap may use the adapter's bypass.
                    if (!sample_window_bypassed(layer) &&
                        (window_control_for_sample(sample_hx, sample_y) &
                         (1u << layer)) == 0) {
                        continue;
                    }
                    if (wrap) {
                        tex_x &= (bg_pixels - 1);
                        tex_y &= (bg_pixels - 1);
                    } else if (tex_x < 0 || tex_x >= bg_pixels ||
                               tex_y < 0 || tex_y >= bg_pixels) {
                        continue;
                    }
                    uint32_t remap_map_off =
                        screen_base + (tex_y >> 3) * bg_tiles + (tex_x >> 3);
                    if (remap_map_off >= 96u * 1024u) continue;
                    const uint8_t remap_tile = vram[remap_map_off];
                    const uint32_t remap_addr = char_base + remap_tile * 64u +
                        (tex_y & 7) * 8 + (tex_x & 7);
                    if (remap_addr >= 96u * 1024u) continue;
                    const uint8_t remap_pal_idx = vram[remap_addr];
                    if (remap_pal_idx == 0) continue;
                    submit(x, load_u16_le(&pal[remap_pal_idx * 2]),
                           bg_priority * 256 + 128 + static_cast<int>(layer),
                           static_cast<uint8_t>(layer),
                           static_cast<uint8_t>(kWsMarginSourceWrapped),
                           blend_enabled(x) &&
                               ((first_targets & (1u << layer)) != 0),
                           (second_targets & (1u << layer)) != 0);
                    continue;
                }
            }
            if (pixel_scale == 1) {
                tex_x = xt >> 8;
                tex_y = yt >> 8;
                xt += pa;
                yt += pc;
            } else {
                // Keep the affine transform in fixed point until the final
                // sample. This avoids the phase drift caused by duplicating a
                // 240px affine result and gives each native subpixel its own
                // line-affine coordinate.
                const int64_t x_rel = static_cast<int64_t>(x) -
                    static_cast<int64_t>(ox) * pixel_scale;
                const int64_t x_fp = static_cast<int64_t>(refx) +
                    (affine_refs ?
                        static_cast<int64_t>(logical_y - affine_ref_line) * pb :
                        static_cast<int64_t>(logical_y) * pb) +
                    (x_rel * pa) / static_cast<int64_t>(pixel_scale);
                const int64_t y_fp = static_cast<int64_t>(refy) +
                    (affine_refs ?
                        static_cast<int64_t>(logical_y - affine_ref_line) * pd :
                        static_cast<int64_t>(logical_y) * pd) +
                    (x_rel * pc) / static_cast<int64_t>(pixel_scale);
                tex_x = static_cast<int32_t>(x_fp >> 8);
                tex_y = static_cast<int32_t>(y_fp >> 8);
            }
            if (!layer_enabled(x, layer)) continue;
            if (wrap) { tex_x &= (bg_pixels - 1); tex_y &= (bg_pixels - 1); }
            else if (tex_x < 0 || tex_x >= bg_pixels ||
                     tex_y < 0 || tex_y >= bg_pixels) continue;
            uint32_t map_off = screen_base + (tex_y >> 3) * bg_tiles + (tex_x >> 3);
            if (map_off >= 96u * 1024u) continue;
            uint8_t tile_index = vram[map_off];
            uint32_t tile_addr = char_base + tile_index * 64u +
                                 (tex_y & 7) * 8 + (tex_x & 7);
            if (tile_addr >= 96u * 1024u) continue;
            uint8_t pal_idx = vram[tile_addr];
            if (pal_idx == 0) continue;
            const uint16_t color = load_u16_le(&pal[pal_idx * 2]);
            submit(x, color,
                   static_cast<int>(bg_priority * 256 + 128 + layer),
                   static_cast<uint8_t>(layer),
                    static_cast<uint8_t>(kWsMarginSourceWrapped),
                   blend_enabled(x) && ((first_targets & (1u << layer)) != 0),
                   (second_targets & (1u << layer)) != 0);
        }
    };
    if (bg_mode == 0) {
        render_regular_bg(3, 0x0E, 0x1C);
        render_regular_bg(2, 0x0C, 0x18);
        render_regular_bg(1, 0x0A, 0x14);
        render_regular_bg(0, 0x08, 0x10);
    } else if (bg_mode == 1) {
        render_affine_bg(2, 0x0C, 0x20);
        render_regular_bg(1, 0x0A, 0x14);
        render_regular_bg(0, 0x08, 0x10);
    } else if (bg_mode == 2) {
        render_affine_bg(3, 0x0E, 0x30);
        render_affine_bg(2, 0x0C, 0x20);
    }

    if (dispcnt & 0x1000u) {
        uint32_t obj_tile_base = (bg_mode >= 3) ? 0x14000u : 0x10000u;
        bool obj_1d_mapping = (dispcnt & 0x0040u) != 0;
        const uint8_t* obj_pal = pal + 0x200;
        for (int idx = 127; idx >= 0; --idx) {
            const uint8_t* entry = oam + idx * 8;
            uint16_t attr0 = load_u16_le(&entry[0]);
            uint16_t attr1 = load_u16_le(&entry[2]);
            uint16_t attr2 = load_u16_le(&entry[4]);
            bool rot_scale = (attr0 & 0x0100u) != 0;
            bool disable_or_double = (attr0 & 0x0200u) != 0;
            if (!rot_scale && disable_or_double) continue;
            uint32_t obj_mode = (attr0 >> 10) & 0x3u;
            if (obj_mode == 2 || obj_mode == 3) continue;
            uint32_t shape = (attr0 >> 14) & 0x3u;
            if (shape >= 3) continue;
            uint32_t size  = (attr1 >> 14) & 0x3u;
            int sw = kSpriteWH[shape][size][0];
            int sh = kSpriteWH[shape][size][1];
            const int raw_sy = static_cast<int>(attr0 & 0xFFu);
            int sy = raw_sy;
            // Trust tracking: true only when a provider supplied a verified
            // world position for this axis; false when we fell back to the
            // raw OAM coordinate (which Golden Sun sometimes parks off the
            // native 240px window for scratch/park-off-screen sprites).
            bool sy_trusted = false;
            auto resolve_sy = [&] {
                int provided_sy = sy;
                if (g_ws_obj_attr_y_provider &&
                    g_ws_obj_attr_y_provider(idx, attr0, attr1, attr2,
                                             &provided_sy)) {
                    sy = provided_sy;
                    sy_trusted = true;
                } else if (g_ws_obj_y_provider &&
                    g_ws_obj_y_provider(raw_sy, &provided_sy)) {
                    sy = provided_sy;
                    sy_trusted = true;
                } else if (raw_sy >= 160) {
                    sy = raw_sy - 256;
                    sy_trusted = false;
                } else {
                    sy = raw_sy;
                    sy_trusted = false;
                }
            };
            resolve_sy();
            // Mirror the regular-BG world shift above: moving the whole
            // canvas by (shift_x, shift_y) means a sprite fixed at world
            // position W must be drawn shift_y/shift_x pixels *earlier* on
            // screen (screen content shifts opposite to how the camera's
            // effective sample position moves), so this subtracts rather
            // than adds. Zero (the default) is a no-op.
            sy -= g_ws_view_shift_y;
            // OAM X is normally signed 9-bit. An opted-in game may reinterpret
            // values that its widened guest writer emitted beyond hardware X.
            const int raw_sx = static_cast<int>(attr1 & 0x1FFu);
            int sx = raw_sx;
            bool sx_trusted = false;
            auto resolve_sx = [&] {
                int provided_sx = sx;
                if (g_ws_obj_attr_x_provider &&
                    g_ws_obj_attr_x_provider(idx, attr0, attr1, attr2,
                                             &provided_sx)) {
                    sx = provided_sx;
                    sx_trusted = true;
                } else if (g_ws_obj_x_provider &&
                    g_ws_obj_x_provider(raw_sx, &provided_sx)) {
                    sx = provided_sx;
                    sx_trusted = true;
                } else if (sx & 0x100) {
                    sx -= 0x200;
                    sx_trusted = false;
                } else {
                    sx_trusted = false;
                }
            };
            // Resolve X before culling, without replacing an authenticated
            // position with a wrapped OAM coordinate.
            resolve_sx();
            // Tally trust outcome once per OBJ per frame: an OBJ may only be
            // placed into the margin when both axes are trusted.
            if (logical_y == 0) {
                if (sx_trusted && sy_trusted) {
                    ++g_ws_obj_trusted_total;
                } else {
                    ++g_ws_obj_untrusted_total;
                }
            }
            sx -= g_ws_view_shift_x;
            bool color256 = (attr0 & 0x2000u) != 0;
            uint32_t tile_num = attr2 & 0x3FFu;
            uint32_t palette_bank = (attr2 >> 12) & 0xFu;
            int tiles_w = sw / 8;
            int tiles_h = sh / 8;
            int priority = static_cast<int>((attr2 >> 10) & 0x3u);
            // Composite key (lower = front). Per-priority stride 256 with OBJ in
            // [p*256, p*256+127] (tie-break by OAM idx) and BG in [p*256+128,
            // p*256+131] (tie-break by layer) yields the exact GBA order
            // OBJ0<BG0<OBJ1<BG1<OBJ2<BG2<OBJ3<BG3: an OBJ is in front of a same-
            // priority BG, but a BG of priority p sits in front of any OBJ of
            // priority p+1 (this is what lets the player walk BEHIND roof/tree
            // tops). The stride MUST exceed 128 so OBJ idx (0..127) can't bleed
            // into the next priority's BG band.
            int key = priority * 256 + idx;
            bool obj_target2 = (second_targets & (1u << 4)) != 0;
            // A provider-authenticated placement is the only coordinate that
            // may extend an OBJ into the expanded margins. If either axis is
            // unknown, emit_obj below retains the canonical native fallback.
            if (g_ws_field_tilemap_source && sx_trusted && sy_trusted) {
                const int ex_left = static_cast<int>(ox / pixel_scale);
                const int ex_right = static_cast<int>(out_w / pixel_scale) -
                                     ex_left - static_cast<int>(kVanW);
                const int ex_top = static_cast<int>(oy);
                const int ex_bottom = static_cast<int>(out_h) - ex_top -
                                      static_cast<int>(kVanH);
                constexpr int kObjSlack = 64;
                const int slack_x = kObjSlack;
                const int slack_y = kObjSlack;
                const int lo_x = -ex_left - slack_x;
                const int hi_x = static_cast<int>(kVanW) + ex_right + slack_x;
                const int lo_y = -ex_top - slack_y;
                const int hi_y = static_cast<int>(kVanH) + ex_bottom + slack_y;
                const int object_w = rot_scale && disable_or_double
                    ? sw * 2 : sw;
                const int object_h = rot_scale && disable_or_double
                    ? sh * 2 : sh;
                // Cull in the same shifted screen space used by emit_obj.
                if (!(sx + object_w > lo_x && sx < hi_x &&
                      sy + object_h > lo_y && sy < hi_y)) {
                    ++g_ws_expanded_diag[2];
                    continue;
                }
            }
            // Confined to the authentic window, so it cannot appear in the
            // margins. Until 2026-09-17 this was every OBJ the game could not
            // authenticate, which kept battle enemies, spell effects and the
            // world map's towns and caves out of the widened view entirely --
            // the "objects cull too early" report. What actually has to be
            // held back is far narrower: a game that parks its unused sprites
            // off-screen instead of disabling them, whose park position falls
            // inside the new margins. `g_ws_obj_park_provider` lets the game
            // name exactly those slots, and everything else now draws where it
            // really is. An OBJ with no park provider at all is never confined.
            const bool obj_parked =
                g_ws_obj_park_provider &&
                g_ws_obj_park_provider(idx, attr0, attr1, attr2) != 0;
            auto emit_obj = [&](int tex_x, int tex_y, int screen_x) {
                const bool native_row =
                    logical_y >= 0 && logical_y < static_cast<int>(kVanH);
                // `screen_x` is still in logical hardware pixels. Expand each
                // source pixel into a run of native samples, keeping one
                // sprite/OAM decision and one blend key for the whole run.
                const int logical_screen_x = screen_x - static_cast<int>(ox);
                const int out_base = logical_screen_x *
                    static_cast<int>(pixel_scale) + static_cast<int>(ox);
                if (out_base < 0 || out_base >= static_cast<int>(out_w)) return;
                if (obj_parked &&
                    (!native_row || out_base < native_first ||
                     out_base >= native_last)) {
                    return;
                }
                int tile_x_in_sprite = tex_x >> 3;
                int tile_y_in_sprite = tex_y >> 3;
                int px_in_tile = tex_x & 7;
                int py_in_tile = tex_y & 7;
                uint32_t this_tile;
                if (obj_1d_mapping) {
                    this_tile = tile_num + (tile_y_in_sprite * tiles_w + tile_x_in_sprite) *
                                            (color256 ? 2u : 1u);
                } else {
                    this_tile = tile_num + (tile_y_in_sprite * 32u) +
                                tile_x_in_sprite * (color256 ? 2u : 1u);
                }
                uint32_t tile_off = obj_tile_base + this_tile * 32u;
                uint8_t pal_index;
                if (color256) {
                    uint32_t off = tile_off + py_in_tile * 8 + px_in_tile;
                    if (off + 1 > 96u * 1024u) return;
                    pal_index = vram[off];
                    if (pal_index == 0) return;
                } else {
                    uint32_t off = tile_off + py_in_tile * 4 + (px_in_tile / 2);
                    if (off + 1 > 96u * 1024u) return;
                    uint8_t b = vram[off];
                    pal_index = (px_in_tile & 1) ? (b >> 4) : (b & 0x0F);
                    if (pal_index == 0) return;
                    pal_index = static_cast<uint8_t>(pal_index | (palette_bank << 4));
                }
                const uint16_t color = load_u16_le(&obj_pal[pal_index * 2]);
                for (uint32_t sub = 0; sub < pixel_scale; ++sub) {
                    const int out_x = out_base + static_cast<int>(sub);
                    if (out_x < 0 || out_x >= static_cast<int>(out_w)) continue;
                    if (obj_parked &&
                        (!native_row || out_x < native_first ||
                         out_x >= native_last)) {
                        continue;
                    }
                    const uint32_t ux = static_cast<uint32_t>(out_x);
                    if (!layer_enabled(ux, 4)) continue;
                    // First target as in render_scanline_internal.
                    const bool t1 = blend_enabled(ux) &&
                        (obj_mode == 1 || (first_targets & (1u << 4)) != 0);
                    submit(ux, color, key, 4,
                           static_cast<uint8_t>(kWsMarginSourceObj), t1,
                           obj_target2, obj_mode == 1);
                }
            };
            if (rot_scale) {
                int bw = disable_or_double ? sw * 2 : sw;
                int bh = disable_or_double ? sh * 2 : sh;
                int j = logical_y - sy;
                if (j < 0 || j >= bh) continue;
                int affine_group = (attr1 >> 9) & 0x1Fu;
                const uint8_t* ag = oam + affine_group * 0x20u;
                int32_t pa = read_s16(ag, 0x06);
                int32_t pb = read_s16(ag, 0x0E);
                int32_t pc = read_s16(ag, 0x16);
                int32_t pd = read_s16(ag, 0x1E);
                int half_bw = bw >> 1;
                int half_bh = bh >> 1;
                int half_sw = sw >> 1;
                int half_sh = sh >> 1;
                int dy = j - half_bh;
                for (int i = 0; i < bw; ++i) {
                    int dx = i - half_bw;
                    int tex_x = ((pa * dx + pb * dy) >> 8) + half_sw;
                    int tex_y = ((pc * dx + pd * dy) >> 8) + half_sh;
                    if (tex_x < 0 || tex_x >= sw) continue;
                    if (tex_y < 0 || tex_y >= sh) continue;
                    emit_obj(tex_x, tex_y, sx + i + static_cast<int>(ox));
                }
                continue;
            }
            int line = logical_y - sy;
            if (line < 0 || line >= sh) continue;
            bool hflip = (attr1 & 0x1000u) != 0;
            bool vflip = (attr1 & 0x2000u) != 0;
            int ty = line >> 3;
            int py = line & 7;
            int src_ty = vflip ? (tiles_h - 1 - ty) : ty;
            int src_py = vflip ? (7 - py) : py;
            for (int tx = 0; tx < tiles_w; ++tx) {
                int src_tx = hflip ? (tiles_w - 1 - tx) : tx;
                for (int px = 0; px < 8; ++px) {
                    int src_px = hflip ? (7 - px) : px;
                    emit_obj(src_tx * 8 + src_px, src_ty * 8 + src_py,
                             sx + tx * 8 + px + static_cast<int>(ox));
                }
            }
        }
    }

    uint32_t bldy = static_cast<uint32_t>(io[0x54] | (io[0x55] << 8)) & 0x1Fu;
    if (bldy > 16u) bldy = 16u;
    // One branch per row, not per pixel: the census is off unless the row
    // dump is armed (see g_row_census).
    std::uint32_t* census =
        (g_ws_row_state_dump_mode >= 0 && output_y < kRowCensusRows)
            ? &g_row_census[output_y][0] : nullptr;
    // Which side of the canvas a column is on, and whether this row's margins
    // are pillarboxed, do not change across the row. Asking them once here
    // instead of per pixel removes ~86,000 evaluations of a ten-term
    // predicate per frame (FACTS.md, 2026-09-13).
    const bool row_vertical_margin =
        logical_y < 0 || logical_y >= static_cast<int>(kVanH);
    const bool row_pillar_vertical = row_vertical_margin &&
        margin_is_pillarboxed(static_cast<uint32_t>(native_first));
    const bool row_pillar_left = margin_is_pillarboxed(0u);
    const bool row_pillar_right =
        out_w > 0 && margin_is_pillarboxed(out_w - 1u);
    for (uint32_t x = 0; x < out_w; ++x) {
        uint8_t* dst = row + x * 3;
        if (census) {
            ++census[top[x].layer < kRowCensusLayers - 1u
                         ? top[x].layer : kRowCensusLayers - 1u];
        }
        const bool x_left = static_cast<int>(x) < native_first;
        const bool x_right = static_cast<int>(x) >= native_last;
        const bool x_margin = x_left || x_right || row_vertical_margin;
        const bool x_pillar = (x_left && row_pillar_left) ||
            (x_right && row_pillar_right) ||
            (row_pillar_vertical && !x_left && !x_right);
        // A game-owned per-scanline callback is authoritative. In particular,
        // a zero callback result must be able to reopen margins after a
        // savestate restore left the legacy flag set. Only the null-callback
        // path consults the historical global flags.
        if (x_pillar) {
            dst[0] = dst[1] = dst[2] = 0;
            record_margin_final(
                x, kWsMarginTracePillarbox, kWsMarginSourcePillarbox);
            continue;
        }
        // A margin pixel showing the BACKDROP means the room had no answer
        // here, not that the game chose to show its backdrop. BLDCNT can name
        // the backdrop as a blend target (bit 5), and Golden Sun does exactly
        // that when a menu opens: every native pixel has a real layer on top
        // and is unaffected, while the margin's backdrop pixels darken. The
        // result is the extra area dimming on its own while the middle of the
        // screen stays bright (user-reported 2026-09-11). Reconstructed margin
        // layers are still ordinary blend targets and still fade with a real
        // whole-screen fade; only "nothing was drawn here" is exempt.
        if (x_margin && top[x].layer == 5) {
            to_rgb888(top[x].color, dst);
            record_margin_final(x, top[x].layer, top[x].source);
            continue;
        }
        // Alpha blend top (1st target) with the layer below (2nd target). Per
        // GBATEK this occurs when effect==1 OR top is a semi-transparent OBJ
        // (mode 1 forces alpha regardless of BLDCNT). NOT gated on EVA!=0:
        // EVA=0/EVB=16 is a valid blend (1st target fully fades into the 2nd) —
        // the Oak-intro character fade endpoint that previously snapped back to
        // opaque, leaving body/feet (BG2 + semi-transparent OBJ) out of sync.
        if ((effect == 1 || (top[x].layer == 4 && top[x].semi)) &&
            top[x].target1 && second[x].valid && second[x].target2 &&
            !(top[x].layer == 4 && second[x].layer == 4)) {
            const uint16_t blended = blend_alpha_gba555(
                top[x].color, second[x].color,
                bldalpha & 0x1Fu, (bldalpha >> 8) & 0x1Fu);
            to_rgb888(blended, dst);
        } else if ((effect == 2u || effect == 3u) && bldy != 0u && top[x].target1) {
            const uint16_t adjusted = effect == 2u
                ? brighten_gba555(top[x].color, bldy)
                : darken_gba555(top[x].color, bldy);
            to_rgb888(adjusted, dst);
        } else {
            to_rgb888(top[x].color, dst);
        }
        record_margin_final(x, top[x].layer, top[x].source);
    }
    g_ws_obj_census_line = 0;
}

}  // namespace

// Aggregate cost of the scanline rasteriser, for scoping work that would need
// to run it more than once per guest frame (frame interpolation). The windowed
// frame-phase ring cannot answer this because it only records in the present
// block; this counter works headless, which is where throughput is measured.
// Read with gba_ppu_render_ns() / gba_ppu_render_frames().
static std::uint64_t g_ppu_render_ns = 0;
static std::uint64_t g_ppu_render_frames = 0;
std::uint64_t gba_ppu_render_ns() { return g_ppu_render_ns; }
std::uint64_t gba_ppu_render_frames() { return g_ppu_render_frames; }

void GbaPpu::render(uint8_t* rgb,
                    uint16_t dispcnt,
                    const uint8_t* io,
                    const uint8_t* vram,
                    const uint8_t* oam,
                    const uint8_t* pal) const {
    const auto t0 = std::chrono::steady_clock::now();
    struct RenderTimer {
        std::chrono::steady_clock::time_point t0;
        ~RenderTimer() {
            g_ppu_render_ns += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - t0).count());
            ++g_ppu_render_frames;
        }
    } timer{t0};
    if (!view_expanded()) {
        // Faithful path — literally unchanged from before view expansion existed.
        for (uint32_t y = 0; y < kScreenHeight; ++y) {
            const int32_t* refs = affine_line_ref_valid_[y]
                ? &affine_line_ref_[y][0][0] : nullptr;
            render_scanline_internal(rgb, y, dispcnt, io, vram, oam, pal,
                                     kScreenWidth, kScreenHeight, refs);
        }
        return;
    }
    const uint32_t ow = render_width();
    const uint32_t oh = render_height();
    const WsMarginDiagnosticsCallback diagnostics_callback =
        g_ws_margin_diagnostics;
    WsMarginDiagnostics diagnostics{};
    WsMarginDiagnostics* diagnostics_ptr = diagnostics_callback
        ? &diagnostics : nullptr;
    for (uint32_t output_y = 0; output_y < oh; ++output_y) {
        const int logical_y = static_cast<int>(output_y) -
            static_cast<int>(extra_top_);
        const bool synthetic_row = logical_y < 0 ||
            logical_y >= static_cast<int>(kScreenHeight);
        const uint32_t edge_y = logical_y < 0 ? 0u : kScreenHeight - 1u;
        const uint8_t* row_io = io;
        uint16_t row_dispcnt = dispcnt;
        if (synthetic_row && line_io_valid_[edge_y]) {
            row_io = line_io_[edge_y].data();
            row_dispcnt = load_u16_le(row_io);
        }
        const int32_t* refs = nullptr;
        int ref_line = logical_y;
        std::array<int32_t, 4> synthetic_refs{};
        if (synthetic_row) {
            const uint32_t neighbor_y = logical_y < 0 ? 1u : kScreenHeight - 2u;
            if (affine_line_ref_valid_[edge_y] &&
                affine_line_ref_valid_[neighbor_y]) {
                const int64_t steps =
                    static_cast<int64_t>(logical_y) - edge_y;
                for (std::size_t i = 0; i < synthetic_refs.size(); ++i) {
                    const int32_t edge =
                        (&affine_line_ref_[edge_y][0][0])[i];
                    const int32_t neighbor =
                        (&affine_line_ref_[neighbor_y][0][0])[i];
                    const int64_t slope = logical_y < 0
                        ? static_cast<int64_t>(neighbor) - edge
                        : static_cast<int64_t>(edge) - neighbor;
                    synthetic_refs[i] = static_cast<int32_t>(
                        static_cast<int64_t>(edge) + steps * slope);
                }
                refs = synthetic_refs.data();
                // The synthesized reference already includes vertical
                // extrapolation. Prevent a second PB/PD advance below.
                ref_line = logical_y;
            } else if (affine_line_ref_valid_[edge_y]) {
                refs = &affine_line_ref_[edge_y][0][0];
                ref_line = static_cast<int>(edge_y);
            }
        } else if (affine_line_ref_valid_[logical_y]) {
            refs = &affine_line_ref_[logical_y][0][0];
        }
        const unsigned margin_policy_flags = g_ws_margin_policy
            ? g_ws_margin_policy(row_dispcnt, row_io) : 0u;
        // extra_top_/oh matter: they are how the row learns where the view
        // actually ends. Left off, the defaults describe the native 240x160
        // screen, and every rule below that asks "is this row still in view"
        // answered for the wrong screen.
        render_scanline_wide(rgb, logical_y, output_y, row_dispcnt, row_io,
                             vram, oam, pal, ow, extra_left_, 1, nullptr,
                             refs, ref_line, margin_policy_flags,
                             diagnostics_ptr, extra_top_, oh,
                             &affine_line_ref_[0][0][0],
                             affine_line_ref_valid_.data(),
                             line_io_[0].data(), line_io_valid_.data());
    }
    if (diagnostics_callback) diagnostics_callback(diagnostics);
}

void GbaPpu::render_native(uint8_t* rgb,
                           uint32_t scale,
                           uint16_t dispcnt,
                           const uint8_t* io,
                           const uint8_t* vram,
                           const uint8_t* oam,
                           const uint8_t* pal) const {
    if (!rgb) return;
    const auto t0 = std::chrono::steady_clock::now();
    struct NativeRenderTimer {
        std::chrono::steady_clock::time_point t0;
        ~NativeRenderTimer() {
            g_ppu_render_ns += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - t0).count());
            ++g_ppu_render_frames;
        }
    } timer{t0};
    if (scale == 0) scale = 1;
    if (scale > 10) scale = 10;
    const uint32_t out_w = kScreenWidth * scale;
    const std::size_t row_bytes = static_cast<std::size_t>(out_w) * 3u;
    // Native scene rendering is a final-state, opt-in presentation layer. It
    // intentionally does not alter the latched canonical framebuffer or any
    // guest timing/state; callers can fall back to render() for interpolation,
    // frame dumps, or any mode that needs per-scanline snapshots.
    // Write each logical row directly to the first row of its scaled output
    // group. The compositor evaluates affine transforms at native horizontal
    // samples; vertical samples are replicated from the corresponding
    // hardware scanline without leaving alternating source rows behind.
    for (uint32_t y = 0; y < kScreenHeight; ++y) {
        uint8_t* row = rgb + static_cast<std::size_t>(y) * scale * row_bytes;
        const int32_t* refs = affine_line_ref_valid_[y]
            ? &affine_line_ref_[y][0][0] : nullptr;
        render_scanline_wide(rgb, static_cast<int>(y), 0, dispcnt, io, vram,
                             oam, pal, out_w, 0, scale, row, refs,
                             static_cast<int>(y));
        for (uint32_t dy = 1; dy < scale; ++dy) {
            std::memcpy(rgb + (static_cast<std::size_t>(y) * scale + dy) *
                            row_bytes,
                        row, row_bytes);
        }
    }
}

bool GbaPpu::render_native_latched(uint8_t* rgb, uint32_t scale) const {
    if (!has_latched_native_state_ || !rgb) return false;
    if (scale == 0) scale = 1;
    if (scale > 10) scale = 10;
    const uint32_t out_w = kScreenWidth * scale;
    const std::size_t row_bytes = static_cast<std::size_t>(out_w) * 3u;
    for (uint32_t y = 0; y < kScreenHeight; ++y) {
        uint8_t* row = rgb + static_cast<std::size_t>(y) * scale * row_bytes;
        const bool has_line_io = latched_native_line_io_valid_[y];
        const uint8_t* line_io = has_line_io
            ? latched_native_line_io_[y].data() : latched_native_io_.data();
        const uint16_t line_dispcnt = has_line_io
            ? static_cast<uint16_t>(line_io[0] |
                (static_cast<uint16_t>(line_io[1]) << 8))
            : latched_native_dispcnt_;
        const int32_t* refs = latched_native_affine_line_ref_valid_[y]
            ? &latched_native_affine_line_ref_[y][0][0] : nullptr;
        render_scanline_wide(rgb, static_cast<int>(y), 0, line_dispcnt, line_io,
                             latched_native_vram_.data(),
                             latched_native_oam_.data(),
                             latched_native_pal_.data(), out_w, 0, scale, row,
                             refs, static_cast<int>(y));
        for (uint32_t dy = 1; dy < scale; ++dy) {
            std::memcpy(rgb + (static_cast<std::size_t>(y) * scale + dy) *
                            row_bytes,
                        row, row_bytes);
        }
    }
    return true;
}

void GbaPpu::render_captured_scene(uint8_t* rgb,
                                   const uint8_t* line_io,
                                   const int32_t* affine_line_refs,
                                   const uint8_t* vram,
                                   const uint8_t* oam,
                                   const uint8_t* pal) const {
    if (!rgb || !line_io || !affine_line_refs || !vram || !oam || !pal)
        return;
    for (uint32_t y = 0; y < kScreenHeight; ++y) {
        const uint8_t* io = line_io + static_cast<std::size_t>(y) *
            kLineIoBytes;
        const uint16_t dispcnt = static_cast<uint16_t>(io[0] |
            (static_cast<uint16_t>(io[1]) << 8));
        render_scanline_internal(rgb, y, dispcnt, io, vram, oam, pal,
                                 kScreenWidth, kScreenHeight,
                                 affine_line_refs + y * 4u);
    }
}

#if 0
    (void)oam;  // referenced below
    // Start with backdrop (palette entry 0) for every pixel. Backdrop
    // is the BG palette index 0, at PAL[0..1].
    uint16_t backdrop = load_u16_le(&pal[0]);
    uint8_t bd_rgb[3];
    to_rgb888(backdrop, bd_rgb);
    for (uint32_t y = 0; y < kScreenHeight; ++y) {
        for (uint32_t x = 0; x < kScreenWidth; ++x) {
            uint8_t* p = rgb + (y * kScreenWidth + x) * 3;
            p[0] = bd_rgb[0]; p[1] = bd_rgb[1]; p[2] = bd_rgb[2];
        }
    }

    // Forced blank (DISPCNT bit 7): output all-white per hardware.
    if (dispcnt & 0x0080u) {
        std::memset(rgb, 0xFF, kFramebufferBytes);
        return;
    }

    // Render background layers BEFORE OBJ so sprites composite on top.
    // Phase 2.5 scope: BG3 affine for BG mode 2 (GBA BIOS Nintendo
    // logo intro uses this). Other BG configurations land later.
    uint32_t bg_mode = dispcnt & 0x07u;
    bool bg3_enabled = (dispcnt & 0x0800u) != 0;
    if (bg3_enabled && (bg_mode == 1 || bg_mode == 2)) {
        // BG3 controls: BG3CNT at 0x0E, params at 0x30..0x3F.
        render_affine_bg(rgb, io, vram, pal,
                         0x0E, 0x30,
                         kScreenWidth, kScreenHeight);
    }

    // OBJ disabled?
    bool obj_enabled = (dispcnt & 0x1000u) != 0;
    if (!obj_enabled) return;

    // OBJ tile data starts at VRAM 0x10000 in tile modes (0/1/2) and
    // at VRAM 0x14000 in bitmap modes (3/4/5).
    uint32_t obj_tile_base = (bg_mode >= 3) ? 0x14000u : 0x10000u;
    bool obj_1d_mapping = (dispcnt & 0x0040u) != 0;

    // OBJ palette starts at PAL[0x200..0x3FF].
    const uint8_t* obj_pal = pal + 0x200;

    // Walk 128 OAM entries in priority-table order. For Phase 2.4 we
    // ignore priority bits and just back-to-front-draw so higher
    // OAM indices appear on top — close enough for the BIOS intro.
    for (int idx = 127; idx >= 0; --idx) {
        const uint8_t* entry = oam + idx * 8;
        uint16_t attr0 = load_u16_le(&entry[0]);
        uint16_t attr1 = load_u16_le(&entry[2]);
        uint16_t attr2 = load_u16_le(&entry[4]);

        bool rot_scale = (attr0 & 0x0100u) != 0;
        bool disable_or_double = (attr0 & 0x0200u) != 0;
        // For non-affine sprites bit 9 is the disable bit; for affine
        // sprites it's the double-size flag (handled in the affine
        // pass). Skip disabled non-affine sprites so the BIOS intro
        // screen blanks correctly once the BIOS clears the wordmark
        // by setting these bits.
        if (!rot_scale && disable_or_double) continue;

        // OBJ Mode (attr0 bits 10-11) per GBATEK § "GBA OBJs — OAM
        // Attribute 0":
        //   0 = Normal  (visible pixel)
        //   1 = Semi-Transparent (alpha-blend with BLDALPHA)
        //   2 = OBJ Window (the sprite's opaque pixels define a
        //       window region; sprite itself is NOT drawn)
        //   3 = Prohibited
        // Without this check the BIOS intro's OBJ-Window stencil
        // sprites leak into the visible frame as garbage pink pixels.
        uint32_t obj_mode = (attr0 >> 10) & 0x3u;
        if (obj_mode == 2) continue;            // window stencil — invisible
        if (obj_mode == 3) continue;            // prohibited
        // Semi-transparent (mode 1) renders as normal for now; proper
        // alpha blending lands when we wire BLDCNT / BLDALPHA.

        uint32_t shape = (attr0 >> 14) & 0x3u;
        if (shape >= 3) continue;
        uint32_t size  = (attr1 >> 14) & 0x3u;
        int sw = kSpriteWH[shape][size][0];
        int sh = kSpriteWH[shape][size][1];

        int sy = static_cast<int>(attr0 & 0xFFu);
        int sx = static_cast<int>(attr1 & 0x1FFu);
        // Y wraps at 256 (GBATEK).
        if (sy >= 160) sy -= 256;
        // X is 9 bits signed.
        if (sx & 0x100) sx -= 0x200;

        bool color256 = (attr0 & 0x2000u) != 0;
        uint32_t tile_num = attr2 & 0x3FFu;
        uint32_t palette_bank = (attr2 >> 12) & 0xFu;

        // For 1D mapping each row of tiles is contiguous in VRAM.
        // For 2D mapping each row of OBJ tiles is 32 tiles wide.
        int tiles_w = sw / 8;
        int tiles_h = sh / 8;

        // Texel sampler shared by affine + non-affine paths. Returns
        // false on transparent / out-of-VRAM, otherwise writes the
        // RGB888 pixel at (screen_x, screen_y).
        auto sample_and_emit = [&](int tex_x, int tex_y,
                                   int screen_x, int screen_y) {
            int tile_x_in_sprite = tex_x >> 3;
            int tile_y_in_sprite = tex_y >> 3;
            int px_in_tile       = tex_x & 7;
            int py_in_tile       = tex_y & 7;

            uint32_t this_tile;
            if (obj_1d_mapping) {
                this_tile = tile_num + (tile_y_in_sprite * tiles_w + tile_x_in_sprite) *
                                            (color256 ? 2u : 1u);
            } else {
                this_tile = tile_num + (tile_y_in_sprite * 32u) +
                            tile_x_in_sprite * (color256 ? 2u : 1u);
            }
            uint32_t tile_off = obj_tile_base + this_tile * 32u;

            uint8_t pal_index;
            if (color256) {
                uint32_t off = tile_off + py_in_tile * 8 + px_in_tile;
                if (off + 1 > 96u * 1024u) return;
                pal_index = vram[off];
                if (pal_index == 0) return;  // transparent
            } else {
                uint32_t off = tile_off + py_in_tile * 4 + (px_in_tile / 2);
                if (off + 1 > 96u * 1024u) return;
                uint8_t b = vram[off];
                pal_index = (px_in_tile & 1) ? (b >> 4) : (b & 0x0F);
                if (pal_index == 0) return;  // transparent
                pal_index = static_cast<uint8_t>(pal_index | (palette_bank << 4));
            }

            uint16_t color = load_u16_le(&obj_pal[pal_index * 2]);
            uint8_t* dst = rgb + (screen_y * kScreenWidth + screen_x) * 3;
            to_rgb888(color, dst);
        };

        if (rot_scale) {
            // Affine sprite. Bounding box is 2x the sprite size when
            // the double-size flag is set, otherwise = sprite size.
            int bw = disable_or_double ? sw * 2 : sw;
            int bh = disable_or_double ? sh * 2 : sh;

            int affine_group = (attr1 >> 9) & 0x1Fu;
            const uint8_t* ag = oam + affine_group * 0x20u;
            // PA/PB/PC/PD live at offsets 0x06, 0x0E, 0x16, 0x1E within
            // the 32-byte affine block (the other bytes belong to OBJ
            // attr0/1/2 entries that share the same 8-byte slots).
            int32_t pa = read_s16(ag, 0x06);
            int32_t pb = read_s16(ag, 0x0E);
            int32_t pc = read_s16(ag, 0x16);
            int32_t pd = read_s16(ag, 0x1E);

            int half_bw = bw >> 1;
            int half_bh = bh >> 1;
            int half_sw = sw >> 1;
            int half_sh = sh >> 1;

            for (int j = 0; j < bh; ++j) {
                int screen_y = sy + j;
                if (screen_y < 0 || screen_y >= static_cast<int>(kScreenHeight)) continue;
                int dy = j - half_bh;
                for (int i = 0; i < bw; ++i) {
                    int screen_x = sx + i;
                    if (screen_x < 0 || screen_x >= static_cast<int>(kScreenWidth)) continue;
                    int dx = i - half_bw;

                    // (tex_x, tex_y) = matrix * (dx, dy) + sprite_center.
                    int tex_x = ((pa * dx + pb * dy) >> 8) + half_sw;
                    int tex_y = ((pc * dx + pd * dy) >> 8) + half_sh;
                    if (tex_x < 0 || tex_x >= sw) continue;
                    if (tex_y < 0 || tex_y >= sh) continue;

                    sample_and_emit(tex_x, tex_y, screen_x, screen_y);
                }
            }
            continue;
        }

        // Non-affine sprite from here on.
        bool hflip = (attr1 & 0x1000u) != 0;
        bool vflip = (attr1 & 0x2000u) != 0;

        for (int ty = 0; ty < tiles_h; ++ty) {
            for (int tx = 0; tx < tiles_w; ++tx) {
                // Compute the source tile index, taking flips into
                // account (flipping the tile *layout* on top of
                // per-pixel flip).
                int src_tx = hflip ? (tiles_w - 1 - tx) : tx;
                int src_ty = vflip ? (tiles_h - 1 - ty) : ty;

                uint32_t this_tile;
                if (obj_1d_mapping) {
                    this_tile = tile_num + (src_ty * tiles_w + src_tx) *
                                                (color256 ? 2 : 1);
                } else {
                    // 2D mapping: the OBJ tile area is always 32
                    // *slots* wide regardless of color depth. Each
                    // 4bpp tile occupies 1 slot; each 8bpp tile
                    // occupies 2 horizontally-adjacent slots.
                    // Row stride in slot units is always 32.
                    this_tile = tile_num + (src_ty * 32u) +
                                src_tx * (color256 ? 2u : 1u);
                }
                // OBJ tile numbers are in 32-byte *slot* units
                // regardless of color depth. An 8bpp visible tile
                // (64 bytes) occupies 2 consecutive slots.
                uint32_t tile_off = obj_tile_base + this_tile * 32u;

                // Per-pixel emit.
                for (int py = 0; py < 8; ++py) {
                    int screen_y = sy + ty * 8 + py;
                    if (screen_y < 0 || screen_y >= static_cast<int>(kScreenHeight)) continue;
                    int src_py = vflip ? (7 - py) : py;
                    for (int px = 0; px < 8; ++px) {
                        int screen_x = sx + tx * 8 + px;
                        if (screen_x < 0 || screen_x >= static_cast<int>(kScreenWidth)) continue;
                        int src_px = hflip ? (7 - px) : px;

                        uint8_t pal_index;
                        if (color256) {
                            // 1 byte per pixel.
                            uint32_t off = tile_off + src_py * 8 + src_px;
                            if (off + 1 > 96 * 1024) continue;
                            pal_index = vram[off];
                        } else {
                            // 4bpp: 4 bytes per row, low nibble = even
                            // pixel, high nibble = odd pixel.
                            uint32_t off = tile_off + src_py * 4 + (src_px / 2);
                            if (off + 1 > 96 * 1024) continue;
                            uint8_t b = vram[off];
                            pal_index = (src_px & 1) ? (b >> 4) : (b & 0x0F);
                            if (pal_index == 0) continue;  // transparent
                            pal_index |= (palette_bank << 4);
                        }
                        if (color256 && pal_index == 0) continue;

                        uint16_t color = load_u16_le(&obj_pal[pal_index * 2]);
                        uint8_t* dst = rgb +
                            (screen_y * kScreenWidth + screen_x) * 3;
                        to_rgb888(color, dst);
                    }
                }
            }
        }
    }
}

#endif

void GbaPpu::render_scanline(uint32_t y,
                             uint16_t dispcnt,
                             const uint8_t* io,
                             const uint8_t* vram,
                             const uint8_t* oam,
                             const uint8_t* pal) {
    // This, not render(), is the entry the headless/run loop uses — one call
    // per scanline. Count scanlines; the caller divides by kScreenHeight to get
    // a per-frame figure.
    struct ScanTimer {
        std::chrono::steady_clock::time_point t0 =
            std::chrono::steady_clock::now();
        ~ScanTimer() {
            g_ppu_render_ns += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - t0).count());
            ++g_ppu_render_frames;
        }
    } timer;
    if (!affine_ref_valid_) reload_affine_references(io);
    if (y < kScreenHeight) {
        if (y == 0) {
            affine_line_ref_valid_.fill(false);
            line_io_valid_.fill(false);
        }
        affine_line_ref_[y] = affine_ref_;
        affine_line_ref_valid_[y] = true;
        std::memcpy(line_io_[y].data(), io, kLineIoBytes);
        // The caller supplies DISPCNT explicitly because it is read through
        // the bus at the HBlank boundary. Keep that authoritative value in
        // the compact line snapshot even for callers whose IO view omits the
        // mirrored register bytes.
        line_io_[y][0] = static_cast<uint8_t>(dispcnt);
        line_io_[y][1] = static_cast<uint8_t>(dispcnt >> 8);
        line_io_valid_[y] = true;
    }
    const int32_t* refs = (y < kScreenHeight)
        ? &affine_ref_[0][0] : nullptr;
    // GoldenSunRecomp edit: everything above this point is bookkeeping the
    // GPU renderer depends on -- this row's registers, its affine reference
    // -- and everything below is the drawing itself. Only the drawing is
    // skipped (MODIFICATIONS.md). The margin policy call in the expanded
    // branch is kept either way: it is how the game adapter learns what this
    // frame is, not part of putting pixels down.
    if (!native_raster_enabled_) {
        if (view_expanded() && y < kScreenHeight && g_ws_margin_policy)
            (void)g_ws_margin_policy(dispcnt, io);
    } else if (!view_expanded()) {
        wide_margin_diagnostics_ = {};
        wide_margin_diagnostics_callback_ = nullptr;
        wide_margin_diagnostics_active_ = false;
        render_scanline_internal(latched_fb_.data(), y, dispcnt, io, vram, oam,
                                 pal, kScreenWidth, kScreenHeight, refs);
    } else if (y < kScreenHeight) {
        if (y == 0 || !wide_margin_diagnostics_active_ ||
            wide_margin_diagnostics_callback_ != g_ws_margin_diagnostics) {
            wide_margin_diagnostics_ = {};
            wide_margin_diagnostics_callback_ = g_ws_margin_diagnostics;
            wide_margin_diagnostics_active_ =
                wide_margin_diagnostics_callback_ != nullptr;
        }
        const unsigned margin_policy_flags = (y < kScreenHeight &&
                                               g_ws_margin_policy)
            ? g_ws_margin_policy(dispcnt, io) : 0u;
        // The policy call above runs either way: it is how the game adapter
        // learns what this frame is, including whether to defer it.
        if (!g_ws_defer_native_rows) {
            render_scanline_wide(
                latched_fb_.data(), static_cast<int>(y), y + extra_top_,
                dispcnt, io, vram, oam, pal, render_width(), extra_left_, 1,
                nullptr, refs, static_cast<int>(y), margin_policy_flags,
                wide_margin_diagnostics_active_ ? &wide_margin_diagnostics_
                                                : nullptr,
                extra_top_, render_height());
        }
    }
    for (uint32_t bg = 0; bg < 2; ++bg) {
        const uint32_t off = 0x20u + bg * 0x10u;
        affine_ref_[bg][0] += read_s16(io, off + 2u);
        affine_ref_[bg][1] += read_s16(io, off + 6u);
    }
}

void GbaPpu::latch_framebuffer(uint16_t dispcnt,
                               const uint8_t* io,
                               const uint8_t* vram,
                               const uint8_t* oam,
                               const uint8_t* pal) {
    render(latched_fb_.data(), dispcnt, io, vram, oam, pal);
    latch_native_scene_state(dispcnt, io, vram, oam, pal);
    has_latched_fb_ = true;
}

void GbaPpu::mark_framebuffer_latched() {
    // The run loop has already rendered the 160 authentic scanlines one at a
    // time. Synthesize only the host rows outside that raster from the VBlank
    // snapshot captured immediately before this call.
    if (view_expanded() && has_latched_native_state_ &&
        native_raster_enabled_) {
        if (g_ws_defer_native_rows) render_deferred_native_rows();
        render_vertical_margin_rows();
        if (g_ws_row_state_dump_mode >= 0) {
            dump_row_state(latched_native_dispcnt_);
        }
    }
    has_latched_fb_ = true;
    if (wide_margin_diagnostics_active_ &&
        wide_margin_diagnostics_callback_ != nullptr) {
        wide_margin_diagnostics_callback_(wide_margin_diagnostics_);
    }
    wide_margin_diagnostics_ = {};
    wide_margin_diagnostics_callback_ = nullptr;
    wide_margin_diagnostics_active_ = false;
}

void GbaPpu::latch_native_scene_state(uint16_t dispcnt,
                                      const uint8_t* io,
                                      const uint8_t* vram,
                                      const uint8_t* oam,
                                      const uint8_t* pal) {
    if (!io || !vram || !oam || !pal) {
        has_latched_native_state_ = false;
        return;
    }
    latched_native_dispcnt_ = dispcnt;
    std::memcpy(latched_native_io_.data(), io, latched_native_io_.size());
    std::memcpy(latched_native_vram_.data(), vram, latched_native_vram_.size());
    std::memcpy(latched_native_oam_.data(), oam, latched_native_oam_.size());
    std::memcpy(latched_native_pal_.data(), pal, latched_native_pal_.size());
    latched_native_line_io_ = line_io_;
    latched_native_line_io_valid_ = line_io_valid_;
    latched_native_affine_line_ref_ = affine_line_ref_;
    latched_native_affine_line_ref_valid_ = affine_line_ref_valid_;
    has_latched_native_state_ = true;
}

void GbaPpu::dump_row_state(uint16_t dispcnt) {
    if (g_ws_row_state_dump_mode >= 0 &&
    static_cast<int>(dispcnt & 0x07u) == g_ws_row_state_dump_mode) {
    // Two files, because the two questions are different. battle_frames
    // is one line per frame for the whole session: enough to find the
    // moment a screen looks wrong and to see which registers moved
    // between frames. battle_rows is the full 160 rows, written once for
    // each distinct screen (a new combination of display mode, layer
    // controls, camera scale and window band), which is what says whether
    // the game is animating a register WITHIN the frame -- the thing a
    // per-frame trace cannot show and the thing a magnified layer has to
    // follow.
    static unsigned long long dump_frame = 0;
    static unsigned long long summary_lines = 0;
    static int detail_blocks = 0;
    static unsigned long long last_signature = ~0ull;
    ++dump_frame;

    auto u16_at = [](const uint8_t* r, uint32_t off) {
        return static_cast<unsigned>(r[off] | (r[off + 1] << 8));
    };
    auto s16_at = [](const uint8_t* r, uint32_t off) {
        return static_cast<int>(read_s16(r, off));
    };
    const uint8_t* row0 = line_io_valid_[0] ? line_io_[0].data()
                                             : latched_native_io_.data();
    // How many rows differ from the first one: nonzero means the game is
    // changing that register mid-frame.
    unsigned vary_bg1hofs = 0, vary_bg2pa = 0, vary_bg2x = 0, vary_disp = 0;
    for (uint32_t y = 1; y < kScreenHeight; ++y) {
        if (!line_io_valid_[y]) continue;
        const uint8_t* r = line_io_[y].data();
        if (u16_at(r, 0x14) != u16_at(row0, 0x14)) ++vary_bg1hofs;
        if (u16_at(r, 0x20) != u16_at(row0, 0x20)) ++vary_bg2pa;
        if (u16_at(r, 0x00) != u16_at(row0, 0x00)) ++vary_disp;
        if (affine_line_ref_valid_[y] && affine_line_ref_valid_[0] &&
            affine_line_ref_[y][0][0] != affine_line_ref_[0][0][0]) {
            ++vary_bg2x;
        }
    }
    if (summary_lines < 20000ull) {
        if (FILE* f = std::fopen("logs/battle_frames.csv", "a")) {
            if (summary_lines == 0) {
                std::fprintf(f,
                    "frame,dispcnt,bg0cnt,bg1cnt,bg2cnt,bg1hofs,bg1vofs,"
                    "bg2pa,bg2pd,bg2x,bg2y,win0h,win0v,winin,winout,"
                    "bldcnt,rows_vary_bg1hofs,rows_vary_bg2pa,"
                    "rows_vary_bg2x,rows_vary_dispcnt\n");
            }
            ++summary_lines;
            std::fprintf(f,
                "%llu,%u,%u,%u,%u,%u,%u,%d,%d,%d,%d,%u,%u,%u,%u,%u,"
                "%u,%u,%u,%u\n",
                dump_frame, u16_at(row0, 0x00), u16_at(row0, 0x08),
                u16_at(row0, 0x0A), u16_at(row0, 0x0C), u16_at(row0, 0x14),
                u16_at(row0, 0x16), s16_at(row0, 0x20), s16_at(row0, 0x26),
                affine_line_ref_valid_[0] ? affine_line_ref_[0][0][0] : 0,
                affine_line_ref_valid_[0] ? affine_line_ref_[0][0][1] : 0,
                u16_at(row0, 0x40), u16_at(row0, 0x44), u16_at(row0, 0x48),
                u16_at(row0, 0x4A), u16_at(row0, 0x50),
                vary_bg1hofs, vary_bg2pa, vary_bg2x, vary_disp);
            std::fclose(f);
        }
    }
    // One full-row block per distinct screen, capped.
    // BG2PA is deliberately NOT part of this: the camera push-in steps it a
    // little every frame, so including it spent all 16 blocks on the first 18
    // frames of the battle and never reached the screens worth looking at
    // (session_20260912_103331). A screen is the display mode, the layer
    // controls and the window band.
    const unsigned long long signature =
        (static_cast<unsigned long long>(u16_at(row0, 0x00)) << 48) ^
        (static_cast<unsigned long long>(u16_at(row0, 0x0A)) << 32) ^
        (static_cast<unsigned long long>(u16_at(row0, 0x0C)) << 16) ^
        (static_cast<unsigned long long>(u16_at(row0, 0x44)));
    if (signature != last_signature &&
        detail_blocks < g_ws_row_state_dump_frames) {
        last_signature = signature;
        ++detail_blocks;
        if (FILE* f = std::fopen("logs/battle_rows.csv", "a")) {
            if (detail_blocks == 1) {
                std::fprintf(f,
                    "frame,row,io_valid,dispcnt,bg0cnt,bg1cnt,bg2cnt,"
                    "bg0hofs,bg0vofs,bg1hofs,bg1vofs,"
                    "bg2pa,bg2pb,bg2pc,bg2pd,ref_valid,bg2x,bg2y,"
                    "win0h,win0v,winin,winout,bldcnt,bldalpha\n");
            }
            for (uint32_t y = 0; y < kScreenHeight; ++y) {
                const bool iv = line_io_valid_[y];
                const uint8_t* r = iv ? line_io_[y].data()
                                      : latched_native_io_.data();
                std::fprintf(f,
                    "%llu,%u,%d,%u,%u,%u,%u,%u,%u,%u,%u,%d,%d,%d,%d,%d,"
                    "%d,%d,%u,%u,%u,%u,%u,%u\n",
                    dump_frame, y, iv ? 1 : 0, u16_at(r, 0x00),
                    u16_at(r, 0x08), u16_at(r, 0x0A), u16_at(r, 0x0C),
                    u16_at(r, 0x10), u16_at(r, 0x12), u16_at(r, 0x14),
                    u16_at(r, 0x16), s16_at(r, 0x20), s16_at(r, 0x22),
                    s16_at(r, 0x24), s16_at(r, 0x26),
                    affine_line_ref_valid_[y] ? 1 : 0,
                    affine_line_ref_valid_[y] ? affine_line_ref_[y][0][0] : 0,
                    affine_line_ref_valid_[y] ? affine_line_ref_[y][0][1] : 0,
                    u16_at(r, 0x40), u16_at(r, 0x44), u16_at(r, 0x48),
                    u16_at(r, 0x4A), u16_at(r, 0x50), u16_at(r, 0x52));
            }
            std::fclose(f);
        }
        // What each canvas row actually ended up showing, for the same
        // screen: per row, how many pixels each layer won. Read alongside
        // battle_rows.csv -- that file says what the game asked for, this one
        // says what came out.
        if (FILE* f = std::fopen("logs/battle_layers.csv", "a")) {
            if (detail_blocks == 1) {
                std::fprintf(f,
                    "frame,canvas_row,bg0,bg1,bg2,bg3,obj,backdrop,other\n");
            }
            const std::size_t rows =
                std::min<std::size_t>(render_height(), kRowCensusRows);
            for (std::size_t y = 0; y < rows; ++y) {
                std::fprintf(f, "%llu,%zu,%u,%u,%u,%u,%u,%u,%u\n",
                             dump_frame, y, g_row_census[y][0],
                             g_row_census[y][1], g_row_census[y][2],
                             g_row_census[y][3], g_row_census[y][4],
                             g_row_census[y][5], g_row_census[y][6]);
            }
            std::fclose(f);
        }
    }
    }
    // Every frame, dumped or not: the census counts one frame at a time.
    std::memset(g_row_census, 0, sizeof(g_row_census));
}

void GbaPpu::render_deferred_native_rows() {
    if (!view_expanded() || !has_latched_native_state_) return;
    const uint32_t out_w = render_width();
    const uint32_t out_h = render_height();
    for (uint32_t y = 0; y < kScreenHeight; ++y) {
        const uint8_t* row_io = latched_native_line_io_valid_[y]
            ? latched_native_line_io_[y].data() : latched_native_io_.data();
        const uint16_t row_dispcnt = latched_native_line_io_valid_[y]
            ? load_u16_le(row_io) : latched_native_dispcnt_;
        const int32_t* refs = latched_native_affine_line_ref_valid_[y]
            ? &latched_native_affine_line_ref_[y][0][0] : nullptr;
        const unsigned margin_policy_flags = g_ws_margin_policy
            ? g_ws_margin_policy(row_dispcnt, row_io) : 0u;
        render_scanline_wide(
            latched_fb_.data(), static_cast<int>(y), y + extra_top_,
            row_dispcnt, row_io, latched_native_vram_.data(),
            latched_native_oam_.data(), latched_native_pal_.data(), out_w,
            extra_left_, 1, nullptr, refs, static_cast<int>(y),
            margin_policy_flags,
            wide_margin_diagnostics_active_ ? &wide_margin_diagnostics_
                                            : nullptr,
            extra_top_, out_h,
            &latched_native_affine_line_ref_[0][0][0],
            latched_native_affine_line_ref_valid_.data(),
            latched_native_line_io_[0].data(),
            latched_native_line_io_valid_.data());
    }
}

void GbaPpu::render_vertical_margin_rows() {
    if (!view_expanded() || (extra_top_ == 0 && extra_bottom_ == 0) ||
        !has_latched_native_state_) {
        return;
    }

    const uint32_t out_w = render_width();
    const uint32_t out_h = render_height();
    auto render_margin_row = [&](uint32_t output_y) {
        const int logical_y = static_cast<int>(output_y) -
            static_cast<int>(extra_top_);
        if (logical_y >= 0 && logical_y < static_cast<int>(kScreenHeight))
            return;

        const uint32_t edge_y = logical_y < 0 ? 0u : kScreenHeight - 1u;
        const uint8_t* line_io = latched_native_io_.data();
        uint16_t line_dispcnt = latched_native_dispcnt_;
        if (latched_native_line_io_valid_[edge_y]) {
            line_io = latched_native_line_io_[edge_y].data();
            line_dispcnt = load_u16_le(line_io);
        }
        const uint32_t neighbor_y = logical_y < 0 ? 1u : kScreenHeight - 2u;
        std::array<int32_t, 4> synthetic_refs{};
        const int32_t* refs = nullptr;
        int ref_line = static_cast<int>(edge_y);
        if (latched_native_affine_line_ref_valid_[edge_y] &&
            latched_native_affine_line_ref_valid_[neighbor_y]) {
            const int64_t steps = static_cast<int64_t>(logical_y) - edge_y;
            for (std::size_t i = 0; i < synthetic_refs.size(); ++i) {
                const int32_t edge =
                    (&latched_native_affine_line_ref_[edge_y][0][0])[i];
                const int32_t neighbor =
                    (&latched_native_affine_line_ref_[neighbor_y][0][0])[i];
                const int64_t slope = logical_y < 0
                    ? static_cast<int64_t>(neighbor) - edge
                    : static_cast<int64_t>(edge) - neighbor;
                synthetic_refs[i] = static_cast<int32_t>(
                    static_cast<int64_t>(edge) + steps * slope);
            }
            refs = synthetic_refs.data();
            ref_line = logical_y;
        } else if (latched_native_affine_line_ref_valid_[edge_y]) {
            refs = &latched_native_affine_line_ref_[edge_y][0][0];
        }
        const unsigned margin_policy_flags = g_ws_margin_policy
            ? g_ws_margin_policy(line_dispcnt, line_io) : 0u;
        render_scanline_wide(
            latched_fb_.data(), logical_y, output_y, line_dispcnt, line_io,
            latched_native_vram_.data(), latched_native_oam_.data(),
            latched_native_pal_.data(), out_w, extra_left_, 1, nullptr, refs,
            ref_line, margin_policy_flags,
            wide_margin_diagnostics_active_ ? &wide_margin_diagnostics_ : nullptr,
            // The top/bottom bands are rendered here, separately from the
            // native rows. They need the same expanded geometry, or object
            // culling in the vertical margins judges against a 240x160 view
            // and drops or misplaces sprites that belong there.
            extra_top_, out_h);
    };

    for (uint32_t output_y = 0; output_y < extra_top_; ++output_y)
        render_margin_row(output_y);
    const uint32_t bottom_start = extra_top_ + kScreenHeight;
    for (uint32_t output_y = bottom_start; output_y < out_h; ++output_y)
        render_margin_row(output_y);
}

}  // namespace gba
