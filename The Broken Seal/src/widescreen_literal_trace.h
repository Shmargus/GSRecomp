// Payload-free counters for the two reviewed Golden Sun field-list literal
// hooks. The runner owns scene authorization; this header keeps the counter
// semantics independently testable without linking the generated game corpus.
#pragma once

#include <cstdint>

namespace gsr::widescreen {

inline constexpr std::uint32_t kFieldListXUpperLiteralPc = 0x0800C6F2u;
inline constexpr std::uint32_t kFieldListYLowerLiteralPc = 0x0800C6FEu;

struct GoldenSunLiteralTraceStats {
    std::uint32_t pc = 0;
    std::uint64_t calls = 0;
    std::uint64_t overrides = 0;
    std::uint64_t gate_rejects = 0;
    std::uint64_t compare_rejects = 0;
    std::uint32_t min_original = 0xFFFFFFFFu;
    std::uint32_t max_original = 0;
};

inline constexpr int golden_sun_literal_trace_index(std::uint32_t pc) {
    if (pc == kFieldListXUpperLiteralPc) return 0;
    if (pc == kFieldListYLowerLiteralPc) return 1;
    return -1;
}

inline void golden_sun_literal_trace_call(GoldenSunLiteralTraceStats& stats,
                                          std::uint32_t original_value) {
    ++stats.calls;
    if (original_value < stats.min_original) {
        stats.min_original = original_value;
    }
    if (original_value > stats.max_original) {
        stats.max_original = original_value;
    }
}

inline void golden_sun_literal_trace_gate_reject(
    GoldenSunLiteralTraceStats& stats) {
    ++stats.gate_rejects;
}

inline void golden_sun_literal_trace_compare_reject(
    GoldenSunLiteralTraceStats& stats) {
    ++stats.compare_rejects;
}

inline void golden_sun_literal_trace_override(
    GoldenSunLiteralTraceStats& stats) {
    ++stats.overrides;
}

}  // namespace gsr::widescreen
