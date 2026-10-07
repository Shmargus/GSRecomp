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
#include <iterator>

#include "imgui.h"
#include "imgui_internal.h"
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
    // UI-02b: L2/R2 and the stick directions are synthetic ids, not a real
    // SDL_GameControllerButton — label them from the shared name table
    // ("lefttrigger", "leftstickup", ...), matching the lowercase-no-separator
    // convention every other row in this column already uses
    // (SDL_GameControllerGetStringForButton output, e.g. "leftshoulder", "dpup").
    if (const char* s = pad_synth_name(button)) return s;
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

// Golden Sun menu palette: deep blue windows, gold trim and highlights,
// parchment text. Shared by the style below and the page headings.
const ImVec4 kGold      (0.93f, 0.76f, 0.30f, 1.00f);
const ImVec4 kGoldDim   (0.70f, 0.55f, 0.20f, 1.00f);
const ImVec4 kParchment (0.96f, 0.93f, 0.84f, 1.00f);
const ImVec4 kMutedText (0.66f, 0.71f, 0.84f, 1.00f);

// A one-line explanation under a control, always visible so nothing needs
// hovering to be understood.
void caption(const char* text) {
    ImGui::Indent(4.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, kMutedText);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::Unindent(4.0f);
    ImGui::Spacing();
}

void section(const char* title) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, kGold);
    ImGui::SeparatorText(title);
    ImGui::PopStyleColor();
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

// D-pad Left/Right change a nav-focused (not yet activated) slider directly.
// Call right after the slider. Returns -1 / +1 per press (with key repeat),
// else 0. The keys are claimed so nav does not also move focus sideways.
int dpad_slider_step() {
    if (!ImGui::IsItemFocused() || ImGui::IsItemActive()) return 0;
    const ImGuiID id = ImGui::GetItemID();
    ImGui::SetKeyOwner(ImGuiKey_GamepadDpadLeft, id);
    ImGui::SetKeyOwner(ImGuiKey_GamepadDpadRight, id);
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, ImGuiInputFlags_Repeat, id))
        return -1;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, ImGuiInputFlags_Repeat, id))
        return 1;
    return 0;
}

void draw_controls_page(ConfigUiState* st) {
    section("Buttons");
    caption("Click a box, then press the key, controller button or stick direction to use. "
            "Esc cancels.");

    ImGui::BeginChild("controls_bindings_scroll",
                      ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing() * 1.5f),
                      ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_HorizontalScrollbar);
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

    if (ImGui::Button("Restore defaults")) {
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
}

void draw_hotkeys_page(ConfigUiState* st) {
    section("Hotkeys");
    caption("Shortcuts that work while playing. Each can have a key, a "
            "controller button, or both. Auto Fire presses A or B rapidly "
            "while held.");
    ImGui::BeginChild("hotkeys_scroll",
                      ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing()),
                      ImGuiChildFlags_NavFlattened,
                      ImGuiWindowFlags_HorizontalScrollbar);
    if (ImGui::BeginTable("hotkeys", 5,
                          ImGuiTableFlags_SizingFixedFit |
                          ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 150);
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
    section("Window");
    // The slider edits a menu-local copy. host_window republishes st->scale
    // from the live window every frame, which used to overwrite the dragged
    // value before release, so the resize never happened. The copy follows
    // the window only while the slider is idle, and the (expensive) resize
    // is requested once, when the slider is released.
    static int pending_scale = 0;
    static bool scale_active = false;
    const int max_scale = std::clamp(st->max_scale, 1, 8);
    if (!scale_active) pending_scale = st->scale;
    pending_scale = std::clamp(pending_scale, 1, max_scale);
    if (st->fullscreen) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(220.0f);
    ImGui::SliderInt("Window size", &pending_scale, 1, max_scale, "%dx");
    if (!st->fullscreen) {
        const int dir = dpad_slider_step();
        if (dir != 0) {
            pending_scale = std::clamp(pending_scale + dir, 1, max_scale);
            if (pending_scale != st->scale) {
                st->scale = pending_scale;
                st->video_changed = true;
            }
        }
    }
    scale_active = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivatedAfterEdit() && pending_scale != st->scale) {
        st->scale = pending_scale;
        st->video_changed = true;
    }
    if (st->fullscreen) ImGui::EndDisabled();
    caption(st->fullscreen
        ? "Leave fullscreen to change the window size."
        : "Size of the game window. Applied when you let go of the slider.");
    if (ImGui::Checkbox("Fullscreen", &st->fullscreen)) st->video_changed = true;

    section("Picture");
    if (ImGui::Checkbox("Sharp pixels", &st->integer_scale))
        st->video_changed = true;
    caption("Keeps every pixel the same size. Off stretches the picture to "
            "fill the window.");
    if (ImGui::Checkbox("V-Sync", &st->vsync)) st->video_changed = true;
    caption("Matches the monitor's refresh to prevent tearing.");
    // The menu lists only these; screen_kind is a runtime::ScreenKind
    // (color_lut.h). The GBA screen models stay reachable through
    // GBARECOMP_SCREEN but are not offered here.
    static const char kScreenItems[] =
        "Raw (as the game draws it)\0"
        "Handheld (muted, warm)\0"
        "Handheld (lighter)\0"
        "Soft\0"
        "Natural\0"
        "Warm\0"
        "Deep\0"
        "Custom\0";
    static constexpr int kScreenKinds[] = {0, 5, 6, 7, 8, 9, 10, 11};
    int choice = 0;
    for (int i = 0; i < static_cast<int>(std::size(kScreenKinds)); ++i)
        if (kScreenKinds[i] == st->screen_kind) choice = i;
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::Combo("Colours", &choice, kScreenItems)) {
        st->screen_kind = kScreenKinds[choice];
        st->video_changed = true;
    }
    caption("The game's colours were made bright for the dark original GBA "
            "screen. Handheld tones them down like a modern handheld's GBA "
            "mode; Soft, Natural, Warm and Deep calm them less. Custom lets "
            "you set them yourself.");
    if (st->screen_kind == 11) {
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::SliderInt("Saturation", &st->color_saturation, 0, 200,
                             "%d%%"))
            st->video_changed = true;
        if (ImGui::IsItemDeactivatedAfterEdit()) st->color_save = true;
        if (const int dir = dpad_slider_step()) {
            const int v = std::clamp(st->color_saturation + dir, 0, 200);
            if (v != st->color_saturation) {
                st->color_saturation = v;
                st->video_changed = true;
                st->color_save = true;
            }
        }
        caption("100% is the game's own colours, 0% is black and white.");
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::SliderInt("Hue", &st->color_hue, -180, 180, "%d"))
            st->video_changed = true;
        if (ImGui::IsItemDeactivatedAfterEdit()) st->color_save = true;
        if (const int dir = dpad_slider_step()) {
            const int v = std::clamp(st->color_hue + dir, -180, 180);
            if (v != st->color_hue) {
                st->color_hue = v;
                st->video_changed = true;
                st->color_save = true;
            }
        }
        caption("Turns every colour around the colour wheel. 0 is the "
                "game's own.");
    }

    section("Performance counter");
    if (ImGui::Checkbox("Show FPS", &st->show_fps)) st->video_changed = true;
    caption("Shows frames drawn per second and game speed in the corner of "
            "the screen. 100% is full speed.");
}

void draw_audio_page(ConfigUiState* st) {
    section("Sound");
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::SliderInt("Volume", &st->volume, 0, 100, "%d%%"))
        st->audio_changed = true;
    if (const int dir = dpad_slider_step()) {
        const int v = std::clamp(st->volume + dir, 0, 100);
        if (v != st->volume) {
            st->volume = v;
            st->audio_changed = true;
        }
    }
    if (ImGui::Checkbox("Mute", &st->mute)) st->audio_changed = true;
}

void draw_turbo_page(ConfigUiState* st) {
    section("Fast Forward");
    caption("Set the Fast Forward buttons on the Hotkeys page. Hold runs "
            "fast while pressed; Toggle switches it on and off.");
    ImGui::SetNextItemWidth(220.0f);
    ImGui::SliderFloat("Speed", &st->turbo_multiplier, 1.0f,
                       kMaxTurboMultiplier, "%.1fx");
    if (ImGui::IsItemDeactivatedAfterEdit()) st->speed_changed = true;
    if (const int dir = dpad_slider_step()) {
        // 0.1 is ImGui's own controller step for a "%.1f" float slider.
        const float v = std::clamp(st->turbo_multiplier + 0.1f * dir, 1.0f,
                                   static_cast<float>(kMaxTurboMultiplier));
        if (v != st->turbo_multiplier) {
            st->turbo_multiplier = v;
            st->speed_changed = true;
        }
    }
    if (ImGui::Checkbox("As fast as possible", &st->uncapped))
        st->speed_changed = true;
    caption("Ignores the speed above and runs as fast as your PC allows.");
    if (ImGui::Checkbox("Mute while fast forwarding", &st->mute_during_turbo))
        st->speed_changed = true;
}

void draw_enhancements_page(ConfigUiState* st) {
    section("Flicker");
    // VFX-FLICKER-02: selective temporal blend ("LCD ghosting"). Legacy
    // config values 4..6 selected whole-frame modes that are no longer
    // offered; migrate them explicitly through the normal change path so an
    // old file cannot keep a hidden mode active. Greyed, not hidden, while
    // 2x scene interpolation is on: blending across its synthetic midpoint
    // frame would smear the picture (see host_window.cpp's present()).
    static const char kTemporalBlendItems[] =
        "Off\0" "Light\0" "Medium\0" "Strong\0";
    if (st->temporal_blend_index >= 4) {
        st->temporal_blend_index = 3;
        st->enhancements_changed = true;
    }
    if (st->temporal_blend_index < 0 || st->temporal_blend_index > 3)
        st->temporal_blend_index = gbarecomp::kTemporalBlendDefaultIndex;
    const bool blocked =
        st->frame_interpolation_available && st->frame_interpolation_2x;
    if (blocked) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::Combo("Flicker reduction", &st->temporal_blend_index,
                     kTemporalBlendItems))
        st->enhancements_changed = true;
    if (blocked) ImGui::EndDisabled();
    caption(blocked
        ? "Unavailable in the current presentation mode."
        : "Smooths effects that flash every other frame, such as barriers "
          "and the world map, the way the GBA screen did. Normal movement "
          "stays sharp.");

    section("Troubleshooting");
    if (ImGui::Checkbox("Crash log", &st->crash_log))
        st->logging_changed = true;
    caption("Records what the game was doing, so a crash or freeze report "
            "shows where it happened. Slows the game down; turn it on only "
            "if you are hunting a crash.");
}

void draw_fps_overlay(const ConfigUiState* st) {
    if (!st || !st->show_fps) return;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + 8.0f,
                                   viewport->WorkPos.y + 8.0f),
                            ImGuiCond_Always);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::SetNextWindowBgAlpha(0.70f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 4.0f));
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin("##FpsOverlay", nullptr, kFlags)) {
        ImGui::TextColored(kGold, "%.0f FPS", st->fps);
        ImGui::SameLine();
        ImGui::TextColored(kParchment, "%.0f%% speed",
                           st->emulation_speed_percent);
    }
    ImGui::End();
    ImGui::PopStyleVar();
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

// Golden Sun look: deep blue menu windows with gold trim, like the game's
// own menus, and parchment text. Rounded, roomy, same built-in font.
void apply_modern_style() {
    ImGuiStyle& style = ImGui::GetStyle();

    style.WindowRounding    = 8.0f;
    style.ChildRounding     = 6.0f;
    style.FrameRounding     = 5.0f;
    style.PopupRounding     = 5.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding      = 5.0f;
    style.TabRounding       = 5.0f;
    style.WindowBorderSize  = 2.0f;
    style.ChildBorderSize   = 1.0f;
    style.FrameBorderSize   = 1.0f;
    style.PopupBorderSize   = 1.0f;

    style.WindowPadding     = ImVec2(14.0f, 12.0f);
    style.FramePadding      = ImVec2(9.0f, 6.0f);
    style.CellPadding       = ImVec2(6.0f, 5.0f);
    style.ItemSpacing       = ImVec2(9.0f, 7.0f);
    style.ItemInnerSpacing  = ImVec2(7.0f, 5.0f);
    style.IndentSpacing     = 18.0f;
    style.ScrollbarSize     = 14.0f;
    style.GrabMinSize       = 14.0f;
    style.SeparatorTextBorderSize = 2.0f;
    style.SelectableTextAlign = ImVec2(0.0f, 0.5f);

    const ImVec4 navy      (0.06f, 0.09f, 0.22f, 0.97f);
    const ImVec4 navy_deep (0.04f, 0.06f, 0.15f, 1.00f);
    const ImVec4 blue      (0.13f, 0.20f, 0.42f, 1.00f);
    const ImVec4 blue_hover(0.20f, 0.30f, 0.58f, 1.00f);
    const ImVec4 blue_high (0.27f, 0.39f, 0.70f, 1.00f);
    auto gold_a = [](float a) { return ImVec4(kGold.x, kGold.y, kGold.z, a); };

    ImVec4* c = style.Colors;
    c[ImGuiCol_Text]                 = kParchment;
    c[ImGuiCol_TextDisabled]         = kMutedText;
    c[ImGuiCol_WindowBg]             = navy;
    c[ImGuiCol_ChildBg]              = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_PopupBg]              = navy_deep;
    c[ImGuiCol_Border]               = kGoldDim;
    c[ImGuiCol_BorderShadow]         = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_FrameBg]              = navy_deep;
    c[ImGuiCol_FrameBgHovered]       = blue;
    c[ImGuiCol_FrameBgActive]        = blue_hover;
    c[ImGuiCol_TitleBg]              = navy_deep;
    c[ImGuiCol_TitleBgActive]        = blue;
    c[ImGuiCol_TitleBgCollapsed]     = navy_deep;
    c[ImGuiCol_MenuBarBg]            = navy_deep;
    c[ImGuiCol_ScrollbarBg]          = navy_deep;
    c[ImGuiCol_ScrollbarGrab]        = blue;
    c[ImGuiCol_ScrollbarGrabHovered] = blue_hover;
    c[ImGuiCol_ScrollbarGrabActive]  = kGoldDim;
    c[ImGuiCol_CheckMark]            = kGold;
    c[ImGuiCol_SliderGrab]           = kGoldDim;
    c[ImGuiCol_SliderGrabActive]     = kGold;
    c[ImGuiCol_Button]               = blue;
    c[ImGuiCol_ButtonHovered]        = blue_hover;
    c[ImGuiCol_ButtonActive]         = blue_high;
    c[ImGuiCol_Header]               = gold_a(0.30f);
    c[ImGuiCol_HeaderHovered]        = gold_a(0.20f);
    c[ImGuiCol_HeaderActive]         = gold_a(0.40f);
    c[ImGuiCol_Separator]            = kGoldDim;
    c[ImGuiCol_SeparatorHovered]     = kGold;
    c[ImGuiCol_SeparatorActive]      = kGold;
    c[ImGuiCol_ResizeGrip]           = gold_a(0.25f);
    c[ImGuiCol_ResizeGripHovered]    = gold_a(0.55f);
    c[ImGuiCol_ResizeGripActive]     = kGold;
    c[ImGuiCol_Tab]                  = navy_deep;
    c[ImGuiCol_TabHovered]           = blue_hover;
    c[ImGuiCol_TabActive]            = blue;
    c[ImGuiCol_TabUnfocused]         = navy_deep;
    c[ImGuiCol_TabUnfocusedActive]   = blue;
    c[ImGuiCol_DockingPreview]       = gold_a(0.50f);
    c[ImGuiCol_TableHeaderBg]        = navy_deep;
    c[ImGuiCol_TableBorderStrong]    = kGoldDim;
    c[ImGuiCol_TableBorderLight]     = ImVec4(0.22f, 0.27f, 0.45f, 1.00f);
    c[ImGuiCol_TableRowBg]           = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TableRowBgAlt]        = ImVec4(1.0f, 1.0f, 1.0f, 0.04f);
    c[ImGuiCol_TextSelectedBg]       = gold_a(0.35f);
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
    // A press beside a slider (on its label or the window's empty space)
    // used to start dragging the whole menu, so the menu shifted and the
    // slider seemed to snap away from the mouse (Jimmy, 2026-10-05). Windows
    // move by their title bar only.
    io.ConfigWindowsMoveFromTitleBarOnly = true;
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

    // SDL_Renderer rewrites the game window's mouse motion into its logical
    // coordinates whenever a logical size is set (the 240x160 view sets
    // one). ImGui reads the real desktop position while no button is held
    // but only these events during a drag, so a dragged slider saw the
    // pointer jump left by the window scale and snapped toward 0% (Jimmy,
    // 2026-10-05). Hand ImGui the same real position it reads between drags.
    SDL_Event event = *e;
    if (event.type == SDL_MOUSEMOTION && g_window &&
        event.motion.windowID == SDL_GetWindowID(g_window)) {
        int global_x = 0, global_y = 0, window_x = 0, window_y = 0;
        SDL_GetGlobalMouseState(&global_x, &global_y);
        SDL_GetWindowPosition(g_window, &window_x, &window_y);
        event.motion.x = global_x - window_x;
        event.motion.y = global_y - window_y;
    }
    ImGui_ImplSDL2_ProcessEvent(&event);

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
    // exactly as before this change. Stick directions (also synthetic ids)
    // are accepted for both gameplay and hotkey boxes.
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
        // Clamp to the window: at scale 1 the host window is 240x160, and an
        // unclamped panel would open almost entirely offscreen with no way
        // to drag it back.
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const ImVec2 work_pos = viewport->WorkPos;
        const ImVec2 work_size = viewport->WorkSize;
        const float max_w = std::max(1.0f, work_size.x);
        const float max_h = std::max(1.0f, work_size.y);
        // Every time the menu opens it fills the game window (Jimmy,
        // 2026-09-29); it stays resizable and movable while open.
        ImGui::SetNextWindowSize(ImVec2(max_w, max_h), ImGuiCond_Appearing);
        ImGui::SetNextWindowPos(work_pos, ImGuiCond_Appearing);
        // UI-03: floor the resize range so the corner grip never shrinks to
        // a sliver that cannot be grabbed again (the Hotkeys table needs
        // roughly this width). No maximum.
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(std::min(520.0f, max_w), std::min(360.0f, max_h)),
            ImVec2(FLT_MAX, FLT_MAX));
        // Windowed, the menu is its own OS window from the moment it opens
        // and never merges back, so it can be dragged off the game to see
        // the picture behind it (Jimmy, 2026-10-07). Merging back was what
        // made it jump and swallow a click (2026-10-05): a window lifted out
        // only for the drag. Fullscreen (and without the OpenGL backend) it
        // stays inside the game window: there is nowhere else to put it, and
        // the Steam Deck shows one window only.
        if (g_use_opengl && !st->fullscreen) {
            ImGuiWindowClass own_window;
            own_window.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
            ImGui::SetNextWindowClass(&own_window);
        } else {
            ImGui::SetNextWindowViewport(viewport->ID);
        }
        bool window_open = true;
        if (ImGui::Begin("Settings###Configuration", &window_open,
                         ImGuiWindowFlags_NoCollapse)) {
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

            // One sidebar of pages, the chosen page beside it, and a footer.
            // The page is remembered while the menu is closed.
            struct Page { const char* name; const char* blurb; };
            static const Page kPages[] = {
                {"Video",        "Window size, fullscreen and picture."},
                {"Audio",        "Volume."},
                {"Controls",     "Which keys and buttons play the game."},
                {"Hotkeys",      "Shortcuts such as fast forward and auto fire."},
                {"Fast Forward", "How fast fast forward runs."},
                {"Enhancements", "Optional improvements to the picture."},
            };
            constexpr int kPageCount =
                static_cast<int>(sizeof(kPages) / sizeof(kPages[0]));
            static int page = 0;
            page = std::clamp(page, 0, kPageCount - 1);

            const float footer_h = ImGui::GetFrameHeightWithSpacing() + 8.0f;
            ImGui::BeginChild("ConfigSidebar", ImVec2(150.0f, -footer_h),
                              ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);
            for (int i = 0; i < kPageCount; ++i) {
                const bool selected = page == i;
                if (selected) ImGui::PushStyleColor(ImGuiCol_Text, kGold);
                if (ImGui::Selectable(kPages[i].name, selected, 0,
                                      ImVec2(0.0f, 30.0f)))
                    page = i;
                if (selected) ImGui::PopStyleColor();
            }
            ImGui::EndChild();
            ImGui::SameLine();

            ImGui::BeginChild("ConfigContent", ImVec2(0.0f, -footer_h),
                              ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);
            ImGui::TextColored(kGold, "%s", kPages[page].name);
            ImGui::PushStyleColor(ImGuiCol_Text, kMutedText);
            ImGui::TextUnformatted(kPages[page].blurb);
            ImGui::PopStyleColor();
            switch (page) {
                case 0: draw_video_page(st); break;
                case 1: draw_audio_page(st); break;
                case 2: draw_controls_page(st); break;
                case 3: draw_hotkeys_page(st); break;
                case 4: draw_turbo_page(st); break;
                default: draw_enhancements_page(st); break;
            }
            ImGui::EndChild();

            ImGui::Spacing();
            if (ImGui::Button("Close")) window_open = false;
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("F1 or Esc");
            ImGui::SameLine();
            ImGui::TextDisabled("   %.0f FPS   %.0f%% speed",
                                st->fps, st->emulation_speed_percent);
            const char* quit_label = "Quit Game";
            const float quit_w = ImGui::CalcTextSize(quit_label).x +
                                 ImGui::GetStyle().FramePadding.x * 2.0f;
            ImGui::SameLine(ImGui::GetContentRegionMax().x - quit_w);
            if (ImGui::Button(quit_label)) st->request_quit = true;

            // Esc closes the menu, unless it is cancelling a bind capture.
            if (g_capture == Capture::None &&
                ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                ImGui::IsKeyPressed(ImGuiKey_Escape, false))
                window_open = false;
        }
        ImGui::End();
        if (!window_open) config_ui_set_visible(false);
    }

    draw_fps_overlay(st);

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
