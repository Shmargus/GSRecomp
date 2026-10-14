// Golden Sun: The Lost Age -- spell triangles past the console's canvas.
//
// Some battle effects are not stamped but drawn as textured triangles
// (Thunder Card's bolts; bench: state 2 of 2026-10-09, frames 109643+). The
// game copies a triangle rasterizer into IWRAM and calls it with r0 at a
// descriptor: +4 the facing test, +8 the triangle list (12 bytes each, three
// corner indices in the low bytes of the first word, the list ends where they
// are 0), +0xC the corners (8 bytes each, the first word y << 16 | x), +0x10
// the texture (bytes 0/1 log2 width/height, +4 its texels), +0x14 the
// subtracted shade, +0x18 a texture offset. It draws into the 128x128 canvas
// named by slot 0x03000060 and clips at its edges, so what lies past them
// never existed for the margins. Seven variants live in ROM (FACTS.md), one
// routine with a different pixel step each; the last one draws a 64x64
// canvas and is left alone.
//
// At the entry, for each neighbouring 128x128 square the expanded view
// shows, the same routine runs here, natively (rasterize() is the game's
// code line for line, its self-patched shifts as variables), with the
// triangles that reach that square and every corner moved by it, into a
// copy of the capture's history there. What it changed goes into the
// history (effect_capture.h), which the margins draw. The game's own run
// and state are untouched. Bench (Thunder Card, 360x240): every square of
// all 386 calls equals the interpreter's run of the game's code; 0.06 ms a
// call; the interpreter took 0.22-0.29 s over the cast.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "effect_capture.h"
#include "gba_bus.h"
#include "gba_ppu.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"
#include "tla.h"

namespace {

using u32 = std::uint32_t;
using s32 = std::int32_t;

constexpr u32 kEntryPush = 0xE92D4FE0u;  // push {r5-r11, lr}
// mov r3,#0x3000000; ldr r3,[r3,#0x60]; ldr r11,[r0,#4]; ldr r2,[r0,#0x10]
constexpr std::array<u32, 4> kEntryAt16 = {
    0xE3A03403u, 0xE5933060u, 0xE590B004u, 0xE5902010u};
constexpr u32 kCanvasSlot = 0x03000060u;
constexpr int kSide = 128;
constexpr std::size_t kCanvasBytes = kSide * kSide;
constexpr int kMaxTriangles = 4096;

// The pixel step of each variant (dst the canvas byte, t the texel, k the
// patched immediate of `subs sl,sl,#k`).
enum class Op {
    Replace,     // dst = t
    Max,         // dst = max(dst, t)
    AddSat,      // dst = min(dst + t, 63)
    SubMax,      // t -= k; if t > 0: dst = max(dst, t)
    SubAdd,      // t -= k; if t > 0: dst = min(dst + t, 63)
    SubReplace,  // t -= k; if t > 0: dst = t
};
struct Variant {
    u32 rom;         // the routine's first byte in ROM
    u32 loop_words;  // its pixel step, from +0x324
    Op op;
    int side_shift;  // log2 of the canvas side
};
constexpr Variant kVariants[] = {
    {0x081395F8u, 7, Op::Replace, 7},     {0x08139AACu, 9, Op::Max, 7},
    {0x08139F70u, 11, Op::AddSat, 7},     {0x0813A444u, 11, Op::SubMax, 7},
    {0x0813A924u, 13, Op::SubAdd, 7},     {0x0813AE14u, 10, Op::SubReplace, 7},
    {0x0813B2ECu, 7, Op::Replace, 6},
};
// Words the setup patches (shift amounts), never compared.
constexpr u32 kPatched[] = {0x168u, 0x170u, 0x22Cu, 0x324u, 0x328u};

struct Triangle {
    std::array<u32, 3> words;
    int x0, y0, x1, y1;  // the corners' box, inclusive
};
struct Box {
    int x0, y0, x1, y1;  // half-open
    bool empty() const { return x0 >= x1 || y0 >= y1; }
};

u32 rom_word(const gba::GbaBus* bus, u32 a) {
    const u32 off = a - 0x08000000u;
    if (off + 4u > bus->rom_size()) return 0;
    const std::uint8_t* p = bus->rom_ptr() + off;
    return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24);
}

// The variant whose code is live with its entry at `pc`, or null.
const Variant* live_variant(gba::GbaBus* bus, u32 pc) {
    for (const Variant& v : kVariants) {
        const u32 b = rom_word(bus, v.rom + 4u);  // `b` to the setup
        if ((b >> 24) != 0xEAu) continue;
        const u32 base = pc - (((b & 0xFFFFFFu) << 2) + 12u);
        if (bus->read32(base + 4u) != b) continue;
        const u32 end = 0x324u + v.loop_words * 4u;
        bool same = true;
        for (u32 at = 0x10u; at < end && same; at += 4u) {
            if (std::find(std::begin(kPatched), std::end(kPatched), at) !=
                std::end(kPatched))
                continue;
            u32 want = rom_word(bus, v.rom + at), live = bus->read32(base + at);
            if (at == 0x334u && v.op >= Op::SubMax) {  // its immediate byte
                want &= ~0xFFu;
                live &= ~0xFFu;
            }
            same = want == live;
        }
        if (same) return &v;
    }
    return nullptr;
}

struct Raster {
    gba::GbaBus* bus;
    Op op;
    int side_shift;
    int n;          // log2 texture width
    u32 mask;       // ((1 << m) - 1) << n
    u32 facing;     // descriptor +4
    u32 offset;     // descriptor +0x18
    u32 sub;        // the patched `subs` immediate
    u32 recip;      // the reciprocal table (2^32 / i, 0 for 1)
    const std::uint8_t* texels;
    u32 recip_of(u32 i) const { return bus->read32(recip + i * 4u); }
};

u32 asr(u32 v, int s) { return u32(s32(v) >> s); }
u32 mulhi(u32 a, u32 b) { return u32((std::uint64_t(a) * b) >> 32); }
u32 ror16(u32 v) { return (v >> 16) | (v << 16); }
bool neg(u32 v) { return s32(v) < 0; }

// The game's rasterizer from its triangle loop (+0x18) on, register for
// register, into `canvas` (side x side, row-major). `corners` holds the
// corner words by index.
void rasterize(const Raster& p, const std::vector<const Triangle*>& triangles,
               const std::array<u32, 256>& corners, std::uint8_t* canvas) {
    const int side = 1 << p.side_shift;
    const int n = p.n;
    auto put = [&](int x, int y, u32 t) {
        // Outside the row the game's address leaves the canvas; not ours.
        if (x < 0 || x >= side) return;
        std::uint8_t& d = canvas[std::size_t(y) * side + x];
        switch (p.op) {
            case Op::Replace: d = std::uint8_t(t); break;
            case Op::Max: if (d < t) d = std::uint8_t(t); break;
            case Op::AddSat: d = std::uint8_t(std::min<u32>(d + t, 0x3Fu)); break;
            case Op::SubMax: {
                const s32 v = s32(t) - s32(p.sub);
                if (v > 0 && s32(d) < v) d = std::uint8_t(v);
                break;
            }
            case Op::SubAdd: {
                const s32 v = s32(t) - s32(p.sub);
                if (v > 0) d = std::uint8_t(std::min<s32>(s32(d) + v, 0x3F));
                break;
            }
            case Op::SubReplace: {
                const s32 v = s32(t) - s32(p.sub);
                if (v > 0) d = std::uint8_t(v);
                break;
            }
        }
    };
    for (const Triangle* tri : triangles) {
        const u32 r3w = tri->words[0];
        u32 sb = tri->words[1], sl = tri->words[2];
        u32 r6 = corners[r3w & 0xFFu], r7 = corners[(r3w >> 8) & 0xFFu],
            r8 = corners[(r3w >> 16) & 0xFFu];
        if (p.facing & 3u) {  // +0x3C
            u32 r5 = asr(r8, 16) - asr(r6, 16);
            u32 r4 = asr((r7 - r6) << 16, 16);
            u32 r3 = r4 * r5;
            r5 = asr(r6, 16) - asr(r7, 16);
            r4 = asr((r8 - r6) << 16, 16);
            r3 = r4 * r5 + r3;
            if (r3 == 0u) continue;
            if (!neg(r3 ^ (p.facing << 31))) continue;
        }
        // +0x80: sort the corners by their words, the texture words along.
        if (s32(r6) > s32(r7)) { std::swap(r6, r7); sb = ror16(sb); }
        if (neg(r7 & r8)) continue;
        if (s32(r7) > s32(r8)) {
            std::swap(r7, r8);
            const u32 r5 = sl << 16;
            sl = ror16(sb);
            sb = r5 | (sl >> 16);
            if (s32(r6) > s32(r7)) { std::swap(r6, r7); sb = ror16(sb); }
        }
        // +0xD0
        if (s32(asr(r6, 16 + p.side_shift)) > 0) continue;
        u32 r3 = asr(r6, 16);
        u32 r4 = asr(r8, 16) - r3;
        if (r4 == 0u) continue;
        const u32 mid = r7, bottom = r8;  // stored at +8 / +0xC
        u32 st[7];                       // [sp] .. [sp+0x18]
        r6 <<= 16;
        u32 r0 = (r8 << 16) - r6;
        const bool long_left = neg(r0);
        if (long_left) r0 = 0u - r0;
        u32 r1 = p.recip_of(r4);
        u32 ip = r1 ? mulhi(r1, r0) : r0;
        if (long_left) ip = 0u - ip;
        st[2] = ip;
        r0 = sl - (sb & 0xFF00u);
        u32 r2 = r0 << 16;
        if (neg(r2)) r2 = 0u - r2;
        r4 = r1 ? mulhi(r1, r2) : r2;
        if (neg(r0 << 16)) r4 = 0u - r4;
        r0 = sl - sb;
        r2 = r0 << 24;
        if (neg(r2)) r2 = 0u - r2;
        if (r1 != 0u) r2 = mulhi(r1, r2);
        if (neg(r0 << 24)) r2 = 0u - r2;
        r0 = p.offset;  // +0x158
        u32 t = sb + r0;
        t += t;
        t += 1u;
        st[0] = t << (31 - n);
        st[4] = r2 << (8 - n);
        r0 >>= 8;
        u32 fp = (sb << 16) & 0xFF000000u;
        st[1] = (fp | 0x800000u) + (r0 << 24);
        sl = sb << 24;
        st[5] = r4;
        st[6] = r7;
        r0 = asr(r7, 16) - r3;  // +0x19C
        if (r0 != 0u) {
            fp = r4 * r0 + fp;
            sl = r2 * r0 + sl;
        }
        r1 = ip * r0 + r6;
        r1 = (r7 << 16) - r1;
        r1 = asr(r1, 16);
        if (r1 == 0u) continue;
        r2 = p.recip_of(r0);  // +0x1B8
        r0 = (r7 << 16) - r6;
        const bool short_left = neg(r0);
        if (short_left) r0 = 0u - r0;
        if (r2 != 0u) r0 = mulhi(r2, r0);
        if (short_left) r0 = 0u - r0;
        st[3] = r0;
        r2 = neg(r1) ? 0u - r1 : r1;
        r2 = p.recip_of(r2);
        r4 = (sb & 0xFF000000u) - fp;  // +0x1E8
        u32 r5 = r1 ^ r4;
        if (neg(r4)) r4 = 0u - r4;
        if (r2 != 0u) r4 = mulhi(r2, r4);
        if (neg(r5)) r4 = 0u - r4;
        u32 step = ((sb & 0xFF0000u) << 8) - sl;  // sb at +0x208
        r5 = r1 ^ step;
        if (neg(step)) step = 0u - step;
        if (r2 != 0u) step = mulhi(r2, step);
        if (neg(r5)) step = 0u - step;
        step = (step >> 16) << (24 - n);  // +0x22C, patched
        step |= r4 >> 16;
        const bool left = neg(r1);  // +0x234
        if (left) r6 -= 0x10000u;
        r5 = r6;
        if (left) step = 0u - step;
        const int dir = left ? -1 : 1;
        for (;;) {
            // +0x250
            s32 end = s32(st[6]) >> 16;
            if (end >= side) end = side;
            st[6] = u32(end);
            if (s32(r3) != end) {
                u32 a7 = st[0], a8 = st[1];
                do {
                    if (!neg(r3)) [&] {  // +0x278
                        const s32 q = s32(r6) >> (16 + p.side_shift);
                        const s32 last = q < 0 ? -1 : q > 0 ? side : s32(r6) >> 16;
                        s32 x = s32(r5) >> 16;
                        u32 skip = u32(x >> p.side_shift);
                        if (skip != 0u) {
                            if (neg(skip)) {  // +0x2AC
                                if (neg(r6)) return;
                                skip = u32(-x);
                                x = 0;
                            } else {
                                if (s32(r6) >= (side << 16)) return;
                                skip = u32(x - (side - 1));
                                x = side - 1;
                            }
                            skip = step * skip;  // +0x2BC
                            if (x - dir == last) return;
                        }
                        s32 count = x - last;  // +0x2CC
                        if (count == 0) return;
                        u32 frac = r5 & 0xFF00u;
                        if (count < 0) {
                            count = -count;
                            frac = 0x10000u - frac;
                        }
                        const u32 u = frac * asr(step, 16) + a7;
                        const u32 v = frac * asr(step << 16, 16) + a8;
                        u32 at = skip + ((u >> 16) << 16) + (v >> 16);
                        const int y = s32(r3);
                        for (; count > 0; --count, x += dir) {
                            const u32 lo = n == 8 ? at : at >> (8 - n);
                            const u32 texel = (p.mask & lo) | (at >> (32 - n));
                            at += step;
                            put(x, y, p.texels[texel]);
                        }
                    }();
                    // +0x364
                    r5 += st[2];
                    r6 += st[3];
                    a7 = st[0] + st[4];
                    a8 = st[1] + st[5];
                    st[0] = a7;
                    st[1] = a8;
                    r3 += 1u;
                } while (s32(r3) < s32(st[6]));
            }
            // +0x388
            if (s32(r3) >= side) break;
            r4 = asr(bottom, 16) - r3;
            if (r4 == 0u) break;
            st[6] = bottom;
            r6 = mid << 16;
            u32 r8b = (bottom << 16) - r6;
            const bool lower_left = neg(r8b);
            if (lower_left) r8b = 0u - r8b;
            const u32 rc = p.recip_of(r4);
            if (rc != 0u) r8b = mulhi(rc, r8b);
            if (lower_left) r8b = 0u - r8b;
            st[3] = r8b;
            if (left) r6 -= 0x10000u;
        }
    }
}

std::vector<std::uint8_t> read_guest(gba::GbaBus* bus, u32 a, std::size_t len) {
    std::vector<std::uint8_t> out(len);
    const std::uint8_t* src = nullptr;
    if (a - 0x02000000u + len <= 0x40000u) src = bus->ewram_ptr() + (a - 0x02000000u);
    else if (a - 0x03000000u + len <= 0x8000u) src = bus->iwram_ptr() + (a - 0x03000000u);
    else if (a >= 0x08000000u && a - 0x08000000u + len <= bus->rom_size())
        src = bus->rom_ptr() + (a - 0x08000000u);
    if (src) std::copy_n(src, len, out.begin());
    else
        for (std::size_t i = 0; i < len; ++i) out[i] = bus->read8(a + u32(i));
    return out;
}

// The canvas pixels the expanded view shows: its corners through BG2's
// transform, when BG2 is the 128 px affine layer that shows the canvas
// (Thunder Card: PA 0x80, PD 0x100, Y -16.0, F12 0111); else every
// neighbouring square. Empty in the console's own view.
Box visible_canvas(gba::GbaBus* bus) {
    const gba::GbaPpu* ppu = gbarecomp::active_ppu();
    if (!ppu) return {0, 0, 0, 0};
    const int l = int(ppu->view_extra_left()), r = int(ppu->view_extra_right());
    const int t = int(ppu->view_extra_top()), b = int(ppu->view_extra_bottom());
    if ((l | r | t | b) == 0) return {0, 0, 0, 0};
    const std::uint8_t* io = bus->io().raw();
    auto u16 = [&](int at) { return u32(io[at] | (io[at + 1] << 8)); };
    const u32 dispcnt = u16(0), bg2cnt = u16(0x0C);
    const u32 mode = dispcnt & 7u;
    if ((mode != 1u && mode != 2u) || (dispcnt & 0x400u) == 0u || (bg2cnt >> 14) != 0u)
        return {-kSide, -kSide, 2 * kSide, 2 * kSide};
    const int pa = std::int16_t(u16(0x20)), pb = std::int16_t(u16(0x22));
    const int pc = std::int16_t(u16(0x24)), pd = std::int16_t(u16(0x26));
    const int rx = s32((u16(0x28) | (u16(0x2A) << 16)) << 4) >> 4;
    const int ry = s32((u16(0x2C) | (u16(0x2E) << 16)) << 4) >> 4;
    Box box{0x7FFF, 0x7FFF, -0x8000, -0x8000};
    for (const int sx : {-l, 240 + r})
        for (const int sy : {-t, 160 + b}) {
            const int cx = (rx + pa * sx + pb * sy) >> 8;
            const int cy = (ry + pc * sx + pd * sy) >> 8;
            box.x0 = std::min(box.x0, cx); box.x1 = std::max(box.x1, cx + 1);
            box.y0 = std::min(box.y0, cy); box.y1 = std::max(box.y1, cy + 1);
        }
    return box;
}

}  // namespace

void tla_spell_mesh_on_entry(std::uint32_t pc) {
    if (!gsr::effect_capture_enabled() || pc < 0x03000000u || pc >= 0x03008000u)
        return;
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus || bus->read32(pc) != kEntryPush) return;
    for (std::size_t i = 0; i < kEntryAt16.size(); ++i)
        if (bus->read32(pc + 0x10u + u32(i) * 4u) != kEntryAt16[i]) return;
    const Box seen = visible_canvas(bus);
    if (seen.empty()) return;
    const Variant* variant = live_variant(bus, pc);
    if (!variant || variant->side_shift != 7) return;
    const u32 descriptor = g_cpu.R[0];
    const u32 canvas = bus->read32(kCanvasSlot);
    const u32 list = bus->read32(descriptor + 8u);
    const u32 corners = bus->read32(descriptor + 0xCu);
    const u32 texture = bus->read32(descriptor + 0x10u);
    const int n = bus->read8(texture), m = bus->read8(texture + 1u);
    if (n < 1 || n > 8 || m > 8) return;
    const std::vector<std::uint8_t> texels =
        read_guest(bus, bus->read32(texture + 4u), std::size_t(1) << (n + m));
    // The reciprocal table's literal, from the routine's own `ldr` at +0x100.
    const u32 base = pc - (((rom_word(bus, variant->rom + 4u) & 0xFFFFFFu) << 2) + 12u);
    const u32 load = bus->read32(base + 0x100u);
    Raster raster{bus, variant->op, variant->side_shift, n,
                  ((1u << m) - 1u) << n, bus->read32(descriptor + 4u),
                  bus->read32(descriptor + 0x18u),
                  (0u - bus->read32(descriptor + 0x14u)) & 0xFFu,
                  bus->read32(base + 0x108u + (load & 0xFFFu)) & ~1u,
                  texels.data()};
    std::vector<Triangle> triangles;
    std::array<u32, 256> corner{};
    for (int i = 0; i < 256; ++i) corner[i] = 0;
    std::array<bool, 256> have{};
    for (int n_tri = 0; n_tri < kMaxTriangles; ++n_tri) {
        Triangle t{};
        for (int k = 0; k < 3; ++k)
            t.words[k] = bus->read32(list + u32(n_tri * 12 + k * 4));
        if ((t.words[0] << 8) == 0u) break;
        t.x0 = t.y0 = 0x7FFF;
        t.x1 = t.y1 = -0x8000;
        for (int k = 0; k < 3; ++k) {
            const u32 i = (t.words[0] >> (k * 8)) & 0xFFu;
            if (!have[i]) {
                corner[i] = bus->read32(corners + i * 8u);
                have[i] = true;
            }
            const int x = static_cast<std::int16_t>(corner[i]);
            const int y = static_cast<std::int16_t>(corner[i] >> 16);
            t.x0 = std::min(t.x0, x); t.x1 = std::max(t.x1, x);
            t.y0 = std::min(t.y0, y); t.y1 = std::max(t.y1, y);
        }
        triangles.push_back(t);
    }
    if (triangles.empty()) return;
    for (int sy = -1; sy <= 1; ++sy)
        for (int sx = -1; sx <= 1; ++sx) {
            if (sx == 0 && sy == 0) continue;
            // The part of this square the margins show, a pixel wider for
            // the rasterizer's rounding.
            const Box need{std::max(sx * kSide, seen.x0 - 1),
                           std::max(sy * kSide, seen.y0 - 1),
                           std::min((sx + 1) * kSide, seen.x1 + 1),
                           std::min((sy + 1) * kSide, seen.y1 + 1)};
            if (need.empty()) continue;
            std::vector<const Triangle*> chosen;
            for (const Triangle& t : triangles)
                if (t.x1 >= need.x0 && t.x0 < need.x1 && t.y1 >= need.y0 &&
                    t.y0 < need.y1)
                    chosen.push_back(&t);
            if (chosen.empty()) continue;
            const int left = sx * kSide, top = sy * kSide;
            std::array<std::uint8_t, kCanvasBytes> before{};
            if (!gsr::effect_capture_history_read(canvas, left, top, before.data()))
                return;
            std::array<u32, 256> moved{};
            for (int i = 0; i < 256; ++i) {
                const int x = static_cast<std::int16_t>(corner[i]) - left;
                const int y = static_cast<std::int16_t>(corner[i] >> 16) - top;
                moved[i] = (u32(y) << 16) | (u32(x) & 0xFFFFu);
            }
            std::array<std::uint8_t, kCanvasBytes> after = before;
            rasterize(raster, chosen, moved, after.data());
            int x0 = kSide, y0 = kSide, x1 = -1, y1 = -1;
            for (int y = 0; y < kSide; ++y)
                for (int x = 0; x < kSide; ++x) {
                    const std::size_t i = std::size_t(y) * kSide + x;
                    if (after[i] == before[i]) continue;
                    x0 = std::min(x0, x); y0 = std::min(y0, y);
                    x1 = std::max(x1, x); y1 = std::max(y1, y);
                }
            if (x1 < 0) continue;
            const int w = x1 - x0 + 1, h = y1 - y0 + 1;
            std::vector<std::uint8_t> changed(std::size_t(w) * h);
            for (int y = 0; y < h; ++y)
                std::copy_n(after.data() + std::size_t(y0 + y) * kSide + x0, w,
                            changed.data() + std::size_t(y) * w);
            gsr::effect_capture_history_paint(canvas, left + x0, top + y0, w, h,
                                              changed.data());
        }
}
