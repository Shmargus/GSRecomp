// host_prof_phase.h -- which part of the host emulation thread is running, for
// the host sampling profiler (GBARECOMP_HOST_PROF, launcher "Host profiler").
//
// The sampler thread (runtime_bus_bridge.cpp, host_prof_loop) suspends the
// emulation thread and reads its instruction pointer. That says WHERE in the
// host binary it was, not WHY: a sample in ntdll.dll could be the vsync wait
// inside present, SDL event pumping, or a lock inside guest execution. The
// emulation thread therefore publishes a small phase id, and the sampler
// records it next to every sample.
//
// Deliberately NOT included by runtime_arm.h: every translated game file
// includes that header, so anything added there recompiles ~450 files. Only
// the engine's own .cpp files include this one.
//
// Cost when the profiler is off: one load of g_host_prof_enabled and a
// not-taken branch per scope; nothing is written.

#pragma once

#include <atomic>
#include <cstdint>

namespace gbarecomp {

// Phase ids. Written to the profile header as "# phase <id> <name>" (names
// come from host_prof_phase_name in runtime_bus_bridge.cpp), so keep the two
// lists in step. 0 is the default: guest dispatch, and any host code that is
// not inside a marked scope.
enum HostProfPhase : std::uint8_t {
    kHpGuest = 0,       // guest dispatch / unmarked
    kHpIrqHandler = 1,  // runtime_irq(): the guest interrupt handler
    kHpTickDevices = 2, // tick_devices(): audio, timers, DMA, PPU events
    kHpPpuRender = 3,   // PPU scanline compositor + VBlank latch (inside 2)
    kHpRender = 4,      // present-hook work other than the calls below:
                        // view sync, latched copy, fresh render, interpolation
    kHpPresent = 5,     // win.present / present_native (vsync waits land here)
    kHpAudio = 6,       // service_audio()
    kHpPump = 7,        // pump_host_input(): config UI, input, win.pump()
    kHpPacer = 8,       // frame limiter wait (sampling is skipped while idle)
    kHpHaltPump = 9,    // HALT idle pump (pump_idle loop)
    kHpCompileWait = 10,// overlay_wait_resolve(): waiting on a native compile
    kHpSdlEvents = 11,  // SDL_PumpEvents / SDL_PeepEvents / SDL_PollEvent
    kHpPhaseCount = 12
};

// True only while the host profiler is running. Set once, on the emulation
// thread, before any guest code runs (start_host_prof, called from
// set_active_bus); read-only afterwards.
extern bool g_host_prof_enabled;
// Current phase. Single writer (the emulation thread); the sampler reads it.
extern std::atomic<std::uint8_t> g_host_prof_phase;
// Guest frame counter mirror (GbaPpu::frame_count()), updated on the
// emulation thread where the PPU wraps a frame (tick_devices), and again after
// a savestate load moves it. runtime_current_frame() itself reads the PPU and
// is not safe to call from the sampler thread.
extern std::atomic<std::uint64_t> g_host_prof_frame;
// The frame key of the last row appended to the frame-phase CSV
// (frame_phase.record in runtime.cpp). Samples taken after it belong to the
// NEXT row: a CSV row's guest_us spans the time between two presents, and the
// PPU counter alone straddles that span (it wraps mid-way).
extern std::atomic<std::uint64_t> g_host_prof_present_frame;

// RAII: mark a region, restore the enclosing phase on exit. Scopes nest
// (guest > tick_devices > ppu_render).
struct HostProfPhaseScope {
    std::uint8_t prev = 0;
    explicit HostProfPhaseScope(std::uint8_t id) {
        if (g_host_prof_enabled) {
            prev = g_host_prof_phase.load(std::memory_order_relaxed);
            g_host_prof_phase.store(id, std::memory_order_relaxed);
        }
    }
    ~HostProfPhaseScope() {
        if (g_host_prof_enabled)
            g_host_prof_phase.store(prev, std::memory_order_relaxed);
    }
    HostProfPhaseScope(const HostProfPhaseScope&) = delete;
    HostProfPhaseScope& operator=(const HostProfPhaseScope&) = delete;
};

// Called by the frame-phase recorder each time it appends a row.
inline void host_prof_note_row_recorded(std::uint64_t frame) {
    if (g_host_prof_enabled)
        g_host_prof_present_frame.store(frame, std::memory_order_relaxed);
}

}  // namespace gbarecomp

// ── Per-instruction bookkeeping call counts (GBARECOMP_COST_PROBE) ───────────
// Generated code calls these out-of-line helpers for every guest instruction
// (runtime_arm.h). With the cost probe on, each helper bumps its slot here
// (one predictable branch on g_cost_probe); runtime.cpp turns the per-frame
// deltas into trailing columns of the frame-phase CSV. Each helper counts its
// OWN entries, including entries made by another helper: runtime_insn_begin
// calls runtime_insn_boundary, which calls runtime_should_yield, and all of
// them reach runtime_mem_cycles, so the slots are logical call counts, not
// disjoint shares. Names are the CSV column suffixes.
enum CostBk {
    kBkInsnBegin = 0,
    kBkInsnBoundary,
    kBkShouldYield,
    kBkTick,           // runtime_tick (generated code's per-instruction tick)
    kBkTickRealtime,   // runtime_tick_realtime (HALT pump)
    kBkMemCycles,
    kBkFetchNsDelta,
    kBkRefillCycles,
    kBkLd,             // runtime_ld_u32 + _u16 + _u8
    kBkSt,             // runtime_st_u32 + _u16 + _u8
    kBkLdm,
    kBkStm,
    kBkMulCycles,
    kBkBusRead,        // runtime_bus_read_u32/u16/u8 (outlined bus)
    kBkBusWrite,       // runtime_bus_write_u32/u16/u8
    kBkIdleBackedge,   // runtime_idle_backedge
    kBkCount
};
inline constexpr const char* kCostBkNames[kBkCount] = {
    "insn_begin", "insn_boundary", "should_yield", "tick", "tick_realtime",
    "mem_cycles", "fetch_ns_delta", "refill_cycles", "ld", "st", "ldm", "stm",
    "mul_cycles", "bus_read", "bus_write", "idle_backedge",
};
// Defined in runtime_bus_bridge.cpp.
extern "C" unsigned long long g_cost_bk[kBkCount];
