// audio_shadow.cpp — see audio_shadow.h.
//
// Ported from JRickey/gba-recomp (crates/gba-core/src/shadow.rs), © Jrickey,
// MIT OR Apache-2.0, used with permission. See THIRD_PARTY_ATTRIBUTION.md.

#include "audio_shadow.h"

#include <cmath>
#include <cstdio>
#include <algorithm>
#include <limits>

namespace gba {
namespace {

constexpr uint32_t kDecim        = 64;     // one envelope entry per ~1 ms
constexpr std::size_t kWindow    = 1024;   // ~1 s window
constexpr std::size_t kMaxLag    = 56;     // ~3.5 driver frames, step 2
constexpr float kEnvAlpha        = 0.0075f;
constexpr double kMinLevel       = 0.002;
constexpr float kMinR            = 0.5f;
constexpr uint32_t kMaxStrikes   = 3;

template <typename T>
void saturating_increment(T& value) {
    if (value != std::numeric_limits<T>::max()) ++value;
}

double mean_of(const std::vector<float>& v) {
    double s = 0;
    for (float x : v) s += x;
    return v.empty() ? 0.0 : s / static_cast<double>(v.size());
}

}  // namespace

SignedStereoMetrics compare_signed_stereo_block(
    const int8_t* guest_left, const int8_t* guest_right,
    const float* native_left, const float* native_right, std::size_t count) {
    SignedStereoMetrics out{};
    if (!guest_left || !guest_right || !native_left || !native_right ||
        count < 8) {
        return out;
    }

    double dot = 0.0;
    double guest_energy = 0.0;
    double native_energy = 0.0;
    double abs_error = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        const double gl = static_cast<double>(guest_left[i]) / 128.0;
        const double gr = static_cast<double>(guest_right[i]) / 128.0;
        const double nl = std::clamp(static_cast<double>(native_left[i]),
                                     -1.0, 127.0 / 128.0);
        const double nr = std::clamp(static_cast<double>(native_right[i]),
                                     -1.0, 127.0 / 128.0);
        dot += gl * nl + gr * nr;
        guest_energy += gl * gl + gr * gr;
        native_energy += nl * nl + nr * nr;
        abs_error += std::fabs(gl - nl) + std::fabs(gr - nr);
    }
    const double denom = std::sqrt(guest_energy * native_energy);
    out.correlation = denom > 1e-12
        ? static_cast<float>(dot / denom) : 0.0f;
    out.level_ratio = guest_energy > 1e-12
        ? static_cast<float>(std::sqrt(native_energy / guest_energy)) : 0.0f;
    out.mean_abs_error = static_cast<float>(
        abs_error / static_cast<double>(count * 2));
    // Keep this gate deliberately signed and conservative.  A polarity flip
    // or A/B swap has a negative/low dot product and cannot pass.
    out.pass = out.correlation >= 0.90f &&
               out.level_ratio >= 0.70f && out.level_ratio <= 1.30f &&
               out.mean_abs_error <= 0.18f;
    return out;
}

void AudioSourceTimeline::reset() {
    source_ = Source::Canonical;
    switch_sequence_ = 0;
}

void AudioSourceTimeline::request(Source source, uint64_t sequence) {
    source_ = source;
    switch_sequence_ = sequence;
}

AudioSourceTimeline::Source AudioSourceTimeline::source_at(
    uint64_t sequence) const {
    if (sequence < switch_sequence_) return Source::Canonical;
    return source_;
}

float AudioSourceTimeline::blend_weight(uint64_t sequence) const {
    // A short, deterministic 32-sample transition avoids a click while the
    // caller retains canonical samples as the safety source.  The endpoint
    // is exact, so steady-state source selection is not attenuated.
    if (sequence < switch_sequence_) return 0.0f;
    const uint64_t delta = sequence - switch_sequence_;
    return delta >= 32 ? 1.0f : static_cast<float>(delta + 1) / 32.0f;
}

void ProducerHostResampler::reset() {
    if (!blocks_.empty() || started_ || last_sequence_ != UINT64_MAX)
        ++stats_.resampler_resets;
    blocks_.clear();
    last_sequence_ = UINT64_MAX;
    started_ = false;
    underruns_ = 0;
}

bool ProducerHostResampler::push(uint64_t sequence, uint64_t start_host_q32,
                                 uint32_t producer_rate, uint32_t host_rate,
                                 const float* route_a, const float* route_b,
                                 std::size_t count) {
    if (!route_a || !route_b || count < 2 || producer_rate == 0 ||
        host_rate == 0) return false;
    if (last_sequence_ != UINT64_MAX && sequence != last_sequence_ + 1u) {
        saturating_increment(stats_.sequence_gaps);
        reset();
    }
    Block block{};
    block.sequence = sequence;
    block.step_q32 = (static_cast<uint64_t>(host_rate) << 32) /
                     producer_rate;
    block.start_q32 = start_host_q32;
    block.route_a.assign(route_a, route_a + count);
    block.route_b.assign(route_b, route_b + count);
    last_sequence_ = sequence;
    blocks_.push_back(std::move(block));
    ++stats_.resampler_pushes;
    stats_.resampler_queue = static_cast<uint32_t>(blocks_.size());
    stats_.resampler_queue_max = std::max(
        stats_.resampler_queue_max, stats_.resampler_queue);
    stats_.last_sequence = sequence;
    return true;
}

void ProducerDmaTimeline::reset() {
    blocks_.clear();
    last_sequence_ = UINT64_MAX;
    last_source_addr_[0] = last_source_addr_[1] = 0;
    have_source_addr_[0] = have_source_addr_[1] = false;
    stats_.dma_queue = 0;
}

bool ProducerDmaTimeline::publish(uint64_t sequence, uint32_t route_a_addr,
                                  uint32_t route_b_addr, std::size_t count,
                                  uint32_t producer_rate,
                                  uint32_t host_rate) {
    if (route_a_addr == 0 || route_b_addr == 0 || count < 2 ||
        producer_rate == 0 || host_rate == 0) return false;
    if (last_sequence_ != UINT64_MAX && sequence != last_sequence_ + 1u) {
        saturating_increment(stats_.sequence_gaps);
        reset();
    }
    Block block{};
    block.sequence = sequence;
    block.route_addr[0] = route_a_addr;
    block.route_addr[1] = route_b_addr;
    block.count = count;
    block.step_q32 = (static_cast<uint64_t>(host_rate) << 32) /
                     producer_rate;
    for (const Block& old : blocks_) {
        for (uint8_t route = 0; route < 2; ++route) {
            const uint64_t old_end = static_cast<uint64_t>(
                old.route_addr[route]) + old.count;
            const uint64_t new_end = static_cast<uint64_t>(
                block.route_addr[route]) + block.count;
            if (!old.released && old.route_addr[route] < new_end &&
                block.route_addr[route] < old_end) {
                ++stats_.overwrites;
                route = 2;
                break;
            }
        }
    }
    blocks_.push_back(block);
    last_sequence_ = sequence;
    ++stats_.published;
    stats_.dma_queue = static_cast<uint32_t>(blocks_.size());
    stats_.dma_queue_max = std::max(stats_.dma_queue_max, stats_.dma_queue);
    stats_.last_sequence = sequence;
    while (blocks_.size() > 32) blocks_.pop_front();
    return true;
}

bool ProducerDmaTimeline::consume(uint8_t route, uint32_t source_addr,
                                  uint64_t host_sample,
                                  ProducerDmaRelease& release) {
    release = {};
    if (route > 1) return false;
    if (have_source_addr_[route] && source_addr < last_source_addr_[route])
        ++stats_.wraps;
    have_source_addr_[route] = true;
    last_source_addr_[route] = source_addr;
    bool matched_released_range = false;
    for (Block& block : blocks_) {
        if (source_addr < block.route_addr[route]) continue;
        const uint64_t index = source_addr - block.route_addr[route];
        if (index >= block.count) continue;
        if (block.released) {
            matched_released_range = true;
            continue;
        }
        const uint64_t offset_q32 = index * block.step_q32;
        const uint64_t host_q32 = host_sample << 32;
        if (offset_q32 > host_q32) return false;
        block.released = true;
        release.sequence = block.sequence;
        release.start_host_q32 = host_q32 - offset_q32;
        saturating_increment(stats_.matched);
        ++stats_.releases;
        stats_.last_release_sequence = release.sequence;
        stats_.last_host_cursor = host_sample;
        stats_.dma_queue = static_cast<uint32_t>(blocks_.size());
        return true;
    }
    if (!matched_released_range) saturating_increment(stats_.unmatched);
    if (!matched_released_range && !first_unmatched_.valid) {
        first_unmatched_.valid = true;
        first_unmatched_.kind = ProducerDmaMissKind::OutsidePublishedRange;
        first_unmatched_.route = route;
        first_unmatched_.source_addr = source_addr;
        for (const Block& block : blocks_) {
            const uint8_t other_route = static_cast<uint8_t>(route ^ 1u);
            const uint64_t other_end = static_cast<uint64_t>(
                block.route_addr[other_route]) + block.count;
            if (source_addr >= block.route_addr[other_route] &&
                static_cast<uint64_t>(source_addr) < other_end) {
                first_unmatched_.kind = ProducerDmaMissKind::RouteMismatch;
                first_unmatched_.sequence = block.sequence;
                first_unmatched_.range_start =
                    block.route_addr[other_route];
                first_unmatched_.range_end = static_cast<uint32_t>(
                    std::min<uint64_t>(other_end, UINT32_MAX));
                first_unmatched_.offset = source_addr -
                    block.route_addr[other_route];
                break;
            }
        }
        if (first_unmatched_.kind ==
                ProducerDmaMissKind::OutsidePublishedRange &&
            !blocks_.empty()) {
            const Block& latest = blocks_.back();
            const uint64_t latest_end = static_cast<uint64_t>(
                latest.route_addr[route]) + latest.count;
            first_unmatched_.sequence = latest.sequence;
            first_unmatched_.range_start = latest.route_addr[route];
            first_unmatched_.range_end = static_cast<uint32_t>(
                std::min<uint64_t>(latest_end, UINT32_MAX));
            first_unmatched_.offset = source_addr >= latest.route_addr[route]
                ? source_addr - latest.route_addr[route] : 0;
        }
    }
    stats_.dma_queue = static_cast<uint32_t>(blocks_.size());
    return false;
}

bool ProducerHostResampler::sample(uint64_t host_sample, float& route_a,
                                   float& route_b) {
    route_a = route_b = 0.0f;
    const uint64_t now_q32 = host_sample << 32;
    while (blocks_.size() > 1 && blocks_[1].start_q32 <= now_q32)
        blocks_.pop_front();
    if (blocks_.empty()) return false;
    if (now_q32 < blocks_.front().start_q32) {
        saturating_increment(stats_.late_blocks);
        return false;
    }

    const Block& block = blocks_.front();
    const uint64_t position_q32 =
        ((now_q32 - block.start_q32) << 16) /
        (block.step_q32 >> 16);
    const std::size_t index = static_cast<std::size_t>(position_q32 >> 32);
    if (index >= block.route_a.size()) {
        if (blocks_.size() > 1 &&
            now_q32 < blocks_[1].start_q32)
            saturating_increment(stats_.late_blocks);
        if (started_) { ++underruns_; ++stats_.resampler_underruns; }
        return false;
    }
    const float fraction = static_cast<float>(position_q32 & 0xFFFFFFFFull) /
                           4294967296.0f;
    float next_a = block.route_a[index];
    float next_b = block.route_b[index];
    if (index + 1u < block.route_a.size()) {
        next_a = block.route_a[index + 1u];
        next_b = block.route_b[index + 1u];
    } else if (blocks_.size() > 1) {
        next_a = blocks_[1].route_a.front();
        next_b = blocks_[1].route_b.front();
    } else {
        if (started_) { ++underruns_; ++stats_.resampler_underruns; }
        return false;
    }
    route_a = block.route_a[index] +
              (next_a - block.route_a[index]) * fraction;
    route_b = block.route_b[index] +
              (next_b - block.route_b[index]) * fraction;
    stats_.last_host_cursor = host_sample;
    stats_.last_phase_q32 = position_q32;
    stats_.resampler_queue = static_cast<uint32_t>(blocks_.size());
    started_ = true;
    return true;
}

ProducerHostResampler::Diagnostic ProducerHostResampler::diagnostic(
    uint64_t host_sample) const {
    Diagnostic out{};
    out.started = started_;
    out.host_sample = host_sample;
    out.queued_blocks = static_cast<uint32_t>(blocks_.size());
    out.last_sequence = last_sequence_;
    out.stats = stats_;
    if (blocks_.empty()) return out;

    const Block& block = blocks_.front();
    out.current_sequence = block.sequence;
    out.current_start_q32 = block.start_q32;
    out.current_step_q32 = block.step_q32;
    const uint64_t frames = block.route_a.empty()
        ? 0u : static_cast<uint64_t>(block.route_a.size() - 1u);
    const bool span_overflows = frames != 0 &&
        block.step_q32 > std::numeric_limits<uint64_t>::max() / frames;
    const uint64_t span = span_overflows ? std::numeric_limits<uint64_t>::max()
                                         : block.step_q32 * frames;
    out.current_end_q32 = span_overflows ||
            block.start_q32 > std::numeric_limits<uint64_t>::max() - span
        ? std::numeric_limits<uint64_t>::max()
        : block.start_q32 + span;
    const uint64_t cursor_q32 = host_sample << 32;
    if (cursor_q32 >= block.start_q32) {
        out.cursor_offset_q32 = static_cast<int64_t>(std::min<uint64_t>(
            cursor_q32 - block.start_q32,
            static_cast<uint64_t>(std::numeric_limits<int64_t>::max())));
    } else {
        const uint64_t delta = block.start_q32 - cursor_q32;
        out.cursor_offset_q32 = delta >
                static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
            ? std::numeric_limits<int64_t>::min()
            : -static_cast<int64_t>(delta);
    }
    if (block.step_q32 != 0 && cursor_q32 >= block.start_q32) {
        const uint64_t delta = cursor_q32 - block.start_q32;
        const uint64_t step_hi = block.step_q32 >> 16;
        if (step_hi != 0) {
            const unsigned __int128 numerator =
                static_cast<unsigned __int128>(delta) << 16;
            out.current_phase_q32 = static_cast<uint64_t>(numerator / step_hi);
        }
    }
    return out;
}

void ShadowSelfCheck::reset() {
    lp_x_l_ = lp_x_r_ = 0.0f;
    lp_y_l_ = lp_y_r_ = 0.0f;
    ex_l_.clear();
    ex_r_.clear();
    ey_l_.clear();
    ey_r_.clear();
    phase_ = 0;
    strikes_ = 0;
}

bool ShadowSelfCheck::push(float canon_l, float canon_r, float shadow_l,
                           float shadow_r, float& best_r, float& level_ratio) {
    lp_x_l_ += kEnvAlpha * (std::fabs(canon_l) - lp_x_l_);
    lp_x_r_ += kEnvAlpha * (std::fabs(canon_r) - lp_x_r_);
    lp_y_l_ += kEnvAlpha * (std::fabs(shadow_l) - lp_y_l_);
    lp_y_r_ += kEnvAlpha * (std::fabs(shadow_r) - lp_y_r_);
    if (++phase_ < kDecim) return false;
    phase_ = 0;
    ex_l_.push_back(lp_x_l_);
    ex_r_.push_back(lp_x_r_);
    ey_l_.push_back(lp_y_l_);
    ey_r_.push_back(lp_y_r_);
    if (ex_l_.size() < kWindow) return false;

    const std::size_t n = kWindow - kMaxLag;
    double m0 = mean_of(ex_l_), m1 = mean_of(ex_r_);
    double canon_level  = (m0 + m1) / 2.0;
    double shadow_level = (mean_of(ey_l_) + mean_of(ey_r_)) / 2.0;

    bool have_verdict = false;
    if (canon_level >= kMinLevel) {
        float ratio = static_cast<float>(shadow_level / canon_level);
        // Does the canon envelope carry structure this window (vs a held
        // constant amplitude)?
        double canon_var = 0;
        for (std::size_t i = 0; i < ex_l_.size(); ++i) {
            double a = ex_l_[i] - m0, b = ex_r_[i] - m1;
            canon_var += a * a + b * b;
        }
        canon_var /= static_cast<double>(ex_l_.size());

        float best = 0.0f;
        if (canon_var <= 1e-10) {
            // Flat canon: nothing to correlate — the ratio is the verdict.
            best = 1.0f;
        } else {
            bool any = false;
            for (std::size_t lag = 0; lag <= kMaxLag; lag += 2) {
                double sum = 0;
                int sides = 0;
                for (int side = 0; side < 2; ++side) {
                    const std::vector<float>& xv = side == 0 ? ex_l_ : ex_r_;
                    const std::vector<float>& yv = side == 0 ? ey_l_ : ey_r_;
                    // x = ex[kMaxLag .. kWindow); y = ey[kMaxLag-lag .. kWindow-lag).
                    double mx = 0, my = 0;
                    for (std::size_t i = 0; i < n; ++i) {
                        mx += xv[kMaxLag + i];
                        my += yv[kMaxLag - lag + i];
                    }
                    mx /= static_cast<double>(n);
                    my /= static_cast<double>(n);
                    double cov = 0, vx = 0, vy = 0;
                    for (std::size_t i = 0; i < n; ++i) {
                        double a = xv[kMaxLag + i] - mx;
                        double b = yv[kMaxLag - lag + i] - my;
                        cov += a * b;
                        vx += a * a;
                        vy += b * b;
                    }
                    if (vx > 1e-12 && vy > 1e-12) {
                        sum += cov / std::sqrt(vx * vy);
                        ++sides;
                    }
                }
                if (sides > 0) {
                    float r = static_cast<float>(sum / sides);
                    if (!any || r > best) best = r;
                    any = true;
                }
            }
            if (!any) best = 0.0f;
        }
        best_r = best;
        level_ratio = ratio;
        have_verdict = true;
    }

    ex_l_.clear(); ex_r_.clear();
    ey_l_.clear(); ey_r_.clear();
    return have_verdict;
}

void ShadowVerifier::reset() {
    check_.reset();
    gain_ = 1.0f;
    calibrated_ = false;
    ratio_hist_[0] = ratio_hist_[1] = ratio_hist_[2] = 0.0f;
    ratio_n_ = 0;
    proven_ = false;
    prove_need_ = 2;
    consec_pass_ = 0;
    probation_failures_ = 0;
    pauses_ = 0;
    reverted_.clear();
    last_r_ = 0.0f;
    last_ratio_ = 0.0f;
    fail_structural_ = false;
    signed_gate_seen_ = false;
    signed_gate_passed_ = false;
    signed_correlation_ = 0.0f;
    signed_level_ratio_ = 0.0f;
}

void ShadowVerifier::set_signed_producer_gate(bool seen, bool pass,
                                               float correlation,
                                               float level_ratio) {
    signed_gate_seen_ = seen;
    signed_gate_passed_ = pass;
    signed_correlation_ = correlation;
    signed_level_ratio_ = level_ratio;
    if (seen && !pass && proven_) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "signed producer block mismatch (correlation %.3f, "
                      "level ratio %.3f)", correlation, level_ratio);
        pause(buf);
    }
}

void ShadowVerifier::pause(std::string reason) {
    proven_ = false;
    consec_pass_ = 0;
    check_.reset_strikes();
    prove_need_ = prove_need_ * 2 < 16 ? prove_need_ * 2 : 16;
    ++pauses_;
    reverted_ = std::move(reason);
}

Judgement ShadowVerifier::judge(float canon_l, float canon_r,
                                float chk_l, float chk_r) {
    float r, ratio;
    if (!check_.push(canon_l, canon_r, chk_l, chk_r, r, ratio))
        return Judgement::None;
    last_r_ = r;
    last_ratio_ = ratio;

    // Probation auto-calibration: strong structure with a stable off-band
    // level means this driver revision scales its mixer by a constant —
    // adopt it instead of striking.
    if (!calibrated_ && r >= 0.7f && !(ratio >= 0.85f && ratio <= 1.15f) &&
        (ratio >= 0.2f && ratio <= 5.0f) && ratio_n_ < 6) {
        ratio_hist_[ratio_n_ % 3] = ratio;
        ++ratio_n_;
        if (ratio_n_ >= 3) {
            float m = (ratio_hist_[0] + ratio_hist_[1] + ratio_hist_[2]) / 3.0f;
            bool stable = true;
            for (float x : ratio_hist_)
                if (std::fabs(x / m - 1.0f) >= 0.1f) stable = false;
            if (stable) {
                float g = gain_ / m;
                gain_ = g < 0.25f ? 0.25f : (g > 4.0f ? 4.0f : g);
                calibrated_ = true;
                check_.reset_strikes();
            }
        }
        return Judgement::None;
    }

    // Two failure axes: structure (correlation) and level (envelope ratio —
    // scale-invariant correlation alone would bless wrong volume math).
    bool level_ok = ratio >= 0.55f && ratio <= 1.6f;
    // The signed producer block is authoritative.  Until one matching
    // block has passed, envelope correlation is diagnostic only.
    bool signed_ok = signed_gate_seen_ && signed_gate_passed_;
    bool window_ok = signed_ok && r >= kMinR && level_ok;
    if (proven_) {
        if (window_ok) {
            check_.sub_strike();
        } else {
            check_.add_strike();
            if (check_.strikes() >= kMaxStrikes) {
                char buf[160];
                std::snprintf(buf, sizeof(buf),
                              "shadow/canon correlation %.2f, level ratio %.2f "
                              "(driver variant or unsupported feature)", r, ratio);
                pause(buf);
            }
        }
    } else if (window_ok) {
        ++consec_pass_;
        probation_failures_ = 0;
        if (consec_pass_ >= prove_need_) {
            proven_ = true;
            check_.reset_strikes();
        }
    } else {
        consec_pass_ = 0;
        ++probation_failures_;
    }
    fail_structural_ = !window_ok && (r < kMinR && level_ok);
    return window_ok ? Judgement::Pass : Judgement::Fail;
}

}  // namespace gba
