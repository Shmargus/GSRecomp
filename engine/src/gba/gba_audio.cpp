// gba_audio.cpp — see gba_audio.h. SOUND1/2 (square), SOUND3 (wave RAM),
// SOUND4 (noise), and Direct Sound A/B FIFOs, mixed in mix_one_sample().

#include "gba_audio.h"
#include "env_flag.h"

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

namespace gba {

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
        // Per-channel x8 scale; see mix_psg_samples in gba_audio.h.
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

}  // namespace

GbaAudio::GbaAudio() {
    ring_.assign(kRingSize, 0);
    stereo_fallback_ring_.assign(kRingSize * 2, 0);
    cap_ring_.assign(kCapRingSize, CapSample{});
    reset();
}
GbaAudio::~GbaAudio() {}

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
    clear_stereo_fallback();
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
    stereo_fallback_head_ = stereo_fallback_tail_ = 0;
    std::fill(stereo_fallback_ring_.begin(), stereo_fallback_ring_.end(), int16_t{0});
    canonical_stereo_requested_ = false;
}

void GbaAudio::write_io8(uint32_t off, uint8_t v, uint64_t cycles) {
    write_io8_impl(off, v);
}

void GbaAudio::write_io8_impl(uint32_t off, uint8_t v) {
    // Each 8-bit write is handled as a partial update: only the byte
    // changed counts. The bus has the canonical store; we just react.
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
        // SOUND A/B DMA controls (low byte bits 0..3 here, high byte below).
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
    write_io8_impl(off,     static_cast<uint8_t>(v & 0xFF));
    write_io8_impl(off + 1, static_cast<uint8_t>((v >> 8) & 0xFF));
}

void GbaAudio::write_io32(uint32_t off, uint32_t v, uint64_t cycles) {
    if (off == 0x0A0) {
        fifo_push_word(fifo_a_, 0, v);
        return;
    }
    if (off == 0x0A4) {
        fifo_push_word(fifo_b_, 1, v);
        return;
    }
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
        fifo.shift_word = fifo.words[fifo.read];
        fifo.read = static_cast<uint8_t>((fifo.read + 1) & 7u);
        --fifo.count;
        fifo.bytes_remaining = 4;
    }
    int8_t sample = static_cast<int8_t>(fifo.shift_word & 0xFFu);
    uint32_t slots = samples_per_event();
    uint32_t until_cycles = cycles_until_next_sample_event();
    uint32_t until = ((until_cycles - 1u) + cycles_per_sample_) /
        cycles_per_sample_;
    if (until > slots) until = slots;
    uint32_t start = slots - until;
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
    if (a_enabled && timer_id == (direct_a_timer1_ ? 1 : 0)) {
        fifo_timer_step(fifo_a_, 0, cycles);
    }
    if (b_enabled && timer_id == (direct_b_timer1_ ? 1 : 0)) {
        fifo_timer_step(fifo_b_, 1, cycles);
    }
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
    // bits are applied per side in mix_psg_samples below.
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
        // Envelope clock: 1/64 second per step = sample_rate()/64
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
        // Length clock: 256 Hz = sample_rate()/256 sample-ticks.
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
    // mix_psg_samples() applies the shared x8 PSG scale (see gba_audio.h).
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

void GbaAudio::configure_stereo() {
    // Opt-in canonical stereo output: GBARECOMP_AUDIO_STEREO (env_flag.h).
    // Called by the bus from set_rom.
    const bool stereo_want = gbarecomp::env_flag("GBARECOMP_AUDIO_STEREO");
    canonical_stereo_requested_ = stereo_want;
}

void GbaAudio::run_sample_event() {
    uint32_t samples = samples_per_event();
    CapSample caps[kMaxSamplesPerEvent] = {};
    while (sample_index_ < samples) {
        uint32_t slot = sample_index_;

        // Generate the canonical hardware mix once.
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

        const int16_t output_s = canonical_s;
        if (stereo_audio_requested()) {
            stereo_fallback_push(tap.canonical_left, tap.canonical_right);
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
