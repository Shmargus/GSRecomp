// Shared input-recording rule for Windows, Linux and Android sessions.
// The session opens the file.
#pragma once

#include <string>

namespace gsr {

struct LauncherInputRecordPolicy {
    bool enabled = false;
    bool replay_conflict = false;
    std::wstring path;
};

// Recording needs a GBARECOMP_INPUT_RECORD path. Report an error if
// replay is requested too.
inline LauncherInputRecordPolicy resolve_launcher_input_record_policy(
    const std::wstring& explicit_record_path, bool replay_requested) {
    LauncherInputRecordPolicy result{};
    if (replay_requested) {
        result.replay_conflict = !explicit_record_path.empty();
        return result;
    }
    if (!explicit_record_path.empty()) {
        result.enabled = true;
        result.path = explicit_record_path;
    }
    return result;
}

}  // namespace gsr
