// arm_codegen.h: IR → C code emission.
//
// Lowers each decoded `Instr` to a sequence of C statements that
// operate on the recomp ABI (g_cpu / bus_read_* / bus_write_* /
// arm_shift_* / arm_set_* / runtime_dispatch; see runtime_arm.h).
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
// If emit_instr can't lower an instruction, it sets `not_implemented` and
// the caller emits `runtime_unimplemented_op(...)`, which aborts loudly (it
// is not recovered at run time).

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

    // Backward branches eligible for idle-loop skipping. The loop may contain
    // DP/MOV, loads, MUL and MRS, with one conditional branch back to the
    // start. emit_function.cpp finds candidates and emit_direct_branch adds
    // a `runtime_idle_backedge()` probe before the `goto`. The runtime skips
    // whole iterations only after confirming the loop is idle. Null disables it.
    const std::unordered_set<uint32_t>* idle_backedge_pcs = nullptr;

    // When image_size is nonzero, emit PC-relative guest addresses as
    // `image_base_var` plus their offset from image_origin (wrapping uint32).
    // The prologue saves g_runtime_image_base in that local so one translation
    // can run at any base, including on the guest stack.
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

    // Exact ARM data-processing PCs whose operand-2 field (bits 11:0) the
    // guest rewrites at run time ([[dynamic_operand2]]): the emitted code
    // reads the field from the live instruction word on every execution.
    const std::unordered_set<uint32_t>* dynamic_operand2_pcs = nullptr;
};

// A guest transfer that never comes back to this function (B, a computed
// jump, a BX/POP that is not a C return, a fall-through). Emitted as
// GBARECOMP_TAIL_TRANSFER (runtime_arm_types.h), a guaranteed host tail jump
// under GCC 15+: at -Og GCC does not turn `call(); return;` into a jump, so a
// guest loop that crosses generated functions without BL grew the native
// stack until it overflowed (TLA's map decompressor).
// `call` is a void C call expression without the trailing semicolon.
std::string emit_tail_transfer(const std::string& call);

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
