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

// ---- the floating menu button ----------------------------------------

// Radius as a share of the short side: about 9 mm across on a phone, above
// Android's 48 dp touch target.
constexpr float kMenuRadius = 0.07f;
// Slide into the edge after this much idle time.
constexpr Uint64 kTuckAfterMs = 3000;
// Leave this fraction visible so the button can still be tapped.
constexpr float kTuckedShowing = 0.4f;
// A finger that moves this far (share of the short side) drags instead of
// tapping (about Android's 8 dp touch slop).
constexpr float kDragSlop = 0.02f;

struct MenuButton {
    bool right = true;        // the edge it sits on
    float y_share = 0.435f;   // centre height, share of the window height
    float x = -1.0f;          // drawn centre x (eased toward its target)
    Uint64 last_used = 0;     // SDL_GetTicks64 of the last touch
    SDL_FingerID finger = 0;  // the finger on it
    bool held = false;
    bool dragging = false;
    float down_x = 0, down_y = 0;
    float drag_x = 0, drag_y = 0;
    Uint64 last_ease = 0;
} g_menu;

float menu_radius(float w, float h) { return kMenuRadius * std::min(w, h); }

// Where its centre belongs now: out from its edge, or tucked into it.
float menu_target_x(float w, float r, bool tucked) {
    const float out = r * 1.15f;
    const float in = r * (2.0f * kTuckedShowing) - r;
    const float from_edge = tucked ? in : out;
    return g_menu.right ? w - from_edge : from_edge;
}

bool menu_tucked(bool menu_open) {
    return !menu_open && !g_menu.held &&
           SDL_GetTicks64() - g_menu.last_used > kTuckAfterMs;
}

Circle menu_circle(float w, float h) {
    const float r = menu_radius(w, h);
    if (g_menu.dragging) return {g_menu.drag_x, g_menu.drag_y, r};
    float x = g_menu.x;
    if (x < 0) x = menu_target_x(w, r, false);
    return {x, std::clamp(g_menu.y_share, 0.0f, 1.0f) * h, r};
}

}  // namespace

bool touch_controls_enabled() { return true; }

void touch_controls_set_buttons_shown(bool shown) { g_buttons_shown = shown; }

TouchControlsInput touch_controls_poll(int width, int height, bool menu_open) {
    TouchControlsInput in;
    if (width <= 0 || height <= 0) return in;
    const float w = static_cast<float>(width), h = static_cast<float>(height);
    const Layout k = layout_for(w, h);
    const Uint64 now = SDL_GetTicks64();
    if (g_menu.last_used == 0) g_menu.last_used = now;

    // Track a finger dragging the menu button, or capture a new touch on it.
    // Hide the button while the menu is open.
    bool menu_finger_down = false;
    if (menu_open) {
        g_menu.held = g_menu.dragging = false;
        g_menu.last_used = now;
    }
    for (int d = 0; !menu_open && d < SDL_GetNumTouchDevices(); ++d) {
        const SDL_TouchID id = SDL_GetTouchDevice(d);
        for (int f = 0; f < SDL_GetNumTouchFingers(id); ++f) {
            const SDL_Finger* finger = SDL_GetTouchFinger(id, f);
            if (!finger) continue;
            const float x = finger->x * w, y = finger->y * h;
            if (g_menu.held && finger->id == g_menu.finger) {
                menu_finger_down = true;
                const float slop = kDragSlop * std::min(w, h);
                if (!g_menu.dragging && std::hypot(x - g_menu.down_x, y - g_menu.down_y) > slop)
                    g_menu.dragging = true;
                g_menu.drag_x = x;
                g_menu.drag_y = y;
                g_menu.last_used = now;
            } else if (!g_menu.held && in_circle(menu_circle(w, h), x, y, 1.2f)) {
                g_menu.held = true;
                g_menu.dragging = false;
                g_menu.finger = finger->id;
                g_menu.down_x = g_menu.drag_x = x;
                g_menu.down_y = g_menu.drag_y = y;
                g_menu.last_used = now;
                menu_finger_down = true;
            }
        }
    }
    if (g_menu.held && !menu_finger_down) {
        // On release, snap a drag to the nearer edge. A tap toggles the menu.
        if (g_menu.dragging) {
            g_menu.right = g_menu.drag_x > w * 0.5f;
            const float r = menu_radius(w, h);
            g_menu.y_share = std::clamp(g_menu.drag_y, r, h - r) / h;
            g_menu.x = g_menu.drag_x;
        } else {
            in.menu_pressed = true;
        }
        g_menu.held = false;
        g_menu.dragging = false;
        g_menu.last_used = now;
    }

    if (!menu_open && g_buttons_shown) {
        for (int d = 0; d < SDL_GetNumTouchDevices(); ++d) {
            const SDL_TouchID id = SDL_GetTouchDevice(d);
            for (int f = 0; f < SDL_GetNumTouchFingers(id); ++f) {
                const SDL_Finger* finger = SDL_GetTouchFinger(id, f);
                if (!finger || (g_menu.held && finger->id == g_menu.finger)) continue;
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
    // The menu button, eased toward where it belongs (about 0.15 s).
    const float r = menu_radius(width, height);
    const Uint64 now = SDL_GetTicks64();
    const float dt = g_menu.last_ease ? std::min(0.1f, (now - g_menu.last_ease) / 1000.0f) : 0.0f;
    g_menu.last_ease = now;
    const float target = menu_target_x(width, r, menu_tucked(menu_open));
    if (g_menu.x < 0) g_menu.x = target;
    else g_menu.x += (target - g_menu.x) * std::min(1.0f, dt * 12.0f);
    const Circle m = menu_circle(width, height);
    const bool lit = g_menu.held || menu_open;
    draw->AddCircleFilled({m.x, m.y}, m.r,
                          lit ? IM_COL32(40, 40, 40, 220) : IM_COL32(20, 20, 20, 170), 48);
    draw->AddCircle({m.x, m.y}, m.r, IM_COL32(255, 255, 255, 140), 48, m.r * 0.06f);
    // Three lines: the usual "menu" sign. Tucked, it shows on the part that
    // is still on screen.
    const float visible_left = std::max(0.0f, m.x - m.r);
    const float visible_right = std::min(width, m.x + m.r);
    const float lo = visible_left + m.r * 0.45f, hi = visible_right - m.r * 0.45f;
    const float icon_x = lo <= hi ? std::clamp(m.x, lo, hi) : (visible_left + visible_right) * 0.5f;
    const float line_w = m.r * 0.38f, line_h = m.r * 0.09f;
    for (int i = -1; i <= 1; ++i) {
        const float y = m.y + i * m.r * 0.28f;
        draw->AddRectFilled({icon_x - line_w, y - line_h}, {icon_x + line_w, y + line_h},
                            IM_COL32(255, 255, 255, 230), line_h);
    }
}

}  // namespace gbarecomp

#else  // desktop: no on-screen buttons

namespace gbarecomp {
bool touch_controls_enabled() { return false; }
TouchControlsInput touch_controls_poll(int, int, bool) { return {}; }
void touch_controls_set_buttons_shown(bool) {}
void touch_controls_draw_overlay(float, float, bool) {}
}  // namespace gbarecomp

#endif
