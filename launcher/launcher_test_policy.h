// Fresh-session defaults for the launcher's test options.
//
// The options are intentionally not persisted.  The user can opt into a
// diagnostic run from the collection launcher, while self-healing remains
// available for the normal run.  Frame rewind, the screen filters and the
// unpacker catcher are always on and are not listed here.
#pragma once

#include <string_view>

namespace gsr {

struct LauncherTestDefaults {
    bool master = false;
    bool self_heal_ram = true;
    bool cost_probe = false;
    bool host_prof = false;
    bool present_cadence = false;
    bool map_record = false;
    bool obj_record = false;
    bool effect_trace = false;
    bool unpacker_force_fault = false;
    bool unpacker_audit = false;
};

constexpr LauncherTestDefaults launcher_test_defaults() {
    return {};
}

// Developer launcher sessions always collect these three performance
// diagnostics, even when the general test-variable switch is off.
inline constexpr const char* developer_performance_options[] = {
    "GBARECOMP_COST_PROBE", "GBARECOMP_HOST_PROF", "GBARECOMP_PRESENT_CADENCE"};

inline constexpr bool is_developer_performance_option(std::string_view environment) {
    for (const char* name : developer_performance_options)
        if (environment == name) return true;
    return false;
}

}  // namespace gsr
