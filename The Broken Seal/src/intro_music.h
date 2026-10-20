#pragma once

#include <cstdint>
#include "env_flag.h"
#include "host_window.h"
#include "local_music_tracks.h"

// Guest sequencing continues normally; only this player's final voice gains
// are silenced while its preloaded local recording is selected. Files that
// failed startup loading leave the game's original voice gains intact.
namespace gsr::intro_music {
constexpr std::uint32_t kPlayer = 0x02004290u;
constexpr std::uint32_t kSongHeader = recordings[0].header; // title, song0x46
constexpr std::uint32_t kTracks = 0x020044E0u;
constexpr std::uint32_t kTrackCount = 10u;
constexpr std::uint32_t kTrackBytes = 0x50u;
constexpr std::uint32_t kPlayerMain = 0x080F9C90u;
constexpr std::uint32_t kHeaderWrite = 0x080FAAA4u;
inline bool active = false;

inline bool enabled() {
    static const bool value = gbarecomp::env_flag("GSR_INTRO_MUSIC");
    return value;
}

inline bool owns_track(std::uint32_t track) {
    return track >= kTracks && track - kTracks < kTrackCount * kTrackBytes &&
           (track - kTracks) % kTrackBytes == 0;
}

inline void player_state(std::uint32_t header, std::uint32_t status,
                         std::uint16_t fade_period, std::uint16_t fade_volume,
                         bool restart = false) {
    if (!enabled()) return;
    const unsigned recording = recording_for(header);
    active = gbarecomp::local_music_select(recording);
    const float gain = fade_period
        ? float(fade_volume & 0xFFFCu) / 256.0f : 1.0f;
    gbarecomp::local_music_playback(active, (status & 0x80000000u) != 0,
                                   gain, restart);
}

inline void header_write(std::uint32_t pc, std::uint32_t address,
                         std::uint32_t header, std::uint32_t width) {
    if (!enabled() || pc != kHeaderWrite || address != kPlayer || width != 4u)
        return;
    // This store is reached only after MPlayStart accepts the song. Other
    // players (effects) never restart or stop the replacement.
    player_state(header, 0, 0, 0, true);
}

inline bool mute(std::uint32_t track) {
    return active && owns_track(track);
}

inline int branch(std::uint32_t pc, std::uint32_t track,
                   std::uint32_t* decision) {
    if (!decision || (pc != 0x080F9F50u && pc != 0x080F9F64u) || !mute(track))
        return 0;
    // Fall through the original saturation MOVS; its immediate hook below
    // supplies zero. The original store, channel ownership and timing remain.
    *decision = 0;
    return 1;
}

inline int immediate(std::uint32_t pc, std::uint32_t track,
                      std::uint32_t* value) {
    if (!value || (pc != 0x080F9F52u && pc != 0x080F9F66u) || !mute(track))
        return 0;
    *value = 0;
    return 1;
}
} // namespace gsr::intro_music
