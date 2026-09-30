// runtime_arm.h — C-ABI surface that generated recompiled cart code
// calls into.
//
// Generated code is .cpp but uses C linkage on these symbols so the
// recompiler doesn't have to worry about C++ name mangling. The
// implementations live in runtime_arm.cpp and delegate to the
// existing C++ runtime (armv4t::CPUState, gba::GbaBus, etc.).
//
// ABI principle: the symbols declared here are the ONLY interface
// between gba_recompile's generated output and the runtime. If new
// codegen needs an operation, declare a helper here and implement
// once — never inline runtime-internal types into generated code.

#pragma once

#include <stdint.h>

// Shared POD types + bit/constant macros (ArmCpuState, CPSR_*_BIT,
// RUNTIME_TRACE_*, RuntimeTraceEntry, RuntimeFpEntry). Split out so the
// Stage-2 overlay shim can reuse the exact same struct layouts without
// pulling in this header's `extern g_cpu` + inline accessors.
#include "runtime_arm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// ── CPU state ──────────────────────────────────────────────────────
// Generated code reads/writes the register file via this global.
// The CPSR is packed:
//
//   bit 31 N
//   bit 30 Z
//   bit 29 C
//   bit 28 V
//   bit  7 I (IRQ disable)
//   bit  6 F (FIQ disable)
//   bit  5 T (THUMB state)
//   bits 4..0 mode
//
// ArmCpuState + the ARM_BANK_* / CPSR_*_BIT macros are defined in
// runtime_arm_types.h (shared with the overlay shim).

extern ArmCpuState g_cpu;

// Optional observer for the inline fast path's EWRAM stores. This is a
// diagnostics-neutral upstream seam: it receives only the guest address and
// width, is never called for IWRAM or slow-path stores, and remains null in
// ordinary runs. The owning game may install it only while an opt-in probe is
// active.
typedef void (*RuntimeFastEwramWriteObserver)(uint32_t address,
                                                uint32_t size);
extern RuntimeFastEwramWriteObserver g_runtime_fast_ewram_write_observer;
// Same diagnostics-neutral seam for generated fast-path IWRAM stores. The
// callback is null in ordinary runs. When installed, one callback receives
// exactly one start and one committed notification per store, bracketing the
// backing-memory write.
enum : uint32_t {
    RUNTIME_FAST_IWRAM_WRITE_START = 0u,
    RUNTIME_FAST_IWRAM_WRITE_COMMITTED = 1u,
};
typedef void (*RuntimeFastIwramWriteObserver)(uint32_t address,
                                               uint32_t size,
                                               uint32_t phase);
extern RuntimeFastIwramWriteObserver g_runtime_fast_iwram_write_observer;
// 512-bit watch bitmap over IWRAM at 64-byte granularity: bit ((off >> 6) & 63)
// of word (off >> 12), off = addr & 0x7FFF. The inline store paths call the
// observer only when it is installed AND a block the store touches is set, so
// stores the observer would ignore (stack traffic) skip the two indirect
// calls. Zero by default; the game owner fills it when it installs an
// observer and the runtime clears it with the observer.
extern uint64_t g_runtime_fast_iwram_watch[8];
static inline int runtime_fast_iwram_watched(uint32_t addr, uint32_t width) {
    const uint32_t lo = addr & 0x7FFFu;
    const uint32_t hi = (lo + width - 1u) & 0x7FFFu;
    return (int)(((g_runtime_fast_iwram_watch[lo >> 12] >> ((lo >> 6) & 63u)) |
                  (g_runtime_fast_iwram_watch[hi >> 12] >> ((hi >> 6) & 63u))) &
                 1u);
}

// Optional game-owned observer for generated guest memory writes. The
// callback receives the exact instruction PC, aligned guest address, value,
// and access width before the backing-memory write. It never changes the
// write, and remains null in ordinary runs.
typedef void (*RuntimeMemWriteObserverHook)(uint32_t pc, uint32_t addr,
                                             uint32_t value, uint32_t width);
extern RuntimeMemWriteObserverHook g_runtime_mem_write_observer;

// Optional game-owned memory-write transform. The callback is invoked from
// runtime_trace_event immediately before a generated guest store. Returning
// nonzero requests `*out_value`; the generic runtime arms that value only for
// the immediately following 32-bit store. nullptr preserves guest behavior.
typedef int (*RuntimeMemWriteOverrideHook)(uint32_t pc, uint32_t addr,
                                           uint32_t requested, uint32_t width,
                                           uint32_t* out_value);
extern RuntimeMemWriteOverrideHook g_runtime_mem_write_override;

// Single-use override state. These are touched only by the emulation thread
// between the generated trace call and its immediate bus write.
extern uint32_t g_runtime_mem_write_override_pending_addr;
extern uint32_t g_runtime_mem_write_override_pending_value;
extern uint32_t g_runtime_mem_write_override_pending_valid;

// Optional game-owned override for immediate operands in recompiled THUMB
// data-processing instructions. Generated code passes the exact guest
// instruction PC and decoded immediate, and uses *out_value only when the
// callback returns non-zero. nullptr preserves the original instruction.
typedef int (*RuntimeThumbAluImmediateOverride)(uint32_t instruction_pc,
                                                uint32_t original_value,
                                                uint32_t* out_value);
extern RuntimeThumbAluImmediateOverride g_runtime_thumb_alu_imm_override;

// Optional game-owned override for exact THUMB PC-relative literal loads.
// Generated code performs the ordinary bus read/timing first, then consults
// this hook only at reviewed PCs. nullptr preserves the immutable literal.
typedef int (*RuntimeThumbLiteralOverride)(uint32_t instruction_pc,
                                           uint32_t original_value,
                                           uint32_t* out_value);
extern RuntimeThumbLiteralOverride g_runtime_thumb_literal_override;

// Optional game-owned override for the decision of an exact conditional
// branch. Generated code passes the instruction PC and the decision produced
// from the guest CPSR; the callback may replace that decision at reviewed PCs.
// nullptr or a rejecting callback preserves the original branch semantics.
typedef int (*RuntimeConditionalBranchOverride)(uint32_t instruction_pc,
                                                uint32_t original_decision,
                                                uint32_t* out_decision);
extern RuntimeConditionalBranchOverride g_runtime_conditional_branch_override;

// Optional game-owned override for one SWI, consulted by runtime_swi before
// the BIOS (HLE or recompiled) sees it. return_pc is the address after the
// SWI instruction; returning 1 skips the call entirely, as if the BIOS had
// returned at once with the registers unchanged. nullptr or 0 keeps it.
typedef int (*RuntimeSwiOverride)(uint32_t return_pc, uint32_t swi_num);
extern RuntimeSwiOverride g_runtime_swi_override;

// Optional game-owned dispatcher/canonicalizer for identity-verified code
// copied to transient RAM addresses (including overlapping static overlays and
// routines executed from a moving stack frame). It runs before the fixed
// dispatch table. The hook resolves a verified native target but does not call
// it; runtime_dispatch owns the final invoke so the transfer can be emitted as
// a host tail call. nullptr preserves normal dispatch.
typedef void (*RuntimeGuestFn)(void);
typedef RuntimeGuestFn (*RuntimeRamDispatchHook)(uint32_t pc, int thumb);
extern RuntimeRamDispatchHook g_runtime_ram_dispatch_hook;

// Optional game-owned query, consulted only when the static dispatch table
// already has a fixed AOT entry for a transient-RAM pc AND that pc's page has
// been marked dirty (some write landed in the containing 4 KiB page since
// boot/load). RuntimeRamDispatchHook above already ran for this exact pc/
// thumb earlier in the same runtime_dispatch call; a nonzero return here
// means that hook independently confirmed (via its own identity/CRC check)
// that the live bytes at this pc still match the reviewed ROM-sourced image
// the AOT entry was built from, so the page write that set the dirty bit
// landed somewhere else in the same page. A zero return (including a null
// hook) preserves the existing heal/JIT fallback exactly as before.
typedef int (*RuntimeRamIdentityConfirmedHook)(uint32_t pc, int thumb);
extern RuntimeRamIdentityConfirmedHook g_runtime_ram_identity_confirmed_hook;

// Optional notification after generated guest return/cancel handling has
// unwound the call-return stack.  The transient-RAM resolver uses this to
// retire its active-entry marker while keeping runtime_dispatch's native
// transfer as a true host tail jump.  A null hook preserves the normal path.
typedef void (*RuntimeCallReturnHook)(uint32_t return_pc,
                                     uint32_t call_stack_depth);
extern RuntimeCallReturnHook g_runtime_call_return_hook;

// Optional notification at the outer runner's host dispatch boundary. A
// generated body may return to the runner for scheduling (yield/SWI) without
// executing a guest BX-LR, so return-hook bookkeeping cannot retire a
// top-level transient marker in that case. The callback is invoked only after
// the outer step has regained control; nested generated calls do not use it.
typedef void (*RuntimeGuestStepBoundaryHook)(void);
extern RuntimeGuestStepBoundaryHook g_runtime_guest_step_boundary_hook;

// Optional native-audio observer. The runtime bridge installs this callback
// when a bus is active; runtime_trace_event invokes it only when an MP2K
// enhancement request is present, so ordinary runs pay no callback cost.
typedef void (*RuntimeMp2kWriteHook)(uint32_t pc, uint32_t addr,
                                     uint32_t value, uint32_t width,
                                     uint32_t operand_sample,
                                     uint32_t operand_gain,
                                     uint32_t before_value,
                                     uint8_t thumb,
                                     uint8_t pre_write);
extern RuntimeMp2kWriteHook g_runtime_mp2k_write_hook;
extern "C" void runtime_mp2k_write_observer(uint32_t pc, uint32_t addr,
                                              uint32_t value, uint32_t width,
                                              uint32_t operand_sample,
                                              uint32_t operand_gain,
                                              uint32_t before_value,
                                              uint8_t thumb, uint8_t pre_write);

// Optional native-audio control-flow observer. The runtime reports dispatch
// events; the game-owned audio layer applies any measured target-PC gate.
typedef void (*RuntimeMp2kControlHook)(uint32_t pc, uint32_t target,
                                       uint64_t cycles, uint8_t thumb);
extern RuntimeMp2kControlHook g_runtime_mp2k_control_hook;
extern "C" void runtime_mp2k_control_observer(uint32_t pc, uint32_t target,
                                                uint64_t cycles, uint8_t thumb);

// Optional, diagnostic-only attribution for writes of a pointer into the
// measured generated image. The callback receives no guest bytes: only the
// frame, destination, writer PC/mode, and pointer target/mode.
typedef void (*RuntimeRamPointerWriteProbe)(uint64_t frame, uint32_t dest,
                                            uint32_t writer_pc,
                                            uint8_t writer_thumb,
                                            uint32_t pointer_target,
                                            uint8_t pointer_thumb);
extern RuntimeRamPointerWriteProbe g_runtime_ram_pointer_write_probe;
void runtime_note_ram_pointer_write(uint32_t dest, uint32_t value,
                                    uint32_t width);

// Optional, diagnostic-only lifecycle probe for the one measured transient
// image. The callback reports a DMA trigger/completion without guest bytes;
// completion carries the CRC32 of the full live image range.
typedef void (*RuntimeRamImageDmaProbe)(uint64_t frame, uint8_t completed,
                                        uint32_t source, uint32_t dest,
                                        uint32_t size, uint32_t writer_pc,
                                        uint8_t writer_thumb, uint32_t crc32,
                                        uint32_t channel);
extern RuntimeRamImageDmaProbe g_runtime_ram_image_dma_probe;
void runtime_note_ram_image_dma(uint8_t completed, uint32_t source,
                                uint32_t dest, uint32_t size,
                                uint32_t crc32, uint32_t channel);

// Optional, diagnostic-only CPU-store lifecycle observer for the measured
// transient image. It receives addresses and widths only; the stored value is
// deliberately excluded. Generated stores are observed before their write;
// direct/interpreter bus stores are observed after commit. Completion is
// committed only at a return/outer-step boundary.
typedef void (*RuntimeRamImageWriteProbe)(uint64_t frame, uint32_t pc,
                                          uint8_t thumb, uint32_t addr,
                                          uint32_t width);
extern RuntimeRamImageWriteProbe g_runtime_ram_image_write_probe;
void runtime_note_ram_image_cpu_write(uint32_t pc, uint32_t addr,
                                      uint32_t width);

// Optional, diagnostic-only observer for direct/interpreter stores into the
// bounded Func_2808 stack slots used by the pool LDM probe. It is inert until
// the pool dispatch history arms the probe.
void runtime_note_pool_ldm_bus_write(uint32_t addr, uint32_t width);
// The byte window runtime_note_pool_ldm_bus_write acts on (physical IWRAM,
// end exclusive). A caller may skip the call for stores that cannot overlap it.
#define RUNTIME_POOL_LDM_WINDOW_BASE 0x03007E00u
#define RUNTIME_POOL_LDM_WINDOW_END 0x03007E40u

// Diagnostic-only savestate boundary for the pool-LDM provenance probe. This
// clears host-side history/rings, arms the probe before resumed guest
// execution, and emits one payload-free restore CRC. It never writes guest
// memory.
void runtime_pool_ldm_probe_restore_boundary(void);

// Diagnostic-only CRC-32 of the tracked pool-LDM stack window
// (0x03007E00..0x03007E40), payload-free. The runner calls it alongside each
// [wide-auth-epoch] emission so a stale-since-load window can be
// discriminated from one poisoned after load.
void runtime_note_pool_ldm_epoch_crc(void);

// Optional, diagnostic-only boundary observer paired with the write probe.
// outer=0 denotes generated guest return/cancel handling; outer=1 denotes
// return to the runner's outer guest-step boundary.
typedef void (*RuntimeRamImageBoundaryProbe)(uint8_t outer,
                                              uint32_t return_pc,
                                              uint32_t call_stack_depth);
extern RuntimeRamImageBoundaryProbe g_runtime_ram_image_boundary_probe;

// Optional, diagnostic-only first-dispatch observer paired with the lifecycle
// probe above. It is called only while the probe is enabled.
typedef void (*RuntimeRamImageDispatchProbe)(uint64_t frame,
                                             uint32_t pc, uint8_t thumb);
extern RuntimeRamImageDispatchProbe g_runtime_ram_image_dispatch_probe;

// Base address a POSITION-INDEPENDENT RAM code image is currently executing
// at. A relocatable corpus (gba_recompile --relocatable-image) derives every
// guest address it owns — PC pipeline values, literal-pool bases, branch
// targets, link registers — from this instead of baking the address the
// corpus was generated at. The RAM dispatch hook sets it to the verified base
// immediately before calling into the image; each generated image function
// snapshots it on entry, and restores it before calling a sibling, so a
// nested dispatch into a DIFFERENT image at a different base cannot corrupt
// the caller's view. Meaningless (and untouched) for fixed-address corpora.
extern uint32_t g_runtime_image_base;

// Only exact PCs opted in by the recompiler config call this helper. The
// universal/default generated path keeps the compile-time operand and emits no
// call at all; a configured site's null/reject path still returns it unchanged.
static inline uint32_t runtime_thumb_alu_immediate(uint32_t instruction_pc,
                                                   uint32_t original_value) {
    uint32_t overridden = original_value;
    if (g_runtime_thumb_alu_imm_override &&
        g_runtime_thumb_alu_imm_override(instruction_pc, original_value,
                                         &overridden)) {
        return overridden;
    }
    return original_value;
}

static inline uint32_t runtime_thumb_literal(uint32_t instruction_pc,
                                              uint32_t original_value) {
    uint32_t overridden = original_value;
    if (g_runtime_thumb_literal_override &&
        g_runtime_thumb_literal_override(instruction_pc, original_value,
                                         &overridden)) {
        return overridden;
    }
    return original_value;
}

static inline uint32_t runtime_conditional_branch(
        uint32_t instruction_pc, uint32_t original_decision) {
    uint32_t overridden = original_decision;
    if (g_runtime_conditional_branch_override &&
        g_runtime_conditional_branch_override(instruction_pc,
                                              original_decision,
                                              &overridden)) {
        return overridden ? 1u : 0u;
    }
    return original_decision ? 1u : 0u;
}

static inline uint32_t cpsr_n(void) { return (g_cpu.cpsr & CPSR_N_BIT) ? 1u : 0u; }
static inline uint32_t cpsr_z(void) { return (g_cpu.cpsr & CPSR_Z_BIT) ? 1u : 0u; }
static inline uint32_t cpsr_c(void) { return (g_cpu.cpsr & CPSR_C_BIT) ? 1u : 0u; }
static inline uint32_t cpsr_v(void) { return (g_cpu.cpsr & CPSR_V_BIT) ? 1u : 0u; }

// ARM condition-code evaluators — true if the cond passes given
// the current CPSR. cond is the 4-bit code from the instruction
// encoding. AL (1110) is unconditional (true); NV (1111) is "never"
// on ARMv4T.
int arm_cond_passes(unsigned cond);

// ── Bus ────────────────────────────────────────────────────────────
// All cart reads/writes flow through these. The runtime sets up the
// bus pointer before any cart code runs; bus_set_active() is called
// from the runner's main().

// General path: region classification, MMIO catch-up, idle-epoch bookkeeping,
// BIOS-access/open-bus sync, then a VIRTUAL call through the active bus.
// Correct for every region, and the only path for anything with side effects
// (MMIO, VRAM/OAM/PAL, ROM/GPIO/EEPROM, save, open bus).
uint32_t bus_read_u32_slow(uint32_t addr);
uint16_t bus_read_u16_slow(uint32_t addr);
uint8_t  bus_read_u8_slow (uint32_t addr);
void     bus_write_u32_slow(uint32_t addr, uint32_t val);
void     bus_write_u16_slow(uint32_t addr, uint16_t val);
void     bus_write_u8_slow (uint32_t addr, uint8_t  val);

// EWRAM / IWRAM fast path. 33,198 static access sites in the translated corpus
// took the slow path unconditionally — an out-of-line call plus a virtual
// dispatch — though the common case is plain RAM, which needs none of it:
//   * region 0x02/0x03 is never IO -> no MMIO catch-up, no horizon resync;
//   * both are is_idle_safe_read -> a read bumps no idle epoch;
//   * neither is BIOS nor open bus, so sync_bios_access()'s effects cannot
//     change what THIS access returns, and every access that could observe
//     them performs the sync itself;
//   * nothing there is unmapped -> no trace_unmapped_read record.
// What remains is a mirror mask and an indexed load/store. Masks are exactly
// resolve_offset()'s (EWRAM mirrors every 256 KB, IWRAM every 32 KB), so the
// value is bit-identical to GbaBus::read*/write*. A WRITE still bumps the idle
// epoch, matching the slow path.
//
// Pointers are installed by set_active_bus() and null before then; null falls
// back to the slow path. Raw pointers, so armv4t gains no dependency on gba.
extern uint8_t* g_fast_ewram;
extern uint8_t* g_fast_iwram;
// Declared again further down with its full explanation; repeated here because
// the inline writes below use it and C requires a declaration before use.
extern unsigned long long g_idle_disturb_epoch;
// Monotonic generation of writable GBA RAM.  RAM-overlay identity checks can
// cache a verified image until a CPU/DMA store changes RAM; unlike the idle
// disturbance epoch this counter is not bumped by device ticks.
extern unsigned long long g_ram_write_epoch;
// Code-page masks are installed by a game runner from its reviewed transient
// image registry.  Only writes touching a registered RAM-code page advance the
// identity generation; ordinary WRAM data traffic must not invalidate every
// cached code image.
extern unsigned long long g_ram_code_page_mask_ewram_lo;
extern unsigned long long g_ram_code_page_mask_ewram_hi;
extern unsigned int g_ram_code_page_mask_iwram;
// Per-page generations let transient-image validation ignore writes to an
// unrelated RAM-code page.  The global epoch remains for diagnostics and
// backwards-compatible cache statistics; hot dispatch caches compare only
// the pages belonging to their candidate image.
extern unsigned int g_ram_code_page_epoch_ewram[64];
extern unsigned int g_ram_code_page_epoch_iwram[8];

// Existing AOT corpora may predate mutable-function entry guards. These
// generic dispatch/write hooks track exact RAM words changed after a static
// RAM page first executes, allowing runtime_dispatch to bypass a stale body.
void runtime_register_ram_code_page(uint32_t pc);
void runtime_note_ram_code_write(uint32_t addr, uint32_t width);
int runtime_ram_code_range_dirty(uint32_t start, uint32_t end);
void runtime_ram_code_dirty_reset(void);

static inline bool ram_code_write_affects(uint32_t addr, uint32_t width) {
    const uint32_t region = addr >> 24;
    if (region == 0x03u) {
        const uint32_t off = addr & 0x00007FFFu;
        const uint32_t first = off >> 12;
        const uint32_t last = (off + width - 1u) >> 12;
        const unsigned int span = (1u << (last - first + 1u)) - 1u;
        return (g_ram_code_page_mask_iwram & (span << first)) != 0u;
    }
    if (region == 0x02u) {
        const uint32_t off = addr & 0x0003FFFFu;
        const uint32_t first = off >> 12;
        const uint32_t last = (off + width - 1u) >> 12;
        bool hit = false;
        if (first < 32u) {
            const uint32_t low_last = last < 31u ? last : 31u;
            const unsigned long long span =
                (1ull << (low_last - first + 1u)) - 1ull;
            hit = (g_ram_code_page_mask_ewram_lo & (span << first)) != 0ull;
        }
        if (last >= 32u) {
            const uint32_t high_first = first > 32u ? first - 32u : 0u;
            const uint32_t high_last = last - 32u;
            const unsigned long long span =
                (1ull << (high_last - high_first + 1u)) - 1ull;
            hit = hit || ((g_ram_code_page_mask_ewram_hi &
                           (span << high_first)) != 0ull);
        }
        return hit;
    }
    return false;
}

static inline void note_ram_write_for_identity(uint32_t addr, uint32_t width) {
    if (!ram_code_write_affects(addr, width)) return;
    runtime_note_ram_code_write(addr, width);
    ++g_ram_write_epoch;
    const uint32_t region = addr >> 24;
    if (region == 0x03u) {
        const uint32_t off = addr & 0x00007FFFu;
        const uint32_t first = off >> 12;
        const uint32_t last = (off + width - 1u) >> 12;
        for (uint32_t page = first; page <= last && page < 8u; ++page)
            ++g_ram_code_page_epoch_iwram[page];
    } else if (region == 0x02u) {
        const uint32_t off = addr & 0x0003FFFFu;
        const uint32_t first = off >> 12;
        const uint32_t last = (off + width - 1u) >> 12;
        for (uint32_t page = first; page <= last && page < 64u; ++page)
            ++g_ram_code_page_epoch_ewram[page];
    }
}

static inline uint8_t* bus_fast_ram(uint32_t addr) {
    const uint32_t region = addr >> 24;
    if (region == 0x02u) {
        uint8_t* p = g_fast_ewram;
        return p ? p + (addr & 0x0003FFFFu) : (uint8_t*)0;
    }
    if (region == 0x03u) {
        uint8_t* p = g_fast_iwram;
        return p ? p + (addr & 0x00007FFFu) : (uint8_t*)0;
    }
    return (uint8_t*)0;
}

// ROM fast path (reads only — ROM is never written by the guest through this
// path). The cart image is constant for the whole run, so a plain array read
// is correct as long as it can never observe any of the four Region::Rom
// special cases in gba_bus.cpp's read8/16/32:
//   1. RTC/GPIO window (0x080000C4..0xC9) — guarded off by g_fast_rom_ok
//      (false whenever the cart has an active RTC) AND by requiring
//      off >= 0xD0, clear of the guard band with margin.
//   2. EEPROM save reads — guarded off by g_fast_rom_ok (false whenever the
//      cart's save type is EEPROM).
//   3. g_rom_read32_override — checked live (an LLE patch may install/clear
//      it during play), since it is not fixed for the run like (1) and (2).
//   4. Reads past the end of the ROM image (open-bus) — off + width must
//      not exceed g_fast_rom_size.
// g_fast_rom_ok is computed once by set_active_bus() from the bus's own RTC /
// save-type accessors; a false value (including "couldn't tell") just means
// every ROM access takes the slow path, which is always correct.
extern const uint8_t* g_fast_rom;
extern uint32_t        g_fast_rom_size;
extern int              g_fast_rom_ok;

// Declared here (not through gba_bus.h) to keep armv4t decoupled from the gba
// namespace; extern "C" linkage means it is the same symbol either way. Null
// during ordinary play; an LLE patch may install it.
extern int (*g_rom_read32_override)(uint32_t address, uint32_t original_value,
                                    uint32_t* out_value);

static inline const uint8_t* bus_fast_rom(uint32_t addr, uint32_t width) {
    if (!g_fast_rom_ok || !g_fast_rom || g_rom_read32_override)
        return (const uint8_t*)0;
    const uint32_t region = addr >> 24;
    if (region < 0x08u || region > 0x0Du) return (const uint8_t*)0;
    const uint32_t off = addr & 0x01FFFFFFu;  // mirrors resolve_offset(Region::Rom)
    if (off < 0x000000D0u) return (const uint8_t*)0;  // clear of the GPIO guard band
    if (off + width > g_fast_rom_size) return (const uint8_t*)0;
    return g_fast_rom + off;
}

static inline uint32_t bus_read_u32_inline(uint32_t addr) {
    const uint8_t* p = bus_fast_ram(addr);
    if (p) { uint32_t v; __builtin_memcpy(&v, p, 4); return v; }
    p = bus_fast_rom(addr, 4u);
    if (p) { uint32_t v; __builtin_memcpy(&v, p, 4); return v; }
    return bus_read_u32_slow(addr);
}

static inline uint16_t bus_read_u16_inline(uint32_t addr) {
    const uint8_t* p = bus_fast_ram(addr);
    if (p) { uint16_t v; __builtin_memcpy(&v, p, 2); return v; }
    p = bus_fast_rom(addr, 2u);
    if (p) { uint16_t v; __builtin_memcpy(&v, p, 2); return v; }
    return bus_read_u16_slow(addr);
}

static inline uint8_t bus_read_u8_inline(uint32_t addr) {
    const uint8_t* p = bus_fast_ram(addr);
    if (p) return *p;
    p = bus_fast_rom(addr, 1u);
    if (p) {
        return *p;
    }
    return bus_read_u8_slow(addr);
}

static inline void bus_write_u32_inline(uint32_t addr, uint32_t val) {
    // A transform is valid only for the immediately following bus write.
    // Clear it even on an address/width mismatch so a branch, yield, or
    // unrelated store cannot apply stale game-owned state later.
    if (g_runtime_mem_write_override_pending_valid) {
        if (g_runtime_mem_write_override_pending_addr == addr)
            val = g_runtime_mem_write_override_pending_value;
        g_runtime_mem_write_override_pending_valid = 0u;
    }
    uint8_t* p = bus_fast_ram(addr);
    if (p) {
        if (g_runtime_fast_ewram_write_observer &&
            (addr >> 24) == 0x02u) {
            g_runtime_fast_ewram_write_observer(addr, 4u);
        }
        if (g_runtime_fast_iwram_write_observer &&
            (addr >> 24) == 0x03u &&
            runtime_fast_iwram_watched(addr, 4u)) {
            const RuntimeFastIwramWriteObserver observer =
                g_runtime_fast_iwram_write_observer;
            observer(
                addr, 4u, RUNTIME_FAST_IWRAM_WRITE_START);
            __builtin_memcpy(p, &val, 4);
            observer(
                addr, 4u, RUNTIME_FAST_IWRAM_WRITE_COMMITTED);
        } else {
            __builtin_memcpy(p, &val, 4);
        }
        ++g_idle_disturb_epoch;
        note_ram_write_for_identity(addr, 4u);
        return;
    }
    bus_write_u32_slow(addr, val);
}

static inline void bus_write_u16_inline(uint32_t addr, uint16_t val) {
    // A pending transform can only target a 32-bit store; consume it on any
    // intervening write to prevent stale state from crossing the boundary.
    g_runtime_mem_write_override_pending_valid = 0u;
    uint8_t* p = bus_fast_ram(addr);
    if (p) {
        if (g_runtime_fast_ewram_write_observer &&
            (addr >> 24) == 0x02u) {
            g_runtime_fast_ewram_write_observer(addr, 2u);
        }
        if (g_runtime_fast_iwram_write_observer &&
            (addr >> 24) == 0x03u &&
            runtime_fast_iwram_watched(addr, 2u)) {
            const RuntimeFastIwramWriteObserver observer =
                g_runtime_fast_iwram_write_observer;
            observer(
                addr, 2u, RUNTIME_FAST_IWRAM_WRITE_START);
            __builtin_memcpy(p, &val, 2);
            observer(
                addr, 2u, RUNTIME_FAST_IWRAM_WRITE_COMMITTED);
        } else {
            __builtin_memcpy(p, &val, 2);
        }
        ++g_idle_disturb_epoch;
        note_ram_write_for_identity(addr, 2u);
        return;
    }
    bus_write_u16_slow(addr, val);
}

static inline void bus_write_u8_inline(uint32_t addr, uint8_t val) {
    // See bus_write_u16: any intervening write retires the one-shot state.
    g_runtime_mem_write_override_pending_valid = 0u;
    uint8_t* p = bus_fast_ram(addr);
    if (p) {
        if (g_runtime_fast_ewram_write_observer &&
            (addr >> 24) == 0x02u) {
            g_runtime_fast_ewram_write_observer(addr, 1u);
        }
        if (g_runtime_fast_iwram_write_observer &&
            (addr >> 24) == 0x03u &&
            runtime_fast_iwram_watched(addr, 1u)) {
            const RuntimeFastIwramWriteObserver observer =
                g_runtime_fast_iwram_write_observer;
            observer(
                addr, 1u, RUNTIME_FAST_IWRAM_WRITE_START);
            *p = val;
            observer(
                addr, 1u, RUNTIME_FAST_IWRAM_WRITE_COMMITTED);
        } else {
            *p = val;
        }
        ++g_idle_disturb_epoch;
        note_ram_write_for_identity(addr, 1u);
        return;
    }
    bus_write_u8_slow(addr, val);
}

// Generated game code (GBARECOMP_OUTLINE_BUS, set by CMake on every
// recompiled_*.cpp) calls one shared out-of-line copy of each fast path
// instead of inlining it at all ~890k access sites: same behaviour, about a
// quarter of the game's machine code less (FACTS.md, "Generated code size").
// Engine code keeps the inline copies.
#if defined(GBARECOMP_OUTLINE_BUS)
uint32_t runtime_bus_read_u32(uint32_t addr);
uint16_t runtime_bus_read_u16(uint32_t addr);
uint8_t  runtime_bus_read_u8(uint32_t addr);
void     runtime_bus_write_u32(uint32_t addr, uint32_t val);
void     runtime_bus_write_u16(uint32_t addr, uint16_t val);
void     runtime_bus_write_u8(uint32_t addr, uint8_t val);
static inline uint32_t bus_read_u32(uint32_t a) { return runtime_bus_read_u32(a); }
static inline uint16_t bus_read_u16(uint32_t a) { return runtime_bus_read_u16(a); }
static inline uint8_t  bus_read_u8(uint32_t a) { return runtime_bus_read_u8(a); }
static inline void bus_write_u32(uint32_t a, uint32_t v) { runtime_bus_write_u32(a, v); }
static inline void bus_write_u16(uint32_t a, uint16_t v) { runtime_bus_write_u16(a, v); }
static inline void bus_write_u8(uint32_t a, uint8_t v) { runtime_bus_write_u8(a, v); }
#else
static inline uint32_t bus_read_u32(uint32_t a) { return bus_read_u32_inline(a); }
static inline uint16_t bus_read_u16(uint32_t a) { return bus_read_u16_inline(a); }
static inline uint8_t  bus_read_u8(uint32_t a) { return bus_read_u8_inline(a); }
static inline void bus_write_u32(uint32_t a, uint32_t v) { bus_write_u32_inline(a, v); }
static inline void bus_write_u16(uint32_t a, uint16_t v) { bus_write_u16_inline(a, v); }
static inline void bus_write_u8(uint32_t a, uint8_t v) { bus_write_u8_inline(a, v); }
#endif

// ── Per-instruction cycle cost (memory + multiply) ─────────────────
// Generated code computes the fixed part of an instruction's cost
// statically (instr_cycle_base + shift/PC-write surcharges) and adds
// these runtime-dependent parts as it executes, then ticks the total
// once at the instruction boundary. `runtime_mem_cycles` returns the
// N/S access cost for the active bus region; sequential=2 requests the
// post-side-effect single-transfer data cost (including a platform prefetch
// adjustment). `runtime_mul_cycles`
// returns the ARM7TDMI multiply operand wait. Both mirror the IR
// interpreter (the timing oracle) exactly. `width` is 1/2/4 bytes;
// ordinary `sequential` and `signed_variant` values are 0/1 flags.
uint32_t runtime_mem_cycles(uint32_t addr, uint32_t width,
                            uint32_t sequential);
uint32_t runtime_mul_cycles(uint32_t rs_value, uint32_t signed_variant,
                            uint32_t extra);

// ── Shifter helpers ────────────────────────────────────────────────
// Generated code uses these for data-processing operand2 shifts and
// for register-shifted-by-register cases. They update the shifter
// carry-out into CPSR.C when set_carry != 0.

uint32_t arm_shift_lsl(uint32_t v, uint32_t n, int set_carry);
uint32_t arm_shift_lsr(uint32_t v, uint32_t n, int set_carry);
uint32_t arm_shift_asr(uint32_t v, uint32_t n, int set_carry);
uint32_t arm_shift_ror(uint32_t v, uint32_t n, int set_carry);

// ── Flag updaters ──────────────────────────────────────────────────
// After an arithmetic/logical op with S=1, generated code calls one
// of these to set CPSR.NZ(CV). The C-out / V-out semantics follow
// ARM ARM A4.1 (data processing).

// Defined here, not out-of-line in runtime_arm.cpp, so callers can fold them in.
// 39% of translated instructions end in one of these; out-of-line each was a
// real call in a different TU from every caller, so un-inlinable without LTO.
// `static inline` gives each TU its own copy; taking the address stays valid,
// which is what the overlay ABI callback table (overlay_abi.h) does.
static inline void arm_set_nz(uint32_t r) {
    uint32_t c = g_cpu.cpsr & ~(CPSR_N_BIT | CPSR_Z_BIT);
    if (r & 0x80000000u) c |= CPSR_N_BIT;
    if (r == 0)          c |= CPSR_Z_BIT;
    g_cpu.cpsr = c;
}

static inline void arm_set_nzc_logic(uint32_t r, uint32_t shifter_carry) {
    uint32_t c = g_cpu.cpsr & ~(CPSR_N_BIT | CPSR_Z_BIT | CPSR_C_BIT);
    if (r & 0x80000000u) c |= CPSR_N_BIT;
    if (r == 0)          c |= CPSR_Z_BIT;
    if (shifter_carry)   c |= CPSR_C_BIT;
    g_cpu.cpsr = c;
}

static inline void arm_set_nzcv_add(uint32_t a, uint32_t b, uint32_t r) {
    uint32_t c = g_cpu.cpsr & ~(CPSR_N_BIT | CPSR_Z_BIT |
                                CPSR_C_BIT | CPSR_V_BIT);
    if (r & 0x80000000u) c |= CPSR_N_BIT;
    if (r == 0)          c |= CPSR_Z_BIT;
    if (r < a)           c |= CPSR_C_BIT;
    if ((~(a ^ b) & (a ^ r)) & 0x80000000u) c |= CPSR_V_BIT;
    g_cpu.cpsr = c;
}

static inline void arm_set_nzcv_adc(uint32_t a, uint32_t b, uint32_t c_in,
                                    uint32_t r) {
    uint32_t c = g_cpu.cpsr & ~(CPSR_N_BIT | CPSR_Z_BIT |
                                CPSR_C_BIT | CPSR_V_BIT);
    if (r & 0x80000000u) c |= CPSR_N_BIT;
    if (r == 0)          c |= CPSR_Z_BIT;
    uint64_t wide = (uint64_t)a + b + c_in;
    if (wide >> 32)      c |= CPSR_C_BIT;
    if ((~(a ^ b) & (a ^ r)) & 0x80000000u) c |= CPSR_V_BIT;
    g_cpu.cpsr = c;
}

static inline void arm_set_nzcv_sub(uint32_t a, uint32_t b, uint32_t r) {
    uint32_t c = g_cpu.cpsr & ~(CPSR_N_BIT | CPSR_Z_BIT |
                                CPSR_C_BIT | CPSR_V_BIT);
    if (r & 0x80000000u) c |= CPSR_N_BIT;
    if (r == 0)          c |= CPSR_Z_BIT;
    if (a >= b)          c |= CPSR_C_BIT;
    if (((a ^ b) & (a ^ r)) & 0x80000000u) c |= CPSR_V_BIT;
    g_cpu.cpsr = c;
}

static inline void arm_set_nzcv_sbc(uint32_t a, uint32_t b, uint32_t c_in,
                                    uint32_t r) {
    uint32_t c = g_cpu.cpsr & ~(CPSR_N_BIT | CPSR_Z_BIT |
                                CPSR_C_BIT | CPSR_V_BIT);
    if (r & 0x80000000u) c |= CPSR_N_BIT;
    if (r == 0)          c |= CPSR_Z_BIT;
    uint64_t wide = (uint64_t)a + (~b & 0xFFFFFFFFu) + c_in;
    if (wide >> 32)      c |= CPSR_C_BIT;
    if (((a ^ b) & (a ^ r)) & 0x80000000u) c |= CPSR_V_BIT;
    g_cpu.cpsr = c;
}

// ── Dispatch ───────────────────────────────────────────────────────
// `target_pc` is a guest PC. Low bit indicates THUMB on BX/BLX. The
// dispatcher binary-searches the per-game dispatch table; if the
// target is found, the corresponding generated function is called.
// Misses route to runtime_dispatch_miss for logging + fallback.

void runtime_dispatch(uint32_t target_pc);
void runtime_dispatch_with_exchange(uint32_t target_pc);
void runtime_dispatch_miss(uint32_t target_pc);

// AOT code copied into EWRAM/IWRAM is mutable guest memory.  The generated
// function prologue validates its original instruction bytes through this
// page-generation-cached guard before executing the native body.  A mismatch
// must use runtime_mutable_ram_code_miss(), which bypasses the stale static
// entry and enters the existing loud interpreter/self-heal tier.
int runtime_ram_code_guard(uint32_t start, uint32_t end,
                           uint32_t expected_crc32);
void runtime_mutable_ram_code_miss(uint32_t entry_pc, int thumb);

// Whole-program force-interpreter backend (co-simulation "interp" side). When
// g_force_interp != 0, the main run loop calls runtime_force_interp_step() once
// per guest instruction instead of dispatching generated code — interpreting the
// main thread while reusing runtime_tick / runtime_swi for the device/IRQ/BIOS
// path (so both co-sim backends share everything but instruction execution). Set
// from GBARECOMP_FORCE_INTERP at startup. See COSIM_ORACLE.md §1.
extern int g_force_interp;
void runtime_force_interp_step(void);

// True iff a STATIC (recompiled) dispatch-table entry exists for this guest PC
// + instruction-set state. The on-miss bridge uses it to detect re-entry into
// statically recompiled code and hand control back (heal-to-static) when it has
// no reliable stop address (a top-level / exception-return miss).
int runtime_has_static_entry(uint32_t pc, int thumb);

// The interpret-the-missed-subtree core of the on-miss bridge, factored so the
// P6 sljit differential gate can reuse it as its "interpreter pass" (the kept
// result a healed shard is validated against). Interprets from (entry_pc,
// entry_thumb) until control returns to the stop address, mutating g_cpu live
// and ticking each instruction (IRQ self-delivery + SWI routing exactly as in
// normal play). forced_stop_pc=0 → use the call-return-stack contract (the
// on-miss bridge). Non-zero → stop there instead: the gate passes the entry LR,
// since a function reached by a computed jump (not a BL) has no matching
// call-return frame and the stack-top contract would mis-target and run away.
// max_instrs=0 → the default 200M abort-on-runaway; non-zero bounds the walk and
// returns 0 (instead of aborting) if the stop isn't reached in that budget — the
// gate uses this to fall back safely when its forced stop turns out wrong.
// Returns 1 if it stopped cleanly at the stop address, 0 if it hit the budget.
int runtime_bridge_interpret(uint32_t entry_pc, bool entry_thumb,
                             uint32_t forced_stop_pc, uint64_t max_instrs);

// Direct generated BL calls use the host C stack for speed and clarity.
// Return idioms (`bx lr`, `mov pc, lr`, `pop {..., pc}`) are only C
// returns when they match the top direct-call return address; otherwise
// they are real guest branches and must dispatch.
void runtime_call_push_return(uint32_t return_pc);
int  runtime_call_should_return(uint32_t target_pc);
void runtime_call_cancel_return(uint32_t return_pc);

// Save-state support: expose the host-side call-return stack so the
// snapshot orchestrator can serialize/restore it. The stack lives in
// runtime_arm.cpp (file-local); these accessors are the only sanctioned
// window. restore replaces the live stack wholesale (clamped to the
// stack capacity). Returned pointer is valid until the next push/pop.
uint32_t        runtime_call_stack_depth(void);
const uint32_t* runtime_call_stack_data(void);
void            runtime_call_stack_restore(const uint32_t* entries,
                                           uint32_t depth);

// Test-only window onto the IRQ-nesting floor (normally raised/restored
// only by runtime_irq()). Lets unit tests exercise should_return /
// cancel_return's floor guard without driving a full IRQ dispatch.
void     runtime_call_stack_set_floor_for_test(uint32_t floor);
uint32_t runtime_call_stack_floor_for_test(void);

// Always-on structured execution trace. The RUNTIME_TRACE_* kind macros
// and RuntimeTraceEntry are defined in runtime_arm_types.h. This records
// diagnostic state only; it never routes execution or substitutes for
// missing codegen.
void runtime_trace_event(uint32_t kind, uint32_t pc, uint32_t addr,
                         uint32_t value, uint32_t aux);
void runtime_trace_reset(void);
void runtime_trace_dump_recent(uint32_t max_entries);
// Total trace events recorded since the last runtime_trace_reset(), NOT
// capped by the ring's retention window. Lets a successful (non-aborting)
// run report the same "trace events" figure that an abort's recent-trace
// dump otherwise only reveals implicitly through its sequence numbers.
uint32_t runtime_trace_total(void);

// ── Per-instruction fingerprint ring (MC-HP-002 cycle-aligned diff) ──
// When g_runtime_insn_trace != 0 the generated per-instruction prologue calls
// runtime_insn_fp() to record the pre-execution architectural state of EVERY
// instruction: {cumulative cycles, pc, cpsr, R0..R15}. The ring is always-on
// once armed (armed at machine reset from GBARECOMP_INSN_TRACE, or via the TCP
// `insn_trace` command) and bounded by eviction; a targeted dump pulls the
// window of interest. This is the substrate for diffing the recomp against the
// bios_smoke interp oracle at identical cycle counts — the FIRST fingerprint
// that differs (pc or any register) localizes the first real divergence.
// Disarmed (default) it costs one not-taken branch per instruction. When
// GBARECOMP_BIOS_PC_LOG is set, the same seam records only the compact BIOS
// PC/mode set and does not allocate the full fingerprint ring.
// RuntimeFpEntry is defined in runtime_arm_types.h (shared with the overlay
// shim).
extern unsigned g_runtime_insn_trace;  // 0 = off; 1 = GFP1; 2 = BIOS PC set
void runtime_insn_fp(void);            // emit one fingerprint (armed-gated by caller)
// F1 "Crash log": arms the same per-instruction trail with a small ring
// (~5 MB, the last ~65k instructions) so a crash or freeze report can show
// what the game was doing. Costs speed while on. Survives machine reset.
void runtime_set_crash_log(int on);
void runtime_fp_reset(void);
uint32_t runtime_fp_count(void);
// Write the whole ring (oldest-first) as a compact binary file: a 16-byte
// header {u32 magic 'GFP1', u32 entry_size, u64 count} followed by `count`
// RuntimeFpEntry records. Returns the number of records written (0 on error).
uint32_t runtime_fp_save_file(const char* path);
// Dump the LAST `n` fingerprint records (oldest-first in the window) as CSV
// (idx,cycles,pc,cpsr,r0..r15). The hang watchdog calls this on trip so the
// execution history leading into a freeze is on disk with no live interaction
// (n==0 → whole ring). Requires GBARECOMP_INSN_TRACE armed. Returns records.
uint32_t runtime_fp_save_tail_csv(const char* path, uint32_t n);
// Cycle-anchor sampler (Axis 2 — accuracy burndown): filter the always-on
// insn-fingerprint ring by guest PC, writing up to `max_hits` cumulative
// g_runtime_cycles stamps (oldest-first) for every recorded execution of `pc`
// into `out_cycles`. Returns the number written. The Δ between consecutive hits
// is the offset-cancelled cycle "ruler" peer to the NBA oracle's cyc_anchor.
// Requires GBARECOMP_INSN_TRACE armed (the ring is the only always-on
// per-instruction (pc,cycle) source); returns 0 when the ring is empty.
uint32_t runtime_fp_query_pc(uint32_t pc, uint32_t max_hits,
                             unsigned long long* out_cycles);

// Current recompiled-CPU guest PC (g_cpu.R[15]). Read-only accessor exposed so
// always-on observability taps outside the armv4t lib (e.g. the gba_io MMIO
// write-trace ring) can stamp the originating PC without coupling to the
// ArmCpuState layout. Reflects the recomp register file (meaningful in the
// recompiled runtime; the bios_smoke interpreter drives its own CPUState).
uint32_t runtime_current_pc(void);

// ── Function-entry hook (general debug observability) ──────────────────
// When g_runtime_fn_entry_hook != nullptr the generated function prologue
// calls it with the guest entry PC, BEFORE the first instruction of that
// function runs. At that instant g_cpu.R[0..3] hold the AAPCS arguments and
// g_cpu.R[14] the return address, so a host probe can read a recompiled
// function's arguments and guest memory at call time. This is the general
// substrate for host-side observation of ANY recompiled guest function across
// ANY game (e.g. the widescreen tilemap-provenance probe reads DrawMetatileAt's
// world-coord args). nullptr (the default) costs one not-taken branch per
// function entry and is byte-identical to the un-hooked guest state.
extern void (*g_runtime_fn_entry_hook)(uint32_t entry_pc);

// Bounded observer multiplexer for function-entry research/debug features.
// Observers are called in registration order and cannot replace each other.
// The reset is a run-lifecycle operation; generated code still pays one null
// check when no observer is installed.
int runtime_fn_entry_observer_add(void (*observer)(uint32_t entry_pc));
void runtime_fn_entry_observer_remove(void (*observer)(uint32_t entry_pc));
void runtime_fn_entry_observers_reset(void);

// ── Mid-function alias resume PC ───────────────────────────────────────
// Set by a thin per-alias dispatch wrapper (generated) to the alternate
// entry PC immediately before it tail-calls the host function; the host's
// resume prologue reads it, clears it, and computed-goto's to the interior
// label for that PC. 0 (the default) = normal entry: the host falls through
// the prologue and starts at its first instruction. This is how an IRQ/SWI
// return into the middle of an already-recompiled function (e.g. a
// WaitForVBlank busy-spin) re-enters the WHOLE native function at the right
// point instead of fragmenting it into dispatch-chained pieces.
extern uint32_t g_runtime_resume_pc;

// Dedicated always-on IRQ-vector log (MC-HP-002): one entry per IRQ vectoring,
// dumped as CSV. Armed by env GBARECOMP_IRQ_LOG. g_runtime_irq_from_halt is set
// by runtime_tick's wake-from-HALT path so each log entry records whether the
// vector woke the CPU from HALT.
void runtime_irq_log_record(uint32_t src, uint32_t ret, uint32_t cpsr);
uint32_t runtime_irq_log_save_file(const char* path);
extern uint32_t g_runtime_irq_from_halt;
// Live TCP query peer of the IRQ-vector log (Axis 3 — accuracy burndown). The
// ring records the IRQ TAKE point (vectoring): cycle stamp, active source mask
// (IE & IF), return address, saved CPSR, and the from-HALT flag. Raise-time (the
// IF-set instant) is NOT separately recorded — this is take-time only; that gap
// is noted in the burndown. Recording is always-on (Release too); the env
// GBARECOMP_IRQ_LOG only governs the on-exit CSV dump.
typedef struct RuntimeIrqLogEntry {
    unsigned long long cycles;
    uint32_t src;
    uint32_t ret;
    uint32_t cpsr;
    uint32_t from_halt;
} RuntimeIrqLogEntry;
// Copy the most recent `max` IRQ-vector entries (oldest-first) into `out`.
uint32_t runtime_irq_log_copy_recent(RuntimeIrqLogEntry* out, uint32_t max);
uint32_t runtime_irq_log_count(void);
// SWI log (milestone-PC sequence): every SWI with cycle + caller + r0/r1 +
// BIOS IntrWait flags (0x03007FF8) and the route taken (LLE, HLE handled, or
// HLE fallback). Armed by env GBARECOMP_SWI_LOG.
void runtime_swi_log_record(unsigned long long cycles, uint32_t imm,
                            uint32_t ret, uint32_t r0, uint32_t r1,
                            uint32_t r2, uint32_t lr, uint32_t iwflags,
                            uint32_t route);
uint32_t runtime_swi_log_save_file(const char* path);

// Lightweight BIOS execution inventory. When GBARECOMP_BIOS_PC_LOG is set,
// generated instruction prologues feed this path instead of allocating the
// large full fingerprint ring; the output is the complete set of executed
// BIOS halfword PCs, split by ARM/THUMB mode, for the run, each marked boot
// (only before the first cartridge instruction), play (only after it) or
// both.
uint32_t runtime_bios_pc_log_save_file(const char* path);
unsigned long long runtime_bios_pc_log_sample_count(void);

// ── Function coverage (input-explorer measurement) ──────────────────────
// GBARECOMP_FUNC_COVERAGE=<path> opt-in (armed once at machine reset, from
// runtime_trace_reset()). Records, as a single hit/no-hit bit, every
// dispatch-table (BIOS or cart) entry whose generated function executes at
// least once — via the existing general function-entry hook every generated
// function prologue already calls (g_runtime_fn_entry_hook above), so an
// unarmed run pays nothing beyond the null-pointer check that is already
// there. Write the recorded set as a deterministic, address-sorted text
// file ("0xADDRESS MODE" per line, BIOS entries then cart entries) with
// runtime_coverage_save_file(); returns the number of functions recorded, or
// 0 if coverage was never armed. Two runs that executed identical code
// produce byte-identical files.
uint32_t runtime_coverage_save_file(const char* path);
uint32_t runtime_trace_copy_recent(RuntimeTraceEntry* out,
                                   uint32_t max_entries);
void runtime_tick(uint32_t cycles);
// Real-elapsed-hardware-time door into the master clock: used ONLY by the
// halt/idle pump (never by executed-instruction call sites). Unlike
// runtime_tick, this NEVER applies GBARECOMP_CPU_OVERCLOCK scaling — halt
// time is real hardware time, not CPU work, and must stay faithful to true
// rate regardless of the overclock setting. See runtime_bus_bridge.cpp.
void runtime_tick_realtime(uint32_t cycles);
bool runtime_should_yield(void);

// ── Combined per-instruction helpers (tools/slim_corpus.py) ────────────
// Each replaces a fixed sequence the translator writes at every instruction,
// load or store with ONE call that does exactly the same work in the same
// order, so the game's machine code carries one call where it carried two
// to four (FACTS.md, "Generated code size").
//
// `if (runtime_should_yield()) return; if (g_runtime_insn_trace)
//  runtime_insn_fp();`  ->  `if (runtime_insn_boundary()) return;`
bool runtime_insn_boundary(void);
// For an unconditional instruction the translator writes `g_cpu.R[15] = pc;`,
// the boundary above, then the sequential fetch cost
// runtime_mem_cycles(pc, w, 1u). This does all three and returns that cost
// (always >= 1), or 0 where the boundary said return.
uint32_t runtime_insn_begin(uint32_t pc, uint32_t width);
// Block timing (game code made by tools/block_timing.py; the normal corpus
// never uses these). Inside a translated block only the first instruction,
// labels and branches keep runtime_insn_begin / runtime_insn_boundary; the
// others take their fetch cost with runtime_insn_fetch (R15 and the cost, no
// yield check), and every runtime_tick becomes runtime_tick_deferred, which
// only adds to g_runtime_deferred_cycles. The engine pays that debt through
// runtime_tick before anything can see the clock: each boundary, dispatch,
// SWI, MSR, idle back-edge, and any load/store to 0x04000000-0x07FFFFFF (I/O,
// palette, VRAM, OAM; there with interrupts held to the next boundary). The
// cycle total is unchanged; an interrupt is taken at the next boundary.
extern uint32_t g_runtime_deferred_cycles;
static inline void runtime_tick_deferred(uint32_t cycles) {
    g_runtime_deferred_cycles += cycles;
}
uint32_t runtime_insn_fetch(uint32_t pc, uint32_t width);
void runtime_flush_deferred(void);
// runtime_mem_cycles(pc, w, 0u) - runtime_mem_cycles(pc, w, 1u)
uint32_t runtime_fetch_ns_delta(uint32_t pc, uint32_t width);
// (runtime_mem_cycles(t, w, 0u) - 1u) + (runtime_mem_cycles(t + w, w, 1u) - 1u):
// the pipeline refill after a taken branch to t.
uint32_t runtime_refill_cycles(uint32_t target, uint32_t width);
// Single loads: the bus read, then its data-access cost
// (runtime_mem_cycles(ea, w, 2u), which also advances the prefetch model),
// left in g_runtime_data_cost for the `_cyc += ...` that follows.
extern uint32_t g_runtime_data_cost;
uint32_t runtime_ld_u32(uint32_t addr, uint32_t ea);
uint32_t runtime_ld_u16(uint32_t addr, uint32_t ea);
uint32_t runtime_ld_u8(uint32_t addr, uint32_t ea);
// Single stores: the write trace event, the bus write, then the data-access
// cost, which is returned. `value` is the traced 32-bit value; the stored
// value is its low bytes, exactly as the translator's own store expression.
uint32_t runtime_st_u32(uint32_t pc, uint32_t addr, uint32_t ea, uint32_t value);
uint32_t runtime_st_u16(uint32_t pc, uint32_t addr, uint32_t ea, uint32_t value);
uint32_t runtime_st_u8(uint32_t pc, uint32_t addr, uint32_t ea, uint32_t value);
// LDM/STM slots: runtime_mem_cycles(addr, 4u, seq) (pure: region and
// WAITCNT only), then the access. The cost is returned.
uint32_t runtime_ldm_u32(uint32_t addr, uint32_t seq, uint32_t* dst);
uint32_t runtime_stm_u32(uint32_t pc, uint32_t addr, uint32_t value,
                         uint32_t seq);

// ── GBARECOMP_CPU_OVERCLOCK (TURBO-B2-UI) ──────────────────────────────
// Live control for the runtime_tick scaling factor (see runtime_bus_bridge.cpp
// "GBARECOMP_CPU_OVERCLOCK" section). Safe to call at any time, including
// while the guest is executing / mid-frame / from a nested runtime_dispatch:
// backed by a relaxed atomic, no lock, no getenv. factor < 1 is clamped to 1
// (off). The env var supplies only the process-start default; a host UI
// control that calls this overrides it live and is expected to. The host UI
// exposes this as an Off/On control: Off pins 1x, On pins the 10x ceiling —
// see kOverclockFactors in host_config_ui.h.
void runtime_set_overclock_factor(unsigned factor);
unsigned runtime_get_overclock_factor(void);

// The same control in 8.8 fixed point, where kOverclockOne is 1.0x.
// runtime_get_overclock_factor() above keeps reporting whole multiples for
// the pinned-factor menu item.
enum { kOverclockOne = 256u };
void runtime_set_overclock_factor_q8(unsigned q8);
unsigned runtime_get_overclock_factor_q8(void);

// Opt-in Golden Sun gameplay cheats. Both default OFF. The VBlank hook in
// runtime_bus_bridge calls runtime_apply_infinite_hp_pp once per boundary.
void runtime_set_infinite_hp(int on);
void runtime_set_infinite_pp(int on);
int  runtime_get_infinite_hp(void);
int  runtime_get_infinite_pp(void);
void runtime_apply_infinite_hp_pp(void);

// Generic live enable bit for an optional game-owned memory-write transform.
// The game runner owns the callback and all guest metadata; the canonical
// runtime remains unchanged when this is off (the default).
void runtime_set_mem_write_override_enabled(int on);
int  runtime_get_mem_write_override_enabled(void);

// Fixed view mode (0 = Native) requested by game code; applied by the host
// window on its next pump like a change in its own menu, and saved. -1 from
// take means no request. get returns the mode the host last reported.
void runtime_request_view_mode(int mode);
int  runtime_take_view_mode_request(void);
void runtime_publish_view_mode(int mode);
int  runtime_get_view_mode(void);

// No Slowdown: Enhanced Timing and CPU Overclock switched on or off
// together by game code, applied and saved by the host window's next pump.
// get reports 1 only while both are on.
void runtime_request_no_slowdown(int on);
int  runtime_take_no_slowdown_request(void);
void runtime_publish_no_slowdown(int on);
int  runtime_get_no_slowdown(void);

// ── "Additional debug logging" toggle (config UI Logging tab) ───────────
// Single live control shared by every verbose/diagnostic stream that is
// otherwise env-var gated: the hot-path trace ring in this file's own
// runtime_trace_event (GBARECOMP_TRACE), plus the Group-B diagnostics in
// runtime_bus_bridge.cpp / overlay_loader.cpp / runner_main.cpp (guest-PC
// sampler, phase profiler, frame-events CSV, relocatable profile/log,
// blitter-shadow observer, dynamic-RAM probe, transient-identity verbose
// report). Each site keeps its OWN env var as an explicit override — this
// toggle is consulted only when that env var is unset, so existing scripts
// and repro recipes keep working unchanged. Excluded: GBARECOMP_HOST_PROF
// (suspends/resumes the emulation thread, qualitatively costlier, stays
// env-var-only) and GBARECOMP_CYC_LO/HI (needs a numeric cycle range a
// boolean cannot supply, stays env-var-only).
// Backed by a relaxed atomic, same shape as runtime_set_overclock_factor
// above: cheap enough to read from runtime_trace_event's hot path, safe to
// flip live from the config UI at any time, no lock. Default OFF.
void gsr_set_additional_debug_logging(int on);
int gsr_additional_debug_logging(void);

// ── Stage 2 idle-loop elision ──────────────────────────────────────
// Emitted by codegen at the back-edge of a statically-eligible quiescent
// loop (CodegenCtx::idle_backedge_pcs), called once per completed iteration
// with the loop-header PC as the site identity. The runtime keeps a per-site
// rolling fixed-point proof: when two consecutive iterations leave {R0-R14,
// full CPSR} identical, span the same cycle period, and no disturbance
// (memory write, MMIO read, IRQ, or serviced device event) occurred between
// them, the loop is provably idle until an external agent changes its watched
// state. It then fast-forwards the Stage-1 master clock by whole loop periods
// to one period before the next scheduled event (g_event_budget horizon) —
// servicing NO event inside the hook — and returns with the CPU positioned at
// the loop header, so the final event-crossing iterations run normally and the
// IRQ is delivered at the exact baseline cycle. Bit-identical by construction.
void runtime_idle_backedge(uint32_t header_pc);

// Monotonic "something that could change a watched poll value or the timing
// rules happened" counter. Bumped on every guest memory write, every MMIO
// read (so MMIO-polling loops never qualify), every IRQ entry, and every
// device event materialization. The idle prover requires it to be unchanged
// across the two proof iterations. (Correctness-first: false invalidations
// only cost a re-prove; a missed bump would be unsound.)
extern unsigned long long g_idle_disturb_epoch;
// Monotonic host-side notification for presentation caches that are not part
// of the serialized GBA state. Incremented after every successful state load.
extern unsigned long long g_runtime_state_epoch;
// Cumulative guest-cycle clock. Incremented by runtime_tick on EVERY tick
// (per-instruction exec ticks AND halt-pump chunks), so it is the authoritative
// total-cycle count — unlike runtime.cpp's `cycles_elapsed`, which only tallied
// the halt path (the MC-HP-002 "cycles incomparable" red herring). Reset to 0
// at machine reset (runtime_trace_reset). Stamped onto every ring entry so the
// recomp and interp oracle can be aligned by identical cycle counts. (MC-HP-002.)
extern unsigned long long g_runtime_cycles;

// P6 sljit differential gate — shadow-tick mode. While g_runtime_shadow_tick is
// nonzero (only during a healed shard's throwaway validation re-run) runtime_tick
// accumulates the cost into g_runtime_shadow_cycles and pumps nothing (the
// interpreter pass already pumped devices / delivered IRQs). Default 0 → normal
// play and the gcc path are untouched.
extern unsigned          g_runtime_shadow_tick;
extern unsigned long long g_runtime_shadow_cycles;

// Debug PC breakpoint: when nonzero, runtime_should_yield() unwinds the
// current runtime_dispatch the moment the guest PC equals this value.
// Set via the TCP set_break_pc command; 0 = disabled. (MC-HP-002.)
extern uint32_t g_runtime_break_pc;

// ── BIOS / SWI ─────────────────────────────────────────────────────
// SWI emits a call here. The runtime sets up the exception frame
// (LR_svc = PC+4, SPSR_svc = CPSR, mode=SVC, I=1, T=0) and
// dispatches to the recompiled BIOS at 0x00000008 via
// runtime_dispatch. Returns when the recompiled handler issues
// `movs pc, lr` and we land back at the recompiled site. A missed
// BIOS handler self-heals (bridge + on-the-fly recompile + log), it
// is never hand-written HLE — see PRINCIPLES.md "BIOS is sacred —
// recompiled and dispatched" and "Honest self-healing".

void runtime_swi(uint32_t swi_imm);
void runtime_irq(uint32_t return_address);

// ── BIOS HLE hook (opt-in alternative to the recompiled/LLE BIOS) ──────
// nullptr (the default) = pure LLE: runtime_swi always enters SVC mode and
// dispatches the recompiled BIOS, byte-identical to a build without HLE. When
// installed (by gba::bios_hle_set_mode(), see src/runtime/bios_hle.h),
// runtime_swi consults it BEFORE the SVC-mode entry with the decoded SWI number
// (0..0xFF). The hook returns 1 if it fully serviced the call — guest resumes at
// LR, no BIOS dispatch — or 0 to fall through to the recompiled BIOS (the case
// for every SWI the HLE layer does not implement). LLE stays load-bearing and
// remains the verification oracle; HLE never becomes the oracle. This is the
// PRINCIPLES.md "verified-enhancement HLE" carve-out: opt-in, reverts to LLE.
extern int (*g_bios_hle_hook)(uint32_t swi_num);

// ── No-BIOS mode (BIOS_REMOVAL_PLAN.md) ────────────────────────────
// Nonzero: no BIOS code is ever executed. runtime_irq performs the IRQ
// vector's documented work itself (push r0-r3,r12,lr on the IRQ stack, call
// the user handler at [0x03007FFC] with LR at the synthetic return 0x138,
// then pop and return from the exception); runtime_swi services Halt itself
// and every other SWI through g_bios_hle_hook, failing loudly when that hook
// declines. runtime_dispatch of any PC below 0x4000 reaches only the no-BIOS
// stand-ins: the IRQ return (0x138) and the SWI exit at the Halt wait (0x1B4)
// where every existing savestate sits; any other BIOS PC aborts. Set once at
// bring-up by runtime_set_no_bios(); default 0 leaves every path unchanged.
extern int g_runtime_no_bios;
void runtime_set_no_bios(int enabled);
// Called with the value a protected BIOS-region read returns after each
// no-BIOS stand-in, the word hardware would have prefetched at that point
// (GBATEK "Reading from BIOS Memory"). Installed by the runtime; null in
// codegen tests.
extern void (*g_runtime_bios_open_bus_hook)(uint32_t value);

// ── PSR transfer ───────────────────────────────────────────────────
// MRS/MSR helpers. Routing through the runtime lets us validate mode
// transitions and keep banked registers coherent.
//
// runtime_msr_cpsr handles bank-swap when CPSR.mode changes: the
// outgoing mode's R13/R14 are saved into banked_sp/banked_lr, and
// the incoming mode's are loaded into R[13]/R[14]. FIQ entry/exit
// additionally swaps R8..R12 with r8_12_fiq.

uint32_t runtime_mrs_cpsr(void);
uint32_t runtime_mrs_spsr(void);
void runtime_msr_cpsr(uint32_t value, uint32_t mask);
void runtime_msr_spsr(uint32_t value, uint32_t mask);

// LDM/STM with S=1 and PC absent transfers User-mode registers while
// remaining in the current mode.
uint32_t runtime_read_user_reg(uint32_t reg);
void runtime_write_user_reg(uint32_t reg, uint32_t value);

// ── Exception return ───────────────────────────────────────────────
// Implements the DP-S/LDM-S "Rd=PC" exception return path:
// SPSR_<current mode> → CPSR, bank-swap R13/R14 to the restored
// mode, set PC to new_pc. Used by lowered MOVS PC, LR and LDM with
// PC in list + S bit.

void runtime_exception_return(uint32_t new_pc);
void runtime_restore_cpsr_from_spsr(void);

// ── Codegen gap (NOT a dispatch miss) ──────────────────────────────
// Emitted for IrOps codegen hasn't lowered yet. The runtime ALWAYS
// aborts here — this is a CODEGEN gap, not a dispatch miss, so it is
// NOT self-healed; it is a P0 codegen-completion task. (Analogous to
// the interpreter's own NotImplemented gate, which also stays loud —
// see PRINCIPLES.md "Honest self-healing", "Genuine interpreter gaps
// still abort loudly". Dispatch misses, by contrast, self-heal.)

void runtime_unimplemented_op(const char* op_name, uint32_t pc);

// ── Lifecycle ──────────────────────────────────────────────────────
// Called by the runner before any cart code executes.
// `bus_handle` is a void* pointing to the active gba::GbaBus.

void runtime_init(void* bus_handle);
void runtime_shutdown(void);

#ifdef __cplusplus
}  // extern "C"
#endif
