// host_window.cpp â€” SDL2-backed window. Stubs out when SDL2 isn't found.

#include "host_window.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "color_lut.h"
#include "env_flag.h"
#include "host_config_ui.h"
#include "host_overlay.h"
#include "frame_timing.h"
#include "host_prof_phase.h"
#include "presentation_layout.h"
#include "screen_filter.h"
#include "player_walk_run_speed_config.h"
#include "temporal_blend.h"
// runtime_set_overclock_factor / runtime_get_overclock_factor (TURBO-B2-UI).
#include "../armv4t/runtime_arm.h"
// gba::g_hide_bg0..g_hide_bg3: live BG-layer visibility toggle (F1 menu).
#include "../gba/gba_ppu.h"

#if defined(GBARECOMP_HAVE_SDL2)

#include <SDL.h>

// Shared ecosystem clock-domain bridge (callback-driven DRC). Replaces the
// SDL_QueueAudio push path, which silence-filled on queue underrun (~3.4/s
// measured on Minish Cap) and hard-flushed on overflow â€” the same output-side
// crackle fixed on NES. IMPL is defined in exactly this one translation unit.
#define RECOMP_AUDIO_DRC_IMPL
#include "recomp_audio_drc.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>
#endif

namespace gbarecomp {

namespace {

bool host_pump_timing_enabled() {
    static const bool enabled = [] {
        const char* phase = std::getenv("GBARECOMP_FRAME_PHASE");
        const char* events = std::getenv("GBARECOMP_FRAME_EVENTS");
        return (phase && phase[0]) || (events && events[0]);
    }();
    return enabled;
}

uint64_t host_pump_now_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

uint32_t host_pump_elapsed_us(uint64_t start, uint64_t end) {
    const uint64_t us = end > start ? (end - start) / 1000ull : 0ull;
    return us > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(us);
}

bool write_enhancements_ini(const std::string& dir, bool enhanced_timing,
                            unsigned overclock_factor,
                            int temporal_blend_index,
                            int view_mode);
bool write_cheats_ini(const std::string& dir, bool infinite_hp,
                      bool infinite_pp, int player_speed_multiplier);
// The Custom colour sliders (F1 > Video > Colours), as ints.
struct CustomColors {
    int saturation = 100, hue = 0, brightness = 100, warmth = 0, darken = 0;
    bool operator==(const CustomColors& o) const {
        return saturation == o.saturation && hue == o.hue &&
               brightness == o.brightness && warmth == o.warmth &&
               darken == o.darken;
    }
    bool operator!=(const CustomColors& o) const { return !(*this == o); }
};
bool write_picture_ini(const std::string& dir, int screen_kind,
                       const CustomColors& custom, int aspect);

// System hotkey ids (config.ini [KeyMap] rows this host implements). Reset,
// PauseDimmed and ToggleRenderer are intentionally absent â€” gbarecomp has no
// in-process reset, no attract-dim, and one renderer; the launcher's GBA
// hotkey catalog omits them to match.
//
// HK_TURBO ("Turbo Held" in the UI) is level-triggered: fast-forward is on
// only while the binding is down (UI-02). HK_TURBO_TOGGLE ("Turbo Toggle")
// is the edge-triggered latch added alongside it, appended at the end so
// existing HostHotkey values (and therefore existing config.ini [KeyMap]
// rows) keep their meaning. Both drive the same Events::fast_forward flag
// (see the pad/keyboard read at the end of pump()); UI-02's chosen rule:
// fast_forward is true when EITHER Held is currently down OR the Toggle
// latch is on. Releasing Held never clears the Toggle latch.
enum HostHotkey {
    HK_FULLSCREEN = 0, HK_PAUSE, HK_TURBO,
    HK_WINDOW_BIGGER, HK_WINDOW_SMALLER,
    HK_VOLUME_UP, HK_VOLUME_DOWN, HK_DISPLAY_PERF,
    HK_MENU,
    HK_TURBO_TOGGLE,
    // A game-owned menu drawn inside the game (GoldenSunRecomp's cheat
    // menu); the run loop hands the press to RunOptions::game_menu_toggle.
    // Appended so existing [KeyMap] rows keep their meaning.
    HK_GAME_MENU,
    // Auto Fire A / B: level-triggered like Turbo Held. While the binding is
    // down the button is pressed and released on alternating frame pairs
    // (~15 presses a second), read at the end of the KEYINPUT build in
    // pump(). Appended so existing [KeyMap] rows keep their meaning.
    HK_AUTOFIRE_A,
    HK_AUTOFIRE_B,
    // Quit Game: the F1 menu's Quit Game button on a key. Appended so
    // existing [KeyMap] rows keep their meaning.
    HK_QUIT,
    HK_COUNT
};

struct HotkeyBind {
    SDL_Keycode key = SDLK_UNKNOWN;   // SDLK_UNKNOWN = unbound
    Uint16      mods = 0;             // required KMOD_CTRL/ALT/SHIFT bits
    // SDL_GameControllerButton, -1 = unbound (UI-02); or kPadTriggerLeft/
    // kPadTriggerRight (UI-02b) for the L2/R2 analog triggers, which SDL
    // never reports as a button.
    int         pad_button = -1;
};

// host_config_ui.h's kPadTriggerLeft/Right sentinels are a literal (that
// header carries no SDL include) rather than SDL_CONTROLLER_BUTTON_MAX
// directly; keep them honest against this SDL2's real enum value.
static_assert(kPadTriggerLeftValue == static_cast<int>(SDL_CONTROLLER_BUTTON_MAX),
             "kPadTriggerLeftValue must track SDL_CONTROLLER_BUTTON_MAX so the "
             "synthetic L2/R2 ids never collide with a real pad button");

// UI-02b: hysteresis thresholds treating an analog L2/R2 trigger pull
// (SDL_CONTROLLERAXISMOTION, 0..32767) as a digital press/release. Two
// thresholds rather than one so a trigger resting exactly on the boundary
// (mechanical play, drift) cannot chatter on/off: press fires only above
// ~61% travel, release only below ~37%, leaving a dead band between them.
constexpr Sint16 kTriggerPressThreshold   = 20000;  // ~61% of 32767 -> pressed
constexpr Sint16 kTriggerReleaseThreshold = 12000;  // ~37% of 32767 -> released

// A bound stick direction (stick_dir_held). 7849 is XInput's documented
// XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE (the standard value for this exact
// stick), not a tuned number.
constexpr float kStickDeadZone = 7849.0f;
// sin(22.5 deg): the geometry of eight equal 45-degree sectors centred on the
// axes. A normalised axis past this value presses that direction, and a
// diagonal presses two.
constexpr float kStickSectorSin = 0.38268343f;

// â”€â”€ MC-WS-002 present-cadence ring â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Always-on (Release too) ring recording EVERY SDL_RenderPresent from window
// open: wall time blocked inside the call, entry-to-entry gap, and the DWM
// composition refresh counter (cRefresh) at exit â€” the scanout-side ruler
// that delivered-content framedumps cannot see. The ring records
// unconditionally (~24 B/present); GBARECOMP_PRESENT_CADENCE=1 adds a
// periodic stderr summary and a full CSV dump at close
// (GBARECOMP_PRESENT_CADENCE_DUMP=path overrides ./_present_cadence.csv).
// Interpretation (tools/analyze_present_cadence.py automates this):
//   block_us â‰ˆ 0 on every present â†’ vsync is NOT blocking (tear-prone);
//   rdelta 1,1,1â€¦ at ~16.74 ms gaps on a high-Hz VRR panel â†’ VRR engaged;
//   rdelta 2/3 alternation on 164 Hz â†’ 59.73â†’164 pulldown (cadence judder);
//   rdelta 0 rows â†’ two presents inside one refresh (frame replaced).
struct PresentSample {
    uint64_t qpc = 0;          // SDL performance counter at present entry
    uint64_t dwm_refresh = 0;  // DWM cRefresh after present (0 = unavailable)
    uint32_t block_us = 0;     // wall time inside SDL_RenderPresent
    uint32_t gap_us = 0;       // entry-to-entry gap from the previous present
    uint8_t  fullscreen = 0;   // window was fullscreen for this present
};

constexpr int kCadenceRingSize     = 16384;  // ~4.5 min at 60 presents/s
constexpr int kCadenceSummaryEvery = 360;    // ~6 s between stderr summaries

struct PresentCadence {
    std::vector<PresentSample> ring;
    uint64_t total = 0;        // lifetime presents (ring keeps the last N)
    uint64_t qpc_freq = 0;
    uint64_t last_qpc = 0;     // previous present entry (0 = none yet)
    bool verbose = false;
    std::string dump_path;
#if defined(_WIN32)
    HRESULT (WINAPI* dwm_gcti)(HWND, DWM_TIMING_INFO*) = nullptr;
#endif

    void init() {
        ring.resize(kCadenceRingSize);
        qpc_freq = SDL_GetPerformanceFrequency();
        verbose = env_flag("GBARECOMP_PRESENT_CADENCE");
        const char* d = std::getenv("GBARECOMP_PRESENT_CADENCE_DUMP");
        dump_path = (d && *d) ? d : "_present_cadence.csv";
#if defined(_WIN32)
        if (HMODULE m = LoadLibraryA("dwmapi.dll")) {
            dwm_gcti = reinterpret_cast<HRESULT (WINAPI*)(HWND, DWM_TIMING_INFO*)>(
                reinterpret_cast<void*>(
                    GetProcAddress(m, "DwmGetCompositionTimingInfo")));
        }
#endif
    }

    // Query the always-on DWM composition clock: refresh counter now, and
    // (optionally) the compositor's nominal refresh rate in Hz.
    uint64_t dwm_refresh_now(double* rate_hz) {
#if defined(_WIN32)
        if (dwm_gcti) {
            DWM_TIMING_INFO ti{};
            ti.cbSize = sizeof(ti);
            if (SUCCEEDED(dwm_gcti(nullptr, &ti))) {
                if (rate_hz)
                    *rate_hz = ti.rateRefresh.uiDenominator
                        ? static_cast<double>(ti.rateRefresh.uiNumerator) /
                              static_cast<double>(ti.rateRefresh.uiDenominator)
                        : 0.0;
                return ti.cRefresh;
            }
        }
#endif
        if (rate_hz) *rate_hz = 0.0;
        return 0;
    }

    uint32_t to_us(uint64_t qpc_delta) const {
        if (!qpc_freq) return 0;
        const uint64_t us = qpc_delta * 1000000ull / qpc_freq;
        return us > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(us);
    }

    void record(uint64_t qpc0, uint64_t qpc1, bool fullscreen) {
        PresentSample s;
        s.qpc = qpc0;
        s.block_us = to_us(qpc1 - qpc0);
        s.gap_us = last_qpc ? to_us(qpc0 - last_qpc) : 0;
        last_qpc = qpc0;
        s.dwm_refresh = dwm_refresh_now(nullptr);
        s.fullscreen = fullscreen ? 1 : 0;
        ring[total % kCadenceRingSize] = s;
        ++total;
        if (verbose && (total % kCadenceSummaryEvery) == 0) summarize();
    }

    void summarize() {
        const int n = static_cast<int>(
            std::min<uint64_t>(total, kCadenceSummaryEvery));
        if (n < 2) return;
        std::vector<uint32_t> gaps, blocks;
        gaps.reserve(n);
        blocks.reserve(n);
        int hist[7] = {};  // rdelta 0..4, [5]=5+, [6]=n/a
        uint64_t prev_refresh = 0;
        bool have_prev = false;
        uint64_t gap_sum = 0;
        int fs = 0;
        for (int i = n; i >= 1; --i) {
            const PresentSample& s = ring[(total - i) % kCadenceRingSize];
            blocks.push_back(s.block_us);
            if (s.gap_us) { gaps.push_back(s.gap_us); gap_sum += s.gap_us; }
            fs += s.fullscreen;
            if (s.dwm_refresh == 0) {
                ++hist[6];
                have_prev = false;
            } else {
                if (have_prev) {
                    const uint64_t d = s.dwm_refresh - prev_refresh;
                    ++hist[d >= 5 ? 5 : static_cast<int>(d)];
                }
                prev_refresh = s.dwm_refresh;
                have_prev = true;
            }
        }
        auto pct = [](std::vector<uint32_t>& v, double p) -> uint32_t {
            if (v.empty()) return 0;
            const size_t k = static_cast<size_t>(p * (v.size() - 1));
            std::nth_element(v.begin(), v.begin() + k, v.end());
            return v[k];
        };
        const uint32_t bmax = blocks.empty()
            ? 0 : *std::max_element(blocks.begin(), blocks.end());
        const uint32_t b95 = pct(blocks, 0.95);
        const uint32_t b50 = pct(blocks, 0.50);
        const uint32_t g50 = pct(gaps, 0.50);
        double rate_hz = 0.0;
        dwm_refresh_now(&rate_hz);
        const double fps = gap_sum
            ? static_cast<double>(gaps.size()) * 1e6 / static_cast<double>(gap_sum)
            : 0.0;
        std::fprintf(stderr,
            "[present-cadence] n=%llu fps=%.2f block_us p50=%u p95=%u max=%u "
            "gap_us p50=%u rdel{0:%d 1:%d 2:%d 3:%d 4:%d 5+:%d na:%d} "
            "dwm_hz=%.2f fs=%d/%d\n",
            static_cast<unsigned long long>(total), fps, b50, b95, bmax, g50,
            hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6],
            rate_hz, fs, n);
        std::fflush(stderr);
    }

    // Full-ring CSV dump (verbose only) â€” the queryable record of the run.
    void dump() {
        if (!verbose || total == 0) return;
        std::FILE* f = std::fopen(dump_path.c_str(), "w");
        if (!f) {
            std::fprintf(stderr, "[present-cadence] cannot write %s\n",
                         dump_path.c_str());
            return;
        }
        std::fprintf(f, "idx,t_ms,gap_us,block_us,dwm_refresh,rdelta,fullscreen\n");
        const uint64_t n = std::min<uint64_t>(total, kCadenceRingSize);
        const uint64_t first_qpc = ring[(total - n) % kCadenceRingSize].qpc;
        uint64_t prev_refresh = 0;
        bool have_prev = false;
        for (uint64_t i = 0; i < n; ++i) {
            const PresentSample& s = ring[(total - n + i) % kCadenceRingSize];
            long long rdelta = -1;
            if (s.dwm_refresh) {
                if (have_prev)
                    rdelta = static_cast<long long>(s.dwm_refresh - prev_refresh);
                prev_refresh = s.dwm_refresh;
                have_prev = true;
            }
            std::fprintf(f, "%llu,%.3f,%u,%u,%llu,%lld,%u\n",
                static_cast<unsigned long long>(total - n + i),
                qpc_freq ? static_cast<double>(s.qpc - first_qpc) * 1000.0 /
                               static_cast<double>(qpc_freq)
                         : 0.0,
                s.gap_us, s.block_us,
                static_cast<unsigned long long>(s.dwm_refresh), rdelta,
                static_cast<unsigned>(s.fullscreen));
        }
        std::fclose(f);
        std::fprintf(stderr, "[present-cadence] dumped %llu presents -> %s\n",
                     static_cast<unsigned long long>(n), dump_path.c_str());
        std::fflush(stderr);
    }
};

// One-line display-mode report (index, geometry, nominal Hz) so cadence data
// can be interpreted against the panel the window actually sits on.
void log_display_mode(SDL_Window* win, const char* tag) {
    if (!win) return;
    const int di = SDL_GetWindowDisplayIndex(win);
    SDL_DisplayMode dm{};
    if (di >= 0 && SDL_GetCurrentDisplayMode(di, &dm) == 0) {
        std::fprintf(stderr, "host_window: %s display=%d mode=%dx%d@%dHz\n",
                     tag, di, dm.w, dm.h, dm.refresh_rate);
        std::fflush(stderr);
    }
}

struct Backend {
    SDL_Window*   window   = nullptr;
    SDL_Renderer* renderer = nullptr;
    // True when SDL actually created an "opengl" SDL_Renderer (checked via
    // SDL_GetRendererInfo after creation, never assumed from the hint).
    // Drives config_ui_init()'s runtime choice between the ImGui OpenGL3
    // backend (real multi-viewport) and the SDLRenderer2 backend (docking
    // only, no viewports) — see host_config_ui.h.
    bool          renderer_is_opengl = false;
    SDL_Texture*  texture  = nullptr;
    SDL_Texture*  native_texture = nullptr;
    SDL_AudioDeviceID audio_dev = 0;
    int            audio_channels = 1;
    // Callback-driven clock-domain bridge (replaces SDL queue push).
    rab_bridge    bridge{};
    bool          bridge_ready = false;
    SDL_mutex*    audio_mtx = nullptr;
    // Present-time screen-color simulation. Built once from
    // GBARECOMP_SCREEN; default Raw = exact passthrough (no copy, no
    // grading), so default behavior is byte-identical to upstream.
    std::unique_ptr<runtime::ColorLut> color_lut;
    runtime::ScreenKind screen_kind = runtime::ScreenKind::Raw;
    // Custom colours as last built into color_lut (percent, degrees).
    CustomColors applied_custom;
    std::vector<uint8_t> graded_fb;  // scratch RGB888 (base_w*base_h*3)
    // Diagnostic-only guest-frame marker. It is armed by the existing sprite
    // recorder switch so a video can be joined to stderr's guest-frame rows.
    // The marker is drawn into a presentation copy and never into PPU output.
    bool obj_record_frame_marker = false;
    std::vector<uint8_t> diagnostic_fb;
    int base_w = 240;   // logical surface width  (240 faithful, wider if expanded)
    int base_h = 160;   // logical surface height (160 faithful, taller if expanded)
    bool expanded_view = false;  // native games retain the historical SDL path
    bool aspect_4_3 = false;     // present only the middle 4:3 of a wider view
    // Test-only screen filters (launcher "Screen filters" box). Off unless the
    // box is on AND SDL really created the opengl renderer; Off never calls
    // into screen_filter.cpp.
    ScreenFilter screen_filter = ScreenFilter::Off;
    bool screen_filters_on = false;
    bool resize_driven_view = false;
    bool linear_filter = false;
    bool native_renderer = false;
    bool native_renderer_game_allowed = false;
    int native_scale = 2;
    std::vector<uint8_t> native_fb;
    std::vector<uint8_t> native_graded_fb;
    bool native_scene_pending = false;
    int native_scene_w = 0;
    int native_scene_h = 0;

    // VFX-FLICKER-02/03: temporal frame blend ("LCD ghosting").
    // temporal_blend_index is the persisted/applied setting (mirrors
    // overclock_factor's relationship to cfg.overclock_index below);
    // temporal_blend_prev/prev2 hold the RAW (unblended) previous two
    // PRESENTED frames (N-1, N-2 — needed by the selective/"flicker only"
    // filter's exact-2-frame-period check), temporal_blend_out is blend
    // scratch. All three are base_w*base_h*3 and resized alongside
    // graded_fb. temporal_blend_history_count tracks how many of
    // prev/prev2 currently hold real previous frames (0, 1, or 2) rather
    // than stale/zeroed data, so present() knows when it's safe to blend
    // vs. must pass the frame through untouched. See present() for where
    // this runs.
    int temporal_blend_index = gbarecomp::kTemporalBlendDefaultIndex;
    bool temporal_blend_needs_normalization = false;
    std::vector<uint8_t> temporal_blend_prev;
    std::vector<uint8_t> temporal_blend_prev2;
    std::vector<uint8_t> temporal_blend_out;
    int temporal_blend_history_count = 0;

    // ---- rebindable input (see HostWindow::load_input_config) --------------
    // GBA KEYINPUT bit (0..9) per bound SDL scancode; seeded from the
    // built-in defaults, overridden by keybinds.ini [player1].
    SDL_Scancode bind_sc[10] = {};      // indexed by GBA KEYINPUT bit
    HotkeyBind   hotkeys[HK_COUNT];     // [KeyMap] bindings

    std::string  config_dir = ".";        // where keybinds.ini lives
    // The walking-speed multiplier config.ini last recorded. Game code may
    // change the live value (runtime_set_mem_write_override_enabled) from
    // its own menus; a difference from this is saved on the next pump.
    int          persisted_player_speed = 2;  // halves: 2 = 1x
    // Same for Infinite HP/PP, which a game-owned menu can also switch.
    bool         persisted_infinite_hp = false;
    bool         persisted_infinite_pp = false;

    // ---- ImGui configuration UI (HK_MENU, default F1) ----------------------
    // Mirrors bind/video/audio state in and out of the UI each frame. The UI
    // never touches the backend directly; host_window applies what changed.
    // While it is open the guest sees all buttons released and every key press
    // is consumed by the UI, so a rebind cannot leak an input into the game.
    ConfigUiState  cfg{};
    bool           cfg_ready = false;
    SDL_GameController* pad = nullptr;    // first attached controller
    int            pad_bind[10];          // SDL_GameControllerButton per bit
    // UI-02b: debounced digital state of the L2/R2 analog triggers, derived
    // from SDL_CONTROLLERAXISMOTION via kTriggerPressThreshold/
    // kTriggerReleaseThreshold. Doubles as the edge source for hotkey
    // dispatch/capture and the level source for a Turbo-Held trigger bind.
    bool           trigger_left_down = false;
    bool           trigger_right_down = false;
    // Same debounced state for the eight stick directions, indexed by
    // synthetic id - kPadLeftStickRight (Right, Left, Up, Down per stick).
    bool           stick_down[8] = {};
    int          scale = 3;             // current integer window scale
    bool         fullscreen = false;
    bool         vsync = true;          // as negotiated with the renderer
    bool         integer_scale = true;  // applied to the present copy
    // Turbo shape, surfaced to the run loop through Events so the pacer can
    // run at a multiple of 59.7275 Hz instead of only all-or-nothing.
    float        turbo_multiplier = 4.0f;
    bool         turbo_uncapped = false;
    bool         turbo_mute_requested = false;
    bool         turbo_audio_muted = false;
    bool         strict_static = false;
    std::vector<HostWindow::FixedViewMode> fixed_view_modes;
    std::vector<const char*> fixed_view_mode_labels;
    bool         fixed_view_modes_allowed = false;
    int          fixed_view_mode = 0;
    bool         view_mode_config_present = false;
    bool         widescreen_game_allowed = false;
    bool         widescreen = false;
    // Legacy [Enhancements] Widescreen=true/false state. A new ViewMode key
    // takes precedence; the bool is retained on write for older builds.
    bool         widescreen_config_present = false;
    // UI-02: Turbo Toggle's latch state. Edge-triggered by the TurboToggle
    // hotkey (keyboard or pad); read every pump alongside Turbo Held to
    // drive the one shared Events::fast_forward flag (fast_forward = Held
    // down OR this latch on). Runtime-only, not persisted â€” only the two
    // bindings are saved, not whether turbo happened to be toggled on when
    // the app last closed.
    bool         turbo_toggle_on = false;
    bool         frame_interpolation_game_allowed = false;
    bool         frame_interpolation_display_ok = false;
    bool         enhanced_timing_game_allowed = false;
    // TURBO-B2-UI: Enhancements-tab "Guest CPU Overclock" control. Off/On;
    // persisted as the literal factor (1 or 10) in config.ini, applied live
    // via runtime_set_overclock_factor — see write_enhancements_ini and the
    // enhancements_changed handler below.
    unsigned     overclock_factor = 1;
    double       display_refresh_hz = 0.0;
    int          volume = 100;          // 0..100 gain on pushed samples
    std::vector<int16_t> volume_buf;    // scratch for gain != 100
    // What config.ini [Audio] last recorded, so a change from the menu or
    // the Volume Up/Down hotkeys is written once, not every frame.
    int          persisted_volume = 100;
    bool         persisted_mute = false;
    // Readouts: actual SDL_RenderPresent calls/sec and guest emulation speed.
    // The latter advances from every guest frame, including Turbo frames that
    // the presentation decimator skips.
    bool         fps_readout = false;
    std::string  title;                 // base window title (readout restores it)
    Uint64       fps_window_start = 0;
    uint64_t     fps_presents = 0;
    // Rolling presents/sec, kept even when the title-bar readout is off so
    // the config UI can show it without toggling anything.
    float        fps_last = 0.0f;
    EmulationSpeedTracker emulation_speed;
    // MC-WS-002: always-on per-present timing/scanout ring (see above).
    PresentCadence cadence;
};

void apply_audio_timing(Backend* b) {
    if (!b || !b->bridge_ready || !b->audio_mtx) return;
    const double scale = b->cfg.enhanced_timing
        ? kEnhancedAudioRateScale : 1.0;
    SDL_LockMutex(b->audio_mtx);
    rab_set_rate_scale(&b->bridge, scale);
    SDL_UnlockMutex(b->audio_mtx);
}

std::uint64_t performance_counter_ns() {
    const Uint64 now = SDL_GetPerformanceCounter();
    const Uint64 frequency = SDL_GetPerformanceFrequency();
    if (frequency == 0) return 0;
    return static_cast<std::uint64_t>(
        static_cast<long double>(now) * 1.0e9L /
        static_cast<long double>(frequency));
}

double emulation_normal_hz(const Backend* b) {
    return b->cfg.enhanced_timing ? kEnhancedGuestHz : kGbaFrameHz;
}

void reset_emulation_speed(Backend* b) {
    b->emulation_speed.reset();
    b->cfg.emulation_speed_percent = 0.0f;
}

void age_emulation_speed(Backend* b, std::uint64_t now_ns) {
    if (now_ns == 0) return;
    b->cfg.emulation_speed_percent = b->emulation_speed.age(now_ns);
}

int native_scale_from_env() {
    const char* e = std::getenv("GBARECOMP_NATIVE_RENDER_SCALE");
    if (!e || !*e) return 2;
    return std::clamp(std::atoi(e), 1, 10);
}

void destroy_native_texture(Backend* b) {
    if (!b) return;
    if (b->native_texture) {
        SDL_DestroyTexture(b->native_texture);
        b->native_texture = nullptr;
    }
    b->native_fb.clear();
    b->native_graded_fb.clear();
}

bool ensure_native_texture(Backend* b) {
    if (!b || !b->renderer || !b->native_renderer) return false;
    const int w = b->base_w * b->native_scale;
    const int h = b->base_h * b->native_scale;
    if (b->native_texture &&
        b->native_fb.size() == static_cast<std::size_t>(w) * h * 3u)
        return true;
    destroy_native_texture(b);
    b->native_texture = SDL_CreateTexture(
        b->renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING,
        w, h);
    if (!b->native_texture) {
        std::fprintf(stderr, "host_window: native texture failed: %s\n",
                     SDL_GetError());
        return false;
    }
    SDL_SetTextureScaleMode(b->native_texture, SDL_ScaleModeNearest);
    b->native_fb.resize(static_cast<std::size_t>(w) * h * 3u);
    return true;
}

void set_native_window_mode(Backend* b, bool enabled) {
    if (!b || !b->window || !b->renderer) return;
    if (enabled) {
        // SDL's logical-size transform reports the fixed 240x160 target to
        // RendererOutputSize. Native presentation must see the actual
        // drawable so a small window can downsample the high-resolution scene.
        SDL_RenderSetLogicalSize(b->renderer, 0, 0);
    } else if (!b->expanded_view && !b->resize_driven_view) {
        SDL_RenderSetLogicalSize(b->renderer, b->base_w, b->base_h);
        SDL_RenderSetIntegerScale(
            b->renderer, b->integer_scale ? SDL_TRUE : SDL_FALSE);
    }
}

// The width actually shown for a w x h picture: with 4:3 chosen, the
// Expanded View's middle h*4/3 columns (320 of 360); otherwise all of it.
// Native 240x160 is never cropped.
int shown_width(const Backend* b, int w, int h) {
    if (!b->aspect_4_3 || !b->expanded_view || b->resize_driven_view ||
        w * 3 <= h * 4)
        return w;
    return h * 4 / 3;
}

// Resize and re-centre the window to the current shown width at the current
// scale. Fullscreen and resize-driven views own their geometry.
void apply_aspect_window_size(Backend* b) {
    if (b->resize_driven_view || b->fullscreen || !b->window) return;
    if (SDL_GetWindowFlags(b->window) & SDL_WINDOW_MAXIMIZED)
        SDL_RestoreWindow(b->window);
    SDL_SetWindowSize(b->window, shown_width(b, b->base_w, b->base_h) * b->scale,
                      b->base_h * b->scale);
    SDL_SetWindowPosition(b->window, SDL_WINDOWPOS_CENTERED,
                          SDL_WINDOWPOS_CENTERED);
}

void native_upscale_nearest(const uint8_t* src, int src_w, int src_h,
                            int scale, std::vector<uint8_t>& dst) {
    const int dst_w = src_w * scale;
    const std::size_t src_row_bytes = static_cast<std::size_t>(src_w) * 3u;
    const std::size_t dst_row_bytes = static_cast<std::size_t>(dst_w) * 3u;
    for (int y = 0; y < src_h; ++y) {
        const uint8_t* src_row = src + static_cast<std::size_t>(y) * src_row_bytes;
        uint8_t* dst_row = dst.data() + static_cast<std::size_t>(y * scale) * dst_row_bytes;
        for (int x = 0; x < src_w; ++x) {
            const uint8_t* px = src_row + static_cast<std::size_t>(x) * 3u;
            for (int dx = 0; dx < scale; ++dx)
                std::memcpy(dst_row + static_cast<std::size_t>(x * scale + dx) * 3u,
                            px, 3u);
        }
        for (int dy = 1; dy < scale; ++dy)
            std::memcpy(dst_row + static_cast<std::size_t>(dy) * dst_row_bytes,
                        dst_row, dst_row_bytes);
    }
}

// GBA KEYINPUT bit order: 0=A 1=B 2=Sel 3=Sta 4=Right 5=Left 6=Up 7=Down 8=R 9=L.
// Defaults MATCH recomp-ui's generic keybinds defaults (keybinds.c) so the
// launcher's rebind page and the game agree even before keybinds.ini exists:
// A=X, B=Z, L=C, R=V, Start=Return, Select=RShift, D-pad=arrows.
// (This supersedes the pre-launcher hardcoded Z/X/A/S layout â€” one defaults
// source across the recomp ecosystem; rebind in the launcher to taste.)
const SDL_Scancode kDefaultBinds[10] = {
    SDL_SCANCODE_X,       // A
    SDL_SCANCODE_Z,       // B
    SDL_SCANCODE_RSHIFT,  // Select
    SDL_SCANCODE_RETURN,  // Start
    SDL_SCANCODE_RIGHT,   // Right
    SDL_SCANCODE_LEFT,    // Left
    SDL_SCANCODE_UP,      // Up
    SDL_SCANCODE_DOWN,    // Down
    SDL_SCANCODE_V,       // R
    SDL_SCANCODE_C,       // L
};

// keybinds.ini [player1] key name -> GBA KEYINPUT bit. x/y/l2/r2/l3/r3 from
// the generic 16-slot format are ignored (no GBA equivalent).
const struct { const char* name; int bit; } kBindKeys[] = {
    { "a", 0 }, { "b", 1 }, { "select", 2 }, { "start", 3 },
    { "right", 4 }, { "left", 5 }, { "up", 6 }, { "down", 7 },
    { "r", 8 }, { "l", 9 },
};

// config.ini [KeyMap] key names, in HostHotkey order, with the same defaults
// recomp-ui's hotkey panel displays for the GBA catalog. "Turbo" keeps its
// original persistence name for HK_TURBO (now "Turbo Held" in the UI) so a
// config.ini saved before UI-02 still loads that binding unchanged; the
// friendlier UI-02 label lives in kHotkeyLabels below instead of renaming
// this key. "TurboToggle" is new (UI-02).
const char* const kHotkeyNames[HK_COUNT] = {
    "Fullscreen", "Pause", "Turbo",
    "WindowBigger", "WindowSmaller",
    "VolumeUp", "VolumeDown", "DisplayPerf",
    "Menu",
    "TurboToggle",
    "CheatMenu",
    "AutoFireA",
    "AutoFireB",
    "QuitGame",
};
// UI display labels, same order as kHotkeyNames. Only Turbo (-> "Turbo
// Held") and the new TurboToggle (-> "Turbo Toggle") differ from the
// persisted key name.
const char* const kHotkeyLabels[HK_COUNT] = {
    "Fullscreen", "Pause", "Fast Forward (hold)",
    "Bigger Window", "Smaller Window",
    "Volume Up", "Volume Down", "Show FPS",
    "Settings Menu",
    "Fast Forward (toggle)",
    "Cheat Menu",
    "Auto Fire A",
    "Auto Fire B",
    "Quit Game",
};
const char* const kHotkeyDefaults[HK_COUNT] = {
    "Alt+Return", "Shift+P", "Tab",
    "", "",
    "", "", "F",
    // In-window rebind menu. F1 was save-state slot 1; the slots moved to
    // F2..F10 so this key is free. Rebindable like any other hotkey â€” set
    // config.ini [KeyMap] Menu= to something else and F1 goes back to being
    // an ordinary (unbound) key.
    "F1",
    // Turbo Toggle ships unbound by default: no keyboard/pad button is a
    // proven-safe default for a brand-new latching hotkey, so the user picks
    // one explicitly in the Hotkeys tab.
    "",
    // F11 is the one function key nothing else claims (F1 menu, F2..F10
    // save-state slots, F12 the renderer's frame dump).
    "F11",
    // Auto Fire ships unbound, like Turbo Toggle: the user picks the button.
    "",
    "",
    // Quit Game ships unbound so no stray press closes the game.
    "",
};

// keybinds.ini [player1_pad] key name -> GBA KEYINPUT bit. Same key names as
// [player1] above, so the two halves of a layout read alike; values are SDL
// game-controller button names (a, b, dpup, leftshoulder, ...).
const struct { const char* name; int bit; } kPadKeys[] = {
    { "a", 0 }, { "b", 1 }, { "select", 2 }, { "start", 3 },
    { "right", 4 }, { "left", 5 }, { "up", 6 }, { "down", 7 },
    { "r", 8 }, { "l", 9 },
};

// SDL_GetScancodeFromName plus the same lowercase aliases recomp-ui's
// keybinds.c accepts, so a file either side writes round-trips identically.
SDL_Scancode scancode_from_name(const char* name) {
    if (!name || !*name) return SDL_SCANCODE_UNKNOWN;
    SDL_Scancode sc = SDL_GetScancodeFromName(name);
    if (sc != SDL_SCANCODE_UNKNOWN) return sc;
    std::string b;
    for (const char* p = name; *p; ++p) b += (char)std::tolower((unsigned char)*p);
    if (b == "enter" || b == "return") return SDL_SCANCODE_RETURN;
    if (b == "tab")       return SDL_SCANCODE_TAB;
    if (b == "space")     return SDL_SCANCODE_SPACE;
    if (b == "lshift")    return SDL_SCANCODE_LSHIFT;
    if (b == "rshift")    return SDL_SCANCODE_RSHIFT;
    if (b == "lctrl")     return SDL_SCANCODE_LCTRL;
    if (b == "rctrl")     return SDL_SCANCODE_RCTRL;
    if (b == "lalt")      return SDL_SCANCODE_LALT;
    if (b == "ralt")      return SDL_SCANCODE_RALT;
    if (b == "backslash") return SDL_SCANCODE_BACKSLASH;
    if (b == "escape" || b == "esc") return SDL_SCANCODE_ESCAPE;
    if (b == "backspace") return SDL_SCANCODE_BACKSPACE;
    return SDL_SCANCODE_UNKNOWN;
}

// Parse a [KeyMap] value ("Ctrl+R", "Alt+Return", "F", "" = unbound) into a
// HotkeyBind. Mirrors the format recomp-ui's hotkey editor writes
// (SDL keycode name with Ctrl+/Alt+/Shift+ prefixes).
HotkeyBind parse_hotkey(const char* value) {
    HotkeyBind hb;
    if (!value || !*value) return hb;
    std::string v = value;
    Uint16 mods = 0;
    for (;;) {
        if (v.rfind("Ctrl+", 0) == 0)       { mods |= KMOD_CTRL;  v.erase(0, 5); }
        else if (v.rfind("Alt+", 0) == 0)   { mods |= KMOD_ALT;   v.erase(0, 4); }
        else if (v.rfind("Shift+", 0) == 0) { mods |= KMOD_SHIFT; v.erase(0, 6); }
        else break;
    }
    SDL_Keycode k = SDL_GetKeyFromName(v.c_str());
    if (k == SDLK_UNKNOWN) return hb;   // unparseable = unbound
    hb.key = k;
    hb.mods = mods;
    return hb;
}

// True when `hb` is bound and its required modifiers are (all) held. Ctrl/
// Alt/Shift not required by the binding must NOT be held â€” so "P" and
// "Shift+P" stay distinct bindings.
bool hotkey_mods_ok(const HotkeyBind& hb, Uint16 state_mods) {
    auto want = [&](Uint16 m) { return (hb.mods & m) != 0; };
    auto held = [&](Uint16 m) { return (state_mods & m) != 0; };
    return want(KMOD_CTRL) == held(KMOD_CTRL) &&
           want(KMOD_ALT) == held(KMOD_ALT) &&
           want(KMOD_SHIFT) == held(KMOD_SHIFT);
}

// True while the stick direction `id` (kPadLeftStickRight..kPadRightStickDown)
// is held: stick magnitude past the dead zone and the direction's component
// past the sector edge. One definition for the gameplay D-pad, the stick binds
// and hold-type hotkeys, so they all agree on what "pushed up" means.
bool stick_dir_held(SDL_GameController* pad, int id) {
    const int slot = id - kPadLeftStickRight;   // 0..7: Right, Left, Up, Down per stick
    const bool right_stick = slot >= 4;
    const float sx = static_cast<float>(SDL_GameControllerGetAxis(
        pad, right_stick ? SDL_CONTROLLER_AXIS_RIGHTX : SDL_CONTROLLER_AXIS_LEFTX));
    const float sy = static_cast<float>(SDL_GameControllerGetAxis(
        pad, right_stick ? SDL_CONTROLLER_AXIS_RIGHTY : SDL_CONTROLLER_AXIS_LEFTY));  // positive = down
    const float mag = std::sqrt(sx * sx + sy * sy);
    if (mag <= kStickDeadZone) return false;
    const float nx = sx / mag;
    const float ny = sy / mag;
    switch (slot & 3) {
    case 0:  return nx >  kStickSectorSin;   // Right
    case 1:  return nx < -kStickSectorSin;   // Left
    case 2:  return ny < -kStickSectorSin;   // Up
    default: return ny >  kStickSectorSin;   // Down
    }
}

// Level read of a hotkey binding: its key (with exactly its modifiers) or its
// controller button is down right now. L2/R2 use the debounced trigger state
// pump() maintains, since SDL never reports them as buttons (UI-02b).
bool hotkey_held(const Backend* b, const HotkeyBind& hb, const Uint8* ks) {
    if (hb.key != SDLK_UNKNOWN) {
        const SDL_Scancode sc = SDL_GetScancodeFromKey(hb.key);
        if (sc != SDL_SCANCODE_UNKNOWN && ks[sc] &&
            hotkey_mods_ok(hb, SDL_GetModState()))
            return true;
    }
    if (!b->pad) return false;
    if (hb.pad_button == kPadTriggerLeft) return b->trigger_left_down;
    if (hb.pad_button == kPadTriggerRight) return b->trigger_right_down;
    if (pad_is_stick(hb.pad_button)) return stick_dir_held(b->pad, hb.pad_button);
    return hb.pad_button >= 0 && hb.pad_button < SDL_CONTROLLER_BUTTON_MAX &&
           SDL_GameControllerGetButton(
               b->pad, static_cast<SDL_GameControllerButton>(hb.pad_button));
}

// Edge-triggered hotkey actions, shared by the keyboard (SDL_KEYDOWN) and
// controller (SDL_CONTROLLERBUTTONDOWN) dispatch in pump() (UI-02: every
// hotkey is bindable to either device through the same HotkeyBind table).
// HK_TURBO ("Turbo Held") is intentionally absent from this switch — it
// stays level-triggered and is read once per pump, alongside
// HK_TURBO_TOGGLE's latch, at the very end of pump().
void fire_hotkey(int h, Backend* b, HostWindow::Events& ev) {
    switch (h) {
        case HK_FULLSCREEN:     ev.toggle_fullscreen = true;             break;
        case HK_PAUSE:          ev.toggle_pause = true;                  break;
        case HK_WINDOW_BIGGER:  ev.window_bigger = true;                 break;
        case HK_WINDOW_SMALLER: ev.window_smaller = true;                break;
        case HK_VOLUME_UP:      ev.volume_up = true;                     break;
        case HK_VOLUME_DOWN:    ev.volume_down = true;                   break;
        case HK_DISPLAY_PERF:   ev.toggle_fps = true;                    break;
        case HK_MENU:           config_ui_toggle();                      break;
        // Turbo Toggle: press to flip the latch (UI-02). Never re-toggles
        // while held because this only runs on the down-edge (SDL_KEYDOWN
        // with repeat==0, or SDL_CONTROLLERBUTTONDOWN, both one-shot).
        case HK_TURBO_TOGGLE:   b->turbo_toggle_on = !b->turbo_toggle_on; break;
        case HK_GAME_MENU:      ev.game_menu = true;                     break;
        case HK_QUIT:           ev.quit = true;                          break;
        default: break;  // HK_TURBO, HK_AUTOFIRE_*: level-triggered, read in pump()
    }
}

// Minimal INI section scan shared by keybinds.ini and config.ini [KeyMap]:
// calls fn(key, value) for each assignment inside `section`.
template <typename Fn>
void ini_scan_section(const char* path, const char* section, Fn fn) {
    std::FILE* f = std::fopen(path, "r");
    if (!f) return;
    char line[512];
    bool in_section = false;
    while (std::fgets(line, sizeof(line), f)) {
        char* s = line;
        while (*s == ' ' || *s == '\t') ++s;
        size_t n = std::strlen(s);
        while (n && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' ' || s[n-1] == '\t'))
            s[--n] = '\0';
        if (!*s || *s == '#' || *s == ';') continue;
        if (*s == '[') {
            char* close = std::strchr(s, ']');
            if (close) *close = '\0';
            in_section = SDL_strcasecmp(s + 1, section) == 0;
            continue;
        }
        if (!in_section) continue;
        char* eq = std::strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char* key = s;
        char* val = eq + 1;
        size_t kl = std::strlen(key);
        while (kl && (key[kl-1] == ' ' || key[kl-1] == '\t')) key[--kl] = '\0';
        while (*val == ' ' || *val == '\t') ++val;
        char* hash = std::strchr(val, '#');
        if (hash) *hash = '\0';
        size_t vl = std::strlen(val);
        while (vl && (val[vl-1] == ' ' || val[vl-1] == '\t')) val[--vl] = '\0';
        fn(key, val);
    }
    std::fclose(f);
}

// Resolve the screen model: the per-game [video].screen from game.toml
// (`toml_screen`) is the default; GBARECOMP_SCREEN overrides it at launch.
// Unset/unrecognized â†’ Raw (passthrough). Tokens: raw|unlit|frontlit|
// backlit|classic.
runtime::ColorSettings resolve_color_settings(const char* toml_screen) {
    runtime::ColorSettings s;
    runtime::ScreenKind k;
    if (toml_screen && runtime::screen_kind_from_name(toml_screen, k)) s.screen = k;
    if (const char* env = std::getenv("GBARECOMP_SCREEN")) {
        if (runtime::screen_kind_from_name(env, k)) s.screen = k;
    }
    return s;
}

CustomColors custom_colors_of(const ConfigUiState& cfg) {
    return {cfg.color_saturation, cfg.color_hue, cfg.color_brightness,
            cfg.color_warmth, cfg.color_darken};
}

// Rebuild the colour table from b->screen_kind and the Custom sliders
// (F1 > Video > Colours), and size the graded frame buffers to match.
void rebuild_color_lut(Backend* b) {
    runtime::ColorSettings settings;
    settings.screen = b->screen_kind;
    settings.saturation = b->cfg.color_saturation / 100.0;
    settings.hue_degrees = static_cast<double>(b->cfg.color_hue);
    settings.brightness = b->cfg.color_brightness / 100.0;
    settings.warmth = b->cfg.color_warmth / 100.0;
    settings.curve = 1.0 + b->cfg.color_darken / 100.0;
    b->color_lut = std::make_unique<runtime::ColorLut>(settings);
    b->applied_custom = custom_colors_of(b->cfg);
    if (b->color_lut->is_passthrough()) {
        b->graded_fb.clear();
        b->native_graded_fb.clear();
    } else {
        b->graded_fb.resize(static_cast<std::size_t>(b->base_w) *
                            b->base_h * 3u);
    }
}

// SDL audio pull callback: render exactly `len` bytes of device-rate mono S16
// from the bridge ring. The bridge emits faded silence (not raw zeros) before
// prime / on underrun, so a momentarily-starved producer no longer clicks.
void gba_audio_callback(void* userdata, Uint8* stream, int len) {
    auto* b = static_cast<Backend*>(userdata);
    int frames = len / static_cast<int>(sizeof(int16_t) *
                                        (b ? b->audio_channels : 1));
    if (b && b->bridge_ready) {
        SDL_LockMutex(b->audio_mtx);
        rab_pull(&b->bridge, reinterpret_cast<int16_t*>(stream), frames);
        SDL_UnlockMutex(b->audio_mtx);
    } else {
        SDL_memset(stream, 0, len);
    }
}

}  // namespace

HostWindow::HostWindow() = default;

HostWindow::~HostWindow() {
    close();
}

bool HostWindow::is_available() { return true; }

bool HostWindow::open(int scale, int base_w, int base_h, const char* title,
                      const char* screen, bool linear_filter,
                      bool resize_driven_view, bool stereo_audio) {
    if (open_) return true;
    if (scale < 1) scale = 1;
    if (base_w < 1) base_w = 240;
    if (base_h < 1) base_h = 160;

    if (SDL_WasInit(SDL_INIT_VIDEO) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
            std::fprintf(stderr, "host_window: SDL_InitSubSystem(VIDEO) failed: %s\n",
                         SDL_GetError());
            return false;
        }
    }
    if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
        // Non-fatal if audio fails â€” keep video working.
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            std::fprintf(stderr, "host_window: SDL_InitSubSystem(AUDIO) failed: %s\n",
                         SDL_GetError());
        }
    }

    auto* b = new Backend{};
    b->base_w = base_w;
    b->base_h = base_h;
    b->expanded_view = base_w != 240 || base_h != 160;
    b->resize_driven_view = resize_driven_view;
    b->linear_filter = linear_filter;
    b->scale = scale;
    b->native_scale = native_scale_from_env();
    b->strict_static = env_flag("GBARECOMP_STRICT_STATIC");
    b->native_renderer = env_flag("GBARECOMP_NATIVE_RENDERER", b->native_renderer);
    b->title = title ? title : "gbarecomp";
    // Gamepad. A host-side convenience: if it fails to come up the emulator
    // runs exactly as before, just without pad input. (The config UI is
    // initialized further down â€” it needs the window and renderer, which do
    // not exist yet here.)
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0) {
        for (int i = 0; i < SDL_NumJoysticks(); ++i) {
            if (!SDL_IsGameController(i)) continue;
            b->pad = SDL_GameControllerOpen(i);
            if (b->pad) {
                std::fprintf(stderr, "host_window: controller: %s\n",
                             SDL_GameControllerName(b->pad));
                break;
            }
        }
    }
    {
        static const int kDefaultPads[10] = {
            SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B,
            SDL_CONTROLLER_BUTTON_BACK, SDL_CONTROLLER_BUTTON_START,
            SDL_CONTROLLER_BUTTON_DPAD_RIGHT, SDL_CONTROLLER_BUTTON_DPAD_LEFT,
            SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
            SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
            SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
        };
        for (int i = 0; i < 10; ++i) b->pad_bind[i] = kDefaultPads[i];
    }
    std::memcpy(b->bind_sc, kDefaultBinds, sizeof(kDefaultBinds));
    for (int h = 0; h < HK_COUNT; ++h)
        b->hotkeys[h] = parse_hotkey(kHotkeyDefaults[h]);
    // Linear vs nearest scaling is a texture-creation-time hint.
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, linear_filter ? "linear" : "nearest");
    const int win_w = shown_width(b, base_w, base_h) * scale;
    const int win_h = base_h * scale;
    // Keep the host window user-resizable for both canonical and enhanced
    // presentations. Canonical mode still preserves a 240x160 logical image;
    // native mode removes that SDL logical-size constraint and fits its
    // supersampled texture to the live drawable below.
    Uint32 window_flags = SDL_WINDOW_SHOWN |
        static_cast<Uint32>(SDL_WINDOW_RESIZABLE);
#if defined(GBARECOMP_HAVE_IMGUI)
    // Real ImGui multi-viewport needs a GL-capable window: imgui_impl_opengl3
    // is the only backend among the ones we ship with viewport support
    // (imgui_impl_sdlrenderer2 has none upstream). The renderer driver
    // selection below tries "opengl" first and falls back to the historical
    // d3d11/software chain on the SAME window if that fails, so this flag is
    // harmless even when the opengl renderer never ends up chosen.
    window_flags |= static_cast<Uint32>(SDL_WINDOW_OPENGL);
#endif
    b->window = SDL_CreateWindow(title ? title : "gbarecomp",
                                 SDL_WINDOWPOS_CENTERED,
                                 SDL_WINDOWPOS_CENTERED,
                                 win_w, win_h,
                                 window_flags);
    if (!b->window) {
        std::fprintf(stderr, "host_window: SDL_CreateWindow failed: %s\n",
                     SDL_GetError());
        delete b;
        return false;
    }
    // The independent FramePacer still governs emulation at 59.7275 Hz
    // (MC-HP-004) â€” vsync below only aligns scanout, in series after the
    // pacer, and can never become the game clock.
    // HP-002: EVERY path now requests a synchronized present. The cadence
    // probe measured the legacy D3D9 blit returning in <1 ms despite
    // vsync=yes (presents never sync to scanout â†’ the tear band users see
    // toward the bottom at native res). SDL2's D3D11 backend is a DXGI
    // flip-model swapchain whose vsync genuinely blocks, and windowed VRR
    // (G-Sync "windowed and full screen") can engage through it â€” so prefer
    // it by default on Windows; SDL_RENDER_DRIVER in the environment still
    // overrides. GBARECOMP_NO_VSYNC=1 restores the historical
    // unsynchronized present for A/B.
    const bool want_vsync = !env_flag("GBARECOMP_NO_VSYNC");
    const Uint32 renderer_flags = SDL_RENDERER_ACCELERATED |
        (want_vsync ? static_cast<Uint32>(SDL_RENDERER_PRESENTVSYNC)
                    : Uint32{0});
#if defined(GBARECOMP_HAVE_IMGUI)
    // Real ImGui multi-viewport (panels torn into their own OS windows)
    // needs imgui_impl_opengl3, the only shipped backend with upstream
    // viewport support. Try the "opengl" SDL render driver first; if this
    // system can't create one (no GL, driver issue), b->renderer stays null
    // and the direct3d11/software chain below runs exactly as before —
    // config_ui_init() below detects which driver actually won and routes
    // to the matching ImGui backend (docking-only, no viewports) at runtime,
    // not via a compile-time driver choice.
    SDL_SetHintWithPriority(SDL_HINT_RENDER_DRIVER, "opengl",
                            SDL_HINT_DEFAULT);
    b->renderer = SDL_CreateRenderer(b->window, -1, renderer_flags);
    if (!b->renderer) {
        std::fprintf(stderr,
                     "host_window: opengl renderer unavailable, falling "
                     "back (no multi-viewport UI): %s\n", SDL_GetError());
    }
#endif
    if (!b->renderer) {
#if defined(_WIN32)
        SDL_SetHintWithPriority(SDL_HINT_RENDER_DRIVER, "direct3d11",
                                SDL_HINT_DEFAULT);
#endif
        b->renderer = SDL_CreateRenderer(b->window, -1, renderer_flags);
    }
    if (!b->renderer && want_vsync) {
        std::fprintf(stderr,
                     "host_window: synchronized renderer unavailable; "
                     "falling back to unsynchronized presentation: %s\n",
                     SDL_GetError());
        b->renderer = SDL_CreateRenderer(
            b->window, -1, SDL_RENDERER_ACCELERATED);
    }
    if (!b->renderer) {
        // Fall back to software renderer if accelerated path is
        // unavailable (headless Windows, RDP, etc.).
        b->renderer = SDL_CreateRenderer(b->window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!b->renderer) {
        std::fprintf(stderr, "host_window: SDL_CreateRenderer failed: %s\n",
                     SDL_GetError());
        SDL_DestroyWindow(b->window);
        delete b;
        return false;
    }
    {
        SDL_RendererInfo info{};
        if (SDL_GetRendererInfo(b->renderer, &info) == 0) {
            // The actual driver that won, not the one we hinted for — the
            // hint is a preference, not a guarantee. Multi-viewport ImGui
            // is only wired up when this really is the opengl driver.
            b->renderer_is_opengl =
                info.name && std::strcmp(info.name, "opengl") == 0;
            std::fprintf(stderr,
                         "host_window: renderer=%s flags=0x%08x vsync=%s%s\n",
                         info.name ? info.name : "unknown",
                         static_cast<unsigned>(info.flags),
                         (info.flags & SDL_RENDERER_PRESENTVSYNC) ? "yes" : "no",
                         b->resize_driven_view ? " (adaptive)" : "");
            std::fflush(stderr);
        }
        log_display_mode(b->window, "open");
    }
    b->cadence.init();
    // Test-variable switches are process-scoped and already resolved by the
    // runtime/launcher. Cache their display values once at window bring-up;
    // the overlay never performs getenv during a frame.
    b->cfg.debug_overlay_state.self_heal_ram =
        !b->strict_static && env_flag("GBARECOMP_SELFHEAL_RAM");
    b->cfg.debug_overlay_state.cost_probe =
        env_flag("GBARECOMP_COST_PROBE");
    b->cfg.debug_overlay_state.present_cadence = b->cadence.verbose;
    b->cfg.debug_overlay_state.ram_churn_probe =
        env_flag("GSR_RAM_CHURN_PROBE");
    b->obj_record_frame_marker = env_flag("GSR_OBJ_RECORD");
    b->screen_filters_on =
        env_flag("GSR_SCREEN_FILTERS") && b->renderer_is_opengl;
    if (b->expanded_view || b->resize_driven_view) {
        // The destination viewport is computed explicitly in present() so
        // resizing maximally fills the drawable at the selected widescreen
        // aspect. Exact multiples retain integer scale; filtering follows the
        // linear_filter choice set above (historical default: nearest).
        SDL_SetRenderDrawColor(b->renderer, 0, 0, 0, 255);
    } else {
        // Non-opting games retain the historical fixed 240x160 SDL logical
        // renderer, including its window flags and copy path.
        SDL_RenderSetLogicalSize(b->renderer, base_w, base_h);
        SDL_RenderSetIntegerScale(b->renderer, SDL_TRUE);
    }

    b->texture = SDL_CreateTexture(b->renderer,
                                   SDL_PIXELFORMAT_RGB24,
                                   SDL_TEXTUREACCESS_STREAMING,
                                   base_w, base_h);
    if (!b->texture) {
        std::fprintf(stderr, "host_window: SDL_CreateTexture failed: %s\n",
                     SDL_GetError());
        SDL_DestroyRenderer(b->renderer);
        SDL_DestroyWindow(b->window);
        delete b;
        return false;
    }

    // Config UI. Needs a live window + renderer, so it comes up here rather
    // than with the rest of the host-side conveniences above. Failure is
    // non-fatal: the emulator runs exactly as before, just without the menu.
    b->cfg_ready = config_ui_init(b->window, b->renderer, b->renderer_is_opengl);
    if (!b->cfg_ready) {
        std::fprintf(stderr,
                     "host_window: config UI unavailable (built without "
                     "Dear ImGui, or init failed) â€” %s is inert\n",
                     kHotkeyDefaults[HK_MENU]);
    }
    // Seed the UI's view of video state from what was actually negotiated,
    // so the checkboxes open showing the truth rather than their defaults.
    {
        SDL_RendererInfo info{};
        b->vsync = SDL_GetRendererInfo(b->renderer, &info) == 0 &&
                   (info.flags & SDL_RENDERER_PRESENTVSYNC) != 0;
        b->cfg.vsync = b->vsync;
        b->cfg.linear_filter = b->linear_filter;
        b->cfg.integer_scale = b->integer_scale;
        b->cfg.native_scale = b->native_scale;
        b->cadence.dwm_refresh_now(&b->display_refresh_hz);
        if (!(b->display_refresh_hz > 0.0)) {
            const int display = SDL_GetWindowDisplayIndex(b->window);
            SDL_DisplayMode mode{};
            if (display >= 0 &&
                SDL_GetCurrentDisplayMode(display, &mode) == 0) {
                b->display_refresh_hz = static_cast<double>(mode.refresh_rate);
            }
        }
        b->frame_interpolation_display_ok = b->display_refresh_hz >= 119.0;
        b->cfg.frame_interpolation_display_ok =
            b->frame_interpolation_display_ok;
        std::fprintf(stderr, "host_window: measured refresh %.6f Hz%s\n",
                     b->display_refresh_hz,
                     b->display_refresh_hz > 0.0 ? "" : " (unavailable)");
    }

    // Open the audio device at 65536 Hz 16-bit signed. The normal faithful
    // path remains mono; canonical-stereo and native-MP2K opt-ins use stereo.
    // running BIOS sets SOUNDBIAS resolution=1 which raises the
    // mixer's effective sample rate from 32768 to 65536; opening
    // the device at 32768 (the power-on default) caused SDL to play
    // back at half speed, hit the 250 ms queue cap, and flush â€”
    // audible as muffled / watery chime artifacts. SDL2 resamples
    // internally if the host hardware doesn't natively support
    // 65536, so this rate is portable. Failure is non-fatal â€”
    // silent video still works.
    SDL_AudioSpec want{};
    want.freq     = 65536;
    want.format   = AUDIO_S16SYS;
    b->audio_channels = stereo_audio ? 2 : 1;
    want.channels = static_cast<Uint8>(b->audio_channels);
    want.samples  = 1024;  // ~15 ms callback quantum at 65 kHz
    want.callback = gba_audio_callback;  // pull mode (bridge-fed)
    want.userdata = b;
    SDL_AudioSpec got{};
    // Allow the device to pick its native rate; the bridge resamples the GBA
    // mixer rate (65536) to whatever the device opened at.
    b->audio_dev = SDL_OpenAudioDevice(nullptr, /*iscapture=*/0,
                                       &want, &got, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (b->audio_dev != 0) {
        b->audio_mtx = SDL_CreateMutex();
        rab_config cfg;
        rab_config_defaults(&cfg);
        cfg.channels    = b->audio_channels;
        cfg.source_rate = 65536.0;                 // engine's standardized GBA mixer rate
        cfg.host_rate   = static_cast<double>(got.freq);
        cfg.target_ms   = 60.0;                     // steady cushion (matches NES)
        cfg.preroll_ms  = 250.0;                    // boot pre-roll: hide the cold-start
                                                    // recomp warm-up hitch (drains to target)
        if (rab_init(&b->bridge, &cfg) == 0) b->bridge_ready = true;
        SDL_PauseAudioDevice(b->audio_dev, 0);      // start the callback
    }

    const runtime::ColorSettings initial_color = resolve_color_settings(screen);
    b->screen_kind = initial_color.screen;
    b->cfg.screen_kind = static_cast<int>(b->screen_kind);
    b->color_lut = std::make_unique<runtime::ColorLut>(initial_color);
    if (!b->color_lut->is_passthrough())
        b->graded_fb.resize(static_cast<std::size_t>(base_w) * base_h * 3);

    impl_ = b;
    open_ = true;
    return true;
}

// Game-thread codegen time (overlay_loader.cpp) â€” forward-declared to avoid a
// heavy include. ~0 during async play; >0 means the game thread is compiling.
// (We are already inside namespace gbarecomp, so declare it unqualified.)
uint64_t overlay_game_thread_compile_ns();

static void probe_stereo_audio_push(Backend* b, std::size_t count) {
    static int s_probe = -1;
    if (s_probe < 0) {
        s_probe = env_flag("GBARECOMP_AUDIO_PROBE") ? 1 : 0;
    }
    if (!s_probe) return;
    static unsigned long long s_pushes = 0, s_samples = 0;
    s_pushes++;
    s_samples += count;
    if ((s_pushes % 120ULL) != 0ULL) return;
    rab_stats st;
    rab_get_stats(&b->bridge, &st);
    double secs = static_cast<double>(s_samples) / 65536.0;
    double stretch_ms = st.stretch_frames * 1000.0 /
                        static_cast<double>(b->bridge.cfg.host_rate);
    static unsigned long long s_prev_cc_ns = 0;
    unsigned long long cc_ns = overlay_game_thread_compile_ns();
    double gt_ms = cc_ns / 1e6;
    double gt_dms = (cc_ns - s_prev_cc_ns) / 1e6;
    s_prev_cc_ns = cc_ns;
    std::fprintf(stderr,
        "[gba-audio-probe] pushes=%llu audio=%.1fs bridge_underrun=%llu(%.2f/s) "
        "stretch=%.0fms(ev=%llu) overflow_drops=%llu fill_ms=%.1f corr=%+.3f%% "
        "gt_compile=%.1fms(+%.1fms)\n",
        s_pushes, secs, (unsigned long long)st.underrun_events,
        secs > 0 ? st.underrun_events / secs : 0.0,
        stretch_ms, (unsigned long long)st.stretch_events,
        (unsigned long long)st.overflow_drops, rab_fill_ms(&b->bridge),
        st.last_correction * 100.0, gt_ms, gt_dms);
    std::fflush(stderr);
}

void HostWindow::push_audio_samples(const int16_t* samples, std::size_t count) {
    if (!open_ || !impl_ || !samples || count == 0) return;
    auto* b = static_cast<Backend*>(impl_);
    if (b->turbo_audio_muted) return;
    if (b->audio_dev == 0 || !b->bridge_ready) return;

    // A native-capable device expects interleaved frames. Keep the canonical
    // producer safe if it is used while that device is open.
    if (b->audio_channels == 2) {
        b->volume_buf.resize(count * 2);
        for (std::size_t i = 0; i < count; ++i) {
            int32_t v = samples[i];
            if (b->volume != 100) v = (v * b->volume) / 100;
            b->volume_buf[i * 2] = static_cast<int16_t>(v);
            b->volume_buf[i * 2 + 1] = static_cast<int16_t>(v);
        }
        SDL_LockMutex(b->audio_mtx);
        rab_push(&b->bridge, b->volume_buf.data(), static_cast<int>(count));
        SDL_UnlockMutex(b->audio_mtx);
        return;
    }

    // Volume (launcher setting + VolumeUp/Down hotkeys): scale into scratch
    // before the bridge. 100 = passthrough, byte-identical to before.
    if (b->volume != 100) {
        b->volume_buf.resize(count);
        const int v = b->volume;
        for (std::size_t i = 0; i < count; ++i)
            b->volume_buf[i] = static_cast<int16_t>(
                (static_cast<int32_t>(samples[i]) * v) / 100);
        samples = b->volume_buf.data();
    }

    // Producer: append mono frames into the bridge ring. The SDL callback
    // (gba_audio_callback) drains it at the device rate with band-limited
    // resampling + a P-only fill servo â€” no queue underrun, no hard flush.
    SDL_LockMutex(b->audio_mtx);
    rab_push(&b->bridge, samples, static_cast<int>(count)); // mono: count == frames
    SDL_UnlockMutex(b->audio_mtx);

    // â”€â”€ NES-mode crackle probe (measure step) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    // GBARECOMP_AUDIO_PROBE=1 reports the BRIDGE's underrun/overflow counters
    // (the post-fix equivalent of SDL queue underruns) so a before/after is
    // directly comparable. Expect ~0 underruns once primed.
    static int s_probe = -1;
    if (s_probe < 0) { s_probe = env_flag("GBARECOMP_AUDIO_PROBE") ? 1 : 0; }
    if (s_probe) {
        static unsigned long long s_pushes = 0, s_samples = 0;
        s_pushes++; s_samples += count;
        if ((s_pushes % 120ULL) == 0ULL) {
            rab_stats st; rab_get_stats(&b->bridge, &st);
            double secs = static_cast<double>(s_samples) / 65536.0;
            double stretch_ms = st.stretch_frames * 1000.0
                              / static_cast<double>(b->bridge.cfg.host_rate);
            // Game-thread codegen time: cumulative + this-window delta. The delta
            // is the smoking gun â€” async play holds it at 0; sync play grows it.
            static unsigned long long s_prev_cc_ns = 0;
            unsigned long long cc_ns = overlay_game_thread_compile_ns();
            double gt_ms      = cc_ns / 1e6;
            double gt_dms     = (cc_ns - s_prev_cc_ns) / 1e6;
            s_prev_cc_ns = cc_ns;
            std::fprintf(stderr,
                "[gba-audio-probe] pushes=%llu audio=%.1fs bridge_underrun=%llu(%.2f/s) "
                "stretch=%.0fms(ev=%llu) overflow_drops=%llu fill_ms=%.1f corr=%+.3f%% "
                "gt_compile=%.1fms(+%.1fms)\n",
                s_pushes, secs, (unsigned long long)st.underrun_events,
                secs > 0 ? st.underrun_events / secs : 0.0,
                stretch_ms, (unsigned long long)st.stretch_events,
                (unsigned long long)st.overflow_drops, rab_fill_ms(&b->bridge),
                st.last_correction * 100.0, gt_ms, gt_dms);
            std::fflush(stderr);
        }
    }
}

void HostWindow::close() {
    if (!impl_) { open_ = false; return; }
    auto* b = static_cast<Backend*>(impl_);
    b->cadence.dump();  // MC-WS-002: flush the cadence ring (verbose only)
    if (b->audio_dev) SDL_CloseAudioDevice(b->audio_dev);  // stops the callback first
    if (b->bridge_ready) rab_free(&b->bridge);
    if (b->audio_mtx) SDL_DestroyMutex(b->audio_mtx);
    // SHUTDOWN-FREEZE-01: config_ui_shutdown() was never called on any exit
    // path. With the old imgui_impl_sdlrenderer2 backend that was a harmless
    // leak; with the OpenGL backend + ImGuiConfigFlags_ViewportsEnable,
    // ImGui owns live detached OS windows each with its own GL context, and
    // SDL_DestroyRenderer() below destroys the MAIN GL context out from
    // under them, hanging the driver on exit. Must run before the texture/
    // renderer/window teardown, and only once (cfg_ready guards a double
    // close()).
    if (b->cfg_ready) {
        config_ui_shutdown();
        b->cfg_ready = false;
    }
    if (b->texture)   SDL_DestroyTexture(b->texture);
    destroy_native_texture(b);
    screen_filter_shutdown();
    if (b->renderer)  SDL_DestroyRenderer(b->renderer);
    if (b->window)    SDL_DestroyWindow(b->window);
    delete b;
    impl_ = nullptr;
    open_ = false;
}

bool HostWindow::set_surface_size(int base_w, int base_h) {
    if (!open_ || !impl_ || base_w < 1 || base_h < 1) return false;
    auto* b = static_cast<Backend*>(impl_);
    if (base_w == b->base_w && base_h == b->base_h) return true;

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY,
                b->linear_filter ? "linear" : "nearest");
    SDL_Texture* replacement = SDL_CreateTexture(
        b->renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING,
        base_w, base_h);
    if (!replacement) {
        std::fprintf(stderr,
                     "host_window: dynamic SDL_CreateTexture failed: %s\n",
                     SDL_GetError());
        return false;
    }
    SDL_DestroyTexture(b->texture);
    b->texture = replacement;
    b->base_w = base_w;
    b->base_h = base_h;
    b->expanded_view = base_w != 240 || base_h != 160;
    // The faithful path installs SDL's 240x160 logical transform at open.
    // Fixed widescreen presents use an explicit destination rectangle in
    // renderer-output pixels; leaving that transform installed makes SDL
    // scale the rectangle a second time, pushing the image toward the
    // bottom-right and clipping it. Adaptive view owns its surface size and
    // already runs without a logical transform, so leave that path alone.
    if (!b->resize_driven_view) {
        if (b->expanded_view && !b->native_renderer) {
            SDL_RenderSetLogicalSize(b->renderer, 0, 0);
        } else {
            set_native_window_mode(b, b->native_renderer);
        }
    }

    // A fixed-view toggle changes the logical surface, so keep the window at
    // the current integer scale instead of presenting a 288x160 surface in
    // the old 240x160-sized window. Fullscreen and adaptive resizing own
    // their geometry and must not be changed here.
    if (!b->resize_driven_view && !b->fullscreen && b->window) {
        SDL_SetWindowSize(b->window,
                          shown_width(b, b->base_w, b->base_h) * b->scale,
                          b->base_h * b->scale);
        SDL_SetWindowPosition(b->window, SDL_WINDOWPOS_CENTERED,
                              SDL_WINDOWPOS_CENTERED);
    }
    destroy_native_texture(b);
    if (b->color_lut && !b->color_lut->is_passthrough())
        b->graded_fb.resize(static_cast<std::size_t>(base_w) * base_h * 3u);
    return true;
}

bool HostWindow::drawable_size(int* width, int* height) const {
    if (!open_ || !impl_ || !width || !height) return false;
    const auto* b = static_cast<const Backend*>(impl_);
    // Capture/debug override: GBARECOMP_FORCE_DRAWABLE="WxH" makes the
    // resize-driven view resolve as if the client window were WxH, so the
    // adaptive wide path can be exercised faithfully under the SDL dummy
    // video driver (headless, session-independent) for the MC-WS-002
    // frame-capture investigation. No effect unless set.
    static int s_fd_w = -1, s_fd_h = -1;
    if (s_fd_w == -1) {
        s_fd_w = 0;
        if (const char* e = std::getenv("GBARECOMP_FORCE_DRAWABLE")) {
            int w = 0, h = 0;
            if (std::sscanf(e, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                s_fd_w = w; s_fd_h = h;
            }
        }
    }
    if (s_fd_w > 0) { *width = s_fd_w; *height = s_fd_h; return true; }
    // The feature follows window aspect, not texture/renderer target size.
    // Some SDL backends keep RendererOutputSize pinned to the streaming
    // target while the client window is resized, so use the authoritative
    // live client extent first. HiDPI scaling is uniform and does not alter
    // the aspect ratio used by the policy.
    SDL_GetWindowSize(b->window, width, height);
    if (*width > 0 && *height > 0) return true;
    return SDL_GetRendererOutputSize(b->renderer, width, height) == 0 &&
           *width > 0 && *height > 0;
}


void HostWindow::present(const uint8_t* rgb888) {
    if (!open_ || !impl_) return;
    auto* b = static_cast<Backend*>(impl_);
    const bool supplied_native = b->native_scene_pending &&
        !b->native_fb.empty() && b->native_scene_w > 0 && b->native_scene_h > 0;
    if (!rgb888 && !supplied_native) return;

    // VFX-FLICKER-02: temporal frame blend ("LCD ghosting"), opt-in and OFF
    // by default. Runs first — before color grading, integer/native scaling,
    // and any of that downstream work below — directly on the PPU-produced
    // frame, so it sits ahead of every other presentation stage the same way
    // it would on a real GBA's screen (the pixels blend, then everything
    // downstream of the "screen" is just how a photo of that would be
    // scaled/graded). Skipped entirely for the native-compositor path
    // (supplied_native is a separately-scaled buffer this feature does not
    // reach — that path is out of scope here, see VFX-FLICKER-02 report) and
    // force-disabled whenever 2x scene interpolation is the user's enabled
    // setting: interpolation presents a synthesized midpoint frame and the
    // real frame through this same function every guest frame (see
    // runtime.cpp's present_first/finish_presentation_pacing), and this
    // function cannot tell which call is which, so blending across that
    // synthetic/real pair would smear the picture rather than steady it.
    // Turbo/frameskip need no special handling here: present() is only ever
    // called for a frame runtime.cpp's turbo_present_decimator actually
    // decided to present, so temporal_blend_prev below only ever holds a
    // consecutively PRESENTED frame, never a guest frame from several
    // skipped frames ago.
    {
        const bool interpolation_engaged =
            b->cfg.frame_interpolation_available && b->cfg.frame_interpolation_2x;
        const int blend_index = std::clamp(b->temporal_blend_index, 0,
                                           gbarecomp::kTemporalBlendItemCount - 1);
        const bool blend_engaged = !supplied_native && rgb888 != nullptr &&
            blend_index != 0 && !interpolation_engaged;
        if (blend_engaged) {
            const bool selective =
                gbarecomp::temporal_blend_index_is_selective(blend_index);
            const float weight =
                gbarecomp::temporal_blend_weight_for_index(blend_index);
            const std::size_t bytes =
                static_cast<std::size_t>(b->base_w) * b->base_h * 3u;
            if (b->temporal_blend_prev.size() != bytes ||
                b->temporal_blend_prev2.size() != bytes) {
                b->temporal_blend_prev.assign(bytes, 0);
                b->temporal_blend_prev2.assign(bytes, 0);
                b->temporal_blend_history_count = 0;  // buffers just (re)sized
            }
            if (b->temporal_blend_out.size() != bytes)
                b->temporal_blend_out.resize(bytes);
            // Selective mode needs frame N-1 AND N-2; whole-frame mode
            // needs only N-1. Either way, pass the frame through untouched
            // (never a read of stale/zeroed history) until enough real
            // previous frames have been captured.
            const int required = selective ? 2 : 1;
            const bool did_blend = b->temporal_blend_history_count >= required;
            if (did_blend) {
                if (selective) {
                    gbarecomp::temporal_blend_apply_selective(
                        rgb888, b->temporal_blend_prev.data(),
                        b->temporal_blend_prev2.data(),
                        b->temporal_blend_out.data(), bytes, weight);
                } else {
                    gbarecomp::temporal_blend_apply(
                        rgb888, b->temporal_blend_prev.data(),
                        b->temporal_blend_out.data(), bytes, weight);
                }
            }
            // Rotate history for the next frame by swapping the prev/prev2
            // buffers (pointer swap, not a byte copy) so what was frame
            // N-1 becomes frame N-2, then snapshot the current raw frame
            // into the now-freed prev slot. Runs every engaged frame,
            // regardless of mode, so a live mode switch (e.g. whole-frame
            // -> flicker only) has real N-2 history as soon as it's needed
            // rather than restarting from empty. Must happen after the
            // blend call above (which still needs the OLD prev/prev2) but
            // before rgb888 is possibly reassigned to out below (rgb888
            // here is still the original raw frame either way).
            std::swap(b->temporal_blend_prev, b->temporal_blend_prev2);
            std::memcpy(b->temporal_blend_prev.data(), rgb888, bytes);
            if (b->temporal_blend_history_count < 2)
                ++b->temporal_blend_history_count;
            if (did_blend) rgb888 = b->temporal_blend_out.data();
        } else {
            // Not actively blending this frame (Off, interpolation active,
            // or the native-compositor path) — drop history so a later
            // re-enable starts clean instead of blending against stale or
            // differently-sized frames.
            b->temporal_blend_history_count = 0;
        }
    }

    // Present-time color grading (opt-in). Raw is passthrough: the raw
    // PPU frame is uploaded untouched, so verify/frame-hash are unaffected.
    if (!supplied_native && b->color_lut && !b->color_lut->is_passthrough() &&
        b->graded_fb.size() == static_cast<std::size_t>(b->base_w) * b->base_h * 3u) {
        b->color_lut->map_rgb888(rgb888, b->graded_fb.data(), b->base_w, b->base_h);
        rgb888 = b->graded_fb.data();
    }
    // Keep the video and payload-free diagnostics in the same guest-frame
    // domain. This is intentionally gated by the existing Record sprite
    // placement switch; normal captures and gameplay remain unchanged.
    const uint8_t* diagnostic_rgb888 = rgb888;
    const uint8_t* diagnostic_native = nullptr;
    int diagnostic_w = b->base_w;
    int diagnostic_h = b->base_h;
    if (b->obj_record_frame_marker) {
        const uint8_t* source = supplied_native ? b->native_fb.data() : rgb888;
        diagnostic_w = supplied_native ? b->native_scene_w : b->base_w;
        diagnostic_h = supplied_native ? b->native_scene_h : b->base_h;
        if (source && diagnostic_w > 0 && diagnostic_h > 0) {
            const std::size_t bytes = static_cast<std::size_t>(diagnostic_w) *
                diagnostic_h * 3u;
            b->diagnostic_fb.resize(bytes);
            std::memcpy(b->diagnostic_fb.data(), source, bytes);
            char label[64];
            std::snprintf(label, sizeof(label), "GUEST %llu",
                          static_cast<unsigned long long>(b->cfg.guest_frame));
            const int text_w = overlay_text_width(label);
            overlay_fill(b->diagnostic_fb.data(), diagnostic_w, diagnostic_h,
                         4, 4, text_w + 8, 16, 0, 0, 0, 220);
            overlay_text(b->diagnostic_fb.data(), diagnostic_w, diagnostic_h,
                         8, 8, label, 255, 220, 64);
            if (supplied_native)
                diagnostic_native = b->diagnostic_fb.data();
            else
                diagnostic_rgb888 = b->diagnostic_fb.data();
        }
    }
    const bool use_native = b->native_renderer && ensure_native_texture(b);
    SDL_Texture* frame_texture = b->texture;
    int frame_w = b->base_w;
    int frame_h = b->base_h;
    if (use_native) {
        if (supplied_native) {
            frame_w = b->native_scene_w;
            frame_h = b->native_scene_h;
        } else {
            native_upscale_nearest(diagnostic_rgb888, b->base_w, b->base_h,
                                   b->native_scale, b->native_fb);
            frame_w *= b->native_scale;
            frame_h *= b->native_scale;
        }
        frame_texture = b->native_texture;
        const uint8_t* native_upload = supplied_native && diagnostic_native
            ? diagnostic_native : b->native_fb.data();
        if (supplied_native && b->color_lut &&
            !b->color_lut->is_passthrough()) {
            const std::size_t bytes = static_cast<std::size_t>(frame_w) *
                frame_h * 3u;
            b->native_graded_fb.resize(bytes);
            b->color_lut->map_rgb888(native_upload,
                                     b->native_graded_fb.data(),
                                     frame_w, frame_h);
            native_upload = b->native_graded_fb.data();
        }
        SDL_UpdateTexture(b->native_texture, nullptr, native_upload,
                          frame_w * 3);
    } else {
        SDL_UpdateTexture(b->texture, nullptr, diagnostic_rgb888,
                          b->base_w * 3);
    }
    SDL_RenderClear(b->renderer);
    if (!b->expanded_view && !b->resize_driven_view && !use_native) {
        // Native 240x160: SDL's logical size scales this copy. A filter draws
        // with raw GL in output pixels, so it is placed by the same layout.
        bool filtered = false;
        if (b->screen_filter != ScreenFilter::Off) {
            int out_w = 0, out_h = 0;
            SDL_GetRendererOutputSize(b->renderer, &out_w, &out_h);
            const PresentationLayout layout = compute_presentation_layout(
                out_w, out_h, b->base_w, b->base_h,
                b->integer_scale ? ScalingMode::IntegerLetterbox
                                 : ScalingMode::AspectFill);
            const SDL_Rect whole = {0, 0, b->base_w, b->base_h};
            const SDL_Rect destination = {
                layout.x, layout.y, layout.width, layout.height};
            filtered = layout.width > 0 && layout.height > 0 &&
                       screen_filter_draw(b->renderer, frame_texture,
                                          b->screen_filter, whole, b->base_w,
                                          b->base_h, destination);
        }
        if (!filtered)
            SDL_RenderCopy(b->renderer, frame_texture, nullptr, nullptr);
    } else {
        int drawable_w = 0;
        int drawable_h = 0;
        // Preserve the established renderer-output path for fixed-width
        // extended views (including MMZ). Resize-driven view uses the same
        // live client dimensions that selected its logical width, so the
        // texture and destination cannot disagree on backends whose renderer
        // output stays pinned to the original streaming target.
        int logical_w = 0;
        int logical_h = 0;
        SDL_RenderGetLogicalSize(b->renderer, &logical_w, &logical_h);
        if (b->resize_driven_view || (use_native && logical_w > 0)) {
            SDL_GetWindowSize(b->window, &drawable_w, &drawable_h);
        } else if (SDL_GetRendererOutputSize(
                       b->renderer, &drawable_w, &drawable_h) != 0) {
            SDL_GetWindowSize(b->window, &drawable_w, &drawable_h);
        }
        // The "Integer scaling" preference has to be applied HERE. This copy
        // uses an explicit destination rect, so it never goes through
        // SDL_RenderSetLogicalSize and SDL_RenderSetIntegerScale cannot reach
        // it — before this, the checkbox was on by default and silently did
        // nothing on the path that actually presents the frame, so every
        // non-multiple window size got unevenly duplicated pixels.
        const int shown_w = shown_width(b, frame_w, frame_h);
        const PresentationLayout layout = compute_presentation_layout(
            drawable_w, drawable_h, shown_w, frame_h,
            b->integer_scale ? ScalingMode::IntegerLetterbox
                             : ScalingMode::AspectFill);
        if (layout.width > 0 && layout.height > 0) {
            if (use_native && b->native_texture) {
                // Native scale is an off-screen quality setting, not a window
                // resize setting. Use the filtered sampler for every native
                // factor above 1x: otherwise 2x-9x enlarge with nearest while
                // 10x downsamples with linear, which makes the intermediate
                // factors show hard scanline/edge artifacts.
                const bool downsampling = layout.width < shown_w ||
                    layout.height < frame_h;
                const bool native_filter = b->native_scale > 1;
                SDL_SetTextureScaleMode(
                    b->native_texture,
                    (b->linear_filter || native_filter || downsampling)
                        ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
            }
            const SDL_Rect destination = {
                layout.x, layout.y, layout.width, layout.height};
            const SDL_Rect source = {(frame_w - shown_w) / 2, 0, shown_w,
                                     frame_h};
            // Test-only screen filter; falls back to the plain copy below
            // whenever it cannot draw.
            const bool filtered =
                b->screen_filter != ScreenFilter::Off &&
                screen_filter_draw(b->renderer, frame_texture,
                                   b->screen_filter, source,
                                   shown_width(b, b->base_w, b->base_h),
                                   b->base_h, destination);
            // 3:2 copies the whole texture, exactly as before 4:3 existed.
            if (!filtered)
                SDL_RenderCopy(b->renderer, frame_texture,
                               shown_w < frame_w ? &source : nullptr,
                               &destination);
        }
    }
    // MC-WS-002: time the present itself (vsync blocks here â€” or doesn't)
    // and stamp the DWM refresh counter into the always-on cadence ring.
    const uint64_t cad_qpc0 = SDL_GetPerformanceCounter();
    // Config UI last, so it draws over the scaled guest frame at WINDOW
    // resolution rather than being upscaled with the 240x160 image.
    // This also expires the guest-speed sample while paused: pause re-presents
    // the same frame, but must not keep the old speed alive.
    age_emulation_speed(b, performance_counter_ns());
    if (b->cfg_ready) {
        // Commit SDL_Renderer's batched draw commands (the guest frame copy
        // above) to the GPU before ImGui issues its own GL calls on the
        // opengl path — SDL_Renderer batches by default, and leaving that
        // batch pending while ImGui draws would interleave state and corrupt
        // both pictures. A no-op on the non-GL renderer paths.
        SDL_RenderFlush(b->renderer);
        b->cfg.fps = b->fps_last;
        b->cfg.frame_ms = b->fps_last > 0.0f ? 1000.0f / b->fps_last : 0.0f;
        // The faithful path pins a 240x160 logical size, and the SDL_Renderer
        // ImGui backend draws in renderer coordinates — so the UI would come
        // out magnified by the window scale and clipped to the top-left
        // sixteenth of the window. ImGui already lays out in window pixels
        // (its io.DisplaySize is the window), so drop to 1:1 for the UI pass
        // and restore immediately: the guest frame was already copied above
        // and is not affected.
        int logical_w = 0, logical_h = 0;
        SDL_RenderGetLogicalSize(b->renderer, &logical_w, &logical_h);
        const bool had_logical = (logical_w > 0 && logical_h > 0);
        if (had_logical) SDL_RenderSetLogicalSize(b->renderer, 0, 0);
        config_ui_draw(&b->cfg);
        if (had_logical)
            SDL_RenderSetLogicalSize(b->renderer, logical_w, logical_h);
    }
    // Harness: GBARECOMP_WINDOW_SHOT=<path> writes ONE readback of the actual
    // renderer output â€” scaled guest frame plus any UI drawn over it â€” as a
    // binary PPM. The framedump path records the guest framebuffer instead, so
    // it cannot show host UI; this is the only capture that proves what was
    // really on screen.
    {
        static const char* shot_path = std::getenv("GBARECOMP_WINDOW_SHOT");
        // Which present to capture. The first few are before the guest has
        // drawn anything (and before any UI exists), so a shot at 0 is white.
        static const long shot_at = [] {
            const char* e = std::getenv("GBARECOMP_WINDOW_SHOT_AT");
            return e && *e ? std::strtol(e, nullptr, 0) : 120L;
        }();
        static long shot_seen = 0;
        static bool shot_done = false;
        if (shot_path && *shot_path && !shot_done && shot_seen++ >= shot_at) {
            shot_done = true;
            int rw = 0, rh = 0;
            if (SDL_GetRendererOutputSize(b->renderer, &rw, &rh) == 0 &&
                rw > 0 && rh > 0) {
                std::vector<uint8_t> px(static_cast<std::size_t>(rw) * rh * 3u);
                if (SDL_RenderReadPixels(b->renderer, nullptr,
                                         SDL_PIXELFORMAT_RGB24,
                                         px.data(), rw * 3) == 0) {
                    if (std::FILE* f = std::fopen(shot_path, "wb")) {
                        std::fprintf(f, "P6\n%d %d\n255\n", rw, rh);
                        std::fwrite(px.data(), 1, px.size(), f);
                        std::fclose(f);
                        std::fprintf(stderr,
                                     "host_window: wrote %dx%d window shot %s\n",
                                     rw, rh, shot_path);
                    }
                } else {
                    std::fprintf(stderr, "host_window: readback failed: %s\n",
                                 SDL_GetError());
                }
            }
        }
    }
    SDL_RenderPresent(b->renderer);
    b->cadence.record(cad_qpc0, SDL_GetPerformanceCounter(), b->fullscreen);

    // Count only completed SDL_RenderPresent calls. Turbo guest frames that
    // the decimator skips never enter this block, so FPS stays a display
    // cadence, not a misleading guest/target rate.
    ++b->fps_presents;
    const Uint64 now = SDL_GetPerformanceCounter();
    if (b->fps_window_start == 0) b->fps_window_start = now;
    const Uint64 frequency = SDL_GetPerformanceFrequency();
    const Uint64 span = now - b->fps_window_start;
    if (frequency != 0 && span >= frequency / 2) {
        const double seconds = static_cast<double>(span) /
                               static_cast<double>(frequency);
        b->fps_last = static_cast<float>(b->fps_presents / seconds);
        if (b->fps_readout) {
            char buf[192];
            std::snprintf(buf, sizeof(buf), "%s - %.0f fps - %.0f%% speed",
                          b->title.c_str(), b->fps_last,
                          b->cfg.emulation_speed_percent);
            SDL_SetWindowTitle(b->window, buf);
        }
        b->fps_window_start = now;
        b->fps_presents = 0;
    }
}

void HostWindow::load_input_config(const char* dir) {
    if (!open_ || !impl_ || !dir) return;
    auto* b = static_cast<Backend*>(impl_);
    const std::string base = std::string(dir) + "/";
    // Remembered so the rebind menu writes keybinds.ini back where it was
    // read from, rather than into whatever the process CWD happens to be.
    b->config_dir = dir;

    // keybinds.ini [player1] (recomp-ui generic format, scancode names).
    ini_scan_section((base + "keybinds.ini").c_str(), "player1",
                     [b](const char* key, const char* val) {
        for (const auto& bk : kBindKeys) {
            if (SDL_strcasecmp(key, bk.name) != 0) continue;
            SDL_Scancode sc = scancode_from_name(val);
            b->bind_sc[bk.bit] = sc;   // "None"/unknown => unbound (UNKNOWN)
            return;
        }
    });

    // keybinds.ini [player1_pad] (SDL game-controller button names). Absent
    // section => the built-in pad layout set in open().
    ini_scan_section((base + "keybinds.ini").c_str(), "player1_pad",
                     [b](const char* key, const char* val) {
        for (const auto& pk : kPadKeys) {
            if (SDL_strcasecmp(key, pk.name) != 0) continue;
            // Stick directions are synthetic ids with their own names.
            const int synth = pad_synth_from_name(val);
            if (synth >= 0) {
                b->pad_bind[pk.bit] = synth;
                return;
            }
            const SDL_GameControllerButton btn =
                SDL_GameControllerGetButtonFromString(val);
            b->pad_bind[pk.bit] = (btn == SDL_CONTROLLER_BUTTON_INVALID)
                ? -1 : static_cast<int>(btn);   // "None"/unknown => unbound
            return;
        }
    });

    // config.ini [KeyMap] (keycode names with Ctrl+/Alt+/Shift+ prefixes).
    ini_scan_section((base + "config.ini").c_str(), "KeyMap",
                     [b](const char* key, const char* val) {
        for (int h = 0; h < HK_COUNT; ++h) {
            if (SDL_strcasecmp(key, kHotkeyNames[h]) != 0) continue;
            b->hotkeys[h] = parse_hotkey(val);
            return;
        }
    });

    // config.ini [KeyMap.Pad] (UI-02): the controller half of the same
    // hotkey rows, same names, SDL game-controller button strings like
    // keybinds.ini [player1_pad]. Scanned after [KeyMap] above so it only
    // fills in pad_button on the struct parse_hotkey() already built â€”
    // never clobbers the keyboard half.
    ini_scan_section((base + "config.ini").c_str(), "KeyMap.Pad",
                     [b](const char* key, const char* val) {
        for (int h = 0; h < HK_COUNT; ++h) {
            if (SDL_strcasecmp(key, kHotkeyNames[h]) != 0) continue;
            // UI-02b: "lefttrigger"/"righttrigger" are SDL's own axis-name
            // strings (SDL_GameControllerGetStringForAxis on
            // SDL_CONTROLLER_AXIS_TRIGGERLEFT/RIGHT) — checked first since
            // SDL_GameControllerGetButtonFromString does not know them (L2/
            // R2 are axes, not buttons). A config.ini written before this
            // change never contained these strings, so old files are
            // unaffected and fall through to the button parse exactly as
            // before.
            // The stick direction names ("leftstickup", ...) come from the
            // same shared table.
            const int synth = pad_synth_from_name(val);
            if (synth >= 0) {
                b->hotkeys[h].pad_button = synth;
                return;
            }
            const SDL_GameControllerButton btn =
                SDL_GameControllerGetButtonFromString(val);
            b->hotkeys[h].pad_button = (btn == SDL_CONTROLLER_BUTTON_INVALID)
                ? -1 : static_cast<int>(btn);   // "None"/unknown => unbound
            return;
        }
    });

    // Speed settings share config.ini but live in their own section. Missing
    // or malformed values retain the conservative defaults.
    ini_scan_section((base + "config.ini").c_str(), "Speed",
                     [b](const char* key, const char* val) {
        if (SDL_strcasecmp(key, "TurboMultiplier") == 0) {
            const float parsed = std::strtof(val, nullptr);
            if (parsed >= 1.0f && parsed <= kMaxTurboMultiplier)
                b->turbo_multiplier = parsed;
        } else if (SDL_strcasecmp(key, "MuteDuringTurbo") == 0) {
            b->turbo_mute_requested = SDL_strcasecmp(val, "true") == 0 ||
                                      std::strcmp(val, "1") == 0;
        } else if (SDL_strcasecmp(key, "Uncapped") == 0) {
            b->turbo_uncapped = SDL_strcasecmp(val, "true") == 0 ||
                                std::strcmp(val, "1") == 0;
        }
    });
    b->cfg.turbo_multiplier = b->turbo_multiplier;
    b->cfg.mute_during_turbo = b->turbo_mute_requested;
    b->cfg.uncapped = b->turbo_uncapped;

    // Missing or malformed values keep the conservative first-run default.
    // Strict-static and capture policy can still force this preference off.
    // CPU Overclock (TURBO-B2-UI): start from whatever the process already
    // has live (the GBARECOMP_CPU_OVERCLOCK env var default, or 1x if unset)
    // so a first-ever run with no config.ini keeps the env var as the
    // effective default; a saved CpuOverclock= value then overrides it.
    // Off/On control: Off pins 1x, On pins the 50x ceiling. Backwards
    // compatibility with older config.ini files: the legacy "auto" value and
    // any numeric value greater than 1 (2/4/8 from the old multi-step combo)
    // both map to On (50x). "1", missing, or malformed map to Off (1x).
    b->overclock_factor = runtime_get_overclock_factor();
    ini_scan_section((base + "config.ini").c_str(), "Enhancements",
                     [b](const char* key, const char* val) {
        // EnhancedTiming is no longer a user setting: exact 60 Hz timing is
        // always on where the game allows it (see saved_enhanced_timing()),
        // so an old saved "false" is deliberately ignored.
        if (SDL_strcasecmp(key, "CpuOverclock") == 0) {
            if (SDL_strcasecmp(val, "auto") == 0) {
                b->overclock_factor = kOverclockFactors[1];
            } else {
                const long parsed = std::strtol(val, nullptr, 10);
                b->overclock_factor = parsed > 1 ? kOverclockFactors[1]
                                                  : kOverclockFactors[0];
            }
        } else if (SDL_strcasecmp(key, "Widescreen") == 0) {
            b->widescreen_config_present = true;
            b->cfg.widescreen = SDL_strcasecmp(val, "true") == 0 ||
                                std::strcmp(val, "1") == 0;
        } else if (SDL_strcasecmp(key, "ViewMode") == 0) {
            const long parsed = std::strtol(val, nullptr, 10);
            if (parsed >= 0) {
                b->view_mode_config_present = true;
                b->fixed_view_mode = static_cast<int>(parsed);
            }
        } else if (SDL_strcasecmp(key, "TemporalBlend") == 0) {
            // VFX-FLICKER-02/03: visible values are 0=Off, 1..3=Flicker
            // only Light/Medium/Strong. Legacy whole-frame values 4..6 are
            // normalized before the runtime sees them: 4->1, 5->2, 6->3.
            const long parsed = std::strtol(val, nullptr, 10);
            if (parsed >= 0 && parsed < gbarecomp::kTemporalBlendItemCount) {
                b->temporal_blend_index = parsed >= 4
                    ? static_cast<int>(parsed - 3)
                    : static_cast<int>(parsed);
                b->temporal_blend_needs_normalization = parsed >= 4;
            }
        } else if (SDL_strcasecmp(key, "ColorProfile") == 0) {
            // F1 > Video > Colours. GBARECOMP_SCREEN, read when the window
            // opened, still wins for a launch that sets it.
            runtime::ScreenKind k;
            if (!std::getenv("GBARECOMP_SCREEN") &&
                runtime::screen_kind_from_name(val, k) &&
                k != b->screen_kind) {
                b->screen_kind = k;
                b->cfg.screen_kind = static_cast<int>(k);
                rebuild_color_lut(b);
            }
        } else if (SDL_strcasecmp(key, "AspectRatio") == 0) {
            // F1 > Video > Aspect ratio. Anything but 4:3 is the original 3:2.
            b->aspect_4_3 = SDL_strcasecmp(val, "4:3") == 0;
            b->cfg.aspect = b->aspect_4_3 ? 1 : 0;
        } else if (SDL_strcasecmp(key, "ColorSaturation") == 0) {
            b->cfg.color_saturation = std::clamp(
                static_cast<int>(std::strtol(val, nullptr, 10)), 0, 200);
        } else if (SDL_strcasecmp(key, "ColorHue") == 0) {
            b->cfg.color_hue = std::clamp(
                static_cast<int>(std::strtol(val, nullptr, 10)), -180, 180);
        } else if (SDL_strcasecmp(key, "ColorBrightness") == 0) {
            b->cfg.color_brightness = std::clamp(
                static_cast<int>(std::strtol(val, nullptr, 10)), 50, 150);
        } else if (SDL_strcasecmp(key, "ColorWarmth") == 0) {
            b->cfg.color_warmth = std::clamp(
                static_cast<int>(std::strtol(val, nullptr, 10)), 0, 100);
        } else if (SDL_strcasecmp(key, "ColorDarken") == 0) {
            b->cfg.color_darken = std::clamp(
                static_cast<int>(std::strtol(val, nullptr, 10)), 0, 50);
        }
    });
    // The Custom colour keys may follow ColorProfile in the file.
    if (b->screen_kind == runtime::ScreenKind::Custom) rebuild_color_lut(b);
    // The window opened before this file was read: size it for a saved 4:3.
    // (A later set_surface_size sizes it the same way if the view changes.)
    if (b->aspect_4_3) apply_aspect_window_size(b);
    // Apply the resolved factor live and mirror it into the UI's combo index:
    // 0 (Off/1x) unless the resolved factor is the 50x ceiling.
    runtime_set_overclock_factor(b->overclock_factor);
    b->cfg.overclock_index =
        (b->overclock_factor == kOverclockFactors[1]) ? 1 : 0;
    b->cfg.temporal_blend_index = b->temporal_blend_index;
    if (b->temporal_blend_needs_normalization) {
        if (write_enhancements_ini(b->config_dir, b->cfg.enhanced_timing,
                                   b->overclock_factor,
                                   b->temporal_blend_index,
                                   b->view_mode_config_present
                                       ? b->fixed_view_mode
                                       : (b->cfg.widescreen ? 1 : 0))) {
            b->temporal_blend_needs_normalization = false;
        } else {
            b->cfg.enhancements_changed = true;
        }
    }

    // Gameplay cheats have their own section and are OFF when absent. Keep
    // these separate from [Enhancements] because they change guest state.
    int persisted_player_speed = 0;
    bool legacy_player_speed_present = false;
    bool legacy_player_speed_2x = false;
    ini_scan_section((base + "config.ini").c_str(), "Cheats",
                     [b, &persisted_player_speed,
                      &legacy_player_speed_present,
                      &legacy_player_speed_2x](const char* key,
                                               const char* val) {
        const bool on = SDL_strcasecmp(val, "true") == 0 ||
                        std::strcmp(val, "1") == 0;
        if (SDL_strcasecmp(key, "InfiniteHP") == 0)
            b->cfg.infinite_hp = on;
        else if (SDL_strcasecmp(key, "InfinitePP") == 0)
            b->cfg.infinite_pp = on;
        else if (SDL_strcasecmp(
                     key, "PlayerWalkRunSpeedMultiplier") == 0)
            persisted_player_speed =
                parse_player_walk_run_speed_multiplier(val);
        else if (SDL_strcasecmp(key, "PlayerWalkRun2x") == 0) {
            legacy_player_speed_present = true;
            legacy_player_speed_2x = parse_legacy_player_walk_run_2x(val);
        }
    });
    b->cfg.player_walk_run_speed_multiplier =
        resolve_player_walk_run_speed_multiplier(
            persisted_player_speed, legacy_player_speed_present,
            legacy_player_speed_2x);
    runtime_set_infinite_hp(b->cfg.infinite_hp ? 1 : 0);
    runtime_set_infinite_pp(b->cfg.infinite_pp ? 1 : 0);
    runtime_set_mem_write_override_enabled(
        b->cfg.player_walk_run_speed_multiplier);
    b->persisted_player_speed = b->cfg.player_walk_run_speed_multiplier;
    b->persisted_infinite_hp = b->cfg.infinite_hp;
    b->persisted_infinite_pp = b->cfg.infinite_pp;
    if (legacy_player_speed_present && persisted_player_speed == 0) {
        if (!write_cheats_ini(
                b->config_dir, b->cfg.infinite_hp, b->cfg.infinite_pp,
                b->cfg.player_walk_run_speed_multiplier)) {
            std::fprintf(stderr,
                         "host_window: could not migrate %s/config.ini\n",
                         b->config_dir.c_str());
        }
    }

    // Audio tab: Volume (0..100) and Mute, in their own [Audio] section.
    // Before this they lived only in memory, so every run started at 100%.
    // A --volume argument still wins (runtime.cpp applies it after this).
    ini_scan_section((base + "config.ini").c_str(), "Audio",
                     [b](const char* key, const char* val) {
        if (SDL_strcasecmp(key, "Volume") == 0)
            b->cfg.volume = std::clamp(std::atoi(val), 0, 100);
        else if (SDL_strcasecmp(key, "Mute") == 0)
            b->cfg.mute = SDL_strcasecmp(val, "true") == 0 ||
                          std::strcmp(val, "1") == 0;
    });
    b->volume = b->cfg.mute ? 0 : b->cfg.volume;
    b->persisted_volume = b->cfg.volume;
    b->persisted_mute = b->cfg.mute;

    // Logging tab: own [Logging] section (not folded into [Enhancements]).
    // Missing section (an INI from an older build) keeps the conservative
    // default (false / off). See runtime_arm.h
    // gsr_set_additional_debug_logging for what this controls.
    ini_scan_section((base + "config.ini").c_str(), "Logging",
                     [b](const char* key, const char* val) {
        if (SDL_strcasecmp(key, "AdditionalDebugLogging") == 0) {
            b->cfg.additional_debug_logging =
                SDL_strcasecmp(val, "true") == 0 || std::strcmp(val, "1") == 0;
        }
    });
    gsr_set_additional_debug_logging(b->cfg.additional_debug_logging ? 1 : 0);
    b->cfg.debug_overlay_state.additional_debug_logging =
        b->cfg.additional_debug_logging;
    ini_scan_section((base + "config.ini").c_str(), "Logging",
                     [b](const char* key, const char* val) {
        if (SDL_strcasecmp(key, "CrashLog") == 0)
            b->cfg.crash_log =
                SDL_strcasecmp(val, "true") == 0 || std::strcmp(val, "1") == 0;
    });
    runtime_set_crash_log(b->cfg.crash_log ? 1 : 0);
    // Developer Tools owns its UI preference in a small, independent
    // section. Missing values stay false for conservative first-run behavior.
    ini_scan_section((base + "config.ini").c_str(), "Debug",
                     [b](const char* key, const char* val) {
        if (SDL_strcasecmp(key, "DebugOverlay") == 0)
            b->cfg.debug_overlay = SDL_strcasecmp(val, "true") == 0 ||
                                   std::strcmp(val, "1") == 0;
    });
}

void HostWindow::set_fullscreen(bool on) {
    if (!open_ || !impl_) return;
    auto* b = static_cast<Backend*>(impl_);
    if (b->fullscreen == on) return;
    if (SDL_SetWindowFullscreen(b->window,
                                on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) == 0) {
        b->fullscreen = on;
        // Record which panel/mode the cadence data now runs on.
        log_display_mode(b->window, on ? "fullscreen" : "windowed");
    }
}

bool HostWindow::fullscreen() const {
    if (!open_ || !impl_) return false;
    return static_cast<const Backend*>(impl_)->fullscreen;
}

void HostWindow::adjust_scale(int delta) {
    if (!open_ || !impl_) return;
    auto* b = static_cast<Backend*>(impl_);
    if (b->fullscreen) return;   // meaningless while fullscreen
    int s = b->scale + delta;
    if (s < 1) s = 1;
    if (s > 8) s = 8;
    if (s == b->scale) return;
    b->scale = s;
    // A maximized window ignores SDL_SetWindowSize; restore it first so the
    // chosen size actually takes.
    if (SDL_GetWindowFlags(b->window) & SDL_WINDOW_MAXIMIZED)
        SDL_RestoreWindow(b->window);
    SDL_SetWindowSize(b->window, shown_width(b, b->base_w, b->base_h) * s,
                      b->base_h * s);
    SDL_SetWindowPosition(b->window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
}

void HostWindow::set_volume(int pct) {
    if (!open_ || !impl_) return;
    auto* b = static_cast<Backend*>(impl_);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    b->volume = pct;
}

int HostWindow::volume() const {
    if (!open_ || !impl_) return 100;
    return static_cast<const Backend*>(impl_)->volume;
}

void HostWindow::set_fps_readout(bool on) {
    if (!open_ || !impl_) return;
    auto* b = static_cast<Backend*>(impl_);
    if (b->fps_readout == on) return;
    b->fps_readout = on;
    b->fps_window_start = 0;
    b->fps_presents = 0;
    if (!on) SDL_SetWindowTitle(b->window, b->title.c_str());
}

bool HostWindow::fps_readout() const {
    if (!open_ || !impl_) return false;
    return static_cast<const Backend*>(impl_)->fps_readout;
}

double HostWindow::display_refresh_hz() const {
    if (!open_ || !impl_) return 0.0;
    return static_cast<const Backend*>(impl_)->display_refresh_hz;
}

void HostWindow::set_frame_interpolation_available(bool on) {
    if (!open_ || !impl_) return;
    auto* b = static_cast<Backend*>(impl_);
    b->frame_interpolation_game_allowed = on;
    b->cfg.frame_interpolation_available =
        on && b->frame_interpolation_display_ok;
    if (!b->cfg.frame_interpolation_available)
        b->cfg.frame_interpolation_2x = false;
}

void HostWindow::set_frame_interpolation_enabled(bool on) {
    if (!open_ || !impl_) return;
    auto* b = static_cast<Backend*>(impl_);
    b->cfg.frame_interpolation_2x =
        on && b->cfg.frame_interpolation_available;
}

void HostWindow::set_enhanced_timing_available(bool on) {
    if (!open_ || !impl_) return;
    auto* b = static_cast<Backend*>(impl_);
    b->enhanced_timing_game_allowed = on;
    b->cfg.enhanced_timing_available = on;
    if (!on) b->cfg.enhanced_timing = false;
    reset_emulation_speed(b);
    apply_audio_timing(b);
}

void HostWindow::set_enhanced_timing_enabled(bool on) {
    if (!open_ || !impl_) return;
    auto* b = static_cast<Backend*>(impl_);
    b->cfg.enhanced_timing = on && b->enhanced_timing_game_allowed;
    reset_emulation_speed(b);
    apply_audio_timing(b);
}

bool HostWindow::saved_enhanced_timing() const {
    // Exact 60 Hz timing is always on where the game allows it; it is no
    // longer a menu setting. GBARECOMP_ENHANCED_TIMING and strict/capture
    // policy still decide first (see enhanced_timing_initial_requested).
    return open_ && impl_;
}

void HostWindow::set_native_renderer_available(bool on) {
    if (!open_ || !impl_) return;
    auto* b = static_cast<Backend*>(impl_);
    b->native_renderer_game_allowed = on;
    b->cfg.native_renderer_available = on;
    if (!on) {
        b->native_renderer = false;
        b->cfg.native_renderer = false;
        destroy_native_texture(b);
        set_native_window_mode(b, false);
    }
}

void HostWindow::push_audio_samples_stereo(const int16_t* samples,
                                            std::size_t count) {
    if (!open_ || !impl_ || !samples || count == 0) return;
    auto* b = static_cast<Backend*>(impl_);
    if (b->turbo_audio_muted) return;
    if (b->audio_dev == 0 || !b->bridge_ready) return;
    if (b->audio_channels != 2) {
        // Graceful fallback when a host/device negotiated mono.
        for (std::size_t i = 0; i < count; ++i) {
            int32_t v = (static_cast<int32_t>(samples[i * 2]) +
                         static_cast<int32_t>(samples[i * 2 + 1])) / 2;
            b->volume_buf.resize(1);
            b->volume_buf[0] = static_cast<int16_t>(v);
            push_audio_samples(b->volume_buf.data(), 1);
        }
        return;
    }
    const int16_t* input = samples;
    if (b->volume != 100) {
        b->volume_buf.resize(count * 2);
        for (std::size_t i = 0; i < count * 2; ++i)
            b->volume_buf[i] = static_cast<int16_t>(
                (static_cast<int32_t>(samples[i]) * b->volume) / 100);
        input = b->volume_buf.data();
    }
    SDL_LockMutex(b->audio_mtx);
    rab_push(&b->bridge, input, static_cast<int>(count));
    SDL_UnlockMutex(b->audio_mtx);
    probe_stereo_audio_push(b, count);
}

void HostWindow::reset_audio() {
    if (!open_ || !impl_) return;
    auto* b = static_cast<Backend*>(impl_);
    if (b->audio_dev == 0 || !b->bridge_ready || !b->audio_mtx) return;
    SDL_LockMutex(b->audio_mtx);
    rab_reset(&b->bridge);
    SDL_UnlockMutex(b->audio_mtx);
}

void HostWindow::audio_bridge_stats(std::uint64_t* underruns,
                                    std::uint64_t* overflows) const {
    if (underruns) *underruns = 0;
    if (overflows) *overflows = 0;
    if (!open_ || !impl_) return;
    auto* b = static_cast<Backend*>(impl_);
    if (b->audio_dev == 0 || !b->bridge_ready) return;
    rab_stats stats{};
    if (b->audio_mtx) SDL_LockMutex(b->audio_mtx);
    rab_get_stats(&b->bridge, &stats);
    if (b->audio_mtx) SDL_UnlockMutex(b->audio_mtx);
    if (underruns) *underruns = stats.underrun_events;
    if (overflows) *overflows = stats.overflow_drops;
}

void HostWindow::set_turbo_audio_muted(bool on) {
    if (!open_ || !impl_) return;
    auto* b = static_cast<Backend*>(impl_);
    if (b->turbo_audio_muted == on) return;
    b->turbo_audio_muted = on;
    reset_audio();
}

void HostWindow::present_native(const uint8_t* rgb888, int width, int height) {
    if (!open_ || !impl_ || !rgb888 || width <= 0 || height <= 0) return;
    auto* b = static_cast<Backend*>(impl_);
    if (!b->native_renderer || width != b->base_w * b->native_scale ||
        height != b->base_h * b->native_scale) {
        return;
    }
    // Establish the texture before copying the supplied scene. The texture
    // creation path clears/reallocates native_fb; doing this after the copy
    // would blank the first native frame.
    if (!ensure_native_texture(b)) return;
    const std::size_t bytes = static_cast<std::size_t>(width) * height * 3u;
    b->native_fb.assign(rgb888, rgb888 + bytes);
    b->native_scene_w = width;
    b->native_scene_h = height;
    b->native_scene_pending = true;
    present(nullptr);
    b->native_scene_pending = false;
}

void HostWindow::set_native_renderer_enabled(bool on) {
    if (!open_ || !impl_) return;
    auto* b = static_cast<Backend*>(impl_);
    b->native_renderer = on && b->native_renderer_game_allowed;
    b->cfg.native_renderer = b->native_renderer;
    set_native_window_mode(b, b->native_renderer);
    if (!b->native_renderer) destroy_native_texture(b);
}

void HostWindow::set_native_renderer_scale(int scale) {
    if (!open_ || !impl_) return;
    auto* b = static_cast<Backend*>(impl_);
    b->native_scale = std::clamp(scale, 1, 10);
    b->cfg.native_scale = b->native_scale;
    // Texture dimensions are tied to the selected render resolution.
    destroy_native_texture(b);
}

bool HostWindow::native_renderer_enabled() const {
    if (!open_ || !impl_) return false;
    return static_cast<const Backend*>(impl_)->native_renderer;
}

int HostWindow::native_renderer_scale() const {
    if (!open_ || !impl_) return 1;
    return static_cast<const Backend*>(impl_)->native_scale;
}

void HostWindow::set_fixed_view_modes(bool on, const FixedViewMode* modes,
                                      int count, int active_mode) {
    if (!open_ || !impl_) return;
    auto* b = static_cast<Backend*>(impl_);
    b->fixed_view_modes.clear();
    b->fixed_view_mode_labels.clear();
    if (modes && count > 0) {
        count = std::clamp(count, 0, 16);
        b->fixed_view_modes.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            FixedViewMode mode = modes[i];
            if (!mode.label || !*mode.label) mode.label = "View";
            if (mode.width < 1) mode.width = 240;
            if (mode.height < 1) mode.height = 160;
            b->fixed_view_modes.push_back(mode);
        }
    }
    for (const FixedViewMode& mode : b->fixed_view_modes)
        b->fixed_view_mode_labels.push_back(mode.label);

    // Native is always the runtime fallback. Expose the selector only when
    // the game supplied at least one actual expanded mode and the current run
    // is not strict/capture. The caller's `on` already includes capture policy.
    b->fixed_view_modes_allowed = on && !b->strict_static &&
                                  b->fixed_view_modes.size() > 1;
    b->widescreen_game_allowed = b->fixed_view_modes_allowed;
    b->cfg.fixed_view_modes_available = b->fixed_view_modes_allowed;
    b->cfg.fixed_view_mode_count = b->fixed_view_modes_allowed
        ? static_cast<int>(b->fixed_view_modes.size()) : 0;
    b->cfg.fixed_view_mode_labels =
        b->fixed_view_modes_allowed ? b->fixed_view_mode_labels.data() : nullptr;
    b->cfg.widescreen_available = b->fixed_view_modes_allowed;

    int selected = 0;
    if (b->fixed_view_modes_allowed) {
        // New ViewMode is authoritative. Existing Widescreen=true/false files
        // migrate to Native/Widescreen once, while a launcher seed remains the
        // default for files with neither key.
        if (b->view_mode_config_present) {
            selected = b->fixed_view_mode;
        } else if (b->widescreen_config_present) {
            selected = b->cfg.widescreen ? 1 : 0;
        } else {
            selected = active_mode;
        }
        selected = std::clamp(selected, 0,
                              static_cast<int>(b->fixed_view_modes.size()) - 1);
    }
    b->fixed_view_mode = selected;
    b->widescreen = selected != 0;
    b->cfg.fixed_view_mode = selected;
    b->cfg.widescreen = b->widescreen;

    if (b->fixed_view_modes_allowed && b->widescreen_config_present &&
        !b->view_mode_config_present) {
        if (write_enhancements_ini(
                b->config_dir, b->cfg.enhanced_timing, b->overclock_factor,
                b->temporal_blend_index, selected)) {
            b->view_mode_config_present = true;
        }
    }
}

int HostWindow::fixed_view_mode() const {
    if (!open_ || !impl_) return 0;
    return static_cast<const Backend*>(impl_)->fixed_view_mode;
}

void HostWindow::set_widescreen_available(bool on, bool enabled) {
    static const FixedViewMode kLegacyModes[] = {
        {"Native 240x160", 240, 160},
        {"Widescreen 288x160", 288, 160},
    };
    set_fixed_view_modes(on, kLegacyModes, 2, enabled ? 1 : 0);
}

bool HostWindow::widescreen_enabled() const {
    return fixed_view_mode() != 0;
}

void HostWindow::set_guest_frame(std::uint64_t frame) {
    if (!open_ || !impl_) return;
    auto* b = static_cast<Backend*>(impl_);
    b->cfg.guest_frame = frame;

    const std::uint64_t now_ns = performance_counter_ns();
    if (now_ns == 0) return;
    b->cfg.emulation_speed_percent = b->emulation_speed.observe(
        frame, now_ns, emulation_normal_hz(b));
}

void HostWindow::set_debug_overlay_audio_state(
    bool native_requested, bool native_live, bool native_output_selected,
    const char* native_reason,
    bool turbo_requested,
    bool turbo_decoupled, bool turbo_pending, bool turbo_fallback,
    bool turbo_muted, bool turbo_uncapped, const char* reason) {
    if (!open_ || !impl_) return;
    auto* b = static_cast<Backend*>(impl_);
    DebugOverlayState& state = b->cfg.debug_overlay_state;
    state.audio_status_available = true;
    state.native_mp2k_requested = !b->strict_static && native_requested;
    state.native_mp2k_live = !b->strict_static && native_live;
    state.native_output_selected =
        !b->strict_static && native_output_selected;
    std::snprintf(state.native_mp2k_reason,
                  sizeof(state.native_mp2k_reason), "%s",
                  native_reason ? native_reason : "");
    state.turbo_audio_requested = !b->strict_static && turbo_requested;
    state.turbo_audio_decoupled = !b->strict_static && turbo_decoupled;
    state.turbo_audio_pending = !b->strict_static && turbo_pending;
    state.turbo_audio_fallback = !b->strict_static && turbo_fallback;
    state.turbo_audio_muted = turbo_muted;
    state.turbo_audio_uncapped = turbo_uncapped;
    std::snprintf(state.turbo_audio_reason,
                  sizeof(state.turbo_audio_reason), "%s",
                  reason ? reason : "");
}

namespace {

// Write keybinds.ini [player1] in the recomp-ui generic format, so a file the
// menu writes is the same file the launcher's rebind page reads. Unbound is
// written as "None", matching what scancode_from_name() treats as unbound.
bool write_keybinds_ini(const std::string& dir, const SDL_Scancode* bind_sc,
                        const int* pad_bind) {
    const std::string path = dir + "/keybinds.ini";
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f,
        "; Written by the in-game config menu (default F1).\n"
        "; Generic recomp-ui keybind format: SDL scancode names.\n"
        "[player1]\n");
    for (const auto& bk : kBindKeys) {
        SDL_Scancode sc = bind_sc[bk.bit];
        const char* name = (sc == SDL_SCANCODE_UNKNOWN)
            ? "None" : SDL_GetScancodeName(sc);
        if (!name || !*name) name = "None";
        std::fprintf(f, "%s=%s\n", bk.name, name);
    }
    // Controller half. A separate section rather than new keys in [player1],
    // so a launcher that only knows the keyboard format still round-trips the
    // file without losing or misreading the pad layout.
    std::fprintf(f, "\n[player1_pad]\n");
    for (const auto& pk : kPadKeys) {
        const int pb = pad_bind[pk.bit];
        const char* name = (pb < 0) ? nullptr
            : pad_synth_name(pb);
        if (pb >= 0 && !name)
            name = SDL_GameControllerGetStringForButton(
                static_cast<SDL_GameControllerButton>(pb));
        std::fprintf(f, "%s=%s\n", pk.name, (name && *name) ? name : "None");
    }
    std::fclose(f);
    return true;
}

// Replace the keys of one named INI section in place, appending any of
// `names`/`values` not already present at the end of that section (creating
// the section if it was absent). Shared by write_hotkeys_ini's two passes
// (the [KeyMap] keyboard half and the UI-02 [KeyMap.Pad] controller half) so
// both round-trip through the same read-modify-write shape as every other
// section writer in this file.
void rewrite_ini_section(std::vector<std::string>& lines, const char* section,
                         const char* const* names, const std::string* values,
                         int count) {
    const std::size_t section_len = std::strlen(section);
    std::vector<bool> written(static_cast<std::size_t>(count), false);
    bool in_section = false;
    bool seen_section = false;
    std::size_t section_end = 0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::string t = lines[i];
        std::size_t a = t.find_first_not_of(" \t");
        if (a == std::string::npos) continue;
        if (t[a] == '[') {
            in_section = t.size() > a + 1 + section_len &&
                        t[a + 1 + section_len] == ']' &&
                        SDL_strncasecmp(t.c_str() + a + 1, section,
                                       section_len) == 0;
            if (in_section) { seen_section = true; section_end = i + 1; }
            continue;
        }
        if (!in_section || t[a] == ';' || t[a] == '#') continue;
        section_end = i + 1;
        const std::size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = t.substr(a, eq - a);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
            key.pop_back();
        for (int h = 0; h < count; ++h) {
            if (SDL_strcasecmp(key.c_str(), names[h]) != 0) continue;
            lines[i] = std::string(names[h]) + "=" + values[h];
            written[static_cast<std::size_t>(h)] = true;
            break;
        }
    }

    std::vector<std::string> add;
    if (!seen_section) {
        add.push_back("");
        add.push_back(std::string("[") + section + "]");
    }
    for (int h = 0; h < count; ++h) {
        if (written[static_cast<std::size_t>(h)]) continue;
        add.push_back(std::string(names[h]) + "=" + values[h]);
    }
    if (!add.empty()) {
        const std::size_t at = seen_section ? section_end : lines.size();
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(at),
                     add.begin(), add.end());
    }
}

// Write the hotkey bindings back into config.ini, preserving every other
// line of the file. config.ini is the launcher's, not ours: rewriting it
// wholesale would silently drop settings this build knows nothing about, so
// this is a read-modify-write that only touches keys it owns. Two sections:
// [KeyMap] (keyboard, unchanged since before UI-02) and [KeyMap.Pad]
// (controller, UI-02 â€” mirrors keybinds.ini's [player1]/[player1_pad] split).
bool write_hotkeys_ini(const std::string& dir, const HotkeyBind* hotkeys) {
    const std::string path = dir + "/config.ini";

    auto render_key = [](const HotkeyBind& hb) -> std::string {
        if (hb.key == SDLK_UNKNOWN) return "None";
        std::string pre;
        if (hb.mods & KMOD_CTRL)  pre += "Ctrl+";
        if (hb.mods & KMOD_ALT)   pre += "Alt+";
        if (hb.mods & KMOD_SHIFT) pre += "Shift+";
        const char* kn = SDL_GetKeyName(hb.key);
        return pre + ((kn && *kn) ? kn : "None");
    };
    auto render_pad = [](const HotkeyBind& hb) -> std::string {
        if (hb.pad_button < 0) return "None";
        // UI-02b: L2/R2 and the stick directions are synthetic ids (see
        // host_config_ui.h), not real SDL_GameControllerButton values —
        // persist them under the shared names so the round-trip through the
        // [KeyMap.Pad] reader above is exact.
        if (const char* s = pad_synth_name(hb.pad_button)) return s;
        const char* n = SDL_GameControllerGetStringForButton(
            static_cast<SDL_GameControllerButton>(hb.pad_button));
        return (n && *n) ? n : "None";
    };

    // Read the existing file (if any) into lines.
    std::vector<std::string> lines;
    if (std::FILE* in = std::fopen(path.c_str(), "rb")) {
        std::string cur;
        int c;
        while ((c = std::fgetc(in)) != EOF) {
            if (c == '\n') { lines.push_back(cur); cur.clear(); }
            else if (c != '\r') cur.push_back(static_cast<char>(c));
        }
        if (!cur.empty()) lines.push_back(cur);
        std::fclose(in);
    }

    std::vector<std::string> key_values(HK_COUNT);
    std::vector<std::string> pad_values(HK_COUNT);
    for (int h = 0; h < HK_COUNT; ++h) {
        key_values[static_cast<std::size_t>(h)] = render_key(hotkeys[h]);
        pad_values[static_cast<std::size_t>(h)] = render_pad(hotkeys[h]);
    }
    rewrite_ini_section(lines, "KeyMap", kHotkeyNames, key_values.data(),
                        HK_COUNT);
    rewrite_ini_section(lines, "KeyMap.Pad", kHotkeyNames, pad_values.data(),
                        HK_COUNT);

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    for (const std::string& l : lines) std::fprintf(f, "%s\n", l.c_str());
    std::fclose(f);
    return true;
}

// Update only [Speed], preserving launcher settings and unknown sections.
bool write_speed_ini(const std::string& dir, float multiplier,
                     bool mute_during_turbo, bool uncapped) {
    const std::string path = dir + "/config.ini";
    std::vector<std::string> lines;
    if (std::FILE* in = std::fopen(path.c_str(), "rb")) {
        std::string cur;
        int c;
        while ((c = std::fgetc(in)) != EOF) {
            if (c == '\n') { lines.push_back(cur); cur.clear(); }
            else if (c != '\r') cur.push_back(static_cast<char>(c));
        }
        if (!cur.empty()) lines.push_back(cur);
        std::fclose(in);
    }

    const char* const names[] = {
        "TurboMultiplier", "MuteDuringTurbo", "Uncapped"
    };
    char multiplier_text[32];
    std::snprintf(multiplier_text, sizeof(multiplier_text), "%.1f",
                  std::clamp(multiplier, 1.0f, kMaxTurboMultiplier));
    const std::string values[] = {
        multiplier_text, mute_during_turbo ? "true" : "false",
        uncapped ? "true" : "false"
    };
    bool written[3] = {};
    bool in_speed = false;
    bool seen_speed = false;
    std::size_t speed_end = 0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::string t = lines[i];
        const std::size_t a = t.find_first_not_of(" \t");
        if (a == std::string::npos) continue;
        if (t[a] == '[') {
            in_speed = SDL_strncasecmp(t.c_str() + a, "[Speed]", 7) == 0;
            if (in_speed) { seen_speed = true; speed_end = i + 1; }
            continue;
        }
        if (!in_speed || t[a] == ';' || t[a] == '#') continue;
        speed_end = i + 1;
        const std::size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = t.substr(a, eq - a);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
            key.pop_back();
        for (int k = 0; k < 3; ++k) {
            if (SDL_strcasecmp(key.c_str(), names[k]) != 0) continue;
            lines[i] = std::string(names[k]) + "=" + values[k];
            written[k] = true;
            break;
        }
    }

    std::vector<std::string> add;
    if (!seen_speed) { add.push_back(""); add.push_back("[Speed]"); }
    for (int k = 0; k < 3; ++k) {
        if (!written[k]) add.push_back(std::string(names[k]) + "=" + values[k]);
    }
    if (!add.empty()) {
        const std::size_t at = seen_speed ? speed_end : lines.size();
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(at),
                     add.begin(), add.end());
    }

    std::FILE* out = std::fopen(path.c_str(), "wb");
    if (!out) return false;
    for (const std::string& line : lines)
        std::fprintf(out, "%s\n", line.c_str());
    std::fclose(out);
    return true;
}

// Update only [Enhancements], preserving launcher settings and unknown
// sections. No section is created until the user changes a control.
bool write_enhancements_ini(const std::string& dir, bool enhanced_timing,
                            unsigned overclock_factor,
                            int temporal_blend_index,
                            int view_mode) {
    const std::string path = dir + "/config.ini";
    std::vector<std::string> lines;
    if (std::FILE* in = std::fopen(path.c_str(), "rb")) {
        std::string cur;
        int c;
        while ((c = std::fgetc(in)) != EOF) {
            if (c == '\n') { lines.push_back(cur); cur.clear(); }
            else if (c != '\r') cur.push_back(static_cast<char>(c));
        }
        if (!cur.empty()) lines.push_back(cur);
        std::fclose(in);
    }

    bool in_section = false;
    bool seen_section = false;
    bool written = false;
    const std::string overclock_value = std::to_string(overclock_factor);
    bool written_overclock = false;
    bool written_temporal_blend = false;
    bool written_view_mode = false;
    bool written_widescreen = false;
    const int persisted_view_mode = std::max(0, view_mode);
    const bool persisted_widescreen = persisted_view_mode != 0;
    std::size_t section_end = 0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::string t = lines[i];
        const std::size_t a = t.find_first_not_of(" \t");
        if (a == std::string::npos) continue;
        if (t[a] == '[') {
            in_section =
                SDL_strncasecmp(t.c_str() + a, "[Enhancements]", 14) == 0;
            if (in_section) { seen_section = true; section_end = i + 1; }
            continue;
        }
        if (!in_section || t[a] == ';' || t[a] == '#') continue;
        section_end = i + 1;
        const std::size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = t.substr(a, eq - a);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
            key.pop_back();
        if (SDL_strcasecmp(key.c_str(), "EnhancedTiming") == 0) {
            lines[i] = std::string("EnhancedTiming=") +
                       (enhanced_timing ? "true" : "false");
            written = true;
        } else if (SDL_strcasecmp(key.c_str(), "CpuOverclock") == 0) {
            lines[i] = "CpuOverclock=" + overclock_value;
            written_overclock = true;
        } else if (SDL_strcasecmp(key.c_str(), "TemporalBlend") == 0) {
            lines[i] = "TemporalBlend=" + std::to_string(temporal_blend_index);
            written_temporal_blend = true;
        } else if (SDL_strcasecmp(key.c_str(), "ViewMode") == 0) {
            lines[i] = "ViewMode=" + std::to_string(persisted_view_mode);
            written_view_mode = true;
        } else if (SDL_strcasecmp(key.c_str(), "Widescreen") == 0) {
            lines[i] = std::string("Widescreen=") +
                       (persisted_widescreen ? "true" : "false");
            written_widescreen = true;
        }
    }

    std::vector<std::string> add;
    if (!seen_section) { add.push_back(""); add.push_back("[Enhancements]"); }
    if (!written)
        add.push_back(std::string("EnhancedTiming=") +
                      (enhanced_timing ? "true" : "false"));
    if (!written_overclock)
        add.push_back("CpuOverclock=" + overclock_value);
    if (!written_temporal_blend)
        add.push_back("TemporalBlend=" + std::to_string(temporal_blend_index));
    if (!written_view_mode)
        add.push_back("ViewMode=" + std::to_string(persisted_view_mode));
    if (!written_widescreen)
        add.push_back(std::string("Widescreen=") +
                      (persisted_widescreen ? "true" : "false"));
    if (!add.empty()) {
        const std::size_t at = seen_section ? section_end : lines.size();
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(at),
                     add.begin(), add.end());
    }

    std::FILE* out = std::fopen(path.c_str(), "wb");
    if (!out) return false;
    for (const std::string& line : lines)
        std::fprintf(out, "%s\n", line.c_str());
    std::fclose(out);
    return true;
}

// Update only [Enhancements] ColorProfile=, ColorSaturation=, ColorHue=,
// ColorBrightness=, ColorWarmth=, ColorDarken= and AspectRatio=, preserving
// everything else.
bool write_picture_ini(const std::string& dir, int screen_kind,
                       const CustomColors& custom, int aspect) {
    if (screen_kind < 0 || screen_kind >= runtime::kScreenKindCount)
        return false;
    const std::string path = dir + "/config.ini";
    const std::string keys[7] = {"ColorProfile", "ColorSaturation", "ColorHue",
                                 "ColorBrightness", "ColorWarmth",
                                 "ColorDarken", "AspectRatio"};
    const std::string rows[7] = {
        keys[0] + "=" +
            runtime::screen_kind_name(static_cast<runtime::ScreenKind>(screen_kind)),
        keys[1] + "=" + std::to_string(custom.saturation),
        keys[2] + "=" + std::to_string(custom.hue),
        keys[3] + "=" + std::to_string(custom.brightness),
        keys[4] + "=" + std::to_string(custom.warmth),
        keys[5] + "=" + std::to_string(custom.darken),
        keys[6] + "=" + (aspect == 1 ? "4:3" : "3:2")};
    std::vector<std::string> lines;
    if (std::FILE* in = std::fopen(path.c_str(), "rb")) {
        std::string cur;
        int c;
        while ((c = std::fgetc(in)) != EOF) {
            if (c == '\n') { lines.push_back(cur); cur.clear(); }
            else if (c != '\r') cur.push_back(static_cast<char>(c));
        }
        if (!cur.empty()) lines.push_back(cur);
        std::fclose(in);
    }
    bool in_section = false;
    bool seen_section = false;
    bool written[7] = {false, false, false, false, false, false, false};
    std::size_t section_end = 0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string& t = lines[i];
        const std::size_t a = t.find_first_not_of(" \t");
        if (a == std::string::npos) continue;
        if (t[a] == '[') {
            in_section =
                SDL_strncasecmp(t.c_str() + a, "[Enhancements]", 14) == 0;
            if (in_section) { seen_section = true; section_end = i + 1; }
            continue;
        }
        if (!in_section || t[a] == ';' || t[a] == '#') continue;
        section_end = i + 1;
        const std::size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = t.substr(a, eq - a);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
            key.pop_back();
        for (int k = 0; k < 7; ++k) {
            if (SDL_strcasecmp(key.c_str(), keys[k].c_str()) == 0) {
                lines[i] = rows[k];
                written[k] = true;
            }
        }
    }
    if (!seen_section) {
        lines.push_back("");
        lines.push_back("[Enhancements]");
        section_end = lines.size();
    }
    for (int k = 0; k < 7; ++k) {
        if (written[k]) continue;
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(section_end),
                     rows[k]);
        ++section_end;
    }
    std::FILE* out = std::fopen(path.c_str(), "wb");
    if (!out) return false;
    for (const std::string& line : lines)
        std::fprintf(out, "%s\n", line.c_str());
    std::fclose(out);
    return true;
}

// Update only [Cheats], preserving launcher settings and unknown sections.
bool write_cheats_ini(const std::string& dir, bool infinite_hp,
                      bool infinite_pp, int player_speed_multiplier) {
    const std::string path = dir + "/config.ini";
    std::vector<std::string> lines;
    if (std::FILE* in = std::fopen(path.c_str(), "rb")) {
        std::string cur;
        int c;
        while ((c = std::fgetc(in)) != EOF) {
            if (c == '\n') { lines.push_back(cur); cur.clear(); }
            else if (c != '\r') cur.push_back(static_cast<char>(c));
        }
        if (!cur.empty()) lines.push_back(cur);
        std::fclose(in);
    }

    bool in_section = false;
    bool seen_section = false;
    bool wrote_hp = false;
    bool wrote_pp = false;
    bool wrote_player_speed = false;
    std::size_t section_end = 0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::string t = lines[i];
        const std::size_t a = t.find_first_not_of(" \t");
        if (a == std::string::npos) continue;
        if (t[a] == '[') {
            in_section = SDL_strncasecmp(t.c_str() + a, "[Cheats]", 8) == 0;
            if (in_section) { seen_section = true; section_end = i + 1; }
            continue;
        }
        if (!in_section || t[a] == ';' || t[a] == '#') continue;
        section_end = i + 1;
        const std::size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = t.substr(a, eq - a);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
            key.pop_back();
        if (SDL_strcasecmp(key.c_str(), "InfiniteHP") == 0) {
            lines[i] = std::string("InfiniteHP=") +
                       (infinite_hp ? "true" : "false");
            wrote_hp = true;
        } else if (SDL_strcasecmp(key.c_str(), "InfinitePP") == 0) {
            lines[i] = std::string("InfinitePP=") +
                       (infinite_pp ? "true" : "false");
            wrote_pp = true;
        } else if (SDL_strcasecmp(
                       key.c_str(), "PlayerWalkRunSpeedMultiplier") == 0 ||
                   SDL_strcasecmp(key.c_str(), "PlayerWalkRun2x") == 0) {
            if (!wrote_player_speed) {
                lines[i] = std::string("PlayerWalkRunSpeedMultiplier=") +
                    player_walk_run_speed_persisted_value(
                        player_speed_multiplier);
                wrote_player_speed = true;
            } else {
                lines[i].clear();
            }
        }
    }

    std::vector<std::string> add;
    if (!seen_section) { add.push_back(""); add.push_back("[Cheats]"); }
    if (!wrote_hp)
        add.push_back(std::string("InfiniteHP=") +
                      (infinite_hp ? "true" : "false"));
    if (!wrote_pp)
        add.push_back(std::string("InfinitePP=") +
                      (infinite_pp ? "true" : "false"));
    if (!wrote_player_speed)
        add.push_back(std::string("PlayerWalkRunSpeedMultiplier=") +
                      player_walk_run_speed_persisted_value(
                          player_speed_multiplier));
    if (!add.empty()) {
        const std::size_t at = seen_section ? section_end : lines.size();
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(at),
                     add.begin(), add.end());
    }

    std::FILE* out = std::fopen(path.c_str(), "wb");
    if (!out) return false;
    for (const std::string& line : lines)
        std::fprintf(out, "%s\n", line.c_str());
    std::fclose(out);
    return true;
}

// Update only [Audio], preserving launcher settings and unknown sections.
bool write_audio_ini(const std::string& dir, int volume, bool mute) {
    const std::string path = dir + "/config.ini";
    std::vector<std::string> lines;
    if (std::FILE* in = std::fopen(path.c_str(), "rb")) {
        std::string cur;
        int c;
        while ((c = std::fgetc(in)) != EOF) {
            if (c == '\n') { lines.push_back(cur); cur.clear(); }
            else if (c != '\r') cur.push_back(static_cast<char>(c));
        }
        if (!cur.empty()) lines.push_back(cur);
        std::fclose(in);
    }

    static const char* const kNames[] = {"Volume", "Mute"};
    const std::string values[] = {std::to_string(volume),
                                  mute ? "true" : "false"};
    rewrite_ini_section(lines, "Audio", kNames, values, 2);

    std::FILE* out = std::fopen(path.c_str(), "wb");
    if (!out) return false;
    for (const std::string& line : lines)
        std::fprintf(out, "%s\n", line.c_str());
    std::fclose(out);
    return true;
}

// Write [Audio] when it differs from what config.ini last recorded.
void persist_audio(Backend* b) {
    if (b->cfg.volume == b->persisted_volume && b->cfg.mute == b->persisted_mute)
        return;
    if (!write_audio_ini(b->config_dir, b->cfg.volume, b->cfg.mute)) {
        std::fprintf(stderr, "host_window: could not write %s/config.ini\n",
                     b->config_dir.c_str());
        return;
    }
    b->persisted_volume = b->cfg.volume;
    b->persisted_mute = b->cfg.mute;
}

// Update only [Logging], preserving launcher settings and unknown sections.
// Own section (not folded into [Enhancements]) — see load_input_config's
// [Logging] read above and runtime_arm.h gsr_set_additional_debug_logging.
bool write_logging_ini(const std::string& dir, bool additional_debug_logging,
                       bool crash_log) {
    const std::string path = dir + "/config.ini";
    std::vector<std::string> lines;
    if (std::FILE* in = std::fopen(path.c_str(), "rb")) {
        std::string cur;
        int c;
        while ((c = std::fgetc(in)) != EOF) {
            if (c == '\n') { lines.push_back(cur); cur.clear(); }
            else if (c != '\r') cur.push_back(static_cast<char>(c));
        }
        if (!cur.empty()) lines.push_back(cur);
        std::fclose(in);
    }

    static const char* const kNames[] = {"AdditionalDebugLogging",
                                         "CrashLog"};
    const std::string values[] = {additional_debug_logging ? "true" : "false",
                                  crash_log ? "true" : "false"};
    rewrite_ini_section(lines, "Logging", kNames, values, 2);

    std::FILE* out = std::fopen(path.c_str(), "wb");
    if (!out) return false;
    for (const std::string& line : lines)
        std::fprintf(out, "%s\n", line.c_str());
    std::fclose(out);
    return true;
}

// Update only [Debug], preserving launcher settings and unknown sections.
bool write_debug_overlay_ini(const std::string& dir, bool debug_overlay) {
    const std::string path = dir + "/config.ini";
    std::vector<std::string> lines;
    if (std::FILE* in = std::fopen(path.c_str(), "rb")) {
        std::string cur;
        int c;
        while ((c = std::fgetc(in)) != EOF) {
            if (c == '\n') { lines.push_back(cur); cur.clear(); }
            else if (c != '\r') cur.push_back(static_cast<char>(c));
        }
        if (!cur.empty()) lines.push_back(cur);
        std::fclose(in);
    }

    bool in_section = false;
    bool seen_section = false;
    bool written = false;
    std::size_t section_end = 0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string& t = lines[i];
        const std::size_t a = t.find_first_not_of(" \t");
        if (a == std::string::npos) continue;
        if (t[a] == '[') {
            in_section = SDL_strncasecmp(t.c_str() + a, "[Debug]", 7) == 0;
            if (in_section) { seen_section = true; section_end = i + 1; }
            continue;
        }
        if (!in_section || t[a] == ';' || t[a] == '#') continue;
        section_end = i + 1;
        const std::size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = t.substr(a, eq - a);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
            key.pop_back();
        if (SDL_strcasecmp(key.c_str(), "DebugOverlay") == 0) {
            lines[i] = std::string("DebugOverlay=") +
                       (debug_overlay ? "true" : "false");
            written = true;
        }
    }

    std::vector<std::string> add;
    if (!seen_section) { add.push_back(""); add.push_back("[Debug]"); }
    if (!written)
        add.push_back(std::string("DebugOverlay=") +
                      (debug_overlay ? "true" : "false"));
    if (!add.empty()) {
        const std::size_t at = seen_section ? section_end : lines.size();
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(at),
                     add.begin(), add.end());
    }

    std::FILE* out = std::fopen(path.c_str(), "wb");
    if (!out) return false;
    for (const std::string& line : lines)
        std::fprintf(out, "%s\n", line.c_str());
    std::fclose(out);
    return true;
}

}  // namespace

HostWindow::Events HostWindow::pump() {
    Events ev{};
    if (!open_) { ev.quit = true; return ev; }
    auto* b = static_cast<Backend*>(impl_);
    const bool pump_timing = host_pump_timing_enabled();
    const uint64_t pump_t0 = pump_timing ? host_pump_now_ns() : 0;

    // Harness switch: open the menu at startup. Lets a capture run verify the
    // UI renders without a human at the keyboard (and makes screenshots of it
    // reproducible). Same family as the other GBARECOMP_* switches.
    {
        static const bool autoopen = [] {
            const char* e = std::getenv("GBARECOMP_MENU_OPEN");
            return e && e[0] == '1';
        }();
        static bool applied = false;
        if (autoopen && !applied) { applied = true; config_ui_set_visible(true); }
    }

    // Publish current host state into the UI before it draws, so the widgets
    // show what is actually in effect rather than a stale copy.
    //
    // Each group is skipped while its change flag is pending. The UI runs at
    // present time — i.e. AFTER the apply block at the bottom of the previous
    // pump — so an unguarded publish here would overwrite the edit the user
    // just made with the old backend value and the change would vanish.
    b->cfg.hotkey_count = HK_COUNT;
    // hotkey_names is display-only (see ConfigUiState); kHotkeyLabels carries
    // the UI-02 "Turbo Held"/"Turbo Toggle" wording while config.ini keeps
    // persisting under the original kHotkeyNames keys.
    b->cfg.hotkey_names = kHotkeyLabels;
    if (!b->cfg.binds_changed) {
        for (int i = 0; i < 10; ++i) {
            b->cfg.key_bind[i] = static_cast<int>(b->bind_sc[i]);
            b->cfg.pad_bind[i] = b->pad_bind[i];
        }
        for (int h = 0; h < HK_COUNT; ++h) {
            b->cfg.hotkey_key[h] = static_cast<int>(b->hotkeys[h].key);
            b->cfg.hotkey_mods[h] = b->hotkeys[h].mods;
            b->cfg.hotkey_pad[h] = b->hotkeys[h].pad_button;
        }
    }
    // Largest window scale that still fits the display the window is on, so
    // the Window Size slider never offers a size the desktop cannot show.
    {
        int max_scale = 8;
        SDL_Rect usable{};
        const int display = SDL_GetWindowDisplayIndex(b->window);
        if (display >= 0 && b->base_w > 0 && b->base_h > 0 &&
            SDL_GetDisplayUsableBounds(display, &usable) == 0) {
            max_scale = std::clamp(std::min(usable.w /
                                                shown_width(b, b->base_w, b->base_h),
                                            usable.h / b->base_h), 1, 8);
        }
        b->cfg.max_scale = std::max(max_scale, b->scale);
    }
    if (!b->cfg.video_changed) {
        b->cfg.scale = b->scale;
        b->cfg.fullscreen = b->fullscreen;
        b->cfg.show_fps = b->fps_readout;
        b->cfg.vsync = b->vsync;
        b->cfg.linear_filter = b->linear_filter;
        b->cfg.integer_scale = b->integer_scale;
        b->cfg.screen_kind = static_cast<int>(b->screen_kind);
        b->cfg.aspect = b->aspect_4_3 ? 1 : 0;
        b->cfg.native_scale = b->native_scale;
        b->cfg.screen_filters_available = b->screen_filters_on;
        b->cfg.screen_filter = static_cast<int>(b->screen_filter);
    }
    if (!b->cfg.audio_changed && !b->cfg.mute) {
        b->cfg.volume = b->volume;
        // The Volume Up/Down hotkeys change it outside the menu: save that too.
        persist_audio(b);
    }
    if (!b->cfg.enhancements_changed) {
        b->cfg.fixed_view_modes_available = b->fixed_view_modes_allowed;
        b->cfg.fixed_view_mode = b->fixed_view_mode;
        b->cfg.fixed_view_mode_count = b->fixed_view_modes_allowed
            ? static_cast<int>(b->fixed_view_modes.size()) : 0;
        b->cfg.fixed_view_mode_labels = b->fixed_view_modes_allowed
            ? b->fixed_view_mode_labels.data() : nullptr;
        b->cfg.widescreen_available = b->widescreen_game_allowed;
        b->cfg.widescreen = b->widescreen;
    }
    if (!b->cfg.cheats_changed) {
        b->cfg.infinite_hp = runtime_get_infinite_hp() != 0;
        b->cfg.infinite_pp = runtime_get_infinite_pp() != 0;
        b->cfg.player_walk_run_speed_multiplier =
            runtime_get_mem_write_override_enabled();
        // Changed by game code rather than this menu: save it the same way.
        if ((b->cfg.player_walk_run_speed_multiplier !=
                 b->persisted_player_speed ||
             b->cfg.infinite_hp != b->persisted_infinite_hp ||
             b->cfg.infinite_pp != b->persisted_infinite_pp) &&
            write_cheats_ini(b->config_dir, b->cfg.infinite_hp,
                             b->cfg.infinite_pp,
                             b->cfg.player_walk_run_speed_multiplier)) {
            b->persisted_player_speed =
                b->cfg.player_walk_run_speed_multiplier;
            b->persisted_infinite_hp = b->cfg.infinite_hp;
            b->persisted_infinite_pp = b->cfg.infinite_pp;
        }
    }

    // Harness switch: run the save path once, with the bindings exactly as
    // just published, so the keybinds.ini + config.ini [KeyMap] round-trip can
    // be checked without a human clicking a bind box. A no-op edit by
    // construction — what it writes is what it just read. Deliberately AFTER
    // the publish above: setting the flag any earlier would suppress the
    // publish and save an empty layout.
    {
        static const bool save_once = [] {
            const char* e = std::getenv("GBARECOMP_MENU_SAVE_ONCE");
            return e && e[0] == '1';
        }();
        static bool done = false;
        if (save_once && !done) { done = true; b->cfg.binds_changed = true; }
    }

    const uint64_t pump_events_start = pump_timing ? host_pump_now_ns() : 0;
    uint64_t pump_poll_ns = 0;
    uint64_t pump_sdl_pump_ns = 0;
    uint64_t pump_sdl_peep_ns = 0;
    uint64_t pump_dispatch_ns = 0;
    uint64_t pump_event_max_ns = 0;
    uint32_t pump_event_count = 0;
    uint32_t pump_event_max_type = 0;
    uint32_t pump_event_max_subtype = 0;
    SDL_Event e;
    for (;;) {
        const uint64_t poll_start = pump_timing ? host_pump_now_ns() : 0;
        int has_event = 0;
        // Host profiler: SDL's own pump/dequeue, apart from our handling of
        // the events it returns.
        {
            HostProfPhaseScope _hp_sdl(kHpSdlEvents);
            if (pump_timing) {
                // SDL_PollEvent is SDL_PumpEvents followed by one
                // SDL_PeepEvents call. Keep that ordering and call frequency,
                // but expose the backend pump separately from queue dequeue for
                // rare long empty polls. The uninstrumented path below remains
                // the original SDL_PollEvent path.
                const uint64_t backend_start = host_pump_now_ns();
                SDL_PumpEvents();
                const uint64_t backend_end = host_pump_now_ns();
                const uint64_t peep_start = backend_end;
                has_event = SDL_PeepEvents(&e, 1, SDL_GETEVENT,
                                           SDL_FIRSTEVENT, SDL_LASTEVENT);
                const uint64_t peep_end = host_pump_now_ns();
                pump_sdl_pump_ns += backend_end - backend_start;
                pump_sdl_peep_ns += peep_end - peep_start;
            } else {
                has_event = SDL_PollEvent(&e);
            }
        }
        if (pump_timing)
            pump_poll_ns += host_pump_now_ns() - poll_start;
        if (!has_event) break;
        ++pump_event_count;
        const uint64_t event_start = pump_timing ? host_pump_now_ns() : 0;
        const auto finish_event = [&] {
            if (!pump_timing) return;
            const uint64_t elapsed = host_pump_now_ns() - event_start;
            pump_dispatch_ns += elapsed;
            if (elapsed > pump_event_max_ns) {
                pump_event_max_ns = elapsed;
                pump_event_max_type = static_cast<uint32_t>(e.type);
                pump_event_max_subtype =
                    e.type == SDL_WINDOWEVENT
                        ? static_cast<uint32_t>(e.window.event)
                        : 0u;
            }
        };
        // UI-02b: L2/R2 arrive as SDL_CONTROLLERAXISMOTION, never as a
        // button event, so turn a threshold crossing into a synthetic
        // "button" press/release right here, before any of the dispatch
        // below — everything downstream then treats a trigger exactly like
        // SDL_CONTROLLERBUTTONDOWN with pad_button == kPadTriggerLeft/Right.
        // trigger_left_down/right_down (debounced by the hysteresis pair
        // above) also double as the level-triggered read Turbo Held uses at
        // the end of pump().
        bool synth_pad_down = false;
        int  synth_pad_button = -1;
        if (e.type == SDL_CONTROLLERAXISMOTION &&
            (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ||
             e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)) {
            const bool is_left = (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT);
            bool& down = is_left ? b->trigger_left_down : b->trigger_right_down;
            if (!down && e.caxis.value >= kTriggerPressThreshold) {
                down = true;
                synth_pad_down = true;
                synth_pad_button = is_left ? kPadTriggerLeft : kPadTriggerRight;
            } else if (down && e.caxis.value <= kTriggerReleaseThreshold) {
                down = false;
            }
        } else if (e.type == SDL_CONTROLLERAXISMOTION &&
                   (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX ||
                    e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY ||
                    e.caxis.axis == SDL_CONTROLLER_AXIS_RIGHTX ||
                    e.caxis.axis == SDL_CONTROLLER_AXIS_RIGHTY)) {
            // Stick directions get the same treatment as the triggers. An
            // axis event concerns one axis, so at most one direction presses.
            // Slot order per stick: Right, Left, Up, Down (SDL Y is down-positive).
            const bool is_x = (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX ||
                               e.caxis.axis == SDL_CONTROLLER_AXIS_RIGHTX);
            const bool is_right_stick = (e.caxis.axis == SDL_CONTROLLER_AXIS_RIGHTX ||
                                         e.caxis.axis == SDL_CONTROLLER_AXIS_RIGHTY);
            const int base_slot = (is_right_stick ? 4 : 0) + (is_x ? 0 : 2);
            bool& pos_down = b->stick_down[base_slot];      // Right / Up slot
            bool& neg_down = b->stick_down[base_slot + 1];  // Left / Down slot
            // Slot meaning: X = {Right, Left}; Y = {Up, Down}. Y's positive
            // value is Down, so swap which flag the value's sign drives.
            bool& positive_flag = is_x ? pos_down : neg_down;
            bool& negative_flag = is_x ? neg_down : pos_down;
            const int positive_id = kPadLeftStickRight + base_slot + (is_x ? 0 : 1);
            const int negative_id = kPadLeftStickRight + base_slot + (is_x ? 1 : 0);
            const int v = e.caxis.value;
            if (positive_flag && v <= kTriggerReleaseThreshold) positive_flag = false;
            if (negative_flag && v >= -kTriggerReleaseThreshold) negative_flag = false;
            if (!positive_flag && v >= kTriggerPressThreshold) {
                positive_flag = true;
                synth_pad_down = true;
                synth_pad_button = positive_id;
            } else if (!negative_flag && v <= -kTriggerPressThreshold) {
                negative_flag = true;
                synth_pad_down = true;
                synth_pad_button = negative_id;
            }
        }

        // The config UI gets first refusal on every event. When a bind box is
        // armed it consumes the raw press; when merely visible it owns the
        // mouse and keyboard so typing in the UI never reaches the guest.
        if (b->cfg_ready) {
            if (e.type == SDL_KEYDOWN && e.key.repeat == 0 &&
                config_ui_capture_key(&b->cfg,
                                      static_cast<int>(e.key.keysym.scancode),
                                      static_cast<int>(e.key.keysym.sym),
                                      e.key.keysym.mod)) {
                finish_event();
                continue;
            }
            if (e.type == SDL_CONTROLLERBUTTONDOWN &&
                config_ui_capture_pad(&b->cfg, e.cbutton.button)) {
                finish_event();
                continue;
            }
            if (synth_pad_down &&
                config_ui_capture_pad(&b->cfg, synth_pad_button)) {
                finish_event();
                continue;
            }
            if (config_ui_handle_event(&e)) {
                // Still honour the menu key so it can close itself.
                if (e.type == SDL_KEYDOWN && e.key.repeat == 0 &&
                    b->hotkeys[HK_MENU].key != SDLK_UNKNOWN &&
                    e.key.keysym.sym == b->hotkeys[HK_MENU].key &&
                    hotkey_mods_ok(b->hotkeys[HK_MENU], e.key.keysym.mod)) {
                    config_ui_toggle();
                }
                finish_event();
                continue;
            }
        }
        if (e.type == SDL_CONTROLLERDEVICEADDED && !b->pad) {
            b->pad = SDL_GameControllerOpen(e.cdevice.which);
        } else if (e.type == SDL_CONTROLLERDEVICEREMOVED && b->pad &&
                   SDL_GameControllerFromInstanceID(e.cdevice.which) == b->pad) {
            SDL_GameControllerClose(b->pad);
            b->pad = nullptr;
        }
        if (e.type == SDL_QUIT) {
            ev.quit = true;
        } else if (e.type == SDL_WINDOWEVENT &&
                   e.window.event == SDL_WINDOWEVENT_CLOSE) {
            // Multi-viewport ImGui gives detached panels their own OS
            // windows, each capable of firing its own CLOSE event (e.g. Alt+
            // F4 on a torn-off panel, or its titlebar X). Only the CLOSE of
            // the main game window means "quit the emulator" — a detached
            // panel's close is ImGui's to handle (it deletes the viewport),
            // never ours.
            if (SDL_GetWindowID(b->window) == e.window.windowID)
                ev.quit = true;
        } else if (e.type == SDL_KEYDOWN && e.key.repeat == 0) {
            // Edge-triggered hotkeys (ignore key-repeat). F2..F10 are
            // save-state slots: plain = load, Shift = save. SDL's F1..F12
            // keycodes are contiguous, so slot = sym - F2 + 1. (Slots used to
            // start at F1; they moved up one so the rebind menu could have
            // F1, which is where a menu is expected. Nine slots either way.)
            SDL_Keycode sym = e.key.keysym.sym;
            Uint16 mods = e.key.keysym.mod;
            if (sym == SDLK_ESCAPE) {
                ev.quit = true;
            } else if (sym >= SDLK_F2 && sym <= SDLK_F10) {
                int slot = static_cast<int>(sym - SDLK_F2) + 1;
                if (mods & KMOD_SHIFT) ev.save_slot = slot;
                else                   ev.load_slot = slot;
            } else {
                // Rebindable system hotkeys (config.ini [KeyMap]), including
                // Menu — folded into fire_hotkey's switch (UI-02) rather than
                // special-cased here, since the loop below already matches it.
                for (int h = 0; h < HK_COUNT; ++h) {
                    const HotkeyBind& hb = b->hotkeys[h];
                    if (hb.key == SDLK_UNKNOWN || hb.key != sym ||
                        !hotkey_mods_ok(hb, mods))
                        continue;
                    fire_hotkey(h, b, ev);
                }
            }
        } else if (e.type == SDL_CONTROLLERBUTTONDOWN && !config_ui_visible()) {
            // Controller half of the same edge-triggered hotkeys (UI-02).
            // Gated by !config_ui_visible() to match the gameplay pad read
            // below, which does the same while the config menu is open.
            for (int h = 0; h < HK_COUNT; ++h) {
                const HotkeyBind& hb = b->hotkeys[h];
                if (hb.pad_button < 0 || hb.pad_button != e.cbutton.button)
                    continue;
                fire_hotkey(h, b, ev);
            }
        } else if (synth_pad_down && !config_ui_visible()) {
            // UI-02b: L2/R2 half of the same edge-triggered hotkeys, fired on
            // the debounced press edge computed above — so a held trigger
            // bound to Turbo Toggle latches once per pull, not once per
            // SDL_CONTROLLERAXISMOTION event (there are many per pull).
            for (int h = 0; h < HK_COUNT; ++h) {
                const HotkeyBind& hb = b->hotkeys[h];
                if (hb.pad_button != synth_pad_button) continue;
                fire_hotkey(h, b, ev);
            }
        }
        finish_event();
    }

    const uint64_t pump_input_start = pump_timing ? host_pump_now_ns() : 0;

    // Build the GBA KEYINPUT value from current keyboard state via the
    // rebindable table (keybinds.ini; defaults in kDefaultBinds).
    const Uint8* ks = SDL_GetKeyboardState(nullptr);
    uint16_t keys = 0x03FFu;  // all released
    // Also skip while game-owned extra content wants the keyboard (e.g. a
    // function-call tracer label box) -- see g_config_ui_extra_wants_keyboard
    // (host_config_ui.h). Symmetrical with the config_ui_visible() check.
    const bool extra_wants_keyboard =
        gbarecomp::g_config_ui_extra_wants_keyboard &&
        gbarecomp::g_config_ui_extra_wants_keyboard();
    if (!config_ui_visible() && !extra_wants_keyboard) {
        for (int bit = 0; bit < 10; ++bit) {
            SDL_Scancode sc = b->bind_sc[bit];
            if (sc != SDL_SCANCODE_UNKNOWN && ks[sc])
                keys &= static_cast<uint16_t>(~(1u << bit));
        }
        // Controller is additive with the keyboard: either source can press a
        // button, which is what every emulator does and what a second player
        // on the same pad would expect.
        // The sticks move nothing by default: the D-pad is the default
        // movement, and a stick direction works once it is bound on the
        // Controls page (Jimmy, 2026-10-05).
        if (b->pad) {
            for (int bit = 0; bit < 10; ++bit) {
                const int pb = b->pad_bind[bit];
                if (pb < 0) continue;
                if (pad_is_stick(pb)) {
                    if (stick_dir_held(b->pad, pb))
                        keys &= static_cast<uint16_t>(~(1u << bit));
                    continue;
                }
                if (pb >= SDL_CONTROLLER_BUTTON_MAX) continue;  // trigger id: not a gameplay bind
                if (SDL_GameControllerGetButton(
                        b->pad, static_cast<SDL_GameControllerButton>(pb)))
                    keys &= static_cast<uint16_t>(~(1u << bit));
            }
        }
        // Auto Fire: two frames pressed, two released, so the game sees a
        // fresh press every fourth frame. Keyed to the guest frame so the
        // rhythm does not change with the paused/idle pump rate.
        const bool fire_phase = ((b->cfg.guest_frame >> 1) & 1u) == 0;
        if (fire_phase && hotkey_held(b, b->hotkeys[HK_AUTOFIRE_A], ks))
            keys &= static_cast<uint16_t>(~(1u << 0));
        if (fire_phase && hotkey_held(b, b->hotkeys[HK_AUTOFIRE_B], ks))
            keys &= static_cast<uint16_t>(~(1u << 1));
    }
    ev.keyinput = keys;

    const uint64_t pump_apply_start = pump_timing ? host_pump_now_ns() : 0;

    // Apply UI edits. Done here (not inside the UI) so the UI stays a pure
    // view and every state change goes through the one path that also writes
    // the config files.
    if (b->cfg_ready) {
        if (b->cfg.binds_changed) {
            for (int i = 0; i < 10; ++i) {
                b->bind_sc[i] = static_cast<SDL_Scancode>(b->cfg.key_bind[i]);
                b->pad_bind[i] = b->cfg.pad_bind[i];
            }
            for (int h = 0; h < HK_COUNT; ++h) {
                b->hotkeys[h].key = static_cast<SDL_Keycode>(b->cfg.hotkey_key[h]);
                b->hotkeys[h].mods = static_cast<Uint16>(b->cfg.hotkey_mods[h]);
                b->hotkeys[h].pad_button = b->cfg.hotkey_pad[h];
            }
            if (!write_keybinds_ini(b->config_dir, b->bind_sc, b->pad_bind)) {
                std::fprintf(stderr,
                             "host_window: could not write %s/keybinds.ini\n",
                             b->config_dir.c_str());
            }
            if (!write_hotkeys_ini(b->config_dir, b->hotkeys)) {
                std::fprintf(stderr,
                             "host_window: could not write %s/config.ini\n",
                             b->config_dir.c_str());
            }
            b->cfg.binds_changed = false;
        }
        if (b->cfg.video_changed) {
            if (b->cfg.fullscreen != b->fullscreen)
                set_fullscreen(b->cfg.fullscreen);
            if (b->cfg.scale != b->scale)
                adjust_scale(b->cfg.scale - b->scale);
            if (b->cfg.show_fps != b->fps_readout) ev.toggle_fps = true;
            if (b->cfg.vsync != b->vsync) {
                // SDL_RenderSetVSync is 2.0.18+. On older SDL the renderer's
                // vsync flag is fixed at creation, so the toggle would lie —
                // say so instead of silently doing nothing.
#if SDL_VERSION_ATLEAST(2, 0, 18)
                if (SDL_RenderSetVSync(b->renderer, b->cfg.vsync ? 1 : 0) == 0) {
                    b->vsync = b->cfg.vsync;
                } else {
                    std::fprintf(stderr,
                                 "host_window: SDL_RenderSetVSync failed: %s\n",
                                 SDL_GetError());
                    b->cfg.vsync = b->vsync;
                }
#else
                std::fprintf(stderr, "host_window: built against SDL %d.%d.%d; "
                             "V-Sync cannot be changed after startup\n",
                             SDL_MAJOR_VERSION, SDL_MINOR_VERSION,
                             SDL_PATCHLEVEL);
                b->cfg.vsync = b->vsync;
#endif
            }
            if (b->cfg.linear_filter != b->linear_filter) {
                b->linear_filter = b->cfg.linear_filter;
                SDL_SetTextureScaleMode(b->texture,
                                        b->linear_filter ? SDL_ScaleModeLinear
                                                         : SDL_ScaleModeNearest);
                std::fprintf(stderr,
                    "[presentation-filter] %s frame=%llu refresh=%.6fHz; "
                    "presentation-only, whole image %s\n",
                    b->linear_filter ? "ON" : "OFF",
                    static_cast<unsigned long long>(b->cfg.guest_frame),
                    b->display_refresh_hz,
                    b->linear_filter ? "softened" : "crisp");
            }
            if (b->cfg.integer_scale != b->integer_scale) {
                b->integer_scale = b->cfg.integer_scale;
                // Only the fixed 240x160 logical-size path honours this; the
                // expanded/resize-driven views compute their own destination
                // rect in present() and are unaffected.
                SDL_RenderSetIntegerScale(
                    b->renderer, b->integer_scale ? SDL_TRUE : SDL_FALSE);
            }
            const bool kind_changed =
                b->cfg.screen_kind != static_cast<int>(b->screen_kind);
            const bool custom_changed =
                b->cfg.screen_kind == static_cast<int>(runtime::ScreenKind::Custom) &&
                custom_colors_of(b->cfg) != b->applied_custom;
            if (kind_changed || custom_changed) {
                b->cfg.screen_kind = std::clamp(
                    b->cfg.screen_kind, 0,
                    static_cast<int>(runtime::ScreenKind::Custom));
                b->screen_kind = static_cast<runtime::ScreenKind>(
                    b->cfg.screen_kind);
                rebuild_color_lut(b);
            }
            // A slider saves when let go, not on every step of a drag.
            const bool aspect_changed =
                (b->cfg.aspect == 1) != b->aspect_4_3;
            if (aspect_changed) {
                b->aspect_4_3 = b->cfg.aspect == 1;
                apply_aspect_window_size(b);
            }
            // Test-only, never saved.
            if (b->screen_filters_on)
                b->screen_filter = b->cfg.screen_filter == 1
                    ? ScreenFilter::Lcd3x
                    : b->cfg.screen_filter == 2 ? ScreenFilter::Xbr
                    : b->cfg.screen_filter == 3 ? ScreenFilter::CrtLottes
                    : b->cfg.screen_filter == 4 ? ScreenFilter::ScaleFx
                                                : ScreenFilter::Off;
            if (kind_changed || aspect_changed || b->cfg.color_save) {
                write_picture_ini(b->config_dir, b->cfg.screen_kind,
                                  custom_colors_of(b->cfg), b->cfg.aspect);
                b->cfg.color_save = false;
            }
            if (b->cfg.native_renderer != b->native_renderer)
                set_native_renderer_enabled(b->cfg.native_renderer);
            if (b->cfg.native_scale != b->native_scale)
                set_native_renderer_scale(b->cfg.native_scale);
            b->cfg.video_changed = false;
        }
        if (b->cfg.audio_changed) {
            b->volume = b->cfg.mute ? 0 : b->cfg.volume;
            b->cfg.audio_changed = false;
            persist_audio(b);
        }
        // Turbo shape is read every pump (no change flag): it only matters
        // while the Turbo binding is held, and the run loop wants the current
        // value, not an edge.
        b->turbo_multiplier = b->cfg.turbo_multiplier;
        b->turbo_uncapped = b->cfg.uncapped;
        b->turbo_mute_requested = b->cfg.mute_during_turbo;
        if (b->cfg.speed_changed) {
            if (!write_speed_ini(b->config_dir, b->turbo_multiplier,
                                 b->turbo_mute_requested,
                                 b->turbo_uncapped)) {
                std::fprintf(stderr,
                             "host_window: could not write %s/config.ini\n",
                             b->config_dir.c_str());
            }
            b->cfg.speed_changed = false;
        }
        // A view mode requested by game code goes through the same path as
        // a change made in this menu.
        if (const int requested = runtime_take_view_mode_request();
            requested >= 0 && b->fixed_view_modes_allowed &&
            requested != b->fixed_view_mode) {
            b->cfg.fixed_view_mode = requested;
            b->cfg.enhancements_changed = true;
        }
        if (const int requested = runtime_take_no_slowdown_request();
            requested >= 0) {
            // No Slowdown (in-game menu) owns only the CPU overclock. Exact
            // 60 Hz timing stays on regardless.
            b->cfg.overclock_index = requested != 0 ? 1 : 0;
            b->cfg.enhancements_changed = true;
        }
        if (b->cfg.enhancements_changed) {
            // CPU Overclock (TURBO-B2-UI): apply live immediately — the
            // factor is read with a relaxed atomic load from runtime_tick,
            // so this takes effect mid-run, including while a nested
            // wake-from-HALT runtime_dispatch is executing.
            const int idx = std::clamp(b->cfg.overclock_index, 0,
                                       gbarecomp::kOverclockItemCount - 1);
            b->overclock_factor = kOverclockFactors[idx];
            runtime_set_overclock_factor(b->overclock_factor);
            // VFX-FLICKER-02: applied live the same way — present() reads
            // b->temporal_blend_index directly every frame, no extra hook.
            b->temporal_blend_index = std::clamp(
                b->cfg.temporal_blend_index, 0,
                gbarecomp::kTemporalBlendItemCount - 1);
            if (b->fixed_view_modes_allowed && !b->fixed_view_modes.empty()) {
                b->fixed_view_mode = std::clamp(
                    b->cfg.fixed_view_mode, 0,
                    static_cast<int>(b->fixed_view_modes.size()) - 1);
            } else {
                b->fixed_view_mode = 0;
            }
            b->cfg.fixed_view_mode = b->fixed_view_mode;
            b->widescreen = b->fixed_view_mode != 0;
            b->cfg.widescreen = b->widescreen;
            ev.view_mode_changed = true;
            ev.view_mode = b->fixed_view_mode;
            ev.widescreen_changed = true;
            ev.widescreen = b->widescreen;
            if (!write_enhancements_ini(b->config_dir,
                                        b->cfg.enhanced_timing,
                                        b->overclock_factor,
                                        b->temporal_blend_index,
                                        b->fixed_view_mode)) {
                std::fprintf(stderr,
                             "host_window: could not write %s/config.ini\n",
                             b->config_dir.c_str());
            }
            // The write helper emits ViewMode plus the legacy Widescreen bool.
            // Remember that the preference now exists so a failed runtime
            // surface resize cannot mistake it for a launcher-only startup
            // seed and overwrite it during reconciliation.
            b->view_mode_config_present = true;
            b->widescreen_config_present = true;
            b->cfg.enhancements_changed = false;
        }
        runtime_publish_view_mode(b->fixed_view_mode);
        runtime_publish_no_slowdown(
            b->overclock_factor == kOverclockFactors[1]);
        if (b->cfg.layers_changed) {
            // Applied live immediately, like logging_changed below. Render-
            // only; never persisted (session-only, defaults unchecked).
            gba::g_hide_bg0 = b->cfg.hide_bg0;
            gba::g_hide_bg1 = b->cfg.hide_bg1;
            gba::g_hide_bg2 = b->cfg.hide_bg2;
            gba::g_hide_bg3 = b->cfg.hide_bg3;
            b->cfg.layers_changed = false;
        }
        if (b->cfg.cheats_changed) {
            runtime_set_infinite_hp(b->cfg.infinite_hp ? 1 : 0);
            runtime_set_infinite_pp(b->cfg.infinite_pp ? 1 : 0);
            runtime_set_mem_write_override_enabled(
                b->cfg.player_walk_run_speed_multiplier);
            if (!write_cheats_ini(b->config_dir, b->cfg.infinite_hp,
                                  b->cfg.infinite_pp,
                                  b->cfg.player_walk_run_speed_multiplier)) {
                std::fprintf(stderr,
                             "host_window: could not write %s/config.ini\n",
                             b->config_dir.c_str());
            } else {
                b->persisted_player_speed =
                    b->cfg.player_walk_run_speed_multiplier;
                b->persisted_infinite_hp = b->cfg.infinite_hp;
                b->persisted_infinite_pp = b->cfg.infinite_pp;
            }
            b->cfg.cheats_changed = false;
        }
        if (b->cfg.logging_changed) {
            // Applied live immediately — see gsr_set_additional_debug_logging
            // (relaxed atomic, safe from any point mid-run). Some Group-B
            // diagnostic streams only resolve their own env-var-vs-toggle
            // check once, at process/machine bring-up, so flipping this
            // mid-session does not retroactively start those specific
            // streams; the Logging tab's help text says so.
            gsr_set_additional_debug_logging(
                b->cfg.additional_debug_logging ? 1 : 0);
            b->cfg.debug_overlay_state.additional_debug_logging =
                b->cfg.additional_debug_logging;
            runtime_set_crash_log(b->cfg.crash_log ? 1 : 0);
            if (!write_logging_ini(b->config_dir,
                                   b->cfg.additional_debug_logging,
                                   b->cfg.crash_log)) {
                std::fprintf(stderr,
                             "host_window: could not write %s/config.ini\n",
                             b->config_dir.c_str());
            }
            b->cfg.logging_changed = false;
        }
        if (b->cfg.debug_overlay_changed) {
            if (!write_debug_overlay_ini(b->config_dir,
                                         b->cfg.debug_overlay)) {
                std::fprintf(stderr,
                             "host_window: could not write %s/config.ini\n",
                             b->config_dir.c_str());
            }
            b->cfg.debug_overlay_changed = false;
        }
        if (b->cfg.request_quit) { ev.quit = true; b->cfg.request_quit = false; }
    }

    const uint64_t pump_finalize_start = pump_timing ? host_pump_now_ns() : 0;

    // Turbo Held is level-triggered: down = speed up the frame limiter
    // (default Tab). The Speed tab decides whether that means a multiplier
    // or no cap at all; both travel with the flag so the run loop needs no
    // extra state. UI-02: Turbo Toggle is a separate, edge-triggered latch
    // (flipped by fire_hotkey's HK_TURBO_TOGGLE case above) that drives the
    // SAME ev.fast_forward flag rather than a second turbo path. Chosen
    // interaction rule: fast_forward is true when EITHER Held is currently
    // down OR the Toggle latch is on; releasing Held never clears the
    // Toggle latch (least-surprise reading of "press once -> on, press
    // again -> off" for Toggle, independent of what Held is doing).
    ev.turbo_multiplier = b->turbo_multiplier;
    ev.turbo_uncapped = b->turbo_uncapped;
    ev.turbo_mute_audio = b->turbo_mute_requested;
    ev.frame_interpolation_2x =
        b->cfg.frame_interpolation_available && b->cfg.frame_interpolation_2x;
    ev.enhanced_timing =
        b->cfg.enhanced_timing_available && b->cfg.enhanced_timing;
    const bool turbo_held = hotkey_held(b, b->hotkeys[HK_TURBO], ks);
    ev.fast_forward = turbo_held || b->turbo_toggle_on;
    if (pump_timing) {
        const uint64_t end = host_pump_now_ns();
        ev.pump_setup_us = host_pump_elapsed_us(pump_t0, pump_events_start);
        ev.pump_events_us = host_pump_elapsed_us(
            pump_events_start, pump_input_start);
        ev.pump_input_us = host_pump_elapsed_us(
            pump_input_start, pump_apply_start);
        ev.pump_apply_us = host_pump_elapsed_us(
            pump_apply_start, pump_finalize_start);
        ev.pump_finalize_us = host_pump_elapsed_us(
            pump_finalize_start, end);
        ev.pump_event_count = pump_event_count;
        ev.pump_poll_us = host_pump_elapsed_us(0, pump_poll_ns);
        ev.pump_sdl_pump_us = host_pump_elapsed_us(0, pump_sdl_pump_ns);
        ev.pump_sdl_peep_us = host_pump_elapsed_us(0, pump_sdl_peep_ns);
        ev.pump_dispatch_us = host_pump_elapsed_us(0, pump_dispatch_ns);
        ev.pump_event_max_us = host_pump_elapsed_us(0, pump_event_max_ns);
        ev.pump_event_max_type = pump_event_max_type;
        ev.pump_event_max_subtype = pump_event_max_subtype;
    }
    return ev;
}

}  // namespace gbarecomp

#else  // !GBARECOMP_HAVE_SDL2 â€” stub backend

namespace gbarecomp {

HostWindow::HostWindow()  = default;
HostWindow::~HostWindow() = default;

bool HostWindow::is_available() { return false; }

bool HostWindow::open(int /*scale*/, int /*base_w*/, int /*base_h*/,
                      const char* /*title*/, const char* /*screen*/,
                      bool /*linear_filter*/, bool /*resize_driven_view*/,
                      bool /*stereo_audio*/) {
    std::fprintf(stderr,
                 "host_window: built without SDL2; --window unavailable\n");
    return false;
}

void HostWindow::close() { open_ = false; }

bool HostWindow::set_surface_size(int /*base_w*/, int /*base_h*/) {
    return false;
}

bool HostWindow::drawable_size(int* /*width*/, int* /*height*/) const {
    return false;
}

void HostWindow::present(const uint8_t* /*rgb888*/) {}
void HostWindow::present_native(const uint8_t* /*rgb888*/, int /*width*/, int /*height*/) {}

void HostWindow::load_input_config(const char* /*dir*/) {}
void HostWindow::set_fullscreen(bool /*on*/) {}
bool HostWindow::fullscreen() const { return false; }
void HostWindow::adjust_scale(int /*delta*/) {}
void HostWindow::set_volume(int /*pct*/) {}
int  HostWindow::volume() const { return 100; }
void HostWindow::set_fps_readout(bool /*on*/) {}
bool HostWindow::fps_readout() const { return false; }
double HostWindow::display_refresh_hz() const { return 0.0; }
void HostWindow::set_frame_interpolation_available(bool) {}
void HostWindow::set_frame_interpolation_enabled(bool) {}
void HostWindow::set_enhanced_timing_available(bool) {}
void HostWindow::set_enhanced_timing_enabled(bool) {}
bool HostWindow::saved_enhanced_timing() const { return false; }
void HostWindow::set_native_renderer_available(bool) {}
void HostWindow::set_native_renderer_enabled(bool) {}
void HostWindow::set_native_renderer_scale(int) {}
bool HostWindow::native_renderer_enabled() const { return false; }
int HostWindow::native_renderer_scale() const { return 1; }
void HostWindow::set_fixed_view_modes(bool, const FixedViewMode*, int, int) {}
int HostWindow::fixed_view_mode() const { return 0; }
void HostWindow::set_widescreen_available(bool, bool) {}
bool HostWindow::widescreen_enabled() const { return false; }
void HostWindow::set_guest_frame(std::uint64_t) {}
void HostWindow::set_debug_overlay_audio_state(
    bool, bool, bool, const char*, bool, bool, bool, bool, bool, bool,
    const char*) {}

void HostWindow::push_audio_samples(const int16_t* /*samples*/,
                                    std::size_t /*count*/) {}
void HostWindow::push_audio_samples_stereo(const int16_t* /*samples*/,
                                            std::size_t /*count*/) {}
void HostWindow::set_turbo_audio_muted(bool /*on*/) {}
void HostWindow::reset_audio() {}
void HostWindow::audio_bridge_stats(std::uint64_t* underruns,
                                    std::uint64_t* overflows) const {
    if (underruns) *underruns = 0;
    if (overflows) *overflows = 0;
}

HostWindow::Events HostWindow::pump() {
    Events ev{};
    ev.quit = true;
    return ev;
}

}  // namespace gbarecomp

#endif  // GBARECOMP_HAVE_SDL2
