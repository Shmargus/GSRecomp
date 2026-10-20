// host_local_music.cpp — host-side "local music" player: WAV openings streamed
// beside the game audio. Split out of host_window.cpp; behaviour unchanged.
// Compiles to nothing without SDL2, like host_window.cpp's real backend.

#include "host_local_music.h"

#if defined(GBARECOMP_HAVE_SDL2)

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace gbarecomp {

namespace {

struct WavInfo {
    std::uint16_t channels = 0;
    std::uint32_t rate = 0;
    std::uint64_t data_offset = 0;
    std::uint64_t data_bytes = 0;
};

bool rw_read_exact(SDL_RWops* rw, void* out, std::size_t size) {
    return SDL_RWread(rw, out, 1, size) == size;
}

std::uint16_t read_le16(const Uint8* p) {
    return static_cast<std::uint16_t>(p[0] | (std::uint16_t(p[1]) << 8));
}
std::uint32_t read_le32(const Uint8* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

bool parse_music_wav(SDL_RWops* rw, WavInfo& info) {
    if (!rw) return false;
    const Sint64 file_size = SDL_RWsize(rw);
    Uint8 header[12];
    if (file_size < 12 || !rw_read_exact(rw, header, sizeof(header)) ||
        std::memcmp(header, "RIFF", 4) != 0 ||
        std::memcmp(header + 8, "WAVE", 4) != 0) return false;
    const std::uint64_t riff_end = std::uint64_t(read_le32(header + 4)) + 8;
    if (riff_end > static_cast<std::uint64_t>(file_size) || riff_end < 12) return false;
    bool have_fmt = false, have_data = false;
    std::uint16_t bits = 0, encoding = 0, block_align = 0;
    while (static_cast<std::uint64_t>(SDL_RWtell(rw)) + 8 <= riff_end) {
        Uint8 chunk[8];
        if (!rw_read_exact(rw, chunk, sizeof(chunk))) return false;
        const std::uint32_t size = read_le32(chunk + 4);
        const std::uint64_t payload = static_cast<std::uint64_t>(SDL_RWtell(rw));
        const std::uint64_t end = payload + size;
        if (end < payload || end > riff_end) return false;
        if (std::memcmp(chunk, "fmt ", 4) == 0) {
            if (size < 16) return false;
            Uint8 fmt[16];
            if (!rw_read_exact(rw, fmt, sizeof(fmt))) return false;
            encoding = read_le16(fmt);
            info.channels = read_le16(fmt + 2);
            info.rate = read_le32(fmt + 4);
            block_align = read_le16(fmt + 12);
            bits = read_le16(fmt + 14);
            have_fmt = true;
        } else if (std::memcmp(chunk, "data", 4) == 0 && !have_data) {
            info.data_offset = payload;
            info.data_bytes = size;
            have_data = true;
        }
        const std::uint64_t next = end + (size & 1u);
        if (next > riff_end || SDL_RWseek(rw, static_cast<Sint64>(next), RW_SEEK_SET) < 0)
            return false;
    }
    const std::uint16_t expected_align = static_cast<std::uint16_t>(info.channels * 2u);
    return have_fmt && have_data && encoding == 1 && bits == 16 &&
           info.channels >= 1 && info.channels <= 2 && info.rate != 0 &&
           info.rate <= static_cast<std::uint32_t>(INT_MAX) &&
           block_align == expected_align && info.data_bytes >= block_align &&
           info.data_bytes % block_align == 0;
}

SDL_AudioStream* music_stream(const WavInfo& source, const SDL_AudioSpec& device) {
    return SDL_NewAudioStream(AUDIO_S16LSB, static_cast<Uint8>(source.channels),
        static_cast<int>(source.rate), AUDIO_S16SYS, device.channels, device.freq);
}

bool read_music_source_chunk(SDL_RWops* rw, const WavInfo& info,
                             std::uint64_t& offset, std::vector<Uint8>& input,
                             std::size_t max_frames) {
    const std::size_t frame_bytes = std::size_t(info.channels) * 2;
    const std::uint64_t remaining = info.data_bytes - offset;
    const std::size_t frames = std::min<std::uint64_t>(max_frames,
        remaining / frame_bytes);
    if (frames == 0) return false;
    const std::size_t bytes = frames * frame_bytes;
    input.resize(bytes);
    if (SDL_RWseek(rw, static_cast<Sint64>(info.data_offset + offset), RW_SEEK_SET) < 0 ||
        !rw_read_exact(rw, input.data(), bytes)) return false;
    offset += bytes;
    return true;
}

bool make_music_prefix(const std::string& path, const SDL_AudioSpec& device,
                       std::size_t wanted_frames, std::vector<int16_t>& prefix,
                       bool& complete, std::uint64_t& data_bytes) {
    SDL_RWops* rw = SDL_RWFromFile(path.c_str(), "rb");
    if (!rw) return false;
    std::unique_ptr<SDL_RWops, decltype(&SDL_RWclose)> rw_owner(rw, &SDL_RWclose);
    WavInfo info;
    if (!parse_music_wav(rw, info)) return false;
    data_bytes = info.data_bytes;
    SDL_AudioStream* stream = music_stream(info, device);
    if (!stream) return false;
    std::unique_ptr<SDL_AudioStream, decltype(&SDL_FreeAudioStream)> stream_owner(
        stream, &SDL_FreeAudioStream);
    const std::size_t out_frame_bytes = sizeof(int16_t) * device.channels;
    const std::size_t in_chunk_frames = std::max<std::size_t>(1,
        (std::size_t(device.samples) * info.rate + device.freq - 1) / device.freq);
    std::vector<Uint8> input, output(std::max<std::size_t>(out_frame_bytes,
        std::size_t(device.samples) * out_frame_bytes));
    std::uint64_t source_offset = 0;
    bool eof = false;
    prefix.clear();
    complete = false;
    try { prefix.reserve(wanted_frames * device.channels); }
    catch (...) { return false; }
    while (prefix.size() / device.channels < wanted_frames) {
        const int available = SDL_AudioStreamAvailable(stream);
        if (available < 0) return false;
        if (available >= static_cast<int>(out_frame_bytes)) {
            const std::size_t need = wanted_frames - prefix.size() / device.channels;
            const std::size_t take = std::min<std::size_t>(need,
                std::min<std::size_t>(output.size() / out_frame_bytes,
                                      static_cast<std::size_t>(available) / out_frame_bytes));
            const int bytes = static_cast<int>(take * out_frame_bytes);
            if (SDL_AudioStreamGet(stream, output.data(), bytes) != bytes) {
                return false;
            }
            const auto* samples = reinterpret_cast<const int16_t*>(output.data());
            prefix.insert(prefix.end(), samples, samples + take * device.channels);
            continue;
        }
        if (!eof) {
            if (source_offset < info.data_bytes) {
                if (!read_music_source_chunk(rw, info, source_offset, input, in_chunk_frames) ||
                    SDL_AudioStreamPut(stream, input.data(), static_cast<int>(input.size())) < 0) {
                    return false;
                }
            } else {
                if (SDL_AudioStreamFlush(stream) < 0) return false;
                eof = true;
            }
        } else break;
    }
    const bool reached_eof = source_offset == info.data_bytes && eof;
    if (reached_eof) {
        for (;;) {
            const int available = SDL_AudioStreamAvailable(stream);
            if (available <= 0) break;
            const std::size_t take = std::min<std::size_t>(
                output.size(), static_cast<std::size_t>(available));
            const int got = SDL_AudioStreamGet(stream, output.data(),
                                               static_cast<int>(take));
            if (got <= 0 || static_cast<std::size_t>(got) % sizeof(int16_t) != 0) {
                return false;
            }
            const std::size_t samples = static_cast<std::size_t>(got) / sizeof(int16_t);
            const auto* data = reinterpret_cast<const int16_t*>(output.data());
            prefix.insert(prefix.end(), data, data + samples);
        }
        complete = true;
    }
    return !prefix.empty() && prefix.size() % device.channels == 0;
}

bool parse_local_music_filename(const std::string& filename, unsigned& track) {
    constexpr char prefix[] = "track-";
    constexpr char suffix[] = ".wav";
    if (filename.size() <= sizeof(prefix) - 1 + sizeof(suffix) - 1 ||
        filename.compare(0, sizeof(prefix) - 1, prefix) != 0 ||
        filename.compare(filename.size() - (sizeof(suffix) - 1),
                         sizeof(suffix) - 1, suffix) != 0)
        return false;
    const std::size_t digits_begin = sizeof(prefix) - 1;
    const std::size_t digits_end = filename.size() - (sizeof(suffix) - 1);
    unsigned value = 0;
    for (std::size_t i = digits_begin; i < digits_end; ++i) {
        const char ch = filename[i];
        if (ch < '0' || ch > '9') return false;
        const unsigned digit = static_cast<unsigned>(ch - '0');
        if (value > (UINT_MAX - digit) / 10u) return false;
        value = value * 10u + digit;
    }
    if (value == 0) return false;
    track = value;
    return true;
}

}  // namespace

void LocalMusicPlayer::mix(int16_t* output, int frames) {
    auto& music = local_music_;
    if (!music.ready || !music.active || !music.prefix || frames <= 0) return;
    const std::size_t channels = static_cast<std::size_t>(audio_channels_);
    if (music.host_paused || music.guest_paused) return;
    const float gain = music.gain * music.host_gain;
    std::size_t ring_count = music.ring_frames;
    std::size_t ring_read = music.ring_read;
    const std::size_t prefix_frames = music.prefix->size() / channels;
    for (int frame = 0; frame < frames; ++frame) {
        const int16_t* source = nullptr;
        if (music.prefix_cursor < prefix_frames) {
            source = music.prefix->data() + music.prefix_cursor * channels;
            ++music.prefix_cursor;
        } else if (music.complete && prefix_frames != 0) {
            music.prefix_cursor = 1;
            source = music.prefix->data();
        } else if (ring_count != 0) {
            source = music_bank_.ring.data() + ring_read * channels;
            ring_read = (ring_read + 1) % music_bank_.ring_capacity_frames;
            --ring_count;
        }
        if (!source) {
            ++music.underrun_frames;
            continue;
        }
        if (!music.turbo_muted && gain != 0.0f) {
            const std::size_t target = static_cast<std::size_t>(frame) * channels;
            for (std::size_t channel = 0; channel < channels; ++channel) {
                const int mixed = static_cast<int>(output[target + channel]) +
                    static_cast<int>(source[channel] * gain);
                output[target + channel] = static_cast<int16_t>(
                    std::clamp(mixed, -32768, 32767));
            }
        }
    }
    music.ring_read = ring_read;
    music.ring_frames = ring_count;
    if (ring_count < music_bank_.ring_capacity_frames) {
        music_bank_.epoch.fetch_add(1, std::memory_order_release);
        music_bank_.epoch.notify_one();
    }
}

bool LocalMusicPlayer::prepare_music_track(const std::string& path,
                                          MusicTrack& track) {
    SDL_RWops* rw = SDL_RWFromFile(path.c_str(), "rb");
    if (!rw) return false;
    WavInfo info;
    const bool valid = parse_music_wav(rw, info);
    SDL_RWclose(rw);
    if (!valid) return false;
    track.path = path;
    track.channels = info.channels;
    track.rate = info.rate;
    track.data_offset = info.data_offset;
    track.data_bytes = info.data_bytes;
    track.source_frames = info.data_bytes / (info.channels * 2u);
    return make_music_prefix(track.path, music_bank_.device,
        music_bank_.ring_capacity_frames, track.prefix, track.complete,
        track.data_bytes);
}

void LocalMusicPlayer::music_worker() {
    auto& bank = music_bank_;
    std::uint64_t generation = UINT64_MAX;
    unsigned track_id = 0;
    SDL_RWops* rw = nullptr;
    SDL_AudioStream* stream = nullptr;
    WavInfo info{};
    std::uint64_t source_offset = 0;
    std::size_t discard_frames = 0;
    bool flushed = false;
    std::vector<Uint8> input, output;
    try {
        const std::size_t frame_bytes = sizeof(int16_t) * audio_channels_;
        const std::size_t quantum = std::max<std::size_t>(1, bank.device.samples);
        input.resize(std::max<std::size_t>(frame_bytes,
            quantum * 2 * sizeof(int16_t)));
        output.resize(quantum * frame_bytes);
    } catch (...) {
        SDL_LockMutex(audio_mtx_);
        local_music_.worker_failed = true;
        local_music_.ready = false;
        SDL_UnlockMutex(audio_mtx_);
        return;
    }
    const std::size_t output_frame_bytes = sizeof(int16_t) * audio_channels_;
    auto reset_decoder = [&] {
        if (stream) SDL_FreeAudioStream(stream);
        if (rw) SDL_RWclose(rw);
        stream = nullptr;
        rw = nullptr;
        source_offset = 0;
        flushed = false;
    };
    auto open_decoder = [&](const MusicTrack& track) -> bool {
        reset_decoder();
        rw = SDL_RWFromFile(track.path.c_str(), "rb");
        if (!rw || !parse_music_wav(rw, info)) { reset_decoder(); return false; }
        stream = music_stream(info, bank.device);
        if (!stream) { reset_decoder(); return false; }
        return true;
    };
    auto produce = [&](std::size_t requested_frames, std::size_t& produced) -> bool {
        produced = 0;
        while (produced < requested_frames) {
            int available = SDL_AudioStreamAvailable(stream);
            if (available < 0) return false;
            const std::size_t have_frames = static_cast<std::size_t>(available) /
                                            output_frame_bytes;
            if (have_frames != 0) {
                const std::size_t take = std::min(requested_frames - produced,
                    std::min(have_frames, output.size() / output_frame_bytes));
                const int bytes = static_cast<int>(take * output_frame_bytes);
                if (SDL_AudioStreamGet(stream, output.data() + produced * output_frame_bytes,
                                       bytes) != bytes) return false;
                produced += take;
                continue;
            }
            if (source_offset < info.data_bytes) {
                const std::size_t frame_bytes = info.channels * 2u;
                const std::size_t source_chunk_frames = std::max<std::size_t>(1,
                    (std::size_t(bank.device.samples) * info.rate +
                     bank.device.freq - 1) / bank.device.freq);
                const std::size_t max_frames = std::max<std::size_t>(1,
                    std::min(source_chunk_frames, input.size() / frame_bytes));
                const std::uint64_t remain = info.data_bytes - source_offset;
                const std::size_t count = std::min<std::uint64_t>(max_frames,
                    remain / frame_bytes);
                const std::size_t bytes = count * frame_bytes;
                if (SDL_RWseek(rw, static_cast<Sint64>(info.data_offset + source_offset),
                               RW_SEEK_SET) < 0 ||
                    !rw_read_exact(rw, input.data(), bytes) ||
                    SDL_AudioStreamPut(stream, input.data(), static_cast<int>(bytes)) < 0)
                    return false;
                source_offset += bytes;
            } else if (!flushed) {
                if (SDL_AudioStreamFlush(stream) < 0) return false;
                flushed = true;
            } else {
                const MusicTrack* track = nullptr;
                auto found = bank.tracks.find(track_id);
                if (found == bank.tracks.end()) return false;
                track = &found->second;
                if (!open_decoder(*track)) return false;
            }
        }
        return true;
    };

    for (;;) {
        const std::uint64_t observed = bank.epoch.load(std::memory_order_acquire);
        bool stop = false, need_fill = false, failed = false;
        std::size_t free_frames = 0;
        unsigned selected = 0;
        std::uint64_t selected_generation = 0;
        const MusicTrack* selected_track = nullptr;
        SDL_LockMutex(audio_mtx_);
        stop = bank.stop;
        selected = bank.current_track;
        selected_generation = local_music_.generation;
        if (!stop && local_music_.ready && local_music_.active &&
            !local_music_.complete && !local_music_.host_paused &&
            !local_music_.guest_paused &&
            local_music_.ring_frames < bank.ring_capacity_frames) {
            auto found = bank.tracks.find(selected);
            if (found != bank.tracks.end()) {
                selected_track = &found->second;
                free_frames = bank.ring_capacity_frames -
                              local_music_.ring_frames;
                need_fill = true;
            }
        }
        SDL_UnlockMutex(audio_mtx_);
        if (stop) break;
        if (!need_fill) {
            bank.epoch.wait(observed, std::memory_order_acquire);
            continue;
        }
        if (selected_generation != generation || selected != track_id) {
            reset_decoder();
            generation = selected_generation;
            track_id = selected;
            discard_frames = selected_track->prefix.size() / audio_channels_;
            if (!open_decoder(*selected_track)) failed = true;
        }
        if (!failed && discard_frames != 0) {
            std::size_t discarded = 0;
            const std::size_t want = std::min(discard_frames,
                std::max<std::size_t>(1, bank.device.samples));
            if (!produce(want, discarded) || discarded == 0) failed = true;
            else discard_frames -= discarded;
        } else if (!failed) {
            const std::size_t wanted = std::min<std::size_t>(
                bank.device.samples, free_frames);
            std::size_t produced = 0;
            if (!produce(wanted, produced) || produced == 0) failed = true;
            else {
                SDL_LockMutex(audio_mtx_);
                if (local_music_.generation == generation && !bank.stop) {
                    const std::size_t put = produced;
                    const std::size_t write = (local_music_.ring_read +
                        local_music_.ring_frames) % bank.ring_capacity_frames;
                    const std::size_t first = std::min(put,
                        bank.ring_capacity_frames - write);
                    std::memcpy(bank.ring.data() + write * audio_channels_,
                        output.data(), first * output_frame_bytes);
                    if (put > first)
                        std::memcpy(bank.ring.data(),
                            output.data() + first * output_frame_bytes,
                            (put - first) * output_frame_bytes);
                    local_music_.ring_frames += put;
                }
                SDL_UnlockMutex(audio_mtx_);
            }
        }
        if (failed) {
            SDL_LockMutex(audio_mtx_);
            if (local_music_.generation == selected_generation) {
                local_music_.worker_failed = true;
                local_music_.ready = false;
                local_music_.active = false;
            }
            SDL_UnlockMutex(audio_mtx_);
        }
    }
    reset_decoder();
}

void LocalMusicPlayer::preload_directory(const char* directory, bool can_stream,
                                         const SDL_AudioSpec& device,
                                         double preroll_ms) {
    if (!directory || !directory[0]) return;
    music_bank_.directory_mode = true;
    if (!audio_mtx_ || !can_stream ||
        device.freq <= 0 || device.samples == 0 || preroll_ms <= 0.0) return;
    auto& bank = music_bank_;
    const auto startup_begin = std::chrono::steady_clock::now();
    bank.device = device;
    bank.preroll_ms = preroll_ms;
    const long double frames = std::ceil(
        static_cast<long double>(device.freq) * preroll_ms / 1000.0L);
    if (frames <= 0 || frames > static_cast<long double>(SIZE_MAX /
            (sizeof(int16_t) * audio_channels_))) return;
    bank.ring_capacity_frames = static_cast<std::size_t>(frames);
    try {
        bank.ring.resize(bank.ring_capacity_frames * audio_channels_);
    } catch (...) {
        bank.ring_capacity_frames = 0;
        return;
    }

    using Candidate = std::pair<unsigned, std::filesystem::path>;
    std::vector<Candidate> candidates;
    std::error_code ec;
    try {
        const auto* dir_begin = reinterpret_cast<const char8_t*>(directory);
        const auto* dir_end = dir_begin + std::strlen(directory);
        const std::filesystem::path root(std::u8string(dir_begin, dir_end));
        std::filesystem::directory_iterator it(root, ec), end;
        while (!ec && it != end) {
            std::error_code entry_error;
            if (it->is_regular_file(entry_error) && !entry_error) {
                const std::u8string name8 = it->path().filename().u8string();
                const std::string name(reinterpret_cast<const char*>(name8.data()),
                                       name8.size());
                unsigned track = 0;
                if (parse_local_music_filename(name, track))
                    candidates.emplace_back(track, it->path());
            }
            it.increment(ec);
        }
    } catch (const std::bad_alloc&) {
        std::fprintf(stderr, "host_window: not enough memory to enumerate local music\n");
        return;
    } catch (const std::filesystem::filesystem_error& error) {
        std::fprintf(stderr, "host_window: local music directory could not be read: %s\n",
                     error.what());
        return;
    }
    if (ec) {
        std::fprintf(stderr, "host_window: local music directory scan failed: %s\n",
                     ec.message().c_str());
        return;
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a,
                                                        const Candidate& b) {
        if (a.first != b.first) return a.first < b.first;
        return a.second.native() < b.second.native();
    });

    for (std::size_t i = 0; i < candidates.size();) {
        std::size_t group_end = i + 1;
        while (group_end < candidates.size() &&
               candidates[group_end].first == candidates[i].first)
            ++group_end;
        const unsigned track = candidates[i].first;
        if (group_end - i > 1) {
            std::fprintf(stderr,
                         "host_window: local music track %u has duplicate filenames; skipping\n",
                         track);
            i = group_end;
            continue;
        }
        try {
            const std::u8string path8 = candidates[i].second.u8string();
            const std::string path(reinterpret_cast<const char*>(path8.data()),
                                   path8.size());
            MusicTrack prepared;
            if (prepare_music_track(path, prepared))
                bank.tracks.emplace(track, std::move(prepared));
        } catch (const std::bad_alloc&) {
            std::fprintf(stderr,
                         "host_window: not enough memory to cache local music opening %u\n",
                         track);
        } catch (const std::filesystem::filesystem_error& error) {
            std::fprintf(stderr, "host_window: local music track %u path failed: %s\n",
                         track, error.what());
        }
        i = group_end;
    }
    try {
        bank.worker = std::thread(&LocalMusicPlayer::music_worker, this);
        bank.worker_available = true;
    }
    catch (...) {
        std::fprintf(stderr, "host_window: local music streaming worker could not start\n");
    }
    std::size_t cached_bytes = 0;
    for (const auto& item : bank.tracks)
        cached_bytes += item.second.prefix.size() * sizeof(int16_t);
    const auto startup_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - startup_begin).count();
    std::fprintf(stderr,
        "[local-music] openings=%zu cached=%zu bytes startup=%.3fms\n",
        bank.tracks.size(), cached_bytes, startup_ms);
}

void LocalMusicPlayer::load_file(const char* path, bool can_stream,
                                 const SDL_AudioSpec& device, double preroll_ms,
                                 float host_gain) {
    if (!path || !path[0] || !audio_mtx_ || !can_stream ||
        device.freq <= 0 || device.samples == 0 || preroll_ms <= 0.0) return;
    auto& bank = music_bank_;
    bank.device = device;
    bank.preroll_ms = preroll_ms;
    const long double frames = std::ceil(
        static_cast<long double>(device.freq) * preroll_ms / 1000.0L);
    if (frames <= 0 || frames > static_cast<long double>(SIZE_MAX /
            (sizeof(int16_t) * audio_channels_))) return;
    bank.ring_capacity_frames = static_cast<std::size_t>(frames);
    try {
        bank.ring.resize(bank.ring_capacity_frames * audio_channels_);
        MusicTrack prepared;
        if (!prepare_music_track(path, prepared)) return;
        auto inserted = bank.tracks.emplace(1u, std::move(prepared));
        auto& music = local_music_;
        SDL_LockMutex(audio_mtx_);
        music.prefix = &inserted.first->second.prefix;
        music.prefix_cursor = music.ring_read = music.ring_frames = 0;
        music.complete = inserted.first->second.complete;
        music.host_gain = host_gain;
        music.ready = !music.prefix->empty();
        bank.current_track = music.ready ? 1u : 0u;
        SDL_UnlockMutex(audio_mtx_);
        if (music.ready) {
            bank.worker = std::thread(&LocalMusicPlayer::music_worker, this);
            bank.worker_available = true;
        }
    } catch (const std::bad_alloc&) {
        SDL_LockMutex(audio_mtx_);
        local_music_.ready = false;
        bank.current_track = 0;
        SDL_UnlockMutex(audio_mtx_);
        std::fprintf(stderr, "host_window: not enough memory to cache local music opening\n");
    } catch (const std::system_error& error) {
        SDL_LockMutex(audio_mtx_);
        local_music_.ready = false;
        bank.current_track = 0;
        SDL_UnlockMutex(audio_mtx_);
        std::fprintf(stderr, "host_window: local music streaming worker could not start: %s\n",
                     error.what());
    }
}

// Control surface. The locking methods take audio_mtx_ themselves; callers
// must not already hold it. mix() is the exception (see the header).

void LocalMusicPlayer::attach(SDL_mutex* audio_mtx, int audio_channels) {
    audio_mtx_ = audio_mtx;
    audio_channels_ = audio_channels;
}

bool LocalMusicPlayer::has_prefix() const {
    return local_music_.prefix != nullptr;
}

bool LocalMusicPlayer::ready() {
    if (!audio_mtx_) return false;
    SDL_LockMutex(audio_mtx_);
    const bool ready = local_music_.ready;
    SDL_UnlockMutex(audio_mtx_);
    return ready;
}

bool LocalMusicPlayer::select(unsigned track) {
    if (!audio_mtx_) return false;
    auto& bank = music_bank_;
    if (!bank.directory_mode) {
        if (track == 1 && ready()) return true;
        playback(false, false, 0.0f, false);
        return false;
    }
    auto selected = bank.tracks.find(track);
    SDL_LockMutex(audio_mtx_);
    auto& music = local_music_;
    if (track != 0 && selected != bank.tracks.end()) {
        if (bank.current_track == track && music.ready) {
            SDL_UnlockMutex(audio_mtx_);
            return true;
        }
        bank.current_track = track;
        music.prefix = &selected->second.prefix;
        music.prefix_cursor = 0;
        music.ring_read = 0;
        music.ring_frames = 0;
        music.complete = selected->second.complete;
        music.worker_failed = false;
        ++music.generation;
        music.active = false;
        music.ready = !music.prefix->empty() &&
                      (music.complete || bank.worker_available);
    } else {
        bank.current_track = 0;
        music.prefix = nullptr;
        music.prefix_cursor = 0;
        music.ring_read = 0;
        music.ring_frames = 0;
        music.complete = false;
        music.worker_failed = false;
        ++music.generation;
        music.active = false;
        music.ready = false;
    }
    const bool ready = music.ready;
    SDL_UnlockMutex(audio_mtx_);
    bank.epoch.fetch_add(1, std::memory_order_release);
    bank.epoch.notify_one();
    return ready;
}

void LocalMusicPlayer::playback(bool active, bool paused, float gain, bool restart) {
    if (!audio_mtx_) return;
    if (!std::isfinite(gain) || gain < 0.0f) gain = 0.0f;
    gain = std::clamp(gain, 0.0f, 1.0f);
    SDL_LockMutex(audio_mtx_);
    auto& music = local_music_;
    if (!music.ready) {
        SDL_UnlockMutex(audio_mtx_);
        return;
    }
    music.gain = gain;
    music.guest_paused = paused;
    if (!active) {
        const bool reset = music.active || music.prefix_cursor != 0 ||
                           music.ring_frames != 0;
        music.active = false;
        if (reset) {
            music.prefix_cursor = 0;
            music.ring_read = 0;
            music.ring_frames = 0;
            ++music.generation;
        }
    } else {
        if (restart) {
            music.prefix_cursor = 0;
            music.ring_read = 0;
            music.ring_frames = 0;
            ++music.generation;
        }
        music.active = true;
    }
    SDL_UnlockMutex(audio_mtx_);
    music_bank_.epoch.fetch_add(1, std::memory_order_release);
    music_bank_.epoch.notify_one();
}

void LocalMusicPlayer::set_host_gain(float gain) {
    if (!audio_mtx_) return;
    SDL_LockMutex(audio_mtx_);
    local_music_.host_gain = gain;
    SDL_UnlockMutex(audio_mtx_);
}

void LocalMusicPlayer::set_turbo_muted(bool on) {
    if (!audio_mtx_) return;
    SDL_LockMutex(audio_mtx_);
    local_music_.turbo_muted = on;
    SDL_UnlockMutex(audio_mtx_);
}

void LocalMusicPlayer::set_host_paused(bool paused) {
    if (!audio_mtx_) return;
    SDL_LockMutex(audio_mtx_);
    local_music_.host_paused = paused;
    SDL_UnlockMutex(audio_mtx_);
    music_bank_.epoch.fetch_add(1, std::memory_order_release);
    music_bank_.epoch.notify_one();
}

void LocalMusicPlayer::shutdown() {
    if (audio_mtx_) {
        SDL_LockMutex(audio_mtx_);
        music_bank_.stop = true;
        local_music_.ready = false;
        local_music_.active = false;
        SDL_UnlockMutex(audio_mtx_);
    } else {
        music_bank_.stop = true;
        local_music_.ready = false;
        local_music_.active = false;
    }
    music_bank_.epoch.fetch_add(1, std::memory_order_release);
    music_bank_.epoch.notify_one();
    if (music_bank_.worker.joinable()) music_bank_.worker.join();
}

std::uint64_t LocalMusicPlayer::underrun_frames() const {
    return local_music_.underrun_frames;
}

}  // namespace gbarecomp

#endif  // GBARECOMP_HAVE_SDL2
