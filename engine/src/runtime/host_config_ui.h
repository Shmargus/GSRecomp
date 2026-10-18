// host_config_ui.h — Dear ImGui configuration UI for the host window.
//
// One Settings window with a sidebar of pages (Video, Audio, Controls,
// Hotkeys, Fast Forward, Enhancements, and Game when the game adds switches),
// usable by mouse, keyboard or controller. To bind, click a box then press a
// key or pad button.
//
// Strictly host-side. It runs at present time, after the guest frame has been
// uploaded, and touches nothing the emulation reads. The one exception is
// deliberate and narrow: while a bind box is armed, the UI swallows input so a
// keypress being captured is not also delivered to the guest.
//
// The whole file compiles away when GBARECOMP_HAVE_IMGUI is off; host_window
// calls the same entry points either way and gets no-ops.

#pragma once

#include <cstdint>

struct SDL_Window;
struct SDL_Renderer;
union SDL_Event;

namespace gbarecomp {

// CPU overclock choices (TURBO-B2-UI), used by host_window.cpp to persist and
// apply the factor. Index -> factor: 0=Off(1x) 1=On(50x ceiling). A pinned
// factor is already a ceiling: the game gets up to this many GBA cycles per
// frame and HALTs once its frame's work is done, so it never spends more than
// it needs.
inline constexpr unsigned kOverclockFactors[2] = {1, 50};
inline constexpr int kOverclockItemCount = 2;

// Synthetic "pad button" ids for the L2/R2 analog triggers, stored in the same
// int fields (HotkeyBind::pad_button / ConfigUiState::hotkey_pad) that
// otherwise hold an SDL_GameControllerButton. SDL reports the triggers as an
// axis (SDL_CONTROLLERAXISMOTION), never as a button. These ids sit strictly
// above SDL_CONTROLLER_BUTTON_MAX (21 in the pinned SDL2), so they cannot
// collide with a real button (0..20) or the -1 "unbound" sentinel.
// Written as a literal because this header deliberately carries no SDL
// include; each .cpp that uses them static_asserts the literal against
// SDL_CONTROLLER_BUTTON_MAX.
// Shared by host_window.cpp (event dispatch, INI parse/write) and
// host_config_ui.cpp (bind-box capture + display label).
inline constexpr int kPadTriggerLeftValue  = 21;  // == SDL_CONTROLLER_BUTTON_MAX
inline constexpr int kPadTriggerLeft  = kPadTriggerLeftValue;      // synthetic "L2"
inline constexpr int kPadTriggerRight = kPadTriggerLeftValue + 1;  // synthetic "R2"

// Synthetic ids for the eight analog stick directions, so a stick can be bound
// like a button. Same idea as the triggers: SDL reports sticks only as axes.
// Generated from SDL_CONTROLLERAXISMOTION in host_window.cpp with the same
// press/release hysteresis as the triggers.
inline constexpr int kPadLeftStickRight  = kPadTriggerLeftValue + 2;
inline constexpr int kPadLeftStickLeft   = kPadTriggerLeftValue + 3;
inline constexpr int kPadLeftStickUp     = kPadTriggerLeftValue + 4;
inline constexpr int kPadLeftStickDown   = kPadTriggerLeftValue + 5;
inline constexpr int kPadRightStickRight = kPadTriggerLeftValue + 6;
inline constexpr int kPadRightStickLeft  = kPadTriggerLeftValue + 7;
inline constexpr int kPadRightStickUp    = kPadTriggerLeftValue + 8;
inline constexpr int kPadRightStickDown  = kPadTriggerLeftValue + 9;

inline constexpr bool pad_is_stick(int id) {
    return id >= kPadLeftStickRight && id <= kPadRightStickDown;
}

// One name table for the synthetic ids, used by the ini load/save code and the
// bind-box label. Names are lowercase with no separators, like SDL's own
// button strings. "lefttrigger"/"righttrigger" are kept for old ini files.
inline const char* pad_synth_name(int id) {
    switch (id) {
    case kPadTriggerLeft:     return "lefttrigger";
    case kPadTriggerRight:    return "righttrigger";
    case kPadLeftStickRight:  return "leftstickright";
    case kPadLeftStickLeft:   return "leftstickleft";
    case kPadLeftStickUp:     return "leftstickup";
    case kPadLeftStickDown:   return "leftstickdown";
    case kPadRightStickRight: return "rightstickright";
    case kPadRightStickLeft:  return "rightstickleft";
    case kPadRightStickUp:    return "rightstickup";
    case kPadRightStickDown:  return "rightstickdown";
    default:                  return nullptr;
    }
}

// Returns the synthetic id for a name (case-insensitive), or -1 if unknown.
inline int pad_synth_from_name(const char* name) {
    if (!name) return -1;
    for (int id = kPadTriggerLeft; id <= kPadRightStickDown; ++id) {
        const char* n = pad_synth_name(id);
        const char* a = n;
        const char* b = name;
        while (*a && *b) {
            char c = *b;
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (*a != c) break;
            ++a; ++b;
        }
        if (!*a && !*b) return id;
    }
    return -1;
}

// Cached, host-owned values shown by the optional in-game debug overlay.
// Runtime code publishes audio state at its normal emulation boundary; the
// overlay never reads environment variables or guest memory while drawing.
struct DebugOverlayState {
    bool self_heal_ram = false;
    bool cost_probe = false;
    bool present_cadence = false;
    bool ram_churn_probe = false;
    bool additional_debug_logging = false;

    bool audio_status_available = false;
    bool turbo_audio_muted = false;
    bool turbo_audio_uncapped = false;
};

// Backend-provided choices keep the shared menu independent of each engine's
// colour implementation. Missing choices leave the control visibly disabled.
struct ConfigUiColourChoice {
    int value;
    const char* label;
    bool custom;
};

// Everything the UI can read or change. host_window owns the storage and
// passes a pointer in; the UI never reaches into the backend directly, which
// keeps this file free of SDL backend details and testable in isolation.
struct ConfigUiState {
    // --- input bindings -----------------------------------------------------
    // GBA KEYINPUT bit order: 0=A 1=B 2=Sel 3=Start 4=Right 5=Left 6=Up 7=Down 8=R 9=L.
    int   key_bind[10] = {};        // SDL_Scancode, 0 = unbound
    int   pad_bind[10] = {};        // SDL_GameControllerButton, -1 = unbound
    int   pad_bind_alt[10] = {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1}; // optional second controller bind
    int   hotkey_key[16] = {};      // SDL_Keycode per HostHotkey
    unsigned hotkey_mods[16] = {};  // required KMOD_* bits
    int   hotkey_pad[16] = {};      // SDL_GameControllerButton, -1 = unbound
    int   hotkey_count = 0;
    const char* const* hotkey_names = nullptr;  // display labels, not the persisted config.ini keys

    bool  stick_bindings_available = false;  // host supports synthetic stick ids

    // --- video --------------------------------------------------------------
    int   scale = 3;                // integer window scale
    int   max_scale = 8;            // largest scale that fits the display
    bool  fullscreen = false;
    bool  vsync = true;
    bool  linear_filter = false;    // off = crisp nearest-neighbour
    bool  integer_scale = true;
    const ConfigUiColourChoice* colour_choices = nullptr;
    int   colour_choice_count = 0;
    int   screen_kind = 0;          // backend-owned colour choice value
    int   color_saturation = 100;   // Custom colours: percent, 0..200
    int   color_hue = 0;            // Custom colours: degrees, -180..180
    int   color_brightness = 100;   // Custom: percent, 50..150
    int   color_warmth = 0;         // Custom: percent, 0..100
    int   color_darken = 0;         // Custom: percent, 0..50 (curve 1 + pct/100)
    bool  color_save = false;       // a Custom slider was let go: save it
    int   color_vision = 0;         // runtime::ColorVision: 0 normal, 1 deuteranopia, 2 tritanopia
    int   aspect = 0;               // ViewAspect index from view_config.h
    bool  aspect_view_available = false;
    bool  screen_filters_supported = false;
    bool  screen_filters_available = false;  // test: launcher box on + opengl renderer
    int   screen_filter = 0;        // test: 0 off, 1 LCD3x, 2 xBR, 3 CRT Lottes, 4 ScaleFX
    bool  show_fps = false;

    // --- audio --------------------------------------------------------------
    int   volume = 100;             // 0..100
    bool  mute = false;

    // --- speed --------------------------------------------------------------
    float turbo_multiplier = 4.0f;  // fast-forward speed multiple; `uncapped` overrides it
    bool  uncapped = false;
    bool  mute_during_turbo = false;

    // --- optional enhancements --------------------------------------------
    bool  frame_interpolation_available = false;
    bool  frame_interpolation_display_ok = false;
    bool  frame_interpolation_2x = false;
    bool  enhanced_timing_available = false;
    bool  enhanced_timing = false;
    bool  native_renderer_available = false;
    bool  native_renderer = false;
    int   native_scale = 2;          // native scene resolution, 1x..10x
    // Fixed logical view modes. The host only exposes these when the runtime
    // has explicitly authorized them; strict and capture runs force Native.
    bool  fixed_view_modes_available = false;
    int   fixed_view_mode = 0;
    int   fixed_view_mode_count = 0;
    const char* const* fixed_view_mode_labels = nullptr;
    // Legacy aliases kept in sync by host_window for callers that only know
    // the old bool Widescreen surface.
    bool  widescreen_available = false;
    bool  widescreen = false;
    int   overclock_index = 0;       // 0=Off(1x) 1=On(50x), see kOverclockFactors
    // VFX-FLICKER-02/03: temporal frame blend ("LCD ghosting"). The menu
    // offers 0=Off, 1..3=Flicker only Light/Medium/Strong; 4..6 are legacy
    // whole-frame values that host_window.cpp migrates to 1..3. See
    // kTemporalBlendItemCount and kTemporalBlendDefaultIndex in
    // temporal_blend.h (not included here). The default (3) must match
    // kTemporalBlendDefaultIndex.
    int   temporal_blend_index = 3;
    // Live per-BG-layer visibility toggle, applied when layers_changed is set
    // (no page in this menu sets it now). Render-only, session-only (never
    // persisted). Default false = all layers visible.
    bool  hide_bg0 = false;
    bool  hide_bg1 = false;
    bool  hide_bg2 = false;
    bool  hide_bg3 = false;

    // --- cheats (explicitly separate from optional enhancements) ------------
    // Gameplay-changing options are opt-in and default OFF.
    bool  infinite_hp = false;
    bool  infinite_pp = false;
    int   player_walk_run_speed_multiplier = 2;  // halves: 2 = 1x

    // --- logging --------------------------------------------------------
    // Single toggle for every verbose/diagnostic stream that isn't already
    // always-on (coverage-honesty banner, dispatch-miss log, hang watchdog,
    // etc. stay on regardless). Off by default. See runtime_arm.h
    // gsr_set_additional_debug_logging / gsr_additional_debug_logging.
    bool  additional_debug_logging = false;
    // Enhancements > Troubleshooting: record the game's last instructions
    // for crash and freeze reports (runtime_set_crash_log). Saved in
    // [Logging] as CrashLog; off by default because it costs speed.
    bool  crash_log = false;
    bool  debug_overlay = false;
    DebugOverlayState debug_overlay_state{};

    // --- readouts (UI shows, never sets) ------------------------------------
    float fps = 0.0f;
    float frame_ms = 0.0f;
    float emulation_speed_percent = 0.0f;
    unsigned long long guest_frame = 0;

    // --- change flags, consumed by host_window each frame -------------------
    bool  binds_changed = false;
    bool  video_changed = false;
    bool  audio_changed = false;
    bool  speed_changed = false;
    bool  enhancements_changed = false;
    bool  layers_changed = false;
    bool  cheats_changed = false;
    bool  logging_changed = false;
    bool  debug_overlay_changed = false;
    bool  request_quit = false;
};

// Create/destroy the ImGui context and SDL backends. Safe to call when ImGui
// is not compiled in (init returns false, shutdown does nothing).
//
// `use_opengl` is host_window's runtime check (SDL_GetRendererInfo on the
// created renderer, not the hint) of whether `renderer` really is the
// "opengl" SDL_Renderer driver. When true, this wires up imgui_impl_opengl3 +
// docking + real multi-viewport (panels torn into their own OS windows). When
// false (the d3d11/software fallback) it uses imgui_impl_sdlrenderer2 with
// docking but no viewports, since that backend has none upstream.
bool config_ui_init(SDL_Window* window, SDL_Renderer* renderer,
                     bool use_opengl);
void config_ui_shutdown();

// Optional release decoration, supplied beside the executable. No badge is
// shown unless the game supplies both the bitmap and its caption.
void config_ui_set_edition_badge(const char* bitmap_path, const char* caption,
                                 const char* flourish);

// Feed every SDL event here BEFORE the host window interprets it. Returns true
// when the UI consumed the event and the host/guest must ignore it — true
// whenever the menu is visible and the event is mouse/keyboard input.
bool config_ui_handle_event(const SDL_Event* e);

// Is the menu currently visible? While visible, host_window feeds the guest
// all-buttons-released.
bool config_ui_visible();
void config_ui_toggle();
void config_ui_set_visible(bool on);

// Feed a raw key / pad press to whichever bind box is armed. Returns true if
// the press was consumed as a binding (or cancelled one), meaning the caller
// must not treat it as a hotkey or guest input. No-ops when nothing is armed.
bool config_ui_capture_key(ConfigUiState* st, int scancode, int keycode,
                           unsigned mods);
bool config_ui_capture_pad(ConfigUiState* st, int button);

// Build and draw the UI for this frame. Call between the guest-frame render
// and SDL_RenderPresent. `st` is read for current values and written when the
// user changes something.
void config_ui_draw(ConfigUiState* st);

// Extra host-side ImGui content, drawn every frame (like the debug overlay)
// regardless of whether the F1 menu itself is open. Game-owned code (e.g. a
// function-call tracer) sets this once at startup; nullptr draws nothing
// extra. Declared unconditionally so the assignment compiles the same way
// whether or not Dear ImGui was actually compiled in; the pointer is simply
// unused in that case. This is the one seam game code uses to draw its own
// ImGui windows -- it must not call any other ImGui/SDL internals here.
extern void (*g_config_ui_extra_draw)();

// Paired predicate: does the extra content above want to own the keyboard
// right now (e.g. a text box in it is focused, or it has a modal prompt
// up)? The host consults this next to config_ui_visible() wherever it gates
// guest keyinput and hotkeys on the F1 menu, so typing into a game-owned
// ImGui window (which can be open while the F1 menu itself is closed)
// doesn't also reach the guest or fire a hotkey. nullptr means "never wants
// it" -- the default, and what every build without such a window gets.
// Declared unconditionally, same as g_config_ui_extra_draw.
extern bool (*g_config_ui_extra_wants_keyboard)();

// On/off switches a game adds to F1's "Game" page, each stored as 0/1 under
// its key in game_options.ini (game_options.h). The array must outlive the
// menu; without switches or game-owned controls the page is hidden.
// Declared unconditionally, same as above.
struct GameOptionSwitch {
    const char* label;
    const char* caption;
    const char* key;
};
void config_ui_set_game_switches(const GameOptionSwitch* switches, int count);
// Optional game-owned controls below the switches on F1's Game page.
void config_ui_set_game_page(void (*draw)());

}  // namespace gbarecomp
