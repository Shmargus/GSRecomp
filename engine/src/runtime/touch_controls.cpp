#include "touch_controls.h"

#if defined(__ANDROID__) && defined(GBARECOMP_HAVE_SDL2)

#include <SDL.h>

#include <algorithm>
#include <cmath>

#include "imgui.h"

namespace gbarecomp {
namespace {

// GBA KEYINPUT bit numbers.
enum : int { kA = 0, kB, kSelect, kStart, kRight, kLeft, kUp, kDown, kR, kL };

struct Circle { float x, y, r; };
struct Rect { float x, y, w, h; };

// Use the window's short side `u` to size controls around the game view.
struct Layout {
    float u = 0;
    Circle dpad{}, a{}, b{}, start{}, select{};
    Rect l{}, r{};
};

Layout layout_for(float w, float h) {
    Layout k;
    const float u = std::min(w, h);
    k.u = u;
    k.dpad = {0.24f * u, h - 0.32f * u, 0.20f * u};
    k.a = {w - 0.13f * u, h - 0.40f * u, 0.095f * u};
    k.b = {w - 0.33f * u, h - 0.24f * u, 0.095f * u};
    k.select = {0.11f * u, 0.30f * u, 0.065f * u};
    k.start = {w - 0.11f * u, 0.30f * u, 0.065f * u};
    k.l = {0.0f, 0.0f, 0.30f * u, 0.15f * u};
    k.r = {w - 0.30f * u, 0.0f, 0.30f * u, 0.15f * u};
    return k;
}

// Accept taps slightly outside the visible button.
constexpr float kSlack = 1.35f;

bool in_circle(const Circle& c, float x, float y, float slack = kSlack) {
    const float dx = x - c.x, dy = y - c.y, r = c.r * slack;
    return dx * dx + dy * dy <= r * r;
}

bool in_rect(const Rect& r, float x, float y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

// Choose a diagonal when the touch is far enough from both axes.
std::uint16_t dpad_bits(const Circle& c, float x, float y) {
    const float dx = x - c.x, dy = y - c.y;
    const float dist = std::sqrt(dx * dx + dy * dy);
    if (dist < c.r * 0.15f || dist > c.r * 1.6f) return 0;
    std::uint16_t bits = 0;
    const float ax = std::fabs(dx), ay = std::fabs(dy);
    const bool both = ax > 0.4f * dist && ay > 0.4f * dist;
    if (both || ax >= ay) bits |= static_cast<std::uint16_t>(1u << (dx > 0 ? kRight : kLeft));
    if (both || ay > ax) bits |= static_cast<std::uint16_t>(1u << (dy > 0 ? kDown : kUp));
    return bits;
}

std::uint16_t g_held = 0;
bool g_buttons_shown = true;  // touch_controls_set_buttons_shown
bool g_fast_forward = false;  // touch_controls_set_fast_forward

// ---- the floating buttons: menu and fast forward -----------------------

// Radius as a share of the short side: about 9 mm across on a phone, above
// Android's 48 dp touch target.
constexpr float kFloatRadius = 0.07f;
// Slide into the edge after this much idle time.
constexpr Uint64 kTuckAfterMs = 3000;
// Leave this fraction visible so the button can still be tapped.
constexpr float kTuckedShowing = 0.4f;
// A finger that moves this far (share of the short side) drags instead of
// tapping (about Android's 8 dp touch slop).
constexpr float kDragSlop = 0.02f;

// A round button on a side edge: tap it, or drag it to another spot (it
// snaps to the nearer edge). Idle, it tucks partly into the edge.
struct FloatButton {
    bool right = true;        // the edge it sits on
    float y_share = 0.5f;     // centre height, share of the window height
    float x = -1.0f;          // drawn centre x (eased toward its target)
    Uint64 last_used = 0;     // SDL_GetTicks64 of the last touch
    SDL_FingerID finger = 0;  // the finger on it
    bool held = false;
    bool dragging = false;
    float down_x = 0, down_y = 0;
    float drag_x = 0, drag_y = 0;
    Uint64 last_ease = 0;
};
// The menu button starts on the right edge; fast forward on the left, below
// Select and above the D-pad.
FloatButton g_menu{true, 0.435f};
FloatButton g_speed{false, 0.47f};

float float_radius(float w, float h) { return kFloatRadius * std::min(w, h); }

// Where its centre belongs now: out from its edge, or tucked into it.
float float_target_x(const FloatButton& fb, float w, float r, bool tucked) {
    const float out = r * 1.15f;
    const float in = r * (2.0f * kTuckedShowing) - r;
    const float from_edge = tucked ? in : out;
    return fb.right ? w - from_edge : from_edge;
}

// Fast forward stays out while it runs, so it shows that it is on.
bool float_tucked(const FloatButton& fb) {
    if (&fb == &g_speed && g_fast_forward) return false;
    return !fb.held && SDL_GetTicks64() - fb.last_used > kTuckAfterMs;
}

Circle float_circle(const FloatButton& fb, float w, float h) {
    const float r = float_radius(w, h);
    if (fb.dragging) return {fb.drag_x, fb.drag_y, r};
    float x = fb.x;
    if (x < 0) x = float_target_x(fb, w, r, false);
    return {x, std::clamp(fb.y_share, 0.0f, 1.0f) * h, r};
}

bool owns_finger(SDL_FingerID id) {
    return (g_menu.held && g_menu.finger == id) || (g_speed.held && g_speed.finger == id);
}

// Follow the finger on `fb`, or take a new touch on it. True when a tap
// (not a drag) ends this frame.
bool poll_float(FloatButton& fb, float w, float h, Uint64 now) {
    if (fb.last_used == 0) fb.last_used = now;
    bool finger_down = false;
    for (int d = 0; d < SDL_GetNumTouchDevices(); ++d) {
        const SDL_TouchID id = SDL_GetTouchDevice(d);
        for (int f = 0; f < SDL_GetNumTouchFingers(id); ++f) {
            const SDL_Finger* finger = SDL_GetTouchFinger(id, f);
            if (!finger) continue;
            const float x = finger->x * w, y = finger->y * h;
            if (fb.held && finger->id == fb.finger) {
                finger_down = true;
                const float slop = kDragSlop * std::min(w, h);
                if (!fb.dragging && std::hypot(x - fb.down_x, y - fb.down_y) > slop)
                    fb.dragging = true;
                fb.drag_x = x;
                fb.drag_y = y;
                fb.last_used = now;
            } else if (!fb.held && !owns_finger(finger->id) &&
                       in_circle(float_circle(fb, w, h), x, y, 1.2f)) {
                fb.held = true;
                fb.dragging = false;
                fb.finger = finger->id;
                fb.down_x = fb.drag_x = x;
                fb.down_y = fb.drag_y = y;
                fb.last_used = now;
                finger_down = true;
            }
        }
    }
    bool tapped = false;
    if (fb.held && !finger_down) {
        // On release, snap a drag to the nearer edge.
        if (fb.dragging) {
            fb.right = fb.drag_x > w * 0.5f;
            const float r = float_radius(w, h);
            fb.y_share = std::clamp(fb.drag_y, r, h - r) / h;
            fb.x = fb.drag_x;
        } else {
            tapped = true;
        }
        fb.held = false;
        fb.dragging = false;
        fb.last_used = now;
    }
    return tapped;
}

// Draw `fb`, eased toward where it belongs (about 0.15 s). Sets `icon_x` to
// where an icon `icon_half_w` wide centres: tucked, on the part still on
// screen.
Circle draw_float(ImDrawList* draw, FloatButton& fb, float width, float height, bool on,
                  float icon_half_w, float* icon_x) {
    const float r = float_radius(width, height);
    const Uint64 now = SDL_GetTicks64();
    const float dt = fb.last_ease ? std::min(0.1f, (now - fb.last_ease) / 1000.0f) : 0.0f;
    fb.last_ease = now;
    const float target = float_target_x(fb, width, r, float_tucked(fb));
    if (fb.x < 0) fb.x = target;
    else fb.x += (target - fb.x) * std::min(1.0f, dt * 12.0f);
    const Circle m = float_circle(fb, width, height);
    // On (fast forward running): gold, like the menu's headings.
    const ImU32 fill = on ? IM_COL32(190, 140, 40, 230)
                     : fb.held ? IM_COL32(40, 40, 40, 220) : IM_COL32(20, 20, 20, 170);
    draw->AddCircleFilled({m.x, m.y}, m.r, fill, 48);
    draw->AddCircle({m.x, m.y}, m.r, IM_COL32(255, 255, 255, 140), 48, m.r * 0.06f);
    const float visible_left = std::max(0.0f, m.x - m.r);
    const float visible_right = std::min(width, m.x + m.r);
    const float lo = visible_left + icon_half_w + m.r * 0.07f;
    const float hi = visible_right - icon_half_w - m.r * 0.07f;
    *icon_x = lo <= hi ? std::clamp(m.x, lo, hi) : (visible_left + visible_right) * 0.5f;
    return m;
}

}  // namespace

bool touch_controls_enabled() { return true; }

void touch_controls_set_buttons_shown(bool shown) { g_buttons_shown = shown; }

void touch_controls_set_fast_forward(bool on) { g_fast_forward = on; }

TouchControlsInput touch_controls_poll(int width, int height, bool menu_open) {
    TouchControlsInput in;
    if (width <= 0 || height <= 0) return in;
    const float w = static_cast<float>(width), h = static_cast<float>(height);
    const Layout k = layout_for(w, h);
    const Uint64 now = SDL_GetTicks64();

    // The floating buttons hide while the menu is open.
    if (menu_open) {
        for (FloatButton* fb : {&g_menu, &g_speed}) {
            fb->held = fb->dragging = false;
            fb->last_used = now;
        }
    } else {
        in.menu_pressed = poll_float(g_menu, w, h, now);
        in.fast_forward_pressed = poll_float(g_speed, w, h, now);
    }

    if (!menu_open && g_buttons_shown) {
        for (int d = 0; d < SDL_GetNumTouchDevices(); ++d) {
            const SDL_TouchID id = SDL_GetTouchDevice(d);
            for (int f = 0; f < SDL_GetNumTouchFingers(id); ++f) {
                const SDL_Finger* finger = SDL_GetTouchFinger(id, f);
                if (!finger || owns_finger(finger->id)) continue;
                const float x = finger->x * w, y = finger->y * h;
                in.held |= dpad_bits(k.dpad, x, y);
                if (in_circle(k.a, x, y)) in.held |= 1u << kA;
                if (in_circle(k.b, x, y)) in.held |= 1u << kB;
                if (in_circle(k.start, x, y)) in.held |= 1u << kStart;
                if (in_circle(k.select, x, y)) in.held |= 1u << kSelect;
                if (in_rect(k.l, x, y)) in.held |= 1u << kL;
                if (in_rect(k.r, x, y)) in.held |= 1u << kR;
            }
        }
    }
    g_held = in.held;
    return in;
}

void touch_controls_draw_overlay(float width, float height, bool menu_open) {
    if (width <= 0 || height <= 0) return;
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    const Layout k = layout_for(width, height);
    const auto fill = [](bool pressed) {
        return pressed ? IM_COL32(255, 255, 255, 140) : IM_COL32(255, 255, 255, 60);
    };
    const ImU32 ink = IM_COL32(255, 255, 255, 210);
    // A label centred on (x, y), `size` pixels tall.
    const auto label = [&](float x, float y, float size, const char* text) {
        const ImVec2 extent = font->CalcTextSizeA(size, 1e9f, 0.0f, text);
        draw->AddText(font, size, {x - extent.x * 0.5f, y - extent.y * 0.5f}, ink, text);
    };
    const auto held = [](int bit) { return ((g_held >> bit) & 1u) != 0; };

    if (!menu_open && g_buttons_shown) {
        // D-pad: a cross, each arm lit while held.
        const float arm = k.dpad.r, bar = k.dpad.r * 0.62f;
        const float cx = k.dpad.x, cy = k.dpad.y;
        const auto rect = [&](float x, float y, float w, float h, bool pressed) {
            draw->AddRectFilled({x, y}, {x + w, y + h}, fill(pressed));
        };
        rect(cx - bar / 2, cy - bar / 2, bar, bar, false);
        rect(cx - bar / 2, cy - arm, bar, arm - bar / 2, held(kUp));
        rect(cx - bar / 2, cy + bar / 2, bar, arm - bar / 2, held(kDown));
        rect(cx - arm, cy - bar / 2, arm - bar / 2, bar, held(kLeft));
        rect(cx + bar / 2, cy - bar / 2, arm - bar / 2, bar, held(kRight));
        // Arrowheads on the arms.
        const float tip = arm * 0.85f, back = arm * 0.55f, half = bar * 0.28f;
        draw->AddTriangleFilled({cx, cy - tip}, {cx - half, cy - back}, {cx + half, cy - back}, ink);
        draw->AddTriangleFilled({cx, cy + tip}, {cx + half, cy + back}, {cx - half, cy + back}, ink);
        draw->AddTriangleFilled({cx - tip, cy}, {cx - back, cy + half}, {cx - back, cy - half}, ink);
        draw->AddTriangleFilled({cx + tip, cy}, {cx + back, cy - half}, {cx + back, cy + half}, ink);

        const auto button = [&](const Circle& c, bool pressed, const char* text,
                                float text_size) {
            draw->AddCircleFilled({c.x, c.y}, c.r, fill(pressed), 48);
            label(c.x, c.y, text_size, text);
        };
        button(k.a, held(kA), "A", k.a.r * 0.9f);
        button(k.b, held(kB), "B", k.b.r * 0.9f);
        // Start and Select sit under their labels' width, so smaller text.
        button(k.select, held(kSelect), "SELECT", k.select.r * 0.42f);
        button(k.start, held(kStart), "START", k.start.r * 0.42f);
        draw->AddRectFilled({k.l.x, k.l.y}, {k.l.x + k.l.w, k.l.y + k.l.h}, fill(held(kL)));
        draw->AddRectFilled({k.r.x, k.r.y}, {k.r.x + k.r.w, k.r.y + k.r.h}, fill(held(kR)));
        label(k.l.x + k.l.w * 0.5f, k.l.y + k.l.h * 0.5f, k.l.h * 0.55f, "L");
        label(k.r.x + k.r.w * 0.5f, k.r.y + k.r.h * 0.5f, k.r.h * 0.55f, "R");
    }

    if (menu_open) return;  // the menu has its own Resume
    const ImU32 icon = IM_COL32(255, 255, 255, 230);
    const float r = float_radius(width, height);
    float icon_x = 0;

    // The menu button: three lines, the usual "menu" sign.
    const Circle m = draw_float(draw, g_menu, width, height, false, r * 0.38f, &icon_x);
    const float line_w = m.r * 0.38f, line_h = m.r * 0.09f;
    for (int i = -1; i <= 1; ++i) {
        const float y = m.y + i * m.r * 0.28f;
        draw->AddRectFilled({icon_x - line_w, y - line_h}, {icon_x + line_w, y + line_h},
                            icon, line_h);
    }

    // Fast forward: two chevrons (>>), gold while it runs.
    const Circle s = draw_float(draw, g_speed, width, height, g_fast_forward, r * 0.36f, &icon_x);
    const float arm_w = s.r * 0.22f, arm_h = s.r * 0.30f, step = s.r * 0.26f;
    for (int i = 0; i < 2; ++i) {
        const float x = icon_x - (step + arm_w) * 0.5f + i * step;
        const ImVec2 chevron[3] = {{x, s.y - arm_h}, {x + arm_w, s.y}, {x, s.y + arm_h}};
        draw->AddPolyline(chevron, 3, icon, ImDrawFlags_None, s.r * 0.12f);
    }
}

}  // namespace gbarecomp

#else  // desktop: no on-screen buttons

namespace gbarecomp {
bool touch_controls_enabled() { return false; }
TouchControlsInput touch_controls_poll(int, int, bool) { return {}; }
void touch_controls_set_buttons_shown(bool) {}
void touch_controls_set_fast_forward(bool) {}
void touch_controls_draw_overlay(float, float, bool) {}
}  // namespace gbarecomp

#endif
