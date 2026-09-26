// arm_codegen.h — IR → C code emission.
//
// Lowers each decoded `Instr` to a sequence of C statements that
// operate on the recomp ABI (g_cpu / bus_read_* / bus_write_* /
// arm_shift_* / arm_set_* / runtime_dispatch — see runtime_arm.h).
//
// The output is meant to be dropped directly into a `void fname(void)`
// body emitted by tools/gba_recompile/main.cpp. Each call to
// emit_instr returns a block of C source that:
//   - wraps in `if (arm_cond_passes(...))` for non-AL conditions,
//   - reads operands from g_cpu (with PC = pc+8 / pc+4 baked in
//     statically when R15 is read in operand position),
//   - writes results to g_cpu and (when the instruction writes PC)
//     emits a trailing `return;` so the runtime exec loop re-enters
//     runtime_dispatch with the new PC.
//
// PRINCIPLES.md "Interpreter is informative, never load-bearing":
// the interpreter is the semantic reference but is NEVER called from
// generated code. If emit_instr can't lower an op yet it returns
// `not_implemented = true` and the caller emits a
// `runtime_unimplemented_op(...)` abort — never an interpreter
// fallback.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "arm_ir.h"

namespace armv4t {

struct CodegenResult {
    std::string text;          // emitted C source for the block
    bool not_implemented;      // true if at least one Instr fell through
    std::size_t emitted_count;
};

// Context passed to per-instruction emission. The function-name map
// lets direct B/BL targets lower to a C function call when the
// target is known to be a recompiled function in the same dispatch
// table; unknown targets fall back to runtime_dispatch.
struct CodegenCtx {
    // Key is (addr << 1) | thumb_bit. Direct B/BL preserves the
    // current instruction-set state, so codegen can resolve the
    // correct same-mode callee even when ARM and THUMB entries share
    // the same numeric address.
    const std::unordered_map<uint64_t, std::string>* names_by_key = nullptr;
    uint32_t current_function_addr = 0xFFFFFFFFu;
    uint32_t current_function_end_addr = 0xFFFFFFFFu;
    bool current_function_thumb = false;
    bool force_bx_c_return = false;

    // Stage 2 idle-loop elision. Set of back-edge branch PCs whose
    // in-function backward branch closes a statically-eligible
    // quiescent loop (no stores / calls / SWI / mode-change / PC-write
    // in the body — only DP/MOV, loads, MUL, MRS, and this single
    // conditional back-edge). For those branches emit_direct_branch
    // emits a runtime_idle_backedge() probe before the `goto` so the
    // runtime can dynamically prove the loop idle and fast-forward the
    // scheduler to the next event boundary. nullptr → no elision.
    // Populated per-function by emit_function.cpp's pre-scan.
    const std::unordered_set<uint32_t>* idle_backedge_pcs = nullptr;

    // Position-independent RAM image. A routine the game DMA-copies into a
    // relocatable working area (or onto the stack) executes at whatever base
    // its allocator handed out, so one translation must serve every base.
    // When image_size != 0 every guest address this function derives from its
    // own PC is emitted as `(<image_base_var> + offset)` — offset taken
    // against image_origin in wrapping uint32 arithmetic — instead of the
    // address the corpus was generated at. image_base_var names the local the
    // emitted prologue snapshots g_runtime_image_base into.
    //
    // Compile-time-only quantities (goto labels, temporary-name suffixes,
    // names_by_key lookups, alias/resume tokens) stay origin-based: they never
    // reach the guest, and keeping them stable keeps one corpus one corpus.
    uint32_t image_origin = 0;
    uint32_t image_size = 0;
    const char* image_base_var = "_imgbase";

    // Exact instruction PCs whose decoded THUMB data-processing immediate is
    // allowed to consult runtime_thumb_alu_immediate(). nullptr/empty keeps
    // the original compile-time literal, so games that do not explicitly opt
    // in pay no generated-code branch or callback cost.
    const std::unordered_set<uint32_t>* alu_immediate_override_pcs = nullptr;

    // Exact THUMB PC-relative literal-load PCs allowed to consult
    // runtime_thumb_literal(). nullptr/empty keeps literals immutable.
    const std::unordered_set<uint32_t>* literal_override_pcs = nullptr;

    // Exact conditional-branch PCs allowed to consult
    // runtime_conditional_branch(). The original CPSR-derived decision is
    // preserved when nullptr/empty or when the runtime callback rejects it.
    const std::unordered_set<uint32_t>* conditional_branch_override_pcs = nullptr;
};

class ArmCodegen {
public:
    // Emit C source for one decoded instruction. `not_implemented`
    // is set true if the IR shape is not yet lowered. The string
    // ends in a newline; callers may indent it as they please.
    static std::string emit_instr(const Instr& i, const CodegenCtx& ctx,
                                  bool* not_implemented);

    // Block-level helper: invoke emit_instr on every entry,
    // concatenate the result, and report whether any instruction
    // fell through.
    static CodegenResult emit_block(const std::vector<Instr>& block,
                                    const CodegenCtx& ctx);
};

}  // namespace armv4t
