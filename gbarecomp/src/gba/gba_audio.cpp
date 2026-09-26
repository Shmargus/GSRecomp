// gba_audio.cpp — see gba_audio.h. SOUND1/2 (square), SOUND3 (wave RAM),
// SOUND4 (noise), and Direct Sound A/B FIFOs, mixed in mix_one_sample().

#include "gba_audio.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

#include "gba_ppu.h"
#include "runtime_arm_types.h"
#include "snapshot.h"

// Present in the AOT runtime, absent from standalone audio unit-test links.
// This only reads the existing bounded runtime ring.
extern "C" uint32_t runtime_trace_copy_recent(
    RuntimeTraceEntry* out, uint32_t max_entries)
#if defined(__GNUC__)
    __attribute__((weak))
#endif
    ;

// Cost Probe audio counters. They stay inert unless the existing cost probe
// env is enabled; standalone audio tests still link without runtime bridge.
extern "C" unsigned long long g_cost_mp2k_frame_hooks = 0;
extern "C" unsigned long long g_cost_mp2k_render_ns = 0;
extern "C" unsigned long long g_cost_mp2k_render_calls = 0;
extern "C" unsigned long long g_cost_mp2k_producer_blocks_ns = 0;
extern "C" unsigned long long g_cost_mp2k_producer_blocks = 0;
extern "C" unsigned long long g_cost_mp2k_judge_output_ns = 0;
extern "C" unsigned long long g_cost_mp2k_judge_output_calls = 0;

namespace gba {
namespace {

bool valid_audio_mmio_range(uint32_t off, uint8_t width) {
    const uint32_t end = off + width;
    constexpr uint32_t kRanges[][2] = {
        {0x060u, 0x066u}, {0x068u, 0x06Eu}, {0x070u, 0x076u},
        {0x078u, 0x07Eu}, {0x080u, 0x085u}, {0x088u, 0x08Au},
        {0x090u, 0x0A0u},
    };
    for (const auto& range : kRanges)
        if (off >= range[0] && end <= range[1]) return true;
    return false;
}

bool audio_cost_probe_enabled() {
    static const bool enabled = [] {
        const char* e = std::getenv("GBARECOMP_COST_PROBE");
        return e && e[0] && !(e[0] == '0' && e[1] == '\0');
    }();
    return enabled;
}

const char* producer_reject_reason_name(
    Mp2kShadow::ProducerRejectReason reason) {
    switch (reason) {
        case Mp2kShadow::ProducerRejectReason::SignedMetrics:
            return "signed-metrics";
        case Mp2kShadow::ProducerRejectReason::DmaPublish:
            return "dma-publish";
        default:
            return "none";
    }
}

const char* producer_dma_miss_kind_name(ProducerDmaMissKind kind) {
    switch (kind) {
        case ProducerDmaMissKind::OutsidePublishedRange:
            return "outside-range";
        case ProducerDmaMissKind::RouteMismatch:
            return "route-mismatch";
        default:
            return "none";
    }
}

void log_mp2k_output_domain_diagnostic(
    const Mp2kOutputDomainDiagnostic& d) {
    auto rms = [](double sum_sq, uint64_t count) {
        return count == 0 ? 0.0 : std::sqrt(sum_sq / count);
    };
    auto log_pair = [&](const char* name,
                        const std::array<double, 2>& sum,
                        const std::array<double, 2>& sum_sq,
                        const std::array<double, 2>& peak,
                        const std::array<uint64_t, 2>& nonnegative,
                        const std::array<uint64_t, 2>& negative) {
        std::fprintf(stderr,
            "[audio] MP2K first probation window failure: %s_l=(rms=%.6f "
            "peak=%.6f sum=%.6f sign=%llu/%llu) %s_r=(rms=%.6f "
            "peak=%.6f sum=%.6f sign=%llu/%llu)\n",
            name, rms(sum_sq[0], d.samples), peak[0], sum[0],
            static_cast<unsigned long long>(nonnegative[0]),
            static_cast<unsigned long long>(negative[0]), name,
            rms(sum_sq[1], d.samples), peak[1], sum[1],
            static_cast<unsigned long long>(nonnegative[1]),
            static_cast<unsigned long long>(negative[1]));
    };
    std::fprintf(stderr,
        "[audio] MP2K first probation window failure: window=%llu "
        "samples=%llu missing=%llu missing_total=%llu cursor=%llu-%llu "
        "producer=%llu published=%llu corr=%.3f ratio=%.3f route=0x%02x "
        "full=0x%02x map_changes=%u psg=%d/%d bias=%u\n",
        static_cast<unsigned long long>(d.verifier_window),
        static_cast<unsigned long long>(d.samples),
        static_cast<unsigned long long>(d.missing),
        static_cast<unsigned long long>(d.missing_total),
        static_cast<unsigned long long>(d.first_cursor),
        static_cast<unsigned long long>(d.last_cursor),
        static_cast<unsigned long long>(d.producer_block),
        static_cast<unsigned long long>(d.published_block), d.correlation,
        d.level_ratio, static_cast<unsigned>(d.route_mask),
        static_cast<unsigned>(d.full_volume_mask), d.route_mapping_changes,
        d.psg_left, d.psg_right, static_cast<unsigned>(d.soundbias));
    log_pair("canonical", d.canonical_sum, d.canonical_sum_sq,
             d.canonical_peak, d.canonical_nonnegative,
             d.canonical_negative);
    log_pair("native", d.native_sum, d.native_sum_sq, d.native_peak,
             d.native_nonnegative, d.native_negative);
    log_pair("route", d.route_sum, d.route_sum_sq, d.route_peak,
             d.route_nonnegative, d.route_negative);
    log_pair("fifo", d.fifo_sum, d.fifo_sum_sq, d.fifo_peak,
             d.fifo_nonnegative, d.fifo_negative);
    const auto& r = d.resampler;
    const auto& s = r.stats;
    std::fprintf(stderr,
        "[audio] MP2K first probation resampler: host=%llu started=%u "
        "queue=%u seq=%llu last_seq=%llu start_q32=0x%016llx "
        "end_q32=0x%016llx step_q32=0x%016llx phase_q32=0x%016llx "
        "offset_q32=%lld pushes=%llu gaps=%llu resets=%llu "
        "underruns=%llu late=%llu last_phase_q32=0x%016llx\n",
        static_cast<unsigned long long>(r.host_sample), r.started ? 1u : 0u,
        r.queued_blocks, static_cast<unsigned long long>(r.current_sequence),
        static_cast<unsigned long long>(r.last_sequence),
        static_cast<unsigned long long>(r.current_start_q32),
        static_cast<unsigned long long>(r.current_end_q32),
        static_cast<unsigned long long>(r.current_step_q32),
        static_cast<unsigned long long>(r.current_phase_q32),
        static_cast<long long>(r.cursor_offset_q32),
        static_cast<unsigned long long>(s.resampler_pushes),
        static_cast<unsigned long long>(s.sequence_gaps),
        static_cast<unsigned long long>(s.resampler_resets),
        static_cast<unsigned long long>(s.resampler_underruns),
        static_cast<unsigned long long>(s.late_blocks),
        static_cast<unsigned long long>(s.last_phase_q32));
}

bool known_mp2k_writer_pc(uint32_t pc) {
    const uint32_t normalized = pc & ~1u;
    for (const uint32_t known : kMp2kKnownWriterPcs)
        if (known == normalized) return true;
    return false;
}

constexpr uint32_t kMp2kControlHookPc = 0x03000828u;

bool range_overlaps(uint32_t addr, uint32_t width,
                   uint32_t lo, uint32_t hi) {
    if (width == 0 || lo >= hi) return false;
    const uint64_t end = static_cast<uint64_t>(addr) + width;
    return static_cast<uint64_t>(addr) < hi && end > lo;
}

}  // namespace

DirectSoundOutput mix_direct_sound_output(
        float route_a, float route_b, int32_t psg_left, int32_t psg_right,
        const DirectSoundRouteState& state) {
    const float a = route_a * 128.0f;
    const float b = route_b * 128.0f;
    const float a_scaled = a * (state.a_full_volume ? 4.0f : 2.0f);
    const float b_scaled = b * (state.b_full_volume ? 4.0f : 2.0f);
    float left = static_cast<float>(psg_left);
    float right = static_cast<float>(psg_right);
    if (state.a_left) left += a_scaled;
    if (state.a_right) right += a_scaled;
    if (state.b_left) left += b_scaled;
    if (state.b_right) right += b_scaled;

    const float bias = static_cast<float>(state.soundbias & 0x03FFu);
    auto apply_bias = [bias](float sample) {
        const float biased = std::clamp(sample + bias, 0.0f, 1023.0f);
        return std::clamp((biased - bias) * 48.0f,
                          -32767.0f, 32767.0f);
    };
    return {apply_bias(left), apply_bias(right)};
}

PsgMixResult mix_psg_samples(const PsgChannelSamples& samples,
                             const PsgChannelRouting& routing) {
    PsgMixResult out;
    auto add_scaled = [&](int32_t sample, bool left, bool right) {
        // mGBA sums ch1+ch2+ch3 and applies one <<3 to that combined sum,
        // then adds a separately pre-shifted ch4
        // (third_party/mgba/src/gb/audio.c:752-763). Left shift distributes
        // over addition, so scaling each channel individually before
        // summing here is equivalent and gives all four channels the same
        // ×8 scale.
        const int32_t scaled = sample << 3;
        if (left) out.left += scaled;
        if (right) out.right += scaled;
    };
    add_scaled(samples.ch1, routing.ch1_left, routing.ch1_right);
    add_scaled(samples.ch2, routing.ch2_left, routing.ch2_right);
    add_scaled(samples.ch3, routing.ch3_left, routing.ch3_right);
    add_scaled(samples.ch4, routing.ch4_left, routing.ch4_right);
    return out;
}


namespace {

// Duty patterns for the four duty cycles (12.5 / 25 / 50 / 75 %).
// 8-step waveform; each entry is "1 if output high at this phase".
constexpr uint8_t kDutyPatterns[4][8] = {
    {0, 0, 0, 0, 0, 0, 0, 1},  // 12.5%
    {1, 0, 0, 0, 0, 0, 0, 1},  // 25%
    {1, 0, 0, 0, 0, 1, 1, 1},  // 50%
    {0, 1, 1, 1, 1, 1, 1, 0},  // 75% (alias of 25% inverted)
};

constexpr std::size_t kRingSize = 1u << 14;  // 16384 samples ~ 0.5 s
constexpr uint32_t kAudioStateExtensionMagic = 0x31445541u; // "AUD1"
constexpr uint32_t kNativeProbeFailureLimit = 4; // require repeated failed windows

uint32_t load_u32le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

GbaAudio::GbaAudio() {
    ring_.assign(kRingSize, 0);
    native_ring_.assign(kRingSize * 2, 0);
    stereo_fallback_ring_.assign(kRingSize * 2, 0);
    cap_ring_.assign(kCapRingSize, CapSample{});
    reset();
}
GbaAudio::~GbaAudio() {
    log_native_timeline_summary("exit");
}

void GbaAudio::log_native_timeline_summary(const char* event) const {
    if (!audio_cost_probe_enabled()) return;
    const AudioTimelineStats& dma = shadow_.producer_dma_stats();
    const AudioTimelineStats& host = shadow_.producer_host_stats();
    const ProducerDmaMiss& first_dma_miss =
        shadow_.producer_dma_first_unmatched();
    if (!native_audio_requested_ && !native_live_reported_ &&
        shadow_.producer_blocks_judged() == 0 && dma.published == 0 &&
        host.resampler_pushes == 0 && dma.unmatched == 0 &&
        shadow_.producer_blocks_incomplete() == 0 &&
        shadow_.producer_blocks_rejected() == 0) {
        return;
    }
    std::fprintf(
        stderr,
        "[audio-native-timeline] event=%s judged=%llu passed=%llu "
        "rejected=%llu incomplete=%llu published=%llu "
        "dma_matched=%llu dma_unmatched=%llu dma_releases=%llu "
        "host_pushed=%llu host_seq_gaps=%llu dma_seq_gaps=%llu "
        "resets=%llu underruns=%llu late=%llu "
        "candidate_ready=%llu candidate_missing=%llu "
        "verifier_samples=%llu verifier_windows=%llu "
        "first_incomplete=%llu expected=%u guest=%u native=%u "
        "cursor=%llu aligned=%u routes=0x%08x/0x%08x "
        "first_rejected=%llu reason=%s corr=%.3f ratio=%.3f mae=%.3f "
        "cursor=%llu routes=0x%08x/0x%08x "
        "first_dma_miss=%s route=%u src=0x%08x seq=%llu "
        "range=0x%08x-0x%08x offset=%u\n",
        event ? event : "unknown",
        static_cast<unsigned long long>(shadow_.producer_blocks_judged()),
        static_cast<unsigned long long>(shadow_.producer_blocks_passed()),
        static_cast<unsigned long long>(shadow_.producer_blocks_rejected()),
        static_cast<unsigned long long>(shadow_.producer_blocks_incomplete()),
        static_cast<unsigned long long>(dma.published),
        static_cast<unsigned long long>(dma.matched),
        static_cast<unsigned long long>(dma.unmatched),
        static_cast<unsigned long long>(dma.releases),
        static_cast<unsigned long long>(host.resampler_pushes),
        static_cast<unsigned long long>(host.sequence_gaps),
        static_cast<unsigned long long>(dma.sequence_gaps),
        static_cast<unsigned long long>(host.resampler_resets),
        static_cast<unsigned long long>(host.resampler_underruns),
        static_cast<unsigned long long>(host.late_blocks),
        static_cast<unsigned long long>(shadow_.candidate_samples_ready()),
        static_cast<unsigned long long>(shadow_.candidate_samples_missing()),
        static_cast<unsigned long long>(shadow_.verifier_samples()),
        static_cast<unsigned long long>(shadow_.verifier_windows()),
        static_cast<unsigned long long>(shadow_.first_incomplete_block()),
        static_cast<unsigned>(shadow_.first_incomplete_expected_samples()),
        static_cast<unsigned>(shadow_.first_incomplete_guest_samples()),
        static_cast<unsigned>(shadow_.first_incomplete_native_samples()),
        static_cast<unsigned long long>(shadow_.first_incomplete_cursor()),
        shadow_.first_incomplete_alignment_ready() ? 1u : 0u,
        shadow_.first_incomplete_route_a(),
        shadow_.first_incomplete_route_b(),
        static_cast<unsigned long long>(shadow_.first_rejected_block()),
        producer_reject_reason_name(shadow_.first_rejected_reason()),
        static_cast<double>(shadow_.first_rejected_correlation()),
        static_cast<double>(shadow_.first_rejected_level_ratio()),
        static_cast<double>(shadow_.first_rejected_mean_abs_error()),
        static_cast<unsigned long long>(shadow_.first_rejected_cursor()),
        shadow_.first_rejected_route_a(),
        shadow_.first_rejected_route_b(),
        producer_dma_miss_kind_name(first_dma_miss.kind),
        static_cast<unsigned>(first_dma_miss.route),
        first_dma_miss.source_addr,
        static_cast<unsigned long long>(first_dma_miss.sequence),
        first_dma_miss.range_start,
        first_dma_miss.range_end,
        first_dma_miss.offset);
}

void GbaAudio::log_native_probation_status(uint64_t cursor) const {
    const AudioTimelineStats& dma = shadow_.producer_dma_stats();
    const AudioTimelineStats& host = shadow_.producer_host_stats();
    const char* wall_reason = "none";
    if (shadow_.wall_export_reverb_rejected() != 0)
        wall_reason = "reverb";
    else if (shadow_.wall_export_not_verified() != 0)
        wall_reason = "not-verified";
    else if (shadow_.wall_export_bad_asset() != 0)
        wall_reason = "asset";
    else if (shadow_.wall_export_bad_sequence() != 0)
        wall_reason = "sequence";
    else if (shadow_.wall_export_bad_rate() != 0)
        wall_reason = "rate";
    std::fprintf(
        stderr,
        "[audio] MP2K probation status: cursor=%llu active=%u "
        "engaged=%u proven=%u live=%u shadow_enabled=%u "
        "judged=%llu passed=%llu rejected=%llu incomplete=%llu "
        "published_block=%llu verifier_samples=%llu verifier_windows=%llu "
        "probation_failures=%u signed_seen=%u signed_pass=%u "
        "last_corr=%.3f last_ratio=%.3f "
        "candidate_ready=%llu candidate_missing=%llu "
        "dma_published=%llu dma_matched=%llu dma_unmatched=%llu "
        "dma_seq_gaps=%llu host_pushes=%llu host_seq_gaps=%llu "
        "wall_active=%u wall_live=%u wall_exports=%llu "
        "wall_unsupported=%llu wall_reason=%s wall_reverb=%llu "
        "wall_not_verified=%llu wall_asset=%llu wall_sequence=%llu "
        "wall_rate=%llu fallback=%u\n",
        static_cast<unsigned long long>(cursor), shadow_.active() ? 1u : 0u,
        shadow_.engaged() ? 1u : 0u, shadow_.proven() ? 1u : 0u,
        shadow_.live() ? 1u : 0u, shadow_enabled_ ? 1u : 0u,
        static_cast<unsigned long long>(shadow_.producer_blocks_judged()),
        static_cast<unsigned long long>(shadow_.producer_blocks_passed()),
        static_cast<unsigned long long>(shadow_.producer_blocks_rejected()),
        static_cast<unsigned long long>(shadow_.producer_blocks_incomplete()),
        static_cast<unsigned long long>(shadow_.published_producer_block()),
        static_cast<unsigned long long>(shadow_.verifier_samples()),
        static_cast<unsigned long long>(shadow_.verifier_windows()),
        shadow_.probation_failures(), shadow_.signed_gate_seen() ? 1u : 0u,
        shadow_.signed_gate_passed() ? 1u : 0u,
        static_cast<double>(shadow_.last_correlation()),
        static_cast<double>(shadow_.last_level_ratio()),
        static_cast<unsigned long long>(shadow_.candidate_samples_ready()),
        static_cast<unsigned long long>(shadow_.candidate_samples_missing()),
        static_cast<unsigned long long>(dma.published),
        static_cast<unsigned long long>(dma.matched),
        static_cast<unsigned long long>(dma.unmatched),
        static_cast<unsigned long long>(dma.sequence_gaps),
        static_cast<unsigned long long>(host.resampler_pushes),
        static_cast<unsigned long long>(host.sequence_gaps),
        native_wall_candidate_active_ ? 1u : 0u,
        native_wall_output_live_ ? 1u : 0u,
        static_cast<unsigned long long>(shadow_.wall_export_successes()),
        static_cast<unsigned long long>(shadow_.wall_export_unsupported()),
        wall_reason,
        static_cast<unsigned long long>(
            shadow_.wall_export_reverb_rejected()),
        static_cast<unsigned long long>(shadow_.wall_export_not_verified()),
        static_cast<unsigned long long>(shadow_.wall_export_bad_asset()),
        static_cast<unsigned long long>(shadow_.wall_export_bad_sequence()),
        static_cast<unsigned long long>(shadow_.wall_export_bad_rate()),
        shadow_.canonical_fallback() ? 1u : 0u);
}

void GbaAudio::serialize(gbarecomp::debug::SnapshotWriter& w) const {
    // Channels + FIFOs are POD nested structs (no pointers); raw bytes
    // are stable within a build and the snapshot version gates layout.
    w.bytes(&ch1_, sizeof(ch1_));
    w.bytes(&ch2_, sizeof(ch2_));
    // The ch3/ch4/wave state is appended as a backward-compatible extension
    // below. The fixed prefix remains unchanged for existing v1 states.
    w.boolean(master_enable_);
    w.u8(volume_l_);
    w.u8(volume_r_);
    w.boolean(ch1_left_enable_);
    w.boolean(ch1_right_enable_);
    w.boolean(ch2_left_enable_);
    w.boolean(ch2_right_enable_);
    w.u8(dmg_volume_ratio_);
    w.bytes(&fifo_a_, sizeof(fifo_a_));
    w.bytes(&fifo_b_, sizeof(fifo_b_));
    w.boolean(direct_a_left_);
    w.boolean(direct_a_right_);
    w.boolean(direct_a_timer1_);
    w.boolean(direct_a_full_volume_);
    w.boolean(direct_b_left_);
    w.boolean(direct_b_right_);
    w.boolean(direct_b_timer1_);
    w.boolean(direct_b_full_volume_);
    w.u16(soundbias_);
    w.u32(cycles_per_sample_);
    w.u32(cycle_accumulator_);
    // Pending host output ring so playback resumes seamlessly.
    w.u32(static_cast<uint32_t>(ring_.size()));
    if (!ring_.empty()) w.bytes(ring_.data(), ring_.size() * sizeof(int16_t));
    w.u64(ring_head_);
    w.u64(ring_tail_);
    w.u64(samples_generated_);
    w.bytes(current_samples_, sizeof(current_samples_));
    w.u32(sample_index_);
    // Optional tail extension. Old v1 states end at sample_index_; keeping
    // this append-only lets the current runtime load them while new states
    // retain the PSG channels that were previously treated as transient.
    w.u32(kAudioStateExtensionMagic);
    w.bytes(&ch3_, sizeof(ch3_));
    w.bytes(&ch4_, sizeof(ch4_));
    w.bytes(wave_ram_, sizeof(wave_ram_));
    w.boolean(ch3_left_enable_);
    w.boolean(ch3_right_enable_);
    w.boolean(ch4_left_enable_);
    w.boolean(ch4_right_enable_);
}

void GbaAudio::deserialize(gbarecomp::debug::SnapshotReader& r,
                           const uint8_t* legacy_io,
                           std::size_t legacy_io_len) {
    r.bytes(&ch1_, sizeof(ch1_));
    r.bytes(&ch2_, sizeof(ch2_));
    // Old v1 states have no audio extension, so start these fields clean and
    // let the guest re-sync them. New states replace them from the extension
    // appended below.
    ch3_ = Sound3{};
    ch4_ = Sound4{};
    ch4_output_sample_ = 0;
    std::fill(std::begin(wave_ram_), std::end(wave_ram_), uint8_t{0});
    master_enable_    = r.boolean();
    volume_l_         = r.u8();
    volume_r_         = r.u8();
    ch1_left_enable_  = r.boolean();
    ch1_right_enable_ = r.boolean();
    ch2_left_enable_  = r.boolean();
    ch2_right_enable_ = r.boolean();
    dmg_volume_ratio_ = r.u8();
    r.bytes(&fifo_a_, sizeof(fifo_a_));
    r.bytes(&fifo_b_, sizeof(fifo_b_));
    direct_a_left_        = r.boolean();
    direct_a_right_       = r.boolean();
    direct_a_timer1_      = r.boolean();
    direct_a_full_volume_ = r.boolean();
    direct_b_left_        = r.boolean();
    direct_b_right_       = r.boolean();
    direct_b_timer1_      = r.boolean();
    direct_b_full_volume_ = r.boolean();
    soundbias_         = r.u16();
    cycles_per_sample_ = r.u32();
    cycle_accumulator_ = r.u32();
    uint32_t ring_len = r.u32();
    ring_.assign(ring_len, 0);
    if (ring_len) r.bytes(ring_.data(), ring_len * sizeof(int16_t));
    ring_head_         = static_cast<std::size_t>(r.u64());
    ring_tail_         = static_cast<std::size_t>(r.u64());
    samples_generated_ = r.u64();
    r.bytes(current_samples_, sizeof(current_samples_));
    sample_index_      = r.u32();
    bool has_psg_extension = false;
    if (r.remaining() >= sizeof(uint32_t) &&
        r.u32() == kAudioStateExtensionMagic) {
        has_psg_extension = true;
        r.bytes(&ch3_, sizeof(ch3_));
        r.bytes(&ch4_, sizeof(ch4_));
        r.bytes(wave_ram_, sizeof(wave_ram_));
        ch3_left_enable_  = r.boolean();
        ch3_right_enable_ = r.boolean();
        ch4_left_enable_  = r.boolean();
        ch4_right_enable_ = r.boolean();
    }
    if (!has_psg_extension)
        restore_legacy_psg_from_io(legacy_io, legacy_io_len);
    // Captured events belong to the pre-load guest timeline. Drop them and
    // advance the epoch so a later consumer cannot mix save-state eras.
    event_capture_.reset();
    // Native MP2K state is derived from live guest RAM, not the canonical
    // snapshot blob. Rebuild it for the restored timeline instead of carrying
    // pre-load voices/verifier history forward.
    reset_shadow_runtime();
}

void GbaAudio::restore_legacy_psg_from_io(const uint8_t* io,
                                          std::size_t len) {
    // Older v1 states serialized the IO page but not SOUND3/SOUND4 or wave
    // RAM. Reconstruct the register-visible part so those states do not load
    // with both PSG channels silently cleared. Internal phase is necessarily
    // reset; a trigger/length write at the saved boundary re-synchronizes it.
    if (!io || len < 0xA0u) return;

    const uint8_t s3l = io[0x070];
    const uint8_t s3h = io[0x073];
    const uint8_t s3x = io[0x075];
    ch3_.two_banks = (s3l & 0x20u) != 0;
    ch3_.bank = static_cast<uint8_t>((s3l >> 6) & 1u);
    ch3_.dac_on = (s3l & 0x80u) != 0;
    ch3_.length = static_cast<uint16_t>(256u - io[0x072]);
    ch3_.volume_code = static_cast<uint8_t>((s3h >> 5) & 0x3u);
    ch3_.force_volume = (s3h & 0x80u) != 0;
    ch3_.frequency = static_cast<uint16_t>(io[0x074] |
                                           ((s3x & 0x7u) << 8));
    ch3_.length_enabled = (s3x & 0x40u) != 0;
    ch3_.active = ch3_.dac_on && ((s3x & 0x80u) != 0);

    const uint8_t s4h = io[0x07C];
    const uint8_t s4x = io[0x07D];
    ch4_.length = static_cast<uint8_t>(64u - (io[0x078] & 0x3Fu));
    ch4_.envelope_step = static_cast<uint8_t>(io[0x079] & 0x7u);
    ch4_.envelope_increase = (io[0x079] & 0x8u) != 0;
    ch4_.envelope_initial = static_cast<uint8_t>((io[0x079] >> 4) & 0xFu);
    ch4_.volume = ch4_.envelope_initial;
    ch4_.divisor_code = static_cast<uint8_t>(s4h & 0x7u);
    ch4_.width_7bit = (s4h & 0x8u) != 0;
    ch4_.shift = static_cast<uint8_t>((s4h >> 4) & 0xFu);
    ch4_.length_enabled = (s4x & 0x40u) != 0;
    ch4_.active = (s4x & 0x80u) != 0;

    ch3_left_enable_ = (io[0x081] & 0x40u) != 0;
    ch3_right_enable_ = (io[0x081] & 0x04u) != 0;
    ch4_left_enable_ = (io[0x081] & 0x80u) != 0;
    ch4_right_enable_ = (io[0x081] & 0x08u) != 0;

    // The old IO image retains the currently CPU-visible wave bank. Restore
    // it into the corresponding physical bank; the other bank was not
    // representable in the old snapshot format.
    const uint32_t bank_base = (ch3_.bank ^ 1u) ? 16u : 0u;
    std::copy(io + 0x090, io + 0x0A0, wave_ram_ + bank_base);
}

void GbaAudio::reset() {
    ch1_ = Sound1{};
    ch2_ = Sound2{};
    ch3_ = Sound3{};
    ch4_ = Sound4{};
    ch4_output_sample_ = 0;
    std::fill(std::begin(wave_ram_), std::end(wave_ram_), uint8_t{0});
    master_enable_ = false;
    volume_l_ = 0;
    volume_r_ = 0;
    ch1_left_enable_  = false;
    ch1_right_enable_ = false;
    ch2_left_enable_  = false;
    ch2_right_enable_ = false;
    ch3_left_enable_  = false;
    ch3_right_enable_ = false;
    ch4_left_enable_  = false;
    ch4_right_enable_ = false;
    dmg_volume_ratio_ = 0;
    fifo_reset(fifo_a_);
    fifo_reset(fifo_b_);
    direct_a_left_ = false;
    direct_a_right_ = false;
    direct_a_timer1_ = false;
    direct_a_full_volume_ = false;
    direct_b_left_ = false;
    direct_b_right_ = false;
    direct_b_timer1_ = false;
    direct_b_full_volume_ = false;
    soundbias_ = 0x0200;
    cycles_per_sample_ = kDefaultCyclesPerSample;
    // mGBA schedules the GBA audio sample event at reset time and then
    // every 1024 CPU cycles. Prime the accumulator so the first host
    // tick emits that reset-time event, matching the oracle stream.
    cycle_accumulator_ = kSampleEventCycles;
    ring_head_ = ring_tail_ = 0;
    samples_generated_ = 0;
    sample_index_ = 0;
    std::fill(std::begin(current_samples_), std::end(current_samples_),
              int16_t{0});
    trace_write_ = 0;
    trace_count_ = 0;
    std::fill(ring_.begin(), ring_.end(), int16_t{0});
    native_head_ = native_tail_ = 0;
    std::fill(native_ring_.begin(), native_ring_.end(), int16_t{0});
    stereo_fallback_head_ = stereo_fallback_tail_ = 0;
    std::fill(stereo_fallback_ring_.begin(), stereo_fallback_ring_.end(), int16_t{0});
    native_audio_requested_ = false;
    canonical_stereo_requested_ = false;
    sound_fifo_dma_write_context_ = false;
    event_capture_.reset();
    reset_shadow_runtime();
}

void GbaAudio::reset_shadow_runtime() {
    audio_probe_enabled_ = std::getenv("GBARECOMP_AUDIO_PROBE") != nullptr;
    shadow_.set_published_block_callback(nullptr, nullptr);
    shadow_.reset_runtime();
    mp2k_watch_ranges_valid_ = false;
    mp2k_watch_sound_info_ = 0;
    mp2k_watch_channel_lo_ = mp2k_watch_channel_hi_ = 0;
    mp2k_watch_ring_a_lo_ = mp2k_watch_ring_a_hi_ = 0;
    mp2k_watch_ring_b_lo_ = mp2k_watch_ring_b_hi_ = 0;
    update_shadow_timing();
    shadow_cursor_ = 0;
    fifo_source_words_ = {};
    fifo_pending_source_ = {};
    fifo_pending_source_valid_ = {};
    fifo_shift_source_ = {};
    last_guest_hook_cursor_ = UINT64_MAX;
    mp2k_active_block_id_ = UINT64_MAX;
    mp2k_pending_block_id_ = UINT64_MAX;
    mp2k_completed_routes_ = 0;
    mp2k_writer_completion_count_ = 0;
    mp2k_last_guest_hook_cycles_ = UINT64_MAX;
    mp2k_active_hook_cycles_ = 0;
    mp2k_boundary_snapshot_pending_ = false;
    mp2k_boundary_snapshot_block_ = UINT64_MAX;
    mp2k_boundary_snapshot_cycles_ = 0;
    mp2k_pcm_writers_ = {};
    for (std::size_t i = 0; i < kMp2kKnownNonC70WriterPcs.size(); ++i)
        mp2k_pcm_writers_[i].pc = kMp2kKnownNonC70WriterPcs[i];
    mp2k_c70_writer_ = {};
    mp2k_c70_writer_calls_ = 0;
    mp2k_c70_writer_completions_ = 0;
    mp2k_c70_call_diag_logs_ = 0;
    mp2k_c70_completion_diag_logs_ = 0;
    mp2k_dry_writer_diag_logs_ = 0;
    mp2k_status_diag_logs_ = 0;
    mp2k_boot_probe_diag_logs_ = 0;
    mp2k_boot_probe_last_stage_ = 0xFFu;
    mp2k_boundary_ring_ = {};
    mp2k_boundary_write_ = 0;
    mp2k_boundary_count_ = 0;
    mp2k_last_block_cycles_ = 0;
    mp2k_have_block_cycles_ = false;
    shadow_hook_phase_ = 0;
    native_live_reported_ = false;
    native_wall_candidate_active_ = false;
    native_wall_candidate_failed_ = false;
    native_wall_output_live_ = false;
    native_wall_failure_logged_ = false;
    native_wall_failure_reason_[0] = '\0';
    native_wall_last_wall_ns_ = 0;
    native_wall_sample_remainder_ = 0;
    native_wall_cursor_q32_ = 0;
    native_wall_sequence_ = UINT64_MAX;
    native_wall_candidate_frames_ = 0;
    native_wall_composed_frames_ = 0;
    native_wall_canonical_start_sample_ = 0;
    native_wall_canonical_rate_ = 0;
    native_wall_last_composed_left_ = 0;
    native_wall_last_composed_right_ = 0;
    turbo_audio_scheduler_.reset(0, 0);
    turbo_audio_decoupled_ = false;
    turbo_audio_muted_ = false;
    turbo_audio_pending_ = false;
    turbo_audio_reason_logged_ = false;
    turbo_audio_reason_[0] = '\0';
    turbo_audio_multiplier_ = 1.0f;
    turbo_audio_uncapped_ = false;
    turbo_audio_last_wall_ns_ = 0;
    turbo_audio_sample_remainder_ = 0;
    turbo_audio_sequence_ = UINT64_MAX;
    turbo_audio_native_overwrites_ = 0;
    turbo_wall_mixer_.reset();
    turbo_audio_updates_.reset(0);
    native_head_ = native_tail_ = 0;
    std::fill(native_ring_.begin(), native_ring_.end(), int16_t{0});
    clear_stereo_fallback();
}

void GbaAudio::capture_audio_io(uint32_t off, uint32_t value,
                                uint8_t width, uint64_t cycles) {
    if (!event_capture_.enabled() || !valid_audio_mmio_range(off, width))
        return;
    const uint32_t end = off + width;
    const bool full_wave = off >= 0x090 && end <= 0x0A0;
    const uint8_t bank = static_cast<uint8_t>(ch3_.bank ^ 1u);
    const uint32_t bank_base = bank ? 16u : 0u;
    uint32_t before = 0;
    if (full_wave) {
        for (uint8_t i = 0; i < width; ++i)
            before |= static_cast<uint32_t>(
                wave_ram_[bank_base + off - 0x090u + i]) << (i * 8u);
    }
    bool trigger = false;
    for (uint8_t i = 0; i < width; ++i) {
        const uint32_t byte_off = off + i;
        if ((byte_off == 0x065 || byte_off == 0x06D ||
             byte_off == 0x075 || byte_off == 0x07D) &&
            (((value >> (i * 8u)) & 0x80u) != 0)) {
            trigger = true;
        }
    }
    AudioCaptureResult result = full_wave
        ? event_capture_.record_psg_wave_ram(
              cycles, off - 0x090u, bank, value, before, width)
        : event_capture_.record_psg_register(
              cycles, off, value, width, trigger, before);
    event_capture_.note_result(result);
}

void GbaAudio::write_io8(uint32_t off, uint8_t v, uint64_t cycles) {
    capture_audio_io(off, v, 1, cycles);
    write_io8_impl(off, v);
}

void GbaAudio::write_io8_impl(uint32_t off, uint8_t v) {
    // Promote to 16-bit writes by tracking the matching pair byte
    // via an internal cache. For now, treat any 8-bit write to a
    // pair as a partial update — only the byte changed counts.
    // The bus has the canonical store; we just need to react.
    switch (off) {
        // SOUND1CNT_L (0x060..0x061): sweep
        case 0x060: {
            sample_until_current_time();
            ch1_.sweep_shift    = static_cast<uint8_t>(v & 0x7);
            ch1_.sweep_decrease = (v & 0x08) != 0;
            ch1_.sweep_time     = static_cast<uint8_t>((v >> 4) & 0x7);
            break;
        }
        // SOUND1CNT_H (0x062..0x063): length+duty + envelope (same
        // layout as SOUND2CNT_L at 0x068).
        case 0x062: {
            sample_until_current_time();
            ch1_.length = static_cast<uint8_t>(64 - (v & 0x3F));
            ch1_.duty   = static_cast<uint8_t>((v >> 6) & 0x3);
            break;
        }
        case 0x063: {
            sample_until_current_time();
            ch1_.envelope_step     = static_cast<uint8_t>(v & 0x7);
            ch1_.envelope_increase = (v & 0x08) != 0;
            ch1_.envelope_initial  = static_cast<uint8_t>((v >> 4) & 0xF);
            break;
        }
        // SOUND1CNT_X (0x064..0x065): frequency + length-enable + initial
        case 0x064: {
            sample_until_current_time();
            ch1_.frequency = static_cast<uint16_t>(
                (ch1_.frequency & 0x0700) | v);
            break;
        }
        case 0x065: {
            sample_until_current_time();
            ch1_.frequency = static_cast<uint16_t>(
                (ch1_.frequency & 0x00FF) | (static_cast<uint16_t>(v & 0x7) << 8));
            ch1_.length_enabled = (v & 0x40) != 0;
            if (v & 0x80) ch1_trigger();
            break;
        }
        // SOUND2CNT_L (0x068..0x069): length+duty/envelope
        case 0x068: {
            sample_until_current_time();
            ch2_.length = static_cast<uint8_t>(64 - (v & 0x3F));
            ch2_.duty   = static_cast<uint8_t>((v >> 6) & 0x3);
            break;
        }
        case 0x069: {
            sample_until_current_time();
            ch2_.envelope_step    = static_cast<uint8_t>(v & 0x7);
            ch2_.envelope_increase = (v & 0x08) != 0;
            ch2_.envelope_initial = static_cast<uint8_t>((v >> 4) & 0xF);
            // Envelope of 0 with direction "decrease" silences the
            // DAC at next trigger; spec quirk we model on trigger.
            break;
        }
        // SOUND2CNT_H (0x06C..0x06D): frequency + length-enable + initial
        case 0x06C: {
            sample_until_current_time();
            ch2_.frequency = static_cast<uint16_t>(
                (ch2_.frequency & 0x0700) | v);
            break;
        }
        case 0x06D: {
            sample_until_current_time();
            ch2_.frequency = static_cast<uint16_t>(
                (ch2_.frequency & 0x00FF) | (static_cast<uint16_t>(v & 0x7) << 8));
            ch2_.length_enabled = (v & 0x40) != 0;
            if (v & 0x80) ch2_trigger();
            break;
        }
        // ── SOUND3 (wave RAM) ─────────────────────────────────────
        // SOUND3CNT_L (0x070): dimension (bit5), bank (bit6), DAC on (bit7).
        case 0x070: {
            sample_until_current_time();
            ch3_.two_banks = (v & 0x20) != 0;
            ch3_.bank      = static_cast<uint8_t>((v >> 6) & 0x1);
            ch3_.dac_on    = (v & 0x80) != 0;
            if (!ch3_.dac_on) ch3_.active = false;
            break;
        }
        // SOUND3CNT_H (0x072..0x073): length (0x072 bits0-7), volume code
        // (0x073 bits5-6 = SOUND3CNT_H bits13-14), force-75% (0x073 bit7).
        case 0x072: {
            sample_until_current_time();
            ch3_.length = static_cast<uint16_t>(256u - v);
            break;
        }
        case 0x073: {
            sample_until_current_time();
            ch3_.volume_code  = static_cast<uint8_t>((v >> 5) & 0x3);
            ch3_.force_volume = (v & 0x80) != 0;
            break;
        }
        // SOUND3CNT_X (0x074..0x075): frequency + length-enable + trigger.
        case 0x074: {
            sample_until_current_time();
            ch3_.frequency = static_cast<uint16_t>(
                (ch3_.frequency & 0x0700) | v);
            break;
        }
        case 0x075: {
            sample_until_current_time();
            ch3_.frequency = static_cast<uint16_t>(
                (ch3_.frequency & 0x00FF) |
                (static_cast<uint16_t>(v & 0x7) << 8));
            ch3_.length_enabled = (v & 0x40) != 0;
            if (v & 0x80) ch3_trigger();
            break;
        }
        // ── SOUND4 (noise) ────────────────────────────────────────
        // SOUND4CNT_L (0x078): length (bits0-5); 0x079: envelope (same
        // layout as SOUND2CNT_L high byte at 0x069).
        case 0x078: {
            sample_until_current_time();
            ch4_.length = static_cast<uint8_t>(64 - (v & 0x3F));
            break;
        }
        case 0x079: {
            sample_until_current_time();
            ch4_.envelope_step     = static_cast<uint8_t>(v & 0x7);
            ch4_.envelope_increase = (v & 0x08) != 0;
            ch4_.envelope_initial  = static_cast<uint8_t>((v >> 4) & 0xF);
            break;
        }
        // SOUND4CNT_H (0x07C): divisor (bits0-2), width (bit3), shift
        // (bits4-7); 0x07D: length-enable (bit6) + trigger (bit7).
        case 0x07C: {
            sample_until_current_time();
            ch4_.divisor_code = static_cast<uint8_t>(v & 0x7);
            ch4_.width_7bit   = (v & 0x08) != 0;
            ch4_.shift        = static_cast<uint8_t>((v >> 4) & 0xF);
            break;
        }
        case 0x07D: {
            sample_until_current_time();
            ch4_.length_enabled = (v & 0x40) != 0;
            if (v & 0x80) ch4_trigger();
            break;
        }
        // SOUNDCNT_L (0x080..0x081): master L/R volumes + channel routes
        case 0x080: {
            sample_until_current_time();
            volume_r_ = static_cast<uint8_t>(v & 0x7);
            volume_l_ = static_cast<uint8_t>((v >> 4) & 0x7);
            break;
        }
        case 0x081: {
            sample_until_current_time();
            // bits 0..3 = right enables (1=SOUND1, 2=SOUND2, ...)
            // bits 4..7 = left enables
            ch1_right_enable_ = (v & 0x01) != 0;
            ch2_right_enable_ = (v & 0x02) != 0;
            ch3_right_enable_ = (v & 0x04) != 0;
            ch4_right_enable_ = (v & 0x08) != 0;
            ch1_left_enable_  = (v & 0x10) != 0;
            ch2_left_enable_  = (v & 0x20) != 0;
            ch3_left_enable_  = (v & 0x40) != 0;
            ch4_left_enable_  = (v & 0x80) != 0;
            break;
        }
        // SOUNDCNT_H (0x082..0x083): DMG channel volume ratio +
        // SOUND A/B DMA controls. We only model the DMG ratio for
        // now (bits 0..1 of low byte).
        case 0x082: {
            dmg_volume_ratio_ = static_cast<uint8_t>(v & 0x3);
            direct_a_full_volume_ = (v & 0x04) != 0;
            direct_b_full_volume_ = (v & 0x08) != 0;
            break;
        }
        case 0x083: {
            direct_a_right_ = (v & 0x01) != 0;
            direct_a_left_  = (v & 0x02) != 0;
            direct_a_timer1_ = (v & 0x04) != 0;
            if (v & 0x08) fifo_clear_queue(fifo_a_);
            direct_b_right_ = (v & 0x10) != 0;
            direct_b_left_  = (v & 0x20) != 0;
            direct_b_timer1_ = (v & 0x40) != 0;
            if (v & 0x80) fifo_clear_queue(fifo_b_);
            break;
        }
        // SOUNDBIAS (0x088..0x089): bias + output resolution.
        case 0x088: {
            sample_until_current_time();
            update_soundbias(static_cast<uint16_t>(
                (soundbias_ & 0xFF00u) | v));
            break;
        }
        case 0x089: {
            sample_until_current_time();
            update_soundbias(static_cast<uint16_t>(
                (soundbias_ & 0x00FFu) |
                (static_cast<uint16_t>(v) << 8)));
            break;
        }
        // SOUNDCNT_X (0x084): master enable + per-channel ON flags
        case 0x084: {
            sample_until_current_time();
            master_enable_ = (v & 0x80) != 0;
            if (!master_enable_) {
                // Disabling kills all channel state per hardware.
                ch1_.active = false;
                ch2_.active = false;
                ch3_.active = false;
                ch4_.active = false;
            }
            break;
        }
        default:
            // Wave RAM (0x090..0x09F): the CPU window maps to the bank NOT
            // selected for playback (SOUND3CNT_L bit6), so a game can fill the
            // next bank while the current one plays (mGBA model). wave_ram_ is
            // 32 bytes = two 16-byte banks (bank0 = bytes 0-15, bank1 = 16-31).
            if (off >= 0x090 && off <= 0x09F) {
                sample_until_current_time();
                uint32_t bank_base = (ch3_.bank ^ 1u) ? 16u : 0u;
                wave_ram_[bank_base + (off - 0x090u)] = v;
            }
            break;
    }
}

void GbaAudio::write_io16(uint32_t off, uint16_t v, uint64_t cycles) {
    capture_audio_io(off, v, 2, cycles);
    write_io8_impl(off,     static_cast<uint8_t>(v & 0xFF));
    write_io8_impl(off + 1, static_cast<uint8_t>((v >> 8) & 0xFF));
}

void GbaAudio::write_io32(uint32_t off, uint32_t v, uint64_t cycles) {
    if (off == 0x0A0) {
        if (event_capture_.enabled() && !sound_fifo_dma_write_context_)
            event_capture_.note_result(
                event_capture_.record_direct_fifo_cpu_word(cycles, 0, v));
        fifo_push_word(fifo_a_, 0, v);
        return;
    }
    if (off == 0x0A4) {
        if (event_capture_.enabled() && !sound_fifo_dma_write_context_)
            event_capture_.note_result(
                event_capture_.record_direct_fifo_cpu_word(cycles, 1, v));
        fifo_push_word(fifo_b_, 1, v);
        return;
    }
    capture_audio_io(off, v, 4, cycles);
    write_io8_impl(off,     static_cast<uint8_t>(v & 0xFF));
    write_io8_impl(off + 1, static_cast<uint8_t>((v >> 8) & 0xFF));
    write_io8_impl(off + 2, static_cast<uint8_t>((v >> 16) & 0xFF));
    write_io8_impl(off + 3, static_cast<uint8_t>((v >> 24) & 0xFF));
}

void GbaAudio::ch1_trigger() {
    ch1_.active = true;
    if (ch1_.length == 0) ch1_.length = 64;
    ch1_.volume          = ch1_.envelope_initial;
    ch1_.waveform_cycles = 0;
    ch1_.waveform_phase  = 0;
    ch1_.envelope_cycles = 0;
    ch1_.length_cycles   = 0;
    ch1_.sweep_cycles    = 0;
    if (ch1_.envelope_initial == 0 && !ch1_.envelope_increase) {
        ch1_.active = false;
    }
}

void GbaAudio::ch2_trigger() {
    ch2_.active = true;
    if (ch2_.length == 0) ch2_.length = 64;
    ch2_.volume          = ch2_.envelope_initial;
    ch2_.waveform_cycles = 0;
    ch2_.waveform_phase  = 0;
    ch2_.envelope_cycles = 0;
    ch2_.length_cycles   = 0;
    // A DAC-disabled trigger (init volume 0, decrease envelope) is
    // still considered "started" by the channel but emits silence.
    if (ch2_.envelope_initial == 0 && !ch2_.envelope_increase) {
        ch2_.active = false;
    }
}

void GbaAudio::ch3_trigger() {
    // SOUND3CNT_X bit15. With the DAC off (bit7 of SOUND3CNT_L) the channel
    // produces no output regardless of the trigger.
    if (!ch3_.dac_on) { ch3_.active = false; return; }
    ch3_.active = true;
    if (ch3_.length == 0) ch3_.length = 256;
    ch3_.wave_pos    = 0;   // restart at the first digit of the selected bank
    ch3_.wave_cycles = 0;
    ch3_.length_cycles = 0;
}

void GbaAudio::ch4_trigger() {
    ch4_.active = true;
    if (ch4_.length == 0) ch4_.length = 64;
    ch4_.volume          = ch4_.envelope_initial;
    ch4_.lfsr            = ch4_.width_7bit ? 0x7F : 0x7FFF;
    ch4_output_sample_  = 0;
    ch4_.noise_cycles    = 0;
    ch4_.envelope_cycles = 0;
    ch4_.length_cycles   = 0;
    // DAC-disabled trigger (init vol 0, decreasing envelope) → silent.
    if (ch4_.envelope_initial == 0 && !ch4_.envelope_increase) {
        ch4_.active = false;
    }
}

void GbaAudio::ring_push(int16_t s) {
    ring_[ring_head_] = s;
    ring_head_ = (ring_head_ + 1) % kRingSize;
    if (ring_head_ == ring_tail_) {
        // Overrun — drop oldest by advancing tail.
        ring_tail_ = (ring_tail_ + 1) % kRingSize;
    }
    ++samples_generated_;
}

void GbaAudio::cap_push(const CapSample& s) {
    // Indexed by the absolute sample number that ring_push is about to assign
    // (samples_generated_ is bumped by ring_push immediately after). Keeping
    // the two in lockstep means cap_ring_[idx % N] holds the same sample the
    // playback FIFO received at absolute index idx.
    cap_ring_[samples_generated_ % kCapRingSize] = s;
}

bool GbaAudio::compose_mp2k_with_canonical(
    float route_a, float route_b, const CapSample& canonical,
    bool include_separate_fifo, int16_t& left, int16_t& right) {
    if (!canonical.canonical_valid || canonical.sample_rate == 0 ||
        !std::isfinite(route_a) || !std::isfinite(route_b))
        return false;
    if (include_separate_fifo) {
        // The only evidence currently available identifies the captured FIFO
        // bytes as the MP2K buses being replaced. Keep additive FIFO mixing an
        // explicit opt-in for a future independently identified source.
        route_a += static_cast<float>(canonical.direct_a) / 128.0f;
        route_b += static_cast<float>(canonical.direct_b) / 128.0f;
    }
    const DirectSoundOutput output = mix_direct_sound_output(
        route_a, route_b, canonical.psg_left_input,
        canonical.psg_right_input, canonical.direct_route);
    auto to_i16 = [](float value) {
        if (!std::isfinite(value)) return int16_t{0};
        const int32_t sample = static_cast<int32_t>(std::lround(value));
        return static_cast<int16_t>(std::clamp(sample, -32767, 32767));
    };
    left = to_i16(output.left);
    right = to_i16(output.right);
    return true;
}

std::size_t GbaAudio::query_capture(uint64_t start, std::size_t count,
                                    CapSample* out, uint64_t& out_first) const {
    if (cap_ring_.empty() || count == 0) { out_first = start; return 0; }
    uint64_t oldest = capture_oldest_index();
    uint64_t head   = samples_generated_;          // one past newest
    if (start < oldest) start = oldest;
    if (start >= head) { out_first = start; return 0; }
    uint64_t avail = head - start;
    std::size_t n = count < avail ? count
                                  : static_cast<std::size_t>(avail);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = cap_ring_[(start + i) % kCapRingSize];
    }
    out_first = start;
    return n;
}

std::size_t GbaAudio::drain_samples(int16_t* out, std::size_t max) {
    std::size_t n = 0;
    while (n < max && ring_tail_ != ring_head_) {
        out[n++] = ring_[ring_tail_];
        ring_tail_ = (ring_tail_ + 1) % kRingSize;
    }
    return n;
}

void GbaAudio::native_ring_push(int16_t left, int16_t right) {
    if ((!native_audio_enabled_ && !turbo_audio_decoupled_) ||
        native_ring_.empty()) return;
    const std::size_t cap = native_ring_.size() / 2;
    const std::size_t next = (native_head_ + 1) % cap;
    if (next == native_tail_) {
        if (turbo_audio_decoupled_) {
            ++turbo_audio_native_overwrites_;
            turbo_audio_fail("native wall ring overflow");
        } else {
            native_audio_fail("native wall ring overflow");
        }
        return;
    }
    native_ring_[native_head_ * 2] = left;
    native_ring_[native_head_ * 2 + 1] = right;
    native_head_ = next;
    if (native_head_ == native_tail_) native_tail_ = (native_tail_ + 1) % cap;
}

std::size_t GbaAudio::native_audio_ring_size() const {
    if (native_ring_.empty()) return 0;
    const std::size_t cap = native_ring_.size() / 2;
    return native_head_ >= native_tail_
        ? native_head_ - native_tail_
        : cap - native_tail_ + native_head_;
}

std::size_t GbaAudio::drain_native_samples(int16_t* out, std::size_t max) {
    if ((!native_audio_enabled_ && !turbo_audio_decoupled_) ||
        native_ring_.empty()) return 0;
    const std::size_t cap = native_ring_.size() / 2;
    std::size_t n = 0;
    while (n < max && native_tail_ != native_head_) {
        out[n * 2] = native_ring_[native_tail_ * 2];
        out[n * 2 + 1] = native_ring_[native_tail_ * 2 + 1];
        native_tail_ = (native_tail_ + 1) % cap;
        ++n;
    }
    return n;
}

void GbaAudio::turbo_audio_fail(const char* reason) {
    std::snprintf(turbo_audio_reason_, sizeof(turbo_audio_reason_), "%s",
                  reason ? reason : "unsupported failure");
    if (!turbo_audio_reason_logged_) {
        std::fprintf(stderr,
                     "[audio] Turbo decoupling unavailable: %s; canonical "
                     "coupled retained\n",
                     reason ? reason : "unsupported failure");
        turbo_audio_reason_logged_ = true;
    }
    turbo_audio_decoupled_ = false;
    turbo_audio_muted_ = true;
    turbo_audio_pending_ = false;
    shadow_.set_published_block_callback(nullptr, nullptr);
    turbo_wall_mixer_.reset();
    turbo_audio_updates_.reset(0);
    native_head_ = native_tail_ = 0;
    std::fill(native_ring_.begin(), native_ring_.end(), int16_t{0});
    clear_stereo_fallback();
    turbo_audio_scheduler_.set_mode(TurboAudioMode::Muted);
}

void GbaAudio::native_audio_fail(const char* reason) {
    std::snprintf(native_wall_failure_reason_,
                  sizeof(native_wall_failure_reason_), "%s",
                  reason ? reason : "unsupported failure");
    if (!native_wall_failure_logged_) {
        std::fprintf(stderr,
                     "[audio] Native MP2K wall candidate unavailable: %s; "
                     "canonical coupled retained\n",
                     reason ? reason : "unsupported failure");
        native_wall_failure_logged_ = true;
    }
    native_wall_candidate_active_ = false;
    native_wall_candidate_failed_ = true;
    native_wall_output_live_ = false;
    native_audio_enabled_ = false;
    shadow_.set_published_block_callback(nullptr, nullptr);
    turbo_wall_mixer_.reset();
    turbo_audio_updates_.reset(0);
    native_wall_last_wall_ns_ = 0;
    native_wall_sample_remainder_ = 0;
    native_wall_cursor_q32_ = 0;
    native_wall_sequence_ = UINT64_MAX;
    native_wall_candidate_frames_ = 0;
    native_wall_composed_frames_ = 0;
    native_wall_canonical_start_sample_ = 0;
    native_wall_canonical_rate_ = 0;
    native_wall_last_composed_left_ = 0;
    native_wall_last_composed_right_ = 0;
    native_head_ = native_tail_ = 0;
    std::fill(native_ring_.begin(), native_ring_.end(), int16_t{0});
}

void GbaAudio::reset_native_wall_candidate() {
    native_wall_candidate_active_ = false;
    native_wall_candidate_failed_ = false;
    native_wall_output_live_ = false;
    native_wall_failure_logged_ = false;
    native_wall_failure_reason_[0] = '\0';
    native_wall_last_wall_ns_ = 0;
    native_wall_sample_remainder_ = 0;
    native_wall_cursor_q32_ = 0;
    native_wall_sequence_ = UINT64_MAX;
    native_wall_candidate_frames_ = 0;
    native_wall_composed_frames_ = 0;
    native_wall_canonical_start_sample_ = 0;
    native_wall_canonical_rate_ = 0;
    native_wall_last_composed_left_ = 0;
    native_wall_last_composed_right_ = 0;
}

void GbaAudio::turbo_audio_published_block_callback(void* context,
                                                    uint64_t sequence) {
    auto* audio = static_cast<GbaAudio*>(context);
    if (audio && (audio->turbo_audio_decoupled_ ||
                  audio->native_wall_candidate_active_)) {
        audio->turbo_audio_capture_published_block(sequence);
    }
}

bool GbaAudio::turbo_audio_capture_published_block(uint64_t sequence) {
    const bool turbo = turbo_audio_decoupled_;
    if (!turbo && !native_wall_candidate_active_) return false;
    uint64_t& last_sequence = turbo ? turbo_audio_sequence_
                                    : native_wall_sequence_;
    if (sequence == UINT64_MAX || sequence == last_sequence) return true;
    // Called at the verified publication boundary. Source IDs stay monotonic;
    // the bounded queue retains every publication before wall service.
    if (last_sequence != UINT64_MAX && sequence < last_sequence)
        return true;
    Mp2kWallSnapshotInput snapshot{};
    const uint64_t cursor = shadow_.producer_block_start_cursor();
    const uint64_t cursor_q32 = q32_seconds_from_frames(cursor, 65536u);
    const uint64_t reverb_rejects = shadow_.wall_export_reverb_rejected();
    const uint64_t asset_rejects = shadow_.wall_export_bad_asset();
    if (!shadow_.export_wall_snapshot(
            shadow_mem_, sequence, cursor_q32, snapshot,
            Mp2kWallExportMode::FullProducerSeed)) {
        const char* reason = "verified producer export rejected";
        if (shadow_.wall_export_reverb_rejected() != reverb_rejects)
            reason = "MP2K reverb unsupported";
        else if (shadow_.wall_export_bad_asset() != asset_rejects)
            reason = "MP2K asset unsupported";
        if (turbo) turbo_audio_fail(reason);
        else native_audio_fail(reason);
        return false;
    }
    // Runtime wall audio carries the verified producer seed. PSG/FIFO stays
    // on the canonical path; this snapshot is only MP2K replacement state.
    if (turbo_audio_updates_.enqueue_snapshot(snapshot, turbo_wall_mixer_) !=
        Mp2kWallCaptureResult::Accepted) {
        if (turbo) turbo_audio_fail("verified producer update rejected");
        else native_audio_fail("verified producer update rejected");
        return false;
    }
    last_sequence = sequence;
    return true;
}

bool GbaAudio::native_audio_begin_wall(uint64_t wall_ns) {
    if (native_wall_candidate_active_ || native_wall_candidate_failed_ ||
        !native_audio_requested_ || !native_audio_enabled_ ||
        turbo_audio_decoupled_ || !shadow_.live()) return false;
    const uint64_t sequence = shadow_.published_producer_block();
    if (sequence == UINT64_MAX) return false;
    const uint64_t cursor_q32 = q32_seconds_from_frames(
        shadow_.producer_block_start_cursor(), 65536u);
    const uint64_t reverb_rejects = shadow_.wall_export_reverb_rejected();
    const uint64_t asset_rejects = shadow_.wall_export_bad_asset();
    Mp2kWallSnapshotInput snapshot{};
    if (!shadow_.export_wall_snapshot(
            shadow_mem_, sequence, cursor_q32, snapshot,
            Mp2kWallExportMode::FullProducerSeed)) {
        if (shadow_.wall_export_reverb_rejected() != reverb_rejects)
            native_audio_fail("MP2K reverb unsupported");
        else if (shadow_.wall_export_bad_asset() != asset_rejects)
            native_audio_fail("MP2K asset unsupported");
        else
            native_audio_fail("verified snapshot rejected");
        return false;
    }
    turbo_wall_mixer_.reset();
    turbo_audio_updates_.reset(0);
    snapshot.sequence = 0;
    if (turbo_wall_mixer_.begin_snapshot(snapshot) !=
        Mp2kWallCaptureResult::Accepted ||
        !turbo_audio_updates_.prime(snapshot, turbo_wall_mixer_)) {
        native_audio_fail("wall snapshot queue prime failed");
        return false;
    }
    native_wall_candidate_active_ = true;
    native_wall_candidate_failed_ = false;
    native_wall_output_live_ = false;
    native_wall_last_wall_ns_ = wall_ns;
    native_wall_sample_remainder_ = 0;
    native_wall_cursor_q32_ = 0;
    native_wall_sequence_ = sequence;
    native_wall_candidate_frames_ = 0;
    native_wall_composed_frames_ = 0;
    native_wall_canonical_start_sample_ = samples_generated_;
    native_wall_canonical_rate_ = sample_rate();
    native_wall_last_composed_left_ = 0;
    native_wall_last_composed_right_ = 0;
    native_head_ = native_tail_ = 0;
    std::fill(native_ring_.begin(), native_ring_.end(), int16_t{0});
    shadow_.set_published_block_callback(
        &GbaAudio::turbo_audio_published_block_callback, this);
    std::fprintf(stderr,
                 "[audio] Native MP2K wall candidate ON at normal speed; "
                 "canonical PSG/FIFO output retained\n");
    return true;
}

bool GbaAudio::turbo_audio_enter(float multiplier, bool uncapped,
                                 uint64_t wall_ns) {
    if (!turbo_audio_requested_) return false;
    if (native_wall_candidate_active_ || native_wall_candidate_failed_) {
        shadow_.set_published_block_callback(nullptr, nullptr);
        reset_native_wall_candidate();
    }
    // A fatal edge failure stays coupled until Turbo release. Do not let a
    // held-key multiplier/config change silently retry wall audio mid-edge.
    if (!turbo_audio_pending_ && turbo_audio_muted_ &&
        turbo_audio_reason_logged_)
        return false;
    if (turbo_audio_decoupled_ && !turbo_audio_muted_ &&
        multiplier == turbo_audio_multiplier_ && !uncapped)
        return true;
    if (turbo_audio_decoupled_ ||
        (turbo_audio_muted_ && !turbo_audio_pending_))
        turbo_audio_reset(wall_ns);
    turbo_audio_multiplier_ = multiplier;
    turbo_audio_uncapped_ = uncapped;
    if (!turbo_audio_multiplier_supported(multiplier, uncapped) ||
        !std::isfinite(multiplier)) {
        turbo_audio_fail(uncapped ? "uncapped Turbo unsupported"
                                  : "Turbo multiplier outside 2x..4x");
        return false;
    }
    if (!shadow_enabled_ || !shadow_.live()) {
        turbo_audio_pending_ = true;
        turbo_audio_muted_ = true;
        if (!turbo_audio_reason_logged_) {
            std::fprintf(stderr,
                         "[audio] Turbo decoupling pending: MP2K not live; "
                         "canonical coupled retained\n");
            turbo_audio_reason_logged_ = true;
        }
        return false;
    }
    const uint64_t sequence = shadow_.published_producer_block();
    if (sequence == UINT64_MAX) {
        turbo_audio_pending_ = true;
        turbo_audio_muted_ = true;
        if (!turbo_audio_reason_logged_) {
            std::fprintf(stderr,
                         "[audio] Turbo decoupling pending: no published block; "
                         "canonical coupled retained\n");
            turbo_audio_reason_logged_ = true;
        }
        return false;
    }
    const uint64_t cursor_q32 = q32_seconds_from_frames(
        shadow_.producer_block_start_cursor(), 65536u);
    turbo_wall_mixer_.reset();
    turbo_audio_updates_.reset(0);
    const uint64_t reverb_rejects = shadow_.wall_export_reverb_rejected();
    const uint64_t asset_rejects = shadow_.wall_export_bad_asset();
    Mp2kWallSnapshotInput snapshot{};
    if (!shadow_.export_wall_snapshot(
            shadow_mem_, sequence, cursor_q32, snapshot,
            Mp2kWallExportMode::FullProducerSeed)) {
        if (shadow_.wall_export_reverb_rejected() != reverb_rejects)
            turbo_audio_fail("MP2K reverb unsupported");
        else if (shadow_.wall_export_bad_asset() != asset_rejects)
            turbo_audio_fail("MP2K asset unsupported");
        else
            turbo_audio_fail("verified snapshot rejected");
        return false;
    }
    snapshot.sequence = 0;
    if (turbo_wall_mixer_.begin_snapshot(snapshot) !=
        Mp2kWallCaptureResult::Accepted ||
        !turbo_audio_updates_.prime(snapshot, turbo_wall_mixer_)) {
        turbo_audio_fail("wall snapshot queue prime failed");
        return false;
    }
    turbo_audio_scheduler_.enter_turbo(
        TurboAudioMode::Decoupled, cursor_q32, 0);
    turbo_audio_decoupled_ = true;
    turbo_audio_muted_ = false;
    turbo_audio_pending_ = false;
    turbo_audio_reason_logged_ = false;
    turbo_audio_reason_[0] = '\0';
    turbo_audio_multiplier_ = multiplier;
    turbo_audio_last_wall_ns_ = wall_ns;
    turbo_audio_sample_remainder_ = 0;
    turbo_audio_sequence_ = sequence;
    shadow_.set_published_block_callback(
        &GbaAudio::turbo_audio_published_block_callback, this);
    turbo_audio_native_overwrites_ = 0;
    native_head_ = native_tail_ = 0;
    std::fill(native_ring_.begin(), native_ring_.end(), int16_t{0});
    clear_stereo_fallback();
    std::fprintf(stderr,
                 "[audio] Turbo decoupled ON: %.1fx, verified MP2K "
                 "producer-seed wall output (PSG/FIFO composition pending)\n",
                 static_cast<double>(multiplier));
    return true;
}

void GbaAudio::service_wall_audio(uint64_t wall_ns) {
    const bool turbo = turbo_audio_decoupled_;
    const bool normal = native_wall_candidate_active_;
    if ((!turbo && !normal) || (turbo && turbo_audio_muted_)) return;
    auto fail = [&](const char* reason) {
        if (turbo) turbo_audio_fail(reason);
        else native_audio_fail(reason);
    };
    if (!shadow_enabled_ || !shadow_.live()) {
        fail("verified MP2K session ended");
        return;
    }
    uint64_t& last_wall_ns = turbo ? turbo_audio_last_wall_ns_
                                   : native_wall_last_wall_ns_;
    if (wall_ns < last_wall_ns) {
        fail("wall clock reversed");
        return;
    }
    const uint64_t delta_ns = wall_ns - last_wall_ns;
    last_wall_ns = wall_ns;
    const uint64_t delta_q32 = q32_seconds_from_nanoseconds(delta_ns);
    uint64_t wall_end_q32 = 0;
    if (turbo) {
        if (!turbo_audio_scheduler_.advance_wall_q32(delta_q32)) {
            fail("wall clock overflow");
            return;
        }
        wall_end_q32 = turbo_audio_scheduler_.wall_cursor_q32();
    } else {
        if (delta_q32 > UINT64_MAX - native_wall_cursor_q32_) {
            fail("wall clock overflow");
            return;
        }
        native_wall_cursor_q32_ += delta_q32;
        wall_end_q32 = native_wall_cursor_q32_;
    }
    constexpr uint64_t kNsPerSecond = 1'000'000'000ull;
    constexpr uint64_t kRate = Mp2kWallMixer::kCanonicalRenderRate;
    const uint64_t whole_seconds = delta_ns / kNsPerSecond;
    const uint64_t remainder_ns = delta_ns % kNsPerSecond;
    if (whole_seconds > UINT64_MAX / kRate) {
        fail("wall sample budget overflow");
        return;
    }
    uint64_t& sample_remainder = turbo ? turbo_audio_sample_remainder_
                                       : native_wall_sample_remainder_;
    const uint64_t fractional_numerator = remainder_ns * kRate +
                                           sample_remainder;
    uint64_t frames = whole_seconds * kRate +
        fractional_numerator / kNsPerSecond;
    sample_remainder = fractional_numerator % kNsPerSecond;
    constexpr uint64_t kMaxServiceFrames = 4096;
    if (frames > kMaxServiceFrames) {
        fail("wall sample backlog exceeded bound");
        return;
    }
    const uint64_t anchor = turbo_audio_updates_.anchor_guest_cursor_q32();
    if (wall_end_q32 > UINT64_MAX - anchor) {
        fail("wall guest cursor overflow");
        return;
    }
    constexpr uint64_t kQ32PerWallFrame =
        Mp2kWallMixer::kQ32One / Mp2kWallMixer::kCanonicalRenderRate;
    while (frames != 0) {
        const uint64_t mixer_wall_q32 = turbo_wall_mixer_.wall_cursor_q32();
        if (mixer_wall_q32 > UINT64_MAX - anchor) {
            fail("wall guest cursor overflow");
            return;
        }
        const uint64_t current_guest_q32 = anchor + mixer_wall_q32;
        uint64_t next_guest_q32 = 0;
        if (turbo_audio_updates_.next_due_guest_cursor_q32(next_guest_q32) &&
            next_guest_q32 <= current_guest_q32) {
            if (!turbo_audio_updates_.apply_due(turbo_wall_mixer_,
                                                current_guest_q32)) {
                fail("wall update queue failed");
                return;
            }
            continue;
        }
        uint64_t segment_frames = std::min<uint64_t>(
            frames, Mp2kWallStereoChunk::kMaxFrames);
        if (turbo_audio_updates_.next_due_guest_cursor_q32(next_guest_q32) &&
            next_guest_q32 > current_guest_q32 &&
            next_guest_q32 <= anchor + wall_end_q32) {
            const uint64_t event_wall_q32 = next_guest_q32 - anchor;
            const uint64_t until_event_q32 = event_wall_q32 - mixer_wall_q32;
            const uint64_t until_event_frames = until_event_q32 /
                                                kQ32PerWallFrame;
            segment_frames = std::min(segment_frames, until_event_frames);
            // A sub-frame deadline is handled at the next frame boundary;
            // never spin without advancing the wall mixer.
            if (segment_frames == 0) segment_frames = 1;
        }
        const uint32_t want = static_cast<uint32_t>(segment_frames);
        std::array<CapSample, Mp2kWallStereoChunk::kMaxFrames>
            canonical_taps{};
        uint64_t canonical_first_offset = 0;
        std::size_t canonical_tap_count = 0;
        if (normal) {
            if (native_wall_canonical_rate_ == 0 ||
                sample_rate() != native_wall_canonical_rate_) {
                fail("canonical sample rate changed during wall candidate");
                return;
            }
            const uint64_t frame_base = native_wall_candidate_frames_;
            if (frame_base > UINT64_MAX / native_wall_canonical_rate_) {
                fail("canonical tap timeline overflow");
                return;
            }
            canonical_first_offset = (frame_base *
                native_wall_canonical_rate_) /
                Mp2kWallMixer::kCanonicalRenderRate;
            const uint64_t frame_last = frame_base + want - 1u;
            if (frame_last < frame_base ||
                frame_last > UINT64_MAX / native_wall_canonical_rate_) {
                fail("canonical tap timeline overflow");
                return;
            }
            const uint64_t canonical_last_offset = (frame_last *
                native_wall_canonical_rate_) /
                Mp2kWallMixer::kCanonicalRenderRate;
            if (canonical_last_offset < canonical_first_offset ||
                canonical_last_offset - canonical_first_offset + 1u >
                    canonical_taps.size()) {
                fail("canonical tap span exceeded bound");
                return;
            }
            canonical_tap_count = static_cast<std::size_t>(
                canonical_last_offset - canonical_first_offset + 1u);
            if (native_wall_canonical_start_sample_ >
                UINT64_MAX - canonical_first_offset) {
                fail("canonical tap sample index overflow");
                return;
            }
            const uint64_t canonical_start =
                native_wall_canonical_start_sample_ + canonical_first_offset;
            uint64_t actual_first = 0;
            if (query_capture(canonical_start, canonical_tap_count,
                              canonical_taps.data(), actual_first) !=
                    canonical_tap_count || actual_first != canonical_start) {
                fail("canonical tap missing or stale");
                return;
            }
            for (std::size_t i = 0; i < canonical_tap_count; ++i) {
                if (!canonical_taps[i].canonical_valid ||
                    canonical_taps[i].sample_rate != native_wall_canonical_rate_) {
                    fail("canonical tap epoch or rate mismatch");
                    return;
                }
            }
        }
        Mp2kWallStereoChunk chunk{};
        if (turbo_wall_mixer_.render(want, chunk) != want ||
            turbo_wall_mixer_.stats().fallback_required) {
            fail("wall mixer render failed");
            return;
        }
        for (uint32_t i = 0; i < want; ++i) {
            if (!std::isfinite(chunk.left[i]) ||
                !std::isfinite(chunk.right[i])) {
                fail("wall mixer produced nonfinite audio");
                return;
            }
            auto to_i16 = [](float value) {
                const float clipped = std::clamp(value, -1.0f, 1.0f);
                const int32_t sample = static_cast<int32_t>(
                    std::lround(clipped * 32767.0f));
                return static_cast<int16_t>(std::clamp(sample, -32767, 32767));
            };
            if (turbo) {
                native_ring_push(to_i16(chunk.left[i]), to_i16(chunk.right[i]));
                if (!turbo_audio_decoupled_) return;
            } else {
                const uint64_t frame_index = native_wall_candidate_frames_;
                if (frame_index > UINT64_MAX /
                    native_wall_canonical_rate_) {
                    fail("canonical tap timeline overflow");
                    return;
                }
                const uint64_t canonical_offset = (frame_index *
                    native_wall_canonical_rate_) /
                    Mp2kWallMixer::kCanonicalRenderRate;
                const std::size_t tap_index = static_cast<std::size_t>(
                    canonical_offset - canonical_first_offset);
                if (tap_index >= canonical_tap_count ||
                    !compose_mp2k_with_canonical(
                        chunk.right[i], chunk.left[i],
                        canonical_taps[tap_index],
                        /*include_separate_fifo=*/false,
                        native_wall_last_composed_left_,
                        native_wall_last_composed_right_)) {
                    fail("canonical tap composition failed");
                    return;
                }
                if (!native_wall_output_live_) {
                    native_wall_output_live_ = true;
                    std::fprintf(
                        stderr,
                        "[audio] Native MP2K wall output LIVE: synchronized "
                        "FullProducerSeed + canonical PSG stream\n");
                }
                native_ring_push(native_wall_last_composed_left_,
                                 native_wall_last_composed_right_);
                if (!native_wall_output_live_) return;
                if (native_wall_candidate_frames_ == UINT64_MAX) {
                    fail("wall candidate frame counter overflow");
                    return;
                }
                ++native_wall_candidate_frames_;
                ++native_wall_composed_frames_;
            }
        }
        frames -= want;
    }
    // Apply a boundary event even when the elapsed interval had <1 sample.
    const uint64_t final_wall_q32 = turbo_wall_mixer_.wall_cursor_q32();
    if (final_wall_q32 <= UINT64_MAX - anchor &&
        !turbo_audio_updates_.apply_due(
            turbo_wall_mixer_, anchor + final_wall_q32)) {
        fail("wall update queue failed");
    }
}

void GbaAudio::turbo_audio_service(uint64_t wall_ns) {
    if (!turbo_audio_decoupled_ || turbo_audio_muted_) return;
    service_wall_audio(wall_ns);
}

void GbaAudio::native_audio_service(uint64_t wall_ns) {
    if (turbo_audio_decoupled_ || !native_audio_requested_ ||
        !native_audio_enabled_ || native_wall_candidate_failed_)
        return;
    if (!shadow_.live()) return;
    if (!native_wall_candidate_active_ &&
        !native_audio_begin_wall(wall_ns)) return;
    service_wall_audio(wall_ns);
}

void GbaAudio::turbo_audio_exit(uint64_t wall_ns) {
    (void)wall_ns;
    if (!turbo_audio_decoupled_ && !turbo_audio_muted_) return;
    log_native_timeline_summary("turbo-exit");
    turbo_audio_scheduler_.exit_turbo(
        q32_seconds_from_frames(shadow_cursor_, 65536u), 0);
    turbo_audio_decoupled_ = false;
    turbo_audio_muted_ = false;
    turbo_audio_pending_ = false;
    turbo_audio_reason_logged_ = false;
    turbo_audio_reason_[0] = '\0';
    turbo_audio_last_wall_ns_ = 0;
    turbo_audio_sample_remainder_ = 0;
    turbo_audio_sequence_ = UINT64_MAX;
    turbo_audio_native_overwrites_ = 0;
    turbo_audio_uncapped_ = false;
    shadow_.set_published_block_callback(nullptr, nullptr);
    reset_native_wall_candidate();
    turbo_wall_mixer_.reset();
    turbo_audio_updates_.reset(0);
    native_head_ = native_tail_ = 0;
    std::fill(native_ring_.begin(), native_ring_.end(), int16_t{0});
    clear_stereo_fallback();
    refresh_mp2k_hook_filter();
}

void GbaAudio::turbo_audio_reset(uint64_t wall_ns) {
    (void)wall_ns;
    turbo_audio_scheduler_.reset(
        q32_seconds_from_frames(shadow_cursor_, 65536u), 0);
    turbo_audio_decoupled_ = false;
    turbo_audio_muted_ = false;
    turbo_audio_pending_ = false;
    turbo_audio_reason_logged_ = false;
    turbo_audio_reason_[0] = '\0';
    turbo_audio_last_wall_ns_ = 0;
    turbo_audio_sample_remainder_ = 0;
    turbo_audio_sequence_ = UINT64_MAX;
    turbo_audio_native_overwrites_ = 0;
    turbo_audio_uncapped_ = false;
    shadow_.set_published_block_callback(nullptr, nullptr);
    reset_native_wall_candidate();
    turbo_wall_mixer_.reset();
    turbo_audio_updates_.reset(0);
    native_head_ = native_tail_ = 0;
    std::fill(native_ring_.begin(), native_ring_.end(), int16_t{0});
    clear_stereo_fallback();
}

void GbaAudio::stereo_fallback_push(int16_t left, int16_t right) {
    if (!stereo_audio_requested() || stereo_fallback_ring_.empty()) return;
    const std::size_t cap = stereo_fallback_ring_.size() / 2;
    stereo_fallback_ring_[stereo_fallback_head_ * 2] = left;
    stereo_fallback_ring_[stereo_fallback_head_ * 2 + 1] = right;
    stereo_fallback_head_ = (stereo_fallback_head_ + 1) % cap;
    if (stereo_fallback_head_ == stereo_fallback_tail_)
        stereo_fallback_tail_ = (stereo_fallback_tail_ + 1) % cap;
}

void GbaAudio::clear_stereo_fallback() {
    stereo_fallback_head_ = stereo_fallback_tail_ = 0;
}

std::size_t GbaAudio::drain_stereo_fallback_samples(int16_t* out,
                                                     std::size_t max) {
    if (!stereo_audio_requested() || stereo_fallback_ring_.empty()) return 0;
    const std::size_t cap = stereo_fallback_ring_.size() / 2;
    std::size_t n = 0;
    while (n < max && stereo_fallback_tail_ != stereo_fallback_head_) {
        out[n * 2] = stereo_fallback_ring_[stereo_fallback_tail_ * 2];
        out[n * 2 + 1] = stereo_fallback_ring_[stereo_fallback_tail_ * 2 + 1];
        stereo_fallback_tail_ = (stereo_fallback_tail_ + 1) % cap;
        ++n;
    }
    return n;
}

void GbaAudio::fifo_reset(DirectFifo& fifo) {
    fifo = DirectFifo{};
}

void GbaAudio::fifo_clear_queue(DirectFifo& fifo) {
    fifo.write = 0;
    fifo.read = 0;
    fifo.count = 0;
}

void GbaAudio::fifo_push_word(DirectFifo& fifo, int fifo_id, uint32_t word) {
    if (fifo_id >= 0 && fifo_id < 2) {
        fifo_source_words_[fifo_id][fifo.write] =
            fifo_pending_source_valid_[fifo_id]
                ? fifo_pending_source_[fifo_id] : 0;
        fifo_pending_source_valid_[fifo_id] = false;
    }
    fifo.words[fifo.write] = word;
    fifo.write = static_cast<uint8_t>((fifo.write + 1) & 7u);
    if (fifo.count < 8) {
        ++fifo.count;
    } else {
        fifo.read = static_cast<uint8_t>((fifo.read + 1) & 7u);
    }
}

void GbaAudio::fifo_timer_step(DirectFifo& fifo, int fifo_id,
                               uint64_t cycles) {
    if (fifo.bytes_remaining == 0 && fifo.count != 0) {
        fifo_shift_source_[fifo_id] = fifo_source_words_[fifo_id][fifo.read];
        fifo.shift_word = fifo.words[fifo.read];
        fifo.read = static_cast<uint8_t>((fifo.read + 1) & 7u);
        --fifo.count;
        fifo.bytes_remaining = 4;
    } else if (fifo.bytes_remaining == 0) {
        fifo_shift_source_[fifo_id] = 0;
    }
    int8_t sample = static_cast<int8_t>(fifo.shift_word & 0xFFu);
    uint32_t slots = samples_per_event();
    uint32_t until_cycles = cycles_until_next_sample_event();
    uint32_t until = ((until_cycles - 1u) + cycles_per_sample_) /
        cycles_per_sample_;
    if (until > slots) until = slots;
    uint32_t start = slots - until;
    const uint32_t byte_index = 4u - fifo.bytes_remaining;
    if (event_capture_.enabled() && fifo.bytes_remaining != 0) {
        event_capture_.note_result(event_capture_.record_direct_fifo_consume(
            cycles, static_cast<uint8_t>(fifo_id),
            fifo_shift_source_[fifo_id] + byte_index,
            static_cast<uint8_t>(byte_index), sample));
    }
    if (fifo_shift_source_[fifo_id] != 0 && shadow_enabled_ &&
        shadow_.engaged()) {
        // The native timeline is reset on savestate load and render() reads
        // it with shadow_cursor_. samples_generated_ is restored from the
        // state and therefore belongs to a different absolute epoch.
        const uint64_t release_cursor = shadow_cursor_ + start + 1u;
        shadow_.producer_dma_consume(
            static_cast<uint8_t>(fifo_id),
            fifo_shift_source_[fifo_id] + byte_index,
            release_cursor, cycles);
    }
    for (uint32_t i = start; i < slots; ++i) {
        fifo.samples[i] = sample;
    }
    FifoTrace& trace = trace_[trace_write_];
    trace.sample_base = samples_generated_;
    trace.fifo_id = static_cast<uint8_t>(fifo_id);
    trace.until_cycles = until_cycles;
    trace.start_slot = start;
    trace.slots = slots;
    trace.count = fifo.count;
    trace.bytes_remaining = fifo.bytes_remaining;
    trace.sample = sample;
    trace_write_ = (trace_write_ + 1u) % kFifoTraceSize;
    if (trace_count_ < kFifoTraceSize) ++trace_count_;
    if (fifo.bytes_remaining != 0) {
        fifo.shift_word >>= 8;
        --fifo.bytes_remaining;
    }
}

void GbaAudio::update_soundbias(uint16_t value) {
    soundbias_ = value;
    uint32_t resolution = (value >> 14) & 0x3u;
    cycles_per_sample_ = 0x200u >> resolution;
    if (cycles_per_sample_ == 0) cycles_per_sample_ = 1;
    update_shadow_timing();
}

void GbaAudio::update_shadow_timing() {
    const uint32_t rate = sample_rate();
    shadow_.set_render_rate(rate);
    // SoundMain is driven by the GBA VBlank cadence, not a nominal 60 Hz
    // host clock. Keep the scheduler fractional: at 65536 Hz one frame is
    // 1097.25 samples, and rounding that to 1092 or 1097 every frame drifts
    // the shadow envelope against the guest over time.
    shadow_hook_interval_ = static_cast<uint64_t>(rate) *
                            static_cast<uint64_t>(GbaPpu::kCyclesPerFrame);
    shadow_hook_period_ = static_cast<double>(shadow_hook_interval_) /
                          static_cast<double>(kSystemHz);
    if (shadow_hook_interval_ == 0) shadow_hook_interval_ = 1;
    shadow_hook_phase_ = 0;
}

void GbaAudio::timer_overflow(int timer_id, uint64_t cycles) {
    // mGBA clocks a Direct Sound FIFO only while the audio master is enabled
    // and that FIFO is routed to at least one output side. Without this gate,
    // muting/restarting music consumes queued bytes while silent, so the next
    // note begins at the wrong FIFO phase.
    const bool a_enabled = master_enable_ &&
        (direct_a_left_ || direct_a_right_);
    const bool b_enabled = master_enable_ &&
        (direct_b_left_ || direct_b_right_);
    if (event_capture_.enabled()) {
        const uint8_t active_mask = static_cast<uint8_t>(
            (a_enabled ? 1u : 0u) | (b_enabled ? 2u : 0u));
        event_capture_.note_result(event_capture_.record_direct_fifo_timer(
            cycles, static_cast<uint8_t>(timer_id), active_mask));
    }
    if (a_enabled && timer_id == (direct_a_timer1_ ? 1 : 0)) {
        fifo_timer_step(fifo_a_, 0, cycles);
    }
    if (b_enabled && timer_id == (direct_b_timer1_ ? 1 : 0)) {
        fifo_timer_step(fifo_b_, 1, cycles);
    }
}

void GbaAudio::sound_fifo_dma_word(int fifo_id, uint32_t source_addr,
                                   uint64_t) {
    if (fifo_id < 0 || fifo_id > 1) return;
    fifo_pending_source_[fifo_id] = source_addr;
    fifo_pending_source_valid_[fifo_id] = true;
}

void GbaAudio::capture_sound_fifo_dma_word(int fifo_id,
                                           uint32_t source_addr,
                                           uint32_t value,
                                           uint64_t cycles) {
    if (!event_capture_.enabled() || fifo_id < 0 || fifo_id > 1) return;
    event_capture_.note_result(event_capture_.record_direct_fifo_dma_word(
        cycles, static_cast<uint8_t>(fifo_id), source_addr, value));
}

bool GbaAudio::fifo_needs_dma(int fifo_id) const {
    const DirectFifo& fifo = (fifo_id == 0) ? fifo_a_ : fifo_b_;
    const bool routed = fifo_id == 0
        ? (direct_a_left_ || direct_a_right_)
        : (direct_b_left_ || direct_b_right_);
    if (!master_enable_ || !routed) return false;
    // The hardware/mGBA model requests another four-word block only when
    // fewer than four words remain in the FIFO ring. `count` excludes the
    // internal shift word, matching the hardware FIFO-size calculation.
    return fifo.count < 4;
}

GbaAudio::FifoDebugState GbaAudio::debug_fifo_state(int fifo_id) const {
    const DirectFifo& fifo = (fifo_id == 0) ? fifo_a_ : fifo_b_;
    FifoDebugState out{};
    out.write = fifo.write;
    out.read = fifo.read;
    out.count = fifo.count;
    out.shift_word = fifo.shift_word;
    out.bytes_remaining = fifo.bytes_remaining;
    for (uint32_t i = 0; i < kMaxSamplesPerEvent; ++i) {
        out.samples[i] = fifo.samples[i];
    }
    return out;
}

GbaAudio::FifoTrace GbaAudio::debug_trace_entry(uint32_t index) const {
    if (index >= trace_count_) return FifoTrace{};
    uint32_t start = (trace_write_ + kFifoTraceSize - trace_count_) %
        kFifoTraceSize;
    return trace_[(start + index) % kFifoTraceSize];
}

// ─────────────────────────────────────────────────────────────────────
// Sample generation
// ─────────────────────────────────────────────────────────────────────

uint32_t GbaAudio::samples_per_event() const {
    uint32_t samples = kSampleEventCycles / cycles_per_sample_;
    if (samples == 0) samples = 1;
    if (samples > kMaxSamplesPerEvent) samples = kMaxSamplesPerEvent;
    return samples;
}

uint32_t GbaAudio::cycles_until_next_sample_event() const {
    if (cycle_accumulator_ >= kSampleEventCycles) return 1;
    uint32_t remaining = kSampleEventCycles - cycle_accumulator_;
    return remaining ? remaining : 1;
}

void GbaAudio::sample_until_current_time() {
    uint32_t slots = samples_per_event();
    uint32_t elapsed = cycle_accumulator_ / cycles_per_sample_;
    if (elapsed > slots) elapsed = slots;
    while (sample_index_ < elapsed) {
        current_samples_[sample_index_] = mix_one_sample(sample_index_);
        ++sample_index_;
    }
}

int16_t GbaAudio::mix_one_sample(uint32_t direct_slot, bool include_direct,
                                 ChannelTap* tap) {
    if (!master_enable_) return 0;

    // Per-channel contributions in the range [-15, +15]. Routing
    // bits gate whether the channel contributes to either side; we
    // mix mono for now and treat "either side enabled" as "audible".
    int32_t ch1_sample = 0;
    int32_t ch2_sample = 0;

    // ── Channel 1 (square + sweep) ───────────────────────────────
    if (ch1_.active) {
        uint32_t freq_period = (2048u - ch1_.frequency) * 16u;
        if (freq_period == 0) freq_period = 16u;
        ch1_.waveform_cycles += cycles_per_sample_;
        while (ch1_.waveform_cycles >= freq_period) {
            ch1_.waveform_cycles -= freq_period;
            ch1_.waveform_phase = (ch1_.waveform_phase + 1) & 7;
        }
        if (ch1_.envelope_step != 0) {
            ++ch1_.envelope_cycles;
            uint32_t env_period = ch1_.envelope_step * (sample_rate() / 64u);
            if (ch1_.envelope_cycles >= env_period) {
                ch1_.envelope_cycles = 0;
                if (ch1_.envelope_increase) {
                    if (ch1_.volume < 15) ++ch1_.volume;
                } else {
                    if (ch1_.volume > 0) --ch1_.volume;
                }
            }
        }
        if (ch1_.length_enabled) {
            ++ch1_.length_cycles;
            if (ch1_.length_cycles >= (sample_rate() / 256u)) {
                ch1_.length_cycles = 0;
                if (ch1_.length > 0) {
                    --ch1_.length;
                    if (ch1_.length == 0) ch1_.active = false;
                }
            }
        }
        // Sweep clock: 128 Hz (every 1/128 s adjust frequency by
        // freq +/- (freq >> shift)). sweep_time=0 disables.
        if (ch1_.sweep_time != 0) {
            ++ch1_.sweep_cycles;
            uint32_t sweep_period = ch1_.sweep_time * (sample_rate() / 128u);
            if (ch1_.sweep_cycles >= sweep_period) {
                ch1_.sweep_cycles = 0;
                uint16_t delta = static_cast<uint16_t>(
                    ch1_.frequency >> ch1_.sweep_shift);
                if (ch1_.sweep_decrease) {
                    if (delta <= ch1_.frequency) ch1_.frequency -= delta;
                } else {
                    if (ch1_.frequency + delta > 2047) {
                        // Sweep overflow disables the channel per spec.
                        ch1_.active = false;
                    } else {
                        ch1_.frequency = static_cast<uint16_t>(
                            ch1_.frequency + delta);
                    }
                }
            }
        }
        if (ch1_.active) {
            uint8_t high = kDutyPatterns[ch1_.duty][ch1_.waveform_phase];
            ch1_sample = (high ? 1 : -1) * static_cast<int32_t>(ch1_.volume);
        }
    }

    // ── Channel 2 ────────────────────────────────────────────────
    if (ch2_.active) {
        // Advance the waveform timer. The square-wave clock is
        // 131072 Hz scaled by (2048 - frequency). One waveform
        // period is 8 phases, so the per-phase increment in cycles
        // is (2048 - freq) * 16. (Per GBATEK § "GBA Sound Channel 2".)
        uint32_t freq_period = (2048u - ch2_.frequency) * 16u;
        ch2_.waveform_cycles += cycles_per_sample_;
        while (ch2_.waveform_cycles >= freq_period) {
            ch2_.waveform_cycles -= freq_period;
            ch2_.waveform_phase = (ch2_.waveform_phase + 1) & 7;
        }
        // Envelope clock: 1/64 second per step = kSampleRate/64 = 512
        // sample-ticks per step.
        if (ch2_.envelope_step != 0) {
            ++ch2_.envelope_cycles;
            uint32_t env_period = ch2_.envelope_step * (sample_rate() / 64u);
            if (ch2_.envelope_cycles >= env_period) {
                ch2_.envelope_cycles = 0;
                if (ch2_.envelope_increase) {
                    if (ch2_.volume < 15) ++ch2_.volume;
                } else {
                    if (ch2_.volume > 0) --ch2_.volume;
                }
            }
        }
        // Length clock: 256 Hz = kSampleRate/256 = 128 sample-ticks.
        if (ch2_.length_enabled) {
            ++ch2_.length_cycles;
            if (ch2_.length_cycles >= (sample_rate() / 256u)) {
                ch2_.length_cycles = 0;
                if (ch2_.length > 0) {
                    --ch2_.length;
                    if (ch2_.length == 0) ch2_.active = false;
                }
            }
        }
        if (ch2_.active) {
            uint8_t high = kDutyPatterns[ch2_.duty][ch2_.waveform_phase];
            ch2_sample = (high ? 1 : -1) * static_cast<int32_t>(ch2_.volume);
        }
    }

    // ── Channel 3 (wave RAM, 4-bit PCM) ──────────────────────────
    int32_t ch3_sample = 0;
    if (ch3_.active && ch3_.dac_on) {
        // mGBA's GBA PSG uses 2*(2048-frequency) PSG cycles and a 4x GBA
        // timing factor, i.e. 8*(2048-frequency) system cycles. The two-bank
        // mode exposes 64 digits; single-bank mode wraps after 32.
        uint32_t digit_period = (2048u - ch3_.frequency) * 8u;
        if (digit_period == 0) digit_period = 8u;
        uint32_t digits = ch3_.two_banks ? 64u : 32u;
        ch3_.wave_cycles += cycles_per_sample_;
        while (ch3_.wave_cycles >= digit_period) {
            ch3_.wave_cycles -= digit_period;
            ch3_.wave_pos = static_cast<uint8_t>((ch3_.wave_pos + 1u) % digits);
        }
        if (ch3_.length_enabled) {
            ++ch3_.length_cycles;
            if (ch3_.length_cycles >= (sample_rate() / 256u)) {
                ch3_.length_cycles = 0;
                if (ch3_.length > 0) {
                    --ch3_.length;
                    if (ch3_.length == 0) ch3_.active = false;
                }
            }
        }
        if (ch3_.active) {
            uint32_t abs_digit =
                (static_cast<uint32_t>(ch3_.bank) * 32u + ch3_.wave_pos) & 63u;
            uint8_t byte = wave_ram_[abs_digit >> 1];
            uint8_t digit = (abs_digit & 1u) ? (byte & 0x0Fu)
                                             : static_cast<uint8_t>(byte >> 4);
            // GBA wave output is an unsigned 4-bit DAC value. mGBA applies
            // the register's 3-bit volume encoding as shifts: 0=mute,
            // 1=100%, 2=50%, 3=25%, and 7=forced 75%.
            if (ch3_.force_volume) {
                ch3_sample = (static_cast<int32_t>(digit) * 3) >> 2;
            } else {
                switch (ch3_.volume_code) {
                    case 0:  ch3_sample = 0; break;
                    case 1:  ch3_sample = digit; break;
                    case 2:  ch3_sample = digit >> 1; break;
                    default: ch3_sample = digit >> 2; break;
                }
            }
        }
    }

    // ── Channel 4 (noise, LFSR) ──────────────────────────────────
    int32_t ch4_sample = 0;
    if (ch4_.active) {
        // This is the GBA polynomial used by mGBA: ratio 0 has a base period
        // of 32 system cycles, other ratios use 64, 128, ...; the feedback
        // taps are 0x6000 (15-bit) or 0x60 (7-bit).
        static constexpr uint32_t kNoiseDiv[8] =
            {8u, 16u, 32u, 48u, 64u, 80u, 96u, 112u};
        uint32_t step_period =
            (kNoiseDiv[ch4_.divisor_code] << ch4_.shift) * 4u;
        if (step_period == 0) step_period = 4u;
        ch4_.noise_cycles += cycles_per_sample_;
        while (ch4_.noise_cycles >= step_period) {
            ch4_.noise_cycles -= step_period;
            const uint32_t lsb = ch4_.lfsr & 1u;
            ch4_.lfsr = static_cast<uint16_t>(ch4_.lfsr >> 1);
            ch4_.lfsr = static_cast<uint16_t>(
                ch4_.lfsr ^ (lsb * (ch4_.width_7bit ? 0x60u : 0x6000u)));
            ch4_output_sample_ = static_cast<int8_t>(
                lsb ? ch4_.volume : 0);
        }
        if (ch4_.envelope_step != 0) {
            ++ch4_.envelope_cycles;
            uint32_t env_period = ch4_.envelope_step * (sample_rate() / 64u);
            if (ch4_.envelope_cycles >= env_period) {
                ch4_.envelope_cycles = 0;
                if (ch4_.envelope_increase) {
                    if (ch4_.volume < 15) ++ch4_.volume;
                } else {
                    if (ch4_.volume > 0) --ch4_.volume;
                }
            }
        }
        if (ch4_.length_enabled) {
            ++ch4_.length_cycles;
            if (ch4_.length_cycles >= (sample_rate() / 256u)) {
                ch4_.length_cycles = 0;
                if (ch4_.length > 0) {
                    --ch4_.length;
                    if (ch4_.length == 0) ch4_.active = false;
                }
            }
        }
        if (ch4_.active) ch4_sample = ch4_output_sample_;
    }

    // Match mGBA's GBAAudioSamplePSG + _applyBias path. The two output
    // sides must remain separate: Golden Sun commonly routes its music to
    // different Direct Sound buses, and averaging before the bias stage can
    // cancel those buses or change their clipping.
    // mix_psg_samples() applies the shared ×8 PSG scale to all four
    // channels equally, matching mGBA's GBAAudioSamplePSG
    // (third_party/mgba/src/gb/audio.c:752-763). See its definition above
    // for why ch3 previously being unscaled here was a bug.
    const PsgMixResult psg_mix = mix_psg_samples(
        PsgChannelSamples{ch1_sample, ch2_sample, ch3_sample, ch4_sample},
        PsgChannelRouting{ch1_left_enable_, ch1_right_enable_,
                          ch2_left_enable_, ch2_right_enable_,
                          ch3_left_enable_, ch3_right_enable_,
                          ch4_left_enable_, ch4_right_enable_});
    int32_t psg_left = psg_mix.left;
    int32_t psg_right = psg_mix.right;

    const uint32_t psg_volume = std::min<uint32_t>(dmg_volume_ratio_, 3u);
    const uint32_t psg_shift = 4u - psg_volume;
    psg_left >>= psg_shift;
    psg_right >>= psg_shift;
    psg_left *= 1 + static_cast<int32_t>(volume_l_);
    psg_right *= 1 + static_cast<int32_t>(volume_r_);

    DirectSoundRouteState route_state{};
    route_state.a_left = include_direct && direct_a_left_;
    route_state.a_right = include_direct && direct_a_right_;
    route_state.a_full_volume = direct_a_full_volume_;
    route_state.b_left = include_direct && direct_b_left_;
    route_state.b_right = include_direct && direct_b_right_;
    route_state.b_full_volume = direct_b_full_volume_;
    route_state.soundbias = soundbias_;
    const DirectSoundOutput mixed = mix_direct_sound_output(
        static_cast<float>(fifo_a_.samples[direct_slot]) / 128.0f,
        static_cast<float>(fifo_b_.samples[direct_slot]) / 128.0f,
        psg_left, psg_right, route_state);
    DirectSoundRouteState psg_state{};
    psg_state.soundbias = soundbias_;
    const DirectSoundOutput psg = mix_direct_sound_output(
        0.0f, 0.0f, psg_left, psg_right, psg_state);
    const int32_t left = static_cast<int32_t>(mixed.left);
    const int32_t right = static_cast<int32_t>(mixed.right);
    const int32_t psg_left_out = static_cast<int32_t>(psg.left);
    const int32_t psg_right_out = static_cast<int32_t>(psg.right);
    const int32_t psg_mono = (psg_left_out + psg_right_out) / 2;
    const int32_t out = (left + right) / 2;

    if (tap) {
        // Raw per-channel synthesis values for the always-on capture ring.
        // ch[] are the raw hardware-domain PSG values (deterministic from the
        // IO write stream → bit-reproducible). direct_* are the raw FIFO DAC
        // bytes for this slot ([-128,127]); zero when include_direct=false.
        tap->ch[0] = static_cast<int16_t>(ch1_sample);
        tap->ch[1] = static_cast<int16_t>(ch2_sample);
        tap->ch[2] = static_cast<int16_t>(ch3_sample);
        tap->ch[3] = static_cast<int16_t>(ch4_sample);
        tap->direct_a = include_direct
            ? static_cast<int16_t>(fifo_a_.samples[direct_slot]) : 0;
        tap->direct_b = include_direct
            ? static_cast<int16_t>(fifo_b_.samples[direct_slot]) : 0;
        tap->canonical_left = static_cast<int16_t>(std::clamp(left, -32767, 32767));
        tap->canonical_right = static_cast<int16_t>(std::clamp(right, -32767, 32767));
        tap->psg_left = static_cast<int16_t>(std::clamp(psg_left_out, -32767, 32767));
        tap->psg_right = static_cast<int16_t>(std::clamp(psg_right_out, -32767, 32767));
        tap->psg_mono = static_cast<int16_t>(std::clamp(psg_mono, -32767, 32767));
        tap->psg_left_input = psg_left;
        tap->psg_right_input = psg_right;
        tap->direct_mix = out - psg_mono;
        tap->direct_left_mix = left - psg_left_out;
        tap->direct_right_mix = right - psg_right_out;
    }
    return static_cast<int16_t>(out);
}

void GbaAudio::configure_shadow(const std::vector<Mp2kSig>& sigs,
                                const uint8_t* rom, std::size_t rom_len,
                                const uint8_t* ewram, std::size_t ewram_len,
                                const uint8_t* iwram, std::size_t iwram_len,
                                bool enable_request, bool native_request) {
    shadow_mem_.rom = rom;       shadow_mem_.rom_len = rom_len;
    shadow_mem_.ewram = ewram;   shadow_mem_.ewram_len = ewram_len;
    shadow_mem_.iwram = iwram;   shadow_mem_.iwram_len = iwram_len;
    audio_probe_enabled_ = std::getenv("GBARECOMP_AUDIO_PROBE") != nullptr;
    shadow_.init(sigs);
    refresh_mp2k_watch_ranges();
    shadow_cursor_ = 0;
    shadow_hook_phase_ = 0;
    update_shadow_timing();
    // Opt-in. `enable_request` is the per-game default ([audio].shadow in
    // game.toml); GBARECOMP_AUDIO_SHADOW overrides it ("0" forces off, any
    // other value forces on). Only takes effect if the ROM links MP2K.
    bool want = enable_request;
    if (const char* e = std::getenv("GBARECOMP_AUDIO_SHADOW")) {
        want = !(e[0] == '0' && e[1] == '\0');
    }
    bool native_want = native_request;
    bool stereo_want = native_request;
    if (const char* e = std::getenv("GBARECOMP_AUDIO_STEREO")) {
        stereo_want = !(e[0] == '0' && e[1] == '\0');
    }
    const char* turbo_env = std::getenv("GBARECOMP_TURBO_AUDIO");
    const char* strict_static_env = std::getenv("GBARECOMP_STRICT_STATIC");
    turbo_audio_requested_ = turbo_audio_env_allows_decoupled(
        turbo_env, strict_static_env);
    if (turbo_env && std::strcmp(turbo_env, "decoupled") == 0 &&
        strict_static_env && strict_static_env[0] &&
        strict_static_env[0] != '0') {
        std::fprintf(stderr,
                     "[audio] Turbo decoupled forced OFF by strict-static acceptance\n");
    }
    // GBARECOMP_TURBO_AUDIO=decoupled arms the verified shadow for the
    // experimental edge-triggered wall mixer, but does not substitute native
    // audio until Turbo actually enters.
    shadow_enabled_ = shadow_.armed() &&
        (want || native_want || turbo_audio_requested_);
    native_audio_enabled_ = shadow_.armed() && native_want;
    native_audio_requested_ = native_request;
    native_wall_candidate_active_ = false;
    native_wall_candidate_failed_ = false;
    native_wall_output_live_ = false;
    native_wall_failure_logged_ = false;
    native_wall_failure_reason_[0] = '\0';
    native_wall_last_wall_ns_ = 0;
    native_wall_sample_remainder_ = 0;
    native_wall_cursor_q32_ = 0;
    native_wall_sequence_ = UINT64_MAX;
    native_wall_candidate_frames_ = 0;
    native_wall_composed_frames_ = 0;
    native_wall_canonical_start_sample_ = 0;
    native_wall_canonical_rate_ = 0;
    native_wall_last_composed_left_ = 0;
    native_wall_last_composed_right_ = 0;
    canonical_stereo_requested_ = stereo_want;
    refresh_mp2k_hook_filter();
    // Camelot's producer has a measured dispatch boundary; defer its VBlank
    // fallback so that boundary callback is the sole snapshot for the block.
    // Direct SoundMainRAM hooks remain immediate and unchanged.
    mp2k_defer_vblank_snapshot_ = shadow_enabled_;
    native_live_reported_ = false;
    if (shadow_enabled_) {
        std::fprintf(stderr, "[audio] MP2K shadow mixer ARMED (verified-"
                             "enhancement; reverts to hardware mix on divergence)\n");
    }
    if (native_audio_enabled_)
        std::fprintf(stderr, "[audio] native MP2K stereo output REQUESTED (canonical fallback until verified)\n");
    if (turbo_audio_requested_)
        std::fprintf(stderr, "[audio] Turbo decoupled requested (default-off until Turbo edge verifies MP2K)\n");
}

void GbaAudio::update_mp2k_watch_ranges(uint32_t sound_info,
                                        uint32_t block_bytes,
                                        uint32_t dma_period) {
    if (sound_info == 0 || block_bytes == 0 || block_bytes > 4096 ||
        dma_period == 0) {
        mp2k_watch_ranges_valid_ = false;
        return;
    }
    const uint64_t ring_bytes = static_cast<uint64_t>(block_bytes) *
        static_cast<uint64_t>(dma_period + 2u) + 32u;
    const uint64_t chan_lo = static_cast<uint64_t>(sound_info) +
        kSoundChansOff;
    const uint64_t chan_hi = chan_lo +
        static_cast<uint64_t>(12u) * kSoundChanStride;
    const uint64_t ring_a_lo = static_cast<uint64_t>(sound_info) + 0x350u;
    const uint64_t ring_b_lo = static_cast<uint64_t>(sound_info) + 0x410u;
    if (chan_hi > UINT32_MAX || ring_a_lo + ring_bytes > UINT32_MAX + 1ull ||
        ring_b_lo + ring_bytes > UINT32_MAX + 1ull) {
        mp2k_watch_ranges_valid_ = false;
        return;
    }
    mp2k_watch_sound_info_ = sound_info;
    mp2k_watch_channel_lo_ = static_cast<uint32_t>(chan_lo);
    mp2k_watch_channel_hi_ = static_cast<uint32_t>(chan_hi);
    mp2k_watch_ring_a_lo_ = static_cast<uint32_t>(ring_a_lo);
    mp2k_watch_ring_a_hi_ = static_cast<uint32_t>(ring_a_lo + ring_bytes);
    mp2k_watch_ring_b_lo_ = static_cast<uint32_t>(ring_b_lo);
    mp2k_watch_ring_b_hi_ = static_cast<uint32_t>(ring_b_lo + ring_bytes);
    mp2k_watch_ranges_valid_ = true;
}

void GbaAudio::refresh_mp2k_watch_ranges() {
    uint32_t sound_info = 0;
    const uint8_t* head = nullptr;
    if (!shadow_mem_.u32(kSoundInfoPtr, sound_info) ||
        !(head = shadow_mem_.slice(sound_info, 0x50))) {
        mp2k_watch_ranges_valid_ = false;
        return;
    }
    const uint32_t block_bytes = load_u32le(head + 0x10);
    const uint32_t dma_period = std::clamp<uint32_t>(head[0x0B], 1u, 16u);
    update_mp2k_watch_ranges(sound_info, block_bytes, dma_period);
}

void GbaAudio::refresh_mp2k_hook_filter() {
    mp2k_hook_filter_ = Mp2kHookFastFilter{};
    mp2k_hook_filter_.valid = shadow_.hook_key_count() != 0;
    mp2k_hook_filter_.frame_enabled = shadow_enabled_;
    mp2k_hook_filter_.control_enabled =
        shadow_enabled_ && native_audio_requested_;
    mp2k_hook_filter_.control_pc = kMp2kControlHookPc;
    for (uint8_t i = 0; i < shadow_.hook_key_count() && i < 4u; ++i)
        mp2k_hook_filter_.premix_pc[mp2k_hook_filter_.premix_count++] =
            shadow_.hook_key_at(i);

    // These are the currently proven post-observer writer PCs. Dynamic
    // writers discovered later are appended by remember_mp2k_postmix_pc().
    for (const uint32_t pc : kMp2kKnownWriterPcs) {
        mp2k_hook_filter_.postmix_pc[
            mp2k_hook_filter_.postmix_count++] = pc;
    }
    for (const auto& writer : mp2k_pcm_writers_)
        remember_mp2k_postmix_pc(writer.pc);
}

void GbaAudio::remember_mp2k_postmix_pc(uint32_t pc) {
    if (!pc || !mp2k_hook_filter_.valid) return;
    const uint32_t normalized = pc & ~1u;
    for (uint8_t i = 0; i < mp2k_hook_filter_.postmix_count; ++i)
        if ((mp2k_hook_filter_.postmix_pc[i] & ~1u) == normalized) return;
    if (mp2k_hook_filter_.postmix_count <
        static_cast<uint8_t>(std::size(mp2k_hook_filter_.postmix_pc))) {
        mp2k_hook_filter_.postmix_pc[
            mp2k_hook_filter_.postmix_count++] = pc;
    }
}

bool GbaAudio::mp2k_write_may_be_relevant(uint32_t pc, uint32_t addr,
                                          uint32_t width) const {
    if (width == 0) return false;
    // Proven generated writers can touch relocated ring addresses; always
    // admit them. The exact address/range checks below handle generic stores.
    if (known_mp2k_writer_pc(pc)) return true;
    if (range_overlaps(addr, width, kSoundInfoPtr, kSoundInfoPtr + 4u))
        return true;
    if (!mp2k_watch_ranges_valid_) return true;  // fail-open during relocation
    if (range_overlaps(addr, width, mp2k_watch_sound_info_,
                       mp2k_watch_sound_info_ + 0x50u) ||
        range_overlaps(addr, width, mp2k_watch_channel_lo_,
                       mp2k_watch_channel_hi_) ||
        range_overlaps(addr, width, mp2k_watch_ring_a_lo_,
                       mp2k_watch_ring_a_hi_) ||
        range_overlaps(addr, width, mp2k_watch_ring_b_lo_,
                       mp2k_watch_ring_b_hi_))
        return true;
    return false;
}

void GbaAudio::update_mp2k_shadow_producer_context() {
    uint32_t writer_count = 0;
    uint32_t writer_bytes_max = 0;
    for (const auto& writer : mp2k_pcm_writers_) {
        if (writer.pc == 0) continue;
        ++writer_count;
        writer_bytes_max = std::max(writer_bytes_max, writer.block_bytes);
    }
    if (mp2k_c70_writer_.pc != 0) {
        ++writer_count;
        writer_bytes_max = std::max(writer_bytes_max,
                                    mp2k_c70_writer_.block_bytes);
    }
    shadow_.set_producer_hook_context(
        mp2k_active_block_id_, mp2k_pending_block_id_,
        mp2k_completed_routes_, writer_count, writer_bytes_max,
        mp2k_writer_completion_count_, mp2k_c70_writer_calls_,
        mp2k_c70_writer_completions_);
}

void GbaAudio::shadow_frame_hook_timed(
    const MemView& mem, uint64_t cursor, uint32_t key,
    Mp2kShadow::HookPhase phase, uint64_t block_id,
    uint64_t boundary_cycle, bool producer_block) {
    update_mp2k_shadow_producer_context();
    if (!audio_cost_probe_enabled()) {
        shadow_.frame_hook(mem, cursor, key, phase, block_id, boundary_cycle);
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    shadow_.frame_hook(mem, cursor, key, phase, block_id, boundary_cycle);
    const auto elapsed = static_cast<unsigned long long>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count());
    g_cost_mp2k_frame_hooks++;
    if (producer_block) {
        g_cost_mp2k_producer_blocks_ns += elapsed;
        g_cost_mp2k_producer_blocks++;
    }
}

bool GbaAudio::consume_mp2k_pending_block(uint32_t key,
                                          uint8_t hook_source,
                                          uint64_t cycles) {
    if (mp2k_pending_block_id_ == UINT64_MAX) return false;
    const uint64_t pending = mp2k_pending_block_id_;
    if (mp2k_active_block_id_ == UINT64_MAX ||
        pending != mp2k_active_block_id_ + 1u) {
        mp2k_pending_block_id_ = UINT64_MAX;
        shadow_.force_canonical_fallback(
            "pending producer sequence is not contiguous");
        return true;
    }
    // Clear first. Any re-entrant/duplicate hook must observe an already
    // consumed pending ID and cannot snapshot it twice.
    mp2k_pending_block_id_ = UINT64_MAX;
    mp2k_active_block_id_ = pending;
    mp2k_completed_routes_ = 0;
    mp2k_boundary_snapshot_pending_ = false;
    mp2k_boundary_snapshot_block_ = UINT64_MAX;
    mp2k_boundary_snapshot_cycles_ = cycles;
    mp2k_active_hook_cycles_ = cycles;
    last_guest_hook_cursor_ = shadow_cursor_;
    shadow_.set_snapshot_hook_identity(key & ~1u, hook_source);
    shadow_frame_hook_timed(shadow_mem_, shadow_cursor_, key,
                            Mp2kShadow::HookPhase::PreMix,
                            mp2k_active_block_id_, cycles);
    return true;
}

void GbaAudio::mp2k_frame_hook(uint32_t key, uint64_t cycles) {
    if (!shadow_enabled_ || !shadow_.matches_hook(key)) return;
    if (shadow_.canonical_fallback()) {
        mp2k_pending_block_id_ = UINT64_MAX;
        return;
    }
    // runtime_should_yield() can be revisited at the same guest PC when a
    // host yield unwinds before the instruction executes. Do not count that
    // retry as a second MP2K tick.
    if (cycles == mp2k_last_guest_hook_cycles_) return;
    mp2k_last_guest_hook_cycles_ = cycles;
    mp2k_active_hook_cycles_ = cycles;
    shadow_.set_snapshot_hook_identity(key & ~1u, 1);
    last_guest_hook_cursor_ = shadow_cursor_;
    if (consume_mp2k_pending_block(key, 1, cycles)) return;
    ++mp2k_active_block_id_;
    mp2k_completed_routes_ = 0;
    mp2k_boundary_snapshot_pending_ = false;
    mp2k_boundary_snapshot_block_ = UINT64_MAX;
    shadow_frame_hook_timed(shadow_mem_, shadow_cursor_, key,
                            Mp2kShadow::HookPhase::PreMix,
                            mp2k_active_block_id_);
}

void GbaAudio::mp2k_control_hook(uint32_t pc, uint32_t target,
                                 uint64_t cycles, uint8_t thumb) {
    constexpr uint32_t kCamelotBoundaryPc = 0x03000828u;
    if (!shadow_enabled_ || !native_audio_requested_ || thumb ||
        (pc & ~1u) != kCamelotBoundaryPc || target != kCamelotBoundaryPc)
        return;
    if (shadow_.canonical_fallback()) {
        mp2k_pending_block_id_ = UINT64_MAX;
        return;
    }
    if (mp2k_pending_block_id_ != UINT64_MAX) {
        consume_mp2k_pending_block(kCamelotBoundaryPc, 3, cycles);
        return;
    }
    if (!mp2k_boundary_snapshot_pending_ ||
        mp2k_active_block_id_ == UINT64_MAX ||
        mp2k_boundary_snapshot_block_ == mp2k_active_block_id_) return;
    mp2k_active_hook_cycles_ = cycles;
    shadow_.set_snapshot_hook_identity(kCamelotBoundaryPc, 3);
    shadow_frame_hook_timed(shadow_mem_, shadow_cursor_, kCamelotBoundaryPc,
                            Mp2kShadow::HookPhase::PreMix,
                            mp2k_active_block_id_, cycles);
    mp2k_boundary_snapshot_block_ = mp2k_active_block_id_;
    mp2k_boundary_snapshot_pending_ = false;
}

bool GbaAudio::valid_mp2k_channel_field(uint32_t field) {
    return field <= 0x09u || field == 0x18u || field == 0x1Cu ||
           field == 0x20u || field == 0x24u || field == 0x28u;
}

void GbaAudio::mp2k_pcm_write_hook(uint32_t pc, uint32_t addr,
                                    uint32_t value, uint32_t width,
                                    uint32_t operand_sample,
                                    uint32_t operand_gain,
                                    uint64_t cycles,
                                    uint32_t before_value, uint8_t thumb,
                                    uint8_t pre_write) {
    if (width == 0) return;
    if (!mp2k_write_may_be_relevant(pc, addr, width)) return;

    // Capture only the proven post-write semantic point. The runtime also
    // calls this observer before each store for identity diagnostics; that
    // pre-call must never become a second audio event.
    if (event_capture_.enabled() && !pre_write) {
        uint8_t channel = 0xFF;
        uint16_t field = 0xFFFF;
        uint32_t si_capture = 0;
        if (shadow_mem_.u32(kSoundInfoPtr, si_capture)) {
            refresh_mp2k_watch_ranges();
            const uint32_t chan_base = si_capture + kSoundChansOff;
            if (addr >= chan_base &&
                addr < chan_base + 12u * kSoundChanStride) {
                const uint32_t rel = addr - chan_base;
                channel = static_cast<uint8_t>(rel / kSoundChanStride);
                field = static_cast<uint16_t>(rel % kSoundChanStride);
            }
        }
        if (channel != 0xFF && valid_mp2k_channel_field(field)) {
            event_capture_.note_result(event_capture_.record_mp2k_channel(
                cycles, pc, addr, value, before_value, width, channel, field,
                thumb, AudioCaptureClass::Unknown));
        }
    }

    if (!shadow_enabled_ || !shadow_.armed()) return;
    // A canonical mismatch freezes the native path.  Clear any deferred ID
    // so later callbacks cannot mutate or resurrect shadow state.
    if (shadow_.canonical_fallback()) {
        mp2k_pending_block_id_ = UINT64_MAX;
        return;
    }

    if (audio_probe_enabled_) {
        static uint32_t final_store_logs = 0;
        if (pc == 0x03000C70u && final_store_logs < 8) {
            std::fprintf(stderr,
                         "[audio-producer-store] pc=0x%08x addr=0x%08x "
                         "width=%u seq=%llu\n",
                         pc, addr, width,
                         static_cast<unsigned long long>(mp2k_active_block_id_));
            ++final_store_logs;
        }
    }

    // The PCM ring is embedded at SoundInfo + 0x350.  The stock layout uses
    // 352-byte packed-word blocks; Camelot's world-map path uses 224-byte
    // byte-written blocks and rewrites the same slot on every tick. Count a
    // completed contiguous block per writer instead of looking for an
    // address discontinuity: the latter only sees the first block when a
    // driver reuses its output slot.
    uint32_t si = 0;
    if (!shadow_mem_.u32(kSoundInfoPtr, si)) return;
    const uint8_t* head = shadow_mem_.slice(si, 0x50);
    if (!head) return;
    const uint32_t block_bytes = load_u32le(head + 0x10);
    if (block_bytes == 0 || block_bytes > 4096) return;
    update_mp2k_watch_ranges(
        si, block_bytes, std::clamp<uint32_t>(head[0x0B], 1u, 16u));

    if (pre_write) {
        if (mp2k_active_block_id_ == UINT64_MAX) return;
        auto& probe = mp2k_guest_mix_probe_;
        if (probe.block_id != mp2k_active_block_id_) {
            probe = Mp2kGuestMixProbe{};
            probe.block_id = mp2k_active_block_id_;
        }
        const uint32_t chan_base = si + kSoundChansOff;
        if (addr >= chan_base && addr < chan_base + 8u * kSoundChanStride) {
            const uint32_t rel = addr - chan_base;
            const uint32_t field = rel % kSoundChanStride;
            if (field <= 0x09u || field == 0x18u || field == 0x1Cu ||
                field == 0x20u || field == 0x24u || field == 0x28u) {
                const uint32_t index = probe.channel_write_count %
                    probe.channel_writes.size();
                auto& w = probe.channel_writes[index];
                ++probe.channel_write_count;
                w.pc = pc; w.cycle = cycles; w.addr = addr;
                w.channel = static_cast<uint16_t>(rel / kSoundChanStride);
                w.field = static_cast<uint16_t>(field);
                w.before = before_value; w.value = value;
                w.width = static_cast<uint8_t>(width);
                w.mode = thumb ? 1u : 0u;
            }
        }
        return;
    }

    // Sequence-aware guest dry-mixer observation. PCM and FIX voices rewrite
    // the same accumulator block through four-word STMs at B4/A8C;
    // the destination reset identifies the next voice without relying on a
    // global instruction probe that can be confused by nested execution.
    if ((pc == 0x030008B4u || pc == 0x03000A8Cu) && width == 4 &&
        mp2k_active_block_id_ != UINT64_MAX) {
        auto& probe = mp2k_guest_mix_probe_;
        if (probe.block_id != mp2k_active_block_id_) {
            probe = Mp2kGuestMixProbe{};
            probe.block_id = mp2k_active_block_id_;
            probe.previous_voice.assign(block_bytes, 0);
            probe.base_addr = addr;
        } else if (probe.previous_voice.empty()) {
            probe.previous_voice.assign(block_bytes, 0);
            probe.base_addr = addr;
        } else if (probe.last_addr != 0 && addr <= probe.last_addr) {
            ++probe.voice;
            probe.word_index = 0;
        }
        ++probe.word_index;
        const uint32_t index = addr >= probe.base_addr &&
            ((addr - probe.base_addr) & 3u) == 0
            ? (addr - probe.base_addr) / 4u : UINT32_MAX;
        const uint32_t before = index < probe.previous_voice.size()
            ? probe.previous_voice[index] : 0;
        if (index < probe.previous_voice.size())
            probe.previous_voice[index] = value;
        if (index == 0 && probe.voice < probe.index0_steps.size()) {
            auto& step = probe.index0_steps[probe.voice];
            step.pc = pc;
            step.cycle = cycles;
            step.mode = thumb ? 1u : 0u;
            step.addr = addr;
            step.before = before;
            step.after = value;
            step.operand_sample = static_cast<int32_t>(operand_sample);
            step.operand_gain = operand_gain;
            // Every index-0 store is `after = before + gain * sample` in
            // plain 32-bit modular arithmetic (mp2k_packed_accumulate has no
            // per-voice saturation; that happens once after all voices are
            // summed), so this inversion is exact for any voice/sequence,
            // not only the seq5/voice3 debug case below.
            step.actual_before = value - operand_gain * operand_sample;
            if (probe.index0_count == 0)
                probe.first_write_cycles = cycles;
            probe.last_write_cycles = cycles;
            probe.index0_count = std::max(probe.index0_count,
                                          probe.voice + 1u);
            if (probe.index0_count == 1) {
                for (uint32_t ch = 0; ch < probe.write_channel_status.size(); ++ch) {
                    const uint32_t base = si + kSoundChansOff + ch * kSoundChanStride;
                    uint8_t status = 0, ctype = 0;
                    shadow_mem_.u8(base, status);
                    shadow_mem_.u8(base + 1u, ctype);
                    probe.write_channel_status[ch] =
                        static_cast<uint16_t>(status) |
                        (static_cast<uint16_t>(ctype) << 8);
                }
                auto& boundary = probe.boundary;
                boundary = CanonicalBoundaryTrace{};
                boundary.valid = true;
                boundary.sequence = mp2k_active_block_id_;
                boundary.first_write_cycle = cycles;
                boundary.first_write_pc = pc;
                boundary.first_write_addr = addr;
                boundary.first_write_mode = thumb ? 1u : 0u;
                boundary.control_cycle = mp2k_boundary_snapshot_cycles_;
                boundary.control_pc = 0x03000828u;
                boundary.control_target = 0x03000828u;
                boundary.control_kind = RUNTIME_TRACE_DISPATCH;
                boundary.control_mode = 0;
                boundary.channel_status = probe.write_channel_status;
                const uint32_t retained = std::min<uint32_t>(
                    probe.channel_write_count,
                    static_cast<uint32_t>(probe.channel_writes.size()));
                for (uint32_t n = 0; n < retained; ++n) {
                    const uint32_t pos = (probe.channel_write_count -
                        retained + n) % probe.channel_writes.size();
                    const auto& write = probe.channel_writes[pos];
                    if (write.cycle <= cycles) {
                        boundary.last_channel_cycle = write.cycle;
                        boundary.last_channel_pc = write.pc;
                        boundary.last_channel_addr = write.addr;
                        boundary.last_channel = write.channel;
                        boundary.last_field = write.field;
                        boundary.last_channel_before = write.before;
                        boundary.last_channel_value = write.value;
                    }
                }
                if (runtime_trace_copy_recent) {
                    RuntimeTraceEntry prior{};
                    if (runtime_trace_copy_recent(&prior, 1) == 1 &&
                        prior.cycles <= cycles &&
                        (prior.kind == RUNTIME_TRACE_DISPATCH ||
                         prior.kind == RUNTIME_TRACE_EXCHANGE ||
                         prior.kind == RUNTIME_TRACE_CALL)) {
                        boundary.control_cycle = prior.cycles;
                        boundary.control_pc = prior.pc;
                        boundary.control_target = prior.addr;
                        boundary.control_kind = prior.kind;
                        boundary.control_mode =
                            (prior.cpsr & CPSR_T_BIT) ? 1u : 0u;
                    }
                }
            }
            const uint32_t channel = probe.voice;
            if (channel < probe.voice_identities.size()) {
                const uint32_t base = si + kSoundChansOff +
                    channel * kSoundChanStride;
                const uint8_t* c = shadow_mem_.slice(base, 0x2Cu);
                if (c) {
                    auto& id = probe.voice_identities[probe.voice];
                    id.channel = static_cast<uint16_t>(channel);
                    id.channel_base = base;
                    id.status = c[0x00];
                    id.ctype = c[0x01];
                    id.gain_right = c[0x02];
                    id.gain_left = c[0x03];
                    id.count = load_u32le(c + 0x18);
                    id.frac = load_u32le(c + 0x1C);
                    id.frequency = load_u32le(c + 0x20);
                    id.wave = load_u32le(c + 0x24);
                    id.sample_cursor = 0;
                    shadow_mem_.u32(base + 0x28u, id.sample_cursor);
                    id.resolved_wave = mp2k_resolve_wave_address(
                        shadow_mem_, id.wave, 16);
                    id.resolved_data = id.resolved_wave
                        ? id.resolved_wave + 16u : 0u;
                    id.cursor = id.count;
                    id.wave_flags = id.wave_loop = id.wave_size = 0;
                    id.looped = 0;
                    if (const uint8_t* h = shadow_mem_.slice(id.wave, 0x10)) {
                        id.wave_flags = static_cast<uint32_t>(h[2]) |
                            (static_cast<uint32_t>(h[3]) << 8);
                        id.wave_loop = load_u32le(h + 0x08);
                        id.wave_size = load_u32le(h + 0x0C);
                        id.looped = (id.wave_flags & 0xC000u) != 0;
                    }
                }
            }
        }
        probe.last_addr = addr;
        if (probe.block_id == 5 && probe.voice == 2 && index == 0 &&
            pc == 0x030008B4u) {
            const uint32_t channel = 2;
            const uint32_t base = si + kSoundChansOff +
                channel * kSoundChanStride;
            uint32_t cp = 0;
            const uint8_t* c = shadow_mem_.slice(base, 0x2Cu);
            if (c && shadow_mem_.u32(base + 0x28u, cp)) {
                const uint32_t fw = load_u32le(c + 0x1Cu) & kMp2kFracMask;
                const uint32_t freq = load_u32le(c + 0x20u);
                const uint32_t wav = load_u32le(c + 0x24u);
                const uint32_t wave = mp2k_resolve_wave_address(
                    shadow_mem_, wav, 16);
                const uint32_t data = wave ? wave + 16u : 0;
                uint32_t cursor = 0;
                int32_t s0 = 0, s1 = 0;
                uint8_t b0 = 0, b1 = 0;
                if (data && mp2k_cursor_index(data,
                        wave ? load_u32le(shadow_mem_.slice(wave + 12u, 4)) : 0,
                        cp, cursor) &&
                    shadow_mem_.u8(cp, b0) && shadow_mem_.u8(cp + 1u, b1)) {
                    s0 = static_cast<int8_t>(b0);
                    s1 = static_cast<int8_t>(b1);
                }
                const int32_t sample = mp2k_interpolate_s8_x2(s0, s1, fw);
                std::fprintf(stderr,
                    "[audio-guest-mix] seq=5 invocation=%u voice=2 index=0 "
                    "before=0x%08x after=0x%08x cp=0x%08x cursor=%u "
                    "fw=0x%06x step=0x%08x s0=%d s1=%d sample=%d "
                    "gain=0x000d000d cycles=%llu\n",
                    probe.voice, before, value, cp, cursor, fw,
                    mp2k_step_q23(freq, 65536u), s0, s1, sample,
                    static_cast<unsigned long long>(cycles));
            }
        } else if (audio_probe_enabled_ &&
                   probe.block_id == 5 && probe.voice == 3 && index == 0 &&
                   pc == 0x03000A8Cu) {
            const uint32_t channel = 3;
            const uint32_t base = si + kSoundChansOff +
                channel * kSoundChanStride;
            uint32_t cp = 0;
            const uint8_t* c = shadow_mem_.slice(base, 0x2Cu);
            if (c && shadow_mem_.u32(base + 0x28u, cp)) {
                const uint32_t ct = load_u32le(c + 0x18u);
                const uint32_t fw = load_u32le(c + 0x1Cu) & kMp2kFracMask;
                uint8_t source = 0;
                shadow_mem_.u8(cp, source);
                const uint32_t gain_r = mp2k_gain_q9(c[0x02], c[0x09]);
                const uint32_t gain_l = mp2k_gain_q9(c[0x03], c[0x09]);
                const uint32_t raw_gain = (gain_r & 0xFFFFu) |
                    ((gain_l & 0xFFFFu) << 16);
                const int32_t actual_sample = static_cast<int32_t>(operand_sample);
                const uint32_t actual_before = value -
                    operand_gain * operand_sample;
                std::fprintf(stderr,
                    "[audio-guest-fix] seq=5 invocation=%u voice=3 index=0 "
                    "ctype=0x%02x ct=%u cp=0x%08x fw=0x%06x source=%d "
                    "gain_raw=0x%08x gain_halved=0x%08x operand_sample=%d "
                    "operand_gain=0x%08x prior_voice=0x%08x "
                    "actual_before=0x%08x after=0x%08x addr=0x%08x cycles=%llu\n",
                    probe.voice, c[0x01], ct, cp, fw,
                    static_cast<int32_t>(static_cast<int8_t>(source)),
                    raw_gain, mp2k_pack_stereo_gain(gain_r, gain_l),
                    actual_sample, operand_gain, before, actual_before,
                    value, addr,
                    static_cast<unsigned long long>(cycles));
            }
        }
    }

    // C70 is an intermediate 32-bit A/B producer block used to seed native
    // state. The hardware FIFO consumes the paired EWRAM route blocks written
    // by 0x03000BCC/BD4; the shadow keeps those routes as the signed/public
    // producer boundary.
    if (pc == 0x03000C70u) {
        const uint64_t c70_call = ++mp2k_c70_writer_calls_;
        if (audio_probe_enabled_ &&
            mp2k_active_block_id_ == 5) {
            static bool fix_cursor_logged = false;
            if (!fix_cursor_logged) {
                const uint32_t base = si + kSoundChansOff +
                    3u * kSoundChanStride;
                const uint8_t* c = shadow_mem_.slice(base, 0x2Cu);
                uint32_t cp = 0;
                if (c && shadow_mem_.u32(base + 0x28u, cp)) {
                    std::fprintf(stderr,
                        "[audio-guest-fix-final] seq=5 voice=3 ct=%u "
                        "cp=0x%08x fw=0x%06x\n",
                        load_u32le(c + 0x18u), cp,
                        load_u32le(c + 0x1Cu) & kMp2kFracMask);
                    fix_cursor_logged = true;
                }
            }
        }
        // C70 is the signed producer. Keep its progress out of the generic
        // writer pool so unrelated relevant/dynamic PCs cannot consume its
        // state slot and silently drop the producer block.
        Mp2kPcmWriter* writer = &mp2k_c70_writer_;
        writer->pc = pc;
        if (mp2k_active_block_id_ == UINT64_MAX) return;
        const uint32_t producer_bytes = block_bytes * 4u;
        if (mp2k_pending_block_id_ != UINT64_MAX) {
            // The deferred ID belongs to the next complete writer block.
            // Consume it before the first store of that block; a partial
            // writer here would mix two guest epochs and must fail closed.
            if (writer->block_bytes != 0) {
                mp2k_pending_block_id_ = UINT64_MAX;
                shadow_.force_canonical_fallback(
                    "pending producer consumed mid-block");
                return;
            }
            consume_mp2k_pending_block(pc, 4, cycles);
            if (shadow_.canonical_fallback()) return;
        }
        const bool contiguous = writer->block_bytes != 0 &&
            addr == writer->last_addr + writer->last_width;
        if (!contiguous) writer->block_bytes = 0;
        writer->last_addr = addr;
        writer->last_width = width;
        writer->block_bytes += width;
        const bool completed = writer->block_bytes >= producer_bytes;
        if (mp2k_c70_call_diag_logs_ < 8u) {
            std::fprintf(
                stderr,
                "[audio] MP2K C70 writer: call=%llu completions=%llu "
                "block=%llu route=2 addr=0x%08x width=%u progress=%u/%u "
                "frames=%u cursor=%llu cycles=%llu snapshot_pc=0x%08x "
                "snapshot_source=%u complete=%u\n",
                static_cast<unsigned long long>(c70_call),
                static_cast<unsigned long long>(mp2k_c70_writer_completions_),
                static_cast<unsigned long long>(mp2k_active_block_id_), addr,
                width, writer->block_bytes, producer_bytes, block_bytes,
                static_cast<unsigned long long>(shadow_cursor_),
                static_cast<unsigned long long>(cycles),
                shadow_.snapshot_hook_pc(),
                static_cast<unsigned>(shadow_.snapshot_hook_source()),
                completed ? 1u : 0u);
            ++mp2k_c70_call_diag_logs_;
        }
        if (!completed) return;
        writer->block_bytes %= producer_bytes;
        const uint32_t completed_start = addr + width - producer_bytes;
        ++mp2k_c70_writer_completions_;
        if (mp2k_c70_completion_diag_logs_ < 8u) {
            std::fprintf(
                stderr,
                "[audio] MP2K C70 completion: completion=%llu call=%llu "
                "block=%llu route=2 progress=0/%u frames=%u cursor=%llu "
                "cycles=%llu snapshot_pc=0x%08x snapshot_source=%u\n",
                static_cast<unsigned long long>(
                    mp2k_c70_writer_completions_),
                static_cast<unsigned long long>(c70_call),
                static_cast<unsigned long long>(mp2k_active_block_id_),
                producer_bytes, block_bytes,
                static_cast<unsigned long long>(shadow_cursor_),
                static_cast<unsigned long long>(cycles),
                shadow_.snapshot_hook_pc(),
                static_cast<unsigned>(shadow_.snapshot_hook_source()));
            ++mp2k_c70_completion_diag_logs_;
        }
        ++mp2k_writer_completion_count_;
        // Publish the guest boundary metadata before judging the completed
        // block. The post-probation reject diagnostic runs inside
        // producer_interleaved_block(); capturing here makes the same-block
        // cursor/fraction provenance available before that judge.
        CanonicalMixTrace canonical_mix{};
        canonical_mix.valid = mp2k_guest_mix_probe_.block_id ==
            mp2k_active_block_id_ && mp2k_guest_mix_probe_.index0_count != 0;
        canonical_mix.sequence = mp2k_active_block_id_;
        canonical_mix.base_addr = mp2k_guest_mix_probe_.base_addr;
        canonical_mix.count = mp2k_guest_mix_probe_.index0_count;
        canonical_mix.completion_cursor = shadow_cursor_;
        canonical_mix.completion_cycles = cycles;
        canonical_mix.pre_hook_cycles =
            mp2k_guest_mix_probe_.boundary.control_cycle != 0
                ? mp2k_guest_mix_probe_.boundary.control_cycle
                : mp2k_active_hook_cycles_;
        canonical_mix.first_write_cycles = mp2k_guest_mix_probe_.first_write_cycles;
        canonical_mix.last_write_cycles = mp2k_guest_mix_probe_.last_write_cycles;
        canonical_mix.write_channel_status =
            mp2k_guest_mix_probe_.write_channel_status;
        canonical_mix.steps = mp2k_guest_mix_probe_.index0_steps;
        canonical_mix.channel_write_count =
            mp2k_guest_mix_probe_.channel_write_count;
        canonical_mix.channel_write_total =
            mp2k_guest_mix_probe_.channel_write_count;
        canonical_mix.channel_writes = mp2k_guest_mix_probe_.channel_writes;
        canonical_mix.voice_identities =
            mp2k_guest_mix_probe_.voice_identities;
        if (mp2k_guest_mix_probe_.boundary.valid) {
            auto boundary = mp2k_guest_mix_probe_.boundary;
            boundary.voice_identities =
                mp2k_guest_mix_probe_.voice_identities;
            mp2k_boundary_ring_[mp2k_boundary_write_] = boundary;
            mp2k_boundary_write_ = (mp2k_boundary_write_ + 1u) %
                static_cast<uint32_t>(mp2k_boundary_ring_.size());
            mp2k_boundary_count_ = std::min<uint32_t>(
                mp2k_boundary_count_ + 1u,
                static_cast<uint32_t>(mp2k_boundary_ring_.size()));
        }
        shadow_.capture_canonical_mix_trace(canonical_mix);
        shadow_.producer_interleaved_block(
            shadow_mem_, completed_start, block_bytes, mp2k_active_block_id_);
        if (shadow_.canonical_fallback()) {
            mp2k_pending_block_id_ = UINT64_MAX;
            return;
        }
        // Repeat the capture after the judge so the existing first-rejection
        // canonical trace remains available when this block becomes the
        // first rejected block.
        shadow_.capture_canonical_mix_trace(canonical_mix);
        shadow_.annotate_bad_wave_writes(
            mp2k_guest_mix_probe_.channel_writes,
            mp2k_guest_mix_probe_.channel_write_count);
        shadow_frame_hook_timed(shadow_mem_, shadow_cursor_, pc,
                                Mp2kShadow::HookPhase::PostMix,
                                mp2k_active_block_id_, 0, true);
        if (shadow_.canonical_fallback()) {
            mp2k_pending_block_id_ = UINT64_MAX;
            return;
        }
        // Producer metadata is meaningful only after the shadow's verified
        // gate is live. Capture alone must not force/arm the enhancement.
        if (event_capture_.enabled() && shadow_.live()) {
            const uint64_t start_cycles =
                mp2k_guest_mix_probe_.first_write_cycles != 0
                    ? mp2k_guest_mix_probe_.first_write_cycles
                    : mp2k_active_hook_cycles_;
            event_capture_.note_result(event_capture_.record_producer_block(
                start_cycles, cycles, mp2k_active_block_id_, 0xFF,
                shadow_.producer_pcm_rate(), block_bytes, completed_start, 0,
                shadow_.producer_block_start_cursor(),
                shadow_.producer_pcm_rate()));
        }
        // This Camelot path has no separate SoundMainRAM entry on subsequent
        // blocks. Reserve the next ID, but let the current native tail finish
        // before taking the next live snapshot. The next writer, a real
        // SoundMainRAM hook, or the guarded VBlank fallback consumes it once.
        if (mp2k_pending_block_id_ != UINT64_MAX ||
            mp2k_active_block_id_ == UINT64_MAX - 1u) {
            mp2k_pending_block_id_ = UINT64_MAX;
            shadow_.force_canonical_fallback(
                "duplicate pending producer sequence");
            return;
        }
        mp2k_pending_block_id_ = mp2k_active_block_id_ + 1u;
        mp2k_have_block_cycles_ = true;
        mp2k_last_block_cycles_ = cycles;
        mp2k_completed_routes_ = 0;
        return;
    }
    const uint32_t dma_period = std::min<uint32_t>(
        std::max<uint32_t>(head[0x0B], 1u), 16u);
    // Camelot keeps the two routed PCM copies at opposite ends of the
    // software ring. The second copy can land exactly one ring-span past
    // the first base, so include that endpoint plus one block.
    const uint64_t ring_bytes = static_cast<uint64_t>(block_bytes) *
        static_cast<uint64_t>(dma_period + 2u) + 32u;
    Mp2kPcmWriter* writer = nullptr;
    const uint32_t normalized_writer_pc = pc & ~1u;
    const auto log_dry_writer = [&](const char* event, uint8_t route,
                                    uint32_t route_reason, bool recognized,
                                    uint32_t progress, bool complete,
                                    uint32_t writer_pc) {
        if (mp2k_dry_writer_diag_logs_ >= 16u) return;
        std::fprintf(
            stderr,
            "[audio] MP2K dry writer diagnostic: event=%s pc=0x%08x "
            "pc_norm=0x%08x writer=0x%08x recognized=%u route=%u "
            "route_reason=%u addr=0x%08x progress=%u expected=%u "
            "complete=%u active=%llu pending=%llu producer=%llu\n",
            event ? event : "unknown", pc, normalized_writer_pc, writer_pc,
            recognized ? 1u : 0u, static_cast<unsigned>(route), route_reason,
            addr, progress, block_bytes, complete ? 1u : 0u,
            static_cast<unsigned long long>(mp2k_active_block_id_),
            static_cast<unsigned long long>(mp2k_pending_block_id_),
            static_cast<unsigned long long>(shadow_.producer_block_id()));
        ++mp2k_dry_writer_diag_logs_;
    };
    const uint32_t normalized_pc = pc & ~1u;
    for (auto& candidate : mp2k_pcm_writers_) {
        if (candidate.pc != 0 && (candidate.pc & ~1u) == normalized_pc) {
            writer = &candidate;
            break;
        }
    }
    if (!writer) {
        // Relevance intentionally fails open while SoundInfo/ring ranges are
        // being discovered, but those unrelated stores must not consume the
        // reserved dynamic slot before MP2K owns a live producer epoch.
        if (mp2k_active_block_id_ == UINT64_MAX) {
            log_dry_writer("pre-ownership", 0xFFu, 5u, false, 0, false, 0);
            return;
        }
        for (auto& candidate : mp2k_pcm_writers_) {
            if (candidate.pc == 0) {
                candidate.pc = normalized_pc;
                writer = &candidate;
                remember_mp2k_postmix_pc(pc);
                break;
            }
        }
    }
    if (!writer) {
        log_dry_writer("no-slot", 0xFFu, 3u, false, 0, false, 0);
        return;  // More than the recognized writer set is unknown.
    }
    if (mp2k_active_block_id_ == UINT64_MAX) {
        log_dry_writer("pre-ownership", 0xFFu, 5u, false, 0, false,
                       writer->pc);
        return;
    }
    if (mp2k_pending_block_id_ != UINT64_MAX) {
        // Consume the deferred epoch before any dry/reverb probe writes.
        if (writer->block_bytes != 0) {
            log_dry_writer("pending-mid-block", 0xFFu, 4u, false,
                           writer->block_bytes, false, writer->pc);
            mp2k_pending_block_id_ = UINT64_MAX;
            shadow_.force_canonical_fallback(
                "pending dry producer consumed mid-block");
            return;
        }
        consume_mp2k_pending_block(pc, 4, cycles);
        if (shadow_.canonical_fallback()) return;
    }
    // GS1's live state-5 layout is proven by the DMA trace: A begins at
    // SoundInfo+0x410 and B is 0x630 bytes later. Keep the stock +0x350
    // layout as the other known MP2K producer location, but select only a
    // base that actually contains this completed write.
    uint8_t route = 0xFFu;
    const bool proven_gs1_route = mp2k_gs1_dry_route(pc, route);
    if (proven_gs1_route && mp2k_active_block_id_ != UINT64_MAX) {
        uint32_t guest_old = 0;
        if (shadow_mem_.u32(addr, guest_old)) {
            shadow_.reverb_history_word(route, addr, guest_old,
                                        mp2k_active_block_id_);
            if (shadow_.canonical_fallback()) {
                mp2k_pending_block_id_ = UINT64_MAX;
                return;
            }
        }
    }
    uint32_t pcm_base = 0;
    for (const uint32_t offset : {0x350u, 0x410u}) {
        const uint32_t candidate = si + offset;
        if (addr >= candidate &&
            static_cast<uint64_t>(addr - candidate) < ring_bytes) {
            pcm_base = candidate;
            break;
        }
    }
    if (pcm_base == 0 && !proven_gs1_route) {
        log_dry_writer("unresolved-route", 0xFFu, 2u, false,
                       writer->block_bytes, false, writer->pc);
        return;
    }

    const bool contiguous = writer->block_bytes != 0 &&
        addr == writer->last_addr + writer->last_width;
    if (!contiguous) writer->block_bytes = 0;
    writer->last_addr = addr;
    writer->last_width = width;
    writer->block_bytes += width;
    const uint32_t progress = writer->block_bytes;
    const bool complete = progress >= block_bytes;
    log_dry_writer(
        complete ? "complete" : (contiguous ? "partial" : "restart"),
        route, proven_gs1_route ? 0u : 1u, proven_gs1_route, progress,
        complete, writer->pc);
    if (!complete) return;
    writer->block_bytes %= block_bytes;

    if (mp2k_active_block_id_ == UINT64_MAX) return;
    // The writer completion points at the end of the contiguous block.  The
    // producer comparison must read exactly that newly written block, not
    // whatever the delayed Direct Sound FIFO currently consumes.
    const uint32_t completed_end = addr + width;
    const uint32_t completed_start = completed_end - block_bytes;
    const uint64_t block_offset = pcm_base != 0 && completed_start >= pcm_base
        ? static_cast<uint64_t>(completed_start - pcm_base) : ring_bytes;
    if (!proven_gs1_route) {
        if (block_offset >= ring_bytes) return;
        route = block_offset < ring_bytes / 2 ? 0 : 1;
    }
    if (audio_probe_enabled_ &&
        mp2k_active_block_id_ == 5) {
        std::fprintf(stderr,
                     "[audio-dry-capture] seq=5 pc=0x%08x route=%u "
                     "start=0x%08x end=0x%08x base=0x%08x "
                     "offset=0x%llx bytes=%u\n",
                     pc, route, completed_start, completed_end, pcm_base,
                     static_cast<unsigned long long>(block_offset),
                     block_bytes);
    }
    // These are dry-ring completions. They remain useful tick evidence, but
    // are deliberately excluded from the signed producer oracle.
    ++mp2k_writer_completion_count_;
    shadow_.diagnostic_dry_block(shadow_mem_, completed_start, block_bytes,
                                 route, mp2k_active_block_id_);
    if (shadow_.canonical_fallback()) {
        mp2k_pending_block_id_ = UINT64_MAX;
        return;
    }
    // Producer metadata is a Stage-3 handoff: require the already-verified
    // shadow, and retain the exact guest sample cursor when available.
    if (event_capture_.enabled() && shadow_.live()) {
        const uint64_t start_cycles =
            mp2k_guest_mix_probe_.first_write_cycles != 0
                ? mp2k_guest_mix_probe_.first_write_cycles
                : mp2k_active_hook_cycles_;
        event_capture_.note_result(event_capture_.record_producer_block(
            start_cycles, cycles, mp2k_active_block_id_, route,
            shadow_.producer_pcm_rate(), block_bytes, completed_start, 0,
            shadow_.producer_block_start_cursor(),
            shadow_.producer_pcm_rate()));
    }
    mp2k_completed_routes_ = static_cast<uint8_t>(
        mp2k_completed_routes_ | static_cast<uint8_t>(1u << route));
    // Both routed copies are required before the post-mix marker is emitted.
    // It records evidence only; Mp2kShadow never advances from this state.
    if (mp2k_completed_routes_ == 3) {
        mp2k_have_block_cycles_ = true;
        mp2k_last_block_cycles_ = cycles;
        shadow_frame_hook_timed(shadow_mem_, shadow_cursor_, pc,
                                Mp2kShadow::HookPhase::PostMix,
                                mp2k_active_block_id_, 0, true);
        if (shadow_.canonical_fallback()) {
            mp2k_pending_block_id_ = UINT64_MAX;
            return;
        }
        mp2k_completed_routes_ = 0;
    }
}

void GbaAudio::mp2k_vblank_hook(uint64_t cycles) {
    if (!shadow_enabled_ || !shadow_.armed()) return;
    constexpr uint64_t kFrameCycles = 280896u;

    // Bounded boot/savestate evidence: a VBlank can arm a deferred producer
    // epoch without engaging the shadow.  Log the state transition so a
    // waiting boot is distinguishable from a missing producer boundary.
    if (mp2k_boot_probe_diag_logs_ < 8u) {
        uint32_t sound_info = 0, ident = 0, spv = 0, pcm_freq = 0;
        const bool sound_info_ok = shadow_mem_.u32(kSoundInfoPtr, sound_info);
        const bool ident_ok = sound_info_ok && shadow_mem_.u32(sound_info, ident);
        const bool timing_ok = sound_info_ok &&
            shadow_mem_.u32(sound_info + 0x10u, spv) &&
            shadow_mem_.u32(sound_info + 0x14u, pcm_freq);
        const bool live_state = sound_info_ok && ident_ok && timing_ok &&
            ident >= kMp2kMagicBase &&
            ident - kMp2kMagicBase <= 8u && spv != 0 && pcm_freq != 0;
        const uint8_t stage = shadow_.canonical_fallback() ? 3u :
            (shadow_.engaged() ? 2u : (live_state ? 1u : 0u));
        if (stage != mp2k_boot_probe_last_stage_) {
            const char* stage_name = stage == 0u ? "waiting-state" :
                (stage == 1u ? "state-present-boundary-pending" :
                 (stage == 2u ? "engaged" : "canonical-fallback"));
            std::fprintf(
                stderr,
                "[audio] MP2K boot probe: stage=%s source=vblank "
                "deferred=%u active_block=%llu pending=%llu "
                "sound_info=0x%08x valid=%u ident=0x%08x spv=%u pcm=%u "
                "hooks=%llu engaged=%u fallback=%u\n",
                stage_name, mp2k_defer_vblank_snapshot_ ? 1u : 0u,
                static_cast<unsigned long long>(mp2k_active_block_id_),
                static_cast<unsigned long long>(mp2k_pending_block_id_),
                sound_info, live_state ? 1u : 0u, ident, spv, pcm_freq,
                static_cast<unsigned long long>(shadow_.hooks()),
                shadow_.engaged() ? 1u : 0u,
                shadow_.canonical_fallback() ? 1u : 0u);
            mp2k_boot_probe_last_stage_ = stage;
            ++mp2k_boot_probe_diag_logs_;
        }
    }
    if (shadow_.canonical_fallback()) {
        // Keep the terminal boot-probe stage visible.  The normal fallback
        // path below intentionally does not mutate pending producer state.
        mp2k_pending_block_id_ = UINT64_MAX;
        return;
    }
    if (mp2k_have_block_cycles_ && cycles >= mp2k_last_block_cycles_ &&
        cycles - mp2k_last_block_cycles_ <= kFrameCycles * 2u) {
        return;
    }
    if (cycles == mp2k_last_guest_hook_cycles_) return;
    mp2k_last_guest_hook_cycles_ = cycles;
    mp2k_active_hook_cycles_ = cycles;
    mp2k_boundary_snapshot_cycles_ = cycles;
    shadow_.set_snapshot_hook_identity(shadow_.hook_key0() & ~1u, 2);
    last_guest_hook_cursor_ = shadow_cursor_;

    if (mp2k_pending_block_id_ != UINT64_MAX) {
        // A guarded VBlank fallback may consume the reserved ID, but must not
        // allocate another sequence or snapshot the same block twice.
        consume_mp2k_pending_block(shadow_.hook_key0(), 2, cycles);
        return;
    }
    if (mp2k_defer_vblank_snapshot_) {
        // The measured control boundary is the sole snapshot for a deferred
        // VBlank block. Reserve one ID for the pending boundary and keep it
        // stable until that callback consumes it; repeated VBlanks must not
        // manufacture skipped producer IDs.
        if (!mp2k_boundary_snapshot_pending_) {
            ++mp2k_active_block_id_;
            mp2k_completed_routes_ = 0;
            mp2k_boundary_snapshot_pending_ = true;
            mp2k_boundary_snapshot_block_ = UINT64_MAX;
        }
        return;
    }
    ++mp2k_active_block_id_;
    mp2k_completed_routes_ = 0;
    shadow_frame_hook_timed(shadow_mem_, shadow_cursor_, shadow_.hook_key0(),
                            Mp2kShadow::HookPhase::PreMix,
                            mp2k_active_block_id_);
}

void GbaAudio::run_sample_event() {
    uint32_t samples = samples_per_event();
    CapSample caps[kMaxSamplesPerEvent] = {};
    while (sample_index_ < samples) {
        uint32_t slot = sample_index_;
        float sl = 0.0f, sr = 0.0f;
        // Keep the native shadow cursor in host samples. The MP2K frame hook
        // itself is delivered by the guest's SoundMainRAM dispatch, not by
        // this renderer's sample scheduler.
        ++shadow_cursor_;

        // Generate the canonical hardware mix once. The Direct Sound FIFO
        // bytes are the native MP2K handoff oracle; final stereo remains the
        // fallback stream and preserves the PSG/hardware routing.
        ChannelTap tap;
        const int16_t canonical_s =
            mix_one_sample(slot, /*include_direct=*/true, &tap);
        tap.canonical_valid = true;
        tap.sample_rate = sample_rate();
        tap.direct_route.a_left = direct_a_left_;
        tap.direct_route.a_right = direct_a_right_;
        tap.direct_route.a_full_volume = direct_a_full_volume_;
        tap.direct_route.b_left = direct_b_left_;
        tap.direct_route.b_right = direct_b_right_;
        tap.direct_route.b_full_volume = direct_b_full_volume_;
        tap.direct_route.soundbias = soundbias_;

        if (shadow_enabled_ && shadow_.engaged()) {
            std::string degraded;
            float candidate_a = 0.0f, candidate_b = 0.0f;
            const bool cost_probe = audio_cost_probe_enabled();
            const auto render_start = cost_probe
                ? std::chrono::steady_clock::now()
                : std::chrono::steady_clock::time_point{};
            const bool candidate_ready = shadow_.render(
                shadow_mem_, shadow_cursor_, candidate_a, candidate_b);
            if (cost_probe) {
                g_cost_mp2k_render_ns += static_cast<unsigned long long>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - render_start).count());
                ++g_cost_mp2k_render_calls;
            }
            if (candidate_ready) {
                DirectSoundRouteState route_state{};
                route_state.a_left = direct_a_left_;
                route_state.a_right = direct_a_right_;
                route_state.a_full_volume = direct_a_full_volume_;
                route_state.b_left = direct_b_left_;
                route_state.b_right = direct_b_right_;
                route_state.b_full_volume = direct_b_full_volume_;
                route_state.soundbias = soundbias_;
                const DirectSoundOutput candidate = mix_direct_sound_output(
                    candidate_a, candidate_b, tap.psg_left_input,
                    tap.psg_right_input, route_state);
                constexpr float kOutputDomain = 32768.0f;
                sl = candidate.left / kOutputDomain;
                sr = candidate.right / kOutputDomain;
                const auto judge_start = cost_probe
                    ? std::chrono::steady_clock::now()
                    : std::chrono::steady_clock::time_point{};
                shadow_.judge_output(
                    shadow_cursor_,
                    static_cast<float>(tap.canonical_left) / kOutputDomain,
                    static_cast<float>(tap.canonical_right) / kOutputDomain,
                    sl, sr,
                    Mp2kOutputRouteObservation{
                        candidate_a, candidate_b,
                        static_cast<float>(tap.direct_a) / 128.0f,
                        static_cast<float>(tap.direct_b) / 128.0f,
                        static_cast<uint8_t>(
                            (route_state.a_left ? 0x01u : 0u) |
                            (route_state.a_right ? 0x02u : 0u) |
                            (route_state.b_left ? 0x04u : 0u) |
                            (route_state.b_right ? 0x08u : 0u)),
                        static_cast<uint8_t>(
                            (route_state.a_full_volume ? 0x01u : 0u) |
                            (route_state.b_full_volume ? 0x02u : 0u)),
                        tap.psg_left_input, tap.psg_right_input,
                        route_state.soundbias},
                    degraded);
                if (cost_probe) {
                    g_cost_mp2k_judge_output_ns +=
                        static_cast<unsigned long long>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - judge_start).count());
                    ++g_cost_mp2k_judge_output_calls;
                }
                Mp2kOutputDomainDiagnostic output_diag{};
                if (shadow_.take_first_output_domain_diagnostic(output_diag))
                    log_mp2k_output_domain_diagnostic(output_diag);
                if (audio_probe_enabled_ &&
                    shadow_cursor_ < last_guest_hook_cursor_ + 64u) {
                    std::fprintf(stderr,
                        "[audio-output-domain] cursor=%llu fifo=(%d,%d) "
                        "route=%d%d%d%d full=%d%d psg=(%d,%d) bias=%u "
                        "native_byte=(%.3f,%.3f) canon=(%d,%d) "
                        "native=(%.1f,%.1f)\n",
                        static_cast<unsigned long long>(shadow_cursor_),
                        tap.direct_a, tap.direct_b,
                        direct_a_left_, direct_a_right_, direct_b_left_,
                        direct_b_right_, direct_a_full_volume_,
                        direct_b_full_volume_, tap.psg_left_input,
                        tap.psg_right_input, soundbias_ & 0x03FFu,
                        candidate_a * 128.0f, candidate_b * 128.0f,
                        tap.canonical_left, tap.canonical_right,
                        candidate.left, candidate.right);
                }
            }
            if (!degraded.empty()) {
                std::fprintf(stderr, "[audio] MP2K shadow DEGRADED: %s\n",
                             degraded.c_str());
                log_native_timeline_summary("degraded");
                if (turbo_audio_decoupled_)
                    turbo_audio_fail("MP2K verifier divergence");
                else if (native_wall_candidate_active_)
                    native_audio_fail("MP2K verifier divergence");
                // A verifier pause is a definitive enhancement failure
                // for this run. Keep the canonical guest mixer as the oracle
                // and stop rendering the rejected shadow on every subsequent
                // sample; continuing here can monopolize the VBlank IRQ's
                // wall time and look like a game freeze.
                shadow_enabled_ = false;
                native_audio_enabled_ = false;
                if (!native_audio_requested_)
                    clear_stereo_fallback();
                std::fprintf(stderr,
                             "[audio] MP2K native output OFF: canonical "
                             "mixer restored\n");
            }
        }

        // A native shadow that cannot pass repeated probation windows is not
        // an enhancement candidate for this driver. A boot can legitimately
        // have no SoundInfo yet, so stale hooks do not spend this budget.
        if (shadow_enabled_ && shadow_.engaged() && !shadow_.live() &&
            shadow_.probation_failures() >= kNativeProbeFailureLimit) {
            const bool native_requested = native_audio_enabled_;
            const float r = shadow_.last_correlation();
            const float ratio = shadow_.last_level_ratio();
            log_native_timeline_summary("probation-failed");
            if (turbo_audio_decoupled_)
                turbo_audio_fail("MP2K verifier probation failed");
            else if (native_wall_candidate_active_)
                native_audio_fail("MP2K verifier probation failed");
            shadow_enabled_ = false;
            native_audio_enabled_ = false;
            native_live_reported_ = false;
            native_head_ = native_tail_ = 0;
            if (!native_audio_requested_)
                clear_stereo_fallback();
            std::fprintf(
                stderr,
                "[audio] %s OFF: probation failed (correlation %.2f, "
                "level ratio %.2f); canonical mixer retained\n",
                native_requested ? "native MP2K output" : "MP2K shadow",
                r, ratio);
        }

        // The verifier has now judged the final canonical stereo behavior.
        // Nothing native is allowed to substitute before this point.
        const bool substitute = shadow_enabled_ && shadow_.live() &&
            (native_audio_enabled_ || turbo_audio_decoupled_);
        int16_t output_s = canonical_s;
        int16_t native_l = canonical_s;
        int16_t native_r = canonical_s;
        if (substitute) {
            auto to_i16 = [](float v) {
                int32_t x = static_cast<int32_t>(std::lround(v * 32768.0f));
                return static_cast<int16_t>(std::clamp(x, -32767, 32767));
            };
            native_l = to_i16(sl);
            native_r = to_i16(sr);
            const int16_t shadow_mono = static_cast<int16_t>(
                (static_cast<int32_t>(native_l) + native_r) / 2);
            // Native mode keeps the canonical mono ring available for an
            // immediate, synchronized fallback. Shadow-only mode retains the
            // existing verified mono substitution.
            if (!native_audio_enabled_) output_s = shadow_mono;
        }
        if (stereo_audio_requested()) {
            stereo_fallback_push(tap.canonical_left, tap.canonical_right);
        }
        // Do not queue speculative native samples. The host drains canonical
        // audio until the verifier proves the stream, then switches forward
        // with no replayed probation buffer.
        if (native_audio_enabled_ && substitute &&
            (turbo_audio_decoupled_ || !native_audio_requested_))
            native_ring_push(native_l, native_r);
        if (native_audio_enabled_ && shadow_.live() &&
            !native_live_reported_ && !native_audio_requested_) {
            clear_stereo_fallback();
            native_live_reported_ = true;
            std::fprintf(stderr,
                         "[audio] native MP2K stereo output LIVE (verified window passed)\n");
            log_native_timeline_summary("probation-passed");
        }
        if (native_audio_enabled_ && !native_live_reported_ &&
            !native_wall_candidate_active_ &&
            (shadow_cursor_ % 65536u) == 0u && shadow_cursor_ != 0u &&
            mp2k_status_diag_logs_ < 8u) {
            std::fprintf(stderr,
                         "[audio] native MP2K waiting: rate=%u hook_period=%.3f engaged=%s proven=%s hooks=%llu stale=%llu bad_waves=%llu\n",
                         sample_rate(), shadow_hook_period_,
                         shadow_.engaged() ? "yes" : "no",
                         shadow_.live() ? "yes" : "no",
                         static_cast<unsigned long long>(shadow_.hooks()),
                         static_cast<unsigned long long>(shadow_.stale_ticks()),
                         static_cast<unsigned long long>(shadow_.bad_waves()));
            log_native_probation_status(shadow_cursor_);
            ++mp2k_status_diag_logs_;
        }
        current_samples_[slot] = output_s;
        caps[slot].mixed = output_s;
        caps[slot].ch[0] = tap.ch[0];
        caps[slot].ch[1] = tap.ch[1];
        caps[slot].ch[2] = tap.ch[2];
        caps[slot].ch[3] = tap.ch[3];
        caps[slot].direct_a = tap.direct_a;
        caps[slot].direct_b = tap.direct_b;
        caps[slot].canonical_valid = tap.canonical_valid;
        caps[slot].sample_rate = tap.sample_rate;
        caps[slot].psg_left_input = tap.psg_left_input;
        caps[slot].psg_right_input = tap.psg_right_input;
        caps[slot].direct_route = tap.direct_route;
        ++sample_index_;
    }
    for (uint32_t i = 0; i < samples; ++i) {
        // cap_push first (keyed by samples_generated_ pre-increment), then
        // ring_push which bumps samples_generated_ — keeps the two aligned.
        cap_push(caps[i]);
        ring_push(current_samples_[i]);
    }
    sample_index_ = 0;

    int8_t last_a = fifo_a_.samples[samples - 1u];
    int8_t last_b = fifo_b_.samples[samples - 1u];
    std::fill(std::begin(fifo_a_.samples), std::end(fifo_a_.samples), last_a);
    std::fill(std::begin(fifo_b_.samples), std::end(fifo_b_.samples), last_b);
}

void GbaAudio::tick(uint32_t cycles) {
    cycle_accumulator_ += cycles;
    while (cycle_accumulator_ >= kSampleEventCycles) {
        cycle_accumulator_ -= kSampleEventCycles;
        run_sample_event();
    }
}

uint32_t GbaAudio::cycles_until_next_sample() const {
    return cycles_until_next_sample_event();
}

}  // namespace gba
