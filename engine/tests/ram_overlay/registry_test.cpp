#include <cstdint>
#include <cstdio>
#include <string>

#include "ram_overlay_registry.h"
#include "runtime_arm.h"
#include "stubs.h"

namespace {

constexpr uint32_t kSharedPc = 0x02001000u;
constexpr uint32_t kStaticRamPc = 0x03000100u;
gbarecomp::RamOverlayRegistry* g_registry = nullptr;
unsigned g_a_calls = 0;
unsigned g_b_calls = 0;
unsigned g_c_calls = 0;
bool g_tail_probe = false;
unsigned g_tail_laps = 0;
std::uintptr_t g_tail_low = 0;
std::uintptr_t g_tail_high = 0;
bool g_block_ram_hook = false;
bool g_reenter_after_hook_decline = false;
bool g_decline_with_handled_sentinel = false;
bool g_c_reenter = false;
unsigned g_overlay_fallback_calls = 0;
unsigned g_return_hook_calls = 0;
uint32_t g_return_hook_pc = 0;
uint32_t g_return_hook_depth = 0;
int g_failures = 0;

void record_tail_stack() {
    int stack_marker = 0;
    const auto address = reinterpret_cast<std::uintptr_t>(&stack_marker);
    if (g_tail_low == 0 || address < g_tail_low) g_tail_low = address;
    if (address > g_tail_high) g_tail_high = address;
}

#if defined(__GNUC__) && !defined(__clang__)
// This synthetic body models a generated guest tail transfer. Keep its final
// runtime_dispatch call optimized in Debug builds so the test exercises the
// host-tail ABI rather than accumulating ordinary C++ call frames.
__attribute__((optimize("O2")))
#endif
void image_a_entry() {
    ++g_a_calls;
    g_cpu.R[0] = 0xAAAAAAAAu;
    if (g_block_ram_hook) {
        ++g_overlay_fallback_calls;
        return;
    }
    if (g_reenter_after_hook_decline) {
        g_block_ram_hook = true;
        runtime_dispatch(kSharedPc);
        g_block_ram_hook = false;
    }
    if (!g_tail_probe) return;
    record_tail_stack();
    if (++g_tail_laps < 2000u) {
#if defined(__clang__)
        [[clang::musttail]]
#endif
        return runtime_dispatch(kSharedPc);
    }
}

void image_b_entry() {
    ++g_b_calls;
    g_cpu.R[0] = 0xBBBBBBBBu;
}

void image_c_entry() {
    ++g_c_calls;
    if (!g_c_reenter) return;
    g_c_reenter = false;
    g_decline_with_handled_sentinel = true;
    runtime_dispatch(kStaticRamPc);
    g_decline_with_handled_sentinel = false;
}

void handled_dispatch_noop() {}

RuntimeGuestFn registry_dispatch_hook(uint32_t pc, int thumb) {
    if (g_decline_with_handled_sentinel && pc == kStaticRamPc) {
        runtime_dispatch_miss(pc | (thumb ? 1u : 0u));
        return &handled_dispatch_noop;
    }
    if (g_block_ram_hook) return nullptr;
    return g_registry ? g_registry->resolve(pc, thumb != 0) : nullptr;
}

void return_hook(uint32_t return_pc, uint32_t call_stack_depth) {
    ++g_return_hook_calls;
    g_return_hook_pc = return_pc;
    g_return_hook_depth = call_stack_depth;
}

void expect(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++g_failures;
}

void expect_registration(bool registered, const std::string& error) {
    expect(registered, error.empty() ? "overlay registration failed"
                                     : error.c_str());
}

void expect_activation(bool activated, const std::string& error) {
    expect(activated, error.empty() ? "overlay activation failed"
                                    : error.c_str());
}

}  // namespace

int main() {
    using gbarecomp::RamOverlayDispatchEntry;
    using gbarecomp::RamOverlayImageView;
    using gbarecomp::RamOverlayRegistry;

    const RamOverlayDispatchEntry image_a_entries[] = {
        {kSharedPc, 1u, image_a_entry},
    };
    const RamOverlayDispatchEntry image_b_entries[] = {
        {kSharedPc, 1u, image_b_entry},
    };
    const RamOverlayDispatchEntry image_c_entries[] = {
        {kStaticRamPc, 0u, image_c_entry},
    };

    RamOverlayRegistry registry;
    std::string error;
    bool registered = registry.register_image(
        RamOverlayImageView{"synthetic-a", kSharedPc,
                            kSharedPc + 0x20u, image_a_entries, 1u},
        &error);
    expect_registration(registered, error);
    registered = registry.register_image(
        RamOverlayImageView{"synthetic-b", kSharedPc,
                            kSharedPc + 0x20u, image_b_entries, 1u},
        &error);
    expect_registration(registered, error);
    registered = registry.register_image(
        RamOverlayImageView{"synthetic-c", kStaticRamPc,
                            kStaticRamPc + 0x20u, image_c_entries, 1u},
        &error);
    expect_registration(registered, error);

    g_registry = &registry;
    g_runtime_ram_dispatch_hook = registry_dispatch_hook;
    codegen_test::bus_reset(0x02000000u, 0x100u);
    g_cpu.cpsr = CPSR_T_BIT | 0x1Fu;

    // The tail resolver's active-entry lifetime is retired from guest return
    // handling, not from runtime_dispatch's C++ return. Verify the callback
    // observes the already-truncated guest stack for both return forms.
    runtime_call_stack_restore(nullptr, 0);
    g_return_hook_calls = 0;
    g_runtime_call_return_hook = return_hook;
    runtime_call_push_return(0x08000100u);
    runtime_call_push_return(0x08000200u);
    expect(runtime_call_should_return(0x08000200u) == 1,
           "nested guest return was not recognized");
    expect(g_return_hook_calls == 1u && g_return_hook_pc == 0x08000200u &&
               g_return_hook_depth == 1u,
           "return hook did not observe the post-return stack depth");
    runtime_call_cancel_return(0x08000100u);
    expect(g_return_hook_calls == 2u && g_return_hook_pc == 0x08000100u &&
               g_return_hook_depth == 0u,
           "cancel hook did not observe the truncated guest stack");
    g_runtime_call_return_hook = nullptr;

    bool activated = registry.activate("synthetic-a", &error);
    expect_activation(activated, error);
    runtime_dispatch(kSharedPc);
    expect(g_cpu.R[0] == 0xAAAAAAAAu, "image A did not dispatch");
    expect(g_a_calls == 1u && g_b_calls == 0u,
           "unexpected call count after image A dispatch");

    activated = registry.activate("synthetic-b", &error);
    expect_activation(activated, error);
    expect(!registry.is_active("synthetic-a"),
           "overlapping image A remained active after B activation");
    expect(registry.active_identity(kSharedPc) == "synthetic-b",
           "shared PC did not resolve to active image B identity");
    runtime_dispatch(kSharedPc);
    expect(g_cpu.R[0] == 0xBBBBBBBBu, "image B did not dispatch");
    expect(g_a_calls == 1u && g_b_calls == 1u,
           "stale image A was selected after B activation");

    expect(registry.invalidate_range(kSharedPc + 2u, kSharedPc + 4u) == 1u,
           "overwriting the active range did not invalidate image B");
    codegen_test::g_dispatch_called = false;
    runtime_dispatch(kSharedPc);
    expect(codegen_test::g_dispatch_called,
           "invalidated overlay did not fall through to dispatch miss");
    expect(codegen_test::g_last_dispatch_target == kSharedPc,
           "dispatch miss recorded the wrong PC");
    expect(g_a_calls == 1u && g_b_calls == 1u,
           "an invalidated overlay function was called");

    activated = registry.activate("synthetic-b", &error);
    expect_activation(activated, error);
    codegen_test::g_dispatch_called = false;
    g_cpu.cpsr &= ~CPSR_T_BIT;
    runtime_dispatch(kSharedPc);
    expect(codegen_test::g_dispatch_called,
           "ARM mode incorrectly selected a THUMB overlay entry");
    expect(g_a_calls == 1u && g_b_calls == 1u,
           "mode mismatch called an overlay function");

    // The resolver ABI is specifically intended to make repeated guest tail
    // transfers host-stack stable. Rebind image A and run 2000 synthetic
    // same-PC laps, recording a local stack address on every entry.
    activated = registry.activate("synthetic-a", &error);
    expect_activation(activated, error);
    g_tail_probe = true;
    g_tail_laps = 0;
    g_tail_low = g_tail_high = 0;
    g_cpu.cpsr = CPSR_T_BIT | 0x1Fu;
    runtime_dispatch(kSharedPc);
    g_tail_probe = false;
    expect(g_tail_laps == 2000u, "tail-dispatch probe did not complete 2000 laps");
    expect(g_tail_high >= g_tail_low && g_tail_high - g_tail_low < 64u * 1024u,
           "tail-dispatch probe grew the host stack unexpectedly");

    // A RAM resolver can decline a recursive re-entry after its active-entry
    // guard fires. runtime_dispatch must not bypass that decision by calling
    // the Stage-2 resolver for the same PC; doing so re-enters the same native
    // body. The correct fallback is the ordinary dispatch-miss path.
    g_tail_probe = false;
    g_reenter_after_hook_decline = true;
    g_block_ram_hook = false;
    g_overlay_fallback_calls = 0;
    codegen_test::g_overlay_resolve_fn = image_a_entry;
    codegen_test::g_dispatch_called = false;
    runtime_dispatch(kSharedPc);
    g_reenter_after_hook_decline = false;
    codegen_test::g_overlay_resolve_fn = nullptr;
    expect(codegen_test::g_dispatch_called,
           "RAM-hook decline bypassed to Stage-2 instead of dispatch miss");
    expect(g_overlay_fallback_calls == 0u,
           "Stage-2 resolver re-entered RAM body after hook decline");

    // A declined recursive RAM PC may also have a fixed/static table entry.
    // The resolver must return a non-null handled sentinel after recording the
    // miss; otherwise runtime_dispatch would select that stale static body.
    activated = registry.activate("synthetic-c", &error);
    expect_activation(activated, error);
    g_cpu.cpsr = 0x1Fu;
    g_c_calls = 0;
    g_c_reenter = true;
    codegen_test::g_dispatch_called = false;
    codegen_test::g_static_ram_calls = 0;
    runtime_dispatch(kStaticRamPc);
    expect(codegen_test::g_dispatch_called,
           "handled RAM decline did not record its interpreter bridge");
    expect(codegen_test::g_last_dispatch_target == kStaticRamPc,
           "handled RAM decline recorded the wrong PC");
    expect(codegen_test::g_static_ram_calls == 0u,
           "handled RAM decline re-entered a static RAM entry");
    expect(g_c_calls == 1u,
           "recursive RAM decline re-entered the native overlay body");

    g_runtime_ram_dispatch_hook = nullptr;
    g_registry = nullptr;

    // Existing generated RAM bodies have no entry CRC prologue. Prove the
    // generic dispatcher registers the page, observes a later write inside
    // [entry,next-entry), and bypasses the stale static function.
    codegen_test::bus_reset(kStaticRamPc, 0x100u);
    runtime_ram_code_dirty_reset();
    g_cpu.cpsr = 0x1Fu;
    codegen_test::g_static_ram_calls = 0;
    runtime_dispatch(kStaticRamPc);
    expect(codegen_test::g_static_ram_calls == 1u,
           "clean static RAM entry did not dispatch");
    bus_write_u32(kStaticRamPc + 4u, 0xE1A00000u);
    codegen_test::g_dispatch_called = false;
    runtime_dispatch(kStaticRamPc);
    expect(codegen_test::g_dispatch_called,
           "modified static RAM body did not enter fallback");
    expect(codegen_test::g_last_dispatch_target == kStaticRamPc,
           "modified static RAM fallback recorded the wrong PC");
    expect(codegen_test::g_static_ram_calls == 1u,
           "modified static RAM body executed stale AOT code");
    if (g_failures != 0) {
        std::fprintf(stderr, "%d ram overlay registry test(s) failed\n",
                     g_failures);
        return 1;
    }
    std::puts("ram overlay registry tests passed");
    return 0;
}
