// Pure Stage-3b MP2K wall-clock mixer foundation.
//
// Snapshot fields mirror Mp2kShadow's verified voice state. This component
// owns copied assets and never reads guest memory after capture. Stage 3 owns
// all calls on the emulation thread; SDL/audio callbacks consume a later
// separate PCM ring and never call this scheduler or mixer.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "mp2k_shadow.h"

namespace gba {

// Existing shadow timing/diagnostic paths clamp a canonical block-related
// span at 8192 host-grid frames; keep the export pool bounded to that range.
constexpr uint32_t kMp2kWallMaxProducerFrames = 8192u;
constexpr std::size_t kMp2kWallMaxAssets = 128u;

enum class Mp2kWallAssetKind : uint8_t {
    Pcm8,
    CompressedDpcm,
    Synth,
};

enum class Mp2kWallRoute : uint8_t {
    RouteA,
    RouteB,
    Both,
};

enum class Mp2kWallCaptureResult : uint8_t {
    Accepted,
    InvalidInput,
    InvalidVoice,
    Unsupported,
    AssetOverflow,
    NonFinite,
    StaleAsset,
    OutOfOrder,
};

// Input bytes/payload are borrowed only during begin_snapshot. No pointer is
// retained. PCM16 is deliberately unsupported: MP2K's verified path here is
// signed PCM8, DPCM, or Camelot synth.
struct Mp2kWallAssetInput {
    Mp2kWallAssetKind kind = Mp2kWallAssetKind::Pcm8;
    const uint8_t* bytes = nullptr;
    uint32_t byte_count = 0;
    uint32_t sample_count = 0;
    uint32_t loop_start = 0;
    bool looped = false;
    const uint8_t* synth_payload = nullptr;
    uint32_t synth_payload_len = 0;
    Mp2kSynthParams synth{};
};

// Exact shadow-derived state. g0/g1 are canonical envelope-derived gains;
// there is no second invented ADSR model here.
struct Mp2kWallVoiceSnapshot {
    bool on = false;
    uint8_t ctype = 0;
    uint8_t asset_index = 0;
    Mp2kWallRoute route = Mp2kWallRoute::Both;
    uint32_t pos_index = 0;
    uint32_t pos_frac = 0;
    uint32_t step_q23 = 0;
    uint32_t size = 0;
    uint32_t loop_start = 0;
    bool looped = false;
    // Zero means legacy snapshot input. Nonzero binds the voice to a staged
    // immutable asset generation; queued updates reject stale generations.
    uint32_t asset_generation = 0;
    bool compressed = false;
    bool reversed = false;
    float g0r = 0.0f;
    float g0l = 0.0f;
    float g1r = 0.0f;
    float g1l = 0.0f;
    uint32_t env_now = 0;
    uint32_t env_next = 0;
    uint8_t synth_kind = 0;
    uint8_t synth_base = 0;
    uint8_t synth_step = 0;
    uint8_t synth_depth = 0;
    uint8_t synth_init_duty = 0;
    uint32_t synth_duty_pos = 0;
    uint32_t synth_pos = 0;
    double synth_phase = 0.0;
    double synth_step_render = 0.0;
    float synth_duty = 0.0f;
    float synth_duty_step = 0.0f;
};

struct Mp2kWallSnapshotInput {
    static constexpr std::size_t kMaxVoices = kMp2kMaxChans;

    uint64_t sequence = 0;
    uint64_t guest_cursor_q32 = 0;
    uint32_t render_rate = 0;
    uint32_t pcm_rate = 0;
    uint8_t reverb = 0;
    uint32_t producer_block_frames = 0;
    uint32_t gain_ramp_frames = 0;
    std::array<uint32_t, kMp2kWallMaxProducerFrames> seed_words{};
    uint32_t seed_word_count = 0;
    bool seed_valid = false;
    float route_a_gain = 1.0f;
    float route_b_gain = 1.0f;
    uint8_t asset_count = 0;
    std::array<Mp2kWallAssetInput, kMp2kWallMaxAssets> assets{};
    std::array<Mp2kWallVoiceSnapshot, kMaxVoices> voices{};
};

struct Mp2kWallVoiceUpdate {
    static constexpr std::size_t kMaxVoices = kMp2kMaxChans;
    static constexpr uint8_t kGlobal = 0xFFu;
    uint64_t sequence = 0;
    uint64_t guest_cursor_q32 = 0;
    uint8_t channel = kGlobal;
    Mp2kWallVoiceSnapshot voice{};
    bool has_pcm_rate = false;
    uint32_t pcm_rate = 0;
    bool has_route = false;
    float route_a_gain = 1.0f;
    float route_b_gain = 1.0f;
    bool has_reverb = false;
    uint8_t reverb = 0;
    bool has_gain_ramp = false;
    uint32_t gain_ramp_frames = 0;
    // A producer-block/global update replaces the seed block atomically.
    // Pointers are never used; all words are copied by the mixer.
    bool has_seed_block = false;
    uint32_t producer_block_frames = 0;
    uint32_t seed_word_count = 0;
    std::array<uint32_t, kMp2kWallMaxProducerFrames> seed_words{};
    // Runtime Turbo applies one coherent full voice/global block. The array
    // is fixed-size; no per-event heap or borrowed guest pointers.
    bool has_full_snapshot = false;
    std::array<Mp2kWallVoiceSnapshot, kMaxVoices> voices{};
};

struct Mp2kWallAssetHandle {
    uint64_t epoch = 0;
    uint8_t slot = 0;
    uint32_t generation = 0;
};

struct Mp2kWallStereoChunk {
    static constexpr std::size_t kMaxFrames = 512;
    std::array<float, kMaxFrames> left{};
    std::array<float, kMaxFrames> right{};
    uint32_t frame_count = 0;
};

struct Mp2kWallMixerStats {
    uint64_t snapshots_accepted = 0;
    uint64_t snapshots_rejected = 0;
    uint64_t voice_updates = 0;
    uint64_t rendered_frames = 0;
    uint64_t silent_frames = 0;
    uint64_t resets = 0;
    uint64_t asset_overflows = 0;
    uint64_t unsupported = 0;
    uint64_t nonfinite = 0;
    uint64_t out_of_order = 0;
    uint64_t late_updates = 0;
    uint64_t seed_exhaustions = 0;
    uint64_t assets_staged = 0;
    uint32_t active_voices = 0;
    uint32_t pcm_rate = 0;
    bool fallback_required = false;
};

class Mp2kWallMixer {
public:
    static constexpr std::size_t kMaxVoices = kMp2kMaxChans;
    static constexpr std::size_t kMaxAssets = kMp2kWallMaxAssets;
    // One bounded arena, allocated lazily on first decoupled snapshot. Larger
    // total snapshots fail closed; render never allocates.
    static constexpr std::size_t kAssetArenaBytes = 16u * 1024u * 1024u;
    static constexpr uint32_t kCanonicalRenderRate = 65536u;
    static constexpr uint32_t kMaxProducerFrames =
        kMp2kWallMaxProducerFrames;
    static constexpr std::size_t kMaxChunkFrames =
        Mp2kWallStereoChunk::kMaxFrames;
    static constexpr uint64_t kQ32One = uint64_t{1} << 32;

    explicit Mp2kWallMixer(uint32_t render_rate = kCanonicalRenderRate,
                           std::size_t arena_bytes = kAssetArenaBytes);
    ~Mp2kWallMixer();
    Mp2kWallMixer(const Mp2kWallMixer&) = delete;
    Mp2kWallMixer& operator=(const Mp2kWallMixer&) = delete;

    uint32_t wall_rate() const { return wall_rate_; }
    uint64_t epoch() const { return epoch_; }
    uint64_t wall_cursor_q32() const { return wall_cursor_q32_; }
    const Mp2kWallMixerStats& stats() const { return stats_; }

    // Starts a fresh snapshot epoch. Re-enables output only after all checks
    // pass; reset/fallback acknowledgement alone never does.
    Mp2kWallCaptureResult begin_snapshot(
        const Mp2kWallSnapshotInput& input);
    Mp2kWallCaptureResult begin_snapshot_from_shadow(
        const Mp2kShadow& shadow, const MemView& mem,
        uint64_t sequence, uint64_t guest_cursor_q32);

    // Applies canonical-derived voice state without resetting cursor, epoch,
    // or copied assets. A failed update latches fallback and silences output.
    Mp2kWallCaptureResult apply_voice_update(
        const Mp2kWallVoiceUpdate& update);

    // Copy one immutable asset into the current epoch's bounded arena.
    // Existing slots are never overwritten; callers keep the returned
    // generation in queued voice events.
    Mp2kWallCaptureResult stage_asset(
        const Mp2kWallAssetInput& input, Mp2kWallAssetHandle& handle);

    void reset();
    // Acknowledge diagnostics only. It never re-enables output without a new
    // successful begin_snapshot.
    void acknowledge_fallback() { stats_.fallback_required = false; }

    Mp2kWallAssetHandle asset_handle(uint8_t slot) const;
    bool asset_valid(const Mp2kWallAssetHandle& handle) const;

    // Produces at most kMaxChunkFrames at wall_rate. No guest clock/events.
    uint32_t render(uint32_t frames, Mp2kWallStereoChunk& out);

private:
    struct AssetSlot {
        Mp2kWallAssetKind kind = Mp2kWallAssetKind::Pcm8;
        Mp2kSynthParams synth{};
        uint32_t byte_count = 0;
        uint32_t sample_count = 0;
        uint32_t loop_start = 0;
        bool looped = false;
        uint32_t generation = 1;
        bool occupied = false;
        uint32_t offset = 0;
    };

    struct VoiceState {
        Mp2kWallVoiceSnapshot snapshot{};
        uint32_t dpcm_block = UINT32_MAX;
        std::array<int8_t, 64> dpcm_samples{};
        uint32_t ramp_frame = 0;
        uint64_t ramp_anchor_q32 = 0;
    };

    static bool finite(float value);
    static bool finite(double value);
    static bool valid_kind(Mp2kWallAssetKind kind);
    static bool valid_route(Mp2kWallRoute route);
    static bool validate_voice(const Mp2kWallVoiceSnapshot& voice);
    bool validate_asset(const Mp2kWallAssetInput& input) const;
    bool ensure_arena();
    bool checked_asset(const Mp2kWallAssetHandle& handle) const;
    void begin_epoch();
    Mp2kWallCaptureResult fail(Mp2kWallCaptureResult result);
    Mp2kWallCaptureResult install_voice(uint8_t channel,
                                        const Mp2kWallVoiceSnapshot& voice,
                                        bool initial,
                                        uint64_t guest_cursor_q32);
    int32_t fetch_s8(VoiceState& voice, const AssetSlot& asset,
                     uint32_t index);
    int32_t sample_voice_q8(VoiceState& voice, bool& fix_pcm);
    uint32_t packed_gain(const VoiceState& voice,
                         bool fix_pcm) const;
    void reset_ramps(uint64_t guest_cursor_q32);
    bool seed_for_frame(uint32_t& word);

    uint32_t wall_rate_ = kCanonicalRenderRate;
    uint64_t epoch_ = 1;
    uint64_t wall_cursor_q32_ = 0;
    uint64_t wall_remainder_ = 0;
    uint32_t pcm_rate_ = 0;
    uint32_t producer_block_frames_ = 0;
    uint32_t gain_ramp_frames_ = 0;
    uint64_t rendered_in_snapshot_ = 0;
    std::array<uint32_t, kMp2kWallMaxProducerFrames> seed_words_{};
    uint32_t seed_word_count_ = 0;
    bool seed_valid_ = false;
    uint64_t seed_block_wall_start_frame_ = 0;
    uint64_t seed_block_guest_cursor_q32_ = 0;
    uint64_t last_sequence_ = 0;
    uint64_t last_guest_cursor_q32_ = 0;
    bool have_timeline_ = false;
    float route_a_gain_ = 1.0f;
    float route_b_gain_ = 1.0f;
    bool output_enabled_ = false;
    std::size_t arena_bytes_ = 0;
    std::size_t arena_used_ = 0;
    std::unique_ptr<uint8_t[]> arena_;
    std::array<AssetSlot, kMaxAssets> assets_{};
    std::array<VoiceState, kMaxVoices> voices_{};
    Mp2kWallMixerStats stats_{};
};

}  // namespace gba
