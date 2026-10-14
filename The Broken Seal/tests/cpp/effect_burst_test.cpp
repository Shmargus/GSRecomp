// Does the dirt explosion behave as specified? Synthetic; no ROM, no graphics.
#include "effect_burst.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

using gsr::BurstKind;
using gsr::BurstQuad;

std::size_t count_kind(const std::vector<BurstQuad>& q, BurstKind k) {
    std::size_t n = 0;
    for (const auto& s : q)
        if (s.kind == k) ++n;
    return n;
}

const BurstQuad* first_of(const std::vector<BurstQuad>& q, BurstKind k) {
    for (const auto& s : q)
        if (s.kind == k) return &s;
    return nullptr;
}

void test_constants() {
    assert(gsr::kBurstGeyserCount == 36 && gsr::kBurstChunkCount == 96);
    assert(gsr::kBurstDustCount == 22 && gsr::kBurstGritCount == 48);
    assert(gsr::kBurstGeyserSpeedMin == 9.0f && gsr::kBurstGeyserSpeedMax == 15.0f);
    assert(gsr::kBurstGeyserDrag == 0.93f && gsr::kBurstGeyserGravity == 0.30f);
    assert(gsr::kBurstGeyserSizeMin == 4 && gsr::kBurstGeyserSizeMax == 7);
    assert(gsr::kBurstGeyserLifeMin == 36 && gsr::kBurstGeyserLifeMax == 56);
    assert(gsr::kBurstChunkSpeedMin == 6.0f && gsr::kBurstChunkSpeedMax == 14.0f);
    assert(gsr::kBurstChunkSizeMin == 4 && gsr::kBurstChunkSizeMax == 8);
    assert(gsr::kBurstChunkLifeMin == 34 && gsr::kBurstChunkLifeMax == 56);
    assert(gsr::kBurstDustRadiusEnd == 64.0f && gsr::kBurstDustFrames == 30);
    assert(gsr::kBurstDustAlpha == 0.6f);
    assert(gsr::kBurstGritSpeedMin == 8.0f && gsr::kBurstGritSpeedMax == 14.0f);
    assert(gsr::kBurstGritLifeMin == 12 && gsr::kBurstGritLifeMax == 20);
    assert(gsr::kBurstFlashSizeStart == 12.0f && gsr::kBurstFlashSizeEnd == 96.0f);
    assert(gsr::kBurstFlashGrowFrames == 4 && gsr::kBurstFlashHoldFrames == 2 &&
           gsr::kBurstFlashFrames == 14 && gsr::kBurstFlashAlpha == 0.95f);
    assert(gsr::kShakeImpactAmplitude == 6.0f && gsr::kShakeImpactFrames == 26);
    assert(gsr::kBurstOffsetY == 12.0f);
}

void test_spawn_counts_and_order() {
    gsr::EffectBurst b;
    assert(!b.alive());
    b.spawn(100.0f, 50.0f);
    assert(b.alive());
    assert(b.live_particles() == gsr::kBurstDustCount + gsr::kBurstChunkCount +
                                     gsr::kBurstGeyserCount +
                                     gsr::kBurstGritCount);
    std::vector<BurstQuad> q;
    b.collect(&q);
    // No trails at age 0.
    assert(count_kind(q, BurstKind::Dust) == gsr::kBurstDustCount);
    assert(count_kind(q, BurstKind::Chunk) == gsr::kBurstChunkCount);
    assert(count_kind(q, BurstKind::Geyser) == gsr::kBurstGeyserCount);
    assert(count_kind(q, BurstKind::Grit) == gsr::kBurstGritCount);
    assert(count_kind(q, BurstKind::Flash) == 1);
    assert(count_kind(q, BurstKind::Trail) == 0);
    // Draw order in the list: dust, chunks, geyser, grit, flash.
    assert(q.front().kind == BurstKind::Dust);
    assert(q.back().kind == BurstKind::Flash);
    BurstKind prev = BurstKind::Dust;
    for (const auto& s : q) {
        assert(static_cast<int>(s.kind) >= static_cast<int>(prev));
        prev = s.kind;
        assert(std::fabs(s.x - 100.0f) <= gsr::kBurstOriginSpread + 1e-4f);
        assert(std::fabs(s.y - 50.0f) <= gsr::kBurstOriginSpread + 1e-4f);
        assert(s.a >= 0.0f && s.a <= 1.0f);
    }
}

void test_chunks_and_geyser_launch_upward() {
    gsr::EffectBurst b;
    b.spawn(0, 0);
    // Screen y grows downward: up is negative vy. At least 70% of the radial
    // chunks go up; every geyser chunk does (within 15 degrees of vertical).
    assert(b.moving_up(BurstKind::Chunk) * 100 >= 70 * gsr::kBurstChunkCount);
    assert(b.moving_up(BurstKind::Geyser) == gsr::kBurstGeyserCount);
    // The geyser column really rises and stays narrow: after 6 steps its
    // chunks are far above the impact point and close to the vertical axis.
    for (int i = 0; i < 6; ++i) b.step();
    std::vector<BurstQuad> q;
    b.collect(&q);
    int n = 0;
    for (const auto& s : q) {
        if (s.kind != BurstKind::Geyser || s.a < 0.6f) continue;
        ++n;
        assert(s.y < -30.0f);
        // tan(15 degrees) * height + origin spread
        assert(std::fabs(s.x) <= std::fabs(s.y) * 0.2679f + 2.0f *
                                     gsr::kBurstOriginSpread);
    }
    assert(n > 0);
}

void test_deterministic() {
    gsr::EffectBurst a, b;
    a.spawn(10, 20);
    b.spawn(10, 20);
    for (int i = 0; i < 20; ++i) { a.step(); b.step(); }
    std::vector<BurstQuad> qa, qb;
    a.collect(&qa);
    b.collect(&qb);
    assert(qa.size() == qb.size());
    for (std::size_t i = 0; i < qa.size(); ++i)
        assert(qa[i].x == qb[i].x && qa[i].y == qb[i].y &&
               qa[i].a == qb[i].a && qa[i].kind == qb[i].kind);
}

void test_flash_timeline() {
    gsr::EffectBurst b;
    b.spawn(0, 0);
    std::vector<BurstQuad> q;
    b.collect(&q);
    const BurstQuad f0v = *first_of(q, BurstKind::Flash);
    const BurstQuad* f0 = &f0v;
    assert(f0->size == gsr::kBurstFlashSizeStart);
    assert(f0->a == gsr::kBurstFlashAlpha);
    // Starts pale (#FFF0C8).
    assert(std::fabs(f0->r - 1.0f) < 1e-4f);
    for (int i = 0; i < gsr::kBurstFlashGrowFrames; ++i) b.step();
    q.clear();
    b.collect(&q);
    const BurstQuad* f = first_of(q, BurstKind::Flash);
    assert(f && std::fabs(f->size - gsr::kBurstFlashSizeEnd) < 1e-4f);
    assert(f->a == gsr::kBurstFlashAlpha);  // peak
    // Held for two frames at full size and alpha.
    for (int i = 0; i < gsr::kBurstFlashHoldFrames; ++i) {
        b.step();
        q.clear();
        b.collect(&q);
        f = first_of(q, BurstKind::Flash);
        assert(f->size == gsr::kBurstFlashSizeEnd && f->a == gsr::kBurstFlashAlpha);
    }
    b.step();
    q.clear();
    b.collect(&q);
    assert(first_of(q, BurstKind::Flash)->a < gsr::kBurstFlashAlpha);
    // The colour moves toward #E8C888.
    assert(first_of(q, BurstKind::Flash)->b < f0->b);
    for (int i = gsr::kBurstFlashGrowFrames + gsr::kBurstFlashHoldFrames + 1;
         i < gsr::kBurstFlashFrames; ++i)
        b.step();
    q.clear();
    b.collect(&q);
    assert(!first_of(q, BurstKind::Flash));  // gone by frame 14
}

void test_dust_timeline() {
    gsr::EffectBurst b;
    b.spawn(0, 0);
    std::vector<BurstQuad> q;
    b.collect(&q);
    const BurstQuad* d0 = first_of(q, BurstKind::Dust);
    assert(std::fabs(d0->size - 2.0f * gsr::kBurstDustRadiusStart) < 1e-4f);
    assert(std::fabs(d0->a - gsr::kBurstDustAlpha) < 1e-6f);
    float prev_size = d0->size, prev_a = d0->a;
    for (int i = 1; i < gsr::kBurstDustFrames; ++i) {
        b.step();
        q.clear();
        b.collect(&q);
        assert(count_kind(q, BurstKind::Dust) == gsr::kBurstDustCount);
        const BurstQuad* d = first_of(q, BurstKind::Dust);
        assert(d->size > prev_size && d->a < prev_a);
        prev_size = d->size;
        prev_a = d->a;
    }
    assert(prev_size < 2.0f * gsr::kBurstDustRadiusEnd);
    b.step();
    q.clear();
    b.collect(&q);
    assert(count_kind(q, BurstKind::Dust) == 0);  // done after 30 frames
}

void test_trails_and_fades() {
    gsr::EffectBurst b;
    b.spawn(0, 0);
    b.step();
    std::vector<BurstQuad> q;
    b.collect(&q);
    // After one step chunks, geyser and grit each have their one trail quad.
    assert(count_kind(q, BurstKind::Chunk) == 2u * gsr::kBurstChunkCount);
    assert(count_kind(q, BurstKind::Geyser) == 2u * gsr::kBurstGeyserCount);
    assert(count_kind(q, BurstKind::Grit) == 2u * gsr::kBurstGritCount);
    float chunk_top = 0;
    for (const auto& s : q)
        if (s.kind == BurstKind::Chunk && s.a > chunk_top) chunk_top = s.a;
    assert(std::fabs(chunk_top - gsr::kBurstChunkAlpha) < 1e-6f);
    // Near the end of the shortest chunk life the chunks are fading.
    for (int i = 1; i < gsr::kBurstChunkLifeMin - 2; ++i) b.step();
    q.clear();
    b.collect(&q);
    bool faded = false;
    for (const auto& s : q) {
        assert(s.a >= 0.0f && s.a <= 1.0f);
        if (s.kind == BurstKind::Chunk && s.a > 0.0f && s.a < 0.4f)
            faded = true;
    }
    assert(faded);
}

void test_everything_ends() {
    gsr::EffectBurst b;
    b.spawn(0, 0);
    for (int i = 0; i < gsr::kBurstChunkLifeMax; ++i) b.step();
    assert(b.live_particles() == 0);
    assert(!b.alive());
    std::vector<BurstQuad> q;
    b.collect(&q);
    assert(q.empty());
}

void test_earth_toned() {
    // No additive draw and no white core: every burst colour is a brown/tan
    // (red >= green >= blue, not near-white).
    gsr::EffectBurst b;
    b.spawn(0, 0);
    for (int step = 0; step < 15; ++step) {
        std::vector<BurstQuad> q;
        b.collect(&q);
        for (const auto& s : q) {
            assert(s.r >= s.g && s.g >= s.b);
            assert(s.b < 0.8f);
        }
        b.step();
    }
}

void test_shake() {
    gsr::ScreenShake sh;
    assert(!sh.active());
    sh.start();
    assert(sh.active());
    assert(std::fabs(sh.amplitude() - 6.0f) < 1e-6f);
    float prev = sh.amplitude();
    float last_dx = sh.dx();
    for (int i = 1; i < gsr::kShakeImpactFrames; ++i) {
        assert(std::fabs(sh.dx()) <= sh.amplitude() + 1e-4f);
        assert(std::fabs(sh.dy()) <=
               sh.amplitude() * gsr::kShakeYFactor + 1e-4f);
        // The scaled picture still covers the shifted view edges.
        const float s = sh.cover_scale(360, 240);
        assert(360.0f * (s - 1.0f) * 0.5f >= std::fabs(sh.dx()) - 1e-4f);
        assert(240.0f * (s - 1.0f) * 0.5f >= std::fabs(sh.dy()) - 1e-4f);
        sh.step();
        const float expect = gsr::kShakeImpactAmplitude *
                             (1.0f - static_cast<float>(i) /
                                         gsr::kShakeImpactFrames);
        assert(std::fabs(sh.amplitude() - expect) < 1e-4f);  // linear decay
        assert(sh.amplitude() < prev);
        prev = sh.amplitude();
        assert(sh.dx() * last_dx < 0.0f);  // sign alternates every frame
        last_dx = sh.dx();
    }
    sh.step();
    assert(!sh.active() && sh.dx() == 0.0f && sh.dy() == 0.0f);
    assert(sh.cover_scale(360, 240) == 1.0f);
    // Deterministic.
    gsr::ScreenShake a, b;
    a.start();
    b.start();
    for (int i = 0; i < 10; ++i) {
        assert(a.dx() == b.dx() && a.dy() == b.dy());
        a.step();
        b.step();
    }
}

void test_white_trail() {
    gsr::WhiteTrail t;
    assert(!t.alive());
    t.add(10, 20, 40, 50);
    assert(t.count() == static_cast<std::size_t>(gsr::kTrailGlows));
    std::vector<BurstQuad> q;
    t.collect(&q);
    assert(q.size() == static_cast<std::size_t>(gsr::kTrailGlows));
    // Spread along the segment, the last glow at the current position.
    assert(std::fabs(q.back().x - 40.0f) < 1e-4f && std::fabs(q.back().y - 50.0f) < 1e-4f);
    assert(q[0].x > 10.0f && q[0].x < q[1].x && q[1].x < q[2].x);
    for (const auto& s : q) {
        assert(s.kind == BurstKind::Trail);
        assert(s.r == 1.0f && s.g == 1.0f && s.b == 1.0f);  // white
        assert(s.size >= gsr::kTrailSizeMin && s.size <= gsr::kTrailSizeMax);
        assert(std::fabs(s.a - gsr::kTrailAlpha) < 1e-6f);
    }
    // Still and fading to nothing over kTrailFrames.
    float prev = q[0].a;
    for (int i = 1; i < gsr::kTrailFrames; ++i) {
        t.step();
        q.clear();
        t.collect(&q);
        assert(q.size() == static_cast<std::size_t>(gsr::kTrailGlows));
        assert(q[0].x == 10.0f + 30.0f / 3.0f);  // no motion
        assert(q[0].a < prev);
        prev = q[0].a;
    }
    t.step();
    assert(!t.alive());
}

}  // namespace

int main() {
    test_constants();
    test_spawn_counts_and_order();
    test_chunks_and_geyser_launch_upward();
    test_deterministic();
    test_flash_timeline();
    test_dust_timeline();
    test_trails_and_fades();
    test_everything_ends();
    test_earth_toned();
    test_shake();
    test_white_trail();
    std::puts("effect_burst_test: ok");
    return 0;
}
