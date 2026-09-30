// runtime_bus_bridge.cpp — overrides the weak bus-accessor stubs in
// runtime_arm.cpp with real implementations that delegate to the
// active gba::GbaBus.
//
// The runner's main() must call `gbarecomp::set_active_bus(&bus)`
// before any recompiled cart code executes.

#include "../armv4t/runtime_arm.h"
#include "../armv4t/arm_ir.h"
#include "../armv4t/symbol_lookup.h"
#include "../gba/gba_bus.h"
#include "../gba/gba_irq.h"
#include "../gba/gba_m4a.h"
#include "../gba/gba_ppu.h"
#include "host_prof_phase.h"

#ifdef GBA_COSIM
#include "../debug/cosim.h"   // cosim_on_tick() — first-divergence checkpoint hook
#endif

#include <algorithm>
#ifdef _WIN32
#include <windows.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <map>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

extern "C" void runtime_ram_image_bus_begin(void);
extern "C" void runtime_ram_image_bus_end(void);

// Debug PC breakpoint (MC-HP-002 sound-engine investigation). When set
// (via the TCP `set_break_pc` command), the per-instruction
// runtime_should_yield() returns true the moment the guest PC reaches
// this address, unwinding the current runtime_dispatch back to the exec
// loop so the TCP server can inspect register/memory state. The spin we
// need to inspect (0x08004286) lives inside a single runtime_dispatch,
// so ordinary step granularity can't enter it. 0 disables (no overhead).
extern "C" uint32_t g_runtime_break_pc = 0;

// Count of PPU VBlank-start events (scanline 159->160), incremented
// unconditionally in runtime_tick regardless of DISPSTAT IRQ-enable.
// The debug step-one-frame primitive (runtime.cpp step_frame) stops when
// this advances, so the recomp's TCP `step` parks at VBlank-start — the
// same PPU phase the interpreter (tools/bios_smoke step_one_frame) and
// mGBA (runFrame) park at. Stopping at scanline-WRAP instead (the old
// ppu.frame_count() convention) parked the recomp ~68 scanlines / one
// frame of game-logic later than the oracles, manufacturing a spurious
// "recomp runs a frame ahead" when diffing memory at the same step index.
extern "C" unsigned long long g_runtime_vblank_starts = 0;

// ── Phase profiler (GBARECOMP_PHASE_PROF=1) ─────────────────────────────────
// De-confounds the guest-PC sampler's biggest attribution trap: PPU pixel
// composition runs per-visible-scanline inside tick_devices, which runs inside
// runtime_tick — called from EVERY recompiled guest instruction, including the
// WaitForVBlank busy-spin. A guest-PC profiler therefore charges composition to
// whatever PC was live (usually the spin). Timing render_scanline directly
// measures composition's true wall-time share. ~160 timed calls/frame, so the
// steady_clock overhead is negligible; fully gated off (no clock reads) unless
// the env var is set. Dumped to stderr at exit.
extern "C" unsigned long long g_prof_scanline_ns = 0;
extern "C" unsigned long long g_prof_scanline_count = 0;
// Lazily evaluated (function-local static) rather than a namespace-scope
// initializer: a namespace-scope static runs at dynamic-init time, BEFORE
// main() and therefore before config.ini has been read, so it could never
// see the config UI's "Additional debug logging" toggle. Checked here on
// first call (the first visible-scanline HBlank, well after config load) —
// GBARECOMP_PHASE_PROF, if explicitly set, wins outright; otherwise falls
// back to the toggle (default OFF). Result is cached, same "single bool
// check per call site when off" cost as before.
static bool phase_prof_enabled() {
    static const bool on = [] {
        const char* e = std::getenv("GBARECOMP_PHASE_PROF");
        bool enabled = e ? !(e[0] == '0' && e[1] == '\0')
                         : (gsr_additional_debug_logging() != 0);
        if (enabled) {
            std::atexit([] {
                std::fprintf(stderr,
                    "[phase] render_scanline: %llu ns over %llu calls "
                    "(avg %.1f ns/call)\n",
                    g_prof_scanline_ns, g_prof_scanline_count,
                    g_prof_scanline_count
                        ? static_cast<double>(g_prof_scanline_ns) /
                              static_cast<double>(g_prof_scanline_count)
                        : 0.0);
            });
        }
        return enabled;
    }();
    return on;
}
// ── TEMPORARY cost-attribution probe (GBARECOMP_COST_PROBE=1) ──────────────
// GS-011-perf follow-up: ranks where uncapped guest_us goes now that the
// pacer/vsync/trace-ring/sync-compile costs are ruled out. Diagnostic-only,
// env-gated (a single bool check per call site when off; near-zero when on
// given the call counts involved), and intended for removal once the report
// is written. Do NOT leave this enabled by default.
extern "C" unsigned long long g_cost_bus_slow_ns = 0;
extern "C" unsigned long long g_cost_bus_slow_calls = 0;
extern "C" unsigned long long g_cost_audio_tick_ns = 0;
extern "C" unsigned long long g_cost_audio_tick_calls = 0;
extern "C" unsigned long long g_cost_timers_ns = 0;
extern "C" unsigned long long g_cost_timers_calls = 0;
extern "C" unsigned long long g_cost_dma_ns = 0;
extern "C" unsigned long long g_cost_dma_calls = 0;
extern "C" unsigned long long g_cost_dispatch_ns = 0;
extern "C" unsigned long long g_cost_dispatch_calls = 0;
extern "C" unsigned long long g_cost_halt_ns;
extern "C" unsigned long long g_cost_halt_calls;
extern "C" unsigned long long g_cost_halt_iters;
// ── Halt-bucket breakdown (GS-011-perf halt-attribution follow-up) ─────────
// Sub-divides the 74.6%-of-wall halt/idle pump bucket into (a) per-iteration
// overhead OUTSIDE tick_devices — pump_idle's own duplicated horizon min(),
// runtime_tick's own bookkeeping, recompute_event_budget, drain_dma_steal,
// the IRQ-eligibility check — versus (b) tick_devices itself, split into PPU
// scanline render vs everything else (audio/timers/dma already had counters
// above; this adds PPU render + the containers needed to isolate overhead).
// Same idiom as the existing probe: single bool check per call site when
// off, CostTimer when on. Diagnostic-only; intended for removal.
extern "C" unsigned long long g_cost_pump_idle_ns = 0;
extern "C" unsigned long long g_cost_pump_idle_calls = 0;
extern "C" unsigned long long g_cost_tick_total_ns = 0;
extern "C" unsigned long long g_cost_tick_total_calls = 0;
extern "C" unsigned long long g_cost_tick_devices_ns = 0;
extern "C" unsigned long long g_cost_tick_devices_calls = 0;
extern "C" unsigned long long g_cost_recompute_budget_ns = 0;
extern "C" unsigned long long g_cost_recompute_budget_calls = 0;
extern "C" unsigned long long g_cost_drain_dma_call_ns = 0;
extern "C" unsigned long long g_cost_drain_dma_call_calls = 0;
extern "C" unsigned long long g_cost_irq_check_ns = 0;
extern "C" unsigned long long g_cost_irq_check_calls = 0;
extern "C" unsigned long long g_cost_ppu_render_ns = 0;
extern "C" unsigned long long g_cost_ppu_render_calls = 0;
extern "C" unsigned long long g_cost_irq_handler_ns = 0;
extern "C" unsigned long long g_cost_irq_handler_calls = 0;
extern "C" unsigned long long g_cost_mp2k_trace_stores = 0;
extern "C" unsigned long long g_cost_mp2k_fast_accepts = 0;
extern "C" unsigned long long g_cost_mp2k_hook_pre = 0;
extern "C" unsigned long long g_cost_mp2k_hook_post = 0;
extern "C" unsigned long long g_cost_mp2k_exact_writer_matches = 0;
extern "C" unsigned long long g_cost_mp2k_fast_filter_ns = 0;
extern "C" unsigned long long g_cost_mp2k_hook_checks = 0;
extern "C" unsigned long long g_cost_mp2k_hook_matches = 0;
extern "C" unsigned long long g_cost_mp2k_hook_deep_calls = 0;
extern "C" unsigned long long g_cost_mp2k_frame_hooks;
extern "C" unsigned long long g_cost_mp2k_render_ns;
extern "C" unsigned long long g_cost_mp2k_render_calls;
extern "C" unsigned long long g_cost_mp2k_producer_blocks_ns;
extern "C" unsigned long long g_cost_mp2k_producer_blocks;
extern "C" unsigned long long g_cost_mp2k_judge_output_ns;
extern "C" unsigned long long g_cost_mp2k_judge_output_calls;
// Per-instruction bookkeeping call counts (see CostBk in host_prof_phase.h);
// bumped by cost_bk() below, read per frame by runtime.cpp.
extern "C" unsigned long long g_cost_bk[kBkCount] = {};
extern "C" bool g_cost_probe_enabled(void);
static const bool g_cost_probe = [] {
    const char* e = std::getenv("GBARECOMP_COST_PROBE");
    bool on = (e != nullptr) && !(e[0] == '0' && e[1] == '\0');
    if (on) {
        std::atexit([] {
            std::fprintf(stderr,
                "[cost] dispatch:       %llu ns over %llu calls\n"
                "[cost] halt_pump:      %llu ns over %llu halts (%llu pump_idle iters)\n"
                "[cost]   pump_idle:    %llu ns over %llu calls (container: incl runtime_tick; single-level, non-recursive — trustworthy total)\n"
                "[cost]     tick_calls (NOT timed, see note): %llu calls (runtime_tick is per-instruction + re-entrant via nested IRQ dispatch; a clock-timed wrapper here both inflated wall time ~2.2x AND double-counted via recursion, so it is call-count-only now)\n"
                "[cost]       tick_devices:      %llu ns over %llu calls (container: incl ppu/audio/timers/dma; leaf-safe, not self-recursive)\n"
                "[cost]         ppu_render:      %llu ns over %llu calls\n"
                "[cost]       recompute_budget:  %llu ns over %llu calls (leaf-safe)\n"
                "[cost]       drain_dma_steal (NOT timed, see note): %llu calls (as hot as tick_calls; its own cost when cyc!=0 already appears under tick_devices/recompute_budget above)\n"
                "[cost]       irq_check:         %llu ns over %llu calls (container: incl tick_devices on wake + irq_handler; mostly single-level, ~5%% of calls may nest on a double-IRQ halt period)\n"
                "[cost]         irq_handler:     %llu ns over %llu calls (NOT overhead: nested runtime_dispatch of the guest ISR)\n"
                "[cost] bus_slow:       %llu ns over %llu calls\n"
                "[cost] audio_tick:     %llu ns over %llu calls\n"
                "[cost] timers:         %llu ns over %llu calls\n"
                "[cost] dma:            %llu ns over %llu calls\n",
                g_cost_dispatch_ns, g_cost_dispatch_calls,
                g_cost_halt_ns, g_cost_halt_calls, g_cost_halt_iters,
                g_cost_pump_idle_ns, g_cost_pump_idle_calls,
                g_cost_tick_total_calls,
                g_cost_tick_devices_ns, g_cost_tick_devices_calls,
                g_cost_ppu_render_ns, g_cost_ppu_render_calls,
                g_cost_recompute_budget_ns, g_cost_recompute_budget_calls,
                g_cost_drain_dma_call_calls,
                g_cost_irq_check_ns, g_cost_irq_check_calls,
                g_cost_irq_handler_ns, g_cost_irq_handler_calls,
                g_cost_bus_slow_ns, g_cost_bus_slow_calls,
                g_cost_audio_tick_ns, g_cost_audio_tick_calls,
                g_cost_timers_ns, g_cost_timers_calls,
                g_cost_dma_ns, g_cost_dma_calls);
            std::fprintf(stderr,
                "[cost] mp2k_trace: stores=%llu accepted=%llu "
                "pre=%llu post=%llu exact_writers=%llu filter_ns=%llu\n"
                "[cost] mp2k_hooks: checks=%llu matches=%llu deep=%llu\n"
                "[cost] mp2k_audio: frame_hooks=%llu render=%llu ns/%llu "
                "producer_blocks=%llu ns/%llu judge_output=%llu ns/%llu\n",
                g_cost_mp2k_trace_stores, g_cost_mp2k_fast_accepts,
                g_cost_mp2k_hook_pre, g_cost_mp2k_hook_post,
                g_cost_mp2k_exact_writer_matches, g_cost_mp2k_fast_filter_ns,
                g_cost_mp2k_hook_checks, g_cost_mp2k_hook_matches,
                g_cost_mp2k_hook_deep_calls,
                g_cost_mp2k_frame_hooks, g_cost_mp2k_render_ns,
                g_cost_mp2k_render_calls, g_cost_mp2k_producer_blocks,
                g_cost_mp2k_producer_blocks_ns,
                g_cost_mp2k_judge_output_calls,
                g_cost_mp2k_judge_output_ns);
            std::fprintf(stderr, "[cost] bookkeeping calls (logical, nested helpers counted at each level):");
            for (int i = 0; i < kBkCount; ++i)
                std::fprintf(stderr, " %s=%llu", kCostBkNames[i], g_cost_bk[i]);
            std::fprintf(stderr, "\n");
        });
    }
    return on;
}();
extern "C" bool g_cost_probe_enabled(void) { return g_cost_probe; }
// One predictable branch when the probe is off; a plain increment when on.
static inline void cost_bk(int slot) {
    if (g_cost_probe) ++g_cost_bk[slot];
}
struct CostTimer {
    unsigned long long* ns_accum;
    unsigned long long* call_accum;
    std::chrono::steady_clock::time_point t0;
    explicit CostTimer(unsigned long long* ns, unsigned long long* calls)
        : ns_accum(ns), call_accum(calls) {
        if (g_cost_probe) t0 = std::chrono::steady_clock::now();
    }
    ~CostTimer() {
        if (!g_cost_probe) return;
        *ns_accum += static_cast<unsigned long long>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0).count());
        ++*call_accum;
    }
};

static unsigned long long g_runtime_yielded_vblank_start = 0;
// Live IRQ-handler nesting depth (defined in armv4t/runtime_arm.cpp: ++ on IRQ
// entry, -- after the handler unwinds). Used by the vblank path to distinguish
// a host-only present (safe) from a guest-stack unwind (never safe in an IRQ).
extern "C" uint32_t g_irq_nest_depth;

// Host-only presentation context. An IRQ-safe present must never unwind the
// guest or request quit: runtime_irq() is synchronously driving the handler.
extern "C" uint32_t g_runtime_frame_present_in_irq = 0;
// Defined in runtime_arm.cpp so the unpacker-slot journal can record it.
extern "C" bool g_frame_present_in_progress;

// Cumulative guest-cycle clock (MC-HP-002 cycle-aligned divergence hunt).
// Incremented by runtime_tick on EVERY tick — both the per-instruction exec
// ticks emitted by generated code and the halt-pump chunks — so it is the
// authoritative total guest-cycle count. (runtime.cpp's `cycles_elapsed` only
// summed the halt path, which is why it was incomparable to the interpreter's
// fixed-quantum clock.) runtime_trace_event stamps this onto every ring entry
// so the recomp and the bios_smoke interp oracle align by identical cycles.
extern "C" unsigned long long g_runtime_cycles = 0;

// ── Stage 2 idle-loop elision: disturbance epoch + skip counters ─────────────
// Bumped whenever something happens that could change a watched poll value or
// the timing rules: a guest memory write, an MMIO read (disqualifies MMIO
// polling), or a device-event materialization (tick_devices). The per-site
// prover (runtime_idle_backedge) requires it unchanged across the two proof
// iterations. See runtime_arm.h.
extern "C" unsigned long long g_idle_disturb_epoch = 0;
// Direct EWRAM/IWRAM pointers for the inline fast path in runtime_arm.h.
// Installed by set_active_bus(); null until then, which routes every access
// through the slow path.
extern "C" uint8_t* g_fast_ewram = nullptr;
extern "C" uint8_t* g_fast_iwram = nullptr;
// Direct ROM pointer/size for the inline fast path in runtime_arm.h, plus the
// "is this cart eligible" flag. Installed by set_active_bus(); g_fast_rom_ok
// stays false (slow path only) until proven true from the bus's own RTC /
// save-type state — see set_active_bus() below.
extern "C" const uint8_t* g_fast_rom      = nullptr;
extern "C" uint32_t       g_fast_rom_size = 0;
extern "C" int            g_fast_rom_ok   = 0;
extern "C" unsigned long long g_runtime_state_epoch = 0;
// Diagnostics for the exit banner: cycles/iterations the prover fast-forwarded.
static unsigned long long g_idle_skipped_cycles = 0;
static unsigned long long g_idle_skipped_iters  = 0;
static unsigned long long g_idle_confirmed_sites = 0;

// P6 sljit differential gate — shadow-tick mode. While g_runtime_shadow_tick is
// set (during a throwaway validation or transactional guest re-run), runtime_tick
// accumulates the shard's cycle cost into g_runtime_shadow_cycles and does
// NOTHING else: no device pump, no IRQ delivery, no real-clock advance. MMIO
// catch-up/rescheduling obey the same contract below; otherwise merely reaching
// a trapped MMIO access could mutate devices before the transaction rejects it.
// Default 0 → normal play / the gcc path are untouched.
extern "C" unsigned          g_runtime_shadow_tick   = 0;
extern "C" unsigned long long g_runtime_shadow_cycles = 0;

// Memory timing table (runtime_mem_cycles below), exported so the
// block-timing game code (tools/block_timing.py) can read it inline instead
// of calling runtime_mem_cycles: [region][width == 4][sequential].
// g_runtime_mem_cost_key is the WAITCNT value the table was built for
// (0xFFFFFFFF: not built); g_runtime_waitcnt_live points at the active bus's
// live WAITCNT bytes (null without a bus). A reader uses the table only when
// the live value equals the key, and otherwise calls runtime_mem_cycles,
// which rebuilds it.
extern "C" uint8_t g_runtime_mem_cost[16][2][2] = {};
extern "C" uint32_t g_runtime_mem_cost_key = 0xFFFFFFFFu;
extern "C" const uint8_t* g_runtime_waitcnt_live = nullptr;

namespace gbarecomp {

static gba::GbaBus* g_active_bus = nullptr;
static gba::GbaPpu* g_active_ppu = nullptr;
static std::function<void()> g_frame_snapshot_hook;
static std::function<void()> g_scene_capture_hook;
static std::function<bool(std::uint8_t*, std::uint32_t, std::uint32_t)>
    g_frame_present_override_hook;
static std::function<void()> g_savestate_load_hook;
static std::function<void()> g_guest_step_boundary_hook;
// See runtime_set_frame_render_gate_hook / runtime_bus_bridge.h.
static std::function<bool(unsigned long long)> g_frame_render_gate_hook;
static RuntimeGuestStepBoundaryHook g_chained_guest_step_boundary_hook =
    nullptr;

static void runtime_guest_step_boundary_dispatch() {
    if (g_chained_guest_step_boundary_hook)
        g_chained_guest_step_boundary_hook();
    if (g_guest_step_boundary_hook) g_guest_step_boundary_hook();
}

// ── Guest-PC sampling profiler (debug tooling) ─────────────────────────
// A background thread samples the guest PC (g_cpu.R[15]) while the
// single-threaded interpreter core runs, building a histogram. Racy
// reads are fine for statistical sampling. Enabled by GBARECOMP_SAMPLE;
// the top hot PCs are printed to stderr at process exit. This is what
// localized the MC-HP-002 transition freeze to the M4A sound-engine
// sequence walker (0x08004286) when reasoning + per-call timing failed.
static std::thread       g_sampler;
static std::atomic<bool> g_sampling{false};
static std::unordered_map<uint32_t, uint64_t> g_pc_hist;  // sampler-thread only

static void dump_sample_hist(const char* tag) {
    std::vector<std::pair<uint32_t, uint64_t>> v(g_pc_hist.begin(),
                                                 g_pc_hist.end());
    std::sort(v.begin(), v.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    uint64_t total = 0;
    for (const auto& p : v) total += p.second;
    if (total == 0) return;

    auto* ppu = g_active_ppu;
    std::fprintf(stderr,
                 "[sample] %s samples=%llu current_pc=0x%08X cpsr=0x%08X "
                 "cycles=%llu",
                 tag ? tag : "dump",
                 static_cast<unsigned long long>(total),
                 g_cpu.R[15],
                 g_cpu.cpsr,
                 static_cast<unsigned long long>(g_runtime_cycles));
    if (ppu) {
        std::fprintf(stderr, " ppu_frame=%llu vcount=%u",
                     static_cast<unsigned long long>(ppu->frame_count()),
                     static_cast<unsigned>(ppu->vcount()));
    }
    std::fprintf(stderr, "\n");

    for (std::size_t i = 0; i < v.size() && i < 30; ++i) {
        uint32_t off = 0;
        const char* sym = gba_symbol_lookup(v[i].first, &off);
        char symbuf[96];
        symbuf[0] = '\0';
        if (sym) {
            std::snprintf(symbuf, sizeof(symbuf), " <%s+0x%X>", sym, off);
        }
        std::fprintf(stderr, "  0x%08X%s  %5.2f%%  (%llu)\n",
                     v[i].first,
                     symbuf,
                     100.0 * static_cast<double>(v[i].second) /
                         static_cast<double>(total),
                     static_cast<unsigned long long>(v[i].second));
    }
    std::fflush(stderr);
}

static long long sampler_live_seconds() {
    const char* e = std::getenv("GBARECOMP_SAMPLE_LIVE_SECONDS");
    long long s = e ? std::atoll(e) : 0;
    return s > 0 ? s : 0;
}

static void sampler_loop() {
    const long long live_secs = sampler_live_seconds();
    auto next_dump = std::chrono::steady_clock::now() +
        std::chrono::seconds(live_secs > 0 ? live_secs : 86400);
    while (g_sampling.load(std::memory_order_relaxed)) {
        g_pc_hist[g_cpu.R[15]]++;  // racy read; approximate is fine
        if (live_secs > 0 && std::chrono::steady_clock::now() >= next_dump) {
            dump_sample_hist("live");
            next_dump = std::chrono::steady_clock::now() +
                std::chrono::seconds(live_secs);
        }
        std::this_thread::sleep_for(std::chrono::microseconds(250));
    }
}

// ── HOST profiler (GBARECOMP_HOST_PROF=<path>) ──────────────────────────────
// The sampler above records the GUEST pc, which answers "what game code is
// hot". It cannot answer "what HOST code is hot" — dispatch lookups, the
// per-instruction tick, bus helpers — which is the question that matters for
// throughput. External profilers are not usable here: the MinGW build emits
// DWARF, and Windows Performance Analyzer wants PDB, so a 29,000-function
// binary shows up as raw addresses.
//
// So: sample the emulation thread's instruction pointer directly. Suspend it,
// read RIP, resume — the standard sampling-profiler move. ~1 kHz, so the cost
// is negligible and it perturbs timing far less than instrumenting anything.
// Writes "rip count" lines plus the module base; symbolise offline against
// `nm` output (the exe keeps ~83k symbols). Fully off unless the env var is
// set — no thread, no clock reads, nothing.
//
// The tick is a HIGH-RESOLUTION waitable timer on an absolute 1 ms schedule
// (see host_prof_loop). std::this_thread::sleep_for(1ms) runs at the Windows
// timer resolution (~15.6 ms), which is how one 32 s capture got 808 samples.
// The timer belongs to the sampler thread alone and exists only while the
// profiler runs: nothing here changes the process-wide timer resolution, so
// the frame limiter's sleeps behave exactly as they do with the profiler off.
//
// Besides the aggregated "0x<rip> <count>" lines, every sample is also kept
// raw (rip, guest frame, host phase id, first in-module caller) and written
// under "# samples" -- that is what lets a profile be restricted to slow
// frames or split by phase. See host_prof_phase.h.

// Phase / frame mirrors read by the sampler (host_prof_phase.h). Defined on
// every platform: the scopes that write them compile everywhere.
bool g_host_prof_enabled = false;
std::atomic<std::uint8_t> g_host_prof_phase{0};
std::atomic<std::uint64_t> g_host_prof_frame{0};
std::atomic<std::uint64_t> g_host_prof_present_frame{0};

[[maybe_unused]] static const char* host_prof_phase_name(unsigned id) {
    switch (id) {
        case kHpGuest:       return "guest";
        case kHpIrqHandler:  return "irq_handler";
        case kHpTickDevices: return "tick_devices";
        case kHpPpuRender:   return "ppu_render";
        case kHpRender:      return "render";
        case kHpPresent:     return "present";
        case kHpAudio:       return "audio";
        case kHpPump:        return "pump";
        case kHpPacer:       return "pacer";
        case kHpHaltPump:    return "halt_pump";
        case kHpCompileWait: return "compile_wait";
        case kHpSdlEvents:   return "sdl_events";
        default:             return "unknown";
    }
}

#ifdef _WIN32
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
static std::thread                                 g_host_prof;
static std::atomic<bool>                           g_host_profiling{false};
static std::unordered_map<unsigned long long, unsigned long long> g_host_hist;
// The RIP histogram alone cannot identify who called a hot CRT helper such
// as getenv. Keep the sampled RIP and the first return address outside that
// CRT together; no environment values or stack contents are written.
static std::map<std::pair<unsigned long long, unsigned long long>,
                unsigned long long> g_host_crt_callers;
static uintptr_t g_host_crt_begin = 0;
static uintptr_t g_host_crt_end = 0;
static void* g_host_thread_handle = nullptr;
// Address ranges of the two modules that hold code we own: the engine exe and
// the translated game code DLL (absent in a single-exe build). A sample
// outside both gets its first return address inside them recorded as caller.
static uintptr_t g_host_exe_begin = 0;
static uintptr_t g_host_exe_end = 0;
static uintptr_t g_host_game_begin = 0;
static uintptr_t g_host_game_end = 0;
struct HostProfSample {
    unsigned long long rip;
    unsigned long long caller;       // first in-module return address, or 0
    std::uint32_t frame;             // PPU frame counter (g_host_prof_frame)
    std::uint32_t present_frame;     // frame key of the last phase-CSV row
    std::uint8_t phase;              // HostProfPhase
};
// Raw samples, pre-reserved so a push never allocates. Capped: at ~1 kHz the
// cap is about 33 minutes; later samples still reach the aggregated histogram
// and are counted in "# raw_dropped".
static constexpr std::size_t kHostProfMaxRawSamples = 2000000;
static std::vector<HostProfSample> g_host_samples;
static unsigned long long g_host_samples_dropped = 0;
// Sampler-thread only until it is joined: ticks skipped because the frame
// limiter was asleep on purpose, and the wall span the loop ran for.
static unsigned long long g_host_idle_ticks = 0;
static std::chrono::steady_clock::time_point g_host_loop_t0;
static std::chrono::steady_clock::time_point g_host_loop_t1;
static const char* g_host_timer_method = "none";
// Set while the frame limiter is asleep on purpose; the sampler skips
// those ticks entirely, so what remains is work.
static std::atomic<bool> g_host_prof_idle{false};

// One step up the stack. False when the walk cannot continue (no way to read
// the return address, a null address, or a stack pointer that did not rise).
static bool host_prof_unwind_one(CONTEXT& ctx) {
    const DWORD64 previous_rsp = ctx.Rsp;
    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
    if (fn) {
        PVOID handler_data = nullptr;
        DWORD64 establisher_frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fn, &ctx,
                         &handler_data, &establisher_frame, nullptr);
    } else {
        // A leaf function has only its return address on the stack.
        DWORD64 return_address = 0;
        SIZE_T read = 0;
        if (!ReadProcessMemory(GetCurrentProcess(),
                reinterpret_cast<const void*>(ctx.Rsp), &return_address,
                sizeof(return_address), &read) || read != sizeof(return_address))
            return false;
        ctx.Rip = return_address;
        ctx.Rsp += sizeof(return_address);
    }
    return ctx.Rip && ctx.Rsp > previous_rsp;
}

static unsigned long long host_prof_crt_caller(CONTEXT ctx) {
    if (!g_host_crt_begin || ctx.Rip < g_host_crt_begin ||
        ctx.Rip >= g_host_crt_end) return 0;
    while (ctx.Rip >= g_host_crt_begin && ctx.Rip < g_host_crt_end) {
        if (!host_prof_unwind_one(ctx)) return 0;
    }
    return ctx.Rip;
}

static bool host_prof_in_own_code(unsigned long long rip) {
    return (g_host_exe_begin && rip >= g_host_exe_begin && rip < g_host_exe_end) ||
           (g_host_game_begin && rip >= g_host_game_begin && rip < g_host_game_end);
}

// For a sample outside both the exe and the game DLL (ntdll, the graphics
// driver, SDL, the CRT ...): the first return address that IS inside one of
// them, i.e. which of our functions asked for the time. 0 when the sample is
// already ours, or no such frame within 64 frames.
static unsigned long long host_prof_module_caller(CONTEXT ctx) {
    if (host_prof_in_own_code(ctx.Rip)) return 0;
    for (int depth = 0; depth < 64; ++depth) {
        if (!host_prof_unwind_one(ctx)) return 0;
        if (host_prof_in_own_code(ctx.Rip)) return ctx.Rip;
    }
    return 0;
}

static void host_prof_resolve_game_dll() {
    if (g_host_game_begin) return;
    HMODULE game = GetModuleHandleW(L"GoldenSunGame.dll");
    if (!game) return;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(game);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const char*>(game) + dos->e_lfanew);
    g_host_game_end = reinterpret_cast<uintptr_t>(game) +
                      nt->OptionalHeader.SizeOfImage;
    g_host_game_begin = reinterpret_cast<uintptr_t>(game);  // set last
}

static void host_prof_loop() {
    // High-resolution waitable timer, owned by this thread. If the OS refuses
    // it (pre-Windows 10 1803) fall back to sleep_for and say so in the dump:
    // the achieved rate is then the coarse timer's.
    HANDLE timer = CreateWaitableTimerExW(
        nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS);
    g_host_timer_method = timer ? "high_resolution_waitable_timer"
                                : "sleep_for_fallback";
    using clock = std::chrono::steady_clock;
    constexpr auto kPeriod = std::chrono::milliseconds(1);
    g_host_loop_t0 = clock::now();
    auto next = g_host_loop_t0;
    unsigned long long iteration = 0;
    while (g_host_profiling.load(std::memory_order_relaxed)) {
        // The thread is running here, so taking the loader lock is safe. The
        // game DLL is normally an import and already present at start; this
        // is only a late retry.
        if (!g_host_game_begin && (++iteration & 1023u) == 0)
            host_prof_resolve_game_dll();
        HANDLE h = reinterpret_cast<HANDLE>(g_host_thread_handle);
        if (h && g_host_prof_idle.load(std::memory_order_relaxed)) {
            ++g_host_idle_ticks;
        } else if (h) {
            if (SuspendThread(h) != (DWORD)-1) {
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_FULL;
                const bool sampled = GetThreadContext(h, &ctx) != 0;
                // Read while frozen so RIP, phase and frame describe the
                // same instant.
                HostProfSample s{};
                s.phase = g_host_prof_phase.load(std::memory_order_relaxed);
                s.frame = static_cast<std::uint32_t>(
                    g_host_prof_frame.load(std::memory_order_relaxed));
                s.present_frame = static_cast<std::uint32_t>(
                    g_host_prof_present_frame.load(std::memory_order_relaxed));
                const unsigned long long crt_caller = sampled
                    ? host_prof_crt_caller(ctx) : 0;
                s.caller = sampled ? host_prof_module_caller(ctx) : 0;
                s.rip = sampled ? ctx.Rip : 0;
                ResumeThread(h);
                // Map insertion may allocate: do it only after resuming the
                // sampled thread, which could have been holding a heap lock.
                if (sampled) {
                    ++g_host_hist[ctx.Rip];
                    if (crt_caller) ++g_host_crt_callers[{ctx.Rip, crt_caller}];
                    if (g_host_samples.size() < kHostProfMaxRawSamples)
                        g_host_samples.push_back(s);  // capacity is reserved
                    else
                        ++g_host_samples_dropped;
                }
            }
        }
        // Absolute schedule: a late wakeup is made up by the next one, so the
        // long-run rate holds at 1 kHz. If the thread fell far behind (a stall
        // in the sampled process), resynchronise instead of bursting.
        next += kPeriod;
        const auto now = clock::now();
        if (next > now) {
            if (timer) {
                const auto ns = std::chrono::duration_cast<
                    std::chrono::nanoseconds>(next - now).count();
                LARGE_INTEGER due;
                due.QuadPart = -(ns / 100);  // relative, 100 ns units
                if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
                    WaitForSingleObject(timer, 100);
                else
                    std::this_thread::sleep_for(next - now);
            } else {
                std::this_thread::sleep_for(next - now);
            }
        } else if (now - next > std::chrono::milliseconds(20)) {
            next = now;
        }
    }
    g_host_loop_t1 = clock::now();
    if (timer) CloseHandle(timer);
}

static void dump_host_hist() {
    const char* path = std::getenv("GBARECOMP_HOST_PROF");
    if (!path || !path[0]) return;
    std::FILE* f = std::fopen(path, "w");
    if (!f) return;
    // Module base, so RIPs can be turned into link-time addresses offline.
    std::fprintf(f, "# module_base 0x%llx\n",
                 (unsigned long long)(uintptr_t)GetModuleHandleW(nullptr));
    const auto* module = reinterpret_cast<const char*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(module + dos->e_lfanew);
    std::fprintf(f, "# module_size 0x%lx\n", nt->OptionalHeader.SizeOfImage);
    // The translated game code, when it is a separate DLL. 0 = single exe.
    std::fprintf(f, "# game_dll_base 0x%llx\n",
                 (unsigned long long)g_host_game_begin);
    std::fprintf(f, "# game_dll_size 0x%llx\n",
                 (unsigned long long)(g_host_game_end - g_host_game_begin));
    unsigned long long total = 0;
    for (const auto& kv : g_host_hist) total += kv.second;
    std::fprintf(f, "# total_samples %llu\n", total);
    // Achieved rate: samples taken / wall seconds the sampler loop ran.
    // tick_hz also counts the ticks skipped while the frame limiter slept.
    const double wall = std::chrono::duration<double>(
        g_host_loop_t1 - g_host_loop_t0).count();
    std::fprintf(f, "# sample_wall_seconds %.3f\n", wall);
    std::fprintf(f, "# sample_hz %.1f\n", wall > 0.0 ? total / wall : 0.0);
    std::fprintf(f, "# idle_ticks %llu\n", g_host_idle_ticks);
    std::fprintf(f, "# tick_hz %.1f\n",
                 wall > 0.0 ? (total + g_host_idle_ticks) / wall : 0.0);
    std::fprintf(f, "# sample_timer %s\n", g_host_timer_method);
    std::fprintf(f, "# raw_samples %zu\n", g_host_samples.size());
    std::fprintf(f, "# raw_cap %zu\n", kHostProfMaxRawSamples);
    std::fprintf(f, "# raw_dropped %llu\n", g_host_samples_dropped);
    for (unsigned id = 0; id < kHpPhaseCount; ++id)
        std::fprintf(f, "# phase %u %s\n", id, host_prof_phase_name(id));
    std::fprintf(f, "# crt_range 0x%llx 0x%llx\n",
                 (unsigned long long)g_host_crt_begin,
                 (unsigned long long)g_host_crt_end);
    for (const auto& kv : g_host_hist)
        std::fprintf(f, "0x%llx %llu\n", kv.first, kv.second);
    for (const auto& kv : g_host_crt_callers)
        std::fprintf(f, "# crt_caller 0x%llx 0x%llx %llu\n",
                     kv.first.first, kv.first.second, kv.second);

    // Which DLL each out-of-module sample belongs to. Without this a capture
    // can only say "62% of the time was outside the executable", which names
    // no suspect and cannot be acted on -- the graphics driver, a syscall
    // stub and the frame limiter all look identical as bare addresses.
    // Resolved here, at dump time, from the sampled addresses themselves:
    // GetModuleHandleEx with FROM_ADDRESS needs no module enumeration and no
    // extra library, and UNCHANGED_REFCOUNT means this cannot affect the
    // lifetime of anything it looks at.
    {
        std::map<uintptr_t, std::pair<uintptr_t, std::wstring>> seen;
        for (const auto& kv : g_host_hist) {
            HMODULE mod = nullptr;
            if (!GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(
                        static_cast<uintptr_t>(kv.first)),
                    &mod) ||
                !mod) {
                continue;
            }
            const auto base = reinterpret_cast<uintptr_t>(mod);
            if (seen.find(base) != seen.end()) continue;
            wchar_t path[MAX_PATH] = {};
            if (!GetModuleFileNameW(mod, path, MAX_PATH)) continue;
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mod);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
                reinterpret_cast<const char*>(mod) + dos->e_lfanew);
            seen.emplace(base,
                         std::make_pair(
                             static_cast<uintptr_t>(
                                 nt->OptionalHeader.SizeOfImage),
                             std::wstring(path)));
        }
        for (const auto& kv : seen) {
            // Basename only: the full path adds nothing and can carry a user
            // name.
            const std::wstring& full = kv.second.second;
            const std::size_t cut = full.find_last_of(L"\\/");
            const std::wstring name =
                cut == std::wstring::npos ? full : full.substr(cut + 1);
            std::fprintf(f, "# module 0x%llx 0x%llx %ls\n",
                         (unsigned long long)kv.first,
                         (unsigned long long)kv.second.first, name.c_str());
        }
    }

    // Raw samples, last so the aggregated section above stays first.
    //   rip(hex) frame(dec) phase(dec) caller(hex, 0 = none) present_frame(dec)
    // The fifth column is the frame key of the last row written to the
    // frame-phase CSV before the sample; the sample belongs to the CSV row
    // AFTER it (see g_host_prof_present_frame).
    std::fprintf(f, "# samples\n");
    for (const HostProfSample& s : g_host_samples) {
        std::fprintf(f, "0x%llx %u %u ", s.rip, s.frame,
                     static_cast<unsigned>(s.phase));
        if (s.caller) std::fprintf(f, "0x%llx", s.caller);
        else std::fputc('0', f);
        std::fprintf(f, " %u\n", s.present_frame);
    }
    std::fclose(f);
    std::fprintf(stderr,
                 "[host-prof] %llu samples over %zu distinct RIPs, %.0f Hz "
                 "(%s), %llu idle ticks -> %s\n",
                 total, g_host_hist.size(), wall > 0.0 ? total / wall : 0.0,
                 g_host_timer_method, g_host_idle_ticks, path);
}

static void start_host_prof() {
    if (!std::getenv("GBARECOMP_HOST_PROF")) return;
    if (g_host_profiling.exchange(true)) return;  // start once
    if (HMODULE crt = GetModuleHandleW(L"msvcrt.dll")) {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(crt);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
            reinterpret_cast<const char*>(crt) + dos->e_lfanew);
        g_host_crt_begin = reinterpret_cast<uintptr_t>(crt);
        g_host_crt_end = g_host_crt_begin + nt->OptionalHeader.SizeOfImage;
    }
    if (HMODULE exe = GetModuleHandleW(nullptr)) {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
            reinterpret_cast<const char*>(exe) + dos->e_lfanew);
        g_host_exe_begin = reinterpret_cast<uintptr_t>(exe);
        g_host_exe_end = g_host_exe_begin + nt->OptionalHeader.SizeOfImage;
    }
    host_prof_resolve_game_dll();
    g_host_samples.reserve(kHostProfMaxRawSamples);
    HANDLE dup = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                    GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS);
    g_host_thread_handle = dup;
    // Phase scopes and the frame mirror only write while this is set.
    g_host_prof_enabled = true;
    if (g_active_ppu)
        g_host_prof_frame.store(g_active_ppu->frame_count(),
                                std::memory_order_relaxed);
    g_host_prof = std::thread(host_prof_loop);
    std::atexit([] {
        g_host_profiling.store(false);
        if (g_host_prof.joinable()) g_host_prof.join();
        dump_host_hist();
    });
}
#else
static void start_host_prof() {}
#endif

// Marks the frame limiter's deliberate sleep so the sampler skips it. Defined
// on every platform because the header declares it unconditionally and
// runtime.cpp calls it from the pacer path; the body is a no-op where there
// is no sampler.
// The limiter waits are not nested, so one saved phase is enough. Samples are
// skipped while idle, so kHpPacer never appears in a capture; it is set so the
// phase is right for anything that reads it during the wait.
static std::uint8_t g_host_idle_prev_phase = 0;
void host_prof_begin_idle() {
#ifdef _WIN32
    g_host_prof_idle.store(true, std::memory_order_relaxed);
#endif
    if (g_host_prof_enabled) {
        g_host_idle_prev_phase =
            g_host_prof_phase.load(std::memory_order_relaxed);
        g_host_prof_phase.store(kHpPacer, std::memory_order_relaxed);
    }
}
void host_prof_end_idle() {
#ifdef _WIN32
    g_host_prof_idle.store(false, std::memory_order_relaxed);
#endif
    if (g_host_prof_enabled)
        g_host_prof_phase.store(g_host_idle_prev_phase,
                                std::memory_order_relaxed);
}

// GBARECOMP_SAMPLE, if explicitly set, wins outright. Otherwise falls back
// to the config UI's "Additional debug logging" toggle (default OFF) — see
// runtime_arm.h. Read once here, at the single point this thread can ever
// start (called once from set_active_bus, at cart load — after the config
// UI has loaded config.ini): flipping the toggle later in the same run
// cannot retroactively start this background sampler thread.
static bool sample_requested() {
    const char* env = std::getenv("GBARECOMP_SAMPLE");
    return env ? (env[0] != '\0' && env[0] != '0')
               : (gsr_additional_debug_logging() != 0);
}

static void start_sampler() {
    if (!sample_requested()) return;
    if (g_sampling.exchange(true)) return;  // start once
    g_sampler = std::thread(sampler_loop);
    std::atexit([] {
        g_sampling.store(false);
        if (g_sampler.joinable()) g_sampler.join();
        dump_sample_hist("exit");
    });
}

bool should_trace_unmapped_read(uint32_t addr) {
    return (addr >> 24) >= 0x0Eu;
}

void trace_unmapped_read(uint32_t addr, uint32_t value, uint32_t width) {
    if (should_trace_unmapped_read(addr)) {
        runtime_trace_event(RUNTIME_TRACE_MEM_READ, g_cpu.R[15], addr, value,
                            width);
    }
}

void sync_bios_access() {
    if (g_active_bus) {
        g_active_bus->set_bios_access_enabled(g_cpu.R[15] < 0x00004000u);
        // Stash the live PC so an unmapped (open-bus) read returns the CURRENT
        // prefetch of the executing code (GBATEK open-bus). MC-HP-002.
        g_active_bus->note_pc(g_cpu.R[15], (g_cpu.cpsr & CPSR_T_BIT) != 0);
    }
}

extern "C" void runtime_mp2k_write_observer(uint32_t pc, uint32_t addr,
                                               uint32_t value, uint32_t width,
                                               uint32_t operand_sample,
                                               uint32_t operand_gain,
                                               uint32_t before_value,
                                               uint8_t thumb,
                                               uint8_t pre_write) {
    if (g_active_bus)
        g_active_bus->audio().mp2k_pcm_write_hook(
            pc, addr, value, width, operand_sample, operand_gain,
            g_runtime_cycles, before_value, thumb, pre_write);
}

extern "C" bool runtime_mp2k_write_relevant(uint32_t pc, uint32_t addr,
                                               uint32_t width) {
    // No active bus means no cached range; fail open for test/bootstrap and
    // never suppress a possible observer event.
    return !g_active_bus ||
        g_active_bus->audio().mp2k_write_may_be_relevant(pc, addr, width);
}

extern "C" bool runtime_mp2k_control_relevant(uint32_t pc) {
    // No active bus means no filter; fail open so bootstrap/interpreter paths
    // retain the established observer semantics.
    return !g_active_bus ||
        g_active_bus->audio().mp2k_control_hook_may_be_relevant(pc);
}

extern "C" void runtime_mp2k_control_observer(uint32_t pc, uint32_t target,
                                                uint64_t cycles,
                                                uint8_t thumb) {
    if (g_active_bus)
        g_active_bus->audio().mp2k_control_hook(pc, target, cycles, thumb);
}

void set_active_bus(gba::GbaBus* bus) {
    g_active_bus = bus;
    // WAITCNT is the u16 at IO offset 0x204 (gba_io.h, IoReg::WAITCNT).
    g_runtime_waitcnt_live = bus ? bus->io().raw() + 0x204u : nullptr;
    g_runtime_mem_cost_key = 0xFFFFFFFFu;
    g_runtime_bios_open_bus_hook = bus
        ? [](uint32_t value) {
              if (g_active_bus) g_active_bus->set_bios_open_bus(value);
          }
        : nullptr;
    g_runtime_mp2k_write_hook = bus ? &runtime_mp2k_write_observer : nullptr;
    g_runtime_mp2k_control_hook = bus ? &runtime_mp2k_control_observer : nullptr;
    g_fast_ewram = bus ? bus->ewram_ptr() : nullptr;
    g_fast_iwram = bus ? bus->iwram_ptr() : nullptr;
    g_fast_rom      = bus ? bus->rom_ptr()  : nullptr;
    g_fast_rom_size = bus ? static_cast<uint32_t>(bus->rom_size()) : 0u;
    // "This cart can never take the RTC/GPIO or EEPROM Region::Rom branches
    // in gba_bus.cpp" — i.e. it has no active RTC and its save type is not
    // EEPROM. Both are properties of the loaded cart, fixed for the run, and
    // read straight from the bus's own accessors (never guessed). A false
    // value here (including "no bus installed") just routes every ROM access
    // through the slow path, which is always correct.
    g_fast_rom_ok = (bus && bus->rom_ptr() && bus->rom_size() > 0 &&
                     !bus->rtc().active() && !bus->save().eeprom_enabled())
                        ? 1
                        : 0;
    start_sampler();     // no-op unless GBARECOMP_SAMPLE is set
    start_host_prof();   // no-op unless GBARECOMP_HOST_PROF is set
}

void set_active_ppu(gba::GbaPpu* ppu) {
    g_active_ppu = ppu;
}

void set_frame_snapshot_hook(std::function<void()> hook) {
    g_frame_snapshot_hook = std::move(hook);
}

void set_scene_capture_hook(std::function<void()> hook) {
    g_scene_capture_hook = std::move(hook);
}

void set_frame_present_override_hook(
    std::function<bool(std::uint8_t*, std::uint32_t, std::uint32_t)> hook) {
    g_frame_present_override_hook = std::move(hook);
}

bool invoke_frame_present_override_hook(std::uint8_t* rgb, std::uint32_t width,
                                        std::uint32_t height) {
    if (!g_frame_present_override_hook) return false;
    return g_frame_present_override_hook(rgb, width, height);
}

void set_savestate_load_hook(std::function<void()> hook) {
    g_savestate_load_hook = std::move(hook);
}

void notify_savestate_loaded() {
    // The load moved the PPU frame counter discontinuously; re-mirror it for
    // the host profiler (no-op unless it is running).
    if (g_host_prof_enabled && g_active_ppu)
        g_host_prof_frame.store(g_active_ppu->frame_count(),
                                std::memory_order_relaxed);
    if (g_savestate_load_hook) g_savestate_load_hook();
}

void runtime_set_frame_render_gate_hook(
    std::function<bool(unsigned long long)> hook) {
    g_frame_render_gate_hook = std::move(hook);
}

gba::GbaBus* active_bus() {
    return g_active_bus;
}

gba::GbaPpu* active_ppu() {
    return g_active_ppu;
}

void runtime_set_guest_step_boundary_hook(std::function<void()> hook) {
    if (!hook && !g_guest_step_boundary_hook) return;
    if (hook && !g_guest_step_boundary_hook) {
        g_chained_guest_step_boundary_hook =
            g_runtime_guest_step_boundary_hook;
    }
    g_guest_step_boundary_hook = std::move(hook);
    if (g_guest_step_boundary_hook) {
        g_runtime_guest_step_boundary_hook =
            &runtime_guest_step_boundary_dispatch;
    } else {
        g_runtime_guest_step_boundary_hook =
            g_chained_guest_step_boundary_hook;
        g_chained_guest_step_boundary_hook = nullptr;
    }
}

// Always-on hang watchdog capture. Called once when the watchdog trips;
// snapshots the live MP2K channel state (the MC-HP-002 spin walks a
// corrupt voice pointer) to stderr and hang_dump.log next to the runner.
// Pure observation — does not alter execution. See PRINCIPLES.md
// "always-on ring first": the freeze documents itself, no attach timing.
static void dump_hang_state(const char* reason) {
    if (!g_active_bus) return;
    std::string m4a;
    gba::mp2k_dump_live(*g_active_bus, m4a);

    // Full CPU register snapshot — the hang is a busy loop, so r0..r15 pin the
    // object/pointer it is walking (MC-HP-002: UpdateAnimationVariableFrames
    // walks a corrupt frame-list pointer r1 = *(animObj+0x5c)). Also dump the
    // candidate animation object (r0) and the word it loads at +0x5c so the
    // corrupt value is attributable without a second run.
    char regs[512];
    int n = 0;
    for (int i = 0; i < 16; ++i) {
        n += std::snprintf(regs + n, sizeof(regs) - n, "r%d=0x%08X ",
                           i, g_cpu.R[i]);
    }
    uint32_t r0 = g_cpu.R[0];
    uint32_t cmdptr = g_active_bus->read32(r0 + 0x5cu);
    char objbuf[256];
    int m = std::snprintf(objbuf, sizeof(objbuf),
                          "obj@r0=0x%08X  *(r0+0x5c)=0x%08X  obj[0x00..0x60]:",
                          r0, cmdptr);
    for (uint32_t off = 0; off < 0x60u && m < (int)sizeof(objbuf) - 4; off += 4) {
        m += std::snprintf(objbuf + m, sizeof(objbuf) - m, " %08X",
                           g_active_bus->read32(r0 + off));
    }

    std::fprintf(stderr,
                 "\n[hang-watchdog] %s\n  pc=0x%08X cpsr=0x%08X cycles=%llu "
                 "vblank_starts=%llu irq_nest_depth=%u frame_present_in_irq=%u\n"
                 "  %s\n  %s\n  m4a=%s\n",
                 reason, g_cpu.R[15], g_cpu.cpsr,
                 static_cast<unsigned long long>(g_runtime_cycles),
                 static_cast<unsigned long long>(g_runtime_vblank_starts),
                 g_irq_nest_depth, g_runtime_frame_present_in_irq,
                 regs, objbuf, m4a.c_str());
    if (FILE* f = std::fopen("hang_dump.log", "w")) {
        std::fprintf(f,
                     "reason=%s\npc=0x%08X\ncpsr=0x%08X\ncycles=%llu\n"
                     "vblank_starts=%llu\nirq_nest_depth=%u\n"
                     "frame_present_in_irq=%u\nregs=%s\n%s\nm4a=%s\n",
                     reason, g_cpu.R[15], g_cpu.cpsr,
                     static_cast<unsigned long long>(g_runtime_cycles),
                     static_cast<unsigned long long>(g_runtime_vblank_starts),
                     g_irq_nest_depth, g_runtime_frame_present_in_irq,
                     regs, objbuf, m4a.c_str());
        std::fclose(f);
    }

    // Self-documenting execution history: dump the tail of the always-on
    // per-instruction fingerprint ring so the PCs the game thread executed
    // leading INTO the freeze are on disk (PRINCIPLES.md "always-on ring
    // first" — query the ring for the window, never arm-then-capture). This
    // is what distinguishes a native cart spin (a small cycling PC set) from a
    // marching pointer walk (monotonic PC/addr) from an interp-bridge over RAM
    // code (PCs in 0x02/0x03) WITHOUT a second run. Requires GBARECOMP_INSN_TRACE
    // armed at launch; harmlessly writes 0 records if the ring is empty.
    uint32_t fpn = runtime_fp_save_tail_csv("hang_fp_tail.csv", 16384);
    std::fprintf(stderr, "[hang-watchdog] wrote hang_fp_tail.csv (%u records); "
                 "arm GBARECOMP_INSN_TRACE=1 if 0.\n", fpn);

    // Recent mem-write/branch trace ring (ALWAYS-ON — emitted unconditionally by
    // codegen, independent of GBARECOMP_INSN_TRACE). A busy-spin freeze loop
    // performs only reads, so this ring is FROZEN at the instant the spin began:
    // it still holds the WRITES that produced the corrupt state the loop chokes
    // on (e.g. whoever wrote the bad pointer being walked). Dumping it makes the
    // writer of a wrong value attributable from the freeze alone — trace-the-
    // writer without a second run (PRINCIPLES.md "find the first divergence").
    {
        static RuntimeTraceEntry tr[4096];
        uint32_t ntr = runtime_trace_copy_recent(tr, 4096);
        if (FILE* tf = std::fopen("hang_trace.csv", "w")) {
            std::fprintf(tf, "seq,cycles,kind,pc,addr,value,aux,"
                             "r0,r1,r2,r3,r4,r5,r12,sp,lr\n");
            for (uint32_t i = 0; i < ntr; ++i) {
                const RuntimeTraceEntry& e = tr[i];
                std::fprintf(tf,
                    "%u,%llu,%u,0x%08X,0x%08X,0x%08X,0x%X,"
                    "0x%08X,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X\n",
                    e.seq, static_cast<unsigned long long>(e.cycles), e.kind,
                    e.pc, e.addr, e.value, e.aux, e.r0, e.r1, e.r2, e.r3,
                    e.r4, e.r5, e.r12, e.r13, e.r14);
            }
            std::fclose(tf);
            std::fprintf(stderr,
                "[hang-watchdog] wrote hang_trace.csv (%u mem-write/branch "
                "records — frozen at spin onset; find the corrupt-value writer "
                "here).\n", ntr);
        }
    }
}

}  // namespace gbarecomp

extern "C" unsigned long long runtime_current_frame() {
    const gba::GbaPpu* ppu = gbarecomp::active_ppu();
    return ppu ? static_cast<unsigned long long>(ppu->frame_count()) : 0ull;
}

// ── Stage 1: lazy device catch-up (MMIO access hooks) ───────────────────────
// runtime_tick now accumulates guest cycles and only materializes device state
// at the next scheduled-event horizon (see runtime_tick below). Any MMIO access
// must therefore first catch the devices up to 'now' so the access observes
// current state; a config-changing write additionally reschedules the horizon.
// Defined after tick_devices. The IO region is 0x04000000-0x040003FF (page 4).
extern "C" void runtime_mmio_catch_up(void);     // materialize lagged device state to now
extern "C" void runtime_resync_horizon(void);    // recompute next-event budget after a write
static inline bool is_io_addr(uint32_t addr) {
    return (addr & 0xFF000000u) == 0x04000000u;
}

// Stage 2: is a load from `addr` safe to treat as side-effect-free and stable
// inside an idle-candidate loop? Only writable RAM whose every mutation bumps
// g_idle_disturb_epoch (EWRAM/IWRAM) and immutable memory (ROM, BIOS) qualify.
// Everything else — MMIO (read side effects / FIFOs), the cartridge GPIO window
// (RTC etc. clock state out on read), save/flash (0x0E/0x0F, command state
// machine), palette/VRAM/OAM, open-bus/unmapped — is rejected: a load from such
// a region bumps the disturbance epoch so the loop can never reach a confirmed
// fixed point and is never elided. (ChatGPT-validated "probe memory-class
// check" — registers + epoch are sufficient ONLY with this guard. The bless of
// the omitted explicit load trace is conditional on it.)
static inline bool is_idle_safe_read(uint32_t addr) {
    const uint32_t region = addr >> 24;
    if (region == 0x02u || region == 0x03u) return true;   // EWRAM / IWRAM
    if (addr < 0x00004000u) return true;                   // BIOS (immutable)
    if (region >= 0x08u && region <= 0x0Du) {              // ROM (immutable) …
        const uint32_t off = addr & 0x01FFFFFFu;
        // … except the GPIO window 0x080000C4-0x080000C8 (RTC/sensor reads have
        // side effects). A small guard band covers wide accesses straddling it.
        if (off >= 0x000000C0u && off < 0x000000CCu) return false;
        return true;
    }
    return false;  // MMIO, palette, VRAM, OAM, save/flash, open-bus, unmapped
}

extern "C" uint32_t bus_read_u32_slow(uint32_t addr) {
    CostTimer _ct(&g_cost_bus_slow_ns,
                              &g_cost_bus_slow_calls);
    if (is_io_addr(addr)) runtime_mmio_catch_up();
    if (!is_idle_safe_read(addr)) ++g_idle_disturb_epoch;
    gbarecomp::sync_bios_access();
    uint32_t v = gbarecomp::g_active_bus
        ? gbarecomp::g_active_bus->read32(addr)
        : 0u;
    gbarecomp::trace_unmapped_read(addr, v, 4u);
    return v;
}

extern "C" uint16_t bus_read_u16_slow(uint32_t addr) {
    CostTimer _ct(&g_cost_bus_slow_ns,
                              &g_cost_bus_slow_calls);
    if (is_io_addr(addr)) runtime_mmio_catch_up();
    if (!is_idle_safe_read(addr)) ++g_idle_disturb_epoch;
    gbarecomp::sync_bios_access();
    uint16_t v = gbarecomp::g_active_bus
        ? gbarecomp::g_active_bus->read16(addr)
        : uint16_t{0};
    gbarecomp::trace_unmapped_read(addr, v, 2u);
    return v;
}

extern "C" uint8_t bus_read_u8_slow(uint32_t addr) {
    CostTimer _ct(&g_cost_bus_slow_ns,
                              &g_cost_bus_slow_calls);
    if (is_io_addr(addr)) runtime_mmio_catch_up();
    if (!is_idle_safe_read(addr)) ++g_idle_disturb_epoch;
    gbarecomp::sync_bios_access();
    uint8_t v = gbarecomp::g_active_bus
        ? gbarecomp::g_active_bus->read8(addr)
        : uint8_t{0};
    gbarecomp::trace_unmapped_read(addr, v, 1u);
    return v;
}

extern "C" void bus_write_u32_slow(uint32_t addr, uint32_t val) {
    // Direct slow-path callers are also a store boundary. Retire any pending
    // one-shot transform so it cannot survive past the generated store seam.
    g_runtime_mem_write_override_pending_valid = 0u;
    CostTimer _ct(&g_cost_bus_slow_ns,
                              &g_cost_bus_slow_calls);
    bool io = is_io_addr(addr);
    if (io) runtime_mmio_catch_up();
    const uint32_t before = gbarecomp::g_active_bus
        ? gbarecomp::g_active_bus->read32(addr) : 0u;
    if (g_runtime_mp2k_write_hook)
        gbarecomp::runtime_mp2k_write_observer(g_cpu.R[15], addr, val, 4,
                                    g_cpu.R[6], g_cpu.R[11], before,
                                    (g_cpu.cpsr & CPSR_T_BIT) != 0, 1);
    const bool ram_image_probe = g_runtime_ram_image_write_probe != nullptr;
    if (ram_image_probe) runtime_ram_image_bus_begin();
    if (gbarecomp::g_active_bus) gbarecomp::g_active_bus->write32(addr, val);
    if (ram_image_probe) runtime_ram_image_bus_end();
    if (g_runtime_mp2k_write_hook)
        gbarecomp::runtime_mp2k_write_observer(g_cpu.R[15], addr, val, 4,
                                    g_cpu.R[6], g_cpu.R[11], before,
                                    (g_cpu.cpsr & CPSR_T_BIT) != 0, 0);
    ++g_idle_disturb_epoch;   // any guest write may change a watched poll value
    if (io) runtime_resync_horizon();
}

extern "C" void bus_write_u16_slow(uint32_t addr, uint16_t val) {
    g_runtime_mem_write_override_pending_valid = 0u;
    CostTimer _ct(&g_cost_bus_slow_ns,
                              &g_cost_bus_slow_calls);
    bool io = is_io_addr(addr);
    if (io) runtime_mmio_catch_up();
    const uint32_t before = gbarecomp::g_active_bus
        ? gbarecomp::g_active_bus->read16(addr) : 0u;
    if (g_runtime_mp2k_write_hook)
        gbarecomp::runtime_mp2k_write_observer(g_cpu.R[15], addr, val, 2,
                                    g_cpu.R[6], g_cpu.R[11], before,
                                    (g_cpu.cpsr & CPSR_T_BIT) != 0, 1);
    const bool ram_image_probe = g_runtime_ram_image_write_probe != nullptr;
    if (ram_image_probe) runtime_ram_image_bus_begin();
    if (gbarecomp::g_active_bus) gbarecomp::g_active_bus->write16(addr, val);
    if (ram_image_probe) runtime_ram_image_bus_end();
    if (g_runtime_mp2k_write_hook)
        gbarecomp::runtime_mp2k_write_observer(g_cpu.R[15], addr, val, 2,
                                    g_cpu.R[6], g_cpu.R[11], before,
                                    (g_cpu.cpsr & CPSR_T_BIT) != 0, 0);
    ++g_idle_disturb_epoch;
    if (io) runtime_resync_horizon();
}

extern "C" void bus_write_u8_slow(uint32_t addr, uint8_t val) {
    g_runtime_mem_write_override_pending_valid = 0u;
    CostTimer _ct(&g_cost_bus_slow_ns,
                              &g_cost_bus_slow_calls);
    bool io = is_io_addr(addr);
    if (io) runtime_mmio_catch_up();
    const uint32_t before = gbarecomp::g_active_bus
        ? gbarecomp::g_active_bus->read8(addr) : 0u;
    if (g_runtime_mp2k_write_hook)
        gbarecomp::runtime_mp2k_write_observer(g_cpu.R[15], addr, val, 1,
                                    g_cpu.R[6], g_cpu.R[11], before,
                                    (g_cpu.cpsr & CPSR_T_BIT) != 0, 1);
    const bool ram_image_probe = g_runtime_ram_image_write_probe != nullptr;
    if (ram_image_probe) runtime_ram_image_bus_begin();
    if (gbarecomp::g_active_bus) gbarecomp::g_active_bus->write8(addr, val);
    if (ram_image_probe) runtime_ram_image_bus_end();
    if (g_runtime_mp2k_write_hook)
        gbarecomp::runtime_mp2k_write_observer(g_cpu.R[15], addr, val, 1,
                                    g_cpu.R[6], g_cpu.R[11], before,
                                    (g_cpu.cpsr & CPSR_T_BIT) != 0, 0);
    ++g_idle_disturb_epoch;
    if (io) runtime_resync_horizon();
}

// Block timing (runtime_arm.h). Always 0 in the normal game code.
extern "C" uint32_t g_runtime_deferred_cycles = 0;
// Set while a deferred debt is paid in the middle of an instruction (before
// an I/O, palette, VRAM or OAM access): the clock and devices advance, but
// an interrupt waits for the next instruction boundary, as it would today.
static bool g_tick_irq_held = false;

extern "C" void runtime_flush_deferred(void) {
    if (g_runtime_deferred_cycles) runtime_tick(0u);
}

static inline void flush_deferred_for_access(uint32_t addr) {
    if (g_runtime_deferred_cycles && addr - 0x04000000u < 0x04000000u) {
        g_tick_irq_held = true;
        runtime_tick(0u);
        g_tick_irq_held = false;
    }
}

extern "C" uint32_t runtime_insn_fetch(uint32_t pc, uint32_t width) {
    g_cpu.R[15] = pc;
    return runtime_mem_cycles(pc, width, 1u);
}

// The shared copies generated game code calls (runtime_arm.h,
// GBARECOMP_OUTLINE_BUS). noinline keeps LTO from copying them back into
// every access site, which is the size this exists to remove.
extern "C" __attribute__((noinline)) uint32_t runtime_bus_read_u32(uint32_t a) {
    cost_bk(kBkBusRead);
    flush_deferred_for_access(a);
    return bus_read_u32_inline(a);
}
extern "C" __attribute__((noinline)) uint16_t runtime_bus_read_u16(uint32_t a) {
    cost_bk(kBkBusRead);
    flush_deferred_for_access(a);
    return bus_read_u16_inline(a);
}
extern "C" __attribute__((noinline)) uint8_t runtime_bus_read_u8(uint32_t a) {
    cost_bk(kBkBusRead);
    flush_deferred_for_access(a);
    return bus_read_u8_inline(a);
}
extern "C" __attribute__((noinline)) void runtime_bus_write_u32(uint32_t a, uint32_t v) {
    cost_bk(kBkBusWrite);
    flush_deferred_for_access(a);
    bus_write_u32_inline(a, v);
}
extern "C" __attribute__((noinline)) void runtime_bus_write_u16(uint32_t a, uint16_t v) {
    cost_bk(kBkBusWrite);
    flush_deferred_for_access(a);
    bus_write_u16_inline(a, v);
}
extern "C" __attribute__((noinline)) void runtime_bus_write_u8(uint32_t a, uint8_t v) {
    cost_bk(kBkBusWrite);
    flush_deferred_for_access(a);
    bus_write_u8_inline(a, v);
}

// Combined per-instruction helpers: see runtime_arm.h. Each body is the
// exact sequence it replaces in generated code.
extern "C" uint32_t g_runtime_data_cost = 0;

extern "C" bool runtime_insn_boundary(void) {
    cost_bk(kBkInsnBoundary);
    runtime_flush_deferred();
    if (runtime_should_yield()) return true;
    if (g_runtime_insn_trace) runtime_insn_fp();
    return false;
}

extern "C" uint32_t runtime_insn_begin(uint32_t pc, uint32_t width) {
    cost_bk(kBkInsnBegin);
    g_cpu.R[15] = pc;
    if (runtime_insn_boundary()) return 0u;
    return runtime_mem_cycles(pc, width, 1u);
}

extern "C" uint32_t runtime_fetch_ns_delta(uint32_t pc, uint32_t width) {
    cost_bk(kBkFetchNsDelta);
    return runtime_mem_cycles(pc, width, 0u) - runtime_mem_cycles(pc, width, 1u);
}

extern "C" uint32_t runtime_refill_cycles(uint32_t target, uint32_t width) {
    cost_bk(kBkRefillCycles);
    const uint32_t n = runtime_mem_cycles(target, width, 0u) - 1u;
    return n + (runtime_mem_cycles(target + width, width, 1u) - 1u);
}

extern "C" uint32_t runtime_ld_u32(uint32_t addr, uint32_t ea) {
    cost_bk(kBkLd);
    flush_deferred_for_access(addr);
    const uint32_t v = bus_read_u32_inline(addr);
    g_runtime_data_cost = runtime_mem_cycles(ea, 4u, 2u);
    return v;
}
extern "C" uint32_t runtime_ld_u16(uint32_t addr, uint32_t ea) {
    cost_bk(kBkLd);
    flush_deferred_for_access(addr);
    const uint32_t v = bus_read_u16_inline(addr);
    g_runtime_data_cost = runtime_mem_cycles(ea, 2u, 2u);
    return v;
}
extern "C" uint32_t runtime_ld_u8(uint32_t addr, uint32_t ea) {
    cost_bk(kBkLd);
    flush_deferred_for_access(addr);
    const uint32_t v = bus_read_u8_inline(addr);
    g_runtime_data_cost = runtime_mem_cycles(ea, 1u, 2u);
    return v;
}

extern "C" uint32_t runtime_st_u32(uint32_t pc, uint32_t addr, uint32_t ea,
                                   uint32_t value) {
    cost_bk(kBkSt);
    flush_deferred_for_access(addr);
    runtime_trace_event(RUNTIME_TRACE_MEM_WRITE, pc, addr, value, 4u);
    bus_write_u32_inline(addr, value);
    return runtime_mem_cycles(ea, 4u, 2u);
}
extern "C" uint32_t runtime_st_u16(uint32_t pc, uint32_t addr, uint32_t ea,
                                   uint32_t value) {
    cost_bk(kBkSt);
    flush_deferred_for_access(addr);
    runtime_trace_event(RUNTIME_TRACE_MEM_WRITE, pc, addr, value, 2u);
    bus_write_u16_inline(addr, static_cast<uint16_t>(value));
    return runtime_mem_cycles(ea, 2u, 2u);
}
extern "C" uint32_t runtime_st_u8(uint32_t pc, uint32_t addr, uint32_t ea,
                                  uint32_t value) {
    cost_bk(kBkSt);
    flush_deferred_for_access(addr);
    runtime_trace_event(RUNTIME_TRACE_MEM_WRITE, pc, addr, value, 1u);
    bus_write_u8_inline(addr, static_cast<uint8_t>(value));
    return runtime_mem_cycles(ea, 1u, 2u);
}

extern "C" uint32_t runtime_ldm_u32(uint32_t addr, uint32_t seq,
                                    uint32_t* dst) {
    cost_bk(kBkLdm);
    flush_deferred_for_access(addr);
    const uint32_t c = runtime_mem_cycles(addr, 4u, seq);
    *dst = bus_read_u32_inline(addr);
    return c;
}
extern "C" uint32_t runtime_stm_u32(uint32_t pc, uint32_t addr,
                                    uint32_t value, uint32_t seq) {
    cost_bk(kBkStm);
    flush_deferred_for_access(addr);
    const uint32_t c = runtime_mem_cycles(addr, 4u, seq);
    runtime_trace_event(RUNTIME_TRACE_MEM_WRITE, pc, addr, value, 4u);
    bus_write_u32_inline(addr, value);
    return c;
}

// Memory timing table (ROADMAP.md "block bookkeeping", step 2). The heaviest
// Ragnarok frames make ~2 M runtime_mem_cycles calls (FACTS.md, "What the
// block build still spends Ragnarok on"). GbaBus::access_cycles depends only
// on the address's region, the width (8 and 16 cost the same), S/N and
// WAITCNT, so its answers are kept per region x width x S/N and rebuilt when
// WAITCNT changes, instead of a virtual call and a switch per access. The
// table and its key are defined at the top of this file.

// Kept out of line (and the prefetch call below): inlined, the loop made
// runtime_mem_cycles save and restore eight registers on every call, which
// is where most of its 22% of the heaviest Ragnarok frames went
// (logs/session_20260930_182758.hostprof.txt).
__attribute__((noinline, cold))
static void rebuild_mem_cost(const gba::GbaBus& bus, uint16_t waitcnt) {
    for (uint32_t region = 0; region < 16u; ++region) {
        for (uint32_t w = 0; w < 2u; ++w) {
            for (uint32_t s = 0; s < 2u; ++s) {
                g_runtime_mem_cost[region][w][s] = static_cast<uint8_t>(
                    bus.access_cycles(region << 24, w ? 4u : 2u, s != 0u));
            }
        }
    }
    g_runtime_mem_cost_key = waitcnt;
}

__attribute__((noinline))
static uint32_t prefetch_data_cycles(gba::GbaBus& bus, uint32_t addr,
                                     uint32_t width) {
    return static_cast<uint32_t>(bus.data_access_cycles(
        addr, static_cast<uint8_t>(width), false, g_cpu.R[15]));
}

extern "C" uint32_t runtime_mem_cycles(uint32_t addr, uint32_t width,
                                       uint32_t sequential) {
    cost_bk(kBkMemCycles);
    auto* bus = gbarecomp::g_active_bus;
    if (!bus) return 1u;
    const uint16_t waitcnt = bus->io().waitcnt();
    if (waitcnt != g_runtime_mem_cost_key) rebuild_mem_cost(*bus, waitcnt);
    const uint32_t region = (addr >> 24) & 0xFu;
    const bool seq = sequential != 0u && sequential != 2u;
    const uint32_t cost = g_runtime_mem_cost[region][width == 4u][seq];
    if (sequential == 2u) {
        // GbaBus::data_access_cycles returns the plain N cost unless the
        // access is below the cart while cart code runs with prefetch on;
        // only that case needs the prefetch model.
        if (addr >= 0x08000000u || ((g_cpu.R[15] >> 24) & 0xFu) < 0x8u ||
            (waitcnt & 0x4000u) == 0u)
            return cost;
        return prefetch_data_cycles(*bus, addr, width);
    }
    return cost;
}

extern "C" uint32_t runtime_mul_cycles(uint32_t rs_value,
                                       uint32_t signed_variant,
                                       uint32_t extra) {
    cost_bk(kBkMulCycles);
    return armv4t::mul_wait_cycles(rs_value, signed_variant != 0u, extra);
}

// Advance the devices (audio, timers, PPU) by `cycles`, rendering scanlines
// and raising IRQ-pending bits (IF) as events occur — but WITHOUT taking the
// IRQ. Shared by runtime_tick's normal path and the wake-from-HALT latency
// pump, so the delay window ticks devices identically without re-vectoring.
// True while inside tick_devices. Timed/FIFO DMA fires from here and accumulates
// its stolen cycles in GbaIo; the drain (which itself ticks devices) must run
// OUTSIDE this window to avoid re-entrancy — this guard makes that explicit.
static bool g_in_device_tick = false;
struct DeviceTickGuard {
    bool prev;
    DeviceTickGuard() : prev(g_in_device_tick) { g_in_device_tick = true; }
    ~DeviceTickGuard() { g_in_device_tick = prev; }
};

// Turbo render-skip: latched once per guest frame, at the moment the
// scanline counter wraps into that frame (ev.frame_completed, BEFORE its own
// scanline 0 HBlank), via g_frame_render_gate_hook. Persists across
// tick_devices calls for the rest of that frame. Only gates PPU pixel
// production (render_scanline / mark_framebuffer_latched below); every other
// per-scanline/per-frame effect (DMA, IRQ, timing, state latches) is
// unaffected. Defaults to false (always render) when no hook is registered.
static bool g_skip_frame_render = false;

static void tick_devices(gba::GbaBus* bus, gba::GbaPpu* ppu, uint32_t cycles) {
    CostTimer _ct_total(&g_cost_tick_devices_ns, &g_cost_tick_devices_calls);
    gbarecomp::HostProfPhaseScope _hp_tick(gbarecomp::kHpTickDevices);
    DeviceTickGuard _dtg;
    // Stage 2: materializing device state can raise IF, advance the PPU phase,
    // run timed DMA into watched RAM, etc. Any of these can change a polled
    // value or the timing rules, so it disturbs an in-flight idle proof.
    ++g_idle_disturb_epoch;
    uint32_t remaining = cycles;
    while (remaining != 0) {
        uint32_t chunk = remaining;
        uint32_t until_sample = bus->audio().cycles_until_next_sample();
        uint32_t until_timer = bus->io().cycles_until_next_timer_event();
        uint32_t until_sio = bus->io().cycles_until_next_sio_event();
        uint32_t until_ppu = ppu->cycles_until_next_event();
        if (until_sample < chunk) chunk = until_sample;
        if (until_timer < chunk) chunk = until_timer;
        if (until_sio < chunk) chunk = until_sio;
        if (until_ppu < chunk) chunk = until_ppu;
        if (chunk == 0) chunk = 1;

        {
            CostTimer _ct(&g_cost_audio_tick_ns, &g_cost_audio_tick_calls);
            bus->audio().tick(chunk);
        }
        {
            CostTimer _ct(&g_cost_timers_ns, &g_cost_timers_calls);
            bus->io().tick_timers(chunk);
        }
        bus->io().tick_sio(chunk);

        uint16_t vc_compare = static_cast<uint16_t>(
            (bus->io().dispstat() >> 8) & 0xFFu);
        auto events = ppu->tick(chunk, vc_compare);
        if (events.frame_completed) {
            // Host profiler: the sampler thread cannot read the PPU.
            if (gbarecomp::g_host_prof_enabled)
                gbarecomp::g_host_prof_frame.store(
                    ppu->frame_count(), std::memory_order_relaxed);
            // Earliest point the newly-started frame's index is known, and
            // strictly before that frame's own scanline 0 HBlank render
            // below — the decision is committed before any of its pixel
            // work runs, never retrofitted after the fact.
            g_skip_frame_render = gbarecomp::g_frame_render_gate_hook &&
                !gbarecomp::g_frame_render_gate_hook(ppu->frame_count());
        }
        uint16_t ds = bus->io().dispstat();
        if (events.hblank_started &&
            ppu->vcount() < gba::GbaPpu::kLinesVisible) {
            if (!g_skip_frame_render) {
                std::chrono::steady_clock::time_point _prof_t0;
                if (phase_prof_enabled()) _prof_t0 = std::chrono::steady_clock::now();
                {
                    CostTimer _ct_render(&g_cost_ppu_render_ns, &g_cost_ppu_render_calls);
                    gbarecomp::HostProfPhaseScope _hp_ppu(gbarecomp::kHpPpuRender);
                    ppu->render_scanline(ppu->vcount(),
                                         bus->io().read16(0x000),
                                         bus->io().raw(),
                                         bus->vram_ptr(),
                                         bus->oam_ptr(),
                                         bus->pal_ptr());
                }
                if (phase_prof_enabled()) {
                    g_prof_scanline_ns += static_cast<unsigned long long>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - _prof_t0).count());
                    ++g_prof_scanline_count;
                }
            }
            // HBlank-timed DMA fires on each visible-line HBlank — AFTER the
            // line is rendered, so line N used the value the previous HBlank's
            // DMA loaded (matching the HBlank-IRQ ordering below and hardware:
            // the DMA at HBlank N loads the register used by line N+1). This is
            // what walks the per-scanline WIN0H circle table for the transition
            // iris (MC-HP-003).
            {
                CostTimer _ct(&g_cost_dma_ns, &g_cost_dma_calls);
                bus->io().run_timed_dma(2);
            }
        }
        if (events.vblank_started) {
            // Hardware reloads the hidden affine reference points from BGxX/Y
            // at VBlank. HBlank writes may reload them again before the next
            // visible line.
            ppu->reload_affine_references(bus->io().raw());
            // Capture the frame-level render inputs before VBlank DMA mutates
            // OAM/palette/VRAM. Native presentation then follows the same
            // boundary as the canonical latched framebuffer, so scrolling
            // registers cannot appear one frame late/early.
            ppu->latch_native_scene_state(
                bus->io().read16(0x000), bus->io().raw(),
                bus->vram_ptr(), bus->oam_ptr(), bus->pal_ptr());
            // mark_framebuffer_latched() normally only does host-side
            // readiness bookkeeping. Expanded vertical margins and deferred
            // rows are rendered there at VBlank, though, so include that work
            // in the same PPU cost bucket. The visible scanlines are skipped
            // while deferred rows are enabled, so this cannot double-count
            // native rows.
            if (!g_skip_frame_render) {
                const bool mark_renders = ppu->view_expanded() &&
                    ppu->has_latched_native_scene_state() &&
                    (gba::g_ws_defer_native_rows != 0 ||
                     ppu->view_extra_top() != 0 ||
                     ppu->view_extra_bottom() != 0);
                if (mark_renders) {
                    CostTimer _ct_render(&g_cost_ppu_render_ns,
                                         &g_cost_ppu_render_calls);
                    gbarecomp::HostProfPhaseScope _hp_ppu(gbarecomp::kHpPpuRender);
                    ppu->mark_framebuffer_latched();
                } else {
                    gbarecomp::HostProfPhaseScope _hp_ppu(gbarecomp::kHpPpuRender);
                    ppu->mark_framebuffer_latched();
                }
            }
            if (gbarecomp::g_frame_snapshot_hook)
                gbarecomp::g_frame_snapshot_hook();
            // Second, independent observer of the same boundary -- see
            // set_scene_capture_hook's header comment.
            if (gbarecomp::g_scene_capture_hook)
                gbarecomp::g_scene_capture_hook();
            ++g_runtime_vblank_starts;
            // Host-controlled cheats run at the same stable boundary as the
            // frame counter. They touch only the confirmed party RAM fields
            // and are a no-op while both options are off.
            runtime_apply_infinite_hp_pp();
            {
                CostTimer _ct(&g_cost_dma_ns, &g_cost_dma_calls);
                bus->io().run_timed_dma(1);   // VBlank-timed DMA
            }
            bus->audio().mp2k_vblank_hook(g_runtime_cycles);
        }
        if (events.vblank_started && (ds & 0x0008u)) {
            bus->io().request_irq(gba::GbaIo::IrqVBlank);
        }
        if (events.hblank_started && (ds & 0x0010u)) {
            bus->io().request_irq(gba::GbaIo::IrqHBlank);
        }
        if (events.vcount_matched && (ds & 0x0020u)) {
            bus->io().request_irq(gba::GbaIo::IrqVCount);
        }

        remaining -= chunk;
    }
}

// ── LP-005 clock-event probe (env GBARECOMP_CYC_LO / GBARECOMP_CYC_HI) ───────
// One-shot diagnostic: when g_runtime_cycles is within [LO,HI], log every event
// that advances the master clock (runtime_tick, drain_dma_steal, resync) with
// the current guest PC. Off by default (both env unset). Harness-only; compiled
// out cost is a single env read cached in statics.
extern "C" const char* g_tick_ctx = "gen";  // set by interp/bridge paths around runtime_tick
static inline void cyc_probe(const char* what, uint32_t amt) {
    static long long lo = -1, hi = -1;
    if (lo < 0) {
        const char* l = std::getenv("GBARECOMP_CYC_LO");
        const char* h = std::getenv("GBARECOMP_CYC_HI");
        lo = l ? std::atoll(l) : 0;
        hi = h ? std::atoll(h) : 0;
    }
    if (hi == 0) return;  // probe disabled
    unsigned long long now = g_runtime_cycles;
    if (static_cast<long long>(now) < lo ||
        static_cast<long long>(now) > hi) return;
    std::fprintf(stderr, "[cycprobe] %-12s [%s] +%u  clk=%llu->%llu  pc=%08x cpsr=%08x\n",
                 what, g_tick_ctx, amt, now, now + amt, g_cpu.R[15], g_cpu.cpsr);
}

// ── Stage 1: lazy device catch-up state ─────────────────────────────────────
// g_pending_cycles: guest cycles advanced on the master clock but not yet
// materialized into device state. g_event_budget: cycles remaining until the
// next scheduled device event; when it reaches 0, runtime_tick materializes
// (tick_devices) and reschedules. tick_devices chunks internally to exact
// sub-event boundaries, so materializing a batched delta is bit-identical to
// per-instruction ticking — at a fraction of the call overhead.
static unsigned long long g_pending_cycles = 0;
static long long          g_event_budget   = 0;

static inline void recompute_event_budget(gba::GbaBus* bus, gba::GbaPpu* ppu) {
    CostTimer _ct(&g_cost_recompute_budget_ns, &g_cost_recompute_budget_calls);
    uint32_t h  = ppu->cycles_until_next_event();
    uint32_t ut = bus->io().cycles_until_next_timer_event();
    uint32_t us = bus->audio().cycles_until_next_sample();
    uint32_t usio = bus->io().cycles_until_next_sio_event();
    if (ut < h) h = ut;
    if (us < h) h = us;
    if (usio < h) h = usio;
    if (h == 0u) h = 1u;
    g_event_budget = static_cast<long long>(h);
}

// Charge DMA-stolen bus cycles (accumulated by the GbaIo DMA loops) to the
// master clock and advance devices for that window — i.e. model cycle-stealing.
// Called OUTSIDE tick_devices (end of runtime_tick for timed/FIFO DMA; after a
// guest write for immediate DMA), so its own tick_devices is not re-entrant.
// Guarded against the (currently unreachable) in-tick case for safety.
static inline void drain_dma_steal(gba::GbaBus* bus, gba::GbaPpu* ppu) {
    // NOTE: deliberately NOT wrapped in a CostTimer. This function is called
    // on EVERY runtime_tick (~65M times per 1800 frames) — a per-call
    // std::chrono::steady_clock::now() pair at that frequency measurably
    // distorts wall time (see cost-probe distortion note in runtime.cpp /
    // the GS-011-perf halt-attribution report). Its cost is captured
    // indirectly: when cyc!=0 it calls tick_devices (timed) and
    // recompute_event_budget (timed); its own branch/take_dma_steal_cycles
    // overhead is negligible and folds into the pump_idle/dispatch container.
    if (g_cost_probe) ++g_cost_drain_dma_call_calls;
    if (!bus || !ppu) return;
    uint32_t cyc = bus->io().take_dma_steal_cycles();
    if (cyc == 0) return;
    cyc_probe("dma_steal", cyc);
    g_runtime_cycles += cyc;
    if (g_in_device_tick) {
        // Defensive: defer the device-advance into the pending window.
        g_pending_cycles += cyc;
        g_event_budget   -= static_cast<long long>(cyc);
        return;
    }
    if (g_pending_cycles) {
        tick_devices(bus, ppu, static_cast<uint32_t>(g_pending_cycles));
        g_pending_cycles = 0;
    }
    tick_devices(bus, ppu, cyc);
    recompute_event_budget(bus, ppu);
}

// Materialize lagged device state up to 'now'. Called before any MMIO access so
// the access observes current device state. Fires NO events: between flushes
// g_pending_cycles < g_event_budget (we flush the instant the budget hits 0),
// so the accumulated delta never reaches the next event boundary. The budget
// already accounts for these cycles (runtime_tick debited it as they accrued),
// so it is left unchanged here.
extern "C" void runtime_mmio_catch_up(void) {
    // A shadow/transactional re-run must not materialize the real machine's
    // pending time. The bus observer will reject or diagnose the MMIO access;
    // flushing here first would irreversibly advance PPU/audio/timers despite
    // the guest transaction subsequently being rolled back.
    if (g_runtime_shadow_tick) return;
    auto* bus = gbarecomp::g_active_bus;
    auto* ppu = gbarecomp::g_active_ppu;
    if (!bus || !ppu || g_pending_cycles == 0) return;
    tick_devices(bus, ppu, static_cast<uint32_t>(g_pending_cycles));
    g_pending_cycles = 0;
}

// Recompute the next-event horizon after a config-changing MMIO write (timer
// reload/control, DISPSTAT, DMA registers, etc. move the next event).
extern "C" void runtime_resync_horizon(void) {
    // Device stores are suppressed by the shadow transaction's bus observer.
    // Do not drain DMA or rewrite the real scheduler horizon for that rejected
    // store. This also keeps the established shadow-tick promise symmetric with
    // runtime_mmio_catch_up() and runtime_tick().
    if (g_runtime_shadow_tick) return;
    auto* bus = gbarecomp::g_active_bus;
    auto* ppu = gbarecomp::g_active_ppu;
    if (!bus || !ppu) return;
    // An immediate-mode DMA may have been triggered by the just-completed guest
    // write; charge its stolen cycles before re-arming the horizon.
    cyc_probe("resync", 0);
    drain_dma_steal(bus, ppu);
    recompute_event_budget(bus, ppu);
}

// runtime_tick_common — the shared body, moved verbatim out of what used to
// be runtime_tick itself. Two doors call in (see below): runtime_tick (the
// scaled, CPU-work door every generated-code / bridge / finterp call site
// uses) and runtime_tick_realtime (the unscaled, real-hardware-time door the
// halt/idle pump uses). Keeping this as a single static function with two
// thin callers — rather than a shared mutable flag toggled around call
// sites — is deliberate: runtime_tick is genuinely re-entrant (a
// wake-from-HALT IRQ drives a nested runtime_dispatch, which issues its own
// nested runtime_tick calls while the outer call's frame is still live), so
// any flag readable by a nested call has a real chance of being observed in
// the wrong state. Static call-site separation has no shared mutable state
// for re-entrancy to corrupt.
static void runtime_tick_common(uint32_t cycles) {
    // P6 shadow-tick: a healed shard's validation re-run only accumulates its
    // cycle cost (for the cycle diff); the interpreter pass already pumped the
    // devices / delivered IRQs for this window, so do nothing else here.
    if (g_runtime_shadow_tick) { g_runtime_shadow_cycles += cycles; return; }

    auto* bus = gbarecomp::g_active_bus;
    auto* ppu = gbarecomp::g_active_ppu;
    if (!bus || !ppu || cycles == 0) return;

    // NOTE: deliberately NOT wrapped in a CostTimer. runtime_tick is called
    // per guest instruction (~65M times per 1800 frames) AND is genuinely
    // re-entrant (a wake-from-HALT IRQ drives the ISR to completion via
    // nested runtime_dispatch, which issues its own nested runtime_tick
    // calls while THIS call's frame is still on the stack) — an early
    // version of this probe timed the whole function body here and got
    // both effects at once: ~2.2x wall-time inflation from the per-call
    // clock reads, AND self-overlapping (double-counted) totals from the
    // recursion, since the outer call's measured span already contains
    // every nested call's span. See the GS-011-perf halt-attribution
    // report. Use g_cost_pump_idle_ns (single-level, non-recursive — pump_idle
    // itself is never re-entered) for the trustworthy "everything reachable
    // from one halt-loop iteration" total instead.
    if (g_cost_probe) ++g_cost_tick_total_calls;
    cyc_probe("tick", cycles);
    g_runtime_cycles += cycles;
    // Lazy device catch-up: advance the master clock every instruction (cheap),
    // but materialize device state only when the next-event horizon is reached.
    // (IRQ-eligibility is still checked every instruction below — that is cheap,
    // and IF can only change at a flush, so checking between flushes is exact.)
    g_pending_cycles += cycles;
    g_event_budget   -= static_cast<long long>(cycles);
    if (g_event_budget <= 0) {
        tick_devices(bus, ppu, static_cast<uint32_t>(g_pending_cycles));
        g_pending_cycles = 0;
        recompute_event_budget(bus, ppu);
    }

    // A timed/FIFO DMA may have fired inside that flush; charge its stolen bus
    // cycles (advancing the clock + devices) before the IRQ-eligibility check,
    // so a DMA-end IRQ or a timer that overflowed during the steal is seen now.
    drain_dma_steal(bus, ppu);

    if (!g_tick_irq_held && bus->io().irq_pending() &&
        (g_cpu.cpsr & CPSR_I_BIT) == 0) {
        CostTimer _ct_irq(&g_cost_irq_check_ns, &g_cost_irq_check_calls);
        g_runtime_irq_from_halt = bus->io().halted() ? 1u : 0u;
        if (bus->io().halted()) {
            // Wake-from-HALT IRQ latency. The ARM7TDMI does not vector the
            // instant the IRQ pends out of HALT, and the interpreter oracle
            // (bios_smoke) models this delay. Taking it immediately here
            // vectored ~kIrqWakeDelayCycles early every VBlank (the game
            // VBlankIntrWaits each frame), shifting m4a sequencer phase enough
            // to double-tick a channel once a fade started → MC-HP-002 hang.
            // Pump the devices for the latency window WITHOUT re-taking the
            // IRQ (tick_devices, not runtime_tick), then vector — matching the
            // oracle exactly. Newly-pended bits stay in IF for the next check.
            // These wake-delay cycles really elapse on hardware, so they must
            // also advance the cumulative cycle clock (the oracle's pump_step
            // counts them in cycles_elapsed). Omitting this left g_runtime_cycles
            // 7 cyc/IRQ short of the oracle, growing the fingerprint cycle skew
            // by -7/IRQ (the MC-HP-002 cycle drift) — cosmetic for execution
            // (the PPU/audio above are ticked regardless) but it broke cycle-
            // aligned diffing.
            bus->io().clear_halt();
            // Materialize any cycles accumulated before the wake first (preserve
            // device ordering), then the wake-latency window, then re-arm the
            // horizon — all devices are now current as of this cycle.
            if (g_pending_cycles) {
                tick_devices(bus, ppu, static_cast<uint32_t>(g_pending_cycles));
                g_pending_cycles = 0;
            }
            cyc_probe("irq_wake", gba::kIrqWakeDelayCycles);
            g_runtime_cycles += gba::kIrqWakeDelayCycles;
            tick_devices(bus, ppu, gba::kIrqWakeDelayCycles);
            recompute_event_budget(bus, ppu);
        }
        {
            gbarecomp::HostProfPhaseScope _hp_irq(gbarecomp::kHpIrqHandler);
            runtime_irq(g_cpu.R[15]);
        }
    }

#ifdef GBA_COSIM
    // First-divergence co-simulation checkpoint. runtime_tick is the shared
    // per-instruction master-clock advance for BOTH backends (generated code emits
    // it per instruction; the force-interp driver calls it per instruction), so
    // this is the alignment clock: it records/parks at cycle-stride boundaries so
    // the recomp and interp instances stop at identical guest cycles. Placed at the
    // very end so g_runtime_cycles reflects this instruction's cost + any DMA-steal
    // / HALT-wake cycles added above. See COSIM_ORACLE.md §3.
    cosim_on_tick();
#endif
}

// runtime_tick_realtime — the real-elapsed-hardware-time door. Used ONLY by
// the halt/idle pump (runtime.cpp pump_idle -> here), which advances the
// master clock while the CPU is halted and no instruction is executing.
// That time is real hardware time, not CPU work, so it is NEVER scaled by
// GBARECOMP_CPU_OVERCLOCK regardless of the setting — calls straight through
// to the shared body with no scaling logic at all.
extern "C" void runtime_tick_realtime(uint32_t cycles) {
    cost_bk(kBkTickRealtime);
    runtime_tick_common(cycles);
}

// ── GBARECOMP_CPU_OVERCLOCK ─────────────────────────────────────────────────
// Opt-in guest CPU overclock (TURBO-B2). Scales ONLY the per-instruction
// cost arriving here (the runtime_tick door every generated-code, bridge,
// and finterp call site uses — real CPU work). Never scales: the halt/idle
// pump (routes through runtime_tick_realtime above, a separate door),
// drain_dma_steal's DMA-stolen cycles, kIrqWakeDelayCycles, or the
// shadow-tick path — none of those reach the scaling logic below, since
// they either bypass runtime_tick entirely or add to g_runtime_cycles
// directly inside runtime_tick_common rather than through the `cycles`
// parameter.
//
// PPU/audio/timer/IRQ cadence is driven by master cycles, which are totally
// unaffected: scaling down the cost charged per instruction just lets more
// instructions execute before the same number of master cycles elapse, i.e.
// more guest CPU work per true hardware frame. Self-limiting — a guest that
// isn't CPU-bound simply reaches the next halt/event sooner in wall time.
//
// Env var supplies the INITIAL value only, read once at process start into
// the atomic below (existing GBARECOMP_* convention: unset, non-numeric, or
// < 1 is treated as 1 / off). The host Enhancements-tab "CPU Overclock"
// control (TURBO-B2-UI) then overrides it live via runtime_set_overclock_factor,
// which the menu can call at any point while the game is running.
//
// Live-changeable design:
//  - g_overclock_factor is a std::atomic<unsigned> read with memory_order_relaxed
//    from runtime_tick — the hottest function in the runtime (~65M calls per
//    1800 frames). A relaxed atomic load compiles to the same plain load as a
//    non-atomic read on x86-64; there is no lock, no getenv, no string compare
//    on this path, satisfying the "must stay cheap" requirement. At factor <= 1
//    the function returns immediately after that one load+compare, exactly as
//    cheap as the prior function-local-static version — no scaling math, no
//    extra branch.
//  - g_overclock_carry is a std::atomic<unsigned long long> for the same
//    reason: runtime_tick (single logical writer — the guest execution
//    context, including its own reentrant nested dispatch) updates it with
//    plain relaxed load/store, while runtime_set_overclock_factor (called
//    from the host UI) may reset it from a different point in the call
//    graph. Using an atomic avoids a data race being undefined behavior;
//    relaxed ops cost the same as non-atomic ones on x86-64.
//  - Changing the factor resets the carry to 0. Accepted tradeoff: at most
//    (old_factor - 1) unscaled guest cycles of already-buffered fractional
//    charge are dropped — a few cycles, once, only on the frame the menu
//    selection changes. The alternative (rescaling the outstanding carry
//    into the new factor's units) buys sub-cycle precision across a control
//    change nobody times to the cycle, at the cost of extra logic in the hot
//    path; not worth it here.
// The factor is held as 8.8 fixed point: kOverclockOne (256) is 1.0x, 512 is
// 2.0x. Whole multiples are exactly representable, so the shipped Off/On
// setting (1x and 10x) is exact. The fixed point is a leftover from the
// removed automatic controller, which needed to move in fractions of a step;
// changing the factor at all was measured to glitch moving sprites
// (user-confirmed 2026-09-11: clean at a pinned 1x and a pinned 8x,
// flickering only while the factor was being switched), which is why the
// setting is now pinned and nothing moves it during play.
// runtime_arm.h's kOverclockOne, as an unsigned so the arithmetic below has
// one type throughout.
static constexpr unsigned kQ8One = static_cast<unsigned>(kOverclockOne);

static std::atomic<unsigned> g_overclock_factor_q8{[] {
    const char* e = std::getenv("GBARECOMP_CPU_OVERCLOCK");
    if (!e || !*e) return kQ8One;
    long v = std::strtol(e, nullptr, 10);
    return (v >= 1) ? static_cast<unsigned>(v) * kQ8One : kQ8One;
}()};
static std::atomic<unsigned long long> g_overclock_carry{0};

extern "C" void runtime_set_overclock_factor_q8(unsigned q8) {
    if (q8 < kQ8One) q8 = kQ8One;
    g_overclock_factor_q8.store(q8, std::memory_order_relaxed);
    // The carry is deliberately NOT cleared. It holds guest cycles already run
    // and not yet charged, in units of 1/256 of an unscaled guest cycle -- a
    // quantity that does not depend on the factor -- so the next call divides
    // the same remainder by the new factor and the master clock stays
    // continuous across the change. Clearing it was a bounded, documented
    // tradeoff back when the factor moved once or twice a session; under the
    // per-frame ramp below it would be a discontinuity on every change.
}

extern "C" unsigned runtime_get_overclock_factor_q8() {
    return g_overclock_factor_q8.load(std::memory_order_relaxed);
}

extern "C" void runtime_set_overclock_factor(unsigned factor) {
    if (factor < 1u) factor = 1u;
    runtime_set_overclock_factor_q8(factor * kQ8One);
}

// Whole multiples only: this is what the pinned-factor menu item and the
// headroom CSV's original column report. Automatic mode's fractional value is
// read through the _q8 door.
extern "C" unsigned runtime_get_overclock_factor() {
    return g_overclock_factor_q8.load(std::memory_order_relaxed) /
           kQ8One;
}

extern "C" void runtime_tick(uint32_t cycles) {
    cost_bk(kBkTick);
    if (g_runtime_deferred_cycles) {
        cycles += g_runtime_deferred_cycles;
        g_runtime_deferred_cycles = 0;
    }
    const unsigned factor = g_overclock_factor_q8.load(std::memory_order_relaxed);
    if (factor <= kQ8One) {
        // Default: exact current behavior, no scaling branch entered.
        runtime_tick_common(cycles);
        return;
    }
    // Integer carry accumulator (numerator/denominator: units of unscaled
    // guest cycles, denominator == factor) so scaled cost accumulates
    // exactly with no drift and no lost cycles: over any run, the sum of
    // charged (scaled) cycles equals floor(total_unscaled_cycles / factor),
    // with at most (factor - 1) unscaled cycles' worth ever outstanding in
    // the carry. An instruction may charge 0 scaled cycles this call (its
    // cost fully absorbed into the carry) — that never stalls
    // g_event_budget or starves a device flush, because it means the master
    // clock genuinely has not advanced yet; the very next call whose sum
    // reaches `factor` pays out the accumulated charge and runtime_tick_common
    // runs its normal budget/flush/IRQ bookkeeping on it.
    //
    // In 8.8 fixed point the carry is held in units of 1/256 of an unscaled
    // guest cycle, so the same exactness argument holds: over any run the sum
    // of charged cycles equals floor(total_unscaled_cycles * 256 / factor_q8),
    // with less than one charged cycle ever outstanding.
    const unsigned long long carry = g_overclock_carry.load(std::memory_order_relaxed);
    const unsigned long long sum =
        static_cast<unsigned long long>(cycles) * kQ8One + carry;
    const unsigned long long charged = sum / factor;
    g_overclock_carry.store(sum - charged * factor, std::memory_order_relaxed);
    runtime_tick_common(static_cast<uint32_t>(charged));
}

// ── Stage 2: idle-loop elision prover ───────────────────────────────────────
// Per back-edge of a statically-eligible quiescent loop (codegen emits the
// call), prove the loop idle via a rolling fixed point and fast-forward the
// Stage-1 master clock to one period before the next scheduled event. See
// runtime_arm.h for the full contract and the cycle-accuracy argument.
namespace {
struct IdleSite {
    uint32_t regs[15];                 // R0-R14 at the previous back-edge
    uint32_t cpsr;                     // full CPSR at the previous back-edge
    unsigned long long last_cycles;    // g_runtime_cycles at previous back-edge
    unsigned long long last_epoch;     // g_idle_disturb_epoch at previous edge
    unsigned long long period;         // measured per-iteration cycle cost
    int  matches;                      // period-stable confirmations (>=1 → skip)
    bool have_period;
    bool valid;
};
static std::unordered_map<uint32_t, IdleSite> g_idle_sites;
static const bool g_idle_elision_on = [] {
    const char* e = std::getenv("GBARECOMP_IDLE_ELISION");
    bool on = !(e && e[0] == '0' && e[1] == '\0');   // default ON
    if (on) {
        std::atexit([] {
            if (g_idle_skipped_cycles == 0) return;
            std::fprintf(stderr,
                "[idle] elided %llu loop iterations (%llu cycles) over %llu "
                "skip ops across %zu site(s)\n",
                g_idle_skipped_iters, g_idle_skipped_cycles,
                g_idle_confirmed_sites, g_idle_sites.size());
        });
    }
    return on;
}();
}  // namespace

extern "C" void runtime_idle_backedge(uint32_t header_pc) {
    cost_bk(kBkIdleBackedge);
    runtime_flush_deferred();
    if (!g_idle_elision_on) return;
    if (g_runtime_shadow_tick) return;  // never alter time during a shadow re-run

    const unsigned long long now   = g_runtime_cycles;
    const unsigned long long epoch = g_idle_disturb_epoch;

    IdleSite& s = g_idle_sites[header_pc];

    bool regs_match = s.valid && s.cpsr == g_cpu.cpsr;
    if (regs_match) {
        for (int i = 0; i < 15; ++i) {
            if (s.regs[i] != g_cpu.R[i]) { regs_match = false; break; }
        }
    }
    const bool undisturbed = s.valid && (epoch == s.last_epoch);
    const unsigned long long delta = s.valid ? (now - s.last_cycles) : 0ull;

    // Rolling fixed point: the prior iteration left identical {R0-R14, CPSR},
    // nothing disturbed the watched state, and the period is stable. The loaded
    // poll value flows into a register, so an unchanged register set proves the
    // load returned the same value — no explicit load trace needed given the
    // static no-store / no-MMIO filter and the disturbance epoch.
    bool confirmed = false;
    if (regs_match && undisturbed && delta != 0ull) {
        if (s.have_period && delta == s.period) {
            if (++s.matches >= 1) confirmed = true;
        } else {
            s.period = delta;
            s.have_period = true;
            s.matches = 0;
        }
    } else {
        s.have_period = false;
        s.matches = 0;
    }

    if (confirmed && s.period != 0ull &&
        g_event_budget > static_cast<long long>(s.period)) {
        // next_event - now == g_event_budget (Stage-1 horizon). Skip whole
        // periods that all end strictly before it: periods = (budget-1)/period
        // guarantees now+skipped < next_event, so NO event fires in the hook.
        const unsigned long long budget =
            static_cast<unsigned long long>(g_event_budget);
        const unsigned long long periods = (budget - 1ull) / s.period;
        if (periods != 0ull) {
            const unsigned long long skipped = periods * s.period;
            g_runtime_cycles += skipped;   // master clock
            g_pending_cycles += skipped;   // materialized lazily at the horizon
            g_event_budget   -= static_cast<long long>(skipped);
            g_idle_skipped_cycles += skipped;
            g_idle_skipped_iters  += periods;
            ++g_idle_confirmed_sites;
        }
    }

    // Snapshot for the next iteration. Registers are unchanged by a skip;
    // last_cycles takes the post-skip clock so the next delta measures one real
    // period.
    for (int i = 0; i < 15; ++i) s.regs[i] = g_cpu.R[i];
    s.cpsr = g_cpu.cpsr;
    s.last_cycles = g_runtime_cycles;
    s.last_epoch  = g_idle_disturb_epoch;
    s.valid = true;
}

// Frame-present hook (present-in-place). When set, the per-VBlank frame-present
// yield does NOT unwind the guest stack: it calls this hook (which presents the
// frame, polls input, paces) and resumes the guest in place — so R15 is never
// re-dispatched from an arbitrary interior PC, eliminating the whole class of
// frame-boundary resume dispatch-misses. The hook returns true to request quit
// (then the yield DOES unwind, so the runner can exit). Set by the windowed
// runner only; unset for headless/TCP, which keep the unwind-and-redispatch path.
std::function<bool()> g_frame_present_hook;
// IRQ-safe present-in-place defers the wall-clock wait until the first
// mainline instruction after the handler returns. This prevents a 120 Hz
// monitor from becoming the guest clock while avoiding a sleep on the IRQ
// stack itself.
extern "C" uint32_t g_runtime_frame_pace_pending = 0;
// Sticky quit: once the present hook requests exit, EVERY subsequent yield must
// unwind (return true) so the guest's whole host call stack pops back to the
// runner — one return only frees one frame. Cleared when a hook is (re)set.
bool g_frame_present_quit = false;

void runtime_set_frame_present_hook(std::function<bool()> h) {
    g_frame_present_hook = std::move(h);
    g_frame_present_quit = false;
    g_runtime_frame_pace_pending = 0;
}

// Decides whether the guest must yield; runtime_should_yield() below turns a
// "yes" into the R15 sentinel. Every non-yield side effect (MP2K hook, BIOS
// prefetch latch, present-in-place hook, hang watchdog) stays here and still
// sees the real R15.
static bool runtime_should_yield_decide(void) {
    auto* bus = gbarecomp::g_active_bus;

    // Generated code enters this prologue for every guest instruction,
    // including direct calls that never pass through runtime_dispatch. This
    // is the common execution path for the copied SoundMainRAM routine, so
    // notify the native MP2K shadow here; it filters the notification to the
    // ROM-detected entry point.
    if (bus) {
        const uint32_t key = g_cpu.R[15] |
            ((g_cpu.cpsr & CPSR_T_BIT) ? 1u : 0u);
        const bool relevant = bus->audio().mp2k_frame_hook_may_be_relevant(key);
        if (g_cost_probe) {
            ++g_cost_mp2k_hook_checks;
            if (relevant) ++g_cost_mp2k_hook_matches;
        }
        if (relevant) {
            if (g_cost_probe) ++g_cost_mp2k_hook_deep_calls;
            bus->audio().mp2k_frame_hook(key, g_runtime_cycles);
        }
    }

    // ── BIOS open-bus prefetch latch (MC-HP-002) ─────────────────────────
    // Called once per guest instruction (the generated prologue calls this with
    // g_cpu.R[15] == the current PC). While PC is in the BIOS, keep the bus's
    // biosPrefetch latch current; when the BIOS hands control to the cart it
    // then holds the prefetch at the exit instruction — exactly what a later
    // cart read of the protected BIOS region must return (mGBA biosPrefetch;
    // for the GBA SWI return at BIOS 0x188 that is mem[0x190]=0xE3A02004). A
    // single compare per instruction; the image read happens only inside the
    // BIOS. See gba_bus.cpp prefetch_word / the open-bus read paths.
    if (bus && g_cpu.R[15] < 0x00004000u)
        bus->latch_bios_prefetch(g_cpu.R[15], (g_cpu.cpsr & CPSR_T_BIT) != 0);

    // Present-in-place quit: fully unwind the guest to the runner once requested.
    if (g_frame_present_quit) return true;

    // A VBlank present may have happened from inside an IRQ handler. Complete
    // its host pacing at the first safe mainline yield before the guest runs
    // ahead to the next frame. The hook recognizes this as a pacing-only call
    // when the frame number is unchanged.
    if (g_frame_present_hook && g_runtime_frame_pace_pending &&
        g_irq_nest_depth == 0u) {
        g_runtime_frame_pace_pending = 0;
        g_runtime_frame_present_in_irq = 0;
        if (g_frame_present_in_progress) return false;
        g_frame_present_in_progress = true;
        bool quit = g_frame_present_hook();
        g_frame_present_in_progress = false;
        if (quit) g_frame_present_quit = true;
        return quit;
    }

    bool halted = bus && bus->io().halted();

    // Some games, including Pokemon FireRed, busy-wait on a VBlank flag instead
    // of entering HALT. Yield once per VBlank-start in normal guest modes so the
    // host runner can present frames, poll input, and honor --frames even while
    // the guest stays inside one long-running dispatch. Do NOT yield from inside
    // an exception handler; runtime_irq runs the whole IRQ chain synchronously
    // (runtime_dispatch(0x18) between ++/-- g_irq_nest_depth), so unwinding there
    // would unwind that nested dispatch and abandon the handler before it acks
    // the interrupt — the FireRed VBlank freeze. The cpsr mode alone is NOT a
    // sufficient guard: a GBA IRQ dispatcher (intr_main) switches IRQ→System mode
    // mid-handler to allow nested IRQs, so it would pass a User/System-mode test
    // while still inside the IRQ. Gate on the live IRQ nesting depth too.
    static const bool yield_on_vblank = [] {
        const char* e = std::getenv("GBARECOMP_YIELD_ON_VBLANK");
        return !(e && e[0] == '0' && e[1] == '\0');
    }();
    if (yield_on_vblank &&
        g_runtime_vblank_starts != g_runtime_yielded_vblank_start) {
        const uint32_t mode = g_cpu.cpsr & 0x1Fu;
        if (mode == 0x10u || mode == 0x1Fu) {
            const bool in_irq = g_irq_nest_depth != 0u;
            if (in_irq && !g_frame_present_hook) return false;
            g_runtime_yielded_vblank_start = g_runtime_vblank_starts;
            // Present-in-place when a hook is registered (windowed runner): the
            // frame is presented from here and the guest resumes WITHOUT
            // unwinding — no interior-PC re-dispatch, so no frame-boundary
            // dispatch-miss. Only unwind (return true) when the hook asks to
            // quit. With no hook (headless/TCP), keep the original unwind path.
            if (g_frame_present_hook) {
                if (g_frame_present_in_progress) return false;
                g_runtime_frame_present_in_irq = in_irq ? 1u : 0u;
                g_frame_present_in_progress = true;
                bool quit = g_frame_present_hook();
                g_frame_present_in_progress = false;
                g_runtime_frame_present_in_irq = 0u;
                // An active IRQ must reach its guest iret before the host can
                // unwind for quit. The callback defers that decision.
                if (!in_irq && quit) g_frame_present_quit = true;
                return !in_irq && quit;
            }
            return true;
        }
    }

    // ── Always-on hang watchdog ──────────────────────────────────────
    // A healthy GBA game HALTs (VBlankIntrWait) ~60x/sec; the MC-HP-002
    // freeze is a busy-spin in the M4A mixer that NEVER halts (the PPU
    // still ticks, so frame counters keep advancing — "no frames" is the
    // wrong signal; "no HALT" is the right one). If we go many seconds of
    // wall-clock with no HALT, snapshot the live M4A state ONCE and keep
    // running (observation, not a fix). Disable with
    // GBARECOMP_HANG_WATCHDOG=0; tune seconds with GBARECOMP_HANG_SECONDS.
    static const bool wd_on = [] {
        const char* e = std::getenv("GBARECOMP_HANG_WATCHDOG");
        return !(e && e[0] == '0' && e[1] == '\0');
    }();
    if (wd_on) {
        static const long long wd_secs = [] {
            const char* e = std::getenv("GBARECOMP_HANG_SECONDS");
            long long s = e ? std::atoll(e) : 0;
            return s > 0 ? s : 4;
        }();
        static bool tripped = false;
        static std::chrono::steady_clock::time_point last_halt =
            std::chrono::steady_clock::now();
        static unsigned long long calls = 0;
        if (halted) {
            last_halt = std::chrono::steady_clock::now();
        } else if (!tripped && (++calls & 0xFFFFFull) == 0) {
            auto idle = std::chrono::steady_clock::now() - last_halt;
            if (std::chrono::duration_cast<std::chrono::seconds>(idle).count() >= wd_secs) {
                gbarecomp::dump_hang_state(
                    "guest has not HALTed for several seconds — likely a busy-spin "
                    "freeze (MC-HP-002 class)");
                tripped = true;
            }
        }
    }

    if (g_runtime_break_pc != 0u && g_cpu.R[15] == g_runtime_break_pc) {
        return true;  // debug breakpoint hit — unwind to the exec loop
    }
    return halted;
}

extern "C" uint32_t g_yield_resume_pc;
extern "C" bool     g_yield_resume_pending;

// A yield unwinds the host C stack: each generated call site sees R15 != its
// return address, cancels its frame and returns. Stale sites (left live by
// code whose trampoline returned elsewhere, e.g. the overlay unpacker at
// 0x03002000) can expect the very PC we yielded at and would then CONTINUE
// old C code on new state. Hide the resume PC behind a sentinel that no call
// site can match; runtime_yield_restore_pc() puts it back at the loop that
// re-dispatches it. Sentinel 0xFFFFFFF0 is outside the GBA bus (max 0x0FFFFFFF)
// so it is never a return address. FACTS.md "Door+Turbo crash".
extern "C" bool runtime_should_yield(void) {
    cost_bk(kBkShouldYield);
    if (!runtime_should_yield_decide()) return false;
    // Keep the first stash if a second yield is reported before the restore.
    if (!g_yield_resume_pending) {
        g_yield_resume_pc = g_cpu.R[15];
        g_yield_resume_pending = true;
    }
    g_cpu.R[15] = 0xFFFFFFF0u;
    return true;
}
