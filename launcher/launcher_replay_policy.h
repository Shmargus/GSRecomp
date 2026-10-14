#pragma once

#include <cstdint>
#include <string>

namespace gsr {

// Quote one argument using the CommandLineToArgvW/CreateProcessW rules.
// The launcher only uses this for developer-provided replay paths.
inline std::wstring quote_windows_command_arg(const std::wstring& value) {
    std::wstring quoted;
    quoted.reserve(value.size() + 2);
    quoted.push_back(L'"');

    std::size_t backslashes = 0;
    for (const wchar_t c : value) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
            backslashes = 0;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(c);
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

inline bool parse_replay_frame_budget(const std::wstring& value,
                                      std::uint32_t* out) {
    if (!out || value.empty()) return false;
    std::uint64_t parsed = 0;
    for (const wchar_t c : value) {
        if (c < L'0' || c > L'9') return false;
        const std::uint64_t digit = static_cast<std::uint64_t>(c - L'0');
        if (parsed > (0x7fffffffULL - digit) / 10ULL) return false;
        parsed = parsed * 10 + digit;
    }
    if (parsed == 0) return false;
    *out = static_cast<std::uint32_t>(parsed);
    return true;
}

// Add the optional developer replay controls to an otherwise normal game
// command. Empty values leave the faithful launcher command unchanged.
inline bool append_developer_replay_arguments(
    std::wstring& command, const std::wstring& load_state,
    const std::wstring& frame_budget) {
    if (load_state.empty()) return false;
    command += L" --load-state " + quote_windows_command_arg(load_state);

    std::uint32_t frames = 0;
    if (parse_replay_frame_budget(frame_budget, &frames)) {
        command += L" --window --frames " + std::to_wstring(frames);
    }
    return true;
}

}  // namespace gsr
