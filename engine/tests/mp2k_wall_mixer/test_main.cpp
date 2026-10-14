#include "mp2k_wall_mixer.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>

using namespace gba;

namespace gba {
// Synthetic, ROM-free access seam for the adapter gate. It models the state
// that the real post-judge boundary publishes; no protected bytes are used.
struct Mp2kShadowWallFixture {
    static void prepare(Mp2kShadow& shadow, uint64_t sequence) {
        shadow.active_ = true;
        shadow.engaged_ = true;
        shadow.reverb_ = 0;
        shadow.render_rate_ = Mp2kWallMixer::kCanonicalRenderRate;
        shadow.pcm_freq_ = 13379;
        shadow.producer_samples_ = 8;
        shadow.env_span_ = 4;
        shadow.current_seed_words_.resize(shadow.producer_samples_);
        for (uint32_t i = 0; i < shadow.producer_samples_; ++i)
            shadow.current_seed_words_[i] = 0x00010000u * (i + 1u);
        shadow.producer_block_id_ = sequence;
        shadow.producer_block_start_cursor_ = 0;
        shadow.producer_route_addr_[0] = 0x02001000u;
        shadow.producer_route_addr_[1] = 0x02002000u;
        shadow.reverb_block_start_[0] = 0x02001000u;
        shadow.reverb_block_start_[1] = 0x02002000u;
        shadow.producer_a_.resize(8);
        shadow.producer_b_.resize(8);
        shadow.native_block_r_.resize(8);
        shadow.native_block_l_.resize(8);
        for (uint32_t i = 0; i < 8; ++i) {
            shadow.producer_a_[i] = static_cast<int8_t>(i * 3 - 10);
            shadow.producer_b_[i] = static_cast<int8_t>(20 - i * 2);
            shadow.native_block_r_[i] =
                static_cast<float>(shadow.producer_a_[i]) / 128.0f;
            shadow.native_block_l_[i] =
                static_cast<float>(shadow.producer_b_[i]) / 128.0f;
        }
        auto& voice = shadow.voices_[0];
        voice = Mp2kShadow::Voice{};
        voice.on = true;
        voice.ctype = 0;
        voice.data = 0x08000110u;
        voice.size = 65;
        voice.loop_start = 0;
        voice.looped = true;
        voice.step_q23 = kMp2kFracOne;
        voice.g0r = voice.g0l = voice.g1r = voice.g1l = 1.0f;
        shadow.vf_.set_signed_producer_gate(true, true, 1.0f, 1.0f);
        for (uint32_t i = 0; i < 131200; ++i) {
            const float sample = 0.45f * std::sin(
                static_cast<float>(i) * 0.037f);
            shadow.vf_.judge(sample, sample, sample, sample);
        }
        shadow.judge_producer_block(sequence);
    }
    static void make_compressed(Mp2kShadow& shadow, uint64_t sequence) {
        // Synthetic export/DPCM seam only; this is not end-to-end publication
        // proof. Reuse the prepared complete/proven vectors and publish the
        // requested synthetic sequence through the real judge gate.
        shadow.voices_[0].compressed = true;
        shadow.voices_[0].ctype = 0x20;
        shadow.voices_[0].size = 65;
        shadow.voices_[0].data = 0x08000110u;
        shadow.producer_block_id_ = sequence;
        shadow.judge_producer_block(sequence);
    }
    static void clear_seeds(Mp2kShadow& shadow) {
        shadow.current_seed_words_.clear();
    }
};

struct PublishedCallbackProbe {
    std::array<uint64_t, 3> sequences{};
    std::size_t count = 0;

    static void record(void* context, uint64_t sequence) {
        auto& probe = *static_cast<PublishedCallbackProbe*>(context);
        if (probe.count < probe.sequences.size())
            probe.sequences[probe.count] = sequence;
        ++probe.count;
    }
};
}  // namespace gba

namespace {

constexpr uint32_t kOne = kMp2kFracOne;

int fail(const char* expression, int line) {
    std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
    return 1;
}

#define CHECK(condition) \
    do { if (!(condition)) return fail(#condition, __LINE__); } while (false)

bool close_enough(float a, float b) {
    return std::fabs(a - b) < 1.0e-6f;
}

Mp2kWallSnapshotInput pcm_snapshot(const uint8_t* bytes, uint32_t byte_count,
                                   uint32_t samples, uint64_t sequence = 1,
                                   uint64_t cursor = 0) {
    Mp2kWallSnapshotInput input{};
    input.sequence = sequence;
    input.guest_cursor_q32 = cursor;
    input.render_rate = Mp2kWallMixer::kCanonicalRenderRate;
    input.pcm_rate = Mp2kWallMixer::kCanonicalRenderRate;
    input.asset_count = 1;
    input.assets[0].bytes = bytes;
    input.assets[0].byte_count = byte_count;
    input.assets[0].sample_count = samples;
    input.assets[0].looped = true;
    auto& voice = input.voices[0];
    voice.on = true;
    voice.asset_index = 0;
    voice.route = Mp2kWallRoute::Both;
    voice.size = samples;
    voice.looped = true;
    voice.step_q23 = kOne;
    voice.g0r = voice.g0l = voice.g1r = voice.g1l = 1.0f;
    return input;
}

float expected_bus(int32_t sample, uint32_t right = 4, uint32_t left = 4,
                   bool fix = false) {
    uint32_t acc = mp2k_packed_accumulate(
        0, fix ? mp2k_pack_fix_gain(right, left)
               : mp2k_pack_stereo_gain(right, left), sample);
    acc = mp2k_saturate_packed_lanes(acc);
    int8_t route_a = 0, route_b = 0;
    mp2k_extract_dry_sample(acc, route_a, route_b);
    return static_cast<float>(route_b) / 128.0f;
}

}  // namespace

int main() {
    const std::array<uint8_t, 4> pcm{0x40, 0x80, 0xC0, 0x00};
    Mp2kWallStereoChunk chunk{};

    // Canonical rate is fixed here; Enhanced correction belongs in host code.
    Mp2kWallMixer wrong_rate(32768);
    auto input = pcm_snapshot(pcm.data(), pcm.size(), 4);
    CHECK(wrong_rate.begin_snapshot(input) ==
          Mp2kWallCaptureResult::InvalidInput);

    Mp2kWallMixer mixer;
    auto invalid_sequence = input;
    invalid_sequence.sequence = UINT64_MAX;
    CHECK(mixer.begin_snapshot(invalid_sequence) ==
          Mp2kWallCaptureResult::InvalidInput);
    input.voices[0].pos_index = 1;
    input.voices[0].pos_frac = kOne / 2;
    // Independent PCM8 vector: (-128,-64) at half phase uses the guest x2
    // interpolation, yielding -192 Q8.  Q9 gain 4 packs as 0x00020002;
    // canonical packed accumulation gives 0xFE7FFE80: extraction yields
    // route A=-3 and route B=-4 (the signed lane carry is intentional).
    // Distinct route gains prove left/right mapping and host gain application.
    input.route_a_gain = 0.5f;
    input.route_b_gain = 0.25f;
    CHECK(mixer.begin_snapshot(input) == Mp2kWallCaptureResult::Accepted);
    CHECK(mixer.render(1, chunk) == 1);
    CHECK(close_enough(chunk.right[0], -3.0f / 256.0f));
    CHECK(close_enough(chunk.left[0], -1.0f / 128.0f));
    CHECK(std::isfinite(chunk.right[0]) && std::isfinite(chunk.left[0]));
    CHECK(mp2k_interpolate_s8(-128, 127, kOne / 2) == -1);

    // Loop and one-shot cursor boundaries use the exact Q23 helpers.
    input = pcm_snapshot(pcm.data(), pcm.size(), 4, 2, 1);
    input.assets[0].loop_start = input.voices[0].loop_start = 2;
    input.voices[0].pos_index = 3;
    CHECK(mixer.begin_snapshot(input) == Mp2kWallCaptureResult::Accepted);
    CHECK(mixer.render(2, chunk) == 2);
    // The packed-lane carry is route-specific: the loop sample is -2 on
    // route A (right) and -3 on route B (left).
    const std::array<float, 2> loop_expected_a{0.0f, -2.0f / 128.0f};
    const std::array<float, 2> loop_expected_b{0.0f, -3.0f / 128.0f};
    for (std::size_t i = 0; i < loop_expected_a.size(); ++i)
        CHECK(close_enough(chunk.left[i], loop_expected_b[i]) &&
              close_enough(chunk.right[i], loop_expected_a[i]));
    input = pcm_snapshot(pcm.data(), pcm.size(), 4, 3, 2);
    input.assets[0].looped = input.voices[0].looped = false;
    input.voices[0].pos_index = 3;
    CHECK(mixer.begin_snapshot(input) == Mp2kWallCaptureResult::Accepted);
    CHECK(mixer.render(2, chunk) == 2);
    CHECK(close_enough(chunk.left[1], 0.0f));

    // DPCM vector is independent of the decoder: initial 0 plus nibble 4
    // gives [0,16,32,48], then compressed playback doubles each sample.
    std::array<uint8_t, 66> dpcm{};
    dpcm[0] = 0x00;
    dpcm[33] = 0x00;
    for (std::size_t i = 1; i < 33; ++i) dpcm[i] = 0x44;
    for (std::size_t i = 34; i < dpcm.size(); ++i) dpcm[i] = 0x44;
    input = pcm_snapshot(dpcm.data(), 66, 65, 4, 3);
    input.assets[0].kind = Mp2kWallAssetKind::CompressedDpcm;
    input.voices[0].compressed = true;
    input.voices[0].ctype = 0x20;
    CHECK(mixer.begin_snapshot(input) == Mp2kWallCaptureResult::Accepted);
    CHECK(mixer.render(4, chunk) == 4);
    const std::array<float, 4> dpcm_expected{
        0.0f, 0.0f, 1.0f / 128.0f, 1.0f / 128.0f};
    for (std::size_t i = 0; i < dpcm_expected.size(); ++i)
        CHECK(close_enough(chunk.left[i], dpcm_expected[i]) &&
              close_enough(chunk.right[i], dpcm_expected[i]));
    CHECK(input.assets[0].byte_count ==
          ((static_cast<uint64_t>(input.assets[0].sample_count) + 63u) / 64u) * 33u);

    // Synth vectors use hardcoded PWM and saw trajectories.  Expected PCM is
    // not produced by calling the synth helper, so a helper regression cannot
    // make both sides agree.
    const std::array<uint8_t, 5> pwm_payload{0, 0x22, 3, 0x10, 0x80};
    Mp2kSynthParams synth{};
    CHECK(mp2k_decode_synth_payload(pwm_payload.data(), pwm_payload.size(),
                                    synth));
    input = {};
    input.sequence = 5;
    input.guest_cursor_q32 = 4;
    input.render_rate = Mp2kWallMixer::kCanonicalRenderRate;
    input.pcm_rate = Mp2kWallMixer::kCanonicalRenderRate;
    input.asset_count = 1;
    input.assets[0].kind = Mp2kWallAssetKind::Synth;
    input.assets[0].synth_payload = pwm_payload.data();
    input.assets[0].synth_payload_len = pwm_payload.size();
    auto& synth_voice = input.voices[0];
    synth_voice.on = true;
    synth_voice.asset_index = 0;
    synth_voice.synth_kind = synth.kind;
    synth_voice.synth_base = synth.base;
    synth_voice.synth_step = synth.step;
    synth_voice.synth_depth = synth.depth;
    synth_voice.synth_init_duty = synth.initial_duty;
    synth_voice.synth_phase = 0.0;
    synth_voice.synth_step_render = 0.25;
    synth_voice.synth_duty = 0.25f;
    synth_voice.synth_duty_step = 0.0f;
    synth_voice.g0r = synth_voice.g0l = 1.0f;
    CHECK(mixer.begin_snapshot(input) == Mp2kWallCaptureResult::Accepted);
    CHECK(mixer.render(4, chunk) == 4);
    const std::array<float, 4> pwm_expected_b{
        3.0f / 128.0f, -2.0f / 128.0f,
        -2.0f / 128.0f, -2.0f / 128.0f};
    const std::array<float, 4> pwm_expected_a{
        3.0f / 128.0f, -1.0f / 128.0f,
        -1.0f / 128.0f, -1.0f / 128.0f};
    for (std::size_t i = 0; i < pwm_expected_b.size(); ++i)
        CHECK(close_enough(chunk.left[i], pwm_expected_b[i]) &&
              close_enough(chunk.right[i], pwm_expected_a[i]));

    const std::array<uint8_t, 5> saw_payload{1, 0x22, 3, 0x10, 0x80};
    Mp2kSynthParams saw{};
    CHECK(mp2k_decode_synth_payload(saw_payload.data(), saw_payload.size(),
                                     saw));
    input.sequence = 6;
    input.assets[0].synth_payload = saw_payload.data();
    input.assets[0].synth_payload_len = saw_payload.size();
    auto& saw_voice = input.voices[0];
    saw_voice.synth_kind = saw.kind;
    saw_voice.synth_base = saw.base;
    saw_voice.synth_step = saw.step;
    saw_voice.synth_depth = saw.depth;
    saw_voice.synth_init_duty = saw.initial_duty;
    saw_voice.synth_phase = 0.0;
    saw_voice.synth_step_render = 0.25;
    saw_voice.synth_duty = 0.0f;
    saw_voice.synth_duty_step = 0.0f;
    CHECK(mixer.begin_snapshot(input) == Mp2kWallCaptureResult::Accepted);
    CHECK(mixer.render(4, chunk) == 4);
    // Selector 1 is the fixed-point saw. With phase .25/.5/.75/0 and an
    // initial synth position of zero, the independent operands are
    // [-64,-16,56,-84]. Packed-lane saturation yields route A/right
    // [-1,-1,0,-2] and route B/left [-2,-1,0,-2].
    const std::array<float, 4> saw_expected_b{
        -2.0f / 128.0f, -1.0f / 128.0f,
        0.0f, -2.0f / 128.0f};
    const std::array<float, 4> saw_expected_a{
        -1.0f / 128.0f, -1.0f / 128.0f,
        0.0f, -2.0f / 128.0f};
    for (std::size_t i = 0; i < saw_expected_b.size(); ++i)
        CHECK(close_enough(chunk.left[i], saw_expected_b[i]) &&
              close_enough(chunk.right[i], saw_expected_a[i]));

    // Packed vectors and saturation remain the canonical reference.
    CHECK(mp2k_pack_stereo_gain(35, 47) == 0x00180012u);
    CHECK(mp2k_packed_accumulate(0xE5BDED84u, 0x00180012u, -30) ==
          0xE2EDEB68u);
    CHECK(mp2k_pack_fix_gain(26, 26) == 0x001A001Au);
    CHECK(mp2k_saturate_packed_lanes(0xB13370F0u) == 0xC0333FF0u);
    const std::array<uint32_t, 4> accum{
        0xF584F2EAu, 0xF483F1C6u, 0xF3C9F127u, 0xF43CF1B5u};
    const auto finalized = mp2k_finalize_producer_group(
        accum, 0x4F565657u, 0x4D525254u);
    CHECK((finalized == std::array<uint32_t, 4>{
        0x084A0743u, 0x07EA06BBu, 0x07BC0693u, 0x06F90617u}));

    // Multiple voices use packed accumulation and exact lane saturation.
    const std::array<uint8_t, 1> loud{127};
    input = pcm_snapshot(loud.data(), 1, 1, 7, 4);
    for (std::size_t i = 1; i < 3; ++i) {
        input.assets[i] = input.assets[0];
        input.asset_count = static_cast<uint8_t>(i + 1);
        input.voices[i] = input.voices[0];
        input.voices[i].asset_index = static_cast<uint8_t>(i);
    }
    CHECK(mixer.begin_snapshot(input) == Mp2kWallCaptureResult::Accepted);
    CHECK(mixer.render(1, chunk) == 1);
    uint32_t saturated = 0;
    for (int i = 0; i < 3; ++i)
        saturated = mp2k_packed_accumulate(saturated,
            mp2k_pack_stereo_gain(4, 4), 254);
    saturated = mp2k_saturate_packed_lanes(saturated);
    int8_t a = 0, b = 0;
    mp2k_extract_dry_sample(saturated, a, b);
    CHECK(close_enough(chunk.left[0], static_cast<float>(b) / 128.0f));

    // g0 -> g1 interpolates per producer block, with no float clipping.
    input = pcm_snapshot(loud.data(), 1, 1, 8, 5);
    input.producer_block_frames = 4;
    input.voices[0].g0r = input.voices[0].g0l = 0.0f;
    input.voices[0].g1r = input.voices[0].g1l = 1.0f;
    CHECK(mixer.begin_snapshot(input) == Mp2kWallCaptureResult::Accepted);
    CHECK(mixer.render(4, chunk) == 4);
    for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t q = (4u * i) / 3u;
        CHECK(close_enough(chunk.left[i], expected_bus(254, q, q)));
    }
    CHECK(mixer.render(2, chunk) == 2);
    CHECK(close_enough(chunk.left[0], expected_bus(254, 4, 4)) &&
          close_enough(chunk.left[1], expected_bus(254, 4, 4)));

    // Stale/out-of-order updates latch fallback; acknowledgement is not a
    // fresh snapshot and therefore cannot restore wall output.
    Mp2kWallVoiceUpdate update{};
    update.sequence = 7;
    update.guest_cursor_q32 = 5;
    update.channel = 0;
    update.voice = input.voices[0];
    CHECK(mixer.apply_voice_update(update) ==
          Mp2kWallCaptureResult::OutOfOrder);
    CHECK(mixer.stats().out_of_order != 0 && mixer.render(1, chunk) == 1 &&
          close_enough(chunk.left[0], 0.0f));
    update.sequence = UINT64_MAX;
    CHECK(mixer.apply_voice_update(update) ==
          Mp2kWallCaptureResult::OutOfOrder);
    update.sequence = 10;
    update.guest_cursor_q32 = 7;
    CHECK(mixer.apply_voice_update(update) ==
          Mp2kWallCaptureResult::OutOfOrder);
    mixer.acknowledge_fallback();
    CHECK(mixer.render(1, chunk) == 1 && close_enough(chunk.left[0], 0.0f));
    update.sequence = 9;
    update.guest_cursor_q32 = 6;
    CHECK(mixer.apply_voice_update(update) == Mp2kWallCaptureResult::Accepted);
    CHECK(mixer.render(1, chunk) == 1);
    CHECK(close_enough(chunk.left[0], expected_bus(254, 0, 0)));

    // Single bounded arena: total bytes, not per-voice allocations.
    const std::array<uint8_t, 3> tiny{1, 2, 3};
    Mp2kWallMixer small(Mp2kWallMixer::kCanonicalRenderRate, 2);
    auto too_big = pcm_snapshot(tiny.data(), 3, 3, 1, 0);
    CHECK(small.begin_snapshot(too_big) ==
          Mp2kWallCaptureResult::AssetOverflow);
    Mp2kWallMixer no_arena(Mp2kWallMixer::kCanonicalRenderRate, 0);
    CHECK(no_arena.begin_snapshot(too_big) ==
          Mp2kWallCaptureResult::AssetOverflow);

    // Reset invalidates handles and permits a new sequence after epoch reset.
    auto handle = mixer.asset_handle(0);
    const uint64_t old_epoch = mixer.epoch();
    mixer.reset();
    CHECK(mixer.epoch() != old_epoch && !mixer.asset_valid(handle));

    // Four-times production step, one-times wall trajectory: no chunking
    // assumption, just the exact Q23 phase progression.
    const std::array<uint8_t, 8> timeline{0, 10, 20, 30, 40, 50, 60, 70};
    auto turbo_input = pcm_snapshot(timeline.data(), timeline.size(), 8, 1, 0);
    turbo_input.pcm_rate = Mp2kWallMixer::kCanonicalRenderRate * 4u;
    turbo_input.voices[0].step_q23 = mp2k_step_q23(
        turbo_input.pcm_rate, Mp2kWallMixer::kCanonicalRenderRate);
    Mp2kWallMixer timeline_mixer;
    CHECK(timeline_mixer.begin_snapshot(turbo_input) ==
          Mp2kWallCaptureResult::Accepted);
    CHECK(timeline_mixer.render(4, chunk) == 4);
    CHECK(timeline_mixer.wall_cursor_q32() == (uint64_t{4} << 16));

    // Four-times production updates, applied in order between wall samples,
    // follow the same trajectory as a one-times reference timeline.
    const std::array<uint8_t, 32> produced4x{
        0, 1, 2, 3, 40, 41, 42, 43, 80, 81, 82, 83,
        120, 121, 122, 123, 20, 21, 22, 23, 60, 61, 62, 63,
        100, 101, 102, 103, 10, 11, 12, 13};
    const std::array<uint8_t, 8> reference1x{
        0, 40, 80, 120, 20, 60, 100, 10};
    auto reference_input = pcm_snapshot(reference1x.data(),
                                         reference1x.size(), 8, 1, 0);
    auto produced_input = pcm_snapshot(produced4x.data(),
                                       produced4x.size(), 32, 1, 0);
    produced_input.pcm_rate = Mp2kWallMixer::kCanonicalRenderRate * 4u;
    produced_input.voices[0].step_q23 = mp2k_step_q23(
        produced_input.pcm_rate, Mp2kWallMixer::kCanonicalRenderRate);
    Mp2kWallMixer reference_mixer, produced_mixer;
    CHECK(reference_mixer.begin_snapshot(reference_input) ==
          Mp2kWallCaptureResult::Accepted);
    CHECK(produced_mixer.begin_snapshot(produced_input) ==
          Mp2kWallCaptureResult::Accepted);
    Mp2kWallStereoChunk ref_one{}, prod_one{};
    auto queued_update = Mp2kWallVoiceUpdate{};
    queued_update.channel = 0;
    queued_update.voice = produced_input.voices[0];
    const std::array<float, 8> trajectory_expected{
        0.0f, 1.0f / 128.0f, 2.0f / 128.0f, 3.0f / 128.0f,
        0.0f, 1.0f / 128.0f, 3.0f / 128.0f, 0.0f};
    for (uint64_t frame = 0; frame < 8; ++frame) {
        queued_update.sequence = frame + 2;
        queued_update.guest_cursor_q32 = (frame + 1) * 4;
        queued_update.voice.pos_index = static_cast<uint32_t>(frame * 4);
        CHECK(produced_mixer.apply_voice_update(queued_update) ==
              Mp2kWallCaptureResult::Accepted);
        CHECK(reference_mixer.render(1, ref_one) == 1 &&
              produced_mixer.render(1, prod_one) == 1);
        CHECK(close_enough(ref_one.left[0], trajectory_expected[frame]) &&
              close_enough(ref_one.right[0], trajectory_expected[frame]) &&
              close_enough(prod_one.left[0], trajectory_expected[frame]) &&
              close_enough(prod_one.right[0], trajectory_expected[frame]));
    }

    // Synthetic verified-block fixture: adapter requires live/proven,
    // post-judge publication, copies the asset, full seed block, and rate.
    std::array<uint8_t, 0x200> fixture_rom{};
    fixture_rom[0x110] = 0x40;
    fixture_rom[0x111] = 0x00;
    MemView fixture_mem{fixture_rom.data(), fixture_rom.size(), nullptr, 0,
                        nullptr, 0};
    Mp2kShadow shadow;
    Mp2kShadowWallFixture::prepare(shadow, 41);
    Mp2kWallSnapshotInput exported{};
    CHECK(shadow.export_wall_snapshot(fixture_mem, 41, 0, exported));
    CHECK(shadow.live() && exported.seed_word_count == 8 &&
          exported.seed_words[7] != exported.seed_words[3] &&
          exported.pcm_rate == 13379 && exported.gain_ramp_frames == 4);
    auto seed_only = exported;
    seed_only.asset_count = 0;
    seed_only.voices = {};
    seed_only.pcm_rate = Mp2kWallMixer::kCanonicalRenderRate;
    Mp2kWallMixer seed_mixer;
    CHECK(seed_mixer.begin_snapshot(seed_only) ==
          Mp2kWallCaptureResult::Accepted);
    Mp2kWallStereoChunk seed_chunk{};
    CHECK(seed_mixer.render(8, seed_chunk) == 8);
    int8_t seed_a = 0, seed_b = 0;
    mp2k_extract_dry_sample(
        mp2k_saturate_packed_lanes(exported.seed_words[7]), seed_a, seed_b);
    CHECK(close_enough(seed_chunk.left[7], static_cast<float>(seed_b) / 128.0f));
    CHECK(seed_mixer.render(1, seed_chunk) == 1 &&
          seed_mixer.stats().seed_exhaustions == 1 &&
          close_enough(seed_chunk.left[0], 0.0f));
    Mp2kShadow zero_seed_shadow;
    Mp2kShadowWallFixture::prepare(zero_seed_shadow, 60);
    Mp2kShadowWallFixture::clear_seeds(zero_seed_shadow);
    Mp2kWallSnapshotInput zero_seed_export{};
    CHECK(zero_seed_shadow.export_wall_snapshot(
              fixture_mem, 60, 0, zero_seed_export,
              Mp2kWallExportMode::ZeroSeedMp2kOnly));
    CHECK(!zero_seed_export.seed_valid &&
          zero_seed_export.seed_word_count == 0 &&
          zero_seed_export.producer_block_frames == 0);
    Mp2kWallVoiceUpdate next_seed{};
    next_seed.sequence = 42;
    next_seed.guest_cursor_q32 = 1;
    next_seed.channel = Mp2kWallVoiceUpdate::kGlobal;
    next_seed.has_seed_block = true;
    next_seed.producer_block_frames = 8;
    next_seed.seed_word_count = 8;
    next_seed.has_pcm_rate = true;
    next_seed.pcm_rate = Mp2kWallMixer::kCanonicalRenderRate;
    next_seed.has_gain_ramp = true;
    next_seed.gain_ramp_frames = 4;
    for (uint32_t i = 0; i < next_seed.seed_word_count; ++i)
        next_seed.seed_words[i] = 0x01000000u * (i + 1u);
    CHECK(seed_mixer.apply_voice_update(next_seed) ==
          Mp2kWallCaptureResult::Accepted);
    CHECK(seed_mixer.render(1, seed_chunk) == 1 &&
          !close_enough(seed_chunk.left[0], 0.0f));
    Mp2kWallMixer from_adapter, from_copy;
    CHECK(from_adapter.begin_snapshot_from_shadow(
        shadow, fixture_mem, 41, 0) == Mp2kWallCaptureResult::Accepted);
    CHECK(from_copy.begin_snapshot(exported) ==
          Mp2kWallCaptureResult::Accepted);
    fixture_rom[0x110] = 0;
    fixture_rom[0x111] = 0;
    Mp2kWallStereoChunk adapter_chunk{}, copy_chunk{};
    CHECK(from_adapter.render(2, adapter_chunk) == 2);
    CHECK(from_copy.render(2, copy_chunk) == 2);
    CHECK(adapter_chunk.left[0] == copy_chunk.left[0] &&
          adapter_chunk.left[1] == copy_chunk.left[1]);

    // Publication callback fires at the successful judge/DMA edge, once per
    // block, even when three guest publications happen before wall service.
    Mp2kShadow callback_shadow;
    PublishedCallbackProbe callback_probe;
    callback_shadow.set_published_block_callback(
        &PublishedCallbackProbe::record, &callback_probe);
    Mp2kShadowWallFixture::prepare(callback_shadow, 1);
    Mp2kShadowWallFixture::prepare(callback_shadow, 2);
    Mp2kShadowWallFixture::prepare(callback_shadow, 3);
    CHECK(callback_probe.count == 3 &&
          (callback_probe.sequences == std::array<uint64_t, 3>{1, 2, 3}));
    callback_shadow.set_published_block_callback(nullptr, nullptr);
    Mp2kShadowWallFixture::prepare(callback_shadow, 4);
    CHECK(callback_probe.count == 3);

    // Exported DPCM payload size is ceil(samples/64)*33, including a
    // non-aligned final block.
    Mp2kShadowWallFixture::make_compressed(shadow, 42);
    Mp2kWallSnapshotInput dpcm_export{};
    const bool dpcm_ok = shadow.export_wall_snapshot(fixture_mem, 42, 1,
                                                      dpcm_export);
    CHECK(dpcm_ok);
    CHECK(dpcm_export.assets[0].byte_count == 66 &&
          dpcm_export.assets[0].kind == Mp2kWallAssetKind::CompressedDpcm);

    // Unverified shadow cannot cross the adapter gate.
    Mp2kShadow unverified;
    Mp2kWallMixer blocked;
    CHECK(blocked.begin_snapshot_from_shadow(
        unverified, MemView{}, 1, 0) == Mp2kWallCaptureResult::Unsupported);
    CHECK(unverified.wall_export_not_verified() == 1);
    return 0;
}
