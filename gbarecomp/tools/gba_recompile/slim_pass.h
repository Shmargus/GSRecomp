// slim_pass.h -- gba_recompile --slim.
//
// Rewrites fixed multi-call sequences in emitted recompiled*.cpp text into
// one call to a combined runtime helper whose body is that same sequence
// (runtime_arm.h, "Combined per-instruction helpers"). Same behaviour, about
// half the machine code. A line-for-line port of the game repository's
// tools/slim_corpus.py, which it must match byte for byte.
#pragma once

#include <map>
#include <string>

namespace gbarecomp {

// Rewrite one file's text ('\n' line ends). Adds each rule's count to
// `counts` when given.
std::string slim_text(const std::string& text,
                      std::map<std::string, long>* counts = nullptr);

// Rewrite every recompiled*.cpp directly in `dir` in place. Returns false
// (with `error`) if a file cannot be read or written.
bool slim_directory(const std::string& dir, std::map<std::string, long>* counts,
                    std::string* error);

}  // namespace gbarecomp
