// runtime_arm_default_aborts.cpp — production definitions of
// runtime_dispatch_miss + runtime_unimplemented_op.
//
// These would conceptually live alongside the rest of runtime_arm.cpp
// in gbarecomp_armv4t, but MinGW's PE-COFF target doesn't reliably
// resolve weak symbols out of static archives — so we can't use the
// "weak default + strong override" pattern that works on ELF. Instead
// we split: gbarecomp_armv4t has no defaults for these symbols, and
// every consumer must supply them.
//
//   * runtime_unimplemented_op — a CODEGEN gap (an IrOp the recompiler
//     hasn't lowered). This ALWAYS aborts loudly: it is a P0
//     codegen-completion task, never self-healed (PRINCIPLES.md
//     "Genuine interpreter gaps still abort loudly").
//
//   * runtime_dispatch_miss — a DISPATCH gap (a guest PC with no
//     generated function: the finder didn't reach it, or it was
//     deliberately excluded). As of Stage 1 this SELF-HEALS: it bridges
//     the missed call through the reference interpreter and returns
//     control to recompiled code, while loudly logging the miss and
//     recording it for a reviewed TOML proposal. See PRINCIPLES.md
//     "Honest self-healing" and src/runtime/self_heal.h.
//
// Test consumers (tests/codegen/stubs.cpp) link only gbarecomp_armv4t
// and supply their own versions that record state for the diff runner.

#include "runtime_arm.h"
#include "symbol_lookup.h"
#include "env_flag.h"
#include "self_heal.h"
#include "overlay_loader.h"
#include "arm_cpu_bridge.h"
#include "runtime_bus_bridge.h"

#include "cpu_state.h"
#include "interpreter.h"
#include "arm_decode.h"
#include "thumb_decode.h"
#include "arm_ir.h"

#include "../gba/gba_bus.h"
#include "../gba/crc32.h"
#include "../gba/gba_ppu.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <tuple>
#include <vector>

extern "C" bool g_cost_probe_enabled(void);
extern "C" void runtime_yield_restore_pc(void);
extern "C" unsigned long long g_cost_mp2k_hook_checks;
extern "C" unsigned long long g_cost_mp2k_hook_matches;
extern "C" unsigned long long g_cost_mp2k_hook_deep_calls;
extern "C" void runtime_pool_dispatch_history_dump(uint32_t invalid_pc);

namespace {

// ── Self-heal coverage bookkeeping ─────────────────────────────────
// Per-missed-PC record. `thumb` is the instruction-set state at the
// miss; `count` is how many times the bridge ran for this PC.
struct MissRec {
    bool          thumb = false;
    std::uint64_t count = 0;
    // The top-level/empty-stack "fallback bridge" stderr line is emitted ONCE
    // per PC (like the dispatch-miss line); per-occurrence counts live in the
    // exit report. A hot interior-resume bridge (e.g. the WaitForVBlank poll
    // yielding at an interior PC every frame) would otherwise flood stderr —
    // and console I/O is slow enough to dominate windowed wall-time. (recomp-
    // template PRINCIPLES: structured debug surface, never printf spam.)
    bool          toplevel_logged = false;
};

std::map<std::uint32_t, MissRec> g_misses;        // keyed by PC & ~1
std::uint64_t                    g_interp_insns = 0;
// Times the bridge's call-boundary check (below) found a callee that already
// had native code (AOT-static or a healed overlay) and dispatched it directly
// instead of single-stepping through its body. Each handoff is instructions
// NOT added to g_interp_insns that the old bridge would have interpreted.
std::uint64_t                    g_bridge_native_handoffs = 0;

struct RamCodeGuardEntry {
    bool valid = false;
    bool matched = false;
    std::array<unsigned int, 64> page_epochs{};
};

using RamCodeGuardKey = std::tuple<uint32_t, uint32_t, uint32_t>;
std::map<RamCodeGuardKey, RamCodeGuardEntry> g_ram_code_guards;
std::uint64_t g_ram_smc_fallbacks = 0;

// Program identity, set once at startup, stamped into the persisted miss/
// coverage logs so they are never ambiguous about which game produced them
// (and so per-game default filenames don't clobber each other).
std::string g_prog_title;
std::string g_prog_code;
std::string g_prog_sha1;

// Hard backstop so a runaway bridge (e.g. an interpreted infinite loop
// that never reaches the stop address) fails loudly instead of hanging
// forever. Far above any real BIOS/cart subroutine length.
constexpr std::uint64_t kBridgeIterationCap = 200'000'000ull;

// A dispatch miss with an empty call-return stack has no trustworthy return
// address.  The old bridge interpreted until it re-entered static code, so a
// generated RAM entry could block the game thread for hundreds of thousands
// of instructions in one host call.  Return periodically for RAM code; the
// outer dispatch loop will resume at g_cpu.R15 and keep the exact same guest
// state/clock.  This is a scheduling quantum, not a semantic instruction
// limit (the hard cap above still applies to the bounded/genuine bridge).
constexpr std::uint64_t kTopLevelRamBridgeSlice = 4096ull;

// Keep the interpreter-gap report useful even when the general runtime trace
// is disabled or has been consumed by the dispatch-miss path. This is a fixed
// ring: an abort can print at most this many preceding decoded instructions.
constexpr std::size_t kBridgeGapHistory = 12;

struct BridgeGapInstruction {
    std::uint32_t pc = 0;
    std::uint32_t raw = 0;
    std::uint16_t op = 0;
    std::uint8_t result = 0;
    bool thumb = false;
    bool is_undefined = false;
    std::uint32_t r0 = 0;
    std::uint32_t r1 = 0;
    std::uint32_t r2 = 0;
    std::uint32_t r3 = 0;
    std::uint32_t r12 = 0;
    std::uint32_t sp = 0;
    std::uint32_t lr = 0;
    std::uint32_t cpsr = 0;
};

std::uint32_t bridge_gap_cpsr(const armv4t::CPSR& cpsr) {
    return (cpsr.n ? 1u << 31 : 0u) |
           (cpsr.z ? 1u << 30 : 0u) |
           (cpsr.c ? 1u << 29 : 0u) |
           (cpsr.v ? 1u << 28 : 0u) |
           (cpsr.i ? 1u << 7 : 0u) |
           (cpsr.f ? 1u << 6 : 0u) |
           (cpsr.t ? 1u << 5 : 0u) |
           (static_cast<std::uint32_t>(cpsr.mode) & 0x1Fu);
}

const char* bridge_result_name(std::uint8_t result) {
    using Result = armv4t::Interpreter::Result;
    switch (static_cast<Result>(result)) {
        case Result::Normal: return "normal";
        case Result::Branched: return "branched";
        case Result::Swi: return "swi";
        case Result::Undefined: return "undefined";
        case Result::NotImplemented: return "not-implemented";
    }
    return "unknown";
}

void capture_bridge_gap_instruction(BridgeGapInstruction& out,
                                    const armv4t::CPUState& cpu,
                                    const armv4t::Instr& insn,
                                    std::uint8_t result) {
    out.pc = insn.pc;
    out.raw = insn.raw;
    out.op = static_cast<std::uint16_t>(insn.op);
    out.result = result;
    out.thumb = insn.thumb;
    out.is_undefined = insn.is_undefined;
    out.r0 = cpu.R[0];
    out.r1 = cpu.R[1];
    out.r2 = cpu.R[2];
    out.r3 = cpu.R[3];
    out.r12 = cpu.R[12];
    out.sp = cpu.R[13];
    out.lr = cpu.R[14];
    out.cpsr = bridge_gap_cpsr(cpu.cpsr);
}

void log_bridge_gap_history(
    const std::array<BridgeGapInstruction, kBridgeGapHistory>& history,
    std::size_t count, std::size_t next) {
    std::fprintf(stderr,
                 "runtime_arm: interpreter-gap history count=%zu\n", count);
    for (std::size_t offset = 0; offset < count; ++offset) {
        const std::size_t index =
            (next + kBridgeGapHistory - count + offset) % kBridgeGapHistory;
        const BridgeGapInstruction& item = history[index];
        const armv4t::IrOp op = static_cast<armv4t::IrOp>(item.op);
        std::fprintf(stderr,
                     "  gap-history[%zu] pc=0x%08x mode=%s raw=0x%08x "
                     "op=%s result=%s undefined=%u cpsr=0x%08x "
                     "r0=0x%08x r1=0x%08x r2=0x%08x r3=0x%08x "
                     "r12=0x%08x sp=0x%08x lr=0x%08x\n",
                     offset, item.pc, item.thumb ? "thumb" : "arm", item.raw,
                     armv4t::ir_op_name(op), bridge_result_name(item.result),
                     item.is_undefined ? 1u : 0u, item.cpsr, item.r0, item.r1,
                     item.r2, item.r3, item.r12, item.sp, item.lr);
    }
}

}  // namespace

namespace gbarecomp {

void self_heal_reset() {
    g_misses.clear();
    g_interp_insns = 0;
    g_bridge_native_handoffs = 0;
    g_ram_code_guards.clear();
    g_ram_smc_fallbacks = 0;
}

void self_heal_set_program_identity(const char* title, const char* code,
                                    const char* sha1) {
    g_prog_title = title ? title : "";
    g_prog_code  = code ? code : "";
    g_prog_sha1  = sha1 ? sha1 : "";
}

bool self_heal_any_misses() { return !g_misses.empty(); }

std::uint32_t self_heal_distinct_misses() {
    return static_cast<std::uint32_t>(g_misses.size());
}

std::uint64_t self_heal_interpreted_insns() { return g_interp_insns; }

void self_heal_write_report(const char* frag_path) {
    // Stage-2: a warm-cache run can heal PCs to native with ZERO misses this
    // session (the overlay tier catches them before the bridge). Healed-from-
    // cache is STILL NOT fully static (PRINCIPLES.md "Coverage honesty is
    // load-bearing"), so the banner must factor in heals, not just misses.
    std::uint64_t healed = 0, native_calls = 0, inflight = 0, failed = 0;
    std::uint64_t ram_healed = 0, ram_revalidations = 0, ram_crc_mismatches = 0;
    gbarecomp::overlay_counters(&healed, &native_calls, &inflight, &failed,
                                &ram_healed, &ram_revalidations,
                                &ram_crc_mismatches);

    if (g_misses.empty() && healed == 0) {
        std::printf("self_heal_coverage=FULLY_STATIC dispatch_misses=0 "
                    "interpreted_insns=0 healed_native=0 ram_smc_fallbacks=%llu "
                    "bridge_native_handoffs=%llu trace_events=%u\n",
                    static_cast<unsigned long long>(g_ram_smc_fallbacks),
                    static_cast<unsigned long long>(g_bridge_native_handoffs),
                    runtime_trace_total());
        return;
    }

    // Coverage honesty: this run was NOT fully static.
    std::printf("self_heal_coverage=NOT_STATIC dispatch_misses=%zu "
                "interpreted_insns=%llu healed_native=%llu native_calls=%llu "
                "inflight=%llu failed=%llu ram_healed=%llu "
                "ram_revalidations=%llu ram_crc_mismatches=%llu "
                "ram_smc_fallbacks=%llu bridge_native_handoffs=%llu "
                "trace_events=%u\n",
                g_misses.size(),
                static_cast<unsigned long long>(g_interp_insns),
                static_cast<unsigned long long>(healed),
                static_cast<unsigned long long>(native_calls),
                static_cast<unsigned long long>(inflight),
                static_cast<unsigned long long>(failed),
                static_cast<unsigned long long>(ram_healed),
                static_cast<unsigned long long>(ram_revalidations),
                static_cast<unsigned long long>(ram_crc_mismatches),
                static_cast<unsigned long long>(g_ram_smc_fallbacks),
                static_cast<unsigned long long>(g_bridge_native_handoffs),
                runtime_trace_total());
    // Build the PC-sorted miss list (std::map already orders by PC) and group
    // it once for both the banner summary and the frag: a tight run of
    // same-mode misses is the signature of a computed-jump switch's case
    // targets the finder couldn't size (the Kid_Head lesson,
    // 0x08062894..0x08062920). kJtMaxGap spans an un-hit case block between
    // two hit targets without merging genuinely separate functions.
    constexpr std::uint32_t kJtMaxGap = 0x80;  // bytes between case targets
    constexpr std::size_t   kJtMinRun = 3;     // run length to call it a table
    std::vector<gbarecomp::SelfHealMiss> sorted;
    sorted.reserve(g_misses.size());
    for (const auto& kv : g_misses)
        sorted.push_back({kv.first, kv.second.thumb, kv.second.count});
    const auto clusters =
        gbarecomp::self_heal_cluster_misses(sorted, kJtMaxGap, kJtMinRun);
    std::size_t jt_candidates = 0;
    for (const auto& c : clusters)
        if (c.jump_table_candidate) ++jt_candidates;

    if (!g_misses.empty()) {
        std::printf("  (the interpreter bridged %zu distinct PC(s) this session; "
                    "see %s for [[extra_func]] / [[jump_table]] proposals — "
                    "human-review + merge, never auto-merged)\n",
                    g_misses.size(), frag_path ? frag_path : "(none)");
        if (jt_candidates) {
            std::printf("  %zu jump-table candidate region(s) flagged — prefer a "
                        "sized [[jump_table]] over the per-PC [[extra_func]] "
                        "runs.\n", jt_candidates);
        }
    }
    for (const auto& kv : g_misses) {
        std::uint32_t off = 0;
        const char* sym = gba_symbol_lookup(kv.first, &off);
        std::uint64_t ncalls = 0;
        const bool is_healed =
            gbarecomp::overlay_query(kv.first, kv.second.thumb, &ncalls);
        std::printf("    bridged 0x%08X (%s) x%llu%s%s%s\n",
                    kv.first, kv.second.thumb ? "thumb" : "arm",
                    static_cast<unsigned long long>(kv.second.count),
                    is_healed ? " [HEALED->native]" : "",
                    sym ? " near " : "", sym ? sym : "");
        (void)ncalls;
    }

    if (g_misses.empty() || !frag_path) return;
    std::FILE* f = std::fopen(frag_path, "w");
    if (!f) {
        std::fprintf(stderr,
                     "self_heal: could not open %s for the miss proposal\n",
                     frag_path);
        return;
    }
    std::fprintf(f,
        "# recomp_master_misses.toml.frag — AUTO-GENERATED proposal.\n"
        "# These guest PCs were reached by runtime_dispatch with no generated\n"
        "# function and were bridged through the interpreter this session.\n"
        "# A HUMAN reviews these and merges the genuine ones into the binary's\n"
        "# config; this file is NEVER auto-merged (PRINCIPLES.md \"Never\n"
        "# auto-write game.toml\").\n"
        "#\n"
        "# Proposal shapes:\n"
        "#   [[extra_func]]  — a standalone missed function entry.\n"
        "#   [[jump_table]]  — flagged in a comment when a tight run of\n"
        "#       consecutive same-mode misses looks like a computed-jump\n"
        "#       switch's case targets. Sizing the table covers the whole\n"
        "#       switch in ONE entry; prefer it over the per-case [[extra_func]].\n"
        "# BIOS PCs (< 0x4000) belong in bios/gba_bios.toml; cart PCs in the\n"
        "# game's game.toml.\n");
    std::fprintf(f,
        "#\n# game:  %s\n# code:  %s\n# sha1:  %s\n\n",
        g_prog_title.empty() ? "(unknown)" : g_prog_title.c_str(),
        g_prog_code.empty()  ? "(unknown)" : g_prog_code.c_str(),
        g_prog_sha1.empty()  ? "(unknown)" : g_prog_sha1.c_str());
    for (const auto& c : clusters) {
        if (c.jump_table_candidate) {
            std::fprintf(f,
                "# ── JUMP-TABLE CANDIDATE: %zu consecutive %s misses in "
                "[0x%08X, 0x%08X] ──\n"
                "# Likely the case targets of a computed-jump switch the finder\n"
                "# could not size. PREFER one sized [[jump_table]] over the %zu\n"
                "# [[extra_func]] below: find the abs32 table base (the\n"
                "# `ldr rT,[pc,#..]; add rT,index<<2; ldr/mov pc` dispatcher) and\n"
                "# add `[[jump_table]] addr=<base> stride=4 count=<CMP bound> "
                "format=\"abs32\" entries_mode=\"auto\"`.\n",
                c.end - c.begin, sorted[c.begin].thumb ? "thumb" : "arm",
                sorted[c.begin].pc, sorted[c.end - 1].pc, c.end - c.begin);
        }
        for (std::size_t k = c.begin; k < c.end; ++k) {
            std::fprintf(f,
                "[[extra_func]]\n"
                "addr = 0x%08X\n"
                "mode = \"%s\"\n"
                "note = \"proposed from self-heal miss-log; bridged x%llu%s\"\n\n",
                sorted[k].pc, sorted[k].thumb ? "thumb" : "arm",
                static_cast<unsigned long long>(sorted[k].count),
                c.jump_table_candidate
                  ? "; part of a JUMP-TABLE CANDIDATE (prefer a sized [[jump_table]])"
                  : "");
        }
    }
    std::fclose(f);
}

std::string self_heal_misses_json() {
    std::uint64_t healed = 0, native_calls = 0, inflight = 0, failed = 0;
    std::uint64_t ram_healed = 0, ram_revalidations = 0, ram_crc_mismatches = 0;
    gbarecomp::overlay_counters(&healed, &native_calls, &inflight, &failed,
                                &ram_healed, &ram_revalidations,
                                &ram_crc_mismatches);

    // Reuse the proposal writer's jump-table-candidate grouping so the
    // machine-readable summary flags which misses are case targets of an
    // unsized computed-jump switch (build loop / CI can prefer a sized
    // [[jump_table]] over the per-PC [[extra_func]] runs).
    std::vector<gbarecomp::SelfHealMiss> sorted;
    sorted.reserve(g_misses.size());
    for (const auto& kv : g_misses)
        sorted.push_back({kv.first, kv.second.thumb, kv.second.count});
    const auto clusters = gbarecomp::self_heal_cluster_misses(sorted, 0x80, 3);
    std::map<std::uint32_t, bool> jt_pc;  // pc -> in a jump-table candidate run
    std::size_t jt_regions = 0;
    for (const auto& c : clusters) {
        if (c.jump_table_candidate) ++jt_regions;
        for (std::size_t k = c.begin; k < c.end; ++k)
            jt_pc[sorted[k].pc] = c.jump_table_candidate;
    }

    // FULLY_STATIC iff nothing was bridged AND nothing was healed (a warm
    // cache heals with zero misses but is still NOT fully static).
    const bool fully_static = g_misses.empty() && healed == 0;

    std::string out;
    char buf[640];
    std::snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"program\":\"%s\",\"code\":\"%s\",\"sha1\":\"%s\","
        "\"coverage\":\"%s\",\"enabled\":%s,"
        "\"distinct_misses\":%zu,\"interpreted_insns\":%llu,"
        "\"healed_native\":%llu,\"native_calls\":%llu,\"inflight\":%llu,"
        "\"failed\":%llu,\"ram_healed\":%llu,\"ram_revalidations\":%llu,"
        "\"ram_crc_mismatches\":%llu,\"ram_smc_fallbacks\":%llu,"
        "\"jump_table_candidate_regions\":%zu,\"misses\":[",
        g_prog_title.c_str(), g_prog_code.c_str(), g_prog_sha1.c_str(),
        fully_static ? "FULLY_STATIC" : "NOT_STATIC",
        gbarecomp::overlay_was_enabled() ? "true" : "false",
        g_misses.size(),
        static_cast<unsigned long long>(g_interp_insns),
        static_cast<unsigned long long>(healed),
        static_cast<unsigned long long>(native_calls),
        static_cast<unsigned long long>(inflight),
        static_cast<unsigned long long>(failed),
        static_cast<unsigned long long>(ram_healed),
        static_cast<unsigned long long>(ram_revalidations),
        static_cast<unsigned long long>(ram_crc_mismatches),
        static_cast<unsigned long long>(g_ram_smc_fallbacks),
        jt_regions);
    out += buf;

    bool first = true;
    for (const auto& kv : g_misses) {
        std::uint64_t ncalls = 0;
        const bool is_healed =
            gbarecomp::overlay_query(kv.first, kv.second.thumb, &ncalls);
        std::snprintf(buf, sizeof(buf),
            "%s{\"pc\":\"0x%08X\",\"mode\":\"%s\",\"bridged\":%llu,"
            "\"healed\":%s,\"native_calls\":%llu,\"jump_table_candidate\":%s}",
            first ? "" : ",", kv.first, kv.second.thumb ? "thumb" : "arm",
            static_cast<unsigned long long>(kv.second.count),
            is_healed ? "true" : "false",
            static_cast<unsigned long long>(ncalls),
            jt_pc[kv.first] ? "true" : "false");
        out += buf;
        first = false;
    }
    out += "]}";
    return out;
}

bool self_heal_write_coverage_json(const char* path) {
    if (!path) return false;
    std::FILE* f = std::fopen(path, "w");
    if (!f) {
        std::fprintf(stderr,
                     "self_heal: could not open %s for the coverage summary\n",
                     path);
        return false;
    }
    // Written on EVERY exit (unlike the frag, which only writes on misses) so
    // the build loop can read one file to confirm FULLY_STATIC or drive the
    // TOML merge loop. Same content as the TCP `misses` command.
    const std::string json = self_heal_misses_json();
    std::fwrite(json.data(), 1, json.size(), f);
    std::fputc('\n', f);
    std::fclose(f);
    return true;
}

}  // namespace gbarecomp

namespace {

// Record a bridged miss + loudly log the FIRST occurrence of each PC
// (subsequent occurrences only bump the counter, to avoid log spam on a
// hot bridged routine — the count is surfaced in the exit report).
void record_and_log_miss(std::uint32_t pc, bool thumb) {
    MissRec& r = g_misses[pc];
    bool first = (r.count == 0);
    r.thumb = thumb;
    ++r.count;
    if (first) {
        char symbuf[96];
        symbuf[0] = '\0';
        std::uint32_t off = 0;
        const char* sym = gba_symbol_lookup(pc, &off);
        if (sym) std::snprintf(symbuf, sizeof(symbuf), " <near %s+0x%X>", sym, off);
        std::fprintf(stderr,
            "runtime_arm: SELF-HEAL dispatch miss for pc=0x%08X%s (%s) — no "
            "generated function; bridging through the interpreter and "
            "recording for a TOML proposal (Stage-1 self-healing; NOT fully "
            "static). %s\n",
            pc, symbuf, thumb ? "thumb" : "arm",
            pc < 0x00004000u
                ? "Re-run `gba_recompile --bios bios/gba_bios.bin "
                  "--config bios/gba_bios.toml` after merging the proposal."
                : "Merge the proposal into game.toml and regenerate.");

        const char* trace_on_miss = std::getenv("GBARECOMP_TRACE_ON_DISPATCH_MISS");
        if (trace_on_miss && trace_on_miss[0] != '\0') {
            bool dump = trace_on_miss[0] == '1' && trace_on_miss[1] == '\0';
            if (!dump) {
                const std::uint32_t wanted =
                    static_cast<std::uint32_t>(std::strtoul(trace_on_miss, nullptr, 0));
                dump = wanted == (pc & ~1u);
            }
            if (dump) {
                std::uint32_t depth = 512u;
                if (const char* depth_env = std::getenv("GBARECOMP_TRACE_DUMP_DEPTH")) {
                    const std::uint32_t parsed =
                        static_cast<std::uint32_t>(std::strtoul(depth_env, nullptr, 0));
                    if (parsed != 0u) depth = parsed;
                }
                std::fprintf(stderr,
                             "runtime_trace: dispatch-miss trace for pc=0x%08X\n",
                             pc);
                runtime_trace_dump_recent(depth);
            }
        }
    }
}

}  // namespace

// runtime_dispatch_miss — Stage-1 self-healing interpreter bridge.
//
// Stop-address contract (validated against the codegen direct-call ABI in
// src/armv4t/arm_codegen.cpp): the recompiled caller that reached this miss
// did `runtime_call_push_return(return_pc); runtime_dispatch(target)`. So the
// address control must resume at — when the missed subroutine returns — is the
// top of the call-return stack. We:
//   1. snapshot the call-return stack and capture stop_pc = top frame;
//   2. load g_cpu into the interpreter and point it at target_pc;
//   3. interpret the WHOLE missed call subtree (recompiled callees included)
//      purely, ticking each instruction so IRQs self-deliver via runtime_tick
//      -> runtime_irq into the recompiled BIOS, and routing SWIs via
//      runtime_swi, until the interpreter's PC reaches stop_pc;
//   4. write the interpreter state back into g_cpu and pop exactly the one
//      frame the recompiled callee's `bx lr` would have popped, so the caller
//      resumes as if the subroutine had been a normal recompiled call.
//
// Every interpreted instruction is fingerprinted into the SAME always-on ring
// the generated prologue uses (runtime_insn_fp), so a bridged run diffs
// bit-identically against a non-excluded build.
// Max ARM call-nesting depth reached during the most recent bridge pass. The P6
// sljit gate reads this after its interpreter pass: re-running a healed shard
// recurses one host C-stack frame per nested dispatch, so a function whose call
// nesting exceeds the safe stack headroom is pinned rather than re-run. Tracked
// by the flat interp loop below (which itself costs O(1) host stack), so it is a
// cheap pre-flight of how deep the shard re-run would recurse. Reset at entry.
extern "C" int runtime_ram_code_guard(uint32_t start, uint32_t end,
                                       uint32_t expected_crc32) {
    const uint32_t region = start >> 24;
    if ((region != 0x02u && region != 0x03u) || end <= start ||
        (end >> 24) != region) return 0;

    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus) return 0;
    const uint32_t mask = region == 0x03u ? 0x00007FFFu : 0x0003FFFFu;
    const uint32_t off = start & mask;
    const uint32_t len = end - start;
    if (off + len > mask + 1u) return 0;

    const uint32_t first_page = off >> 12;
    const uint32_t last_page = (off + len - 1u) >> 12;
    if (region == 0x03u) {
        for (uint32_t page = first_page; page <= last_page; ++page)
            g_ram_code_page_mask_iwram |= 1u << page;
    } else {
        for (uint32_t page = first_page; page <= last_page; ++page) {
            if (page < 32u) g_ram_code_page_mask_ewram_lo |= 1ull << page;
            else g_ram_code_page_mask_ewram_hi |= 1ull << (page - 32u);
        }
    }

    RamCodeGuardEntry& guard =
        g_ram_code_guards[{start, end, expected_crc32}];
    bool current = guard.valid;
    for (uint32_t page = first_page; current && page <= last_page; ++page) {
        const unsigned int live_epoch = region == 0x03u
            ? g_ram_code_page_epoch_iwram[page]
            : g_ram_code_page_epoch_ewram[page];
        current = guard.page_epochs[page] == live_epoch;
    }
    if (current) return guard.matched ? 1 : 0;

    const uint8_t* live = region == 0x03u
        ? bus->iwram_ptr() + off
        : bus->ewram_ptr() + off;
    guard.matched = gba::crc32(live, len) == expected_crc32;
    guard.valid = true;
    for (uint32_t page = first_page; page <= last_page; ++page) {
        guard.page_epochs[page] = region == 0x03u
            ? g_ram_code_page_epoch_iwram[page]
            : g_ram_code_page_epoch_ewram[page];
    }
    return guard.matched ? 1 : 0;
}

// Optional game-side observer of the two RAM fallback paths below
// (runtime_mutable_ram_code_miss, runtime_dispatch_miss), called before
// either acts, with the live g_cpu untouched. `entry_pc` is the pc the caller
// asked for; `pc` is where execution will actually continue (an alias resume
// pc can replace it). Null unless the game installs one.
extern "C" void (*g_runtime_ram_fallback_probe)(uint32_t pc, uint32_t entry_pc,
                                                int thumb, const char* why) =
    nullptr;

extern "C" void runtime_mutable_ram_code_miss(uint32_t entry_pc, int thumb) {
    uint32_t actual_pc = entry_pc & ~1u;
    if (g_runtime_resume_pc != 0u) {
        actual_pc = g_runtime_resume_pc & ~1u;
        g_runtime_resume_pc = 0u;
    }
    if (g_runtime_ram_fallback_probe)
        g_runtime_ram_fallback_probe(actual_pc, entry_pc & ~1u, thumb,
                                     "mutable-ram-code");
    ++g_ram_smc_fallbacks;
    gbarecomp::overlay_note_smc_fallback(actual_pc, thumb != 0);
    if (g_ram_smc_fallbacks == 1u) {
        std::fprintf(stderr,
            "runtime_arm: mutable RAM code changed before pc=0x%08X (%s); "
            "bypassing stale AOT code through the interpreter/self-heal tier. "
            "This run is NOT fully static.\n",
            actual_pc, thumb ? "thumb" : "arm");
    }
    // This call site is reached from INSIDE a generated function's own AOT
    // guard (emit_function.cpp), so — unlike the normal runtime_dispatch()
    // miss path — the Stage-2 healed-overlay tier has never been consulted
    // for this exact (pc, mode, live-bytes) yet. A RAM slot that alternates
    // between a small, bounded set of self-assembled variants (the Golden
    // Sun sound-driver blitter pool is exactly this shape) heals each
    // variant once; every later return to a previously-seen variant would
    // otherwise pay a full interpreter bridge again on every single
    // occurrence, forever, instead of the one memoized native call +
    // overlay_try_dispatch's own CRC recheck. overlay_try_dispatch only
    // returns 1 after independently re-verifying the live bytes against the
    // exact range/CRC its cached entry was compiled from, so this can never
    // dispatch code that does not match what is resident right now — same
    // guarantee the ordinary runtime_dispatch() miss path already relies on
    // (runtime_arm.cpp calls it before runtime_dispatch_miss too). On a
    // miss (unknown/never-healed content) this is a single hash-map probe
    // that returns 0, and behavior is byte-for-byte the same as before.
    if (overlay_try_dispatch(actual_pc, thumb)) return;
    runtime_dispatch_miss(actual_pc | (thumb ? 1u : 0u));
}

extern "C" void runtime_iwram_code_write_dump(const char* why);
extern "C" void runtime_unpacker_slot_dump(const char* why);
extern "C" uint32_t g_bridge_max_call_depth = 0;

// LP-005 probe: tick-origin tag consumed by cyc_probe() in runtime_bus_bridge.cpp.
extern "C" const char* g_tick_ctx;

// Set by runtime_irq() in runtime_arm.cpp (>0 while an IRQ handler is on the host
// stack). When an in-handler SWI unwinds and runtime_irq() re-dispatches a mid-
// handler PC that MISSES, the bridge must DRIVE the handler forward (heal back to
// native at the first static entry so the recompiled intr_main can iret) rather
// than stopping at the LR fallback / call-stack-top — otherwise the handler's
// tail (e.g. LeafGreen VBlankIntr's gMain.intrCheck |= VBLANK) is skipped and the
// game's VBlank wait spins forever. Pairs with the call-return floor in
// runtime_irq() (g_call_return_floor): the floor keeps the cancel cascade from
// corrupting mainline frames; this keeps the self-heal bridge from bailing early.
extern "C" uint32_t g_irq_nest_depth;

struct Mp2kObserverBus : armv4t::Bus {
    gba::GbaBus* raw;
    explicit Mp2kObserverBus(gba::GbaBus* b) : raw(b) {}
    uint8_t  read8 (uint32_t a) override { return raw->read8(a); }
    uint16_t read16(uint32_t a) override { return raw->read16(a); }
    uint32_t read32(uint32_t a) override { return raw->read32(a); }
    void write8(uint32_t a, uint8_t v) override {
        const uint32_t before = raw->read8(a);
        if (g_runtime_mp2k_write_hook)
            g_runtime_mp2k_write_hook(g_cpu.R[15], a, v, 1,
                                      g_cpu.R[6], g_cpu.R[11], before,
                                      (g_cpu.cpsr & CPSR_T_BIT) != 0, 1);
        raw->write8(a, v);
        if (g_runtime_mp2k_write_hook)
            g_runtime_mp2k_write_hook(g_cpu.R[15], a, v, 1,
                                      g_cpu.R[6], g_cpu.R[11], before,
                                      (g_cpu.cpsr & CPSR_T_BIT) != 0, 0);
    }
    void write16(uint32_t a, uint16_t v) override {
        const uint32_t before = raw->read16(a);
        if (g_runtime_mp2k_write_hook)
            g_runtime_mp2k_write_hook(g_cpu.R[15], a, v, 2,
                                      g_cpu.R[6], g_cpu.R[11], before,
                                      (g_cpu.cpsr & CPSR_T_BIT) != 0, 1);
        raw->write16(a, v);
        if (g_runtime_mp2k_write_hook)
            g_runtime_mp2k_write_hook(g_cpu.R[15], a, v, 2,
                                      g_cpu.R[6], g_cpu.R[11], before,
                                      (g_cpu.cpsr & CPSR_T_BIT) != 0, 0);
    }
    void write32(uint32_t a, uint32_t v) override {
        const uint32_t before = raw->read32(a);
        if (g_runtime_mp2k_write_hook)
            g_runtime_mp2k_write_hook(g_cpu.R[15], a, v, 4,
                                      g_cpu.R[6], g_cpu.R[11], before,
                                      (g_cpu.cpsr & CPSR_T_BIT) != 0, 1);
        raw->write32(a, v);
        if (g_runtime_mp2k_write_hook)
            g_runtime_mp2k_write_hook(g_cpu.R[15], a, v, 4,
                                      g_cpu.R[6], g_cpu.R[11], before,
                                      (g_cpu.cpsr & CPSR_T_BIT) != 0, 0);
    }
    uint32_t access_cycles(uint32_t a, uint8_t w, bool s) const override {
        return raw->access_cycles(a, w, s);
    }
};

// runtime_bridge_interpret — the interpret-the-missed-subtree core, factored out
// of runtime_dispatch_miss so the P6 sljit differential gate can reuse it as its
// "interpreter pass": the kept, correct result a healed shard is validated
// against. Interprets from (entry_pc, entry_thumb) using the SAME stop-address
// contract and per-instruction tick (IRQ self-delivery + SWI routing) as the
// on-miss bridge, mutating g_cpu live. The miss log + heal enqueue belong to the
// dispatch-miss handler (below), not here.
extern "C" int runtime_bridge_interpret(uint32_t entry_pc, bool entry_thumb,
                                        uint32_t forced_stop_pc,
                                        std::uint64_t max_instrs) {
    using gbarecomp::active_bus;

    // Instruction budget: 0 → the default 200M abort-on-runaway (the on-miss
    // bridge); non-zero → a bounded gate pass that returns 0 instead of aborting.
    const std::uint64_t cap = max_instrs != 0u ? max_instrs : kBridgeIterationCap;

    gba::GbaBus* bus = active_bus();
    if (!bus) {
        // No bus bound (e.g. a unit harness that didn't set one up): we
        // cannot bridge. Stay loud — this is not a self-healable state.
        std::fprintf(stderr,
                     "runtime_arm: bridge interpret for pc=0x%08X with no active "
                     "bus — cannot bridge.\n", entry_pc);
        runtime_trace_dump_recent(96);
        std::abort();
    }

    // ── (1) Stop-address contract ──────────────────────────────────
    const std::uint32_t entry_depth = runtime_call_stack_depth();
    std::uint32_t stop_pc;
    bool top_level = (entry_depth == 0);
    if (forced_stop_pc != 0u) {
        // Caller-supplied stop (the P6 gate passes the entry LR). A function
        // reached by a computed jump has no matching call-return frame, so the
        // stack-top contract below would mis-target and the walk would run away.
        stop_pc = forced_stop_pc & ~1u;
    } else if (!top_level) {
        stop_pc = runtime_call_stack_data()[entry_depth - 1] & ~1u;
    } else {
        // No pending direct-call return — a vector/top-level miss (e.g. an
        // exception-return that dispatched a mid-function resume point, like
        // the GBA BIOS IntrWait/Halt continuation at 0xBD4). There is no
        // reliable return address: LR can equal entry_pc, a stop the routine
        // never reaches, which would interpret the whole program and run away
        // to a garbage PC. Use LR as a fallback stop, but the loop below ALSO
        // heals-to-static: it hands control back the instant execution
        // re-enters a statically-recompiled function. Stay loud + capped.
        stop_pc = g_cpu.R[14] & ~1u;
        // Loud ONCE per PC, then silent (counts surfaced in the exit miss
        // report) — see MissRec::toplevel_logged. Mirrors record_and_log_miss.
        MissRec& mr = g_misses[entry_pc & ~1u];
        if (!mr.toplevel_logged) {
            mr.toplevel_logged = true;
            std::fprintf(stderr,
                         "runtime_arm: SELF-HEAL bridge entered at top level "
                         "(empty call-return stack) for pc=0x%08X; LR=0x%08X "
                         "(fallback stop) — will heal to the next static entry. "
                         "[logged once/PC; see exit miss-report for counts]\n",
                         entry_pc, stop_pc);
        }
    }
    // IRQ-continuation mode: bridging a dispatch miss while an IRQ handler is in
    // progress (runtime_irq() re-dispatched a mid-handler PC after an in-handler
    // SWI unwound). The normal stop contract (LR fallback / call-stack-top) is
    // invalid here — drive forward and heal to native at the first static entry
    // so the recompiled intr_main reaches its iret. (forced_stop_pc != 0 is the
    // sljit gate's bounded pass — never an IRQ continuation.)
    const bool irq_cont = (g_irq_nest_depth != 0u) && (forced_stop_pc == 0u);

    // Snapshot the entry stack so we can restore it exactly (minus the popped
    // frame) regardless of how IRQ/SWI excursions perturb it mid-bridge.
    std::vector<std::uint32_t> entry_stack;
    if (entry_depth > 0) {
        const std::uint32_t* data = runtime_call_stack_data();
        entry_stack.assign(data, data + entry_depth);
    }

    // ── (2) Enter the interpreter at target_pc ─────────────────────
    armv4t::CPUState cpu{};
    gbarecomp::load_arm_cpu_into_interp(g_cpu, cpu);
    cpu.R[15]    = entry_pc;
    cpu.cpsr.t   = entry_thumb;
    cpu.thumb    = entry_thumb;

    // ── (3) Interpret the subtree until control returns to stop_pc ──
    // Call-nesting pre-flight (for the gate's depth bound): a stack of the
    // return addresses of currently-open BL/BLX calls. Push when a call branches,
    // pop when the PC lands back on an open return address. The high-water mark is
    // a conservative estimate of how deep the shard re-run will recurse on the
    // host C stack. Over-counting is safe (pins more); we only pop on an actual
    // branch to avoid a spurious pop under-counting the depth.
    std::vector<std::uint32_t> call_nest;
    Mp2kObserverBus observer_bus(bus);
    g_bridge_max_call_depth = 0;
    std::uint64_t iters = 0;
    std::uint64_t bridged_insns = 0;
    std::array<BridgeGapInstruction, kBridgeGapHistory> gap_history{};
    std::size_t gap_history_count = 0;
    std::size_t gap_history_next = 0;
    // GBARECOMP_AUDIO_PROBE is a debug-only kill switch for the MP2K
    // audio-bridge trace below; read once per process (function-local
    // static, same pattern as golden_sun_wide_diagnostics_enabled() in
    // src/runner_main.cpp), not once per interpreted instruction.
    static const bool audio_probe_enabled =
        std::getenv("GBARECOMP_AUDIO_PROBE") != nullptr;
    for (;;) {
        // Sync interp -> g_cpu (pre-execution state) so the fingerprint ring,
        // the BIOS-read gate, and any IRQ/SWI excursion all see exactly what
        // the generated per-instruction prologue would have set.
        gbarecomp::store_interp_into_arm_cpu(cpu, g_cpu);
        const std::uint32_t pc = cpu.R[15];
        const bool was_thumb = cpu.thumb;
        g_cpu.R[15] = pc;  // store_interp already set this; explicit for clarity
        if (audio_probe_enabled &&
            (pc == 0x080F9B74u || pc == 0x080F9C90u ||
             pc == 0x080F9EE8u || pc == 0x080F9F6Cu ||
             pc == 0x080FA1FCu)) {
            static unsigned audio_bridge_logs = 0;
            if (audio_bridge_logs++ < 24u) {
                std::fprintf(stderr,
                             "[audio-bridge] pc=0x%08x r0=0x%08x r1=0x%08x "
                             "r2=0x%08x r3=0x%08x r4=0x%08x r5=0x%08x r6=0x%08x "
                             "r7=0x%08x r9=0x%08x r10=0x%08x r11=0x%08x\n",
                             pc, cpu.R[0], cpu.R[1], cpu.R[2], cpu.R[3],
                             cpu.R[4], cpu.R[5], cpu.R[6], cpu.R[7], cpu.R[9],
                             cpu.R[10], cpu.R[11]);
            }
        }
        // A dispatch miss can enter SoundMainRAM directly into this
        // interpreter bridge, bypassing the ordinary RAM dispatcher. Keep
        // the native MP2K shadow synchronized in that path too; the audio
        // object filters this notification to the ROM-detected entry only.
        const uint32_t hook_key = pc | (was_thumb ? 1u : 0u);
        const bool hook_relevant =
            bus->audio().mp2k_frame_hook_may_be_relevant(hook_key);
        const bool cost_probe = g_cost_probe_enabled();
        if (cost_probe) {
            ++g_cost_mp2k_hook_checks;
            if (hook_relevant) ++g_cost_mp2k_hook_matches;
        }
        if (hook_relevant) {
            if (cost_probe) ++g_cost_mp2k_hook_deep_calls;
            bus->audio().mp2k_frame_hook(hook_key, g_runtime_cycles);
        }
        bus->set_bios_access_enabled(pc < 0x00004000u);
        if (g_runtime_insn_trace) runtime_insn_fp();

        // Fetch + decode at the current PC.
        armv4t::Instr insn{};
        if (cpu.thumb) {
            insn = armv4t::ThumbDecoder::decode(bus->read16(pc), pc);
        } else {
            insn = armv4t::ArmDecoder::decode(bus->read32(pc), pc);
        }

        std::uint32_t cyc = 1;
        armv4t::Interpreter::Result r =
            armv4t::Interpreter::step(cpu, observer_bus, insn, &cyc);
        ++bridged_insns;

        capture_bridge_gap_instruction(
            gap_history[gap_history_next], cpu, insn,
            static_cast<std::uint8_t>(r));
        gap_history_next = (gap_history_next + 1) % kBridgeGapHistory;
        if (gap_history_count < kBridgeGapHistory) ++gap_history_count;

        if (r == armv4t::Interpreter::Result::NotImplemented ||
            r == armv4t::Interpreter::Result::Undefined) {
            // A genuine interpreter gap inside the bridged subtree. The
            // interpreter is the reference model; if IT can't execute this,
            // self-healing has nothing to fall back to. Stay loud (this is the
            // interpreter's own NotImplemented gate — PRINCIPLES.md "Genuine
            // interpreter gaps still abort loudly").
            gbarecomp::store_interp_into_arm_cpu(cpu, g_cpu);
            std::fprintf(stderr,
                "runtime_arm: SELF-HEAL bridge hit interpreter %s at "
                "pc=0x%08X while bridging dispatch miss 0x%08X. The reference "
                "interpreter cannot execute this op; add its lowering in "
                "src/armv4t/interpreter.cpp.\n",
                r == armv4t::Interpreter::Result::Undefined ? "Undefined"
                                                            : "NotImplemented",
                pc, entry_pc);
            std::fprintf(stderr,
                "runtime_arm: interpreter-gap context: mode=%s raw=0x%08x "
                "decoded_op=%s is_undefined=%u entry=0x%08x stop=0x%08x "
                "iters=%llu bridged=%llu call_depth=%zu max_call_depth=%u "
                "cycles=%llu cpsr=0x%08x r0=0x%08x r1=0x%08x r2=0x%08x "
                "r3=0x%08x r12=0x%08x sp=0x%08x lr=0x%08x\n",
                was_thumb ? "thumb" : "arm", insn.raw,
                armv4t::ir_op_name(insn.op), insn.is_undefined ? 1u : 0u,
                entry_pc, stop_pc, static_cast<unsigned long long>(iters),
                static_cast<unsigned long long>(bridged_insns), call_nest.size(),
                g_bridge_max_call_depth,
                static_cast<unsigned long long>(g_runtime_cycles),
                bridge_gap_cpsr(cpu.cpsr), cpu.R[0], cpu.R[1], cpu.R[2],
                cpu.R[3], cpu.R[12], cpu.R[13], cpu.R[14]);
            log_bridge_gap_history(gap_history, gap_history_count,
                                   gap_history_next);
            runtime_trace_dump_recent(96);
            std::abort();
        }

        if (r == armv4t::Interpreter::Result::Swi) {
            // Route the SWI through the recompiled BIOS exactly as generated
            // SWI codegen does: set the return address, then runtime_swi
            // (which masks IRQs, ticks the SWI's 3 cycles, and dispatches to
            // the recompiled BIOS SWI vector). Do NOT use the interpreter's
            // enter_swi, and do NOT additionally tick here.
            const std::uint32_t next_pc = pc + (cpu.thumb ? 2u : 4u);
            gbarecomp::store_interp_into_arm_cpu(cpu, g_cpu);
            g_cpu.R[15] = next_pc;
            runtime_swi(insn.swi_imm);
            gbarecomp::load_arm_cpu_into_interp(g_cpu, cpu);
        } else {
            // Normal / Branched: tick the instruction's cost. runtime_tick
            // self-delivers any pending IRQ via runtime_irq into the
            // recompiled BIOS (mutating g_cpu), so sync around it.
            gbarecomp::store_interp_into_arm_cpu(cpu, g_cpu);
            g_tick_ctx = "bridge"; runtime_tick(cyc); g_tick_ctx = "gen";
            gbarecomp::load_arm_cpu_into_interp(g_cpu, cpu);
        }

        // ── HALT inside a bridged subtree ───────────────────────────────
        // The main loop (runtime.cpp step_once) pumps idle cycles while the
        // guest is halted, which is what lets a pending IRQ become due and
        // wake it. The bridge had no equivalent: a subtree that executed a
        // HALT just kept interpreting, so the only cycles that elapsed were
        // the ones the guest's own spin loop charged. A wait-for-VBlank inside
        // a bridged routine therefore never woke — an unrecoverable freeze
        // with no abort and no diagnostic, which is exactly what a hang looks
        // like from the player's side.
        //
        // Pump the same way step_once does: advance to the nearest device
        // event, letting runtime_tick deliver the IRQ (it handles the
        // wake-from-HALT latency itself). Bounded per instruction so a guest
        // that halts with no interrupt possible still hits the iteration cap
        // below and reports, rather than spinning here forever.
        if (bus->io().halted()) {
            gbarecomp::store_interp_into_arm_cpu(cpu, g_cpu);
            gba::GbaPpu* ppu = gbarecomp::active_ppu();
            std::uint32_t idle_budget = gba::GbaPpu::kCyclesPerFrame;
            while (bus->io().halted() && idle_budget != 0) {
                std::uint32_t chunk = ppu ? ppu->cycles_until_next_event()
                                          : 1u;
                const std::uint32_t until_timer =
                    bus->io().cycles_until_next_timer_event();
                if (until_timer < chunk) chunk = until_timer;
                const std::uint32_t until_sample =
                    bus->audio().cycles_until_next_sample();
                if (until_sample < chunk) chunk = until_sample;
                if (chunk == 0u || chunk == 0xFFFFFFFFu) chunk = 1u;
                if (chunk > idle_budget) chunk = idle_budget;
                g_tick_ctx = "bridge-halt";
                runtime_tick(chunk);
                g_tick_ctx = "gen";
                idle_budget -= chunk;
            }
            gbarecomp::load_arm_cpu_into_interp(g_cpu, cpu);
        }

        // Call-nesting pre-flight update (see the call_nest declaration above).
        {
            const std::uint32_t newpc  = cpu.R[15] & ~1u;
            const std::uint32_t seqpc  = (pc + (was_thumb ? 2u : 4u)) & ~1u;
            const bool branched = (newpc != seqpc);
            // Pop returns: an actual branch landing on an open call's return addr.
            if (branched)
                while (!call_nest.empty() && newpc == (call_nest.back() & ~1u))
                    call_nest.pop_back();
            // Push calls: BL/BL_suffix(THUMB)/BLX that actually transferred.
            const bool is_call = branched &&
                (insn.op == armv4t::IrOp::BL ||
                 insn.op == armv4t::IrOp::BL_suffix ||
                 insn.op == armv4t::IrOp::BLX_reg);
            if (is_call) {
                call_nest.push_back(cpu.R[14]);  // the return address the call set
                if (call_nest.size() > g_bridge_max_call_depth)
                    g_bridge_max_call_depth =
                        static_cast<std::uint32_t>(call_nest.size());

                // Native call-boundary handoff. The bridge exists to interpret
                // GENUINELY unknown code; a callee this BL just reached may
                // already have a verified native body (an AOT-static function
                // or a healed overlay) sitting right there in the dispatch
                // tables/g_healed. Without this check the loop above would
                // single-step through that callee's entire body — and any
                // calls IT makes — even though calling it natively is both
                // correct and available. Mirror the exact idiom generated code
                // uses for an unresolved BL (arm_codegen.cpp emit_branch): the
                // return is already pushed (call_nest.push_back above mirrors
                // runtime_call_push_return; push the real one too so a nested
                // dispatch_miss — the rare RAM-CRC-race case below — gets the
                // correct stop_pc contract), then call runtime_dispatch, which
                // re-runs the SAME static-table lookup / RAM dirty-CRC gate /
                // overlay_try_dispatch chain any other dispatch pays — no new
                // verification path, no bypass of the CRC32 check.
                const uint32_t callee_pc = newpc;
                const bool callee_thumb = cpu.thumb;
                // A background heal may finish while this one bridge is still
                // walking a large unknown subtree. Publish only already-ready
                // results here, at the same guest call boundary where we are
                // about to query/dispatch them. This never waits or compiles.
                // Without it, a hot generated callee can be interpreted
                // hundreds of times until the outer bridge finally returns to
                // the frame loop, even though its native body is ready.
                gbarecomp::overlay_drain_ready();
                if (runtime_has_static_entry(callee_pc, callee_thumb ? 1 : 0) ||
                    gbarecomp::overlay_query(callee_pc, callee_thumb, nullptr)) {
                    const std::uint32_t expect_ret = cpu.R[14] & ~1u;
                    runtime_call_push_return(expect_ret);
                    runtime_dispatch(callee_pc | (callee_thumb ? 1u : 0u));
                    gbarecomp::load_arm_cpu_into_interp(g_cpu, cpu);
                    ++g_bridge_native_handoffs;
                    if ((cpu.R[15] & ~1u) == expect_ret) {
                        // Ordinary return: this call's frame is closed. Pop
                        // both the just-pushed real return-stack entry (it
                        // served its purpose — nothing beneath us needs it any
                        // more) and our local call_nest mirror of it.
                        runtime_call_cancel_return(expect_ret);
                        if (!call_nest.empty() &&
                            (call_nest.back() & ~1u) == expect_ret)
                            call_nest.pop_back();
                    } else {
                        // The callee did NOT return to the expected site — the
                        // exact "unresolved control flow" case codegen's own
                        // indirect-BL post-dispatch check exists for (see
                        // emit_direct_branch: `if (R15 != link_value) {
                        // runtime_call_cancel_return(link_value); return; }`).
                        // Mirror it exactly: drop our own pushed frame if it's
                        // still there (a nested miss that healed-to-static
                        // bypasses should_return entirely and never pops it —
                        // should_return only fires on a normal `bx lr`/`pop
                        // {pc}` return idiom), then STOP interpreting rather
                        // than looping back to single-step from wherever we
                        // landed. Continuing here — as an earlier version of
                        // this fix did — re-decodes the landing PC as a fresh
                        // guest instruction on the NEXT iteration; if that PC
                        // is itself a call boundary into native code, the same
                        // mismatch recurs and leaks one call_return_stack
                        // frame per cycle, eventually overflowing it (observed
                        // in testing: depth climbing 183, 184, 185... to the
                        // 1024-entry limit). Breaking here hands the landed PC
                        // straight back to whatever called this bridge — a
                        // generated caller's own post-dispatch check, or an
                        // enclosing bridge's own match check just like this
                        // one — which is exactly how a genuine non-local
                        // control transfer already propagates outward
                        // everywhere else in this runtime.
                        runtime_call_cancel_return(expect_ret);
                        gbarecomp::store_interp_into_arm_cpu(cpu, g_cpu);
                        break;
                    }
                }
            }
        }

        if (!irq_cont && (cpu.R[15] & ~1u) == stop_pc) break;

        // Heal-to-static: for a top-level miss (no reliable return address),
        // the moment the bridged subtree branches/returns back into a
        // statically-recompiled function, stop interpreting and let the main
        // dispatch loop resume native execution there. g_cpu.R[15] is already
        // synced (store_interp_into_arm_cpu above), so returning hands the PC
        // straight to runtime_dispatch. Without this, a degenerate stop_pc
        // (LR == entry_pc) makes the bridge interpret the whole program and
        // eventually run away to a garbage PC (FireRed BIOS IntrWait).
        // Heal to static for an IRQ continuation too (regardless of top_level):
        // the moment execution re-enters a recompiled function, hand it back to
        // native so the recompiled intr_main can run its iret (which sets
        // g_irq_iret_depth via runtime_exception_return) and terminate
        // runtime_irq()'s drive loop.
        if ((irq_cont || top_level) && (cpu.R[15] & ~1u) != entry_pc &&
            runtime_has_static_entry(cpu.R[15], cpu.thumb ? 1 : 0)) {
            break;
        }

        // Top-level generated-RAM misses are resumed by the normal outer
        // dispatch loop.  Keeping this slice outside the forced-stop gate is
        // important: the sljit differential pass relies on one complete,
        // bounded subtree and must retain its existing stop-address contract.
        // IRQ continuations stay in runtime_irq()'s synchronous drive loop,
        // which has no host-present boundary to service, so do not split them.
        const bool ram_entry =
            (entry_pc >= 0x02000000u && entry_pc < 0x04000000u);
        if (forced_stop_pc == 0u && top_level && !irq_cont && ram_entry &&
            bridged_insns >= kTopLevelRamBridgeSlice) {
            break;
        }

        if (++iters > cap) {
            if (max_instrs != 0u) {
                // Gate (bounded) mode: the validation's interp pass didn't reach
                // its stop within the budget — almost always a wrong stop (a
                // function reached via a computed jump whose LR points far up the
                // call chain). Don't abort; return 0 so the gate restores the
                // pre-call state and falls back to the normal dispatch path.
                gbarecomp::store_interp_into_arm_cpu(cpu, g_cpu);
                return 0;
            }
            gbarecomp::store_interp_into_arm_cpu(cpu, g_cpu);
            std::fprintf(stderr,
                "runtime_arm: SELF-HEAL bridge for 0x%08X exceeded %llu "
                "instructions without returning to stop_pc=0x%08X (current "
                "pc=0x%08X). Aborting rather than spinning silently.\n",
                entry_pc,
                static_cast<unsigned long long>(kBridgeIterationCap),
                stop_pc, cpu.R[15]);
            runtime_iwram_code_write_dump("bridge-abort");
            if ((entry_pc >= 0x03002000u && entry_pc < 0x030022C4u) ||
                (cpu.R[15] >= 0x03002000u && cpu.R[15] < 0x030022C4u) ||
                (stop_pc >= 0x03002000u && stop_pc < 0x030022C4u)) {
                runtime_unpacker_slot_dump("bridge-abort");
            }
            runtime_trace_dump_recent(96);
            std::abort();
        }
    }

    g_interp_insns += bridged_insns;

    // ── (4) Write back + pop the returned frame ────────────────────
    gbarecomp::store_interp_into_arm_cpu(cpu, g_cpu);  // g_cpu.R[15] == stop_pc
    bus->set_bios_access_enabled(g_cpu.R[15] < 0x00004000u);
    // An IRQ continuation that healed to static is not a subroutine return —
    // runtime_irq()'s loop re-dispatches the static entry natively — so do not
    // pop a call-return frame here.
    if (!top_level && !irq_cont) {
        // Restore the entry stack minus exactly the one frame the missed
        // subroutine's return idiom (`bx lr` / `pop {pc}`) would have popped
        // via runtime_call_should_return.
        runtime_call_stack_restore(entry_stack.data(), entry_depth - 1);
    }
    // Control returns to the recompiled caller's BL site, which checks
    // g_cpu.R[15] == its pushed return address (== stop_pc) and continues.
    return 1;
}

// ── Whole-program force-interpreter backend (GBA_COSIM / GBARECOMP_FORCE_INTERP) ──
// The recomp-vs-interp co-simulation's "interp" backend (COSIM_ORACLE.md §1). Set
// from env at startup (runtime.cpp); when nonzero, the main loop calls
// runtime_force_interp_step() once per guest instruction INSTEAD of dispatching a
// generated function. Each call fetches/decodes/steps ONE instruction through the
// reference interpreter and advances the shared machine through the SAME
// runtime_tick (master clock + device catch-up + IRQ self-delivery into the
// recompiled BIOS) and runtime_swi (recompiled BIOS SWI) the generated backend
// uses. So the two co-sim backends share the ENTIRE device / IRQ / BIOS /
// master-clock path and differ ONLY in how a main-thread guest instruction is
// executed — isolating recompiler codegen correctness. This is the proven
// self-heal bridge body (runtime_bridge_interpret) minus the subtree stop/heal/
// call-nest return contract: there is no native frame to return to, and guest
// branches are handled inside the interpreter, so runtime_dispatch is never
// called from here.
//
// v1 LIMITATION (documented, closeable later): SWI handlers route through
// runtime_swi and IRQ handlers through runtime_tick -> runtime_irq, both of which
// run the RECOMPILED BIOS (and any game IRQ callback the BIOS dispatches runs
// generated). So BIOS + IRQ-handler code executes recompiled on BOTH co-sim sides
// and is NOT compared. Acceptable for v1 (BIOS is gate-validated flawless) and it
// keeps device/IRQ timing byte-identical across backends. Closing it (force-interp
// the dispatch inside runtime_irq) is a v2 item.
extern "C" int g_force_interp = 0;

// The recomp routes EVERY guest data access through these bridge accessors
// (runtime_bus_bridge.cpp), which do runtime_mmio_catch_up() on IO addresses —
// materializing lazily-lagged device state MID-instruction — plus the idle-disturb
// epoch bump and unmapped-read trace. The interpreter's raw GbaBus accessors skip
// all of that, so a timed sound-FIFO DMA cycle-steal lands at a different
// instruction boundary between the two backends (a benign ±N transient that nets to
// zero, but it flags the cosim). Routing the force-interp interpreter's DATA
// accesses through the SAME bridge makes the two memory paths bit-identical.
extern "C" uint8_t  bus_read_u8 (uint32_t);
extern "C" uint16_t bus_read_u16(uint32_t);
extern "C" uint32_t bus_read_u32(uint32_t);
extern "C" void     bus_write_u8 (uint32_t, uint8_t);
extern "C" void     bus_write_u16(uint32_t, uint16_t);
extern "C" void     bus_write_u32(uint32_t, uint32_t);
extern "C" void runtime_note_ram_image_bus_write(uint32_t, uint32_t);

namespace {
// armv4t::Bus adapter whose data accesses forward to the runtime bridge (identical
// to the recomp path). access_cycles forwards to the real bus (the bridge's
// runtime_mem_cycles does the same), so cycle COST is unchanged — only the
// device-materialization timing is now matched. NOT used for the instruction FETCH
// (code fetches don't route through the bridge in the recomp either).
struct ForceInterpBus : armv4t::Bus {
    gba::GbaBus* raw;
    explicit ForceInterpBus(gba::GbaBus* b) : raw(b) {}
    uint8_t  read8 (uint32_t a) override { return bus_read_u8(a); }
    uint16_t read16(uint32_t a) override { return bus_read_u16(a); }
    uint32_t read32(uint32_t a) override { return bus_read_u32(a); }
    void write8 (uint32_t a, uint8_t  v) override {
        const uint32_t before = bus_read_u8(a);
        if (g_runtime_mp2k_write_hook)
            g_runtime_mp2k_write_hook(g_cpu.R[15], a, v, 1,
                                      g_cpu.R[6], g_cpu.R[11], before,
                                      (g_cpu.cpsr & CPSR_T_BIT) != 0, 1);
        bus_write_u8(a, v);
        runtime_note_ram_image_bus_write(a, 1u);
        if (g_runtime_mp2k_write_hook)
            g_runtime_mp2k_write_hook(g_cpu.R[15], a, v, 1,
                                      g_cpu.R[6], g_cpu.R[11], before,
                                      (g_cpu.cpsr & CPSR_T_BIT) != 0, 0);
    }
    void write16(uint32_t a, uint16_t v) override {
        const uint32_t before = bus_read_u16(a);
        if (g_runtime_mp2k_write_hook)
            g_runtime_mp2k_write_hook(g_cpu.R[15], a, v, 2,
                                      g_cpu.R[6], g_cpu.R[11], before,
                                      (g_cpu.cpsr & CPSR_T_BIT) != 0, 1);
        bus_write_u16(a, v);
        runtime_note_ram_image_bus_write(a, 2u);
        if (g_runtime_mp2k_write_hook)
            g_runtime_mp2k_write_hook(g_cpu.R[15], a, v, 2,
                                      g_cpu.R[6], g_cpu.R[11], before,
                                      (g_cpu.cpsr & CPSR_T_BIT) != 0, 0);
    }
    void write32(uint32_t a, uint32_t v) override {
        const uint32_t before = bus_read_u32(a);
        if (g_runtime_mp2k_write_hook)
            g_runtime_mp2k_write_hook(g_cpu.R[15], a, v, 4,
                                      g_cpu.R[6], g_cpu.R[11], before,
                                      (g_cpu.cpsr & CPSR_T_BIT) != 0, 1);
        bus_write_u32(a, v);
        runtime_note_ram_pointer_write(a, v, 4u);
        runtime_note_ram_image_bus_write(a, 4u);
        if (g_runtime_mp2k_write_hook)
            g_runtime_mp2k_write_hook(g_cpu.R[15], a, v, 4,
                                      g_cpu.R[6], g_cpu.R[11], before,
                                      (g_cpu.cpsr & CPSR_T_BIT) != 0, 0);
    }
    uint32_t access_cycles(uint32_t a, uint8_t w, bool s) const override {
        return raw->access_cycles(a, w, s);
    }
};
}  // namespace

extern "C" void runtime_force_interp_step(void) {
    using gbarecomp::active_bus;
    gba::GbaBus* bus = active_bus();
    if (!bus) return;

    // Per-instruction prologue, matching the generated-code prologue exactly
    // (arm_codegen.cpp: "if (runtime_should_yield()) return;"). runtime_should_yield
    // ALSO latches the BIOS open-bus prefetch (runtime_bus_bridge.cpp) while PC is in
    // the BIOS, so the interp backend maintains the SAME bios_prefetch_ / open-bus
    // state the recomp does — without this the co-sim's prefetch sub-hash split every
    // BIOS instruction. On a yield, return without executing (step_once re-enters
    // next iteration), exactly as the generated function returns to the runner.
    if (runtime_should_yield()) { runtime_yield_restore_pc(); return; }

    armv4t::CPUState cpu{};
    gbarecomp::load_arm_cpu_into_interp(g_cpu, cpu);
    const std::uint32_t pc = cpu.R[15];
    bus->set_bios_access_enabled(pc < 0x00004000u);
    if (g_runtime_insn_trace) runtime_insn_fp();

    armv4t::Instr insn{};
    if (cpu.thumb) {
        insn = armv4t::ThumbDecoder::decode(bus->read16(pc), pc);
    } else {
        insn = armv4t::ArmDecoder::decode(bus->read32(pc), pc);
    }

    // Data accesses route through the runtime bridge (device catch-up matched to
    // the recomp); the instruction fetch above stayed on the raw bus, as in the
    // recomp (generated code does not re-fetch its own opcode via the bridge).
    ForceInterpBus fib(bus);
    std::uint32_t cyc = 1;
    const armv4t::Interpreter::Result r =
        armv4t::Interpreter::step(cpu, fib, insn, &cyc);

    if (r == armv4t::Interpreter::Result::NotImplemented ||
        r == armv4t::Interpreter::Result::Undefined) {
        // A genuine interpreter gap. The interpreter is the reference model here;
        // stay loud (PRINCIPLES.md "Genuine interpreter gaps still abort loudly").
        gbarecomp::store_interp_into_arm_cpu(cpu, g_cpu);
        std::fprintf(stderr,
            "runtime_arm: FORCE-INTERP hit interpreter %s at pc=0x%08X. The "
            "reference interpreter cannot execute this op; add its lowering in "
            "src/armv4t/interpreter.cpp.\n",
            r == armv4t::Interpreter::Result::Undefined ? "Undefined"
                                                        : "NotImplemented",
            pc);
        runtime_trace_dump_recent(96);
        std::abort();
    }

    if (r == armv4t::Interpreter::Result::Swi) {
        // Route the SWI through the recompiled BIOS exactly as generated SWI
        // codegen (and the self-heal bridge) does: set the return address, then
        // runtime_swi (masks IRQs, ticks the SWI's cycles, dispatches the
        // recompiled BIOS SWI vector). Do NOT use the interpreter's enter_swi,
        // and do NOT additionally tick here.
        const std::uint32_t next_pc = pc + (cpu.thumb ? 2u : 4u);
        gbarecomp::store_interp_into_arm_cpu(cpu, g_cpu);
        g_cpu.R[15] = next_pc;
        runtime_swi(insn.swi_imm);
    } else {
        // Normal / Branched: tick the instruction's cost. runtime_tick
        // self-delivers any pending IRQ via runtime_irq into the recompiled BIOS.
        gbarecomp::store_interp_into_arm_cpu(cpu, g_cpu);
        g_tick_ctx = "finterp"; runtime_tick(cyc); g_tick_ctx = "gen";
    }
    bus->set_bios_access_enabled(g_cpu.R[15] < 0x00004000u);

    // Post-HALT open-bus prefetch latch — faithfulness parity with the recomp.
    // The recomp's generated code is a contiguous block stream: after the
    // instruction that writes HALTCNT, control falls through to the NEXT block's
    // prologue, which latches the BIOS prefetch for the post-halt PC and only THEN
    // yields on `halted`. The force-interp path instead returns to step_once, whose
    // halt branch intercepts before the next instruction — so that final latch is
    // missed and the open-bus value freezes one instruction early (cosim LP-004:
    // recomp latched prefetch_word(0x348)=BIOS[0x350]=PC+8, the correct ARM fetch;
    // force-interp froze at prefetch_word(0x344)=PC+4). Re-latch here for the new
    // PC when we just halted, matching the recomp's fall-through prologue. (Only
    // the HALT path needs this: a VBlank-spin yield goes through the
    // runtime_should_yield() call at the TOP of this function, which already
    // latches; HALT is checked in step_once before this function is entered.)
    if (bus->io().halted() && g_cpu.R[15] < 0x00004000u) {
        bus->latch_bios_prefetch(g_cpu.R[15], (g_cpu.cpsr & CPSR_T_BIT) != 0);
    }
}

// runtime_dispatch_miss — Stage-1 self-healing entry: log the miss, enqueue a
// Stage-2 heal (so the NEXT hit dispatches native), then bridge through the
// interpreter via runtime_bridge_interpret. See that function's note above.
extern "C" void runtime_dispatch_miss(uint32_t target_pc) {
    const std::uint32_t entry_pc = target_pc & ~1u;
    const bool entry_thumb = (g_cpu.cpsr & CPSR_T_BIT) != 0;
    if (g_runtime_ram_fallback_probe)
        g_runtime_ram_fallback_probe(entry_pc, entry_pc, entry_thumb ? 1 : 0,
                                     "dispatch-miss");
    // Pool history is for the final escaped control target, not ordinary
    // self-heal misses.  The latter are common during startup and world-map
    // entry; dumping 16 rows for each one made the diagnostic itself stall
    // the game and inflated logs into the megabytes.  Only an address outside
    // every GBA bus region is an escaped target worth dumping here.
    const bool plausible_gba_address =
        (entry_pc < 0x00004000u) ||
        (entry_pc >= 0x02000000u && entry_pc < 0x02040000u) ||
        (entry_pc >= 0x03000000u && entry_pc < 0x03008000u) ||
        (entry_pc >= 0x04000000u && entry_pc < 0x04000400u) ||
        (entry_pc >= 0x05000000u && entry_pc < 0x05000400u) ||
        (entry_pc >= 0x06000000u && entry_pc < 0x06018000u) ||
        (entry_pc >= 0x07000000u && entry_pc < 0x07000400u) ||
        (entry_pc >= 0x08000000u && entry_pc < 0x0A000000u) ||
        (entry_pc >= 0x0E000000u && entry_pc < 0x0E010000u);
    if (!plausible_gba_address)
        runtime_pool_dispatch_history_dump(entry_pc);
    // Read once per process, not once per miss. Without the cache this
    // getenv ran on EVERY dispatch miss whose entry sits in IWRAM -- and
    // Golden Sun builds its sprite-drawing routines in IWRAM, so during a
    // summon that is most of them. `miss_iwram_captured` does not help: it
    // is only set once a dump actually happens, so with the variable unset
    // (the normal case) the lookup repeated forever. See the measurement
    // below.
    static const char* const miss_iwram_dump_path =
        std::getenv("GBARECOMP_MISS_IWRAM_DUMP");
    static bool miss_iwram_captured = false;
    if (!miss_iwram_captured &&
        entry_pc >= 0x03000000u && entry_pc < 0x03008000u) {
        if (const char* path = miss_iwram_dump_path) {
            if (gba::GbaBus* bus = gbarecomp::active_bus()) {
                if (std::FILE* f = std::fopen(path, "wb")) {
                    std::fwrite(bus->iwram_ptr(), 1, 32 * 1024, f);
                    std::fclose(f);
                    miss_iwram_captured = true;
                    std::fprintf(stderr,
                        "runtime_arm: captured live IWRAM at RAM dispatch miss "
                        "pc=0x%08X path=\"%s\"\n", entry_pc, path);
                }
            }
        }
    }
    // Also once per process. This one was unconditional: every dispatch miss
    // paid a full scan of the environment block for it.
    //
    // Measured 2026-09-17 on session_20260917_195930, and it settles the CRT
    // hotspot PERFORMANCE.md had carried open since 2026-09-13. The host
    // profiler records the first caller outside msvcrt.dll for each CRT
    // sample: of the 80 such samples, **76 are this function** -- 42 at one
    // call site and 34 at the other, which are exactly these two getenv
    // calls. Nothing else in the process accounted for more than one.
    static const bool strict_static =
        gbarecomp::env_flag("GBARECOMP_STRICT_STATIC");
    if (strict_static) {
        std::fprintf(stderr,
            "runtime_arm: STRICT_STATIC dispatch miss for pc=0x%08X (%s) — "
            "interpreter and overlay fallback are disabled. Add reviewed "
            "static discovery/code-copy metadata and regenerate.\n",
            entry_pc, entry_thumb ? "thumb" : "arm");
        runtime_trace_dump_recent(96);
        std::abort();
    }
    // A worker may have finished this exact miss earlier in the same guest
    // frame. Publish all already-ready results and retry before paying for
    // another interpreter bridge. This is game-thread-only, nonblocking, and
    // never emits or compiles; overlay_try_dispatch directly calls the proven
    // native body and this miss function simply returns.
    gbarecomp::overlay_drain_ready();
    if (RuntimeGuestFn fn =
            gbarecomp::overlay_resolve(entry_pc, entry_thumb ? 1 : 0)) {
        return fn();
    }
    record_and_log_miss(entry_pc, entry_thumb);
    const gbarecomp::OverlayRequestOutcome request_outcome =
        gbarecomp::overlay_request_compile(entry_pc, entry_thumb);
    gbarecomp::overlay_note_dispatch_miss(entry_pc, entry_thumb,
                                          request_outcome);
    // A relocatable exact-byte match is bound synchronously by
    // overlay_request_compile(). Dispatch it now instead of interpreting the
    // same body once unnecessarily. This remains behind overlay_try_dispatch's
    // normal live-RAM CRC/byte validation and does not wait or compile.
    if (request_outcome == gbarecomp::OverlayRequestOutcome::Alias) {
        if (RuntimeGuestFn fn =
                gbarecomp::overlay_resolve(entry_pc, entry_thumb ? 1 : 0)) {
            return fn();
        }
    }
    // Timed so the frame-events instrumentation (GBARECOMP_FRAME_EVENTS) can
    // attribute synchronous game-thread bridge time — not overlay compile,
    // which always runs on the async worker thread — to the frame it stalled.
    const auto bridge_t0 = std::chrono::steady_clock::now();
    const std::uint64_t insns_before = g_interp_insns;
    runtime_bridge_interpret(entry_pc, entry_thumb, /*forced_stop_pc=*/0u,
                             /*max_instrs=*/0u);
    const auto bridge_t1 = std::chrono::steady_clock::now();
    gbarecomp::overlay_note_bridge_miss(entry_pc, entry_thumb, static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            bridge_t1 - bridge_t0).count()), g_interp_insns - insns_before,
            request_outcome);
}

extern "C" void runtime_unimplemented_op(const char* op_name,
                                          uint32_t pc) {
    std::fprintf(stderr,
                 "runtime_arm: unimplemented op %s at pc=0x%08X "
                 "(a CODEGEN gap, not a dispatch miss — NOT self-healed). "
                 "Add the lowering in src/armv4t/arm_codegen.cpp.\n",
                 op_name ? op_name : "(null)", pc);
    runtime_trace_dump_recent(96);
    std::abort();
}
