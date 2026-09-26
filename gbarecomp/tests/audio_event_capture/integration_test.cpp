#include "gba_audio.h"
#include "gba_bus.h"
#include "mp2k_shadow.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

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

}  // namespace

// Synthetic, ROM-free publication seam. Friend access lets this fixture drive
// a judge-shaped edge without protected ROM bytes or live gameplay.
namespace gba {
struct GbaAudioTurboFixture {
    static void prepare(gba::Mp2kShadow& shadow, uint64_t sequence,
                        uint32_t pcm_rate = 13379,
                        uint64_t producer_cursor = UINT64_MAX,
                        bool judge = true) {
        shadow.active_ = true;
        shadow.engaged_ = true;
        shadow.reverb_ = 0;
        shadow.render_rate_ = gba::Mp2kWallMixer::kCanonicalRenderRate;
        shadow.pcm_freq_ = pcm_rate;
        shadow.producer_samples_ = 8;
        shadow.env_span_ = 4;
        shadow.producer_block_id_ = sequence;
        shadow.producer_block_start_cursor_ = producer_cursor == UINT64_MAX
            ? sequence * 65536u : producer_cursor;
        shadow.producer_route_addr_[0] = 0x02001000u;
        shadow.producer_route_addr_[1] = 0x02002000u;
        shadow.reverb_block_start_[0] = 0x02001000u;
        shadow.reverb_block_start_[1] = 0x02002000u;
        shadow.producer_a_.resize(8);
        shadow.producer_b_.resize(8);
        shadow.native_block_r_.resize(8);
        shadow.native_block_l_.resize(8);
        shadow.current_seed_words_.resize(8);
        for (uint32_t i = 0; i < 8; ++i) {
            shadow.producer_a_[i] = static_cast<int8_t>(i * 3 - 10);
            shadow.producer_b_[i] = static_cast<int8_t>(20 - i * 2);
            shadow.native_block_r_[i] =
                static_cast<float>(shadow.producer_a_[i]) / 128.0f;
            shadow.native_block_l_[i] =
                static_cast<float>(shadow.producer_b_[i]) / 128.0f;
            shadow.current_seed_words_[i] = 0x01000000u * (i + 1u);
        }
        auto& voice = shadow.voices_[0];
        voice = gba::Mp2kShadow::Voice{};
        voice.on = true;
        voice.ctype = 0;
        voice.data = 0x08000110u;
        voice.size = 8;
        voice.loop_start = 0;
        voice.looped = true;
        voice.step_q23 = gba::kMp2kFracOne;
        voice.g0r = voice.g0l = voice.g1r = voice.g1l = 1.0f;
        shadow.vf_.set_signed_producer_gate(true, true, 1.0f, 1.0f);
        if (!shadow.vf_.proven()) {
            for (uint32_t i = 0; i < 131200; ++i) {
                const float sample = 0.45f * std::sin(
                    static_cast<float>(i) * 0.037f);
                shadow.vf_.judge(sample, sample, sample, sample);
            }
        }
        if (judge) shadow.judge_producer_block(sequence);
    }

    static void arm(gba::GbaAudio& audio, const gba::MemView& mem,
                    uint64_t sequence) {
        audio.shadow_mem_ = mem;
        prepare(audio.shadow_, sequence);
        gba::Mp2kWallSnapshotInput snapshot{};
        CHECK(audio.shadow_.export_wall_snapshot(
            mem, sequence,
            gba::q32_seconds_from_frames(
                audio.shadow_.producer_block_start_cursor(),
                gba::Mp2kWallMixer::kCanonicalRenderRate),
            snapshot, gba::Mp2kWallExportMode::ZeroSeedMp2kOnly));
        snapshot.sequence = 0;
        CHECK(audio.turbo_wall_mixer_.begin_snapshot(snapshot) ==
               gba::Mp2kWallCaptureResult::Accepted);
        CHECK(audio.turbo_audio_updates_.prime(snapshot,
                                                 audio.turbo_wall_mixer_));
        audio.turbo_audio_decoupled_ = true;
        audio.turbo_audio_muted_ = false;
        audio.turbo_audio_sequence_ = sequence;
        audio.shadow_.set_published_block_callback(
            &gba::GbaAudio::turbo_audio_published_block_callback, &audio);
    }

    static bool enter_ready(gba::GbaAudio& audio, const gba::MemView& mem,
                            uint64_t sequence, bool reverb = false) {
        audio.shadow_mem_ = mem;
        prepare(audio.shadow_, sequence);
        audio.shadow_enabled_ = true;
        audio.turbo_audio_requested_ = true;
        audio.shadow_.reverb_ = reverb ? 1u : 0u;
        return audio.turbo_audio_enter(4.0f, false, 0);
    }

    static void check_three(gba::GbaAudio& audio) {
        prepare(audio.shadow_, 2);
        prepare(audio.shadow_, 3);
        prepare(audio.shadow_, 4);
        CHECK(audio.turbo_audio_updates_.size() == 3);
        for (uint64_t sequence = 2; sequence <= 4; ++sequence) {
            uint64_t due = 0;
            CHECK(audio.turbo_audio_updates_.next_due_guest_cursor_q32(
                due));
            CHECK(due == sequence << 32);
            CHECK(audio.turbo_audio_updates_.apply_due(
                audio.turbo_wall_mixer_, due));
            CHECK(audio.turbo_audio_updates_.stats().applied ==
                   sequence - 1);
        }
        CHECK(audio.turbo_audio_updates_.empty());
        audio.shadow_.set_published_block_callback(nullptr, nullptr);
    }

    static void init_pending_scheduler(gba::GbaAudio& audio,
                                       std::vector<uint8_t>& rom,
                                       std::vector<uint8_t>& ewram,
                                       std::vector<uint8_t>& iwram,
                                       uint32_t block_bytes = 8) {
        const uint32_t sound_info = 0x02000100u;
        auto put32 = [](std::vector<uint8_t>& bytes, std::size_t off,
                        uint32_t value) {
            bytes[off + 0] = static_cast<uint8_t>(value);
            bytes[off + 1] = static_cast<uint8_t>(value >> 8);
            bytes[off + 2] = static_cast<uint8_t>(value >> 16);
            bytes[off + 3] = static_cast<uint8_t>(value >> 24);
        };
        put32(iwram, gba::kSoundInfoPtr & 0x7FFFu, sound_info);
        put32(ewram, sound_info & 0x3FFFFu, gba::kMp2kMagicBase);
        ewram[(sound_info & 0x3FFFFu) + 0x05] = 0;
        ewram[(sound_info & 0x3FFFFu) + 0x06] = 1;
        ewram[(sound_info & 0x3FFFFu) + 0x0B] = 1;
        put32(ewram, (sound_info & 0x3FFFFu) + 0x10, block_bytes);
        put32(ewram, (sound_info & 0x3FFFFu) + 0x14, 13379);
        // The synthetic seam models the canonical MP2K wall grid. GbaAudio
        // resets to 32768 Hz, but native MP2K accepts only the 65536 Hz grid.
        audio.write_io16(0x088, 0x4200, 0);
        audio.configure_shadow(
            {gba::Mp2kSig{0, 0x08000101u}}, rom.data(), rom.size(),
            ewram.data(), ewram.size(), iwram.data(), iwram.size(), true,
            false);
        audio.shadow_enabled_ = true;
        audio.native_audio_requested_ = true;
        audio.mp2k_frame_hook(0x08000101u, 1);
        CHECK(audio.mp2k_active_block_id_ == 0);
        CHECK(audio.shadow_.authoritative_snapshots_ == 1);
    }

    static void render_pending_samples(gba::GbaAudio& audio,
                                       const gba::MemView& mem,
                                       uint32_t count) {
        for (uint32_t i = 0; i < count; ++i) {
            ++audio.shadow_cursor_;
            float route_a = 0.0f, route_b = 0.0f;
            audio.shadow_.render(mem, audio.shadow_cursor_, route_a, route_b);
        }
    }

    static void write_pending_block(gba::GbaAudio& audio,
                                    const gba::MemView& mem,
                                    uint64_t cycles) {
        (void)mem;
        const uint32_t base = 0x02000100u + 0x350u;
        for (uint32_t i = 0; i < 8; ++i) {
            audio.mp2k_pcm_write_hook(0x03000C70u, base + i * 4u, 0,
                                      4, 0, 0, cycles + i);
        }
    }

    static void reserve_pending(gba::GbaAudio& audio,
                                const gba::MemView& mem,
                                uint64_t cycles) {
        render_pending_samples(audio, mem, 29);
        write_pending_block(audio, mem, cycles);
        render_pending_samples(audio, mem, 32);
        CHECK(!audio.shadow_.canonical_fallback());
        CHECK(audio.mp2k_pending_block_id_ == 1);
    }

    static bool canonical_fallback(const gba::GbaAudio& audio) {
        return audio.shadow_.canonical_fallback();
    }
    static uint64_t active_block_id(const gba::GbaAudio& audio) {
        return audio.mp2k_active_block_id_;
    }
    static uint64_t pending_block_id(const gba::GbaAudio& audio) {
        return audio.mp2k_pending_block_id_;
    }
    static void set_pending_block_id(gba::GbaAudio& audio, uint64_t value) {
        audio.mp2k_pending_block_id_ = value;
    }
    static uint64_t authoritative_snapshots(const gba::GbaAudio& audio) {
        return audio.shadow_.authoritative_snapshots_;
    }
    static std::size_t native_block_size(const gba::GbaAudio& audio) {
        return audio.shadow_.native_block_l_.size();
    }
    static uint32_t writer_block_bytes(const gba::GbaAudio& audio,
                                       std::size_t index) {
        return index < audio.mp2k_pcm_writers_.size()
            ? audio.mp2k_pcm_writers_[index].block_bytes : 0;
    }
    static uint32_t c70_writer_block_bytes(const gba::GbaAudio& audio) {
        return audio.mp2k_c70_writer_.block_bytes;
    }
    static void set_have_block_cycles(gba::GbaAudio& audio, bool value) {
        audio.mp2k_have_block_cycles_ = value;
    }
    static void set_writer_block_bytes(gba::GbaAudio& audio,
                                       std::size_t index, uint32_t value) {
        if (index < audio.mp2k_pcm_writers_.size())
            audio.mp2k_pcm_writers_[index].block_bytes = value;
    }
    static void set_c70_writer_block_bytes(gba::GbaAudio& audio,
                                           uint32_t value) {
        audio.mp2k_c70_writer_.block_bytes = value;
    }
    static uint64_t c70_writer_calls(const gba::GbaAudio& audio) {
        return audio.mp2k_c70_writer_calls_;
    }
    static uint64_t c70_writer_completions(const gba::GbaAudio& audio) {
        return audio.mp2k_c70_writer_completions_;
    }
    static std::size_t mp2k_writer_count(const gba::GbaAudio& audio) {
        std::size_t count = 0;
        for (const auto& writer : audio.mp2k_pcm_writers_)
            if (writer.pc != 0) ++count;
        if (audio.mp2k_c70_writer_.pc != 0) ++count;
        return count;
    }
    static std::size_t non_c70_writer_count(const gba::GbaAudio& audio) {
        std::size_t count = 0;
        for (const auto& writer : audio.mp2k_pcm_writers_)
            if (writer.pc != 0) ++count;
        return count;
    }
    static bool c70_writer_registered(const gba::GbaAudio& audio) {
        return audio.mp2k_c70_writer_.pc == 0x03000C70u;
    }
    static std::size_t guest_producer_a_size(const gba::GbaAudio& audio) {
        return audio.shadow_.producer_a_.size();
    }
    static std::size_t guest_producer_b_size(const gba::GbaAudio& audio) {
        return audio.shadow_.producer_b_.size();
    }
    static void reset_shadow_runtime(gba::GbaAudio& audio) {
        audio.reset_shadow_runtime();
    }
    static void force_canonical_fallback(gba::GbaAudio& audio,
                                         const char* reason) {
        audio.shadow_.force_canonical_fallback(reason);
    }

    static void arm_normal_wall(gba::GbaAudio& audio,
                                const gba::MemView& mem,
                                uint64_t sequence,
                                uint32_t pcm_rate,
                                uint64_t producer_cursor) {
        audio.shadow_mem_ = mem;
        prepare(audio.shadow_, sequence, pcm_rate, producer_cursor);
        audio.shadow_enabled_ = true;
        audio.native_audio_requested_ = true;
        audio.native_audio_enabled_ = true;
    }
    static void publish_normal(gba::GbaAudio& audio, uint64_t sequence,
                               uint32_t pcm_rate, uint64_t producer_cursor) {
        prepare(audio.shadow_, sequence, pcm_rate, producer_cursor);
    }
    static std::size_t native_wall_queue_size(const gba::GbaAudio& audio) {
        return audio.turbo_audio_updates_.size();
    }
    static bool native_wall_failed(const gba::GbaAudio& audio) {
        return audio.native_wall_candidate_failed_;
    }
    static bool native_wall_active(const gba::GbaAudio& audio) {
        return audio.native_wall_candidate_active_;
    }
    static uint64_t native_wall_frames(const gba::GbaAudio& audio) {
        return audio.native_wall_candidate_frames_;
    }
    static uint64_t native_wall_composed_frames(const gba::GbaAudio& audio) {
        return audio.native_wall_composed_frames_;
    }
    static std::size_t native_ring_size(const gba::GbaAudio& audio) {
        return audio.native_audio_ring_size();
    }
    static std::size_t drain_native(gba::GbaAudio& audio,
                                    std::size_t max = 4096) {
        std::vector<int16_t> samples(max * 2);
        return audio.drain_native_samples(samples.data(), max);
    }
    static void fail_normal(gba::GbaAudio& audio, const char* reason) {
        audio.native_audio_fail(reason);
    }
    static void reset_normal(gba::GbaAudio& audio) {
        audio.reset_native_wall_candidate();
        audio.turbo_wall_mixer_.reset();
        audio.turbo_audio_updates_.reset(0);
        audio.native_audio_enabled_ = true;
        audio.native_audio_requested_ = true;
        audio.shadow_enabled_ = true;
        audio.native_head_ = audio.native_tail_ = 0;
    }
    static void prepare_normal(gba::GbaAudio& audio, uint64_t sequence,
                               uint32_t pcm_rate, uint64_t producer_cursor) {
        prepare(audio.shadow_, sequence, pcm_rate, producer_cursor);
    }
    static uint64_t shadow_cursor(const gba::GbaAudio& audio) {
        return audio.shadow_cursor_;
    }

    static void append_canonical_taps(gba::GbaAudio& audio, uint32_t count,
                                      uint32_t sample_rate = 32768) {
        gba::GbaAudio::CapSample tap{};
        tap.canonical_valid = true;
        tap.sample_rate = sample_rate;
        tap.direct_route.a_left = true;
        tap.direct_route.a_right = true;
        tap.direct_route.b_left = true;
        tap.direct_route.b_right = true;
        tap.direct_route.soundbias = 0x0200;
        for (uint32_t i = 0; i < count; ++i) {
            audio.cap_ring_[audio.samples_generated_ %
                            gba::GbaAudio::kCapRingSize] = tap;
            ++audio.samples_generated_;
        }
    }

    static void test_verifier_short_successor() {
        // Synthetic verifier seam: a native-short block must remain unjudged
        // and unpadded, while the next complete block can still establish
        // alignment and publish normally.
        gba::Mp2kShadow shadow;
        prepare(shadow, 10, 13379, 0, false);
        shadow.spv_ = 8;
        shadow.reset_producer_block(10, 0);
        shadow.producer_a_.assign(8, 12);
        shadow.producer_b_.assign(8, -9);
        shadow.native_block_r_.assign(7, 12.0f / 128.0f);
        shadow.native_block_l_.assign(7, -9.0f / 128.0f);
        shadow.judge_producer_block(10);
        CHECK(shadow.native_block_r_.size() == 7);
        CHECK(shadow.native_block_l_.size() == 7);
        CHECK(shadow.producer_blocks_judged_ == 0);
        CHECK(shadow.producer_blocks_passed_ == 0);

        shadow.reset_producer_block(11, 8);
        shadow.producer_route_addr_[0] = 0x02001000u;
        shadow.producer_route_addr_[1] = 0x02002000u;
        shadow.reverb_block_start_[0] = 0x02001000u;
        shadow.reverb_block_start_[1] = 0x02002000u;
        shadow.producer_a_.assign(8, 12);
        shadow.producer_b_.assign(8, -9);
        shadow.native_block_r_.assign(8, 12.0f / 128.0f);
        shadow.native_block_l_.assign(8, -9.0f / 128.0f);
        shadow.judge_producer_block(11);
        CHECK(shadow.producer_blocks_judged_ == 1);
        CHECK(shadow.producer_blocks_passed_ == 1);
        CHECK(shadow.published_block_id_ == 11);
    }

    static void test_writer_pool_isolation() {
        // Synthetic writer seam: fail-open pre-ownership stores must not
        // consume capacity reserved for every evidenced non-C70 writer.
        std::vector<uint8_t> rom(0x100, 0x40);
        std::vector<uint8_t> ewram(0x1000, 0);
        std::vector<uint8_t> iwram(0x8000, 0);
        gba::GbaAudio audio;
        init_pending_scheduler(audio, rom, ewram, iwram, 352);
        reset_shadow_runtime(audio);
        for (uint32_t i = 0; i < 24; ++i) {
            audio.mp2k_pcm_write_hook(
                0x03001000u + i * 4u, 0x02000020u + i * 4u, 0, 4,
                0, 0, 10u + i);
        }
        CHECK(non_c70_writer_count(audio) ==
              kMp2kKnownNonC70WriterPcs.size());

        // Acquire ownership after the unrelated burst. Reverb state is
        // synthetic and numeric; no protected bytes are used.
        ewram[(0x02000100u & 0x3FFFFu) + 0x05] = 50;
        audio.mp2k_frame_hook(0x08000101u, 100);
        const uint32_t pcm_base = 0x02000100u + 0x350u;
        const uint32_t dry_b = 0x02000100u + 0x410u;
        const uint32_t dry_a = 0x02000B40u;
        auto write_block = [&](uint32_t pc, uint32_t base,
                               uint64_t cycles) {
            for (uint32_t i = 0; i < 88; ++i) {
                audio.mp2k_pcm_write_hook(pc, base + i * 4u, 0, 4,
                                          0, 0, cycles + i);
            }
        };
        write_block(0x030008B4u, pcm_base, 200);
        write_block(0x03000A8Cu, pcm_base, 400);
        write_block(0x03000BCCu, dry_b, 600);
        write_block(0x03000BD4u, dry_a, 800);
        CHECK(!audio.shadow_.canonical_fallback());
        CHECK(audio.shadow_.reverb_block_start_[0] == dry_a);
        CHECK(audio.shadow_.reverb_block_start_[1] == dry_b);
        CHECK(audio.shadow_.guest_reverb_new_words_[0].size() == 88);
        CHECK(audio.shadow_.guest_reverb_new_words_[1].size() == 88);
        CHECK(non_c70_writer_count(audio) ==
              kMp2kKnownNonC70WriterPcs.size());
        for (std::size_t i = 0; i < kMp2kKnownNonC70WriterPcs.size(); ++i)
            CHECK(writer_block_bytes(audio, i) == 0);

        // C70 remains dedicated and captures one complete guest A/B block.
        for (uint32_t i = 0; i < 352; ++i) {
            audio.mp2k_pcm_write_hook(
                0x03000C70u, pcm_base + i * 4u, 0, 4, 0, 0, 1000u + i);
        }
        CHECK(!audio.shadow_.canonical_fallback());
        CHECK(c70_writer_completions(audio) == 1);
        CHECK(guest_producer_a_size(audio) == 352);
        CHECK(guest_producer_b_size(audio) == 352);
    }
};
}  // namespace gba
using gba::GbaAudioTurboFixture;

void test_mp2k_verifier_short_successor() {
    GbaAudioTurboFixture::test_verifier_short_successor();
}

void test_mp2k_writer_pool_isolation() {
    GbaAudioTurboFixture::test_writer_pool_isolation();
}

void test_mp2k_canonical_composition() {
    gba::GbaAudio::CapSample tap{};
    tap.canonical_valid = true;
    tap.sample_rate = 32768;
    tap.psg_left_input = 37;
    tap.psg_right_input = -29;
    tap.direct_route.a_left = true;
    tap.direct_route.a_right = true;
    tap.direct_route.b_left = true;
    tap.direct_route.b_right = true;
    tap.direct_route.soundbias = 0x0200;

    int16_t left = 0;
    int16_t right = 0;
    CHECK(gba::GbaAudio::compose_mp2k_with_canonical(
        0.25f, -0.125f, tap, false, left, right));
    const auto expected = gba::mix_direct_sound_output(
        0.25f, -0.125f, tap.psg_left_input, tap.psg_right_input,
        tap.direct_route);
    CHECK(left == static_cast<int16_t>(std::lround(expected.left)));
    CHECK(right == static_cast<int16_t>(std::lround(expected.right)));

    // An independently evidenced FIFO can be added in the same hardware
    // domain. This mode is not selected by the runtime yet.
    tap.direct_a = 16;
    tap.direct_b = -24;
    CHECK(gba::GbaAudio::compose_mp2k_with_canonical(
        0.25f, -0.125f, tap, true, left, right));
    const auto expected_with_fifo = gba::mix_direct_sound_output(
        0.25f + static_cast<float>(tap.direct_a) / 128.0f,
        -0.125f + static_cast<float>(tap.direct_b) / 128.0f,
        tap.psg_left_input, tap.psg_right_input, tap.direct_route);
    CHECK(left == static_cast<int16_t>(std::lround(expected_with_fifo.left)));
    CHECK(right == static_cast<int16_t>(std::lround(expected_with_fifo.right)));

    // Silence is stable and missing/stale tap data fails closed.
    tap = gba::GbaAudio::CapSample{};
    tap.canonical_valid = true;
    tap.sample_rate = 32768;
    tap.direct_route.soundbias = 0x0200;
    CHECK(gba::GbaAudio::compose_mp2k_with_canonical(
        0.0f, 0.0f, tap, false, left, right));
    CHECK(left == 0 && right == 0);
    CHECK(!gba::GbaAudio::compose_mp2k_with_canonical(
        0.0f, 0.0f, gba::GbaAudio::CapSample{}, false, left, right));
}

void test_normal_speed_wall_candidate() {
    std::vector<uint8_t> rom(0x400, 0x40);
    gba::MemView mem{rom.data(), rom.size(), nullptr, 0, nullptr, 0};

    // Wall cadence advances with host time even when the guest sample cursor
    // is untouched. Ownership changes only after the first valid composition.
    gba::GbaAudio cadence;
    GbaAudioTurboFixture::arm_normal_wall(cadence, mem, 1, 32, 0);
    cadence.native_audio_service(0);
    CHECK(GbaAudioTurboFixture::native_wall_active(cadence));
    CHECK(GbaAudioTurboFixture::native_wall_frames(cadence) == 0);
    CHECK(!cadence.native_audio_live() && cadence.stereo_audio_requested());
    CHECK(std::string(cadence.native_audio_status_reason()) ==
          "native candidate awaiting canonical tap");
    GbaAudioTurboFixture::append_canonical_taps(cadence, 20000);
    cadence.native_audio_service(50'000'000);
    CHECK(GbaAudioTurboFixture::native_wall_frames(cadence) == 3276);
    CHECK(GbaAudioTurboFixture::native_wall_composed_frames(cadence) == 3276);
    CHECK(cadence.native_audio_live());
    CHECK(std::string(cadence.native_audio_status_reason()) == "live");
    CHECK(GbaAudioTurboFixture::native_ring_size(cadence) == 3276);
    CHECK(GbaAudioTurboFixture::drain_native(cadence, 1024) == 1024);
    CHECK(!GbaAudioTurboFixture::native_wall_failed(cadence));
    CHECK(GbaAudioTurboFixture::shadow_cursor(cadence) == 0);

    // Verified publications retain order until their guest timeline arrives.
    GbaAudioTurboFixture::publish_normal(cadence, 2, 32, 16384);
    CHECK(GbaAudioTurboFixture::native_wall_queue_size(cadence) == 1);
    GbaAudioTurboFixture::publish_normal(cadence, 3, 32, 32768);
    CHECK(GbaAudioTurboFixture::native_wall_queue_size(cadence) == 2);
    for (uint64_t wall_ns = 100'000'000; wall_ns <= 550'000'000;
         wall_ns += 50'000'000) {
        cadence.native_audio_service(wall_ns);
        GbaAudioTurboFixture::drain_native(cadence);
    }
    CHECK(GbaAudioTurboFixture::native_wall_queue_size(cadence) == 0);
    CHECK(GbaAudioTurboFixture::native_wall_frames(cadence) ==
          gba::frames_from_q32_seconds(
              gba::q32_seconds_from_nanoseconds(550'000'000),
              gba::Mp2kWallMixer::kCanonicalRenderRate));
    CHECK(!GbaAudioTurboFixture::native_wall_failed(cadence));

    // A publication that moves backward in guest time fails closed; it is
    // never silently reordered or skipped.
    gba::GbaAudio out_of_order;
    GbaAudioTurboFixture::arm_normal_wall(out_of_order, mem, 1, 32, 0);
    out_of_order.native_audio_service(0);
    GbaAudioTurboFixture::publish_normal(out_of_order, 2, 32, 16384);
    GbaAudioTurboFixture::publish_normal(out_of_order, 3, 32, 8192);
    CHECK(GbaAudioTurboFixture::native_wall_failed(out_of_order));
    CHECK(!GbaAudioTurboFixture::native_wall_active(out_of_order));
    CHECK(!out_of_order.native_audio_live());

    // Without the next verified seed block, wall rendering exhausts its
    // bounded producer seed and returns to canonical audio.
    gba::GbaAudio starved;
    GbaAudioTurboFixture::arm_normal_wall(starved, mem, 1, 32, 0);
    starved.native_audio_service(0);
    GbaAudioTurboFixture::append_canonical_taps(starved, 10000);
    starved.native_audio_service(50'000'000);
    GbaAudioTurboFixture::drain_native(starved);
    CHECK(!GbaAudioTurboFixture::native_wall_failed(starved));
    starved.native_audio_service(100'000'000);
    GbaAudioTurboFixture::drain_native(starved);
    starved.native_audio_service(150'000'000);
    GbaAudioTurboFixture::drain_native(starved);
    starved.native_audio_service(200'000'000);
    GbaAudioTurboFixture::drain_native(starved);
    CHECK(!GbaAudioTurboFixture::native_wall_failed(starved));
    starved.native_audio_service(250'000'000);
    GbaAudioTurboFixture::drain_native(starved);
    CHECK(!GbaAudioTurboFixture::native_wall_failed(starved));
    starved.native_audio_service(300'000'000);
    CHECK(GbaAudioTurboFixture::native_wall_failed(starved));
    CHECK(!GbaAudioTurboFixture::native_wall_active(starved));
    CHECK(!starved.native_audio_live());

    // A wall candidate without a retained canonical tap, or with a changed
    // canonical rate, fails closed before producing a composed frame.
    gba::GbaAudio missing_tap;
    GbaAudioTurboFixture::arm_normal_wall(missing_tap, mem, 1, 32, 0);
    missing_tap.native_audio_service(0);
    missing_tap.native_audio_service(50'000'000);
    CHECK(GbaAudioTurboFixture::native_wall_failed(missing_tap));

    gba::GbaAudio stale_tap;
    GbaAudioTurboFixture::arm_normal_wall(stale_tap, mem, 1, 32, 0);
    stale_tap.native_audio_service(0);
    GbaAudioTurboFixture::append_canonical_taps(stale_tap, 20000, 65536);
    stale_tap.native_audio_service(50'000'000);
    CHECK(GbaAudioTurboFixture::native_wall_failed(stale_tap));

    // Ownership failure returns to canonical immediately and a new verified
    // wall epoch can recover without replaying the old native ring.
    gba::GbaAudio recovery;
    GbaAudioTurboFixture::arm_normal_wall(recovery, mem, 1, 32, 0);
    recovery.native_audio_service(0);
    GbaAudioTurboFixture::append_canonical_taps(recovery, 20000);
    recovery.native_audio_service(50'000'000);
    CHECK(recovery.native_audio_live());
    CHECK(GbaAudioTurboFixture::native_ring_size(recovery) != 0);
    GbaAudioTurboFixture::fail_normal(recovery, "test ownership failure");
    CHECK(!recovery.native_audio_live());
    CHECK(std::string(recovery.native_audio_status_reason()) ==
          "test ownership failure");
    GbaAudioTurboFixture::reset_normal(recovery);
    GbaAudioTurboFixture::prepare_normal(recovery, 2, 32, 0);
    recovery.native_audio_service(0);
    GbaAudioTurboFixture::append_canonical_taps(recovery, 20000);
    recovery.native_audio_service(50'000'000);
    CHECK(recovery.native_audio_live());
    CHECK(GbaAudioTurboFixture::native_ring_size(recovery) == 3276);

    // A host backlog cannot overwrite old native frames; it fails closed.
    gba::GbaAudio backlog;
    GbaAudioTurboFixture::arm_normal_wall(backlog, mem, 1, 32, 0);
    backlog.native_audio_service(0);
    GbaAudioTurboFixture::append_canonical_taps(backlog, 20000);
    backlog.native_audio_service(50'000'000);
    backlog.native_audio_service(100'000'000);
    backlog.native_audio_service(150'000'000);
    backlog.native_audio_service(200'000'000);
    backlog.native_audio_service(250'000'000);
    CHECK(!backlog.native_audio_live());
    CHECK(std::string(backlog.native_audio_status_reason()) ==
          "native wall ring overflow");
}

void test_pending_mp2k_snapshot_scheduler() {
    std::vector<uint8_t> rom(0x100, 0x40);
    std::vector<uint8_t> ewram(0x1000, 0);
    std::vector<uint8_t> iwram(0x8000, 0);
    gba::GbaAudio audio;
    GbaAudioTurboFixture::init_pending_scheduler(audio, rom, ewram, iwram);
    const gba::MemView mem{rom.data(), rom.size(), ewram.data(), ewram.size(),
                           iwram.data(), iwram.size()};

    // Synthetic 352-sample seam: the control boundary arrives after a
    // 351-sample native tail, then the C70 writer completes the guest block.
    // This records the ordering without padding samples or changing gates.
    std::vector<uint8_t> ewram_short(0x1000, 0);
    std::vector<uint8_t> iwram_short(0x8000, 0);
    auto short_audio = std::make_unique<gba::GbaAudio>();
    GbaAudioTurboFixture::init_pending_scheduler(
        *short_audio, rom, ewram_short, iwram_short, 352);
    const gba::MemView short_mem{rom.data(), rom.size(),
                                 ewram_short.data(), ewram_short.size(),
                                 iwram_short.data(), iwram_short.size()};
    GbaAudioTurboFixture::render_pending_samples(*short_audio, short_mem, 1719);
    CHECK(GbaAudioTurboFixture::native_block_size(*short_audio) == 351);
    short_audio->mp2k_vblank_hook(1000);
    short_audio->mp2k_control_hook(0x03000828u, 0x03000828u, 1100, 0);
    CHECK(!GbaAudioTurboFixture::canonical_fallback(*short_audio));
    CHECK(GbaAudioTurboFixture::active_block_id(*short_audio) == 1);
    CHECK(GbaAudioTurboFixture::native_block_size(*short_audio) == 0);
    const uint32_t short_base = 0x02000100u + 0x350u;
    // Occupy every other recognized writer slot first. C70 must still acquire
    // state and publish the interleaved guest block as the fifth writer.
    short_audio->mp2k_pcm_write_hook(
        0x030008B4u, short_base, 0, 4, 0, 0, 1150);
    short_audio->mp2k_pcm_write_hook(
        0x03000A8Cu, short_base + 4u, 0, 4, 0, 0, 1151);
    short_audio->mp2k_pcm_write_hook(
        0x03000BCCu, 0x02000510u, 0, 4, 0, 0, 1152);
    short_audio->mp2k_pcm_write_hook(
        0x03000BD4u, 0x02000B40u, 0, 4, 0, 0, 1153);
    // A relevant but not-yet-recognized writer must not be able to consume
    // the dedicated C70 state.
    short_audio->mp2k_pcm_write_hook(
        0x03000D00u, short_base + 8u, 0, 4, 0, 0, 1154);
    CHECK(GbaAudioTurboFixture::non_c70_writer_count(*short_audio) ==
          gba::kMp2kKnownWriterPcs.size());
    CHECK(!GbaAudioTurboFixture::c70_writer_registered(*short_audio));
    for (uint32_t i = 0; i < 352; ++i) {
        short_audio->mp2k_pcm_write_hook(
            0x03000C70u, short_base + i * 4u, 0, 4, 0, 0, 1200u + i);
    }
    CHECK(!GbaAudioTurboFixture::canonical_fallback(*short_audio));
    CHECK(GbaAudioTurboFixture::c70_writer_calls(*short_audio) == 352);
    CHECK(GbaAudioTurboFixture::c70_writer_completions(*short_audio) == 1);
    CHECK(GbaAudioTurboFixture::mp2k_writer_count(*short_audio) ==
          gba::kMp2kKnownWriterPcs.size() + 1u);
    CHECK(GbaAudioTurboFixture::c70_writer_registered(*short_audio));
    CHECK(GbaAudioTurboFixture::guest_producer_a_size(*short_audio) == 352);
    CHECK(GbaAudioTurboFixture::guest_producer_b_size(*short_audio) == 352);
    CHECK(GbaAudioTurboFixture::pending_block_id(*short_audio) == 2);
    GbaAudioTurboFixture::render_pending_samples(*short_audio, short_mem, 1719);
    short_audio->mp2k_control_hook(0x03000828u, 0x03000828u, 3000, 0);
    CHECK(!GbaAudioTurboFixture::canonical_fallback(*short_audio));
    CHECK(GbaAudioTurboFixture::active_block_id(*short_audio) == 2);

    // 28/29 host samples leave the 8-sample producer block partial. Reserve
    // the next ID without throwing away the old native tail.
    GbaAudioTurboFixture::render_pending_samples(audio, mem, 29);
    CHECK(GbaAudioTurboFixture::native_block_size(audio) < 8);
    GbaAudioTurboFixture::write_pending_block(audio, mem, 100);
    CHECK(!GbaAudioTurboFixture::canonical_fallback(audio));
    CHECK(GbaAudioTurboFixture::active_block_id(audio) == 0);
    CHECK(GbaAudioTurboFixture::pending_block_id(audio) == 1);

    // Full host interval completes old native state before next snapshot.
    GbaAudioTurboFixture::render_pending_samples(audio, mem, 32);
    CHECK(GbaAudioTurboFixture::native_block_size(audio) >= 8);
    CHECK(!GbaAudioTurboFixture::canonical_fallback(audio));

    // First next writer consumes pending ID exactly once, then accepts data.
    audio.mp2k_pcm_write_hook(0x03000C70u, 0x02000450u, 0, 4,
                              0, 0, 200);
    CHECK(GbaAudioTurboFixture::active_block_id(audio) == 1);
    CHECK(GbaAudioTurboFixture::pending_block_id(audio) == UINT64_MAX);
    CHECK(GbaAudioTurboFixture::authoritative_snapshots(audio) == 2);
    CHECK(GbaAudioTurboFixture::c70_writer_block_bytes(audio) == 4);
    audio.mp2k_pcm_write_hook(0x03000C70u, 0x02000450u, 0, 4,
                              0, 0, 201);
    CHECK(GbaAudioTurboFixture::authoritative_snapshots(audio) == 2);

    // A real SoundMainRAM hook consumes the same pending ID, once.
    std::vector<uint8_t> ewram_frame(0x1000, 0);
    std::vector<uint8_t> iwram_frame(0x8000, 0);
    gba::GbaAudio frame_audio;
    GbaAudioTurboFixture::init_pending_scheduler(
        frame_audio, rom, ewram_frame, iwram_frame);
    const gba::MemView frame_mem{rom.data(), rom.size(), ewram_frame.data(),
                                 ewram_frame.size(), iwram_frame.data(),
                                 iwram_frame.size()};
    GbaAudioTurboFixture::reserve_pending(frame_audio, frame_mem, 300);
    frame_audio.mp2k_frame_hook(0x08000101u, 400);
    CHECK(GbaAudioTurboFixture::active_block_id(frame_audio) == 1);
    CHECK(GbaAudioTurboFixture::pending_block_id(frame_audio) == UINT64_MAX);
    CHECK(GbaAudioTurboFixture::authoritative_snapshots(frame_audio) == 2);
    frame_audio.mp2k_frame_hook(0x08000101u, 400);
    CHECK(GbaAudioTurboFixture::authoritative_snapshots(frame_audio) == 2);

    // The measured control boundary can consume the same pending ID too.
    std::vector<uint8_t> ewram_control(0x1000, 0);
    std::vector<uint8_t> iwram_control(0x8000, 0);
    gba::GbaAudio control_audio;
    GbaAudioTurboFixture::init_pending_scheduler(
        control_audio, rom, ewram_control, iwram_control);
    const gba::MemView control_mem{rom.data(), rom.size(),
                                   ewram_control.data(), ewram_control.size(),
                                   iwram_control.data(), iwram_control.size()};
    GbaAudioTurboFixture::reserve_pending(control_audio, control_mem, 600);
    control_audio.mp2k_control_hook(0x03000828u, 0x03000828u, 700, 0);
    CHECK(GbaAudioTurboFixture::active_block_id(control_audio) == 1);
    CHECK(GbaAudioTurboFixture::pending_block_id(control_audio) == UINT64_MAX);
    CHECK(GbaAudioTurboFixture::authoritative_snapshots(control_audio) == 2);

    // Deferred VBlank fallback reserves one boundary ID. Repeated VBlanks
    // before the measured control callback must not skip producer IDs.
    std::vector<uint8_t> ewram_deferred(0x1000, 0);
    std::vector<uint8_t> iwram_deferred(0x8000, 0);
    auto deferred_audio = std::make_unique<gba::GbaAudio>();
    GbaAudioTurboFixture::init_pending_scheduler(
        *deferred_audio, rom, ewram_deferred, iwram_deferred);
    for (uint64_t i = 0; i < 8; ++i)
        deferred_audio->mp2k_vblank_hook(800u + i);
    CHECK(GbaAudioTurboFixture::active_block_id(*deferred_audio) == 1);
    CHECK(!GbaAudioTurboFixture::canonical_fallback(*deferred_audio));
    deferred_audio->mp2k_control_hook(0x03000828u, 0x03000828u, 900, 0);
    CHECK(GbaAudioTurboFixture::active_block_id(*deferred_audio) == 1);
    // Startup realignment resets the shadow counters, then the control hook
    // records exactly one fresh snapshot for the reserved ID.
    CHECK(GbaAudioTurboFixture::authoritative_snapshots(*deferred_audio) == 1);
    CHECK(!GbaAudioTurboFixture::canonical_fallback(*deferred_audio));

    // VBlank fallback consumes pending without allocating another ID.
    std::vector<uint8_t> ewram_vblank(0x1000, 0);
    std::vector<uint8_t> iwram_vblank(0x8000, 0);
    gba::GbaAudio vblank_audio;
    GbaAudioTurboFixture::init_pending_scheduler(
        vblank_audio, rom, ewram_vblank, iwram_vblank);
    const gba::MemView vblank_mem{rom.data(), rom.size(),
                                  ewram_vblank.data(), ewram_vblank.size(),
                                  iwram_vblank.data(), iwram_vblank.size()};
    GbaAudioTurboFixture::reserve_pending(vblank_audio, vblank_mem, 800);
    GbaAudioTurboFixture::set_have_block_cycles(vblank_audio, false);
    vblank_audio.mp2k_vblank_hook(900);
    CHECK(GbaAudioTurboFixture::active_block_id(vblank_audio) == 1);
    CHECK(GbaAudioTurboFixture::pending_block_id(vblank_audio) == UINT64_MAX);
    CHECK(GbaAudioTurboFixture::authoritative_snapshots(vblank_audio) == 2);
    vblank_audio.mp2k_vblank_hook(900);
    CHECK(GbaAudioTurboFixture::authoritative_snapshots(vblank_audio) == 2);

    // Dry/reverb writer must consume the deferred ID before its history probe
    // writes state. The first store snapshots once, then remains partial.
    std::vector<uint8_t> ewram_dry(0x1000, 0);
    std::vector<uint8_t> iwram_dry(0x8000, 0);
    gba::GbaAudio dry_audio;
    GbaAudioTurboFixture::init_pending_scheduler(
        dry_audio, rom, ewram_dry, iwram_dry);
    const gba::MemView dry_mem{rom.data(), rom.size(), ewram_dry.data(),
                               ewram_dry.size(), iwram_dry.data(),
                               iwram_dry.size()};
    GbaAudioTurboFixture::reserve_pending(dry_audio, dry_mem, 1000);
    const uint32_t dry_addr = 0x02000100u + 0x410u;
    dry_audio.mp2k_pcm_write_hook(0x03000BCCu, dry_addr, 0, 4,
                                  0, 0, 1100);
    CHECK(!GbaAudioTurboFixture::canonical_fallback(dry_audio));
    CHECK(GbaAudioTurboFixture::active_block_id(dry_audio) == 1);
    CHECK(GbaAudioTurboFixture::pending_block_id(dry_audio) == UINT64_MAX);
    CHECK(GbaAudioTurboFixture::authoritative_snapshots(dry_audio) == 2);
    CHECK(GbaAudioTurboFixture::writer_block_bytes(dry_audio, 2) == 4);

    // A deferred ID cannot be consumed halfway through a producer block.
    std::vector<uint8_t> ewram_mid(0x1000, 0);
    std::vector<uint8_t> iwram_mid(0x8000, 0);
    gba::GbaAudio mid_audio;
    GbaAudioTurboFixture::init_pending_scheduler(
        mid_audio, rom, ewram_mid, iwram_mid);
    const gba::MemView mid_mem{rom.data(), rom.size(), ewram_mid.data(),
                               ewram_mid.size(), iwram_mid.data(),
                               iwram_mid.size()};
    GbaAudioTurboFixture::reserve_pending(mid_audio, mid_mem, 1200);
    GbaAudioTurboFixture::set_c70_writer_block_bytes(mid_audio, 4);
    mid_audio.mp2k_pcm_write_hook(0x03000C70u, 0x02000450u, 0, 4,
                                  0, 0, 1300);
    CHECK(GbaAudioTurboFixture::canonical_fallback(mid_audio));
    CHECK(GbaAudioTurboFixture::pending_block_id(mid_audio) == UINT64_MAX);
    const auto mid_active = GbaAudioTurboFixture::active_block_id(mid_audio);
    const auto mid_snapshots =
        GbaAudioTurboFixture::authoritative_snapshots(mid_audio);
    mid_audio.mp2k_pcm_write_hook(0x03000C70u, 0x02000450u, 0, 4,
                                  0, 0, 1301);
    GbaAudioTurboFixture::render_pending_samples(mid_audio, mid_mem, 4);
    CHECK(GbaAudioTurboFixture::active_block_id(mid_audio) == mid_active);
    CHECK(GbaAudioTurboFixture::authoritative_snapshots(mid_audio) ==
          mid_snapshots);

    // Reset clears a reserved ID and releases the fallback latch for a new
    // runtime epoch.
    GbaAudioTurboFixture::set_pending_block_id(mid_audio, 7);
    GbaAudioTurboFixture::reset_shadow_runtime(mid_audio);
    CHECK(GbaAudioTurboFixture::pending_block_id(mid_audio) == UINT64_MAX);
    CHECK(!GbaAudioTurboFixture::canonical_fallback(mid_audio));

    // Once fallback is latched, same-ID writes/render cannot revive state.
    std::vector<uint8_t> ewram_frozen(0x1000, 0);
    std::vector<uint8_t> iwram_frozen(0x8000, 0);
    gba::GbaAudio frozen_audio;
    GbaAudioTurboFixture::init_pending_scheduler(
        frozen_audio, rom, ewram_frozen, iwram_frozen);
    const gba::MemView frozen_mem{rom.data(), rom.size(), ewram_frozen.data(),
                                  ewram_frozen.size(), iwram_frozen.data(),
                                  iwram_frozen.size()};
    GbaAudioTurboFixture::reserve_pending(frozen_audio, frozen_mem, 1400);
    GbaAudioTurboFixture::force_canonical_fallback(
        frozen_audio, "test frozen state");
    GbaAudioTurboFixture::set_pending_block_id(frozen_audio, 1);
    const auto frozen_active =
        GbaAudioTurboFixture::active_block_id(frozen_audio);
    const auto frozen_snapshots =
        GbaAudioTurboFixture::authoritative_snapshots(frozen_audio);
    const auto frozen_native_size =
        GbaAudioTurboFixture::native_block_size(frozen_audio);
    frozen_audio.mp2k_pcm_write_hook(0x03000C70u, 0x02000450u, 0, 4,
                                     0, 0, 1500);
    GbaAudioTurboFixture::render_pending_samples(frozen_audio, frozen_mem, 4);
    CHECK(GbaAudioTurboFixture::pending_block_id(frozen_audio) == UINT64_MAX);
    CHECK(GbaAudioTurboFixture::active_block_id(frozen_audio) == frozen_active);
    CHECK(GbaAudioTurboFixture::authoritative_snapshots(frozen_audio) ==
          frozen_snapshots);
    CHECK(GbaAudioTurboFixture::native_block_size(frozen_audio) ==
          frozen_native_size);

    // Invalid pending order fails closed; it never silently skips IDs.
    std::vector<uint8_t> ewram_bad(0x1000, 0);
    std::vector<uint8_t> iwram_bad(0x8000, 0);
    gba::GbaAudio bad_audio;
    GbaAudioTurboFixture::init_pending_scheduler(
        bad_audio, rom, ewram_bad, iwram_bad);
    GbaAudioTurboFixture::set_pending_block_id(bad_audio, 2);
    bad_audio.mp2k_frame_hook(0x08000101u, 500);
    CHECK(GbaAudioTurboFixture::canonical_fallback(bad_audio));
}

void set_environment(const char* name, const char* value);

void test_turbo_audio_fallback_state() {
    set_environment("GBARECOMP_TURBO_AUDIO", "decoupled");
    set_environment("GBARECOMP_STRICT_STATIC", nullptr);

    gba::GbaAudio pending;
    pending.configure_shadow({}, nullptr, 0, nullptr, 0, nullptr, 0, false);
    CHECK(!pending.turbo_audio_enter(4.0f, false, 0));
    CHECK(pending.turbo_audio_pending());
    CHECK(!pending.turbo_audio_fallback_active());
    CHECK(!pending.turbo_audio_decoupled());
    pending.turbo_audio_exit(0);

    gba::GbaAudio unsupported;
    unsupported.configure_shadow({}, nullptr, 0, nullptr, 0,
                                 nullptr, 0, false);
    CHECK(!unsupported.turbo_audio_enter(4.0f, true, 0));
    CHECK(!unsupported.turbo_audio_pending());
    CHECK(unsupported.turbo_audio_fallback_active());
    CHECK(!unsupported.turbo_audio_decoupled());
    CHECK(!unsupported.turbo_audio_enter(4.0f, false, 0));
    CHECK(unsupported.turbo_audio_fallback_active());
    unsupported.turbo_audio_exit(0);
    CHECK(!unsupported.turbo_audio_fallback_active());
    // Release clears the fatal latch; the next Turbo edge may retry NotReady.
    CHECK(!unsupported.turbo_audio_enter(4.0f, false, 0));
    CHECK(unsupported.turbo_audio_pending());

    set_environment("GBARECOMP_TURBO_AUDIO", nullptr);
}

void set_environment(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

void test_strict_static_turbo_gate() {
    gba::GbaAudio audio;
    set_environment("GBARECOMP_TURBO_AUDIO", "decoupled");
    set_environment("GBARECOMP_STRICT_STATIC", nullptr);
    audio.configure_shadow({}, nullptr, 0, nullptr, 0, nullptr, 0, false);
    CHECK(audio.turbo_audio_decoupled_requested());

    set_environment("GBARECOMP_STRICT_STATIC", "1");
    audio.configure_shadow({}, nullptr, 0, nullptr, 0, nullptr, 0, false);
    CHECK(!audio.turbo_audio_decoupled_requested());

    set_environment("GBARECOMP_STRICT_STATIC", nullptr);
    set_environment("GBARECOMP_TURBO_AUDIO", nullptr);
}

int main() {
    test_mp2k_verifier_short_successor();
    test_mp2k_writer_pool_isolation();
    test_mp2k_canonical_composition();
    test_normal_speed_wall_candidate();
    test_pending_mp2k_snapshot_scheduler();
    test_strict_static_turbo_gate();
    test_turbo_audio_fallback_state();
    // GbaBus embeds the large canonical memory/audio state. Keep it off the
    // test stack so later heavyweight audio fixtures can coexist safely.
    auto bus = std::make_unique<gba::GbaBus>();
    auto& audio = bus->audio();
    audio.set_event_capture_enabled(true);

    // GbaIo must preserve the original access width/value. The nested byte
    // state-machine writes must not become extra capture events.
    bus->io().write8(0x065, 0x80);
    bus->io().write16(0x060, 0x1234);
    bus->io().write32(0x090, 0xA1B2C3D4);
    bus->io().write32(0x0A0, 0x11223344);
    CHECK(audio.event_capture().size() == 4);
    gba::AudioCaptureEvent event{};
    CHECK(audio.event_capture().pop(event) && event.width == 1 &&
           event.value == 0x80 && event.kind ==
               gba::AudioCaptureKind::PsgRegisterWrite);
    CHECK(audio.event_capture().pop(event) && event.width == 2 &&
           event.value == 0x1234);
    CHECK(audio.event_capture().pop(event) && event.width == 4 &&
           event.value == 0xA1B2C3D4 && event.kind ==
               gba::AudioCaptureKind::PsgWaveRamWrite);
    CHECK(audio.event_capture().pop(event) && event.width == 4 &&
           event.value == 0x11223344 && event.kind ==
               gba::AudioCaptureKind::DirectFifoCpuWord);

    // Reserved gaps do not become false audio events. Valid ranges still do.
    audio.reset_event_capture();
    bus->io().write8(0x066, 1);
    bus->io().write8(0x085, 1);
    bus->io().write32(0x086, 0x12345678);
    CHECK(audio.event_capture().empty());
    bus->io().write8(0x060, 1);
    CHECK(audio.event_capture().size() == 1);

    // FIFO labels preserve CPU then DMA ordering.
    audio.reset_event_capture();
    audio.write_io32(0x0A0, 0x01020304, 1);
    audio.sound_fifo_dma_word(0, 0x02000000, 2);
    audio.capture_sound_fifo_dma_word(0, 0x02000000, 0xAABBCCDD, 2);
    CHECK(audio.event_capture().pop(event) && event.kind ==
           gba::AudioCaptureKind::DirectFifoCpuWord);
    CHECK(audio.event_capture().pop(event) && event.kind ==
           gba::AudioCaptureKind::DirectFifoDmaWord);

    // Timer and byte-consume cadence are explicit events.
    audio.reset_event_capture();
    audio.write_io8(0x084, 0x80, 0);
    audio.write_io8(0x083, 0x03, 0);
    audio.write_io32(0x0A0, 0x44332211, 0);
    audio.timer_overflow(0, 10);
    bool saw_timer = false, saw_consume = false;
    while (audio.event_capture().pop(event)) {
        saw_timer |= event.kind == gba::AudioCaptureKind::DirectFifoTimer;
        saw_consume |= event.kind == gba::AudioCaptureKind::DirectFifoConsume;
    }
    CHECK(saw_timer && saw_consume);

    // Capture-disabled and capture-enabled canonical state/output agree.
    gba::GbaAudio canonical;
    gba::GbaAudio observed;
    observed.set_event_capture_enabled(true);
    auto drive = [](gba::GbaAudio& a) {
        a.write_io8(0x084, 0x80, 0);
        a.write_io8(0x083, 0x03, 0);
        a.write_io32(0x0A0, 0x44332211, 0);
        a.tick(4096);
    };
    drive(canonical);
    drive(observed);
    CHECK(canonical.sample_rate() == observed.sample_rate());
    CHECK(canonical.samples_generated() == observed.samples_generated());
    CHECK(canonical.debug_cycle_accumulator() ==
           observed.debug_cycle_accumulator());
    CHECK(canonical.debug_fifo_state(0).count ==
           observed.debug_fifo_state(0).count);
    std::array<int16_t, 64> canonical_samples{};
    std::array<int16_t, 64> observed_samples{};
    CHECK(canonical.drain_samples(canonical_samples.data(),
                                   canonical_samples.size()) ==
           observed.drain_samples(observed_samples.data(),
                                   observed_samples.size()));
    CHECK(canonical_samples == observed_samples);

    // Existing MP2K write observer calls pre and post. Only post, and only a
    // proven channel/control field, enters the capture queue.
    std::vector<uint8_t> iwram(0x8000, 0);
    const uint32_t sound_info = 0x03000100u;
    const std::size_t ptr = gba::kSoundInfoPtr & 0x7FFFu;
    iwram[ptr + 0] = static_cast<uint8_t>(sound_info);
    iwram[ptr + 1] = static_cast<uint8_t>(sound_info >> 8);
    iwram[ptr + 2] = static_cast<uint8_t>(sound_info >> 16);
    iwram[ptr + 3] = static_cast<uint8_t>(sound_info >> 24);
    audio.configure_shadow({}, nullptr, 0, nullptr, 0,
                           iwram.data(), iwram.size(), false, false);
    const uint32_t channel11 = sound_info + gba::kSoundChansOff +
        11u * gba::kSoundChanStride + 0x18u;
    const std::size_t before_mp2k = audio.event_capture().size();
    audio.mp2k_pcm_write_hook(0x030008B4u, channel11, 0x55, 4,
                              0, 0, 10, 0x11, 0, 1);
    CHECK(audio.event_capture().size() == before_mp2k);
    audio.mp2k_pcm_write_hook(0x030008B4u, channel11, 0x55, 4,
                              0, 0, 10, 0x11, 0, 0);
    CHECK(audio.event_capture().size() == before_mp2k + 1);
    CHECK(audio.event_capture().pop(event) && event.channel == 11 &&
           event.aux0 == 0x18);
    audio.mp2k_pcm_write_hook(0x030008B4u, channel11, 0x55, 4,
                              0, 0, 10, 0x11, 0, 0);
    CHECK(audio.event_capture().size() == before_mp2k + 1);

    // Real hook overflow is fatal even while classification remains unknown.
    audio.reset_event_capture();
    const uint64_t epoch = audio.event_capture().epoch();
    gba::AudioCaptureEvent filler{};
    filler.epoch = epoch;
    for (std::size_t i = 0; i < gba::AudioEventCapture::kCapacity; ++i) {
        filler.aux0 = static_cast<uint32_t>(i);
        CHECK(audio.event_capture().push(filler) ==
               gba::AudioCaptureResult::Accepted);
    }
    bus->io().write8(0x060, 1);
    CHECK(audio.event_capture().stats().fallback_required);

    const uint64_t old_epoch = audio.event_capture().epoch();
    audio.reset_event_capture();
    CHECK(audio.event_capture().epoch() != old_epoch);

    // Synthetic judge-publication seam -> GbaAudio callback -> bounded wall
    // queue. Three fixture blocks publish before wall service; cursors prove
    // ordered delivery. This is not live ROM/gameplay integration.
    std::vector<uint8_t> turbo_rom(0x400, 0x40);
    gba::MemView turbo_mem{turbo_rom.data(), turbo_rom.size(),
                           nullptr, 0, nullptr, 0};
    gba::GbaAudio turbo_audio;
    GbaAudioTurboFixture::arm(turbo_audio, turbo_mem, 1);
    GbaAudioTurboFixture::check_three(turbo_audio);

    // A synthetic verified 4x request enters wall audio; unsupported reverb
    // falls back instead of forcing host mute.
    gba::GbaAudio turbo_ready;
    CHECK(GbaAudioTurboFixture::enter_ready(turbo_ready, turbo_mem, 1));
    CHECK(turbo_ready.turbo_audio_decoupled());
    turbo_ready.turbo_audio_exit(0);
    gba::GbaAudio turbo_reverb;
    CHECK(!GbaAudioTurboFixture::enter_ready(
        turbo_reverb, turbo_mem, 1, true));
    CHECK(turbo_reverb.turbo_audio_fallback_active());
    CHECK(!turbo_reverb.turbo_audio_decoupled());
    return 0;
}
