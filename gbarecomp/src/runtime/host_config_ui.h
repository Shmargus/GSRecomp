// host_config_ui.h — Dear ImGui configuration UI for the host window.
//
// Ship-of-Harkinian-shaped: a menu bar across the top with dockable panels
// under it (Controller, Hotkeys, Video, Audio, Enhancements), mouse-driven,
// click a bind box then press a key or pad button.
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

// CPU Overclock combo values (TURBO-B2-UI), shared by host_config_ui.cpp
// (draws the combo) and host_window.cpp (persists + applies the factor).
// Index -> factor: 0=Off(1x) 1=On(50x ceiling; 10x until 2026-09-30, raised
// at Jimmy's request). A pinned factor is already a
// ceiling: the game gets up to this many GBA cycles per frame and HALTs once
// its frame's work is done, so it never spends more than it needs.
inline constexpr unsigned kOverclockFactors[2] = {1, 50};
inline constexpr int kOverclockItemCount = 2;

// UI-02b: synthetic "pad button" ids for the L2/R2 analog triggers, stored in
// the same int fields (HotkeyBind::pad_button / ConfigUiState::hotkey_pad)
// that otherwise hold an SDL_GameControllerButton. SDL reports the triggers
// as an axis (SDL_CONTROLLER_AXIS_TRIGGERLEFT/RIGHT via
// SDL_CONTROLLERAXISMOTION), never as a button, so there is no real
// SDL_GameControllerButton value for them to reuse. These sentinels sit
// strictly above SDL_CONTROLLER_BUTTON_MAX (21 in the pinned SDL2), so they
// can never collide with a real button value (0..20) or the -1 "unbound"
// sentinel — existing bindings keep meaning exactly what they meant before.
// Written as a literal (not SDL_CONTROLLER_BUTTON_MAX) because this header
// deliberately carries no SDL include, so it stays testable in isolation;
// each .cpp that uses these sentinels static_asserts the literal still
// matches SDL_CONTROLLER_BUTTON_MAX at compile time.
// Shared by host_window.cpp (event dispatch, INI parse/write) and
// host_config_ui.cpp (bind-box capture + display label).
inline constexpr int kPadTriggerLeftValue  = 21;  // == SDL_CONTROLLER_BUTTON_MAX
inline constexpr int kPadTriggerLeft  = kPadTriggerLeftValue;      // synthetic "L2"
inline constexpr int kPadTriggerRight = kPadTriggerLeftValue + 1;  // synthetic "R2"

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
    bool native_mp2k_requested = false;
    bool native_mp2k_live = false;
    bool native_output_selected = false;
    char native_mp2k_reason[64] = {};
    bool turbo_audio_requested = false;
    bool turbo_audio_decoupled = false;
    bool turbo_audio_pending = false;
    bool turbo_audio_fallback = false;
    bool turbo_audio_muted = false;
    bool turbo_audio_uncapped = false;
    char turbo_audio_reason[64] = {};
};

// Everything the UI can read or change. host_window owns the storage and
// passes a pointer in; the UI never reaches into the backend directly, which
// keeps this file free of SDL backend details and testable in isolation.
struct ConfigUiState {
    // --- input bindings -----------------------------------------------------
    // GBA KEYINPUT bit order: 0=A 1=B 2=Sel 3=Start 4=R 5=L 6=Up 7=Down 8=R 9=L.
    int   key_bind[10] = {};        // SDL_Scancode, 0 = unbound
    int   pad_bind[10] = {};        // SDL_GameControllerButton, -1 = unbound
    int   hotkey_key[16] = {};      // SDL_Keycode per HostHotkey
    unsigned hotkey_mods[16] = {};  // required KMOD_* bits
    int   hotkey_pad[16] = {};      // SDL_GameControllerButton, -1 = unbound (UI-02)
    int   hotkey_count = 0;
    const char* const* hotkey_names = nullptr;  // display labels (UI-02: not the persistence keys)

    // --- video --------------------------------------------------------------
    int   scale = 3;                // integer window scale
    int   max_scale = 8;            // largest scale that fits the display
    bool  fullscreen = false;
    bool  vsync = true;
    bool  linear_filter = false;    // off = crisp nearest-neighbour
    bool  integer_scale = true;
    int   screen_kind = 0;          // Raw/Unlit/Frontlit/Backlit/Classic
    bool  show_fps = false;

    // --- audio --------------------------------------------------------------
    int   volume = 100;             // 0..100
    bool  mute = false;

    // --- speed --------------------------------------------------------------
    float turbo_multiplier = 4.0f;  // held-Turbo speed cap, 0 = uncapped
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
    // VFX-FLICKER-02/03: temporal frame blend ("LCD ghosting"). 0=Off,
    // 1..3=Flicker only (selective) Light/Medium/Strong, 4..6=Whole frame
    // Light/Medium/Strong — see kTemporalBlendItemCount and
    // kTemporalBlendDefaultIndex in temporal_blend.h (host_config_ui.h
    // itself stays free of that include — it only draws the combo,
    // host_window.cpp applies the weight). Default (3) is "Flicker only —
    // Strong"; must match kTemporalBlendDefaultIndex.
    int   temporal_blend_index = 3;
    // Live per-BG-layer visibility toggle (Enhancements > Visual > Layers).
    // Render-only, session-only (never persisted, never serialized). Default
    // false = all layers visible, matching current behavior exactly.
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
// is not compiled in (both become no-ops and `active()` stays false).
//
// `use_opengl` is host_window's own runtime check (SDL_GetRendererInfo on
// the already-created renderer, never assumed from a hint) of whether
// `renderer` really is the "opengl" SDL_Renderer driver. When true, this
// wires up imgui_impl_opengl3 + docking + real multi-viewport (panels torn
// into their own OS windows). When false — the d3d11/software fallback —
// this keeps the historical imgui_impl_sdlrenderer2 backend with docking
// enabled but viewports off, since that backend has no upstream viewport
// support. Either way the caller gets a working config UI; only the
// multi-viewport capability differs.
bool config_ui_init(SDL_Window* window, SDL_Renderer* renderer,
                     bool use_opengl);
void config_ui_shutdown();

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

}  // namespace gbarecomp
