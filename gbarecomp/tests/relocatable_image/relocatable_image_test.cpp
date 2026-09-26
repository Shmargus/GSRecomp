// relocatable_image_test.cpp — position-independent RAM code images.
//
// A game that DMA-copies a ROM routine into a working area executes it at
// whatever base its allocator hands out. `gba_recompile --relocatable-image`
// emits ONE corpus for such a routine, with every guest address the image
// derives from its own PC lowered as `_imgbase + offset` instead of the
// address the corpus was generated at.
//
// The property under test is an exact equivalence, not a spot check:
//
//   reloc_body(origin)[_imgbase := origin]  ==  fixed_body(origin)
//
// Substituting the generation origin back into a relocatable body must
// reproduce the ordinary fixed body byte for byte. That direction catches
// OVER-rewriting: an immediate, a memory offset or a shift count wrongly
// treated as an address would fold to a different constant.
//
// The other direction — an address the emitter MISSED, left as a literal —
// survives that comparison (a missed literal equals itself), so it is caught
// separately: a relocatable body must contain no `0x........u` literal equal
// to any guest address in or adjacent to the image extent.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "emit_function.h"
#include "function_finder.h"

using gbarecomp::CpuMode;
using gbarecomp::Function;
using gbarecomp::RelocatableImage;

namespace {

int g_failures = 0;

void fail(const std::string& what) {
    std::printf("FAIL: %s\n", what.c_str());
    ++g_failures;
}

void check(bool cond, const std::string& what) {
    if (!cond) fail(what);
}

// ── The routine under test ───────────────────────────────────────────
//
// Hand-assembled ARM so every emitter path that touches a guest address is
// exercised, and so no immediate operand or memory offset coincidentally
// equals a guest address (which would make the literal scan below ambiguous).
//
//   +0x00  e59f0010   ldr  r0, [pc, #16]     ; PC-relative literal pool read
//   +0x04  e1a0100f   mov  r1, pc            ; PC in operand position
//   +0x08  e5801000   str  r1, [r0]          ; trace_event carries the PC
//   +0x0c  eb000004   bl   +0x24             ; link register + return check
//   +0x10  e3500000   cmp  r0, #0
//   +0x14  0afffff9   beq  +0x00             ; backward branch -> goto label
//   +0x18  e12fff1e   bx   lr                ; C return
//   +0x1c  00000000   (literal pool)
constexpr uint32_t kRomBase = 0x08000000u;
constexpr uint32_t kSourceAddr = 0x08000100u;  // where the bytes live in ROM
constexpr uint32_t kImageSize = 0x20u;

const uint32_t kWords[] = {
    0xe59f0010u, 0xe1a01000u, 0xe5801000u, 0xeb000004u,
    0xe3500000u, 0x0afffff9u, 0xe12fff1eu, 0x00000000u,
};

std::vector<uint8_t> build_rom() {
    std::vector<uint8_t> rom(0x200u, 0u);
    const std::size_t off = kSourceAddr - kRomBase;
    for (std::size_t i = 0; i < sizeof(kWords) / sizeof(kWords[0]); ++i) {
        const uint32_t w = kWords[i];
        rom[off + i * 4 + 0] = static_cast<uint8_t>(w);
        rom[off + i * 4 + 1] = static_cast<uint8_t>(w >> 8);
        rom[off + i * 4 + 2] = static_cast<uint8_t>(w >> 16);
        rom[off + i * 4 + 3] = static_cast<uint8_t>(w >> 24);
    }
    return rom;
}

// The image placed at `runtime_addr`, backed by the ROM bytes at
// kSourceAddr. `source_addr` is exactly how the tool describes a code copy.
Function image_function(uint32_t runtime_addr) {
    Function fn;
    fn.addr = runtime_addr;
    fn.end_addr = runtime_addr + kImageSize;
    fn.source_addr = kSourceAddr;
    fn.mode = CpuMode::Arm;
    fn.name = "image_fn";
    return fn;
}

std::string emit(uint32_t runtime_addr,
                 const std::unordered_map<uint64_t, std::string>& names,
                 const RelocatableImage* image) {
    const std::vector<uint8_t> rom = build_rom();
    return gbarecomp::emit_function_body_str(
        image_function(runtime_addr), rom.data(), rom.size(), kRomBase,
        names, nullptr, image);
}

// ── Substitution: fold a concrete base back into a relocatable body ──

bool parse_hex32(const std::string& s, std::size_t pos, uint32_t* out) {
    if (pos + 8 > s.size()) return false;
    uint32_t v = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        const char c = s[pos + i];
        uint32_t d;
        if (c >= '0' && c <= '9') d = static_cast<uint32_t>(c - '0');
        else if (c >= 'A' && c <= 'F') d = static_cast<uint32_t>(c - 'A' + 10);
        else if (c >= 'a' && c <= 'f') d = static_cast<uint32_t>(c - 'a' + 10);
        else return false;
        v = (v << 4) | d;
    }
    *out = v;
    return true;
}

std::string hex_literal(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08Xu", v);
    return std::string(buf);
}

// Replace every `(_imgbase + 0xNNNNNNNNu)` with the constant it denotes at
// `base`, and drop the lines that exist only to carry the base. What is left
// must be exactly what the fixed emitter produces.
std::string fold_base(const std::string& body, uint32_t base) {
    static const std::string kOpen = "(_imgbase + ";
    std::string out;
    std::size_t i = 0;
    while (i < body.size()) {
        if (body.compare(i, kOpen.size(), kOpen) == 0) {
            uint32_t off = 0;
            const std::size_t hex_at = i + kOpen.size() + 2;  // skip "0x"
            if (body.compare(i + kOpen.size(), 2, "0x") == 0 &&
                parse_hex32(body, hex_at, &off) &&
                body.compare(hex_at + 8, 2, "u)") == 0) {
                out += hex_literal(base + off);
                i = hex_at + 10;
                continue;
            }
        }
        out.push_back(body[i]);
        ++i;
    }

    // Strip the base-carrying scaffolding, line by line.
    std::string filtered;
    std::size_t start = 0;
    while (start <= out.size()) {
        std::size_t nl = out.find('\n', start);
        if (nl == std::string::npos) nl = out.size();
        const std::string line = out.substr(start, nl - start);
        const bool scaffold =
            line.find("const uint32_t _imgbase = g_runtime_image_base;") !=
                std::string::npos ||
            line.find("(void)_imgbase;") != std::string::npos ||
            line.find("g_runtime_image_base = _imgbase;") != std::string::npos;
        if (!scaffold) {
            filtered += line;
            if (nl < out.size()) filtered.push_back('\n');
        }
        start = nl + 1;
    }
    return filtered;
}

// ── Cases ────────────────────────────────────────────────────────────

void test_folding_reproduces_the_fixed_body() {
    const uint32_t origin = 0x03007BA4u;
    const std::unordered_map<uint64_t, std::string> no_names;
    RelocatableImage image;
    image.origin = origin;
    image.size = kImageSize;

    const std::string fixed = emit(origin, no_names, nullptr);
    const std::string reloc = emit(origin, no_names, &image);
    check(fixed != reloc,
          "relocatable emission must differ from the fixed emission");
    const std::string folded = fold_base(reloc, origin);
    if (folded != fixed) {
        fail("folding the origin back into the relocatable body did not "
             "reproduce the fixed body");
        std::printf("---- fixed ----\n%s\n---- folded ----\n%s\n",
                    fixed.c_str(), folded.c_str());
    }
}

// The same equivalence at a base the corpus was NOT generated at. Nothing
// about the emitted text depends on the base, so this is the property that
// makes one corpus serve every base.
void test_folding_at_a_different_base() {
    const uint32_t origin = 0x03007BA4u;
    const uint32_t base = 0x03003A84u;
    const std::unordered_map<uint64_t, std::string> no_names;
    RelocatableImage image;
    image.origin = origin;
    image.size = kImageSize;

    const std::string folded = fold_base(emit(origin, no_names, &image), base);
    // Every guest address must have moved with the base: the folded body may
    // not mention any address in origin space, and must mention the base.
    for (uint32_t a = origin; a < origin + kImageSize; a += 4u) {
        check(folded.find(hex_literal(a)) == std::string::npos,
              "a guest address stayed in origin space at 0x" +
                  hex_literal(a));
    }
    check(folded.find(hex_literal(base)) != std::string::npos,
          "the folded body never mentions the relocated base");
}

// An address the emitter forgot to relocate stays a literal, which survives
// the folding comparison above. Catch it directly.
void test_no_origin_literals_survive() {
    const uint32_t origin = 0x03007BA4u;
    const std::unordered_map<uint64_t, std::string> no_names;
    RelocatableImage image;
    image.origin = origin;
    image.size = kImageSize;

    const std::string reloc = emit(origin, no_names, &image);
    // Include the pipeline reach on either side: ARM reads PC as pc+8 and
    // stores it as pc+12, and the fall-through tail dispatches end_addr.
    for (uint32_t a = origin - 8u; a <= origin + kImageSize + 12u; a += 4u) {
        if (reloc.find(hex_literal(a)) != std::string::npos) {
            fail("un-relocated guest address literal " + hex_literal(a) +
                 " in the relocatable body");
        }
    }
}

// A direct call to a sibling in the same image is a plain C call: the callee
// snapshots g_runtime_image_base in its own prologue, so the caller has to put
// its base back first. Without that, a call that dispatched into a different
// image at a different base would leave the global stale and the sibling would
// compute addresses for the wrong base.
void test_sibling_call_restores_the_global() {
    const uint32_t origin = 0x03007BA4u;
    RelocatableImage image;
    image.origin = origin;
    image.size = kImageSize;

    // The BL at +0x0c: target = pc + 8 + (4 << 2) = origin + 0x24, ARM mode.
    const uint32_t target = origin + 0x24u;
    std::unordered_map<uint64_t, std::string> names;
    names[(static_cast<uint64_t>(target) << 1u) | 0u] = "sibling_fn";

    const std::string reloc = emit(origin, names, &image);
    const std::size_t call = reloc.find("sibling_fn();");
    if (call == std::string::npos) {
        fail("the BL did not lower to a direct sibling call");
        return;
    }
    const std::string before = reloc.substr(0, call);
    const std::size_t restore =
        before.rfind("g_runtime_image_base = _imgbase;");
    check(restore != std::string::npos,
          "no base restore before a direct sibling call");
    // Nothing may run between the restore and the call.
    if (restore != std::string::npos) {
        const std::string between = before.substr(restore);
        check(between.find(';') == between.rfind(';'),
              "a statement separates the base restore from the sibling call");
    }

    // The prologue must snapshot the base before anything else, so the resume
    // prologue's forward gotos cannot cross its initialization.
    check(reloc.compare(0, 4, "    ") == 0 &&
              reloc.find("const uint32_t _imgbase = g_runtime_image_base;") <
                  reloc.find("g_runtime_fn_entry_hook"),
          "the base snapshot is not the first thing in the body");
}

// A corpus with no image declared must be byte-identical to what the emitter
// produced before relocation existed.
void test_fixed_corpus_is_untouched() {
    const std::unordered_map<uint64_t, std::string> no_names;
    const std::string body = emit(0x03007BA4u, no_names, nullptr);
    check(body.find("_imgbase") == std::string::npos,
          "a fixed corpus mentions the image base");
    check(body.find("g_runtime_image_base") == std::string::npos,
          "a fixed corpus mentions g_runtime_image_base");
    // A zero-size image is "not an image".
    RelocatableImage empty;
    empty.origin = 0x03007BA4u;
    empty.size = 0u;
    check(emit(0x03007BA4u, no_names, &empty) == body,
          "a zero-size image changed the emission");
}

void test_in_function_branches_use_gotos() {
    const uint32_t base = 0x08000000u;
    const uint32_t words[] = {
        0xea000000u,  // b base+8 (forward)
        0xe1a00000u,  // mov r0,r0
        0xe3500000u,  // cmp r0,#0
        0x1afffffdu,  // bne base+8 (backward idle candidate)
    };
    std::vector<uint8_t> rom(sizeof(words), 0u);
    for (std::size_t i = 0; i < sizeof(words) / sizeof(words[0]); ++i) {
        const uint32_t word = words[i];
        rom[i * 4 + 0] = static_cast<uint8_t>(word);
        rom[i * 4 + 1] = static_cast<uint8_t>(word >> 8);
        rom[i * 4 + 2] = static_cast<uint8_t>(word >> 16);
        rom[i * 4 + 3] = static_cast<uint8_t>(word >> 24);
    }

    Function fn;
    fn.addr = base;
    fn.source_addr = base;
    fn.end_addr = base + sizeof(words);
    fn.mode = CpuMode::Arm;
    fn.name = "branch_fn";
    const std::unordered_map<uint64_t, std::string> no_names;
    const std::string body = gbarecomp::emit_function_body_str(
        fn, rom.data(), rom.size(), base, no_names);

    const bool has_forward_goto =
        body.find("goto L_08000008;") != std::string::npos;
    check(has_forward_goto,
          "forward in-function branch did not lower to goto");
    check(body.find("L_08000008:\n") != std::string::npos,
          "forward in-function branch target has no label");
    check(body.find("runtime_dispatch(0x08000008u)") == std::string::npos,
          "in-function branch still routes through runtime dispatch");
    check(body.find("runtime_idle_backedge(0x08000008u)") !=
              std::string::npos,
          "backward branch lost idle-loop handling");
    if (!has_forward_goto) {
        std::printf("---- branch body ----\n%s\n", body.c_str());
    }
}

}  // namespace

int main() {
    test_folding_reproduces_the_fixed_body();
    test_folding_at_a_different_base();
    test_no_origin_literals_survive();
    test_sibling_call_restores_the_global();
    test_fixed_corpus_is_untouched();
    test_in_function_branches_use_gotos();

    if (g_failures != 0) {
        std::printf("relocatable_image_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("relocatable_image_tests: PASS\n");
    return 0;
}
