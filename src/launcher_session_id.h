// Pure session-log naming policy shared by the root launcher and its tests.
#pragma once

#include <string>

namespace gsr {

// Return the user-facing identifier for a launcher session log path. The
// extension is removed, while the `session_` prefix is retained so the value
// can be pasted directly into the existing log filename convention.
inline std::wstring session_log_id_from_path(const std::wstring& path) {
    const std::size_t filename_start = path.find_last_of(L"\\/");
    const std::wstring filename = filename_start == std::wstring::npos
        ? path : path.substr(filename_start + 1);
    constexpr wchar_t extension[] = L".log";
    if (filename.size() <= std::size(extension) - 1 ||
        filename.compare(filename.size() - (std::size(extension) - 1),
                         std::size(extension) - 1, extension) != 0) {
        return {};
    }
    return filename.substr(0, filename.size() - (std::size(extension) - 1));
}

}  // namespace gsr
