// ram_heal_test.cpp — regression coverage for Stage-2 self-heal over MUTABLE
// (IWRAM/EWRAM) code, the RAM-blitter case (Golden Sun assembles a sprite
// blitter into IWRAM at runtime; it can never be in the static corpus).
//
// Exercises the real pipeline end to end: overlay_request_compile ->
// worker-thread emit + g++ compile -> overlay_drain_ready installs the
// native fn -> overlay_try_dispatch calls it. No ROM/BIOS bytes are used —
// every function body here is a synthetic two-instruction ARM stub written
// directly into the test's own GbaBus.
//
// This needs a real g++ on PATH (or GBARECOMP_HEAL_CXX) to actually compile
// the emitted overlay; if the toolchain named by gxx_path() genuinely isn't
// reachable the heal will fail loudly (self_heal: compile FAILED ...) and
// the polling loop below times out with a clear failure message rather than
// hanging silently.

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "gba_bus.h"
#include "crc32.h"
#include "hot_queue_policy.h"
#include "overlay_loader.h"
#include "overlay_compile.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"
#include "self_heal.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
    if (cond) return;
    std::printf("FAIL: %s\n", what.c_str());
    ++g_failures;
}

void put32(uint8_t* dst, uint32_t off, uint32_t value) {
    dst[off + 0] = static_cast<uint8_t>(value);
    dst[off + 1] = static_cast<uint8_t>(value >> 8);
    dst[off + 2] = static_cast<uint8_t>(value >> 16);
    dst[off + 3] = static_cast<uint8_t>(value >> 24);
}

void test_hot_queue_priority_and_fairness() {
    struct Item { int id; bool hot; };
    std::deque<Item> queue{
        {0, false}, {1, false}, {2, true}, {3, true},
        {4, true}, {5, true}
    };
    unsigned burst = 0;
    std::vector<int> order;
    while (!queue.empty()) {
        const std::size_t index = gbarecomp::hot_queue_pick_index(
            queue, burst, [](const Item& item) { return item.hot; });
        order.push_back(queue[index].id);
        queue.erase(queue.begin() + static_cast<std::ptrdiff_t>(index));
    }
    const std::vector<int> expected{2, 3, 4, 0, 5, 1};
    check(order == expected,
          "hot queue did not prioritize repeated work with bounded FIFO fairness");
}

void put16(uint8_t* dst, uint32_t off, uint16_t value) {
    dst[off + 0] = static_cast<uint8_t>(value);
    dst[off + 1] = static_cast<uint8_t>(value >> 8);
}

// "MOV r0, #imm8; BX LR" — a two-instruction ARM leaf function. Distinct
// imm8 values produce distinct bodies (distinct CRC32) at the SAME address
// and SAME extent, which is exactly the shape of a re-assembled RAM blitter
// variant.
void write_mov_bx_lr(uint8_t* dst, uint32_t off, uint8_t imm8) {
    put32(dst, off + 0, 0xE3A00000u | imm8);  // MOV r0, #imm8
    put32(dst, off + 4, 0xE12FFF1Eu);          // BX lr
}

// A healed leaf's `bx lr` is emitted as the standard return idiom: it asks
// runtime_call_should_return whether LR matches an open call frame, and only
// falls through to runtime_dispatch(lr) when it does not. A test that calls
// a healed function without an open frame therefore does not "return" — it
// dispatches to whatever LR happens to hold (0 => the BIOS reset vector) and
// spins. Every dispatch below goes through this helper so the call looks
// exactly like a recompiled caller's BL site.
constexpr std::uint32_t kTestReturnPc = 0x08000100u;

int dispatch_as_call(std::uint32_t pc, int thumb) {
    g_cpu.R[14] = kTestReturnPc;
    runtime_call_push_return(kTestReturnPc);
    const int hit = overlay_try_dispatch(pc, thumb);
    // A miss leaves the frame we just pushed; drop it so frames do not leak
    // across tests. A hit consumed it via the return idiom.
    if (!hit) runtime_call_cancel_return(kTestReturnPc);
    return hit;
}

// Poll until `pred` is true or the deadline passes. Drains ready compiles on
// every iteration (that's the game thread's job in production).
bool wait_until(int timeout_ms, const std::function<bool()>& pred) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        gbarecomp::overlay_drain_ready();
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    gbarecomp::overlay_drain_ready();
    return pred();
}

bool wait_healed(uint32_t pc, bool thumb, int timeout_ms = 30000) {
    return wait_until(timeout_ms,
                      [&] { return gbarecomp::overlay_query(pc, thumb, nullptr); });
}

// ── Test 1: IWRAM self-heal — a re-assembled function heals to native and
//    executes it (the motivating case: Golden Sun's sprite blitter). ──────
void test_iwram_heal_and_dispatch(gba::GbaBus& bus) {
    constexpr uint32_t kPc = 0x03000200u;  // well inside IWRAM (32 KiB)
    write_mov_bx_lr(bus.iwram_ptr(), kPc - 0x03000000u, 0xAA);

    gbarecomp::overlay_request_compile(kPc, /*thumb=*/false);
    check(wait_healed(kPc, false), "IWRAM function did not heal to native");

    g_cpu.R[0] = 0;
    const int dispatched = dispatch_as_call(kPc, /*thumb=*/0);
    check(dispatched == 1, "healed IWRAM entry was not dispatched natively");
    check(g_cpu.R[0] == 0xAAu,
         "native IWRAM function did not set r0 (got " +
             std::to_string(g_cpu.R[0]) + ")");
}

void write_three_insn(uint8_t* dst, uint32_t off, uint8_t r0, uint8_t r2) {
    put32(dst, off + 0, 0xE3A00000u | r0);       // MOV r0,#imm
    put32(dst, off + 4, 0xE3A02000u | r2);       // MOV r2,#imm
    put32(dst, off + 8, 0xE12FFF1Eu);            // BX lr
}

uint32_t g_nested_pic_target = 0;
bool g_nested_pic_in_hook = false;
void nested_pic_hook(uint32_t) {
    if (g_nested_pic_in_hook || !g_nested_pic_target) return;
    g_nested_pic_in_hook = true;
    dispatch_as_call(g_nested_pic_target, 0);
    g_nested_pic_in_hook = false;
}

void test_pic_arm_and_thumb_aliases(gba::GbaBus& bus) {
    constexpr uint32_t kArmA = 0x03000240u;
    constexpr uint32_t kArmB = 0x03000280u;
    // MOV r0,pc; BX lr. ARM's visible PC is instruction address + 8.
    for (uint32_t pc : {kArmA, kArmB}) {
        const uint32_t off = pc - 0x03000000u;
        put32(bus.iwram_ptr(), off, 0xE1A0000Fu);
        put32(bus.iwram_ptr(), off + 4u, 0xE12FFF1Eu);
    }
    gbarecomp::overlay_request_compile(kArmA, false);
    check(wait_healed(kArmA, false), "PIC ARM source did not heal");
    check(gbarecomp::overlay_alias_relocatable_for_test(kArmA, kArmB, false),
          "PIC ARM body could not be aliased to second address");
    g_cpu.R[0] = 0;
    check(dispatch_as_call(kArmB, 0) == 1, "PIC ARM alias did not dispatch");
    check(g_cpu.R[0] == kArmB + 8u,
          "PIC ARM alias used source PC instead of destination-relative PC");

    constexpr uint32_t kThumbA = 0x030002C0u;
    constexpr uint32_t kThumbB = 0x030002E0u;
    // ADD r0,pc,#0; BX lr. THUMB PC operand is Align(pc+4,4).
    for (uint32_t pc : {kThumbA, kThumbB}) {
        const uint32_t off = pc - 0x03000000u;
        put16(bus.iwram_ptr(), off, 0xA000u);
        put16(bus.iwram_ptr(), off + 2u, 0x4770u);
    }
    gbarecomp::overlay_request_compile(kThumbA, true);
    check(wait_healed(kThumbA, true), "PIC THUMB source did not heal");
    check(gbarecomp::overlay_alias_relocatable_for_test(kThumbA, kThumbB, true),
          "PIC THUMB body could not be aliased to second address");
    g_cpu.R[0] = 0;
    check(dispatch_as_call(kThumbB, 1) == 1, "PIC THUMB alias did not dispatch");
    check(g_cpu.R[0] == kThumbB + 4u,
          "PIC THUMB alias used source PC instead of destination-relative PC");

    // Mutating the destination must reject the aliased body before execution.
    put32(bus.iwram_ptr(), kArmB - 0x03000000u, 0xE3A00055u);
    g_cpu.R[0] = 0xDEADBEEFu;
    check(dispatch_as_call(kArmB, 0) == 0,
          "stale PIC alias executed after destination bytes changed");
    check(g_cpu.R[0] == 0xDEADBEEFu,
          "rejected PIC alias mutated registers");
}

void test_pic_nested_dispatch_preserves_caller_base(gba::GbaBus& bus) {
    constexpr uint32_t kOuterA = 0x03000340u;
    constexpr uint32_t kOuterB = 0x03000380u;
    constexpr uint32_t kInnerA = 0x030003C0u;
    constexpr uint32_t kInnerB = 0x03000400u;
    for (uint32_t outer : {kOuterA, kOuterB}) {
        const uint32_t off = outer - 0x03000000u;
        put32(bus.iwram_ptr(), off + 0u, 0xE1A0100Fu); // MOV r1,pc
        put32(bus.iwram_ptr(), off + 4u, 0xE12FFF1Eu); // BX lr
    }
    write_mov_bx_lr(bus.iwram_ptr(), kInnerA - 0x03000000u, 0x55u);
    write_mov_bx_lr(bus.iwram_ptr(), kInnerB - 0x03000000u, 0x55u);
    gbarecomp::overlay_request_compile(kOuterA, false);
    gbarecomp::overlay_request_compile(kInnerA, false);
    check(wait_healed(kOuterA, false), "nested PIC outer source did not heal");
    check(wait_healed(kInnerA, false), "nested PIC inner source did not heal");
    check(gbarecomp::overlay_alias_relocatable_for_test(kOuterA, kOuterB, false),
          "nested PIC outer alias failed");
    check(gbarecomp::overlay_alias_relocatable_for_test(kInnerA, kInnerB, false),
          "nested PIC inner alias failed");

    // The direct tail-dispatch path leaves the global base at the native
    // callee, but dispatch_as_call() exercises the compatibility wrapper,
    // whose non-tail ABI must restore the caller's PIC context.
    constexpr uint32_t kSentinel = 0x12345678u;
    g_runtime_image_base = kSentinel;
    g_nested_pic_target = kInnerB;
    g_runtime_fn_entry_hook = nested_pic_hook;
    g_cpu.R[0] = 0;
    g_cpu.R[1] = 0;
    check(dispatch_as_call(kOuterB, 0) == 1,
          "nested PIC outer alias did not dispatch");
    check(g_cpu.R[0] == 0x55u, "nested-base PIC alias returned wrong result");
    check(g_cpu.R[1] == kOuterB + 8u,
          "parent resumed with wrong relocation base after nested dispatch");
    check(g_runtime_image_base == kSentinel,
          "non-tail overlay_try_dispatch did not restore caller image base");
    g_runtime_fn_entry_hook = nullptr;
    g_nested_pic_target = 0;
}

void test_pic_automatic_cross_address_reuse(gba::GbaBus& bus) {
    constexpr uint32_t kA = 0x03001000u;
    constexpr uint32_t kB = 0x03001040u;
    write_mov_bx_lr(bus.iwram_ptr(), kA - 0x03000000u, 0xA5u);
    write_mov_bx_lr(bus.iwram_ptr(), kB - 0x03000000u, 0xA5u);
    gbarecomp::overlay_request_compile(kA, false);
    check(wait_healed(kA, false), "automatic PIC source did not heal");

    uint64_t hits_before = 0, misses_before = 0;
    gbarecomp::overlay_relocatable_alias_counters(&hits_before, &misses_before);
    gbarecomp::overlay_request_compile(kB, false);
    uint64_t hits_after = 0, misses_after = 0, inflight = 99;
    gbarecomp::overlay_relocatable_alias_counters(&hits_after, &misses_after);
    gbarecomp::overlay_counters(nullptr, nullptr, &inflight, nullptr);
    check(hits_after == hits_before + 1,
          "exact cross-address bytes did not bind completed PIC artifact");
    check(inflight == 0, "exact PIC reuse enqueued a second compiler job");
    check(gbarecomp::overlay_query(kB, false, nullptr),
          "exact PIC alias was not immediately queryable");
    g_cpu.R[0] = 0;
    check(dispatch_as_call(kB, 0) == 1 && g_cpu.R[0] == 0xA5u,
          "automatic PIC alias did not execute natively at destination");

    // One changed byte must refuse reuse and enqueue an ordinary heal.
    constexpr uint32_t kMismatch = 0x03001080u;
    write_mov_bx_lr(bus.iwram_ptr(), kMismatch - 0x03000000u, 0xA4u);
    gbarecomp::overlay_request_compile(kMismatch, false);
    gbarecomp::overlay_relocatable_alias_counters(&hits_after, &misses_after);
    check(hits_after == hits_before + 1 && misses_after > misses_before,
          "one-byte mismatch incorrectly reused PIC artifact");
    check(!gbarecomp::overlay_query(kMismatch, false, nullptr),
          "one-byte mismatch became native before its own compile completed");
    check(wait_healed(kMismatch, false), "mismatch fallback compile failed");

    // Exact bytes in the wrong ISA must never bind the ARM artifact.
    constexpr uint32_t kWrongMode = 0x030010C0u;
    write_mov_bx_lr(bus.iwram_ptr(), kWrongMode - 0x03000000u, 0xA5u);
    gbarecomp::overlay_request_compile(kWrongMode, true);
    check(!gbarecomp::overlay_query(kWrongMode, true, nullptr),
          "wrong-mode bytes reused ARM PIC artifact");

    // A shorter function sharing the first instruction is not identical in
    // extent or bytes and must also take its own compile path.
    constexpr uint32_t kLong = 0x03001100u;
    constexpr uint32_t kShort = 0x03001140u;
    put32(bus.iwram_ptr(), kLong - 0x03000000u + 0u, 0xE3A00001u);
    put32(bus.iwram_ptr(), kLong - 0x03000000u + 4u, 0xE3A01002u);
    put32(bus.iwram_ptr(), kLong - 0x03000000u + 8u, 0xE12FFF1Eu);
    put32(bus.iwram_ptr(), kShort - 0x03000000u + 0u, 0xE3A00001u);
    put32(bus.iwram_ptr(), kShort - 0x03000000u + 4u, 0xE12FFF1Eu);
    gbarecomp::overlay_request_compile(kLong, false);
    check(wait_healed(kLong, false), "long PIC fixture did not heal");
    gbarecomp::overlay_request_compile(kShort, false);
    check(!gbarecomp::overlay_query(kShort, false, nullptr),
          "wrong-length PIC body was incorrectly reused");

    // The real miss path must consume a synchronous alias immediately.
    // Previously it bound C natively, then still interpreted C once before
    // returning, causing visible one-frame stalls for generated-code copies.
    constexpr uint32_t kC = 0x030010E0u;
    write_mov_bx_lr(bus.iwram_ptr(), kC - 0x03000000u, 0xA5u);
    g_cpu.R[0] = 0;
    g_cpu.R[14] = kTestReturnPc;
    runtime_call_push_return(kTestReturnPc);
    const uint64_t interpreted_before =
        gbarecomp::self_heal_interpreted_insns();
    runtime_dispatch_miss(kC);
    check(gbarecomp::self_heal_interpreted_insns() == interpreted_before,
          "dispatch miss interpreted despite synchronous exact PIC alias");
    check(g_cpu.R[0] == 0xA5u && g_cpu.R[15] == kTestReturnPc,
          "dispatch-miss PIC alias returned wrong result");
}

void test_same_pc_distinct_requests_are_independent(gba::GbaBus& bus) {
    constexpr uint32_t kPc = 0x03001400u;
    const uint32_t off = kPc - 0x03000000u;
    write_three_insn(bus.iwram_ptr(), off, 0x31u, 0x41u);
    check(gbarecomp::overlay_request_compile(kPc, false) ==
              gbarecomp::OverlayRequestOutcome::Queued,
          "first same-PC content was not queued");
    write_three_insn(bus.iwram_ptr(), off, 0x32u, 0x42u);
    check(gbarecomp::overlay_request_compile(kPc, false) ==
              gbarecomp::OverlayRequestOutcome::Queued,
          "second same-PC content was dropped as already inflight");
    check(wait_until(30000, [] {
        uint64_t inflight = 1;
        gbarecomp::overlay_counters(nullptr, nullptr, &inflight, nullptr);
        return inflight == 0;
    }), "same-PC content requests did not both complete");
    for (const auto pair : {std::pair<uint8_t, uint8_t>{0x31u, 0x41u},
                            std::pair<uint8_t, uint8_t>{0x32u, 0x42u}}) {
        write_three_insn(bus.iwram_ptr(), off, pair.first, pair.second);
        check(gbarecomp::overlay_query(kPc, false, nullptr),
              "one same-PC inflight variant was not installed");
    }

    constexpr uint32_t kFailedPc = 0x03001440u;
    const uint32_t failed_off = kFailedPc - 0x03000000u;
    write_three_insn(bus.iwram_ptr(), failed_off, 0x51u, 0x61u);
    check(gbarecomp::overlay_mark_current_request_failed_for_test(kFailedPc, false),
          "could not seed failed-content identity");
    check(gbarecomp::overlay_request_compile(kFailedPc, false) ==
              gbarecomp::OverlayRequestOutcome::Failed,
          "failed content identity was not remembered");
    write_three_insn(bus.iwram_ptr(), failed_off, 0x52u, 0x62u);
    check(gbarecomp::overlay_request_compile(kFailedPc, false) ==
              gbarecomp::OverlayRequestOutcome::Queued,
          "failed content A poisoned distinct content B");
    check(wait_healed(kFailedPc, false),
          "content B did not heal after content A failed");
}

void test_dispatch_miss_drains_ready_before_bridge(gba::GbaBus& bus) {
    constexpr uint32_t kPc = 0x03001480u;
    write_three_insn(bus.iwram_ptr(), kPc - 0x03000000u, 0x73u, 0x83u);
    check(gbarecomp::overlay_request_compile(kPc, false) ==
              gbarecomp::OverlayRequestOutcome::Queued,
          "drain-before-bridge fixture was not queued");
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(30);
    while (!gbarecomp::overlay_ready_pending_for_test() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check(gbarecomp::overlay_ready_pending_for_test(),
          "worker did not publish drain-before-bridge fixture");
    check(!gbarecomp::overlay_query(kPc, false, nullptr),
          "fixture installed before explicit game-thread drain");
    g_cpu.R[14] = kTestReturnPc;
    runtime_call_push_return(kTestReturnPc);
    const uint64_t before = gbarecomp::self_heal_interpreted_insns();
    runtime_dispatch_miss(kPc);
    check(gbarecomp::self_heal_interpreted_insns() == before,
          "dispatch miss interpreted despite already-ready native result");
    check(g_cpu.R[0] == 0x73u && g_cpu.R[2] == 0x83u,
          "drain-before-bridge native body returned wrong result");
}

// ── Test 2: the regression this design exists for — overwrite the SAME
//    address with a DIFFERENT function body. The stale native must NOT run;
//    the new content must heal and execute instead. ──────────────────────
void test_iwram_content_change_forces_reheal(gba::GbaBus& bus) {
    constexpr uint32_t kPc = 0x03000300u;
    write_mov_bx_lr(bus.iwram_ptr(), kPc - 0x03000000u, 0x11);
    gbarecomp::overlay_request_compile(kPc, false);
    check(wait_healed(kPc, false), "first IWRAM variant did not heal");

    g_cpu.R[0] = 0;
    check(dispatch_as_call(kPc, 0) == 1, "first variant did not dispatch");
    check(g_cpu.R[0] == 0x11u, "first variant did not set r0=0x11");

    // Overwrite with a DIFFERENT function at the same address/extent.
    write_mov_bx_lr(bus.iwram_ptr(), kPc - 0x03000000u, 0x22);

    // The very next dispatch must see the CRC mismatch: it must NOT call the
    // stale native body (which would leave r0 == 0x11, or 0x22 is what we'd
    // wrongly see if it silently used still-mapped-but-wrong code — the only
    // way to tell "did not call stale native" apart from "correctly healed
    // new content already" is to check the return value on THIS exact call:
    // a sound implementation reports a miss (0) here because re-heal is
    // asynchronous, not a same-call hot-swap).
    g_cpu.R[0] = 0xDEADBEEFu;
    const int first_hit_after_overwrite = dispatch_as_call(kPc, 0);
    check(first_hit_after_overwrite == 0,
         "stale native was called after IWRAM content changed underneath it");
    check(g_cpu.R[0] == 0xDEADBEEFu,
         "dispatch mutated r0 even though it reported a miss (stale code ran)");

    // The mismatch must have re-enqueued a heal for the new bytes.
    check(wait_healed(kPc, false), "new IWRAM content did not heal after CRC mismatch");
    g_cpu.R[0] = 0;
    check(dispatch_as_call(kPc, 0) == 1, "new variant did not dispatch");
    check(g_cpu.R[0] == 0x22u,
         "new variant did not set r0=0x22 (got " + std::to_string(g_cpu.R[0]) + ")");
}

// A synthetic caller rewrites a later RAM target and reaches it in the same
// logical invocation. The stale AOT body must be rejected; the bridge must
// fetch and execute the newly written instruction.
void test_write_then_execute_same_invocation(gba::GbaBus& bus) {
    constexpr uint32_t kPc = 0x03000400u;
    constexpr uint32_t kReturnPc = 0x08000100u;
    uint8_t original[8]{};
    put32(original, 0, 0xE1A00000u);  // MOV r0,r0
    put32(original, 4, 0xE12FFF1Eu);  // BX lr
    std::memcpy(bus.iwram_ptr() + (kPc - 0x03000000u), original,
                sizeof(original));
    const uint32_t expected_crc = gba::crc32(original, sizeof(original));
    check(runtime_ram_code_guard(kPc, kPc + 8u, expected_crc) == 1,
          "unchanged synthetic RAM target failed its AOT guard");

    bus.write32(kPc, 0xE3A0002Au);  // MOV r0,#42
    check(runtime_ram_code_guard(kPc, kPc + 8u, expected_crc) == 0,
          "rewritten synthetic RAM target passed its stale AOT guard");

    g_cpu.R[0] = 0;
    g_cpu.R[14] = kReturnPc;
    runtime_call_push_return(kReturnPc);
    runtime_mutable_ram_code_miss(kPc, 0);
    check(g_cpu.R[0] == 42u,
          "same-invocation RAM rewrite was not visible to bridge execution");
    check(g_cpu.R[15] == kReturnPc,
          "same-invocation RAM bridge did not return to its caller");
}

// ── Test: the SMC-guard fallback (runtime_mutable_ram_code_miss, called
//    from inside a generated function's own AOT guard when it fails) must
//    consult the Stage-2 healed-overlay cache before paying for a fresh
//    interpreter bridge. Golden Sun's fixed-address AOT dispatch entries
//    guard their own extent against a single ROM-derived CRC; once a RAM
//    slot has settled on self-assembled content that will never match that
//    baked CRC, EVERY later call re-derives the same "not the AOT body"
//    verdict from runtime_ram_code_guard and used to unconditionally pay
//    for a fresh interpreter bridge on every single occurrence — even
//    though the exact same bytes had already healed to native the first
//    time. This proves: (1) the first sighting of a variant still bridges
//    and heals, (2) a later call with UNCHANGED bytes reuses the healed
//    native entry instead of re-interpreting, (3) a genuine content change
//    still bridges + re-heals (the fast path never masks a real miss —
//    overlay_try_dispatch independently re-verifies the live bytes' CRC
//    before it will ever call the cached native fn), and (4) results stay
//    byte-for-byte what the interpreter would have produced throughout. ──
void test_mutable_ram_miss_reuses_healed_variant(gba::GbaBus& bus) {
    constexpr uint32_t kPc = 0x03000500u;
    constexpr uint32_t kReturnPc = 0x08000100u;
    const uint32_t off = kPc - 0x03000000u;

    auto dispatch_guard_miss = [&](uint8_t expect_r0) {
        g_cpu.R[0] = 0xEEu;
        g_cpu.R[14] = kReturnPc;
        runtime_call_push_return(kReturnPc);
        runtime_mutable_ram_code_miss(kPc, 0);
        check(g_cpu.R[0] == expect_r0,
             "runtime_mutable_ram_code_miss produced the wrong result (got 0x" +
             std::to_string(g_cpu.R[0]) + ", want 0x" +
             std::to_string(static_cast<unsigned>(expect_r0)) + ")");
        check(g_cpu.R[15] == kReturnPc,
             "runtime_mutable_ram_code_miss did not return to its caller");
    };

    // Variant A, first sighting at this address: must bridge + heal.
    write_mov_bx_lr(bus.iwram_ptr(), off, 0xA1);
    dispatch_guard_miss(0xA1);
    check(wait_healed(kPc, false), "variant A did not heal via the SMC-guard path");

    // Same bytes, called again with NOTHING rewritten (the steady-state
    // case: the AOT guard keeps failing against its baked CRC because this
    // slot is permanently self-assembled code now, but the content itself
    // hasn't changed since it healed). Must resolve WITHOUT a fresh
    // interpreter bridge.
    const std::uint64_t insns_before = gbarecomp::self_heal_interpreted_insns();
    dispatch_guard_miss(0xA1);
    const std::uint64_t insns_after = gbarecomp::self_heal_interpreted_insns();
    check(insns_after == insns_before,
         "a repeat call with unchanged, already-healed RAM content "
         "re-interpreted instead of reusing the Stage-2 healed overlay "
         "(insns " + std::to_string(insns_before) + " -> " +
         std::to_string(insns_after) + ")");
    // And once more, to rule out a one-shot cache that only survives a
    // single extra call.
    dispatch_guard_miss(0xA1);
    check(gbarecomp::self_heal_interpreted_insns() == insns_after,
         "a second repeat call with unchanged content re-interpreted");

    // A genuine content change (a different self-assembled variant lands
    // at the same address) must still bridge + re-heal — the fast path
    // must never dispatch stale code.
    write_mov_bx_lr(bus.iwram_ptr(), off, 0xB2);
    const std::uint64_t insns_before_new = gbarecomp::self_heal_interpreted_insns();
    dispatch_guard_miss(0xB2);
    check(gbarecomp::self_heal_interpreted_insns() > insns_before_new,
         "a genuinely new RAM variant did not bridge through the interpreter");
    check(wait_healed(kPc, false), "the second variant did not heal");

    // ...and now IT is the steady state: repeat calls with variant B's
    // unchanged bytes must also skip re-interpretation.
    const std::uint64_t insns_before_b_repeat =
        gbarecomp::self_heal_interpreted_insns();
    dispatch_guard_miss(0xB2);
    check(gbarecomp::self_heal_interpreted_insns() == insns_before_b_repeat,
         "a repeat call with unchanged variant-B content re-interpreted");
}

// A three-instruction ARM decrement loop with a backward branch closing an
// idle-simple body:
//   SUBS r0, r0, #1   (idle-simple: writes only a GP register + flags)
//   BNE  <loop head>  (backward branch inside the function -> codegen marks
//                      this back-edge eligible for Stage-2 idle-loop elision
//                      and emits a call to runtime_idle_backedge(), see
//                      arm_codegen.cpp / CodegenCtx::idle_backedge_pcs)
//   BX   lr
void write_idle_backedge_loop(uint8_t* dst, uint32_t off) {
    put32(dst, off + 0, 0xE2500001u);  // SUBS r0, r0, #1
    // BNE back to off+0: imm24 = (target - (branch_pc + 8)) >> 2
    //                          = (off - (off+4+8)) >> 2 = -12 >> 2 = -3.
    put32(dst, off + 4, 0x1AFFFFFDu);  // BNE off+0
    put32(dst, off + 8, 0xE12FFF1Eu);  // BX lr
}

// ── Regression: the Stage-2 overlay shard emitter must declare/wire
//    runtime_idle_backedge exactly like every other runtime_*/bus_*/arm_*
//    helper codegen can call (overlay_abi.h / overlay_runtime_arm.h /
//    overlay_loader.cpp). A function containing a statically-eligible
//    quiescent backward branch used to emit a call to an undeclared
//    runtime_idle_backedge in the generated overlay .c, which failed to
//    compile (gcc: 'runtime_idle_backedge' was not declared in this scope)
//    and permanently stranded that PC on the interpreter bridge. ──────────
void test_idle_backedge_overlay_compiles(gba::GbaBus& bus) {
    constexpr uint32_t kPc = 0x03000600u;
    write_idle_backedge_loop(bus.iwram_ptr(), kPc - 0x03000000u);

    gbarecomp::overlay_request_compile(kPc, /*thumb=*/false);
    check(wait_healed(kPc, false),
         "a function with a statically-eligible idle back-edge (calls "
         "runtime_idle_backedge) did not heal to native -- the overlay "
         "shard likely failed to compile");

    g_cpu.R[0] = 5;
    const int dispatched = dispatch_as_call(kPc, /*thumb=*/0);
    check(dispatched == 1,
         "healed idle-backedge loop was not dispatched natively");
    check(g_cpu.R[0] == 0u,
         "native idle-backedge loop did not count down to 0 (got " +
             std::to_string(g_cpu.R[0]) + ")");
}

// Encodes "BL <target>" at guest address `at_pc` (ARM mode): imm24 =
// (target - (at_pc + 8)) >> 2, per the standard ARM PC+8 pipeline offset.
uint32_t encode_bl(uint32_t at_pc, uint32_t target) {
    const int32_t off =
        (static_cast<int32_t>(target) - static_cast<int32_t>(at_pc + 8)) >> 2;
    return 0xEB000000u | (static_cast<uint32_t>(off) & 0x00FFFFFFu);
}

// ── Regression: the interpreter bridge's call-boundary handoff. A BL to a
//    callee that ALREADY has native code (a healed overlay, here) must be
//    dispatched natively instead of single-stepped through -- the fix this
//    session's storm-diagnosis work landed. Without it, every interpreted
//    instruction inside the callee's body would show up in
//    self_heal_interpreted_insns() too. ────────────────────────────────
void test_bridge_handoff_to_already_healed_callee(gba::GbaBus& bus) {
    constexpr uint32_t kPcA = 0x03000900u;  // callee: healed BEFORE B ever runs
    constexpr uint32_t kPcB = 0x03000940u;  // caller: never healed, always bridges
    const uint32_t offA = kPcA - 0x03000000u;
    const uint32_t offB = kPcB - 0x03000000u;

    // A: "MOV r0, #0x77; BX lr" -- heal it to native up front, like
    // test_iwram_heal_and_dispatch above.
    write_mov_bx_lr(bus.iwram_ptr(), offA, 0x77u);
    gbarecomp::overlay_request_compile(kPcA, /*thumb=*/false);
    check(wait_healed(kPcA, false),
         "callee A did not heal to native ahead of the handoff test");

    // B is a genuinely unknown function, always dispatched via
    // runtime_dispatch_miss directly (Stage-1) below, never healed itself:
    //   MOV r2, lr      -- save LR (BL is about to clobber it)
    //   BL  A
    //   MOV r1, #0x99   -- proves the bridge resumed B's OWN body afterward
    //   MOV lr, r2      -- restore LR
    //   BX  lr          -- return to B's real caller
    uint8_t* dst = bus.iwram_ptr();
    constexpr uint32_t kBlAt = kPcB + 4u;
    put32(dst, offB + 0,  0xE1A0200Eu);           // MOV r2, lr
    put32(dst, offB + 4,  encode_bl(kBlAt, kPcA)); // BL A
    put32(dst, offB + 8,  0xE3A01099u);           // MOV r1, #0x99
    put32(dst, offB + 12, 0xE1A0E002u);           // MOV lr, r2
    put32(dst, offB + 16, 0xE12FFF1Eu);           // BX lr

    constexpr uint32_t kReturnPc = 0x08000200u;
    g_cpu.R[14] = kReturnPc;
    g_cpu.R[0] = 0;
    g_cpu.R[1] = 0;

    const std::uint64_t insns_before = gbarecomp::self_heal_interpreted_insns();
    runtime_dispatch_miss(kPcB);
    const std::uint64_t insns_after = gbarecomp::self_heal_interpreted_insns();

    check(g_cpu.R[15] == kReturnPc,
         "bridged caller B did not return to its own caller (got 0x" +
         std::to_string(g_cpu.R[15]) + ")");
    check(g_cpu.R[0] == 0x77u,
         "callee A's native effect (r0) was not observed through the "
         "bridge's call-boundary handoff (got 0x" +
         std::to_string(g_cpu.R[0]) + ")");
    check(g_cpu.R[1] == 0x99u,
         "the bridge did not resume interpreting B's own body after "
         "handing the call off to native code (got r1=0x" +
         std::to_string(g_cpu.R[1]) + ")");
    // B alone is 5 interpreted instructions (mov/bl/mov/mov/bx). A's own 2
    // instructions must NOT be added -- that is the entire point of the
    // handoff. Before the fix this delta was 7 (the bridge single-stepped
    // through A's body too).
    check(insns_after - insns_before == 5u,
         "the bridge interpreted the callee's body instead of handing off "
         "to its native code (insns " + std::to_string(insns_before) +
         " -> " + std::to_string(insns_after) + ", delta " +
         std::to_string(insns_after - insns_before) + ", want 5)");
    check(runtime_call_stack_depth() == 0u,
         "the call-boundary handoff leaked a call-return frame (depth=" +
             std::to_string(runtime_call_stack_depth()) + ", want 0)");
}

// A top-level RAM miss has no reliable guest return address.  It must still
// yield back to the outer dispatch loop periodically: interpreting an
// unregistered generated image until its first static re-entry made one menu
// action block for hundreds of thousands of instructions.  The slice is only
// a scheduling boundary; resuming at the same PC must preserve guest state.
void test_top_level_ram_bridge_is_resumable(gba::GbaBus& bus) {
    constexpr uint32_t kPc = 0x03001400u;
    const uint32_t off = kPc - 0x03000000u;

    // B . — one instruction per bridge iteration, with no calls or returns.
    put32(bus.iwram_ptr(), off, 0xEAFFFFFEu);
    g_cpu.R[14] = 0x08000280u;  // deliberately unusable as a stop address
    g_cpu.R[15] = kPc;
    g_cpu.cpsr = CPSR_I_BIT | CPSR_F_BIT | 0x1Fu;

    const std::uint64_t before = gbarecomp::self_heal_interpreted_insns();
    check(runtime_bridge_interpret(kPc, false, 0u, 0u) == 1,
          "top-level RAM bridge slice did not return to its caller");
    const std::uint64_t delta =
        gbarecomp::self_heal_interpreted_insns() - before;
    check(delta == 4096u,
          "top-level RAM bridge did not stop at its scheduling slice (" +
              std::to_string(delta) + " instructions, want 4096)");
    check(g_cpu.R[15] == kPc,
          "top-level RAM bridge did not preserve the resumable guest PC");
    check(runtime_call_stack_depth() == 0u,
          "top-level RAM bridge created a call-return frame");
}

// ── Bounded multi-variant healed-code cache regressions ─────────────────
// The motivating case (0x03000820 in the real game) toggles among exactly 3
// resident variants, all of which are cheap to have compiled already; a
// single-slot cache turns every toggle into a full interpreter bridge. These
// tests exercise the fix directly through runtime_mutable_ram_code_miss --
// the SAME call site the generated code's own AOT guard uses (see
// test_mutable_ram_miss_reuses_healed_variant above for the 2-variant case
// this design generalizes).

// Shared helper: dispatch through runtime_mutable_ram_code_miss (as a
// generated AOT guard failure would) and assert the resulting r0. Every
// variant body here is "MOV r0,#imm8; BX lr" -- always the SAME 8-byte
// length, so any variant-to-variant distinction below is necessarily made by
// the CRC32 content check, never a length shortcut.
void dispatch_ram_guard_miss(uint32_t pc, uint8_t expect_r0) {
    constexpr uint32_t kReturnPc = 0x08000100u;
    g_cpu.R[0] = 0xEEu;
    g_cpu.R[14] = kReturnPc;
    runtime_call_push_return(kReturnPc);
    runtime_mutable_ram_code_miss(pc, 0);
    check(g_cpu.R[0] == expect_r0,
         "dispatch produced the wrong result for pc=0x" +
         std::to_string(pc) + " (got 0x" + std::to_string(g_cpu.R[0]) +
         ", want 0x" + std::to_string(static_cast<unsigned>(expect_r0)) + ")");
    check(g_cpu.R[15] == kReturnPc,
         "dispatch did not return to its caller for pc=0x" + std::to_string(pc));
}

// ── Test: alternating between exactly THREE resident variants at one
//    address hits the cache after the first sighting of each (generalizes
//    the existing 2-variant regression to the real Golden Sun shape). ─────
void test_ram_three_variant_alternation_no_reinterpretation(gba::GbaBus& bus) {
    constexpr uint32_t kPc = 0x03000700u;
    const uint32_t off = kPc - 0x03000000u;
    auto write_variant = [&](uint8_t imm8) {
        write_mov_bx_lr(bus.iwram_ptr(), off, imm8);
    };

    // First sighting of A, B, C: each must bridge through the interpreter
    // once and heal to native.
    for (uint8_t imm : {0xA0u, 0xB0u, 0xC0u}) {
        write_variant(static_cast<uint8_t>(imm));
        dispatch_ram_guard_miss(kPc, static_cast<uint8_t>(imm));
        check(wait_healed(kPc, false),
             "variant 0x" + std::to_string(imm) +
             " did not heal on its first sighting");
    }

    // Now cycle A, B, C, A, B, C, A, B, C -- every one of these is a REPEAT
    // sighting of an already-resident variant, so none of them may grow the
    // interpreted-instruction counter.
    const std::uint64_t insns_before = gbarecomp::self_heal_interpreted_insns();
    for (int rep = 0; rep < 3; ++rep) {
        for (uint8_t imm : {0xA0u, 0xB0u, 0xC0u}) {
            write_variant(static_cast<uint8_t>(imm));
            dispatch_ram_guard_miss(kPc, static_cast<uint8_t>(imm));
        }
    }
    check(gbarecomp::self_heal_interpreted_insns() == insns_before,
         "re-visiting already-healed variants among a 3-way alternation "
         "re-interpreted instead of hitting the multi-variant cache (insns "
         + std::to_string(insns_before) + " -> " +
         std::to_string(gbarecomp::self_heal_interpreted_insns()) + ")");
}

// ── Test: a genuinely new variant (never seen at this address before) must
//    still bridge through the interpreter and heal -- the multi-variant
//    cache must never mask a real miss. ───────────────────────────────────
void test_ram_genuinely_new_variant_still_bridges(gba::GbaBus& bus) {
    constexpr uint32_t kPc = 0x03000710u;
    const uint32_t off = kPc - 0x03000000u;

    write_mov_bx_lr(bus.iwram_ptr(), off, 0xD0u);
    dispatch_ram_guard_miss(kPc, 0xD0u);
    check(wait_healed(kPc, false), "first variant did not heal");

    // A second, DIFFERENT, never-before-seen variant: must bridge (grow the
    // interpreted-insn counter), not silently resolve via the cache.
    const std::uint64_t insns_before = gbarecomp::self_heal_interpreted_insns();
    write_mov_bx_lr(bus.iwram_ptr(), off, 0xD1u);
    dispatch_ram_guard_miss(kPc, 0xD1u);
    check(gbarecomp::self_heal_interpreted_insns() > insns_before,
         "a genuinely new RAM variant did not bridge through the "
         "interpreter (the multi-variant cache masked a real miss)");
    check(wait_healed(kPc, false), "the new variant did not heal");
}

// ── Test: exceeding the 10-variant-per-key cap evicts the LEAST-RECENTLY-
//    USED resident variant, never a more-recently-seen one, and every
//    dispatch keeps producing the correct (interpreter-equivalent) result
//    whether it hits the cache or re-bridges after eviction. ──────────────
void test_ram_cap_eviction_lru(gba::GbaBus& bus) {
    constexpr uint32_t kPc = 0x03000720u;
    const uint32_t off = kPc - 0x03000000u;
    auto write_variant = [&](int idx) {
        write_mov_bx_lr(bus.iwram_ptr(), off, static_cast<uint8_t>(0x40 + idx));
    };
    auto expect_r0 = [](int idx) {
        return static_cast<uint8_t>(0x40 + idx);
    };

    // Sight 10 distinct variants (0..9) in order. All 10 must remain resident
    // under the 10-entry bound; this is the working-set regression that
    // motivated this larger generic cap.
    for (int i = 0; i < 10; ++i) {
        write_variant(i);
        dispatch_ram_guard_miss(kPc, expect_r0(i));
        check(wait_healed(kPc, false),
             "variant " + std::to_string(i) + " did not heal");
    }

    const std::uint64_t ten_variant_insns =
        gbarecomp::self_heal_interpreted_insns();
    for (int i = 0; i < 10; ++i) {
        write_variant(i);
        dispatch_ram_guard_miss(kPc, expect_r0(i));
    }
    check(gbarecomp::self_heal_interpreted_insns() == ten_variant_insns,
          "a 10-variant RAM working set churned before reaching the cache cap");

    // Touch variants 2..9 once more so variants 0 and 1 are the two least
    // recently used entries before overflow. Push past the cap: sightings
    // 11 and 12 (0-indexed 10 and 11) must
    // evict variants 0 and 1 respectively -- the two least recently touched.
    for (int i = 2; i < 10; ++i) {
        write_variant(i);
        dispatch_ram_guard_miss(kPc, expect_r0(i));
    }
    for (int i = 10; i < 12; ++i) {
        write_variant(i);
        dispatch_ram_guard_miss(kPc, expect_r0(i));
        check(wait_healed(kPc, false),
             "variant " + std::to_string(i) + " did not heal");
    }

    // Variants 0 and 1 were evicted: revisiting them must re-bridge (a
    // genuine cache miss), but must still produce the CORRECT result.
    for (int evicted : {0, 1}) {
        const std::uint64_t insns_before = gbarecomp::self_heal_interpreted_insns();
        write_variant(evicted);
        dispatch_ram_guard_miss(kPc, expect_r0(evicted));
        check(gbarecomp::self_heal_interpreted_insns() > insns_before,
             "evicted variant " + std::to_string(evicted) +
             " resolved without re-bridging -- eviction did not actually "
             "drop it");
        check(wait_healed(kPc, false),
             "evicted variant " + std::to_string(evicted) +
             " did not re-heal after being re-sighted");
    }

    // Re-healing the two evictees above is itself two more insertions, which
    // (per the same LRU policy) evicts two MORE alternates in turn -- eviction
    // cascades exactly like an ordinary LRU cache under sustained pressure.
    // What must NOT happen is the most recently touched resident (variant 9,
    // never evicted at any point above) losing its cached entry. Re-check it.
    {
        const std::uint64_t insns_before = gbarecomp::self_heal_interpreted_insns();
        write_variant(9);
        dispatch_ram_guard_miss(kPc, expect_r0(9));
        check(gbarecomp::self_heal_interpreted_insns() == insns_before,
             "variant 9 (never evicted) re-interpreted instead of hitting "
             "the cache");
    }
}

// ── Test 3: ROM-backed healing is unaffected by the RAM-snapshot path. ───
void test_rom_heal_unaffected(gba::GbaBus& bus, std::vector<uint8_t>& rom) {
    constexpr uint32_t kOff = 0x400u;
    constexpr uint32_t kPc = 0x08000000u + kOff;
    write_mov_bx_lr(rom.data(), kOff, 0x33);
    bus.set_rom(rom.data(), rom.size());

    gbarecomp::overlay_request_compile(kPc, false);
    check(wait_healed(kPc, false), "ROM-backed function did not heal");

    g_cpu.R[0] = 0;
    check(dispatch_as_call(kPc, 0) == 1, "ROM-backed entry did not dispatch");
    check(g_cpu.R[0] == 0x33u, "ROM-backed function did not set r0=0x33");

    // Rewriting the ROM buffer (the cart image is immutable during a real
    // run, but this proves the ROM path takes the zero-overhead route and
    // does NOT re-validate on every call the way RAM entries do).
    write_mov_bx_lr(rom.data(), kOff, 0x99);
    g_cpu.R[0] = 0;
    check(dispatch_as_call(kPc, 0) == 1,
         "ROM-backed dispatch stopped working after the (immutable-in-"
         "practice) backing buffer changed — it should never re-check");
    check(g_cpu.R[0] == 0x33u,
         "ROM-backed dispatch re-validated content — it must stay "
         "zero-overhead and keep calling the originally compiled body");
}

// ── Test 4: an address in neither BIOS, ROM, IWRAM, nor EWRAM must fail
//    region resolution cleanly (no crash, no heal, marked so it isn't
//    retried). region_bytes() itself is file-local, so this exercises it
//    through its only caller, overlay_request_compile. ────────────────────
void test_unmapped_region_never_heals() {
    constexpr uint32_t kPc = 0x05000100u;  // PAL RAM: not BIOS/ROM/IWRAM/EWRAM
    gbarecomp::overlay_request_compile(kPc, false);
    // No worker thread will ever produce an entry for this key; a short
    // settle is enough since nothing was enqueued.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    gbarecomp::overlay_drain_ready();
    check(!gbarecomp::overlay_query(kPc, false, nullptr),
         "an unmapped-region PC healed anyway");
    check(dispatch_as_call(kPc, 0) == 0,
         "dispatch reported success for an unmapped-region PC");

    // Calling it again must stay a clean no-op (region_bytes' failure was
    // remembered — this only proves it doesn't crash/loop on repeat).
    gbarecomp::overlay_request_compile(kPc, false);
    check(!gbarecomp::overlay_query(kPc, false, nullptr),
         "retrying an unmapped-region PC healed on the second attempt");
}

// ── Background warm-load regressions ─────────────────────────────────────
// These simulate a second process launch against a cache directory a PRIOR
// session already populated, by calling overlay_loader_shutdown() +
// overlay_loader_reset_state_for_test() + overlay_loader_init() again in the
// SAME process with the SAME cache_root/image id: the in-memory heal state
// resets exactly like a fresh process would start, but the on-disk cache is
// untouched — precisely what the background warm-loader now reads at init.

// Poll overlay_drain_ready for up to `timeout_ms`, letting the background
// warm-load thread (and any worker-thread compiles it triggers) settle.
void settle_warm_load(int timeout_ms = 5000) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        gbarecomp::overlay_drain_ready();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    gbarecomp::overlay_drain_ready();
}

// A heal can finish while an outer unknown subtree is already being bridged.
// Keep the result intentionally undrained, then prove the bridge publishes it
// at the BL boundary instead of interpreting the now-ready callee once more.
void test_bridge_handoff_drains_ready_callee(gba::GbaBus& bus) {
    constexpr uint32_t kPcA = 0x03000A00u;
    constexpr uint32_t kPcB = 0x03000A40u;
    const uint32_t offA = kPcA - 0x03000000u;
    const uint32_t offB = kPcB - 0x03000000u;

    write_mov_bx_lr(bus.iwram_ptr(), offA, 0x66u);
    check(gbarecomp::overlay_request_compile(kPcA, false) ==
              gbarecomp::OverlayRequestOutcome::Queued,
          "ready-at-BL callee was not queued");
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(10);
    while (!gbarecomp::overlay_ready_contains_for_test(kPcA, false) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(gbarecomp::overlay_ready_contains_for_test(kPcA, false),
          "ready-at-BL callee compile did not finish");
    check(!gbarecomp::overlay_query(kPcA, false, nullptr),
          "ready-at-BL setup accidentally installed the callee early");

    uint8_t* dst = bus.iwram_ptr();
    constexpr uint32_t kBlAt = kPcB + 4u;
    put32(dst, offB + 0,  0xE1A0200Eu);            // MOV r2, lr
    put32(dst, offB + 4,  encode_bl(kBlAt, kPcA)); // BL A
    put32(dst, offB + 8,  0xE3A01055u);            // MOV r1, #0x55
    put32(dst, offB + 12, 0xE1A0E002u);            // MOV lr, r2
    put32(dst, offB + 16, 0xE12FFF1Eu);            // BX lr

    constexpr uint32_t kReturnPc = 0x08000240u;
    g_cpu.R[14] = kReturnPc;
    g_cpu.R[0] = 0;
    g_cpu.R[1] = 0;
    const std::uint64_t before = gbarecomp::self_heal_interpreted_insns();
    check(runtime_bridge_interpret(kPcB, false, kReturnPc, 1000u) == 1,
          "ready-at-BL caller bridge did not return");
    const std::uint64_t after = gbarecomp::self_heal_interpreted_insns();

    check(g_cpu.R[0] == 0x66u && g_cpu.R[1] == 0x55u,
          "ready-at-BL native handoff produced wrong register state");
    check(after - before == 5u,
          "ready-at-BL bridge interpreted the completed callee (delta=" +
              std::to_string(after - before) + ", want 5)");
    check(gbarecomp::overlay_query(kPcA, false, nullptr),
          "ready-at-BL result was not installed at the call boundary");
}

fs::path find_cache_sidecar(const std::string& cache_root, uint32_t pc,
                            const char* extension) {
    char prefix[10];
    std::snprintf(prefix, sizeof(prefix), "%08X_", pc);
    std::error_code ec;
    for (const auto& de : fs::recursive_directory_iterator(cache_root, ec)) {
        if (ec) break;
        if (de.is_regular_file() && de.path().extension() == extension &&
            de.path().filename().string().compare(0, 9, prefix) == 0)
            return de.path();
    }
    return {};
}

// A fresh session can reuse an ABI5 RAM PIC DLL at another address using only
// exact persisted bytes. Bad metadata, another ISA, or changed bytes refuse.
void test_disk_pic_cross_address_reuse(gba::GbaBus& bus,
                                       const std::string& cache_root) {
    constexpr uint32_t kA = 0x03001200u, kB = 0x03001240u;
    constexpr uint32_t kWrongMode = 0x03001280u, kMutated = 0x030012C0u;
    write_three_insn(bus.iwram_ptr(), kA - 0x03000000u, 0xD1u, 0x71u);
    write_three_insn(bus.iwram_ptr(), kB - 0x03000000u, 0xD1u, 0x71u);
    gbarecomp::overlay_request_compile(kA, false);
    check(wait_healed(kA, false), "disk PIC source did not heal");
    const fs::path source_pic = find_cache_sidecar(cache_root, kA, ".pic");
    check(!source_pic.empty(), "disk PIC exact-byte metadata was not written");
    if (!source_pic.empty()) {
        const uint32_t crc = gba::crc32(
            bus.iwram_ptr() + (kA - 0x03000000u), 12u);
        std::shared_ptr<const std::vector<uint8_t>> loaded;
        check(gbarecomp::overlay_read_relocatable_metadata(
                  source_pic.string(), kA, false, crc, kA + 12u, &loaded),
              "valid PIC metadata did not read");
        const fs::path trailing = source_pic.string() + ".trailing";
        const fs::path truncated = source_pic.string() + ".truncated";
        std::error_code copy_ec;
        fs::copy_file(source_pic, trailing,
                      fs::copy_options::overwrite_existing, copy_ec);
        std::FILE* f = std::fopen(trailing.string().c_str(), "ab");
        if (f) { std::fputc(0x7F, f); std::fclose(f); }
        check(!gbarecomp::overlay_read_relocatable_metadata(
                   trailing.string(), kA, false, crc, kA + 12u, &loaded),
              "PIC metadata with trailing bytes was accepted");
        f = std::fopen(truncated.string().c_str(), "wb");
        if (f) { std::fwrite("bad", 1, 3, f); std::fclose(f); }
        check(!gbarecomp::overlay_read_relocatable_metadata(
                   truncated.string(), kA, false, crc, kA + 12u, &loaded),
              "truncated PIC metadata was accepted");
        check(!gbarecomp::overlay_read_relocatable_metadata(
                   source_pic.string(), kA, false, crc,
                   kA + 16u * 1024u + 4u, &loaded),
              "oversized PIC metadata extent was accepted");
        fs::remove(trailing, copy_ec);
        fs::remove(truncated, copy_ec);
    }
    gbarecomp::overlay_loader_shutdown();
    gbarecomp::overlay_loader_reset_state_for_test();
    gbarecomp::overlay_loader_init(cache_root, "ram_heal_test_image", nullptr);
    check(wait_healed(kA, false, 5000), "disk PIC source did not warm-load");

    uint64_t h0 = 0, h1 = 0, inflight = 99;
    gbarecomp::overlay_relocatable_alias_counters(&h0, nullptr);
    gbarecomp::overlay_request_compile(kB, false);
    gbarecomp::overlay_relocatable_alias_counters(&h1, nullptr);
    gbarecomp::overlay_counters(nullptr, nullptr, &inflight, nullptr);
    check(h1 == h0 + 1 && inflight == 0,
          "restart exact bytes did not reuse disk-backed PIC");
    g_cpu.R[0] = g_cpu.R[2] = 0;
    check(dispatch_as_call(kB, 0) == 1 && g_cpu.R[0] == 0xD1u &&
              g_cpu.R[2] == 0x71u,
          "disk-backed PIC did not execute destination-relative alias");

    write_three_insn(bus.iwram_ptr(), kWrongMode - 0x03000000u, 0xD1u, 0x71u);
    gbarecomp::overlay_request_compile(kWrongMode, true);
    check(!gbarecomp::overlay_query(kWrongMode, true, nullptr),
          "disk PIC reused artifact in wrong mode");
    check(wait_healed(kWrongMode, true), "wrong-mode fallback compile failed");
    write_three_insn(bus.iwram_ptr(), kMutated - 0x03000000u, 0xD1u, 0x70u);
    gbarecomp::overlay_request_compile(kMutated, false);
    check(!gbarecomp::overlay_query(kMutated, false, nullptr),
          "disk PIC reused mutated bytes");
    check(wait_healed(kMutated, false), "mutated fallback compile failed");

    // Produce another unique artifact, then corrupt its exact-byte sidecar.
    constexpr uint32_t kCorruptA = 0x03001300u, kCorruptB = 0x03001340u;
    write_three_insn(bus.iwram_ptr(), kCorruptA - 0x03000000u, 0xE2u, 0x62u);
    write_three_insn(bus.iwram_ptr(), kCorruptB - 0x03000000u, 0xE2u, 0x62u);
    gbarecomp::overlay_request_compile(kCorruptA, false);
    check(wait_healed(kCorruptA, false), "corrupt-metadata source did not heal");
    gbarecomp::overlay_loader_shutdown();
    const fs::path pic = find_cache_sidecar(cache_root, kCorruptA, ".pic");
    check(!pic.empty(), "RAM PIC metadata sidecar was not written");
    if (!pic.empty()) {
        std::FILE* f = std::fopen(pic.string().c_str(), "wb");
        check(f != nullptr, "could not corrupt PIC metadata fixture");
        if (f) { std::fwrite("bad", 1, 3, f); std::fclose(f); }
    }
    gbarecomp::overlay_loader_reset_state_for_test();
    gbarecomp::overlay_loader_init(cache_root, "ram_heal_test_image", nullptr);
    check(wait_healed(kCorruptA, false, 5000),
          "address-keyed warm load broke on corrupt PIC metadata");
    gbarecomp::overlay_request_compile(kCorruptB, false);
    check(!gbarecomp::overlay_query(kCorruptB, false, nullptr),
          "corrupt PIC metadata enabled cross-address reuse");
    check(wait_healed(kCorruptB, false), "corrupt metadata fallback compile failed");
}

// warm_scan_ram's MRU cap orders candidates by real on-disk last-write-time,
// which is exactly right for production (the file the OS actually wrote most
// recently IS the most-recently-produced variant). A test that wants to
// assert a SPECIFIC MRU ordering across several real g++ invocations cannot
// rely on wall-clock proximity between compiles to preserve that ordering:
// gcc's own completion latency jitters by tens to hundreds of milliseconds
// under ordinary system noise (antivirus, scheduler contention, disk cache
// flushes), so two variants compiled back-to-back can occasionally finish
// (and therefore have their .dll renamed into place) out of logical order --
// nothing about the warm-loader's own ordering is wrong when that happens,
// but a test built on "variant N always finishes after variant N-1" is not
// sound. This makes the on-disk ordering deterministic for the test WITHOUT
// touching production's MRU policy: after each variant is confirmed healed
// (so its .dll has already been renamed into place), find that ONE new .dll
// file for the key (matched by filename prefix, excluded from `seen` so each
// call only ever finds the file the last compile just produced) and stamp
// its last-write-time to an explicit, strictly increasing value. Later reads
// of that same timestamp (by warm_scan_ram, or by a human via
// PowerShell/`stat`) see an unambiguous order that matches the test's
// intended variant sequence exactly, regardless of how fast or slow any
// individual g++ invocation happened to run.
#ifdef _WIN32
// Force a specific, unambiguous last-write-time on `path`. Deliberately NOT
// std::filesystem::last_write_time: every DLL this test compiles stays
// LoadLibrary'd for the rest of the process (overlay_loader.cpp never
// FreeLibrary's a healed module), and MinGW's libstdc++ opens the file
// without FILE_SHARE_WRITE/FILE_SHARE_DELETE, so restamping an
// already-loaded .dll fails with ERROR_SHARING_VIOLATION ("Permission
// denied") -- reliably, on every run, not intermittently. CreateFileW with
// explicit sharing flags plus FILE_WRITE_ATTRIBUTES opens the SAME handle
// kind LoadLibrary itself tolerates.
void win32_set_last_write_time(const fs::path& path,
                               ULONGLONG hundred_ns_since_1601) {
    HANDLE h = CreateFileW(
        path.wstring().c_str(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(h != INVALID_HANDLE_VALUE,
         "CreateFileW failed for " + path.string());
    if (h == INVALID_HANDLE_VALUE) return;
    ULARGE_INTEGER uli;
    uli.QuadPart = hundred_ns_since_1601;
    FILETIME ft;
    ft.dwLowDateTime = uli.LowPart;
    ft.dwHighDateTime = uli.HighPart;
    check(SetFileTime(h, nullptr, nullptr, &ft) != 0,
         "SetFileTime failed for " + path.string());
    CloseHandle(h);
}
#endif

// warm_scan_ram's MRU cap orders candidates by real on-disk last-write-time,
// which is exactly right for production (the file the OS actually wrote most
// recently IS the most-recently-produced variant). A test that wants to
// assert a SPECIFIC MRU ordering across several real g++ invocations cannot
// rely on wall-clock proximity between compiles to preserve that ordering:
// gcc's own completion latency jitters by tens to hundreds of milliseconds
// under ordinary system noise (antivirus, scheduler contention, disk cache
// flushes), so two variants compiled back-to-back can occasionally finish
// (and therefore have their .dll renamed into place) out of logical order --
// nothing about the warm-loader's own ordering is wrong when that happens,
// but a test built on "variant N always finishes after variant N-1" is not
// sound. This makes the on-disk ordering deterministic for the test WITHOUT
// touching production's MRU policy: after each variant is confirmed healed
// (so its .dll has already been renamed into place), find that ONE new .dll
// file for the key (matched by filename prefix, excluded from `seen` so each
// call only ever finds the file the last compile just produced) and stamp
// its last-write-time to an explicit, strictly increasing value (one hour
// per step -- far past any real clock/filesystem resolution concern). Later
// reads of that timestamp (by warm_scan_ram) see an unambiguous order that
// matches the test's intended variant sequence exactly, regardless of how
// fast or slow any individual g++ invocation happened to run.
void stamp_newest_cache_dll(const std::string& cache_root, uint32_t pc,
                            std::unordered_set<std::string>& seen,
                            int step) {
    char prefix[10];
    std::snprintf(prefix, sizeof(prefix), "%08X_", pc);
    std::error_code ec;
    for (const auto& de : fs::recursive_directory_iterator(cache_root, ec)) {
        if (ec) break;
        if (!de.is_regular_file()) continue;
        const std::string name = de.path().filename().string();
        if (name.compare(0, 9, prefix) != 0) continue;
        if (de.path().extension() != ".dll") continue;
        const std::string key = de.path().string();
        if (!seen.insert(key).second) continue;  // already stamped earlier
#ifdef _WIN32
        FILETIME now_ft;
        GetSystemTimeAsFileTime(&now_ft);
        ULARGE_INTEGER now_uli;
        now_uli.LowPart = now_ft.dwLowDateTime;
        now_uli.HighPart = now_ft.dwHighDateTime;
        // 1 hour = 3600 * 10^7 (100-ns units).
        const ULONGLONG stamp =
            now_uli.QuadPart + static_cast<ULONGLONG>(step) * 36000000000ULL;
        win32_set_last_write_time(de.path(), stamp);
#else
        std::error_code sec;
        fs::last_write_time(
            de.path(),
            fs::file_time_type::clock::now() + std::chrono::hours(step), sec);
        check(!sec, "failed to stamp deterministic mtime on " + key +
                    " (" + sec.message() + ")");
#endif
    }
}

// ── Test: multiple resident variants for one key are warm-loaded from disk
//    on a fresh session, all without a single dispatch having happened —
//    the multi-variant cache starts warm instead of only being populated by
//    live misses. ─────────────────────────────────────────────────────────
void test_warm_load_multivariant_after_restart(gba::GbaBus& bus,
                                               const std::string& cache_root) {
    constexpr uint32_t kPc = 0x03000800u;
    const uint32_t off = kPc - 0x03000000u;

    // Session 1: heal 3 distinct variants live so 3 DLL+.c pairs land on disk.
    for (uint8_t imm : {0x60u, 0x61u, 0x62u}) {
        write_three_insn(bus.iwram_ptr(), off, imm, 0xC7u);
        dispatch_ram_guard_miss(kPc, imm);
        check(wait_healed(kPc, false),
             "warm-load setup: variant 0x" + std::to_string(imm) + " did not heal");
    }
    gbarecomp::overlay_loader_shutdown();

    // Session 2: same cache_root/image — simulate a fresh process launch.
    gbarecomp::overlay_loader_reset_state_for_test();
    gbarecomp::overlay_loader_init(cache_root, "ram_heal_test_image", nullptr);
    check(gbarecomp::overlay_enabled(), "self-heal did not re-activate for session 2");
    settle_warm_load();

    // All 3 variants must be resident WITHOUT re-bridging: dispatch each and
    // confirm the interpreted-instruction counter does not move.
    for (uint8_t imm : {0x60u, 0x61u, 0x62u}) {
        write_three_insn(bus.iwram_ptr(), off, imm, 0xC7u);
        const std::uint64_t before = gbarecomp::self_heal_interpreted_insns();
        dispatch_ram_guard_miss(kPc, imm);
        check(gbarecomp::self_heal_interpreted_insns() == before,
             "warm-loaded variant 0x" + std::to_string(imm) +
             " re-interpreted instead of hitting the multi-variant warm cache");
    }
}

// ── Test: more cache files than kMaxVariantsPerKey exist on disk for one
//    key — warm-load must cap at 10 and prefer the most-recently-modified
//    files, not an arbitrary subset. ───────────────────────────────────────
void test_warm_load_cap_prefers_most_recent(gba::GbaBus& bus,
                                            const std::string& cache_root) {
    constexpr uint32_t kPc = 0x03000810u;
    const uint32_t off = kPc - 0x03000000u;

    // Session 1: heal 12 distinct variants live, sequentially. The runtime's
    // own LRU cap evicts the 2 oldest from IN-MEMORY residency by the end,
    // but every compiled DLL/.c pair stays on disk (heal_slot_promote never
    // deletes a cache file on eviction). Each variant's on-disk mtime is
    // forced to an explicit, strictly increasing value right after it heals
    // (see stamp_newest_cache_dll) so the warm-load MRU ordering this test
    // asserts on is deterministic regardless of real g++ completion jitter.
    std::unordered_set<std::string> stamped;
    for (int i = 0; i < 12; ++i) {
        const uint8_t imm = static_cast<uint8_t>(0x70 + i);
        write_three_insn(bus.iwram_ptr(), off, imm, 0xC7u);
        dispatch_ram_guard_miss(kPc, imm);
        check(wait_healed(kPc, false),
             "cap-test setup: variant " + std::to_string(i) + " did not heal");
        stamp_newest_cache_dll(cache_root, kPc, stamped, i);
    }
    gbarecomp::overlay_loader_shutdown();

    gbarecomp::overlay_loader_reset_state_for_test();
    gbarecomp::overlay_loader_init(cache_root, "ram_heal_test_image", nullptr);
    check(gbarecomp::overlay_enabled(),
         "self-heal did not re-activate for the cap-test session");
    settle_warm_load();

    // The 10 MOST RECENTLY modified files on disk (variants 2..11) must be
    // warm-loaded without re-bridging.
    for (int i = 2; i < 12; ++i) {
        const uint8_t imm = static_cast<uint8_t>(0x70 + i);
        write_three_insn(bus.iwram_ptr(), off, imm, 0xC7u);
        const std::uint64_t before = gbarecomp::self_heal_interpreted_insns();
        dispatch_ram_guard_miss(kPc, imm);
        check(gbarecomp::self_heal_interpreted_insns() == before,
             "warm-load cap test: recent variant " + std::to_string(i) +
             " re-interpreted instead of hitting the warm cache");
    }

    // The 2 OLDEST files on disk (variants 0, 1) must NOT have been
    // warm-loaded — the cap must have preferred the more recent 10.
    for (int i = 0; i < 2; ++i) {
        const uint8_t imm = static_cast<uint8_t>(0x70 + i);
        write_three_insn(bus.iwram_ptr(), off, imm, 0xC7u);
        const std::uint64_t before = gbarecomp::self_heal_interpreted_insns();
        dispatch_ram_guard_miss(kPc, imm);
        check(gbarecomp::self_heal_interpreted_insns() > before,
             "warm-load cap test: oldest variant " + std::to_string(i) +
             " was warm-loaded even though the cap should have preferred "
             "the 10 most-recently-modified files on disk");
        check(wait_healed(kPc, false),
             "oldest variant " + std::to_string(i) + " failed to re-heal");
    }
}

// ── Test: a warm-loaded entry whose LIVE bytes no longer match must NOT be
//    entered. Proves CRC verification stays authoritative at dispatch time
//    even for entries installed from filename/sidecar metadata rather than
//    a live-byte recompute. ─────────────────────────────────────────────────
void test_warm_load_never_enters_on_live_mismatch(gba::GbaBus& bus,
                                                   const std::string& cache_root) {
    constexpr uint32_t kPc = 0x03000820u;
    const uint32_t off = kPc - 0x03000000u;

    write_mov_bx_lr(bus.iwram_ptr(), off, 0x80u);
    dispatch_ram_guard_miss(kPc, 0x80u);
    check(wait_healed(kPc, false), "mismatch-test setup variant did not heal");
    gbarecomp::overlay_loader_shutdown();
    gbarecomp::overlay_loader_reset_state_for_test();

    // Before the new session even inits, live RAM changes to content that
    // was NEVER cached for this pc — realistic: a fresh process does not, in
    // general, find IWRAM holding whatever a past session left there.
    write_mov_bx_lr(bus.iwram_ptr(), off, 0x81u);

    gbarecomp::overlay_loader_init(cache_root, "ram_heal_test_image", nullptr);
    check(gbarecomp::overlay_enabled(),
         "self-heal did not re-activate for the mismatch-test session");

    // Let the warm-load install the STRUCTURAL entry from disk (RAM warm-load
    // does not check live bytes at install time — see warm_scan_ram).
    uint64_t ram_healed = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
    while (std::chrono::steady_clock::now() < deadline) {
        gbarecomp::overlay_drain_ready();
        gbarecomp::overlay_counters(nullptr, nullptr, nullptr, nullptr, &ram_healed);
        if (ram_healed > 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    check(ram_healed > 0,
         "warm-load never installed the structural entry (test setup is "
         "broken — not what this test is checking)");

    // overlay_query performs the SAME live-byte verification dispatch does:
    // a structurally-present, warm-loaded entry must NOT report healed
    // against live bytes it does not match.
    check(!gbarecomp::overlay_query(kPc, false, nullptr),
         "overlay_query reported a warm-loaded entry as healed even though "
         "its content does not match the current live RAM bytes");

    // Dispatch must not enter the stale native body: it must report a miss
    // and re-bridge through the interpreter (growing the interpreted-insn
    // counter) rather than silently running cached native code compiled
    // from different bytes.
    const std::uint64_t insns_before = gbarecomp::self_heal_interpreted_insns();
    dispatch_ram_guard_miss(kPc, 0x81u);
    check(gbarecomp::self_heal_interpreted_insns() > insns_before,
         "warm-loaded entry was entered on a live-byte mismatch instead of "
         "re-bridging through the interpreter — CRC verification was "
         "bypassed for a warm-loaded (filename/sidecar-sourced) entry");
}

// ── Test: the background warm-load thread and an immediate organic heal
//    race for the SAME key/content. Neither corrupts nor duplicates state;
//    dispatch ends up stable and correct regardless of which one wins. ─────
void test_warm_load_races_organic_heal_no_corruption(gba::GbaBus& bus,
                                                      const std::string& cache_root) {
    constexpr uint32_t kPc = 0x03000830u;
    const uint32_t off = kPc - 0x03000000u;

    write_mov_bx_lr(bus.iwram_ptr(), off, 0x90u);
    dispatch_ram_guard_miss(kPc, 0x90u);
    check(wait_healed(kPc, false), "race-test setup variant did not heal");
    gbarecomp::overlay_loader_shutdown();
    gbarecomp::overlay_loader_reset_state_for_test();

    // Live bytes still hold the SAME content as the cached DLL this time —
    // unlike the mismatch test above — so both the background warm-load
    // thread and an immediate organic dispatch race to install this key.
    gbarecomp::overlay_loader_init(cache_root, "ram_heal_test_image", nullptr);
    check(gbarecomp::overlay_enabled(),
         "self-heal did not re-activate for the race-test session");

    // Fire the organic path immediately, without waiting for the background
    // warm-load thread at all.
    dispatch_ram_guard_miss(kPc, 0x90u);
    check(wait_healed(kPc, false), "organic re-heal did not complete during the race");

    settle_warm_load(3000);  // let any remaining warm-load activity finish

    // Whichever path won, dispatch must now be stable and correct across
    // repeated calls — no corruption, no flapping, no re-interpretation.
    for (int i = 0; i < 5; ++i) {
        const std::uint64_t before = gbarecomp::self_heal_interpreted_insns();
        dispatch_ram_guard_miss(kPc, 0x90u);
        check(gbarecomp::self_heal_interpreted_insns() == before,
             "race between warm-load and an organic heal for the same "
             "content left dispatch unstable (re-interpreted on repeat call "
             + std::to_string(i) + ")");
    }
}

// Clean exit must not serialize every cold miss accumulated in the work
// queue.  The worker may finish its one active compile, but queued requests
// are irrelevant after the guest has stopped and must be discarded safely.
void test_shutdown_discards_queued_work(gba::GbaBus& bus) {
    constexpr uint32_t kBase = 0x03002000u;
    constexpr unsigned kRequests = 32;
    for (unsigned i = 0; i < kRequests; ++i) {
        const uint32_t pc = kBase + i * 8u;
        write_mov_bx_lr(bus.iwram_ptr(), pc - 0x03000000u,
                        static_cast<uint8_t>(0x20u + i));
        gbarecomp::overlay_request_compile(pc, false);
    }
    gbarecomp::overlay_loader_shutdown();
    check(gbarecomp::overlay_shutdown_discarded_work_for_test() > 0,
          "shutdown waited for or failed to discard queued compile work");
}

}  // namespace

int main() {
    test_hot_queue_priority_and_fairness();
#ifdef _WIN32
    _putenv_s("GBARECOMP_SELFHEAL_RAM", "1");
#else
    setenv("GBARECOMP_SELFHEAL_RAM", "1", 1);
#endif
    const std::string cache_root =
        (fs::temp_directory_path() / "gbarecomp_ram_heal_test_cache").string();
    std::error_code ec;
    fs::remove_all(cache_root, ec);  // start clean every run
    const std::string frame_events =
        (fs::path(cache_root) / "frame_events.csv").string();
#ifdef _WIN32
    _putenv_s("GBARECOMP_FRAME_EVENTS", frame_events.c_str());
#else
    setenv("GBARECOMP_FRAME_EVENTS", frame_events.c_str(), 1);
#endif

    gba::GbaBus bus;
    gbarecomp::set_active_bus(&bus);

    std::vector<uint8_t> rom(0x1000, 0);
    bus.set_rom(rom.data(), rom.size());

    gbarecomp::overlay_loader_init(cache_root, "ram_heal_test_image", nullptr);
    if (!gbarecomp::overlay_enabled()) {
        std::printf(
            "ram_heal_test: SKIPPED — self-heal did not activate "
            "(GBARECOMP_STRICT_STATIC or GBARECOMP_SELFHEAL_RECOMPILE=0 set?)\n");
        return 0;
    }

    test_iwram_heal_and_dispatch(bus);
    test_pic_arm_and_thumb_aliases(bus);
    test_pic_nested_dispatch_preserves_caller_base(bus);
    test_pic_automatic_cross_address_reuse(bus);
    test_same_pc_distinct_requests_are_independent(bus);
    test_dispatch_miss_drains_ready_before_bridge(bus);
    test_iwram_content_change_forces_reheal(bus);
    test_write_then_execute_same_invocation(bus);
    test_mutable_ram_miss_reuses_healed_variant(bus);
    test_idle_backedge_overlay_compiles(bus);
    test_bridge_handoff_to_already_healed_callee(bus);
    test_top_level_ram_bridge_is_resumable(bus);
    test_bridge_handoff_drains_ready_callee(bus);
    test_ram_three_variant_alternation_no_reinterpretation(bus);
    test_ram_genuinely_new_variant_still_bridges(bus);
    test_ram_cap_eviction_lru(bus);
    test_rom_heal_unaffected(bus, rom);
    test_unmapped_region_never_heals();
    test_disk_pic_cross_address_reuse(bus, cache_root);
    test_warm_load_multivariant_after_restart(bus, cache_root);
    test_warm_load_cap_prefers_most_recent(bus, cache_root);
    test_warm_load_never_enters_on_live_mismatch(bus, cache_root);
    test_warm_load_races_organic_heal_no_corruption(bus, cache_root);
    test_shutdown_discards_queued_work(bus);
    gbarecomp::set_active_bus(nullptr);

    const fs::path misses = frame_events + ".misses.csv";
    std::FILE* frame_file = std::fopen(frame_events.c_str(), "rb");
    check(frame_file != nullptr, "frame-events CSV was not written");
    if (frame_file) {
        char header[512]{};
        std::fgets(header, sizeof(header), frame_file);
        std::fclose(frame_file);
        check(std::strstr(header,
              "ready_pending_at_frame,ready_pending_after_drain,"
              "ready_drain_calls,ready_drain_entries,ready_drain_us,"
              "warm_load_done") != nullptr,
              "frame-events loader diagnostic header is incomplete");
    }
    std::FILE* miss_file = std::fopen(misses.string().c_str(), "rb");
    check(miss_file != nullptr, "per-miss frame-events CSV was not written");
    if (miss_file) {
        char header[160]{};
        std::fgets(header, sizeof(header), miss_file);
        std::fclose(miss_file);
        check(std::strstr(header,
              "frame,pc,mode,bridge_us,interpreted_insns,request") != nullptr,
              "per-miss frame-events CSV header is incomplete");
    }

    if (g_failures != 0) {
        std::printf("ram_heal_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("ram_heal_test: PASS\n");
    return 0;
}
