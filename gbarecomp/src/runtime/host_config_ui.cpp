// host_config_ui.cpp — see host_config_ui.h.

#include "host_config_ui.h"

namespace gbarecomp {
// Defined unconditionally (see host_config_ui.h) so a game runner can assign
// it the same way in every build configuration.
void (*g_config_ui_extra_draw)() = nullptr;
bool (*g_config_ui_extra_wants_keyboard)() = nullptr;
}  // namespace gbarecomp

#ifdef GBARECOMP_HAVE_IMGUI

#include <SDL.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include "imgui_impl_opengl3.h"
#include "frame_timing.h"
#include "temporal_blend.h"

namespace gbarecomp {

namespace {

bool g_ready = false;
bool g_visible = false;
SDL_Renderer* g_renderer = nullptr;
// The MAIN window and its GL context, captured at config_ui_init() (opengl
// path only). SHUTDOWN-FREEZE-01: with real multi-viewport, ImGui may leave
// a torn-off viewport's own GL context current at exit time; shutdown must
// make the MAIN context current again before tearing anything down, or
// ImGui_ImplOpenGL3_Shutdown() deletes GL objects (VAOs/shaders/textures)
// against the wrong context and DestroyPlatformWindows() can destroy the
// context that owns them.
SDL_Window* g_window = nullptr;
SDL_GLContext g_gl_context = nullptr;
// Which ImGui rendering backend is live — set once at config_ui_init() from
// host_window's own SDL_GetRendererInfo check, never guessed here. Routes
// config_ui_draw()/config_ui_shutdown() between imgui_impl_opengl3 (real
// multi-viewport) and imgui_impl_sdlrenderer2 (docking only).
bool g_use_opengl = false;

// Which bind box is armed, if any. Exactly one capture can be live at a time,
// and while it is, every key/pad press is swallowed and consumed as the bind.
// HotkeyPad is the UI-02 controller half of a hotkey row (Hotkey is its
// keyboard half — same row, two independent bind boxes).
enum class Capture { None, Key, Pad, Hotkey, HotkeyPad };
Capture g_capture = Capture::None;
int     g_capture_index = -1;
bool    g_config_window_recover = false;

// GBA buttons in controller order rather than KEYINPUT bit order, with the
// bit each row maps to. Reads like a pad, not like a hardware register.
const struct { const char* label; int bit; } kButtons[] = {
    { "D-Pad Up",    6 }, { "D-Pad Down",  7 },
    { "D-Pad Left",  5 }, { "D-Pad Right", 4 },
    { "A",           0 }, { "B",           1 },
    { "L",           9 }, { "R",           8 },
    { "Start",       3 }, { "Select",      2 },
};
constexpr int kButtonCount = static_cast<int>(sizeof(kButtons) /
                                              sizeof(kButtons[0]));

const char* key_label(int scancode) {
    if (scancode == 0) return "(unset)";
    const char* n = SDL_GetScancodeName(static_cast<SDL_Scancode>(scancode));
    return (n && *n) ? n : "(unset)";
}

// host_config_ui.h's kPadTriggerLeft/Right sentinels are a literal (that
// header carries no SDL include) rather than SDL_CONTROLLER_BUTTON_MAX
// directly; keep them honest against this SDL2's real enum value.
static_assert(kPadTriggerLeftValue == static_cast<int>(SDL_CONTROLLER_BUTTON_MAX),
             "kPadTriggerLeftValue must track SDL_CONTROLLER_BUTTON_MAX so the "
             "synthetic L2/R2 ids never collide with a real pad button");

const char* pad_label(int button) {
    if (button < 0) return "(unset)";
    // UI-02b: L2/R2 are synthetic ids, not a real SDL_GameControllerButton —
    // label them with SDL's own axis-name strings ("lefttrigger"/
    // "righttrigger"), matching the lowercase-no-separator convention every
    // other row in this column already uses (SDL_GameControllerGetStringForButton
    // output, e.g. "leftshoulder", "dpup").
    if (button == kPadTriggerLeft)  return "lefttrigger";
    if (button == kPadTriggerRight) return "righttrigger";
    const char* n = SDL_GameControllerGetStringForButton(
        static_cast<SDL_GameControllerButton>(button));
    return (n && *n) ? n : "(unset)";
}

// Hotkeys carry modifiers, so they render as "Shift+P" rather than a bare key.
void hotkey_label(char* out, std::size_t n, int keycode, unsigned mods) {
    if (keycode == 0) { std::snprintf(out, n, "(unset)"); return; }
    char buf[96];
    buf[0] = '\0';
    if (mods & KMOD_CTRL)  std::strncat(buf, "Ctrl+",  sizeof(buf) - 1);
    if (mods & KMOD_ALT)   std::strncat(buf, "Alt+",   sizeof(buf) - 1);
    if (mods & KMOD_SHIFT) std::strncat(buf, "Shift+", sizeof(buf) - 1);
    const char* kn = SDL_GetKeyName(static_cast<SDL_Keycode>(keycode));
    std::snprintf(out, n, "%s%s", buf, (kn && *kn) ? kn : "?");
}

// Keep the main pages compact. Long explanations belong in a hover tooltip so
// the control they describe stays visible without pushing the next setting
// below the fold. AllowWhenDisabled is intentional: unavailable options still
// explain why they cannot currently be used.
void hover_tooltip(const char* text) {
    if (!text || !*text ||
        !ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) return;
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 36.0f);
    ImGui::TextWrapped("%s", text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

void help_marker(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    hover_tooltip(text);
}

// A bind row: label on the left, a button showing the current binding that
// arms capture when clicked, and a clear button. Returns true if changed.
// `column` lets a caller place the label/value/clear triple at a table
// column offset other than 0/1/2 (UI-02: draw_hotkeys_page puts the pad half
// of a row at columns 3/4 alongside the keyboard half at 1/2).
bool bind_row(const char* label, const char* value, bool armed,
              Capture kind, int index, bool* cleared, int column = 0) {
    const bool is_pad = (kind == Capture::Pad || kind == Capture::HotkeyPad);
    ImGui::PushID(index * 8 + static_cast<int>(kind));
    if (column == 0) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::TableSetColumnIndex(1);
    } else {
        ImGui::TableSetColumnIndex(column);
    }

    bool clicked = false;
    if (armed) {
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImVec4(0.85f, 0.55f, 0.15f, 1.0f));
        clicked = ImGui::Button(is_pad ? "press a button..." : "press a key...",
                                ImVec2(-FLT_MIN, 0));
        ImGui::PopStyleColor();
    } else {
        clicked = ImGui::Button(value, ImVec2(-FLT_MIN, 0));
    }
    ImGui::TableSetColumnIndex(column + 1);
    *cleared = ImGui::SmallButton(is_pad ? "clear##p" : "clear##k");
    ImGui::PopID();
    return clicked;
}

void draw_controls_page(ConfigUiState* st) {
    ImGui::SeparatorText("Gameplay Bindings");
    help_marker("Click a binding, then press a key or controller button. "
                "Press Esc to cancel a capture.");

    ImGui::BeginChild("controls_bindings_scroll",
                      ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing() * 2.0f),
                      false, ImGuiWindowFlags_HorizontalScrollbar);
    if (ImGui::BeginTable("binds", 5,
                          ImGuiTableFlags_SizingFixedFit |
                          ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Button", ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("Keyboard", ImGuiTableColumnFlags_WidthFixed, 130);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 46);
        ImGui::TableSetupColumn("Controller", ImGuiTableColumnFlags_WidthFixed, 130);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 46);
        ImGui::TableHeadersRow();

        for (int i = 0; i < kButtonCount; ++i) {
            const int bit = kButtons[i].bit;
            ImGui::PushID(i);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(kButtons[i].label);

            // Keyboard column.
            ImGui::TableSetColumnIndex(1);
            const bool key_armed =
                (g_capture == Capture::Key && g_capture_index == bit);
            if (key_armed) {
                ImGui::PushStyleColor(ImGuiCol_Button,
                                      ImVec4(0.85f, 0.55f, 0.15f, 1.0f));
                if (ImGui::Button("press a key...", ImVec2(-FLT_MIN, 0)))
                    g_capture = Capture::None;
                ImGui::PopStyleColor();
            } else if (ImGui::Button(key_label(st->key_bind[bit]),
                                     ImVec2(-FLT_MIN, 0))) {
                g_capture = Capture::Key;
                g_capture_index = bit;
            }
            ImGui::TableSetColumnIndex(2);
            if (ImGui::SmallButton("x##k")) {
                st->key_bind[bit] = 0;
                st->binds_changed = true;
            }

            // Controller column.
            ImGui::TableSetColumnIndex(3);
            const bool pad_armed =
                (g_capture == Capture::Pad && g_capture_index == bit);
            if (pad_armed) {
                ImGui::PushStyleColor(ImGuiCol_Button,
                                      ImVec4(0.85f, 0.55f, 0.15f, 1.0f));
                if (ImGui::Button("press a button...", ImVec2(-FLT_MIN, 0)))
                    g_capture = Capture::None;
                ImGui::PopStyleColor();
            } else if (ImGui::Button(pad_label(st->pad_bind[bit]),
                                     ImVec2(-FLT_MIN, 0))) {
                g_capture = Capture::Pad;
                g_capture_index = bit;
            }
            ImGui::TableSetColumnIndex(4);
            if (ImGui::SmallButton("x##p")) {
                st->pad_bind[bit] = -1;
                st->binds_changed = true;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    ImGui::SeparatorText("Reset");
    if (ImGui::Button("Restore Defaults")) {
        static const int kDefaultKeys[10] = {
            SDL_SCANCODE_X, SDL_SCANCODE_Z, SDL_SCANCODE_RSHIFT,
            SDL_SCANCODE_RETURN, SDL_SCANCODE_RIGHT, SDL_SCANCODE_LEFT,
            SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_V,
            SDL_SCANCODE_C,
        };
        static const int kDefaultPads[10] = {
            SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B,
            SDL_CONTROLLER_BUTTON_BACK, SDL_CONTROLLER_BUTTON_START,
            SDL_CONTROLLER_BUTTON_DPAD_RIGHT, SDL_CONTROLLER_BUTTON_DPAD_LEFT,
            SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
            SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
            SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
        };
        for (int i = 0; i < 10; ++i) {
            st->key_bind[i] = kDefaultKeys[i];
            st->pad_bind[i] = kDefaultPads[i];
        }
        st->binds_changed = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("keybinds.ini");
    hover_tooltip("Gameplay bindings are saved to keybinds.ini.");
}

void draw_hotkeys_page(ConfigUiState* st) {
    ImGui::SeparatorText("Hotkeys");
    help_marker("System hotkeys work while the game has focus. Bind either "
                "the keyboard key, the controller button, or both; values "
                "are saved to config.ini.");
    ImGui::BeginChild("hotkeys_scroll",
                      ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing()),
                      false, ImGuiWindowFlags_HorizontalScrollbar);
    if (ImGui::BeginTable("hotkeys", 5,
                          ImGuiTableFlags_SizingFixedFit |
                          ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 130);
        ImGui::TableSetupColumn("Keyboard", ImGuiTableColumnFlags_WidthFixed, 130);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 46);
        ImGui::TableSetupColumn("Controller", ImGuiTableColumnFlags_WidthFixed, 130);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 46);
        ImGui::TableHeadersRow();
        for (int h = 0; h < st->hotkey_count; ++h) {
            // Keyboard half.
            char label[128];
            hotkey_label(label, sizeof(label), st->hotkey_key[h],
                         st->hotkey_mods[h]);
            bool key_cleared = false;
            const bool key_armed =
                (g_capture == Capture::Hotkey && g_capture_index == h);
            if (bind_row(st->hotkey_names ? st->hotkey_names[h] : "?",
                         label, key_armed, Capture::Hotkey, h, &key_cleared)) {
                if (key_armed) g_capture = Capture::None;
                else { g_capture = Capture::Hotkey; g_capture_index = h; }
            }
            if (key_cleared) {
                st->hotkey_key[h] = 0;
                st->hotkey_mods[h] = 0;
                st->binds_changed = true;
            }

            // Controller half (UI-02): same row, columns 3/4.
            bool pad_cleared = false;
            const bool pad_armed =
                (g_capture == Capture::HotkeyPad && g_capture_index == h);
            if (bind_row("", pad_label(st->hotkey_pad[h]), pad_armed,
                         Capture::HotkeyPad, h, &pad_cleared, 3)) {
                if (pad_armed) g_capture = Capture::None;
                else { g_capture = Capture::HotkeyPad; g_capture_index = h; }
            }
            if (pad_cleared) {
                st->hotkey_pad[h] = -1;
                st->binds_changed = true;
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

void draw_video_page(ConfigUiState* st) {
    // Apply the scale ONLY when the drag ends. Signalling on every tick made
    // the host resize the window and rebuild the swapchain once per step, so
    // dragging the slider stuttered badly and was hard to land on a value.
    // The number still tracks the cursor live; only the expensive resize is
    // deferred to release. IsItemDeactivatedAfterEdit also covers keyboard and
    // ctrl-click entry, which SliderInt's return value alone does not.
    ImGui::SeparatorText("Display");
    ImGui::SliderInt("Window scale", &st->scale, 1, 8, "%dx");
    hover_tooltip("Integer scale of the host window. The resize is applied "
                  "when you release the slider.");
    if (ImGui::IsItemDeactivatedAfterEdit()) st->video_changed = true;
    if (ImGui::Checkbox("Fullscreen", &st->fullscreen)) st->video_changed = true;
    hover_tooltip("Toggle fullscreen presentation.");
    if (ImGui::Checkbox("V-Sync", &st->vsync)) st->video_changed = true;
    hover_tooltip("Synchronize presentation to the display. The guest clock "
                  "and emulated hardware timing remain unchanged.");
    if (ImGui::Checkbox("Integer scaling (no stretched pixels)",
                        &st->integer_scale))
        st->video_changed = true;
    hover_tooltip("Keep whole GBA pixels sharp instead of stretching them.");
    ImGui::SeparatorText("Color");
    static const char kScreenItems[] =
        "Raw (faithful output)\0"
        "Original GBA (unlit)\0"
        "GBA SP (frontlit)\0"
        "GBA SP (backlit)\0"
        "Classic vivid\0";
    if (ImGui::Combo("Color profile", &st->screen_kind, kScreenItems))
        st->video_changed = true;
    hover_tooltip("Choose the display color treatment. Raw is the faithful "
                  "unmodified output.");
    if (ImGui::Checkbox("Show FPS", &st->show_fps)) st->video_changed = true;
    hover_tooltip("Show the host FPS readout in the game window.");

    ImGui::SeparatorText("Status");
    ImGui::Text("Present: %.1f fps  (%.2f ms/frame)", st->fps, st->frame_ms);
    ImGui::Text("Guest frame: %llu", st->guest_frame);
    help_marker("The faithful GBA rate is 59.73 Hz. Above that the pacer "
                "idles; below it, the emulator is the bottleneck.");
}

void draw_audio_page(ConfigUiState* st) {
    ImGui::SeparatorText("Output");
    if (ImGui::SliderInt("Volume", &st->volume, 0, 100, "%d%%"))
        st->audio_changed = true;
    hover_tooltip("Set the host output volume.");
    if (ImGui::Checkbox("Mute", &st->mute)) st->audio_changed = true;
    hover_tooltip("Mute host audio without changing guest audio timing.");
}

void draw_turbo_page(ConfigUiState* st) {
    ImGui::SeparatorText("Turbo");
    help_marker("Bind Turbo Held or Turbo Toggle under Hotkeys. Held speeds "
                "up only while pressed; Toggle latches on or off. Default audio "
                "is coupled. The root-launcher experimental toggle "
                "enables MP2K music-only wall audio; PSG/FIFO are omitted. "
                "Only 2x-4x is supported; 1x, above 4x, uncapped, reverb, or "
                "other unsupported state falls back to canonical coupled audio. "
                "Explicit MuteDuringTurbo wins.");
    ImGui::SliderFloat("Turbo speed", &st->turbo_multiplier, 1.0f,
                       kMaxTurboMultiplier, "%.1fx");
    hover_tooltip("Speed cap used while Turbo is active. The root launcher can "
                  "enable experimental normal-speed MP2K Turbo audio. The "
                  "default path remains coupled. Experimental path supports only 2x-4x; 1x, above "
                  "4x, uncapped, reverb, or unsupported state falls back to "
                  "canonical coupled audio. Explicit MuteDuringTurbo wins.");
    if (ImGui::IsItemDeactivatedAfterEdit()) st->speed_changed = true;
    if (ImGui::Checkbox("Mute audio while Turbo is held",
                        &st->mute_during_turbo))
        st->speed_changed = true;
    hover_tooltip("Mute audio during held Turbo. This does not change guest "
                  "timing.");
    if (ImGui::Checkbox("Uncapped while held (ignore the multiplier)",
                        &st->uncapped))
        st->speed_changed = true;
    hover_tooltip("Run as fast as the host allows while Turbo is held, ignoring "
                  "the multiplier. This is not faithful timing.");
}

void draw_timing_page(ConfigUiState* st) {
    ImGui::SeparatorText("Timing");
    if (st->enhanced_timing_available) {
        if (ImGui::Checkbox("Enhanced Timing (exact 60/120 Hz)",
                            &st->enhanced_timing))
            st->enhancements_changed = true;
        hover_tooltip("Paces guest updates at exactly 60.000 Hz and 2x "
                      "presentation at 120.000 Hz. Per-frame hardware cycles "
                      "stay unchanged; canonical audio is resampled to the "
                      "active host clock.");
        if (st->frame_interpolation_display_ok)
            help_marker("Recommended for smoother cadence on this high-refresh "
                        "display. Your choice is saved.");
        else
            help_marker("Optional presentation timing mode. Your choice is saved.");
    } else {
        ImGui::TextDisabled("Enhanced timing is unavailable in this build.");
    }
}

void draw_performance_page(ConfigUiState* st) {
    ImGui::SeparatorText("Performance");
    static const char kOverclockItems[] = "Off\0" "On\0";
    if (st->overclock_index < 0 ||
        st->overclock_index >= gbarecomp::kOverclockItemCount)
        st->overclock_index = 0;
    if (ImGui::Combo("Guest CPU Overclock", &st->overclock_index, kOverclockItems))
        st->enhancements_changed = true;
    hover_tooltip("On lets the game use up to ten times a Game Boy Advance's "
                  "processor cycles in a frame, which it only takes while it "
                  "is running out of time; picture and sound speed are "
                  "unchanged. If music or animation misbehaves, turn this "
                  "off.");
}

bool fixed_view_mode_item_getter(void* data, int index,
                                 const char** out_text) {
    if (!data || !out_text || index < 0) return false;
    auto* st = static_cast<ConfigUiState*>(data);
    if (!st->fixed_view_mode_labels ||
        index >= st->fixed_view_mode_count) return false;
    const char* label = st->fixed_view_mode_labels[index];
    if (!label || !*label) return false;
    *out_text = label;
    return true;
}

void draw_visual_page(ConfigUiState* st) {
    ImGui::SeparatorText("Display");
    if (st->fixed_view_modes_available &&
        st->fixed_view_mode_count > 0 && st->fixed_view_mode_labels) {
        st->fixed_view_mode = std::clamp(
            st->fixed_view_mode, 0, st->fixed_view_mode_count - 1);
        if (ImGui::Combo("Expanded Widescreen", &st->fixed_view_mode,
                         fixed_view_mode_item_getter, st,
                         st->fixed_view_mode_count))
            st->enhancements_changed = true;
        hover_tooltip("Choose Native 240x160, Widescreen 288x160, or the "
                      "game's Expanded Widescreen 360x240 view. The guest "
                      "timing remains unchanged.");
    } else if (st->widescreen_available) {
        // Compatibility fallback for an older host caller that has not yet
        // supplied the mode table.
        if (ImGui::Checkbox("Widescreen (288x160)", &st->widescreen))
            st->enhancements_changed = true;
        hover_tooltip("Use the game's fixed 288x160 expanded view. The guest "
                      "remains at its authentic 160-line timing.");
    } else {
        ImGui::TextDisabled("Expanded Widescreen is unavailable in this run.");
    }

    ImGui::SeparatorText("Flicker");
    {
        // VFX-FLICKER-02: selective temporal blend ("LCD ghosting"),
        // opt-in and off by default. Whole-frame modes remain implemented
        // for compatibility, but are intentionally hidden from this page.
        // Disabled (greyed, not hidden) while 2x
        // scene interpolation is on: interpolation already synthesizes a
        // midpoint frame between real ones, and blending across that
        // synthetic pair would smear the picture instead of steadying it —
        // see host_window.cpp's present() for where that is enforced.
        static const char kTemporalBlendItems[] =
            "Off\0"
            "Flicker only (Light)\0" "Flicker only (Medium)\0"
            "Flicker only (Strong)\0";
        // Legacy config values 4..6 selected hidden whole-frame modes. Make
        // that migration explicit and persist it through the normal change
        // path, so an old file cannot silently keep a hidden mode active.
        if (st->temporal_blend_index >= 4) {
            st->temporal_blend_index = 3;
            st->enhancements_changed = true;
        }
        if (st->temporal_blend_index < 0 || st->temporal_blend_index > 3)
            st->temporal_blend_index = gbarecomp::kTemporalBlendDefaultIndex;
        const bool blend_blocked_by_interp =
            st->frame_interpolation_available && st->frame_interpolation_2x;
        if (blend_blocked_by_interp) ImGui::BeginDisabled();
        if (ImGui::Combo("Temporal blend (LCD ghosting)",
                         &st->temporal_blend_index, kTemporalBlendItems))
            st->enhancements_changed = true;
        hover_tooltip("Reduces intentional alternating-frame flicker, such as "
                      "the Kolima barrier or world map. Flicker-only keeps "
                      "ordinary movement sharp. Off leaves the picture "
                      "untouched.");
        if (blend_blocked_by_interp) ImGui::EndDisabled();
        if (blend_blocked_by_interp)
            ImGui::TextDisabled(
                "Disabled by the current presentation mode.");
    }

    ImGui::SeparatorText("Layers");
    help_marker("Render-only BG layer visibility. Takes effect immediately "
                "and never touches guest memory or timing.");
    if (ImGui::Checkbox("Hide BG0", &st->hide_bg0)) st->layers_changed = true;
    if (ImGui::Checkbox("Hide BG1", &st->hide_bg1)) st->layers_changed = true;
    if (ImGui::Checkbox("Hide BG2", &st->hide_bg2)) st->layers_changed = true;
    if (ImGui::Checkbox("Hide BG3", &st->hide_bg3)) st->layers_changed = true;
}

void draw_timing_performance_page(ConfigUiState* st) {
    draw_timing_page(st);
    draw_performance_page(st);
}

[[maybe_unused]] void draw_native_rendering_page(ConfigUiState* st) {
    ImGui::SeparatorText("Native Rendering");
    if (!st->native_renderer_available) {
        ImGui::TextDisabled("Native rendering is unavailable in this build.");
        return;
    }
    if (ImGui::Checkbox("Native compositor (experimental)",
                        &st->native_renderer))
        st->video_changed = true;
    hover_tooltip("Supersamples the scene with the optional native compositor. "
                  "Canonical rendering remains the fallback.");
    static const char kNativeScaleItems[] =
        "1x\0" "2x\0" "3x\0" "4x\0" "5x\0"
        "6x\0" "7x\0" "8x\0" "9x\0" "10x\0";
    if (st->native_scale < 1) st->native_scale = 1;
    if (st->native_scale > 10) st->native_scale = 10;
    int native_scale_index = st->native_scale - 1;
    if (ImGui::Combo("Native render resolution", &native_scale_index,
                     kNativeScaleItems)) {
        st->native_scale = native_scale_index + 1;
        st->video_changed = true;
    }
    hover_tooltip("Native scene scale from 1x to 10x, fitted to the current "
                  "window.");
}

void draw_cheats_player_page(ConfigUiState* st) {
    ImGui::SeparatorText("Player");
    help_marker("Opt-in gameplay cheats. These change game state and are "
                "disabled by default.");
    if (ImGui::Checkbox("Infinite HP", &st->infinite_hp))
        st->cheats_changed = true;
    hover_tooltip("Restore each occupied party slot's current HP from its live "
                  "maximum at VBlank.");
    if (ImGui::Checkbox("Infinite PP", &st->infinite_pp))
        st->cheats_changed = true;
    hover_tooltip("Restore each occupied party slot's current PP from its live "
                  "maximum at VBlank.");
    static const char kWalkingSpeedItems[] = "Normal\0" "2x\0" "3x\0";
    st->player_walk_run_speed_multiplier = std::clamp(
        st->player_walk_run_speed_multiplier, 1, 3);
    int walking_speed_index = st->player_walk_run_speed_multiplier - 1;
    if (ImGui::Combo("Walking speed", &walking_speed_index,
                     kWalkingSpeedItems)) {
        st->player_walk_run_speed_multiplier = walking_speed_index + 1;
        st->cheats_changed = true;
    }
    hover_tooltip("Cheat/mod: scales only the measured player walking and "
                  "B-running speed limits. Gap jumps, battles, NPCs, menus, "
                  "and global emulation timing are unchanged. Normal is the "
                  "faithful default.");
}

void draw_logging_page(ConfigUiState* st) {
    ImGui::SeparatorText("Diagnostics");
    help_marker("Controls extra behind-the-scenes logging for bug reports. "
                "Leave it off for normal play because it adds host overhead.");
    if (ImGui::Checkbox("Additional debug logging",
                        &st->additional_debug_logging))
        st->logging_changed = true;
    hover_tooltip("Adds detailed runtime information to logs/session_*.log. "
                  "The newest 20 logs are kept. Crash reports are written "
                  "regardless of this setting.");
    if (ImGui::Checkbox("Debug overlay", &st->debug_overlay))
        st->debug_overlay_changed = true;
    hover_tooltip("Show selected cached test and audio status variables over "
                  "gameplay. It never reads guest memory and is off by default.");
    ImGui::TextDisabled(
        "Some detailed streams start only at launch; changes take full effect "
        "the next time you launch the game.");
}

void draw_debug_overlay(const ConfigUiState* st) {
    if (!st || !st->debug_overlay || g_visible) return;
    const DebugOverlayState& d = st->debug_overlay_state;
    const bool any_test = d.self_heal_ram || d.cost_probe ||
        d.present_cadence || d.ram_churn_probe ||
        d.additional_debug_logging;
    const bool any_audio = d.audio_status_available ||
        d.native_mp2k_requested || d.native_mp2k_live ||
        d.turbo_audio_requested || d.turbo_audio_decoupled ||
        d.turbo_audio_pending || d.turbo_audio_fallback || d.turbo_audio_muted ||
        d.turbo_audio_uncapped;

    // Keep this in the upper-right during gameplay. NoInputs makes it
    // informational only: mouse, keyboard, and controller focus stay in the
    // game. Size/position clamps keep tiny test windows fully covered.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float max_w = viewport->WorkSize.x > 16.0f
        ? viewport->WorkSize.x - 16.0f : viewport->WorkSize.x;
    const float max_h = viewport->WorkSize.y > 16.0f
        ? viewport->WorkSize.y - 16.0f : viewport->WorkSize.y;
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - 8.0f,
               viewport->WorkPos.y + 8.0f),
        ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f),
                                        ImVec2(max_w, max_h));
    ImGui::SetNextWindowBgAlpha(0.76f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin("##DebugOverlay", nullptr, kFlags)) {
        const ImVec2 pos = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        const float max_x = viewport->WorkPos.x + viewport->WorkSize.x - size.x;
        const float max_y = viewport->WorkPos.y + viewport->WorkSize.y - size.y;
        const float clamped_x = std::clamp(pos.x, viewport->WorkPos.x, max_x);
        const float clamped_y = std::clamp(pos.y, viewport->WorkPos.y, max_y);
        if (clamped_x != pos.x || clamped_y != pos.y)
            ImGui::SetWindowPos(ImVec2(clamped_x, clamped_y));
        if (any_test) {
            ImGui::TextDisabled("Test Variables");
            if (d.self_heal_ram) ImGui::TextUnformatted("Self-heal RAM: on");
            if (d.cost_probe) ImGui::TextUnformatted("Cost Probe: on");
            if (d.present_cadence) ImGui::TextUnformatted("Present Cadence: on");
            if (d.ram_churn_probe) ImGui::TextUnformatted("RAM Churn Probe: on");
            if (d.additional_debug_logging)
                ImGui::TextUnformatted("Additional Debug Logging: on");
        }
        if (any_audio) {
            if (any_test) ImGui::Spacing();
            ImGui::TextDisabled("Audio Variables");
            ImGui::Text("Audio Engine: %s",
                        d.native_output_selected ? "Native MP2K" : "GBA");
            if (d.native_mp2k_live)
                ImGui::TextUnformatted("Native MP2K: live");
            else if (d.native_mp2k_requested)
                ImGui::TextUnformatted("Native MP2K: requested");
            if (d.native_mp2k_requested && !d.native_mp2k_live &&
                d.native_mp2k_reason[0] != '\0') {
                ImGui::Text("Native requested: %s", d.native_mp2k_reason);
            }
            if (d.turbo_audio_requested || d.turbo_audio_decoupled ||
                d.turbo_audio_pending || d.turbo_audio_fallback) {
                const char* outcome = d.turbo_audio_decoupled ? "decoupled" :
                    d.turbo_audio_pending ? "pending" :
                    d.turbo_audio_fallback ? "fallback" : "coupled";
                ImGui::Text("Turbo audio: %s", outcome);
            }
            if (d.turbo_audio_muted)
                ImGui::TextUnformatted("Mute During Turbo: on");
            if (d.turbo_audio_uncapped)
                ImGui::TextUnformatted("Uncapped Turbo: on");
            if (d.turbo_audio_fallback && d.turbo_audio_reason[0] != '\0')
                ImGui::Text("Reason: %s", d.turbo_audio_reason);
        }
        if (!any_test && !any_audio)
            ImGui::TextDisabled("None");
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

}  // namespace

// UI-03: a conservative modernization pass over the stock ImGui dark theme —
// rounded corners, roomier padding, and one coherent accent color reused for
// every interactive element (buttons, tabs, headers, sliders). No layout or
// binding-logic change; every widget call in this file is unaffected. Legible
// at the window's default scale, same font (no network fonts, no new deps).
void apply_modern_style() {
    ImGuiStyle& style = ImGui::GetStyle();

    style.WindowRounding    = 6.0f;
    style.ChildRounding     = 6.0f;
    style.FrameRounding     = 4.0f;
    style.PopupRounding     = 4.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding      = 4.0f;
    style.TabRounding       = 4.0f;

    style.WindowPadding     = ImVec2(12.0f, 10.0f);
    style.FramePadding      = ImVec2(8.0f, 5.0f);
    style.CellPadding       = ImVec2(6.0f, 4.0f);
    style.ItemSpacing       = ImVec2(8.0f, 6.0f);
    style.ItemInnerSpacing  = ImVec2(6.0f, 4.0f);
    style.IndentSpacing     = 18.0f;
    style.ScrollbarSize     = 14.0f;
    style.GrabMinSize       = 10.0f;

    // One accent (a muted teal) reused for every interactive/highlight
    // state, over the stock dark grays — keeps contrast and legibility
    // rather than introducing a whole new palette.
    const ImVec4 accent      (0.20f, 0.55f, 0.60f, 1.00f);
    const ImVec4 accent_hover(0.25f, 0.65f, 0.70f, 1.00f);
    const ImVec4 accent_active(0.16f, 0.45f, 0.50f, 1.00f);
    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg]         = ImVec4(0.10f, 0.11f, 0.12f, 0.98f);
    c[ImGuiCol_Header]           = ImVec4(accent.x, accent.y, accent.z, 0.45f);
    c[ImGuiCol_HeaderHovered]    = ImVec4(accent_hover.x, accent_hover.y, accent_hover.z, 0.65f);
    c[ImGuiCol_HeaderActive]     = ImVec4(accent_active.x, accent_active.y, accent_active.z, 0.80f);
    c[ImGuiCol_Button]           = ImVec4(0.20f, 0.21f, 0.23f, 1.00f);
    c[ImGuiCol_ButtonHovered]    = accent_hover;
    c[ImGuiCol_ButtonActive]     = accent_active;
    c[ImGuiCol_FrameBg]          = ImVec4(0.16f, 0.17f, 0.19f, 1.00f);
    c[ImGuiCol_FrameBgHovered]   = ImVec4(0.20f, 0.24f, 0.26f, 1.00f);
    c[ImGuiCol_FrameBgActive]    = ImVec4(0.20f, 0.28f, 0.30f, 1.00f);
    c[ImGuiCol_CheckMark]        = accent_hover;
    c[ImGuiCol_SliderGrab]       = accent;
    c[ImGuiCol_SliderGrabActive] = accent_hover;
    c[ImGuiCol_Tab]              = ImVec4(0.14f, 0.15f, 0.16f, 1.00f);
    c[ImGuiCol_TabHovered]       = accent_hover;
    c[ImGuiCol_TabActive]        = accent_active;
    c[ImGuiCol_TabUnfocused]     = c[ImGuiCol_Tab];
    c[ImGuiCol_TabUnfocusedActive] = accent_active;
    c[ImGuiCol_TitleBgActive]    = ImVec4(0.14f, 0.15f, 0.16f, 1.00f);
    c[ImGuiCol_MenuBarBg]        = ImVec4(0.12f, 0.13f, 0.14f, 1.00f);
    c[ImGuiCol_Separator]        = ImVec4(0.30f, 0.31f, 0.33f, 1.00f);
    c[ImGuiCol_SeparatorHovered] = accent_hover;
    c[ImGuiCol_TableHeaderBg]    = ImVec4(0.16f, 0.17f, 0.19f, 1.00f);
    c[ImGuiCol_TableBorderStrong] = ImVec4(0.30f, 0.31f, 0.33f, 1.00f);
    c[ImGuiCol_TableBorderLight]  = ImVec4(0.22f, 0.23f, 0.25f, 1.00f);
}

bool config_ui_init(SDL_Window* window, SDL_Renderer* renderer,
                     bool use_opengl) {
    if (g_ready || !window || !renderer) return g_ready;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;   // we persist settings ourselves, not via imgui.ini
    // Keep the F1 menu usable without a mouse: the sidebar and controls are
    // ordinary ImGui navigation items, and the SDL2 backend supplies both
    // keyboard and controller navigation inputs when enabled.
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable |
                      ImGuiConfigFlags_NavEnableKeyboard |
                      ImGuiConfigFlags_NavEnableGamepad;
    if (use_opengl) {
        // Real multi-viewport: torn-off panels become their own OS windows,
        // each an SDL window + GL context ImGui creates/destroys itself via
        // the platform backend below.
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    }
    ImGui::StyleColorsDark();
    apply_modern_style();
    bool backend_ok;
    if (use_opengl) {
        backend_ok = ImGui_ImplSDL2_InitForOpenGL(window,
                                                   SDL_GL_GetCurrentContext());
        if (backend_ok) backend_ok = ImGui_ImplOpenGL3_Init("#version 130");
    } else {
        backend_ok = ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
        if (backend_ok) backend_ok = ImGui_ImplSDLRenderer2_Init(renderer);
    }
    if (!backend_ok) {
        ImGui::DestroyContext();
        return false;
    }
    g_renderer = renderer;
    g_use_opengl = use_opengl;
    g_window = window;
    // SHUTDOWN-FREEZE-01: the context ImGui_ImplSDL2_InitForOpenGL() was
    // just wired to above is the MAIN window's context — capture it now
    // while it is known-current, so shutdown can restore it regardless of
    // what a torn-off viewport left current by then.
    g_gl_context = use_opengl ? SDL_GL_GetCurrentContext() : nullptr;
    g_config_window_recover = true;
    g_ready = true;
    return true;
}

void config_ui_shutdown() {
    if (!g_ready) return;
    if (g_use_opengl) {
        // A torn-off viewport's GL context may be current right now (ImGui
        // switches contexts per-viewport while drawing/updating). Restore
        // the MAIN window's context first so ImGui_ImplOpenGL3_Shutdown()
        // deletes its GL objects against the context that actually owns
        // them, and so DestroyPlatformWindows() below tears down every
        // detached viewport window+context while the main one is still
        // alive underneath it.
        if (g_window && g_gl_context) {
            SDL_GL_MakeCurrent(g_window, g_gl_context);
        }
        if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            ImGui::DestroyPlatformWindows();
        }
        ImGui_ImplOpenGL3_Shutdown();
    } else {
        ImGui_ImplSDLRenderer2_Shutdown();
    }
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    g_renderer = nullptr;
    g_window = nullptr;
    g_gl_context = nullptr;
    g_use_opengl = false;
    g_config_window_recover = false;
    g_ready = false;
}

bool config_ui_visible() { return g_ready && g_visible; }

void config_ui_set_visible(bool on) {
    if (!g_ready) return;
    if (on && !g_visible) g_config_window_recover = true;
    g_visible = on;
    if (!on) g_capture = Capture::None;
}

void config_ui_toggle() { config_ui_set_visible(!g_visible); }

bool config_ui_handle_event(const SDL_Event* e) {
    if (!g_ready || !e) return false;

    // A live capture outranks everything: the very next key or pad press is
    // the binding and must not reach ImGui (which would treat it as UI input)
    // or the guest (which would treat it as gameplay).
    if (g_visible && g_capture != Capture::None) {
        if (e->type == SDL_KEYDOWN && e->key.repeat == 0) return true;
        if (e->type == SDL_CONTROLLERBUTTONDOWN) return true;
    }

    ImGui_ImplSDL2_ProcessEvent(const_cast<SDL_Event*>(e));

    if (!g_visible) {
        // The F1 menu itself is closed, but game-owned extra content (e.g. a
        // function-call tracer window) may still want the keyboard -- see
        // g_config_ui_extra_wants_keyboard (host_config_ui.h). ImGui already
        // has the event via ProcessEvent above; consume it here too so the
        // host doesn't ALSO feed it to guest keyinput or a hotkey.
        if (!g_config_ui_extra_wants_keyboard || !g_config_ui_extra_wants_keyboard())
            return false;
        switch (e->type) {
            case SDL_KEYDOWN: case SDL_KEYUP: case SDL_TEXTINPUT:
                return true;
            default:
                return false;
        }
    }

    // While visible the menu owns the mouse and keyboard. Window/quit events
    // still belong to the host.
    const ImGuiIO& io = ImGui::GetIO();
    switch (e->type) {
        case SDL_MOUSEMOTION: case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: case SDL_MOUSEWHEEL:
            return io.WantCaptureMouse;
        case SDL_KEYDOWN: case SDL_KEYUP: case SDL_TEXTINPUT:
            return true;
        default:
            return false;
    }
}

// Consume a raw key/pad press into whichever bind box is armed. Called by
// host_window before it does anything else with the event.
bool config_ui_capture_key(ConfigUiState* st, int scancode, int keycode,
                           unsigned mods) {
    if (!g_visible || g_capture == Capture::None) return false;
    if (keycode == SDLK_ESCAPE) { g_capture = Capture::None; return true; }
    if (g_capture == Capture::Key && g_capture_index >= 0) {
        for (int i = 0; i < 10; ++i)
            if (i != g_capture_index && st->key_bind[i] == scancode)
                st->key_bind[i] = 0;   // move, never duplicate
        st->key_bind[g_capture_index] = scancode;
        st->binds_changed = true;
    } else if (g_capture == Capture::Hotkey && g_capture_index >= 0) {
        st->hotkey_key[g_capture_index] = keycode;
        st->hotkey_mods[g_capture_index] =
            mods & (KMOD_CTRL | KMOD_ALT | KMOD_SHIFT);
        st->binds_changed = true;
    } else {
        return false;   // a pad box is armed; a key does not satisfy it
    }
    g_capture = Capture::None;
    return true;
}

bool config_ui_capture_pad(ConfigUiState* st, int button) {
    if (!g_visible || g_capture_index < 0) return false;
    // UI-02b: L2/R2's synthetic ids are hotkey-only (out of scope for
    // gameplay input — see UI-02b task notes). A trigger pull while the
    // Controller tab's gameplay pad box is armed is simply not consumed,
    // same as pressing an unrelated key would be; gameplay binding stays
    // exactly as before this change.
    const bool is_trigger = (button == kPadTriggerLeft || button == kPadTriggerRight);
    if (g_capture == Capture::Pad) {
        if (is_trigger) return false;
        for (int i = 0; i < 10; ++i)
            if (i != g_capture_index && st->pad_bind[i] == button)
                st->pad_bind[i] = -1;
        st->pad_bind[g_capture_index] = button;
    } else if (g_capture == Capture::HotkeyPad) {
        // UI-02: a hotkey's controller button, unlike the gameplay pad
        // above, is not deduped against the other hotkey rows — Turbo Held
        // and Turbo Toggle sharing one button, for instance, is a legitimate
        // (if unusual) choice, not a conflict to silently resolve.
        st->hotkey_pad[g_capture_index] = button;
    } else {
        return false;
    }
    st->binds_changed = true;
    g_capture = Capture::None;
    return true;
}

void config_ui_draw(ConfigUiState* st) {
    if (!g_ready || !st) return;

    if (g_use_opengl) {
        ImGui_ImplOpenGL3_NewFrame();
    } else {
        ImGui_ImplSDLRenderer2_NewFrame();
    }
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    if (g_visible) {
        // Dockspace over the whole main viewport so panels can dock inside
        // the main window too, not just tear off into their own.
        // PassthruCentralNode keeps the central node fully transparent so
        // the guest picture — already copied into the renderer this frame —
        // stays visible through it. Scoped to g_visible like every other
        // ImGui window this file draws: while the menu is closed, no ImGui
        // window (dockspace host included) exists to capture input the
        // guest is supposed to see.
        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(),
                                     ImGuiDockNodeFlags_PassthruCentralNode);
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Close menu", "F1")) g_visible = false;
                ImGui::Separator();
                if (ImGui::MenuItem("Quit")) st->request_quit = true;
                ImGui::EndMenu();
            }
            ImGui::TextDisabled("|  F1 closes  |  %.0f fps  |  %.0f%% speed",
                                st->fps, st->emulation_speed_percent);
            ImGui::EndMainMenuBar();
        }

        // Clamp to the window: at scale 1 the host window is 240x160, and an
        // unclamped panel would open almost entirely offscreen with no way
        // to drag it back.
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const ImVec2 work_pos = viewport->WorkPos;
        const ImVec2 work_size = viewport->WorkSize;
        const float max_w = std::max(1.0f, work_size.x);
        const float max_h = std::max(1.0f, work_size.y);
        const float pane_w = std::min(700.0f, max_w);
        const float pane_h = std::min(520.0f, max_h);
        const float pane_x = work_pos.x +
            std::min(40.0f, std::max(0.0f, max_w - pane_w));
        const float pane_y = work_pos.y +
            std::min(46.0f, std::max(0.0f, max_h - pane_h));
        // UI-03: raised from 620x460 — the old default was cramped enough on
        // modest host windows that a user recovering from an accidental
        // shrink (see the size-constraint comment below) landed right back
        // in a tight fit.
        ImGui::SetNextWindowSize(ImVec2(pane_w, pane_h), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(pane_x, pane_y), ImGuiCond_FirstUseEver);
        // UI-03: floor the resize range. Proven mechanism (headless drag
        // probe against this exact window): with no constraint, ImGui lets
        // a corner-grip drag shrink this panel all the way down to its bare
        // style.WindowMinSize (32x32) — reproduced exactly, no coordinate
        // bug involved. Below roughly the Hotkeys tab's own content width
        // (~370-400px for its 5-column table, measured headless) the grip
        // itself becomes a sliver of the whole window, so a user trying to
        // grab it to resize back out overwhelmingly grabs the title bar
        // instead and just moves the window — which reads as "resizing is
        // broken; it won't leave the minimum no matter where I click." The
        // floor below keeps the panel always big enough to show that table
        // and stay grabbable. No maximum: the user can still grow it freely.
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(std::min(480.0f, max_w), std::min(360.0f, max_h)),
            ImVec2(FLT_MAX, FLT_MAX));
        if (ImGui::Begin("Configuration", nullptr)) {
            const ImVec2 current_pos = ImGui::GetWindowPos();
            const ImVec2 current_size = ImGui::GetWindowSize();
            // Recover only on first open, an invalid saved/current rect, or
            // when the current viewport leaves the panel completely outside
            // its work area. A normal drag or resize stays untouched; a
            // detached ImGui viewport uses its own work area here.
            const ImGuiViewport* window_viewport = ImGui::GetWindowViewport();
            const ImGuiViewport* bounds_viewport = window_viewport
                ? window_viewport : viewport;
            const ImVec2 bounds_pos = bounds_viewport->WorkPos;
            const ImVec2 bounds_size = bounds_viewport->WorkSize;
            const float bounds_right = bounds_pos.x + bounds_size.x;
            const float bounds_bottom = bounds_pos.y + bounds_size.y;
            const bool invalid_rect =
                !std::isfinite(current_pos.x) ||
                !std::isfinite(current_pos.y) ||
                !std::isfinite(current_size.x) ||
                !std::isfinite(current_size.y) ||
                current_size.x <= 0.0f || current_size.y <= 0.0f;
            const bool entirely_offscreen =
                current_pos.x + current_size.x <= bounds_pos.x ||
                current_pos.y + current_size.y <= bounds_pos.y ||
                current_pos.x >= bounds_right ||
                current_pos.y >= bounds_bottom;
            if (g_config_window_recover || ImGui::IsWindowAppearing() ||
                invalid_rect || entirely_offscreen) {
                const float max_x = std::max(bounds_pos.x,
                    bounds_right - current_size.x);
                const float max_y = std::max(bounds_pos.y,
                    bounds_bottom - current_size.y);
                const ImVec2 recovered_pos = invalid_rect
                    ? bounds_pos
                    : ImVec2(std::clamp(current_pos.x, bounds_pos.x, max_x),
                             std::clamp(current_pos.y, bounds_pos.y, max_y));
                ImGui::SetWindowPos(recovered_pos);
                g_config_window_recover = false;
            }
            // Lighthouse-style navigation: stable top-level categories, then
            // a compact page sidebar and a scrollable content pane. Page IDs
            // persist across menu closes/category switches; enhancement IDs
            // keep old meanings so removed pages fall back safely.
            enum : int {
                kSettings = 0, kEnhancements = 1, kCheats = 2,
                kDeveloper = 3, kCategoryCount = 4,
            };
            struct PageEntry { int id; const char* name; };
            static int category = kSettings;
            static int settings_page = 0;
            static int enhancements_page = 0;
            static int cheats_page = 0;
            static int developer_page = 0;

            static const char* const categories[kCategoryCount] = {
                "Settings", "Enhancements", "Cheats", "Developer Tools",
            };
            static const PageEntry settings_pages[] = {
                {0, "Video"}, {1, "Audio"}, {2, "Controls"},
                {3, "Hotkeys"}, {4, "Speed"},
            };
            // Legacy IDs: 0=Timing, 1=Performance, 2=Visual,
            // 3=Native Rendering. Performance merges into ID 0; removed
            // IDs 1 and 3 therefore clamp to the first visible page.
            static const PageEntry enhancement_pages[] = {
                {0, "Timing & Performance"}, {2, "Visual"},
            };
            static const PageEntry cheat_pages[] = { {0, "Player"} };
            static const PageEntry developer_pages[] = { {0, "Logging"} };
            int* page = &settings_page;
            const PageEntry* page_entries = settings_pages;
            int page_count = static_cast<int>(sizeof(settings_pages) /
                                              sizeof(settings_pages[0]));
            if (category < 0 || category >= kCategoryCount)
                category = kSettings;
            if (category == kEnhancements) {
                page = &enhancements_page;
                page_entries = enhancement_pages;
                page_count = static_cast<int>(sizeof(enhancement_pages) /
                                              sizeof(enhancement_pages[0]));
            } else if (category == kCheats) {
                page = &cheats_page;
                page_entries = cheat_pages;
                page_count = static_cast<int>(sizeof(cheat_pages) /
                                              sizeof(cheat_pages[0]));
            } else if (category == kDeveloper) {
                page = &developer_page;
                page_entries = developer_pages;
                page_count = static_cast<int>(sizeof(developer_pages) /
                                              sizeof(developer_pages[0]));
            }
            bool page_valid = false;
            for (int i = 0; i < page_count; ++i)
                page_valid = page_valid || *page == page_entries[i].id;
            if (!page_valid) *page = page_entries[0].id;

            ImGui::BeginChild("ConfigCategories", ImVec2(0.0f, 38.0f), false,
                              ImGuiWindowFlags_NoScrollbar);
            for (int i = 0; i < kCategoryCount; ++i) {
                if (i != 0) ImGui::SameLine();
                ImGui::PushID(i);
                const float width = ImGui::CalcTextSize(categories[i]).x + 24.0f;
                if (ImGui::Selectable(categories[i], category == i, 0,
                                       ImVec2(width, 30.0f)))
                    category = i;
                ImGui::PopID();
            }
            ImGui::EndChild();
            ImGui::Separator();

            ImGui::BeginChild("ConfigBody", ImVec2(0.0f, 0.0f), false);
            if (category != kCheats) {
                ImGui::BeginChild("ConfigSidebar", ImVec2(168.0f, 0.0f), true);
                for (int i = 0; i < page_count; ++i) {
                    ImGui::PushID(page_entries[i].id);
                    if (ImGui::Selectable(page_entries[i].name,
                                          *page == page_entries[i].id,
                                          ImGuiSelectableFlags_SpanAllColumns,
                                          ImVec2(0.0f, 30.0f)))
                        *page = page_entries[i].id;
                    ImGui::PopID();
                }
                ImGui::EndChild();
                ImGui::SameLine();
            }
            ImGui::BeginChild("ConfigContent", ImVec2(0.0f, 0.0f), true);
            if (category == kSettings) {
                switch (*page) {
                    case 0: draw_video_page(st); break;
                    case 1: draw_audio_page(st); break;
                    case 2: draw_controls_page(st); break;
                    case 3: draw_hotkeys_page(st); break;
                    default: draw_turbo_page(st); break;
                }
            } else if (category == kEnhancements) {
                switch (*page) {
                    case 2: draw_visual_page(st); break;
                    case 0: default: draw_timing_performance_page(st); break;
                }
            } else if (category == kCheats) {
                draw_cheats_player_page(st);
            } else {
                draw_logging_page(st);
            }
            ImGui::EndChild();
            ImGui::EndChild();
        }
        ImGui::End();
    }

    // Unlike the F1 configuration window, this stays visible during normal
    // gameplay whenever the persisted Debug overlay switch is on.
    draw_debug_overlay(st);

    // Game-owned extra ImGui content (see host_config_ui.h). Drawn every
    // frame, independent of g_visible, same as the debug overlay above.
    if (g_config_ui_extra_draw) g_config_ui_extra_draw();

    ImGui::Render();
    if (g_use_opengl) {
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        // Multi-viewport: render each torn-off panel into its own OS
        // window/GL context. SDL_Renderer's opengl backend is left holding
        // whatever context was current before this call — save it and put
        // it back so the caller's own SDL_RenderPresent (immediately after
        // config_ui_draw returns) targets the right window/context again.
        if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            SDL_Window* backup_window = SDL_GL_GetCurrentWindow();
            SDL_GLContext backup_context = SDL_GL_GetCurrentContext();
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            SDL_GL_MakeCurrent(backup_window, backup_context);
        }
    } else {
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), g_renderer);
    }
}

}  // namespace gbarecomp

#else  // !GBARECOMP_HAVE_IMGUI — no-op stubs

namespace gbarecomp {
bool config_ui_init(SDL_Window*, SDL_Renderer*, bool) { return false; }
void config_ui_shutdown() {}
bool config_ui_handle_event(const SDL_Event*) { return false; }
bool config_ui_visible() { return false; }
void config_ui_toggle() {}
void config_ui_set_visible(bool) {}
bool config_ui_capture_key(ConfigUiState*, int, int, unsigned) { return false; }
bool config_ui_capture_pad(ConfigUiState*, int) { return false; }
void config_ui_draw(ConfigUiState*) {}
}  // namespace gbarecomp

#endif
