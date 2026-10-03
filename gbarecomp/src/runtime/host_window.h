// host_window.h — minimal host window + input surface.
//
// Soft-dependency on SDL2. When the build can find SDL2 the cpp
// uses it; otherwise the same symbols compile as no-op stubs so
// headless builds (CI, BIOS smoke without --window) still link.
//
// The window owns a logical-size streaming texture matching the active GBA
// framebuffer pixel format (RGB888). Expanded views opt into a resizable,
// aspect-correct viewport; the faithful 240x160 path retains the historical
// fixed SDL presentation. pump() drains the OS event queue, returns a quit flag
// and a packed GBA KEYINPUT value (active-low, 1 = released).

#pragma once

#include <cstddef>
#include <cstdint>

namespace gbarecomp {

class HostWindow {
public:
    // A fixed logical framebuffer mode exposed by the in-game Visual page.
    // The caller owns the label storage for the duration of the run; the host
    // copies each mode descriptor when set_fixed_view_modes() is called.
    struct FixedViewMode {
        const char* label = nullptr;
        std::uint16_t width = 240;
        std::uint16_t height = 160;
    };

    HostWindow();
    ~HostWindow();

    HostWindow(const HostWindow&) = delete;
    HostWindow& operator=(const HostWindow&) = delete;

    // True if this build was compiled against a real windowing
    // backend. When false, open() always fails.
    static bool is_available();

    // Open a window. `scale` is the integer scale factor applied to
    // the logical surface, whose size is `base_w` x `base_h` (240x160 for the
    // faithful view, wider when view-area expansion is active). Returns false on
    // failure (also when is_available() is false).
    // `screen` is the per-game color model from [video].screen in game.toml
    // (raw|unlit|frontlit|backlit|classic), or nullptr for none. The
    // GBARECOMP_SCREEN env var, when set, overrides it.
    // `linear_filter` selects linear (vs nearest) texture scaling — the
    // launcher's "Linear filtering" toggle; default preserves the historical
    // nearest look.
    bool open(int scale = 3, int base_w = 240, int base_h = 160,
              const char* title = "gbarecomp", const char* screen = nullptr,
              bool linear_filter = false, bool resize_driven_view = false,
              bool stereo_audio = false);
    void close();
    bool is_open() const { return open_; }

    // Resize the logical streaming surface without changing the host window.
    // Used only by the explicit resize-driven view policy; fixed-width callers
    // never invoke it. drawable_size() reports the live client-window extent
    // (and therefore follows drag-resize and borderless desktop fullscreen).
    bool set_surface_size(int base_w, int base_h);
    bool drawable_size(int* width, int* height) const;

    // Load player keybinds + system hotkeys from `dir` (the exe directory):
    //   * keybinds.ini — recomp-ui's generic keybinds format ([player1],
    //     SDL scancode names). Absent file => the built-in defaults below,
    //     which MATCH recomp-ui's defaults so the launcher rebind page and
    //     the game always agree: A=X B=Z L=C R=V Start=Return Select=RShift
    //     + arrow keys.
    //   * config.ini [KeyMap] — hotkey bindings (SDL keycode names with
    //     Ctrl+/Alt+/Shift+ prefixes): Fullscreen, Pause, Turbo (Turbo
    //     Held in the UI), WindowBigger, WindowSmaller, VolumeUp,
    //     VolumeDown, DisplayPerf, Menu, TurboToggle (Turbo Toggle),
    //     CheatMenu, AutoFireA, AutoFireB, QuitGame.
    //   * config.ini [KeyMap.Pad] (UI-02) — the controller half of the same
    //     rows, same names, SDL game-controller button strings (mirrors
    //     keybinds.ini's [player1]/[player1_pad] split).
    // Never called => built-in defaults for both (all hotkeys unbound on
    // the pad side except what the user binds in the Hotkeys tab). Safe to
    // call when the files don't exist.
    void load_input_config(const char* dir);

    // Live window/audio controls (hotkey + launcher-driven). All no-ops when
    // the window isn't open or this build has no SDL2.
    void set_fullscreen(bool on);
    bool fullscreen() const;
    void adjust_scale(int delta);       // integer window scale, clamped 1..8
    void set_volume(int pct);           // 0..100, applied to pushed samples
    int  volume() const;
    void set_fps_readout(bool on);      // presents-per-second in the title bar
    bool fps_readout() const;
    double display_refresh_hz() const;
    void set_frame_interpolation_available(bool on);
    void set_frame_interpolation_enabled(bool on);
    void set_enhanced_timing_available(bool on);
    void set_enhanced_timing_enabled(bool on);
    bool saved_enhanced_timing() const;
    void set_native_renderer_available(bool on);
    void set_native_renderer_enabled(bool on);
    void set_native_renderer_scale(int scale);
    bool native_renderer_enabled() const;
    int native_renderer_scale() const;
    // Advertise the fixed logical modes available to the in-game Visual page
    // and seed it with the mode currently active in the runtime. Strict and
    // capture callers pass false, which forces Native 240x160.
    void set_fixed_view_modes(bool on, const FixedViewMode* modes,
                              int count, int active_mode);
    int fixed_view_mode() const;

    // Legacy bool surface retained for older callers. It maps true to the
    // first non-native mode and false to Native.
    void set_widescreen_available(bool on, bool enabled);
    bool widescreen_enabled() const;
    void set_guest_frame(std::uint64_t frame);

    // Publish cached native/Turbo audio status for the optional in-game
    // developer overlay. Bounded text is copied; the overlay never calls
    // into guest/audio state while drawing.
    void set_debug_overlay_audio_state(bool native_requested,
                                       bool native_live,
                                       bool native_output_selected,
                                       const char* native_reason,
                                       bool turbo_requested,
                                       bool turbo_decoupled,
                                       bool turbo_pending,
                                       bool turbo_fallback,
                                       bool turbo_muted,
                                       bool turbo_uncapped,
                                       const char* reason);

    // Upload one base_w x base_h RGB888 frame (the dimensions passed to open())
    // and present.
    void present(const uint8_t* rgb888);

    // Present a scene already rendered at native supersampled dimensions. The
    // call is accepted only while the opt-in native renderer is enabled; an
    // inactive/mismatched call is ignored so callers can keep canonical
    // fallback selection explicit.
    void present_native(const uint8_t* rgb888, int width, int height);

    // Push `count` int16_t mono samples from the canonical GBA mixer into the
    // audio output queue. Backend converts to the host device's format and
    // applies the enhanced-timing clock ratio when that mode is enabled.
    // No-op if audio init failed or this build has no SDL2.
    void push_audio_samples(const int16_t* samples, std::size_t count);
    // Interleaved stereo variant used by the opt-in native MP2K renderer.
    void push_audio_samples_stereo(const int16_t* samples, std::size_t count);
    // Drop host audio only while Turbo requests it. Transitions flush the
    // resampler so releasing Turbo cannot play stale, sped-up samples.
    void set_turbo_audio_muted(bool on);
    // Flush host-side audio history after a guest savestate load.
    void reset_audio();
    // Read bounded host bridge loss counters. The Turbo wall path treats any
    // new loss after activation as a fatal fallback condition.
    void audio_bridge_stats(std::uint64_t* underruns,
                            std::uint64_t* overflows) const;

    struct Events {
        bool     quit = false;
        // GBA KEYINPUT layout. Active-low: 1 = released, 0 = pressed.
        // Bits: 0=A 1=B 2=Sel 3=Sta 4=Right 5=Left 6=Up 7=Down 8=R 9=L.
        uint16_t keyinput = 0x03FF;
        // Edge-triggered save-state slot hotkeys. F1..F9 load slot
        // 1..9; Shift+F1..F9 save slot 1..9. 0 = no request this pump.
        // The caller acts on these at the top of the loop (a clean
        // dispatch boundary), never mid-frame.
        int      save_slot = 0;
        int      load_slot = 0;
        // True when Turbo should be active this pump. Speeds up the frame
        // limiter — how much is `turbo_multiplier`, unless `turbo_uncapped`,
        // in which case the limiter is switched off entirely (as fast as the
        // host allows; audio will not keep up). Both come from the config
        // menu's Speed tab and are meaningless while fast_forward is false.
        // UI-02: driven by two separately bindable hotkeys sharing this one
        // flag — "Turbo Held" (level-triggered, default Tab: on only while
        // down) and "Turbo Toggle" (edge-triggered latch: press to flip
        // on/off). fast_forward is true when EITHER is currently active;
        // releasing Held does not clear an active Toggle latch.
        bool     fast_forward = false;
        float    turbo_multiplier = 4.0f;
        bool     turbo_uncapped = false;
        bool     turbo_mute_audio = false;
        bool     frame_interpolation_2x = false;
        bool     enhanced_timing = false;
        bool     view_mode_changed = false;
        int      view_mode = 0;
        // Compatibility aliases for older runtime callers.
        bool     widescreen_changed = false;
        bool     widescreen = false;
        // Edge-triggered system hotkeys (config.ini [KeyMap] bindings; see
        // load_input_config). The caller owns the semantics: fullscreen and
        // window scale route back into this window, pause gates stepping in
        // the run loop, volume adjusts pushed-sample gain, FPS toggles the
        // title-bar readout.
        bool     toggle_fullscreen = false;
        bool     toggle_pause = false;
        // The Cheat Menu hotkey (HK_GAME_MENU) was pressed this pump.
        bool     game_menu = false;
        bool     window_bigger = false;
        bool     window_smaller = false;
        bool     volume_up = false;
        bool     volume_down = false;
        bool     toggle_fps = false;

        // Env-gated host-pump phase timings. These split a rare long
        // SDL/input/config pump from the runtime's outer pump_us bucket.
        uint32_t pump_setup_us = 0;
        uint32_t pump_events_us = 0;
        uint32_t pump_input_us = 0;
        uint32_t pump_apply_us = 0;
        uint32_t pump_finalize_us = 0;
        // Bounded event-loop split for rare long pump_events rows. These are
        // populated only when frame diagnostics are enabled.
        uint32_t pump_event_count = 0;
        uint32_t pump_poll_us = 0;
        // Diagnostic-only split of SDL_PollEvent's two operations. The
        // instrumented path calls SDL_PumpEvents then SDL_PeepEvents so a
        // long empty poll can be attributed to the OS backend or queue
        // dequeue without changing the normal (unmeasured) path.
        uint32_t pump_sdl_pump_us = 0;
        uint32_t pump_sdl_peep_us = 0;
        uint32_t pump_dispatch_us = 0;
        uint32_t pump_event_max_us = 0;
        uint32_t pump_event_max_type = 0;
        uint32_t pump_event_max_subtype = 0;
    };
    Events pump();

private:
    bool open_ = false;
    void* impl_ = nullptr;  // backend-specific opaque
};

}  // namespace gbarecomp
