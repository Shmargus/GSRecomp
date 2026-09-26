// mp2k_shadow.cpp — see mp2k_shadow.h.
//
// Ported from JRickey/gba-recomp (crates/gba-core/src/mp2k.rs), © Jrickey,
// MIT OR Apache-2.0, used with permission. See THIRD_PARTY_ATTRIBUTION.md.

#include "mp2k_shadow.h"
#include "mp2k_wall_mixer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <utility>

namespace gba {
namespace {

// SDK gDeltaEncodingTable.
constexpr int8_t kDpcmLut[16] = {0, 1, 4, 9, 16, 25, 36, 49,
                                 -64, -49, -36, -25, -16, -9, -4, -1};

uint32_t load_u32le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

template <typename T>
void saturating_increment(T& value) {
    if (value != std::numeric_limits<T>::max()) ++value;
}

uint32_t mp2k_diag_hash_mix(uint32_t hash, uint32_t value) {
    hash ^= value;
    hash *= 16777619u;
    return hash;
}

uint32_t mp2k_diag_hash_word(uint32_t value) {
    return mp2k_diag_hash_mix(2166136261u, value);
}

uint32_t mp2k_diag_hash_words(const std::array<uint32_t, 4>& words) {
    uint32_t hash = 2166136261u;
    for (const uint32_t word : words) hash = mp2k_diag_hash_mix(hash, word);
    return hash;
}

// Camelot's MP2K PWM threshold calculation, translated from the fixed-point
// routine used by the Golden Sun synth implementation.
float synth_duty_threshold_impl(uint32_t value, uint8_t base, uint8_t depth,
                                uint8_t initial_duty) {
    uint32_t threshold =
        (static_cast<uint32_t>(initial_duty) << 24) + value;
    threshold = static_cast<int32_t>(threshold) < 0
        ? (~threshold >> 8) : (threshold >> 8);
    threshold = threshold * static_cast<uint32_t>(depth) +
        (static_cast<uint32_t>(base) << 24);
    return static_cast<float>(threshold) / 4294967296.0f;
}

// Camelot's channel state stores sound-archive offsets rather than full GBA
// ROM pointers. Resolve those against the cartridge window before considering
// a same-valued IWRAM workspace offset.
}  // namespace

float mp2k_synth_duty_threshold(uint32_t value, uint8_t base, uint8_t depth,
                                uint8_t initial_duty) {
    return synth_duty_threshold_impl(value, base, depth, initial_duty);
}

bool mp2k_cursor_index(uint32_t data, uint32_t size, uint32_t cp,
                       uint32_t& index) {
    if (cp < data || cp - data > size) return false;
    index = cp - data;
    return true;
}

uint32_t mp2k_step_q23(uint32_t source_rate, uint32_t render_rate) {
    if (source_rate == 0 || render_rate == 0) return 0;
    const uint64_t numerator = static_cast<uint64_t>(source_rate)
        << kMp2kFracBits;
    return static_cast<uint32_t>(numerator / render_rate);
}

int32_t mp2k_interpolate_s8(int32_t s0, int32_t s1, uint32_t frac) {
    const int64_t delta = static_cast<int64_t>(s1) - s0;
    // The guest uses an arithmetic ASR #23 on the signed product.
    return s0 + static_cast<int32_t>((delta * (frac & kMp2kFracMask)) >>
                                     kMp2kFracBits);
}

int32_t mp2k_interpolate_s8_x2(int32_t s0, int32_t s1, uint32_t frac) {
    const int64_t delta = static_cast<int64_t>(s1) - s0;
    // GS1 RAM mixer 0x03000920..28: MUL, ASR #22, then add s0 LSL #1.
    return s0 * 2 + static_cast<int32_t>(
        (delta * (frac & kMp2kFracMask)) >> (kMp2kFracBits - 1));
}

bool mp2k_check_hold_refresh(bool block_start, double mix_step,
                             double& accumulator) {
    // A PreMix snapshot replaces cp/fw with the authoritative beginning of a
    // new guest producer block. Its first native sample must therefore refresh
    // the held check value even if the previous block left a positive phase.
    if (block_start) accumulator = 0.0;
    accumulator -= 1.0;
    if (accumulator > 0.0) return false;
    accumulator += mix_step;
    return true;
}

uint32_t mp2k_gain_q9(uint32_t volume, uint32_t envelope) {
    return (volume * envelope) >> 9;
}

uint32_t mp2k_pack_stereo_gain(uint32_t right_q9, uint32_t left_q9) {
    uint32_t packed = (right_q9 & 0xFFFFu) | ((left_q9 & 0xFFFFu) << 16);
    const uint32_t carry = packed & 1u;
    packed = (packed >> 1) + 0x8000u + carry;
    return packed & ~0x0000FF00u;
}

uint32_t mp2k_pack_fix_gain(uint32_t right_q9, uint32_t left_q9) {
    // GS1 FIX path 0x03000A58..8C uses the unhalved packed Q9 gains and one
    // signed source byte per output sample. This is numerically equivalent to
    // the ordinary path's halved gain with a doubled sample, but preserves the
    // actual operands and avoids depending on interpolation state FIX ignores.
    return (right_q9 & 0xFFFFu) | ((left_q9 & 0xFFFFu) << 16);
}

uint32_t mp2k_packed_accumulate(uint32_t accumulator, uint32_t packed_gain,
                                int32_t sample) {
    return accumulator + packed_gain * static_cast<uint32_t>(sample);
}

bool mp2k_gs1_dry_route(uint32_t writer_pc, uint8_t& route) {
    // Proven GS1 RAM-mixer stores: B uses [r5,#0x630], then A uses [r5],#4.
    // Identifying the writers is stable across the circular destination
    // position, unlike splitting a guessed contiguous ring span in half.
    const uint32_t normalized = writer_pc & ~1u;
    if (normalized == 0x03000BD4u) {
        route = 0;
        return true;
    }
    if (normalized == 0x03000BCCu) {
        route = 1;
        return true;
    }
    return false;
}

uint32_t mp2k_reverb_word_address(uint32_t block_start,
                                  std::size_t group_index) {
    return block_start + static_cast<uint32_t>(group_index * 4u);
}

namespace {

uint32_t ror32(uint32_t value, unsigned amount) {
    amount &= 31u;
    return amount == 0 ? value : (value >> amount) | (value << (32u - amount));
}

bool double_overflows(uint32_t value, uint32_t doubled) {
    return ((value ^ doubled) & 0x80000000u) != 0;
}

int32_t asr32(uint32_t value, unsigned amount) {
    return static_cast<int32_t>(value) >> amount;
}

}  // namespace

uint32_t mp2k_saturate_packed_lanes(uint32_t accumulator) {
    uint32_t value = accumulator;
    uint32_t probe = ror32(value, 16);
    uint32_t doubled = probe + probe;
    if (double_overflows(probe, doubled)) {
        value = (value | 0x00004000u) & ~0x00003FC0u;
        if (static_cast<int32_t>(doubled) < 0) value -= 0x40u;
    }
    doubled = value + value;
    if (double_overflows(value, doubled)) {
        value = (value | 0x40000000u) & ~0x3FC00000u;
        if (static_cast<int32_t>(doubled) < 0) value -= 0x00400000u;
    }
    return value;
}

void mp2k_extract_dry_words(const std::array<uint32_t, 4>& accumulators,
                            uint32_t& route_a, uint32_t& route_b) {
    constexpr uint32_t lane_mask = 0xFF00FF00u;
    const uint32_t pair01 =
        (lane_mask & ror32(accumulators[1], 15)) |
        ((lane_mask & ror32(accumulators[0], 15)) >> 8);
    const uint32_t pair23 =
        (lane_mask & ror32(accumulators[3], 15)) |
        ((lane_mask & ror32(accumulators[2], 15)) >> 8);
    route_b = (pair01 & 0x0000FFFFu) | (pair23 << 16);
    route_a = (pair23 & 0xFFFF0000u) | (pair01 >> 16);
}

void mp2k_extract_dry_sample(uint32_t accumulator, int8_t& route_a,
                             int8_t& route_b) {
    route_a = static_cast<int8_t>(
        static_cast<int16_t>(accumulator & 0xFFFFu) >> 7);
    route_b = static_cast<int8_t>(
        static_cast<int16_t>(accumulator >> 16) >> 7);
}

std::array<uint32_t, 4> mp2k_finalize_producer_group(
    const std::array<uint32_t, 4>& accumulators,
    uint32_t old_route_a, uint32_t old_route_b) {
    std::array<uint32_t, 4> out{};
    auto mix_word = [&](std::size_t index) {
        uint32_t word = accumulators[index];
        int32_t high = asr32(word, 18) + asr32(old_route_a, 19);
        if (high < 0) high += 0x28;
        word = ror32(word, 16);
        int32_t low = asr32(word, 18) + asr32(old_route_b, 19);
        if (low < 0) low += 0x28;
        out[index] = static_cast<uint32_t>(low) +
                     (static_cast<uint32_t>(high) << 16);
    };
    mix_word(3);
    old_route_a = ror32(old_route_a, 16);
    old_route_b = ror32(old_route_b, 16);
    mix_word(1);
    old_route_a = ror32(old_route_a, 8);
    old_route_b = ror32(old_route_b, 8);
    mix_word(2);
    old_route_a = ror32(old_route_a, 16);
    old_route_b = ror32(old_route_b, 16);
    mix_word(0);
    return out;
}

void mp2k_sync_reverb_history(
    const std::unordered_map<uint32_t, uint32_t>& canonical,
    std::unordered_map<uint32_t, uint32_t>& native) {
    for (const auto& [address, value] : canonical) native[address] = value;
}

bool mp2k_advance_cursor(uint32_t& index, uint32_t& frac,
                         uint32_t step_q23, uint32_t size,
                         uint32_t loop_start, bool looped) {
    if (size == 0) return false;
    const uint64_t sum = static_cast<uint64_t>(frac & kMp2kFracMask) +
                         step_q23;
    index += static_cast<uint32_t>(sum >> kMp2kFracBits);
    frac = static_cast<uint32_t>(sum & kMp2kFracMask);
    if (index < size) return true;
    if (!looped || loop_start >= size) return false;
    const uint32_t span = size - loop_start;
    index = loop_start + ((index - loop_start) % span);
    return true;
}

namespace {

struct GuestCursorProjection {
    bool valid = false;
    bool exhausted = false;
    uint32_t channel = UINT32_MAX;
    uint32_t sample_cursor = 0;
    uint32_t resolved_data = 0;
    uint32_t start_index = 0;
    uint32_t start_frac = 0;
    uint32_t index = 0;
    uint32_t frac = 0;
    uint32_t step_q23 = 0;
    uint32_t size = 0;
    uint32_t loop_start = 0;
    uint32_t loop_wraps = 0;
};

GuestCursorProjection project_guest_cursor(
    const CanonicalMixTrace::VoiceIdentity& voice, uint32_t pcm_freq,
    uint32_t render_rate, std::size_t advances) {
    GuestCursorProjection out{};
    out.channel = voice.channel;
    out.sample_cursor = voice.sample_cursor;
    out.resolved_data = voice.resolved_data;
    out.start_frac = voice.frac & kMp2kFracMask;
    out.size = voice.wave_size;
    out.loop_start = voice.wave_loop;
    if (voice.resolved_data == 0 || voice.wave_size == 0 ||
        voice.sample_cursor < voice.resolved_data ||
        voice.sample_cursor - voice.resolved_data > voice.wave_size ||
        (voice.ctype & 0x20u) != 0) {
        return out;
    }
    out.start_index = voice.sample_cursor - voice.resolved_data;
    out.index = out.start_index;
    out.frac = out.start_frac;
    const uint32_t source_rate = (voice.ctype & 0x08u) != 0
        ? pcm_freq : voice.frequency;
    out.step_q23 = mp2k_step_q23(source_rate, render_rate);
    if (out.step_q23 == 0) return out;
    out.valid = true;
    for (std::size_t i = 0; i < advances; ++i) {
        const uint32_t previous = out.index;
        if (!mp2k_advance_cursor(out.index, out.frac, out.step_q23,
                                 out.size, out.loop_start,
                                 voice.looped != 0)) {
            out.exhausted = true;
            break;
        }
        if (out.index < previous) ++out.loop_wraps;
    }
    return out;
}

const CanonicalMixTrace::VoiceIdentity* find_guest_voice(
    const CanonicalMixTrace& snapshot, uint32_t channel) {
    for (const auto& voice : snapshot.voice_identities) {
        if (voice.channel == channel && voice.wave != 0) return &voice;
    }
    return nullptr;
}

}  // namespace

Mp2kReverbSample mp2k_gs1_reverb_mix(int32_t dry_right, int32_t dry_left,
                                      int32_t old_right, int32_t old_left) {
    // GS1's packed guest mixer uses signed arithmetic shifts for the
    // quarter-strength cross-feed. Keep this kernel integer so host FP mode
    // and saturation cannot change the feedback cadence.
    auto sat = [](int64_t v) {
        // Q15 representation of the guest signed-byte range [-128, 127].
        return static_cast<int32_t>(std::clamp<int64_t>(v, -32768, 32512));
    };
    return {sat(static_cast<int64_t>(dry_right) + (old_left >> 2)),
            sat(static_cast<int64_t>(dry_left) + (old_right >> 2))};
}

void mp2k_decode_producer_word(uint32_t packed, int8_t& route_a,
                               int8_t& route_b) {
    const int32_t lane_a = static_cast<int16_t>(packed & 0xFFFFu);
    const int32_t lane_b = static_cast<int16_t>(packed >> 16);
    route_a = static_cast<int8_t>(lane_a >> 8);
    route_b = static_cast<int8_t>(lane_b >> 8);
}

uint32_t mp2k_resolve_wave_address(const MemView& mem, uint32_t addr,
                                   std::size_t len) {
    if (mem.slice(addr, len)) return addr;
    auto plausible = [&](uint32_t candidate) {
        const uint8_t* h = mem.slice(candidate, std::max<std::size_t>(len, 16));
        if (!h) return false;
        const uint32_t loop = load_u32le(h + 8);
        const uint32_t size = load_u32le(h + 12);
        return (size == 0 && loop == 0) ||
               (size <= 0x01000000u && loop <= size);
    };
    if (addr < mem.rom_len &&
        mem.slice(0x08000000u | addr, len) &&
        plausible(0x08000000u | addr))
        return 0x08000000u | addr;
    if (addr < mem.iwram_len &&
        mem.slice(0x03000000u | addr, len) &&
        plausible(0x03000000u | addr))
        return 0x03000000u | addr;
    return 0;
}

bool mp2k_decode_synth_payload(const uint8_t* payload, std::size_t len,
                               Mp2kSynthParams& out) {
    if (!payload || len < 5) return false;
    switch (payload[0]) {
        case 0: out.kind = 1; break; // modulated pulse
        case 1: out.kind = 2; break; // sawtooth
        default: out.kind = 3; break; // triangle
    }
    out.base = payload[1];
    out.step = payload[2];
    out.depth = payload[3];
    out.initial_duty = payload[4];
    return true;
}

float mp2k_synth_render_sample(uint8_t kind, double& phase, double step,
                               uint32_t& synth_pos, float& duty,
                               float duty_step) {
    float sample = 0.0f;
    if (kind == 1) {
        const float bounded_duty = clampf(duty, -1.0f, 1.0f);
        sample = phase < bounded_duty ? 0.5f : -0.5f;
        sample += 0.5f - bounded_duty;
        duty += duty_step;
    }
    phase += step;
    if (phase >= 1.0)
        phase -= std::floor(phase);
    if (kind == 3) {
        return phase < 0.5
            ? static_cast<float>(4.0 * phase - 1.0)
            : static_cast<float>(3.0 - 4.0 * phase);
    }
    if (kind == 2) {
        constexpr uint32_t kFix = 0x70u;
        const uint32_t var1 = static_cast<uint32_t>(phase * 256.0) - kFix;
        const uint32_t var2 = static_cast<uint32_t>(phase * 65536.0) << 17;
        const uint32_t var3 = var1 - (var2 >> 27);
        synth_pos = var3 +
            static_cast<uint32_t>(static_cast<int32_t>(synth_pos) >> 1);
        return static_cast<float>(static_cast<int32_t>(synth_pos)) / 256.0f;
    }
    return sample;
}

bool mp2k_decode_dpcm_block(const uint8_t* block, std::size_t len,
                            std::array<int8_t, 64>& out) {
    if (!block || len < 33) return false;
    int8_t current = static_cast<int8_t>(block[0]);
    out[0] = current;
    for (uint32_t i = 1; i < out.size(); ++i) {
        const uint8_t packed = block[1u + (i >> 1)];
        const uint8_t nibble = (i & 1u) ? packed & 0x0Fu : packed >> 4;
        current = static_cast<int8_t>(current + kDpcmLut[nibble]);
        out[i] = current;
    }
    return true;
}

const uint8_t* MemView::slice(uint32_t addr, std::size_t len) const {
    const uint8_t* region = nullptr;
    std::size_t off = 0, rlen = 0;
    switch (addr >> 24) {
        case 0x02: region = ewram; off = addr & 0x3FFFF;     rlen = ewram_len; break;
        case 0x03: region = iwram; off = addr & 0x7FFF;      rlen = iwram_len; break;
        case 0x08: case 0x09: case 0x0A:
        case 0x0B: case 0x0C: case 0x0D:
            region = rom; off = addr & 0x01FFFFFF; rlen = rom_len; break;
        default: return nullptr;
    }
    if (!region || off > rlen || len > rlen - off) return nullptr;
    return region + off;
}

bool MemView::u8(uint32_t addr, uint8_t& out) const {
    const uint8_t* p = slice(addr, 1);
    if (!p) return false;
    out = p[0];
    return true;
}

bool MemView::u32(uint32_t addr, uint32_t& out) const {
    const uint8_t* p = slice(addr, 4);
    if (!p) return false;
    out = load_u32le(p);
    return true;
}

void Mp2kShadow::init(const std::vector<Mp2kSig>& sigs) {
    hook_n_ = 0;
    for (const auto& sig : sigs) {
        if (hook_n_ >= 4) break;
        uint32_t ram = sig.sound_main_ram;
        hook_keys_[hook_n_++] = (ram & 1) ? ram : (ram & ~3u);
    }
    reset_runtime();
}

void Mp2kShadow::reset_runtime() {
    active_ = hook_n_ > 0;
    engaged_ = false;
    // Probe configuration is sampled at the same reset boundary as the
    // shadow runtime. This preserves reset/savestate reconfiguration while
    // keeping getenv out of render/sample hot paths.
    audio_probe_enabled_ = std::getenv("GBARECOMP_AUDIO_PROBE") != nullptr;
    hooks_ = 0;
    stale_ticks_ = 0;
    bad_waves_ = 0;
    bad_wave_traces_ = {};
    bad_wave_trace_write_ = 0;
    bad_wave_trace_count_ = 0;
    authoritative_snapshots_ = 0;
    last_premix_id_ = UINT64_MAX;
    last_postmix_id_ = UINT64_MAX;
    voices_.fill(Voice{});
    producer_voice_traces_.fill(ProducerVoiceTrace{});
    env_pos_ = 0;
    env_span_ = static_cast<uint32_t>(kFrame);
    last_hook_cursor_ = 0;
    reverb_ = 0;
    dma_period_ = 7;
    std::fill(ring_.begin(), ring_.end(), std::pair<float, float>{0.0f, 0.0f});
    ring_pos_ = 0;
    reverb_ring_len_ = std::min<std::size_t>(ring_.size(), kFrame * dma_period_);
    reverb_write_count_ = 0;
    reverb_delay_q32_ = static_cast<uint64_t>(reverb_ring_len_) << 32;
    vf_.reset();
    mode_dwell_ = 0;
    debug_samples_ = 0;
    state_delay_frames_ = 0;
    if (const char* e = std::getenv("GBARECOMP_AUDIO_STATE_DELAY")) {
        state_delay_frames_ = static_cast<uint32_t>(std::strtoul(e, nullptr, 0));
    }
    pcm_freq_ = 13379;
    spv_ = 224;
    mix_step_ = static_cast<double>(render_rate_) / 13379.0;
    producer_block_id_ = UINT64_MAX;
    producer_samples_ = 0;
    producer_phase_ = 0.0;
    producer_block_first_render_ = true;
    producer_block_incomplete_reported_ = false;
    producer_block_start_cursor_ = 0;
    published_block_id_ = UINT64_MAX;
    canonical_fallback_ = false;
    startup_realign_pending_ = true;
    host_stream_.reset();
    dma_timeline_.reset();
    published_native_.clear();
    native_block_l_.clear();
    native_block_r_.clear();
    producer_a_.clear();
    producer_b_.clear();
    dry_guest_a_.clear();
    dry_guest_b_.clear();
    producer_raw_a_.clear();
    producer_raw_b_.clear();
    rolling_seed_words_.clear();
    current_seed_words_.clear();
    native_accumulator_words_.clear();
    for (auto& words : guest_reverb_old_words_) words.clear();
    for (auto& words : guest_reverb_new_words_) words.clear();
    reverb_block_start_ = {};
    for (auto& image : native_reverb_words_) image.clear();
    dry_guest_a_.clear();
    dry_guest_b_.clear();
    producer_raw_a_.clear();
    producer_raw_b_.clear();
    native_block_state_.clear();
    native_block_voice_traces_.clear();
    native_accumulator_words_.clear();
    for (auto& words : guest_reverb_old_words_) words.clear();
    for (auto& words : guest_reverb_new_words_) words.clear();
    reverb_block_start_ = {};
    previous_native_l_.clear();
    previous_native_r_.clear();
    previous_producer_a_.clear();
    previous_producer_b_.clear();
    dry_guest_a_.clear();
    dry_guest_b_.clear();
    producer_raw_a_.clear();
    producer_raw_b_.clear();
    previous_block_id_ = UINT64_MAX;
    producer_route_addr_[0] = producer_route_addr_[1] = 0;
    producer_diag_block_logged_ = false;
    producer_diag_diff_logged_ = false;
    producer_diag_mature_logged_ = false;
    producer_diag_mature_diff_logged_ = false;
    producer_sequence_gap_logged_ = false;
    producer_source_diag_logged_ = false;
    producer_source_diag_ = {};
    producer_post_probation_reject_diag_logged_ = false;
    producer_good_probation_window_seen_ = false;
    producer_voice_diag_ = {};
    dry_guest_block_id_ = {UINT64_MAX, UINT64_MAX};
    producer_startup_diag_count_ = 0;
    producer_verifier_diag_count_ = 0;
    producer_verifier_metric_block_id_ = UINT64_MAX;
    producer_reverb_reject_diag_logged_ = false;
    producer_reverb_branch_diag_logged_ = false;
    producer_context_active_block_id_ = UINT64_MAX;
    producer_context_pending_block_id_ = UINT64_MAX;
    producer_context_completed_routes_ = 0;
    producer_context_writer_count_ = 0;
    producer_context_writer_bytes_max_ = 0;
    producer_context_writer_completions_ = 0;
    producer_context_c70_writer_calls_ = 0;
    producer_context_c70_writer_completions_ = 0;
    sample_stats_ = {};
    producer_diff_trace_ = {};
    canonical_mix_trace_ = {};
    producer_guest_snapshot_ = {};
    producer_cursor_diag_ = {};
    producer_snapshot_hook_pc_ = 0;
    producer_snapshot_hook_source_ = 0;
    producer_snapshot_hook_phase_ = 0;
    producer_snapshot_channel_status_ = {};
    have_previous_verifier_sample_ = false;
    previous_canon_left_ = previous_canon_right_ = 0.0f;
    previous_native_left_ = previous_native_right_ = 0.0f;
    producer_blocks_judged_ = 0;
    producer_blocks_passed_ = 0;
    producer_blocks_rejected_ = 0;
    producer_blocks_incomplete_ = 0;
    candidate_samples_ready_ = 0;
    candidate_samples_missing_ = 0;
    verifier_samples_ = 0;
    verifier_windows_ = 0;
    output_domain_accum_ = {};
    first_output_domain_diag_ = {};
    first_output_domain_diag_consumed_ = false;
    first_output_domain_diag_ready_ = false;
    first_incomplete_block_ = UINT64_MAX;
    first_rejected_block_ = UINT64_MAX;
    first_incomplete_cursor_ = 0;
    first_incomplete_guest_samples_ = 0;
    first_incomplete_native_samples_ = 0;
    first_incomplete_expected_samples_ = 0;
    first_incomplete_alignment_ready_ = false;
    first_incomplete_route_a_ = first_incomplete_route_b_ = 0;
    first_rejected_correlation_ = 0.0f;
    first_rejected_level_ratio_ = 0.0f;
    first_rejected_mean_abs_error_ = 0.0f;
    first_rejected_cursor_ = 0;
    first_rejected_route_a_ = first_rejected_route_b_ = 0;
    first_rejected_reason_ = ProducerRejectReason::None;
    dma_timeline_.reset_epoch_diagnostics();
    producer_alignment_.reset();
    producer_reverb_history_aligned_ = false;
}

void Mp2kShadow::capture_canonical_mix_trace(
    const CanonicalMixTrace& trace) {
    if (!trace.valid) return;
    producer_guest_snapshot_ = trace;
    producer_guest_snapshot_.pre_cursor = producer_snapshot_cursor_;
    producer_guest_snapshot_.sound_info = producer_snapshot_sound_info_;
    producer_guest_snapshot_.pre_snapshot_generation =
        static_cast<uint32_t>(authoritative_snapshots_);
    producer_guest_snapshot_.pre_active_voices =
        producer_snapshot_active_voices_;
    producer_guest_snapshot_.pre_hook_pc = producer_snapshot_hook_pc_;
    producer_guest_snapshot_.pre_hook_source = producer_snapshot_hook_source_;
    producer_guest_snapshot_.pre_channel_status =
        producer_snapshot_channel_status_;
    if (canonical_mix_trace_.valid || first_rejected_block_ != trace.sequence)
        return;
    canonical_mix_trace_ = trace;
    canonical_mix_trace_.pre_cursor = producer_snapshot_cursor_;
    canonical_mix_trace_.sound_info = producer_snapshot_sound_info_;
    canonical_mix_trace_.pre_snapshot_generation =
        static_cast<uint32_t>(authoritative_snapshots_);
    canonical_mix_trace_.pre_active_voices = producer_snapshot_active_voices_;
    canonical_mix_trace_.pre_hook_pc = producer_snapshot_hook_pc_;
    canonical_mix_trace_.pre_hook_source = producer_snapshot_hook_source_;
    canonical_mix_trace_.pre_channel_status =
        producer_snapshot_channel_status_;
}

void Mp2kShadow::set_snapshot_hook_identity(uint32_t pc, uint8_t source) {
    producer_snapshot_hook_pc_ = pc;
    producer_snapshot_hook_source_ = source;
}

void Mp2kShadow::set_producer_hook_context(
    uint64_t active_block_id, uint64_t pending_block_id,
    uint8_t completed_routes, uint32_t writer_count,
    uint32_t writer_bytes_max, uint64_t writer_completions,
    uint64_t c70_writer_calls, uint64_t c70_writer_completions) {
    producer_context_active_block_id_ = active_block_id;
    producer_context_pending_block_id_ = pending_block_id;
    producer_context_completed_routes_ = completed_routes;
    producer_context_writer_count_ = writer_count;
    producer_context_writer_bytes_max_ = writer_bytes_max;
    producer_context_writer_completions_ = writer_completions;
    producer_context_c70_writer_calls_ = c70_writer_calls;
    producer_context_c70_writer_completions_ = c70_writer_completions;
}

void Mp2kShadow::annotate_bad_wave_writes(
    const std::array<CanonicalMixTrace::ChannelWrite, 64>& writes,
    uint32_t count) {
    const uint32_t retained = std::min<uint32_t>(
        count, static_cast<uint32_t>(writes.size()));
    for (uint32_t i = 0; i < bad_wave_trace_count_; ++i) {
        auto& trace = bad_wave_traces_[i];
        if (!trace.valid) continue;
        auto assign = [&](BadWaveTrace::Write& dst,
                          const CanonicalMixTrace::ChannelWrite& src) {
            dst.pc = src.pc;
            dst.cycle = src.cycle;
            dst.addr = src.addr;
            dst.before = src.before;
            dst.value = src.value;
            dst.width = src.width;
            dst.mode = src.mode;
        };
        for (uint32_t n = 0; n < retained; ++n) {
            const uint32_t pos = (count - retained + n) % writes.size();
            const auto& write = writes[pos];
            if (write.channel != trace.channel ||
                (trace.boundary_cycle != 0 &&
                 write.cycle > trace.boundary_cycle)) continue;
            if (write.field == 0) assign(trace.status_write, write);
            else if (write.field == 1) assign(trace.type_write, write);
            else if (write.field == 2) assign(trace.gain_right_write, write);
            else if (write.field == 3) assign(trace.gain_left_write, write);
            else if (write.field == 0x18) assign(trace.count_write, write);
            else if (write.field == 0x1C) assign(trace.frac_write, write);
            else if (write.field == 0x20) assign(trace.frequency_write, write);
            else if (write.field == 0x24) assign(trace.wave_write, write);
            else if (write.field == 0x28) assign(trace.cp_write, write);
        }
    }
}

bool Mp2kShadow::matches_hook(uint32_t key) const {
    // The generated entry prologue exposes the aligned PC while the ROM
    // signature records the callable pointer with its Thumb bit. The address
    // is the stable identity here; mode is still carried in `key` when the
    // hook is forwarded to frame_hook().
    const uint32_t normalized = key & ~1u;
    for (uint8_t i = 0; i < hook_n_; ++i) {
        if ((hook_keys_[i] & ~1u) == normalized) return true;
    }

    return false;
}

void Mp2kShadow::set_render_rate(uint32_t sample_rate) {
    render_rate_ = sample_rate ? sample_rate : 65536u;
}

void Mp2kShadow::canonical_fail(const char* reason) {
    if (canonical_fallback_) return;
    canonical_fallback_ = true;
    active_ = false;
    engaged_ = false;
    std::fprintf(stderr, "[audio] MP2K shadow canonical fallback: %s\n",
                 reason);
    // Keep the terminal ownership boundary explicit.  This is emitted only
    // on the first fail-closed transition, so it does not add work to the
    // render path and distinguishes a borrowed/deferred boot epoch from a
    // later verifier failure.
    std::fprintf(
        stderr,
        "[audio] MP2K canonical-fallback context: producer=%llu "
        "native=%zu/%zu guest=%zu/%zu expected=%u startup_pending=%u "
        "judged=%llu published=%llu snapshot_block=%llu "
        "snapshot_cursor=%llu hook_pc=0x%08x hook_source=%u hook_phase=%u "
        "active=%llu pending=%llu routes=0x%02x writers=%u "
        "writer_bytes_max=%u writer_completions=%llu c70_calls=%llu "
        "c70_completions=%llu\n",
        static_cast<unsigned long long>(producer_block_id_),
        native_block_l_.size(), native_block_r_.size(), producer_a_.size(),
        producer_b_.size(), producer_samples_, startup_realign_pending_ ? 1u : 0u,
        static_cast<unsigned long long>(producer_blocks_judged_),
        static_cast<unsigned long long>(published_block_id_),
        static_cast<unsigned long long>(producer_snapshot_block_),
        static_cast<unsigned long long>(producer_snapshot_cursor_),
        producer_snapshot_hook_pc_,
        static_cast<unsigned>(producer_snapshot_hook_source_),
        static_cast<unsigned>(producer_snapshot_hook_phase_),
        static_cast<unsigned long long>(producer_context_active_block_id_),
        static_cast<unsigned long long>(producer_context_pending_block_id_),
        static_cast<unsigned>(producer_context_completed_routes_),
        producer_context_writer_count_, producer_context_writer_bytes_max_,
        static_cast<unsigned long long>(producer_context_writer_completions_),
        static_cast<unsigned long long>(producer_context_c70_writer_calls_),
        static_cast<unsigned long long>(producer_context_c70_writer_completions_));
}

void Mp2kShadow::frame_hook(const MemView& mem, uint64_t audio_cursor,
                            uint32_t key, HookPhase phase,
                            uint64_t block_id, uint64_t boundary_cycle) {
    if (!active_ || hook_n_ == 0) return;
    producer_snapshot_hook_phase_ = static_cast<uint8_t>(phase);
    if (block_id == UINT64_MAX) block_id = audio_cursor;
    if (phase == HookPhase::PostMix) {
        // PostMix observes the completed guest block.  It must never advance
        // voice state from the already-finalized fields a second time.
        if (block_id != last_postmix_id_) {
            last_postmix_id_ = block_id;
            ++hooks_;
        }
        return;
    }
    if (block_id == last_premix_id_) return;

    if (producer_block_id_ != UINT64_MAX &&
        block_id < producer_block_id_) {
        canonical_fail("out-of-order producer hook");
        return;
    }
    if (producer_block_id_ != UINT64_MAX &&
        block_id > producer_block_id_ &&
        block_id - producer_block_id_ != 1) {
        if (!producer_sequence_gap_logged_) {
            std::fprintf(
                stderr,
                "[audio] MP2K producer sequence gap: "
                "previous=%llu incoming=%llu delta=%llu phase=%s "
                "key=0x%08x cursor=%llu boundary=%llu "
                "snapshot_pc=0x%08x snapshot_source=%u "
                "startup_pending=%u judged=%llu published=%llu "
                "native=%zu guest=%zu expected=%u\n",
                static_cast<unsigned long long>(producer_block_id_),
                static_cast<unsigned long long>(block_id),
                static_cast<unsigned long long>(block_id - producer_block_id_),
                phase == HookPhase::PreMix ? "premix" : "postmix",
                key,
                static_cast<unsigned long long>(audio_cursor),
                static_cast<unsigned long long>(boundary_cycle),
                producer_snapshot_hook_pc_,
                static_cast<unsigned>(producer_snapshot_hook_source_),
                startup_realign_pending_ ? 1u : 0u,
                static_cast<unsigned long long>(producer_blocks_judged_),
                static_cast<unsigned long long>(published_block_id_),
                native_block_l_.size(), producer_a_.size(), producer_samples_);
            producer_sequence_gap_logged_ = true;
        }
        canonical_fail("producer sequence gap");
        return;
    }

    // A deferred hook would need an owned copy of arbitrary voice assets and
    // the evolving reverb ring. Keep the canonical guest path authoritative
    // rather than applying a borrowed/stale future snapshot after the tail.
    const bool old_native_block_incomplete =
        native_block_l_.size() < producer_samples_ ||
        native_block_r_.size() < producer_samples_;
    if (producer_block_id_ != UINT64_MAX && block_id > producer_block_id_ &&
        old_native_block_incomplete) {
        // The first callback after startup/savestate restore may observe the
        // tail of an epoch that began before shadow state was owned. It is
        // safe to discard that prefix before any producer block has been
        // judged or published; established epochs must still fail closed.
        const bool pre_verification_epoch = startup_realign_pending_ &&
            producer_blocks_judged_ == 0 &&
            published_block_id_ == UINT64_MAX;
        if (!pre_verification_epoch) {
            canonical_fail("deferred hook needs owned state");
            return;
        }
        if (producer_startup_diag_count_ < 8u) {
            const uint64_t delta = block_id - producer_block_id_;
            std::fprintf(
                stderr,
                "[audio] MP2K startup realign context: previous=%llu "
                "incoming=%llu delta=%llu phase=%s key=0x%08x source=%u "
                "guest=%zu/%zu native=%zu/%zu expected=%u active=%llu "
                "pending=%llu routes=0x%02x writers=%u writer_bytes_max=%u "
                "writer_expected_bytes=%u writer_completions=%llu "
                "c70_calls=%llu c70_completions=%llu "
                "cursor=%llu boundary=%llu\n",
                static_cast<unsigned long long>(producer_block_id_),
                static_cast<unsigned long long>(block_id),
                static_cast<unsigned long long>(delta),
                phase == HookPhase::PreMix ? "premix" : "postmix", key,
                static_cast<unsigned>(producer_snapshot_hook_source_),
                producer_a_.size(), producer_b_.size(), native_block_l_.size(),
                native_block_r_.size(), producer_samples_,
                static_cast<unsigned long long>(
                    producer_context_active_block_id_),
                static_cast<unsigned long long>(
                    producer_context_pending_block_id_),
                static_cast<unsigned>(producer_context_completed_routes_),
                producer_context_writer_count_,
                producer_context_writer_bytes_max_, producer_samples_ * 4u,
                static_cast<unsigned long long>(
                    producer_context_writer_completions_),
                static_cast<unsigned long long>(
                    producer_context_c70_writer_calls_),
                static_cast<unsigned long long>(
                    producer_context_c70_writer_completions_),
                static_cast<unsigned long long>(audio_cursor),
                static_cast<unsigned long long>(boundary_cycle));
            ++producer_startup_diag_count_;
        }
        std::fprintf(stderr,
                     "[audio] MP2K shadow startup realign: discarded "
                     "incomplete producer epoch seq=%llu native=%zu/%u\n",
                     static_cast<unsigned long long>(producer_block_id_),
                     native_block_l_.size(), producer_samples_);
        const uint32_t realign_diag_count = producer_startup_diag_count_;
        const uint32_t verifier_diag_count = producer_verifier_diag_count_;
        const uint8_t realign_hook_phase = producer_snapshot_hook_phase_;
        auto rolling_seed = std::move(rolling_seed_words_);
        reset_runtime();
        // Keep the bounded realign count across this internal epoch reset; an
        // external reset_runtime() starts a fresh probe.
        producer_startup_diag_count_ = realign_diag_count;
        producer_verifier_diag_count_ = verifier_diag_count;
        producer_snapshot_hook_phase_ = realign_hook_phase;
        rolling_seed_words_ = std::move(rolling_seed);
        current_seed_words_ = rolling_seed_words_;
        // Keep the new epoch unowned until a complete producer block is
        // judged.  The next guest boundary can still arrive one native sample
        // early (the measured 351/352 cadence), so clearing this here turns a
        // normal fractional tail into "deferred hook needs owned state".
    }
    last_premix_id_ = block_id;
    ++authoritative_snapshots_;
    ++hooks_;
    // Do not rewrite the detected hook keys here.  Camelot's PCM writer calls
    // this function with its mixer-store PC, which is not SoundMainRAM's
    // entry point; replacing the keys would make later real entry callbacks
    // disappear from the native timeline.

    uint64_t gap = audio_cursor - last_hook_cursor_;
    if (gap >= 64) {
        env_span_ = static_cast<uint32_t>(std::min<uint64_t>(std::max<uint64_t>(gap, 256), 8192));
        env_pos_ = 0;
        last_hook_cursor_ = audio_cursor;
    }

    uint32_t si = 0;
    if (!mem.u32(kSoundInfoPtr, si) || !(si >> 24 == 2 || si >> 24 == 3)) {
        if (stale_ticks_ == 0)
            std::fprintf(stderr, "[audio] MP2K shadow stale: SoundInfo pointer 0x%08x\n", si);
        ++stale_ticks_; return;
    }
    uint32_t ident = 0;
    // SoundMain briefly marks the structure "live" during its tick, but the
    // finalized channel state is also valid in the steady "base" phase. The
    // old gate sampled only the transient +1 marker and left native audio
    // permanently disengaged between ticks.
    // The stock driver uses Smsh..Smsh+1 while Camelot's copy briefly reaches
    // Smsh+2 during its two-bus mixer pass. mGBA's MP2K lock accepts the same
    // bounded range; rejecting it makes every valid world-map tick stale.
    if (!mem.u32(si, ident) || ident < kMp2kMagicBase ||
        ident - kMp2kMagicBase > 8u) {
        if (stale_ticks_ == 0)
            std::fprintf(stderr, "[audio] MP2K shadow stale: ident at 0x%08x = 0x%08x\n", si, ident);
        ++stale_ticks_; return;
    }
    const uint8_t* head = mem.slice(si, 0x50);
    if (!head) { if (stale_ticks_ == 0) std::fprintf(stderr, "[audio] MP2K shadow stale: SoundInfo slice\n"); ++stale_ticks_; return; }

    reverb_ = head[0x05] & 0x7F;
    int max_chans = std::min<int>(std::max<int>(head[0x06], 1), kMp2kMaxChans);
    uint32_t pcm_freq = load_u32le(head + 0x14);
    uint32_t spv = load_u32le(head + 0x10);
    if (spv == 0 || pcm_freq == 0) { if (stale_ticks_ == 0) std::fprintf(stderr, "[audio] MP2K shadow stale: spv=%u pcm=%u\n", spv, pcm_freq); ++stale_ticks_; return; }
    const uint8_t new_dma_period = static_cast<uint8_t>(
        std::min<int>(std::max<int>(head[0x0B], 1), 16));
    if (new_dma_period != dma_period_) {
        // The GS1 delay is spv * DMA-period source samples converted to the
        // host grid.  Keep the Q32 remainder; a rounded 1097 samples/frame
        // drifts against the guest timeline.
        dma_period_ = new_dma_period;
        const uint64_t delay_q32 =
            (static_cast<uint64_t>(spv) * dma_period_ * render_rate_ << 32) /
            std::max<uint32_t>(pcm_freq, 1u);
        reverb_delay_q32_ = delay_q32;
        const std::size_t delay_samples = static_cast<std::size_t>(
            (delay_q32 + 0xFFFFFFFFull) >> 32);
        reverb_ring_len_ = std::clamp<std::size_t>(
            delay_samples, 1, ring_.size());
        ring_pos_ = 0;
        reverb_write_count_ = 0;
        std::fill(ring_.begin(), ring_.end(), std::pair<float, float>{0.0f, 0.0f});
    }
    mix_step_ = static_cast<double>(render_rate_) /
                static_cast<double>(pcm_freq);
    pcm_freq_ = pcm_freq;
    spv_ = spv;
    const uint64_t delay_q32 =
        (static_cast<uint64_t>(spv_) * dma_period_ * render_rate_ << 32) /
        std::max<uint32_t>(pcm_freq_, 1u);
    if (delay_q32 != reverb_delay_q32_) {
        reverb_delay_q32_ = delay_q32;
        const std::size_t delay_samples = static_cast<std::size_t>(
            (delay_q32 + 0xFFFFFFFFull) >> 32);
        reverb_ring_len_ = std::clamp<std::size_t>(
            delay_samples, 1, ring_.size());
        ring_pos_ = 0;
        reverb_write_count_ = 0;
        std::fill(ring_.begin(), ring_.end(), std::pair<float, float>{0.0f, 0.0f});
    }
    engaged_ = true;
    reset_producer_block(block_id, audio_cursor);

    auto record_bad_wave = [&](uint32_t channel, uint8_t reason,
                               uint8_t status, uint8_t ctype,
                               uint32_t channel_base, uint32_t wav,
                               uint32_t count, uint32_t frac, uint32_t cp) {
        BadWaveTrace trace{};
        trace.valid = true;
        trace.sequence = block_id;
        trace.boundary_cycle = boundary_cycle;
        trace.channel = static_cast<uint16_t>(channel);
        trace.reason = reason;
        trace.status = status;
        trace.ctype = ctype;
        trace.channel_base = channel_base;
        trace.wave = wav;
        trace.resolved_wave = mp2k_resolve_wave_address(mem, wav, 16);
        trace.resolved_data = trace.resolved_wave
            ? trace.resolved_wave + 16u : 0u;
        trace.count = count;
        trace.frac = frac;
        trace.cp = cp;
        trace.sample_cursor = cp;
        if (const uint8_t* h = trace.resolved_wave
                ? mem.slice(trace.resolved_wave, 16) : nullptr) {
            trace.wave_flags = static_cast<uint32_t>(h[2]) |
                (static_cast<uint32_t>(h[3]) << 8);
            trace.wave_loop = load_u32le(h + 8);
            trace.wave_size = load_u32le(h + 12);
            trace.data_base = trace.resolved_data;
            trace.data_size = trace.wave_size;
            const std::size_t bytes = (ctype & 0x20u)
                ? static_cast<std::size_t>((
                    static_cast<uint64_t>(trace.wave_size) * 33 + 63) / 64)
                : static_cast<std::size_t>(trace.wave_size);
            trace.cp_in_range = mem.slice(trace.resolved_data, bytes) &&
                cp >= trace.resolved_data &&
                cp - trace.resolved_data <= trace.wave_size;
        }
        bad_wave_traces_[bad_wave_trace_write_] = trace;
        bad_wave_trace_write_ = (bad_wave_trace_write_ + 1u) %
            static_cast<uint32_t>(bad_wave_traces_.size());
        bad_wave_trace_count_ = std::min<uint32_t>(
            bad_wave_trace_count_ + 1u,
            static_cast<uint32_t>(bad_wave_traces_.size()));
    };

    for (int ch = 0; ch < kMp2kMaxChans; ++ch) {
        Voice& v = voices_[ch];
        if (ch >= max_chans) { v.on = false; continue; }
        const uint32_t base = si + kSoundChansOff + ch * kSoundChanStride;
        const uint8_t* c = mem.slice(base, 0x28);
        if (!c) { v.on = false; continue; }
        uint8_t status = c[0x00];
        uint8_t ctype = c[0x01];
        if ((status & 0xC7u) == 0 || (ctype & 0x07u) != 0) { v.on = false; continue; }
        if (ctype & 0x10u) {
            v.on = false; ++bad_waves_;
            record_bad_wave(ch, 1, status, ctype, base,
                            load_u32le(c + 0x24), load_u32le(c + 0x18),
                            load_u32le(c + 0x1C), 0);
            continue;
        }  // reversed: not modeled

        uint32_t vol_r = c[0x02], vol_l = c[0x03];
        uint32_t attack = c[0x04], decay = c[0x05], sustain = c[0x06], release = c[0x07];
        uint32_t env = c[0x09], echo_vol = c[0x0C];
        uint8_t  echo_len = c[0x0D];
        uint32_t count = load_u32le(c + 0x18);
        uint32_t freq = load_u32le(c + 0x20);
        uint32_t wav = load_u32le(c + 0x24);
        uint32_t cp = 0;
        const bool cp_valid = mem.u32(base + 0x28u, cp);
        const uint32_t fw = load_u32le(c + 0x1Cu) & kMp2kFracMask;

        bool dead = false;
        bool freshly_initialized = false;
        uint32_t env_now = 0;
        uint32_t phase = status & 0x03u;
        bool iec = (status & 0x04u) != 0;
        bool stopping = (status & 0x40u) != 0;
        // A savestate (and the first hook after boot) can expose a voice that
        // is already playing without the transient START bit.  The old
        // shadow only populated its sample cursor in the START branch, so
        // these voices stayed on with size==0 and rendered silence.
        const bool needs_seed = !v.on;
        // The Camelot driver can recycle a channel for a new instrument
        // between shadow hooks without exposing START at the next VBlank.
        // Treat a live wave/type change as a new voice and bind to its
        // current sample cursor, otherwise the old instrument continues
        // playing through the new note.
        const bool source_changed = v.on &&
            (v.wav != wav || v.ctype != ctype);
        if (status & 0x80u) {  // note-on (START)
            if (stopping) {
                dead = true; env_now = 0;
            } else if (note_on(v, mem, ctype, count, wav)) {
                freshly_initialized = true;
                env_now = std::min<uint32_t>(attack, 0xFF);
                phase = 3;
                if (env_now >= 0xFF) phase = 2;
                stopping = false; iec = false;
            } else {
                ++bad_waves_; dead = true; env_now = 0;
                record_bad_wave(ch, 2, status, ctype, base, wav, count, fw, cp);
            }
        } else if (needs_seed || source_changed) {
            // `cp` is the live sample pointer for an already-running voice.
            // `count` is the driver's internal counter, not a PCM index.
            const bool seeded = note_on(v, mem, ctype, count, wav);
            if (seeded) {
                freshly_initialized = true;
                env_now = env;
            } else {
                ++bad_waves_; dead = true; env_now = 0;
                record_bad_wave(ch, 3, status, ctype, base, wav, count, fw, cp);
            }
        } else if (iec) {
            env_now = env;
            if (echo_len <= 1) dead = true;
        } else if (stopping) {
            env_now = (env * release) >> 8;
            if (env_now <= echo_vol) {
                if (echo_vol == 0) dead = true;
                else { iec = true; env_now = echo_vol; }
            }
        } else {
            env_now = env;
            if (phase == 3) {
                env_now = env + attack;
                if (env_now >= 0xFF) { env_now = 0xFF; phase = 2; }
            } else if (phase == 2) {
                env_now = (env * decay) >> 8;
                if (env_now <= sustain) {
                    env_now = sustain;
                    if (sustain == 0) {
                        if (echo_vol == 0) dead = true;
                        else { iec = true; env_now = echo_vol; }
                    }
                    phase = 1;
                }
            }
        }
        // Fresh notes have a valid phase from note_on; the guest publishes
        // their live cp later in the canonical mixer pass. Do not reject that
        // first render on the previous voice's cp. Mature voices retain the
        // original fail-closed range check.
        if (!dead && mp2k_should_validate_live_cursor(
                freshly_initialized, v.synth_kind) && cp_valid) {
            uint32_t cp_addr = cp;
            if (!mem.slice(cp_addr, 1) && cp < mem.iwram_len)
                cp_addr = 0x03000000u | cp;
            uint32_t cursor = 0;
            if (!mp2k_cursor_index(v.data, v.size, cp_addr, cursor)) {
                ++bad_waves_; dead = true; env_now = 0;
                record_bad_wave(ch, 4, status, ctype, base, wav, count, fw, cp);
            } else {
                // The guest carries the 23-bit fw remainder across every
                // sample.  Dropping it here creates a phase discontinuity at
                // each hook and was the main source of garbled native audio.
                v.pos_index = cursor;
                v.pos_frac = fw;
                v.pos = static_cast<double>(cursor) +
                        static_cast<double>(fw) / kMp2kFracOne;
            }
        }
        if (dead) { v.on = false; continue; }

        if (!dead && !v.synth_kind && state_delay_frames_ != 0 &&
            (status & 0x80u || needs_seed || source_changed)) {
            const double source_per_frame = (ctype & 0x08u)
                ? static_cast<double>(pcm_freq_) / 60.0
                : static_cast<double>(freq) / 60.0;
            const uint64_t rewind_q = static_cast<uint64_t>(std::max(
                0.0, source_per_frame * state_delay_frames_ * kMp2kFracOne));
            uint64_t pos_q = (static_cast<uint64_t>(v.pos_index)
                              << kMp2kFracBits) | v.pos_frac;
            if (v.looped && v.size > v.loop_start) {
                const uint64_t span_q = static_cast<uint64_t>(v.size -
                    v.loop_start) << kMp2kFracBits;
                const uint64_t loop_q = static_cast<uint64_t>(v.loop_start)
                    << kMp2kFracBits;
                if (pos_q >= loop_q) {
                    const uint64_t rel = (pos_q - loop_q + span_q -
                        (rewind_q % span_q)) % span_q;
                    pos_q = loop_q + rel;
                }
            } else {
                pos_q = pos_q > rewind_q ? pos_q - rewind_q : 0;
            }
            v.pos_index = static_cast<uint32_t>(pos_q >> kMp2kFracBits);
            v.pos_frac = static_cast<uint32_t>(pos_q & kMp2kFracMask);
            v.pos = static_cast<double>(v.pos_index) +
                    static_cast<double>(v.pos_frac) / kMp2kFracOne;
        }

        uint32_t env_next;
        if (iec) {
            env_next = env_now;
        } else if (stopping) {
            uint32_t e = (env_now * release) >> 8;
            env_next = e <= echo_vol ? echo_vol : e;
        } else if (phase == 3) {
            env_next = std::min<uint32_t>(env_now + attack, 0xFF);
        } else if (phase == 2) {
            env_next = std::max<uint32_t>((env_now * decay) >> 8, sustain);
        } else {
            env_next = env_now;
        }

        // Guest order is (volume * envelope) >> 9, followed by the
        // packed-lane mixer and final /4 scaling. Cache the exact integer
        // gains and packed type flags at the frame boundary; render() then
        // avoids repeating lround/ctype work for every producer sample.
        v.g0r_q9 = mp2k_gain_q9(vol_r, env_now);
        v.g0l_q9 = mp2k_gain_q9(vol_l, env_now);
        v.g1r_q9 = mp2k_gain_q9(vol_r, env_next);
        v.g1l_q9 = mp2k_gain_q9(vol_l, env_next);
        v.g0r = static_cast<float>(v.g0r_q9) * 0.25f;
        v.g0l = static_cast<float>(v.g0l_q9) * 0.25f;
        v.g1r = static_cast<float>(v.g1r_q9) * 0.25f;
        v.g1l = static_cast<float>(v.g1l_q9) * 0.25f;
        v.packed_gain_fix = v.synth_kind == 0 && !v.compressed &&
            !v.reversed && (ctype & 0x08u) != 0;
        v.packed_gain_ordinary_pcm = v.synth_kind == 0 &&
            !v.compressed && !v.reversed && (ctype & 0x08u) == 0;
        v.packed_gain_g0 = v.packed_gain_fix
            ? mp2k_pack_fix_gain(v.g0r_q9, v.g0l_q9)
            : mp2k_pack_stereo_gain(v.g0r_q9, v.g0l_q9);

        // Golden Sun's Camelot-compatible driver stores this live field as
        // the source-rate value used by its mixer. FIX voices instead use the
        // track's fixed PCM rate, matching the driver variant's observed
        // 0x08 behavior. Camelot synths use an additional /64 phase scale.
        if (v.synth_kind) {
            v.step = static_cast<double>(freq) /
                     static_cast<double>(render_rate_) / 64.0;
            if (v.synth_kind == 1) {
                // Golden Sun's PWM duty accumulator advances once per GBA
                // frame, while its threshold is interpolated across that
                // frame.  This is the same four-subframe algorithm used by
                // the Camelot MP2K driver, expressed at the native rate.
                const uint32_t duty_step =
                    static_cast<uint32_t>(v.synth_step) << 24;
                v.synth_duty_pos += duty_step;
                const float from = mp2k_synth_duty_threshold(
                    v.synth_duty_pos, v.synth_base, v.synth_depth,
                    v.synth_init_duty);
                const float to = mp2k_synth_duty_threshold(
                    v.synth_duty_pos + duty_step, v.synth_base,
                    v.synth_depth, v.synth_init_duty);
                v.synth_duty = from;
                v.synth_duty_step = (to - from) /
                    static_cast<float>(std::max<uint32_t>(env_span_, 1));
            }
        } else if (ctype & 0x08u) {
            v.step = static_cast<double>(pcm_freq) /
                     static_cast<double>(render_rate_);
        } else {
            v.step = static_cast<double>(freq) /
                     static_cast<double>(render_rate_);
        }
        if (!v.synth_kind) {
            const uint32_t source_rate = (ctype & 0x08u) ? pcm_freq : freq;
            v.step_q23 = mp2k_step_q23(source_rate, render_rate_);
        }
        v.on = true;
    }
    producer_snapshot_cursor_ = audio_cursor;
    producer_snapshot_block_ = block_id;
    producer_snapshot_sound_info_ = si;
    producer_snapshot_active_voices_ = 0;
    for (const Voice& voice : voices_)
        if (voice.on) ++producer_snapshot_active_voices_;
    for (uint32_t ch = 0; ch < producer_snapshot_channel_status_.size(); ++ch) {
        const uint32_t base = si + kSoundChansOff + ch * kSoundChanStride;
        uint8_t status = 0, ctype = 0;
        mem.u8(base, status);
        mem.u8(base + 1u, ctype);
        producer_snapshot_channel_status_[ch] =
            static_cast<uint16_t>(status) | (static_cast<uint16_t>(ctype) << 8);
    }
}

void Mp2kShadow::reset_producer_block(uint64_t block_id,
                                      uint64_t audio_cursor) {
    if (audio_probe_enabled_ && producer_block_id_ != UINT64_MAX) {
        previous_block_id_ = producer_block_id_;
        previous_native_l_ = native_block_l_;
        previous_native_r_ = native_block_r_;
        previous_producer_a_ = producer_a_;
        previous_producer_b_ = producer_b_;
    }
    producer_block_id_ = block_id;
    producer_block_start_cursor_ = audio_cursor;
    producer_samples_ = spv_;
    current_seed_words_ = rolling_seed_words_;
    producer_phase_ = 0.0;
    producer_block_first_render_ = true;
    producer_block_incomplete_reported_ = false;
    native_block_l_.clear();
    native_block_r_.clear();
    producer_a_.clear();
    producer_b_.clear();
    native_block_state_.clear();
    native_block_voice_traces_.clear();
    native_accumulator_words_.clear();
    for (auto& words : guest_reverb_old_words_) words.clear();
    for (auto& words : guest_reverb_new_words_) words.clear();
    reverb_block_start_ = {};
    dry_guest_block_id_ = {UINT64_MAX, UINT64_MAX};
    native_block_l_.reserve(producer_samples_);
    native_block_r_.reserve(producer_samples_);
    producer_a_.reserve(producer_samples_);
    producer_b_.reserve(producer_samples_);
    dry_guest_a_.reserve(producer_samples_);
    dry_guest_b_.reserve(producer_samples_);
    producer_raw_a_.reserve(producer_samples_);
    producer_raw_b_.reserve(producer_samples_);
    native_block_state_.reserve(producer_samples_);
    if (capture_voice_traces_enabled())
        native_block_voice_traces_.reserve(producer_samples_);
    native_accumulator_words_.reserve(producer_samples_);
    producer_route_addr_[0] = producer_route_addr_[1] = 0;
}

void Mp2kShadow::log_producer_verifier_diag(
    const char* stage, uint64_t block_id, std::size_t guest_a,
    std::size_t guest_b, std::size_t native_l, std::size_t native_r,
    bool alignment_ready, bool evaluated, bool pass, float correlation,
    float level_ratio) {
    if (producer_verifier_diag_count_ >= 8u) return;
    std::fprintf(
        stderr,
        "[audio] MP2K verifier diagnostic: stage=%s block=%llu "
        "producer=%llu previous=%llu active=%llu pending=%llu "
        "guest=%zu/%zu native=%zu/%zu expected=%u alignment=%u "
        "evaluated=%u pass=%u correlation=%.3f ratio=%.3f "
        "reverb=%u history=%u route_addr=0x%08x/0x%08x "
        "hook_pc=0x%08x hook_source=%u hook_phase=%u snapshot_block=%llu "
        "snapshot_cursor=%llu context_routes=0x%02x writers=%u "
        "writer_bytes_max=%u writer_completions=%llu c70_calls=%llu "
        "c70_completions=%llu block_cursor=%llu\n",
        stage ? stage : "unknown",
        static_cast<unsigned long long>(block_id),
        static_cast<unsigned long long>(producer_block_id_),
        static_cast<unsigned long long>(previous_block_id_),
        static_cast<unsigned long long>(producer_context_active_block_id_),
        static_cast<unsigned long long>(producer_context_pending_block_id_),
        guest_a, guest_b, native_l, native_r, producer_samples_,
        alignment_ready ? 1u : 0u, evaluated ? 1u : 0u, pass ? 1u : 0u,
        static_cast<double>(correlation), static_cast<double>(level_ratio),
        static_cast<unsigned>(reverb_),
        producer_reverb_history_aligned_ ? 1u : 0u,
        producer_route_addr_[0], producer_route_addr_[1],
        producer_snapshot_hook_pc_,
        static_cast<unsigned>(producer_snapshot_hook_source_),
        static_cast<unsigned>(producer_snapshot_hook_phase_),
        static_cast<unsigned long long>(producer_snapshot_block_),
        static_cast<unsigned long long>(producer_snapshot_cursor_),
        static_cast<unsigned>(producer_context_completed_routes_),
        producer_context_writer_count_, producer_context_writer_bytes_max_,
        static_cast<unsigned long long>(producer_context_writer_completions_),
        static_cast<unsigned long long>(producer_context_c70_writer_calls_),
        static_cast<unsigned long long>(
            producer_context_c70_writer_completions_),
        static_cast<unsigned long long>(producer_block_start_cursor_));
    ++producer_verifier_diag_count_;
}

void Mp2kShadow::log_reverb_reject_diagnostic(
    uint64_t block_id, std::size_t n, const SignedStereoMetrics& metrics,
    const std::array<ReverbRejectGroupDiag, 2>& groups,
    uint32_t group_count) {
    const bool dma_stage = dry_guest_a_.size() >= n &&
        dry_guest_b_.size() >= n &&
        dry_guest_block_id_[0] == block_id &&
        dry_guest_block_id_[1] == block_id &&
        native_block_state_.size() >= n;
    uint32_t first_diff_index = UINT32_MAX;
    uint32_t first_diff_route = 0;
    uint32_t first_diff_abs_delta = 0;
    uint32_t first_diff_same_sign = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const int32_t guest_a = dma_stage ? dry_guest_a_[i] : producer_a_[i];
        const int32_t native_a = dma_stage
            ? native_block_state_[i].dry_right_q15 / 256
            : static_cast<int32_t>(std::lround(
                std::clamp(native_block_r_[i], -1.0f, 127.0f / 128.0f) *
                128.0f));
        if (guest_a != native_a) {
            first_diff_index = static_cast<uint32_t>(i);
            first_diff_abs_delta = static_cast<uint32_t>(
                guest_a > native_a ? guest_a - native_a
                                    : native_a - guest_a);
            first_diff_same_sign = (guest_a < 0) == (native_a < 0)
                ? 1u : 0u;
            break;
        }
        const int32_t guest_b = dma_stage ? dry_guest_b_[i] : producer_b_[i];
        const int32_t native_b = dma_stage
            ? native_block_state_[i].dry_left_q15 / 256
            : static_cast<int32_t>(std::lround(
                std::clamp(native_block_l_[i], -1.0f, 127.0f / 128.0f) *
                128.0f));
        if (guest_b != native_b) {
            first_diff_index = static_cast<uint32_t>(i);
            first_diff_route = 1;
            first_diff_abs_delta = static_cast<uint32_t>(
                guest_b > native_b ? guest_b - native_b
                                    : native_b - guest_b);
            first_diff_same_sign = (guest_b < 0) == (native_b < 0)
                ? 1u : 0u;
            break;
        }
    }
    const bool current_seed_ready = current_seed_words_.size() >= n;
    const bool rolling_seed_ready = rolling_seed_words_.size() >= n;
    const uint32_t seed_source = current_seed_ready
        ? (rolling_seed_ready ? 1u : 2u) : 0u;
    std::fprintf(
        stderr,
        "[audio] MP2K reverb reject diagnostic: block=%llu n=%zu "
        "reverb=%u history=%u metrics_corr=%.3f metrics_ratio=%.3f "
        "metrics_mae=%.3f pass=%u stage=%s seed_ready=%u seed_source=%u "
        "seed_current=%zu seed_rolling=%zu previous=%llu "
        "first_diff_index=%u first_diff_route=%u first_diff_abs_delta=%u "
        "first_diff_same_sign=%u groups=%u\n",
        static_cast<unsigned long long>(block_id), n,
        static_cast<unsigned>(reverb_),
        producer_reverb_history_aligned_ ? 1u : 0u,
        static_cast<double>(metrics.correlation),
        static_cast<double>(metrics.level_ratio),
        static_cast<double>(metrics.mean_abs_error), metrics.pass ? 1u : 0u,
        dma_stage ? "dma" : "c70",
        current_seed_ready && rolling_seed_ready ? 1u : 0u, seed_source,
        current_seed_words_.size(), rolling_seed_words_.size(),
        static_cast<unsigned long long>(previous_block_id_), first_diff_index,
        first_diff_route, first_diff_abs_delta, first_diff_same_sign,
        group_count);
    for (uint32_t i = 0; i < group_count && i < groups.size(); ++i) {
        const auto& group = groups[i];
        std::fprintf(
            stderr,
            "[audio] MP2K reverb reject group: block=%llu group=%u "
            "sample=%u accumulator_hash=0x%08x input_hash=0x%08x "
            "output_hash=0x%08x guest_old=0x%08x/0x%08x "
            "native_old=0x%08x/0x%08x guest_new=0x%08x/0x%08x "
            "native_new=0x%08x/0x%08x\n",
            static_cast<unsigned long long>(block_id), i,
            group.sample_index, group.accumulator_hash, group.input_hash,
            group.output_hash, group.guest_old_hash[0],
            group.guest_old_hash[1], group.native_old_hash[0],
            group.native_old_hash[1], group.guest_new_hash[0],
            group.guest_new_hash[1], group.native_new_hash[0],
            group.native_new_hash[1]);
    }
}

void Mp2kShadow::log_post_probation_reject_diagnostic(
    uint64_t block_id, std::size_t n, const SignedStereoMetrics& metrics) {
    // This is deliberately separate from the first-reject reverb dump.  The
    // latter is consumed by the startup block, while this one records the
    // first failure after an owned/published and good probation window. The
    // good window need not yet have reached the second-window proof point.
    const bool dma_stage = reverb_block_start_[0] != 0 &&
        reverb_block_start_[1] != 0 && dry_guest_a_.size() >= n &&
        dry_guest_b_.size() >= n &&
        dry_guest_block_id_[0] == block_id &&
        dry_guest_block_id_[1] == block_id && native_block_state_.size() >= n;
    const auto quantize = [](float value) {
        return std::clamp(static_cast<int32_t>(std::lround(
            std::clamp(value, -1.0f, 127.0f / 128.0f) * 128.0f)), -128, 127);
    };
    uint32_t first_diff_index = UINT32_MAX;
    uint32_t first_diff_route = 0;
    int32_t first_guest = 0;
    int32_t first_native_q7 = 0;
    int32_t first_native_q15 = 0;
    int32_t first_post_q7 = 0;
    uint32_t first_abs_delta = 0;
    uint32_t first_same_sign = 0;
    int32_t first_dry_guest_a = 0;
    int32_t first_dry_guest_b = 0;
    int32_t first_dry_native_a_q15 = 0;
    int32_t first_dry_native_b_q15 = 0;
    uint32_t max_diff_index = UINT32_MAX;
    uint32_t max_diff_route = 0;
    uint32_t max_abs_delta = 0;
    int32_t max_guest = 0;
    int32_t max_native_q7 = 0;
    int32_t max_native_q15 = 0;
    int32_t max_post_q7 = 0;
    std::size_t mismatch_count = 0;
    std::size_t mismatch_run = 0;
    std::size_t mismatch_run_start = 0;
    std::size_t max_mismatch_run = 0;
    std::size_t max_mismatch_run_start = 0;
    std::vector<int8_t> selected_guest_a(n);
    std::vector<int8_t> selected_guest_b(n);
    std::vector<float> selected_native_a(n);
    std::vector<float> selected_native_b(n);
    for (std::size_t i = 0; i < n; ++i) {
        const int32_t guest_a = dma_stage ? dry_guest_a_[i] : producer_a_[i];
        const int32_t guest_b = dma_stage ? dry_guest_b_[i] : producer_b_[i];
        const int32_t native_a_q15 = dma_stage
            ? native_block_state_[i].dry_right_q15
            : quantize(native_block_r_[i]) * 256;
        const int32_t native_b_q15 = dma_stage
            ? native_block_state_[i].dry_left_q15
            : quantize(native_block_l_[i]) * 256;
        const int32_t native_a = native_a_q15 / 256;
        const int32_t native_b = native_b_q15 / 256;
        selected_guest_a[i] = static_cast<int8_t>(guest_a);
        selected_guest_b[i] = static_cast<int8_t>(guest_b);
        selected_native_a[i] = static_cast<float>(native_a_q15) / 32768.0f;
        selected_native_b[i] = static_cast<float>(native_b_q15) / 32768.0f;
        const bool mismatch_a = guest_a != native_a;
        const bool mismatch_b = guest_b != native_b;
        if (mismatch_a || mismatch_b) {
            ++mismatch_count;
            if (mismatch_run == 0) mismatch_run_start = i;
            ++mismatch_run;
            if (mismatch_run > max_mismatch_run) {
                max_mismatch_run = mismatch_run;
                max_mismatch_run_start = mismatch_run_start;
            }
        } else {
            mismatch_run = 0;
        }
        if (first_diff_index == UINT32_MAX && (mismatch_a || mismatch_b)) {
            first_diff_index = static_cast<uint32_t>(i);
            first_diff_route = mismatch_a ? 0u : 1u;
            first_guest = first_diff_route == 0 ? guest_a : guest_b;
            first_native_q7 = first_diff_route == 0 ? native_a : native_b;
            first_native_q15 = first_diff_route == 0
                ? native_a_q15 : native_b_q15;
            first_abs_delta = static_cast<uint32_t>(
                first_guest > first_native_q7
                    ? first_guest - first_native_q7
                    : first_native_q7 - first_guest);
            first_same_sign = (first_guest < 0) == (first_native_q7 < 0)
                ? 1u : 0u;
            first_post_q7 = first_diff_route == 0
                ? (i < native_block_state_.size()
                    ? native_block_state_[i].post_right_q15 / 256
                    : quantize(native_block_r_[i]))
                : (i < native_block_state_.size()
                    ? native_block_state_[i].post_left_q15 / 256
                    : quantize(native_block_l_[i]));
            first_dry_guest_a = i < dry_guest_a_.size()
                ? dry_guest_a_[i] : 0;
            first_dry_guest_b = i < dry_guest_b_.size()
                ? dry_guest_b_[i] : 0;
            first_dry_native_a_q15 = i < native_block_state_.size()
                ? native_block_state_[i].dry_right_q15 : 0;
            first_dry_native_b_q15 = i < native_block_state_.size()
                ? native_block_state_[i].dry_left_q15 : 0;
        }
        const auto note_max = [&](uint32_t route, int32_t guest,
                                  int32_t native, int32_t native_q15,
                                  int32_t post_q7) {
            const uint32_t delta = static_cast<uint32_t>(
                guest > native ? guest - native : native - guest);
            if (delta <= max_abs_delta) return;
            max_abs_delta = delta;
            max_diff_index = static_cast<uint32_t>(i);
            max_diff_route = route;
            max_guest = guest;
            max_native_q7 = native;
            max_native_q15 = native_q15;
            max_post_q7 = post_q7;
        };
        const int32_t post_a_q7 = i < native_block_state_.size()
            ? native_block_state_[i].post_right_q15 / 256
            : quantize(native_block_r_[i]);
        const int32_t post_b_q7 = i < native_block_state_.size()
            ? native_block_state_[i].post_left_q15 / 256
            : quantize(native_block_l_[i]);
        if (mismatch_a)
            note_max(0, guest_a, native_a, native_a_q15, post_a_q7);
        if (mismatch_b)
            note_max(1, guest_b, native_b, native_b_q15, post_b_q7);
    }

    SignedStereoMetrics best_signed_alignment{};
    best_signed_alignment.correlation = -2.0f;
    int best_signed_lag = 0;
    uint32_t best_signed_permutation = 0;
    for (int lag = -8; lag <= 8; ++lag) {
        const std::size_t guest_start = lag < 0
            ? static_cast<std::size_t>(-lag) : 0;
        const std::size_t native_start = lag > 0
            ? static_cast<std::size_t>(lag) : 0;
        const std::size_t start = std::max(guest_start, native_start);
        if (start >= n || n - start < 8) continue;
        const std::size_t count = n - start;
        for (uint32_t permutation = 0; permutation < 2; ++permutation) {
            const float* native_a = permutation == 0
                ? selected_native_a.data() + native_start
                : selected_native_b.data() + native_start;
            const float* native_b = permutation == 0
                ? selected_native_b.data() + native_start
                : selected_native_a.data() + native_start;
            const SignedStereoMetrics candidate = compare_signed_stereo_block(
                selected_guest_a.data() + guest_start,
                selected_guest_b.data() + guest_start, native_a, native_b,
                count);
            if (candidate.correlation > best_signed_alignment.correlation ||
                (candidate.correlation == best_signed_alignment.correlation &&
                 candidate.mean_abs_error <
                     best_signed_alignment.mean_abs_error)) {
                best_signed_alignment = candidate;
                best_signed_lag = lag;
                best_signed_permutation = permutation;
            }
        }
    }

    const ProducerSampleState* max_state = nullptr;
    const ProducerSampleState* previous_max_state = nullptr;
    if (max_diff_index < native_block_state_.size()) {
        max_state = &native_block_state_[max_diff_index];
        if (max_diff_index > 0)
            previous_max_state = &native_block_state_[max_diff_index - 1];
    }
    const bool native_state_transition = max_state && previous_max_state &&
        (max_state->active_voices != previous_max_state->active_voices ||
         max_state->first_voice != previous_max_state->first_voice ||
         max_state->first_ctype != previous_max_state->first_ctype ||
         max_state->first_wave != previous_max_state->first_wave ||
         max_state->first_resolved_wave !=
             previous_max_state->first_resolved_wave ||
         max_state->first_pos != previous_max_state->first_pos ||
         max_state->first_frac != previous_max_state->first_frac ||
         max_state->first_step != previous_max_state->first_step ||
         max_state->first_sample != previous_max_state->first_sample);
    uint32_t max_active_voices = 0;
    uint32_t max_first_voice = UINT32_MAX;
    uint8_t max_first_ctype = 0;
    uint32_t max_first_wave = 0;
    uint32_t max_first_resolved_wave = 0;
    uint32_t max_first_pos = 0;
    uint32_t max_first_frac = 0;
    uint32_t max_first_step = 0;
    int32_t max_first_sample = 0;
    uint32_t previous_active_voices = 0;
    uint32_t previous_first_voice = UINT32_MAX;
    uint8_t previous_first_ctype = 0;
    uint32_t previous_first_wave = 0;
    uint32_t previous_first_resolved_wave = 0;
    uint32_t previous_first_pos = 0;
    uint32_t previous_first_frac = 0;
    uint32_t previous_first_step = 0;
    int32_t previous_first_sample = 0;
    if (max_state) {
        max_active_voices = max_state->active_voices;
        max_first_voice = max_state->first_voice;
        max_first_ctype = max_state->first_ctype;
        max_first_wave = max_state->first_wave;
        max_first_resolved_wave = max_state->first_resolved_wave;
        max_first_pos = max_state->first_pos;
        max_first_frac = max_state->first_frac;
        max_first_step = max_state->first_step;
        max_first_sample = max_state->first_sample;
    }
    if (previous_max_state) {
        previous_active_voices = previous_max_state->active_voices;
        previous_first_voice = previous_max_state->first_voice;
        previous_first_ctype = previous_max_state->first_ctype;
        previous_first_wave = previous_max_state->first_wave;
        previous_first_resolved_wave =
            previous_max_state->first_resolved_wave;
        previous_first_pos = previous_max_state->first_pos;
        previous_first_frac = previous_max_state->first_frac;
        previous_first_step = previous_max_state->first_step;
        previous_first_sample = previous_max_state->first_sample;
    }
    const ProducerSampleState* block_start_state =
        native_block_state_.empty() ? nullptr : &native_block_state_.front();
    const uint32_t diagnostic_voice = max_state
        ? max_state->first_voice : UINT32_MAX;
    const auto* guest_voice = producer_guest_snapshot_.valid
        ? find_guest_voice(producer_guest_snapshot_, diagnostic_voice)
        : nullptr;
    const std::size_t guest_advances = max_diff_index == UINT32_MAX
        ? 0u : static_cast<std::size_t>(max_diff_index) + 1u;
    const GuestCursorProjection guest_start = guest_voice
        ? project_guest_cursor(*guest_voice, pcm_freq_, pcm_freq_, 0)
        : GuestCursorProjection{};
    const GuestCursorProjection guest_source_max = guest_voice
        ? project_guest_cursor(*guest_voice, pcm_freq_, pcm_freq_,
                               guest_advances)
        : GuestCursorProjection{};
    const bool native_start_valid = block_start_state &&
        block_start_state->first_pre_voice != UINT32_MAX;
    const bool native_max_valid = max_state &&
        max_state->first_voice != UINT32_MAX;
    uint64_t host_advances = 0;
    if (native_start_valid && native_max_valid && block_start_state &&
        max_state->reverb_write >= block_start_state->reverb_write &&
        block_start_state->reverb_write != 0) {
        host_advances = max_state->reverb_write -
            block_start_state->reverb_write + 1u;
    }
    const bool host_trajectory_valid = host_advances != 0 &&
        host_advances <= UINT32_MAX;
    const GuestCursorProjection guest_host_max = guest_voice &&
        host_trajectory_valid
        ? project_guest_cursor(*guest_voice, pcm_freq_, render_rate_,
                               static_cast<std::size_t>(host_advances))
        : GuestCursorProjection{};
    const bool cursor_start_match = guest_start.valid && native_start_valid &&
        block_start_state->first_pre_voice == diagnostic_voice &&
        guest_start.start_index == block_start_state->first_pre_pos &&
        guest_start.start_frac ==
            (block_start_state->first_pre_frac & kMp2kFracMask);
    const bool cursor_max_match = guest_host_max.valid &&
        host_trajectory_valid && native_max_valid &&
        guest_host_max.index == max_state->first_pos &&
        guest_host_max.frac == (max_state->first_frac & kMp2kFracMask);
    auto& cursor_diag = producer_cursor_diag_;
    cursor_diag = {};
    cursor_diag.valid = true;
    cursor_diag.block_id = block_id;
    cursor_diag.snapshot_block = producer_guest_snapshot_.valid
        ? producer_guest_snapshot_.sequence : UINT64_MAX;
    cursor_diag.snapshot_cursor = producer_guest_snapshot_.pre_cursor;
    cursor_diag.voice = diagnostic_voice;
    cursor_diag.max_index = max_diff_index;
    cursor_diag.advances = static_cast<uint32_t>(std::min<std::size_t>(
        guest_advances, UINT32_MAX));
    cursor_diag.guest_valid = guest_source_max.valid ? 1u : 0u;
    cursor_diag.guest_exhausted = guest_source_max.exhausted ? 1u : 0u;
    cursor_diag.native_start_valid = native_start_valid ? 1u : 0u;
    cursor_diag.native_max_valid = native_max_valid ? 1u : 0u;
    cursor_diag.host_trajectory_valid = host_trajectory_valid ? 1u : 0u;
    cursor_diag.start_match = cursor_start_match ? 1u : 0u;
    cursor_diag.max_match = cursor_max_match ? 1u : 0u;
    cursor_diag.native_start_voice = block_start_state
        ? block_start_state->first_pre_voice : UINT32_MAX;
    cursor_diag.native_start_pos = block_start_state
        ? block_start_state->first_pre_pos : 0u;
    cursor_diag.native_start_frac = block_start_state
        ? block_start_state->first_pre_frac : 0u;
    cursor_diag.native_start_step = block_start_state
        ? block_start_state->first_pre_step : 0u;
    cursor_diag.native_max_pos = max_state ? max_state->first_pos : 0u;
    cursor_diag.native_max_frac = max_state ? max_state->first_frac : 0u;
    cursor_diag.native_max_step = max_state ? max_state->first_step : 0u;
    if (guest_voice) {
        cursor_diag.guest_sample_cursor = guest_voice->sample_cursor;
        cursor_diag.guest_resolved_data = guest_voice->resolved_data;
    }
    cursor_diag.guest_start_index = guest_start.start_index;
    cursor_diag.guest_start_frac = guest_start.start_frac;
    cursor_diag.guest_max_index = guest_source_max.index;
    cursor_diag.guest_max_frac = guest_source_max.frac;
    cursor_diag.guest_step = guest_source_max.step_q23;
    cursor_diag.guest_loop_wraps = guest_source_max.loop_wraps;
    cursor_diag.guest_host_index = guest_host_max.index;
    cursor_diag.guest_host_frac = guest_host_max.frac;
    cursor_diag.guest_host_step = guest_host_max.step_q23;
    cursor_diag.guest_host_loop_wraps = guest_host_max.loop_wraps;
    cursor_diag.host_advances = static_cast<uint32_t>(std::min<uint64_t>(
        host_advances, UINT32_MAX));
    cursor_diag.provenance =
        !guest_host_max.valid || !native_start_valid || !native_max_valid ||
                !host_trajectory_valid
            ? 0u
            : !cursor_start_match ? 1u : cursor_max_match ? 3u : 2u;
    std::fprintf(
        stderr,
        "[audio] MP2K post-probation reject diagnostic: block=%llu "
        "published_block=%llu previous=%llu n=%zu "
        "metrics_corr=%.3f metrics_ratio=%.3f metrics_mae=%.3f "
        "stage=%s reverb=%u history=%u "
        "producer_route=0x%08x/0x%08x dma_route=0x%08x/0x%08x "
        "dry_ids=%llu/%llu first_diff_index=%u first_diff_route=%u "
        "guest_q7=%d native_q7=%d native_q15=%d post_q7=%d "
        "first_abs_delta=%u first_same_sign=%u "
        "dry_guest_q7=%d/%d dry_native_q15=%d/%d "
        "mismatch_count=%zu mismatch_max_run=%zu@%zu "
        "best_lag=%d best_perm=%u best_corr=%.3f best_ratio=%.3f "
        "best_mae=%.3f max_diff_index=%u max_diff_route=%u "
        "max_guest_q7=%d max_native_q7=%d max_native_q15=%d "
        "max_post_q7=%d "
        "context_active=%llu context_pending=%llu context_routes=0x%02x "
        "writers=%u writer_completions=%llu c70_completions=%llu\n",
        static_cast<unsigned long long>(block_id),
        static_cast<unsigned long long>(published_block_id_),
        static_cast<unsigned long long>(previous_block_id_), n,
        static_cast<double>(metrics.correlation),
        static_cast<double>(metrics.level_ratio),
        static_cast<double>(metrics.mean_abs_error), dma_stage ? "dma" : "c70",
        static_cast<unsigned>(reverb_),
        producer_reverb_history_aligned_ ? 1u : 0u,
        producer_route_addr_[0], producer_route_addr_[1],
        reverb_block_start_[0], reverb_block_start_[1],
        static_cast<unsigned long long>(dry_guest_block_id_[0]),
        static_cast<unsigned long long>(dry_guest_block_id_[1]),
        first_diff_index, first_diff_route, first_guest, first_native_q7,
        first_native_q15, first_post_q7, first_abs_delta, first_same_sign,
        first_dry_guest_a,
        first_dry_guest_b, first_dry_native_a_q15, first_dry_native_b_q15,
        mismatch_count, max_mismatch_run, max_mismatch_run_start,
        best_signed_lag, best_signed_permutation,
        static_cast<double>(best_signed_alignment.correlation),
        static_cast<double>(best_signed_alignment.level_ratio),
        static_cast<double>(best_signed_alignment.mean_abs_error),
        max_diff_index, max_diff_route, max_guest, max_native_q7,
        max_native_q15, max_post_q7,
        static_cast<unsigned long long>(producer_context_active_block_id_),
        static_cast<unsigned long long>(producer_context_pending_block_id_),
        static_cast<unsigned>(producer_context_completed_routes_),
        producer_context_writer_count_,
        static_cast<unsigned long long>(producer_context_writer_completions_),
        static_cast<unsigned long long>(
            producer_context_c70_writer_completions_));
    std::fprintf(
        stderr,
        "[audio] MP2K post-probation state: block=%llu max_index=%u "
        "transition=%u current_active=%u current_voice=%u "
        "current_ctype=0x%02x current_wave=0x%08x "
        "current_resolved=0x%08x current_pos=%u current_frac=0x%06x "
        "current_step=0x%08x current_sample=%d "
        "previous_active=%u previous_voice=%u previous_ctype=0x%02x "
        "previous_wave=0x%08x previous_resolved=0x%08x "
        "previous_pos=%u previous_frac=0x%06x previous_step=0x%08x "
        "previous_sample=%d\n",
        static_cast<unsigned long long>(block_id), max_diff_index,
        native_state_transition ? 1u : 0u, max_active_voices,
        max_first_voice, static_cast<unsigned>(max_first_ctype), max_first_wave,
        max_first_resolved_wave, max_first_pos, max_first_frac,
        max_first_step, max_first_sample, previous_active_voices,
        previous_first_voice, static_cast<unsigned>(previous_first_ctype),
        previous_first_wave, previous_first_resolved_wave, previous_first_pos,
        previous_first_frac, previous_first_step, previous_first_sample);
    std::fprintf(
        stderr,
        "[audio] MP2K post-probation cursor: block=%llu "
        "snapshot_block=%llu snapshot_cursor=%llu voice=%u max_index=%u "
        "advances=%u guest_valid=%u exhausted=%u guest_cp=0x%08x "
        "guest_data=0x%08x guest_start=%u/0x%06x "
        "guest_source=%u/0x%06x/0x%08x wraps=%u "
        "guest_host=%u/0x%06x/0x%08x wraps=%u host_advances=%u "
        "native_start=%u:%u/0x%06x/0x%08x "
        "native_max=%u/0x%06x/0x%08x native_valid=%u/%u "
        "host_valid=%u rates=%u/%u "
        "start_match=%u max_match=%u provenance=%u\n",
        static_cast<unsigned long long>(block_id),
        static_cast<unsigned long long>(cursor_diag.snapshot_block),
        static_cast<unsigned long long>(cursor_diag.snapshot_cursor),
        cursor_diag.voice, cursor_diag.max_index, cursor_diag.advances,
        static_cast<unsigned>(cursor_diag.guest_valid),
        static_cast<unsigned>(cursor_diag.guest_exhausted),
        cursor_diag.guest_sample_cursor, cursor_diag.guest_resolved_data,
        cursor_diag.guest_start_index, cursor_diag.guest_start_frac,
        cursor_diag.guest_max_index, cursor_diag.guest_max_frac,
        cursor_diag.guest_step, cursor_diag.guest_loop_wraps,
        cursor_diag.guest_host_index, cursor_diag.guest_host_frac,
        cursor_diag.guest_host_step, cursor_diag.guest_host_loop_wraps,
        cursor_diag.host_advances,
        cursor_diag.native_start_voice, cursor_diag.native_start_pos,
        cursor_diag.native_start_frac, cursor_diag.native_start_step,
        cursor_diag.native_max_pos, cursor_diag.native_max_frac,
        cursor_diag.native_max_step,
        static_cast<unsigned>(cursor_diag.native_start_valid),
        static_cast<unsigned>(cursor_diag.native_max_valid),
        static_cast<unsigned>(cursor_diag.host_trajectory_valid), pcm_freq_,
        render_rate_,
        static_cast<unsigned>(cursor_diag.start_match),
        static_cast<unsigned>(cursor_diag.max_match),
        static_cast<unsigned>(cursor_diag.provenance));

    auto source_name = [](uint8_t source_kind) {
        switch (source_kind) {
        case 1: return "PCM";
        case 2: return "FIX";
        case 3: return "compressed";
        case 4: return "reversed";
        case 5: return "synth";
        default: return "unknown";
        }
    };
    auto abs_i32 = [](int32_t value) -> uint32_t {
        const int64_t wide = value;
        return static_cast<uint32_t>(wide < 0 ? -wide : wide);
    };
    auto& voice_diag = producer_voice_diag_;
    voice_diag = {};
    voice_diag.valid = true;
    voice_diag.block_id = block_id;
    voice_diag.max_index = max_diff_index;
    voice_diag.route = static_cast<uint8_t>(max_diff_route);
    voice_diag.writer_stage = dma_stage ? 1u : 0u;
    const bool max_index_valid = max_diff_index != UINT32_MAX &&
        static_cast<std::size_t>(max_diff_index) < n;
    const bool voice_trace_valid = max_index_valid &&
        static_cast<std::size_t>(max_diff_index) <
            native_block_voice_traces_.size();
    voice_diag.trace_valid = voice_trace_valid ? 1u : 0u;
    if (max_index_valid) {
        const std::size_t index = max_diff_index;
        if (index < native_accumulator_words_.size())
            voice_diag.native_accumulator_hash = mp2k_diag_hash_word(
                native_accumulator_words_[index]);
        if (index < rolling_seed_words_.size())
            voice_diag.guest_seed_hash = mp2k_diag_hash_word(
                rolling_seed_words_[index]);
        if (index < current_seed_words_.size())
            voice_diag.native_seed_hash = mp2k_diag_hash_word(
                current_seed_words_[index]);
        if (index < producer_raw_a_.size() && index < producer_raw_b_.size()) {
            uint32_t hash = mp2k_diag_hash_word(static_cast<uint32_t>(
                static_cast<uint16_t>(producer_raw_a_[index])));
            hash = mp2k_diag_hash_mix(hash, static_cast<uint32_t>(
                static_cast<uint16_t>(producer_raw_b_[index])));
            voice_diag.guest_raw_hash = hash;
        }
        if (index < dry_guest_a_.size() && index < dry_guest_b_.size()) {
            uint32_t hash = mp2k_diag_hash_word(static_cast<uint32_t>(
                static_cast<uint8_t>(dry_guest_a_[index])));
            hash = mp2k_diag_hash_mix(hash, static_cast<uint32_t>(
                static_cast<uint8_t>(dry_guest_b_[index])));
            voice_diag.guest_dry_hash = hash;
        }
    }
    if (voice_trace_valid) {
        const auto& traces = native_block_voice_traces_[max_diff_index];
        for (std::size_t i = 0; i < traces.size(); ++i) {
            const auto& trace = traces[i];
            if (!trace.active) continue;
            auto& out = voice_diag.voices[i];
            out.active = true;
            out.channel = trace.channel;
            out.source_kind = trace.source_kind;
            out.ctype = trace.ctype;
            out.wave = trace.wave;
            out.resolved_wave = trace.resolved_wave;
            out.cursor = trace.cursor;
            out.frac = trace.frac;
            out.step = trace.step;
            out.sample = trace.sample;
            out.gain_right_q9 = trace.gain_right_q9;
            out.gain_left_q9 = trace.gain_left_q9;
            out.packed_gain = trace.packed_gain;
            out.packed_delta = trace.accumulator_after -
                trace.accumulator_before;
            const int32_t delta_right = static_cast<int16_t>(
                out.packed_delta & 0xFFFFu);
            const int32_t delta_left = static_cast<int16_t>(
                (out.packed_delta >> 16) & 0xFFFFu);
            out.lane_abs = std::max(abs_i32(delta_right),
                                    abs_i32(delta_left));
            ++voice_diag.active_voices;
            if (out.lane_abs > voice_diag.dominant_lane_abs) {
                voice_diag.dominant_lane_abs = out.lane_abs;
                voice_diag.dominant_voice = static_cast<uint32_t>(i);
            }
        }
    }
    std::fprintf(
        stderr,
        "[audio] MP2K post-probation voice summary: block=%llu "
        "max_index=%u route=%u writer_stage=%s traces=%u active=%u "
        "dominant_voice=%u dominant_lane_abs=%u "
        "native_acc_hash=0x%08x guest_seed_hash=0x%08x "
        "native_seed_hash=0x%08x guest_raw_hash=0x%08x "
        "guest_dry_hash=0x%08x\n",
        static_cast<unsigned long long>(block_id), max_diff_index,
        max_diff_route, dma_stage ? "dma" : "c70",
        static_cast<unsigned>(voice_diag.trace_valid),
        voice_diag.active_voices, voice_diag.dominant_voice,
        voice_diag.dominant_lane_abs, voice_diag.native_accumulator_hash,
        voice_diag.guest_seed_hash, voice_diag.native_seed_hash,
        voice_diag.guest_raw_hash, voice_diag.guest_dry_hash);
    if (voice_trace_valid) {
        const auto& traces = voice_diag.voices;
        for (std::size_t i = 0; i < traces.size(); ++i) {
            const auto& trace = traces[i];
            if (!trace.active) continue;
            const int64_t contribution_right =
                static_cast<int64_t>(trace.sample) * trace.gain_right_q9;
            const int64_t contribution_left =
                static_cast<int64_t>(trace.sample) * trace.gain_left_q9;
            std::fprintf(
                stderr,
                "[audio] MP2K post-probation voice: block=%llu "
                "max_index=%u voice=%u source=%s source_kind=%u "
                "ctype=0x%02x sample=%d gain_q9=%u/%u "
                "contrib_q9=%lld/%lld packed_gain=0x%08x "
                "packed_delta=0x%08x lane_abs=%u cursor=%u "
                "frac=0x%06x step=0x%08x wave=0x%08x resolved=0x%08x\n",
                static_cast<unsigned long long>(block_id), max_diff_index,
                trace.channel, source_name(trace.source_kind),
                static_cast<unsigned>(trace.source_kind),
                static_cast<unsigned>(trace.ctype), trace.sample,
                trace.gain_right_q9, trace.gain_left_q9,
                static_cast<long long>(contribution_right),
                static_cast<long long>(contribution_left),
                trace.packed_gain, trace.packed_delta, trace.lane_abs,
                trace.cursor, trace.frac, trace.step, trace.wave,
                trace.resolved_wave);
        }
    }
    producer_post_probation_reject_diag_logged_ = true;
}

void Mp2kShadow::maybe_log_producer_source_diagnostic(bool accepted) {
    if (!accepted || producer_source_diag_logged_ || canonical_fallback_ ||
        producer_block_id_ == UINT64_MAX || producer_samples_ == 0) {
        return;
    }
    if (producer_a_.size() < producer_samples_ ||
        producer_b_.size() < producer_samples_ ||
        native_block_state_.size() < producer_samples_ ||
        dry_guest_a_.size() < producer_samples_ ||
        dry_guest_b_.size() < producer_samples_ ||
        dry_guest_block_id_[0] != producer_block_id_ ||
        dry_guest_block_id_[1] != producer_block_id_ ||
        reverb_block_start_[0] == 0 || reverb_block_start_[1] == 0) {
        return;
    }

    // Keep this diagnostic bounded even if a future driver exposes an
    // unusually large producer block. The comparison is same-index and
    // occurs before ProducerHostResampler sees the block, so it cannot hide a
    // timestamp/host-grid problem behind an alignment search.
    constexpr std::size_t kMaxDiagnosticSamples = 4096;
    const std::size_t n = std::min<std::size_t>({
        producer_a_.size(), producer_b_.size(), native_block_state_.size(),
        dry_guest_a_.size(), dry_guest_b_.size(),
        static_cast<std::size_t>(producer_samples_), kMaxDiagnosticSamples});
    if (n < 8) return;

    std::vector<float> dma_a(n), dma_b(n), native_dma_a(n), native_dma_b(n);
    for (std::size_t i = 0; i < n; ++i) {
        dma_a[i] = static_cast<float>(dry_guest_a_[i]) / 128.0f;
        dma_b[i] = static_cast<float>(dry_guest_b_[i]) / 128.0f;
        native_dma_a[i] = static_cast<float>(
            native_block_state_[i].dry_right_q15) / 32768.0f;
        native_dma_b[i] = static_cast<float>(
            native_block_state_[i].dry_left_q15) / 32768.0f;
    }
    const SignedStereoMetrics c70_vs_dma = compare_signed_stereo_block(
        producer_a_.data(), producer_b_.data(), dma_a.data(), dma_b.data(), n);
    const SignedStereoMetrics native_vs_dma = compare_signed_stereo_block(
        dry_guest_a_.data(), dry_guest_b_.data(), native_dma_a.data(),
        native_dma_b.data(), n);

    producer_source_diag_.valid = true;
    producer_source_diag_.block_id = producer_block_id_;
    producer_source_diag_.samples = static_cast<uint32_t>(n);
    producer_source_diag_.c70_addr = producer_route_addr_[0];
    producer_source_diag_.dma_route_a = reverb_block_start_[0];
    producer_source_diag_.dma_route_b = reverb_block_start_[1];
    producer_source_diag_.c70_vs_dma = c70_vs_dma;
    producer_source_diag_.native_vs_dma = native_vs_dma;
    std::fprintf(
        stderr,
        "[audio] MP2K producer-source diagnostic: block=%llu samples=%zu "
        "c70_addr=0x%08x dma_route_a=0x%08x dma_route_b=0x%08x "
        "c70_dma_corr=%.3f c70_dma_ratio=%.3f c70_dma_mae=%.3f "
        "native_dma_corr=%.3f native_dma_ratio=%.3f native_dma_mae=%.3f\n",
        static_cast<unsigned long long>(producer_block_id_), n,
        producer_source_diag_.c70_addr, producer_source_diag_.dma_route_a,
        producer_source_diag_.dma_route_b,
        static_cast<double>(c70_vs_dma.correlation),
        static_cast<double>(c70_vs_dma.level_ratio),
        static_cast<double>(c70_vs_dma.mean_abs_error),
        static_cast<double>(native_vs_dma.correlation),
        static_cast<double>(native_vs_dma.level_ratio),
        static_cast<double>(native_vs_dma.mean_abs_error));
    producer_source_diag_logged_ = true;
}

void Mp2kShadow::reverb_history_word(uint8_t route, uint32_t addr,
                                     uint32_t guest_old,
                                     uint64_t block_id) {
    if (canonical_fallback_) return;
    if (route > 1) return;
    if (block_id != producer_block_id_) {
        if (producer_block_id_ != UINT64_MAX)
            canonical_fail("reverb write outside active block");
        return;
    }
    guest_reverb_old_words_[route][addr] = guest_old;
    // Native derived state is intentionally reset on savestate load.  Seed
    // each circular slot from restored guest RAM once; revisits use only the
    // native word previously written for that address.
    native_reverb_words_[route].try_emplace(addr, guest_old);
}

void Mp2kShadow::judge_producer_block(uint64_t block_id) {
    if (block_id == UINT64_MAX || block_id != producer_block_id_ ||
        producer_samples_ == 0) {
        return;
    }
    const std::size_t guest_a = producer_a_.size();
    const std::size_t guest_b = producer_b_.size();
    const std::size_t native_l = native_block_l_.size();
    const std::size_t native_r = native_block_r_.size();
    const auto log_incomplete = [&](const char* stage) {
        if (producer_block_incomplete_reported_) return;
        // This flag only suppresses duplicate diagnostics for the active
        // block; it does not alter the verifier counters or gate behavior.
        producer_block_incomplete_reported_ = true;
        log_producer_verifier_diag(stage, block_id, guest_a, guest_b,
                                   native_l, native_r,
                                   producer_alignment_.ready(), false, false,
                                   0.0f, 0.0f);
    };
    if (producer_a_.empty() || producer_b_.empty() || native_l < 8 ||
        native_r < 8) {
        if (guest_a != 0 || guest_b != 0 || native_l != 0 || native_r != 0)
            log_incomplete("incomplete");
        return;
    }
    const std::size_t n = std::min({producer_a_.size(), producer_b_.size(),
                                    native_block_l_.size(),
                                    native_block_r_.size(),
                                    static_cast<std::size_t>(producer_samples_)});
    if (n < 8) {
        log_incomplete("insufficient");
        return;
    }
    const bool complete = n >= producer_samples_ &&
        producer_a_.size() >= producer_samples_ &&
        producer_b_.size() >= producer_samples_;
    if (!complete) {
        if (!producer_block_incomplete_reported_) {
            ++producer_blocks_incomplete_;
            log_incomplete("incomplete");
        }
        if (first_incomplete_block_ == UINT64_MAX) {
            first_incomplete_block_ = block_id;
            first_incomplete_cursor_ = producer_block_start_cursor_;
            first_incomplete_guest_samples_ = static_cast<uint32_t>(
                std::min(producer_a_.size(), producer_b_.size()));
            first_incomplete_native_samples_ = static_cast<uint32_t>(
                std::min(native_block_l_.size(), native_block_r_.size()));
            first_incomplete_expected_samples_ = producer_samples_;
            first_incomplete_alignment_ready_ = producer_alignment_.ready();
            first_incomplete_route_a_ = reverb_block_start_[0];
            first_incomplete_route_b_ = reverb_block_start_[1];
        }
        return;
    }
    // Do not compare or publish a partial savestate-attached epoch. The first
    // complete pair establishes the native/guest producer boundary.
    const bool was_aligned = producer_alignment_.ready();
    if (!producer_alignment_.observe(producer_a_.size(),
                                     native_block_l_.size(),
                                     producer_samples_)) {
        log_producer_verifier_diag(
            "alignment", block_id, guest_a, guest_b, native_l, native_r,
            producer_alignment_.ready(), false, false, 0.0f, 0.0f);
        return;
    }
    // A complete guest/native pair is the first point at which the shadow
    // owns state after startup or savestate realignment.  Until this point,
    // another fractional tail is still an unowned epoch and may be discarded
    // by frame_hook().
    startup_realign_pending_ = false;
    if (!was_aligned && !producer_reverb_history_aligned_) {
        // Any partial epoch may have touched a circular slot before the
        // startup boundary. Re-anchor only known canonical history words at
        // the first complete pair; never invent or pad reverb state.
        mp2k_sync_reverb_history(guest_reverb_old_words_[0],
                                 native_reverb_words_[0]);
        mp2k_sync_reverb_history(guest_reverb_old_words_[1],
                                 native_reverb_words_[1]);
        producer_reverb_history_aligned_ = true;
    }
    // GS1 reverb is not a Q15 delay line.  It consumes two packed dry-ring
    // words per four samples, before overwriting those exact circular slots.
    // Rebuild the native producer in that integer domain once the matching
    // guest writer has supplied this block's addresses and initial history.
    const std::size_t groups = n / 4u;
    const bool reverb_enabled = reverb_ > 0;
    const bool accum_ready = native_accumulator_words_.size() >= groups * 4u;
    const bool routes_ready = reverb_block_start_[0] != 0 &&
        reverb_block_start_[1] != 0;
    const bool new_words_ready = guest_reverb_new_words_[0].size() >= groups &&
        guest_reverb_new_words_[1].size() >= groups;
    bool old_words_ready = false;
    uint32_t reverb_branch_reason = 0;
    if (!reverb_enabled) {
        reverb_branch_reason = 1;  // disabled
    } else if (!accum_ready) {
        reverb_branch_reason = 2;  // native accumulator short
    } else if (!routes_ready) {
        reverb_branch_reason = 3;  // one or both dry routes absent
    } else if (!new_words_ready) {
        reverb_branch_reason = 4;  // guest dry output absent/short
    } else {
        const uint32_t addr_a = mp2k_reverb_word_address(
            reverb_block_start_[0], 0);
        const uint32_t addr_b = mp2k_reverb_word_address(
            reverb_block_start_[1], 0);
        old_words_ready = guest_reverb_old_words_[0].find(addr_a) !=
                guest_reverb_old_words_[0].end() &&
            guest_reverb_old_words_[1].find(addr_b) !=
                guest_reverb_old_words_[1].end();
        if (!old_words_ready) reverb_branch_reason = 5;  // old history absent
    }
    if (reverb_enabled && !producer_reverb_branch_diag_logged_) {
        std::fprintf(
            stderr,
            "[audio] MP2K reverb branch diagnostic: block=%llu n=%zu "
            "groups=%zu reverb=%u history=%u accum_words=%zu "
            "route_a=0x%08x route_b=0x%08x new_groups=%zu/%zu "
            "old_group0=%u entered=%u reason=%u\n",
            static_cast<unsigned long long>(block_id), n, groups,
            static_cast<unsigned>(reverb_),
            producer_reverb_history_aligned_ ? 1u : 0u,
            native_accumulator_words_.size(), reverb_block_start_[0],
            reverb_block_start_[1], guest_reverb_new_words_[0].size(),
            guest_reverb_new_words_[1].size(), old_words_ready ? 1u : 0u,
            (accum_ready && routes_ready && new_words_ready) ? 1u : 0u,
            reverb_branch_reason);
        producer_reverb_branch_diag_logged_ = true;
    }
    const bool capture_reverb_diag = !producer_reverb_reject_diag_logged_;
    std::array<ReverbRejectGroupDiag, 2> reverb_diag_groups{};
    uint32_t reverb_diag_group_count = 0;
    if (reverb_enabled && accum_ready && routes_ready && new_words_ready) {
        for (std::size_t group = 0; group < groups; ++group) {
            std::array<uint32_t, 4> accumulators{};
            std::copy_n(native_accumulator_words_.begin() + group * 4u, 4u,
                        accumulators.begin());
            const uint32_t addr_a = mp2k_reverb_word_address(
                reverb_block_start_[0], group);
            const uint32_t addr_b = mp2k_reverb_word_address(
                reverb_block_start_[1], group);
            const auto guest_old_a = guest_reverb_old_words_[0].find(addr_a);
            const auto guest_old_b = guest_reverb_old_words_[1].find(addr_b);
            if (guest_old_a == guest_reverb_old_words_[0].end() ||
                guest_old_b == guest_reverb_old_words_[1].end()) break;
            uint32_t old_a = native_reverb_words_[0].at(addr_a);
            uint32_t old_b = native_reverb_words_[1].at(addr_b);
            // A stale native slot can survive a mid-epoch attach. The guest
            // old word is the authoritative value for this exact ring visit;
            // re-anchor only on an observed mismatch, never by padding.
            if (old_a != guest_old_a->second) {
                native_reverb_words_[0][addr_a] = guest_old_a->second;
                old_a = guest_old_a->second;
            }
            if (old_b != guest_old_b->second) {
                native_reverb_words_[1][addr_b] = guest_old_b->second;
                old_b = guest_old_b->second;
            }
            const auto post = mp2k_finalize_producer_group(
                accumulators, old_a, old_b);
            if (capture_reverb_diag && reverb_diag_group_count <
                    reverb_diag_groups.size()) {
                auto& diag = reverb_diag_groups[reverb_diag_group_count++];
                diag.sample_index = static_cast<uint32_t>(group * 4u);
                diag.accumulator_hash = mp2k_diag_hash_words(accumulators);
                diag.input_hash = diag.accumulator_hash;
                diag.input_hash = mp2k_diag_hash_mix(diag.input_hash, old_a);
                diag.input_hash = mp2k_diag_hash_mix(diag.input_hash, old_b);
                diag.output_hash = mp2k_diag_hash_words(post);
                diag.guest_old_hash[0] = mp2k_diag_hash_word(
                    guest_old_a->second);
                diag.guest_old_hash[1] = mp2k_diag_hash_word(
                    guest_old_b->second);
                diag.native_old_hash[0] = mp2k_diag_hash_word(old_a);
                diag.native_old_hash[1] = mp2k_diag_hash_word(old_b);
                diag.guest_new_hash[0] = mp2k_diag_hash_word(
                    guest_reverb_new_words_[0][group]);
                diag.guest_new_hash[1] = mp2k_diag_hash_word(
                    guest_reverb_new_words_[1][group]);
            }
            uint32_t dry_a = 0, dry_b = 0;
            mp2k_extract_dry_words(accumulators, dry_a, dry_b);
            // Until a native dry word is bit-exact, preserve the canonical
            // safety history for the next ring visit.  Once equal, this is a
            // no-op and the slot is independently native-owned.
            native_reverb_words_[0][addr_a] =
                dry_a == guest_reverb_new_words_[0][group]
                    ? dry_a : guest_reverb_new_words_[0][group];
            native_reverb_words_[1][addr_b] =
                dry_b == guest_reverb_new_words_[1][group]
                    ? dry_b : guest_reverb_new_words_[1][group];
            if (capture_reverb_diag && reverb_diag_group_count != 0 &&
                reverb_diag_group_count <= reverb_diag_groups.size() &&
                reverb_diag_groups[reverb_diag_group_count - 1].sample_index ==
                    group * 4u) {
                auto& diag = reverb_diag_groups[reverb_diag_group_count - 1];
                diag.native_new_hash[0] = mp2k_diag_hash_word(
                    native_reverb_words_[0][addr_a]);
                diag.native_new_hash[1] = mp2k_diag_hash_word(
                    native_reverb_words_[1][addr_b]);
            }
            for (std::size_t lane = 0; lane < 4u; ++lane) {
                int8_t route_a = 0, route_b = 0;
                mp2k_decode_producer_word(post[lane], route_a, route_b);
                const std::size_t i = group * 4u + lane;
                native_block_r_[i] = static_cast<float>(route_a) / 128.0f;
                native_block_l_[i] = static_cast<float>(route_b) / 128.0f;
                if (i < native_block_state_.size()) {
                    auto& state = native_block_state_[i];
                    state.post_right_q15 = static_cast<int32_t>(route_a) * 256;
                    state.post_left_q15 = static_cast<int32_t>(route_b) * 256;
                    state.old_route_addr = addr_a;
                    state.old_route_a = guest_old_a->second;
                    state.old_route_b = guest_old_b->second;
                    state.native_old_route_a = old_a;
                    state.native_old_route_b = old_b;
                }
            }
        }
    }
    // C70 is an intermediate producer block. When the paired EWRAM routes
    // are present, the hardware FIFO consumes those bytes instead; gate and
    // publish the independent native dry-route state at that same boundary.
    // The post-reverb finalizer above still updates native history and remains
    // available to diagnostics. Without paired routes, retain the existing
    // C70 gate so unsupported/partial state still fails closed.
    const bool dma_routes_complete = reverb_block_start_[0] != 0 &&
        reverb_block_start_[1] != 0 &&
        dry_guest_a_.size() >= n && dry_guest_b_.size() >= n &&
        dry_guest_block_id_[0] == block_id &&
        dry_guest_block_id_[1] == block_id &&
        native_block_state_.size() >= n;
    std::vector<float> native_dma_r;
    std::vector<float> native_dma_l;
    if (dma_routes_complete) {
        native_dma_r.resize(n);
        native_dma_l.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            native_dma_r[i] = static_cast<float>(
                native_block_state_[i].dry_right_q15) / 32768.0f;
            native_dma_l[i] = static_cast<float>(
                native_block_state_[i].dry_left_q15) / 32768.0f;
        }
    }
    const std::vector<float>& gate_native_r = dma_routes_complete
        ? native_dma_r : native_block_r_;
    const std::vector<float>& gate_native_l = dma_routes_complete
        ? native_dma_l : native_block_l_;
    const int8_t* gate_guest_a = dma_routes_complete
        ? dry_guest_a_.data() : producer_a_.data();
    const int8_t* gate_guest_b = dma_routes_complete
        ? dry_guest_b_.data() : producer_b_.data();
    const SignedStereoMetrics metrics = compare_signed_stereo_block(
        gate_guest_a, gate_guest_b, gate_native_r.data(),
        gate_native_l.data(), n);
    maybe_log_producer_source_diagnostic(metrics.pass);
    if (producer_verifier_metric_block_id_ != block_id) {
        producer_verifier_metric_block_id_ = block_id;
        log_producer_verifier_diag(
            "metric", block_id, guest_a, guest_b, native_l, native_r,
            producer_alignment_.ready(), true, metrics.pass,
            metrics.correlation, metrics.level_ratio);
    }
    if (!metrics.pass && !producer_diff_trace_.valid) {
        auto native_byte = [](float value) {
            const int32_t rounded = static_cast<int32_t>(std::lround(
                std::clamp(value, -1.0f, 127.0f / 128.0f) * 128.0f));
            return std::clamp(rounded, -128, 127);
        };
        for (std::size_t i = 0; i < n; ++i) {
            const int32_t candidate_a = native_byte(gate_native_r[i]);
            const int32_t candidate_b = native_byte(gate_native_l[i]);
            const int32_t post_a = native_byte(native_block_r_[i]);
            const int32_t post_b = native_byte(native_block_l_[i]);
            uint8_t route = 0;
            int32_t guest = gate_guest_a[i];
            int32_t native = candidate_a;
            if (guest == native) {
                route = 1;
                guest = gate_guest_b[i];
                native = candidate_b;
            }
            if (guest == native) continue;
            const ProducerSampleState state = i < native_block_state_.size()
                ? native_block_state_[i] : ProducerSampleState{};
            auto& trace = producer_diff_trace_;
            trace.valid = true;
            trace.sequence = block_id;
            trace.seed_sequence = previous_block_id_;
            trace.cursor = producer_block_start_cursor_;
            trace.route_a_addr = producer_route_addr_[0];
            trace.route_b_addr = producer_route_addr_[1];
            trace.index = static_cast<uint32_t>(i);
            trace.route = route;
            trace.guest = guest;
            trace.native_quantized = native;
            trace.guest_dry_a = i < dry_guest_a_.size()
                ? dry_guest_a_[i] : 0;
            trace.guest_dry_b = i < dry_guest_b_.size()
                ? dry_guest_b_[i] : 0;
            trace.native_dry_a = state.dry_right_q15 / 256;
            trace.native_dry_b = state.dry_left_q15 / 256;
            trace.native_post_a = post_a;
            trace.native_post_b = post_b;
            trace.guest_raw_a = producer_raw_a_[i];
            trace.guest_raw_b = producer_raw_b_[i];
            trace.native_accumulator = i < native_accumulator_words_.size()
                ? native_accumulator_words_[i] : 0;
            trace.guest_seed = i < rolling_seed_words_.size()
                ? rolling_seed_words_[i] : 0;
            trace.native_seed = i < current_seed_words_.size()
                ? current_seed_words_[i] : 0;
            trace.reverb_start_a = reverb_block_start_[0];
            trace.reverb_start_b = reverb_block_start_[1];
            trace.old_route_addr = state.old_route_addr;
            trace.guest_old_a = state.old_route_a;
            trace.guest_old_b = state.old_route_b;
            trace.native_old_a = state.native_old_route_a;
            trace.native_old_b = state.native_old_route_b;
            if (i < native_block_voice_traces_.size())
                trace.voices = native_block_voice_traces_[i];
            break;
        }
    }
    // Capture the first failure after an owned block has been published and
    // a good probation window has passed. This must run before the gate update
    // below, because a mismatch may pause the verifier and clear its state.
    if (!metrics.pass && !producer_post_probation_reject_diag_logged_ &&
        producer_good_probation_window_seen_ &&
        published_block_id_ != UINT64_MAX && vf_.signed_gate_passed()) {
        log_post_probation_reject_diagnostic(block_id, n, metrics);
    }
    if (!metrics.pass && !producer_reverb_reject_diag_logged_) {
        log_reverb_reject_diagnostic(
            block_id, n, metrics, reverb_diag_groups,
            reverb_diag_group_count);
        producer_reverb_reject_diag_logged_ = true;
    }
    vf_.set_signed_producer_gate(true, metrics.pass, metrics.correlation,
                                 metrics.level_ratio);
    saturating_increment(producer_blocks_judged_);
    if (metrics.pass) saturating_increment(producer_blocks_passed_);
    if (complete && block_id != published_block_id_) {
        if (metrics.pass && dma_timeline_.publish(
                block_id, reverb_block_start_[0], reverb_block_start_[1], n,
                pcm_freq_, render_rate_)) {
            PublishedNativeBlock published{};
            published.sequence = block_id;
            published.route_a.assign(gate_native_r.begin(),
                                     gate_native_r.begin() + n);
            published.route_b.assign(gate_native_l.begin(),
                                     gate_native_l.begin() + n);
            published_native_.push_back(std::move(published));
            while (published_native_.size() > 32)
                published_native_.pop_front();
            published_block_id_ = block_id;
            if (published_block_callback_)
                published_block_callback_(published_block_callback_context_,
                                          block_id);
        } else {
            ++producer_blocks_rejected_;
            if (first_rejected_block_ == UINT64_MAX) {
                first_rejected_block_ = block_id;
                first_rejected_reason_ = metrics.pass
                    ? ProducerRejectReason::DmaPublish
                    : ProducerRejectReason::SignedMetrics;
                first_rejected_correlation_ = metrics.correlation;
                first_rejected_level_ratio_ = metrics.level_ratio;
                first_rejected_mean_abs_error_ = metrics.mean_abs_error;
                first_rejected_cursor_ = producer_block_start_cursor_;
                first_rejected_route_a_ = producer_route_addr_[0];
                first_rejected_route_b_ = producer_route_addr_[1];
            }
            host_stream_.reset();
            dma_timeline_.reset();
            published_native_.clear();
        }
    }
    if (!complete || !audio_probe_enabled_) return;
    auto best_alignment = [&](const std::vector<int8_t>& guest_a,
                              const std::vector<int8_t>& guest_b,
                              const std::vector<float>& native_a,
                              const std::vector<float>& native_b) {
        SignedStereoMetrics best{};
        best.correlation = -2.0f;
        int best_lag = 0;
        for (int lag = -8; lag <= 8; ++lag) {
            const std::size_t guest_start = lag < 0
                ? static_cast<std::size_t>(-lag) : 0;
            const std::size_t native_start = lag > 0
                ? static_cast<std::size_t>(lag) : 0;
            const std::size_t count = std::min({
                guest_a.size() - std::min(guest_start, guest_a.size()),
                guest_b.size() - std::min(guest_start, guest_b.size()),
                native_a.size() - std::min(native_start, native_a.size()),
                native_b.size() - std::min(native_start, native_b.size())});
            if (count < 8) continue;
            const SignedStereoMetrics candidate = compare_signed_stereo_block(
                guest_a.data() + guest_start, guest_b.data() + guest_start,
                native_a.data() + native_start,
                native_b.data() + native_start, count);
            if (candidate.correlation > best.correlation) {
                best = candidate;
                best_lag = lag;
            }
        }
        return std::pair<SignedStereoMetrics, int>{best, best_lag};
    };

    const SignedStereoMetrics swapped = compare_signed_stereo_block(
        producer_a_.data(), producer_b_.data(), native_block_l_.data(),
        native_block_r_.data(), n);
    const auto aligned = best_alignment(producer_a_, producer_b_,
                                        native_block_r_, native_block_l_);
    SignedStereoMetrics guest_vs_previous{};
    if (!previous_native_l_.empty() && !previous_native_r_.empty()) {
        const std::size_t previous_n = std::min({
            producer_a_.size(), producer_b_.size(), previous_native_l_.size(),
            previous_native_r_.size(), static_cast<std::size_t>(producer_samples_)});
        if (previous_n >= 8) {
            guest_vs_previous = compare_signed_stereo_block(
                producer_a_.data(), producer_b_.data(),
                previous_native_r_.data(), previous_native_l_.data(),
                previous_n);
        }
    }
    SignedStereoMetrics previous_guest_vs_native{};
    if (!previous_producer_a_.empty() && !previous_producer_b_.empty()) {
        const std::size_t previous_n = std::min({
            previous_producer_a_.size(), previous_producer_b_.size(),
            native_block_l_.size(), native_block_r_.size(),
            static_cast<std::size_t>(producer_samples_)});
        if (previous_n >= 8) {
            previous_guest_vs_native = compare_signed_stereo_block(
                previous_producer_a_.data(), previous_producer_b_.data(),
                native_block_r_.data(), native_block_l_.data(), previous_n);
        }
    }

    if (!producer_diag_block_logged_) {
        std::fprintf(stderr,
            "[audio-block] seq=%llu prev_seq=%llu n=%zu addrA=0x%08x "
            "addrB=0x%08x pcm=%u render=%u spv=%u dma=%u reverb=%u "
            "direct_r=%.4f direct_ratio=%.4f direct_mae=%.4f "
            "swapped_r=%.4f best_lag=%d best_r=%.4f "
            "guest_vs_prev_r=%.4f prev_guest_vs_native_r=%.4f\n",
            static_cast<unsigned long long>(block_id),
            static_cast<unsigned long long>(previous_block_id_), n,
            producer_route_addr_[0], producer_route_addr_[1], pcm_freq_,
            render_rate_, spv_, static_cast<unsigned>(dma_period_),
            static_cast<unsigned>(reverb_), metrics.correlation,
            metrics.level_ratio, metrics.mean_abs_error,
            swapped.correlation, aligned.second,
            aligned.first.correlation, guest_vs_previous.correlation,
            previous_guest_vs_native.correlation);
        producer_diag_block_logged_ = true;
    }

    if (!producer_diag_diff_logged_) {
        auto native_byte = [](float value) {
            const int32_t rounded = static_cast<int32_t>(std::lround(
                clampf(value, -1.0f, 127.0f / 128.0f) * 128.0f));
            return std::clamp(rounded, -128, 127);
        };
        for (std::size_t i = 0; i < n; ++i) {
            uint8_t route = 0;
            int32_t guest = producer_a_[i];
            float native = native_block_r_[i];
            int32_t quantized = native_byte(native);
            if (guest == quantized) {
                route = 1;
                guest = producer_b_[i];
                native = native_block_l_[i];
                quantized = native_byte(native);
            }
            if (guest == quantized) continue;
            const ProducerSampleState state = i < native_block_state_.size()
                ? native_block_state_[i] : ProducerSampleState{};
            std::fprintf(stderr,
                "[audio-block-diff] seq=%llu route=%c index=%zu guest=%d "
                "native=%.6f native_q=%d dry=(%.6f,%.6f) old=(%.6f,%.6f) "
                "reverb_write=%llu active=%u first_voice=%u pos=%u "
                "frac=0x%06x step=0x%08x gain=(%.3f,%.3f)\n",
                static_cast<unsigned long long>(block_id),
                route == 0 ? 'A' : 'B', i, guest, native, quantized,
                state.dry_right, state.dry_left, state.old_right,
                state.old_left,
                static_cast<unsigned long long>(state.reverb_write),
                state.active_voices, state.first_voice, state.first_pos,
                state.first_frac, state.first_step, state.first_gain_right,
                state.first_gain_left);
            producer_diag_diff_logged_ = true;
            break;
        }
    }

    const uint64_t warm_samples = (reverb_delay_q32_ + 0xFFFFFFFFull) >> 32;
    const bool mature = native_block_state_.size() >= n &&
        native_block_state_.front().reverb_write >= warm_samples &&
        dry_guest_a_.size() >= n && dry_guest_b_.size() >= n;
    if (!mature || producer_diag_mature_logged_) return;

    std::vector<float> native_dry_a(n), native_dry_b(n);
    for (std::size_t i = 0; i < n; ++i) {
        native_dry_a[i] = static_cast<float>(native_block_state_[i].dry_right_q15) /
                          32768.0f;
        native_dry_b[i] = static_cast<float>(native_block_state_[i].dry_left_q15) /
                          32768.0f;
    }
    const SignedStereoMetrics dry = compare_signed_stereo_block(
        dry_guest_a_.data(), dry_guest_b_.data(), native_dry_a.data(),
        native_dry_b.data(), n);
    const SignedStereoMetrics dry_swapped = compare_signed_stereo_block(
        dry_guest_a_.data(), dry_guest_b_.data(), native_dry_b.data(),
        native_dry_a.data(), n);
    const auto dry_aligned = best_alignment(dry_guest_a_, dry_guest_b_,
                                             native_dry_a, native_dry_b);

    SignedStereoMetrics best_permuted{};
    best_permuted.correlation = -2.0f;
    int best_permutation[4] = {0, 1, 2, 3};
    int permutation[4] = {0, 1, 2, 3};
    do {
        std::vector<float> permuted_r(n), permuted_l(n);
        for (std::size_t base = 0; base < n; base += 4) {
            const std::size_t group = std::min<std::size_t>(4, n - base);
            for (std::size_t j = 0; j < group; ++j) {
                const std::size_t source = base +
                    std::min<std::size_t>(permutation[j], group - 1);
                permuted_r[base + j] = native_block_r_[source];
                permuted_l[base + j] = native_block_l_[source];
            }
        }
        const SignedStereoMetrics candidate = compare_signed_stereo_block(
            producer_a_.data(), producer_b_.data(), permuted_r.data(),
            permuted_l.data(), n);
        if (candidate.correlation > best_permuted.correlation) {
            best_permuted = candidate;
            std::copy(std::begin(permutation), std::end(permutation),
                      std::begin(best_permutation));
        }
    } while (std::next_permutation(std::begin(permutation),
                                   std::end(permutation)));

    std::fprintf(stderr,
        "[audio-block-mature] seq=%llu n=%zu delay=%llu reverb_write=%llu "
        "post_r=%.4f post_ratio=%.4f post_swap=%.4f post_lag=%d:%.4f "
        "post_perm=%d%d%d%d:%.4f dry_r=%.4f dry_ratio=%.4f "
        "dry_swap=%.4f dry_lag=%d:%.4f\n",
        static_cast<unsigned long long>(block_id), n,
        static_cast<unsigned long long>(warm_samples),
        static_cast<unsigned long long>(native_block_state_.front().reverb_write),
        metrics.correlation, metrics.level_ratio, swapped.correlation,
        aligned.second, aligned.first.correlation, best_permutation[0],
        best_permutation[1], best_permutation[2], best_permutation[3],
        best_permuted.correlation, dry.correlation, dry.level_ratio,
        dry_swapped.correlation, dry_aligned.second,
        dry_aligned.first.correlation);
    producer_diag_mature_logged_ = true;

    auto native_byte = [](float value) {
        return std::clamp(static_cast<int32_t>(std::lround(
            clampf(value, -1.0f, 127.0f / 128.0f) * 128.0f)), -128, 127);
    };
    for (std::size_t i = 0; i < n; ++i) {
        const int32_t native_post_a = native_byte(native_block_r_[i]);
        const int32_t native_post_b = native_byte(native_block_l_[i]);
        const int32_t native_dry_a_q = native_byte(native_dry_a[i]);
        const int32_t native_dry_b_q = native_byte(native_dry_b[i]);
        if (producer_a_[i] == native_post_a && producer_b_[i] == native_post_b &&
            dry_guest_a_[i] == native_dry_a_q &&
            dry_guest_b_[i] == native_dry_b_q) continue;
        const ProducerSampleState& state = native_block_state_[i];
        std::fprintf(stderr,
            "[audio-block-mature-diff] seq=%llu index=%zu "
            "guest_post=(%d,%d) raw=(%d,%d) native_post=(%d,%d) "
            "guest_dry=(%d,%d) native_dry=(%d,%d) "
            "q15_dry=(%d,%d) q15_old=(%d,%d) q15_post=(%d,%d) "
            "old_addr=0x%08x guest_old=(0x%08x,0x%08x) "
            "native_old=(0x%08x,0x%08x) "
            "voice=%u sample=%d gain_q9=(%d,%d) pos=%u frac=0x%06x "
            "step=0x%08x\n",
            static_cast<unsigned long long>(block_id), i,
            static_cast<int>(producer_a_[i]), static_cast<int>(producer_b_[i]),
            static_cast<int>(producer_raw_a_[i]),
            static_cast<int>(producer_raw_b_[i]), native_post_a, native_post_b,
            static_cast<int>(dry_guest_a_[i]),
            static_cast<int>(dry_guest_b_[i]), native_dry_a_q, native_dry_b_q,
            state.dry_right_q15, state.dry_left_q15, state.old_right_q15,
            state.old_left_q15, state.post_right_q15, state.post_left_q15,
            state.old_route_addr, state.old_route_a, state.old_route_b,
            state.native_old_route_a, state.native_old_route_b,
            state.first_voice, state.first_sample, state.first_gain_right_q9,
            state.first_gain_left_q9, state.first_pos, state.first_frac,
            state.first_step);
        producer_diag_mature_diff_logged_ = true;
        break;
    }
}

void Mp2kShadow::producer_dma_consume(uint8_t route, uint32_t source_addr,
                                      uint64_t host_cursor,
                                      uint64_t cycles) {
    if (canonical_fallback_) return;
    ProducerDmaRelease release{};
    if (!dma_timeline_.consume(route, source_addr, host_cursor, release))
        return;
    const auto found = std::find_if(
        published_native_.begin(), published_native_.end(),
        [&](const PublishedNativeBlock& block) {
            return block.sequence == release.sequence;
        });
    if (found == published_native_.end()) return;
    host_stream_.push(found->sequence, release.start_host_q32,
                      pcm_freq_, render_rate_, found->route_a.data(),
                      found->route_b.data(), found->route_a.size());
    if (audio_probe_enabled_) {
        std::fprintf(stderr,
            "[audio-producer-release] seq=%llu route=%u src=0x%08x "
            "host=%llu start_q32=0x%016llx cycles=%llu\n",
            static_cast<unsigned long long>(release.sequence), route,
            source_addr, static_cast<unsigned long long>(host_cursor),
            static_cast<unsigned long long>(release.start_host_q32),
            static_cast<unsigned long long>(cycles));
    }
}

void Mp2kShadow::producer_interleaved_block(const MemView& mem, uint32_t addr,
                                            uint32_t frames,
                                            uint64_t block_id) {
    if (canonical_fallback_) return;
    if (frames == 0) return;
    if (block_id != producer_block_id_) {
        if (producer_block_id_ != UINT64_MAX)
            canonical_fail("producer PCM write outside active block");
        return;
    }
    const uint8_t* p = mem.slice(addr, static_cast<std::size_t>(frames) * 4u);
    if (!p) return;
    const std::size_t n = std::min<std::size_t>(frames, producer_samples_);
    producer_a_.resize(n);
    producer_b_.resize(n);
    producer_raw_a_.resize(n);
    producer_raw_b_.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        const uint32_t packed = load_u32le(p + i * 4u);
        producer_raw_a_[i] = static_cast<int16_t>(packed & 0xFFFFu);
        producer_raw_b_[i] = static_cast<int16_t>(packed >> 16);
        mp2k_decode_producer_word(packed, producer_a_[i], producer_b_[i]);
    }
    producer_route_addr_[0] = addr;
    producer_route_addr_[1] = addr;
    judge_producer_block(block_id);
    rolling_seed_words_.resize(n);
    for (std::size_t i = 0; i < n; ++i)
        rolling_seed_words_[i] = load_u32le(p + i * 4u);
}

void Mp2kShadow::diagnostic_dry_block(const MemView& mem, uint32_t addr,
                                      uint32_t bytes, uint8_t route,
                                      uint64_t block_id) {
    if (canonical_fallback_) return;
    if (route > 1 || bytes == 0) return;
    if (block_id != producer_block_id_) {
        if (producer_block_id_ != UINT64_MAX)
            canonical_fail("dry write outside active block");
        return;
    }
    const uint8_t* p = mem.slice(addr, bytes);
    if (!p) return;
    const std::size_t n = std::min<std::size_t>(bytes, producer_samples_);
    std::vector<int8_t>& dst = route == 0 ? dry_guest_a_ : dry_guest_b_;
    dst.assign(reinterpret_cast<const int8_t*>(p),
               reinterpret_cast<const int8_t*>(p) + n);
    dry_guest_block_id_[route] = block_id;
    reverb_block_start_[route] = addr;
    const std::size_t groups = n / 4u;
    guest_reverb_new_words_[route].resize(groups);
    for (std::size_t group = 0; group < groups; ++group) {
        guest_reverb_new_words_[route][group] = load_u32le(p + group * 4u);
    }
}

bool Mp2kShadow::render(const MemView& mem, uint64_t audio_cursor,
                        float& route_a, float& route_b) {
    if (canonical_fallback_) {
        route_a = route_b = 0.0f;
        return false;
    }
    if (env_pos_ != 0xFFFFFFFFu) ++env_pos_;

    // Voice stepping remains on the host grid, but the audible/check stream
    // below comes only from sequence-matched integer producer blocks.
    const bool block_start = producer_block_first_render_;
    uint32_t pre_first_voice = UINT32_MAX;
    uint32_t pre_first_pos = 0;
    uint32_t pre_first_frac = 0;
    uint32_t pre_first_step = 0;
    for (Voice& v : voices_) {
        if (!v.on) continue;
        if (pre_first_voice == UINT32_MAX) {
            pre_first_voice = static_cast<uint32_t>(&v - voices_.data());
            pre_first_pos = v.pos_index;
            pre_first_frac = v.pos_frac;
            pre_first_step = v.step_q23;
        }
        float s = sample_voice(v, mem);
        if (mp2k_check_hold_refresh(block_start, mix_step_, v.chk_acc)) {
            v.chk_hold = s;
            v.chk_sample_x2 = v.last_sample_x2;
        }
    }
    producer_block_first_render_ = false;

    auto from_q15 = [](int32_t value) {
        return static_cast<float>(value) / 32768.0f;
    };
    ++reverb_write_count_;  // diagnostic host-grid timestamp only

    // Downsample the native check copy at the guest producer cadence. The
    // next PreMix snapshot resets this buffer, so PostMix can compare the
    // corresponding A/B block without guessing FIFO delay.
    producer_phase_ -= 1.0;
    if (producer_phase_ <= 0.0) {
        float check_dry_r = 0.0f;
        float check_dry_l = 0.0f;
        int32_t check_dry_r_q15 = 0;
        int32_t check_dry_l_q15 = 0;
        const std::size_t sample_index = native_block_r_.size();
        if (capture_voice_traces_enabled())
            producer_voice_traces_.fill(ProducerVoiceTrace{});
        if (sample_index < current_seed_words_.size()) {
            uint32_t accumulator = current_seed_words_[sample_index];
            std::size_t voice_index = 0;
            for (const Voice& voice : voices_) {
                const std::size_t this_voice = voice_index++;
                if (!voice.on) continue;
                // Defensive: this_voice tracks the voice's fixed slot in
                // voices_ (size kMp2kMaxChans). Guard future array changes so
                // a resize mismatch fails safe instead of writing OOB.
                if (this_voice >= kMp2kMaxChans) continue;
                const uint32_t gain_r = voice.g0r_q9;
                const uint32_t gain_l = voice.g0l_q9;
                const bool fix_pcm = voice.packed_gain_fix;
                const uint32_t packed_gain = voice.packed_gain_g0;
                const int32_t sample = voice.packed_gain_ordinary_pcm
                    ? voice.chk_sample_x2
                    : static_cast<int32_t>(std::lround(
                        voice.chk_hold * 128.0f)) * (fix_pcm ? 1 : 2);
                const uint32_t before = accumulator;
                accumulator = mp2k_packed_accumulate(
                    accumulator, packed_gain, sample);
                if (capture_voice_traces_enabled()) {
                    auto& trace = producer_voice_traces_[this_voice];
                    trace.active = true;
                    // Probe-only identity fields: fixed-size, no growth,
                    // and no effect on rendering/verification/gain/DMA.
                    trace.channel = static_cast<uint32_t>(this_voice);
                    trace.source_kind = voice.synth_kind != 0 ? 5u :
                        (voice.reversed ? 4u :
                         (voice.compressed ? 3u :
                          ((voice.ctype & 0x08u) != 0 ? 2u : 1u)));
                    trace.ctype = voice.ctype;
                    trace.wave = voice.wav;
                    trace.resolved_wave =
                        mp2k_resolve_wave_address(mem, voice.wav, 16);
                    trace.cursor = voice.pos_index;
                    trace.frac = voice.pos_frac;
                    trace.step = voice.step_q23;
                    trace.sample = sample;
                    trace.gain_right_q9 = gain_r;
                    trace.gain_left_q9 = gain_l;
                    trace.packed_gain = packed_gain;
                    trace.accumulator_before = before;
                    trace.accumulator_after = accumulator;
                }
                if (audio_probe_enabled_ &&
                    producer_block_id_ == 5 && sample_index == 0) {
                    const char* path = voice.synth_kind != 0 ? "synth" :
                        (voice.reversed ? "reversed" :
                         (voice.compressed ? "compressed" :
                          ((voice.ctype & 0x08u) != 0 ? "FIX" : "PCM")));
                    std::fprintf(stderr,
                        "[audio-native-acc] seq=5 sample=0 voice=%zu path=%s "
                        "ctype=0x%02x cursor=%u frac=0x%06x step=0x%08x "
                        "hold=%d gain=(%u,%u) packed=0x%08x "
                        "before=0x%08x after=0x%08x\n",
                        this_voice, path, voice.ctype, voice.pos_index,
                        voice.pos_frac, voice.step_q23, sample, gain_r,
                        gain_l, packed_gain, before, accumulator);
                }
            }
            accumulator = mp2k_saturate_packed_lanes(accumulator);
            native_accumulator_words_.push_back(accumulator);
            int8_t dry_a = 0, dry_b = 0;
            mp2k_extract_dry_sample(accumulator, dry_a, dry_b);
            check_dry_r = static_cast<float>(dry_a) / 128.0f;
            check_dry_l = static_cast<float>(dry_b) / 128.0f;
            check_dry_r_q15 = static_cast<int32_t>(dry_a) * 256;
            check_dry_l_q15 = static_cast<int32_t>(dry_b) * 256;
        }
        // Placeholder until the matching guest history addresses arrive.
        // judge_producer_block replaces this with exact packed finalizer data.
        const int32_t check_post_r_q15 = check_dry_r_q15;
        const int32_t check_post_l_q15 = check_dry_l_q15;
        native_block_r_.push_back(from_q15(check_post_r_q15));
        native_block_l_.push_back(from_q15(check_post_l_q15));
        ProducerSampleState sample_state{};
        sample_state.dry_right = check_dry_r;
        sample_state.dry_left = check_dry_l;
        sample_state.old_right = 0.0f;
        sample_state.old_left = 0.0f;
        sample_state.dry_right_q15 = check_dry_r_q15;
        sample_state.dry_left_q15 = check_dry_l_q15;
        sample_state.old_right_q15 = 0;
        sample_state.old_left_q15 = 0;
        sample_state.post_right_q15 = check_post_r_q15;
        sample_state.post_left_q15 = check_post_l_q15;
        sample_state.first_pre_voice = pre_first_voice;
        sample_state.first_pre_pos = pre_first_pos;
        sample_state.first_pre_frac = pre_first_frac;
        sample_state.first_pre_step = pre_first_step;
        if (capture_voice_traces_enabled())
            native_block_voice_traces_.push_back(producer_voice_traces_);
        sample_state.reverb_write = reverb_write_count_;
        for (uint32_t i = 0; i < voices_.size(); ++i) {
            const Voice& voice = voices_[i];
            if (!voice.on) continue;
            ++sample_state.active_voices;
            if (sample_state.first_voice != UINT32_MAX) continue;
            sample_state.first_voice = i;
            sample_state.first_ctype = voice.ctype;
            sample_state.first_wave = voice.wav;
            sample_state.first_resolved_wave = voice.data;
            sample_state.first_pos = voice.pos_index;
            sample_state.first_frac = voice.pos_frac;
            sample_state.first_step = voice.step_q23;
            sample_state.first_gain_right = voice.g0r;
            sample_state.first_gain_left = voice.g0l;
            sample_state.first_sample = static_cast<int32_t>(
                std::lround(voice.chk_hold * 128.0f));
            sample_state.first_gain_right_q9 =
                static_cast<int32_t>(voice.g0r_q9);
            sample_state.first_gain_left_q9 =
                static_cast<int32_t>(voice.g0l_q9);
        }
        native_block_state_.push_back(sample_state);
        producer_phase_ += mix_step_;
        judge_producer_block(producer_block_id_);
    }

    // Public verification and audible output consume the same timestamped
    // integer producer stream. Route A is the guest right bus; route B is the
    // left bus. Before the delayed stream becomes available, canonical audio
    // remains the safety source and no false verifier failure is recorded.
    route_a = route_b = 0.0f;
    const bool candidate_ready =
        host_stream_.sample(audio_cursor, route_a, route_b);
    if (candidate_ready) saturating_increment(candidate_samples_ready_);
    else {
        saturating_increment(candidate_samples_missing_);
        if (!first_output_domain_diag_ready_) {
            saturating_increment(output_domain_accum_.missing);
            if (!output_domain_accum_.have_cursor) {
                output_domain_accum_.first_cursor = audio_cursor;
                output_domain_accum_.have_cursor = true;
            }
            output_domain_accum_.last_cursor = audio_cursor;
        }
    }
    return candidate_ready;
}

void Mp2kShadow::judge_output(uint64_t audio_cursor, float canon_left,
                              float canon_right, float candidate_left,
                              float candidate_right,
                              const Mp2kOutputRouteObservation& observation,
                              std::string& degraded) {
    const bool canon_zero = canon_left == 0.0f && canon_right == 0.0f;
    const bool native_zero = candidate_left == 0.0f && candidate_right == 0.0f;
    if (canon_zero) ++sample_stats_.canonical_zero;
    if (native_zero) ++sample_stats_.native_zero;
    if (have_previous_verifier_sample_) {
        const bool canon_repeat = canon_left == previous_canon_left_ &&
            canon_right == previous_canon_right_;
        const bool native_repeat = candidate_left == previous_native_left_ &&
            candidate_right == previous_native_right_;
        if (canon_repeat) {
            ++sample_stats_.canonical_held;
            ++sample_stats_.canonical_repeated;
        }
        if (native_repeat) {
            ++sample_stats_.native_held;
            ++sample_stats_.native_repeated;
        }
    }
    previous_canon_left_ = canon_left;
    previous_canon_right_ = canon_right;
    previous_native_left_ = candidate_left;
    previous_native_right_ = candidate_right;
    have_previous_verifier_sample_ = true;
    degraded.clear();
    if (!first_output_domain_diag_ready_) {
        auto observe = [](double value, double& sum, double& sum_sq,
                          double& peak, uint64_t& nonnegative,
                          uint64_t& negative) {
            sum += value;
            sum_sq += value * value;
            peak = std::max(peak, std::fabs(value));
            if (value >= 0.0) ++nonnegative;
            else ++negative;
        };
        if (!output_domain_accum_.have_cursor) {
            output_domain_accum_.first_cursor = audio_cursor;
            output_domain_accum_.have_cursor = true;
        }
        output_domain_accum_.last_cursor = audio_cursor;
        observe(canon_left, output_domain_accum_.canonical_sum[0],
                output_domain_accum_.canonical_sum_sq[0],
                output_domain_accum_.canonical_peak[0],
                output_domain_accum_.canonical_nonnegative[0],
                output_domain_accum_.canonical_negative[0]);
        observe(canon_right, output_domain_accum_.canonical_sum[1],
                output_domain_accum_.canonical_sum_sq[1],
                output_domain_accum_.canonical_peak[1],
                output_domain_accum_.canonical_nonnegative[1],
                output_domain_accum_.canonical_negative[1]);
        observe(candidate_left, output_domain_accum_.native_sum[0],
                output_domain_accum_.native_sum_sq[0],
                output_domain_accum_.native_peak[0],
                output_domain_accum_.native_nonnegative[0],
                output_domain_accum_.native_negative[0]);
        observe(candidate_right, output_domain_accum_.native_sum[1],
                output_domain_accum_.native_sum_sq[1],
                output_domain_accum_.native_peak[1],
                output_domain_accum_.native_nonnegative[1],
                output_domain_accum_.native_negative[1]);
        observe(observation.route_a, output_domain_accum_.route_sum[0],
                output_domain_accum_.route_sum_sq[0],
                output_domain_accum_.route_peak[0],
                output_domain_accum_.route_nonnegative[0],
                output_domain_accum_.route_negative[0]);
        observe(observation.route_b, output_domain_accum_.route_sum[1],
                output_domain_accum_.route_sum_sq[1],
                output_domain_accum_.route_peak[1],
                output_domain_accum_.route_nonnegative[1],
                output_domain_accum_.route_negative[1]);
        observe(observation.fifo_a, output_domain_accum_.fifo_sum[0],
                output_domain_accum_.fifo_sum_sq[0],
                output_domain_accum_.fifo_peak[0],
                output_domain_accum_.fifo_nonnegative[0],
                output_domain_accum_.fifo_negative[0]);
        observe(observation.fifo_b, output_domain_accum_.fifo_sum[1],
                output_domain_accum_.fifo_sum_sq[1],
                output_domain_accum_.fifo_peak[1],
                output_domain_accum_.fifo_nonnegative[1],
                output_domain_accum_.fifo_negative[1]);
        if (output_domain_accum_.samples == 0) {
            output_domain_accum_.route_mask = observation.route_mask;
            output_domain_accum_.full_volume_mask =
                observation.full_volume_mask;
            output_domain_accum_.psg_left = observation.psg_left;
            output_domain_accum_.psg_right = observation.psg_right;
            output_domain_accum_.soundbias = observation.soundbias;
        } else if (output_domain_accum_.route_mask != observation.route_mask ||
                   output_domain_accum_.full_volume_mask !=
                       observation.full_volume_mask ||
                   output_domain_accum_.psg_left != observation.psg_left ||
                   output_domain_accum_.psg_right != observation.psg_right ||
                   output_domain_accum_.soundbias != observation.soundbias) {
            saturating_increment(output_domain_accum_.route_mapping_changes);
        }
        saturating_increment(output_domain_accum_.samples);
    }
    saturating_increment(verifier_samples_);
    Judgement j = Judgement::None;
    j = vf_.judge(canon_left, canon_right, candidate_left, candidate_right);
    // A single successful envelope window is actionable evidence only after
    // a signed producer block has passed and been published.  Keep this
    // explicit latch independent of vf_.proven(): the next signed block can
    // fail before the second adjacent window establishes full proof.
    if (j == Judgement::Pass && published_block_id_ != UINT64_MAX &&
        vf_.signed_gate_passed()) {
        producer_good_probation_window_seen_ = true;
    }
    if (j != Judgement::None) {
        saturating_increment(verifier_windows_);
        if (j == Judgement::Fail && !first_output_domain_diag_ready_) {
            first_output_domain_diag_.valid = true;
            first_output_domain_diag_.verifier_window = verifier_windows_;
            first_output_domain_diag_.samples = output_domain_accum_.samples;
            first_output_domain_diag_.missing = output_domain_accum_.missing;
            first_output_domain_diag_.missing_total = candidate_samples_missing_;
            first_output_domain_diag_.first_cursor =
                output_domain_accum_.first_cursor;
            first_output_domain_diag_.last_cursor =
                output_domain_accum_.last_cursor;
            first_output_domain_diag_.producer_block = producer_block_id_;
            first_output_domain_diag_.published_block = published_block_id_;
            first_output_domain_diag_.correlation = vf_.last_r();
            first_output_domain_diag_.level_ratio = vf_.last_ratio();
            first_output_domain_diag_.canonical_sum =
                output_domain_accum_.canonical_sum;
            first_output_domain_diag_.canonical_sum_sq =
                output_domain_accum_.canonical_sum_sq;
            first_output_domain_diag_.canonical_peak =
                output_domain_accum_.canonical_peak;
            first_output_domain_diag_.canonical_nonnegative =
                output_domain_accum_.canonical_nonnegative;
            first_output_domain_diag_.canonical_negative =
                output_domain_accum_.canonical_negative;
            first_output_domain_diag_.native_sum =
                output_domain_accum_.native_sum;
            first_output_domain_diag_.native_sum_sq =
                output_domain_accum_.native_sum_sq;
            first_output_domain_diag_.native_peak =
                output_domain_accum_.native_peak;
            first_output_domain_diag_.native_nonnegative =
                output_domain_accum_.native_nonnegative;
            first_output_domain_diag_.native_negative =
                output_domain_accum_.native_negative;
            first_output_domain_diag_.route_sum =
                output_domain_accum_.route_sum;
            first_output_domain_diag_.route_sum_sq =
                output_domain_accum_.route_sum_sq;
            first_output_domain_diag_.route_peak =
                output_domain_accum_.route_peak;
            first_output_domain_diag_.route_nonnegative =
                output_domain_accum_.route_nonnegative;
            first_output_domain_diag_.route_negative =
                output_domain_accum_.route_negative;
            first_output_domain_diag_.fifo_sum = output_domain_accum_.fifo_sum;
            first_output_domain_diag_.fifo_sum_sq =
                output_domain_accum_.fifo_sum_sq;
            first_output_domain_diag_.fifo_peak = output_domain_accum_.fifo_peak;
            first_output_domain_diag_.fifo_nonnegative =
                output_domain_accum_.fifo_nonnegative;
            first_output_domain_diag_.fifo_negative =
                output_domain_accum_.fifo_negative;
            first_output_domain_diag_.route_mask =
                output_domain_accum_.route_mask;
            first_output_domain_diag_.full_volume_mask =
                output_domain_accum_.full_volume_mask;
            first_output_domain_diag_.route_mapping_changes =
                output_domain_accum_.route_mapping_changes;
            first_output_domain_diag_.psg_left = output_domain_accum_.psg_left;
            first_output_domain_diag_.psg_right = output_domain_accum_.psg_right;
            first_output_domain_diag_.soundbias = output_domain_accum_.soundbias;
            first_output_domain_diag_.resampler =
                host_stream_.diagnostic(audio_cursor);
            first_output_domain_diag_ready_ = true;
        }
        output_domain_accum_ = {};
    }
    if (audio_probe_enabled_ && debug_samples_ < 64) {
        std::fprintf(stderr,
                     "[audio-probe] hook=%llu cursor=%llu canon=(%.3f,%.3f) "
                     "producer=(%.3f,%.3f)\n",
                     static_cast<unsigned long long>(hooks_),
                     static_cast<unsigned long long>(audio_cursor),
                     canon_left, canon_right,
                     candidate_left, candidate_right);
        ++debug_samples_;
    }
    if (j == Judgement::Fail && vf_.last_fail_structural() && !vf_.proven()) {
        ++mode_dwell_;
    } else if (j == Judgement::Pass) {
        mode_dwell_ = 0;
    }
    std::string r = vf_.take_reverted();
    if (!r.empty()) degraded = std::move(r);

}

bool Mp2kShadow::take_first_output_domain_diagnostic(
    Mp2kOutputDomainDiagnostic& out) {
    if (!first_output_domain_diag_ready_ ||
        first_output_domain_diag_consumed_)
        return false;
    out = first_output_domain_diag_;
    first_output_domain_diag_consumed_ = true;
    return true;
}

bool Mp2kShadow::note_on(Voice& v, const MemView& mem, uint8_t ctype,
                         uint32_t count, uint32_t wav) {
    (void)count;  // SoundChannel::ct is not a PCM sample index.
    const uint32_t wav_addr = mp2k_resolve_wave_address(mem, wav, 16);
    const uint8_t* h = wav_addr ? mem.slice(wav_addr, 16) : nullptr;
    // A note/source change invalidates all cached source and packed-gain
    // state. ROM payload pointers are rebound below; RAM stays live-read.
    v.immutable_sample_ptr = nullptr;
    v.immutable_sample_bytes = 0;
    v.g0r_q9 = v.g0l_q9 = v.g1r_q9 = v.g1l_q9 = 0;
    v.packed_gain_g0 = 0;
    v.packed_gain_fix = false;
    v.packed_gain_ordinary_pcm = false;
    if (audio_probe_enabled_ && wav < 0x10000u) {
        static uint32_t probe_wave_count = 0;
        if (probe_wave_count++ < 8) {
            auto report = [&](uint32_t candidate, const char* name) {
                const uint8_t* p = mem.slice(candidate, 16);
                if (!p) return;
                std::fprintf(stderr,
                             "[audio-wave] wav=0x%08x %s=0x%08x tag1=%u loop=%u size=%u selected=%s\n",
                             wav, name, candidate, static_cast<unsigned>(p[1]),
                             load_u32le(p + 8), load_u32le(p + 12),
                             candidate == wav_addr ? "yes" : "no");
            };
            report(0x08000000u | wav, "rom");
            report(0x03000000u | wav, "iwram");
        }
    }
    if (!h) return false;
    uint16_t flags = static_cast<uint16_t>(h[2] | (h[3] << 8));
    uint32_t loop_start = load_u32le(h + 8);
    uint32_t size = load_u32le(h + 12);
    v.synth_kind = 0;
    v.synth_base = v.synth_step = v.synth_depth = v.synth_init_duty = 0;
    v.synth_phase = 0.0;
    v.synth_pos = 0;
    if (size == 0 && loop_start == 0) {
        // Camelot's synthetic instruments have no PCM payload. Their
        // selector and PWM parameters live in the five-byte payload after
        // the 16-byte archive header, not in header bytes 1..5.
        Mp2kSynthParams synth{};
        const uint8_t* payload = mem.slice(wav_addr + 16, 5);
        if (!mp2k_decode_synth_payload(payload, 5, synth)) return false;
        v.synth_kind = synth.kind;
        v.synth_base = synth.base;
        v.synth_step = synth.step;
        v.synth_depth = synth.depth;
        v.synth_init_duty = synth.initial_duty;
        v.synth_duty_pos = 0;
        v.synth_duty = 0.0f;
        v.synth_duty_step = 0.0f;
        v.ctype = ctype;
        v.wav = wav;
        v.data = wav_addr;
        v.size = v.loop_start = 0;
        v.looped = false;
        v.compressed = false;
        v.reversed = false;
        v.pos_index = 0;
        v.pos_frac = 0;
        v.step_q23 = 0;
        // Fixed-rate Camelot voices expose their position as a countdown in
        // ct rather than as the cp pointer used by ordinary PCM voices.
        v.pos_index = ((ctype & 0x08u) && count <= size) ? size - count : 0;
        v.pos_frac = 0;
        v.pos = static_cast<double>(v.pos_index);
        v.blk_idx = 0xFFFFFFFFu;
        return true;
    }
    if (size == 0 || size > 0x01000000u || loop_start > size) return false;
    bool compressed = (ctype & 0x20u) != 0;
    std::size_t bytes = compressed
        ? static_cast<std::size_t>((static_cast<uint64_t>(size) * 33 + 63) / 64)
        : static_cast<std::size_t>(size);
    if (!mem.slice(wav_addr + 16, bytes)) return false;
    v.data = wav_addr + 16;
    v.size = size;
    v.loop_start = loop_start;
    v.looped = (flags & 0xC000u) != 0;
    v.compressed = compressed;
    v.reversed = (ctype & 0x10u) != 0;
    if (v.reversed && compressed) return false;
    v.ctype = ctype;
    v.wav = wav;
    const uint8_t region = static_cast<uint8_t>(wav_addr >> 24);
    if (region >= 0x08u && region <= 0x0Du) {
        v.immutable_sample_ptr = mem.slice(v.data, bytes);
        v.immutable_sample_bytes = bytes;
    }
    v.packed_gain_fix = (ctype & 0x08u) != 0;
    v.packed_gain_ordinary_pcm = !compressed && !v.reversed &&
        (ctype & 0x08u) == 0;
    v.pos_index = ((ctype & 0x08u) && count <= size) ? size - count : 0;
    v.pos_frac = 0;
    v.step_q23 = 0;
    v.pos = static_cast<double>(v.pos_index);
    v.blk_idx = 0xFFFFFFFFu;
    return true;
}

float Mp2kShadow::sample_voice(Voice& v, const MemView& mem) {
    if (v.synth_kind) {
        return mp2k_synth_render_sample(v.synth_kind, v.synth_phase,
                                         v.step, v.synth_pos, v.synth_duty,
                                         v.synth_duty_step);
    }
    if (v.pos_index >= v.size) {
        if (v.looped && v.size > v.loop_start) {
            const uint32_t span = v.size - v.loop_start;
            v.pos_index = v.loop_start +
                ((v.pos_index - v.loop_start) % span);
        } else {
            v.on = false;
            return 0.0f;  // one-shot exhausted; envelope ends the note
        }
    }
    const uint32_t i0 = v.pos_index;
    const uint32_t i1 = (i0 + 1 >= v.size)
        ? (v.looped ? v.loop_start : i0) : i0 + 1;
    const int32_t s0 = static_cast<int32_t>(fetch(v, mem, i0));
    const int32_t s1 = static_cast<int32_t>(fetch(v, mem, i1));
    v.last_sample_x2 = mp2k_interpolate_s8_x2(s0, s1, v.pos_frac);
    const int32_t sample = mp2k_interpolate_s8(s0, s1, v.pos_frac);
    if (!mp2k_advance_cursor(v.pos_index, v.pos_frac, v.step_q23,
                             v.size, v.loop_start, v.looped)) {
        // Keep the final sample in the current frame; subsequent frames are
        // silent until the guest envelope retires the channel.
        v.pos_index = v.size;
    }
    v.pos = static_cast<double>(v.pos_index) +
            static_cast<double>(v.pos_frac) / kMp2kFracOne;
    return static_cast<float>(sample) / 128.0f;
}

bool Mp2kShadow::export_wall_snapshot(
    const MemView& mem, uint64_t sequence, uint64_t guest_cursor_q32,
    Mp2kWallSnapshotInput& out, Mp2kWallExportMode mode) const {
    auto unsupported = [&](uint8_t reason) {
        ++wall_export_unsupported_;
        if (reason == 1) ++wall_export_not_verified_;
        if (reason == 2) ++wall_export_reverb_rejected_;
        if (reason == 3) ++wall_export_bad_asset_;
        if (reason == 4) ++wall_export_bad_sequence_;
        if (reason == 5) ++wall_export_bad_rate_;
        return false;
    };
    // A published block exists only after the post-judge verifier and DMA
    // cadence checks pass. Never expose a speculative PreMix voice image.
    if (sequence == UINT64_MAX) return unsupported(4);
    if (sequence != published_block_id_ ||
        producer_blocks_judged_ == 0 || !producer_alignment_.ready() ||
        !live())
        return unsupported(1);
    if (reverb_ != 0) return unsupported(2);
    if (render_rate_ != Mp2kWallMixer::kCanonicalRenderRate ||
        pcm_freq_ == 0) return unsupported(5);
    if (producer_samples_ == 0 ||
        producer_samples_ > Mp2kWallMixer::kMaxProducerFrames ||
        (mode == Mp2kWallExportMode::FullProducerSeed &&
         current_seed_words_.size() < producer_samples_))
        return unsupported(3);

    out = Mp2kWallSnapshotInput{};
    out.sequence = sequence;
    out.guest_cursor_q32 = guest_cursor_q32;
    out.render_rate = render_rate_;
    out.pcm_rate = pcm_freq_;
    out.producer_block_frames = mode == Mp2kWallExportMode::FullProducerSeed
        ? producer_samples_ : 0;
    out.gain_ramp_frames = std::min<uint32_t>(env_span_,
                                               Mp2kWallMixer::kMaxProducerFrames);
    out.route_a_gain = 1.0f;
    out.route_b_gain = 1.0f;
    if (mode == Mp2kWallExportMode::FullProducerSeed) {
        out.seed_word_count = producer_samples_;
        out.seed_valid = true;
        for (std::size_t i = 0; i < out.seed_word_count; ++i)
            out.seed_words[i] = current_seed_words_[i];
    }

    uint8_t next_asset = 0;
    for (std::size_t ch = 0; ch < voices_.size(); ++ch) {
        const Voice& source = voices_[ch];
        auto& voice = out.voices[ch];
        if (!source.on) continue;
        if (next_asset >= out.assets.size() || source.reversed ||
            !std::isfinite(source.step) || !std::isfinite(source.synth_phase))
            return unsupported(3);

        const uint8_t asset_index = next_asset++;
        auto& asset = out.assets[asset_index];
        asset.kind = source.synth_kind
            ? Mp2kWallAssetKind::Synth
            : (source.compressed ? Mp2kWallAssetKind::CompressedDpcm
                                 : Mp2kWallAssetKind::Pcm8);
        asset.synth.kind = source.synth_kind;
        asset.synth.base = source.synth_base;
        asset.synth.step = source.synth_step;
        asset.synth.depth = source.synth_depth;
        asset.synth.initial_duty = source.synth_init_duty;
        if (source.synth_kind) {
            asset.synth_payload = mem.slice(source.data + 16u, 5);
            asset.synth_payload_len = 5;
            if (!asset.synth_payload) return unsupported(3);
        } else {
            asset.sample_count = source.size;
            asset.loop_start = source.loop_start;
            asset.looped = source.looped;
            const uint64_t bytes = source.compressed
                ? ((static_cast<uint64_t>(source.size) + 63u) / 64u) * 33u
                : source.size;
            if (bytes > UINT32_MAX ||
                !(asset.bytes = mem.slice(source.data,
                                           static_cast<std::size_t>(bytes))))
                return unsupported(3);
            asset.byte_count = static_cast<uint32_t>(bytes);
        }

        voice.on = true;
        voice.ctype = source.ctype;
        voice.asset_index = asset_index;
        voice.route = Mp2kWallRoute::Both;
        voice.pos_index = source.pos_index;
        voice.pos_frac = source.pos_frac;
        voice.step_q23 = source.step_q23;
        voice.size = source.size;
        voice.loop_start = source.loop_start;
        voice.looped = source.looped;
        voice.compressed = source.compressed;
        voice.reversed = source.reversed;
        voice.g0r = source.g0r;
        voice.g0l = source.g0l;
        voice.g1r = source.g1r;
        voice.g1l = source.g1l;
        voice.synth_kind = source.synth_kind;
        voice.synth_base = source.synth_base;
        voice.synth_step = source.synth_step;
        voice.synth_depth = source.synth_depth;
        voice.synth_init_duty = source.synth_init_duty;
        voice.synth_duty_pos = source.synth_duty_pos;
        voice.synth_pos = source.synth_pos;
        voice.synth_phase = source.synth_phase;
        voice.synth_step_render = source.step;
        voice.synth_duty = source.synth_duty;
        voice.synth_duty_step = source.synth_duty_step;
    }
    out.asset_count = next_asset;
    ++wall_export_successes_;
    return true;
}

float Mp2kShadow::fetch(Voice& v, const MemView& mem, uint32_t idx) {
    if (!v.compressed) {
        if (v.immutable_sample_ptr && idx < v.immutable_sample_bytes)
            return static_cast<float>(static_cast<int8_t>(
                v.immutable_sample_ptr[idx]));
        uint8_t b = 0;
        return mem.u8(v.data + idx, b)
            ? static_cast<float>(static_cast<int8_t>(b)) : 0.0f;
    }
    // DPCM: 33-byte blocks of 64 samples; byte 0 is the s8 seed, then delta
    // nibbles (even k = high nibble, odd k = low; byte 1's high nibble unused).
    uint32_t blk = idx >> 6;
    if (blk != v.blk_idx) {
        const std::size_t block_offset = static_cast<std::size_t>(blk) * 33u;
        const uint8_t* b = nullptr;
        if (v.immutable_sample_ptr && block_offset + 33u <=
            v.immutable_sample_bytes) {
            b = v.immutable_sample_ptr + block_offset;
        } else {
            b = mem.slice(v.data + static_cast<uint32_t>(block_offset), 33);
        }
        if (!b) return 0.0f;
        int8_t cur = static_cast<int8_t>(b[0]);
        v.blk[0] = cur;
        for (int k = 1; k < 64; ++k) {
            uint8_t byte = b[1 + (k >> 1)];
            uint8_t nib = (k & 1) ? (byte & 0xF) : (byte >> 4);
            cur = static_cast<int8_t>(cur + kDpcmLut[nib]);
            v.blk[k] = cur;
        }
        v.blk_idx = blk;
    }
    return static_cast<float>(v.blk[idx & 63]);
}

}  // namespace gba
