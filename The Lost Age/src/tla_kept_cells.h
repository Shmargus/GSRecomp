// tla_kept_cells.h -- battle sprites the game reuses while they sit past the
// console's screen keep their picture in the margins.
//
// Eclipse's dragon is 32 cells of 32x32 8-bit sprites filling all 32 KB of
// sprite memory (F12 tla_rewind_0177: block map 0x02003410 has no free
// block). When it changes pose (f042) the game takes the wing tip's cell
// (tile 800, x 252, past the console's x 239) for a new piece beside the head
// (x 60, y 42) and paints it over: on the console nothing is lost, in the
// widened view the wing loses a square (FACTS.md 2026-10-28). Jimmy chose to
// keep drawing the old picture.
//
// A sprite counts as reused when, from one frame to the next, the same tile
// moves by at least its own size (positions compared through the hardware's
// 9-bit x and 8-bit y wrap: Haures's body enters past x 255 and reads as
// -244, 0176 f012-f013) while its old place was wholly past the console's
// left or right edge, and its picture changed. The game paints the new
// picture a frame before it moves the sprite (0177: art f042, OAM f043), so
// the old picture is taken from up to two frames back. It is then drawn,
// margins only, at its old offset from its nearest same-size neighbour that
// did not jump, until that neighbour goes, another sprite of its size takes
// its place, or the battle ends. Over all 178 saved F12 histories this keeps
// only Eclipse's wing tip (0140 f052, 0177 f043).
//
// The pictures go into our copy of video memory, in the second sprite page
// (tiles 0x400 and up, which the renderer reads; tla_sprite_memory.cpp)
// from byte 0x1C000; never into the game's. Skipped while any sprite of the
// frame uses that page. Battles only, 1D sprite mapping, expanded view.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "field_scene.h"

struct TlaKeptCells {
    struct Seen {
        unsigned tile = 0;
        int x = 0, y = 0, w = 0, h = 0;
        bool eight = false;
        std::vector<std::uint8_t> art;  // only for sprites past the screen
    };
    struct Kept {
        unsigned ref = 0;  // the neighbour's tile
        int dx = 0, dy = 0;
        gsr::SceneObject object;
        std::vector<std::uint8_t> art;
    };
    std::vector<Seen> prev, prev2;
    std::vector<Kept> kept;
    std::uint64_t last_frame = 0;
    std::vector<std::uint8_t> vram;  // the copy uploaded while any are kept

    void reset() { prev.clear(); prev2.clear(); kept.clear(); }
};

namespace tla_kept_detail {

constexpr std::size_t kObjBase = 0x10000u;
constexpr std::size_t kPage2 = 0x18000u;
constexpr std::size_t kArtStart = 0x1C000u, kArtEnd = 0x20000u;

inline int wrap_x(int d) { return ((d + 256) & 511) - 256; }
inline int wrap_y(int d) { return ((d + 128) & 255) - 128; }

inline const TlaKeptCells::Seen* find(const std::vector<TlaKeptCells::Seen>& v,
                                      unsigned tile) {
    for (const auto& s : v) if (s.tile == tile) return &s;
    return nullptr;
}

}  // namespace tla_kept_detail

// Adds the kept sprites to `margins.margin_objects` and returns the video
// memory to upload: `vram` itself, or the copy holding their pictures.
inline const std::vector<std::uint8_t>& tla_kept_cells(
        const gsr::FieldScene& scene, gsr::SceneMargins& margins,
        const std::vector<std::uint8_t>& vram, bool expanded, bool battle,
        TlaKeptCells& st) {
    using namespace tla_kept_detail;
    using Seen = TlaKeptCells::Seen;
    bool usable = expanded && battle && scene.one_dimensional_objects &&
                  vram.size() >= kPage2;
    std::vector<Seen> cur;
    for (const auto& o : scene.objects) {
        if (!usable) break;
        if (!o.present || o.parked || o.window || o.affine) continue;
        if (o.tile >= 0x400u) { usable = false; break; }
        Seen s;
        s.tile = o.tile;
        s.x = o.x; s.y = o.y; s.w = o.width; s.h = o.height;
        s.eight = o.palette < 0;
        if (s.x >= 240 || s.x + s.w <= 0) {
            const std::size_t at = kObjBase + std::size_t(o.tile) * 32u;
            const std::size_t n = std::size_t(s.w) * s.h / (s.eight ? 1u : 2u);
            if (at + n <= kPage2) s.art.assign(vram.begin() + at, vram.begin() + at + n);
        }
        if (!find(cur, s.tile)) cur.push_back(std::move(s));
    }
    if (!usable || scene.frame <= st.last_frame) st.reset();
    st.last_frame = scene.frame;
    if (!usable) return vram;

    // Newly reused sprites.
    for (const Seen& p : st.prev) {
        if (p.art.empty()) continue;
        const Seen* c = find(cur, p.tile);
        if (!c || c->w != p.w || c->h != p.h || c->eight != p.eight) continue;
        if (std::abs(wrap_x(c->x - p.x)) < p.w && std::abs(wrap_y(c->y - p.y)) < p.h)
            continue;
        const std::size_t at = kObjBase + std::size_t(p.tile) * 32u;
        const std::size_t n = p.art.size();
        const std::uint8_t* now = vram.data() + at;
        auto same = [&](const std::vector<std::uint8_t>& a) {
            return std::equal(a.begin(), a.end(), now);
        };
        const std::vector<std::uint8_t>* old = nullptr;
        if (!same(p.art)) {
            old = &p.art;
        } else if (const Seen* q = find(st.prev2, p.tile)) {
            if (q->art.size() == n && !same(q->art)) old = &q->art;
        }
        if (!old) continue;
        const Seen* best = nullptr;
        long best_d = 0;
        for (const Seen& r : st.prev) {
            if (r.tile == p.tile || r.w != p.w || r.h != p.h) continue;
            const Seen* rc = find(cur, r.tile);
            if (!rc || rc->w != r.w || rc->h != r.h ||
                std::abs(wrap_x(rc->x - r.x)) >= r.w ||
                std::abs(wrap_y(rc->y - r.y)) >= r.h)
                continue;
            const long dx = wrap_x(r.x - p.x), dy = wrap_y(r.y - p.y);
            const long d = dx * dx + dy * dy;
            if (!best || d < best_d) { best = &r; best_d = d; }
        }
        if (!best) continue;
        const gsr::SceneObject* source = nullptr;
        for (const auto& o : scene.objects)
            if (o.present && !o.parked && o.tile == p.tile) { source = &o; break; }
        if (!source) continue;
        TlaKeptCells::Kept k;
        k.ref = best->tile;
        k.dx = wrap_x(p.x - best->x);
        k.dy = wrap_y(p.y - best->y);
        k.object = *source;  // palette, priority, flips and blending
        k.art = *old;
        st.kept.push_back(std::move(k));
    }

    // Follow each kept sprite's neighbour.
    std::vector<TlaKeptCells::Kept> still;
    for (auto& k : st.kept) {
        const Seen* r = find(cur, k.ref);
        if (!r || r->w != k.object.width || r->h != k.object.height) continue;
        const int x = r->x + k.dx, y = r->y + k.dy;
        bool taken = false;
        for (const Seen& c : cur)
            taken = taken || (c.w == k.object.width && c.h == k.object.height &&
                              std::abs(wrap_x(c.x - x)) < c.w / 2 &&
                              std::abs(wrap_y(c.y - y)) < c.h / 2);
        const bool inside = x >= 0 && x + k.object.width <= 240 &&
                            y >= 0 && y + k.object.height <= 160;
        if (taken || inside) continue;
        k.object.x = x;
        k.object.y = y;
        still.push_back(std::move(k));
    }
    st.kept = std::move(still);
    st.prev2 = std::move(st.prev);
    st.prev = std::move(cur);
    if (st.kept.empty()) return vram;

    st.vram.assign(vram.begin(), vram.end());
    st.vram.resize(kArtEnd, 0);
    std::size_t at = kArtStart;
    for (const auto& k : st.kept) {
        if (at + k.art.size() > kArtEnd) break;
        std::copy(k.art.begin(), k.art.end(), st.vram.begin() + at);
        gsr::SceneObject o = k.object;
        o.tile = 0x400u + unsigned((at - kPage2) / 32u);
        o.parked = false;
        margins.margin_objects.push_back(o);
        at += (k.art.size() + 31u) & ~std::size_t(31);
    }
    return st.vram;
}
