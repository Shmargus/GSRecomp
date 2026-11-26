// asset_picker.h: finds the user-supplied assets (BIOS, ROM, save).
//
// It looks in this order:
//   1. the command-line path,
//   2. the sidecar cache file, then
//   3. an OS file dialog (Win32 only).
//
// Each candidate is checked by size, SHA-1 and CRC32 (the BIOS and ROM
// configs already carry the SHA-1). A wrong size is a hard failure. A wrong
// hash only warns and the file is still tried, so the user can boot a
// region or revision we have not catalogued yet.
//
// The first validated pick is cached next to the executable, so end users
// do not have to type --bios / --rom on every launch.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gbarecomp {

struct AssetSpec {
    // Human-facing label, e.g. "GBA BIOS" or "Golden Sun ROM".
    const char* display_name = "asset";
    // Win32 OPENFILENAMEA filter, a double-null-terminated pair list:
    // "GBA BIOS\0*.bin;*.BIN\0All Files\0*.*\0".
    const char* dialog_filter = nullptr;
    // Title bar text for the dialog.
    const char* dialog_title = "Select a file";
    // Sidecar cache filename (lives next to the .exe), e.g. "bios.cfg".
    const char* cache_filename = "asset.cfg";
    // Hard size check, in bytes. 0 = any size accepted.
    std::size_t expected_size = 0;
    // Expected SHA-1, 40-char lowercase hex. nullptr or "" = no check.
    const char* expected_sha1 = nullptr;
    // Expected CRC32 (IEEE 802.3). 0 = no check.
    std::uint32_t expected_crc32 = 0;
};

struct AssetResult {
    bool ok = false;
    std::string path;                  // validated path
    std::vector<unsigned char> bytes;  // file contents (already read)
    std::string sha1_hex;
    std::uint32_t crc32 = 0;
    std::string warning;  // non-empty on hash mismatch (user warned, proceeding)
    std::string error;    // non-empty on hard failure (size / unreadable)
};

// Resolve an asset using the following precedence:
//   1. `argv_path` if non-empty (validate, cache, return).
//   2. Path stored in `<exe_dir>/<spec.cache_filename>` from a prior
//      successful pick (validate, refresh, return).
//   3. Win32 OPENFILENAMEA dialog (loop until cancelled or validated).
//
// `argv0` is the running executable's argv[0]. It is used to locate the
// sidecar cache. If argv0 is empty, the current working directory is the
// cache root.
AssetResult resolve_asset(const std::string& argv_path,
                          const AssetSpec& spec,
                          const std::string& argv0);

}  // namespace gbarecomp
