// Fresh-session defaults for the launcher's diagnostic controls.
//
// These controls are intentionally not persisted.  The user can opt into a
// diagnostic run from the launcher, while self-healing remains available for
// the normal run.
#pragma once

namespace gsr {

struct LauncherTestDefaults {
    bool master = false;
    bool self_heal_ram = true;
    bool cost_probe = false;
    bool host_prof = false;
    bool present_cadence = false;
    bool ram_churn_probe = false;
    bool map_record = false;
    bool obj_record = false;
    bool function_tracer = false;
    bool text_record = false;
    bool headroom_probe = false;
    bool vram_map_trace = false;
    bool effect_trace = false;
    bool frame_rewind = false;
    bool battle_bg1_record = false;
    bool room_buffer = false;
    bool swi_log = false;
    bool mod_field_test = false;
};

constexpr LauncherTestDefaults launcher_test_defaults() {
    return {};
}

// Self-healing is the one diagnostic safety path that remains enabled for a
// normal launch. All other test variables require the opt-in master checkbox.
constexpr bool launcher_test_variable_enabled(bool master_enabled,
                                               bool self_heal_variable,
                                               bool selected) {
    return selected && (self_heal_variable || master_enabled);
}

}  // namespace gsr
