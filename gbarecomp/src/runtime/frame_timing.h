// frame_timing.h — clock-independent presentation-rate policy.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace gbarecomp {

struct FrameTimingRates {
    double guest_hz = 0.0;
    double interpolation_hz = 0.0;
};

inline constexpr double kGbaFrameHz = 16777216.0 / 280896.0;
inline constexpr double kEnhancedGuestHz = 60.0;
inline constexpr double kEnhancedInterpolationHz = 120.0;
inline constexpr double kEnhancedAudioRateScale =
    kEnhancedGuestHz / kGbaFrameHz;
inline constexpr float kMaxTurboMultiplier = 32.0f;

// Guest speed is measured from guest frames advanced per wall second. A GBA
// frame always carries the same normal cycle budget, so this is also the
// emulation-cycle rate relative to faithful real time. It intentionally does
// not use the requested Turbo multiplier: a bottleneck must show as less than
// the requested speed.
inline double emulation_speed_percent(std::uint64_t guest_frames,
                                      std::uint64_t wall_ns,
                                      double normal_hz = kGbaFrameHz) {
    if (guest_frames == 0 || wall_ns == 0 || !(normal_hz > 0.0)) return 0.0;
    const double guest_hz = static_cast<double>(guest_frames) * 1.0e9 /
                            static_cast<double>(wall_ns);
    return guest_hz * 100.0 / normal_hz;
}

// Rolling guest-speed sample. observe() advances the frame counter; age()
// only lets a quiet guest window expire, so pause cannot leave a stale speed.
class EmulationSpeedTracker {
public:
    static constexpr std::uint64_t kMeasurementWindowNs = 500'000'000ull;

    void reset() {
        have_frame_ = false;
        window_start_ns_ = 0;
        window_first_frame_ = 0;
        last_frame_ = 0;
        normal_hz_ = 0.0;
        speed_percent_ = 0.0;
    }

    float observe(std::uint64_t guest_frame, std::uint64_t now_ns,
                  double normal_hz) {
        if (!have_frame_ || normal_hz != normal_hz_) {
            begin(guest_frame, now_ns, normal_hz);
            return speed_percent_;
        }
        if (guest_frame < last_frame_ ||
            guest_frame - last_frame_ > 1) {
            begin(guest_frame, now_ns, normal_hz);
            return speed_percent_;
        }
        last_frame_ = guest_frame;
        refresh(now_ns);
        return speed_percent_;
    }

    float age(std::uint64_t now_ns) {
        refresh(now_ns);
        return speed_percent_;
    }

    float speed_percent() const { return speed_percent_; }

private:
    void begin(std::uint64_t guest_frame, std::uint64_t now_ns,
               double normal_hz) {
        have_frame_ = true;
        window_start_ns_ = now_ns;
        window_first_frame_ = guest_frame;
        last_frame_ = guest_frame;
        normal_hz_ = normal_hz;
        speed_percent_ = 0.0;
    }

    void refresh(std::uint64_t now_ns) {
        if (!have_frame_) return;
        if (now_ns < window_start_ns_) {
            reset();
            return;
        }
        const std::uint64_t span = now_ns - window_start_ns_;
        if (span < kMeasurementWindowNs) return;
        speed_percent_ = static_cast<float>(emulation_speed_percent(
            last_frame_ - window_first_frame_, span, normal_hz_));
        window_start_ns_ = now_ns;
        window_first_frame_ = last_frame_;
    }

    bool have_frame_ = false;
    std::uint64_t window_start_ns_ = 0;
    std::uint64_t window_first_frame_ = 0;
    std::uint64_t last_frame_ = 0;
    double normal_hz_ = 0.0;
    float speed_percent_ = 0.0f;
};

constexpr bool turbo_audio_should_mute(bool fast_forward,
                                       bool mute_during_turbo) {
    return fast_forward && mute_during_turbo;
}

// Decoupled audio owns host output only while live. A failed or not-ready
// opt-in must leave the canonical coupled path available; explicit mute wins.
constexpr bool turbo_audio_should_use_canonical(bool decoupled,
                                                 bool explicit_mute) {
    return !decoupled && !explicit_mute;
}

// Avoid rendering and blocking on VSync for every emulated Turbo frame while
// keeping the displayed image fresh. Normal speed always presents and resets
// the policy. Uncapped Turbo is governed only by the wall-time bound.
class TurboPresentDecimator {
public:
    static constexpr std::uint64_t kMaxWallGapNs = 16'666'667ull;

    bool should_present(bool fast_forward, bool uncapped, float multiplier,
                        std::uint64_t guest_frame, std::uint64_t now_ns) {
        if (!fast_forward) {
            active_ = false;
            return true;
        }
        if (!active_) {
            active_ = true;
            last_frame_ = guest_frame;
            last_ns_ = now_ns;
            return true;
        }
        const std::uint64_t stride = static_cast<std::uint64_t>(
            std::max(1.0f, std::ceil(std::max(1.0f, multiplier))));
        const bool frame_due = !uncapped &&
            guest_frame - last_frame_ >= stride;
        const bool wall_due = now_ns - last_ns_ >= kMaxWallGapNs;
        if (!frame_due && !wall_due) return false;
        last_frame_ = guest_frame;
        last_ns_ = now_ns;
        return true;
    }

private:
    bool active_ = false;
    std::uint64_t last_frame_ = 0;
    std::uint64_t last_ns_ = 0;
};

constexpr FrameTimingRates frame_timing_rates(bool enhanced) {
    return enhanced
        ? FrameTimingRates{kEnhancedGuestHz, kEnhancedInterpolationHz}
        : FrameTimingRates{kGbaFrameHz, kGbaFrameHz * 2.0};
}

constexpr bool enhanced_timing_is_available(bool game_allows,
                                             bool strict_static,
                                             bool frame_capture) {
    return game_allows && !strict_static && !frame_capture;
}

// An environment value is an explicit per-launch override. Otherwise use the
// saved host preference; callers supply false when no preference exists, so a
// first run remains opt-in.
constexpr bool enhanced_timing_initial_requested(bool available,
                                                  bool env_present,
                                                  bool env_requested,
                                                  bool saved_preference) {
    if (!available) return false;
    return env_present ? env_requested : saved_preference;
}

// Produces integer-nanosecond periods while carrying the fractional remainder.
// This avoids the accumulating drift caused by rounding every 60/120 Hz period
// independently (16,666,667 ns / 8,333,333 ns).
class FramePeriodStepper {
public:
    explicit FramePeriodStepper(double target_hz = kGbaFrameHz) {
        set_target_hz(target_hz);
    }

    void set_target_hz(double target_hz);
    std::int64_t next_period_ns();
    void reset_phase() { remainder_ns_ = 0.0L; }
    double target_hz() const { return target_hz_; }

private:
    double target_hz_ = kGbaFrameHz;
    std::int64_t whole_ns_ = 0;
    long double fractional_ns_ = 0.0L;
    long double remainder_ns_ = 0.0L;
};

}  // namespace gbarecomp
