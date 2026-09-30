#include "effect_burst.h"

#include <algorithm>
#include <cmath>

namespace gsr {
namespace {
constexpr float kPi = 3.14159265358979f;

void set_rgb(std::uint32_t c, BurstQuad* q) {
    q->r = static_cast<float>((c >> 16) & 0xFFu) / 255.0f;
    q->g = static_cast<float>((c >> 8) & 0xFFu) / 255.0f;
    q->b = static_cast<float>(c & 0xFFu) / 255.0f;
}

float channel(std::uint32_t c, int shift) {
    return static_cast<float>((c >> shift) & 0xFFu) / 255.0f;
}

// Everything that differs between the simulated kinds.
struct Params {
    float drag, gravity;
    int fade_frames;
    float alpha;          // peak alpha
    float trail_size, trail_brightness;
};

Params params_for(BurstKind k) {
    switch (k) {
    case BurstKind::Dust:
        return {kBurstDustDrag, 0.0f, 0, kBurstDustAlpha, 0.0f, 0.0f};
    case BurstKind::Grit:
        return {kBurstGritDrag, kBurstGritGravity, kBurstGritFadeFrames, 1.0f,
                kBurstGritTrailSize, kBurstGritTrailBrightness};
    case BurstKind::Geyser:
        return {kBurstGeyserDrag, kBurstGeyserGravity, kBurstGeyserFadeFrames,
                kBurstChunkAlpha, kBurstChunkTrailSize,
                kBurstChunkTrailBrightness};
    default:
        return {kBurstChunkDrag, kBurstChunkGravity, kBurstChunkFadeFrames,
                kBurstChunkAlpha, kBurstChunkTrailSize,
                kBurstChunkTrailBrightness};
    }
}
}  // namespace

std::uint32_t EffectBurst::next() {
    rng_ = rng_ * 1664525u + 1013904223u;
    return rng_;
}

float EffectBurst::unit() {
    return static_cast<float>(next() >> 8) / 16777216.0f;
}

void EffectBurst::clear() {
    particles_.clear();
    live_ = 0;
    flash_age_ = kBurstFlashFrames;
}

void EffectBurst::add_particles(BurstKind kind, int count, float x, float y) {
    for (int i = 0; i < count; ++i) {
        Particle p;
        p.kind = kind;
        p.x = x + (unit() * 2.0f - 1.0f) * kBurstOriginSpread;
        p.y = y + (unit() * 2.0f - 1.0f) * kBurstOriginSpread;
        p.trail_x = p.x;
        p.trail_y = p.y;
        if (kind == BurstKind::Chunk || kind == BurstKind::Geyser) {
            // An angle from straight up (screen y grows downward). Chunks use
            // the narrow cone for most and the wide one for the rest; the
            // geyser stays in its own tight cone.
            const bool geyser = kind == BurstKind::Geyser;
            const float cone =
                geyser ? kBurstGeyserConeDegrees
                       : (unit() < kBurstChunkWideShare
                              ? kBurstChunkWideConeDegrees
                              : kBurstChunkConeDegrees);
            const float theta = (unit() * 2.0f - 1.0f) * cone * kPi / 180.0f;
            const float speed_min =
                geyser ? kBurstGeyserSpeedMin : kBurstChunkSpeedMin;
            const float speed_max =
                geyser ? kBurstGeyserSpeedMax : kBurstChunkSpeedMax;
            const float speed = speed_min + unit() * (speed_max - speed_min);
            const int life_min =
                geyser ? kBurstGeyserLifeMin : kBurstChunkLifeMin;
            const int life_max =
                geyser ? kBurstGeyserLifeMax : kBurstChunkLifeMax;
            const int size_min =
                geyser ? kBurstGeyserSizeMin : kBurstChunkSizeMin;
            const int size_max =
                geyser ? kBurstGeyserSizeMax : kBurstChunkSizeMax;
            p.vx = std::sin(theta) * speed;
            p.vy = -std::cos(theta) * speed;
            p.life = life_min + static_cast<int>(next() % (life_max - life_min + 1));
            p.size = static_cast<float>(
                size_min + static_cast<int>(next() % (size_max - size_min + 1)));
            p.colour = kBurstChunkPalette[(next() >> 4) %
                                          (sizeof kBurstChunkPalette /
                                           sizeof kBurstChunkPalette[0])];
        } else if (kind == BurstKind::Dust) {
            const float angle = unit() * 2.0f * kPi;
            const float speed = kBurstDustSpeedMin +
                                unit() * (kBurstDustSpeedMax - kBurstDustSpeedMin);
            p.vx = std::cos(angle) * speed;
            p.vy = std::sin(angle) * speed - kBurstDustUpwardBias;
            p.life = kBurstDustFrames;
            p.size = kBurstDustRadiusStart;  // radius, grown in collect()
            p.colour = kBurstDustColour;
        } else {  // Grit
            const float angle = unit() * 2.0f * kPi;
            const float speed = kBurstGritSpeedMin +
                                unit() * (kBurstGritSpeedMax - kBurstGritSpeedMin);
            p.vx = std::cos(angle) * speed;
            p.vy = std::sin(angle) * speed;
            p.life = kBurstGritLifeMin +
                     static_cast<int>(next() % (kBurstGritLifeMax -
                                                kBurstGritLifeMin + 1));
            p.size = kBurstGritSizeMin +
                     unit() * (kBurstGritSizeMax - kBurstGritSizeMin);
            p.colour = kBurstGritPalette[(next() >> 4) %
                                         (sizeof kBurstGritPalette /
                                          sizeof kBurstGritPalette[0])];
        }
        particles_.push_back(p);
    }
}

void EffectBurst::spawn(float x, float y) {
    clear();
    rng_ = kBurstSeed;  // same seed every burst: reproducible
    origin_x_ = x;
    origin_y_ = y;
    flash_age_ = 0;
    const int total = kBurstDustCount + kBurstChunkCount + kBurstGeyserCount +
                      kBurstGritCount;
    particles_.reserve(total);
    add_particles(BurstKind::Dust, kBurstDustCount, x, y);
    add_particles(BurstKind::Chunk, kBurstChunkCount, x, y);
    add_particles(BurstKind::Geyser, kBurstGeyserCount, x, y);
    add_particles(BurstKind::Grit, kBurstGritCount, x, y);
    live_ = total;
}

int EffectBurst::moving_up(BurstKind kind) const {
    int n = 0;
    for (const Particle& p : particles_)
        if (p.kind == kind && p.age < p.life && p.vy < 0.0f) ++n;
    return n;
}

void EffectBurst::step() {
    if (flash_age_ < kBurstFlashFrames) ++flash_age_;
    int live = 0;
    for (Particle& p : particles_) {
        if (p.age >= p.life) continue;
        const Params k = params_for(p.kind);
        p.trail_x = p.x;
        p.trail_y = p.y;
        p.vx *= k.drag;
        p.vy = p.vy * k.drag + k.gravity;
        p.x += p.vx;
        p.y += p.vy;
        ++p.age;
        if (p.age < p.life) ++live;
    }
    live_ = live;
    if (live_ == 0 && flash_age_ >= kBurstFlashFrames) particles_.clear();
}

void EffectBurst::collect(std::vector<BurstQuad>* out) const {
    if (!out) return;
    // Dust, chunks, geyser, grit (in that order), then the flash on top.
    for (const BurstKind kind : {BurstKind::Dust, BurstKind::Chunk,
                                 BurstKind::Geyser, BurstKind::Grit}) {
        const Params k = params_for(kind);
        for (const Particle& p : particles_) {
            if (p.kind != kind || p.age >= p.life) continue;
            BurstQuad q;
            q.kind = kind;
            set_rgb(p.colour, &q);
            q.x = p.x;
            q.y = p.y;
            if (kind == BurstKind::Dust) {
                const float t = static_cast<float>(p.age) /
                                static_cast<float>(p.life);
                q.size = 2.0f * (kBurstDustRadiusStart +
                                 t * (kBurstDustRadiusEnd - kBurstDustRadiusStart));
                q.a = k.alpha * (1.0f - t);
                out->push_back(q);
                continue;
            }
            const int left = p.life - p.age;
            const float fade = left >= k.fade_frames
                                   ? 1.0f
                                   : static_cast<float>(left) /
                                         static_cast<float>(k.fade_frames);
            const float body = kind == BurstKind::Grit
                                   ? p.size * kBurstGritGlowScale
                                   : p.size * kBurstChunkQuadScale;
            if (p.age >= 1) {  // one trail step behind
                BurstQuad tq = q;
                tq.x = p.trail_x;
                tq.y = p.trail_y;
                tq.size = body * k.trail_size;
                tq.a = k.alpha * fade * k.trail_brightness;
                out->push_back(tq);
            }
            q.size = body;
            q.a = k.alpha * fade;
            out->push_back(q);
        }
    }
    if (flash_age_ < kBurstFlashFrames) {
        const int full = kBurstFlashGrowFrames + kBurstFlashHoldFrames;
        const float grow =
            static_cast<float>(std::min(flash_age_, kBurstFlashGrowFrames)) /
            static_cast<float>(kBurstFlashGrowFrames);
        const float t = static_cast<float>(flash_age_) /
                        static_cast<float>(kBurstFlashFrames);
        BurstQuad q;
        q.kind = BurstKind::Flash;
        q.x = origin_x_;
        q.y = origin_y_;
        q.size = kBurstFlashSizeStart +
                 grow * (kBurstFlashSizeEnd - kBurstFlashSizeStart);
        q.r = channel(kBurstFlashColourStart, 16) * (1.0f - t) +
              channel(kBurstFlashColourEnd, 16) * t;
        q.g = channel(kBurstFlashColourStart, 8) * (1.0f - t) +
              channel(kBurstFlashColourEnd, 8) * t;
        q.b = channel(kBurstFlashColourStart, 0) * (1.0f - t) +
              channel(kBurstFlashColourEnd, 0) * t;
        q.a = flash_age_ <= full
                  ? kBurstFlashAlpha
                  : kBurstFlashAlpha *
                        static_cast<float>(kBurstFlashFrames - flash_age_) /
                        static_cast<float>(kBurstFlashFrames - full);
        out->push_back(q);
    }
}

// ---- ScreenShake ---------------------------------------------------------------

void ScreenShake::start(float amplitude, int frames) {
    impact_amp_ = amplitude;
    impact_frames_ = frames;
    age_ = 0;
    refresh();
}

void ScreenShake::step() {
    if (age_ < impact_frames_) ++age_;
    refresh();
}

void ScreenShake::refresh() {
    float amp = 0.0f;
    if (impact_frames_ > 0 && age_ < impact_frames_)
        amp = impact_amp_ * (1.0f - static_cast<float>(age_) /
                                        static_cast<float>(impact_frames_));
    amplitude_ = amp;
    if (amp <= 0.0f) {
        dx_ = dy_ = 0.0f;
        return;
    }
    auto unit = [this] {
        rng_ = rng_ * 1664525u + 1013904223u;
        return static_cast<float>(rng_ >> 8) / 16777216.0f;
    };
    const float sign = (frame_++ & 1) ? -1.0f : 1.0f;
    const float jx = kShakeJitterMin + (1.0f - kShakeJitterMin) * unit();
    const float jy = kShakeJitterMin + (1.0f - kShakeJitterMin) * unit();
    dx_ = sign * amp * jx;
    dy_ = -sign * amp * kShakeYFactor * jy;
}

float ScreenShake::cover_scale(int width, int height) const {
    if (amplitude_ <= 0.0f || width <= 0 || height <= 0) return 1.0f;
    const float sx = 2.0f * amplitude_ / static_cast<float>(width);
    const float sy = 2.0f * amplitude_ * kShakeYFactor / static_cast<float>(height);
    return 1.0f + (sx > sy ? sx : sy);
}

// ---- WhiteTrail ------------------------------------------------------------------

void WhiteTrail::add(float x0, float y0, float x1, float y1) {
    for (int i = 0; i < kTrailGlows; ++i) {
        const float t = static_cast<float>(i + 1) / static_cast<float>(kTrailGlows);
        Glow g;
        g.x = x0 + (x1 - x0) * t;
        g.y = y0 + (y1 - y0) * t;
        g.size = kTrailGlows > 1
                     ? kTrailSizeMin + (kTrailSizeMax - kTrailSizeMin) *
                                           static_cast<float>(i) /
                                           static_cast<float>(kTrailGlows - 1)
                     : kTrailSizeMax;
        glows_.push_back(g);
    }
}

void WhiteTrail::step() {
    for (Glow& g : glows_) ++g.age;
    glows_.erase(std::remove_if(glows_.begin(), glows_.end(),
                                [](const Glow& g) { return g.age >= kTrailFrames; }),
                 glows_.end());
}

void WhiteTrail::collect(std::vector<BurstQuad>* out) const {
    if (!out) return;
    for (const Glow& g : glows_) {
        BurstQuad q;
        q.kind = BurstKind::Trail;
        set_rgb(kTrailColour, &q);
        q.x = g.x;
        q.y = g.y;
        q.size = g.size;
        q.a = kTrailAlpha * (1.0f - static_cast<float>(g.age) /
                                        static_cast<float>(kTrailFrames));
        out->push_back(q);
    }
}

}  // namespace gsr
