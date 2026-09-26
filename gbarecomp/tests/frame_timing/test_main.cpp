#include "frame_timing.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

void expect(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

long long accumulated_ns(double hz, int ticks) {
    gbarecomp::FramePeriodStepper stepper(hz);
    long long total = 0;
    for (int i = 0; i < ticks; ++i) total += stepper.next_period_ns();
    return total;
}

}  // namespace

int main() {
    const auto faithful = gbarecomp::frame_timing_rates(false);
    const auto enhanced = gbarecomp::frame_timing_rates(true);
    expect(faithful.guest_hz == gbarecomp::kGbaFrameHz,
           "faithful guest rate changed");
    expect(faithful.interpolation_hz == gbarecomp::kGbaFrameHz * 2.0,
           "faithful interpolation rate changed");
    expect(enhanced.guest_hz == 60.0, "enhanced guest rate is not 60 Hz");
    expect(enhanced.interpolation_hz == 120.0,
           "enhanced interpolation rate is not 120 Hz");
    expect(gbarecomp::enhanced_timing_is_available(true, false, false),
           "game opt-in did not enable enhanced timing");
    expect(!gbarecomp::enhanced_timing_is_available(true, true, false),
           "strict-static did not force faithful timing");
    expect(!gbarecomp::enhanced_timing_is_available(true, false, true),
           "frame capture did not force faithful timing");
    expect(!gbarecomp::enhanced_timing_is_available(false, false, false),
           "unsupported game exposed enhanced timing");
    expect(!gbarecomp::enhanced_timing_initial_requested(
               true, false, false, false),
           "missing preference did not default enhanced timing off");
    expect(gbarecomp::enhanced_timing_initial_requested(
               true, false, false, true),
           "saved enhanced timing preference was ignored");
    expect(!gbarecomp::enhanced_timing_initial_requested(
               true, true, false, true),
           "environment OFF did not override saved ON");
    expect(gbarecomp::enhanced_timing_initial_requested(
               true, true, true, false),
           "environment ON did not override saved OFF");
    expect(!gbarecomp::enhanced_timing_initial_requested(
               false, true, true, true),
           "forced-off policy allowed enhanced timing");
    expect(gbarecomp::kMaxTurboMultiplier > 16.0f,
           "Turbo multiplier maximum was not increased");
    expect(gbarecomp::turbo_audio_should_mute(true, true),
           "Turbo mute did not engage while held");
    expect(!gbarecomp::turbo_audio_should_mute(false, true),
           "Turbo mute stayed active after release");
    expect(!gbarecomp::turbo_audio_should_mute(true, false),
           "disabled Turbo mute changed audio");
    expect(gbarecomp::turbo_audio_should_use_canonical(false, false),
           "decoupling failure did not retain canonical audio");
    expect(!gbarecomp::turbo_audio_should_use_canonical(true, false),
           "live wall audio left canonical audio enabled");
    expect(!gbarecomp::turbo_audio_should_use_canonical(false, true),
           "explicit Turbo mute did not win over canonical fallback");

    const auto faithful_speed = gbarecomp::emulation_speed_percent(
        60, 1'000'000'000ull, 60.0);
    expect(std::abs(faithful_speed - 100.0) < 1.0e-9,
           "normal emulation speed was not 100 percent");
    const auto turbo_speed = gbarecomp::emulation_speed_percent(
        240, 1'000'000'000ull, 60.0);
    expect(std::abs(turbo_speed - 400.0) < 1.0e-9,
           "emulation speed used the requested multiplier incorrectly");
    expect(gbarecomp::emulation_speed_percent(0, 1'000'000'000ull, 60.0) == 0.0,
           "empty emulation-speed sample was not zero");

    gbarecomp::EmulationSpeedTracker speed;
    expect(speed.observe(100, 0, gbarecomp::kGbaFrameHz) == 0.0f,
           "speed tracker did not start at zero");
    float faithful_sample = 0.0f;
    for (std::uint64_t frame = 101; frame <= 130; ++frame) {
        faithful_sample = speed.observe(
            frame, (frame - 100) *
                       gbarecomp::EmulationSpeedTracker::kMeasurementWindowNs /
                       30,
            gbarecomp::kGbaFrameHz);
    }
    const float faithful_expected = static_cast<float>(
        gbarecomp::emulation_speed_percent(
            30, gbarecomp::EmulationSpeedTracker::kMeasurementWindowNs,
            gbarecomp::kGbaFrameHz));
    expect(std::abs(faithful_sample - faithful_expected) < 1.0e-4f,
           "faithful speed sample was wrong");
    expect(speed.age(2 * gbarecomp::EmulationSpeedTracker::kMeasurementWindowNs)
               == 0.0f,
           "idle speed did not age to zero");
    expect(speed.observe(200, 2 *
                             gbarecomp::EmulationSpeedTracker::kMeasurementWindowNs,
                         gbarecomp::kGbaFrameHz) == 0.0f,
           "frame discontinuity did not clear speed baseline");

    gbarecomp::EmulationSpeedTracker enhanced_tracker;
    enhanced_tracker.observe(10, 0, gbarecomp::kEnhancedGuestHz);
    float enhanced_sample = 0.0f;
    for (std::uint64_t frame = 11; frame <= 40; ++frame) {
        enhanced_sample = enhanced_tracker.observe(
            frame, (frame - 10) *
                       gbarecomp::EmulationSpeedTracker::kMeasurementWindowNs /
                       30,
            gbarecomp::kEnhancedGuestHz);
    }
    expect(std::abs(enhanced_sample - 100.0f) < 1.0e-4f,
           "Enhanced Timing speed was not normalized to 100 percent");
    enhanced_tracker.reset();
    enhanced_tracker.observe(10, 0, gbarecomp::kEnhancedGuestHz);
    expect(enhanced_tracker.observe(11, 1, gbarecomp::kGbaFrameHz) == 0.0f,
           "timing-mode change did not clear speed baseline");

    gbarecomp::TurboPresentDecimator bounded;
    expect(bounded.should_present(true, false, 4.0f, 100, 1),
           "bounded Turbo did not present its first frame");
    expect(!bounded.should_present(true, false, 4.0f, 103, 4),
           "bounded Turbo presented before its stride");
    expect(bounded.should_present(true, false, 4.0f, 104, 5),
           "bounded Turbo missed its stride");

    gbarecomp::TurboPresentDecimator uncapped;
    expect(uncapped.should_present(true, true, 32.0f, 1, 100),
           "uncapped Turbo did not present its first frame");
    expect(!uncapped.should_present(
               true, true, 32.0f, 1000,
               100 + gbarecomp::TurboPresentDecimator::kMaxWallGapNs - 1),
           "uncapped Turbo used guest-frame stride");
    expect(uncapped.should_present(
               true, true, 32.0f, 1001,
               100 + gbarecomp::TurboPresentDecimator::kMaxWallGapNs),
           "uncapped Turbo left display stale");
    expect(uncapped.should_present(false, true, 32.0f, 1002, 200),
           "Turbo release did not immediately present");
    expect(uncapped.should_present(true, true, 32.0f, 1003, 201),
           "new Turbo hold did not start fresh");

    constexpr long long kHourNs = 3600ll * 1000000000ll;
    expect(std::llabs(accumulated_ns(60.0, 216000) - kHourNs) <= 1,
           "60 Hz accumulates phase drift over one hour");
    expect(std::llabs(accumulated_ns(120.0, 432000) - kHourNs) <= 1,
           "120 Hz accumulates phase drift over one hour");

    gbarecomp::FramePeriodStepper invalid(0.0);
    expect(invalid.target_hz() == gbarecomp::kGbaFrameHz,
           "invalid target did not fall back to faithful timing");
    std::puts("frame timing tests passed");
    return 0;
}
