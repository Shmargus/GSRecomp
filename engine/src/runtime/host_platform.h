// host_platform.h — Host-side things: file I/O, time, threads,
// window + audio device output.

#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

#include "frame_timing.h"

namespace gbarecomp {

// Read an entire file into memory. Returns empty vector + sets `error`
// on failure. Used for ROM and BIOS loading.
std::vector<unsigned char> read_file(const std::string& path,
                                     std::string* error = nullptr);

// ── FramePacer ─────────────────────────────────────────────────────
// Wall-clock frame limiter. Emulation speed must be governed by the
// GBA's real frame rate (59.7275 Hz), NOT by the host monitor's refresh
// rate — relying on SDL's PRESENTVSYNC made the game run at monitor
// speed (e.g. 2.75x on a 164 Hz panel; see MinishCapRecomp MC-HP-004).
//
// Maintains a monotonic deadline advanced by exactly one GBA frame
// period each call. wait() blocks until that deadline using a hybrid
// sleep-then-spin (Windows' default ~15.6 ms timer granularity is too
// coarse for a 16.74 ms target, so the pacer raises timer resolution to
// 1 ms for its lifetime and spins the final sub-ms). If the host falls
// more than a frame behind, the deadline resyncs to now rather than
// trying to catch up (no death spiral). Uncapping enables fast-forward.
class FramePacer {
public:
    // GBA: 16'777'216 Hz / 280'896 cycles-per-frame = 59.7275 Hz.
    static constexpr double kGbaFrameHz = gbarecomp::kGbaFrameHz;

    explicit FramePacer(double target_hz = kGbaFrameHz);
    ~FramePacer();

    FramePacer(const FramePacer&) = delete;
    FramePacer& operator=(const FramePacer&) = delete;

    // Block until the next frame's deadline. Call once per presented
    // frame. No-op while uncapped.
    void wait_for_next_frame();

    // Realign the deadline to "now". Call after any non-real-time jump
    // (startup, save-state load) so the pacer doesn't try to catch up.
    void reset();

    // Fast-forward toggle. While uncapped, wait_for_next_frame() returns
    // immediately and the deadline tracks now.
    void set_uncapped(bool on) { uncapped_ = on; }
    bool uncapped() const { return uncapped_; }

    // Run the deadline at `mult` times the target rate: 1.0 = real time,
    // 4.0 = Turbo at 4x, values <= 0 ignored. This is a PACING change only —
    // emulation still advances one GBA frame per call, so the guest's own
    // sense of time (cycle counts, timers, audio rate) is untouched; it just
    // gets wall-clock permission to run sooner. Unlike set_uncapped() the
    // host still gets to idle, so audio can keep up at modest multiples.
    void set_speed(double mult);
    double speed() const { return speed_; }

    // Switch between faithful and enhanced wall-clock cadence without
    // reconstructing the platform timer. Resets the deadline and fractional
    // phase so a live menu toggle cannot inherit an old-rate deadline.
    void set_target_hz(double target_hz);
    double target_hz() const { return periods_.target_hz(); }

private:
    using clock = std::chrono::steady_clock;
    FramePeriodStepper periods_{};
    double base_target_hz_ = kGbaFrameHz;
    std::chrono::nanoseconds period_{};       // most recently scheduled period
    double speed_ = 1.0;
    clock::time_point next_{};
    bool uncapped_ = false;
    bool raised_timer_res_ = false;
    // HP-002: Windows high-resolution waitable timer (HANDLE, void* here to
    // keep windows.h out of this header). sleep_until quantizes to the 64 Hz
    // system tick under load; the phase drift between that grid and the
    // 16.742 ms frame period produced a measured 234 ms beat of 20-24 ms
    // oversleeps (52/75 late frames in the frame-phase ring). The hi-res
    // timer waits on the exact deadline instead; null = pre-1803 fallback
    // to the old hybrid sleep.
    void* wait_timer_ = nullptr;
};

}  // namespace gbarecomp
