// gba_audio.h — GBA sound channels + sample mixer.
//
// Implemented and mixed into the output: SOUND1 (square + sweep),
// SOUND2 (square), SOUND3 (wave RAM), SOUND4 (noise), and the
// DMA-driven Direct Sound A / B FIFOs (the streamed-PCM path the
// MP2K/m4a music driver uses) — fed via timer_overflow() + DMA refill,
// mixed in mix_one_sample(). The mixer generates 16-bit signed samples;
// the rate follows SOUNDBIAS resolution (32768 Hz default, 65536 Hz
// when the BIOS raises it).
//
// SOUND3/SOUND4 drive many sound EFFECTS (door open, bumps, menu blips,
// percussion); they were previously decoded but silent, so those SFX
// were missing. Now implemented per GBATEK. Audio correctness vs the
// mGBA oracle is validated separately (differential audio sweeps).
//
// References:
//   GBATEK § "GBA Sound Channel 2 - Tone"
//   GBATEK § "GBA Sound Channels 1-4 — Tone Generation"
//   GBATEK § "GBA Sound Controller" (SOUNDCNT_L/H/X master)

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "gba_m4a.h"
#include "audio_event_capture.h"
#include "mp2k_shadow.h"
#include "mp2k_wall_mixer.h"
#include "mp2k_wall_update_queue.h"
#include "turbo_audio_scheduler.h"

namespace gbarecomp::debug { class SnapshotWriter; class SnapshotReader; }

namespace gba {

struct DirectSoundRouteState {
    bool a_left = false;
    bool a_right = false;
    bool a_full_volume = false;
    bool b_left = false;
    bool b_right = false;
    bool b_full_volume = false;
    uint16_t soundbias = 0x0200;
};

// Output is in the signed 16-bit host domain before final integer packing.
// route_a/route_b are normalized signed FIFO bytes: byte / 128.0f.
struct DirectSoundOutput {
    float left = 0.0f;
    float right = 0.0f;
};

DirectSoundOutput mix_direct_sound_output(
    float route_a, float route_b, int32_t psg_left, int32_t psg_right,
    const DirectSoundRouteState& state);

// All proven Camelot post-mix writer PCs. C70 has a dedicated state slot;
// the separate registry below is reserved for the other known/dynamic PCs.
inline constexpr std::array<uint32_t, 5> kMp2kKnownWriterPcs = {
    0x030008B4u, 0x03000A8Cu, 0x03000BCCu, 0x03000BD4u, 0x03000C70u,
};
inline constexpr std::array<uint32_t, 4> kMp2kKnownNonC70WriterPcs = {
    0x030008B4u, 0x03000A8Cu, 0x03000BCCu, 0x03000BD4u,
};

// Cached MP2K observer PCs.  This is intentionally a small POD copied/read
// by the per-instruction runtime path; invalid state fails open.
struct Mp2kHookFastFilter {
    bool valid = false;
    bool frame_enabled = false;
    bool control_enabled = false;
    uint8_t premix_count = 0;
    uint8_t postmix_count = 0;
    uint32_t premix_pc[4] = {};
    uint32_t postmix_pc[9] = {};
    uint32_t control_pc = 0;

    bool frame_may_be_relevant(uint32_t key) const {
        if (!valid) return true;
        if (!frame_enabled) return false;
        const uint32_t normalized = key & ~1u;
        for (uint8_t i = 0; i < premix_count; ++i)
            if ((premix_pc[i] & ~1u) == normalized) return true;
        return false;
    }

    bool postmix_may_be_relevant(uint32_t pc) const {
        if (!valid) return true;
        if (!frame_enabled) return false;
        const uint32_t normalized = pc & ~1u;
        for (uint8_t i = 0; i < postmix_count; ++i)
            if ((postmix_pc[i] & ~1u) == normalized) return true;
        return false;
    }

    // Only PC is filtered here. Target/mode/state checks stay in the deep
    // control hook so this gate cannot reject a future valid dispatch shape.
    bool control_may_be_relevant(uint32_t pc) const {
        if (!valid) return true;
        return control_enabled && (pc & ~1u) == (control_pc & ~1u);
    }
};

// Raw per-channel PSG samples (pre-scale, hardware-domain amplitude) for
// one mix step. Same units for all four channels.
struct PsgChannelSamples {
    int32_t ch1 = 0;
    int32_t ch2 = 0;
    int32_t ch3 = 0;
    int32_t ch4 = 0;
};

// SOUND1-4 left/right routing (SOUNDCNT_L PSG-to-side enable bits).
struct PsgChannelRouting {
    bool ch1_left = false;
    bool ch1_right = false;
    bool ch2_left = false;
    bool ch2_right = false;
    bool ch3_left = false;
    bool ch3_right = false;
    bool ch4_left = false;
    bool ch4_right = false;
};

struct PsgMixResult {
    int32_t left = 0;
    int32_t right = 0;
};

// Combines the four PSG channels into left/right totals the way mGBA's
// GBAAudioSamplePSG does: ch1+ch2+ch3 are summed and the combined sum is
// shifted left by 3; ch4 is pre-shifted left by 3 and added separately
// (third_party/mgba/src/gb/audio.c:752-763). Left shift distributes over
// addition, so scaling each channel by <<3 individually before summing
// (as done here) is arithmetically identical to mGBA's sum-then-shift and
// gives all four channels the same ×8 scale. Does not apply the DMG
// volume-ratio shift or SOUNDCNT_L master volume multiply — callers in
// GbaAudio::mix_one_sample apply those to the result afterward.
PsgMixResult mix_psg_samples(const PsgChannelSamples& samples,
                             const PsgChannelRouting& routing);

class GbaAudio {
public:
    GbaAudio();
    ~GbaAudio();

    // GBA runs at 16.777216 MHz; we sample at exactly 16777216 / 512 =
    // 32768 Hz so timing math is integer-clean.
    static constexpr uint32_t kSystemHz       = 16777216u;
    static constexpr uint32_t kDefaultSampleRate = 32768u;
    static constexpr uint32_t kDefaultCyclesPerSample =
        kSystemHz / kDefaultSampleRate;  // 512
    static constexpr uint32_t kSampleEventCycles = 1024u;
    static constexpr uint32_t kMaxSamplesPerEvent = 16u;

    // Reset all channel state. Called by the bus on power-on.
    void reset();

    // Write to an audio IO register. `off` is relative to 0x04000000
    // (so SOUND1CNT_L is 0x060, SOUND2CNT_L is 0x068, etc.). The
    // backing IO array still stores the byte; this method updates
    // the channel state machines for side effects (envelope reload,
    // length reset, frequency re-trigger).
    void write_io8 (uint32_t off, uint8_t  v, uint64_t cycles = 0);
    void write_io16(uint32_t off, uint16_t v, uint64_t cycles = 0);
    void write_io32(uint32_t off, uint32_t v, uint64_t cycles = 0);

    // Advance the audio subsystem by `cycles` system cycles.
    // Generates samples into the output ring whenever the cycle
    // accumulator crosses the active SOUNDBIAS sample interval.
    void tick(uint32_t cycles);
    uint32_t cycles_until_next_sample() const;

    // Drain up to `max` samples from the ring into `out`. Returns
    // the number actually written. Used by the host audio backend
    // and by the TCP audio_samples command.
    std::size_t drain_samples(int16_t* out, std::size_t max);
    // Opt-in native MP2K output: interleaved stereo frames at the engine rate.
    std::size_t drain_native_samples(int16_t* out, std::size_t max);
    // Canonical stereo fallback used while an opt-in native session is still
    // under probation (or after it fails). The guest/hardware state remains
    // canonical; this only preserves the two Direct Sound output buses.
    std::size_t drain_stereo_fallback_samples(int16_t* out, std::size_t max);

    uint32_t sample_rate() const { return kSystemHz / cycles_per_sample_; }

    // Total samples generated since reset — useful for sync diff.
    uint64_t samples_generated() const { return samples_generated_; }

    // ── Always-on, non-destructive audio capture ring (observability) ──
    // The playback FIFO (ring_) is consume-once and is drained by BOTH the
    // SDL host pump and the legacy audio_samples TCP cmd (they compete). THIS
    // ring is never drained: it records every generated mixed sample plus the
    // raw per-channel PSG and Direct-Sound FIFO ([-128,127])
    // contributions, keyed by the absolute samples_generated_ index. Probes
    // QUERY a [start,count] window backward from the live head — never arm,
    // never drain (project ring-buffer discipline). Sample-generation is
    // cycle-deterministic, so PSG channels are bit-reproducible across runs.
    struct CapSample {
        bool canonical_valid = false;
        uint32_t sample_rate = 0;
        int16_t mixed = 0;     // final mono mixer output (what playback hears)
        int16_t ch[4] = {};    // SOUND1..4 raw hardware-domain synthesis value
        int16_t direct_a = 0;  // Direct-Sound FIFO A raw byte for this slot
        int16_t direct_b = 0;  // Direct-Sound FIFO B raw byte for this slot
        int32_t psg_left_input = 0;  // pre-bias PSG input for this slot
        int32_t psg_right_input = 0;
        DirectSoundRouteState direct_route{};
    };
    static constexpr std::size_t kCapRingSize = 1u << 18;  // 262144 (~8 s @32k)

    // Earliest absolute sample index still retained in the capture ring.
    uint64_t capture_oldest_index() const {
        return samples_generated_ > kCapRingSize
                   ? samples_generated_ - kCapRingSize
                   : 0;
    }
    // Copy up to `count` capture samples starting at absolute index `start`
    // (clamped into the retained window) into `out`. Returns the number
    // copied; sets `out_first` to the absolute index of out[0]. Non-mutating.
    std::size_t query_capture(uint64_t start, std::size_t count,
                              CapSample* out, uint64_t& out_first) const;

    // Compose a wall-clock MP2K Direct Sound candidate with one synchronized
    // canonical tap. The normal replacement mode omits the captured FIFO
    // bytes because those bytes are the MP2K buses being replaced. The
    // separate-FIFO mode is explicit and remains available only for a proven
    // independent FIFO source. No host output ownership is changed here.
    static bool compose_mp2k_with_canonical(
        float route_a, float route_b, const CapSample& canonical,
        bool include_separate_fifo, int16_t& left, int16_t& right);

    struct FifoDebugState {
        uint8_t write = 0;
        uint8_t read = 0;
        uint8_t count = 0;
        uint32_t shift_word = 0;
        uint8_t bytes_remaining = 0;
        int8_t samples[kMaxSamplesPerEvent] = {};
    };
    FifoDebugState debug_fifo_state(int fifo_id) const;
    uint32_t debug_cycle_accumulator() const { return cycle_accumulator_; }
    uint32_t debug_samples_per_event() const { return samples_per_event(); }
    uint32_t debug_cycles_until_event() const {
        return cycles_until_next_sample_event();
    }
    uint16_t debug_soundbias() const { return soundbias_; }

    struct FifoTrace {
        uint64_t sample_base = 0;
        uint8_t fifo_id = 0;
        uint32_t until_cycles = 0;
        uint32_t start_slot = 0;
        uint32_t slots = 0;
        uint8_t count = 0;
        uint8_t bytes_remaining = 0;
        int8_t sample = 0;
    };
    static constexpr uint32_t kFifoTraceSize = 1024u;
    uint32_t debug_trace_count() const { return trace_count_; }
    FifoTrace debug_trace_entry(uint32_t index) const;

    // Timer overflow hook for direct sound FIFO A/B. `timer_id` is
    // 0 or 1; SOUNDCNT_H selects which timer clocks each FIFO.
    void timer_overflow(int timer_id, uint64_t cycles = 0);
    bool fifo_needs_dma(int fifo_id) const;
    void sound_fifo_dma_word(int fifo_id, uint32_t source_addr,
                             uint64_t cycles);
    // Capture the copied DMA word after the canonical bus transfer. This is
    // observability only; sound_fifo_dma_word above keeps its old ordering.
    void capture_sound_fifo_dma_word(int fifo_id, uint32_t source_addr,
                                     uint32_t value, uint64_t cycles);
    // GbaIo brackets the canonical FIFO destination store during DMA so the
    // MMIO write is not mislabeled as a CPU-produced FIFO word.
    void set_sound_fifo_dma_write_context(bool active) {
        sound_fifo_dma_write_context_ = active;
    }

    // Stage-2 capture is opt-in and emulation-thread-owned. It is diagnostic/
    // future PSG observability only; the native MP2K Turbo path does not arm
    // it. The SDL callback never accesses this queue; live capture wiring is
    // a later stage.
    void set_event_capture_enabled(bool enabled) {
        event_capture_.set_enabled(enabled);
    }
    bool event_capture_enabled() const { return event_capture_.enabled(); }
    bool event_capture_allocation_failed() const {
        return event_capture_.allocation_failed();
    }
    void reset_event_capture() { event_capture_.reset(); }
    AudioEventCapture& event_capture() { return event_capture_; }
    const AudioEventCapture& event_capture() const { return event_capture_; }

    // Arm the MP2K HLE shadow mixer (QoL — verified-enhancement HLE, see
    // PRINCIPLES.md). `sigs` come from mp2k_detect over the ROM; the region
    // pointers back the shadow's side-effect-free MemView. Off unless the
    // ROM links MP2K *and* GBARECOMP_AUDIO_SHADOW is set; canon mix is
    // unchanged and remains the verify oracle when disabled. The bus calls
    // this from set_rom.
    void configure_shadow(const std::vector<Mp2kSig>& sigs,
                          const uint8_t* rom, std::size_t rom_len,
                          const uint8_t* ewram, std::size_t ewram_len,
                          const uint8_t* iwram, std::size_t iwram_len,
                          bool enable_request, bool native_request = false);
    const Mp2kHookFastFilter& mp2k_hook_fast_filter() const {
        return mp2k_hook_filter_;
    }
    bool mp2k_frame_hook_may_be_relevant(uint32_t key) const {
        return mp2k_hook_filter_.frame_may_be_relevant(key);
    }
    bool mp2k_control_hook_may_be_relevant(uint32_t pc) const {
        return mp2k_hook_filter_.control_may_be_relevant(pc);
    }
    bool shadow_live() const { return shadow_enabled_ && shadow_.live(); }
    bool shadow_enabled() const { return shadow_enabled_; }
    uint64_t shadow_hooks() const { return shadow_.hooks(); }
    uint64_t shadow_stale_ticks() const { return shadow_.stale_ticks(); }
    uint64_t shadow_bad_waves() const { return shadow_.bad_waves(); }
    uint64_t shadow_producer_underruns() const {
        return shadow_.producer_underruns();
    }
    const AudioTimelineStats& shadow_dma_stats() const {
        return shadow_.producer_dma_stats();
    }
    const AudioTimelineStats& shadow_host_stats() const {
        return shadow_.producer_host_stats();
    }
    const VerifierSampleStats& shadow_sample_stats() const {
        return shadow_.verifier_sample_stats();
    }
    const ProducerDiffTrace& shadow_producer_diff_trace() const {
        return shadow_.producer_diff_trace();
    }
    const CanonicalMixTrace& shadow_canonical_mix_trace() const {
        return shadow_.canonical_mix_trace();
    }
    const std::array<BadWaveTrace, 8>& shadow_bad_wave_traces() const {
        return shadow_.bad_wave_traces();
    }
    uint32_t shadow_bad_wave_trace_count() const {
        return shadow_.bad_wave_trace_count();
    }
    const std::array<CanonicalBoundaryTrace, 16>&
    shadow_canonical_boundaries() const { return mp2k_boundary_ring_; }
    uint32_t shadow_canonical_boundary_count() const {
        return mp2k_boundary_count_;
    }
    uint32_t shadow_canonical_boundary_write() const {
        return mp2k_boundary_write_;
    }
    uint64_t shadow_producer_blocks_judged() const {
        return shadow_.producer_blocks_judged();
    }
    uint64_t shadow_producer_blocks_rejected() const {
        return shadow_.producer_blocks_rejected();
    }
    uint64_t shadow_producer_blocks_incomplete() const {
        return shadow_.producer_blocks_incomplete();
    }
    bool shadow_producer_startup_aligned() const {
        return shadow_.producer_startup_aligned();
    }
    uint64_t shadow_first_incomplete_block() const {
        return shadow_.first_incomplete_block();
    }
    uint64_t shadow_first_rejected_block() const {
        return shadow_.first_rejected_block();
    }
    uint64_t shadow_first_incomplete_cursor() const {
        return shadow_.first_incomplete_cursor();
    }
    uint32_t shadow_first_incomplete_guest_samples() const {
        return shadow_.first_incomplete_guest_samples();
    }
    uint32_t shadow_first_incomplete_native_samples() const {
        return shadow_.first_incomplete_native_samples();
    }
    uint32_t shadow_first_incomplete_expected_samples() const {
        return shadow_.first_incomplete_expected_samples();
    }
    uint32_t shadow_first_incomplete_route_a() const {
        return shadow_.first_incomplete_route_a();
    }
    uint32_t shadow_first_incomplete_route_b() const {
        return shadow_.first_incomplete_route_b();
    }
    float shadow_first_rejected_correlation() const {
        return shadow_.first_rejected_correlation();
    }
    float shadow_first_rejected_level_ratio() const {
        return shadow_.first_rejected_level_ratio();
    }
    float shadow_first_rejected_mean_abs_error() const {
        return shadow_.first_rejected_mean_abs_error();
    }
    uint64_t shadow_first_rejected_cursor() const {
        return shadow_.first_rejected_cursor();
    }
    uint32_t shadow_first_rejected_route_a() const {
        return shadow_.first_rejected_route_a();
    }
    uint32_t shadow_first_rejected_route_b() const {
        return shadow_.first_rejected_route_b();
    }
    bool shadow_engaged() const { return shadow_.engaged(); }
    float shadow_correlation() const { return shadow_.last_correlation(); }
    float shadow_level_ratio() const { return shadow_.last_level_ratio(); }
    bool native_audio_enabled() const { return native_audio_enabled_; }
    bool native_audio_requested() const { return native_audio_requested_; }
    // Normal native output becomes host-live on the first successfully
    // composed wall frame. The guest mixer keeps running as the oracle.
    bool native_audio_live() const {
        if (native_audio_requested_)
            return native_audio_enabled_ && native_wall_output_live_;
        return native_audio_enabled_ && shadow_.live();
    }
    bool native_wall_candidate_active() const {
        return native_wall_candidate_active_;
    }
    bool native_wall_candidate_failed() const {
        return native_wall_candidate_failed_;
    }
    uint64_t native_wall_candidate_frames() const {
        return native_wall_candidate_frames_;
    }
    uint64_t native_wall_composed_frames() const {
        return native_wall_composed_frames_;
    }
    std::size_t native_audio_ring_size() const;
    const char* native_audio_status_reason() const {
        if (!native_audio_requested_) return "";
        if (native_audio_live()) return "live";
        if (native_wall_candidate_active_)
            return native_wall_composed_frames_ != 0
                ? "native candidate pending host handoff"
                : "native candidate awaiting canonical tap";
        if (native_wall_candidate_failed_)
            return native_wall_failure_reason_[0]
                ? native_wall_failure_reason_ : "wall candidate failed";
        if (!shadow_.armed()) return "shadow not armed";
        if (shadow_.probation_failures() != 0) return "probation failed";
        if (!shadow_.engaged()) return "waiting for MP2K";
        return "probation pending";
    }
    // Experimental, default-off Turbo wall-clock MP2K path. Runtime owns all
    // calls on the emulation thread; SDL only drains the native ring.
    bool turbo_audio_decoupled_requested() const {
        return turbo_audio_requested_;
    }
    bool turbo_audio_decoupled() const { return turbo_audio_decoupled_; }
    bool turbo_audio_muted() const { return turbo_audio_muted_; }
    bool turbo_audio_pending() const { return turbo_audio_pending_; }
    // Fatal decoupling failure is held until Turbo release. Runtime uses this
    // to reset the host bridge once, while canonical audio remains available.
    bool turbo_audio_fallback_active() const {
        return turbo_audio_requested_ && turbo_audio_reason_logged_ &&
               !turbo_audio_pending_ && !turbo_audio_decoupled_;
    }
    const char* turbo_audio_failure_reason() const {
        return turbo_audio_reason_;
    }
    void turbo_audio_host_failure(const char* reason) {
        turbo_audio_fail(reason);
    }
    uint64_t turbo_audio_native_overwrites() const {
        return turbo_audio_native_overwrites_;
    }
    bool turbo_audio_enter(float multiplier, bool uncapped, uint64_t wall_ns);
    void turbo_audio_service(uint64_t wall_ns);
    // Normal-speed native requests use the same verified publication boundary
    // but render the MP2K candidate from wall time. Canonical GBA audio remains
    // the host output until PSG/FIFO composition is implemented.
    void native_audio_service(uint64_t wall_ns);
    void turbo_audio_exit(uint64_t wall_ns);
    void turbo_audio_reset(uint64_t wall_ns);
    // Preserve the canonical left/right Direct Sound buses without enabling
    // the still-probationary native MP2K renderer. Native requests imply this
    // path; GBARECOMP_AUDIO_STEREO enables it independently.
    bool stereo_audio_requested() const {
        return native_audio_requested_ || canonical_stereo_requested_ ||
               turbo_audio_requested_;
    }

    // Called by the game-specific RAM dispatcher at the detected
    // SoundMainRAM entry. This is the authoritative MP2K tick boundary;
    // native rendering must not infer it from host sample time.
    void mp2k_frame_hook(uint32_t key, uint64_t cycles);

    // Golden Sun's Camelot copy writes the finished MP2K block through a
    // generated IWRAM mixer instead of dispatching the ROM SoundMainRAM
    // pointer directly. The first store after a discontinuity in the
    // SoundInfo PCM ring is the authoritative tick boundary. `cycles` is the
    // guest clock at the store; Camelot writes two routed copies a few cycles
    // apart, so it is also the de-duplication key for that pair.
    void mp2k_pcm_write_hook(uint32_t pc, uint32_t addr, uint32_t value,
                             uint32_t width, uint32_t operand_sample,
                             uint32_t operand_gain, uint64_t cycles,
                             uint32_t before_value = 0, uint8_t thumb = 0,
                             uint8_t pre_write = 0);
    // Cheap cached observer gate. It admits all known writer PCs and all
    // cached SoundInfo/channel/producer ranges; unknown cache state is
    // fail-open so relocation cannot lose an event.
    bool mp2k_write_may_be_relevant(uint32_t pc, uint32_t addr,
                                    uint32_t width) const;

    // Project-local Camelot boundary observer. Runtime dispatch remains
    // generic; this layer gates the measured target PC and block identity.
    void mp2k_control_hook(uint32_t pc, uint32_t target, uint64_t cycles,
                           uint8_t thumb);

    // Camelot's worldmap copy can update SoundInfo without producing a
    // trace-visible PCM block. Use the VBlank cadence as a guarded fallback
    // boundary when no recent PCM completion was observed.
    void mp2k_vblank_hook(uint64_t cycles);

    // Save-state serialization. Captures the full guest mixer state
    // (channels, FIFOs, master, soundbias, sample accumulator) plus the
    // pending host output ring so playback resumes seamlessly. The
    // diagnostic FIFO trace ring is NOT serialized. See debug/snapshot.h.
    void serialize(gbarecomp::debug::SnapshotWriter& w) const;
    void deserialize(gbarecomp::debug::SnapshotReader& r,
                     const uint8_t* legacy_io = nullptr,
                     std::size_t legacy_io_len = 0);

private:
    friend struct GbaAudioTurboFixture;
    // ── Channel 1: square wave with frequency sweep ──────────────
    struct Sound1 {
        bool     active = false;
        uint8_t  duty = 2;
        uint8_t  length = 64;
        bool     length_enabled = false;
        uint8_t  envelope_initial = 0;
        bool     envelope_increase = false;
        uint8_t  envelope_step  = 0;
        uint8_t  volume = 0;
        uint16_t frequency = 0;
        uint32_t waveform_cycles = 0;
        uint8_t  waveform_phase  = 0;
        uint32_t envelope_cycles = 0;
        uint32_t length_cycles   = 0;
        // Sweep state (SOUND1 only — the BIOS chime uses sweep).
        uint8_t  sweep_shift     = 0;   // 0..7
        bool     sweep_decrease  = false;
        uint8_t  sweep_time      = 0;   // 0..7 (0 = disabled)
        uint32_t sweep_cycles    = 0;
    };
    Sound1 ch1_{};

    // ── Channel 2: square wave ────────────────────────────────────
    struct Sound2 {
        // Live envelope + length state.
        bool     active = false;            // playing
        uint8_t  duty = 2;                  // 0..3 (12.5/25/50/75 % high)
        uint8_t  length = 64;               // remaining length (0..64); 0 = stop if length_enabled
        bool     length_enabled = false;    // bit 14 of SOUND2CNT_H
        uint8_t  envelope_initial = 0;      // 0..15 starting volume
        bool     envelope_increase = false; // direction
        uint8_t  envelope_step  = 0;        // 0..7 (period in 1/64 s steps)
        uint8_t  volume = 0;                // current envelope volume 0..15
        uint16_t frequency = 0;             // 11-bit "frequency" field
        // Internal cycle accumulators (in 1/kSampleRate units for the
        // length / envelope clocks, in dot units for the waveform).
        uint32_t waveform_cycles = 0;
        uint8_t  waveform_phase  = 0;       // 0..7 within the duty pattern
        uint32_t envelope_cycles = 0;
        uint32_t length_cycles   = 0;
    };
    Sound2 ch2_{};

    // ── Channel 3: wave RAM (4-bit PCM, 32/64 digits) ─────────────
    struct Sound3 {
        bool     active = false;
        bool     dac_on = false;          // SOUND3CNT_L bit7 (0=stop, 1=play)
        bool     two_banks = false;       // bit5 (0=32 digits, 1=64 digits)
        uint8_t  bank = 0;                 // bit6 (bank selected for playback)
        uint16_t length = 256;             // 0..256 (256-N); 256Hz clock
        bool     length_enabled = false;   // SOUND3CNT_X bit14
        uint8_t  volume_code = 0;          // bits13..14 (0=mute,1=100,2=50,3=25)
        bool     force_volume = false;     // bit15 (force 75%)
        uint16_t frequency = 0;            // 11-bit rate field
        uint32_t wave_cycles = 0;          // per-digit cycle accumulator
        uint8_t  wave_pos = 0;             // 0..63 current digit
        uint32_t length_cycles = 0;
    };
    Sound3 ch3_{};
    // Two 16-byte banks = 64 4-bit digits. CPU access at 0x090-0x09F maps to
    // the bank NOT selected for playback (Bit 6), so a game can fill the next
    // bank while the current plays (mGBA model).
    uint8_t wave_ram_[32] = {};

    // ── Channel 4: noise (LFSR) ───────────────────────────────────
    struct Sound4 {
        bool     active = false;
        uint8_t  length = 64;              // 0..64 (64-N); 256Hz clock
        bool     length_enabled = false;   // SOUND4CNT_H bit14
        uint8_t  envelope_initial = 0;
        bool     envelope_increase = false;
        uint8_t  envelope_step  = 0;
        uint8_t  volume = 0;
        uint8_t  divisor_code = 0;         // SOUND4CNT_H bits0..2 (r)
        bool     width_7bit = false;       // bit3 (0=15-bit, 1=7-bit LFSR)
        uint8_t  shift = 0;                // bits4..7 (s)
        uint16_t lfsr = 0x7FFF;            // 15-bit linear-feedback shift reg
        uint32_t noise_cycles = 0;         // per-LFSR-step cycle accumulator
        uint32_t envelope_cycles = 0;
        uint32_t length_cycles   = 0;
    };
    Sound4 ch4_{};
    // Runtime-only; kept outside Sound4 so the append-only savestate layout
    // remains compatible with states written before the PSG extension grew.
    int8_t ch4_output_sample_ = 0;

    // ── Master state ──────────────────────────────────────────────
    bool    master_enable_ = false;  // SOUNDCNT_X bit 7
    uint8_t volume_l_ = 0;           // SOUNDCNT_L master volume left (0..7)
    uint8_t volume_r_ = 0;           // SOUNDCNT_L master volume right (0..7)
    bool    ch1_left_enable_  = false;
    bool    ch1_right_enable_ = false;
    bool    ch2_left_enable_  = false;
    bool    ch2_right_enable_ = false;
    bool    ch3_left_enable_  = false;
    bool    ch3_right_enable_ = false;
    bool    ch4_left_enable_  = false;
    bool    ch4_right_enable_ = false;
    // SOUNDCNT_H (0x082..0x083) bits 0..1: PSG volume shift selector.
    // mGBA applies this as `sample >> (4 - volume)` before the GBA bias
    // stage; values above 3 are reserved and clamp to 3.
    uint8_t dmg_volume_ratio_ = 0;

    struct DirectFifo {
        uint32_t words[8] = {};
        uint8_t write = 0;
        uint8_t read = 0;
        uint8_t count = 0;
        uint32_t shift_word = 0;
        uint8_t bytes_remaining = 0;
        int8_t samples[kMaxSamplesPerEvent] = {};
    };

    DirectFifo fifo_a_{};
    DirectFifo fifo_b_{};
    bool direct_a_left_ = false;
    bool direct_a_right_ = false;
    bool direct_a_timer1_ = false;
    bool direct_a_full_volume_ = false;
    bool direct_b_left_ = false;
    bool direct_b_right_ = false;
    bool direct_b_timer1_ = false;
    bool direct_b_full_volume_ = false;
    uint16_t soundbias_ = 0x0200;
    uint32_t cycles_per_sample_ = kDefaultCyclesPerSample;

    // Accumulates toward mGBA's 1024-cycle audio event. Each event
    // posts 2, 4, 8, or 16 samples depending on SOUNDBIAS resolution.
    uint32_t cycle_accumulator_ = 0;

    // Output ring. Mono int16_t samples at kSampleRate. The host
    // audio backend pulls from this in chunks.
    std::vector<int16_t> ring_;
    std::size_t ring_head_ = 0;
    std::size_t ring_tail_ = 0;
    uint64_t    samples_generated_ = 0;

    // Interleaved stereo ring for the opt-in native path.
    std::vector<int16_t> native_ring_;
    std::size_t native_head_ = 0;
    std::size_t native_tail_ = 0;
    // Interleaved canonical stereo ring for the opt-in fallback path.
    std::vector<int16_t> stereo_fallback_ring_;
    std::size_t stereo_fallback_head_ = 0;
    std::size_t stereo_fallback_tail_ = 0;

    // Always-on capture ring (see CapSample / query_capture above). Indexed
    // by (absolute sample index % kCapRingSize). Allocated lazily on reset.
    std::vector<CapSample> cap_ring_;
    AudioEventCapture event_capture_{};
    bool sound_fifo_dma_write_context_ = false;
    void cap_push(const CapSample& s);
    int16_t current_samples_[kMaxSamplesPerEvent] = {};
    uint32_t sample_index_ = 0;
    FifoTrace trace_[kFifoTraceSize] = {};
    uint32_t trace_write_ = 0;
    uint32_t trace_count_ = 0;

    void ring_push(int16_t s);
    void write_io8_impl(uint32_t off, uint8_t v);
    void capture_audio_io(uint32_t off, uint32_t value, uint8_t width,
                          uint64_t cycles);
    static bool valid_mp2k_channel_field(uint32_t field);
    void reset_shadow_runtime();
    void fifo_push_word(DirectFifo& fifo, int fifo_id, uint32_t word);
    void fifo_clear_queue(DirectFifo& fifo);
    void fifo_reset(DirectFifo& fifo);
    void fifo_timer_step(DirectFifo& fifo, int fifo_id, uint64_t cycles);
    void update_soundbias(uint16_t value);
    void update_shadow_timing();
    void refresh_mp2k_watch_ranges();
    void refresh_mp2k_hook_filter();
    void remember_mp2k_postmix_pc(uint32_t pc);
    void update_mp2k_watch_ranges(uint32_t sound_info,
                                  uint32_t block_bytes,
                                  uint32_t dma_period);
    void update_mp2k_shadow_producer_context();
    void shadow_frame_hook_timed(const MemView& mem, uint64_t cursor,
                                 uint32_t key, Mp2kShadow::HookPhase phase,
                                 uint64_t block_id,
                                 uint64_t boundary_cycle = 0,
                                 bool producer_block = false);
    bool consume_mp2k_pending_block(uint32_t key, uint8_t hook_source,
                                    uint64_t cycles);
    void restore_legacy_psg_from_io(const uint8_t* io, std::size_t len);
    uint32_t samples_per_event() const;
    uint32_t cycles_until_next_sample_event() const;
    void sample_until_current_time();
    void run_sample_event();
    void ch1_trigger();
    void ch2_trigger();
    void ch3_trigger();
    void ch4_trigger();
    // include_direct=false omits the Direct Sound FIFO contribution (PSG
    // only) — used when the MP2K shadow substitutes the direct-sound mix.
    // `tap`, if non-null, receives the raw per-channel breakdown for the
    // always-on capture ring (does not affect the returned mixed sample).
    struct ChannelTap {
        bool canonical_valid = false;
        uint32_t sample_rate = 0;
        int16_t ch[4] = {};
        int16_t direct_a = 0;
        int16_t direct_b = 0;
        int32_t direct_mix = 0;
        int32_t direct_left_mix = 0;
        int32_t direct_right_mix = 0;
        int16_t psg_left = 0;
        int16_t psg_right = 0;
        int16_t canonical_left = 0;
        int16_t canonical_right = 0;
        int16_t psg_mono = 0;
        int32_t psg_left_input = 0;
        int32_t psg_right_input = 0;
        DirectSoundRouteState direct_route{};
    };
    int16_t mix_one_sample(uint32_t direct_slot, bool include_direct = true,
                           ChannelTap* tap = nullptr);

    // ── MP2K HLE shadow (verified-enhancement; default off) ──────
    Mp2kShadow shadow_{};
    MemView    shadow_mem_{};
    // Cached at audio/shadow reset/configuration boundaries. Keep getenv out
    // of per-sample and per-hook paths while preserving current presence-only
    // probe semantics.
    bool       audio_probe_enabled_ = false;
    bool       shadow_enabled_ = false;
    bool       native_audio_enabled_ = false;
    bool       native_audio_requested_ = false;
    bool       native_wall_candidate_active_ = false;
    bool       native_wall_candidate_failed_ = false;
    bool       native_wall_output_live_ = false;
    bool       native_wall_failure_logged_ = false;
    char       native_wall_failure_reason_[64] = {};
    uint64_t   native_wall_last_wall_ns_ = 0;
    uint64_t   native_wall_sample_remainder_ = 0;
    uint64_t   native_wall_cursor_q32_ = 0;
    uint64_t   native_wall_sequence_ = UINT64_MAX;
    uint64_t   native_wall_candidate_frames_ = 0;
    uint64_t   native_wall_composed_frames_ = 0;
    uint64_t   native_wall_canonical_start_sample_ = 0;
    uint32_t   native_wall_canonical_rate_ = 0;
    int16_t    native_wall_last_composed_left_ = 0;
    int16_t    native_wall_last_composed_right_ = 0;
    bool       canonical_stereo_requested_ = false;
    bool       native_live_reported_ = false;
    bool       turbo_audio_requested_ = false;
    bool       turbo_audio_decoupled_ = false;
    bool       turbo_audio_muted_ = false;
    bool       turbo_audio_pending_ = false;
    bool       turbo_audio_reason_logged_ = false;
    char       turbo_audio_reason_[64] = {};
    float      turbo_audio_multiplier_ = 1.0f;
    bool       turbo_audio_uncapped_ = false;
    uint64_t   turbo_audio_last_wall_ns_ = 0;
    uint64_t   turbo_audio_sample_remainder_ = 0;
    uint64_t   turbo_audio_sequence_ = UINT64_MAX;
    uint64_t   turbo_audio_native_overwrites_ = 0;
    TurboAudioScheduler turbo_audio_scheduler_{};
    Mp2kWallMixer turbo_wall_mixer_{};
    Mp2kWallUpdateQueue turbo_audio_updates_{};
    Mp2kHookFastFilter mp2k_hook_filter_{};
    void stereo_fallback_push(int16_t left, int16_t right);
    void clear_stereo_fallback();
    void native_ring_push(int16_t left, int16_t right);
    void turbo_audio_fail(const char* reason);
    void native_audio_fail(const char* reason);
    void reset_native_wall_candidate();
    bool native_audio_begin_wall(uint64_t wall_ns);
    void service_wall_audio(uint64_t wall_ns);
    void log_native_timeline_summary(const char* event) const;
    void log_native_probation_status(uint64_t cursor) const;
    static void turbo_audio_published_block_callback(void* context,
                                                     uint64_t sequence);
    bool turbo_audio_capture_published_block(uint64_t sequence);
    uint64_t   shadow_cursor_ = 0;          // running grid-sample count
    std::array<std::array<uint32_t, 8>, 2> fifo_source_words_{};
    std::array<uint32_t, 2> fifo_pending_source_{};
    std::array<bool, 2> fifo_pending_source_valid_{};
    std::array<uint32_t, 2> fifo_shift_source_{};
    uint64_t   last_guest_hook_cursor_ = UINT64_MAX;
    struct Mp2kPcmWriter {
        uint32_t pc = 0;
        uint32_t last_addr = 0;
        uint32_t last_width = 0;
        uint32_t block_bytes = 0;
    };
    std::array<Mp2kPcmWriter, kMp2kKnownWriterPcs.size()> mp2k_pcm_writers_{};
    Mp2kPcmWriter mp2k_c70_writer_{};
    uint64_t mp2k_c70_writer_calls_ = 0;
    uint64_t mp2k_c70_writer_completions_ = 0;
    uint8_t  mp2k_c70_call_diag_logs_ = 0;
    uint8_t  mp2k_c70_completion_diag_logs_ = 0;
    uint8_t  mp2k_dry_writer_diag_logs_ = 0;
    uint8_t  mp2k_status_diag_logs_ = 0;
    uint8_t  mp2k_boot_probe_diag_logs_ = 0;
    uint8_t  mp2k_boot_probe_last_stage_ = 0xFFu;
    bool     mp2k_watch_ranges_valid_ = false;
    uint32_t mp2k_watch_sound_info_ = 0;
    uint32_t mp2k_watch_channel_lo_ = 0;
    uint32_t mp2k_watch_channel_hi_ = 0;
    uint32_t mp2k_watch_ring_a_lo_ = 0;
    uint32_t mp2k_watch_ring_a_hi_ = 0;
    uint32_t mp2k_watch_ring_b_lo_ = 0;
    uint32_t mp2k_watch_ring_b_hi_ = 0;
    uint64_t   mp2k_last_block_cycles_ = 0;
    bool       mp2k_have_block_cycles_ = false;
    uint64_t   mp2k_active_block_id_ = UINT64_MAX;
    uint64_t   mp2k_pending_block_id_ = UINT64_MAX;
    uint8_t    mp2k_completed_routes_ = 0;
    uint64_t   mp2k_writer_completion_count_ = 0;
    uint64_t   mp2k_last_guest_hook_cycles_ = UINT64_MAX;
    uint64_t   mp2k_active_hook_cycles_ = 0;
    bool       mp2k_defer_vblank_snapshot_ = false;
    bool       mp2k_boundary_snapshot_pending_ = false;
    uint64_t   mp2k_boundary_snapshot_block_ = UINT64_MAX;
    uint64_t   mp2k_boundary_snapshot_cycles_ = 0;
    struct Mp2kGuestMixProbe {
        uint64_t block_id = UINT64_MAX;
        uint32_t base_addr = 0;
        uint32_t last_addr = 0;
        uint32_t voice = 0;
        uint32_t word_index = 0;
        std::vector<uint32_t> previous_voice;
        uint32_t index0_count = 0;
        uint64_t first_write_cycles = 0;
        uint64_t last_write_cycles = 0;
        std::array<uint16_t, 8> write_channel_status{};
        std::array<CanonicalMixStepTrace, 8> index0_steps{};
        uint32_t channel_write_count = 0;
        std::array<CanonicalMixTrace::ChannelWrite, 64> channel_writes{};
        std::array<CanonicalMixTrace::VoiceIdentity, 8> voice_identities{};
        CanonicalBoundaryTrace boundary{};
    } mp2k_guest_mix_probe_{};
    std::array<CanonicalBoundaryTrace, 16> mp2k_boundary_ring_{};
    uint32_t mp2k_boundary_write_ = 0;
    uint32_t mp2k_boundary_count_ = 0;
    uint64_t   shadow_hook_phase_ = 0;      // fractional frame scheduler
    uint64_t   shadow_hook_interval_ = 0;   // sample_rate * GBA frame cycles
    double     shadow_hook_period_ = 1097.0; // exact samples per 59.7275 Hz tick
};

}  // namespace gba
