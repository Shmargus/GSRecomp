#include "gpu_timing.h"

#include <atomic>

namespace gbarecomp {
namespace {
const char* const kStepNames[] = {"game other", "to the screen", "window masks",
                                  "front pass", "second pass", "resolve",
                                  "front sprites", "front BG0", "front BG1",
                                  "front BG2", "front BG3"};
static_assert(sizeof(kStepNames) / sizeof(kStepNames[0]) ==
                  static_cast<int>(GpuTimingStep::Count),
              "one name per step");
std::atomic<int> g_current_step{-1};
void note_step(GpuTimingStep step) {
    g_current_step.store(static_cast<int>(step), std::memory_order_relaxed);
}
}  // namespace

const char* gpu_timing_current_step() {
    const int s = g_current_step.load(std::memory_order_relaxed);
    return s >= 0 && s < static_cast<int>(GpuTimingStep::Count) ? kStepNames[s] : "none";
}
}  // namespace gbarecomp

#if defined(GBARECOMP_HAVE_SDL2)

#include <SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>

#include "../armv4t/env_flag.h"
#include "gl_platform.h"
#include "runtime_bus_bridge.h"

namespace gbarecomp {
namespace {

// Desktop <GL/gl.h> is 1.1, so the query names are spelt out here (the same
// values in desktop GL 3.3 and ES's GL_EXT_disjoint_timer_query).
constexpr unsigned kTimeElapsed = 0x88BF;
constexpr unsigned kQueryResult = 0x8866;
constexpr unsigned kQueryResultAvailable = 0x8867;
constexpr unsigned kGpuDisjoint = 0x8FBB;

using GenQueries = void(APIENTRY*)(int, unsigned*);
using BeginQuery = void(APIENTRY*)(unsigned, unsigned);
using EndQuery = void(APIENTRY*)(unsigned);
using GetQueryObjectuiv = void(APIENTRY*)(unsigned, unsigned, unsigned*);
using GetQueryObjectui64v = void(APIENTRY*)(unsigned, unsigned, std::uint64_t*);
using GetIntegerv = void(APIENTRY*)(unsigned, int*);

using Clock = std::chrono::steady_clock;

// Results come back a few frames late; each step keeps this many in flight.
constexpr int kDepth = 8;
constexpr int kSteps = static_cast<int>(GpuTimingStep::Count);

struct Slot {
    unsigned query = 0;
    bool pending = false;
    unsigned long long frame = 0;  // guest frame the step was issued in
};

// The slowest steps of the minute, card or processor, with the guest frame
// (the frame= of other log lines) and the time into the session, so a hitch
// can be matched to what was on screen. Only steps over a 60 Hz frame
// (16.7 ms) are kept.
struct Slow {
    double ms = 0;
    int step = 0;
    bool card = true;
    unsigned long long frame = 0;
    double at_s = 0;
};
constexpr int kSlowKept = 8;

struct Totals {
    std::uint64_t count = 0;
    double card_ms = 0, card_max = 0;
    double cpu_ms = 0, cpu_max = 0;
    std::uint64_t cpu_count = 0;
    std::uint64_t over_8 = 0, over_16 = 0;  // card time above 8.3 / 16.7 ms
};

struct State {
    bool checked = false, enabled = false, ready = false, disjoint_ext = false;
    GenQueries gen = nullptr;
    BeginQuery begin = nullptr;
    EndQuery end = nullptr;
    GetQueryObjectuiv get_uiv = nullptr;
    GetQueryObjectui64v get_u64 = nullptr;
    GetIntegerv get_int = nullptr;
    std::array<std::array<Slot, kDepth>, kSteps> slots{};
    std::array<int, kSteps> next{};
    int active = -1;            // the step whose query is open, -1 none
    int active_slot = -1;
    Clock::time_point cpu_start{};
    std::array<Totals, kSteps> totals{};
    std::uint64_t skipped = 0, disjoint = 0, frames = 0;
    Clock::time_point since{}, started{};
    std::array<Slow, kSlowKept> slow{};
    int slow_count = 0;
    std::uint64_t slow_total = 0;
} g;

const char* const* kNames = kStepNames;

void note_slow(int step, bool card, double ms, unsigned long long frame) {
    if (ms <= 1000.0 / 60.0) return;
    ++g.slow_total;
    const Slow entry{ms, step, card, frame,
                     std::chrono::duration<double>(Clock::now() - g.started).count()};
    if (g.slow_count < kSlowKept) {
        g.slow[g.slow_count++] = entry;
        return;
    }
    auto least = std::min_element(g.slow.begin(), g.slow.end(),
                                  [](const Slow& a, const Slow& b) { return a.ms < b.ms; });
    if (least->ms < ms) *least = entry;
}

bool ready() {
    if (!g.checked) {
        g.checked = true;
        g.enabled = env_flag("GBARECOMP_GPU_TIMING");
        if (!g.enabled) return false;
        if (!SDL_GL_GetCurrentContext()) { g.checked = false; return false; }
        g.disjoint_ext = SDL_GL_ExtensionSupported("GL_EXT_disjoint_timer_query");
        const bool timer = g.disjoint_ext || SDL_GL_ExtensionSupported("GL_ARB_timer_query");
        const auto load = [](const char* a, const char* b) {
            void* p = SDL_GL_GetProcAddress(a);
            return p ? p : SDL_GL_GetProcAddress(b);
        };
        g.gen = reinterpret_cast<GenQueries>(load("glGenQueries", "glGenQueriesEXT"));
        g.begin = reinterpret_cast<BeginQuery>(load("glBeginQuery", "glBeginQueryEXT"));
        g.end = reinterpret_cast<EndQuery>(load("glEndQuery", "glEndQueryEXT"));
        g.get_uiv = reinterpret_cast<GetQueryObjectuiv>(
            load("glGetQueryObjectuiv", "glGetQueryObjectuivEXT"));
        g.get_u64 = reinterpret_cast<GetQueryObjectui64v>(
            load("glGetQueryObjectui64vEXT", "glGetQueryObjectui64v"));
        g.get_int = reinterpret_cast<GetIntegerv>(SDL_GL_GetProcAddress("glGetIntegerv"));
        g.ready = timer && g.gen && g.begin && g.end && g.get_uiv && g.get_u64 && g.get_int;
        if (g.ready) {
            for (auto& step : g.slots)
                for (Slot& s : step) g.gen(1, &s.query);
            g.since = g.started = Clock::now();
        }
        std::fprintf(stderr, "[gpu-time] %s\n",
                     g.ready ? "timing the graphics card once a minute"
                             : "this graphics card cannot time its work");
    }
    return g.ready;
}

void collect() {
    // A disjoint event (clock change, power state) spoils results read now.
    bool spoiled = false;
    if (g.disjoint_ext) {
        int d = 0;
        g.get_int(kGpuDisjoint, &d);
        spoiled = d != 0;
    }
    for (int step = 0; step < kSteps; ++step) {
        for (Slot& s : g.slots[step]) {
            if (!s.pending || (step == g.active && &s == &g.slots[step][g.active_slot]))
                continue;
            unsigned available = 0;
            g.get_uiv(s.query, kQueryResultAvailable, &available);
            if (!available) continue;
            std::uint64_t ns = 0;
            g.get_u64(s.query, kQueryResult, &ns);
            s.pending = false;
            // Over a second is no real step: the Adreno 619 returned values
            // near 2^64 for the window-mask step (clears and target switches
            // only), so such a result is counted as spoiled.
            if (spoiled || ns > 1000000000ull) { ++g.disjoint; continue; }
            const double ms = static_cast<double>(ns) / 1e6;
            Totals& t = g.totals[step];
            ++t.count;
            t.card_ms += ms;
            t.card_max = std::max(t.card_max, ms);
            if (ms > 1000.0 / 120.0) ++t.over_8;
            if (ms > 1000.0 / 60.0) ++t.over_16;
            note_slow(step, true, ms, s.frame);
        }
    }
}

void print() {
    const double secs =
        std::chrono::duration<double>(Clock::now() - g.since).count();
    if (g.frames == 0 || secs <= 0) return;
    std::fprintf(stderr, "[gpu-time] %.0f s, %llu frames:", secs,
                 static_cast<unsigned long long>(g.frames));
    for (int step = 0; step < kSteps; ++step) {
        const Totals& t = g.totals[step];
        const double n = static_cast<double>(std::max<std::uint64_t>(t.count, 1));
        const double c = static_cast<double>(std::max<std::uint64_t>(t.cpu_count, 1));
        if (t.count == 0 && t.cpu_count == 0) continue;
        std::fprintf(stderr,
                     " %s: %llu runs, card %.2f ms (most %.2f, %llu over 8.3 ms, %llu over 16.7 ms),"
                     " processor %.2f ms (most %.2f);",
                     kNames[step], static_cast<unsigned long long>(t.count),
                     t.card_ms / n, t.card_max,
                     static_cast<unsigned long long>(t.over_8),
                     static_cast<unsigned long long>(t.over_16), t.cpu_ms / c, t.cpu_max);
    }
    std::fprintf(stderr, " not timed %llu, spoiled %llu\n",
                 static_cast<unsigned long long>(g.skipped),
                 static_cast<unsigned long long>(g.disjoint));
    if (g.slow_count > 0) {
        std::sort(g.slow.begin(), g.slow.begin() + g.slow_count,
                  [](const Slow& a, const Slow& b) { return a.frame < b.frame; });
        std::fprintf(stderr, "[gpu-time] slow steps (over 16.7 ms): %llu, slowest %d by frame:",
                     static_cast<unsigned long long>(g.slow_total), g.slow_count);
        for (int i = 0; i < g.slow_count; ++i) {
            const Slow& w = g.slow[i];
            const int at = static_cast<int>(w.at_s);
            std::fprintf(stderr, " frame=%llu (%d:%02d) %s %s %.1f ms;", w.frame, at / 60,
                         at % 60, kNames[w.step], w.card ? "card" : "processor", w.ms);
        }
        std::fprintf(stderr, "\n");
    }
    std::fflush(stderr);
    g.slow_count = 0;
    g.slow_total = 0;
    g.totals = {};
    g.skipped = g.disjoint = g.frames = 0;
    g.since = Clock::now();
}

}  // namespace

void gpu_timing_begin(GpuTimingStep step) {
    note_step(step);
    if (g.checked && !g.ready) return;
    if (!ready()) return;
    if (g.active >= 0) {
        // A step left without its end (an early return): close it; its time
        // still counts on the card, the processor time is dropped.
        g.end(kTimeElapsed);
        g.active = g.active_slot = -1;
    }
    const int i = static_cast<int>(step);
    const int at = g.next[i];
    Slot& s = g.slots[i][at];
    if (s.pending) { ++g.skipped; return; }  // results this far behind: skip
    g.next[i] = (at + 1) % kDepth;
    g.begin(kTimeElapsed, s.query);
    s.pending = true;
    s.frame = runtime_current_frame();
    g.active = i;
    g.active_slot = at;
    g.cpu_start = Clock::now();
}

void gpu_timing_switch(GpuTimingStep next) {
    note_step(next);
    if (!g.ready || g.active < 0) return;
    gpu_timing_end(static_cast<GpuTimingStep>(g.active));
    gpu_timing_begin(next);
}

// Ends whichever step is open: after switches that may not be `step`.
void gpu_timing_end(GpuTimingStep) {
    g_current_step.store(-1, std::memory_order_relaxed);
    if (!g.ready || g.active < 0) return;
    g.end(kTimeElapsed);
    const double ms =
        std::chrono::duration<double, std::milli>(Clock::now() - g.cpu_start).count();
    Totals& t = g.totals[g.active];
    ++t.cpu_count;
    t.cpu_ms += ms;
    t.cpu_max = std::max(t.cpu_max, ms);
    note_slow(g.active, false, ms, g.slots[g.active][g.active_slot].frame);
    g.active = -1;
    g.active_slot = -1;
}

void gpu_timing_frame_done() {
    if (!g.ready) return;
    if (g.active >= 0) gpu_timing_end(static_cast<GpuTimingStep>(g.active));
    ++g.frames;
    collect();
    if (Clock::now() - g.since >= std::chrono::seconds(60)) print();
}

void gpu_timing_flush() {
    if (!g.ready) return;
    if (g.active >= 0) gpu_timing_end(static_cast<GpuTimingStep>(g.active));
    collect();
    print();
}

}  // namespace gbarecomp

#else

namespace gbarecomp {
void gpu_timing_begin(GpuTimingStep step) { note_step(step); }
void gpu_timing_end(GpuTimingStep) { g_current_step.store(-1, std::memory_order_relaxed); }
void gpu_timing_switch(GpuTimingStep next) { note_step(next); }
void gpu_timing_frame_done() {}
void gpu_timing_flush() {}
}  // namespace gbarecomp

#endif
