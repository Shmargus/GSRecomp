#include "host_platform.h"

#include <cstdio>
#include <thread>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <timeapi.h>  // timeBeginPeriod/timeEndPeriod (excluded by LEAN_AND_MEAN)
#  if defined(_MSC_VER)
#    pragma comment(lib, "winmm.lib")  // MinGW links winmm via CMake
#  endif
#endif

namespace gbarecomp {

FramePacer::FramePacer(double target_hz) {
    set_target_hz(target_hz);
#if defined(_WIN32)
    // Raise the scheduler timer resolution to 1 ms so sleep_until is
    // accurate enough for ~16.74 ms frame pacing.
    if (timeBeginPeriod(1) == TIMERR_NOERROR) raised_timer_res_ = true;
    // HP-002: high-resolution waitable timer (Win10 1803+). sleep_until
    // quantizes to the system tick under load; its phase drift against the
    // 16.742 ms frame period measured as a 234 ms beat of 20-24 ms
    // oversleeps. This timer honors sub-millisecond due times. Null on
    // older Windows -> the hybrid sleep below still applies.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    wait_timer_ = CreateWaitableTimerExW(
        nullptr, nullptr,
        CREATE_WAITABLE_TIMER_MANUAL_RESET |
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS);
#endif
    reset();
}

FramePacer::~FramePacer() {
#if defined(_WIN32)
    if (wait_timer_) CloseHandle(static_cast<HANDLE>(wait_timer_));
    if (raised_timer_res_) timeEndPeriod(1);
#endif
}

void FramePacer::reset() {
    periods_.reset_phase();
    period_ = std::chrono::nanoseconds(periods_.next_period_ns());
    next_ = clock::now() + period_;
}

void FramePacer::set_target_hz(double target_hz) {
    if (!(target_hz > 0.0)) target_hz = kGbaFrameHz;
    base_target_hz_ = target_hz;
    periods_.set_target_hz(base_target_hz_ * speed_);
    reset();
}

void FramePacer::set_speed(double mult) {
    if (!(mult > 0.0)) return;            // also rejects NaN
    if (mult == speed_) return;
    speed_ = mult;
    periods_.set_target_hz(base_target_hz_ * speed_);
    // Rebase rather than let the old deadline linger: shortening the period
    // must not make the next frame instantly "late" (and lengthening it must
    // not stall for the difference).
    reset();
}

void FramePacer::wait_for_next_frame() {
    if (uncapped_) {
        next_ = clock::now();  // keep the deadline current for re-cap
        return;
    }
    clock::time_point now = clock::now();
    // If we've fallen more than one whole frame behind, resync instead
    // of trying to replay the lost time (avoids a catch-up death spiral
    // after a hitch or a breakpoint pause).
    if (now > next_ + period_) {
        next_ = now + period_;
        return;
    }
    // Hybrid wait: sleep most of the gap (cheap, but coarse), then spin
    // the final ~1 ms for deadline accuracy.
    constexpr auto kSpinTail = std::chrono::microseconds(1200);
    if (next_ - now > kSpinTail) {
#if defined(_WIN32)
        if (wait_timer_) {
            // Relative due time in 100 ns units (negative = relative).
            const auto due_ns = std::chrono::duration_cast<
                std::chrono::nanoseconds>(next_ - kSpinTail - now).count();
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>(due_ns / 100);
            HANDLE t = static_cast<HANDLE>(wait_timer_);
            if (SetWaitableTimer(t, &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(t, INFINITE);
            } else {
                std::this_thread::sleep_until(next_ - kSpinTail);
            }
        } else {
            std::this_thread::sleep_until(next_ - kSpinTail);
        }
#else
        std::this_thread::sleep_until(next_ - kSpinTail);
#endif
    }
    while (clock::now() < next_) {
        std::this_thread::yield();
    }
    period_ = std::chrono::nanoseconds(periods_.next_period_ns());
    next_ += period_;
}

std::vector<unsigned char> read_file(const std::string& path, std::string* error) {
    std::vector<unsigned char> out;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        if (error) *error = "open failed: " + path;
        return out;
    }
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz < 0) {
        if (error) *error = "ftell failed: " + path;
        std::fclose(f);
        return out;
    }
    out.resize(static_cast<std::size_t>(sz));
    std::size_t got = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    if (got != out.size()) {
        out.clear();
        if (error) *error = "short read: " + path;
    }
    return out;
}

}  // namespace gbarecomp
