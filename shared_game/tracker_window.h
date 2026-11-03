// Tracker windows (Djinn Tracker, Treasure Tracker): one look and behaviour.
// A window in the F1 menu's own frame. Where the graphics card draws
// (OpenGL, ImGui viewports) it can be its own desktop window. Linux fullscreen
// keeps it inside the game window. Dragged by its title line, resized from any edge;
// position and size saved as <name>X, <name>Y, <name>Width, <name>Height.
#pragma once
#ifdef GBARECOMP_HAVE_IMGUI
#include "game_options.h"
#include "host_config_ui.h"
#include "imgui.h"
#include "imgui_internal.h"
// SDL.h can rename the game runner's main on Windows.
#include <SDL_video.h>
#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <string>

namespace gsr::tracker_window {

constexpr ImVec4 kGold(0.93f, 0.76f, 0.30f, 1.0f);
constexpr int kNoSaved = -1000000;

struct Window {
    const char* name;   // ImGui ID and the saved keys' prefix
    float default_down; // default distance below the game's top edge
    int x = kNoSaved, y = kNoSaved, width = 0, height = 0;
    bool loaded = false;
    bool fullscreen_overlay = false;
    ImVec2 fullscreen_pos{FLT_MAX, FLT_MAX};
    void (*previous)() = nullptr;  // the extra drawing installed before this one
};

// Once at startup: draw() runs in F1's extra drawing, after whatever was
// installed before; draw() calls w.previous first.
inline void chain(Window& w, void (*draw)()) {
    w.previous = gbarecomp::g_config_ui_extra_draw;
    gbarecomp::g_config_ui_extra_draw = draw;
}

inline std::string key(const Window& w, const char* field) { return std::string(w.name) + field; }

inline SDL_Window* sdl_window(const ImGuiViewport* viewport) {
    return SDL_GetWindowFromID(static_cast<Uint32>(
        reinterpret_cast<std::uintptr_t>(viewport->PlatformHandle)));
}

inline void sync_topmost(SDL_Window* tracker, bool fullscreen) {
    // SDL's ImGui backend applies TopMost only when it creates a window.
    if (tracker && bool(SDL_GetWindowFlags(tracker) & SDL_WINDOW_ALWAYS_ON_TOP) != fullscreen)
        SDL_SetWindowAlwaysOnTop(tracker, fullscreen ? SDL_TRUE : SDL_FALSE);
}

// Opens the window and draws its frame and title; draw the contents only
// when it returns true, and call end() either way.
inline bool begin(Window& w, ImVec2 default_size, const char* title, ImGuiWindowFlags extra = 0) {
    if (!w.loaded) {
        w.loaded = true;
        w.x = gbarecomp::game_option_get(key(w, "X").c_str(), kNoSaved);
        w.y = gbarecomp::game_option_get(key(w, "Y").c_str(), kNoSaved);
        w.width = gbarecomp::game_option_get(key(w, "Width").c_str(), 0);
        w.height = gbarecomp::game_option_get(key(w, "Height").c_str(), 0);
    }
    const ImGuiViewport* main = ImGui::GetMainViewport();
    SDL_Window* game_window = sdl_window(main);
    const bool fullscreen = game_window &&
        (SDL_GetWindowFlags(game_window) & SDL_WINDOW_FULLSCREEN) != 0;
    const bool viewports = (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0;
#if defined(__linux__)
    // A Linux window manager can stack focused fullscreen above always-on-top
    // windows. Draw trackers in the game's viewport while fullscreen.
    const bool fullscreen_overlay = fullscreen && viewports;
#else
    constexpr bool fullscreen_overlay = false;
#endif
    const bool mode_changed = w.fullscreen_overlay != fullscreen_overlay;
    w.fullscreen_overlay = fullscreen_overlay;
    const bool own_window = viewports && !fullscreen_overlay;
    // In fullscreen there may be no desktop to the right of the game.
    const ImVec2 last_visible(
        main->WorkPos.x + std::max(0.0f, main->WorkSize.x - 64.0f),
        main->WorkPos.y + std::max(0.0f, main->WorkSize.y - 64.0f));
    const ImVec2 inset(std::min(main->WorkPos.x + 8.0f, last_visible.x),
                       std::min(main->WorkPos.y + 8.0f + w.default_down, last_visible.y));
    const ImVec2 size(w.width > 0 ? float(w.width) : default_size.x, w.height > 0 ? float(w.height) : default_size.y);
    ImVec2 pos = w.x != kNoSaved && w.y != kNoSaved ? ImVec2(float(w.x), float(w.y)) :
        own_window && !fullscreen ? ImVec2(main->Pos.x + main->Size.x + 8.0f, main->Pos.y + w.default_down)
                                  : inset;
    ImGuiCond position_condition = mode_changed ? ImGuiCond_Always : ImGuiCond_Appearing;
    if (fullscreen_overlay) {
        ImGui::SetNextWindowViewport(main->ID);
        pos = w.fullscreen_pos.x == FLT_MAX ? inset
            : ImVec2(main->WorkPos.x + w.fullscreen_pos.x, main->WorkPos.y + w.fullscreen_pos.y);
    }
    const auto& monitors = ImGui::GetPlatformIO().Monitors;
    if (fullscreen && own_window && !monitors.empty() && !ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
        std::none_of(monitors.begin(), monitors.end(), [&](const auto& monitor) {
            return pos.x >= monitor.WorkPos.x && pos.x < monitor.WorkPos.x + monitor.WorkSize.x &&
                   pos.y >= monitor.WorkPos.y && pos.y < monitor.WorkPos.y + monitor.WorkSize.y;
        })) {
        pos = inset;
        position_condition = ImGuiCond_Always;
    }
    if (!own_window) {
        pos.x = std::clamp(pos.x, main->WorkPos.x, last_visible.x);
        pos.y = std::clamp(pos.y, main->WorkPos.y, last_visible.y);
    }
    ImGui::SetNextWindowPos(pos, position_condition);
    ImGui::SetNextWindowSize(size, ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(160.0f, 120.0f), ImVec2(FLT_MAX, FLT_MAX));
    ImGuiWindowClass window_class;
    if (own_window) {
        window_class.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
        if (fullscreen) window_class.ViewportFlagsOverrideSet |= ImGuiViewportFlags_TopMost;
    }
    ImGui::SetNextWindowClass(&window_class);
    const std::string id = std::string(title) + "###" + w.name;
    const bool visible = ImGui::Begin(id.c_str(), nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | extra);
    const auto* viewport = ImGui::GetWindowViewport();
    if (viewport != main) {
        sync_topmost(sdl_window(viewport), fullscreen);
    } else if (fullscreen && !ImGui::GetTopMostPopupModal()) {
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    }
    if (!visible) return false;
    gbarecomp::config_ui_draw_menu_frame(ImGui::GetWindowPos().x, ImGui::GetWindowPos().y,
                                         ImGui::GetWindowSize().x, ImGui::GetWindowSize().y);
    // The title line is the drag handle, as on F1's Settings window.
    const ImVec2 title_pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##drag",
        ImVec2(std::max(1.0f, ImGui::GetContentRegionAvail().x), ImGui::GetTextLineHeight()));
    if (ImGui::IsItemActivated()) ImGui::StartMouseMovingWindow(ImGui::GetCurrentWindow());
    ImGui::SetCursorScreenPos(title_pos);
    ImGui::TextColored(kGold, "%s", title);
    ImGui::Spacing();
    return true;
}

// Saves the layout once a drag or resize ends, not on every frame of it.
inline void end(Window& w) {
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const ImVec2 p = ImGui::GetWindowPos(), s = ImGui::GetWindowSize();
        const auto keep = [&](const char* field, int& saved, int value) {
            if (saved == value) return;
            saved = value;
            gbarecomp::game_option_set(key(w, field).c_str(), value);
        };
        if (w.fullscreen_overlay) {
            const ImVec2 origin = ImGui::GetMainViewport()->WorkPos;
            w.fullscreen_pos = ImVec2(p.x - origin.x, p.y - origin.y);
        } else {
            keep("X", w.x, int(p.x));
            keep("Y", w.y, int(p.y));
        }
        keep("Width", w.width, int(s.x));
        keep("Height", w.height, int(s.y));
    }
    ImGui::End();
}

// A gold header line with a fold arrow, the width of the window; returns
// true when clicked.
inline bool fold_header(int id, bool open, const char* text) {
    ImDrawList* list = ImGui::GetWindowDrawList();
    const ImU32 gold = ImGui::ColorConvertFloat4ToU32(kGold);
    const float h = ImGui::GetTextLineHeight();
    const ImVec2 head = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    const bool clicked = ImGui::InvisibleButton("##fold", ImVec2(std::max(1.0f, ImGui::GetContentRegionAvail().x), h));
    ImGui::PopID();
    ImGui::RenderArrow(list, ImVec2(head.x, head.y + h * 0.1f), gold, open ? ImGuiDir_Down : ImGuiDir_Right, 0.7f);
    list->AddText(ImVec2(head.x + h, head.y), gold, text);
    return clicked;
}

} // namespace gsr::tracker_window
#endif
