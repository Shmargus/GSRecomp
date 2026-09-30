#pragma once

#include <cstddef>
#include <string>

namespace gsr {

struct BlockTimingStats {
    std::size_t merged = 0, kept = 0, dropped_boundary = 0, kept_boundary = 0;
    std::size_t ticks = 0, entry_labels = 0, mem_cycles_inline = 0;
    std::size_t mem_cycles_kept = 0, preludes = 0, cond_inline = 0;
};

// One generated .cpp file through the block-timing rewrite
// (tools/block_timing.py). False when a file that needs the inline helpers
// has no `#include "recompiled.h"` to put them after.
bool block_timing_transform(const std::string& text, std::string* out,
                            BlockTimingStats& stats);

}  // namespace gsr
