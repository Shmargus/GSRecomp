#include "host_platform.h"

#include <cstdint>
#include <cstdio>
#include <thread>

#if defined(__ANDROID__)
#  include <dlfcn.h>
#  include <unistd.h>
#endif

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
    // High-resolution waitable timer (Win10 1803+). Under load, sleep_until
    // rounds wake-ups to the 64 Hz system tick. That tick drifts against the
    // 16.742 ms frame period and produced a 234 ms cycle of 20-24 ms
    // oversleeps (52 of 75 late frames in the frame-phase ring). This timer
    // honors sub-millisecond due times. It is null on older Windows, where
    // the hybrid sleep below still applies.
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

// Android: tell the system how long each frame's work took against the
// frame's time (Android 13+ APerformanceHint, fetched from libandroid.so as
// the app supports Android 9). Measured on a Tensor G4 phone (14 November
// 2026, simpleperf): the game thread was busy ~87% of the time on a middle
// core the governor held at 1.2 of its 2.6 GHz, and frames ran 14.5..22.3 ms
// apart. With the hint the governor can raise the clock before frames run
// late. The work is the time from the end of one wait to the start of the
// next.
void FramePacer::report_work(clock::time_point now) {
#if defined(__ANDROID__)
    using GetManager = void* (*)();
    using CreateSession = void* (*)(void*, const std::int32_t*, std::size_t, std::int64_t);
    using UpdateTarget = int (*)(void*, std::int64_t);
    using ReportActual = int (*)(void*, std::int64_t);
    static UpdateTarget update_target = nullptr;
    static ReportActual report_actual = nullptr;
    if (!hint_tried_) {
        hint_tried_ = true;
        work_start_ = now;
        void* android = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
        if (!android) return;
        auto get_manager = reinterpret_cast<GetManager>(
            dlsym(android, "APerformanceHint_getManager"));
        auto create = reinterpret_cast<CreateSession>(
            dlsym(android, "APerformanceHint_createSession"));
        update_target = reinterpret_cast<UpdateTarget>(
            dlsym(android, "APerformanceHint_updateTargetWorkDuration"));
        report_actual = reinterpret_cast<ReportActual>(
            dlsym(android, "APerformanceHint_reportActualWorkDuration"));
        void* manager = get_manager ? get_manager() : nullptr;
        if (!manager || !create || !update_target || !report_actual) return;
        const std::int32_t tid = static_cast<std::int32_t>(gettid());
        hint_session_ = create(manager, &tid, 1, period_.count());
        std::fprintf(stderr, "[perf-hint] %s for the pacing thread (target %.2f ms)\n",
                     hint_session_ ? "on" : "unavailable", period_.count() / 1e6);
        return;
    }
    if (!hint_session_) return;
    const auto work = std::chrono::duration_cast<std::chrono::nanoseconds>(now - work_start_);
    if (work.count() > 0) {
        update_target(hint_session_, period_.count());
        report_actual(hint_session_, work.count());
    }
#else
    (void)now;
#endif
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
    report_work(now);
    // If we've fallen more than one whole frame behind, resync instead
    // of trying to replay the lost time (avoids a catch-up death spiral
    // after a hitch or a breakpoint pause).
    if (now > next_ + period_) {
        next_ = now + period_;
        work_start_ = now;
        return;
    }
    // Hybrid wait: sleep most of the gap (cheap, but coarse), then spin
    // the final 1.2 ms for deadline accuracy.
#if defined(__ANDROID__)
    // Phones: sleep all the way. The spin is 1.2 ms of a busy core before
    // every frame (the figure is for Windows' coarse timers), and the vsync
    // present lines the picture up afterwards. How late the phone's sleep
    // wakes is not measured yet (TODO-EVIDENCE: frame gaps on the phone).
    constexpr auto kSpinTail = std::chrono::microseconds(0);
#else
    constexpr auto kSpinTail = std::chrono::microseconds(1200);
#endif
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
    work_start_ = clock::now();
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
