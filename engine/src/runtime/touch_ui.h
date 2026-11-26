// Touch sizing and scrolling for the Android launcher, session screen
// and in-game menu.
//
// On Android, from the platform's own design guidelines:
//   * Body text is 16 dp (Material Design body text; 1 dp = dpi / 160 px).
//   * Every tappable row is at least 48 dp tall (Android's minimum touch
//     target).
//   * A finger drag scrolls the window under it once it moves past 8 dp
//     (Android's touch slop, ViewConfiguration). The drag also cancels the tap
//     it started, as phone lists do.
#pragma once

#include <SDL.h>

#include <algorithm>
#include <cmath>

#include "imgui.h"
#include "imgui_internal.h"

namespace gbarecomp::touch_ui {

#if defined(__ANDROID__)
inline constexpr bool kEnabled = true;
#else
inline constexpr bool kEnabled = false;
#endif

inline constexpr float kTextDp = 16.0f;
inline constexpr float kTouchTargetDp = 48.0f;
inline constexpr float kTouchSlopDp = 8.0f;

// Pixels per dp on the window's display (1 where the display reports none).
inline float density(SDL_Window* window) {
    float dpi = 0.0f;
    const int display = window ? SDL_GetWindowDisplayIndex(window) : 0;
    if (display >= 0 && SDL_GetDisplayDPI(display, &dpi, nullptr, nullptr) == 0 &&
        dpi > 0.0f)
        return dpi / 160.0f;
    return 1.0f;
}

// How much to enlarge a UI whose body font is `font_px` tall so its text is
// 16 dp. `whole`: a pixel-art font, scaled by a whole number to stay sharp.
// 1 on desktop.
inline float scale(SDL_Window* window, float font_px, bool whole = false) {
    if (!kEnabled || font_px <= 0.0f) return 1.0f;
    const float s = kTextDp * density(window) / font_px;
    return whole ? std::max(1.0f, std::round(s)) : std::max(1.0f, s);
}

// The smallest tappable size in pixels (0 on desktop: no minimum).
inline float min_target(SDL_Window* window) {
    return kEnabled ? kTouchTargetDp * density(window) : 0.0f;
}

// Scale spacing by `s` and pad rows to 48 dp. `font_px` is the scaled
// font height.
inline void apply_style(SDL_Window* window, float s, float font_px) {
    if (!kEnabled) return;
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(s);
    style.FramePadding.y =
        std::max(style.FramePadding.y, (min_target(window) - font_px) * 0.5f);
    style.GrabMinSize = std::max(style.GrabMinSize, min_target(window) * 0.5f);
    style.ScrollbarSize = std::max(style.ScrollbarSize, min_target(window) * 0.25f);
}

// Once per frame, right after ImGui::NewFrame(): finger-drag scrolling.
inline void drag_scroll(SDL_Window* window) {
    if (!kEnabled) return;
    ImGuiContext& g = *GImGui;
    static ImGuiWindow* target = nullptr;
    static int axis = -1;  // -1 undecided, 0 x, 1 y
    if (!g.IO.MouseDown[ImGuiMouseButton_Left]) {
        target = nullptr;
        axis = -1;
        return;
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        target = g.HoveredWindow;
        axis = -1;
    }
    if (!target) return;
    if (axis < 0) {
        const ImVec2 drag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f);
        const float slop = kTouchSlopDp * density(window);
        if (std::fabs(drag.x) < slop && std::fabs(drag.y) < slop) return;
        const int wanted = std::fabs(drag.y) >= std::fabs(drag.x) ? 1 : 0;
        // Use the nearest window that can scroll in this direction. Otherwise
        // leave the drag to the widget, such as a slider.
        ImGuiWindow* w = target;
        while (w && (wanted ? w->ScrollMax.y : w->ScrollMax.x) <= 0.0f)
            w = w->ParentWindow;
        if (!w) {
            target = nullptr;
            return;
        }
        target = w;
        axis = wanted;
    }
    const ImVec2 delta = g.IO.MouseDelta;
    if (axis == 1) ImGui::SetScrollY(target, target->Scroll.y - delta.y);
    else ImGui::SetScrollX(target, target->Scroll.x - delta.x);
    // A scroll is not a tap on whatever the finger started on.
    if (g.ActiveId) ImGui::ClearActiveID();
}

}  // namespace gbarecomp::touch_ui
