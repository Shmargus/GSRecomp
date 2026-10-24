// host_local_music.h — host-side "local music" player (music mods): WAV or
// FLAC recordings from GBARECOMP_MUSIC_DIR's folders ("<disc>-<NN> <title>"),
// streamed beside the game audio and looping where each folder's loops.txt
// says (MUSIC_MODS.md). Owned by host_window.cpp's
// audio Backend; the public local_music_* entry points in host_window.h
// forward here. SDL2 builds only.
#pragma once

#if defined(GBARECOMP_HAVE_SDL2)

#include <SDL.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace gbarecomp {

// One loops.txt entry, in the file's own sample frames: play to `from`, then
// continue at `to`. from == 0 (no entry) repeats the whole file; `once` plays
// it a single time.
struct LocalMusicLoop {
    std::uint64_t from = 0;
    std::uint64_t to = 0;
    bool once = false;
};

class LocalMusicPlayer {
public:
    // The mutex is shared with the audio callback and the bridge; the player
    // never creates or destroys it. Call once the device is open. Without
    // attach() (no audio device) every locking method is a no-op.
    void attach(SDL_mutex* audio_mtx, int audio_channels);

    // `can_stream` is the caller's "bridge up and device open" condition.
    void preload_directory(const char* directory, bool can_stream,
                           const SDL_AudioSpec& device, double preroll_ms);

    // Audio callback side. mix() expects audio_mtx to be held by the caller.
    bool has_prefix() const;
    void mix(int16_t* output, int frames);

    // Game side; each takes audio_mtx itself.
    bool ready();
    // Any recording found at startup (fixed after preload_directory).
    bool available() const { return !music_bank_.tracks.empty(); }
    bool select(unsigned track);
    void playback(bool active, bool paused, float gain, bool restart);
    void set_host_gain(float gain);
    void set_turbo_muted(bool on);
    void set_host_paused(bool paused);

    // Stop the worker (epoch bump + join). Call before closing the device.
    void shutdown();
    std::uint64_t underrun_frames() const;

private:
    struct LocalMusic {
        const std::vector<int16_t>* prefix = nullptr;
        std::size_t prefix_cursor = 0;
        std::size_t ring_read = 0;
        std::size_t ring_frames = 0;
        std::uint64_t generation = 0;
        float gain = 1.0f;         // caller gain times host volume
        float host_gain = 1.0f;
        bool ready = false;
        bool active = false;
        bool guest_paused = false;
        bool host_paused = false;
        bool turbo_muted = false;
        bool worker_failed = false;
        bool complete = false;     // prefix holds the whole playback
        bool ended = false;        // a play-once recording has finished
        std::uint64_t underrun_frames = 0;
    };
    struct MusicTrack {
        std::string path;
        LocalMusicLoop loop;
        std::vector<int16_t> prefix;
        bool complete = false;
    };
    struct MusicBank {
        unsigned current_track = 0;
        std::map<unsigned, MusicTrack> tracks;
        std::vector<int16_t> ring;
        std::size_t ring_capacity_frames = 0;
        SDL_AudioSpec device{};
        double preroll_ms = 0.0;
        std::atomic<std::uint64_t> epoch{0};
        std::thread worker;
        bool worker_available = false;
        bool stop = false;
    };

    bool prepare_music_track(const std::string& path, const LocalMusicLoop& loop,
                             MusicTrack& track);
    void music_worker();

    SDL_mutex* audio_mtx_ = nullptr;
    int audio_channels_ = 1;
    LocalMusic local_music_;
    MusicBank music_bank_;
};

}  // namespace gbarecomp

#endif  // GBARECOMP_HAVE_SDL2
