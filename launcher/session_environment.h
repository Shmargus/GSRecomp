// The game's environment variables that the Windows and Linux sessions set
// the same way. Each session fills in what differs between the platforms
// (a path, the Enhanced Options / graphics-card choice) and applies the
// resulting plan to the child its own way. Plain std::string on purpose: no
// Win32 or POSIX types, so both sessions can include it.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "collection_settings.h"

namespace gsr {

// Variables to set, and variables to remove so a value inherited from the
// shell cannot reach the game. A name is in at most one of the two.
struct ChildEnvironmentPlan {
    std::vector<std::pair<std::string, std::string>> set;
    std::vector<std::string> unset;
    // Names of the test variables that were set (for the session log line).
    std::string enabled_tests;

    void put(std::string name, std::string value) {
        forget(name);
        set.emplace_back(std::move(name), std::move(value));
    }
    void remove(const std::string& name) {
        forget(name);
        unset.push_back(name);
    }

private:
    void forget(const std::string& name) {
        set.erase(std::remove_if(set.begin(), set.end(),
                                 [&](const auto& entry) { return entry.first == name; }),
                  set.end());
        unset.erase(std::remove(unset.begin(), unset.end(), name), unset.end());
    }
};

// What differs between the platforms (or between a player and a developer
// launch); everything else in add_common_environment is the same for all.
struct CommonEnvironment {
    std::string state_base;       // GBARECOMP_STATE_BASE; empty leaves it unset
    std::string heal_cache;       // GBARECOMP_HEAL_CACHE
    std::string heal_toolchain;   // bundled g++ folder; empty when there is none
    std::optional<bool> save_import;  // TLA_GS_SAVE_IMPORT; empty on The Broken Seal
    bool enhanced_options = true; // experimental fixes, room buffer, host effects
    bool gpu_field = true;        // GSR_GPU_FIELD
    bool gpu_field_only = true;   // GSR_GPU_FIELD_ONLY
};

inline void add_common_environment(ChildEnvironmentPlan& plan, const CommonEnvironment& c) {
    if (!c.state_base.empty()) plan.put("GBARECOMP_STATE_BASE", c.state_base);
    // Golden Sun's Camelot intro overflows the Windows host stack with the
    // present-in-place path; use the frame-boundary unwind path everywhere.
    plan.put("GBARECOMP_PRESENT_IN_PLACE", "0");
    if (c.save_import) plan.put("TLA_GS_SAVE_IMPORT", *c.save_import ? "1" : "0");
    // The canonical GBA left/right Direct Sound buses, and the removed
    // experimental audio paths off for older installed engines.
    plan.put("GBARECOMP_AUDIO_STEREO", "1");
    plan.put("GBARECOMP_AUDIO_NATIVE", "0");
    plan.put("GBARECOMP_AUDIO_SHADOW", "0");
    plan.put("GBARECOMP_TURBO_AUDIO", "0");
    plan.put("GSR_MOD_FIELD_TEST", "0");
    plan.put("GBARECOMP_HEAL_CACHE", c.heal_cache);
    if (!c.heal_toolchain.empty()) plan.put("GBARECOMP_HEAL_TOOLCHAIN", c.heal_toolchain);
    plan.put("GBARECOMP_EXPERIMENTAL_FIXES", c.enhanced_options ? "1" : "0");
    if (c.enhanced_options) {
        plan.put("GSR_ROOM_BUFFER_RENDER", "1");
        plan.put("GSR_HOST_EFFECTS", "1");
    } else {
        plan.remove("GSR_ROOM_BUFFER_RENDER");
        plan.remove("GSR_HOST_EFFECTS");
    }
    plan.put("GSR_GPU_FIELD", c.gpu_field ? "1" : "0");
    plan.put("GSR_GPU_FIELD_ONLY", c.gpu_field_only ? "1" : "0");
    // Every launch: F12 saves the last 2 seconds, F1 > Video offers the screen
    // filters, and the unpacker catcher is armed (it costs nothing until the
    // Sol Sanctum fault).
    plan.put("GSR_FRAME_REWIND", "1");
    plan.put("GSR_SCREEN_FILTERS", "1");
    plan.put("GSR_UNPACKER_CATCH", "1");
}

// One answer per test option: true sets it, false removes it, no value
// leaves whatever the launcher inherited alone.
using TestOptionState = std::function<std::optional<bool>(const char* environment)>;

// Applies the launcher's test options (collection_settings.h test_options).
// The sprite recorder also turns on the OAM-shadow write printout. The host
// profiler's value is the file it writes to; empty means a plain "1".
inline void add_test_environment(ChildEnvironmentPlan& plan, const TestOptionState& state,
                                 const std::string& host_prof_file) {
    const auto apply = [&](const char* name, std::optional<bool> selected,
                           const std::string& value) {
        if (!selected) return;
        if (*selected) {
            plan.put(name, value);
            if (!plan.enabled_tests.empty()) plan.enabled_tests += ",";
            plan.enabled_tests += name;
        } else {
            plan.remove(name);
        }
    };
    for (const auto& option : gsr_collection::test_options) {
        const bool host_prof = std::strcmp(option.environment, "GBARECOMP_HOST_PROF") == 0;
        apply(option.environment, state(option.environment),
              host_prof && !host_prof_file.empty() ? host_prof_file : "1");
    }
    apply("GSR_OAM_SHADOW_TRACE", state("GSR_OBJ_RECORD"), "1");
}

// The BIOS logs have no launcher box any more.
inline constexpr const char* bios_log_variables[] = {
    "GBARECOMP_SWI_LOG", "GBARECOMP_BIOS_PC_LOG", "GBARECOMP_BIOS_READ_LOG"};

// Test variables whose launcher box was retired. Never part of a launch: the
// Windows session removes them from the child so a value inherited from a
// shell cannot turn one on.
inline constexpr const char* retired_test_variables[] = {
    "GSR_RAM_CHURN_PROBE", "GSR_TEXT_RECORD", "GBARECOMP_HEADROOM_PROBE",
    "GBARECOMP_FN_TRACER", "GBARECOMP_VRAM_MAP_TRACE", "GSR_AUTO_CAPTURE",
    "GSR_BATTLE_BG1_RECORD", "GSR_ROOM_BUFFER"};

template <std::size_t N>
inline void remove_variables(ChildEnvironmentPlan& plan, const char* const (&names)[N]) {
    for (const char* name : names) plan.remove(name);
}

}  // namespace gsr
