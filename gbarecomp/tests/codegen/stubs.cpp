// tests/codegen/stubs.cpp — host-side glue for the codegen test
// runner.
//
// The codegen test binary links gbarecomp_armv4t directly but does
// NOT pull in gbarecomp_runtime (which would drag in the real
// GbaBus, the runtime exec loop, etc.). To satisfy the linker we
// provide:
//   - bus_read_*/bus_write_* against a singleton FlatBus
//   - kDispatchTable + kDispatchTableLen (empty stubs)
//   - runtime_dispatch_miss + runtime_swi overrides that DO NOT
//     abort — they record what was called so the runner can
//     verify branch tests
//
// runtime_arm.cpp's stock runtime_dispatch_miss + runtime_swi are
// marked __attribute__((weak)) — these strong definitions win at
// link time.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "arm_ir.h"
#include "runtime_arm.h"
#include "stubs.h"

namespace {

// Single bus shared across all test cases. The runner calls
// codegen_test::bus_reset() before each test and seeds it with the
// test case's mem_init entries.
std::vector<uint8_t> g_mem;
uint32_t             g_mem_base = 0;
bool                 g_mem_oob_seen = false;

inline bool in_range(uint32_t addr, std::size_t w) {
    if (addr < g_mem_base) return false;
    std::size_t off = addr - g_mem_base;
    return off + w <= g_mem.size();
}

}  // namespace

namespace codegen_test {

void bus_reset(uint32_t base, uint32_t size) {
    runtime_ram_code_dirty_reset();
    g_mem.assign(size, 0);
    g_mem_base = base;
    g_mem_oob_seen = false;
    g_last_dispatch_target = 0;
    g_dispatch_called = false;
    g_last_swi_imm = 0;
    g_swi_called = false;
    g_ticked_cycles = 0;
    g_unimplemented_called = false;
    if (g_unimplemented_op) {
        std::free(reinterpret_cast<void*>(
            const_cast<char*>(g_unimplemented_op)));
        g_unimplemented_op = nullptr;
    }
    g_unimplemented_pc = 0;
    g_static_ram_calls = 0;
}

void bus_write_u32_direct(uint32_t addr, uint32_t v) {
    if (!in_range(addr, 4)) { g_mem_oob_seen = true; return; }
    uint8_t* p = &g_mem[addr - g_mem_base];
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

uint32_t bus_read_u32_direct(uint32_t addr) {
    if (!in_range(addr, 4)) { g_mem_oob_seen = true; return 0; }
    const uint8_t* p = &g_mem[addr - g_mem_base];
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

bool bus_oob_seen() { return g_mem_oob_seen; }
uint32_t bus_base() { return g_mem_base; }
uint32_t bus_size() { return static_cast<uint32_t>(g_mem.size()); }

const uint8_t* bus_data() {
    return g_mem.empty() ? nullptr : g_mem.data();
}

// Cycle accounting recorder (see stubs.h).
uint64_t g_ticked_cycles        = 0;

// Dispatch / SWI recorders.
uint32_t g_last_dispatch_target = 0;
bool     g_dispatch_called      = false;
uint32_t g_last_swi_imm         = 0;
bool     g_swi_called           = false;
bool     g_unimplemented_called = false;
const char* g_unimplemented_op  = nullptr;
uint32_t g_unimplemented_pc     = 0;
unsigned g_static_ram_calls     = 0;
OverlayResolveFn g_overlay_resolve_fn = nullptr;

}  // namespace codegen_test

// ── DispatchEntry definitions ──────────────────────────────────────
// runtime_arm.cpp declares:
//   struct DispatchEntry { uint32_t addr; uint8_t thumb; void (*fn)(void); };
//   extern "C" const DispatchEntry kDispatchTable[];
//   extern "C" const unsigned kDispatchTableLen;
// Provide minimal stubs. Empty length means every dispatch falls
// through to runtime_dispatch_miss, which we override below.
struct DispatchEntry { uint32_t addr; uint8_t thumb; void (*fn)(void); };

namespace {
void static_ram_entry() {
    ++codegen_test::g_static_ram_calls;
    g_cpu.R[0] = 0x12345678u;
}
}

extern "C" const DispatchEntry kDispatchTable[2] = {
    {0x03000100u, 0u, static_ram_entry},
    {0x03000110u, 0u, static_ram_entry},
};
extern "C" const unsigned kDispatchTableLen = 2u;

// BIOS dispatch table — empty for L1 tests. The runtime consults
// kBiosDispatchTable first for PC < 0x4000; with length 0 every BIOS
// PC falls through to kDispatchTable, then to runtime_dispatch_miss,
// which is the stubbed recorder above.
extern "C" const DispatchEntry kBiosDispatchTable[1] = {
    {0xFFFFFFFFu, 0u, nullptr},
};
extern "C" const unsigned kBiosDispatchTableLen = 0u;

// VBlank-start counter the runtime defines in runtime_bus_bridge.cpp and
// runtime_arm.cpp references for the watchpoint frame-gate. The L1 codegen
// harness links the armv4t lib without the runtime, so it provides its own.
extern "C" unsigned long long g_runtime_vblank_starts = 0;

// Cumulative guest-cycle clock. The production definition + increment live in
// runtime_bus_bridge.cpp (not linked here); runtime_arm.cpp's runtime_trace_event
// stamps it and runtime_trace_reset zeroes it, so the L1 harness must supply the
// symbol. The stub runtime_tick below doesn't touch it (no cycle-aligned diffing
// in unit tests), so it simply stays 0.
extern "C" unsigned long long g_runtime_cycles = 0;

// Savestate epoch paired with the cumulative clock above. runtime_arm.cpp tags
// trace records with both values, while these offline harnesses do not link the
// runtime bridge that owns the production counter.
extern "C" unsigned long long g_runtime_state_epoch = 0;

// TEMPORARY cost-attribution probe globals (see runtime_bus_bridge.cpp).
// runtime_arm.cpp's runtime_irq() now references these unconditionally to
// time the guest IRQ handler; the L1 harness doesn't link
// runtime_bus_bridge.cpp, so it must supply them. Probe stays off (returns
// false) so the timing branch in runtime_irq() never executes here.
extern "C" unsigned long long g_cost_irq_handler_ns = 0;
extern "C" unsigned long long g_cost_irq_handler_calls = 0;
extern "C" bool g_cost_probe_enabled(void) { return false; }
extern "C" unsigned long long g_cost_mp2k_trace_stores = 0;
extern "C" unsigned long long g_cost_mp2k_fast_accepts = 0;
extern "C" unsigned long long g_cost_mp2k_hook_pre = 0;
extern "C" unsigned long long g_cost_mp2k_hook_post = 0;
extern "C" unsigned long long g_cost_mp2k_exact_writer_matches = 0;
extern "C" unsigned long long g_cost_mp2k_fast_filter_ns = 0;
extern "C" unsigned long long g_cost_mp2k_hook_checks = 0;
extern "C" unsigned long long g_cost_mp2k_hook_matches = 0;
extern "C" unsigned long long g_cost_mp2k_hook_deep_calls = 0;

// ── Bus accessors used by generated test functions ─────────────────

extern "C" uint8_t bus_read_u8_slow(uint32_t addr) {
    if (!in_range(addr, 1)) { g_mem_oob_seen = true; return 0; }
    return g_mem[addr - g_mem_base];
}

extern "C" uint16_t bus_read_u16_slow(uint32_t addr) {
    if (!in_range(addr, 2)) { g_mem_oob_seen = true; return 0; }
    const uint8_t* p = &g_mem[addr - g_mem_base];
    return static_cast<uint16_t>(p[0]) |
           (static_cast<uint16_t>(p[1]) << 8);
}

extern "C" uint32_t bus_read_u32_slow(uint32_t addr) {
    return codegen_test::bus_read_u32_direct(addr);
}

extern "C" void bus_write_u8_slow(uint32_t addr, uint8_t v) {
    if (!in_range(addr, 1)) { g_mem_oob_seen = true; return; }
    g_mem[addr - g_mem_base] = v;
    runtime_note_ram_code_write(addr, 1u);
}

extern "C" void bus_write_u16_slow(uint32_t addr, uint16_t v) {
    if (!in_range(addr, 2)) { g_mem_oob_seen = true; return; }
    uint8_t* p = &g_mem[addr - g_mem_base];
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    runtime_note_ram_code_write(addr, 2u);
}

extern "C" void bus_write_u32_slow(uint32_t addr, uint32_t v) {
    codegen_test::bus_write_u32_direct(addr, v);
    runtime_note_ram_code_write(addr, 4u);
}

// ── Test-specific definitions of the runtime-level helpers ─────────
// gbarecomp_armv4t has no defaults for runtime_dispatch_miss and
// runtime_unimplemented_op (production builds get those from
// src/runtime/runtime_arm_default_aborts.cpp). Tests link only the
// armv4t lib, so we MUST supply them here — and we make them
// record-instead-of-abort so the runner can verify branch /
// unimplemented-op behavior cleanly.
//
// runtime_swi is NOT overridden — the production version in
// runtime_arm.cpp does the SVC exception entry and calls
// runtime_dispatch(0x08). That dispatch falls through to the stub
// runtime_dispatch_miss below, which records and returns. SWI tests
// then observe exception-entry state + that dispatch was called
// for pc=0x08.

extern "C" void runtime_dispatch_miss(uint32_t target_pc) {
    codegen_test::g_dispatch_called = true;
    codegen_test::g_last_dispatch_target = target_pc;
}

extern "C" void runtime_mutable_ram_code_miss(uint32_t entry_pc, int thumb) {
    runtime_dispatch_miss(entry_pc | (thumb ? 1u : 0u));
}

// Stage-2 self-heal dispatch tier. The L1 harness links only gbarecomp_armv4t
// (not the runtime lib), so supply null stubs: no overlay is ever healed under
// test and every dispatch falls through to the recorder above.
extern "C" int overlay_try_dispatch(uint32_t /*pc*/, int /*thumb*/) {
    return 0;
}

namespace gbarecomp {
extern "C" void (*overlay_resolve(uint32_t /*pc*/, int /*thumb*/))(void) {
    return codegen_test::g_overlay_resolve_fn;
}
}  // namespace gbarecomp

extern "C" void runtime_unimplemented_op(const char* op_name,
                                          uint32_t pc) {
    codegen_test::g_unimplemented_called = true;
    if (op_name) {
        codegen_test::g_unimplemented_op = strdup(op_name);
    } else {
        codegen_test::g_unimplemented_op = nullptr;
    }
    codegen_test::g_unimplemented_pc = pc;
    // Don't abort — let the runner report the failure cleanly.
}

extern "C" void runtime_tick(uint32_t cycles) {
    // Accumulate so the runner can diff the recompiled instruction's
    // total cycle cost against the interpreter's per-instruction count.
    // (No PPU/audio/IRQ side effects here — production runners link the
    // real runtime_tick from runtime_bus_bridge.cpp for those.)
    codegen_test::g_ticked_cycles += cycles;
}

extern "C" bool runtime_should_yield(void) {
    return false;
}

// The FlatBus on the interpreter side uses armv4t::Bus's default
// access_cycles (1 cycle / access), so the L1 cycle diff validates
// base costs, the shift/PC-write surcharges, multiply operand waits,
// and LDM/STM access *counts*. Region-specific waitstates live in
// gba::GbaBus::access_cycles (shared by both execution paths in the
// full build) and are exercised by the differential oracle, not here.
extern "C" uint32_t runtime_mem_cycles(uint32_t /*addr*/, uint32_t /*width*/,
                                       uint32_t /*sequential*/) {
    return 1u;
}

extern "C" uint32_t runtime_mul_cycles(uint32_t rs_value,
                                       uint32_t signed_variant,
                                       uint32_t extra) {
    return armv4t::mul_wait_cycles(rs_value, signed_variant != 0u, extra);
}

// The RAM fast path in runtime_arm.h consults these. Left null so every access
// in the codegen tests goes through the stub bus above (the tests model a flat
// synthetic bus, not GBA region layout).
extern "C" uint8_t* g_fast_ewram = nullptr;
extern "C" uint8_t* g_fast_iwram = nullptr;
// Same story for the ROM fast path: left disabled (g_fast_rom_ok = 0) so
// these tests keep exercising the flat synthetic bus above.
extern "C" const uint8_t* g_fast_rom      = nullptr;
extern "C" uint32_t       g_fast_rom_size = 0;
extern "C" int            g_fast_rom_ok   = 0;
extern "C" int (*g_rom_read32_override)(uint32_t, uint32_t, uint32_t*) = nullptr;
// The inline write path bumps this; the real definition lives in the runtime
// lib, which these armv4t-only tests do not link.
extern "C" unsigned long long g_idle_disturb_epoch = 0;
