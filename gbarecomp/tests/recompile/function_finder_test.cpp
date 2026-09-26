#include "function_finder.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

constexpr std::uint32_t kBase = 0x08000000u;

void put16(std::vector<std::uint8_t>& rom, std::size_t off,
           std::uint16_t value) {
    rom[off] = static_cast<std::uint8_t>(value);
    rom[off + 1] = static_cast<std::uint8_t>(value >> 8);
}

void put32(std::vector<std::uint8_t>& rom, std::size_t off,
           std::uint32_t value) {
    put16(rom, off, static_cast<std::uint16_t>(value));
    put16(rom, off + 2, static_cast<std::uint16_t>(value >> 16));
}

bool has_function(const gbarecomp::FunctionFinder& finder,
                  std::uint32_t addr, gbarecomp::CpuMode mode) {
    for (const auto& fn : finder.functions()) {
        if (fn.addr == addr && fn.mode == mode) return true;
    }
    return false;
}

const gbarecomp::Function* find_function(
    const gbarecomp::FunctionFinder& finder,
    std::uint32_t addr,
    gbarecomp::CpuMode mode) {
    for (const auto& fn : finder.functions()) {
        if (fn.addr == addr && fn.mode == mode) return &fn;
    }
    return nullptr;
}

}  // namespace

int main() {
    std::vector<std::uint8_t> rom(0x80, 0);

    // ldr r1,[pc,#8]  -> tracker sees the even ROM data pointer below
    // pop {r1}        -> r1 is overwritten from the runtime stack
    // bx r1           -> dynamic return; must not reuse the stale literal
    put16(rom, 0x00, 0x4902u);
    put16(rom, 0x02, 0xBC02u);
    put16(rom, 0x04, 0x4708u);
    put32(rom, 0x0C, kBase + 0x20u);

    // Make the false target valid-looking ARM code. Before the regression
    // fix, the stale constant caused this address to be emitted.
    put32(rom, 0x20, 0xE12FFF1Eu);  // bx lr

    gbarecomp::FunctionFinder finder(rom.data(), rom.size(), kBase);
    finder.set_speculative_literal_harvest(false);
    finder.add_seed({kBase, gbarecomp::CpuMode::Thumb, "pop_bx_return"});
    finder.run(32);

    if (!has_function(finder, kBase, gbarecomp::CpuMode::Thumb)) {
        std::fprintf(stderr, "entry function was not discovered\n");
        return 1;
    }
    if (has_function(finder, kBase + 0x20u, gbarecomp::CpuMode::Arm)) {
        std::fprintf(stderr,
                     "POP destination retained a stale constant and seeded "
                     "0x%08X as ARM code\n",
                     kBase + 0x20u);
        return 1;
    }

    // LR is a general-purpose scratch register in ARMv4T code.  A literal
    // data pointer copied into LR must not be mistaken for the statically
    // known return PC of a later `bx rN` dynamic return.  Golden Sun uses this
    // shape in Func_799b0.
    std::vector<std::uint8_t> scratch_lr_rom(0x80, 0);
    put16(scratch_lr_rom, 0x00, 0x4B03u); // ldr r3,[pc,#12] -> [0x10]
    put16(scratch_lr_rom, 0x02, 0x469Eu); // mov lr,r3
    put16(scratch_lr_rom, 0x04, 0x4708u); // bx r1 (dynamic return)
    put32(scratch_lr_rom, 0x10, kBase + 0x40u);
    put32(scratch_lr_rom, 0x40, 0xE12FFF1Eu); // valid-looking ARM bx lr

    gbarecomp::FunctionFinder scratch_lr(
        scratch_lr_rom.data(), scratch_lr_rom.size(), kBase);
    scratch_lr.set_speculative_literal_harvest(false);
    scratch_lr.add_seed(
        {kBase, gbarecomp::CpuMode::Thumb, "scratch_lr_data_pointer"});
    scratch_lr.run(32);
    if (has_function(scratch_lr, kBase + 0x40u,
                     gbarecomp::CpuMode::Arm)) {
        std::fprintf(stderr,
                     "literal data pointer in LR was inferred as a return PC\n");
        return 1;
    }

    // A stack-local helper may be copied again at an overlapping address.
    // The explicit source address must win over the first fixed mapping, and
    // CFG roots from the older placement must not truncate the newer body.
    std::vector<std::uint8_t> relocated_rom(0x80, 0);
    constexpr std::size_t kTemplate = 0x40;
    put16(relocated_rom, kTemplate + 0x00, 0x2800u); // cmp r0,#0
    put16(relocated_rom, kTemplate + 0x02, 0xD001u); // beq +8
    put16(relocated_rom, kTemplate + 0x04, 0x4770u); // bx lr
    put16(relocated_rom, kTemplate + 0x06, 0x46C0u); // nop
    put16(relocated_rom, kTemplate + 0x08, 0x4770u); // bx lr

    constexpr std::uint32_t kRam = 0x03000000u;
    gbarecomp::FunctionFinder relocated(
        relocated_rom.data(), relocated_rom.size(), kBase);
    relocated.set_speculative_literal_harvest(false);
    relocated.add_code_copy(kRam, kBase + kTemplate, 0x20, "first copy");
    relocated.add_seed({kRam, gbarecomp::CpuMode::Thumb, "first"});
    gbarecomp::FunctionSeed overlapping{
        kRam + 4, gbarecomp::CpuMode::Thumb, "overlapping"};
    overlapping.source_addr = kBase + kTemplate;
    relocated.add_seed(overlapping);
    relocated.run(32);

    const auto* overlap_fn = find_function(
        relocated, kRam + 4, gbarecomp::CpuMode::Thumb);
    if (!overlap_fn || overlap_fn->source_addr != kBase + kTemplate) {
        std::fprintf(stderr,
                     "overlapping relocation did not retain explicit ROM backing\n");
        return 1;
    }
    if (overlap_fn->end_addr <= kRam + 8) {
        std::fprintf(stderr,
                     "older placement incorrectly truncated overlapping CFG\n");
        return 1;
    }
    const auto* overlap_target = find_function(
        relocated, kRam + 12, gbarecomp::CpuMode::Thumb);
    if (!overlap_target ||
        overlap_target->source_addr != kBase + kTemplate + 8) {
        std::fprintf(stderr,
                     "relocation bias did not propagate to direct branch target\n");
        return 1;
    }

    // A reviewed runtime-code entry is a dispatch boundary, not permission to
    // decode the immutable placeholder bytes as instructions. The same walk
    // remains a hard collision without the exact (PC, mode) declaration.
    std::vector<std::uint8_t> dynamic_rom(0x20, 0);
    put32(dynamic_rom, 0x00, 0xE1A00000u); // ARM nop; falls through
    put32(dynamic_rom, 0x04, 0xFEDCBA98u); // runtime-overwritten placeholder

    gbarecomp::FunctionFinder dynamic_rejected(
        dynamic_rom.data(), dynamic_rom.size(), kBase);
    dynamic_rejected.set_speculative_literal_harvest(false);
    dynamic_rejected.add_data_range(kBase + 4u, kBase + 8u,
                                    "runtime placeholder");
    dynamic_rejected.add_seed(
        {kBase, gbarecomp::CpuMode::Arm, "dynamic_rejected"});
    dynamic_rejected.run(8);
    if (dynamic_rejected.collisions().empty()) {
        std::fprintf(stderr,
                     "unreviewed control flow into data was not rejected\n");
        return 1;
    }

    gbarecomp::FunctionFinder dynamic_accepted(
        dynamic_rom.data(), dynamic_rom.size(), kBase);
    dynamic_accepted.set_speculative_literal_harvest(false);
    dynamic_accepted.add_data_range(kBase + 4u, kBase + 8u,
                                    "runtime placeholder");
    dynamic_accepted.add_runtime_code_entry(
        kBase + 4u, gbarecomp::CpuMode::Arm,
        "identity-verified runtime-generated code");
    dynamic_accepted.add_seed(
        {kBase, gbarecomp::CpuMode::Arm, "dynamic_accepted"});
    dynamic_accepted.run(8);
    const auto* dynamic_host = find_function(
        dynamic_accepted, kBase, gbarecomp::CpuMode::Arm);
    if (!dynamic_accepted.collisions().empty() || !dynamic_host ||
        dynamic_host->end_addr != kBase + 4u ||
        has_function(dynamic_accepted, kBase + 4u,
                     gbarecomp::CpuMode::Arm)) {
        std::fprintf(stderr,
                     "runtime-code entry did not remain an external boundary\n");
        return 1;
    }

    // Fix A (THUMB): a long-range BL whose pc+4 continuation lands inside a
    // declared data_range is a proven long branch (a returning call cannot
    // have a literal pool as its return address), not a real collision.
    // BL prefix (H=0, imm11=0) + suffix (H=1, imm11=30) combine to
    // full = (kBase+4) + (30<<1) = kBase+0x40.
    std::vector<std::uint8_t> long_bl_thumb_rom(0x80, 0);
    put16(long_bl_thumb_rom, 0x00, 0xF000u);  // BL prefix, imm11=0
    put16(long_bl_thumb_rom, 0x02, 0xF81Eu);  // BL suffix, imm11=30
    put16(long_bl_thumb_rom, 0x40, 0x4770u);  // bx lr (real BL target)

    gbarecomp::FunctionFinder long_bl_thumb(
        long_bl_thumb_rom.data(), long_bl_thumb_rom.size(), kBase);
    long_bl_thumb.set_speculative_literal_harvest(false);
    long_bl_thumb.add_data_range(kBase + 0x04u, kBase + 0x08u,
                                 "literal pool after long BL");
    long_bl_thumb.add_seed(
        {kBase, gbarecomp::CpuMode::Thumb, "long_branch_thumb"});
    long_bl_thumb.run(32);

    if (!long_bl_thumb.collisions().empty()) {
        std::fprintf(stderr,
                     "THUMB long-branch BL wrongly recorded a data_range "
                     "collision\n");
        return 1;
    }
    if (long_bl_thumb.stats().long_branch_calls != 1) {
        std::fprintf(stderr,
                     "THUMB long-branch BL counter expected 1, got %zu\n",
                     long_bl_thumb.stats().long_branch_calls);
        return 1;
    }
    if (!has_function(long_bl_thumb, kBase + 0x40u,
                      gbarecomp::CpuMode::Thumb)) {
        std::fprintf(stderr,
                     "THUMB long-branch BL's real target was not "
                     "discovered\n");
        return 1;
    }

    // Fix A (ARM): the single-instruction equivalent. BL AL, imm24=14 =>
    // target = (kBase+8) + (14<<2) = kBase+0x40; continuation kBase+4 is
    // inside the declared data_range.
    std::vector<std::uint8_t> long_bl_arm_rom(0x80, 0);
    put32(long_bl_arm_rom, 0x00, 0xEB00000Eu);  // bl kBase+0x40
    put32(long_bl_arm_rom, 0x40, 0xE12FFF1Eu);  // bx lr

    gbarecomp::FunctionFinder long_bl_arm(
        long_bl_arm_rom.data(), long_bl_arm_rom.size(), kBase);
    long_bl_arm.set_speculative_literal_harvest(false);
    long_bl_arm.add_data_range(kBase + 0x04u, kBase + 0x08u,
                               "literal pool after long BL");
    long_bl_arm.add_seed(
        {kBase, gbarecomp::CpuMode::Arm, "long_branch_arm"});
    long_bl_arm.run(32);

    if (!long_bl_arm.collisions().empty()) {
        std::fprintf(stderr,
                     "ARM long-branch BL wrongly recorded a data_range "
                     "collision\n");
        return 1;
    }
    if (long_bl_arm.stats().long_branch_calls != 1) {
        std::fprintf(stderr,
                     "ARM long-branch BL counter expected 1, got %zu\n",
                     long_bl_arm.stats().long_branch_calls);
        return 1;
    }
    if (!has_function(long_bl_arm, kBase + 0x40u, gbarecomp::CpuMode::Arm)) {
        std::fprintf(stderr,
                     "ARM long-branch BL's real target was not "
                     "discovered\n");
        return 1;
    }

    // Fix A negative control: entering a data_range by a route that is NOT
    // a direct call's continuation (here, plain fall-through from a
    // non-call THUMB instruction) must still be a hard collision. This is
    // what proves the fix narrowly targets the BL-continuation case
    // instead of blanket-disabling the data_range gate.
    std::vector<std::uint8_t> plain_fallthrough_rom(0x80, 0);
    put16(plain_fallthrough_rom, 0x00, 0x46C0u);  // mov r8,r8 (nop, non-call)

    gbarecomp::FunctionFinder plain_fallthrough(
        plain_fallthrough_rom.data(), plain_fallthrough_rom.size(), kBase);
    plain_fallthrough.set_speculative_literal_harvest(false);
    plain_fallthrough.add_data_range(kBase + 0x02u, kBase + 0x06u,
                                     "not a call continuation");
    plain_fallthrough.add_seed(
        {kBase, gbarecomp::CpuMode::Thumb, "plain_fallthrough"});
    plain_fallthrough.run(8);

    if (plain_fallthrough.collisions().empty()) {
        std::fprintf(stderr,
                     "non-call fall-through into data_range was wrongly "
                     "suppressed as a long branch\n");
        return 1;
    }
    if (plain_fallthrough.stats().long_branch_calls != 0) {
        std::fprintf(stderr,
                     "non-call fall-through incorrectly counted as a "
                     "long-branch call\n");
        return 1;
    }

    // Fix B: an alias_candidate THUMB seed landing on a BL_suffix halfword
    // (0xF800..0xFFFF) is never a legal instruction boundary in ARMv4T (the
    // only 4-byte THUMB instruction is the BL pair) and must be dropped.
    // A non-alias seed at the same kind of address is a genuine authoring
    // error and is unaffected; an alias_candidate seed on an ordinary
    // halfword is still accepted.
    std::vector<std::uint8_t> alias_bl_suffix_rom(0x80, 0);
    put16(alias_bl_suffix_rom, 0x00, 0xF81Eu);  // BL_suffix halfword
    put16(alias_bl_suffix_rom, 0x10, 0xF81Eu);  // BL_suffix halfword
    put16(alias_bl_suffix_rom, 0x12, 0x4770u);  // bx lr, terminates the walk
    put16(alias_bl_suffix_rom, 0x20, 0x4770u);  // ordinary halfword: bx lr

    gbarecomp::FunctionFinder alias_bl_suffix(
        alias_bl_suffix_rom.data(), alias_bl_suffix_rom.size(), kBase);
    alias_bl_suffix.set_speculative_literal_harvest(false);

    gbarecomp::FunctionSeed alias_on_suffix{
        kBase + 0x00u, gbarecomp::CpuMode::Thumb, "alias_on_suffix"};
    alias_on_suffix.alias_candidate = true;
    alias_bl_suffix.add_seed(alias_on_suffix);

    gbarecomp::FunctionSeed nonalias_on_suffix{
        kBase + 0x10u, gbarecomp::CpuMode::Thumb, "nonalias_on_suffix"};
    alias_bl_suffix.add_seed(nonalias_on_suffix);  // alias_candidate = false

    gbarecomp::FunctionSeed alias_on_normal{
        kBase + 0x20u, gbarecomp::CpuMode::Thumb, "alias_on_normal"};
    alias_on_normal.alias_candidate = true;
    alias_bl_suffix.add_seed(alias_on_normal);

    alias_bl_suffix.run(32);

    if (has_function(alias_bl_suffix, kBase + 0x00u,
                     gbarecomp::CpuMode::Thumb)) {
        std::fprintf(stderr,
                     "alias-candidate seed on a BL_suffix halfword was not "
                     "dropped\n");
        return 1;
    }
    if (alias_bl_suffix.stats().alias_seeds_dropped_bl_suffix != 1) {
        std::fprintf(stderr,
                     "alias_seeds_dropped_bl_suffix expected 1, got %zu\n",
                     alias_bl_suffix.stats().alias_seeds_dropped_bl_suffix);
        return 1;
    }
    if (!has_function(alias_bl_suffix, kBase + 0x10u,
                      gbarecomp::CpuMode::Thumb)) {
        std::fprintf(stderr,
                     "non-alias seed on a BL_suffix halfword was wrongly "
                     "dropped\n");
        return 1;
    }
    if (!has_function(alias_bl_suffix, kBase + 0x20u,
                      gbarecomp::CpuMode::Thumb)) {
        std::fprintf(stderr,
                     "alias-candidate seed on an ordinary halfword was "
                     "wrongly dropped\n");
        return 1;
    }

    // Fix C: an alias_candidate seed on a BL_suffix halfword whose
    // PRECEDING halfword really is the matching BL_prefix is a genuine
    // post-IRQ resume point (ARMv4T's THUMB BL is two architectural
    // instructions; an IRQ can land between them with the partial target
    // already live in LR). It must now be KEPT and rolled into its host as
    // a mid-function alias entry, not dropped.
    std::vector<std::uint8_t> genuine_pair_rom(0x80, 0);
    put16(genuine_pair_rom, 0x10, 0xF000u);  // BL_prefix, imm hi = 0
    put16(genuine_pair_rom, 0x12, 0xF802u);  // BL_suffix, imm lo = 2
    put16(genuine_pair_rom, 0x14, 0x4770u);  // bx lr, terminates the host

    gbarecomp::FunctionFinder genuine_pair(
        genuine_pair_rom.data(), genuine_pair_rom.size(), kBase);
    genuine_pair.set_speculative_literal_harvest(false);
    // The host: a normal, non-alias seed that walks the whole BL pair.
    genuine_pair.add_seed(
        {kBase + 0x10u, gbarecomp::CpuMode::Thumb, "host_with_bl_pair"});
    // The resume candidate: an alias_candidate seed landing exactly on the
    // BL_suffix half of that same pair (as [[resume_range]] expansion or
    // extra_func resume=true would produce from an observed post-IRQ PC).
    gbarecomp::FunctionSeed resume_on_suffix{
        kBase + 0x12u, gbarecomp::CpuMode::Thumb, "resume_on_suffix"};
    resume_on_suffix.alias_candidate = true;
    genuine_pair.add_seed(resume_on_suffix);

    genuine_pair.run(32);

    if (has_function(genuine_pair, kBase + 0x12u, gbarecomp::CpuMode::Thumb)) {
        std::fprintf(stderr,
                     "genuine post-IRQ BL_suffix resume seed was emitted as "
                     "a standalone function instead of being rolled into "
                     "its host\n");
        return 1;
    }
    const gbarecomp::Function* host = find_function(
        genuine_pair, kBase + 0x10u, gbarecomp::CpuMode::Thumb);
    if (host == nullptr) {
        std::fprintf(stderr, "host function for the genuine BL pair was "
                             "not discovered\n");
        return 1;
    }
    bool suffix_is_alias = false;
    for (std::uint32_t a : host->alias_entries) {
        if (a == kBase + 0x12u) suffix_is_alias = true;
    }
    if (!suffix_is_alias) {
        std::fprintf(stderr,
                     "genuine post-IRQ BL_suffix resume seed was not rolled "
                     "into its host's alias_entries — resume would still "
                     "dispatch-miss\n");
        return 1;
    }
    if (genuine_pair.stats().alias_seeds_dropped_bl_suffix != 0) {
        std::fprintf(stderr,
                     "a genuine post-IRQ BL_suffix resume seed was counted "
                     "as dropped (expected 0, got %zu)\n",
                     genuine_pair.stats().alias_seeds_dropped_bl_suffix);
        return 1;
    }

    // Fix C negative control: a BL_suffix-shaped halfword whose preceding
    // halfword is NOT a BL_prefix is still a coincidental bit-pattern
    // match, not a real resume point, and must still be dropped (and
    // counted).
    std::vector<std::uint8_t> fake_suffix_rom(0x80, 0);
    put16(fake_suffix_rom, 0x2Eu, 0x1C00u);  // MOV r0, r0 — NOT a BL_prefix
    put16(fake_suffix_rom, 0x30u, 0xF81Eu);  // BL_suffix-shaped halfword

    gbarecomp::FunctionFinder fake_suffix(
        fake_suffix_rom.data(), fake_suffix_rom.size(), kBase);
    fake_suffix.set_speculative_literal_harvest(false);
    gbarecomp::FunctionSeed fake_resume{
        kBase + 0x30u, gbarecomp::CpuMode::Thumb, "fake_resume"};
    fake_resume.alias_candidate = true;
    fake_suffix.add_seed(fake_resume);
    fake_suffix.run(32);

    if (has_function(fake_suffix, kBase + 0x30u, gbarecomp::CpuMode::Thumb)) {
        std::fprintf(stderr,
                     "alias-candidate seed on a coincidental BL_suffix bit "
                     "pattern (no preceding BL_prefix) was wrongly kept\n");
        return 1;
    }
    if (fake_suffix.stats().alias_seeds_dropped_bl_suffix != 1) {
        std::fprintf(stderr,
                     "fake_suffix: alias_seeds_dropped_bl_suffix expected "
                     "1, got %zu\n",
                     fake_suffix.stats().alias_seeds_dropped_bl_suffix);
        return 1;
    }

    // Fix C constraint: the BL_prefix half must never become an entry
    // point "on its own" by virtue of this fix — the gate only ever
    // matches IrOp::BL_suffix, so an alias_candidate seed sitting on the
    // BL_prefix half is untouched by it and follows the pre-existing
    // generic alias path (here: no containing host, so it stands alone,
    // exactly as any other alias_candidate seed with no host would).
    std::vector<std::uint8_t> prefix_seed_rom(0x80, 0);
    put16(prefix_seed_rom, 0x00, 0xF000u);  // BL_prefix
    put16(prefix_seed_rom, 0x02, 0xF802u);  // BL_suffix
    put16(prefix_seed_rom, 0x04, 0x4770u);  // bx lr

    gbarecomp::FunctionFinder prefix_seed_finder(
        prefix_seed_rom.data(), prefix_seed_rom.size(), kBase);
    prefix_seed_finder.set_speculative_literal_harvest(false);
    gbarecomp::FunctionSeed alias_on_prefix{
        kBase + 0x00u, gbarecomp::CpuMode::Thumb, "alias_on_prefix"};
    alias_on_prefix.alias_candidate = true;
    prefix_seed_finder.add_seed(alias_on_prefix);
    prefix_seed_finder.run(32);

    if (!has_function(prefix_seed_finder, kBase + 0x00u,
                      gbarecomp::CpuMode::Thumb)) {
        std::fprintf(stderr,
                     "alias-candidate seed on the BL_prefix half was "
                     "wrongly dropped by the BL_suffix resume gate\n");
        return 1;
    }
    if (prefix_seed_finder.stats().alias_seeds_dropped_bl_suffix != 0) {
        std::fprintf(stderr,
                     "alias-candidate seed on the BL_prefix half was "
                     "wrongly counted by the BL_suffix drop gate\n");
        return 1;
    }

    // Fix D: the BL-pair combine/continuation logic (Fix A) must apply
    // identically when a walk's very first decoded instruction IS the
    // BL_suffix halfword — not just when the walk decoded the matching
    // BL_prefix moments earlier in the same walk. This is exactly the
    // shape a genuine post-IRQ BL_suffix resume seed (Fix C) produces: its
    // own discover_one walk starts at the suffix address directly. Using
    // a plain (non-alias) seed here isolates the walk behaviour itself
    // from the separate seed-survival gate already covered by Fix C.
    //
    // Same ROM shape as the Fix A THUMB case (prefix @0x00, suffix @0x02,
    // continuation data_range @0x04-0x08, real target @0x40) but the seed
    // lands directly on the suffix instead of the prefix.
    std::vector<std::uint8_t> suffix_entry_into_data_rom(0x80, 0);
    put16(suffix_entry_into_data_rom, 0x00, 0xF000u);  // BL_prefix, imm11=0
    put16(suffix_entry_into_data_rom, 0x02, 0xF81Eu);  // BL_suffix, imm11=30
    put16(suffix_entry_into_data_rom, 0x40, 0x4770u);  // bx lr (real target)

    gbarecomp::FunctionFinder suffix_entry_into_data(
        suffix_entry_into_data_rom.data(), suffix_entry_into_data_rom.size(),
        kBase);
    suffix_entry_into_data.set_speculative_literal_harvest(false);
    suffix_entry_into_data.add_data_range(kBase + 0x04u, kBase + 0x08u,
                                          "literal pool after long BL");
    suffix_entry_into_data.add_seed(
        {kBase + 0x02u, gbarecomp::CpuMode::Thumb,
         "entering_at_suffix_into_data"});
    suffix_entry_into_data.run(32);

    if (!suffix_entry_into_data.collisions().empty()) {
        std::fprintf(stderr,
                     "a walk entering at a BL_suffix whose continuation "
                     "lands in a data_range wrongly recorded a collision\n");
        return 1;
    }
    if (suffix_entry_into_data.stats().long_branch_calls != 1) {
        std::fprintf(stderr,
                     "suffix-entry long-branch counter expected 1, got "
                     "%zu\n",
                     suffix_entry_into_data.stats().long_branch_calls);
        return 1;
    }
    if (!has_function(suffix_entry_into_data, kBase + 0x40u,
                      gbarecomp::CpuMode::Thumb)) {
        std::fprintf(stderr,
                     "suffix-entry walk's real BL target was not "
                     "discovered\n");
        return 1;
    }

    // Fix D negative control: a walk entering at a BL_suffix whose
    // continuation is ordinary code (not a data_range) must still combine
    // the pair, discover the real target, AND keep walking through the
    // continuation normally — proving the fix only suppresses the
    // continuation when it is proven to be a literal pool, not generally.
    std::vector<std::uint8_t> suffix_entry_ordinary_rom(0x80, 0);
    put16(suffix_entry_ordinary_rom, 0x10, 0xF000u);  // BL_prefix, imm11=0
    put16(suffix_entry_ordinary_rom, 0x12, 0xF802u);  // BL_suffix, imm11=2
    put16(suffix_entry_ordinary_rom, 0x14, 0x4770u);  // bx lr: continuation
    put16(suffix_entry_ordinary_rom, 0x18, 0x4770u);  // bx lr: real target

    gbarecomp::FunctionFinder suffix_entry_ordinary(
        suffix_entry_ordinary_rom.data(), suffix_entry_ordinary_rom.size(),
        kBase);
    suffix_entry_ordinary.set_speculative_literal_harvest(false);
    suffix_entry_ordinary.add_seed(
        {kBase + 0x12u, gbarecomp::CpuMode::Thumb,
         "entering_at_suffix_ordinary"});
    suffix_entry_ordinary.run(32);

    if (!suffix_entry_ordinary.collisions().empty()) {
        std::fprintf(stderr,
                     "suffix-entry walk with an ordinary continuation "
                     "wrongly recorded a collision\n");
        return 1;
    }
    if (suffix_entry_ordinary.stats().long_branch_calls != 0) {
        std::fprintf(stderr,
                     "suffix-entry walk with an ordinary continuation was "
                     "wrongly counted as a long branch (expected 0, got "
                     "%zu)\n",
                     suffix_entry_ordinary.stats().long_branch_calls);
        return 1;
    }
    if (!has_function(suffix_entry_ordinary, kBase + 0x18u,
                      gbarecomp::CpuMode::Thumb)) {
        std::fprintf(stderr,
                     "suffix-entry walk's real BL target was not "
                     "discovered (ordinary-continuation case)\n");
        return 1;
    }
    const gbarecomp::Function* suffix_entry_fn = find_function(
        suffix_entry_ordinary, kBase + 0x12u, gbarecomp::CpuMode::Thumb);
    if (suffix_entry_fn == nullptr ||
        suffix_entry_fn->walk_end_addr < kBase + 0x16u) {
        std::fprintf(stderr,
                     "suffix-entry walk did not continue past the "
                     "ordinary continuation instruction\n");
        return 1;
    }

    // Fix E: an alias_candidate seed whose walk runs into a declared
    // [[data_range]] is unreachable by construction (real code cannot fall
    // into its own data_range) and must be dropped, not reported as a
    // collision. Synthetic GS-011 shape: an unconditional ARM computed jump
    // (`LDR pc,[pc,r12,LSL#2]`, is_indirect) terminates the real function's
    // walk; the alias candidate lands one instruction further on, at a NOP
    // the ELF still marks as code, and its own walk falls straight into the
    // declared data_range because nothing terminates it first.
    std::vector<std::uint8_t> alias_into_data_rom(0x40, 0);
    put32(alias_into_data_rom, 0x00, 0xE203C007u);  // AND r12,r3,#7
    put32(alias_into_data_rom, 0x04, 0xE79FF10Cu);  // LDR pc,[pc,r12,LSL#2]
    put32(alias_into_data_rom, 0x08, 0xE1A00000u);  // MOV r0,r0 (nop; alias entry)
    // [0x0C, 0x10) is the declared data_range (jump-table pad); left zeroed.

    gbarecomp::FunctionFinder alias_into_data(
        alias_into_data_rom.data(), alias_into_data_rom.size(), kBase);
    alias_into_data.set_speculative_literal_harvest(false);
    alias_into_data.add_data_range(kBase + 0x0Cu, kBase + 0x10u,
                                   "jump-table pad");
    alias_into_data.add_seed(
        {kBase, gbarecomp::CpuMode::Arm, "real_entry"});
    gbarecomp::FunctionSeed alias_seed{
        kBase + 0x08u, gbarecomp::CpuMode::Arm, "alias_into_data"};
    alias_seed.alias_candidate = true;
    alias_into_data.add_seed(alias_seed);
    alias_into_data.run(32);

    if (!alias_into_data.collisions().empty()) {
        std::fprintf(stderr,
                     "alias-candidate seed whose walk entered a data_range "
                     "wrongly recorded a collision instead of being "
                     "dropped\n");
        return 1;
    }
    if (alias_into_data.stats().alias_seeds_dropped_data_range != 1) {
        std::fprintf(stderr,
                     "alias_seeds_dropped_data_range expected 1, got %zu\n",
                     alias_into_data.stats().alias_seeds_dropped_data_range);
        return 1;
    }
    if (has_function(alias_into_data, kBase + 0x08u,
                     gbarecomp::CpuMode::Arm)) {
        std::fprintf(stderr,
                     "alias-candidate seed whose walk entered a data_range "
                     "was still emitted as a function\n");
        return 1;
    }

    // Fix E negative control: a NON-alias seed at the exact same address,
    // walking into the exact same data_range, must still be a hard
    // collision. This is the regression that matters most — the fix must
    // not weaken the real gate for genuine seeds/symbols.
    gbarecomp::FunctionFinder nonalias_into_data(
        alias_into_data_rom.data(), alias_into_data_rom.size(), kBase);
    nonalias_into_data.set_speculative_literal_harvest(false);
    nonalias_into_data.add_data_range(kBase + 0x0Cu, kBase + 0x10u,
                                      "jump-table pad");
    nonalias_into_data.add_seed(
        {kBase + 0x08u, gbarecomp::CpuMode::Arm, "nonalias_into_data"});
    nonalias_into_data.run(32);

    if (nonalias_into_data.collisions().empty()) {
        std::fprintf(stderr,
                     "non-alias seed entering a data_range was wrongly "
                     "dropped instead of recording a collision\n");
        return 1;
    }
    if (nonalias_into_data.stats().alias_seeds_dropped_data_range != 0) {
        std::fprintf(stderr,
                     "non-alias seed into data_range was wrongly counted "
                     "under alias_seeds_dropped_data_range (expected 0, "
                     "got %zu)\n",
                     nonalias_into_data.stats()
                         .alias_seeds_dropped_data_range);
        return 1;
    }

    // Fix E control: an alias_candidate seed whose walk stays entirely in
    // code (never touches a data_range) is unaffected — it is rolled into
    // its host as a mid-function alias exactly as before, and neither drop
    // counter moves.
    std::vector<std::uint8_t> alias_in_code_rom(0x40, 0);
    put32(alias_in_code_rom, 0x00, 0xE1A00000u);  // MOV r0,r0 (nop; host entry)
    put32(alias_in_code_rom, 0x04, 0xE1A00000u);  // MOV r0,r0 (nop; alias entry)
    put32(alias_in_code_rom, 0x08, 0xE12FFF1Eu);  // BX LR (terminates)

    gbarecomp::FunctionFinder alias_in_code(
        alias_in_code_rom.data(), alias_in_code_rom.size(), kBase);
    alias_in_code.set_speculative_literal_harvest(false);
    alias_in_code.add_seed(
        {kBase, gbarecomp::CpuMode::Arm, "alias_in_code_host"});
    gbarecomp::FunctionSeed alias_in_code_seed{
        kBase + 0x04u, gbarecomp::CpuMode::Arm, "alias_in_code"};
    alias_in_code_seed.alias_candidate = true;
    alias_in_code.add_seed(alias_in_code_seed);
    alias_in_code.run(32);

    if (!alias_in_code.collisions().empty()) {
        std::fprintf(stderr,
                     "alias-candidate seed that stayed in code wrongly "
                     "recorded a collision\n");
        return 1;
    }
    if (alias_in_code.stats().alias_seeds_dropped_data_range != 0) {
        std::fprintf(stderr,
                     "alias-candidate seed that stayed in code was wrongly "
                     "dropped (alias_seeds_dropped_data_range expected 0, "
                     "got %zu)\n",
                     alias_in_code.stats().alias_seeds_dropped_data_range);
        return 1;
    }
    if (alias_in_code.stats().alias_entries_total != 1) {
        std::fprintf(stderr,
                     "alias-candidate seed that stayed in code was not "
                     "rolled up as a mid-function alias (alias_entries_total "
                     "expected 1, got %zu)\n",
                     alias_in_code.stats().alias_entries_total);
        return 1;
    }

    std::printf("function_finder_tests: PASS\n");
    return 0;
}
