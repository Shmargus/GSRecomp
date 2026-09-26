// gba_ppu.h — GBA PPU minimal state.
//
// Phase 2.3 scope: enough PPU to advance VCOUNT and the DISPSTAT
// VBlank/HBlank/VCount-match flags. Real rendering (BG modes,
// sprites, blending, etc.) lands in Phase 2.4+.
//
// Cycle accounting per GBATEK § "GBA Picture Processing Unit":
//   1 dot           = 4 cycles
//   HDraw window    = 252 dots (1008 cycles) per scanline
//   HBlank window   = 56 dots  (224 cycles) per scanline
//   scanline total  = 308 dots / 1232 cycles
//   visible lines   = 160
//   VBlank lines    = 68 (scanlines 160..227)
//   frame total     = 228 scanlines = 280896 cycles
//
// VCOUNT increments at the start of each scanline. HBlank flag sets
// at dot 252 (cycle 1008 of the line), clears at start of next line.

#pragma once

#include <array>
#include <cstdint>
#include <cstddef>

namespace gbarecomp::debug { class SnapshotWriter; class SnapshotReader; }

namespace gba {

// Payload-free accounting for the expanded-margin compositor. These enums are
// deliberately a small ABI contract: a game-side observer can aggregate the
// result without seeing a tile entry, palette value, or framebuffer byte.
enum WsMarginProviderTraceResult : std::size_t {
    kWsMarginProviderReplace = 0,
    kWsMarginProviderKeepWrapped = 1,
    kWsMarginProviderUnavailable = 2,
    kWsMarginProviderTraceResultCount = 3,
};

// Final compositor layer selected for a margin pixel. The last three values
// describe non-layer outcomes rather than guest layers.
enum WsMarginTraceLayer : std::size_t {
    kWsMarginTraceBg0 = 0,
    kWsMarginTraceBg1 = 1,
    kWsMarginTraceBg2 = 2,
    kWsMarginTraceBg3 = 3,
    kWsMarginTraceObj = 4,
    kWsMarginTraceBackdrop = 5,
    kWsMarginTracePillarbox = 6,
    kWsMarginTraceForcedBlank = 7,
    kWsMarginTraceLayerCount = 8,
};

// Source selected for the final margin pixel. `wrapped` is a normal resident
// tilemap entry (including the no-provider path); `keep_wrapped` proves that a
// provider explicitly retained that entry. The matrix in WsMarginDiagnostics
// preserves the layer/source pairing needed to attribute a coloured seam.
enum WsMarginTraceSource : std::size_t {
    kWsMarginSourceWrapped = 0,
    kWsMarginSourceProviderReplace = 1,
    kWsMarginSourceProviderKeepWrapped = 2,
    kWsMarginSourceObj = 3,
    kWsMarginSourceBackdrop = 4,
    kWsMarginSourcePillarbox = 5,
    kWsMarginSourceForcedBlank = 6,
    kWsMarginTraceSourceCount = 7,
};

struct WsMarginDiagnostics {
    std::uint64_t margin_pixels = 0;
    std::uint64_t left_margin_pixels = 0;
    std::uint64_t right_margin_pixels = 0;
    std::uint64_t top_margin_pixels = 0;
    std::uint64_t bottom_margin_pixels = 0;

    // [BG][replace, keep-wrapped, unavailable]. An unavailable result means
    // the provider was queried and declined that sample; it is not a wrapped
    // fallback.
    std::array<std::array<std::uint64_t,
                          kWsMarginProviderTraceResultCount>, 4>
        provider_results{};

    // [final layer][final source] over all margin pixels.
    std::array<std::array<std::uint64_t, kWsMarginTraceSourceCount>,
               kWsMarginTraceLayerCount>
        final_selected{};

    // Same layer/source matrix for horizontal left (0) and right (1) margins.
    // Vertical-only rows are intentionally absent from this matrix; the global
    // matrix above still includes them.
    std::array<std::array<std::array<std::uint64_t,
                                     kWsMarginTraceSourceCount>,
                              kWsMarginTraceLayerCount>, 2>
        horizontal_final_selected{};
};

using WsMarginDiagnosticsCallback = void (*)(const WsMarginDiagnostics&);

// Optional observer, normally installed only by a game's diagnostics build.
// A null pointer keeps the compositor's hot path free of accounting work.
extern "C" WsMarginDiagnosticsCallback g_ws_margin_diagnostics;

// Aggregate scanline-rasteriser cost, readable headless. Used to scope frame
// interpolation, which would need to run render() more than once per guest
// frame; the windowed frame-phase ring cannot answer that question because it
// only records inside the present block.
std::uint64_t gba_ppu_render_ns();
std::uint64_t gba_ppu_render_frames();

class GbaPpu {
public:
    static constexpr std::size_t kLineIoBytes = 0x58;
    // Hardware constants.
    static constexpr uint32_t kDotsVisible      = 252;
    static constexpr uint32_t kDotsHBlank       = 56;
    static constexpr uint32_t kDotsPerScanline  = kDotsVisible + kDotsHBlank;  // 308
    static constexpr uint32_t kCyclesPerDot     = 4;
    static constexpr uint32_t kCyclesPerScanline =
        kDotsPerScanline * kCyclesPerDot;                                       // 1232
    static constexpr uint32_t kLinesVisible     = 160;
    static constexpr uint32_t kLinesTotal       = 228;
    static constexpr uint32_t kCyclesPerFrame   =
        kLinesTotal * kCyclesPerScanline;                                       // 280896

    GbaPpu();
    ~GbaPpu();

    // Events that fired during a tick. The caller routes these to
    // the IRQ controller (gating on the DISPSTAT enable bits the BIOS
    // sets). Keeping events as a return value rather than a callback
    // keeps the PPU pure / portable.
    struct TickEvents {
        bool vblank_started = false;  // scanline crossed from 159 → 160
        bool hblank_started = false;  // dot crossed visible → HBlank
        bool vcount_matched = false;  // scanline became == compare value
        bool frame_completed = false; // scanline wrapped 227 → 0
    };

    // Advance the PPU by `cycles` host cycles. Increments dot/scanline
    // counters and toggles VBlank/HBlank flags appropriately. The
    // caller (scheduler) accumulates the cycle budget per CPU step.
    //
    // `vcount_compare` is the value DISPSTAT[15:8] specifies; we
    // surface VCount-match in the returned events.
    TickEvents tick(uint32_t cycles, uint16_t vcount_compare = 0xFFFFu);

    // Live state for IO reads.
    uint16_t vcount() const { return static_cast<uint16_t>(vcount_); }
    bool     in_vblank() const { return scanline_ >= kLinesVisible; }
    bool     in_hblank() const { return dot_in_scanline_ >= kDotsVisible; }
    uint32_t cycles_until_next_event() const;

    // VCount-match: returns true when current VCOUNT equals the value
    // configured in DISPSTAT bits 8..15 (the IO layer holds that
    // value; we just expose vcount() for the comparison).
    // For Phase 2.3 the bus owns the comparison; this stays simple.

    // Reset to scanline 0, dot 0. Used by the runtime on cold boot.
    void reset();

    // Save-state serialization (all internal timing + latched frame).
    // No live pointers to skip. See debug/snapshot.h.
    void serialize(gbarecomp::debug::SnapshotWriter& w) const;
    void deserialize(gbarecomp::debug::SnapshotReader& r);

    // Total frames completed since reset — useful for sync points.
    uint64_t frame_count() const { return frame_count_; }

    // Render the current frame into a 240*160 RGB888 buffer.
    // Phase 2.4 scope: OBJ (sprite) rendering for non-affine
    // sprites only. BG layers and affine sprites land in 2.5+.
    static constexpr uint32_t kScreenWidth  = 240;
    static constexpr uint32_t kScreenHeight = 160;
    static constexpr std::size_t kFramebufferBytes =
        kScreenWidth * kScreenHeight * 3;

    // ── View-area expansion (opt-in enhancement; default OFF = faithful) ──
    // Generic "render extra margin columns/rows" runner capability. With every
    // margin 0 the PPU runs the LITERAL vanilla path (render_scanline_internal),
    // so OFF-mode is byte-identical to the faithful build BY CONSTRUCTION — not
    // by algebraic equivalence (per the widescreen design review). Horizontal
    // widening reuses each scanline's latched register state. Vertical widening
    // synthesizes host rows from the nearest captured edge scanline, preserving
    // signed logical coordinates while keeping VCOUNT/HBlank state unchanged.
    // These margins are present-time host state and are NEVER serialized into
    // the snapshot (the save format is unchanged).
    // Experimental envelope: 120 horizontal and 40 vertical pixels per side
    // gives a 480x240 maximum while the default remains literal 240x160.
    static constexpr uint32_t kMaxExtraX = 120;
    static constexpr uint32_t kMaxExtraY = 40;
    // Native compositor output is supersampled horizontally (up to 10x). Keep
    // that envelope separate from the established 480px widened-view limit;
    // the faithful/widescreen framebuffer and resize policy remain unchanged.
    static constexpr uint32_t kMaxRenderWidth  = kScreenWidth + 2u * kMaxExtraX;  // 480
    static constexpr uint32_t kMaxNativeRenderWidth = kScreenWidth * 10u;          // 2400
    static constexpr uint32_t kMaxCompositeWidth =
        (kMaxRenderWidth > kMaxNativeRenderWidth)
            ? kMaxRenderWidth : kMaxNativeRenderWidth;
    static constexpr uint32_t kMaxRenderHeight = kScreenHeight + 2u * kMaxExtraY;  // 240
    static constexpr std::size_t kMaxFramebufferBytes =
        static_cast<std::size_t>(kMaxRenderWidth) * kMaxRenderHeight * 3;

    // Set the per-side view margins (clamped to the compile-time max per axis).
    // Call once at runtime init from the runner's --widescreen wiring.
    void set_view_margins(uint32_t left, uint32_t right,
                          uint32_t top, uint32_t bottom);
    uint32_t view_extra_left()   const { return extra_left_; }
    uint32_t view_extra_right()  const { return extra_right_; }
    uint32_t view_extra_top()    const { return extra_top_; }
    uint32_t view_extra_bottom() const { return extra_bottom_; }
    bool     view_expanded()     const {
        return (extra_left_ | extra_right_ | extra_top_ | extra_bottom_) != 0;
    }
    // Active output dimensions = vanilla + active margins (== 240/160 when OFF).
    uint32_t render_width()  const { return kScreenWidth  + extra_left_ + extra_right_; }
    uint32_t render_height() const { return kScreenHeight + extra_top_ + extra_bottom_; }
    std::size_t render_bytes() const {
        return static_cast<std::size_t>(render_width()) * render_height() * 3;
    }

    void render(uint8_t* rgb,
                uint16_t dispcnt,
                const uint8_t* io,    // 1 KB IO page for BGxCNT, BGxX/Y/PA/PB/PC/PD
                const uint8_t* vram,
                const uint8_t* oam,
                const uint8_t* pal) const;

    // Render an opt-in native scene at an integer supersampling scale. This
    // uses the same BG/OBJ/window/blend compositor as the canonical path, but
    // evaluates affine backgrounds at each output sample instead of first
    // reducing them to 240 hardware pixels. `scale` is clamped by the caller
    // to 1..10 and the output is (240*scale)x(160*scale) RGB888.
    void render_native(uint8_t* rgb,
                       uint32_t scale,
                       uint16_t dispcnt,
                       const uint8_t* io,
                       const uint8_t* vram,
                       const uint8_t* oam,
                       const uint8_t* pal) const;
    // Native presentation must use the same VBlank boundary as the canonical
    // framebuffer. Returns false until the first VBlank snapshot exists.
    bool render_native_latched(uint8_t* rgb, uint32_t scale) const;
    // Render a read-only, VBlank-captured presentation state. `line_io`
    // contains 160 consecutive kLineIoBytes records and `affine_line_refs`
    // contains four hidden BG2/BG3 reference coordinates per line.
    void render_captured_scene(uint8_t* rgb,
                               const uint8_t* line_io,
                               const int32_t* affine_line_refs,
                               const uint8_t* vram,
                               const uint8_t* oam,
                               const uint8_t* pal) const;
    const uint8_t* latched_native_line_io() const {
        return latched_native_line_io_[0].data();
    }
    const bool* latched_native_line_io_valid() const {
        return latched_native_line_io_valid_.data();
    }
    const int32_t* latched_native_affine_line_refs() const {
        return &latched_native_affine_line_ref_[0][0][0];
    }
    const bool* latched_native_affine_line_ref_valid() const {
        return latched_native_affine_line_ref_valid_.data();
    }
    void render_scanline(uint32_t y,
                         uint16_t dispcnt,
                         const uint8_t* io,
                         const uint8_t* vram,
                         const uint8_t* oam,
                         const uint8_t* pal);

    // GoldenSunRecomp edit (see MODIFICATIONS.md): stop this compositor
    // DRAWING, while it still records each scanline's registers and walks
    // the affine references. A host that draws the frame some other way --
    // GoldenSunRecomp's GPU renderer, via the frame-present override -- then
    // pays for the raster once instead of twice, and a frame that renderer
    // refuses shows whatever the host paints instead of a picture from here,
    // which is what makes a refusal visible at all. Default on: with nothing
    // calling this, the PPU behaves exactly as before.
    void set_native_raster_enabled(bool on) { native_raster_enabled_ = on; }
    bool native_raster_enabled() const { return native_raster_enabled_; }

    // GBA affine backgrounds keep hidden reference coordinates. Writes to
    // BG2X/Y or BG3X/Y reload the corresponding hidden coordinate; each
    // rendered scanline then advances it by PB/PD. GbaIo calls this after a
    // reference-register write, including writes performed by HBlank DMA.
    void reload_affine_reference(uint32_t layer, bool y_coord,
                                 uint32_t raw_value);
    void reload_affine_references(const uint8_t* io);
    void serialize_affine_state(gbarecomp::debug::SnapshotWriter& w) const;
    void deserialize_affine_state(gbarecomp::debug::SnapshotReader& r);

    // Latch the just-finished visible frame. The BIOS mutates OAM/PAL
    // during VBlank; screenshots must therefore use the frame captured
    // at VBlank start, not whatever live memory contains later.
    void latch_framebuffer(uint16_t dispcnt,
                           const uint8_t* io,
                           const uint8_t* vram,
                           const uint8_t* oam,
                           const uint8_t* pal);
    void mark_framebuffer_latched();
    void latch_native_scene_state(uint16_t dispcnt,
                                  const uint8_t* io,
                                  const uint8_t* vram,
                                  const uint8_t* oam,
                                  const uint8_t* pal);
    bool has_latched_native_scene_state() const {
        return has_latched_native_state_;
    }
    const uint8_t* latched_framebuffer() const { return latched_fb_.data(); }
    bool has_latched_framebuffer() const { return has_latched_fb_; }

private:
    void render_deferred_native_rows();
    void dump_row_state(uint16_t dispcnt);
    void render_vertical_margin_rows();

    uint32_t scanline_        = 0;   // 0..227
    uint32_t dot_in_scanline_ = 0;   // 0..307 (in dots, not cycles)
    uint32_t cycle_in_dot_    = 0;   // 0..3
    uint16_t vcount_          = 0;
    uint64_t frame_count_     = 0;
    // Oversized to the compile-time max so the widened frame fits without
    // reallocation; only the first render_bytes() are used (vanilla = 115200).
    std::array<uint8_t, kMaxFramebufferBytes> latched_fb_{};
    bool has_latched_fb_ = false;
    uint16_t latched_native_dispcnt_ = 0;
    std::array<uint8_t, 0x400> latched_native_io_{};
    std::array<uint8_t, 96 * 1024> latched_native_vram_{};
    std::array<uint8_t, 0x400> latched_native_oam_{};
    std::array<uint8_t, 0x400> latched_native_pal_{};
    using LineIo = std::array<uint8_t, kLineIoBytes>;
    std::array<LineIo, kScreenHeight> line_io_{};
    std::array<bool, kScreenHeight> line_io_valid_{};
    // GoldenSunRecomp edit, see set_native_raster_enabled().
    bool native_raster_enabled_ = true;
    std::array<LineIo, kScreenHeight> latched_native_line_io_{};
    std::array<bool, kScreenHeight> latched_native_line_io_valid_{};
    bool has_latched_native_state_ = false;

    std::array<std::array<int32_t, 2>, 2> affine_ref_{};
    bool affine_ref_valid_ = false;
    std::array<std::array<std::array<int32_t, 2>, 2>, kScreenHeight>
        affine_line_ref_{};
    std::array<bool, kScreenHeight> affine_line_ref_valid_{};
    std::array<std::array<std::array<int32_t, 2>, 2>, kScreenHeight>
        latched_native_affine_line_ref_{};
    std::array<bool, kScreenHeight>
        latched_native_affine_line_ref_valid_{};

    // Live-only, payload-free expanded-margin diagnostics. This state is never
    // serialized and is touched only when an observer is installed.
    WsMarginDiagnostics wide_margin_diagnostics_{};
    WsMarginDiagnosticsCallback wide_margin_diagnostics_callback_ = nullptr;
    bool wide_margin_diagnostics_active_ = false;

    // View-area margins (present-time host state; NOT serialized — see above).
    uint32_t extra_left_   = 0;
    uint32_t extra_right_  = 0;
    uint32_t extra_top_    = 0;
    uint32_t extra_bottom_ = 0;
};

// Widescreen margin tilemap provider (Step C; see gba_ppu.cpp). Set by the
// runtime-side sidecar; nullptr = vanilla wide behavior. It is queried for
// every regular-BG sample whose signed logical X or Y lies outside the native
// 240x160 canvas.
// Result 0 means no authored continuation, 1 replaces the wrapped hardware
// entry through out_entry, and 2 explicitly accepts the wrapped entry for a
// deliberately tiled effect rather than room geometry.
enum WsTilemapProviderResult : int {
    kWsTilemapUnavailable = 0,
    kWsTilemapReplace = 1,
    kWsTilemapKeepWrapped = 2,
};

// Optional read-only policy for expanded-view margins. The callback is called
// for each rendered row with the standard DISPCNT value and that row's IO
// snapshot (edge-line state for synthesized top/bottom rows). It returns a mask
// telling the compositor which margins to blacken; zero permits the normal expanded
// samples. While installed, this per-scanline result is authoritative and the
// legacy g_ws_pillarbox* globals are ignored. A null callback preserves the
// historical global-flag behavior.
enum WsMarginPolicyFlags : unsigned {
    kWsMarginPillarboxLeft  = 1u << 0,
    kWsMarginPillarboxRight = 1u << 1,
    kWsMarginPillarboxBoth  = kWsMarginPillarboxLeft |
                              kWsMarginPillarboxRight,
    kWsMarginPillarboxTop    = 1u << 2,
    kWsMarginPillarboxBottom = 1u << 3,
    kWsMarginPillarboxVertical = kWsMarginPillarboxTop |
                                  kWsMarginPillarboxBottom,
    kWsMarginPillarboxAll = kWsMarginPillarboxBoth |
                             kWsMarginPillarboxVertical,
};
extern "C" unsigned (*g_ws_margin_policy)(uint16_t dispcnt,
                                            const uint8_t* io);

extern "C" int (*g_ws_tilemap_provider)(int bg, int hw_x, int screen_y,
                                        uint16_t* out_entry);
// Optional game-owned source for regular-BG map entries, native and expanded
// alike. Called with logical screen coordinates (which may fall outside
// 0..239 / 0..159 in the expanded renderer); return non-zero having written a
// screen entry to *out_entry, or zero to leave the hardware fetch in place.
//
// Unlike g_ws_tilemap_provider above, this is consulted for EVERY sample, not
// only those beyond the native area. That is the point: a game that can
// reconstruct its own map answers uniformly, so an off-screen sample is an
// ordinary lookup rather than a special case that has to invent an answer.
// The PPU keeps no game knowledge -- it passes screen coordinates and takes
// an entry back. nullptr (default) = unmodified hardware behaviour.
extern "C" int (*g_ws_field_tilemap_source)(int bg, int screen_x, int screen_y,
                                            uint16_t* out_entry);
// Optional per-invocation hook paired with g_ws_field_tilemap_source. The
// compositor calls it once before each rendered row, so a game-owned source
// can prepare row-specific register state and reuse it for that row's pixel
// samples. The callback receives the logical row, including negative/
// vertical-margin rows in the expanded renderer, the DISPCNT used for that
// row, and the row's IO snapshot (or nullptr when none exists). nullptr
// leaves the source callback unchanged.
extern "C" void (*g_ws_field_tilemap_source_begin)(int screen_y,
                                                     uint16_t dispcnt,
                                                     const uint8_t* io);
// Temporary counters for diagnosing the expanded view, read by the game's
// room-buffer report. [0] wide scanlines rendered, [1] entries supplied by the
// game, [2] objects skipped as parked, [3] margin samples left blank.
extern "C" unsigned long long g_ws_expanded_diag[4];
// See the definition site in gba_ppu.cpp: running session totals of OBJ
// entries whose axes were/weren't both authenticated by a provider. Read by
// the game's tracer as a per-frame delta.
extern "C" unsigned long long g_ws_obj_trusted_total;
extern "C" unsigned long long g_ws_obj_untrusted_total;
// 1 while render_scanline_wide is rendering the first logical scanline
// (logical_y == 0) of a frame, 0 otherwise. Measurement-only gate so a
// per-object tally driven by the OBJ providers (which run once per object
// per scanline) can be counted once per object per frame, matching the
// logical_y == 0 gate g_ws_obj_trusted_total/g_ws_obj_untrusted_total above
// already use. Set and cleared by render_scanline_wide itself, including on
// every early-return path.
extern "C" int g_ws_obj_census_line;
// Optional per-game presentation remap for regular BG samples in the expanded
// renderer. The callback receives the physical output X and may suppress the
// sample (-1), leave the hardware X unchanged (0), or provide an authentic
// hardware X through out_hw_x (1). The native 240x160 renderer never calls it.
extern "C" int (*g_ws_bg_x_provider)(int bg, int output_x, int screen_y,
                                     int* out_hw_x);
// Bitmask of regular BG layers that may use g_ws_bg_x_provider. Defaults to
// all layers; game adapters can narrow it to avoid per-pixel callback traffic.
extern "C" unsigned g_ws_bg_x_provider_layers;
// Optional per-game sample remap for regular BG layers in the expanded
// renderer, for scenes the game presents at a different scale or offset than
// the guest chose (a fixed battle backdrop zoomed to fill the wider canvas,
// say). The callback receives the physical output column and the logical row
// (negative above the native window, >=160 below it) and may suppress the
// sample (-1), leave it alone (0), or return the hardware pixel to read
// instead through out_hw_x/out_hw_y (1).
//
// A remapped sample is the game's own answer, so the compositor asks the
// window registers about the SOURCE pixel rather than the output pixel, and
// skips the wrapped-margin providers below: the game has already said which
// hardware pixel belongs here. The native 240x160 renderer never calls it.
extern "C" int (*g_ws_bg_sample_provider)(int bg, int output_x, int screen_y,
                                          int* out_hw_x, int* out_hw_y);
// Diagnostic: while non-zero, the next few expanded frames whose mode matches
// write every authentic row's latched display state to logs/battle_rows.csv
// and then stop. It answers one question a per-frame trace cannot: which of
// these registers the game changes WITHIN a frame, and therefore what a
// magnified layer has to follow. Set by the game adapter, never by a raw
// environment variable.
extern "C" int g_ws_row_state_dump_mode;   // -1 off, else the BG mode to catch
extern "C" int g_ws_row_state_dump_frames; // frames still to write
// While non-zero, the expanded renderer does NOT draw the authentic scanlines
// as the emulator produces them. It draws all 160 of them at VBlank instead,
// from the latched per-row state.
//
// Streaming is right for an untransformed view, where output row N shows row
// N and that row's registers are the ones in hand. It cannot work for a
// magnified layer: output row N then shows a DIFFERENT source row, whose
// registers may not have been written yet -- they belong to a scanline the
// emulator has not reached. Drawing at VBlank gives every row access to every
// other row's registers. Set per frame by the game adapter; zero (default)
// keeps the streaming behaviour exactly.
extern "C" int g_ws_defer_native_rows;
// Bitmask of regular BG layers that may use g_ws_bg_sample_provider. Defaults
// to none, so the hook costs nothing until a game adapter opts a layer in.
extern "C" unsigned g_ws_bg_sample_provider_layers;
// Bitmask of layers whose remapped samples skip the window test entirely:
// the game adapter owns where such a layer appears, so a guest window that
// letterboxes it must not also clip the remap. Defaults to none.
extern "C" unsigned g_ws_bg_sample_provider_ignore_window_layers;
// Per-game policy for self-sufficient authored margin providers. Off keeps the
// established fail-closed window/savestate behavior. On lets provider-sourced
// regular BG margins continue independently beside native HUD/dialog windows.
extern "C" int g_ws_authored_margin_layers;
// Legacy Step-C margin flags. They are consulted only when
// g_ws_margin_policy is null; a game-owned callback supersedes stale values
// (including values restored by a savestate load).
extern "C" int g_ws_pillarbox;
extern "C" int g_ws_pillarbox_left;
extern "C" int g_ws_pillarbox_right;
extern "C" int g_ws_pillarbox_top;
extern "C" int g_ws_pillarbox_bottom;
// Generic, game-owned per-frame world-space pixel shift for the expanded
// (wide) renderer only. When nonzero, every regular-BG sample (native columns
// and margins alike) and every OBJ's screen position moves by this amount in
// hardware pixels before any wrap/native-window math, so the whole 360x240
// canvas pans as one rigid unit instead of the native window and its margins
// drifting apart. Zero (default) leaves rendering byte-for-byte unchanged.
// The narrow (240x160 / non-wide) renderer never reads these.
extern "C" int g_ws_view_shift_x;
extern "C" int g_ws_view_shift_y;
// Optional per-game interpretation of the 9-bit OBJ X field in expanded-view
// rendering. Hardware-faithful signed decoding remains the default.
extern "C" int (*g_ws_obj_x_provider)(int raw_x, int* out_x);
// Optional per-game interpretation of the 8-bit OBJ Y field in expanded-view
// rendering. Hardware-faithful signed decoding remains the default; an
// installed provider may reinterpret authenticated raw values such as
// 160..199 as positive screen coordinates.
extern "C" int (*g_ws_obj_y_provider)(int raw_y, int* out_y);
// Richer per-game OBJ placement hook for screen-space UI. Called only by the
// expanded renderer and before g_ws_obj_x_provider; receives the OAM index and
// raw attributes so a game can distinguish HUD sprites from world objects.
extern "C" int (*g_ws_obj_attr_x_provider)(int oam_index,
                                            std::uint16_t attr0,
                                            std::uint16_t attr1,
                                            std::uint16_t attr2,
                                            int* out_x);
// Richer per-game OBJ placement hook for the Y axis, mirroring
// g_ws_obj_attr_x_provider exactly. Called only by the expanded renderer and
// before g_ws_obj_y_provider; receives the OAM index and raw attributes so a
// game can disambiguate the raw Y field's dual meaning (genuine widened
// positive Y vs. hardware-standard negative Y) using per-slot knowledge.
extern "C" int (*g_ws_obj_attr_y_provider)(int oam_index,
                                            std::uint16_t attr0,
                                            std::uint16_t attr1,
                                            std::uint16_t attr2,
                                            int* out_y);
// Per-game "this OBJ slot is a parked dummy" hook. A game that hides unused
// sprites by moving them off the visible screen -- rather than clearing their
// enable bit -- makes those slots reappear the moment the view is widened,
// because the place it parks them in is inside the new margins. The expanded
// renderer therefore asks the game, once per OBJ per frame, whether a slot is
// one of those; a slot that says yes is confined to the authentic window and
// so stays invisible exactly as it is on hardware.
//
// Returns non-zero for a parked dummy. Null (the default) means no slot is
// parked, which is the correct answer for a game that disables its unused
// sprites properly.
extern "C" int (*g_ws_obj_park_provider)(int oam_index,
                                          std::uint16_t attr0,
                                          std::uint16_t attr1,
                                          std::uint16_t attr2);

// Live BG-layer visibility toggle, driven by the in-game F1 menu
// (Enhancements > Visual > Layers). Purely presentational: forces a
// regular/affine BG layer fully transparent without touching guest memory
// or DISPCNT. false (default) = layer renders normally. Session-only, never
// persisted or serialized into a save state.
extern "C" bool g_hide_bg0;
extern "C" bool g_hide_bg1;
extern "C" bool g_hide_bg2;
extern "C" bool g_hide_bg3;

}  // namespace gba
