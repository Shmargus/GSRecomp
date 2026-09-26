// mp2k_shadow.h — MP2K ("m4a") HLE shadow mixer (the float re-render).
//
// QoL-on-top-of-correctness per PRINCIPLES.md "Verified-enhancement HLE":
// the recompiled guest M4A driver still runs and still produces the canon
// FIFO stream (which stays the verify oracle). This shadow re-renders the
// SAME voice state in float at 65536 Hz internally, but the published
// candidate is NOT a higher-quality re-render: every sample that reaches
// host_stream_ is first quantized back to int8_t by
// mp2k_decode_producer_word / mp2k_extract_dry_sample and rescaled by
// /128.0f, then bit-diffed against the guest's own 8-bit, ~13.4 kHz output
// (compare_signed_stereo_block). So this path is currently a bit-replica of
// the driver's 8-bit requantization and low mix-rate ceiling, not free of
// them — see docs/MP2K_NATIVE_AUDIO_HANDOFF.md "Strategic status" before
// resuming work here. It is policed every grid sample by the
// engine-agnostic ShadowVerifier (audio_shadow.h) and substitutes only
// after a proven window, reverting loudly (DEGRADED).
//
// All guest reads are side-effect-free (region pointers, no bus I/O, no
// waitstates), so rendering never perturbs emulation.
//
// ── Attribution ───────────────────────────────────────────────────
// Ported from JRickey/gba-recomp (crates/gba-core/src/mp2k.rs), © Jrickey,
// MIT OR Apache-2.0, used with permission. C++ port is ours; the engine
// struct map is shared with gba_m4a.h. See THIRD_PARTY_ATTRIBUTION.md.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include "audio_shadow.h"
#include "gba_m4a.h"

namespace gba {

struct Mp2kWallSnapshotInput;

// First-window output-domain evidence. This contains aggregate numeric
// telemetry only; it never stores or emits protected audio samples/bytes.
struct Mp2kOutputRouteObservation {
    float route_a = 0.0f;
    float route_b = 0.0f;
    // Canonical FIFO A/B at this same host sample, normalized as the native
    // producer buses. Aggregate-only diagnostic evidence; never retained.
    float fifo_a = 0.0f;
    float fifo_b = 0.0f;
    uint8_t route_mask = 0;       // A-L=1, A-R=2, B-L=4, B-R=8
    uint8_t full_volume_mask = 0; // A=1, B=2
    int32_t psg_left = 0;
    int32_t psg_right = 0;
    uint16_t soundbias = 0;
};

struct Mp2kOutputDomainDiagnostic {
    bool valid = false;
    uint64_t verifier_window = 0;
    uint64_t samples = 0;
    uint64_t missing = 0;
    uint64_t missing_total = 0;
    uint64_t first_cursor = 0;
    uint64_t last_cursor = 0;
    uint64_t producer_block = UINT64_MAX;
    uint64_t published_block = UINT64_MAX;
    float correlation = 0.0f;
    float level_ratio = 0.0f;
    std::array<double, 2> canonical_sum{};
    std::array<double, 2> canonical_sum_sq{};
    std::array<double, 2> canonical_peak{};
    std::array<uint64_t, 2> canonical_nonnegative{};
    std::array<uint64_t, 2> canonical_negative{};
    std::array<double, 2> native_sum{};
    std::array<double, 2> native_sum_sq{};
    std::array<double, 2> native_peak{};
    std::array<uint64_t, 2> native_nonnegative{};
    std::array<uint64_t, 2> native_negative{};
    std::array<double, 2> route_sum{};
    std::array<double, 2> route_sum_sq{};
    std::array<double, 2> route_peak{};
    std::array<uint64_t, 2> route_nonnegative{};
    std::array<uint64_t, 2> route_negative{};
    std::array<double, 2> fifo_sum{};
    std::array<double, 2> fifo_sum_sq{};
    std::array<double, 2> fifo_peak{};
    std::array<uint64_t, 2> fifo_nonnegative{};
    std::array<uint64_t, 2> fifo_negative{};
    uint8_t route_mask = 0;
    uint8_t full_volume_mask = 0;
    uint32_t route_mapping_changes = 0;
    int32_t psg_left = 0;
    int32_t psg_right = 0;
    uint16_t soundbias = 0;
    ProducerHostResampler::Diagnostic resampler{};
};

// One-shot producer-stage evidence. This compares the decoded C70 block and
// the captured DMA-source route bytes before any host resampling. It contains
// aggregate metrics only; no audio samples or protected bytes are retained.
struct Mp2kProducerSourceDiagnostic {
    bool valid = false;
    uint64_t block_id = UINT64_MAX;
    uint32_t samples = 0;
    uint32_t c70_addr = 0;
    uint32_t dma_route_a = 0;
    uint32_t dma_route_b = 0;
    SignedStereoMetrics c70_vs_dma{};
    SignedStereoMetrics native_vs_dma{};
};

// One-shot cursor/fraction provenance for a post-probation reject. This is
// aggregate state only: it retains addresses, indices, and phase fields, not
// guest PCM bytes.
struct Mp2kCursorDiagnostic {
    bool valid = false;
    uint64_t block_id = UINT64_MAX;
    uint64_t snapshot_block = UINT64_MAX;
    uint64_t snapshot_cursor = 0;
    uint32_t voice = UINT32_MAX;
    uint32_t max_index = UINT32_MAX;
    uint32_t advances = 0;
    uint32_t guest_sample_cursor = 0;
    uint32_t guest_resolved_data = 0;
    uint32_t guest_start_index = 0;
    uint32_t guest_start_frac = 0;
    uint32_t guest_max_index = 0;
    uint32_t guest_max_frac = 0;
    uint32_t guest_step = 0;
    uint32_t guest_loop_wraps = 0;
    uint32_t guest_host_index = 0;
    uint32_t guest_host_frac = 0;
    uint32_t guest_host_step = 0;
    uint32_t guest_host_loop_wraps = 0;
    uint32_t host_advances = 0;
    uint32_t native_start_voice = UINT32_MAX;
    uint32_t native_start_pos = 0;
    uint32_t native_start_frac = 0;
    uint32_t native_start_step = 0;
    uint32_t native_max_pos = 0;
    uint32_t native_max_frac = 0;
    uint32_t native_max_step = 0;
    uint8_t guest_valid = 0;
    uint8_t guest_exhausted = 0;
    uint8_t native_start_valid = 0;
    uint8_t native_max_valid = 0;
    uint8_t host_trajectory_valid = 0;
    uint8_t start_match = 0;
    uint8_t max_match = 0;
    // 0=unavailable, 1=start-seed mismatch, 2=host-trajectory mismatch,
    // 3=seed and host trajectory match.
    uint8_t provenance = 0;
};

// One-shot per-voice evidence at the aggregate max-error sample. This keeps
// source identity, operands, and packed accumulator deltas only; it never
// stores or emits PCM payload bytes.
struct Mp2kVoiceContribution {
    bool active = false;
    uint32_t channel = UINT32_MAX;
    uint8_t source_kind = 0;
    uint8_t ctype = 0;
    uint32_t wave = 0;
    uint32_t resolved_wave = 0;
    uint32_t cursor = 0;
    uint32_t frac = 0;
    uint32_t step = 0;
    int32_t sample = 0;
    uint32_t gain_right_q9 = 0;
    uint32_t gain_left_q9 = 0;
    uint32_t packed_gain = 0;
    uint32_t packed_delta = 0;
    uint32_t lane_abs = 0;
};

struct Mp2kVoiceContributionDiagnostic {
    bool valid = false;
    uint64_t block_id = UINT64_MAX;
    uint32_t max_index = UINT32_MAX;
    uint8_t route = 0;
    uint8_t writer_stage = 0;  // 0=C70, 1=paired DMA route
    uint8_t trace_valid = 0;
    uint32_t active_voices = 0;
    uint32_t dominant_voice = UINT32_MAX;
    uint32_t dominant_lane_abs = 0;
    uint32_t native_accumulator_hash = 0;
    uint32_t guest_seed_hash = 0;
    uint32_t native_seed_hash = 0;
    uint32_t guest_raw_hash = 0;
    uint32_t guest_dry_hash = 0;
    std::array<Mp2kVoiceContribution, kMp2kMaxChans> voices{};
};

// Read-only view over the regions sample data can live in. Side-effect free.
struct MemView {
    const uint8_t* rom = nullptr;    std::size_t rom_len = 0;
    const uint8_t* ewram = nullptr;  std::size_t ewram_len = 0;
    const uint8_t* iwram = nullptr;  std::size_t iwram_len = 0;

    // Resolve [addr, addr+len) to a pointer, or nullptr if it leaves region.
    const uint8_t* slice(uint32_t addr, std::size_t len) const;
    bool  u8(uint32_t addr, uint8_t& out) const;
    bool  u32(uint32_t addr, uint32_t& out) const;
};

struct VerifierSampleStats {
    uint64_t canonical_zero = 0;
    uint64_t canonical_held = 0;
    uint64_t canonical_repeated = 0;
    uint64_t native_zero = 0;
    uint64_t native_held = 0;
    uint64_t native_repeated = 0;
};

struct ProducerVoiceTrace {
    bool active = false;
    uint32_t channel = 0;        // explicit voice/channel index (probe-only)
    uint8_t source_kind = 0;     // 1=PCM, 2=FIX, 3=compressed, 4=reversed, 5=synth
    uint8_t  ctype = 0;          // channel type byte, for identity matching
    uint32_t wave = 0;           // raw wav pointer, matches canonical raw
                                  // VoiceIdentity::wave for identity comparison
    uint32_t resolved_wave = 0;  // archive-resolved wave header address
    uint32_t cursor = 0;
    uint32_t frac = 0;
    uint32_t step = 0;
    int32_t sample = 0;
    uint32_t gain_right_q9 = 0;
    uint32_t gain_left_q9 = 0;
    uint32_t packed_gain = 0;
    uint32_t accumulator_before = 0;
    uint32_t accumulator_after = 0;
};

struct CanonicalMixStepTrace {
    uint32_t pc = 0;
    uint64_t cycle = 0;
    uint8_t mode = 0;
    uint32_t addr = 0;
    // `before` is a snapshot of Mp2kGuestMixProbe::previous_voice[index] at
    // this store's address, which is zero-initialized per block; on a
    // block's first index-0 store it is 0 by construction, NOT an
    // observation of the guest's true starting accumulator. Do not treat it
    // as the seed.
    uint32_t before = 0;
    uint32_t after = 0;
    int32_t operand_sample = 0;
    uint32_t operand_gain = 0;
    // Reconstructed guest accumulator that fed this pass's multiply-add,
    // recovered by inverting `after = before + gain * sample` (plain 32-bit
    // modular arithmetic, exact/invertible for every index-0 pass): distinct
    // from `before` above and always a real intermediate value, never a
    // reset-to-zero artifact.
    uint32_t actual_before = 0;
};

struct CanonicalMixTrace {
    struct VoiceIdentity {
        uint16_t channel = 0;
        uint8_t status = 0;
        uint8_t ctype = 0;
        uint32_t channel_base = 0;
        uint32_t cursor = 0;
        uint32_t frac = 0;
        uint32_t wave = 0;
        uint32_t sample_cursor = 0;
        // Resolved archive/header addresses are diagnostic provenance only;
        // no sample bytes are retained.
        uint32_t resolved_wave = 0;
        uint32_t resolved_data = 0;
        uint32_t count = 0;
        uint32_t frequency = 0;
        uint32_t gain_right = 0;
        uint32_t gain_left = 0;
        uint32_t wave_flags = 0;
        uint32_t wave_loop = 0;
        uint32_t wave_size = 0;
        uint8_t looped = 0;
    };
    struct ChannelWrite {
        uint32_t pc = 0;
        uint64_t cycle = 0;
        uint32_t addr = 0;
        uint16_t channel = 0;
        uint16_t field = 0;
        uint32_t before = 0;
        uint32_t value = 0;
        uint8_t width = 0;
        uint8_t mode = 0;
    };
    bool valid = false;
    uint64_t sequence = UINT64_MAX;
    uint32_t base_addr = 0;
    uint32_t count = 0;
    uint64_t pre_cursor = 0;
    uint64_t completion_cursor = 0;
    uint64_t completion_cycles = 0;
    uint64_t pre_hook_cycles = 0;
    uint64_t first_write_cycles = 0;
    uint64_t last_write_cycles = 0;
    uint32_t sound_info = 0;
    uint32_t pre_snapshot_generation = 0;
    uint8_t pre_active_voices = 0;
    uint32_t pre_hook_pc = 0;
    uint8_t pre_hook_source = 0; // 1=SoundMainRAM, 2=VBlank fallback
    std::array<uint16_t, 8> pre_channel_status{};
    std::array<uint16_t, 8> write_channel_status{};
    uint32_t channel_write_count = 0;
    uint32_t channel_write_total = 0;
    std::array<ChannelWrite, 64> channel_writes{};
    std::array<VoiceIdentity, 8> voice_identities{};
    std::array<CanonicalMixStepTrace, 8> steps{};
};

// Bounded per-block boundary evidence. This is observational only: it records
// the last channel preparation write and the last runtime control-flow event
// before the first canonical accumulator store, without selecting a snapshot.
struct CanonicalBoundaryTrace {
    bool valid = false;
    uint64_t sequence = UINT64_MAX;
    uint64_t last_channel_cycle = 0;
    uint32_t last_channel_pc = 0;
    uint32_t last_channel_addr = 0;
    uint16_t last_channel = 0;
    uint16_t last_field = 0;
    uint32_t last_channel_before = 0;
    uint32_t last_channel_value = 0;
    uint64_t control_cycle = 0;
    uint32_t control_pc = 0;
    uint32_t control_target = 0;
    uint32_t control_kind = 0;
    uint8_t control_mode = 0;
    uint64_t first_write_cycle = 0;
    uint32_t first_write_pc = 0;
    uint32_t first_write_addr = 0;
    uint8_t first_write_mode = 0;
    std::array<uint16_t, 8> channel_status{};
    std::array<CanonicalMixTrace::VoiceIdentity, 8> voice_identities{};
};

struct BadWaveTrace {
    struct Write {
        uint32_t pc = 0;
        uint64_t cycle = 0;
        uint32_t addr = 0;
        uint32_t before = 0;
        uint32_t value = 0;
        uint8_t width = 0;
        uint8_t mode = 0;
    };
    bool valid = false;
    uint64_t sequence = UINT64_MAX;
    uint64_t boundary_cycle = 0;
    uint16_t channel = 0;
    uint8_t reason = 0;
    uint8_t status = 0;
    uint8_t ctype = 0;
    uint32_t channel_base = 0;
    uint32_t wave = 0;
    uint32_t resolved_wave = 0;
    uint32_t resolved_data = 0;
    uint32_t wave_size = 0;
    uint32_t wave_loop = 0;
    uint32_t wave_flags = 0;
    uint32_t count = 0;
    uint32_t frac = 0;
    uint32_t sample_cursor = 0;
    uint32_t data_base = 0;
    uint32_t data_size = 0;
    uint32_t cp = 0;
    uint8_t cp_in_range = 0;
    Write status_write{};
    Write type_write{};
    Write wave_write{};
    Write count_write{};
    Write frac_write{};
    Write frequency_write{};
    Write gain_right_write{};
    Write gain_left_write{};
    Write cp_write{};
};

struct ProducerDiffTrace {
    bool valid = false;
    uint64_t sequence = UINT64_MAX;
    uint64_t seed_sequence = UINT64_MAX;
    uint64_t cursor = 0;
    uint32_t route_a_addr = 0;
    uint32_t route_b_addr = 0;
    uint32_t index = 0;
    uint8_t route = 0;
    int32_t guest = 0;
    int32_t native_quantized = 0;
    int32_t guest_dry_a = 0;
    int32_t guest_dry_b = 0;
    int32_t native_dry_a = 0;
    int32_t native_dry_b = 0;
    int32_t native_post_a = 0;
    int32_t native_post_b = 0;
    int16_t guest_raw_a = 0;
    int16_t guest_raw_b = 0;
    uint32_t native_accumulator = 0;
    uint32_t guest_seed = 0;
    uint32_t native_seed = 0;
    uint32_t reverb_start_a = 0;
    uint32_t reverb_start_b = 0;
    uint32_t old_route_addr = 0;
    uint32_t guest_old_a = 0;
    uint32_t guest_old_b = 0;
    uint32_t native_old_a = 0;
    uint32_t native_old_b = 0;
    std::array<ProducerVoiceTrace, kMp2kMaxChans> voices{};
};

// A savestate can attach between guest writes and native renders. Comparison
// opens only after a complete producer block is present on both sides.
class ProducerStartupAlignment {
public:
    void reset() { ready_ = false; }
    bool observe(std::size_t guest_samples, std::size_t native_samples,
                 std::size_t expected_samples) {
        if (ready_) return true;
        if (expected_samples != 0 && guest_samples >= expected_samples &&
            native_samples >= expected_samples) ready_ = true;
        return ready_;
    }
    bool ready() const { return ready_; }

private:
    bool ready_ = false;
};

enum class Mp2kWallExportMode : uint8_t {
    FullProducerSeed,
    ZeroSeedMp2kOnly,
};

class Mp2kShadow {
public:
    enum class ProducerRejectReason : uint8_t {
        None = 0,
        SignedMetrics = 1,
        DmaPublish = 2,
    };
    enum class HookPhase { PreMix, PostMix };
    using PublishedBlockCallback = void (*)(void*, uint64_t);

    // Arm the shadow from detected MP2K signatures (SoundMainRAM hook keys).
    void init(const std::vector<Mp2kSig>& sigs);
    // Clear state derived from the guest audio timeline while retaining the
    // detected hook keys. Required at savestate boundaries.
    void reset_runtime();
    // Keep voice stepping aligned with the live GBA mixer sample rate. The
    // guest can change SOUNDBIAS after the shadow is armed.
    void set_render_rate(uint32_t sample_rate);
    bool armed() const { return hook_n_ > 0; }

    // True while the frontend should substitute the shadow stream: armed,
    // engaged (saw a live tick), and the verifier has proven a window.
    bool active() const { return active_; }
    bool proven() const { return vf_.proven(); }
    bool live() const { return active_ && engaged_ && vf_.proven(); }
    bool engaged() const { return engaged_; }
    uint64_t hooks() const { return hooks_; }
    uint64_t stale_ticks() const { return stale_ticks_; }
    uint64_t bad_waves() const { return bad_waves_; }
    const std::array<BadWaveTrace, 8>& bad_wave_traces() const {
        return bad_wave_traces_;
    }
    uint32_t bad_wave_trace_count() const { return bad_wave_trace_count_; }
    void annotate_bad_wave_writes(
        const std::array<CanonicalMixTrace::ChannelWrite, 64>& writes,
        uint32_t count);
    uint64_t producer_underruns() const { return host_stream_.underruns(); }
    const AudioTimelineStats& producer_dma_stats() const {
        return dma_timeline_.stats();
    }
    const AudioTimelineStats& producer_host_stats() const {
        return host_stream_.stats();
    }
    const VerifierSampleStats& verifier_sample_stats() const {
        return sample_stats_;
    }
    const ProducerDiffTrace& producer_diff_trace() const {
        return producer_diff_trace_;
    }
    const CanonicalMixTrace& canonical_mix_trace() const {
        return canonical_mix_trace_;
    }
    uint32_t snapshot_hook_pc() const { return producer_snapshot_hook_pc_; }
    uint8_t snapshot_hook_source() const {
        return producer_snapshot_hook_source_;
    }
    // Numeric producer/writer context supplied by GbaAudio immediately
    // before a hook. This is diagnostic-only and never exposes audio bytes.
    void set_producer_hook_context(uint64_t active_block_id,
                                   uint64_t pending_block_id,
                                   uint8_t completed_routes,
                                   uint32_t writer_count,
                                   uint32_t writer_bytes_max,
                                   uint64_t writer_completions,
                                   uint64_t c70_writer_calls,
                                   uint64_t c70_writer_completions);
    void set_snapshot_hook_identity(uint32_t pc, uint8_t source);
    void capture_canonical_mix_trace(const CanonicalMixTrace& trace);
    uint64_t producer_blocks_judged() const { return producer_blocks_judged_; }
    uint64_t producer_blocks_passed() const { return producer_blocks_passed_; }
    uint64_t published_producer_block() const { return published_block_id_; }
    uint64_t candidate_samples_ready() const { return candidate_samples_ready_; }
    uint64_t candidate_samples_missing() const {
        return candidate_samples_missing_;
    }
    uint64_t verifier_samples() const { return verifier_samples_; }
    uint64_t verifier_windows() const { return verifier_windows_; }
    // Optional, emulation-thread-only edge. Called once after a verified
    // producer block and its DMA timeline entry publish. Null by default.
    void set_published_block_callback(PublishedBlockCallback callback,
                                      void* context) {
        published_block_callback_ = callback;
        published_block_callback_context_ = context;
    }
    uint8_t reverb_mode() const { return reverb_; }
    // Producer metadata for bounded Stage-2 event capture. These values come
    // from the verified MP2K block setup, not from host sample timing.
    uint32_t producer_pcm_rate() const { return pcm_freq_; }
    uint32_t producer_frame_count() const { return producer_samples_; }
    uint64_t producer_block_id() const { return producer_block_id_; }
    uint64_t producer_block_start_cursor() const {
        return producer_block_start_cursor_;
    }
    uint64_t producer_blocks_rejected() const { return producer_blocks_rejected_; }
    uint64_t producer_blocks_incomplete() const { return producer_blocks_incomplete_; }
    uint64_t first_incomplete_block() const { return first_incomplete_block_; }
    uint64_t first_rejected_block() const { return first_rejected_block_; }
    uint64_t first_incomplete_cursor() const { return first_incomplete_cursor_; }
    uint32_t first_incomplete_guest_samples() const { return first_incomplete_guest_samples_; }
    uint32_t first_incomplete_native_samples() const { return first_incomplete_native_samples_; }
    uint32_t first_incomplete_expected_samples() const { return first_incomplete_expected_samples_; }
    bool first_incomplete_alignment_ready() const {
        return first_incomplete_alignment_ready_;
    }
    uint32_t first_incomplete_route_a() const { return first_incomplete_route_a_; }
    uint32_t first_incomplete_route_b() const { return first_incomplete_route_b_; }
    ProducerRejectReason first_rejected_reason() const {
        return first_rejected_reason_;
    }
    float first_rejected_correlation() const { return first_rejected_correlation_; }
    float first_rejected_level_ratio() const { return first_rejected_level_ratio_; }
    float first_rejected_mean_abs_error() const { return first_rejected_mean_abs_error_; }
    uint64_t first_rejected_cursor() const { return first_rejected_cursor_; }
    uint32_t first_rejected_route_a() const { return first_rejected_route_a_; }
    uint32_t first_rejected_route_b() const { return first_rejected_route_b_; }
    const ProducerDmaMiss& producer_dma_first_unmatched() const {
        return dma_timeline_.first_unmatched();
    }
    bool producer_startup_aligned() const { return producer_alignment_.ready(); }
    uint32_t probation_failures() const { return vf_.probation_failures(); }
    float last_correlation() const { return vf_.last_r(); }
    float last_level_ratio() const { return vf_.last_ratio(); }
    bool signed_gate_seen() const { return vf_.signed_gate_seen(); }
    bool signed_gate_passed() const { return vf_.signed_gate_passed(); }
    uint64_t authoritative_snapshots() const { return authoritative_snapshots_; }
    uint64_t wall_export_unsupported() const { return wall_export_unsupported_; }
    uint64_t wall_export_successes() const { return wall_export_successes_; }
    uint64_t wall_export_not_verified() const {
        return wall_export_not_verified_;
    }
    uint64_t wall_export_reverb_rejected() const {
        return wall_export_reverb_rejected_;
    }
    uint64_t wall_export_bad_asset() const { return wall_export_bad_asset_; }
    uint64_t wall_export_bad_sequence() const {
        return wall_export_bad_sequence_;
    }
    uint64_t wall_export_bad_rate() const { return wall_export_bad_rate_; }
    bool canonical_fallback() const { return canonical_fallback_; }
    void force_canonical_fallback(const char* reason) {
        canonical_fail(reason);
    }

    // Export only at the verified post-judge boundary. ZeroSeedMp2kOnly
    // snapshots voices/assets without requiring producer seed state; full
    // seed export remains for the future exact PSG/FIFO path. The wall mixer
    // copies all borrowed sample bytes before this call returns. Emulation
    // thread owns export; SDL/audio callbacks never call it.
    bool export_wall_snapshot(const MemView& mem, uint64_t sequence,
                              uint64_t guest_cursor_q32,
                              Mp2kWallSnapshotInput& out,
                              Mp2kWallExportMode mode =
                                  Mp2kWallExportMode::FullProducerSeed) const;

    // Per driver-tick hook: re-read the finalized channel state and mirror
    // the envelope step to derive this tick's gains + a one-tick prediction.
    // `audio_cursor` is a running grid-sample count; `key` is the detected
    // hook (used only to collapse multi-linked driver copies).
    void frame_hook(const MemView& mem, uint64_t audio_cursor,
                    uint32_t key, HookPhase phase = HookPhase::PreMix,
                    uint64_t block_id = UINT64_MAX,
                    uint64_t boundary_cycle = 0);

    // Capture the newly completed intermediate C70 producer block. Each guest
    // word holds signed A/B lanes; this never mutates guest memory. The paired
    // EWRAM route blocks are the FIFO/publication boundary when available.
    void producer_interleaved_block(const MemView& mem, uint32_t addr,
                                    uint32_t frames, uint64_t block_id);
    // Observe the packed four-sample history word immediately before the
    // guest overwrites it.  This seeds the native circular image after a
    // savestate and supplies the exact GS1 ring address/cadence.
    void reverb_history_word(uint8_t route, uint32_t addr,
                             uint32_t guest_old, uint64_t block_id);
    void diagnostic_dry_block(const MemView& mem, uint32_t addr,
                              uint32_t bytes, uint8_t route,
                              uint64_t block_id);
    void producer_dma_consume(uint8_t route, uint32_t source_addr,
                              uint64_t host_cursor, uint64_t cycles);

    // Check whether a RAM dispatch target is one of the detected
    // SoundMainRAM entry points. The game-specific runtime calls this before
    // dispatching every RAM target; unrelated generated RAM must not advance
    // the audio shadow timeline.
    bool matches_hook(uint32_t key) const;
    uint8_t hook_key_count() const { return hook_n_; }
    uint32_t hook_key_at(uint8_t index) const {
        return index < hook_n_ ? hook_keys_[index] : 0;
    }

    // Render one grid sample from the timestamped producer. The returned
    // routes remain normalized signed FIFO bytes; GbaAudio applies hardware
    // routing, volume, PSG, and SOUNDBIAS exactly once.
    bool render(const MemView& mem, uint64_t audio_cursor,
                float& route_a, float& route_b);
    void judge_output(uint64_t audio_cursor, float canon_left,
                      float canon_right, float candidate_left,
                      float candidate_right,
                      const Mp2kOutputRouteObservation& observation,
                      std::string& degraded);
    bool take_first_output_domain_diagnostic(
        Mp2kOutputDomainDiagnostic& out);
    const Mp2kProducerSourceDiagnostic& producer_source_diagnostic() const {
        return producer_source_diag_;
    }
    const Mp2kCursorDiagnostic& cursor_diagnostic() const {
        return producer_cursor_diag_;
    }
    const Mp2kVoiceContributionDiagnostic& voice_contribution_diagnostic() const {
        return producer_voice_diag_;
    }

    uint32_t hook_key0() const { return hook_n_ ? hook_keys_[0] : 0; }

private:
    friend struct Mp2kShadowWallFixture;
    friend struct GbaAudioTurboFixture;
    struct Voice {
        bool     on = false;
        // Golden Sun marks its three Camelot oscillator instruments with a
        // zero-length/zero-loop sample header.  0=PCM, 1=PWM, 2=saw, 3=tri.
        uint8_t  synth_kind = 0;
        uint8_t  synth_base = 0;
        uint8_t  synth_step = 0;
        uint8_t  synth_depth = 0;
        uint8_t  synth_init_duty = 0;
        uint8_t  ctype = 0;
        uint32_t wav = 0;
        uint32_t data = 0;
        uint32_t size = 0;
        // ROM sample payloads are immutable for a live MemView and can be
        // fetched directly. RAM voices keep data null and read guest memory
        // every time so wave-RAM mutation remains observable.
        const uint8_t* immutable_sample_ptr = nullptr;
        std::size_t immutable_sample_bytes = 0;
        uint32_t loop_start = 0;
        bool     looped = false;
        bool     compressed = false;
        bool     reversed = false;
        uint32_t pos_index = 0;
        uint32_t pos_frac = 0;
        uint32_t step_q23 = 0;
        double   pos = 0.0; // synth-only phase / diagnostic compatibility
        double   step = 0.0;
        double   synth_phase = 0.0;
        uint32_t synth_pos = 0;       // Camelot sawtooth accumulator
        uint32_t synth_duty_pos = 0;  // Camelot PWM modulation accumulator
        float    synth_duty = 0.0f;
        float    synth_duty_step = 0.0f;
        float    g0r = 0, g0l = 0;   // gains at tick start
        float    g1r = 0, g1l = 0;   // one-tick prediction
        uint32_t g0r_q9 = 0, g0l_q9 = 0;
        uint32_t g1r_q9 = 0, g1l_q9 = 0;
        uint32_t packed_gain_g0 = 0;
        bool     packed_gain_fix = false;
        bool     packed_gain_ordinary_pcm = false;
        uint32_t blk_idx = 0xFFFFFFFFu;
        int8_t   blk[64] = {};
        float    chk_hold = 0.0f;    // canon-domain check-copy hold
        int32_t  last_sample_x2 = 0; // guest slow-path Q1 interpolation
        int32_t  chk_sample_x2 = 0;
        double   chk_acc = 0.0;
    };

    bool note_on(Voice& v, const MemView& mem, uint8_t ctype, uint32_t count,
                 uint32_t wav);
    float sample_voice(Voice& v, const MemView& mem);
    float fetch(Voice& v, const MemView& mem, uint32_t idx);
    void canonical_fail(const char* reason);
    bool capture_voice_traces_enabled() const {
        return audio_probe_enabled_ ||
            (producer_good_probation_window_seen_ &&
             !producer_post_probation_reject_diag_logged_);
    }

    static constexpr double kRenderHz = 65536.0;
    static constexpr std::size_t kFrame = 1097;  // legacy allocation minimum

    uint32_t hook_keys_[4] = {};
    uint8_t  hook_n_ = 0;
    bool     active_ = true;
    bool     engaged_ = false;
    // Cached at init/reset boundaries. Hot render paths must not query the
    // process environment for every voice/sample.
    bool     audio_probe_enabled_ = false;
    uint64_t hooks_ = 0, stale_ticks_ = 0, bad_waves_ = 0;
    std::array<BadWaveTrace, 8> bad_wave_traces_{};
    uint32_t bad_wave_trace_write_ = 0;
    uint32_t bad_wave_trace_count_ = 0;
    uint64_t authoritative_snapshots_ = 0;
    mutable uint64_t wall_export_unsupported_ = 0;
    mutable uint64_t wall_export_successes_ = 0;
    mutable uint64_t wall_export_not_verified_ = 0;
    mutable uint64_t wall_export_reverb_rejected_ = 0;
    mutable uint64_t wall_export_bad_asset_ = 0;
    mutable uint64_t wall_export_bad_sequence_ = 0;
    mutable uint64_t wall_export_bad_rate_ = 0;
    uint64_t last_premix_id_ = UINT64_MAX;
    uint64_t last_postmix_id_ = UINT64_MAX;
    std::array<Voice, kMp2kMaxChans> voices_{};
    // Reused diagnostic storage. Populated/copied only when the probe is on.
    std::array<ProducerVoiceTrace, kMp2kMaxChans> producer_voice_traces_{};
    uint32_t env_pos_ = 0;
    uint32_t env_span_ = static_cast<uint32_t>(kFrame);
    uint64_t last_hook_cursor_ = 0;
    uint8_t  reverb_ = 0;
    uint8_t  dma_period_ = 7;
    std::vector<std::pair<float, float>> ring_ =
        std::vector<std::pair<float, float>>(kFrame * 16, {0.0f, 0.0f});
    std::size_t ring_pos_ = 0;
    std::size_t reverb_ring_len_ = kFrame * 16;
    uint64_t reverb_write_count_ = 0;
    uint64_t reverb_delay_q32_ = 0;
    ShadowVerifier vf_{};
    uint32_t mode_dwell_ = 0;
    uint32_t debug_samples_ = 0;
    uint32_t state_delay_frames_ = 0;
    uint32_t pcm_freq_ = 13379;
    uint32_t spv_ = 224;
    uint32_t render_rate_ = 65536;
    double   mix_step_ = 65536.0 / 13379.0;

    // One native check block is accumulated at the guest PCM cadence and is
    // matched only when both newly written A/B producer blocks arrive.
    uint64_t producer_block_id_ = UINT64_MAX;
    uint32_t producer_samples_ = 0;
    double producer_phase_ = 0.0;
    bool producer_block_first_render_ = true;
    bool producer_block_incomplete_reported_ = false;
    uint64_t producer_block_start_cursor_ = 0;
    uint64_t published_block_id_ = UINT64_MAX;
    bool canonical_fallback_ = false;
    // A savestate or startup can leave producer epochs with only a native
    // prefix. Discard unverified epochs until a complete guest/native pair
    // establishes owned state for the next live SoundMain snapshot.
    bool startup_realign_pending_ = true;
    PublishedBlockCallback published_block_callback_ = nullptr;
    void* published_block_callback_context_ = nullptr;
    ProducerStartupAlignment producer_alignment_{};
    bool producer_reverb_history_aligned_ = false;
    ProducerHostResampler host_stream_{};
    ProducerDmaTimeline dma_timeline_{};
    struct PublishedNativeBlock {
        uint64_t sequence = UINT64_MAX;
        std::vector<float> route_a;
        std::vector<float> route_b;
    };
    std::deque<PublishedNativeBlock> published_native_;
    std::vector<float> native_block_l_;
    std::vector<float> native_block_r_;
    std::vector<int8_t> producer_a_;
    std::vector<int8_t> producer_b_;
    struct ProducerSampleState {
        float dry_right = 0.0f;
        float dry_left = 0.0f;
        float old_right = 0.0f;
        float old_left = 0.0f;
        uint64_t reverb_write = 0;
        uint32_t active_voices = 0;
        uint32_t first_voice = UINT32_MAX;
        uint8_t first_ctype = 0;
        uint32_t first_wave = 0;
        uint32_t first_resolved_wave = 0;
        uint32_t first_pos = 0;
        uint32_t first_frac = 0;
        uint32_t first_step = 0;
        uint32_t first_pre_voice = UINT32_MAX;
        uint32_t first_pre_pos = 0;
        uint32_t first_pre_frac = 0;
        uint32_t first_pre_step = 0;
        float first_gain_right = 0.0f;
        float first_gain_left = 0.0f;
        int32_t first_sample = 0;
        int32_t first_gain_right_q9 = 0;
        int32_t first_gain_left_q9 = 0;
        int32_t dry_right_q15 = 0;
        int32_t dry_left_q15 = 0;
        int32_t old_right_q15 = 0;
        int32_t old_left_q15 = 0;
        int32_t post_right_q15 = 0;
        int32_t post_left_q15 = 0;
        uint32_t old_route_addr = 0;
        uint32_t old_route_a = 0;
        uint32_t old_route_b = 0;
        uint32_t native_old_route_a = 0;
        uint32_t native_old_route_b = 0;
    };
    struct ReverbRejectGroupDiag {
        uint32_t sample_index = 0;
        uint32_t accumulator_hash = 0;
        uint32_t input_hash = 0;
        uint32_t output_hash = 0;
        uint32_t guest_old_hash[2]{};
        uint32_t native_old_hash[2]{};
        uint32_t guest_new_hash[2]{};
        uint32_t native_new_hash[2]{};
    };
    std::vector<ProducerSampleState> native_block_state_;
    // Per-sample trace storage exists only for the opt-in diagnostic probe;
    // verifier/native state above stays scalar and probe-independent.
    std::vector<std::array<ProducerVoiceTrace, kMp2kMaxChans>>
        native_block_voice_traces_;
    std::vector<float> previous_native_l_;
    std::vector<float> previous_native_r_;
    std::vector<int8_t> previous_producer_a_;
    std::vector<int8_t> previous_producer_b_;
    std::vector<int8_t> dry_guest_a_;
    std::vector<int8_t> dry_guest_b_;
    std::vector<int16_t> producer_raw_a_;
    std::vector<int16_t> producer_raw_b_;
    std::vector<uint32_t> rolling_seed_words_;
    std::vector<uint32_t> current_seed_words_;
    std::vector<uint32_t> native_accumulator_words_;
    std::array<std::unordered_map<uint32_t, uint32_t>, 2>
        guest_reverb_old_words_;
    std::array<std::vector<uint32_t>, 2> guest_reverb_new_words_;
    std::array<uint32_t, 2> reverb_block_start_{};
    std::array<std::unordered_map<uint32_t, uint32_t>, 2>
        native_reverb_words_;
    uint64_t previous_block_id_ = UINT64_MAX;
    uint32_t producer_route_addr_[2] = {};
    bool producer_diag_block_logged_ = false;
    bool producer_diag_diff_logged_ = false;
    bool producer_diag_mature_logged_ = false;
    bool producer_diag_mature_diff_logged_ = false;
    bool producer_sequence_gap_logged_ = false;
    bool producer_source_diag_logged_ = false;
    Mp2kProducerSourceDiagnostic producer_source_diag_{};
    bool producer_post_probation_reject_diag_logged_ = false;
    bool producer_good_probation_window_seen_ = false;
    std::array<uint64_t, 2> dry_guest_block_id_{{UINT64_MAX, UINT64_MAX}};
    uint32_t producer_startup_diag_count_ = 0;
    // Bounded verifier diagnostics cover the first few incomplete/aligned
    // blocks without adding output to the normal hot path.
    uint32_t producer_verifier_diag_count_ = 0;
    uint64_t producer_verifier_metric_block_id_ = UINT64_MAX;
    bool producer_reverb_reject_diag_logged_ = false;
    bool producer_reverb_branch_diag_logged_ = false;
    uint64_t producer_context_active_block_id_ = UINT64_MAX;
    uint64_t producer_context_pending_block_id_ = UINT64_MAX;
    uint8_t producer_context_completed_routes_ = 0;
    uint32_t producer_context_writer_count_ = 0;
    uint32_t producer_context_writer_bytes_max_ = 0;
    uint64_t producer_context_writer_completions_ = 0;
    uint64_t producer_context_c70_writer_calls_ = 0;
    uint64_t producer_context_c70_writer_completions_ = 0;
    VerifierSampleStats sample_stats_{};
    ProducerDiffTrace producer_diff_trace_{};
    CanonicalMixTrace canonical_mix_trace_{};
    // Latest canonical block-boundary snapshot for the bounded cursor
    // discriminator. It contains metadata/addresses only, never PCM bytes.
    CanonicalMixTrace producer_guest_snapshot_{};
    Mp2kCursorDiagnostic producer_cursor_diag_{};
    Mp2kVoiceContributionDiagnostic producer_voice_diag_{};
    uint64_t producer_snapshot_cursor_ = 0;
    uint64_t producer_snapshot_block_ = UINT64_MAX;
    uint32_t producer_snapshot_sound_info_ = 0;
    uint8_t producer_snapshot_active_voices_ = 0;
    uint32_t producer_snapshot_hook_pc_ = 0;
    uint8_t producer_snapshot_hook_source_ = 0;
    uint8_t producer_snapshot_hook_phase_ = 0;
    std::array<uint16_t, 8> producer_snapshot_channel_status_{};
    bool have_previous_verifier_sample_ = false;
    float previous_canon_left_ = 0.0f;
    float previous_canon_right_ = 0.0f;
    float previous_native_left_ = 0.0f;
    float previous_native_right_ = 0.0f;
    uint64_t producer_blocks_judged_ = 0;
    uint64_t producer_blocks_passed_ = 0;
    uint64_t producer_blocks_rejected_ = 0;
    uint64_t producer_blocks_incomplete_ = 0;
    uint64_t candidate_samples_ready_ = 0;
    uint64_t candidate_samples_missing_ = 0;
    uint64_t verifier_samples_ = 0;
    uint64_t verifier_windows_ = 0;
    struct OutputDomainAccum {
        uint64_t samples = 0;
        uint64_t missing = 0;
        uint64_t first_cursor = 0;
        uint64_t last_cursor = 0;
        bool have_cursor = false;
        std::array<double, 2> canonical_sum{};
        std::array<double, 2> canonical_sum_sq{};
        std::array<double, 2> canonical_peak{};
        std::array<uint64_t, 2> canonical_nonnegative{};
        std::array<uint64_t, 2> canonical_negative{};
        std::array<double, 2> native_sum{};
        std::array<double, 2> native_sum_sq{};
        std::array<double, 2> native_peak{};
        std::array<uint64_t, 2> native_nonnegative{};
        std::array<uint64_t, 2> native_negative{};
        std::array<double, 2> route_sum{};
        std::array<double, 2> route_sum_sq{};
        std::array<double, 2> route_peak{};
        std::array<uint64_t, 2> route_nonnegative{};
        std::array<uint64_t, 2> route_negative{};
        std::array<double, 2> fifo_sum{};
        std::array<double, 2> fifo_sum_sq{};
        std::array<double, 2> fifo_peak{};
        std::array<uint64_t, 2> fifo_nonnegative{};
        std::array<uint64_t, 2> fifo_negative{};
        uint8_t route_mask = 0;
        uint8_t full_volume_mask = 0;
        uint32_t route_mapping_changes = 0;
        int32_t psg_left = 0;
        int32_t psg_right = 0;
        uint16_t soundbias = 0;
    } output_domain_accum_{};
    Mp2kOutputDomainDiagnostic first_output_domain_diag_{};
    bool first_output_domain_diag_ready_ = false;
    bool first_output_domain_diag_consumed_ = false;
    uint64_t first_incomplete_block_ = UINT64_MAX;
    uint64_t first_rejected_block_ = UINT64_MAX;
    uint64_t first_incomplete_cursor_ = 0;
    uint32_t first_incomplete_guest_samples_ = 0;
    uint32_t first_incomplete_native_samples_ = 0;
    uint32_t first_incomplete_expected_samples_ = 0;
    bool first_incomplete_alignment_ready_ = false;
    uint32_t first_incomplete_route_a_ = 0;
    uint32_t first_incomplete_route_b_ = 0;
    float first_rejected_correlation_ = 0.0f;
    float first_rejected_level_ratio_ = 0.0f;
    float first_rejected_mean_abs_error_ = 0.0f;
    uint64_t first_rejected_cursor_ = 0;
    uint32_t first_rejected_route_a_ = 0;
    uint32_t first_rejected_route_b_ = 0;
    ProducerRejectReason first_rejected_reason_ =
        ProducerRejectReason::None;

    void reset_producer_block(uint64_t block_id, uint64_t audio_cursor);
    void judge_producer_block(uint64_t block_id);
    void log_producer_verifier_diag(
        const char* stage, uint64_t block_id, std::size_t guest_a,
        std::size_t guest_b, std::size_t native_l, std::size_t native_r,
        bool alignment_ready, bool evaluated, bool pass, float correlation,
        float level_ratio);
    void log_reverb_reject_diagnostic(
        uint64_t block_id, std::size_t n,
        const SignedStereoMetrics& metrics,
        const std::array<ReverbRejectGroupDiag, 2>& groups,
        uint32_t group_count);
    void log_post_probation_reject_diagnostic(
        uint64_t block_id, std::size_t n,
        const SignedStereoMetrics& metrics);
    void maybe_log_producer_source_diagnostic(bool accepted);
};

// Camelot's live channel pointer is the authoritative PCM cursor for both
// ordinary and fixed-rate voices. These small helpers are deliberately pure
// so the archive-offset and synth-payload rules can be tested without a ROM.
bool mp2k_cursor_index(uint32_t data, uint32_t size, uint32_t cp,
                       uint32_t& index);
constexpr uint32_t kMp2kFracBits = 23;
constexpr uint32_t kMp2kFracMask = (1u << kMp2kFracBits) - 1u;
constexpr uint32_t kMp2kFracOne = 1u << kMp2kFracBits;

uint32_t mp2k_step_q23(uint32_t source_rate, uint32_t render_rate);
float mp2k_synth_duty_threshold(uint32_t value, uint8_t base, uint8_t depth,
                                uint8_t initial_duty);
float mp2k_synth_render_sample(uint8_t kind, double& phase, double step,
                               uint32_t& synth_pos, float& duty,
                               float duty_step);
int32_t mp2k_interpolate_s8(int32_t s0, int32_t s1, uint32_t frac);
int32_t mp2k_interpolate_s8_x2(int32_t s0, int32_t s1, uint32_t frac);
bool mp2k_check_hold_refresh(bool block_start, double mix_step,
                             double& accumulator);
uint32_t mp2k_gain_q9(uint32_t volume, uint32_t envelope);
uint32_t mp2k_pack_stereo_gain(uint32_t right_q9, uint32_t left_q9);
uint32_t mp2k_pack_fix_gain(uint32_t right_q9, uint32_t left_q9);
uint32_t mp2k_packed_accumulate(uint32_t accumulator, uint32_t packed_gain,
                                int32_t sample);
bool mp2k_gs1_dry_route(uint32_t writer_pc, uint8_t& route);
uint32_t mp2k_reverb_word_address(uint32_t block_start,
                                  std::size_t group_index);

// A fresh note-on owns its note_on-derived phase until the guest mixer writes
// the live cursor. Existing voices must continue to validate that cursor.
constexpr bool mp2k_should_validate_live_cursor(bool freshly_initialized,
                                                bool synth_voice) {
    return !freshly_initialized && !synth_voice;
}
uint32_t mp2k_saturate_packed_lanes(uint32_t accumulator);
void mp2k_extract_dry_words(const std::array<uint32_t, 4>& accumulators,
                            uint32_t& route_a, uint32_t& route_b);
void mp2k_extract_dry_sample(uint32_t accumulator, int8_t& route_a,
                             int8_t& route_b);
std::array<uint32_t, 4> mp2k_finalize_producer_group(
    const std::array<uint32_t, 4>& accumulators,
    uint32_t old_route_a, uint32_t old_route_b);
void mp2k_sync_reverb_history(
    const std::unordered_map<uint32_t, uint32_t>& canonical,
    std::unordered_map<uint32_t, uint32_t>& native);
bool mp2k_advance_cursor(uint32_t& index, uint32_t& frac,
                         uint32_t step_q23, uint32_t size,
                         uint32_t loop_start, bool looped);

struct Mp2kReverbSample {
    int32_t right = 0;
    int32_t left = 0;
};

Mp2kReverbSample mp2k_gs1_reverb_mix(int32_t dry_right, int32_t dry_left,
                                      int32_t old_right, int32_t old_left);
void mp2k_decode_producer_word(uint32_t packed, int8_t& route_a,
                               int8_t& route_b);
uint32_t mp2k_resolve_wave_address(const MemView& mem, uint32_t addr,
                                   std::size_t len);

struct Mp2kSynthParams {
    uint8_t kind = 0;
    uint8_t base = 0;
    uint8_t step = 0;
    uint8_t depth = 0;
    uint8_t initial_duty = 0;
};

bool mp2k_decode_synth_payload(const uint8_t* payload, std::size_t len,
                               Mp2kSynthParams& out);
bool mp2k_decode_dpcm_block(const uint8_t* block, std::size_t len,
                            std::array<int8_t, 64>& out);

}  // namespace gba
