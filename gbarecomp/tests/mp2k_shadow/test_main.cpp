#include "mp2k_shadow.h"
#include "gba_audio.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace gba {

// Synthetic seam for the producer-boundary regression below. It seeds the
// already-captured guest/native prefix; no ROM bytes are involved.
struct Mp2kShadowWallFixture {
    static void seed_incomplete(Mp2kShadow& shadow,
                                uint32_t samples = 16,
                                uint32_t native_samples = 8) {
        const uint32_t packed = mp2k_saturate_packed_lanes(
            mp2k_packed_accumulate(0, mp2k_pack_stereo_gain(4, 4), 64));
        int8_t route_a = 0, route_b = 0;
        mp2k_extract_dry_sample(packed, route_a, route_b);

        shadow.producer_block_id_ = 1;
        shadow.last_premix_id_ = 1;
        shadow.producer_samples_ = samples;
        shadow.pcm_freq_ = shadow.render_rate_ = 65536;
        shadow.spv_ = samples;
        shadow.mix_step_ = 1.0;
        shadow.producer_phase_ = 0.0;
        shadow.producer_block_first_render_ = false;
        shadow.producer_block_incomplete_reported_ = false;
        shadow.producer_block_start_cursor_ = 0;
        shadow.current_seed_words_.assign(samples, packed);
        shadow.producer_a_.assign(samples, route_a);
        shadow.producer_b_.assign(samples, route_b);
        shadow.native_block_l_.assign(
            native_samples, static_cast<float>(route_b) / 128.0f);
        shadow.native_block_r_.assign(
            native_samples, static_cast<float>(route_a) / 128.0f);
        shadow.native_block_state_.resize(native_samples);
        shadow.reverb_block_start_[0] = 0x02001000u;
        shadow.reverb_block_start_[1] = 0x02002000u;
        shadow.producer_alignment_.reset();
    }

    static uint64_t active_block(const Mp2kShadow& shadow) {
        return shadow.producer_block_id_;
    }

    static std::size_t guest_samples(const Mp2kShadow& shadow) {
        return shadow.producer_a_.size();
    }

    static std::size_t native_samples(const Mp2kShadow& shadow) {
        return shadow.native_block_l_.size();
    }

    static void seed_native_prefix(Mp2kShadow& shadow, std::size_t samples) {
        shadow.native_block_l_.assign(samples, 0.0f);
        shadow.native_block_r_.assign(samples, 0.0f);
    }

    static bool startup_realign_pending(const Mp2kShadow& shadow) {
        return shadow.startup_realign_pending_;
    }

    static void seed_complete(Mp2kShadow& shadow) {
        const std::size_t samples = shadow.producer_samples_;
        shadow.producer_a_.assign(samples, 8);
        shadow.producer_b_.assign(samples, -8);
        shadow.native_block_r_.assign(samples, 8.0f / 128.0f);
        shadow.native_block_l_.assign(samples, -8.0f / 128.0f);
        shadow.native_block_state_.resize(samples);
        shadow.reverb_block_start_[0] = 0x02001000u;
        shadow.reverb_block_start_[1] = 0x02002000u;
    }

    static std::size_t history_words(const Mp2kShadow& shadow, uint8_t route) {
        return shadow.guest_reverb_old_words_[route].size();
    }

    static void judge(Mp2kShadow& shadow) {
        shadow.judge_producer_block(shadow.producer_block_id_);
    }

    static void mark_established(Mp2kShadow& shadow) {
        shadow.producer_blocks_judged_ = 1;
    }

    static void set_signed_gate(Mp2kShadow& shadow) {
        shadow.vf_.set_signed_producer_gate(true, true, 1.0f, 1.0f);
    }

    static void seed_source_diagnostic(Mp2kShadow& shadow) {
        shadow.producer_block_id_ = 42;
        shadow.producer_samples_ = 16;
        shadow.producer_route_addr_[0] = 0x03000DF8u;
        shadow.reverb_block_start_[0] = 0x02003500u;
        shadow.reverb_block_start_[1] = 0x02003B30u;
        shadow.dry_guest_block_id_[0] = 42;
        shadow.dry_guest_block_id_[1] = 42;
        shadow.producer_a_.resize(16);
        shadow.producer_b_.resize(16);
        shadow.dry_guest_a_.resize(16);
        shadow.dry_guest_b_.resize(16);
        shadow.native_block_r_.resize(16);
        shadow.native_block_l_.resize(16);
        shadow.native_block_state_.resize(16);
        for (std::size_t i = 0; i < 16; ++i) {
            const int8_t c70_a = static_cast<int8_t>(i & 1 ? -32 : 32);
            const int8_t c70_b = static_cast<int8_t>(i & 1 ? 24 : -24);
            shadow.producer_a_[i] = c70_a;
            shadow.producer_b_[i] = c70_b;
            shadow.dry_guest_a_[i] = static_cast<int8_t>(-c70_a);
            shadow.dry_guest_b_[i] = static_cast<int8_t>(-c70_b);
            shadow.native_block_r_[i] =
                static_cast<float>(shadow.dry_guest_a_[i]) / 128.0f;
            shadow.native_block_l_[i] =
                static_cast<float>(shadow.dry_guest_b_[i]) / 128.0f;
            shadow.native_block_state_[i].dry_right_q15 =
                static_cast<int32_t>(shadow.dry_guest_a_[i]) * 256;
            shadow.native_block_state_[i].dry_left_q15 =
                static_cast<int32_t>(shadow.dry_guest_b_[i]) * 256;
        }
    }

    static void source_diagnostic(Mp2kShadow& shadow, bool accepted = true) {
        shadow.maybe_log_producer_source_diagnostic(accepted);
    }

    static void seed_dma_route_gate(Mp2kShadow& shadow) {
        constexpr std::size_t samples = 16;
        shadow.producer_block_id_ = 7;
        shadow.producer_samples_ = static_cast<uint32_t>(samples);
        shadow.spv_ = static_cast<uint32_t>(samples);
        shadow.pcm_freq_ = shadow.render_rate_ = 65536;
        shadow.producer_a_.assign(samples, 8);
        shadow.producer_b_.assign(samples, -8);
        shadow.producer_raw_a_.assign(samples, 0);
        shadow.producer_raw_b_.assign(samples, 0);
        shadow.dry_guest_a_.assign(samples, 32);
        shadow.dry_guest_b_.assign(samples, -24);
        shadow.dry_guest_block_id_ = {7, 7};
        shadow.reverb_block_start_[0] = 0x02003660u;
        shadow.reverb_block_start_[1] = 0x02003C90u;
        // Deliberately make the old C70/post-reverb candidate wrong. The
        // route-stage gate must use the paired native dry state below.
        shadow.native_block_r_.assign(samples, -8.0f / 128.0f);
        shadow.native_block_l_.assign(samples, 8.0f / 128.0f);
        shadow.native_block_state_.resize(samples);
        for (auto& state : shadow.native_block_state_) {
            state.dry_right_q15 = 32 * 256;
            state.dry_left_q15 = -24 * 256;
        }
        shadow.producer_alignment_.reset();
    }

    // Exercise one real envelope verdict, which is intentionally not enough
    // to make ShadowVerifier::proven() true (the verifier requires two
    // adjacent windows), then make the next signed producer block fail. This
    // keeps the post-probation diagnostic regression independent of assert().
    static void seed_post_probation_reject(Mp2kShadow& shadow) {
        seed_dma_route_gate(shadow);
        shadow.published_block_id_ = 6;
        shadow.vf_.set_signed_producer_gate(true, true, 1.0f, 1.0f);
        Mp2kOutputRouteObservation observation{};
        std::string degraded;
        constexpr uint32_t kSamples = 65536;
        for (uint32_t i = 0; i < kSamples; ++i) {
            const float phase = static_cast<float>(i % 8192u) / 8192.0f;
            const float left = std::sin(phase * 6.28318530718f) * 0.5f;
            const float right = std::cos(phase * 6.28318530718f) * 0.5f;
            shadow.judge_output(i, left, right, left, right, observation,
                                degraded);
        }
        std::fill(shadow.dry_guest_a_.begin(), shadow.dry_guest_a_.end(),
                  static_cast<int8_t>(-32));
        shadow.judge_producer_block(shadow.producer_block_id_);
    }

    // Same bounded reject, but with a synthetic guest snapshot and a looping
    // cursor. The mismatch begins at index 3 so the diagnostic must compare
    // the snapshot seed against the projected per-sample phase, not merely
    // report the first differing PCM value.
    static void seed_cursor_post_probation_reject(Mp2kShadow& shadow) {
        seed_dma_route_gate(shadow);
        shadow.pcm_freq_ = 21024;
        shadow.render_rate_ = 65536;
        shadow.mix_step_ = static_cast<double>(shadow.render_rate_) /
                           static_cast<double>(shadow.pcm_freq_);
        shadow.producer_snapshot_cursor_ = 900;
        shadow.producer_snapshot_block_ = 7;
        CanonicalMixTrace snapshot{};
        snapshot.valid = true;
        snapshot.sequence = 7;
        auto& voice = snapshot.voice_identities[0];
        voice.channel = 0;
        voice.ctype = 0;
        voice.frequency = 7884;
        voice.wave = 0x08000100u;
        voice.resolved_wave = 0x08000100u;
        voice.resolved_data = 0x08000110u;
        voice.sample_cursor = 0x08000117u; // source index 7
        voice.frac = 0x123u;
        voice.wave_loop = 4;
        voice.wave_size = 8;
        voice.looped = 1;
        snapshot.voice_identities[1] = {};
        shadow.capture_canonical_mix_trace(snapshot);

        uint32_t pos = 7;
        uint32_t frac = 0x123u;
        const uint32_t native_step = mp2k_step_q23(
            voice.frequency, shadow.render_rate_);
        double host_phase = 0.0;
        uint64_t host_cursor = 0;
        for (std::size_t i = 0; i < shadow.native_block_state_.size(); ++i) {
            auto& state = shadow.native_block_state_[i];
            state.first_voice = 0;
            state.first_pre_voice = 0;
            state.first_pre_pos = pos;
            state.first_pre_frac = frac;
            state.first_pre_step = native_step;
            state.first_sample = 0;
            state.dry_right_q15 = 32 * 256;
            state.dry_left_q15 = -24 * 256;
            do {
                ++host_cursor;
                mp2k_advance_cursor(pos, frac, native_step, 8, 4, true);
                host_phase -= 1.0;
            } while (host_phase > 0.0);
            state.reverb_write = host_cursor;
            state.first_pos = pos;
            state.first_frac = frac;
            state.first_step = native_step;
            host_phase += shadow.mix_step_;
        }
        shadow.native_accumulator_words_.resize(
            shadow.native_block_state_.size(), 0x01020304u);
        shadow.rolling_seed_words_.resize(
            shadow.native_block_state_.size(), 0x11121314u);
        shadow.current_seed_words_.resize(
            shadow.native_block_state_.size(), 0x21222324u);
        shadow.producer_raw_a_.resize(shadow.native_block_state_.size(), 0x12);
        shadow.producer_raw_b_.resize(shadow.native_block_state_.size(), -0x12);
        shadow.native_block_voice_traces_.resize(
            shadow.native_block_state_.size());
        auto& voice0 = shadow.native_block_voice_traces_[3][0];
        voice0.active = true;
        voice0.channel = 0;
        voice0.source_kind = 1;
        voice0.ctype = 0x00;
        voice0.sample = 4;
        voice0.gain_right_q9 = 8;
        voice0.gain_left_q9 = 8;
        voice0.packed_gain = 0x00080008u;
        voice0.accumulator_before = 0;
        voice0.accumulator_after = 0x00010001u;
        auto& voice1 = shadow.native_block_voice_traces_[3][1];
        voice1.active = true;
        voice1.channel = 1;
        voice1.source_kind = 2;
        voice1.ctype = 0x08;
        voice1.sample = -12;
        voice1.gain_right_q9 = 16;
        voice1.gain_left_q9 = 16;
        voice1.packed_gain = 0x00100010u;
        voice1.accumulator_before = 0;
        voice1.accumulator_after = 0x00400040u;
        auto& voice2 = shadow.native_block_voice_traces_[3][2];
        voice2.active = true;
        voice2.channel = 2;
        voice2.source_kind = 5;
        voice2.ctype = 0x20;
        voice2.sample = 2;
        voice2.gain_right_q9 = 4;
        voice2.gain_left_q9 = 4;
        voice2.packed_gain = 0x00040004u;
        voice2.accumulator_before = 0;
        voice2.accumulator_after = 0x00020002u;

        shadow.published_block_id_ = 6;
        shadow.vf_.set_signed_producer_gate(true, true, 1.0f, 1.0f);
        Mp2kOutputRouteObservation observation{};
        std::string degraded;
        constexpr uint32_t kSamples = 65536;
        for (uint32_t i = 0; i < kSamples; ++i) {
            const float phase = static_cast<float>(i % 8192u) / 8192.0f;
            const float left = std::sin(phase * 6.28318530718f) * 0.5f;
            const float right = std::cos(phase * 6.28318530718f) * 0.5f;
            shadow.judge_output(i, left, right, left, right, observation,
                                degraded);
        }
        for (std::size_t i = 3; i < shadow.dry_guest_a_.size(); ++i)
            shadow.dry_guest_a_[i] = -32;
        shadow.judge_producer_block(shadow.producer_block_id_);
    }

    static uint64_t verifier_windows(const Mp2kShadow& shadow) {
        return shadow.verifier_windows_;
    }

    static bool verifier_proven(const Mp2kShadow& shadow) {
        return shadow.vf_.proven();
    }

    static bool good_probation_window_seen(const Mp2kShadow& shadow) {
        return shadow.producer_good_probation_window_seen_;
    }

    static bool post_probation_reject_logged(const Mp2kShadow& shadow) {
        return shadow.producer_post_probation_reject_diag_logged_;
    }

    static const Mp2kVoiceContributionDiagnostic& voice_contribution_diag(
        const Mp2kShadow& shadow) {
        return shadow.producer_voice_diag_;
    }

    static std::size_t published_blocks(const Mp2kShadow& shadow) {
        return shadow.published_native_.size();
    }

    static float published_route_a(const Mp2kShadow& shadow) {
        return shadow.published_native_.front().route_a.front();
    }

    static float published_route_b(const Mp2kShadow& shadow) {
        return shadow.published_native_.front().route_b.front();
    }

};

}  // namespace gba

namespace {

[[noreturn]] void test_fail(const char* expression, const char* file,
                            int line) {
    std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", expression, file,
                 line);
    std::exit(1);
}

#define CHECK(expression) \
    do { \
        if (!(expression)) test_fail(#expression, __FILE__, __LINE__); \
    } while (false)

void set_audio_probe(bool enabled) {
#if defined(_WIN32)
    _putenv_s("GBARECOMP_AUDIO_PROBE", enabled ? "1" : "");
#else
    if (enabled) setenv("GBARECOMP_AUDIO_PROBE", "1", 1);
    else unsetenv("GBARECOMP_AUDIO_PROBE");
#endif
}

void set_shadow_request(bool enabled) {
#if defined(_WIN32)
    _putenv_s("GBARECOMP_AUDIO_SHADOW", enabled ? "1" : "");
#else
    if (enabled) setenv("GBARECOMP_AUDIO_SHADOW", "1", 1);
    else unsetenv("GBARECOMP_AUDIO_SHADOW");
#endif
}

}  // namespace

int main() {
    // Direct Sound routing is a hardware-domain operation: signed FIFO byte,
    // 50/100% scale, side enables, PSG sum, then SOUNDBIAS clipping and x48
    // conversion to the host signed-16 domain.
    gba::DirectSoundRouteState direct{};
    direct.a_right = true;
    direct.a_full_volume = true;
    direct.b_left = true;
    direct.b_full_volume = false;
    auto routed = gba::mix_direct_sound_output(
        64.0f / 128.0f, -64.0f / 128.0f, 0, 0, direct);
    CHECK(routed.left == -6144.0f);
    CHECK(routed.right == 12288.0f);
    direct.a_left = true;
    routed = gba::mix_direct_sound_output(
        16.0f / 128.0f, 0.0f, 10, -10, direct);
    CHECK(routed.left == (10 + 64) * 48.0f);
    CHECK(routed.right == (-10 + 64) * 48.0f);
    direct = {};
    direct.a_left = true;
    direct.a_full_volume = true;
    direct.soundbias = 0x0100;
    routed = gba::mix_direct_sound_output(-1.0f, 0.0f, 0, 0, direct);
    CHECK(routed.left == -0x100 * 48.0f);
    routed = gba::mix_direct_sound_output(
        127.0f / 128.0f, 0.0f, 600, 0, direct);
    CHECK(routed.left == 32767.0f);
    direct = {};
    routed = gba::mix_direct_sound_output(1.0f, -1.0f, 12, -12, direct);
    CHECK(routed.left == 12 * 48.0f && routed.right == -12 * 48.0f);

    // Cached MP2K observer gate: known writers always pass, dynamic
    // SoundInfo/channel/ring edges pass, and unrelated RAM is rejected.
    {
        std::vector<uint8_t> filter_rom(0x100, 0);
        std::vector<uint8_t> filter_ewram(0x100, 0);
        std::vector<uint8_t> filter_iwram(0x8000, 0);
        const uint32_t filter_si = 0x03001000u;
        const std::size_t filter_off = filter_si & 0x7FFFu;
        auto filter_put32 = [&](std::size_t off, uint32_t value) {
            filter_iwram[off + 0] = static_cast<uint8_t>(value);
            filter_iwram[off + 1] = static_cast<uint8_t>(value >> 8);
            filter_iwram[off + 2] = static_cast<uint8_t>(value >> 16);
            filter_iwram[off + 3] = static_cast<uint8_t>(value >> 24);
        };
        filter_put32(gba::kSoundInfoPtr & 0x7FFFu, filter_si);
        filter_put32(filter_off, gba::kMp2kMagicBase);
        filter_iwram[filter_off + 0x06] = gba::kMp2kMaxChans;
        filter_iwram[filter_off + 0x0B] = 7;
        filter_put32(filter_off + 0x10, 8);
        filter_put32(filter_off + 0x14, 13379);
        gba::GbaAudio filter_audio;
        filter_audio.configure_shadow(
            {gba::Mp2kSig{0, 0x08000101u}}, filter_rom.data(),
            filter_rom.size(), filter_ewram.data(), filter_ewram.size(),
            filter_iwram.data(), filter_iwram.size(), false, false);
        const uint32_t channels = filter_si + gba::kSoundChansOff;
        const uint32_t channels_end = channels +
            gba::kMp2kMaxChans * gba::kSoundChanStride;
        const uint32_t ring = filter_si + 0x350u;
        const uint32_t ring_end = ring + 8u * (7u + 2u) + 32u;
        const uint32_t ring_b = filter_si + 0x410u;
        const uint32_t ring_b_end = ring_b + 8u * (7u + 2u) + 32u;
        const auto overlaps = [](uint32_t addr, uint32_t width,
                                  uint32_t lo, uint32_t hi) {
            const uint64_t end = static_cast<uint64_t>(addr) + width;
            return width != 0 && lo < hi &&
                static_cast<uint64_t>(addr) < hi && end > lo;
        };
        const auto old_slow_predicate = [&](uint32_t pc, uint32_t addr,
                                            uint32_t width) {
            if (width == 0) return false;
            switch (pc & ~1u) {
                case 0x030008B4u:
                case 0x03000A8Cu:
                case 0x03000BCCu:
                case 0x03000BD4u:
                case 0x03000C70u:
                    return true;
                default:
                    break;
            }
            return overlaps(addr, width, gba::kSoundInfoPtr,
                            gba::kSoundInfoPtr + 4u) ||
                overlaps(addr, width, filter_si, filter_si + 0x50u) ||
                overlaps(addr, width, channels, channels_end) ||
                overlaps(addr, width, ring, ring_end) ||
                overlaps(addr, width, ring_b, ring_b_end);
        };
        struct FilterCase { uint32_t pc, addr, width; };
        const std::array<FilterCase, 19> parity_cases{{
            {0x030008B4u, 0x02000000u, 1},
            {0x03000A8Du, 0x02000000u, 4},
            {0x03000BCCu, 0x08000000u, 2},
            {0x03000BD5u, 0x08000000u, 1},
            {0x03000C70u, 0x0300FFFFu, 4},
            {0x08000001u, filter_si, 1},
            {0x08000001u, filter_si + 0x4Fu, 2},
            {0x08000001u, filter_si + 0x50u, 1},
            {0x08000001u, channels, 1},
            {0x08000001u, channels_end - 1u, 2},
            {0x08000001u, channels_end + 1u, 1},
            {0x08000001u, ring, 1},
            {0x08000001u, ring_end - 1u, 2},
            {0x08000001u, ring_end, 1},
            {0x08000001u, ring_b, 1},
            {0x08000001u, ring_b_end - 1u, 2},
            {0x08000001u, ring_b_end, 1},
            {0x08000001u, gba::kSoundInfoPtr + 3u, 2},
            {0x08000001u, 0x03005000u, 1},
        }};
        for (const auto& c : parity_cases) {
            CHECK(filter_audio.mp2k_write_may_be_relevant(
                c.pc, c.addr, c.width) ==
                old_slow_predicate(c.pc, c.addr, c.width));
        }
        for (const uint32_t pc : {0x030008B4u, 0x03000A8Cu,
                                  0x03000BCCu, 0x03000BD4u,
                                  0x03000C70u}) {
            CHECK(filter_audio.mp2k_write_may_be_relevant(
                pc, 0x02000000u, 1));
            CHECK(filter_audio.mp2k_write_may_be_relevant(
                pc | 1u, 0x02000000u, 4));
        }
        // SoundInfo, channels, and both producer rings use half-open ranges.
        CHECK(filter_audio.mp2k_write_may_be_relevant(
            0x08000001u, filter_si, 1));
        CHECK(filter_audio.mp2k_write_may_be_relevant(
            0x08000001u, filter_si + 0x4Fu, 1));
        // Channel zero begins immediately after the SoundInfo header.
        CHECK(filter_audio.mp2k_write_may_be_relevant(
            0x08000001u, filter_si + 0x50u, 1));
        CHECK(filter_audio.mp2k_write_may_be_relevant(
            0x08000001u, channels, 1));
        CHECK(filter_audio.mp2k_write_may_be_relevant(
            0x08000001u, channels_end - 1u, 1));
        // channels_end is also the known ring start for this fixture.
        CHECK(filter_audio.mp2k_write_may_be_relevant(
            0x08000001u, channels_end, 1));
        CHECK(filter_audio.mp2k_write_may_be_relevant(
            0x08000001u, ring, 1));
        CHECK(filter_audio.mp2k_write_may_be_relevant(
            0x08000001u, ring_end - 1u, 1));
        CHECK(filter_audio.mp2k_write_may_be_relevant(
            0x08000001u, ring_b, 1));
        CHECK(filter_audio.mp2k_write_may_be_relevant(
            0x08000001u, ring_b_end - 1u, 1));
        CHECK(!filter_audio.mp2k_write_may_be_relevant(
            0x08000001u, ring_end, 1));
        CHECK(!filter_audio.mp2k_write_may_be_relevant(
            0x08000001u, ring_b_end, 1));
        CHECK(filter_audio.mp2k_write_may_be_relevant(
            0x08000001u, ring_end - 1u, 2));
        CHECK(!filter_audio.mp2k_write_may_be_relevant(
            0x08000001u, ring_end, 1));
        CHECK(filter_audio.mp2k_write_may_be_relevant(
            0x08000001u, gba::kSoundInfoPtr, 4));
        CHECK(filter_audio.mp2k_write_may_be_relevant(
            0x08000001u, gba::kSoundInfoPtr + 3u, 2));
        CHECK(!filter_audio.mp2k_write_may_be_relevant(
            0x08000001u, gba::kSoundInfoPtr + 4u, 1));
        CHECK(!filter_audio.mp2k_write_may_be_relevant(
            0x08000001u, 0x03005000u, 1));
    }

    // Hook fast-filter parity: configured keys retain old normalized-PC
    // semantics, duplicate signatures do not change matches, disabled audio
    // skips the deep hook, and unconfigured state fails open.
    {
        set_shadow_request(true);
        std::vector<uint8_t> hook_rom(0x100, 0);
        std::vector<uint8_t> hook_ewram(0x100, 0);
        std::vector<uint8_t> hook_iwram(0x8000, 0);
        const uint32_t hook_si = 0x03001000u;
        const std::size_t hook_off = hook_si & 0x7FFFu;
        auto hook_put32 = [&](std::size_t off, uint32_t value) {
            hook_iwram[off + 0] = static_cast<uint8_t>(value);
            hook_iwram[off + 1] = static_cast<uint8_t>(value >> 8);
            hook_iwram[off + 2] = static_cast<uint8_t>(value >> 16);
            hook_iwram[off + 3] = static_cast<uint8_t>(value >> 24);
        };
        hook_put32(gba::kSoundInfoPtr & 0x7FFFu, hook_si);
        hook_put32(hook_off, gba::kMp2kMagicBase);
        hook_iwram[hook_off + 0x06] = gba::kMp2kMaxChans;
        hook_iwram[hook_off + 0x0B] = 7;
        hook_put32(hook_off + 0x10, 8);
        hook_put32(hook_off + 0x14, 65536);
        const std::vector<gba::Mp2kSig> sigs{
            {0, 0x08000101u}, {0, 0x08000101u},
            {0, 0x08000201u}, {0, 0x08000201u},
        };
        gba::Mp2kShadow old_shadow;
        old_shadow.init(sigs);
        gba::GbaAudio hook_audio;
        hook_audio.configure_shadow(
            sigs, hook_rom.data(), hook_rom.size(), hook_ewram.data(),
            hook_ewram.size(), hook_iwram.data(), hook_iwram.size(),
            true, true);
        for (const uint32_t key : {0x08000100u, 0x08000101u,
                                   0x08000200u, 0x08000201u,
                                   0x08000300u, 0x03000828u}) {
            CHECK(hook_audio.mp2k_frame_hook_may_be_relevant(key) ==
                   old_shadow.matches_hook(key));
        }
        const auto& hook_filter = hook_audio.mp2k_hook_fast_filter();
        CHECK(hook_filter.valid && hook_filter.premix_count == 4);
        for (const uint32_t pc : {0x030008B4u, 0x03000A8Cu,
                                  0x03000BCCu, 0x03000BD4u,
                                  0x03000C70u})
            CHECK(hook_filter.postmix_may_be_relevant(pc));
        CHECK(!hook_filter.postmix_may_be_relevant(0x03005000u));
        CHECK(hook_audio.mp2k_control_hook_may_be_relevant(0x03000828u));
        CHECK(hook_audio.mp2k_control_hook_may_be_relevant(0x03000829u));
        CHECK(!hook_audio.mp2k_control_hook_may_be_relevant(0x03005000u));

        set_shadow_request(false);
        gba::GbaAudio disabled_audio;
        disabled_audio.configure_shadow(
            sigs, hook_rom.data(), hook_rom.size(), hook_ewram.data(),
            hook_ewram.size(), hook_iwram.data(), hook_iwram.size(),
            false, false);
        CHECK(!disabled_audio.mp2k_frame_hook_may_be_relevant(0x08000101u));
        CHECK(!disabled_audio.mp2k_control_hook_may_be_relevant(0x03000828u));

        gba::GbaAudio unconfigured_audio;
        CHECK(unconfigured_audio.mp2k_hook_fast_filter().valid == false);
        CHECK(unconfigured_audio.mp2k_frame_hook_may_be_relevant(
            0x08000101u));
        CHECK(unconfigured_audio.mp2k_control_hook_may_be_relevant(
            0x03005000u));
    }

    uint32_t index = 0;
    CHECK(gba::mp2k_cursor_index(0x08000110u, 0x200u,
                                  0x08000190u, index));
    CHECK(index == 0x80u);
    // FIX uses the same live cp/data relationship as PCM.
    CHECK(gba::mp2k_cursor_index(0x08000210u, 0x400u,
                                  0x080002d0u, index));
    CHECK(index == 0xc0u);
    CHECK(!gba::mp2k_cursor_index(0x08000210u, 0x400u,
                                   0x08000200u, index));
    CHECK(!gba::mp2k_cursor_index(0x08000210u, 0x400u,
                                   0x08000611u, index));
    // Fresh note-on phase owns the render until the canonical mixer publishes
    // cp; an existing PCM voice must still validate the same stale cursor.
    CHECK(!gba::mp2k_should_validate_live_cursor(true, false));
    CHECK(gba::mp2k_should_validate_live_cursor(false, false));
    CHECK(!gba::mp2k_should_validate_live_cursor(false, true));

    std::array<uint8_t, 0x140> synthetic{};
    // Archive offset 0x100 resolves into the synthetic ROM window.
    synthetic[0x100 + 8] = 0x20;
    synthetic[0x100 + 12] = 0x40;
    gba::MemView mem{synthetic.data(), synthetic.size(), nullptr, 0,
                     nullptr, 0};
    CHECK(gba::mp2k_resolve_wave_address(mem, 0x100u, 16) ==
           0x08000100u);
    // A resolved archive header does not make an unrelated live cp valid;
    // the cursor gate must remain fail-closed.
    uint32_t synthetic_cursor = 0;
    CHECK(!gba::mp2k_cursor_index(0x08000110u, 0x40u,
                                   0x0800C6EDu, synthetic_cursor));
    CHECK(gba::mp2k_cursor_index(0x08000110u, 0x40u,
                                  0x08000150u, synthetic_cursor) &&
           synthetic_cursor == 0x40u);

    // Header bytes are intentionally different: the payload is authoritative.
    synthetic[0x100 + 1] = 3;
    synthetic[0x100 + 2] = 0x11;
    synthetic[0x100 + 16] = 0;
    synthetic[0x100 + 17] = 0x22;
    synthetic[0x100 + 18] = 0x33;
    synthetic[0x100 + 19] = 0x44;
    synthetic[0x100 + 20] = 0x55;
    gba::Mp2kSynthParams synth{};
    CHECK(gba::mp2k_decode_synth_payload(synthetic.data() + 0x110, 5,
                                          synth));
    CHECK(synth.kind == 1 && synth.base == 0x22 && synth.step == 0x33 &&
           synth.depth == 0x44 && synth.initial_duty == 0x55);

    // Guest integer gain keeps odd products by shifting after the multiply.
    CHECK(gba::mp2k_gain_q9(3, 170) == 0);
    CHECK(gba::mp2k_gain_q9(3, 171) == 1);

    // RAM mixer instructions 0x03000758..60 halve both Q9 lanes with the
    // shared ADC carry and clear the low-lane overlap byte.
    CHECK(gba::mp2k_pack_stereo_gain(35, 47) == 0x00180012u);
    CHECK(gba::mp2k_packed_accumulate(
               0xE5BDED84u, 0x00180012u, -30) == 0xE2EDEB68u);
    // Captured mature seq-5/sample-0 chain: the first two voices agree, then
    // ordinary PCM voice 2 proves that +5 is distinct from native's +6.
    CHECK(gba::mp2k_packed_accumulate(
               0xD241D9FAu, 0x000D000Du, 5) == 0xD282DA3Bu);
    CHECK(gba::mp2k_packed_accumulate(
               0xD241D9FAu, 0x000D000Du, 6) == 0xD28FDA48u);
    // Captured GS1 FIX operands: unhalved packed gain and one signed byte.
    CHECK(gba::mp2k_pack_fix_gain(26, 26) == 0x001A001Au);
    CHECK(gba::mp2k_packed_accumulate(
               0xD282DA3Bu, 0x001A001Au, 5) == 0xD304DABDu);
    uint8_t dry_route = 0xff;
    CHECK(gba::mp2k_gs1_dry_route(0x03000BD4u, dry_route) &&
           dry_route == 0);
    CHECK(gba::mp2k_gs1_dry_route(0x03000BCCu, dry_route) &&
           dry_route == 1);
    CHECK(!gba::mp2k_gs1_dry_route(0x03000C70u, dry_route));
    // A sequence may observe a partial prior writer before the completed
    // block. Reverb history must align from the completion start, not the
    // first observed address.
    CHECK(gba::mp2k_reverb_word_address(0x020037C0u, 0) == 0x020037C0u);
    CHECK(gba::mp2k_reverb_word_address(0x020037C0u, 87) == 0x0200391Cu);
    int8_t seeded_a = 0, seeded_b = 0;
    gba::mp2k_extract_dry_sample(
        gba::mp2k_saturate_packed_lanes(0xB13370F0u),
        seeded_a, seeded_b);
    CHECK(seeded_a == 127 && seeded_b == -128);

    std::array<uint32_t, 4> packed_accumulators{
        0xB13370F0u, 0xB16670EFu, 0xB0EA7034u, 0xAE566D0Au};
    for (uint32_t& value : packed_accumulators)
        value = gba::mp2k_saturate_packed_lanes(value);
    CHECK((packed_accumulators == std::array<uint32_t, 4>{
        0xC0333FF0u, 0xC0263FEFu, 0xC02A3FF4u, 0xC0163FCAu}));
    uint32_t dry_a = 0, dry_b = 0;
    gba::mp2k_extract_dry_words(packed_accumulators, dry_a, dry_b);
    CHECK(dry_a == 0x7F7F7F7Fu);
    CHECK(dry_b == 0x80808080u);
    const auto producer = gba::mp2k_finalize_producer_group(
        packed_accumulators, 0x80808080u, 0xE7ECEFF2u);
    CHECK((producer == std::array<uint32_t, 4>{
        0xE0440E58u, 0xE0410DF9u, 0xE0420D9Au, 0xE03D0CEFu}));
    // Captured seq-5 group: saturation leaves these lane-safe accumulators
    // unchanged. ASR #18 plus old-ring ASR #19 and the conditional +0x28 bias
    // then reproduce all four stored producer words exactly.
    std::array<uint32_t, 4> seq5_acc{
        0xF584F2EAu, 0xF483F1C6u, 0xF3C9F127u, 0xF43CF1B5u};
    for (uint32_t& value : seq5_acc)
        value = gba::mp2k_saturate_packed_lanes(value);
    CHECK((seq5_acc == std::array<uint32_t, 4>{
        0xF584F2EAu, 0xF483F1C6u, 0xF3C9F127u, 0xF43CF1B5u}));
    const auto seq5_post = gba::mp2k_finalize_producer_group(
        seq5_acc, 0x4F565657u, 0x4D525254u);
    CHECK((seq5_post == std::array<uint32_t, 4>{
        0x084A0743u, 0x07EA06BBu, 0x07BC0693u, 0x06F90617u}));
    // The old ring is packed signed-byte history, not Q15.  The captured
    // chain stays lane-isolated and reproduces all four packed producer words.
    int8_t history_a = 0, history_b = 0;
    gba::mp2k_decode_producer_word(producer[0], history_a, history_b);
    CHECK(history_a == 14 && history_b == -32);

    // Exact Q23 interpolation and carry, including a loop crossing.
    CHECK(gba::mp2k_interpolate_s8(-128, 127, 1u << 22) == -1);
    // Mature seq-5 voice 2 starts from this exact signed pair and Q23 phase.
    // The direct interpolation is 2; a native check value of 3 therefore
    // proves stale cadence/hold state rather than a rounding-mode difference.
    CHECK(gba::mp2k_interpolate_s8(3, 2, 0x0029B640u) == 2);
    CHECK(gba::mp2k_interpolate_s8_x2(3, 2, 0x0029B640u) == 5);
    double held_phase = 2.5;
    CHECK(!gba::mp2k_check_hold_refresh(false, 3.0, held_phase));
    CHECK(held_phase == 1.5);
    CHECK(gba::mp2k_check_hold_refresh(true, 3.0, held_phase));
    CHECK(held_phase == 2.0);
    uint32_t frac = gba::kMp2kFracMask - 1u;
    uint32_t cursor = 0;
    CHECK(gba::mp2k_advance_cursor(cursor, frac, gba::kMp2kFracOne + 3,
                                    8, 2, true));
    CHECK(cursor == 2 && frac == 1);
    cursor = 3;
    frac = 0;
    CHECK(gba::mp2k_advance_cursor(cursor, frac, gba::kMp2kFracOne * 2,
                                    4, 1, true));
    CHECK(cursor == 2 && frac == 0);
    cursor = 3;
    frac = 0;
    CHECK(!gba::mp2k_advance_cursor(cursor, frac, gba::kMp2kFracOne,
                                     4, 0, false));

    // Integer GS1 cross-feed is channel-swapped by design and saturates at
    // the signed 8-bit positive endpoint in Q15.
    auto wet = gba::mp2k_gs1_reverb_mix(0, 0, 4000, 8000);
    CHECK(wet.right == 2000 && wet.left == 1000);
    wet = gba::mp2k_gs1_reverb_mix(40000, -40000, 0, 0);
    CHECK(wet.right == 32512 && wet.left == -32768);

    int8_t route_a = 0, route_b = 0;
    gba::mp2k_decode_producer_word(0x007FFF80u, route_a, route_b);
    CHECK(route_a == -1 && route_b == 0);
    gba::mp2k_decode_producer_word(0xE0440E58u, route_a, route_b);
    CHECK(route_a == 14 && route_b == -32);

    // Signed producer blocks reject polarity flips and A/B swaps.
    std::array<int8_t, 16> guest_l{};
    std::array<int8_t, 16> guest_r{};
    std::array<float, 16> native_l{};
    std::array<float, 16> native_r{};
    for (std::size_t i = 0; i < guest_l.size(); ++i) {
        guest_l[i] = static_cast<int8_t>((i * 9) - 64);
        guest_r[i] = static_cast<int8_t>(63 - (i * 7));
        native_l[i] = static_cast<float>(guest_l[i]) / 128.0f;
        native_r[i] = static_cast<float>(guest_r[i]) / 128.0f;
    }
    CHECK(gba::compare_signed_stereo_block(
               guest_l.data(), guest_r.data(), native_l.data(),
               native_r.data(), guest_l.size()).pass);
    auto inverted = native_l;
    for (float& x : inverted) x = -x;
    CHECK(!gba::compare_signed_stereo_block(
                guest_l.data(), guest_r.data(), inverted.data(),
                native_r.data(), guest_l.size()).pass);
    CHECK(!gba::compare_signed_stereo_block(
                guest_l.data(), guest_r.data(), native_r.data(),
                native_l.data(), guest_l.size()).pass);

    // The producer-stage diagnostic separates a C70/source mismatch from a
    // later host-timestamp problem without changing the verifier gate. Here
    // the captured DMA routes are the negated C70 block, while native output
    // matches those routes; the two correlations must point to different
    // stages.
    gba::Mp2kShadow source_shadow;
    gba::Mp2kShadowWallFixture::seed_source_diagnostic(source_shadow);
    gba::Mp2kShadowWallFixture::source_diagnostic(source_shadow, false);
    CHECK(!source_shadow.producer_source_diagnostic().valid);
    gba::Mp2kShadowWallFixture::source_diagnostic(source_shadow);
    const auto& source_diag = source_shadow.producer_source_diagnostic();
    CHECK(source_diag.valid && source_diag.block_id == 42 &&
           source_diag.samples == 16);
    CHECK(source_diag.c70_vs_dma.correlation < -0.99f);
    CHECK(source_diag.native_vs_dma.correlation > 0.99f);
    gba::Mp2kShadowWallFixture::source_diagnostic(source_shadow);
    CHECK(source_shadow.producer_source_diagnostic().block_id == 42);

    // The FIFO route is the producer boundary. A C70/post-reverb candidate
    // that is wrong must not pass or get published when paired DMA routes and
    // native dry state are complete.
    gba::Mp2kShadow dma_route_shadow;
    gba::Mp2kShadowWallFixture::seed_dma_route_gate(dma_route_shadow);
    gba::Mp2kShadowWallFixture::judge(dma_route_shadow);
    CHECK(dma_route_shadow.producer_blocks_passed() == 1);
    CHECK(gba::Mp2kShadowWallFixture::published_blocks(dma_route_shadow) == 1);
    CHECK(gba::Mp2kShadowWallFixture::published_route_a(dma_route_shadow) ==
          32.0f / 128.0f);
    CHECK(gba::Mp2kShadowWallFixture::published_route_b(dma_route_shadow) ==
          -24.0f / 128.0f);

    // One good probation window is intentionally pre-proof. The next signed
    // reject must still emit the bounded post-probation diagnostic.
    gba::Mp2kShadow post_probation_shadow;
    gba::Mp2kShadowWallFixture::seed_post_probation_reject(
        post_probation_shadow);
    CHECK(gba::Mp2kShadowWallFixture::verifier_windows(
              post_probation_shadow) == 1);
    CHECK(gba::Mp2kShadowWallFixture::good_probation_window_seen(
        post_probation_shadow));
    CHECK(!gba::Mp2kShadowWallFixture::verifier_proven(
        post_probation_shadow));
    CHECK(gba::Mp2kShadowWallFixture::post_probation_reject_logged(
        post_probation_shadow));

    gba::Mp2kShadow cursor_shadow;
    gba::Mp2kShadowWallFixture::seed_cursor_post_probation_reject(
        cursor_shadow);
    const auto& cursor_diag = cursor_shadow.cursor_diagnostic();
    CHECK(cursor_diag.valid && cursor_diag.block_id == 7 &&
           cursor_diag.snapshot_block == 7 && cursor_diag.max_index == 3);
    CHECK(cursor_diag.guest_valid && cursor_diag.guest_loop_wraps == 1 &&
           cursor_diag.guest_start_index == 7 &&
           cursor_diag.guest_max_index == 4 &&
           cursor_diag.guest_host_index == 4 &&
           cursor_diag.guest_host_loop_wraps == 1 &&
           cursor_diag.host_advances == 10);
    CHECK(cursor_diag.native_start_valid && cursor_diag.native_max_valid &&
           cursor_diag.start_match && cursor_diag.max_match &&
           cursor_diag.provenance == 3);
    const auto& voice_diag =
        gba::Mp2kShadowWallFixture::voice_contribution_diag(cursor_shadow);
    CHECK(voice_diag.valid && voice_diag.max_index == 3 &&
          voice_diag.writer_stage == 1 && voice_diag.trace_valid &&
          voice_diag.active_voices == 3 && voice_diag.dominant_voice == 1 &&
          voice_diag.dominant_lane_abs == 64 &&
          voice_diag.voices[0].source_kind == 1 &&
          voice_diag.voices[1].source_kind == 2 &&
          voice_diag.voices[2].source_kind == 5 &&
          voice_diag.native_accumulator_hash != 0 &&
          voice_diag.guest_seed_hash != 0 &&
          voice_diag.native_seed_hash != 0 &&
          voice_diag.guest_raw_hash != 0 && voice_diag.guest_dry_hash != 0);

    gba::AudioSourceTimeline timeline;
    timeline.reset();
    timeline.request(gba::AudioSourceTimeline::Source::Native, 100);
    CHECK(timeline.source_at(99) == gba::AudioSourceTimeline::Source::Canonical);
    CHECK(timeline.source_at(100) == gba::AudioSourceTimeline::Source::Native);
    CHECK(timeline.blend_weight(100) > 0.0f &&
           timeline.blend_weight(132) == 1.0f);
    timeline.request(gba::AudioSourceTimeline::Source::Canonical, 200);
    CHECK(timeline.source_at(200) == gba::AudioSourceTimeline::Source::Canonical);

    // Signed producer bytes retain amplitude/polarity while the exact guest
    // cadence is linearly resampled onto the host grid. Every sequence keeps
    // its measured DMA-consumer timestamp, including variable FIFO latency.
    gba::ProducerHostResampler resampler;
    const std::array<float, 4> a0{-1.0f, -0.5f, 0.0f, 0.5f};
    const std::array<float, 4> b0{0.75f, 0.25f, -0.25f, -0.75f};
    const std::array<float, 4> a1{1.0f, 0.5f, 0.0f, -0.5f};
    const std::array<float, 4> b1{-1.0f, -0.5f, 0.0f, 0.5f};
    CHECK(resampler.push(10, 100ull << 32, 4, 8,
                          a0.data(), b0.data(), a0.size()));
    CHECK(resampler.push(11, 108ull << 32, 4, 8,
                          a1.data(), b1.data(), a1.size()));
    float host_a = 0.0f, host_b = 0.0f;
    CHECK(resampler.sample(100, host_a, host_b));
    CHECK(std::fabs(host_a + 1.0f) < 1e-6f);
    CHECK(std::fabs(host_b - 0.75f) < 1e-6f);
    CHECK(resampler.sample(101, host_a, host_b));
    CHECK(std::fabs(host_a + 0.75f) < 1e-6f);
    CHECK(std::fabs(host_b - 0.5f) < 1e-6f);
    CHECK(resampler.sample(107, host_a, host_b));
    CHECK(std::fabs(host_a - 0.75f) < 1e-6f);
    CHECK(std::fabs(host_b + 0.875f) < 1e-6f);
    CHECK(resampler.sample(108, host_a, host_b));
    CHECK(std::fabs(host_a - 1.0f) < 1e-6f);
    CHECK(std::fabs(host_b + 1.0f) < 1e-6f);

    // A later block is not pulled forward to the prior block's nominal end.
    CHECK(resampler.push(12, 120ull << 32, 4, 8,
                          a0.data(), b0.data(), a0.size()));
    CHECK(!resampler.sample(116, host_a, host_b));
    CHECK(resampler.stats().resampler_underruns == 1);
    CHECK(resampler.stats().late_blocks == 1);
    CHECK(resampler.sample(120, host_a, host_b));
    CHECK(!resampler.sample(126, host_a, host_b));
    CHECK(resampler.stats().resampler_underruns == 2);
    resampler.reset();
    CHECK(!resampler.sample(108, host_a, host_b));
    CHECK(resampler.push(20, 200ull << 32, 4, 8,
                          a0.data(), b0.data(), a0.size()));
    CHECK(resampler.push(22, 300ull << 32, 4, 8,
                          a1.data(), b1.data(), a1.size()));
    CHECK(!resampler.sample(200, host_a, host_b));
    CHECK(resampler.sample(300, host_a, host_b));
    CHECK(resampler.stats().sequence_gaps == 1);
    CHECK(resampler.stats().resampler_resets == 2);
    CHECK(resampler.stats().resampler_queue_max == 2);

    // A mid-block attach must not make a partial guest/native epoch eligible
    // for comparison. Alignment opens only on a complete producer block.
    gba::ProducerStartupAlignment startup_alignment;
    CHECK(!startup_alignment.observe(352, 76, 352));
    CHECK(!startup_alignment.ready());
    CHECK(startup_alignment.observe(352, 352, 352));
    CHECK(startup_alignment.ready());
    CHECK(startup_alignment.observe(352, 352, 352));

    // Measured seq4 history operands: a partial epoch left native circular
    // words stale, so the first aligned block must re-anchor known slots to
    // canonical guest history before post-reverb finalization.
    std::unordered_map<uint32_t, uint32_t> canonical_history{
        {0x02003660u, 0x3D3A3B3Au}, {0x02003C90u, 0x322F302Fu}};
    std::unordered_map<uint32_t, uint32_t> native_history{
        {0x02003660u, 0x585F666Cu}, {0x02003C90u, 0x3F4B5254u}};
    gba::mp2k_sync_reverb_history(canonical_history, native_history);
    CHECK(native_history[0x02003660u] == 0x3D3A3B3Au);
    CHECK(native_history[0x02003C90u] == 0x322F302Fu);

    // Measured seq8 first-diff operands remain a dry packed-lane mismatch
    // after old history is equal: native accumulator 0xFC92FBFD decodes to
    // (-9,-7), while the guest raw post word decodes to (-1,-2).
    int8_t dry_sample_a = 0, dry_sample_b = 0;
    gba::mp2k_extract_dry_sample(0xFC92FBFDu, dry_sample_a,
                                  dry_sample_b);
    CHECK(dry_sample_a == -9 && dry_sample_b == -7);
    const uint32_t guest_post = static_cast<uint32_t>(
        static_cast<uint16_t>(-223)) |
        (static_cast<uint32_t>(static_cast<uint16_t>(-468)) << 16);
    gba::mp2k_decode_producer_word(guest_post, dry_sample_a, dry_sample_b);
    CHECK(dry_sample_a == -1 && dry_sample_b == -2);

    // Seq8's bounded trace replays the native rolling seed and all seven
    // guest-order contributions exactly; no order/carry discrepancy is
    // introduced by the native accumulator itself.
    uint32_t seq8_accumulator = 0x0459061Du;
    const std::array<uint32_t, 7> seq8_gains{
        0x00180012u, 0x00090009u, 0x000D000Du, 0x001A001Au,
        0x00020001u, 0x000D000Du, 0x00110015u};
    const std::array<int32_t, 7> seq8_samples{-8, 77, -8, 12, 6, 2, -161};
    for (std::size_t i = 0; i < seq8_gains.size(); ++i)
        seq8_accumulator = gba::mp2k_packed_accumulate(
            seq8_accumulator, seq8_gains[i], seq8_samples[i]);
    CHECK(seq8_accumulator == 0xFC92FBFDu);

    // Canonical boundary trace is a bounded, ordered record: the measured
    // seq8 write stream starts from zero and exposes the first operand before
    // any later voice can be aligned to the native rolling seed.
    gba::CanonicalMixTrace canonical_mix;
    canonical_mix.valid = true;
    canonical_mix.sequence = 8;
    canonical_mix.count = 8;
    canonical_mix.steps[0].pc = 0x030008B4u;
    canonical_mix.steps[0].cycle = 2387762;
    canonical_mix.steps[0].mode = 1;
    canonical_mix.steps[0].before = 0;
    canonical_mix.steps[0].after = 0x03B905A5u;
    canonical_mix.steps[0].operand_sample = -52;
    canonical_mix.steps[0].operand_gain = 0x0014000Fu;
    CHECK(canonical_mix.steps[0].before != 0x0459061Du);
    CHECK(canonical_mix.steps[0].pc == 0x030008B4u);
    CHECK(canonical_mix.steps[0].cycle == 2387762 &&
           canonical_mix.steps[0].mode == 1);
    canonical_mix.pre_cursor = 8404;
    canonical_mix.completion_cursor = 8404;
    canonical_mix.pre_snapshot_generation = 9;
    canonical_mix.pre_active_voices = 7;
    canonical_mix.pre_hook_pc = 0x08000100u;
    canonical_mix.pre_hook_source = 2;
    canonical_mix.pre_channel_status[7] = 0x0001u;
    canonical_mix.write_channel_status[7] = 0x0801u;
    canonical_mix.pre_hook_cycles = 100;
    canonical_mix.first_write_cycles = 110;
    canonical_mix.last_write_cycles = 120;
    CHECK(canonical_mix.sequence == 8 &&
           canonical_mix.pre_cursor == canonical_mix.completion_cursor);
    CHECK(canonical_mix.pre_active_voices < canonical_mix.count);
    CHECK(canonical_mix.pre_hook_source == 2 &&
           canonical_mix.pre_channel_status[7] !=
               canonical_mix.write_channel_status[7]);
    gba::CanonicalMixTrace::ChannelWrite identity_write;
    identity_write.pc = 0x03001234u;
    identity_write.mode = 1;
    identity_write.cycle = 2387000;
    identity_write.channel = 7;
    identity_write.field = 0x24;
    identity_write.width = 4;
    identity_write.before = 0x1000;
    identity_write.value = 0x2000;
    CHECK(identity_write.mode == 1 && identity_write.width == 4 &&
           identity_write.before != identity_write.value &&
           identity_write.cycle < 2387762);
    gba::CanonicalMixTrace::ChannelWrite cursor_write;
    cursor_write.channel = 1;
    cursor_write.field = 0x28;
    cursor_write.width = 4;
    cursor_write.cycle = 2387800;
    CHECK(cursor_write.field == 0x28 && cursor_write.width == 4 &&
           identity_write.cycle < cursor_write.cycle);
    canonical_mix.voice_identities[0].channel = 0;
    canonical_mix.voice_identities[0].channel_base = 0x02003050u;
    canonical_mix.voice_identities[0].status = 0x81;
    canonical_mix.voice_identities[0].ctype = 0x00;
    canonical_mix.voice_identities[0].wave = 0x08010000u;
    canonical_mix.voice_identities[0].sample_cursor = 0x02003028u;
    canonical_mix.voice_identities[0].wave_loop = 0x20u;
    canonical_mix.voice_identities[0].wave_size = 0x100u;
    canonical_mix.voice_identities[0].looped = 1;
    CHECK(canonical_mix.voice_identities[0].status & 0x80u);
    CHECK(canonical_mix.voice_identities[0].sample_cursor != 0);
    CHECK(canonical_mix.voice_identities[0].channel_base == 0x02003050u &&
           canonical_mix.voice_identities[0].looped == 1);
    CHECK(canonical_mix.pre_hook_cycles < canonical_mix.first_write_cycles &&
           canonical_mix.first_write_cycles <= canonical_mix.last_write_cycles);

    gba::CanonicalBoundaryTrace boundary;
    boundary.sequence = 8;
    boundary.last_channel_cycle = 2387207;
    boundary.last_channel_pc = 0x030006E4u;
    boundary.last_field = 0x09;
    boundary.control_cycle = 2387710;
    boundary.control_pc = 0x03000828u;
    boundary.control_kind = 1;
    boundary.first_write_cycle = 2387762;
    boundary.first_write_pc = 0x030008B4u;
    CHECK(boundary.last_channel_cycle < boundary.control_cycle &&
           boundary.control_cycle < boundary.first_write_cycle &&
           boundary.last_channel_pc == 0x030006E4u &&
           boundary.first_write_pc == 0x030008B4u);
    gba::CanonicalBoundaryTrace seven_voice;
    seven_voice.sequence = 7;
    for (std::size_t i = 0; i < 7; ++i)
        seven_voice.voice_identities[i].channel =
            static_cast<uint16_t>(i);
    gba::CanonicalBoundaryTrace eight_voice;
    eight_voice.sequence = 8;
    for (std::size_t i = 0; i < 8; ++i)
        eight_voice.voice_identities[i].channel =
            static_cast<uint16_t>(i);
    CHECK(seven_voice.voice_identities[6].channel == 6 &&
           seven_voice.voice_identities[7].channel == 0 &&
           eight_voice.voice_identities[7].channel == 7);
    uint32_t one_snapshot_per_block = 0;
    ++one_snapshot_per_block; // seq7 boundary
    ++one_snapshot_per_block; // seq8 boundary
    CHECK(one_snapshot_per_block == 2);

    // DMA source identity establishes absolute phase. Consuming byte 7 of a
    // 352-byte block at host sample 500 places sample zero exactly seven
    // producer periods earlier. Duplicate-route consumption cannot release
    // the same sequence twice, and reset discards stale source identities.
    gba::ProducerDmaTimeline dma_timeline;
    gba::ProducerDmaRelease release;
    CHECK(dma_timeline.publish(30, 0x1000u, 0x2000u, 352, 21024, 65536));
    CHECK(dma_timeline.consume(0, 0x1007u, 500, release));
    const uint64_t dma_step = (65536ull << 32) / 21024u;
    CHECK(release.sequence == 30);
    CHECK(release.start_host_q32 == (500ull << 32) - 7u * dma_step);
    CHECK(dma_timeline.stats().matched == 1);
    // Bytes inside the already released source range are expected FIFO
    // continuation words, not association misses.
    CHECK(!dma_timeline.consume(0, 0x1008u, 500, release));
    CHECK(dma_timeline.stats().unmatched == 0);
    CHECK(!dma_timeline.consume(1, 0x2007u, 500, release));
    CHECK(dma_timeline.stats().published == 1);
    CHECK(dma_timeline.stats().releases == 1);
    CHECK(dma_timeline.stats().unmatched == 0);
    CHECK(!dma_timeline.consume(0, 0x3000u, 500, release));
    CHECK(dma_timeline.stats().unmatched == 1);
    const auto& outside_miss = dma_timeline.first_unmatched();
    CHECK(outside_miss.valid);
    CHECK(outside_miss.kind == gba::ProducerDmaMissKind::OutsidePublishedRange);
    CHECK(outside_miss.route == 0 && outside_miss.source_addr == 0x3000u);
    CHECK(outside_miss.sequence == 30);
    CHECK(dma_timeline.publish(31, 0x1160u, 0x2160u, 352, 21024, 65536));
    CHECK(dma_timeline.consume(1, 0x2163u, 1600, release));
    CHECK(release.sequence == 31);
    CHECK(release.start_host_q32 == (1600ull << 32) - 3u * dma_step);
    dma_timeline.reset();
    CHECK(!dma_timeline.consume(0, 0x1164u, 1601, release));

    // A producer sequence discontinuity resets the DMA timeline and is
    // retained as bounded evidence instead of silently dropping the gap.
    gba::ProducerDmaTimeline dma_gap;
    CHECK(dma_gap.publish(1, 0x4000u, 0x5000u, 32, 21024, 65536));
    gba::ProducerDmaRelease exact_start;
    CHECK(dma_gap.consume(0, 0x4000u, 500, exact_start));
    CHECK(exact_start.sequence == 1);
    CHECK(dma_gap.publish(3, 0x4100u, 0x5100u, 32, 21024, 65536));
    CHECK(dma_gap.stats().sequence_gaps == 1);

    // A valid address on the other DMA route is a true route association
    // miss, distinct from an outside-range miss.
    gba::ProducerDmaTimeline dma_route_mismatch;
    CHECK(dma_route_mismatch.publish(
        7, 0x7000u, 0x8000u, 32, 21024, 65536));
    CHECK(!dma_route_mismatch.consume(0, 0x8000u, 500, release));
    const auto& route_miss = dma_route_mismatch.first_unmatched();
    CHECK(route_miss.valid);
    CHECK(route_miss.kind == gba::ProducerDmaMissKind::RouteMismatch);
    CHECK(route_miss.route == 0 && route_miss.source_addr == 0x8000u);
    CHECK(route_miss.sequence == 7);
    CHECK(route_miss.range_start == 0x8000u &&
           route_miss.range_end == 0x8020u && route_miss.offset == 0);
    dma_route_mismatch.reset_epoch_diagnostics();
    CHECK(!dma_route_mismatch.first_unmatched().valid);

    // Repeated PreMix/PostMix notifications for one producer sequence produce
    // exactly one authoritative snapshot.
    std::vector<uint8_t> iwram(0x8000, 0);
    const uint32_t si = 0x03001000u;
    const std::size_t si_off = si & 0x7FFFu;
    auto put32 = [&](std::size_t off, uint32_t value) {
        iwram[off + 0] = static_cast<uint8_t>(value);
        iwram[off + 1] = static_cast<uint8_t>(value >> 8);
        iwram[off + 2] = static_cast<uint8_t>(value >> 16);
        iwram[off + 3] = static_cast<uint8_t>(value >> 24);
    };
    put32(gba::kSoundInfoPtr & 0x7FFFu, si);
    put32(si_off, gba::kMp2kMagicBase);
    iwram[si_off + 0x06] = 1;
    iwram[si_off + 0x0B] = 7;
    put32(si_off + 0x10, 224);
    put32(si_off + 0x14, 13379);
    gba::Mp2kShadow shadow;
    shadow.init({gba::Mp2kSig{0, 0x08000101u}});
    gba::MemView iwram_mem{nullptr, 0, nullptr, 0,
                           iwram.data(), iwram.size()};
    shadow.frame_hook(iwram_mem, 0, 0x08000101u,
                      gba::Mp2kShadow::HookPhase::PreMix, 7);
    shadow.frame_hook(iwram_mem, 0, 0x08000101u,
                      gba::Mp2kShadow::HookPhase::PreMix, 7);
    shadow.frame_hook(iwram_mem, 0, 0x08000101u,
                      gba::Mp2kShadow::HookPhase::PostMix, 7);
    CHECK(shadow.authoritative_snapshots() == 1);
    // The first native tail can be incomplete when shadow state attaches at
    // startup/savestate time. Discard that unverified epoch and accept the
    // next live snapshot instead of borrowing its state.
    shadow.frame_hook(iwram_mem, 1000, 0x08000101u,
                      gba::Mp2kShadow::HookPhase::PreMix, 8);
    CHECK(shadow.authoritative_snapshots() == 1);
    CHECK(!shadow.canonical_fallback());

    // A startup tail is discarded once, then the next valid snapshot owns a
    // fresh producer epoch.
    {
        put32(si_off + 0x10, 16);
        put32(si_off + 0x14, 65536);
        gba::Mp2kShadow pending_shadow;
        pending_shadow.init({gba::Mp2kSig{0, 0x08000101u}});
        gba::Mp2kShadowWallFixture::seed_incomplete(pending_shadow);
        gba::Mp2kShadowWallFixture::judge(pending_shadow);
        CHECK(pending_shadow.producer_blocks_incomplete() == 1);
        CHECK(pending_shadow.published_producer_block() == UINT64_MAX);

        pending_shadow.frame_hook(
            iwram_mem, 10, 0x08000101u,
            gba::Mp2kShadow::HookPhase::PreMix, 2);
        CHECK(!pending_shadow.canonical_fallback());
        CHECK(pending_shadow.engaged());
        CHECK(gba::Mp2kShadowWallFixture::active_block(pending_shadow) == 2);
        CHECK(pending_shadow.authoritative_snapshots() == 1);
        CHECK(gba::Mp2kShadowWallFixture::startup_realign_pending(
            pending_shadow));

        {
            gba::Mp2kShadow startup_tail;
            startup_tail.init({gba::Mp2kSig{0, 0x08000101u}});
            gba::Mp2kShadowWallFixture::seed_incomplete(startup_tail, 352, 351);
            put32(si_off + 0x10, 352);
            gba::Mp2kShadowWallFixture::judge(startup_tail);
            startup_tail.frame_hook(
                iwram_mem, 10, 0x08000101u,
                gba::Mp2kShadow::HookPhase::PreMix, 2);
            CHECK(!startup_tail.canonical_fallback());
            CHECK(gba::Mp2kShadowWallFixture::startup_realign_pending(
                startup_tail));

            // The measured 351/352 tail can repeat at the very next guest
            // boundary because the producer period is fractional on the
            // host grid. It is still unowned; the second partial epoch must
            // realign, not fail closed as an established deferred hook.
            gba::Mp2kShadowWallFixture::seed_native_prefix(startup_tail, 15);
            startup_tail.frame_hook(
                iwram_mem, 20, 0x08000101u,
                gba::Mp2kShadow::HookPhase::PreMix, 3);
            CHECK(!startup_tail.canonical_fallback());
            CHECK(gba::Mp2kShadowWallFixture::active_block(startup_tail) == 3);
            CHECK(gba::Mp2kShadowWallFixture::startup_realign_pending(
                startup_tail));

            // Once a complete pair is judged, ownership is established and
            // the strict deferred-hook failure is restored for later epochs.
            gba::Mp2kShadowWallFixture::seed_complete(startup_tail);
            gba::Mp2kShadowWallFixture::judge(startup_tail);
            CHECK(!gba::Mp2kShadowWallFixture::startup_realign_pending(
                startup_tail));
            startup_tail.frame_hook(
                iwram_mem, 30, 0x08000101u,
                gba::Mp2kShadow::HookPhase::PreMix, 4);
            gba::Mp2kShadowWallFixture::seed_native_prefix(startup_tail, 15);
            startup_tail.frame_hook(
                iwram_mem, 40, 0x08000101u,
                gba::Mp2kShadow::HookPhase::PreMix, 5);
            CHECK(startup_tail.canonical_fallback());
            put32(si_off + 0x10, 16);
        }

        // A later sequence gap still fails closed and cannot be revived by
        // producer writes after the failure.
        pending_shadow.frame_hook(
            iwram_mem, 20, 0x08000101u,
            gba::Mp2kShadow::HookPhase::PreMix, 4);
        pending_shadow.producer_interleaved_block(
            iwram_mem, 0x03002000u, 16, 2);
        pending_shadow.diagnostic_dry_block(
            iwram_mem, 0x03002040u, 16, 0, 2);
        pending_shadow.reverb_history_word(0, 0x03002080u, 0x11223344u, 2);
        CHECK(pending_shadow.authoritative_snapshots() == 1);
        CHECK(!pending_shadow.engaged());
        const std::size_t guest_before =
            gba::Mp2kShadowWallFixture::guest_samples(pending_shadow);
        const std::size_t native_before =
            gba::Mp2kShadowWallFixture::native_samples(pending_shadow);
        const std::size_t history_before =
            gba::Mp2kShadowWallFixture::history_words(pending_shadow, 0);
        const auto dma_before = pending_shadow.producer_dma_stats();
        pending_shadow.producer_interleaved_block(
            iwram_mem, 0x03002000u, 16, 1);
        pending_shadow.diagnostic_dry_block(
            iwram_mem, 0x03002040u, 16, 0, 1);
        pending_shadow.reverb_history_word(0, 0x03002080u, 0x55667788u, 1);
        pending_shadow.producer_dma_consume(0, 0x03002000u, 40, 0);
        float frozen_a = 1.0f, frozen_b = -1.0f;
        CHECK(!pending_shadow.render(iwram_mem, 40, frozen_a, frozen_b));
        CHECK(frozen_a == 0.0f && frozen_b == 0.0f);
        CHECK(gba::Mp2kShadowWallFixture::guest_samples(pending_shadow) ==
              guest_before);
        CHECK(gba::Mp2kShadowWallFixture::native_samples(pending_shadow) ==
              native_before);
        CHECK(gba::Mp2kShadowWallFixture::history_words(pending_shadow, 0) ==
              history_before);
        CHECK(pending_shadow.producer_dma_stats().matched == dma_before.matched);
        CHECK(pending_shadow.producer_dma_stats().unmatched == dma_before.unmatched);
        gba::Mp2kShadow order_shadow;
        order_shadow.init({gba::Mp2kSig{0, 0x08000101u}});
        gba::Mp2kShadowWallFixture::seed_incomplete(order_shadow);
        gba::Mp2kShadowWallFixture::mark_established(order_shadow);
        order_shadow.frame_hook(
            iwram_mem, 11, 0x08000101u,
            gba::Mp2kShadow::HookPhase::PreMix, 2);
        CHECK(order_shadow.canonical_fallback());
        pending_shadow.reset_runtime();
        CHECK(!pending_shadow.canonical_fallback());
        pending_shadow.frame_hook(
            iwram_mem, 30, 0x08000101u,
            gba::Mp2kShadow::HookPhase::PreMix, 5);
        CHECK(pending_shadow.authoritative_snapshots() == 1);
    }

    // Regression: the native voice-render loop indexes its per-voice trace
    // array by the voice's fixed slot in voices_ (size kMp2kMaxChans == 12),
    // not by an active-voice count. Before the fix, that trace array was
    // sized 8, so any render pass reaching 9+ active voices wrote out of
    // bounds on the stack. Drive all 12 channels active and confirm every
    // slot is recorded with no overflow.
    {
        std::vector<uint8_t> rom12(0x140, 0);
        // Camelot synth header: zero size/loop marks it as a synthetic
        // (non-PCM) instrument; the 5-byte payload right after the 16-byte
        // header selects the oscillator. kind=0 -> modulated pulse (PWM).
        rom12[0x110] = 0;     // payload[0]: kind selector -> PWM
        rom12[0x111] = 0x40;  // base
        rom12[0x112] = 0x08;  // step
        rom12[0x113] = 0x10;  // depth
        rom12[0x114] = 0x40;  // initial_duty
        std::vector<uint8_t> ewram12(0x100, 0);
        std::vector<uint8_t> iwram12(0x8000, 0);
        const uint32_t si12 = 0x03001000u;
        const std::size_t si12_off = si12 & 0x7FFFu;
        auto put32_12 = [&](std::size_t off, uint32_t value) {
            iwram12[off + 0] = static_cast<uint8_t>(value);
            iwram12[off + 1] = static_cast<uint8_t>(value >> 8);
            iwram12[off + 2] = static_cast<uint8_t>(value >> 16);
            iwram12[off + 3] = static_cast<uint8_t>(value >> 24);
        };
        put32_12(gba::kSoundInfoPtr & 0x7FFFu, si12);
        put32_12(si12_off, gba::kMp2kMagicBase);  // ident
        iwram12[si12_off + 0x06] = gba::kMp2kMaxChans;  // max_chans = 12
        iwram12[si12_off + 0x0B] = 7;                   // dma period
        const uint32_t spv12 = 8;
        const uint32_t pcm_freq12 = 65536;  // matches default render rate:
                                             // mix_step_ == 1.0, one producer
                                             // sample generated per render().
        put32_12(si12_off + 0x10, spv12);
        put32_12(si12_off + 0x14, pcm_freq12);
        for (int ch = 0; ch < gba::kMp2kMaxChans; ++ch) {
            const std::size_t base = si12_off + gba::kSoundChansOff +
                static_cast<std::size_t>(ch) * gba::kSoundChanStride;
            iwram12[base + 0x00] = 0x80;  // status: START
            iwram12[base + 0x01] = 0x00;  // ctype: plain synth voice
            iwram12[base + 0x02] = 127;   // vol_r
            iwram12[base + 0x03] = 127;   // vol_l
            iwram12[base + 0x04] = 0xFF;  // attack
            iwram12[base + 0x05] = 0xC0;  // decay
            iwram12[base + 0x06] = 0x80;  // sustain
            iwram12[base + 0x07] = 0x40;  // release
            put32_12(base + 0x18, 0);          // count
            put32_12(base + 0x1C, 0);          // fw
            put32_12(base + 0x20, 0x00004000); // freq
            put32_12(base + 0x24, 0x08000100u); // wav (absolute, resolves
                                                 // directly in synthetic ROM)
        }
        gba::MemView mem12{rom12.data(), rom12.size(), ewram12.data(),
                           ewram12.size(), iwram12.data(), iwram12.size()};
        gba::Mp2kShadow shadow12;
        // Full per-voice traces are opt-in diagnostics. Enable them for this
        // legacy trace-shape regression; the benchmark separately proves the
        // probe-off path retains no per-sample trace.
        set_audio_probe(true);
        shadow12.init({gba::Mp2kSig{0, 0x08000101u}});

        // First block: zero PCM makes this warmup guest/native pair match,
        // so it completes without consuming the diagnostic diff trace.
        shadow12.frame_hook(mem12, 0, 0x08000101u,
                            gba::Mp2kShadow::HookPhase::PreMix, 100);
        std::array<uint8_t, 32> seed_bytes{};
        for (std::size_t i = 0; i < seed_bytes.size(); ++i)
            seed_bytes[i] = static_cast<uint8_t>(0x12u + i);
        shadow12.producer_interleaved_block(mem12, 0x02000000u, spv12, 100);
        // Drain and judge block 100 before advancing the producer sequence;
        // the fail-closed shadow cannot accept a future hook over its tail.
        float first_ra = 0.0f, first_rb = 0.0f;
        for (uint32_t i = 0; i < spv12; ++i)
            shadow12.render(mem12, i, first_ra, first_rb);
        // Keep the nonzero producer setup for block 101; its mismatch is the
        // intended path that records all twelve active voice traces.
        std::copy(seed_bytes.begin(), seed_bytes.end(), ewram12.begin());

        // Second block: all 12 channels are active for this render pass, and
        // current_seed_words_ (populated from block 100's rolling words) is
        // non-empty, so the traced accumulate path executes. Each new block
        // clears producer_a_/producer_b_ (guest compare data), so it needs
        // its own interleaved block to become judgeable/complete.
        shadow12.frame_hook(mem12, 10, 0x08000101u,
                            gba::Mp2kShadow::HookPhase::PreMix, 101);
        shadow12.producer_interleaved_block(mem12, 0x02000000u, spv12, 101);
        float ra = 0.0f, rb = 0.0f;
        for (uint32_t i = 0; i < spv12; ++i)
            shadow12.render(mem12, 10 + i, ra, rb);

        const gba::ProducerDiffTrace& diff = shadow12.producer_diff_trace();
        CHECK(diff.valid);
        CHECK(diff.voices.size() ==
               static_cast<std::size_t>(gba::kMp2kMaxChans));
        for (std::size_t i = 0; i < diff.voices.size(); ++i)
            CHECK(diff.voices[i].active);
        set_audio_probe(false);
    }

    {
        // AUD-02 regression: mGBA's GBAAudioSamplePSG sums ch1+ch2+ch3 and
        // shifts the combined sum left by 3, then adds a separately
        // pre-shifted ch4 (third_party/mgba/src/gb/audio.c:752-763). Net
        // effect: all four PSG channels get the same ×8 scale. gba_audio.cpp
        // previously left SOUND3 (ch3) unscaled at the mix step, making it
        // ~8x too quiet relative to SOUND1/2/4. Pin the fixed relationship:
        // identical raw sample values on each channel (routed to the same
        // side, one at a time) must produce identical mixed contributions.
        constexpr int32_t kSample = 5;  // arbitrary shared "identical input"
        const gba::PsgMixResult ch1_only = gba::mix_psg_samples(
            gba::PsgChannelSamples{kSample, 0, 0, 0},
            gba::PsgChannelRouting{true, false, false, false, false, false,
                                   false, false});
        const gba::PsgMixResult ch2_only = gba::mix_psg_samples(
            gba::PsgChannelSamples{0, kSample, 0, 0},
            gba::PsgChannelRouting{false, false, true, false, false, false,
                                   false, false});
        const gba::PsgMixResult ch3_only = gba::mix_psg_samples(
            gba::PsgChannelSamples{0, 0, kSample, 0},
            gba::PsgChannelRouting{false, false, false, false, true, false,
                                   false, false});
        const gba::PsgMixResult ch4_only = gba::mix_psg_samples(
            gba::PsgChannelSamples{0, 0, 0, kSample},
            gba::PsgChannelRouting{false, false, false, false, false, false,
                                   true, false});

        // Each solo channel must land in "left" only, and the very act of
        // reading .left this way would catch a channel wired to the wrong
        // side, though that isn't the bug under test here.
        CHECK(ch1_only.right == 0 && ch2_only.right == 0 &&
               ch3_only.right == 0 && ch4_only.right == 0);

        // The core pin: ch3's contribution for a given raw sample value must
        // equal ch1/ch2/ch4's contribution for the same raw sample value
        // (the shared ×8 scale). Before the fix, ch3_only.left == kSample
        // (unscaled) while the others were kSample << 3 — an 8x mismatch.
        CHECK(ch3_only.left == ch1_only.left);
        CHECK(ch3_only.left == ch2_only.left);
        CHECK(ch3_only.left == ch4_only.left);
        CHECK(ch3_only.left == (kSample << 3));

        // Superposition: all four active at once on one side sums the four
        // equally-scaled contributions.
        const gba::PsgMixResult all_left = gba::mix_psg_samples(
            gba::PsgChannelSamples{kSample, kSample, kSample, kSample},
            gba::PsgChannelRouting{true, false, true, false, true, false,
                                   true, false});
        CHECK(all_left.left == 4 * (kSample << 3));
        CHECK(all_left.right == 0);
    }

    {
        // AUD-03 diagnostic seam: a synthetic low-level candidate must emit
        // exactly one aggregate first-failure record without storing samples.
        gba::Mp2kShadow shadow;
        gba::Mp2kShadowWallFixture::set_signed_gate(shadow);
        const gba::Mp2kOutputRouteObservation observation{
            0.10f, -0.10f, 0.0f, 0.0f, 0x0Fu, 0x03u, 0, 0, 0x0200u};
        std::string degraded;
        for (uint64_t i = 0; i < 64u * 1024u; ++i)
            shadow.judge_output(i, 0.8f, 0.8f, 0.1f, 0.1f,
                                observation, degraded);
        gba::Mp2kOutputDomainDiagnostic diagnostic{};
        CHECK(shadow.take_first_output_domain_diagnostic(diagnostic));
        CHECK(diagnostic.valid && diagnostic.verifier_window == 1);
        CHECK(diagnostic.samples == 64u * 1024u);
        CHECK(diagnostic.missing == 0 && diagnostic.missing_total == 0);
        CHECK(diagnostic.route_mask == 0x0Fu &&
              diagnostic.full_volume_mask == 0x03u);
        CHECK(std::fabs(diagnostic.canonical_peak[0] - 0.8) < 1e-6 &&
              std::fabs(diagnostic.native_peak[0] - 0.1) < 1e-6);
        CHECK(!shadow.take_first_output_domain_diagnostic(diagnostic));
    }
    return 0;
}
