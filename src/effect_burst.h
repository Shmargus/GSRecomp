// effect_burst.h -- a deterministic dirt explosion, a white motion trail and a
// screen shake, drawn by our own renderer on top of the finished picture (used
// by the Earth Surge mod test, src/earth_surge.cpp). CPU simulation only;
// FieldSceneRenderer draws it.
//
// Everything is drawn with NORMAL alpha blending (never additive, so the earth
// colours stay earth-toned over any background). The explosion has a big
// impact flash, a dirt geyser thrown straight up, radial dirt chunks, dust
// clouds and grit sparks.
//
// Coordinates are output (view) pixels. The simulation advances once per
// presented frame. No rand(): fixed-seed LCGs, so a burst is reproducible.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gsr {

constexpr std::uint32_t kBurstSeed = 0x2545F491u;
// Every particle starts within +-this many pixels of the impact point.
constexpr float kBurstOriginSpread = 4.0f;

// ---- Radial dirt chunks ---------------------------------------------------------
constexpr int kBurstChunkCount = 96;
constexpr float kBurstChunkWideShare = 0.2f;    // these use the wide cone
constexpr float kBurstChunkConeDegrees = 70.0f; // about straight up (80%)
constexpr float kBurstChunkWideConeDegrees = 110.0f;  // the rest
constexpr float kBurstChunkSpeedMin = 6.0f;     // px / frame
constexpr float kBurstChunkSpeedMax = 14.0f;
constexpr float kBurstChunkDrag = 0.90f;        // velocity multiplier / frame
constexpr float kBurstChunkGravity = 0.25f;     // px / frame^2, downward
constexpr int kBurstChunkLifeMin = 34;
constexpr int kBurstChunkLifeMax = 56;
constexpr int kBurstChunkFadeFrames = 10;
constexpr int kBurstChunkSizeMin = 4;
constexpr int kBurstChunkSizeMax = 8;
constexpr float kBurstChunkAlpha = 0.95f;
// The hard blob's edge sits at 0.85 of the quad half-size (kSolidFragment).
constexpr float kBurstChunkQuadScale = 1.0f / 0.85f;
constexpr float kBurstChunkTrailSize = 0.85f;
constexpr float kBurstChunkTrailBrightness = 0.5f;  // one trail step
constexpr std::uint32_t kBurstChunkPalette[] = {0x5A3A1Eu, 0x7A5230u,
                                                0xA0703Cu, 0x8A6A45u};

// ---- Geyser: a column of dirt thrown straight up ---------------------------------
constexpr int kBurstGeyserCount = 36;
constexpr float kBurstGeyserConeDegrees = 15.0f;  // +- from straight up
constexpr float kBurstGeyserSpeedMin = 9.0f;
constexpr float kBurstGeyserSpeedMax = 15.0f;
constexpr float kBurstGeyserDrag = 0.93f;
constexpr float kBurstGeyserGravity = 0.30f;
constexpr int kBurstGeyserLifeMin = 36;
constexpr int kBurstGeyserLifeMax = 56;
constexpr int kBurstGeyserFadeFrames = 10;
constexpr int kBurstGeyserSizeMin = 4;
constexpr int kBurstGeyserSizeMax = 7;

// ---- Dust clouds: soft round glows that grow, drift and fade ---------------------
constexpr int kBurstDustCount = 22;
constexpr int kBurstDustFrames = 30;            // life; radius 6 -> 64
constexpr float kBurstDustRadiusStart = 6.0f;
constexpr float kBurstDustRadiusEnd = 64.0f;
constexpr float kBurstDustAlpha = 0.6f;         // fades linearly to 0
constexpr float kBurstDustSpeedMin = 0.6f;
constexpr float kBurstDustSpeedMax = 1.5f;
constexpr float kBurstDustUpwardBias = 0.4f;
constexpr float kBurstDustDrag = 0.96f;
constexpr std::uint32_t kBurstDustColour = 0x8A6A45u;

// ---- Grit sparks: small bright bits -----------------------------------------------
constexpr int kBurstGritCount = 48;
constexpr float kBurstGritSpeedMin = 8.0f;
constexpr float kBurstGritSpeedMax = 14.0f;
constexpr float kBurstGritDrag = 0.86f;
constexpr float kBurstGritGravity = 0.1f;
constexpr int kBurstGritLifeMin = 12;
constexpr int kBurstGritLifeMax = 20;
constexpr int kBurstGritFadeFrames = 4;
constexpr float kBurstGritSizeMin = 1.5f;
constexpr float kBurstGritSizeMax = 2.5f;
constexpr float kBurstGritGlowScale = 3.0f;     // quad side / grit size
constexpr float kBurstGritTrailSize = 0.8f;
constexpr float kBurstGritTrailBrightness = 0.5f;
constexpr std::uint32_t kBurstGritPalette[] = {0xD8B878u, 0xC8A060u};

// ---- Impact flash: soft round glow, grows, holds, then fades ----------------------
constexpr int kBurstFlashFrames = 14;           // gone at this age
constexpr int kBurstFlashGrowFrames = 4;        // size 12 -> 96 over these
constexpr int kBurstFlashHoldFrames = 2;        // then held at full size/alpha
constexpr float kBurstFlashSizeStart = 12.0f;
constexpr float kBurstFlashSizeEnd = 96.0f;
constexpr float kBurstFlashAlpha = 0.95f;       // peak
constexpr std::uint32_t kBurstFlashColourStart = 0xFFF0C8u;  // 0xRRGGBB
constexpr std::uint32_t kBurstFlashColourEnd = 0xE8C888u;    // by the end

// ---- Placement -----------------------------------------------------------------
constexpr float kBurstOffsetY = 12.0f;  // spawn this many view px below the hit

// ---- Screen shake ----------------------------------------------------------------
constexpr float kShakeImpactAmplitude = 6.0f;   // px, at the moment of impact
constexpr int kShakeImpactFrames = 26;          // decays linearly to 0
constexpr float kShakeYFactor = 0.6f;           // y amplitude relative to x
constexpr float kShakeJitterMin = 0.5f;         // offset = sign * amp * [min,1]
constexpr std::uint32_t kShakeSeed = 0x9E3779B1u;

// ---- White motion trail (the jump) -------------------------------------------------
constexpr int kTrailGlows = 3;                  // spread along each segment
constexpr float kTrailSizeMin = 14.0f;          // px, first .. last glow
constexpr float kTrailSizeMax = 20.0f;
constexpr float kTrailAlpha = 0.45f;            // fades linearly to 0
constexpr int kTrailFrames = 12;
constexpr std::uint32_t kTrailColour = 0xFFFFFFu;

// What a quad is, which decides the shader shape. Declaration order is draw
// order (trail behind, flash on top). All are alpha blended.
enum class BurstKind { Trail, Dust, Chunk, Geyser, Grit, Flash };

// One quad to draw: centre x,y, quad side length, colour and alpha, 0..1.
struct BurstQuad {
    BurstKind kind = BurstKind::Chunk;
    float x = 0, y = 0, size = 0;
    float r = 0, g = 0, b = 0, a = 0;
};

class EffectBurst {
public:
    // Replaces any burst in progress.
    void spawn(float x, float y);
    // Advances one presented frame.
    void step();
    bool alive() const { return flash_age_ < kBurstFlashFrames || live_ > 0; }
    // Appends the quads to draw this frame: dust, chunks, geyser, grit, flash.
    void collect(std::vector<BurstQuad>* out) const;
    void clear();
    int live_particles() const { return live_; }
    // Particles of a kind currently moving up the screen (negative vy); right
    // after spawn this is the launch direction share.
    int moving_up(BurstKind kind) const;

private:
    struct Particle {
        BurstKind kind = BurstKind::Chunk;
        float x = 0, y = 0, vx = 0, vy = 0;
        float trail_x = 0, trail_y = 0;  // previous position
        int age = 0, life = 0;
        float size = 0;
        std::uint32_t colour = 0;
    };
    std::uint32_t next();
    float unit();  // [0,1)
    void add_particles(BurstKind kind, int count, float x, float y);

    std::vector<Particle> particles_;
    int live_ = 0;
    int flash_age_ = kBurstFlashFrames;  // done at kBurstFlashFrames
    float origin_x_ = 0, origin_y_ = 0;
    std::uint32_t rng_ = kBurstSeed;
};

// A screen shake: the whole finished picture (and the burst) is offset by a
// small deterministic jitter that alternates sign every frame, its amplitude
// decaying linearly to nothing. Advance once per presented frame.
class ScreenShake {
public:
    // Restarts the shake; the first offset is available immediately.
    void start(float amplitude = kShakeImpactAmplitude,
               int frames = kShakeImpactFrames);
    void step();
    bool active() const { return amplitude_ > 0.0f; }
    float amplitude() const { return amplitude_; }  // current, px
    float dx() const { return dx_; }                // view pixels
    float dy() const { return dy_; }
    // Uniform scale about the view centre that keeps the shifted picture
    // covering the whole view: 1 + 2 * max offset / size, per axis, the larger.
    float cover_scale(int width, int height) const;

private:
    void refresh();
    float impact_amp_ = 0;
    int impact_frames_ = 0, age_ = 0;
    float amplitude_ = 0, dx_ = 0, dy_ = 0;
    int frame_ = 0;
    std::uint32_t rng_ = kShakeSeed;
};

// The white trail Isaac leaves during the jump: soft white glows dropped along
// the path between frames, standing still and fading.
class WhiteTrail {
public:
    // Drops kTrailGlows glows along the segment (x0,y0) -> (x1,y1), the last
    // at (x1,y1).
    void add(float x0, float y0, float x1, float y1);
    void step();
    bool alive() const { return !glows_.empty(); }
    void collect(std::vector<BurstQuad>* out) const;
    void clear() { glows_.clear(); }
    std::size_t count() const { return glows_.size(); }

private:
    struct Glow {
        float x = 0, y = 0, size = 0;
        int age = 0;
    };
    std::vector<Glow> glows_;
};

}  // namespace gsr
