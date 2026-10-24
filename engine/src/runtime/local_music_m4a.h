// local_music_m4a.h — local recordings (host_local_music) standing in for
// one m4a ("Sappy") music player's songs. The game supplies the player's
// addresses, its song-header -> recording table and the two places where
// the driver saturates a voice's left and right gain. While a recording
// plays, those gains become 0: the song still runs (timing, fades, channel
// ownership) but is silent. Songs without a loaded recording are untouched.
// The Music mod option (game_options.ini MusicMod) is read live, so F1 can
// switch it mid-song. Header only; each game keeps one Replacement.
#pragma once

#include <cstddef>
#include <cstdint>

#include "game_options.h"
#include "host_window.h"
#include "runtime_arm.h"

namespace gbarecomp::m4a_music {

// One gain saturation: a conditional branch, the outcome of it that reaches
// `MOVS Rd,#255`, and that MOVS. Configured as a conditional-branch and a
// Thumb ALU immediate override.
struct GainSite {
    std::uint32_t branch_pc;
    std::uint32_t decision;
    std::uint32_t movs_pc;
};

// One game song and the album track that replaces it (host_window.h: plus
// i * kLocalMusicFolderTracks for the i-th music folder).
struct Recording {
    std::uint32_t header;  // the song's m4a header in the ROM
    unsigned track;
};

struct Player {
    std::uint32_t info;          // MusicPlayerInfo
    std::uint32_t tracks;        // its MusicPlayerTrack array
    std::uint32_t track_count;
    std::uint32_t track_bytes;
    std::uint32_t main_entry;    // the player's main routine, entered with R0 = info
    std::uint32_t header_store;  // the start routine's accepted songHeader store
    GainSite left, right;        // in the gain helper; track in R5
};

class Replacement {
public:
    // `songs` is the game's table (count entries), kept for the program's life.
    constexpr Replacement(const Player& player, const Recording* songs, std::size_t count)
        : player_(player), songs_(songs), song_count_(count) {}

    // Music mod on (launcher cog, its first-run question or F1 > Audio).
    static bool enabled() { return game_option_get(kOptionMusicMod, 0) != 0; }

    // Function-entry observer: the player's main routine runs once a frame.
    void on_entry(std::uint32_t pc) {
        if (pc == player_.main_entry && g_cpu.R[0] == player_.info) update();
    }

    // Write observer. The store is reached only after the start routine
    // accepts the song; other players (effects) never restart or stop the
    // replacement.
    void header_write(std::uint32_t pc, std::uint32_t address,
                      std::uint32_t header, std::uint32_t width) {
        // The game's write hook calls this for every store: test the store
        // before reading the option.
        if (pc != player_.header_store || address != player_.info || width != 4u ||
            !enabled())
            return;
        player_state(header, 0, 0, 0, true);
    }

    // Conditional-branch override: steer an owned voice to the MOVS #255.
    bool branch(std::uint32_t pc, std::uint32_t track, std::uint32_t* decision) const {
        if (!decision || !mute(track)) return false;
        if (pc == player_.left.branch_pc) *decision = player_.left.decision;
        else if (pc == player_.right.branch_pc) *decision = player_.right.decision;
        else return false;
        return true;
    }

    // Thumb ALU immediate override: that MOVS then stores 0. The original
    // store, channel ownership and timing remain.
    bool immediate(std::uint32_t pc, std::uint32_t track, std::uint32_t* value) const {
        if (!value || (pc != player_.left.movs_pc && pc != player_.right.movs_pc) ||
            !mute(track))
            return false;
        *value = 0;
        return true;
    }

    // Re-read the player; `restart` after a savestate load. Switching the
    // mod off mid-song stops the recording and brings the game's song back.
    void update(bool restart = false) {
        const bool was_active = active_;
        if (enabled()) {
            player_state(bus_read_u32(player_.info), bus_read_u32(player_.info + 4),
                         bus_read_u16(player_.info + 0x24),
                         bus_read_u16(player_.info + 0x28), restart);
        } else if (active_) {
            active_ = false;
            local_music_playback(false, false, 0.0f, false);
        }
        if (was_active == active_ && !(restart && active_)) return;
        // The main routine recalculates an active track's voice gains when its
        // dirty bits 0x03 are set; set them so existing voices fall silent when
        // a recording takes over, or come back when it stops.
        for (std::uint32_t i = 0; i < player_.track_count; ++i) {
            const std::uint32_t track = player_.tracks + i * player_.track_bytes;
            const std::uint8_t flags = bus_read_u8(track);
            if (flags & 0x80u) bus_write_u8(track, flags | 3u);
        }
    }

private:
    bool owns_track(std::uint32_t track) const {
        return track >= player_.tracks &&
               track - player_.tracks < player_.track_count * player_.track_bytes &&
               (track - player_.tracks) % player_.track_bytes == 0;
    }

    bool mute(std::uint32_t track) const { return active_ && owns_track(track); }

    // The album track for a song, 0 (keep the game's music) when none.
    unsigned recording_for(std::uint32_t header) const {
        for (std::size_t i = 0; i < song_count_; ++i)
            if (songs_[i].header == header) return songs_[i].track;
        return 0;
    }

    // status bit 31: paused. A fade scales the recording like the song.
    void player_state(std::uint32_t header, std::uint32_t status,
                      std::uint16_t fade_period, std::uint16_t fade_volume,
                      bool restart) {
        active_ = local_music_select(recording_for(header));
        const float gain = fade_period ? float(fade_volume & 0xFFFCu) / 256.0f : 1.0f;
        local_music_playback(active_, (status & 0x80000000u) != 0, gain, restart);
    }

    Player player_;
    const Recording* songs_;
    std::size_t song_count_;
    bool active_ = false;
};

}  // namespace gbarecomp::m4a_music
