// file_replace.h — replace a file's contents without ever losing the old
// copy. Shared by the battery-save flush and save-state slots.

#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace gba {

// Write `len` bytes to `path` through a sibling "<path>.tmp": the temp file
// is written and closed (every write and the final flush checked), then
// renamed over `path` in one step. The destination is never deleted or
// written in place, so on any failure it keeps its previous contents.
// A temp file that failed to write is removed; a complete one whose rename
// failed is kept as a recovery copy and named in `err`. Creates the parent
// directory if needed.
bool replace_file(const std::filesystem::path& path, const void* data,
                  std::size_t len, std::string* err);

}  // namespace gba
