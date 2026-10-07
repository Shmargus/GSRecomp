// runtime_arm.cpp — implementation of the C-ABI surface that the
// recompiled cart code calls into.
//
// The interpreter encodes the canonical ARM/THUMB semantics; this
// file is the parallel surface for recompiled code. Helpers here
// must produce the SAME bit-level result as the interpreter would
// for the same inputs — verify any new helper against the
// interpreter's case in src/armv4t/interpreter.cpp.

#include "runtime_arm.h"
#include "symbol_lookup.h"
#include "env_flag.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

// BIOS execution inventory state. The normal fingerprint ring remains the
// default; the lightweight mode is armed only when its output path is set.
constexpr uint32_t kBiosPcRegionEnd = 0x00004000u;
bool g_bios_pc_log_armed = false;
// Per-instruction trail arming: GBARECOMP_INSN_TRACE and the F1 crash log.
static bool g_insn_trace_from_env = false;
static bool g_crash_log = false;
// Per BIOS halfword and mode: bit 0 seen before the first cartridge
// instruction (boot), bit 1 seen after it (play: SWIs, IRQ entry, waits).
std::vector<uint8_t> g_bios_pc_seen;
unsigned long long g_bios_pc_samples = 0;
bool g_bios_pc_handed_off = false;

}  // namespace

// Single-use store override state for the optional game-owned memory-write
// transform seam. The generated trace call and its following bus write
// execute on the emulation thread, so the pending fields do not need a lock.
extern "C" uint32_t g_runtime_mem_write_override_pending_addr = 0u;
extern "C" uint32_t g_runtime_mem_write_override_pending_value = 0u;
extern "C" uint32_t g_runtime_mem_write_override_pending_valid = 0u;

// TEMPORARY cost-attribution probe (GBARECOMP_COST_PROBE=1), see
// runtime_bus_bridge.cpp. runtime_irq()'s "drive to completion" loop below
// is NOT loop overhead — it dispatches the guest's actual IRQ handler (e.g.
// the VBlank ISR / sound-engine mixing) via nested runtime_dispatch() calls,
// same cost class as the top-level dispatch bucket, just charged from a
// different call site. Isolated here so the halt-bucket breakdown does not
// mislabel real guest execution as "IRQ eligibility check" overhead.
// Diagnostic-only; intended for removal after the report.
extern "C" unsigned long long g_cost_irq_handler_ns;
extern "C" unsigned long long g_cost_irq_handler_calls;
extern "C" bool g_cost_probe_enabled(void);
extern "C" bool runtime_mp2k_write_relevant(uint32_t pc, uint32_t addr,
                                              uint32_t width)
#if defined(__GNUC__)
    __attribute__((weak))
#endif
    ;
extern "C" bool runtime_mp2k_control_relevant(uint32_t pc)
#if defined(__GNUC__)
    __attribute__((weak))
#endif
    ;
extern "C" unsigned long long g_cost_mp2k_trace_stores;
extern "C" unsigned long long g_cost_mp2k_fast_accepts;
extern "C" unsigned long long g_cost_mp2k_hook_pre;
extern "C" unsigned long long g_cost_mp2k_hook_post;
extern "C" unsigned long long g_cost_mp2k_exact_writer_matches;
extern "C" unsigned long long g_cost_mp2k_fast_filter_ns;
extern "C" unsigned long long g_cost_mp2k_hook_checks;
extern "C" unsigned long long g_cost_mp2k_hook_matches;
extern "C" unsigned long long g_cost_mp2k_hook_deep_calls;

// runtime_dispatch_miss / runtime_unimplemented_op default-abort
// implementations live in src/runtime/runtime_arm_default_aborts.cpp
// (part of gbarecomp_runtime), NOT here. The split exists because
// MinGW PE-COFF weak symbols don't reliably resolve from static
// archives, so we can't use weak overrides for those. Splitting
// puts the abort versions in the runtime library that production
// builds link, while tests/codegen/ links only gbarecomp_armv4t
// and supplies its own non-aborting stubs.
//
// runtime_swi STAYS here — its production behavior is exactly the
// exception-entry shape every consumer needs; tests verify that
// shape rather than override it.

// Forward decls of the per-game dispatch table. Each game's
// generated/dispatch_table.cpp defines these.
struct DispatchEntry {
    uint32_t addr;
    uint8_t thumb;
    void (*fn)(void);
};
extern "C" const DispatchEntry kDispatchTable[];
extern "C" const unsigned kDispatchTableLen;

// BIOS dispatch table. Lives in src/runtime/generated_bios/
// bios_dispatch_table.cpp — placeholder until `gba_recompile --bios`
// is run. runtime_dispatch consults this FIRST for PC < 0x4000.
extern "C" const DispatchEntry kBiosDispatchTable[];
extern "C" const unsigned kBiosDispatchTableLen;

// Stage-2 self-heal third dispatch tier. Defined in the runtime lib
// (src/runtime/overlay_loader.cpp); test binaries that link only
// gbarecomp_armv4t supply a null-returning stub (tests/codegen/stubs.cpp).
// runtime_dispatch consults it after the static tables miss; it returns the
// healed native function or null. armv4t must not depend on the runtime lib,
// so the link is a plain extern "C" symbol, mirroring runtime_dispatch_miss.
namespace gbarecomp {
extern "C" void (*overlay_resolve(uint32_t pc, int thumb))(void);
}

// ── CPU state ──────────────────────────────────────────────────────

extern "C" ArmCpuState g_cpu = {};
extern "C" RuntimeFastEwramWriteObserver
    g_runtime_fast_ewram_write_observer = nullptr;
extern "C" RuntimeFastIwramWriteObserver
    g_runtime_fast_iwram_write_observer = nullptr;
extern "C" uint64_t g_runtime_fast_iwram_watch[8] = {};
extern "C" RuntimeMemWriteObserverHook
    g_runtime_mem_write_observer = nullptr;
extern "C" RuntimeThumbAluImmediateOverride
    g_runtime_thumb_alu_imm_override = nullptr;
extern "C" RuntimeThumbLiteralOverride
    g_runtime_thumb_literal_override = nullptr;
extern "C" RuntimeConditionalBranchOverride
    g_runtime_conditional_branch_override = nullptr;
extern "C" RuntimeSwiOverride g_runtime_swi_override = nullptr;
extern "C" RuntimeRamDispatchHook g_runtime_ram_dispatch_hook = nullptr;
extern "C" RuntimeRamIdentityConfirmedHook
    g_runtime_ram_identity_confirmed_hook = nullptr;
extern "C" RuntimeCallReturnHook g_runtime_call_return_hook = nullptr;
extern "C" RuntimeGuestStepBoundaryHook
    g_runtime_guest_step_boundary_hook = nullptr;
extern "C" RuntimeMp2kWriteHook g_runtime_mp2k_write_hook = nullptr;
extern "C" RuntimeMp2kControlHook g_runtime_mp2k_control_hook = nullptr;
extern "C" RuntimeMemWriteOverrideHook
    g_runtime_mem_write_override = nullptr;
extern "C" RuntimeRamPointerWriteProbe g_runtime_ram_pointer_write_probe = nullptr;
extern "C" RuntimeRamImageDmaProbe g_runtime_ram_image_dma_probe = nullptr;
extern "C" RuntimeRamImageWriteProbe
    g_runtime_ram_image_write_probe = nullptr;
extern "C" RuntimeRamImageBoundaryProbe
    g_runtime_ram_image_boundary_probe = nullptr;
extern "C" RuntimeRamImageDispatchProbe
    g_runtime_ram_image_dispatch_probe = nullptr;
extern "C" unsigned int g_runtime_ram_image_dma_depth = 0;
extern "C" unsigned int g_runtime_ram_image_bus_depth = 0;
extern "C" uint32_t g_runtime_image_base = 0u;
extern "C" unsigned long long g_ram_write_epoch = 0;
extern "C" unsigned long long g_ram_code_page_mask_ewram_lo = 0;
extern "C" unsigned long long g_ram_code_page_mask_ewram_hi = 0;
extern "C" unsigned int g_ram_code_page_mask_iwram = 0;
extern "C" unsigned int g_ram_code_page_epoch_ewram[64] = {};
extern "C" unsigned int g_ram_code_page_epoch_iwram[8] = {};

extern "C" unsigned long long g_runtime_vblank_starts;

namespace {
uint64_t g_ram_dirty_ewram[1024] = {};
uint64_t g_ram_dirty_iwram[128] = {};

struct PoolDispatchHistoryEntry {
    uint32_t pc = 0;
    uint32_t r8 = 0;
    uint32_t r9 = 0;
    uint32_t image_base = 0;
};
constexpr unsigned kPoolDispatchHistoryCapacity = 16u;
PoolDispatchHistoryEntry g_pool_dispatch_history[
    kPoolDispatchHistoryCapacity]{};
unsigned g_pool_dispatch_history_next = 0;
unsigned g_pool_dispatch_history_count = 0;
uint32_t g_pool_dispatch_history_last_dump_pc = UINT32_MAX;
bool g_pool_dispatch_history_dumped = false;

struct PoolLdmSlotWrite {
    bool valid = false;
    uint32_t addr = 0;
    uint32_t value = 0;
    uint32_t width = 0;
    uint32_t pc = 0;
    uint64_t frame = 0;
    uint32_t sp = 0;
    uint32_t r9 = 0;
    // Savestate-load epoch the write happened in; separates pre/post-load
    // provenance when the poison predates a restore.
    uint64_t state_epoch = 0;
};

// Depth-bounded provenance ring for one tracked slot. Newest entry backs the
// existing last-write reporting; older entries keep an earlier (possibly
// poisoning) writer visible after later carrier pushes would otherwise
// overwrite it. Payload-free fields only — no retained guest bytes beyond
// the recorded word value itself.
struct PoolLdmWriteRing {
    static constexpr unsigned kDepth = 8u;
    PoolLdmSlotWrite e[kDepth]{};
    unsigned count = 0u;  // valid entries, <= kDepth
    unsigned head = 0u;   // next push index
    void push(const PoolLdmSlotWrite& w) {
        e[head] = w;
        head = (head + 1u) % kDepth;
        if (count < kDepth) ++count;
    }
    const PoolLdmSlotWrite* newest() const {
        return count == 0u ? nullptr : &e[(head + kDepth - 1u) % kDepth];
    }
};

struct PoolLdmSourceSnapshot {
    bool valid = false;
    uint64_t frame = 0;
    uint64_t state_epoch = 0;
    uint32_t sp = 0;
    uint32_t r9 = 0;
    uint32_t slot_addr[2] = {};
    uint32_t slot_value[2] = {};
    PoolLdmWriteRing ring[2]{};
    PoolLdmWriteRing fixed_ring[2]{};
};

// First post-arm clean -> poison transition for the physical r9 source word.
// This is deliberately independent of the bounded writer rings: a carrier
// storm must not evict the one transition that matters to CRASH-03.
struct PoolLdmPoisonLatch {
    bool valid = false;
    uint32_t pc = 0;
    uint32_t lr = 0;
    uint32_t sp = 0;
    uint32_t r9 = 0;
    uint64_t state_epoch = 0;
    uint64_t frame = 0;
    uint8_t thumb = 0;
    uint32_t image_base = 0;
    uint32_t register_equal_mask = 0;
    const char* seam = "unknown";
};

constexpr uint32_t kPoolLdmPc = 0x03006100u;
constexpr uint32_t kPoolLdmSlot0 = 0x03007E24u;
constexpr uint32_t kPoolLdmSlot1 = 0x03007E28u;
constexpr uint32_t kPoolLdmSlotCount = 2u;
PoolLdmWriteRing g_pool_ldm_slot_writes[kPoolLdmSlotCount]{};
// The crash context reports the post-LDM SP, not necessarily the pre-LDM
// address. Keep a small, payload-free window around the measured stack so a
// future invalid-pool dump can identify the actual two loaded words even when
// the entry SP is not 0x03007E24.
constexpr uint32_t kPoolLdmStackBase = RUNTIME_POOL_LDM_WINDOW_BASE;
constexpr uint32_t kPoolLdmStackEnd = RUNTIME_POOL_LDM_WINDOW_END;
constexpr uint32_t kPoolLdmStackSlotCount =
    (kPoolLdmStackEnd - kPoolLdmStackBase) / 4u;
PoolLdmWriteRing g_pool_ldm_stack_writes[kPoolLdmStackSlotCount]{};
PoolLdmSourceSnapshot g_pool_ldm_source{};
bool g_pool_ldm_probe_armed = false;
PoolLdmPoisonLatch g_pool_ldm_poison_latch{};
bool g_pool_ldm_poison_present = false;

constexpr uint32_t kPoolLdmPoison = 0xDCEF0210u;
constexpr uint32_t kPoolLdmPoisonTarget = 0x03007E30u;

// GBA IWRAM is mirrored throughout the 0x03xxxxxx region. Generated stores
// normally use the canonical 0x0300xxxx address, but a direct CPU/DMA store
// may use an alias and still modify the same physical stack cell. Match the
// physical address for attribution while retaining the raw guest address in
// the record.
uint32_t pool_ldm_normalize_iwram_addr(uint32_t addr) {
    return (addr >> 24) == 0x03u
        ? 0x03000000u + (addr & 0x00007FFFu)
        : addr;
}

uint32_t pool_ldm_poison_register_mask() {
    uint32_t mask = 0;
    for (uint32_t i = 0; i <= 12u; ++i)
        if (g_cpu.R[i] == kPoolLdmPoison) mask |= 1u << i;
    if (g_cpu.R[14] == kPoolLdmPoison) mask |= 1u << 13u;
    return mask;
}

void pool_ldm_poison_arm(const char* reason) {
    const uint32_t live = bus_read_u32(kPoolLdmPoisonTarget);
    g_pool_ldm_poison_present = live == kPoolLdmPoison;
    std::fprintf(stderr,
        "runtime_arm: [pool-ldm-poison-arm] reason=%s classification=%s "
        "epoch=%llu frame=%llu\n", reason,
        g_pool_ldm_poison_present ? "target-poison-present" : "clean",
        static_cast<unsigned long long>(g_runtime_state_epoch),
        static_cast<unsigned long long>(g_runtime_vblank_starts));
}

uint32_t pool_ldm_reconstruct_post_word(uint32_t addr, uint32_t value,
                                        uint32_t width) {
    uint32_t result = bus_read_u32(kPoolLdmPoisonTarget);
    const uint32_t physical = pool_ldm_normalize_iwram_addr(addr);
    for (uint32_t i = 0; i < width && i < 4u; ++i) {
        const uint32_t byte_addr = physical + i;
        if (byte_addr < kPoolLdmPoisonTarget ||
            byte_addr >= kPoolLdmPoisonTarget + 4u) continue;
        const uint32_t shift = (byte_addr - kPoolLdmPoisonTarget) * 8u;
        result = (result & ~(0xFFu << shift)) |
                 (((value >> (i * 8u)) & 0xFFu) << shift);
    }
    return result;
}

void pool_ldm_poison_transition(uint32_t post_word, uint32_t pc,
                                uint32_t sp, const char* seam) {
    const bool poison = post_word == kPoolLdmPoison;
    if (!poison) {
        g_pool_ldm_poison_present = false;
        return;
    }
    if (g_pool_ldm_poison_latch.valid || g_pool_ldm_poison_present)
        return;
    g_pool_ldm_poison_latch.valid = true;
    g_pool_ldm_poison_latch.pc = pc;
    g_pool_ldm_poison_latch.lr = g_cpu.R[14];
    g_pool_ldm_poison_latch.sp = sp;
    g_pool_ldm_poison_latch.r9 = g_cpu.R[9];
    g_pool_ldm_poison_latch.state_epoch = g_runtime_state_epoch;
    g_pool_ldm_poison_latch.frame = g_runtime_vblank_starts;
    g_pool_ldm_poison_latch.thumb =
        (g_cpu.cpsr & CPSR_T_BIT) != 0u ? 1u : 0u;
    g_pool_ldm_poison_latch.image_base = g_runtime_image_base;
    g_pool_ldm_poison_latch.register_equal_mask =
        pool_ldm_poison_register_mask();
    g_pool_ldm_poison_latch.seam = seam;
    g_pool_ldm_poison_present = true;
}

void pool_ldm_note_write(uint32_t addr, uint32_t value, uint32_t width,
                         uint32_t pc, uint32_t sp, uint32_t r9,
                         uint64_t frame, const char* seam,
                         bool pre_write) {
    if (!g_pool_ldm_probe_armed || width == 0u) return;
    const uint32_t physical_addr = pool_ldm_normalize_iwram_addr(addr);
    const uint64_t write_end = static_cast<uint64_t>(physical_addr) + width;
    if (physical_addr < kPoolLdmPoisonTarget + 4u &&
        write_end > kPoolLdmPoisonTarget) {
        const uint32_t post = pre_write
            ? pool_ldm_reconstruct_post_word(addr, value, width)
            : bus_read_u32(kPoolLdmPoisonTarget);
        pool_ldm_poison_transition(post, pc, sp, seam);
    }
    PoolLdmSlotWrite rec{};
    rec.valid = true;
    rec.addr = addr;
    rec.value = value;
    rec.width = width;
    rec.pc = pc;
    rec.frame = frame;
    rec.sp = sp;
    rec.r9 = r9;
    rec.state_epoch = g_runtime_state_epoch;
    for (uint32_t i = 0; i < kPoolLdmSlotCount; ++i) {
        const uint32_t slot = i == 0u ? kPoolLdmSlot0 : kPoolLdmSlot1;
        const uint64_t slot_end = static_cast<uint64_t>(slot) + 4u;
        if (write_end <= slot || static_cast<uint64_t>(physical_addr) >= slot_end)
            continue;
        g_pool_ldm_slot_writes[i].push(rec);
    }
    const uint64_t tracked_end =
        static_cast<uint64_t>(kPoolLdmStackEnd);
    if (static_cast<uint64_t>(physical_addr) < tracked_end && write_end >
        static_cast<uint64_t>(kPoolLdmStackBase)) {
        const uint32_t first = physical_addr < kPoolLdmStackBase
            ? 0u : (physical_addr - kPoolLdmStackBase) / 4u;
        const uint32_t last = (write_end - 1u) >= tracked_end
            ? kPoolLdmStackSlotCount - 1u
            : static_cast<uint32_t>((write_end - 1u -
                                     kPoolLdmStackBase) / 4u);
        for (uint32_t i = first; i <= last &&
             i < kPoolLdmStackSlotCount; ++i) {
            g_pool_ldm_stack_writes[i].push(rec);
        }
    }
}

void pool_ldm_snapshot_before_body() {
    const uint32_t sp = g_cpu.R[13];
    if (sp < kPoolLdmStackBase || sp + 8u > kPoolLdmStackEnd ||
        (sp & 3u) != 0u) {
        g_pool_ldm_source = {};
        return;
    }
    PoolLdmSourceSnapshot snapshot{};
    snapshot.valid = true;
    snapshot.frame = g_runtime_vblank_starts;
    snapshot.state_epoch = g_runtime_state_epoch;
    snapshot.sp = sp;
    snapshot.r9 = g_cpu.R[9];
    snapshot.slot_addr[0] = sp;
    snapshot.slot_addr[1] = sp + 4u;
    snapshot.slot_value[0] = bus_read_u32(sp);
    snapshot.slot_value[1] = bus_read_u32(sp + 4u);
    const uint32_t stack_index = (sp - kPoolLdmStackBase) / 4u;
    snapshot.ring[0] = g_pool_ldm_stack_writes[stack_index];
    snapshot.ring[1] = g_pool_ldm_stack_writes[stack_index + 1u];
    snapshot.fixed_ring[0] = g_pool_ldm_slot_writes[0];
    snapshot.fixed_ring[1] = g_pool_ldm_slot_writes[1];
    g_pool_ldm_source = snapshot;
}
// IEEE 802.3 reflected CRC-32 over the tracked stack window, matching
// src/gba/crc32.h's crc32() without linking gba-layer code into armv4t.
// Payload-free: the window is hashed, never retained.
uint32_t pool_ldm_window_crc(void) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t a = kPoolLdmStackBase; a < kPoolLdmStackEnd; a += 4u) {
        uint32_t v = bus_read_u32(a);
        for (int b = 0; b < 4; ++b) {
            crc ^= v & 0xFFu;
            for (int bit = 0; bit < 8; ++bit)
                crc = (crc >> 1) ^
                      (0xEDB88320u & (0u - (crc & 1u)));
            v >>= 8;
        }
    }
    return ~crc;
}

void pool_ldm_reset_host_state() {
    for (auto& entry : g_pool_dispatch_history) entry = {};
    g_pool_dispatch_history_next = 0;
    g_pool_dispatch_history_count = 0;
    g_pool_dispatch_history_last_dump_pc = UINT32_MAX;
    g_pool_dispatch_history_dumped = false;
    for (auto& ring : g_pool_ldm_slot_writes) ring = {};
    for (auto& ring : g_pool_ldm_stack_writes) ring = {};
    g_pool_ldm_source = {};
    g_pool_ldm_poison_latch = {};
    g_pool_ldm_poison_present = false;
}
}

extern "C" void runtime_pool_dispatch_history_record(uint32_t pc,
                                                         int thumb) {
    // The generated blitter pool is the only requested source; keep this
    // range test on the dispatch hot path and retain no guest bytes.
    if (thumb ||
        !((pc >= 0x030057E0u && pc < 0x03005A64u) ||
          (pc >= 0x03006000u && pc < 0x03006500u)))
        return;
    const bool was_armed = g_pool_ldm_probe_armed;
    g_pool_ldm_probe_armed = true;
    if (!was_armed) {
        pool_ldm_poison_arm("arm");
        std::fprintf(stderr,
            "runtime_arm: [pool-ldm-crc] reason=arm "
            "range=0x%08X-0x%08X crc=0x%08X epoch=%llu frame=%llu\n",
            kPoolLdmStackBase, kPoolLdmStackEnd, pool_ldm_window_crc(),
            static_cast<unsigned long long>(g_runtime_state_epoch),
            static_cast<unsigned long long>(g_runtime_vblank_starts));
    }
    PoolDispatchHistoryEntry& e =
        g_pool_dispatch_history[g_pool_dispatch_history_next];
    e.pc = pc;
    e.r8 = g_cpu.R[8];
    e.r9 = g_cpu.R[9];
    e.image_base = g_runtime_image_base;
    g_pool_dispatch_history_next =
        (g_pool_dispatch_history_next + 1u) % kPoolDispatchHistoryCapacity;
    if (g_pool_dispatch_history_count < kPoolDispatchHistoryCapacity)
        ++g_pool_dispatch_history_count;
    if (pc == kPoolLdmPc) pool_ldm_snapshot_before_body();
}

extern "C" void runtime_pool_ldm_probe_restore_boundary(void) {
    pool_ldm_reset_host_state();
    g_pool_ldm_probe_armed = true;
    pool_ldm_poison_arm("restore");
    std::fprintf(stderr,
        "runtime_arm: [pool-ldm-crc] reason=restore "
        "range=0x%08X-0x%08X crc=0x%08X epoch=%llu frame=%llu\n",
        kPoolLdmStackBase, kPoolLdmStackEnd, pool_ldm_window_crc(),
        static_cast<unsigned long long>(g_runtime_state_epoch),
        static_cast<unsigned long long>(g_runtime_vblank_starts));
}

extern "C" void runtime_pool_dispatch_history_dump(uint32_t invalid_pc) {
    if ((g_pool_dispatch_history_count == 0u &&
         !g_pool_ldm_poison_latch.valid) ||
        g_pool_dispatch_history_dumped ||
        g_pool_dispatch_history_last_dump_pc == invalid_pc) return;
    g_pool_dispatch_history_last_dump_pc = invalid_pc;
    g_pool_dispatch_history_dumped = true;
    std::fprintf(stderr,
        "runtime_arm: pool dispatch history before invalid pc=0x%08X "
        "(last %u, payload-free)\n",
        invalid_pc, g_pool_dispatch_history_count);
    const unsigned first =
        (g_pool_dispatch_history_next + kPoolDispatchHistoryCapacity -
         g_pool_dispatch_history_count) % kPoolDispatchHistoryCapacity;
    for (unsigned i = 0; i < g_pool_dispatch_history_count; ++i) {
        const PoolDispatchHistoryEntry& e =
            g_pool_dispatch_history[(first + i) % kPoolDispatchHistoryCapacity];
        std::fprintf(stderr,
            "runtime_arm: pool-history[%u] pc=0x%08X r8=0x%08X "
            "r9=0x%08X image_base=0x%08X\n",
            i, e.pc, e.r8, e.r9, e.image_base);
    }
    if (g_pool_ldm_source.valid) {
        std::fprintf(stderr,
            "runtime_arm: pool-ldm load frame=%llu state_epoch=%llu "
            "pc=0x%08X sp=0x%08X pre_r9=0x%08X\n",
            static_cast<unsigned long long>(g_pool_ldm_source.frame),
            static_cast<unsigned long long>(g_pool_ldm_source.state_epoch),
            kPoolLdmPc, g_pool_ldm_source.sp, g_pool_ldm_source.r9);
        for (uint32_t i = 0; i < kPoolLdmSlotCount; ++i) {
            const PoolLdmWriteRing& ring = g_pool_ldm_source.ring[i];
            const PoolLdmSlotWrite* nw = ring.newest();
            if (!nw || !nw->valid) {
                std::fprintf(stderr,
                    "runtime_arm: pool-ldm slot=0x%08X live=0x%08X "
                    "last-write=none\n",
                    g_pool_ldm_source.slot_addr[i],
                    g_pool_ldm_source.slot_value[i]);
            } else {
                std::fprintf(stderr,
                    "runtime_arm: pool-ldm slot=0x%08X live=0x%08X "
                    "last-write addr=0x%08X value=0x%08X width=%u "
                    "pc=0x%08X frame=%llu sp=0x%08X r9=0x%08X"
                    " state_epoch=%llu\n",
                    g_pool_ldm_source.slot_addr[i],
                    g_pool_ldm_source.slot_value[i], nw->addr, nw->value,
                    nw->width, nw->pc,
                    static_cast<unsigned long long>(nw->frame), nw->sp,
                    nw->r9,
                    static_cast<unsigned long long>(nw->state_epoch));
            }
            for (unsigned k = 0; k < ring.count && k < PoolLdmWriteRing::kDepth;
                 ++k) {
                const unsigned idx =
                    (ring.head + PoolLdmWriteRing::kDepth - ring.count + k) %
                    PoolLdmWriteRing::kDepth;
                const PoolLdmSlotWrite& w = ring.e[idx];
                std::fprintf(stderr,
                    "runtime_arm: pool-ldm slot-ring[%u]=%u/%u "
                    "addr=0x%08X value=0x%08X width=%u pc=0x%08X "
                    "epoch=%llu frame=%llu sp=0x%08X r9=0x%08X\n",
                    i, k, ring.count, w.addr, w.value, w.width, w.pc,
                    static_cast<unsigned long long>(w.state_epoch),
                    static_cast<unsigned long long>(w.frame), w.sp, w.r9);
            }
        }
        for (uint32_t i = 0; i < kPoolLdmSlotCount; ++i) {
            const PoolLdmWriteRing& ring = g_pool_ldm_source.fixed_ring[i];
            const PoolLdmSlotWrite* nw = ring.newest();
            if (!nw || !nw->valid) {
                std::fprintf(stderr,
                    "runtime_arm: pool-ldm fixed-slot=0x%08X "
                    "last-write=none\n",
                    i == 0u ? kPoolLdmSlot0 : kPoolLdmSlot1);
            } else {
                std::fprintf(stderr,
                    "runtime_arm: pool-ldm fixed-slot=0x%08X "
                    "last-write addr=0x%08X value=0x%08X width=%u "
                    "pc=0x%08X frame=%llu sp=0x%08X r9=0x%08X"
                    " state_epoch=%llu\n",
                    i == 0u ? kPoolLdmSlot0 : kPoolLdmSlot1,
                    nw->addr, nw->value, nw->width, nw->pc,
                    static_cast<unsigned long long>(nw->frame), nw->sp,
                    nw->r9,
                    static_cast<unsigned long long>(nw->state_epoch));
            }
            for (unsigned k = 0; k < ring.count && k < PoolLdmWriteRing::kDepth;
                 ++k) {
                const unsigned idx =
                    (ring.head + PoolLdmWriteRing::kDepth - ring.count + k) %
                    PoolLdmWriteRing::kDepth;
                const PoolLdmSlotWrite& w = ring.e[idx];
                std::fprintf(stderr,
                    "runtime_arm: pool-ldm fixed-slot-ring[%u]=%u/%u "
                    "addr=0x%08X value=0x%08X width=%u pc=0x%08X "
                    "epoch=%llu frame=%llu sp=0x%08X r9=0x%08X\n",
                    i, k, ring.count, w.addr, w.value, w.width, w.pc,
                    static_cast<unsigned long long>(w.state_epoch),
                    static_cast<unsigned long long>(w.frame), w.sp, w.r9);
            }
        }
    }
    if (g_pool_ldm_poison_latch.valid) {
        const PoolLdmPoisonLatch& p = g_pool_ldm_poison_latch;
        std::fprintf(stderr,
            "runtime_arm: pool-ldm poison-transition pc=0x%08X "
            "lr=0x%08X sp=0x%08X r9=0x%08X state_epoch=%llu frame=%llu "
            "thumb=%u image_base=0x%08X seam=%s reg_equal_mask=0x%08X\n",
            p.pc, p.lr, p.sp, p.r9,
            static_cast<unsigned long long>(p.state_epoch),
            static_cast<unsigned long long>(p.frame), p.thumb, p.image_base,
            p.seam, p.register_equal_mask);
    }
}

// Frame-tagged, no-bytes pointer attribution. Only 32-bit stores whose
// normalized value points into the measured half-open image are reported.
extern "C" void runtime_note_ram_pointer_write(uint32_t dest,
                                                 uint32_t value,
                                                 uint32_t width) {
    if (!g_runtime_ram_pointer_write_probe || width != 4u) return;
    const uint32_t target = value & ~1u;
    if (target < 0x03005B0Cu || target >= 0x03005D5Cu) return;
    g_runtime_ram_pointer_write_probe(
        g_runtime_vblank_starts, dest, g_cpu.R[15],
        (g_cpu.cpsr & CPSR_T_BIT) != 0u ? 1u : 0u,
        target, value & 1u);
}

extern "C" void runtime_note_ram_image_dma(uint8_t completed,
                                             uint32_t source, uint32_t dest,
                                             uint32_t size, uint32_t crc32,
                                             uint32_t channel) {
    if (!g_runtime_ram_image_dma_probe) return;
    g_runtime_ram_image_dma_probe(
        g_runtime_vblank_starts, completed, source, dest, size,
        g_cpu.R[15], (g_cpu.cpsr & CPSR_T_BIT) != 0u ? 1u : 0u,
        crc32, channel);
}

extern "C" void runtime_note_ram_image_cpu_write(uint32_t pc,
                                                    uint32_t addr,
                                                    uint32_t width) {
    if (!g_runtime_ram_image_write_probe || width == 0u) return;
    constexpr uint32_t kImageStart = 0x03005B0Cu;
    constexpr uint32_t kImageEnd = 0x03005D5Cu;
    const uint64_t write_end = static_cast<uint64_t>(addr) + width;
    if (write_end <= kImageStart || addr >= kImageEnd) return;
    const uint32_t clipped_start =
        addr < kImageStart ? kImageStart : addr;
    const uint32_t clipped_end =
        write_end > kImageEnd ? kImageEnd : static_cast<uint32_t>(write_end);
    if (clipped_end <= clipped_start) return;
    g_runtime_ram_image_write_probe(
        g_runtime_vblank_starts, pc,
        (g_cpu.cpsr & CPSR_T_BIT) != 0u ? 1u : 0u,
        clipped_start, clipped_end - clipped_start);
}

// Diagnostic-only window re-CRC, callable from the runner alongside each
// [wide-auth-epoch] emission (see CRASH-03_HANDOFF.md). Payload-free.
extern "C" void runtime_note_pool_ldm_epoch_crc(void) {
    std::fprintf(stderr,
        "runtime_arm: [pool-ldm-crc] reason=auth-epoch "
        "range=0x%08X-0x%08X crc=0x%08X epoch=%llu frame=%llu\n",
        kPoolLdmStackBase, kPoolLdmStackEnd, pool_ldm_window_crc(),
        static_cast<unsigned long long>(g_runtime_state_epoch),
        static_cast<unsigned long long>(g_runtime_vblank_starts));
}

extern "C" void runtime_note_pool_ldm_bus_write(uint32_t addr,
                                                  uint32_t width) {
    if (!g_pool_ldm_probe_armed || width == 0u) return;
    // This seam runs after a direct/interpreter/DMA bus write commits. Feed
    // every write in the measured stack window, not only the two fixed slots:
    // the LDM's actual cells are selected by SP and may be 0x03007E2C/30.
    const uint32_t physical_addr = pool_ldm_normalize_iwram_addr(addr);
    const uint64_t write_end = static_cast<uint64_t>(physical_addr) + width;
    if (static_cast<uint64_t>(physical_addr) >=
            static_cast<uint64_t>(kPoolLdmStackEnd) ||
        write_end <= static_cast<uint64_t>(kPoolLdmStackBase)) {
        return;
    }
    pool_ldm_note_write(addr, bus_read_u32(physical_addr & ~3u), width,
                        g_cpu.R[15], g_cpu.R[13], g_cpu.R[9],
                        g_runtime_vblank_starts,
                        g_runtime_ram_image_dma_depth != 0u ? "dma" : "bus",
                        false);
}

extern "C" void runtime_note_ram_image_bus_write(uint32_t addr,
                                                    uint32_t width) {
    runtime_note_pool_ldm_bus_write(addr, width);
    if (g_runtime_ram_image_dma_depth != 0u ||
        g_runtime_ram_image_bus_depth != 0u) return;
    if (!g_runtime_ram_image_write_probe || width == 0u) return;
    runtime_note_ram_image_cpu_write(
        g_cpu.R[15], addr, width);
}

extern "C" void runtime_ram_image_bus_begin(void) {
    if (g_runtime_ram_image_bus_depth != ~0u)
        ++g_runtime_ram_image_bus_depth;
}

extern "C" void runtime_ram_image_bus_end(void) {
    if (g_runtime_ram_image_bus_depth != 0u)
        --g_runtime_ram_image_bus_depth;
}

extern "C" void runtime_ram_image_dma_begin(void) {
    if (g_runtime_ram_image_dma_depth != ~0u)
        ++g_runtime_ram_image_dma_depth;
}

extern "C" void runtime_ram_image_dma_end(void) {
    if (g_runtime_ram_image_dma_depth != 0u)
        --g_runtime_ram_image_dma_depth;
}

extern "C" void runtime_register_ram_code_page(uint32_t pc) {
    const uint32_t region = pc >> 24;
    if (region == 0x03u) {
        g_ram_code_page_mask_iwram |= 1u << ((pc & 0x7FFFu) >> 12);
    } else if (region == 0x02u) {
        const uint32_t page = (pc & 0x3FFFFu) >> 12;
        if (page < 32u) g_ram_code_page_mask_ewram_lo |= 1ull << page;
        else g_ram_code_page_mask_ewram_hi |= 1ull << (page - 32u);
    }
}

// Diagnostic ring of writes into the IWRAM code area (interrupt handler and
// the copied helper routines, 0x03000000-0x030022FF). Opt-in-free but cheap:
// one write per word per frame is kept, so per-frame scroll shadows do not
// flood it. Dumped when the bridge aborts or the IWRAM code stops matching
// any registered identity, to show what last rewrote the code and when.
namespace {
// Covers the whole overlay unpacker (Func_2544) at 0x03002000..0x030022C4.
constexpr uint32_t kIwramCodeDiagEnd = 0x2300u;
constexpr uint32_t kIwramCodeDiagWords = kIwramCodeDiagEnd / 4u;
constexpr uint32_t kIwramCodeRing = 256u;
struct IwramCodeWrite {
    unsigned long long frame;
    uint32_t addr, width, pc, lr;
};
IwramCodeWrite g_iwram_code_ring[kIwramCodeRing];
uint32_t g_iwram_code_ring_next = 0;
unsigned long long g_iwram_code_word_frame[kIwramCodeDiagWords];
}  // namespace

// Unpacker-slot journal (crash diagnosis). Always on, and independent of the
// ring above: that ring only sees writes to pages registered as RAM-code pages
// (runtime_register_ram_code_page), which is not guaranteed for the unpacker
// page, and it is flooded by data writes elsewhere. This one records only
// events touching the overlay unpacker's slot 0x03002000..0x030022C4:
//   dma   one entry per DMA transfer whose destination overlaps the slot
//         (dst, src, byte count) plus the first word at 0x03002000 afterwards,
//         which says which routine was copied in;
//   cpu   one entry per (word, frame) for bus/interpreter CPU stores, and for
//         generated-code stores on registered RAM-code pages;
//   enter every runtime_dispatch to exactly 0x03002000 (ARM), with r0, r1,
//         r6, sp, lr, the game's IWRAM allocator pointer and whether the
//         dispatch resumed a yield;
//   yield_resume  a yield restore whose resume pc lies inside the slot
//         (repeats of the same pc collapse into one entry with a count);
//   push/ret_match/ret_unwind/cancel  the generated call-return stack
//         (runtime_call_push_return / _should_return / _cancel_return) when
//         the return pc lies inside the slot. Tests whether a stale unpacker
//         return frame is resumed after another routine is copied into the
//         slot (only the address is compared by the return matcher).
// CPU stores have their own small ring so a decompressor writing over the slot
// cannot push the copies, entries and returns out (a player crash on
// 2026-10-03 lost them that way; FACTS.md). The dump merges both rings in
// order and adds the last 16 dispatch targets. Dumped by the runner's
// unknown-identity report and the bridge-abort path.
extern "C" bool g_frame_present_in_progress = false;  // set by the host bridge
extern "C" uint32_t g_irq_nest_depth;

namespace {
constexpr uint32_t kUnpackerSlotOff = 0x2000u;   // within the 32 KiB IWRAM
constexpr uint32_t kUnpackerSlotLen = 0x2C4u;
constexpr uint32_t kUnpackerEventDepth = 256u;  // everything but cpu stores
constexpr uint32_t kUnpackerCpuDepth = 32u;
constexpr uint32_t kRecentDispatchDepth = 16u;
// Golden Sun's IWRAM bump-allocator pointer ([0x03001E50] + 4, Func_4938):
// where the next copied routine lands.
constexpr uint32_t kGameIwramHeapPtr = 0x03001E54u;
constexpr uint32_t kUnpackerSlotWords = kUnpackerSlotLen / 4u;
enum UnpackerSlotKind : uint8_t {
    kUnpackerDma = 0, kUnpackerCpu = 1, kUnpackerEnter = 2,
    kUnpackerPush = 3, kUnpackerRetMatch = 4, kUnpackerRetUnwind = 5,
    kUnpackerCancel = 6, kUnpackerYieldResume = 7
};
struct UnpackerSlotEntry {
    unsigned long long frame;
    uint64_t seq;   // order across both rings
    uint32_t dst, src_or_width, count, pc, lr, irq_depth, first_word;
    uint32_t aux;   // call-return kinds: 1 = matched the top slot;
                    // yield_resume: repeat count
    uint32_t r0, r1, r6, sp, heap;   // enter only
    uint8_t kind, present, resumed;
};
UnpackerSlotEntry g_unpacker_events[kUnpackerEventDepth];
UnpackerSlotEntry g_unpacker_cpu[kUnpackerCpuDepth];
uint64_t g_unpacker_events_total = 0;
uint64_t g_unpacker_cpu_total = 0;
uint64_t g_unpacker_seq = 0;
// Last dispatch targets (pc | thumb bit), oldest at index next.
uint32_t g_recent_dispatch[kRecentDispatchDepth];
uint32_t g_recent_dispatch_next = 0;
// Set by runtime_yield_restore_pc() when it put a resume pc back; the next
// dispatch reads and clears it.
bool g_dispatch_is_yield_resume = false;
unsigned long long g_unpacker_word_frame[kUnpackerSlotWords];
bool g_unpacker_in_dma = false;

// True when [addr, addr+len) (IWRAM, mirror-normalized) overlaps the slot.
bool unpacker_slot_overlaps(uint32_t addr, uint32_t len) {
    if ((addr >> 24) != 0x03u || len == 0u) return false;
    if (len >= 0x8000u) return true;
    const uint32_t lo = addr & 0x7FFFu;
    const uint32_t hi = lo + len;   // may run past 0x8000: wrap check below
    if (lo < kUnpackerSlotOff + kUnpackerSlotLen && hi > kUnpackerSlotOff)
        return true;
    return hi > 0x8000u && (hi - 0x8000u) > kUnpackerSlotOff;
}

UnpackerSlotEntry& unpacker_journal_next(uint8_t kind) {
    UnpackerSlotEntry& e = kind == kUnpackerCpu
        ? g_unpacker_cpu[g_unpacker_cpu_total++ % kUnpackerCpuDepth]
        : g_unpacker_events[g_unpacker_events_total++ % kUnpackerEventDepth];
    e = {};
    e.seq = g_unpacker_seq++;
    e.frame = g_runtime_vblank_starts;
    e.kind = kind;
    e.pc = g_cpu.R[15];
    e.lr = g_cpu.R[14];
    e.irq_depth = g_irq_nest_depth;
    e.present = g_frame_present_in_progress ? 1u : 0u;
    return e;
}

// True when a (thumb-bit-stripped) return pc lies inside the slot. This is the
// only cost the call-return hooks pay when the pc is elsewhere.
inline bool unpacker_pc_in_slot(uint32_t pc) {
    return (pc - (0x03000000u + kUnpackerSlotOff)) < kUnpackerSlotLen;
}

// Call-return stack events. Callers gate on unpacker_pc_in_slot() (except the
// unwind case, which gates on the live stack contents). e.pc is R15 at the
// time; e.frame is the host vblank frame.
//   push:       dst=return pc, src_or_width=depth after push, count=floor
//   ret_match:  dst=target pc, src_or_width=matched slot, count=depth before,
//               aux=1 if it was the top entry, 0 for a deeper match
//   ret_unwind: dst=target pc, src_or_width=depth, count=floor
//   cancel:     dst=cancelled pc, src_or_width=matched slot or 0xFFFFFFFF,
//               count=depth before, aux=1 if top
void unpacker_note_callret(uint8_t kind, uint32_t pc, uint32_t a, uint32_t b,
                           uint32_t aux) {
    UnpackerSlotEntry& e = unpacker_journal_next(kind);
    e.dst = pc;
    e.src_or_width = a;
    e.count = b;
    e.aux = aux;
    e.first_word = bus_read_u32(0x03000000u + kUnpackerSlotOff);
}

// Defined next to the call-return stack (which is declared further down).
void unpacker_dump_live_ret();
}  // namespace

// CPU store hook (bus/interpreter stores unconditionally; generated stores via
// runtime_note_ram_code_write). Cheap: returns at once outside the slot.
extern "C" void runtime_unpacker_slot_note_cpu(uint32_t addr, uint32_t width) {
    if (g_unpacker_in_dma || !unpacker_slot_overlaps(addr, width)) return;
    uint32_t off = (addr & 0x7FFFu);
    if (off < kUnpackerSlotOff) off = kUnpackerSlotOff;
    const uint32_t word = (off - kUnpackerSlotOff) >> 2;
    if (word >= kUnpackerSlotWords) return;
    if (g_unpacker_word_frame[word] == g_runtime_vblank_starts + 1u) return;
    g_unpacker_word_frame[word] = g_runtime_vblank_starts + 1u;
    UnpackerSlotEntry& e = unpacker_journal_next(kUnpackerCpu);
    e.dst = addr;
    e.src_or_width = width;
}

// DMA hook. begin() returns nonzero when the transfer's destination range
// [dst_lo, dst_lo+bytes) overlaps the slot; only then call end() afterwards.
extern "C" int runtime_unpacker_slot_dma_begin(uint32_t dst_lo,
                                               uint32_t bytes) {
    if (!unpacker_slot_overlaps(dst_lo, bytes)) return 0;
    g_unpacker_in_dma = true;   // the per-word bus stores are one DMA entry
    return 1;
}

extern "C" void runtime_unpacker_slot_dma_end(uint32_t dst, uint32_t src,
                                              uint32_t bytes) {
    g_unpacker_in_dma = false;
    UnpackerSlotEntry& e = unpacker_journal_next(kUnpackerDma);
    e.dst = dst;
    e.src_or_width = src;
    e.count = bytes;
    e.first_word = bus_read_u32(0x03000000u + kUnpackerSlotOff);
}

extern "C" void runtime_unpacker_slot_note_dispatch(bool resumed) {
    UnpackerSlotEntry& e = unpacker_journal_next(kUnpackerEnter);
    e.dst = 0x03000000u + kUnpackerSlotOff;
    e.first_word = bus_read_u32(0x03000000u + kUnpackerSlotOff);
    e.r0 = g_cpu.R[0];
    e.r1 = g_cpu.R[1];
    e.r6 = g_cpu.R[6];
    e.sp = g_cpu.R[13];
    e.heap = bus_read_u32(kGameIwramHeapPtr);
    e.resumed = resumed ? 1u : 0u;
}

// A yield restore put `pc` back as the resume pc and it lies in the slot.
static void unpacker_note_yield_resume(uint32_t pc) {
    if (g_unpacker_events_total != 0u) {
        UnpackerSlotEntry& last =
            g_unpacker_events[(g_unpacker_events_total - 1u) % kUnpackerEventDepth];
        if (last.kind == kUnpackerYieldResume && last.dst == pc) {
            ++last.aux;
            last.frame = g_runtime_vblank_starts;
            return;
        }
    }
    UnpackerSlotEntry& e = unpacker_journal_next(kUnpackerYieldResume);
    e.dst = pc;
    e.aux = 1u;
    e.first_word = bus_read_u32(0x03000000u + kUnpackerSlotOff);
}

extern "C" void runtime_unpacker_slot_dump(const char* why) {
    const uint64_t events = g_unpacker_events_total;
    const uint64_t cpus = g_unpacker_cpu_total;
    const uint64_t events_shown =
        events < kUnpackerEventDepth ? events : kUnpackerEventDepth;
    const uint64_t cpus_shown = cpus < kUnpackerCpuDepth ? cpus : kUnpackerCpuDepth;
    std::fprintf(stderr,
        "[unpacker-slot] reason=%s frame=%llu events=%llu of %llu, "
        "cpu stores=%llu of %llu (oldest first; pc/lr are g_cpu at the time "
        "and may lag generated code)\n",
        why ? why : "?", static_cast<unsigned long long>(g_runtime_vblank_starts),
        static_cast<unsigned long long>(events_shown),
        static_cast<unsigned long long>(events),
        static_cast<unsigned long long>(cpus_shown),
        static_cast<unsigned long long>(cpus));
    std::vector<const UnpackerSlotEntry*> merged;
    merged.reserve(static_cast<std::size_t>(events_shown + cpus_shown));
    for (uint64_t i = events - events_shown; i < events; ++i)
        merged.push_back(&g_unpacker_events[i % kUnpackerEventDepth]);
    for (uint64_t i = cpus - cpus_shown; i < cpus; ++i)
        merged.push_back(&g_unpacker_cpu[i % kUnpackerCpuDepth]);
    std::sort(merged.begin(), merged.end(),
              [](const UnpackerSlotEntry* a, const UnpackerSlotEntry* b) {
                  return a->seq < b->seq;
              });
    for (const UnpackerSlotEntry* entry : merged) {
        const UnpackerSlotEntry& e = *entry;
        const char* kind = e.kind == kUnpackerDma ? "dma"
                         : e.kind == kUnpackerCpu ? "cpu"
                         : e.kind == kUnpackerPush ? "push"
                         : e.kind == kUnpackerRetMatch ? "ret_match"
                         : e.kind == kUnpackerRetUnwind ? "ret_unwind"
                         : e.kind == kUnpackerCancel ? "cancel"
                         : e.kind == kUnpackerYieldResume ? "yield_resume"
                         : "enter";
        if (e.kind == kUnpackerDma) {
            std::fprintf(stderr,
                "[unpacker-slot] frame=%llu kind=%s dst=0x%08X src=0x%08X "
                "bytes=%u first_word=0x%08X pc=0x%08X lr=0x%08X irq_depth=%u "
                "present=%u\n",
                e.frame, kind, e.dst, e.src_or_width, e.count, e.first_word,
                e.pc, e.lr, e.irq_depth, e.present);
        } else if (e.kind == kUnpackerCpu) {
            std::fprintf(stderr,
                "[unpacker-slot] frame=%llu kind=%s dst=0x%08X width=%u "
                "pc=0x%08X lr=0x%08X irq_depth=%u present=%u\n",
                e.frame, kind, e.dst, e.src_or_width, e.pc, e.lr,
                e.irq_depth, e.present);
        } else if (e.kind == kUnpackerPush) {
            std::fprintf(stderr,
                "[unpacker-slot] frame=%llu kind=%s ret=0x%08X depth=%u "
                "floor=%u r15=0x%08X first_word=0x%08X irq_depth=%u "
                "present=%u\n",
                e.frame, kind, e.dst, e.src_or_width, e.count, e.pc,
                e.first_word, e.irq_depth, e.present);
        } else if (e.kind == kUnpackerRetMatch) {
            std::fprintf(stderr,
                "[unpacker-slot] frame=%llu kind=%s target=0x%08X slot=%u "
                "depth=%u %s r15=0x%08X first_word=0x%08X irq_depth=%u "
                "present=%u\n",
                e.frame, kind, e.dst, e.src_or_width, e.count,
                e.aux ? "top" : "deeper", e.pc, e.first_word, e.irq_depth,
                e.present);
        } else if (e.kind == kUnpackerRetUnwind) {
            std::fprintf(stderr,
                "[unpacker-slot] frame=%llu kind=%s target=0x%08X depth=%u "
                "floor=%u r15=0x%08X first_word=0x%08X irq_depth=%u "
                "present=%u\n",
                e.frame, kind, e.dst, e.src_or_width, e.count, e.pc,
                e.first_word, e.irq_depth, e.present);
        } else if (e.kind == kUnpackerCancel) {
            if (e.src_or_width == 0xFFFFFFFFu)
                std::fprintf(stderr,
                    "[unpacker-slot] frame=%llu kind=%s pc=0x%08X slot=none "
                    "depth=%u r15=0x%08X first_word=0x%08X irq_depth=%u "
                    "present=%u\n",
                    e.frame, kind, e.dst, e.count, e.pc, e.first_word,
                    e.irq_depth, e.present);
            else
                std::fprintf(stderr,
                    "[unpacker-slot] frame=%llu kind=%s pc=0x%08X slot=%u "
                    "depth=%u %s r15=0x%08X first_word=0x%08X irq_depth=%u "
                    "present=%u\n",
                    e.frame, kind, e.dst, e.src_or_width, e.count,
                    e.aux ? "top" : "deeper", e.pc, e.first_word,
                    e.irq_depth, e.present);
        } else if (e.kind == kUnpackerYieldResume) {
            std::fprintf(stderr,
                "[unpacker-slot] frame=%llu kind=%s pc=0x%08X times=%u "
                "first_word=0x%08X irq_depth=%u present=%u\n",
                e.frame, kind, e.dst, e.aux, e.first_word, e.irq_depth,
                e.present);
        } else {
            std::fprintf(stderr,
                "[unpacker-slot] frame=%llu kind=%s dst=0x%08X "
                "first_word=0x%08X lr=0x%08X pc=0x%08X r0=0x%08X r1=0x%08X "
                "r6=0x%08X sp=0x%08X iwram_heap=0x%08X yield_resume=%u "
                "irq_depth=%u present=%u\n",
                e.frame, kind, e.dst, e.first_word, e.lr, e.pc, e.r0, e.r1,
                e.r6, e.sp, e.heap, e.resumed, e.irq_depth, e.present);
        }
    }
    std::fprintf(stderr, "[unpacker-slot] last dispatches (oldest first):");
    for (uint32_t i = 0; i < kRecentDispatchDepth; ++i) {
        const uint32_t target =
            g_recent_dispatch[(g_recent_dispatch_next + i) % kRecentDispatchDepth];
        if (target != 0u) std::fprintf(stderr, " 0x%08X", target);
    }
    std::fprintf(stderr, "\n");
    unpacker_dump_live_ret();
}

extern "C" void runtime_iwram_code_write_dump(const char* why) {
    std::fprintf(stderr,
        "runtime_arm: [iwram-code-writes] reason=%s frame=%llu "
        "(newest first; one write per word per frame; pc/lr are g_cpu at "
        "the time and may lag generated code)\n",
        why ? why : "?", static_cast<unsigned long long>(g_runtime_vblank_starts));
    for (uint32_t i = 0; i < 96u; ++i) {
        const IwramCodeWrite& e =
            g_iwram_code_ring[(g_iwram_code_ring_next - 1u - i) %
                              kIwramCodeRing];
        if (e.width == 0u) break;
        std::fprintf(stderr,
            "  frame=%llu addr=0x%08X width=%u pc=0x%08X lr=0x%08X\n",
            e.frame, e.addr, e.width, e.pc, e.lr);
    }
}

// Same ring as above, filtered to [lo, hi): scans the whole ring (not just the
// newest 96) so writes into one region are not hidden behind per-frame data
// writes elsewhere. Prints at most 64 matches, newest first, then the count.
extern "C" void runtime_iwram_code_write_dump_range(const char* why,
                                                    uint32_t lo, uint32_t hi) {
    std::fprintf(stderr,
        "runtime_arm: [iwram-code-writes-range] reason=%s range=0x%08X..0x%08X "
        "frame=%llu (newest first, max 64 shown; scans all %u ring entries)\n",
        why ? why : "?", lo, hi,
        static_cast<unsigned long long>(g_runtime_vblank_starts),
        kIwramCodeRing);
    uint32_t matched = 0;
    for (uint32_t i = 0; i < kIwramCodeRing; ++i) {
        const IwramCodeWrite& e =
            g_iwram_code_ring[(g_iwram_code_ring_next - 1u - i) %
                              kIwramCodeRing];
        if (e.width == 0u) break;
        if (e.addr < lo || e.addr >= hi) continue;
        if (matched < 64u) {
            std::fprintf(stderr,
                "  frame=%llu addr=0x%08X width=%u pc=0x%08X lr=0x%08X\n",
                e.frame, e.addr, e.width, e.pc, e.lr);
        }
        ++matched;
    }
    std::fprintf(stderr, "  matched=%u\n", matched);
}

extern "C" void runtime_note_ram_code_write(uint32_t addr, uint32_t width) {
    if (width == 0u) return;
    const uint32_t region = addr >> 24;
    if (region == 0x03u) runtime_unpacker_slot_note_cpu(addr, width);
    if (region == 0x03u && (addr & 0x7FFFu) < kIwramCodeDiagEnd) {
        const uint32_t word = (addr & 0x7FFFu) >> 2;
        if (g_iwram_code_word_frame[word] != g_runtime_vblank_starts + 1u) {
            g_iwram_code_word_frame[word] = g_runtime_vblank_starts + 1u;
            IwramCodeWrite& e =
                g_iwram_code_ring[g_iwram_code_ring_next++ % kIwramCodeRing];
            e = {g_runtime_vblank_starts, addr, width, g_cpu.R[15],
                 g_cpu.R[14]};
        }
    }
    uint64_t* bits = nullptr;
    uint32_t mask = 0;
    if (region == 0x03u) { bits = g_ram_dirty_iwram; mask = 0x7FFFu; }
    else if (region == 0x02u) { bits = g_ram_dirty_ewram; mask = 0x3FFFFu; }
    else return;
    const uint32_t first = (addr & mask) >> 2;
    const uint32_t last = ((addr & mask) + width - 1u) >> 2;
    for (uint32_t word = first; word <= last; ++word)
        bits[word >> 6] |= 1ull << (word & 63u);
}

extern "C" int runtime_ram_code_range_dirty(uint32_t start, uint32_t end) {
    if (end <= start || (start >> 24) != ((end - 1u) >> 24)) return 0;
    const uint32_t region = start >> 24;
    const uint64_t* bits = nullptr;
    uint32_t mask = 0;
    if (region == 0x03u) { bits = g_ram_dirty_iwram; mask = 0x7FFFu; }
    else if (region == 0x02u) { bits = g_ram_dirty_ewram; mask = 0x3FFFFu; }
    else return 0;
    const uint32_t first = (start & mask) >> 2;
    const uint32_t last = ((end - 1u) & mask) >> 2;
    for (uint32_t word = first; word <= last; ++word) {
        if (bits[word >> 6] & (1ull << (word & 63u))) return 1;
    }
    return 0;
}

extern "C" void runtime_ram_code_dirty_reset(void) {
    for (uint64_t& word : g_ram_dirty_ewram) word = 0;
    for (uint64_t& word : g_ram_dirty_iwram) word = 0;
}

// VBlank-start counter (defined in src/runtime/runtime_bus_bridge.cpp).
// Used to frame-gate the mem-write watchpoint.
extern "C" unsigned long long g_runtime_vblank_starts;

namespace {
// "Additional debug logging" toggle — see runtime_arm.h. Relaxed atomic,
// same rationale as g_overclock_factor in runtime_bus_bridge.cpp: read from
// runtime_trace_event's hot path, written live from the config UI.
std::atomic<int> g_additional_debug_logging{0};
// Generic factor for the optional game-owned memory-write transform seam.
// The callback itself remains owned and installed by the game runner.
// The factor is in halves (2 = 1x, 3 = 1.5x, 4 = 2x, 6 = 3x).
std::atomic<int> g_mem_write_override_factor{2};

// Golden Sun party layout, confirmed for the USA/EU image: four slots at
// 0x02000534, 0x14C bytes apart. State is host-controlled and intentionally
// off by default; the VBlank hook applies it only when enabled.
constexpr unsigned kInfiniteHp = 1u << 0;
constexpr unsigned kInfinitePp = 1u << 1;
std::atomic<unsigned> g_infinite_cheats{0};

}  // namespace

extern "C" void gsr_set_additional_debug_logging(int on) {
    g_additional_debug_logging.store(on ? 1 : 0, std::memory_order_relaxed);
}

extern "C" int gsr_additional_debug_logging(void) {
    return g_additional_debug_logging.load(std::memory_order_relaxed);
}

extern "C" void runtime_set_infinite_hp(int on) {
    unsigned old = g_infinite_cheats.load(std::memory_order_relaxed);
    for (;;) {
        unsigned next = on ? (old | kInfiniteHp) : (old & ~kInfiniteHp);
        if (g_infinite_cheats.compare_exchange_weak(
                old, next, std::memory_order_relaxed,
                std::memory_order_relaxed))
            return;
    }
}

extern "C" void runtime_set_infinite_pp(int on) {
    unsigned old = g_infinite_cheats.load(std::memory_order_relaxed);
    for (;;) {
        unsigned next = on ? (old | kInfinitePp) : (old & ~kInfinitePp);
        if (g_infinite_cheats.compare_exchange_weak(
                old, next, std::memory_order_relaxed,
                std::memory_order_relaxed))
            return;
    }
}

extern "C" int runtime_get_infinite_hp(void) {
    return (g_infinite_cheats.load(std::memory_order_relaxed) & kInfiniteHp)
        ? 1 : 0;
}

extern "C" int runtime_get_infinite_pp(void) {
    return (g_infinite_cheats.load(std::memory_order_relaxed) & kInfinitePp)
        ? 1 : 0;
}

extern "C" void runtime_apply_infinite_hp_pp(void) {
    const unsigned cheats = g_infinite_cheats.load(std::memory_order_relaxed);
    if (!cheats) return;

    constexpr uint32_t kPartyBase = 0x02000534u;
    constexpr uint32_t kPartyStride = 0x14Cu;
    for (unsigned slot = 0; slot < 4u; ++slot) {
        const uint32_t base = kPartyBase + slot * kPartyStride;
        if (cheats & kInfiniteHp) {
            const uint16_t max_hp = bus_read_u16_slow(base + 0u);
            if (max_hp != 0u) {
                const uint16_t cur_hp = bus_read_u16_slow(base + 4u);
                if (cur_hp != max_hp)
                    bus_write_u16_slow(base + 4u, max_hp);
            }
        }
        if (cheats & kInfinitePp) {
            const uint16_t max_pp = bus_read_u16_slow(base + 2u);
            if (max_pp != 0u) {
                const uint16_t cur_pp = bus_read_u16_slow(base + 6u);
                if (cur_pp != max_pp)
                    bus_write_u16_slow(base + 6u, max_pp);
            }
        }
    }
}

extern "C" void runtime_set_mem_write_override_enabled(int factor) {
    if (factor != 2 && factor != 3 && factor != 4 && factor != 6) factor = 2;
    g_mem_write_override_factor.store(factor, std::memory_order_relaxed);
}

extern "C" int runtime_get_mem_write_override_enabled(void) {
    return g_mem_write_override_factor.load(std::memory_order_relaxed);
}

// View-mode requests from game code (an in-game settings page). The host
// window takes a request on its next pump and applies it the way its own
// menu does; it publishes the mode in effect after every pump.
static std::atomic<int> g_view_mode_request{-1};
static std::atomic<int> g_view_mode_current{0};

extern "C" void runtime_request_view_mode(int mode) {
    g_view_mode_request.store(mode, std::memory_order_relaxed);
}

extern "C" int runtime_take_view_mode_request(void) {
    return g_view_mode_request.exchange(-1, std::memory_order_relaxed);
}

extern "C" void runtime_publish_view_mode(int mode) {
    g_view_mode_current.store(mode, std::memory_order_relaxed);
}

extern "C" int runtime_get_view_mode(void) {
    return g_view_mode_current.load(std::memory_order_relaxed);
}

// No Slowdown (Enhanced Timing + CPU Overclock together), requested and
// reported the same way as the view mode.
static std::atomic<int> g_no_slowdown_request{-1};
static std::atomic<int> g_no_slowdown_current{0};

extern "C" void runtime_request_no_slowdown(int on) {
    g_no_slowdown_request.store(on ? 1 : 0, std::memory_order_relaxed);
}

extern "C" int runtime_take_no_slowdown_request(void) {
    return g_no_slowdown_request.exchange(-1, std::memory_order_relaxed);
}

extern "C" void runtime_publish_no_slowdown(int on) {
    g_no_slowdown_current.store(on ? 1 : 0, std::memory_order_relaxed);
}

extern "C" int runtime_get_no_slowdown(void) {
    return g_no_slowdown_current.load(std::memory_order_relaxed);
}

namespace {

constexpr uint32_t kTraceSize = 4096u;
RuntimeTraceEntry g_trace[kTraceSize] = {};
uint32_t g_trace_write = 0;
uint32_t g_trace_count = 0;
uint32_t g_trace_seq = 0;

constexpr uint32_t kCallReturnStackSize = 1024u;
uint32_t g_call_return_stack[kCallReturnStackSize] = {};
uint32_t g_call_return_depth = 0;
// Barrier into the GLOBAL guest call-return stack. An IRQ is dispatched on top
// of whatever mainline (or enclosing-IRQ) call frames are live, but the handler's
// guest returns/cancels must NOT match or pop those interrupted frames — doing so
// makes a handler return-cancel unwind into the mainline, skipping the rest of
// the handler (e.g. LeafGreen's VBlankIntr tail `gMain.intrCheck |= VBLANK` is
// never reached, so WaitForVBlank spins forever). runtime_irq() raises this floor
// to the live depth for the duration of the handler; should_return/cancel_return
// never look below it. Saved/restored across nested IRQs by runtime_irq().
uint32_t g_call_return_floor = 0;

// Dump-time view of the call-return stack for the unpacker-slot journal:
// every live entry whose value lies inside the slot, plus depth and floor.
void unpacker_dump_live_ret() {
    std::fprintf(stderr,
        "[unpacker-slot] live_ret depth=%u floor=%u (entries inside "
        "0x%08X..0x%08X follow)\n",
        g_call_return_depth, g_call_return_floor,
        0x03000000u + kUnpackerSlotOff,
        0x03000000u + kUnpackerSlotOff + kUnpackerSlotLen);
    for (uint32_t i = 0; i < g_call_return_depth; ++i) {
        if (!unpacker_pc_in_slot(g_call_return_stack[i])) continue;
        std::fprintf(stderr,
            "[unpacker-slot] live_ret index=%u value=0x%08X%s\n", i,
            g_call_return_stack[i],
            i < g_call_return_floor ? " (below floor)" : "");
    }
}

const char* trace_kind_name(uint32_t kind) {
    switch (kind) {
        case RUNTIME_TRACE_DISPATCH:  return "dispatch";
        case RUNTIME_TRACE_EXCHANGE:  return "exchange";
        case RUNTIME_TRACE_SWI:       return "swi";
        case RUNTIME_TRACE_MEM_WRITE: return "mem_w";
        case RUNTIME_TRACE_BRANCH:    return "branch";
        case RUNTIME_TRACE_IRQ:       return "irq";
        case RUNTIME_TRACE_CALL:      return "call";
        case RUNTIME_TRACE_MEM_READ:  return "mem_r";
        default:                      return "unknown";
    }
}

}  // namespace

extern "C" void runtime_trace_event(uint32_t kind, uint32_t pc,
                                     uint32_t addr, uint32_t value,
                                     uint32_t aux) {
    // A game-owned callback may transform this one immediately following
    // memory write.  The callback is optional and null by default, so the
    // canonical runtime has no game-specific policy here.
    g_runtime_mem_write_override_pending_valid = 0u;
    if (kind == RUNTIME_TRACE_MEM_WRITE && g_runtime_mem_write_observer)
        g_runtime_mem_write_observer(pc, addr, value, aux);
    // The factor selects the player-speed multiplier only; a factor of 1 is
    // the identity transform, and the runtime records nothing unless the
    // callback actually changes the value. Gating the call on factor > 1
    // therefore did not keep the run faithful, it just made the hook
    // unreachable for every other game-owned policy. Each policy gates
    // itself; the hook remains null by default.
    if (kind == RUNTIME_TRACE_MEM_WRITE &&
        g_runtime_mem_write_override) {
        uint32_t transformed = value;
        if (g_runtime_mem_write_override(
                pc, addr, value, aux, &transformed) != 0 &&
            aux == 4u && transformed != value) {
            g_runtime_mem_write_override_pending_addr = addr;
            g_runtime_mem_write_override_pending_value = transformed;
            g_runtime_mem_write_override_pending_valid = 1u;
        }
    }

    if (g_runtime_mp2k_control_hook && kind == RUNTIME_TRACE_DISPATCH) {
        const bool cost_probe = g_cost_probe_enabled();
        const bool relevant = !runtime_mp2k_control_relevant ||
            runtime_mp2k_control_relevant(pc);
        if (cost_probe) {
            ++g_cost_mp2k_hook_checks;
            if (relevant) ++g_cost_mp2k_hook_matches;
        }
        if (relevant) {
            if (cost_probe) ++g_cost_mp2k_hook_deep_calls;
            g_runtime_mp2k_control_hook(
                pc, addr, g_runtime_cycles,
                (g_cpu.cpsr & CPSR_T_BIT) != 0 ? 1u : 0u);
        }
    }
    // Generated guest stores are the only reliable common observation point
    // for the Camelot MP2K copy. Keep this enhancement hook gated by the
    // explicit native/shadow request; faithful runs remain untouched.
    static int mp2k_watch = -1;
    if (mp2k_watch < 0) {
        const char* shadow = std::getenv("GBARECOMP_AUDIO_SHADOW");
        const bool requested =
            gbarecomp::env_flag("GBARECOMP_AUDIO_NATIVE") ||
            (shadow && shadow[0] && !(shadow[0] == '0' && shadow[1] == '\0'));
        mp2k_watch = requested ? 1 : 0;
    }
    if (mp2k_watch && g_runtime_mp2k_write_hook &&
        kind == RUNTIME_TRACE_MEM_WRITE) {
        const bool cost_probe = g_cost_probe_enabled();
        if (cost_probe) ++g_cost_mp2k_trace_stores;
        const auto filter_start = cost_probe
            ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
        const bool relevant = !runtime_mp2k_write_relevant ||
            runtime_mp2k_write_relevant(pc, addr, aux);
        if (cost_probe) {
            g_cost_mp2k_fast_filter_ns += static_cast<unsigned long long>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - filter_start).count());
            if (relevant) ++g_cost_mp2k_fast_accepts;
            switch (pc & ~1u) {
                case 0x030008B4u:
                case 0x03000A8Cu:
                case 0x03000BCCu:
                case 0x03000BD4u:
                case 0x03000C70u:
                    ++g_cost_mp2k_exact_writer_matches;
                    break;
                default:
                    break;
            }
        }
        if (!relevant) {
            // Other trace observers below still run for this store.
        } else {
            uint32_t before = 0;
            if ((addr >> 24) == 0x02u || (addr >> 24) == 0x03u) {
                before = aux == 1u ? bus_read_u8(addr) :
                    (aux == 2u ? bus_read_u16(addr) : bus_read_u32(addr));
            }
            if (cost_probe) ++g_cost_mp2k_hook_pre;
            g_runtime_mp2k_write_hook(pc, addr, value, aux,
                                      g_cpu.R[6], g_cpu.R[11], before,
                                      (g_cpu.cpsr & CPSR_T_BIT) != 0, 1);
            // Preserve the established canonical post observer for generated
            // AOT stores; the pre call above is bounded identity evidence only.
            if (cost_probe) ++g_cost_mp2k_hook_post;
            g_runtime_mp2k_write_hook(pc, addr, value, aux,
                                      g_cpu.R[6], g_cpu.R[11], before,
                                      (g_cpu.cpsr & CPSR_T_BIT) != 0, 0);
        }
    }

    // Opt-in pointer attribution runs before the general trace gate, so the
    // RAM-churn sidecar works with GSR_TRACE off. It records only a target
    // pointer inside the measured generated image; no stored guest word is
    // retained or emitted.
    if (g_runtime_ram_pointer_write_probe &&
        kind == RUNTIME_TRACE_MEM_WRITE && aux == 4u) {
        runtime_note_ram_pointer_write(addr, value, aux);
    }
    // Generated stores are already observed here, before their fast-path
    // write.  The common GbaBus hook covers interpreter/direct stores; the
    // bridge suppresses that hook around generated slow-path calls so this
    // remains one lifecycle event per store.
    if (g_runtime_ram_image_write_probe && kind == RUNTIME_TRACE_MEM_WRITE) {
        runtime_note_ram_image_cpu_write(pc, addr, aux);
    }
    if (g_pool_ldm_probe_armed && kind == RUNTIME_TRACE_MEM_WRITE) {
        pool_ldm_note_write(addr, value, aux, pc, g_cpu.R[13], g_cpu.R[9],
                            g_runtime_vblank_starts, "generated", true);
    }
    // STORE WATCHPOINT. GBARECOMP_STORE_WATCH=<lo>[:<hi>] reports every guest
    // write into that address range with the PC responsible.
    //
    // The DMA watch answers "which bulk copy touched this address"; it cannot
    // see ordinary stores, so an address that a copy fills and an unrelated
    // loop later overwrites looks untouched. The abort ring dump only carries
    // the last 160 events, which is far too short to reach backwards to a
    // corruption that happened many frames earlier.
    {
        static int watch_init = 0;
        static uint32_t watch_lo = 0, watch_hi = 0;
        if (!watch_init) {
            watch_init = 1;
            if (const char* w = std::getenv("GBARECOMP_STORE_WATCH")) {
                char* end = nullptr;
                watch_lo = static_cast<uint32_t>(std::strtoul(w, &end, 0));
                watch_hi = watch_lo + 4u;
                if (end && *end == ':') {
                    watch_hi = static_cast<uint32_t>(
                        std::strtoul(end + 1, nullptr, 0));
                }
            }
        }
        if (watch_hi > watch_lo && kind == RUNTIME_TRACE_MEM_WRITE &&
            addr >= watch_lo && addr < watch_hi) {
            std::fprintf(stderr,
                         "[store-watch] pc=0x%08X addr=0x%08X value=0x%08X "
                         "size=%u cycles=%llu r0=0x%08X r1=0x%08X "
                         "r2=0x%08X r3=0x%08X r4=0x%08X r5=0x%08X "
                         "r6=0x%08X r7=0x%08X r8=0x%08X r9=0x%08X "
                         "r10=0x%08X r11=0x%08X\n",
                         pc, addr, value, aux,
                         static_cast<unsigned long long>(g_runtime_cycles),
                         g_cpu.R[0], g_cpu.R[1], g_cpu.R[2], g_cpu.R[3],
                         g_cpu.R[4], g_cpu.R[5], g_cpu.R[6], g_cpu.R[7],
                         g_cpu.R[8], g_cpu.R[9], g_cpu.R[10], g_cpu.R[11]);
        }
    }
    // PERF MEASUREMENT GATE. The ring is written on every store, branch and
    // dispatch, so it is the first thing to measure when guest throughput is
    // the problem. Turning it off ALSO disables every abort watchpoint and
    // the on-abort ring dump that this project's attribution work depends
    // on — recording only, never a change to guest-observable execution
    // (nothing below this point writes g_cpu, the bus, or g_runtime_cycles;
    // every abort site is a std::abort() diagnostic crash, itself gated by
    // its own separate GBARECOMP_ABORT_ON_* env var).
    //
    // GBARECOMP_TRACE, if explicitly set, wins outright (existing scripts
    // and repro recipes keep working unchanged) and is fixed for the
    // process. With no env var, the config UI's "Additional debug logging"
    // toggle (Logging tab, default OFF) decides live — a relaxed atomic
    // load, as cheap as the old cached-static read, so flipping the
    // checkbox takes effect on the very next call from this hot path with
    // no restart.
    static int trace_env_forced = -2;  // -2 unread, -1 unset, 0/1 forced off/on
    if (trace_env_forced == -2) {
        const char* env = std::getenv("GBARECOMP_TRACE");
        trace_env_forced = env ? (env[0] == '0' ? 0 : 1) : -1;
    }
    const bool trace_enabled = (trace_env_forced >= 0)
        ? (trace_env_forced != 0)
        : (gsr_additional_debug_logging() != 0);
    if (!trace_enabled) return;

    RuntimeTraceEntry& e = g_trace[g_trace_write];
    e.seq = ++g_trace_seq;
    e.cycles = g_runtime_cycles;
    e.kind = kind;
    e.pc = pc;
    e.cpsr = g_cpu.cpsr;
    e.addr = addr;
    e.value = value;
    e.aux = aux;
    e.r0 = g_cpu.R[0];
    e.r1 = g_cpu.R[1];
    e.r2 = g_cpu.R[2];
    e.r3 = g_cpu.R[3];
    e.r4 = g_cpu.R[4];
    e.r5 = g_cpu.R[5];
    e.r12 = g_cpu.R[12];
    e.r13 = g_cpu.R[13];
    e.r14 = g_cpu.R[14];

    g_trace_write = (g_trace_write + 1u) % kTraceSize;
    if (g_trace_count < kTraceSize) ++g_trace_count;

    static int abort_on_bios_write = -1;
    if (abort_on_bios_write < 0) {
        abort_on_bios_write =
            std::getenv("GBARECOMP_ABORT_ON_BIOS_WRITE") ? 1 : 0;
    }
    if (abort_on_bios_write &&
        kind == RUNTIME_TRACE_MEM_WRITE &&
        addr < 0x00004000u) {
        std::fprintf(stderr,
                     "runtime_trace: generated BIOS-region write "
                     "pc=0x%08X addr=0x%08X value=0x%08X width=%u\n",
                     pc, addr, value, aux);
        runtime_trace_dump_recent(96);
        std::abort();
    }

    static uint32_t abort_mem_addr = 0xFFFFFFFFu;
    if (abort_mem_addr == 0xFFFFFFFFu) {
        const char* env = std::getenv("GBARECOMP_ABORT_ON_MEM_WRITE_ADDR");
        abort_mem_addr = env ? static_cast<uint32_t>(std::strtoul(env, nullptr, 0))
                             : 0xFFFFFFFEu;
    }
    // How many trailing trace events the abort handlers dump. Default 160;
    // raise (up to the ring size) to capture the full call chain back to a
    // main loop. GBARECOMP_TRACE_DUMP_DEPTH=4000.
    static uint32_t abort_dump_depth = 0u;
    if (abort_dump_depth == 0u) {
        const char* env = std::getenv("GBARECOMP_TRACE_DUMP_DEPTH");
        abort_dump_depth = env ? static_cast<uint32_t>(std::strtoul(env, nullptr, 0))
                               : 160u;
        if (abort_dump_depth == 0u) abort_dump_depth = 160u;
    }
    // Optional frame gate: suppress the mem-write abort until this many
    // VBlank-starts have elapsed. Lets a watchpoint skip the identical
    // early frames and fire on the write in the frame where the value
    // actually diverges (e.g. MC-HP-002's frame-40 onset). 0 = no gate.
    static uint64_t abort_min_vblank = 0xFFFFFFFFFFFFFFFFull;
    if (abort_min_vblank == 0xFFFFFFFFFFFFFFFFull) {
        const char* env = std::getenv("GBARECOMP_ABORT_ON_MEM_WRITE_MIN_FRAME");
        abort_min_vblank = env ? std::strtoull(env, nullptr, 0) : 0ull;
    }
    // Optional value gate: only abort when the written value matches. Lets a
    // watchpoint skip a correct write to an address and fire on the specific
    // erroneous value (e.g. MC-HP-002: a per-frame countdown is written 0x0C
    // first — correct — then 0x0B by the extra tick; watch value=0x0B to land
    // on the duplicate write, not the legitimate one). -1 = match any value.
    static long long abort_mem_value = -2;
    if (abort_mem_value == -2) {
        const char* env = std::getenv("GBARECOMP_ABORT_ON_MEM_WRITE_VALUE");
        abort_mem_value = env ? static_cast<long long>(std::strtoull(env, nullptr, 0))
                              : -1;
    }
    if (kind == RUNTIME_TRACE_MEM_WRITE && addr == abort_mem_addr &&
        g_runtime_vblank_starts >= abort_min_vblank &&
        (abort_mem_value < 0 || value == static_cast<uint32_t>(abort_mem_value))) {
        std::fprintf(stderr,
                     "runtime_trace: mem-write-addr abort pc=0x%08X "
                     "addr=0x%08X value=0x%08X width=%u (vblanks=%llu)\n",
                     pc, addr, value, aux,
                     static_cast<unsigned long long>(g_runtime_vblank_starts));
        runtime_trace_dump_recent(abort_dump_depth);
        std::abort();
    }

    static int abort_on_high_mem_read = -1;
    if (abort_on_high_mem_read < 0) {
        abort_on_high_mem_read =
            std::getenv("GBARECOMP_ABORT_ON_MEM_READ_HIGH") ? 1 : 0;
    }
    if (abort_on_high_mem_read &&
        kind == RUNTIME_TRACE_MEM_READ &&
        (addr >> 24) >= 0x0Eu) {
        std::fprintf(stderr,
                     "runtime_trace: high mem-read abort pc=0x%08X "
                     "addr=0x%08X value=0x%08X width=%u\n",
                     pc, addr, value, aux);
        runtime_trace_dump_recent(160);
        std::abort();
    }

    static int branch_abort_after = -2;
    if (branch_abort_after == -2) {
        const char* env = std::getenv("GBARECOMP_ABORT_AFTER_BRANCHES");
        branch_abort_after = env ? std::atoi(env) : -1;
    }
    if (kind == RUNTIME_TRACE_BRANCH && branch_abort_after >= 0) {
        if (branch_abort_after == 0) {
            std::fprintf(stderr,
                         "runtime_trace: branch abort at pc=0x%08X "
                         "target=0x%08X\n",
                         pc, addr);
            runtime_trace_dump_recent(96);
            std::abort();
        }
        --branch_abort_after;
    }

    static uint32_t abort_branch_pc = 0xFFFFFFFFu;
    if (abort_branch_pc == 0xFFFFFFFFu) {
        const char* env = std::getenv("GBARECOMP_ABORT_ON_BRANCH_PC");
        abort_branch_pc = env ? static_cast<uint32_t>(std::strtoul(env, nullptr, 0))
                              : 0xFFFFFFFEu;
    }
    if (kind == RUNTIME_TRACE_BRANCH && pc == abort_branch_pc) {
        std::fprintf(stderr,
                     "runtime_trace: branch-pc abort at pc=0x%08X "
                     "target=0x%08X\n",
                     pc, addr);
        runtime_trace_dump_recent(160);
        std::abort();
    }
}

// Forward-declared: defined near lookup_in/the dispatch tables further down
// this file (the "Function coverage" block), which it needs to size the
// per-table coverage bit arrays. Installs the fn-entry-hook handler (a no-op
// no-arm) iff GBARECOMP_FUNC_COVERAGE is set. Idempotent — safe to call on
// every reset.
namespace { void coverage_arm_from_env(); }

extern "C" void runtime_trace_reset(void) {
    g_trace_write = 0;
    g_trace_count = 0;
    g_trace_seq = 0;
    g_runtime_cycles = 0;  // cycle clock shares the machine-reset lifecycle
    // Arm per-instruction fingerprinting for the whole run if requested, so the
    // ring is always-on from reset (no arm-then-run latency gap) and we query it
    // after the fact. Also settable via the TCP `insn_trace` command.
    // A BIOS-PC inventory uses the same generated prologue seam but keeps a
    // compact mode/address set instead of allocating the large fingerprint
    // ring. Its nonzero value still arms the generated call site; runtime_fp
    // distinguishes the lightweight mode below.
    g_bios_pc_log_armed = gbarecomp::env_flag("GBARECOMP_BIOS_PC_LOG");
    if (g_bios_pc_log_armed) {
        g_bios_pc_seen.assign(static_cast<std::size_t>(kBiosPcRegionEnd), 0u);
        g_bios_pc_samples = 0;
    } else {
        g_bios_pc_seen.clear();
        g_bios_pc_samples = 0;
    }
    g_bios_pc_handed_off = false;
    g_insn_trace_from_env = gbarecomp::env_flag("GBARECOMP_INSN_TRACE");
    g_runtime_insn_trace = g_bios_pc_log_armed
        ? 2u : ((g_insn_trace_from_env || g_crash_log) ? 1u : 0u);
    runtime_fp_reset();

    // Function coverage (see the "Function coverage" block, defined further
    // down this file next to lookup_in/the dispatch tables it addresses).
    // Armed once, at machine reset, from GBARECOMP_FUNC_COVERAGE — before any
    // guest code runs, so nothing before this point can be missed. Forward-
    // declared just below since its definition needs lookup_in/kDispatchTable,
    // which come later in the file.
    coverage_arm_from_env();
}

extern "C" uint32_t runtime_trace_total(void) { return g_trace_seq; }

extern "C" void runtime_trace_dump_recent(uint32_t max_entries) {
    if (max_entries > g_trace_count) max_entries = g_trace_count;
    std::fprintf(stderr, "runtime_trace: last %u event(s)\n", max_entries);
    uint32_t start = (g_trace_write + kTraceSize - max_entries) % kTraceSize;
    for (uint32_t i = 0; i < max_entries; ++i) {
        const RuntimeTraceEntry& e = g_trace[(start + i) % kTraceSize];
        // Annotate the PC with the nearest recompiled function name, e.g.
        // <UpdateAnimationVariableFrames+0x10>, when a symbol map is linked.
        char symbuf[96];
        symbuf[0] = '\0';
        uint32_t off = 0;
        const char* sym = gba_symbol_lookup(e.pc, &off);
        if (sym) {
            std::snprintf(symbuf, sizeof(symbuf), " <%s+0x%X>", sym, off);
        }
        std::fprintf(stderr,
                     "  #%u %-8s pc=0x%08X%s cpsr=0x%08X "
                     "addr=0x%08X value=0x%08X aux=0x%X "
                     "r0=0x%08X r1=0x%08X r2=0x%08X r3=0x%08X "
                     "r4=0x%08X r5=0x%08X r12=0x%08X "
                     "sp=0x%08X lr=0x%08X\n",
                     e.seq, trace_kind_name(e.kind), e.pc, symbuf, e.cpsr,
                     e.addr, e.value, e.aux, e.r0, e.r1, e.r2, e.r3,
                     e.r4, e.r5, e.r12, e.r13, e.r14);
    }
}

extern "C" uint32_t runtime_trace_copy_recent(RuntimeTraceEntry* out,
                                               uint32_t max_entries) {
    if (!out || max_entries == 0) return 0;
    if (max_entries > g_trace_count) max_entries = g_trace_count;
    uint32_t start = (g_trace_write + kTraceSize - max_entries) % kTraceSize;
    for (uint32_t i = 0; i < max_entries; ++i) {
        out[i] = g_trace[(start + i) % kTraceSize];
    }
    return max_entries;
}

// ── Per-instruction fingerprint ring ───────────────────────────────
// A large bounded ring of pre-execution architectural fingerprints, one per
// guest instruction, captured when armed. Lives here (not the runtime bridge)
// so the codegen test harness — which links the armv4t lib but not the runtime
// — gets the symbols without a stub. See runtime_arm.h for the contract.

extern "C" unsigned g_runtime_insn_trace = 0;

// General function-entry hook (see runtime_arm.h). nullptr = disabled; a host
// debug probe assigns its own handler. Called from the generated function
// prologue with the guest entry PC while R0..R3 still hold the AAPCS args.
extern "C" void (*g_runtime_fn_entry_hook)(uint32_t) = nullptr;

namespace {
constexpr std::size_t kMaxFnEntryObservers = 8;
void (*g_fn_entry_observers[kMaxFnEntryObservers])(uint32_t) = {};
std::size_t g_fn_entry_observer_count = 0;

void fn_entry_observer_dispatch(uint32_t entry_pc) {
    for (std::size_t i = 0; i < g_fn_entry_observer_count; ++i) {
        g_fn_entry_observers[i](entry_pc);
    }
}
}  // namespace

extern "C" int runtime_fn_entry_observer_add(void (*observer)(uint32_t)) {
    if (!observer) return 0;
    for (std::size_t i = 0; i < g_fn_entry_observer_count; ++i) {
        if (g_fn_entry_observers[i] == observer) return 1;
    }
    if (g_fn_entry_observer_count == kMaxFnEntryObservers) return 0;
    g_fn_entry_observers[g_fn_entry_observer_count++] = observer;
    g_runtime_fn_entry_hook = &fn_entry_observer_dispatch;
    return 1;
}

extern "C" void runtime_fn_entry_observer_remove(void (*observer)(uint32_t)) {
    for (std::size_t i = 0; i < g_fn_entry_observer_count; ++i) {
        if (g_fn_entry_observers[i] != observer) continue;
        for (std::size_t j = i + 1; j < g_fn_entry_observer_count; ++j) {
            g_fn_entry_observers[j - 1] = g_fn_entry_observers[j];
        }
        g_fn_entry_observers[--g_fn_entry_observer_count] = nullptr;
        break;
    }
    g_runtime_fn_entry_hook = g_fn_entry_observer_count
        ? &fn_entry_observer_dispatch : nullptr;
}

extern "C" void runtime_fn_entry_observers_reset(void) {
    for (auto& observer : g_fn_entry_observers) observer = nullptr;
    g_fn_entry_observer_count = 0;
    g_runtime_fn_entry_hook = nullptr;
}
extern "C" uint32_t g_runtime_resume_pc = 0u;

// BIOS-HLE hook (see runtime_arm.h). nullptr = disabled = pure LLE (the
// recompiled BIOS handles every SWI; byte-identical to the un-hooked build).
// When installed (gba::bios_hle_set_mode), runtime_swi consults it BEFORE the
// SVC-mode exception entry; a return of 1 means the SWI was serviced in HLE and
// the guest resumes at LR with no BIOS dispatch. 0 falls through to LLE.
extern "C" int (*g_bios_hle_hook)(uint32_t swi_num) = nullptr;

// No-BIOS mode: see runtime_arm.h. The stand-ins live after the bank helpers.
extern "C" int g_runtime_no_bios = 0;
extern "C" void (*g_runtime_bios_open_bus_hook)(uint32_t value) = nullptr;
static void no_bios_vector(uint32_t pc);

namespace {
// ~8M instructions of history (~60+ PPU-frames). The recomp runs several frames
// ahead of the interp oracle in cumulative cycle (boot step_frame overshoot), so
// a 1M (~8-frame) ring barely overlapped the interp's at the same frame number;
// the MC-HP-002 divergence onset (~f3644, deep in the run) needs a wider window
// so one dump spans both the pre-divergence anchor and the first divergent insn.
// 80 bytes/entry → ~640 MB, only touched when armed (GBARECOMP_INSN_TRACE=1).
constexpr uint32_t kFpSize = 1u << 23;
// The F1 crash log's ring: the last ~65k instructions, 5 MB.
constexpr uint32_t kCrashLogFpSize = 1u << 16;
uint32_t        g_fp_size = kFpSize;  // fixed once g_fp is allocated
RuntimeFpEntry* g_fp = nullptr;       // lazily allocated on first arm
uint32_t        g_fp_write = 0;
uint32_t        g_fp_count = 0;
}  // namespace

extern "C" void runtime_insn_fp(void) {
    if (g_bios_pc_log_armed && g_runtime_insn_trace == 2u) {
        const uint32_t pc = g_cpu.R[15] & ~1u;
        if (pc >= 0x08000000u) g_bios_pc_handed_off = true;
        if (pc < kBiosPcRegionEnd) {
            const std::size_t halfword = static_cast<std::size_t>(pc >> 1);
            const std::size_t index = halfword * 2u +
                ((g_cpu.cpsr & CPSR_T_BIT) ? 1u : 0u);
            if (index < g_bios_pc_seen.size()) {
                g_bios_pc_seen[index] |= g_bios_pc_handed_off ? 2u : 1u;
                ++g_bios_pc_samples;
            }
        }
        return;
    }
    if (!g_fp) {
        g_fp = static_cast<RuntimeFpEntry*>(
            std::calloc(g_fp_size, sizeof(RuntimeFpEntry)));
        if (!g_fp) { g_runtime_insn_trace = 0; return; }  // OOM → disarm quietly
    }
    RuntimeFpEntry& e = g_fp[g_fp_write];
    e.cycles = g_runtime_cycles;
    e.pc = g_cpu.R[15];
    e.cpsr = g_cpu.cpsr;
    for (int i = 0; i < 16; ++i) e.r[i] = g_cpu.R[i];
    g_fp_write = (g_fp_write + 1u) % g_fp_size;
    if (g_fp_count < g_fp_size) ++g_fp_count;
}

extern "C" void runtime_set_crash_log(int on) {
    g_crash_log = on != 0;
    if (g_bios_pc_log_armed) return;
    if (g_crash_log) {
        if (!g_fp && !g_insn_trace_from_env) g_fp_size = kCrashLogFpSize;
        g_runtime_insn_trace = 1u;
    } else if (!g_insn_trace_from_env) {
        g_runtime_insn_trace = 0u;
    }
}

extern "C" void runtime_fp_reset(void) {
    g_fp_write = 0;
    g_fp_count = 0;
}

extern "C" uint32_t runtime_fp_count(void) { return g_fp_count; }

extern "C" uint32_t runtime_fp_save_file(const char* path) {
    if (!path || !g_fp || g_fp_count == 0) return 0;
    std::FILE* f = std::fopen(path, "wb");
    if (!f) return 0;
    uint32_t magic = 0x31504647u;  // 'GFP1'
    uint32_t esz = static_cast<uint32_t>(sizeof(RuntimeFpEntry));
    unsigned long long count = g_fp_count;
    std::fwrite(&magic, sizeof(magic), 1, f);
    std::fwrite(&esz, sizeof(esz), 1, f);
    std::fwrite(&count, sizeof(count), 1, f);
    uint32_t start = (g_fp_write + g_fp_size - g_fp_count) % g_fp_size;
    for (uint32_t i = 0; i < g_fp_count; ++i) {
        std::fwrite(&g_fp[(start + i) % g_fp_size], sizeof(RuntimeFpEntry), 1, f);
    }
    std::fclose(f);
    return g_fp_count;
}

// Dump the LAST `n` fingerprint records (oldest-first within the window) as a
// human-readable CSV. This is the freeze's self-documenting execution history:
// the always-on hang watchdog calls it the moment it trips, so the PCs the
// game thread executed leading INTO the freeze are on disk without any live
// interaction (PRINCIPLES.md "always-on ring first" — query the ring for the
// window of interest, never arm-then-capture). Returns records written.
// Requires GBARECOMP_INSN_TRACE armed (else the ring is empty → 0). Caller is
// the game thread at the watchdog trip, so this is single-threaded w.r.t. the
// ring writes.
extern "C" uint32_t runtime_fp_save_tail_csv(const char* path, uint32_t n) {
    if (!path || !g_fp || g_fp_count == 0) return 0;
    if (n == 0 || n > g_fp_count) n = g_fp_count;
    std::FILE* f = std::fopen(path, "w");
    if (!f) return 0;
    std::fprintf(f, "idx,cycles,pc,cpsr,r0,r1,r2,r3,r4,r5,r6,r7,r8,r9,r10,r11,"
                    "r12,sp,lr,r15\n");
    // The most recent record is at (g_fp_write-1); walk back n, then forward.
    uint32_t start = (g_fp_write + g_fp_size - n) % g_fp_size;
    for (uint32_t i = 0; i < n; ++i) {
        const RuntimeFpEntry& e = g_fp[(start + i) % g_fp_size];
        std::fprintf(f, "%u,%llu,0x%08X,0x%08X", i,
                     static_cast<unsigned long long>(e.cycles), e.pc, e.cpsr);
        for (int k = 0; k < 16; ++k) std::fprintf(f, ",0x%08X", e.r[k]);
        std::fputc('\n', f);
    }
    std::fclose(f);
    return n;
}

// Filter the insn-fingerprint ring by guest PC: collect up to `max_hits`
// cumulative-cycle stamps (oldest-first) for every recorded execution of `pc`.
// The Δ between consecutive returned stamps is the offset-cancelled cycle ruler
// (Axis 2) peered against the NBA oracle's cyc_anchor. Non-mutating; requires
// the ring armed (GBARECOMP_INSN_TRACE) — empty otherwise.
extern "C" uint32_t runtime_fp_query_pc(uint32_t pc, uint32_t max_hits,
                                        unsigned long long* out_cycles) {
    if (!g_fp || g_fp_count == 0 || !out_cycles || max_hits == 0) return 0;
    uint32_t start = (g_fp_write + g_fp_size - g_fp_count) % g_fp_size;
    uint32_t found = 0;
    for (uint32_t i = 0; i < g_fp_count && found < max_hits; ++i) {
        const RuntimeFpEntry& e = g_fp[(start + i) % g_fp_size];
        if (e.pc == pc) out_cycles[found++] = e.cycles;
    }
    return found;
}

extern "C" uint32_t runtime_bios_pc_log_save_file(const char* path) {
    if (!path || !g_bios_pc_log_armed || g_bios_pc_seen.empty()) return 0;
    std::FILE* f = std::fopen(path, "w");
    if (!f) return 0;
    std::fprintf(f, "pc,mode,phase\n");
    static const char* const kPhase[4] = {"", "boot", "play", "both"};
    uint32_t count = 0;
    for (uint32_t halfword = 0;
         halfword < kBiosPcRegionEnd / 2u; ++halfword) {
        for (uint32_t thumb = 0; thumb < 2u; ++thumb) {
            const std::size_t index = static_cast<std::size_t>(halfword) * 2u + thumb;
            if (g_bios_pc_seen[index] == 0u) continue;
            std::fprintf(f, "0x%08x,%s,%s\n", halfword * 2u,
                         thumb ? "thumb" : "arm",
                         kPhase[g_bios_pc_seen[index] & 3u]);
            ++count;
        }
    }
    std::fclose(f);
    return count;
}

extern "C" unsigned long long runtime_bios_pc_log_sample_count(void) {
    return g_bios_pc_samples;
}

// Read-only current recompiled-CPU PC, for always-on observability taps that
// live outside the armv4t lib (the gba_io MMIO write-trace ring).
extern "C" uint32_t runtime_current_pc(void) { return g_cpu.R[15]; }

// ── IRQ-vector log (MC-HP-002) ─────────────────────────────────────
// A dedicated, always-on, cycle-stamped log of EVERY IRQ vectoring. The
// general trace ring (4096) is diluted by mem-writes (thousands/frame) so it
// spans only a fraction of a frame; this log records one entry per IRQ vector
// (~1-2/frame) so it spans ~10^4 frames — enough to find, on a fresh boot, the
// frame where the recomp vectors an EXTRA VBlank IRQ vs the interp oracle (the
// one-extra-M4A-tick that drifts the sequencer into the f3656 spin). The
// `from_halt` flag distinguishes the wake-from-HALT delayed path from the
// running immediate path. Dumped as CSV on exit when GBARECOMP_IRQ_LOG is set.
namespace {
constexpr uint32_t kIrqLogSize = 1u << 17;   // 131072 vectors (~32k+ frames)
using IrqLogEntry = RuntimeIrqLogEntry;       // public type (runtime_arm.h)
IrqLogEntry* g_irq_log = nullptr;
uint32_t     g_irq_log_write = 0;
uint32_t     g_irq_log_count = 0;
}  // namespace
// Set by runtime_tick's wake-from-HALT path just before runtime_irq; cleared
// after. Tells the log whether this vector woke the CPU from HALT.
extern "C" uint32_t g_runtime_irq_from_halt = 0;

extern "C" void runtime_irq_log_record(uint32_t src, uint32_t ret, uint32_t cpsr) {
    // Always-on ring (Release too): recording is unconditional so the live
    // irq_cap TCP query always has the window of interest without arming. The
    // env GBARECOMP_IRQ_LOG only governs the separate on-exit CSV dump. Purely
    // additive bookkeeping — does not alter IRQ behavior.
    if (!g_irq_log) {
        g_irq_log = static_cast<IrqLogEntry*>(
            std::calloc(kIrqLogSize, sizeof(IrqLogEntry)));
        if (!g_irq_log) return;
    }
    IrqLogEntry& e = g_irq_log[g_irq_log_write];
    e.cycles = g_runtime_cycles;
    e.src = src;
    e.ret = ret;
    e.cpsr = cpsr;
    e.from_halt = g_runtime_irq_from_halt;
    g_irq_log_write = (g_irq_log_write + 1u) % kIrqLogSize;
    if (g_irq_log_count < kIrqLogSize) ++g_irq_log_count;
}

extern "C" uint32_t runtime_irq_log_save_file(const char* path) {
    if (!path || !g_irq_log || g_irq_log_count == 0) return 0;
    std::FILE* f = std::fopen(path, "wb");
    if (!f) return 0;
    std::fprintf(f, "seq,cycles,src,ret,cpsr,from_halt\n");
    uint32_t start = (g_irq_log_write + kIrqLogSize - g_irq_log_count) % kIrqLogSize;
    for (uint32_t i = 0; i < g_irq_log_count; ++i) {
        const IrqLogEntry& e = g_irq_log[(start + i) % kIrqLogSize];
        std::fprintf(f, "%u,%llu,0x%04x,0x%08x,0x%08x,%u\n",
                     i, e.cycles, e.src, e.ret, e.cpsr, e.from_halt);
    }
    std::fclose(f);
    return g_irq_log_count;
}

extern "C" uint32_t runtime_irq_log_count(void) { return g_irq_log_count; }

// Copy the most recent `max` IRQ-vector entries (oldest-first within the
// window) for the live irq_cap TCP query. Non-mutating.
extern "C" uint32_t runtime_irq_log_copy_recent(RuntimeIrqLogEntry* out,
                                                uint32_t max) {
    if (!out || max == 0 || !g_irq_log || g_irq_log_count == 0) return 0;
    if (max > g_irq_log_count) max = g_irq_log_count;
    uint32_t start = (g_irq_log_write + kIrqLogSize - max) % kIrqLogSize;
    for (uint32_t i = 0; i < max; ++i) {
        out[i] = g_irq_log[(start + i) % kIrqLogSize];
    }
    return max;
}

// ── SWI log (MC-HP-002 milestone-PC sequence) ──────────────────────
// Always-on, cycle-stamped log of every SWI (BIOS call). VBlankIntrWait is
// SWI 5; it runs once per main-loop iteration, so its sequence IS the main-loop
// cadence. Per recomp-template "use the oracle as an order + state + caller
// reference": we capture cycle + caller(ret) + r0/r1 args + the BIOS IntrWait
// flags at 0x03007FF8, so a recomp-vs-interp SWI-5 sequence diff shows an EXTRA
// main-loop iteration (the recomp's 1-frame-ahead slip ~f272) and the IntrWait
// flag state that produced it. The route column records whether the call used
// the recompiled BIOS (LLE), was serviced by HLE, or fell through HLE to LLE.
// Armed by env GBARECOMP_SWI_LOG; CSV on exit.
namespace {
constexpr uint32_t kSwiLogSize = 1u << 17;
struct SwiLogEntry { unsigned long long cycles; uint32_t imm; uint32_t ret;
                     uint32_t r0; uint32_t r1; uint32_t r2; uint32_t lr;
                     uint32_t iwflags; uint32_t route; };
SwiLogEntry* g_swi_log = nullptr;
uint32_t     g_swi_log_write = 0;
uint32_t     g_swi_log_count = 0;
}  // namespace

extern "C" void runtime_swi_log_record(unsigned long long cycles,
                                       uint32_t imm, uint32_t ret,
                                       uint32_t r0, uint32_t r1, uint32_t r2,
                                       uint32_t lr, uint32_t iwflags,
                                       uint32_t route) {
    static int armed = -1;
    if (armed < 0) armed = std::getenv("GBARECOMP_SWI_LOG") ? 1 : 0;
    if (!armed) return;
    if (!g_swi_log) {
        g_swi_log = static_cast<SwiLogEntry*>(
            std::calloc(kSwiLogSize, sizeof(SwiLogEntry)));
        if (!g_swi_log) { armed = 0; return; }
    }
    SwiLogEntry& e = g_swi_log[g_swi_log_write];
    e.cycles = cycles;
    e.imm = imm; e.ret = ret; e.r0 = r0; e.r1 = r1; e.r2 = r2; e.lr = lr;
    e.iwflags = iwflags; e.route = route;
    g_swi_log_write = (g_swi_log_write + 1u) % kSwiLogSize;
    if (g_swi_log_count < kSwiLogSize) ++g_swi_log_count;
}

extern "C" uint32_t runtime_swi_log_save_file(const char* path) {
    if (!path || !g_swi_log || g_swi_log_count == 0) return 0;
    std::FILE* f = std::fopen(path, "wb");
    if (!f) return 0;
    std::fprintf(f, "seq,cycles,imm,ret,r0,r1,r2,lr,iwflags,route\n");
    uint32_t start = (g_swi_log_write + kSwiLogSize - g_swi_log_count) % kSwiLogSize;
    for (uint32_t i = 0; i < g_swi_log_count; ++i) {
        const SwiLogEntry& e = g_swi_log[(start + i) % kSwiLogSize];
        std::fprintf(f, "%u,%llu,%u,0x%08x,0x%08x,0x%08x,0x%08x,0x%08x,0x%08x,%u\n",
                     i, e.cycles, e.imm, e.ret, e.r0, e.r1, e.r2, e.lr,
                     e.iwflags, e.route);
    }
    std::fclose(f);
    return g_swi_log_count;
}

// ── Bus binding ────────────────────────────────────────────────────

namespace gbarecomp {
namespace runtime_arm {

// We keep this as a void* so the header doesn't need to drag in
// GbaBus. The bus type is known to the implementation file only.
void* g_bus_handle = nullptr;

}  // namespace runtime_arm
}  // namespace gbarecomp

// ── Condition codes ────────────────────────────────────────────────

extern "C" int arm_cond_passes(unsigned cond) {
    // The condition codes are 4 bits. AL / NV are the unconditional
    // bands.
    const uint32_t n = cpsr_n();
    const uint32_t z = cpsr_z();
    const uint32_t c = cpsr_c();
    const uint32_t v = cpsr_v();
    switch (cond & 0xFu) {
        case 0x0: return z != 0;                            // EQ
        case 0x1: return z == 0;                            // NE
        case 0x2: return c != 0;                            // CS/HS
        case 0x3: return c == 0;                            // CC/LO
        case 0x4: return n != 0;                            // MI
        case 0x5: return n == 0;                            // PL
        case 0x6: return v != 0;                            // VS
        case 0x7: return v == 0;                            // VC
        case 0x8: return (c != 0) && (z == 0);              // HI
        case 0x9: return (c == 0) || (z != 0);              // LS
        case 0xA: return n == v;                            // GE
        case 0xB: return n != v;                            // LT
        case 0xC: return (z == 0) && (n == v);              // GT
        case 0xD: return (z != 0) || (n != v);              // LE
        case 0xE: return 1;                                 // AL
        case 0xF: return 0;                                 // NV (ARMv4T: never)
        default:  return 0;
    }
}

// ── Bus accessors ──────────────────────────────────────────────────
//
// The runtime initializer (runtime_init) installs `g_bus_handle` to
// a pointer to the active gba::GbaBus. Calling the bus is the only
// per-instruction host-side work the generated code has to do, so
// these need to be fast.
//
// For first cut we delegate via the bus_handle as a fat pointer. A
// later optimization can compile bus reads inline using the
// runtime's memory map directly.

namespace {

inline uint32_t* bus_ptr32(void* /*h*/, uint32_t /*addr*/) {
    return nullptr;  // not used; placeholder for future inlining
}

}  // namespace

// Bus accessors are NOT defined here — they live in
// src/runtime/runtime_bus_bridge.cpp (part of gbarecomp_runtime),
// which is the only translation unit allowed to include gba_bus.h.
// Anything that links against gbarecomp_runtime gets them; tests
// that link only gbarecomp_armv4t can supply their own stubs.

// ── Shifter helpers ────────────────────────────────────────────────

extern "C" uint32_t arm_shift_lsl(uint32_t v, uint32_t n, int set_carry) {
    if (n == 0) return v;  // ARM ARM A5.1.5: shift by 0 → no carry update
    if (n >= 32) {
        if (set_carry) {
            uint32_t carry = (n == 32) ? (v & 1u) : 0u;
            g_cpu.cpsr = (g_cpu.cpsr & ~CPSR_C_BIT) | (carry ? CPSR_C_BIT : 0u);
        }
        return 0;
    }
    if (set_carry) {
        uint32_t carry = (v >> (32u - n)) & 1u;
        g_cpu.cpsr = (g_cpu.cpsr & ~CPSR_C_BIT) | (carry ? CPSR_C_BIT : 0u);
    }
    return v << n;
}

extern "C" uint32_t arm_shift_lsr(uint32_t v, uint32_t n, int set_carry) {
    if (n == 0) return v;
    if (n >= 32) {
        if (set_carry) {
            uint32_t carry = (n == 32) ? ((v >> 31) & 1u) : 0u;
            g_cpu.cpsr = (g_cpu.cpsr & ~CPSR_C_BIT) | (carry ? CPSR_C_BIT : 0u);
        }
        return 0;
    }
    if (set_carry) {
        uint32_t carry = (v >> (n - 1u)) & 1u;
        g_cpu.cpsr = (g_cpu.cpsr & ~CPSR_C_BIT) | (carry ? CPSR_C_BIT : 0u);
    }
    return v >> n;
}

extern "C" uint32_t arm_shift_asr(uint32_t v, uint32_t n, int set_carry) {
    if (n == 0) return v;
    if (n >= 32) {
        uint32_t carry = (v >> 31) & 1u;
        if (set_carry) {
            g_cpu.cpsr = (g_cpu.cpsr & ~CPSR_C_BIT) | (carry ? CPSR_C_BIT : 0u);
        }
        return carry ? 0xFFFFFFFFu : 0u;
    }
    if (set_carry) {
        uint32_t carry = (v >> (n - 1u)) & 1u;
        g_cpu.cpsr = (g_cpu.cpsr & ~CPSR_C_BIT) | (carry ? CPSR_C_BIT : 0u);
    }
    return static_cast<uint32_t>(static_cast<int32_t>(v) >> n);
}

extern "C" uint32_t arm_shift_ror(uint32_t v, uint32_t n, int set_carry) {
    if (n == 0) return v;
    n &= 31u;
    if (n == 0) {  // ROR by multiple of 32 → no change but use top bit as carry
        if (set_carry) {
            g_cpu.cpsr = (g_cpu.cpsr & ~CPSR_C_BIT) |
                ((v & 0x80000000u) ? CPSR_C_BIT : 0u);
        }
        return v;
    }
    uint32_t r = (v >> n) | (v << (32u - n));
    if (set_carry) {
        g_cpu.cpsr = (g_cpu.cpsr & ~CPSR_C_BIT) |
            ((r & 0x80000000u) ? CPSR_C_BIT : 0u);
    }
    return r;
}

// ── Flag updaters ──────────────────────────────────────────────────

// The six flag updaters are `static inline` in runtime_arm.h so every caller
// (generated corpus included) can fold them inline. The overlay ABI table
// takes their address, which stays valid for a static inline function.

// ── Dispatch ───────────────────────────────────────────────────────

namespace {

// Binary-search a sorted DispatchEntry table for `pc` and current
// instruction-set state. Same numeric addresses can have both ARM
// and THUMB entries, so scan the equal-address run for CPSR.T.
// `out_index`, when non-null, receives the matched table slot (used by the
// function-coverage recorder below to address its per-entry bit array
// without a second search or its own copy of the table).
void (*lookup_in(const DispatchEntry* table, unsigned len,
                 uint32_t pc, bool thumb, unsigned* out_index = nullptr))(void) {
    unsigned lo = 0, hi = len;
    while (lo < hi) {
        unsigned mid = (lo + hi) >> 1u;
        if (table[mid].addr < pc) lo = mid + 1u;
        else                       hi = mid;
    }
    for (unsigned i = lo; i < len && table[i].addr == pc; ++i) {
        if ((table[i].thumb != 0) == thumb) {
            if (out_index) *out_index = i;
            return table[i].fn;
        }
    }
    return nullptr;
}

// PC range covered by the BIOS region (16 KB at 0x0). Anything in
// this range consults the BIOS table; anything else consults the
// cart table. The two ranges don't overlap so the order is just
// "BIOS for <4000, cart otherwise" — no merged search needed.
constexpr uint32_t kBiosRegionEnd = 0x00004000u;

// ── Function coverage (input-explorer measurement, Part 1) ──────────
// GBARECOMP_FUNC_COVERAGE=<path> opt-in, following the GBARECOMP_IRQ_LOG /
// GBARECOMP_SWI_LOG convention: presence of the env var both arms recording
// and names the exit-time output path. Default OFF, and off costs NOTHING
// beyond what every build already pays: generated function prologues always
// carry `if (g_runtime_fn_entry_hook) g_runtime_fn_entry_hook(entry_pc);`
// (see recompile/emit_function.cpp) for the general debug-probe surface —
// this feature only supplies a handler for that existing hook, so an
// unarmed run is BYTE-IDENTICAL to one built without this feature at all.
//
// A direct BL uses the host C stack and never goes through runtime_dispatch
// (see runtime_call_push_return's doc comment above), so hooking
// runtime_dispatch alone would miss most function executions — the vast
// majority of calls in a real run are direct. The function-entry hook fires
// for EVERY generated function body regardless of how it was reached, which
// is why coverage is wired there instead of at the dispatch-table lookup.
//
// One bit per dispatch-table entry (bios table + cart table, kept as two
// arrays since table indices aren't comparable across them), set the first
// time that entry's function executes. A hit/no-hit bit is all an input
// explorer needs ("did this input reach new code"); a per-function count
// would cost an extra memory write on every one of tens of millions of
// dispatches for a number nothing downstream reads.
int g_coverage_armed = 0;
std::vector<uint8_t> g_coverage_bios;  // sized to kBiosDispatchTableLen bits
std::vector<uint8_t> g_coverage_cart;  // sized to kDispatchTableLen bits

inline void coverage_mark(std::vector<uint8_t>& bits, unsigned idx) {
    bits[idx >> 3] |= static_cast<uint8_t>(1u << (idx & 7u));
}

inline bool coverage_marked(const std::vector<uint8_t>& bits, unsigned idx) {
    return (bits[idx >> 3] & static_cast<uint8_t>(1u << (idx & 7u))) != 0;
}

}  // namespace

// Installed as g_runtime_fn_entry_hook when armed. Mirrors runtime_dispatch's
// own table selection (BIOS below kBiosRegionEnd, cart otherwise) and reuses
// lookup_in so the coverage bit addresses the exact same entry runtime_dispatch
// would have called — a binary search per call, same cost class as the
// dispatch that just happened, and only paid at all when armed. extern "C"
// to match the g_runtime_fn_entry_hook pointer's C linkage.
extern "C" void coverage_fn_entry_hook(uint32_t entry_pc) {
    const uint32_t pc = entry_pc & ~1u;
    if (std::getenv("GBARECOMP_AUDIO_PROBE") &&
        (pc == 0x080F9B74u || pc == 0x080F9C90u ||
         pc == 0x080F9EE8u || pc == 0x080F9F6Cu ||
         pc == 0x080FA1FCu)) {
        static unsigned audio_entry_logs = 0;
        if (audio_entry_logs++ < 24u) {
            std::fprintf(stderr,
                         "[audio-entry] pc=0x%08x r0=0x%08x r1=0x%08x "
                         "r2=0x%08x r3=0x%08x r4=0x%08x r5=0x%08x r6=0x%08x "
                         "r7=0x%08x r9=0x%08x r10=0x%08x r11=0x%08x\n",
                         pc, g_cpu.R[0], g_cpu.R[1], g_cpu.R[2], g_cpu.R[3],
                         g_cpu.R[4], g_cpu.R[5], g_cpu.R[6], g_cpu.R[7],
                         g_cpu.R[9], g_cpu.R[10], g_cpu.R[11]);
        }
    }
    const bool thumb = (g_cpu.cpsr & CPSR_T_BIT) != 0;
    unsigned idx = 0;
    if (pc < kBiosRegionEnd) {
        if (lookup_in(kBiosDispatchTable, kBiosDispatchTableLen, pc, thumb, &idx))
            coverage_mark(g_coverage_bios, idx);
    } else {
        if (lookup_in(kDispatchTable, kDispatchTableLen, pc, thumb, &idx))
            coverage_mark(g_coverage_cart, idx);
    }
}

namespace {

// Arms coverage recording from GBARECOMP_FUNC_COVERAGE (see the block above).
// Idempotent — reset() may run more than once (e.g. a future save-state
// reload); only the first call with the env var set does anything.
void coverage_arm_from_env() {
    const char* cov = std::getenv("GBARECOMP_FUNC_COVERAGE");
    if (!cov || !cov[0]) return;
    if (std::getenv("GBARECOMP_AUDIO_PROBE"))
        std::fprintf(stderr, "[audio-entry] coverage hook armed\n");
    if (!g_coverage_armed) {
        g_coverage_bios.assign((kBiosDispatchTableLen + 7u) / 8u, 0u);
        g_coverage_cart.assign((kDispatchTableLen + 7u) / 8u, 0u);
        g_coverage_armed = 1;
    }
    runtime_fn_entry_observer_add(&coverage_fn_entry_hook);
}

}  // namespace

#if defined(__GNUC__) && !defined(__clang__)
// RAM-overlay dispatch is a guest tail-transfer boundary. Keep this function
// optimized even in Debug builds so the final `return fn()` remains a host
// tail jump; without it, the offline repeated-tail probe grows the C++ stack.
extern "C" __attribute__((optimize("O2")))
#else
extern "C"
#endif
void runtime_dispatch(uint32_t target_pc) {
    runtime_flush_deferred();  // block timing: pay before leaving the block
    // Strip THUMB bit; codegen handles the mode via cpsr_T already.
    uint32_t pc = target_pc & ~1u;
    runtime_trace_event(RUNTIME_TRACE_DISPATCH, pc, target_pc, 0, 0);

    bool thumb = (g_cpu.cpsr & CPSR_T_BIT) != 0;
    runtime_pool_dispatch_history_record(pc, thumb ? 1 : 0);
    g_recent_dispatch[g_recent_dispatch_next] = pc | (thumb ? 1u : 0u);
    g_recent_dispatch_next = (g_recent_dispatch_next + 1u) % kRecentDispatchDepth;
    const bool yield_resume = g_dispatch_is_yield_resume;
    g_dispatch_is_yield_resume = false;
    if (pc == 0x03002000u && !thumb)
        runtime_unpacker_slot_note_dispatch(yield_resume);
    if (g_runtime_ram_image_dispatch_probe) {
        g_runtime_ram_image_dispatch_probe(
            g_runtime_vblank_starts, pc, thumb ? 1u : 0u);
    }
    // The game RAM resolver owns the identity check for every RAM dispatch.
    // A null result means "this resolver deliberately declined this exact
    // transfer" (for example, its active-entry guard rejected a recursive
    // re-entry), so do not run the Stage-2 overlay resolver again below.  That
    // second lookup bypasses the resolver's guard and invokes the same healed
    // RAM body recursively.  Static-table lookup still runs below, preserving
    // the hook's normal fixed-image fall-through behavior.
    bool ram_dispatch_hook_attempted = false;
    if (pc >= 0x02000000u && pc < 0x04000000u &&
        g_runtime_ram_dispatch_hook) {
        ram_dispatch_hook_attempted = true;
        // Resolve first, then invoke with no work after the call. This is the
        // guest tail-transfer boundary; GCC/Clang can lower it to a jump.
        if (RuntimeGuestFn fn = g_runtime_ram_dispatch_hook(pc, thumb ? 1 : 0)) {
#if defined(__clang__)
            [[clang::musttail]]
#endif
            return fn();
        }
    }
    void (*fn)(void) = nullptr;
    unsigned fn_index = 0;
    if (pc < kBiosRegionEnd) {
        if (g_runtime_no_bios) {
            no_bios_vector(pc);
            return;
        }
        fn = lookup_in(kBiosDispatchTable, kBiosDispatchTableLen, pc, thumb,
                       &fn_index);
    } else {
        fn = lookup_in(kDispatchTable, kDispatchTableLen, pc, thumb,
                       &fn_index);
    }
    if (fn && pc >= 0x02000000u && pc < 0x04000000u) {
        runtime_register_ram_code_page(pc);
        unsigned next = fn_index + 1u;
        while (next < kDispatchTableLen &&
               kDispatchTable[next].addr <= pc) ++next;
        uint32_t end = (pc & 0xFF000000u) +
            ((pc >> 24) == 0x03u ? 0x00008000u : 0x00040000u);
        if (next < kDispatchTableLen &&
            (kDispatchTable[next].addr >> 24) == (pc >> 24)) {
            end = kDispatchTable[next].addr;
        }
        if (runtime_ram_code_range_dirty(pc, end)) {
            // The dirty bit is page-granular: it can be set by a write
            // anywhere in this pc's 4 KiB page, not necessarily by a write
            // that changed this pc's own bytes. RuntimeRamDispatchHook above
            // already ran for this exact pc/thumb this call; if it
            // independently confirmed (via its own identity/CRC check) that
            // the reviewed ROM-sourced image here still matches live memory,
            // trust the AOT fn instead of re-healing/re-JITing it.
            const bool identity_confirmed = ram_dispatch_hook_attempted &&
                g_runtime_ram_identity_confirmed_hook &&
                g_runtime_ram_identity_confirmed_hook(pc, thumb ? 1 : 0) != 0;
            if (!identity_confirmed) {
                if (RuntimeGuestFn healed =
                        gbarecomp::overlay_resolve(pc, thumb ? 1 : 0)) {
                    return healed();
                }
                runtime_mutable_ram_code_miss(pc, thumb ? 1 : 0);
                return;
            }
        }
    }
    if (fn) return fn();
    // Stage-2 self-heal: third dispatch tier. After the static tables miss,
    // consult the runtime-healed native overlays before bridging. Defined in
    // src/runtime/overlay_loader.cpp (a null stub in tests/codegen/stubs.cpp,
    // since armv4t must not depend on the runtime lib). When the feature is
    // off this is a single bool check and returns 0.
    if (!ram_dispatch_hook_attempted) {
        if (RuntimeGuestFn healed =
                gbarecomp::overlay_resolve(pc, thumb ? 1 : 0)) {
            return healed();
        }
    }
    runtime_dispatch_miss(target_pc);
}

extern "C" void runtime_dispatch_with_exchange(uint32_t target_pc) {
    // Bit 0 of target indicates THUMB.
    if (target_pc & 1u) g_cpu.cpsr |= CPSR_T_BIT;
    else                g_cpu.cpsr &= ~CPSR_T_BIT;
    runtime_trace_event(RUNTIME_TRACE_EXCHANGE, target_pc & ~1u, target_pc, 0, 0);
    runtime_dispatch(target_pc);
}

extern "C" int runtime_has_static_entry(uint32_t pc, int thumb) {
    uint32_t a = pc & ~1u;
    void (*fn)(void) = (a < kBiosRegionEnd)
        ? lookup_in(kBiosDispatchTable, kBiosDispatchTableLen, a, thumb != 0)
        : lookup_in(kDispatchTable, kDispatchTableLen, a, thumb != 0);
    return fn != nullptr ? 1 : 0;
}

// Writes the armed coverage bit arrays as a deterministic, address-sorted
// text file: "0xADDRESS MODE\n" per executed function, BIOS entries first
// then cart entries. Both tables are individually address-sorted (lookup_in
// binary-searches them, so they must be) and the BIOS PC range is strictly
// below the cart range, so emitting bios-then-cart needs no merge step and
// two runs that executed the same code produce byte-identical files — the
// determinism an input explorer needs to diff "did this reach new code".
// A no-op (returns 0) when coverage was never armed, so a build that links
// this in but never sets GBARECOMP_FUNC_COVERAGE writes nothing.
extern "C" uint32_t runtime_coverage_save_file(const char* path) {
    if (!path || !g_coverage_armed) return 0;
    std::FILE* f = std::fopen(path, "w");
    if (!f) return 0;
    uint32_t n = 0;
    for (unsigned i = 0; i < kBiosDispatchTableLen; ++i) {
        if (!coverage_marked(g_coverage_bios, i)) continue;
        const DispatchEntry& e = kBiosDispatchTable[i];
        std::fprintf(f, "0x%08X %s\n", e.addr, e.thumb ? "thumb" : "arm");
        ++n;
    }
    for (unsigned i = 0; i < kDispatchTableLen; ++i) {
        if (!coverage_marked(g_coverage_cart, i)) continue;
        const DispatchEntry& e = kDispatchTable[i];
        std::fprintf(f, "0x%08X %s\n", e.addr, e.thumb ? "thumb" : "arm");
        ++n;
    }
    std::fclose(f);
    return n;
}

extern "C" void runtime_call_push_return(uint32_t return_pc) {
    uint32_t pc = return_pc & ~1u;
    if (g_call_return_depth >= kCallReturnStackSize) {
        std::fprintf(stderr,
                     "runtime_arm: generated call-return stack overflow "
                     "at return_pc=0x%08X\n",
                     pc);
        runtime_trace_dump_recent(96);
        std::abort();
    }
    g_call_return_stack[g_call_return_depth++] = pc;
    if (unpacker_pc_in_slot(pc))
        unpacker_note_callret(kUnpackerPush, pc, g_call_return_depth,
                              g_call_return_floor, 0u);
    runtime_trace_event(RUNTIME_TRACE_CALL, pc, pc, g_call_return_depth, 1u);
}

extern "C" int runtime_call_should_return(uint32_t target_pc) {
    uint32_t pc = target_pc & ~1u;
    // Scan only down to the active IRQ floor — never match an interrupted
    // mainline / enclosing-IRQ return frame.
    for (uint32_t i = g_call_return_depth; i != g_call_return_floor; --i) {
        uint32_t slot = i - 1u;
        if (g_call_return_stack[slot] == pc) {
            if (unpacker_pc_in_slot(pc))
                unpacker_note_callret(kUnpackerRetMatch, pc, slot,
                                      g_call_return_depth,
                                      slot + 1u == g_call_return_depth ? 1u : 0u);
            runtime_trace_event(RUNTIME_TRACE_CALL, pc, pc,
                                g_call_return_depth,
                                (slot + 1u == g_call_return_depth) ? 2u : 5u);
            g_call_return_depth = slot;
            if (g_runtime_call_return_hook)
                g_runtime_call_return_hook(pc, g_call_return_depth);
            if (g_runtime_ram_image_boundary_probe)
                g_runtime_ram_image_boundary_probe(
                    0u, pc, g_call_return_depth);
            return 1;
        }
    }
    uint32_t top = g_call_return_depth != 0
        ? g_call_return_stack[g_call_return_depth - 1u]
        : 0xFFFFFFFFu;
    runtime_trace_event(RUNTIME_TRACE_CALL, pc, top, g_call_return_depth, 3u);
    if (g_runtime_call_return_hook)
        g_runtime_call_return_hook(pc, g_call_return_depth);
    if (g_runtime_ram_image_boundary_probe)
        g_runtime_ram_image_boundary_probe(0u, pc, g_call_return_depth);
    // An unmatched return normally dispatches its target nested inside the
    // returning C frame. A callee that rewrites its own return address
    // (`sub lr, pc, #imm` before `bx lr`) and is called in a loop therefore
    // nests one frame deeper per iteration: Golden Sun's decompressor
    // Func_2544 refills its bit buffer this way, and a large decompression
    // (the world map's R-button view) overflowed the stack and aborted. Past
    // kUnwindDepth live frames, report "return" instead: this C frame returns
    // with R15 already set to the target, each generated call site above it
    // sees the mismatch, cancels its own frame and returns, and the dispatch
    // loop at the bottom continues at R15 -- the same path a scheduler yield
    // takes. Below the threshold nothing changes.
    constexpr uint32_t kUnwindDepth = kCallReturnStackSize / 4u;
    if (g_call_return_depth - g_call_return_floor >= kUnwindDepth) {
        // Journal this even when the target itself is outside the unpacker
        // slot: what matters is that the unwind collapses live frames of which
        // some are unpacker return addresses. Only scan (and log) when at
        // least one live entry in [floor, depth) lies inside the slot.
        // Skip an exact repeat of the previous unwind so a long deep run
        // cannot push the surrounding push/match/cancel entries out.
        static uint32_t s_last_unwind_pc = 0xFFFFFFFFu;
        static uint32_t s_last_unwind_depth = 0xFFFFFFFFu;
        const bool repeat = pc == s_last_unwind_pc &&
                            g_call_return_depth == s_last_unwind_depth;
        s_last_unwind_pc = pc;
        s_last_unwind_depth = g_call_return_depth;
        for (uint32_t i = g_call_return_floor;
             !repeat && i < g_call_return_depth; ++i) {
            if (unpacker_pc_in_slot(g_call_return_stack[i])) {
                unpacker_note_callret(kUnpackerRetUnwind, pc,
                                      g_call_return_depth,
                                      g_call_return_floor, 0u);
                break;
            }
        }
        return 1;
    }
    return 0;
}

extern "C" void runtime_call_cancel_return(uint32_t return_pc) {
    uint32_t pc = return_pc & ~1u;
    // A strict top-only check looks correct but isn't: some ARM code
    // shares one BL target as a trampoline from several call sites, then
    // computes its own return address unconditionally (e.g. `sub r14,r15,
    // #imm`) instead of using the LR each caller actually pushed. Every
    // site's frame resolves to the same nested-dispatch PC, so by the
    // time control unwinds back to a given call site in C, that site's
    // frame is no longer on top — it is buried under frames the nested
    // dispatch pushed while running the trampoline's resolved target.
    // Top-only cancel then silently no-ops, stranding the frame forever;
    // one leaked frame per iteration eventually overflows the stack. Scan
    // down to the floor exactly like runtime_call_should_return does, and
    // truncate at the matched slot: everything above it is provably dead,
    // since control has already unwound back to this call site in C.
    for (uint32_t i = g_call_return_depth; i != g_call_return_floor; --i) {
        uint32_t slot = i - 1u;
        if (g_call_return_stack[slot] == pc) {
            if (unpacker_pc_in_slot(pc))
                unpacker_note_callret(kUnpackerCancel, pc, slot,
                                      g_call_return_depth,
                                      slot + 1u == g_call_return_depth ? 1u : 0u);
            runtime_trace_event(RUNTIME_TRACE_CALL, pc, pc,
                                g_call_return_depth,
                                (slot + 1u == g_call_return_depth) ? 4u : 6u);
            g_call_return_depth = slot;
            if (g_runtime_call_return_hook)
                g_runtime_call_return_hook(pc, g_call_return_depth);
            if (g_runtime_ram_image_boundary_probe)
                g_runtime_ram_image_boundary_probe(
                    0u, pc, g_call_return_depth);
            return;
        }
    }
    if (unpacker_pc_in_slot(pc))
        unpacker_note_callret(kUnpackerCancel, pc, 0xFFFFFFFFu,
                              g_call_return_depth, 0u);
    if (g_runtime_call_return_hook)
        g_runtime_call_return_hook(pc, g_call_return_depth);
    if (g_runtime_ram_image_boundary_probe)
        g_runtime_ram_image_boundary_probe(0u, pc, g_call_return_depth);
}

// ── Save-state accessors for the call-return stack ─────────────────
// The stack + depth are file-local (anonymous namespace above). These
// C-ABI windows let the snapshot orchestrator capture and restore them
// without pulling the debug headers into this translation unit.

extern "C" uint32_t runtime_call_stack_depth(void) {
    return g_call_return_depth;
}

extern "C" const uint32_t* runtime_call_stack_data(void) {
    return g_call_return_stack;
}

extern "C" void runtime_call_stack_restore(const uint32_t* entries,
                                           uint32_t depth) {
    if (depth > kCallReturnStackSize) depth = kCallReturnStackSize;
    g_call_return_depth = depth;
    for (uint32_t i = 0; i < depth; ++i) g_call_return_stack[i] = entries[i];
}

// Test-only: see declaration in runtime_arm.h. Production code only moves
// the floor via runtime_irq()'s save/raise/restore.
extern "C" void runtime_call_stack_set_floor_for_test(uint32_t floor) {
    g_call_return_floor = floor;
}

extern "C" uint32_t runtime_call_stack_floor_for_test(void) {
    return g_call_return_floor;
}

// runtime_dispatch_miss is defined in src/runtime/runtime_arm_default_aborts.cpp
// for production builds, or by test stubs for codegen tests.

// ── PSR transfer + bank machinery ──────────────────────────────────

namespace {

// Map a 5-bit ARM mode to the bank index in g_cpu.banked_*.
unsigned mode_to_bank(uint32_t mode) {
    switch (mode & 0x1Fu) {
        case 0x11u: return ARM_BANK_FIQ;
        case 0x12u: return ARM_BANK_IRQ;
        case 0x13u: return ARM_BANK_SUPERVISOR;
        case 0x17u: return ARM_BANK_ABORT;
        case 0x1Bu: return ARM_BANK_UNDEFINED;
        default:    return ARM_BANK_USER;  // User (0x10) / System (0x1F)
    }
}

// Save the active mode's R13/R14 into its bank; if leaving FIQ,
// also pickle R8..R12 into r8_12_fiq.
void bank_out(unsigned old_bank, uint32_t old_mode) {
    g_cpu.banked_sp[old_bank] = g_cpu.R[13];
    g_cpu.banked_lr[old_bank] = g_cpu.R[14];
    if ((old_mode & 0x1Fu) == 0x11u) {
        for (unsigned i = 0; i < 5; ++i) {
            g_cpu.r8_12_fiq[i] = g_cpu.R[8 + i];
        }
    } else {
        for (unsigned i = 0; i < 5; ++i) {
            g_cpu.r8_12_user[i] = g_cpu.R[8 + i];
        }
    }
}

// Restore the incoming mode's R13/R14; if entering FIQ, also
// pull R8..R12 from r8_12_fiq, else from r8_12_user.
void bank_in(unsigned new_bank, uint32_t new_mode) {
    g_cpu.R[13] = g_cpu.banked_sp[new_bank];
    g_cpu.R[14] = g_cpu.banked_lr[new_bank];
    if ((new_mode & 0x1Fu) == 0x11u) {
        for (unsigned i = 0; i < 5; ++i) {
            g_cpu.R[8 + i] = g_cpu.r8_12_fiq[i];
        }
    } else {
        for (unsigned i = 0; i < 5; ++i) {
            g_cpu.R[8 + i] = g_cpu.r8_12_user[i];
        }
    }
}

}  // namespace

// ── No-BIOS stand-ins ──────────────────────────────────────────────
// Our own code for the only BIOS services a game reaches after boot, written
// from the documented behaviour (GBATEK "BIOS Functions", "Interrupt
// Control") and the register/stack layout the BIOS leaves, never by running
// BIOS code. Cycle charges approximate the replaced sequences (BIOS region:
// zero wait states) so frame timing stays where it was.
namespace {

// Synthetic return address the IRQ entry gives the user handler: the BIOS
// address its own IRQ return sits at, so handlers that inspect LR, and
// savestates, see the same value.
constexpr uint32_t kNoBiosIrqReturn = 0x00000138u;
// Where a halted BIOS Halt/IntrWait sits (every GS savestate), and the SWI
// exit its LR points to.
constexpr uint32_t kNoBiosHaltWait = 0x000001B4u;
constexpr uint32_t kNoBiosSwiExit = 0x00000170u;
// The word a protected BIOS read returns after each exit path: the opcode
// the hardware last prefetched there (GBATEK "Reading from BIOS Memory";
// the same four values every emulator models).
constexpr uint32_t kOpenBusInIrq = 0xE25EF004u;
constexpr uint32_t kOpenBusAfterIrq = 0xE55EC002u;
constexpr uint32_t kOpenBusAfterSwi = 0xE3A02004u;

void no_bios_open_bus(uint32_t value) {
    if (g_runtime_bios_open_bus_hook) g_runtime_bios_open_bus_hook(value);
}

[[noreturn]] void no_bios_fail(const char* what, uint32_t pc) {
    std::fprintf(stderr,
                 "runtime_arm: no-BIOS: %s (pc=0x%08X lr=0x%08X cpsr=0x%08X); "
                 "this run executes no BIOS code\n",
                 what, pc, g_cpu.R[14], g_cpu.cpsr);
    runtime_trace_dump_recent(96);
    std::abort();
}

void no_bios_switch_mode(uint32_t new_cpsr) {
    const uint32_t old_cpsr = g_cpu.cpsr;
    const unsigned old_bank = mode_to_bank(old_cpsr);
    const unsigned new_bank = mode_to_bank(new_cpsr);
    if (old_bank != new_bank) {
        bank_out(old_bank, old_cpsr);
        bank_in(new_bank, new_cpsr);
    }
    g_cpu.cpsr = new_cpsr;
}

uint32_t no_bios_pop() {
    const uint32_t value = bus_read_u32(g_cpu.R[13]);
    g_cpu.R[13] += 4u;
    return value;
}

// The IRQ exit: restore r0-r3, r12 and LR from the IRQ stack and return from
// the exception to LR - 4 (restores CPSR from SPSR_irq, records the iret).
void no_bios_irq_return() {
    if ((g_cpu.cpsr & 0x1Fu) != 0x12u)
        no_bios_fail("IRQ return reached outside IRQ mode", kNoBiosIrqReturn);
    g_cpu.R[0] = no_bios_pop();
    g_cpu.R[1] = no_bios_pop();
    g_cpu.R[2] = no_bios_pop();
    g_cpu.R[3] = no_bios_pop();
    g_cpu.R[12] = no_bios_pop();
    g_cpu.R[14] = no_bios_pop();
    runtime_tick(11u);
    no_bios_open_bus(kOpenBusAfterIrq);
    runtime_exception_return(g_cpu.R[14] - 4u);
}

// The SWI exit from the Halt wait, for states saved there by a BIOS-backed
// run: in System mode with LR = 0x170, the System stack holds {r2, lr} and
// the Supervisor stack {spsr} then {r11, r12, lr}. Pop them, switch to
// Supervisor with IRQs masked, restore SPSR, and return from the exception.
void no_bios_swi_exit() {
    if ((g_cpu.cpsr & 0x1Fu) != 0x1Fu)
        no_bios_fail("SWI exit reached outside System mode", kNoBiosSwiExit);
    g_cpu.R[2] = no_bios_pop();
    g_cpu.R[14] = no_bios_pop();
    g_cpu.R[12] = 0xD3u;
    no_bios_switch_mode(0x000000D3u);
    g_cpu.R[11] = no_bios_pop();
    g_cpu.banked_spsr[ARM_BANK_SUPERVISOR] = g_cpu.R[11];
    g_cpu.R[11] = no_bios_pop();
    g_cpu.R[12] = no_bios_pop();
    g_cpu.R[14] = no_bios_pop();
    runtime_tick(21u);
    no_bios_open_bus(kOpenBusAfterSwi);
    runtime_exception_return(g_cpu.R[14]);
}

}  // namespace

static void no_bios_vector(uint32_t pc) {
    switch (pc) {
        case kNoBiosIrqReturn:
            no_bios_irq_return();
            return;
        case kNoBiosHaltWait:
            // `bx lr` with the LR the Halt wait always holds.
            if (g_cpu.R[14] != kNoBiosSwiExit)
                no_bios_fail("Halt wait resumed with an unexpected LR", pc);
            no_bios_swi_exit();
            return;
        case kNoBiosSwiExit:
            no_bios_swi_exit();
            return;
        default:
            no_bios_fail("guest reached a BIOS address", pc);
    }
}

extern "C" void runtime_set_no_bios(int enabled) {
    g_runtime_no_bios = enabled ? 1 : 0;
}

extern "C" uint32_t runtime_read_user_reg(uint32_t reg) {
    reg &= 15u;
    uint32_t mode = g_cpu.cpsr & 0x1Fu;
    if (reg < 8u || reg == 15u) {
        return g_cpu.R[reg];
    }
    if (reg < 13u) {
        return (mode == 0x11u)
            ? g_cpu.r8_12_user[reg - 8u]
            : g_cpu.R[reg];
    }
    if (mode == 0x10u || mode == 0x1Fu) {
        return g_cpu.R[reg];
    }
    return (reg == 13u)
        ? g_cpu.banked_sp[ARM_BANK_USER]
        : g_cpu.banked_lr[ARM_BANK_USER];
}

extern "C" void runtime_write_user_reg(uint32_t reg, uint32_t value) {
    reg &= 15u;
    uint32_t mode = g_cpu.cpsr & 0x1Fu;
    if (reg < 8u || reg == 15u) {
        g_cpu.R[reg] = value;
        return;
    }
    if (reg < 13u) {
        if (mode == 0x11u) {
            g_cpu.r8_12_user[reg - 8u] = value;
        } else {
            g_cpu.R[reg] = value;
        }
        return;
    }
    if (mode == 0x10u || mode == 0x1Fu) {
        g_cpu.R[reg] = value;
    } else if (reg == 13u) {
        g_cpu.banked_sp[ARM_BANK_USER] = value;
    } else {
        g_cpu.banked_lr[ARM_BANK_USER] = value;
    }
}

extern "C" uint32_t runtime_mrs_cpsr(void) {
    return g_cpu.cpsr;
}

extern "C" uint32_t runtime_mrs_spsr(void) {
    unsigned bank = mode_to_bank(g_cpu.cpsr);
    return g_cpu.banked_spsr[bank];
}

extern "C" void runtime_msr_cpsr(uint32_t value, uint32_t mask) {
    runtime_flush_deferred();
    uint32_t bytewise = 0;
    if (mask & 1u) bytewise |= 0x000000FFu;
    if (mask & 2u) bytewise |= 0x0000FF00u;
    if (mask & 4u) bytewise |= 0x00FF0000u;
    if (mask & 8u) bytewise |= 0xFF000000u;

    // User mode cannot touch the control byte (bits 7..0) — clamp.
    if ((g_cpu.cpsr & 0x1Fu) == 0x10u) {
        bytewise &= 0xFF000000u;
    }

    uint32_t old_cpsr = g_cpu.cpsr;
    uint32_t new_cpsr = (old_cpsr & ~bytewise) | (value & bytewise);

    unsigned old_bank = mode_to_bank(old_cpsr);
    unsigned new_bank = mode_to_bank(new_cpsr);

    g_cpu.cpsr = new_cpsr;

    if (old_bank != new_bank) {
        bank_out(old_bank, old_cpsr);
        bank_in(new_bank, new_cpsr);
    }
}

extern "C" void runtime_msr_spsr(uint32_t value, uint32_t mask) {
    unsigned bank = mode_to_bank(g_cpu.cpsr);
    if (bank == ARM_BANK_USER) {
        // SPSR is undefined in User / System modes — drop silently.
        return;
    }
    uint32_t bytewise = 0;
    if (mask & 1u) bytewise |= 0x000000FFu;
    if (mask & 2u) bytewise |= 0x0000FF00u;
    if (mask & 4u) bytewise |= 0x00FF0000u;
    if (mask & 8u) bytewise |= 0xFF000000u;
    uint32_t old = g_cpu.banked_spsr[bank];
    g_cpu.banked_spsr[bank] = (old & ~bytewise) | (value & bytewise);
}

// ── Exception return ───────────────────────────────────────────────

// IRQ nesting depth (defined below) and the depth at which the most recent
// IRQ-mode exception return (iret) fired. runtime_irq() uses the latter to
// know when its handler has fully unwound — see the re-dispatch loop there.
extern "C" uint32_t g_irq_nest_depth;
extern "C" uint32_t g_irq_iret_depth;

extern "C" void runtime_exception_return(uint32_t new_pc) {
    uint32_t old_cpsr = g_cpu.cpsr;
    uint32_t old_mode = old_cpsr & 0x1Fu;
    if (old_mode == 0x10u || old_mode == 0x1Fu) {
        // User / System have no SPSR; the exception-return form is
        // architecturally undefined. Just set PC.
        g_cpu.R[15] = new_pc;
        return;
    }
    unsigned old_bank = mode_to_bank(old_cpsr);
    uint32_t spsr = g_cpu.banked_spsr[old_bank];

    bank_out(old_bank, old_cpsr);
    g_cpu.cpsr = spsr;
    unsigned new_bank = mode_to_bank(spsr);
    bank_in(new_bank, spsr);

    g_cpu.R[15] = new_pc;

    // Returning FROM IRQ mode (0x12) is the iret that ends a hardware IRQ.
    // Record the nesting depth so runtime_irq()'s drive-to-completion loop
    // can tell when *its* IRQ (vs. a nested inner one) has returned. Set
    // after the bank swap so g_irq_nest_depth still reflects the level being
    // left (runtime_irq decrements only after its loop exits).
    if (old_mode == 0x12u) g_irq_iret_depth = g_irq_nest_depth;
}

extern "C" void runtime_restore_cpsr_from_spsr(void) {
    uint32_t old_cpsr = g_cpu.cpsr;
    uint32_t old_mode = old_cpsr & 0x1Fu;
    if (old_mode == 0x10u || old_mode == 0x1Fu) {
        return;
    }
    unsigned old_bank = mode_to_bank(old_cpsr);
    uint32_t spsr = g_cpu.banked_spsr[old_bank];

    bank_out(old_bank, old_cpsr);
    g_cpu.cpsr = spsr;
    unsigned new_bank = mode_to_bank(spsr);
    bank_in(new_bank, spsr);
}

// ── BIOS / SWI ─────────────────────────────────────────────────────
// Mirror ARM ARM A2.6.4 SWI entry. The recompiled SWI instruction's
// codegen already set g_cpu.R[15] = pc_of_swi + 4 (ARM) or +2 (THUMB)
// before calling this, so R[15] is the return address.
//
// Steps (ARM ARM A2.6.4):
//   LR_svc      ← return_address
//   SPSR_svc    ← CPSR
//   CPSR.mode   ← SVC (0x13)
//   CPSR.T      ← 0           (handler always runs in ARM state)
//   CPSR.I      ← 1           (mask IRQs while in SWI handler)
//   PC          ← 0x00000008
//
// Then we runtime_dispatch(0x08) into the recompiled BIOS SWI
// vector. NO interpreter fallback (PRINCIPLES.md "Interpreter is
// informative, never load-bearing"). If the BIOS dispatch table is
// empty, the dispatch falls through to runtime_dispatch_miss; the
// strong (production) version aborts there — that abort is the
// "BIOS not recompiled" gate.

extern "C" void runtime_swi(uint32_t swi_imm) {
    runtime_flush_deferred();
    uint32_t return_address = g_cpu.R[15];
    uint32_t saved_cpsr     = g_cpu.cpsr;
    const unsigned long long log_cycles = g_runtime_cycles;
    runtime_trace_event(RUNTIME_TRACE_SWI, return_address, swi_imm, saved_cpsr, 0);
    const uint32_t log_r0 = g_cpu.R[0];
    const uint32_t log_r1 = g_cpu.R[1];
    const uint32_t log_r2 = g_cpu.R[2];
    const uint32_t log_lr = g_cpu.R[14];
    const uint32_t log_iwflags = bus_read_u32(0x03007FF8u);
    const bool thumb = (saved_cpsr & CPSR_T_BIT) != 0;
    const uint32_t swi_num = thumb ? (swi_imm & 0xFFu)
                                   : ((swi_imm >> 16) & 0xFFu);
    constexpr uint32_t kSwiRouteLle = 0u;
    constexpr uint32_t kSwiRouteHleHandled = 1u;
    constexpr uint32_t kSwiRouteHleFallback = 2u;
    uint32_t log_route = kSwiRouteLle;

    // A game-owned override may drop this one call (the guest resumes at LR,
    // already in g_cpu.R[15]). Only reviewed return addresses reach it.
    if (g_runtime_swi_override &&
        g_runtime_swi_override(return_address, swi_num)) {
        return;
    }

    // ── No-BIOS mode: every SWI is ours, none falls through ─────────
    // Halt writes HALTCNT itself; the next instruction boundary then yields
    // on `halted`, the frame loop idles until an enabled IRQ is pending, and
    // the IRQ returns to the caller's next instruction, which is where the
    // BIOS's own Halt resumes once its exit has run. Other SWIs go to the HLE
    // layer; one it does not implement stops the run loudly.
    if (g_runtime_no_bios) {
        constexpr uint32_t kSwiRouteNoBios = 3u;
        constexpr uint32_t kSwiHalt = 0x02u;
        constexpr uint32_t kSwiStop = 0x03u;
        runtime_swi_log_record(log_cycles, swi_imm, return_address, log_r0,
                               log_r1, log_r2, log_lr, log_iwflags,
                               kSwiRouteNoBios);
        if (swi_num == kSwiHalt || swi_num == kSwiStop) {
            // HALTCNT 0x00 halts, 0x80 stops (the I/O model wakes both on an
            // enabled IRQ; the keypad IRQ is what ends a Stop).
            runtime_tick(41u);
            bus_write_u8(0x04000301u, swi_num == kSwiStop ? 0x80u : 0x00u);
            no_bios_open_bus(kOpenBusAfterSwi);
            return;
        }
        if (g_bios_hle_hook && g_bios_hle_hook(swi_num)) {
            no_bios_open_bus(kOpenBusAfterSwi);
            return;
        }
        no_bios_fail("SWI with no stand-in", swi_num);
    }

    // ── BIOS HLE (opt-in alternative to the recompiled/LLE BIOS) ────────
    // When an HLE handler is installed and it services this SWI, the guest
    // resumes at LR (already in g_cpu.R[15], set by the SWI codegen) with NO
    // SVC-mode entry and NO BIOS dispatch — the hook computes the effect on
    // g_cpu + memory directly and charges its own cycle cost. A return of 0
    // (default, and for any SWI the HLE layer does not implement) falls through
    // to the recompiled BIOS below. LLE therefore stays fully load-bearing and
    // remains the correctness oracle; HLE is purely additive. The SWI number is
    // decoded the way the real BIOS reads it: THUMB from the low byte, ARM from
    // bits 23..16 (arm_decode stores the full 24-bit comment in swi_imm).
    if (g_bios_hle_hook) {
        log_route = kSwiRouteHleFallback;
        if (g_bios_hle_hook(swi_num)) {
            runtime_swi_log_record(log_cycles, swi_imm, return_address, log_r0,
                                   log_r1, log_r2, log_lr, log_iwflags,
                                   kSwiRouteHleHandled);
            return;
        }
    }

    runtime_swi_log_record(log_cycles, swi_imm, return_address, log_r0, log_r1,
                           log_r2, log_lr, log_iwflags, log_route);

    // Switch to SVC mode. SPSR_svc gets the pre-SWI CPSR. LR_svc gets
    // the return address. R8..R12 don't change unless leaving FIQ.
    uint32_t new_cpsr =
        (saved_cpsr & ~(0x1Fu | CPSR_T_BIT))  // clear mode + T
        | 0x13u                                // SVC
        | CPSR_I_BIT;                          // mask IRQs

    unsigned old_bank = mode_to_bank(saved_cpsr);
    unsigned new_bank = mode_to_bank(new_cpsr);
    if (old_bank != new_bank) {
        bank_out(old_bank, saved_cpsr);
        bank_in(new_bank, new_cpsr);
    }

    g_cpu.cpsr                  = new_cpsr;
    g_cpu.banked_spsr[new_bank] = saved_cpsr;
    g_cpu.R[14]                 = return_address;  // LR_svc
    g_cpu.R[15]                 = 0x00000008u;

    // Charge the SWI instruction's own cost (instr_cycle_base(SWI) = 3:
    // 2S+1N). Ticked here — AFTER CPSR.I is masked above — so a VBlank/
    // timer IRQ that becomes pending during these 3 cycles stays masked
    // until the handler re-enables it, matching the interpreter oracle
    // (enter_swi sets I=1, then pump_step(3), then the next-boundary IRQ
    // check sees I=1). The recompiled SWI codegen does not tick this op.
    runtime_tick(3u);

    runtime_dispatch(0x00000008u);
}

// Count of IRQ vectorings performed by the recompiled runtime (every
// runtime_irq call = one exception entry to 0x18). The recomp delivers IRQs
// here (called from runtime_tick), NOT in runtime.cpp's run loop, so this is
// the authoritative per-engine IRQ-entry tally. The TCP `counters` command
// surfaces it (runtime.cpp wires ctx.irq_entries here) so a probe can compare
// IRQ delivery against the interpreter oracle — the MC-HP-002 1-game-frame-lead
// test (does the recomp vector an extra/early VBlank IRQ?). Never reset; probes
// compare per-frame deltas.
extern "C" unsigned long long g_runtime_irq_entries = 0;
// Live host-recursion nesting depth of IRQ delivery (++ on entry, -- after the
// handler unwinds) and the high-water mark. Distinguishes the MC-HP-002 storm
// shape: deep nesting (depth climbs → a long handler spanning the next IRQ
// boundary) vs. flat re-fire (depth stays ~1 → an unacked source re-vectoring).
// Non-static so runtime_should_yield() (runtime_bus_bridge.cpp) can tell whether
// an IRQ handler is currently on the host stack — the cpsr mode alone can't,
// because GBA IRQ dispatchers (e.g. FireRed's intr_main) switch to System mode
// mid-handler to allow nested IRQs.
extern "C" uint32_t      g_irq_nest_depth = 0;
extern "C" unsigned long long g_runtime_irq_max_depth = 0;
// Depth at which the most recent IRQ-mode iret fired (set in
// runtime_exception_return). runtime_irq() resets this to 0 on entry and
// spins its drive-to-completion loop until it equals the IRQ's own depth.
extern "C" uint32_t      g_irq_iret_depth = 0;

// Yield-unwind resume PC. While a scheduler yield unwinds the host C stack,
// runtime_should_yield() (runtime_bus_bridge.cpp) parks the real resume PC here
// and leaves g_cpu.R[15] = 0xFFFFFFF0 so no generated call site's
// `if (g_cpu.R[15] != <ret>)` can match a stale frame. Every loop that
// re-dispatches R15 after runtime_dispatch() returns calls
// runtime_yield_restore_pc() first. See FACTS.md "Door+Turbo crash".
extern "C" uint32_t g_yield_resume_pc = 0;
extern "C" bool     g_yield_resume_pending = false;

extern "C" void runtime_yield_restore_pc(void) {
    if (g_yield_resume_pending) {
        g_cpu.R[15] = g_yield_resume_pc;
        g_yield_resume_pending = false;
        g_dispatch_is_yield_resume = true;
        if (unpacker_pc_in_slot(g_yield_resume_pc & ~1u))
            unpacker_note_yield_resume(g_yield_resume_pc & ~1u);
    }
}

extern "C" void runtime_irq(uint32_t return_address) {
    ++g_runtime_irq_entries;
    ++g_irq_nest_depth;
    if (g_irq_nest_depth > g_runtime_irq_max_depth)
        g_runtime_irq_max_depth = g_irq_nest_depth;
    // Debug: dump the always-on ring the moment IRQ nesting reaches a
    // threshold, capturing the storm chain BEFORE later busy execution scrolls
    // it out (MC-HP-002). Env-gated, zero-overhead when unset.
    static int irq_depth_gate = -1;
    if (irq_depth_gate < 0) {
        const char* e = std::getenv("GBARECOMP_ABORT_ON_IRQ_DEPTH");
        irq_depth_gate = e ? std::atoi(e) : 0;
    }
    if (irq_depth_gate > 0 && (int)g_irq_nest_depth >= irq_depth_gate) {
        std::fprintf(stderr,
                     "runtime_irq: nesting depth %u reached (entries=%llu) "
                     "ret=0x%08X cpsr=0x%08X\n",
                     g_irq_nest_depth,
                     (unsigned long long)g_runtime_irq_entries,
                     return_address, g_cpu.cpsr);
        runtime_trace_dump_recent(160);
        std::abort();
    }
    uint32_t saved_cpsr = g_cpu.cpsr;
    // Record the active IRQ source(s) (IE & IF) in the trace addr field and the
    // nesting depth in aux so a ring dump names which interrupt is being
    // vectored and how deep — used to pin the MC-HP-002 IRQ storm. Reading
    // IE/IF is side-effect-free.
    uint32_t irq_src = bus_read_u16(0x04000200u) & bus_read_u16(0x04000202u);
    runtime_trace_event(RUNTIME_TRACE_IRQ, return_address, irq_src, saved_cpsr,
                        g_irq_nest_depth);
    runtime_irq_log_record(irq_src, return_address, saved_cpsr);

    uint32_t new_cpsr =
        (saved_cpsr & ~(0x1Fu | CPSR_T_BIT))
        | 0x12u
        | CPSR_I_BIT;

    unsigned old_bank = mode_to_bank(saved_cpsr);
    unsigned new_bank = mode_to_bank(new_cpsr);
    if (old_bank != new_bank) {
        bank_out(old_bank, saved_cpsr);
        bank_in(new_bank, new_cpsr);
    }

    g_cpu.cpsr                  = new_cpsr;
    g_cpu.banked_spsr[new_bank] = saved_cpsr;
    g_cpu.R[14]                 = return_address + 4u;
    g_cpu.R[15]                 = 0x00000018u;

    // No-BIOS mode: do the IRQ vector's work here instead of dispatching it.
    // Push r0-r3, r12 and LR_irq (full-descending, lowest register lowest),
    // point r0 at the I/O base and LR at the synthetic return, and jump to the
    // user handler whose address the game stored at 0x03007FFC (read through
    // its 0x03FFFFFC mirror, as the vector does; ARM state, bits 1:0 ignored).
    // The handler's return to 0x138 reaches no_bios_irq_return through
    // runtime_dispatch, which performs the exit and the iret.
    if (g_runtime_no_bios) {
        const uint32_t sp = g_cpu.R[13] - 24u;
        bus_write_u32(sp + 0u, g_cpu.R[0]);
        bus_write_u32(sp + 4u, g_cpu.R[1]);
        bus_write_u32(sp + 8u, g_cpu.R[2]);
        bus_write_u32(sp + 12u, g_cpu.R[3]);
        bus_write_u32(sp + 16u, g_cpu.R[12]);
        bus_write_u32(sp + 20u, g_cpu.R[14]);
        g_cpu.R[13] = sp;
        g_cpu.R[0] = 0x04000000u;
        g_cpu.R[14] = kNoBiosIrqReturn;
        g_cpu.R[15] = bus_read_u32(0x03FFFFFCu) & ~3u;
        runtime_tick(14u);
        no_bios_open_bus(kOpenBusInIrq);
    }

    // Drive the IRQ handler to completion.
    //
    // A single runtime_dispatch(0x18) finishes the handler ONLY if nothing
    // inside it triggers a "return-to-top" unwind. A BIOS SWI executed inside
    // the handler does exactly that: the SWI return path leaves R15 at the
    // after-SWI PC and unwinds the host C stack via the per-call-site cancel
    // cascade (runtime_call_cancel_return) — the SAME mechanism a SWI in the
    // main thread uses to unwind back to step_frame's dispatch loop, which
    // then re-dispatches R15. runtime_irq has no such loop, so the cascade
    // would abandon the handler mid-flight: e.g. FireRed's VBlank handler
    // (VBlankCB_Copyright -> LoadOam -> CpuSet/SWI) never reaches intr_main's
    // post-handler `REG_IE = saved` restore, leaving VBlank masked in IE
    // forever -> the game's VBlank busy-wait spins -> permanent freeze.
    //
    // Mirror the main loop here: keep re-dispatching R15 until THIS IRQ's
    // handler has executed its iret (an IRQ-mode runtime_exception_return at
    // our nesting depth). g_irq_nest_depth stays > 0 across the whole loop so
    // runtime_should_yield() never unwinds us mid-handler. Nested IRQs are
    // handled because the iret depth is matched to this frame's depth; the
    // saved/restored g_irq_iret_depth keeps an enclosing IRQ's loop intact.
    uint32_t my_depth      = g_irq_nest_depth;
    uint32_t saved_iret    = g_irq_iret_depth;
    g_irq_iret_depth       = 0u;  // sentinel: this IRQ has not iret'd yet
    // Raise the call-return floor so the handler's guest returns/cancels can
    // unwind back to here but never match/pop the interrupted mainline (or an
    // enclosing IRQ's) frames — see g_call_return_floor. Restored below.
    uint32_t saved_floor   = g_call_return_floor;
    g_call_return_floor    = g_call_return_depth;
    const bool _cp = g_cost_probe_enabled();
    const auto _irqh_t0 = _cp ? std::chrono::steady_clock::now()
                               : std::chrono::steady_clock::time_point{};
    runtime_dispatch(g_runtime_no_bios ? g_cpu.R[15] : 0x00000018u);
    runtime_yield_restore_pc();  // a yield unwind hid the resume PC
    constexpr uint32_t kMaxIrqDispatches = 4'000'000u;
    uint32_t irq_guard = 0u;
    while (g_irq_iret_depth != my_depth) {
        if (++irq_guard >= kMaxIrqDispatches) {
            std::fprintf(stderr,
                "runtime_irq: handler at depth %u did not iret after %u "
                "dispatches (R15=0x%08X, cpsr=0x%08X) — abandoning\n",
                my_depth, irq_guard, g_cpu.R[15], g_cpu.cpsr);
            runtime_trace_dump_recent(160);
            break;
        }
        runtime_dispatch(g_cpu.R[15]);
        runtime_yield_restore_pc();
    }
    if (_cp) {
        g_cost_irq_handler_ns += static_cast<unsigned long long>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - _irqh_t0).count());
        ++g_cost_irq_handler_calls;
    }
    g_irq_iret_depth = saved_iret;  // restore an enclosing IRQ's expectation
    g_call_return_floor = saved_floor;
    --g_irq_nest_depth;
}

// runtime_unimplemented_op is defined in
// src/runtime/runtime_arm_default_aborts.cpp for production builds,
// or by test stubs for codegen tests.

// ── Lifecycle ──────────────────────────────────────────────────────

extern "C" void runtime_init(void* bus_handle) {
    gbarecomp::runtime_arm::g_bus_handle = bus_handle;
    g_call_return_depth = 0;
}

extern "C" void runtime_shutdown(void) {
    gbarecomp::runtime_arm::g_bus_handle = nullptr;
    g_call_return_depth = 0;
}
