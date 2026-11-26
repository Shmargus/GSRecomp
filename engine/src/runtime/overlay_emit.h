// overlay_emit.h: emit the full C source for ONE runtime-recompiled
// overlay function (Stage-2 self-healing).
//
// It takes a guest PC and a contiguous code image (cart ROM or a BIOS copy)
// and:
//   1. runs the function finder for that single entry,
//   2. lowers the body with the SHARED emitter
//      (gbarecomp::emit_function_body_str), and
//   3. wraps it with the overlay prelude (#include "overlay_runtime_arm.h")
//      and the exported overlay_abi()/overlay_init()/func_XXXXXXXX symbols the
//      loader expects.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace gbarecomp {

// Returns the .c text for the overlay function at `pc` (thumb selects the
// ISA), or an empty string if the finder discovered no function entry there.
// `out_end` (optional) receives the discovered function's exclusive end
// address: the [pc, end) byte range whose CRC keys the on-disk cache.
std::string emit_overlay_c(uint32_t pc, bool thumb,
                           const uint8_t* bytes, std::size_t size,
                           uint32_t base, uint32_t* out_end = nullptr,
                           bool relocatable = false);

}  // namespace gbarecomp
