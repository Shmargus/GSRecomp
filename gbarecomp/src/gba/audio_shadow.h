// audio_shadow.h — engine-agnostic HLE-shadow differential verifier.
//
// This is the gate that makes the audio shadow mixer SAFE per PRINCIPLES.md
// ("Verified-enhancement HLE is permitted"). An engine shadow (MP2K, …)
// renders voices in float AND a canon-domain check copy; this class decides,
// from the running hardware mix (the canon FIFO stream), whether the shadow
// is proven enough to substitute and reacts the instant it stops matching:
//
//   - envelope correlation + level-ratio self-check vs the canon stream
//   - probation auto-gain calibration (driver revisions scale the mixer)
//   - prove-then-substitute, strike-then-pause, escalating re-prove
//
// The shadow NEVER becomes the verify oracle and reverts loudly (the caller
// logs DEGRADED) on divergence. If this verifier is wrong, the worst case is
// "we keep playing the recompiled hardware mix" — it cannot corrupt output.
//
// ── Attribution ───────────────────────────────────────────────────
// Ported from JRickey/gba-recomp (crates/gba-core/src/shadow.rs), © Jrickey,
// MIT OR Apache-2.0, used with permission. See THIRD_PARTY_ATTRIBUTION.md.

#pragma once

#include <cstdint>
#include <cstddef>
#include <deque>
#include <string>
#include <vector>

namespace gba {

// One window's outcome, for engine-specific behavior search.
enum class Judgement {
    None,  // mid-window, or canon silent (no verdict)
    Pass,  // window passed all gates
    Fail,  // window failed (see structure_at_sane_level)
};

// Signed producer-block comparison.  The native MP2K path must be checked
// against the newly written guest PCM block, not a delayed host FIFO.  Keep
// this helper independent of the emulator so synthetic fixtures can cover
// polarity, channel order, clipping, and source switching without ROM data.
struct SignedStereoMetrics {
    bool pass = false;
    float correlation = 0.0f;
    float level_ratio = 0.0f;
    float mean_abs_error = 0.0f;
};

SignedStereoMetrics compare_signed_stereo_block(
    const int8_t* guest_left, const int8_t* guest_right,
    const float* native_left, const float* native_right, std::size_t count);

// Timeline source handoff policy.  A source change is keyed to an absolute
// producer/sample sequence and blends only samples that exist in both
// timelines, so a verifier transition cannot replay or discard safety data.
class AudioSourceTimeline {
public:
    enum class Source { Canonical, Native };

    void reset();
    void request(Source source, uint64_t sequence);
    Source source_at(uint64_t sequence) const;
    float blend_weight(uint64_t sequence) const;

private:
    Source source_ = Source::Canonical;
    uint64_t switch_sequence_ = 0;
};

// Aggregate timeline evidence exposed through the existing audio_state TCP
// command. This is bounded observability, not a per-sample side channel.
struct AudioTimelineStats {
    uint64_t published = 0;
    uint64_t matched = 0;
    uint64_t releases = 0;
    uint64_t unmatched = 0;
    uint64_t wraps = 0;
    uint64_t overwrites = 0;
    uint64_t sequence_gaps = 0;
    uint64_t resampler_resets = 0;
    uint64_t resampler_pushes = 0;
    uint64_t resampler_underruns = 0;
    uint64_t late_blocks = 0;
    uint64_t last_sequence = UINT64_MAX;
    uint64_t last_release_sequence = UINT64_MAX;
    uint64_t last_host_cursor = 0;
    uint64_t last_phase_q32 = 0;
    uint32_t dma_queue = 0;
    uint32_t dma_queue_max = 0;
    uint32_t resampler_queue = 0;
    uint32_t resampler_queue_max = 0;
};

// Sequence-preserving conversion from the guest producer cadence to the host
// audio grid. Blocks carry the absolute Q32 host timestamp measured at the
// DMA-consumer boundary; later blocks therefore preserve variable FIFO delay
// instead of inheriting a guessed frame cadence.
class ProducerHostResampler {
public:
    void reset();
    bool push(uint64_t sequence, uint64_t start_host_q32,
              uint32_t producer_rate, uint32_t host_rate,
              const float* route_a, const float* route_b,
              std::size_t count);
    bool sample(uint64_t host_sample, float& route_a, float& route_b);
    struct Diagnostic {
        bool started = false;
        uint64_t host_sample = 0;
        uint32_t queued_blocks = 0;
        uint64_t current_sequence = UINT64_MAX;
        uint64_t last_sequence = UINT64_MAX;
        uint64_t current_start_q32 = 0;
        uint64_t current_end_q32 = 0;
        uint64_t current_step_q32 = 0;
        uint64_t current_phase_q32 = 0;
        int64_t cursor_offset_q32 = 0;
        AudioTimelineStats stats{};
    };
    Diagnostic diagnostic(uint64_t host_sample) const;
    bool started() const { return started_; }
    uint64_t underruns() const { return underruns_; }
    const AudioTimelineStats& stats() const { return stats_; }

private:
    struct Block {
        uint64_t sequence = 0;
        uint64_t start_q32 = 0;
        uint64_t step_q32 = 0;
        std::vector<float> route_a;
        std::vector<float> route_b;
    };
    std::deque<Block> blocks_;
    uint64_t last_sequence_ = UINT64_MAX;
    bool started_ = false;
    uint64_t underruns_ = 0;
    AudioTimelineStats stats_{};
};

struct ProducerDmaRelease {
    uint64_t sequence = UINT64_MAX;
    uint64_t start_host_q32 = 0;
};

// A bounded explanation for the first real source association miss in an
// epoch. Continuation bytes inside an already released range are expected and
// never populate this record.
enum class ProducerDmaMissKind : uint8_t {
    None = 0,
    OutsidePublishedRange = 1,
    RouteMismatch = 2,
};

struct ProducerDmaMiss {
    bool valid = false;
    ProducerDmaMissKind kind = ProducerDmaMissKind::None;
    uint8_t route = 0;
    uint32_t source_addr = 0;
    uint64_t sequence = UINT64_MAX;
    uint32_t range_start = 0;
    uint32_t range_end = 0;
    uint32_t offset = 0;
};

// Associates completed producer sequences with their two rotating DMA source
// ranges. A sequence becomes host-visible only when the FIFO consumes a byte
// from that range; the consumed byte index fixes the block's absolute phase.
class ProducerDmaTimeline {
public:
    void reset();
    bool publish(uint64_t sequence, uint32_t route_a_addr,
                 uint32_t route_b_addr, std::size_t count,
                 uint32_t producer_rate, uint32_t host_rate);
    bool consume(uint8_t route, uint32_t source_addr, uint64_t host_sample,
                 ProducerDmaRelease& release);
    const AudioTimelineStats& stats() const { return stats_; }
    const ProducerDmaMiss& first_unmatched() const {
        return first_unmatched_;
    }
    // Clear only the one-record epoch diagnostic; cumulative counters and
    // published ranges remain intact until the normal timeline reset.
    void reset_epoch_diagnostics() { first_unmatched_ = {}; }

private:
    struct Block {
        uint64_t sequence = UINT64_MAX;
        uint32_t route_addr[2]{};
        std::size_t count = 0;
        uint64_t step_q32 = 0;
        bool released = false;
    };
    std::deque<Block> blocks_;
    uint64_t last_sequence_ = UINT64_MAX;
    uint32_t last_source_addr_[2] = {};
    bool have_source_addr_[2] = {};
    AudioTimelineStats stats_{};
    ProducerDmaMiss first_unmatched_{};
};

// Differential self-check: shadow mix vs the canon FIFO DAC stream. We police
// STRUCTURE (same notes, same loudness, same time), not raw samples: both
// streams are rectified, lowpassed to envelopes, decimated, and Pearson-
// correlated over a coarse lag range covering the driver's pipeline delay.
class ShadowSelfCheck {
public:
    // Feed one grid sample of (canon, shadow) stereo pairs. Returns true at
    // window ends that produced a verdict, filling best_r and level_ratio.
    bool push(float canon_l, float canon_r, float shadow_l, float shadow_r,
              float& best_r, float& level_ratio);

    void reset();
    void reset_strikes() { strikes_ = 0; }
    uint32_t strikes() const { return strikes_; }
    void add_strike() { ++strikes_; }
    void sub_strike() { if (strikes_) --strikes_; }

private:
    float lp_x_l_ = 0, lp_x_r_ = 0;  // canon envelope follower (per side)
    float lp_y_l_ = 0, lp_y_r_ = 0;  // shadow envelope follower
    std::vector<float> ex_l_, ex_r_; // decimated canon envelope
    std::vector<float> ey_l_, ey_r_; // decimated shadow envelope
    uint32_t phase_ = 0;
    uint32_t strikes_ = 0;
};

// The engine-agnostic verification state machine.
class ShadowVerifier {
public:
    // Calibrated output gain (1.0 = stock scale). Engines multiply their
    // voice sum by this in BOTH the output and the check copy.
    float gain() const { return gain_; }
    bool  proven() const { return proven_; }
    uint64_t pauses() const { return pauses_; }
    uint32_t probation_failures() const { return probation_failures_; }
    float last_r() const { return last_r_; }
    float last_ratio() const { return last_ratio_; }
    // True if the most recent verdict was a Fail whose correlation broke
    // while the level was in-band (the semantics-mismatch signature an
    // engine can search over). Only meaningful right after judge()==Fail.
    bool last_fail_structural() const { return fail_structural_; }
    // Set on each pause with the reason; caller surfaces it (DEGRADED) then
    // clears. Empty = none.
    std::string take_reverted() { std::string s = reverted_; reverted_.clear(); return s; }

    // Feed one grid sample of canonical and shadow-check pairs in the same
    // normalized output domain. Drives calibration, proving, strikes, pauses.
    Judgement judge(float canon_l, float canon_r, float chk_l, float chk_r);

    // Signed producer-block evidence is the primary gate. Envelope evidence
    // remains useful for diagnostics, but can never prove a native stream by
    // itself.
    void set_signed_producer_gate(bool seen, bool pass, float correlation,
                                  float level_ratio);
    bool signed_gate_seen() const { return signed_gate_seen_; }
    bool signed_gate_passed() const { return signed_gate_passed_; }

    // Savestate loads start a new guest audio timeline.
    void reset();

private:
    void pause(std::string reason);

    ShadowSelfCheck check_{};
    float gain_ = 1.0f;
    bool  calibrated_ = false;
    float ratio_hist_[3] = {0, 0, 0};
    uint32_t ratio_n_ = 0;
    bool  proven_ = false;
    // One passing envelope window is not enough to hand a live host stream
    // over to a native renderer. Require two adjacent windows so a quiet
    // transition cannot prove an unsupported driver variant.
    uint32_t prove_need_ = 2;
    uint32_t consec_pass_ = 0;
    uint32_t probation_failures_ = 0;
    uint64_t pauses_ = 0;
    std::string reverted_;
    float last_r_ = 0;
    float last_ratio_ = 0;
    bool  fail_structural_ = false;
    bool  signed_gate_seen_ = false;
    bool  signed_gate_passed_ = false;
    float signed_correlation_ = 0.0f;
    float signed_level_ratio_ = 0.0f;
};

}  // namespace gba
