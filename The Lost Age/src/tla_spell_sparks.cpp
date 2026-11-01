// Golden Sun: The Lost Age -- spell sparks past the console's canvas.
//
// Golden Sun 1's skipped-spark capture (its FACTS.md, 2026-10-02), re-found
// on this ROM (FACTS.md, 2026-10-08). Eleven battle spell routines move a
// particle record, then bounds tests (x past the canvas, y < 0) jump
// straight to the instruction after the stamp call, so a spark that left
// the 128x128 canvas keeps moving but is not drawn. Here the stamp call is
// `mov lr, rN` / `bl lr` (0xF800), not GS1's `bl 0x080072F4` (`bx r4`).
// Plasma's loop is 0x08149A84..0x08149B0C (bench: state 2 with Plasma,
// the stack at the frame wait held 0x08149B2F); the other ten were found
// by the same shape over the whole ROM: of 2075 `bl lr` calls, these have
// a bounce `b` to the join and x and y bound tests to it. Supernova was
// found later from GS1's loop (FACTS.md, 2026-10-30).
//
// On a taken test the game's own skipped path is dry-run (tla_dry_run.h):
// the remaining bounds tests are treated as passed, and at the stamp call
// its routine (lr), canvas (r0), art (r1), box (r2, r3) and size ([sp],
// [sp+4]) go to the host spark capture. The game's own state and decisions
// are never changed.

#include "tla_rom_version.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>

#include "effect_capture.h"
#include "runtime.h"
#include "runtime_bus_bridge.h"
#include "tla.h"
#include "tla_dry_run.h"

namespace {

struct SkippedSparkLoop {
    std::array<std::uint32_t, 2> calls;  // `bl lr` halfwords
    std::uint32_t join;                  // the instruction after the call
    std::array<std::uint32_t, 3> tests;  // bounds branches to the join
};
const SkippedSparkLoop kLoops[] = {
    {{TLA_ROM(0x08149AFEu), 0u}, TLA_ROM(0x08149B00u), {TLA_ROM(0x08149AB8u), TLA_ROM(0x08149ABCu), TLA_ROM(0x08149AC0u)}},  // Plasma
    // Supernova: GS1's loop instruction for instruction; each test jumps to
    // its own reload tail, not the join, so the shape sweep missed it.
    {{TLA_ROM(0x0814A150u), 0u}, TLA_ROM(0x0814A152u), {TLA_ROM(0x0814A10Au), TLA_ROM(0x0814A10Eu), 0u}},
    {{TLA_ROM(0x0814A732u), 0u}, TLA_ROM(0x0814A734u), {TLA_ROM(0x0814A6E8u), TLA_ROM(0x0814A6EEu), TLA_ROM(0x0814A6F2u)}},
    {{TLA_ROM(0x08151F2Cu), 0u}, TLA_ROM(0x08151F2Eu), {TLA_ROM(0x08151EF2u), TLA_ROM(0x08151EF6u), 0u}},
    {{TLA_ROM(0x081557CEu), 0u}, TLA_ROM(0x081557D0u), {TLA_ROM(0x08155798u), TLA_ROM(0x0815579Cu), 0u}},
    {{TLA_ROM(0x08159ADEu), 0u}, TLA_ROM(0x08159AE0u), {TLA_ROM(0x08159AAAu), TLA_ROM(0x08159AAEu), 0u}},
    {{TLA_ROM(0x08166A08u), 0u}, TLA_ROM(0x08166A0Au), {TLA_ROM(0x081669C2u), TLA_ROM(0x081669C6u), 0u}},
    {{TLA_ROM(0x081671B0u), 0u}, TLA_ROM(0x081671B2u), {TLA_ROM(0x0816717Au), TLA_ROM(0x0816717Eu), 0u}},
    {{TLA_ROM(0x0816A18Au), 0u}, TLA_ROM(0x0816A18Cu), {TLA_ROM(0x0816A146u), TLA_ROM(0x0816A14Cu), TLA_ROM(0x0816A150u)}},
    {{TLA_ROM(0x081821DCu), 0u}, TLA_ROM(0x081821DEu), {TLA_ROM(0x0818218Au), TLA_ROM(0x08182190u), TLA_ROM(0x08182194u)}},
    // Two stamp calls, chosen by a field of the spell's block.
    {{TLA_ROM(0x08189088u), TLA_ROM(0x081890E2u)}, TLA_ROM(0x081890E4u), {TLA_ROM(0x08189044u), TLA_ROM(0x08189048u), 0u}},
    {{TLA_ROM(0x0818DD50u), 0u}, TLA_ROM(0x0818DD52u), {TLA_ROM(0x0818DD1Au), TLA_ROM(0x0818DD1Eu), 0u}},
};

void capture_skipped_spark(const SkippedSparkLoop& loop, std::uint32_t pc) {
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus) return;
    TlaDryRunBus dry(bus);
    armv4t::CPUState cpu{};
    gbarecomp::load_arm_cpu_into_interp(g_cpu, cpu);
    cpu.R[15] = pc + 2u;  // the branch not taken
    cpu.cpsr.t = true;
    cpu.thumb = true;
    // The longest path (a divide helper) is a few hundred steps.
    tla_dry_run(cpu, dry, 4096, [&](armv4t::CPUState& c) {
        const std::uint32_t at = c.R[15];
        if (!c.thumb) return false;
        if (at == loop.join) return true;
        if (std::find(loop.calls.begin(), loop.calls.end(), at) != loop.calls.end()) {
            const std::uint32_t sp = c.R[13];
            const std::uint32_t width = dry.read32(sp);
            const std::uint32_t height = dry.read32(sp + 4u);
            if (!dry.failed)
                gsr::effect_capture_skipped_spark(
                    c.R[14], c.R[0], c.R[1], static_cast<std::int32_t>(c.R[2]),
                    static_cast<std::int32_t>(c.R[3]), width, height,
                    bus->ewram_ptr(), bus->iwram_ptr(), bus->rom_ptr(),
                    bus->rom_size(), runtime_current_frame());
            return true;
        }
        if (std::find(loop.tests.begin(), loop.tests.end(), at) != loop.tests.end())
            c.R[15] = at + 2u;  // this test passes too
        return false;
    });
}

int (*g_next_branch)(std::uint32_t, std::uint32_t, std::uint32_t*) = nullptr;

int spark_branch(std::uint32_t pc, std::uint32_t original, std::uint32_t* out_decision) {
    for (const auto& loop : kLoops)
        for (const std::uint32_t t : loop.tests)
            if (t != 0u && t == pc) {
                if (original != 0u && gsr::effect_capture_enabled())
                    capture_skipped_spark(loop, pc);
                return 0;
            }
    return g_next_branch ? g_next_branch(pc, original, out_decision) : 0;
}

}  // namespace

void tla_install_spell_sparks(gbarecomp::RunOptions& options) {
    g_next_branch = options.conditional_branch_override;
    options.conditional_branch_override = spark_branch;
}
