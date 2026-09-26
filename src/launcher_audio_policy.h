// Pure root-launcher audio environment policy. No Win32/UI dependency.
#pragma once

namespace gsr {

struct LauncherAudioPolicy {
    bool native_mp2k = false;
    bool turbo_decoupled = false;
};

// MuteDuringTurbo belongs to the runtime's saved Speed setting and has
// precedence there. The launcher only supplies the optional decoupled request.
constexpr LauncherAudioPolicy resolve_launcher_audio_policy(
    bool native_requested, bool turbo_requested, bool strict_static,
    bool mute_during_turbo) {
    const bool native = native_requested && !strict_static;
    return {native, native && turbo_requested && !mute_during_turbo};
}

}  // namespace gsr
