#include <cstdint>

// gbarecomp_gba intentionally exposes the ARM runtime's RAM invalidation and
// tracing seams to the bus. This offline test does not link gbarecomp_runtime,
// so provide stable, side-effect-free definitions for the runtime-only pieces
// needed to satisfy that dependency.

struct DispatchEntry {
    uint32_t addr;
    uint8_t thumb;
    void (*fn)(void);
};

extern "C" {
unsigned long long g_runtime_cycles = 0;
unsigned long long g_runtime_state_epoch = 0;
unsigned long long g_runtime_vblank_starts = 0;
unsigned long long g_cost_irq_handler_ns = 0;
unsigned long long g_cost_irq_handler_calls = 0;
unsigned long long g_cost_mp2k_trace_stores = 0;
unsigned long long g_cost_mp2k_fast_accepts = 0;
unsigned long long g_cost_mp2k_hook_pre = 0;
unsigned long long g_cost_mp2k_hook_post = 0;
unsigned long long g_cost_mp2k_exact_writer_matches = 0;
unsigned long long g_cost_mp2k_fast_filter_ns = 0;
unsigned long long g_cost_mp2k_hook_checks = 0;
unsigned long long g_cost_mp2k_hook_matches = 0;
unsigned long long g_cost_mp2k_hook_deep_calls = 0;

uint8_t* g_fast_ewram = nullptr;
uint8_t* g_fast_iwram = nullptr;
const uint8_t* g_fast_rom = nullptr;
uint32_t g_fast_rom_size = 0;
int g_fast_rom_ok = 0;

bool g_cost_probe_enabled(void) {
    return false;
}

uint8_t bus_read_u8_slow(uint32_t) {
    return 0;
}

uint16_t bus_read_u16_slow(uint32_t) {
    return 0;
}

uint32_t bus_read_u32_slow(uint32_t) {
    return 0;
}

void bus_write_u16_slow(uint32_t, uint16_t) {}

void runtime_dispatch_miss(uint32_t) {}
void runtime_mutable_ram_code_miss(uint32_t, int) {}
void runtime_tick(uint32_t) {}

}

// Namespace-scope const objects inside an extern-C block otherwise retain
// internal linkage under C++. These explicit C-linkage definitions satisfy the
// dispatch-table references from runtime_arm.cpp.
extern "C" const DispatchEntry kDispatchTable[1] = {};
extern "C" const unsigned kDispatchTableLen = 0;
extern "C" const DispatchEntry kBiosDispatchTable[1] = {};
extern "C" const unsigned kBiosDispatchTableLen = 0;

namespace gbarecomp {
extern "C" void (*overlay_resolve(uint32_t, int))(void) {
    return nullptr;
}
}  // namespace gbarecomp
