// Pure root-launcher policy for the optional WIDE-01 diagnostics.
// No Win32/UI or runtime dependency.
#pragma once

#include <string>

namespace gsr {

struct LauncherWidescreenDiagnosticsPolicy {
    bool enabled = false;
    const wchar_t* environment_value = L"0";
};

struct LauncherInputRecordPolicy {
    bool enabled = false;
    bool explicit_path = false;
    bool replay_conflict = false;
    std::wstring path;
};

inline std::wstring automatic_input_record_path(const std::wstring& log_path,
                                                unsigned collision_index = 0) {
    if (log_path.empty()) return {};
    std::wstring path = log_path;
    const std::size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || path.substr(dot) != L".log") return {};
    path.resize(dot);
    if (collision_index != 0)
        path += L"_" + std::to_wstring(collision_index);
    path += L".input";
    return path;
}

template <typename Exists>
inline std::wstring choose_unique_input_record_path(
    const std::wstring& log_path, Exists&& exists,
    unsigned max_collisions = 1000) {
    for (unsigned suffix = 0; suffix <= max_collisions; ++suffix) {
        const std::wstring candidate =
            automatic_input_record_path(log_path, suffix);
        if (!candidate.empty() && !exists(candidate)) return candidate;
    }
    return {};
}

// Requested acceptance traces are opt-in per launcher session, never a
// persistent normal-gameplay setting.
constexpr bool launcher_widescreen_diagnostics_default() {
    return false;
}

// The launcher owns this choice. Always return an explicit child value so an
// inherited GBARECOMP_VRAM_MAP_TRACE cannot silently enable a normal launch.
constexpr LauncherWidescreenDiagnosticsPolicy
resolve_launcher_widescreen_diagnostics_policy(bool requested) {
    return requested
        ? LauncherWidescreenDiagnosticsPolicy{true, L"1"}
        : LauncherWidescreenDiagnosticsPolicy{false, L"0"};
}

// Diagnostics automatically record input when no replay is active. An
// explicitly supplied record path wins when it does not conflict with replay;
// path existence/creation is performed by the Windows launcher.
inline LauncherInputRecordPolicy resolve_launcher_input_record_policy(
    bool diagnostics_enabled, const std::wstring& explicit_record_path,
    bool replay_requested, const std::wstring& automatic_path) {
    LauncherInputRecordPolicy result{};
    if (replay_requested) {
        result.replay_conflict = !explicit_record_path.empty();
        return result;
    }
    if (!explicit_record_path.empty()) {
        result.enabled = true;
        result.explicit_path = true;
        result.path = explicit_record_path;
    } else if (diagnostics_enabled) {
        result.enabled = !automatic_path.empty();
        result.path = automatic_path;
    }
    return result;
}

}  // namespace gsr
