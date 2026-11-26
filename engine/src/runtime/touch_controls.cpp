#include "touch_controls.h"

#if defined(__ANDROID__) && defined(GBARECOMP_HAVE_SDL2)

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "host_config_ui.h"
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

// The GBA buttons the player can move (touch_controls_begin_placing), in
// the saved order: D-pad, A, B, Select, Start, L, R.
constexpr int kPlaceable = 7;
// Each one's centre as a share of the window's width and height, or x < 0
// for where layout_for puts it.
struct Placed { float x = -1, y = -1; };
Placed g_placed[kPlaceable];
bool g_layout_changed = false;  // touch_controls_take_layout_changed

// Moves the centre of a control `half_w` by `half_h` to its saved place,
// kept wholly on screen.
void place(float* cx, float* cy, float half_w, float half_h, const Placed& p, float w, float h) {
    if (p.x < 0) return;
    *cx = std::clamp(p.x * w, std::min(half_w, w * 0.5f), std::max(w - half_w, w * 0.5f));
    *cy = std::clamp(p.y * h, std::min(half_h, h * 0.5f), std::max(h - half_h, h * 0.5f));
}

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
    Circle* circles[] = {&k.dpad, &k.a, &k.b, &k.select, &k.start};
    for (int i = 0; i < 5; ++i)
        place(&circles[i]->x, &circles[i]->y, circles[i]->r, circles[i]->r, g_placed[i], w, h);
    Rect* rects[] = {&k.l, &k.r};
    for (int i = 0; i < 2; ++i) {
        Rect& r = *rects[i];
        float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
        place(&cx, &cy, r.w * 0.5f, r.h * 0.5f, g_placed[5 + i], w, h);
        r.x = cx - r.w * 0.5f;
        r.y = cy - r.h * 0.5f;
    }
    return k;
}

// Control `i`'s centre, in placing order.
void control_centre(const Layout& k, int i, float* x, float* y) {
    const Circle* circles[] = {&k.dpad, &k.a, &k.b, &k.select, &k.start};
    if (i < 5) { *x = circles[i]->x; *y = circles[i]->y; return; }
    const Rect& r = i == 5 ? k.l : k.r;
    *x = r.x + r.w * 0.5f;
    *y = r.y + r.h * 0.5f;
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

// ---- placing the GBA buttons (Menu > Controls > Place on-screen buttons) --

bool g_placing = false;
struct Grab { bool held = false; SDL_FingerID finger = 0; float dx = 0, dy = 0; };
Grab g_grab[kPlaceable];  // dx, dy: the finger's offset from the centre
Grab g_bar;               // a finger on Done or Reset; dx, dy: where it is
int g_bar_target = -1;    // 0 Done, 1 Reset

// Done and Reset, side by side at the top centre, between L and R.
void placing_bar(float w, float h, Rect* done, Rect* reset) {
    const float u = std::min(w, h);
    const float bw = 0.30f * u, bh = 0.12f * u, gap = 0.04f * u, top = 0.05f * u;
    *done = {w * 0.5f - gap * 0.5f - bw, top, bw, bh};
    *reset = {w * 0.5f + gap * 0.5f, top, bw, bh};
}

// The control a new touch at (x, y) picks up: the nearest one under it.
int control_at(const Layout& k, float x, float y) {
    int best = -1;
    float best_d = 0;
    const Circle* circles[] = {&k.dpad, &k.a, &k.b, &k.select, &k.start};
    for (int i = 0; i < kPlaceable; ++i) {
        const bool hit = i < 5 ? in_circle(*circles[i], x, y, i == 0 ? 1.2f : kSlack)
                               : in_rect(i == 5 ? k.l : k.r, x, y);
        if (!hit) continue;
        float cx, cy;
        control_centre(k, i, &cx, &cy);
        const float d = std::hypot(x - cx, y - cy);
        if (best < 0 || d < best_d) { best = i; best_d = d; }
    }
    return best;
}

bool finger_is_down(SDL_FingerID id, float w, float h, float* x, float* y) {
    for (int d = 0; d < SDL_GetNumTouchDevices(); ++d) {
        const SDL_TouchID touch = SDL_GetTouchDevice(d);
        for (int f = 0; f < SDL_GetNumTouchFingers(touch); ++f) {
            const SDL_Finger* finger = SDL_GetTouchFinger(touch, f);
            if (finger && finger->id == id) { *x = finger->x * w; *y = finger->y * h; return true; }
        }
    }
    return false;
}

// One frame of placing: drag controls, tap Done or Reset. No game input.
void poll_placing(float w, float h) {
    const Layout k = layout_for(w, h);
    Rect done, reset;
    placing_bar(w, h, &done, &reset);
    // Fingers already holding a control move it, or let go.
    for (int i = 0; i < kPlaceable; ++i) {
        Grab& g = g_grab[i];
        if (!g.held) continue;
        float x, y;
        if (!finger_is_down(g.finger, w, h, &x, &y)) { g.held = false; continue; }
        g_placed[i] = {std::clamp((x - g.dx) / w, 0.0f, 1.0f), std::clamp((y - g.dy) / h, 0.0f, 1.0f)};
    }
    if (g_bar.held) {
        float x = 0, y = 0;
        if (finger_is_down(g_bar.finger, w, h, &x, &y)) {
            g_bar.dx = x;
            g_bar.dy = y;
        } else {
            g_bar.held = false;
            // Acts on release over the same button, like any phone button.
            if (in_rect(g_bar_target == 0 ? done : reset, g_bar.dx, g_bar.dy)) {
                if (g_bar_target == 0) {
                    g_placing = false;
                    g_layout_changed = true;
                    for (Grab& g : g_grab) g.held = false;
                } else {
                    for (Placed& p : g_placed) p = {};
                }
            }
        }
    }
    // New touches pick up a control or press Done / Reset.
    for (int d = 0; d < SDL_GetNumTouchDevices(); ++d) {
        const SDL_TouchID touch = SDL_GetTouchDevice(d);
        for (int f = 0; f < SDL_GetNumTouchFingers(touch); ++f) {
            const SDL_Finger* finger = SDL_GetTouchFinger(touch, f);
            if (!finger) continue;
            bool owned = g_bar.held && g_bar.finger == finger->id;
            for (const Grab& g : g_grab) owned |= g.held && g.finger == finger->id;
            if (owned) continue;
            const float x = finger->x * w, y = finger->y * h;
            if (in_rect(done, x, y) || in_rect(reset, x, y)) {
                if (!g_bar.held) {
                    g_bar = {true, finger->id, x, y};
                    g_bar_target = in_rect(done, x, y) ? 0 : 1;
                }
                continue;
            }
            const int i = control_at(k, x, y);
            if (i < 0 || g_grab[i].held) continue;
            float cx, cy;
            control_centre(k, i, &cx, &cy);
            g_grab[i] = {true, finger->id, x - cx, y - cy};
        }
    }
}

}  // namespace

bool touch_controls_enabled() { return true; }

void touch_controls_begin_placing() {
    g_placing = true;
    g_bar = {};
    for (Grab& g : g_grab) g = {};
}

bool touch_controls_placing() { return g_placing; }

bool touch_controls_take_layout_changed() {
    const bool changed = g_layout_changed;
    g_layout_changed = false;
    return changed;
}

std::string touch_controls_layout() {
    std::string text;
    for (int i = 0; i < kPlaceable; ++i) {
        char item[32];
        if (g_placed[i].x < 0) std::snprintf(item, sizeof(item), "-");
        else std::snprintf(item, sizeof(item), "%.4f,%.4f", g_placed[i].x, g_placed[i].y);
        if (i) text += ';';
        text += item;
    }
    return text;
}

void touch_controls_set_layout(const char* text) {
    for (Placed& p : g_placed) p = {};
    const char* at = text;
    for (int i = 0; i < kPlaceable && at && *at; ++i) {
        char* end = nullptr;
        const float x = std::strtof(at, &end);
        if (end != at && *end == ',') {
            const char* y_at = end + 1;
            const float y = std::strtof(y_at, &end);
            if (end != y_at && x >= 0 && x <= 1 && y >= 0 && y <= 1) g_placed[i] = {x, y};
        }
        at = std::strchr(at, ';');
        if (at) ++at;
    }
}

void touch_controls_set_buttons_shown(bool shown) { g_buttons_shown = shown; }

void touch_controls_set_fast_forward(bool on) { g_fast_forward = on; }

TouchControlsInput touch_controls_poll(int width, int height, bool menu_open) {
    TouchControlsInput in;
    if (width <= 0 || height <= 0) return in;
    const float w = static_cast<float>(width), h = static_cast<float>(height);
    const Uint64 now = SDL_GetTicks64();
    if (g_placing) {
        // Opening the menu (a controller's menu button) ends placing as Done.
        if (menu_open) {
            g_placing = false;
            g_layout_changed = true;
        } else {
            poll_placing(w, h);
        }
        for (FloatButton* fb : {&g_menu, &g_speed}) {
            fb->held = fb->dragging = false;
            fb->last_used = now;
        }
        g_held = 0;
        return in;
    }
    const Layout k = layout_for(w, h);

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

    const bool placing = g_placing && !menu_open;
    // While placing, a dark veil says the game is not taking input.
    if (placing) draw->AddRectFilled({0, 0}, {width, height}, IM_COL32(0, 0, 0, 110));
    if (placing || (!menu_open && g_buttons_shown)) {
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

    if (placing) {
        // Gold outlines on what can be moved; brighter while held.
        const ImU32 gold = IM_COL32(190, 140, 40, 200), bright = IM_COL32(240, 190, 70, 255);
        const float line = k.u * 0.008f;
        const Circle* circles[] = {&k.dpad, &k.a, &k.b, &k.select, &k.start};
        for (int i = 0; i < 5; ++i)
            draw->AddCircle({circles[i]->x, circles[i]->y}, circles[i]->r * (i == 0 ? 1.15f : 1.1f),
                            g_grab[i].held ? bright : gold, 48, line);
        const Rect* rects[] = {&k.l, &k.r};
        for (int i = 0; i < 2; ++i)
            draw->AddRect({rects[i]->x, rects[i]->y}, {rects[i]->x + rects[i]->w, rects[i]->y + rects[i]->h},
                          g_grab[5 + i].held ? bright : gold, 0, 0, line);
        Rect done, reset;
        placing_bar(width, height, &done, &reset);
        const auto bar_button = [&](const Rect& r, int target, const char* text) {
            const bool down = g_bar.held && g_bar_target == target && in_rect(r, g_bar.dx, g_bar.dy);
            draw->AddRectFilled({r.x, r.y}, {r.x + r.w, r.y + r.h},
                                down ? IM_COL32(240, 190, 70, 240) : IM_COL32(190, 140, 40, 230), r.h * 0.2f);
            label(r.x + r.w * 0.5f, r.y + r.h * 0.5f, r.h * 0.45f, ui_text(text));
        };
        bar_button(done, 0, "Done");
        bar_button(reset, 1, "Reset");
        label(width * 0.5f, done.y + done.h + k.u * 0.06f, k.u * 0.045f,
              ui_text("Drag the buttons where you want them."));
        return;  // no menu or >> button while placing
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
void touch_controls_begin_placing() {}
bool touch_controls_placing() { return false; }
bool touch_controls_take_layout_changed() { return false; }
std::string touch_controls_layout() { return {}; }
void touch_controls_set_layout(const char*) {}
void touch_controls_draw_overlay(float, float, bool) {}
}  // namespace gbarecomp

#endif
