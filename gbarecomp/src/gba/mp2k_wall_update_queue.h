// Bounded guest-timeline updates for the Turbo MP2K wall mixer.
// Emulation-thread owned. Events copy state only; guest pointers/assets never
// remain in this queue.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "mp2k_wall_mixer.h"

namespace gba {

class Mp2kWallUpdateQueue {
public:
    // One coherent producer block per event. 8192 blocks is about 45 seconds
    // at 4x guest production; storage is allocated only on prime/request.
    static constexpr std::size_t kCapacity = 8192;
    // Bound aggregate copied producer-seed storage independently of event
    // count; each seed remains owned by the queue and never aliases guest RAM.
    static constexpr std::size_t kMaxQueuedSeedWords = 1u << 20;
    static constexpr std::size_t kAssetRecords = 128;

    struct Stats {
        uint64_t queued = 0;
        uint64_t applied = 0;
        uint64_t coalesced = 0;
        uint64_t stale = 0;
        uint64_t out_of_order = 0;
        uint64_t overflow = 0;
        uint64_t asset_overflows = 0;
        uint64_t high_water = 0;
        uint64_t estimated_remaining_q32 = 0;
        bool fallback_required = false;
    };

    Mp2kWallUpdateQueue() = default;

    void reset(uint64_t anchor_guest_cursor_q32 = 0);
    bool empty() const { return count_ == 0; }
    std::size_t size() const { return count_; }
    std::size_t capacity() const { return events_ ? kCapacity : 0; }
    uint64_t anchor_guest_cursor_q32() const { return anchor_guest_q32_; }
    double estimated_remaining_seconds() const;
    const Stats& stats() const { return stats_; }

    // Prime after the mixer accepted its initial verified snapshot.
    bool prime(const Mp2kWallSnapshotInput& snapshot,
               const Mp2kWallMixer& mixer);

    // Diff one newly published verified guest snapshot. One full coherent
    // event is retained per publication, including unchanged state, so block
    // positions and source ordering cannot be skipped.
    Mp2kWallCaptureResult enqueue_snapshot(
        const Mp2kWallSnapshotInput& snapshot, Mp2kWallMixer& mixer);

    // Apply events whose guest timeline has arrived at the wall cursor.
    // Returns false and latches fallback on stale assets/order/mixer failure.
    bool apply_due(Mp2kWallMixer& mixer, uint64_t due_guest_cursor_q32);
    bool next_due_guest_cursor_q32(uint64_t& cursor) const;

private:
    struct AssetKey {
        Mp2kWallAssetKind kind = Mp2kWallAssetKind::Pcm8;
        uint64_t hash = 0;
        uint32_t byte_count = 0;
        uint32_t sample_count = 0;
        uint32_t loop_start = 0;
        bool looped = false;
        Mp2kSynthParams synth{};
        bool operator==(const AssetKey& other) const;
    };

    struct AssetRecord {
        bool valid = false;
        AssetKey key{};
        Mp2kWallAssetHandle handle{};
    };

    struct GuestState {
        bool valid = false;
        uint64_t source_sequence = UINT64_MAX;
        uint64_t guest_cursor_q32 = 0;
        uint32_t pcm_rate = 0;
        uint8_t reverb = 0;
        uint32_t gain_ramp_frames = 0;
        float route_a_gain = 1.0f;
        float route_b_gain = 1.0f;
        std::array<Mp2kWallVoiceSnapshot, Mp2kWallMixer::kMaxVoices>
            voices{};
    };

    struct Event {
        uint64_t sequence = 0;
        uint64_t source_sequence = UINT64_MAX;
        uint64_t guest_cursor_q32 = 0;
        uint32_t pcm_rate = 0;
        uint8_t reverb = 0;
        uint32_t gain_ramp_frames = 0;
        float route_a_gain = 1.0f;
        float route_b_gain = 1.0f;
        std::array<Mp2kWallVoiceSnapshot, Mp2kWallMixer::kMaxVoices>
            voices{};
        std::vector<uint32_t> seed_words;
        uint32_t producer_block_frames = 0;
        bool has_pcm_rate = false;
        bool has_route = false;
        bool has_reverb = false;
        bool has_gain_ramp = false;
        bool has_seed_block = false;
        bool full_state = true;
    };

    static uint64_t asset_hash(const Mp2kWallAssetInput& input);
    static AssetKey make_key(const Mp2kWallAssetInput& input);
    static bool same_voice(const Mp2kWallVoiceSnapshot& a,
                           const Mp2kWallVoiceSnapshot& b);
    static bool same_global(const GuestState& a, const GuestState& b);
    bool resolve_assets(const Mp2kWallSnapshotInput& snapshot,
                        Mp2kWallMixer& mixer,
                        std::array<Mp2kWallAssetHandle,
                                   Mp2kWallMixer::kMaxAssets>& handles);
    bool append_event(Event event);
    bool next_sequence(uint64_t& sequence);
    void fatal_overflow();
    void set_previous(const Mp2kWallSnapshotInput& snapshot,
                      const std::array<Mp2kWallVoiceSnapshot,
                                       Mp2kWallMixer::kMaxVoices>& voices);
    static bool checked_add(uint64_t a, uint64_t b, uint64_t& out);

    bool ensure_storage();
    std::unique_ptr<Event[]> events_{};
    std::size_t head_ = 0;
    std::size_t count_ = 0;
    uint64_t anchor_guest_q32_ = 0;
    uint64_t next_event_sequence_ = 1;
    uint64_t last_event_guest_q32_ = 0;
    bool have_last_event_ = false;
    GuestState previous_{};
    std::array<AssetRecord, kAssetRecords> assets_{};
    uint64_t consumed_guest_q32_ = 0;
    std::size_t queued_seed_words_ = 0;
    Stats stats_{};
};

}  // namespace gba
