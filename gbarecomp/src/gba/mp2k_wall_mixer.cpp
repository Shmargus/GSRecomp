#include "mp2k_wall_mixer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>

namespace gba {
namespace {

bool checked_add(std::size_t a, std::size_t b, std::size_t limit,
                 std::size_t& out) {
    if (b > limit || a > limit - b) return false;
    out = a + b;
    return true;
}

int32_t lerp_q9(float from, float to, uint64_t frame,
                uint32_t block_frames) {
    const int32_t a = std::max<int32_t>(0, static_cast<int32_t>(
        std::lround(from * 4.0f)));
    const int32_t b = std::max<int32_t>(0, static_cast<int32_t>(
        std::lround(to * 4.0f)));
    if (block_frames <= 1) return a;
    const uint64_t p = std::min<uint64_t>(frame, block_frames - 1u);
    const uint64_t d = block_frames - 1;
    const int64_t delta = static_cast<int64_t>(b) - a;
    return static_cast<int32_t>(a + (delta * static_cast<int64_t>(p)) /
                                static_cast<int64_t>(d));
}

}  // namespace

Mp2kWallMixer::Mp2kWallMixer(uint32_t render_rate, std::size_t arena_bytes)
    : wall_rate_(render_rate),
      arena_bytes_(std::min(arena_bytes, kAssetArenaBytes)) {}

Mp2kWallMixer::~Mp2kWallMixer() = default;

bool Mp2kWallMixer::finite(float value) { return std::isfinite(value); }
bool Mp2kWallMixer::finite(double value) { return std::isfinite(value); }

bool Mp2kWallMixer::valid_kind(Mp2kWallAssetKind kind) {
    return kind == Mp2kWallAssetKind::Pcm8 ||
           kind == Mp2kWallAssetKind::CompressedDpcm ||
           kind == Mp2kWallAssetKind::Synth;
}

bool Mp2kWallMixer::valid_route(Mp2kWallRoute route) {
    return route == Mp2kWallRoute::RouteA ||
           route == Mp2kWallRoute::RouteB || route == Mp2kWallRoute::Both;
}

bool Mp2kWallMixer::validate_voice(const Mp2kWallVoiceSnapshot& voice) {
    if (!valid_route(voice.route) || !finite(voice.g0r) ||
        !finite(voice.g0l) || !finite(voice.g1r) || !finite(voice.g1l) ||
        !finite(voice.synth_phase) || !finite(voice.synth_step_render) ||
        !finite(voice.synth_duty) || !finite(voice.synth_duty_step))
        return false;
    if (voice.pos_frac > kMp2kFracMask || voice.pos_index > voice.size ||
        voice.synth_kind > 3 || voice.synth_phase < 0.0 ||
        voice.synth_phase >= 1.0 || voice.synth_step_render < 0.0 ||
        voice.reversed)
        return false;
    return true;
}

bool Mp2kWallMixer::validate_asset(const Mp2kWallAssetInput& input) const {
    if (!valid_kind(input.kind) || input.loop_start > input.sample_count ||
        (input.looped && input.loop_start == input.sample_count)) return false;
    if (input.kind == Mp2kWallAssetKind::Synth) {
        if (input.byte_count != 0 || input.sample_count != 0) return false;
        if (input.synth_payload) return input.synth_payload_len >= 5;
        return input.synth.kind >= 1 && input.synth.kind <= 3;
    }
    if (input.sample_count == 0 || !input.bytes) return false;
    if (input.kind == Mp2kWallAssetKind::Pcm8)
        return input.byte_count >= input.sample_count;
    const uint64_t blocks = (static_cast<uint64_t>(input.sample_count) + 63) / 64;
    return blocks <= (std::numeric_limits<uint32_t>::max() / 33u) &&
           input.byte_count >= blocks * 33u;
}

bool Mp2kWallMixer::ensure_arena() {
    if (arena_) return true;
    if (arena_bytes_ == 0) return true;
    try {
        arena_ = std::unique_ptr<uint8_t[]>(
            new (std::nothrow) uint8_t[arena_bytes_]);
    } catch (...) {
        arena_.reset();
    }
    return arena_ != nullptr;
}

Mp2kWallCaptureResult Mp2kWallMixer::fail(Mp2kWallCaptureResult result) {
    ++stats_.snapshots_rejected;
    switch (result) {
    case Mp2kWallCaptureResult::AssetOverflow: ++stats_.asset_overflows; break;
    case Mp2kWallCaptureResult::Unsupported:
    case Mp2kWallCaptureResult::StaleAsset: ++stats_.unsupported; break;
    case Mp2kWallCaptureResult::NonFinite: ++stats_.nonfinite; break;
    case Mp2kWallCaptureResult::OutOfOrder: ++stats_.out_of_order; break;
    default: break;
    }
    stats_.fallback_required = true;
    output_enabled_ = false;
    return result;
}

void Mp2kWallMixer::begin_epoch() {
    if (epoch_ != std::numeric_limits<uint64_t>::max()) ++epoch_;
    wall_cursor_q32_ = 0;
    wall_remainder_ = 0;
    pcm_rate_ = 0;
    producer_block_frames_ = 0;
    gain_ramp_frames_ = 0;
    rendered_in_snapshot_ = 0;
    route_a_gain_ = route_b_gain_ = 1.0f;
    seed_words_ = {};
    seed_word_count_ = 0;
    seed_valid_ = false;
    seed_block_wall_start_frame_ = 0;
    seed_block_guest_cursor_q32_ = 0;
    output_enabled_ = false;
    const std::size_t old_arena_used = arena_used_;
    arena_used_ = 0;
    if (arena_) std::fill_n(arena_.get(), old_arena_used, uint8_t{0});
    for (auto& asset : assets_) {
        const uint32_t old_generation = asset.generation;
        asset = AssetSlot{};
        asset.generation = old_generation == std::numeric_limits<uint32_t>::max()
            ? 1 : old_generation + 1;
    }
    for (auto& voice : voices_) voice = VoiceState{};
    stats_.active_voices = 0;
    stats_.pcm_rate = 0;
}

Mp2kWallCaptureResult Mp2kWallMixer::install_voice(
    uint8_t channel, const Mp2kWallVoiceSnapshot& voice, bool initial,
    uint64_t guest_cursor_q32) {
    if (channel >= kMaxVoices || !validate_voice(voice))
        return fail(Mp2kWallCaptureResult::InvalidVoice);
    if (voice.on) {
        if (voice.asset_index >= kMaxAssets ||
            !assets_[voice.asset_index].occupied)
            return fail(Mp2kWallCaptureResult::StaleAsset);
        const AssetSlot& asset = assets_[voice.asset_index];
        if (voice.asset_generation != 0 &&
            voice.asset_generation != asset.generation)
            return fail(Mp2kWallCaptureResult::StaleAsset);
        if (voice.size != asset.sample_count ||
            voice.loop_start != asset.loop_start || voice.looped != asset.looped)
            return fail(Mp2kWallCaptureResult::InvalidVoice);
        if (asset.kind == Mp2kWallAssetKind::Synth) {
            if (voice.synth_kind == 0 || voice.compressed ||
                voice.synth_kind != asset.synth.kind)
                return fail(Mp2kWallCaptureResult::InvalidVoice);
        } else if (voice.synth_kind != 0 ||
                   voice.compressed !=
                       (asset.kind == Mp2kWallAssetKind::CompressedDpcm)) {
            return fail(Mp2kWallCaptureResult::InvalidVoice);
        }
    }
    const bool preserve_dpcm = !initial && voices_[channel].snapshot.on &&
        voices_[channel].snapshot.asset_index == voice.asset_index;
    Mp2kWallVoiceSnapshot stored = voice;
    if (stored.on && stored.asset_generation == 0)
        stored.asset_generation = assets_[stored.asset_index].generation;
    voices_[channel].snapshot = stored;
    voices_[channel].ramp_frame = 0;
    voices_[channel].ramp_anchor_q32 = guest_cursor_q32;
    if (!preserve_dpcm) {
        voices_[channel].dpcm_block = UINT32_MAX;
        voices_[channel].dpcm_samples = {};
    }
    stats_.active_voices = 0;
    for (const auto& state : voices_)
        if (state.snapshot.on) ++stats_.active_voices;
    if (!initial) ++stats_.voice_updates;
    return Mp2kWallCaptureResult::Accepted;
}

Mp2kWallCaptureResult Mp2kWallMixer::begin_snapshot(
    const Mp2kWallSnapshotInput& input) {
    if (wall_rate_ != kCanonicalRenderRate ||
        input.render_rate != kCanonicalRenderRate || input.pcm_rate == 0 ||
        input.asset_count > kMaxAssets || input.sequence == UINT64_MAX ||
        input.seed_word_count > kMaxProducerFrames ||
        input.producer_block_frames > kMaxProducerFrames ||
        input.gain_ramp_frames > kMaxProducerFrames ||
        (input.seed_valid && (input.seed_word_count == 0 ||
                              input.seed_word_count !=
                                  input.producer_block_frames)))
        return fail(Mp2kWallCaptureResult::InvalidInput);
    if (have_timeline_ && (input.sequence <= last_sequence_ ||
                           input.guest_cursor_q32 < last_guest_cursor_q32_))
        return fail(Mp2kWallCaptureResult::OutOfOrder);
    if (!finite(input.route_a_gain) || !finite(input.route_b_gain))
        return fail(Mp2kWallCaptureResult::NonFinite);
    if (input.reverb != 0) return fail(Mp2kWallCaptureResult::Unsupported);
    for (std::size_t i = 0; i < input.asset_count; ++i) {
        if (!validate_asset(input.assets[i]))
            return fail(Mp2kWallCaptureResult::Unsupported);
        if (input.assets[i].byte_count > arena_bytes_)
            return fail(Mp2kWallCaptureResult::AssetOverflow);
    }
    if (input.asset_count != 0 && !ensure_arena())
        return fail(Mp2kWallCaptureResult::AssetOverflow);
    for (const auto& voice : input.voices) {
        if (voice.on && (voice.asset_index >= input.asset_count ||
                         !validate_voice(voice)))
            return fail(Mp2kWallCaptureResult::InvalidVoice);
    }

    begin_epoch();
    pcm_rate_ = input.pcm_rate;
    producer_block_frames_ = input.producer_block_frames;
    gain_ramp_frames_ = input.gain_ramp_frames != 0
        ? input.gain_ramp_frames : input.producer_block_frames;
    route_a_gain_ = input.route_a_gain;
    route_b_gain_ = input.route_b_gain;
    seed_words_ = input.seed_words;
    seed_word_count_ = input.seed_word_count;
    seed_valid_ = input.seed_valid && seed_word_count_ != 0;
    seed_block_wall_start_frame_ = 0;
    seed_block_guest_cursor_q32_ = input.guest_cursor_q32;
    stats_.pcm_rate = pcm_rate_;
    for (std::size_t i = 0; i < input.asset_count; ++i) {
        const auto& source = input.assets[i];
        auto& asset = assets_[i];
        asset.kind = source.kind;
        asset.byte_count = source.byte_count;
        asset.sample_count = source.sample_count;
        asset.loop_start = source.loop_start;
        asset.looped = source.looped;
        asset.offset = static_cast<uint32_t>(arena_used_);
        if (source.kind == Mp2kWallAssetKind::Synth && source.synth_payload) {
            if (!mp2k_decode_synth_payload(source.synth_payload,
                                            source.synth_payload_len,
                                            asset.synth))
                return fail(Mp2kWallCaptureResult::Unsupported);
        } else {
            asset.synth = source.synth;
        }
        std::size_t next = 0;
        if (!checked_add(arena_used_, source.byte_count, arena_bytes_, next) ||
            (source.byte_count != 0 && !arena_))
            return fail(Mp2kWallCaptureResult::AssetOverflow);
        if (source.byte_count != 0)
            std::copy_n(source.bytes, source.byte_count,
                        arena_.get() + arena_used_);
        arena_used_ = next;
        asset.occupied = true;
    }
    for (uint8_t channel = 0; channel < kMaxVoices; ++channel) {
        const auto result = install_voice(channel, input.voices[channel], true,
                                          input.guest_cursor_q32);
        if (result != Mp2kWallCaptureResult::Accepted) return result;
    }
    last_sequence_ = input.sequence;
    last_guest_cursor_q32_ = input.guest_cursor_q32;
    have_timeline_ = true;
    ++stats_.snapshots_accepted;
    stats_.fallback_required = false;
    output_enabled_ = true;
    return Mp2kWallCaptureResult::Accepted;
}

Mp2kWallCaptureResult Mp2kWallMixer::begin_snapshot_from_shadow(
    const Mp2kShadow& shadow, const MemView& mem, uint64_t sequence,
    uint64_t guest_cursor_q32) {
    Mp2kWallSnapshotInput input{};
    if (!shadow.export_wall_snapshot(mem, sequence, guest_cursor_q32, input))
        return fail(Mp2kWallCaptureResult::Unsupported);
    return begin_snapshot(input);
}

Mp2kWallCaptureResult Mp2kWallMixer::stage_asset(
    const Mp2kWallAssetInput& input, Mp2kWallAssetHandle& handle) {
    handle = {};
    if (!have_timeline_ || !validate_asset(input))
        return fail(Mp2kWallCaptureResult::Unsupported);
    if (input.byte_count > arena_bytes_ ||
        (input.byte_count != 0 && !ensure_arena()))
        return fail(Mp2kWallCaptureResult::AssetOverflow);
    uint8_t slot = 0xFFu;
    for (uint8_t i = 0; i < kMaxAssets; ++i) {
        if (!assets_[i].occupied) {
            slot = i;
            break;
        }
    }
    if (slot == 0xFFu) return fail(Mp2kWallCaptureResult::AssetOverflow);
    auto& asset = assets_[slot];
    asset.kind = input.kind;
    asset.byte_count = input.byte_count;
    asset.sample_count = input.sample_count;
    asset.loop_start = input.loop_start;
    asset.looped = input.looped;
    asset.offset = static_cast<uint32_t>(arena_used_);
    if (input.kind == Mp2kWallAssetKind::Synth && input.synth_payload) {
        if (!mp2k_decode_synth_payload(input.synth_payload,
                                       input.synth_payload_len,
                                       asset.synth))
            return fail(Mp2kWallCaptureResult::Unsupported);
    } else {
        asset.synth = input.synth;
    }
    std::size_t next = 0;
    if (!checked_add(arena_used_, input.byte_count, arena_bytes_, next))
        return fail(Mp2kWallCaptureResult::AssetOverflow);
    if (input.byte_count != 0)
        std::copy_n(input.bytes, input.byte_count, arena_.get() + arena_used_);
    arena_used_ = next;
    asset.occupied = true;
    handle = {epoch_, slot, asset.generation};
    ++stats_.assets_staged;
    return Mp2kWallCaptureResult::Accepted;
}

Mp2kWallCaptureResult Mp2kWallMixer::apply_voice_update(
    const Mp2kWallVoiceUpdate& update) {
    if (!have_timeline_ || update.sequence == UINT64_MAX ||
        update.sequence <= last_sequence_ ||
        (last_sequence_ != UINT64_MAX &&
         update.sequence != last_sequence_ + 1u) ||
        update.guest_cursor_q32 < last_guest_cursor_q32_)
        return fail(Mp2kWallCaptureResult::OutOfOrder);
    if (update.has_route && (!finite(update.route_a_gain) ||
                             !finite(update.route_b_gain)))
        return fail(Mp2kWallCaptureResult::NonFinite);
    if (update.has_reverb && update.reverb != 0)
        return fail(Mp2kWallCaptureResult::Unsupported);
    if (update.has_gain_ramp && update.gain_ramp_frames > kMaxProducerFrames)
        return fail(Mp2kWallCaptureResult::InvalidInput);
    if (update.has_seed_block) {
        if (update.channel != Mp2kWallVoiceUpdate::kGlobal ||
            update.guest_cursor_q32 <= last_guest_cursor_q32_ ||
            update.seed_word_count == 0 ||
            update.seed_word_count > kMaxProducerFrames ||
            update.producer_block_frames == 0 ||
            update.producer_block_frames > kMaxProducerFrames ||
            update.seed_word_count != update.producer_block_frames)
            return fail(Mp2kWallCaptureResult::InvalidInput);
    }
    if (update.has_full_snapshot &&
        update.channel != Mp2kWallVoiceUpdate::kGlobal)
        return fail(Mp2kWallCaptureResult::InvalidInput);
    if (update.has_full_snapshot) {
        for (uint8_t channel = 0; channel < kMaxVoices; ++channel) {
            const auto result = install_voice(channel, update.voices[channel],
                                              false,
                                              update.guest_cursor_q32);
            if (result != Mp2kWallCaptureResult::Accepted) return result;
        }
    }
    if (update.channel != Mp2kWallVoiceUpdate::kGlobal) {
        const auto result = install_voice(update.channel, update.voice, false,
                                          update.guest_cursor_q32);
        if (result != Mp2kWallCaptureResult::Accepted) return result;
    }
    if (update.has_pcm_rate) {
        if (update.pcm_rate == 0) return fail(Mp2kWallCaptureResult::InvalidInput);
        pcm_rate_ = update.pcm_rate;
        stats_.pcm_rate = pcm_rate_;
    }
    if (update.has_route) {
        route_a_gain_ = update.route_a_gain;
        route_b_gain_ = update.route_b_gain;
    }
    if (update.has_gain_ramp) gain_ramp_frames_ = update.gain_ramp_frames;
    if (update.has_seed_block) {
        seed_words_ = update.seed_words;
        seed_word_count_ = update.seed_word_count;
        seed_valid_ = true;
        producer_block_frames_ = update.producer_block_frames;
        seed_block_wall_start_frame_ = rendered_in_snapshot_;
        seed_block_guest_cursor_q32_ = update.guest_cursor_q32;
        output_enabled_ = true;
        stats_.fallback_required = false;
    }
    reset_ramps(update.guest_cursor_q32);
    last_sequence_ = update.sequence;
    last_guest_cursor_q32_ = update.guest_cursor_q32;
    return Mp2kWallCaptureResult::Accepted;
}

void Mp2kWallMixer::reset() {
    begin_epoch();
    have_timeline_ = false;
    last_sequence_ = 0;
    last_guest_cursor_q32_ = 0;
    ++stats_.resets;
}

Mp2kWallAssetHandle Mp2kWallMixer::asset_handle(uint8_t slot) const {
    if (slot >= kMaxAssets) return {};
    return {epoch_, slot, assets_[slot].generation};
}

bool Mp2kWallMixer::checked_asset(const Mp2kWallAssetHandle& handle) const {
    return handle.slot < kMaxAssets && handle.epoch == epoch_ &&
           assets_[handle.slot].occupied &&
           assets_[handle.slot].generation == handle.generation;
}

bool Mp2kWallMixer::asset_valid(const Mp2kWallAssetHandle& handle) const {
    return checked_asset(handle);
}

int32_t Mp2kWallMixer::fetch_s8(VoiceState& voice, const AssetSlot& asset,
                                uint32_t index) {
    if (index >= asset.sample_count || !arena_) return 0;
    const uint8_t* data = arena_.get() + asset.offset;
    if (asset.kind == Mp2kWallAssetKind::Pcm8)
        return static_cast<int8_t>(data[index]);
    const uint32_t block = index >> 6;
    if (voice.dpcm_block != block) {
        if (!mp2k_decode_dpcm_block(data + block * 33u, 33,
                                    voice.dpcm_samples)) {
            output_enabled_ = false;
            stats_.fallback_required = true;
            return 0;
        }
        voice.dpcm_block = block;
    }
    return voice.dpcm_samples[index & 63u];
}

int32_t Mp2kWallMixer::sample_voice_q8(VoiceState& state, bool& fix_pcm) {
    fix_pcm = false;
    auto& v = state.snapshot;
    if (!v.on || v.asset_index >= kMaxAssets ||
        !assets_[v.asset_index].occupied) return 0;
    const auto& asset = assets_[v.asset_index];
    if (asset.kind == Mp2kWallAssetKind::Synth) {
        const float sample = mp2k_synth_render_sample(
            v.synth_kind, v.synth_phase, v.synth_step_render, v.synth_pos,
            v.synth_duty, v.synth_duty_step);
        // Shadow's producer path uses the ordinary packed half-gain with a
        // doubled synth operand, exactly like compressed PCM.
        return static_cast<int32_t>(std::lround(sample * 128.0f)) * 2;
    }
    if (v.pos_index >= v.size) {
        if (!mp2k_advance_cursor(v.pos_index, v.pos_frac, 0, v.size,
                                 v.loop_start, v.looped)) {
            v.on = false;
            return 0;
        }
    }
    const uint32_t next = v.pos_index + 1u >= v.size
        ? (v.looped ? v.loop_start : v.pos_index) : v.pos_index + 1u;
    const int32_t s0 = fetch_s8(state, asset, v.pos_index);
    const int32_t s1 = fetch_s8(state, asset, next);
    fix_pcm = !v.compressed && !v.reversed && (v.ctype & 0x08u) != 0;
    const bool ordinary = !v.compressed && !fix_pcm;
    const int32_t sample = ordinary
        ? mp2k_interpolate_s8_x2(s0, s1, v.pos_frac)
        : mp2k_interpolate_s8(s0, s1, v.pos_frac);
    if (!mp2k_advance_cursor(v.pos_index, v.pos_frac, v.step_q23,
                             v.size, v.loop_start, v.looped))
        v.pos_index = v.size;
    return v.compressed ? sample * 2 : sample;
}

void Mp2kWallMixer::reset_ramps(uint64_t guest_cursor_q32) {
    for (auto& voice : voices_) {
        voice.ramp_frame = 0;
        voice.ramp_anchor_q32 = guest_cursor_q32;
    }
}

bool Mp2kWallMixer::seed_for_frame(uint32_t& word) {
    word = 0;
    if (!seed_valid_) return true;
    if (seed_word_count_ == 0 || rendered_in_snapshot_ <
        seed_block_wall_start_frame_)
        return false;
    const uint64_t elapsed = rendered_in_snapshot_ -
        seed_block_wall_start_frame_;
    if (pcm_rate_ != 0 &&
        elapsed > std::numeric_limits<uint64_t>::max() / pcm_rate_)
        return false;
    const uint64_t source_index = pcm_rate_ == 0 ? 0 :
        (elapsed * pcm_rate_) / wall_rate_;
    if (source_index >= seed_word_count_) return false;
    word = seed_words_[static_cast<std::size_t>(source_index)];
    return true;
}

uint32_t Mp2kWallMixer::packed_gain(const VoiceState& state,
                                    bool fix_pcm) const {
    const int32_t right = lerp_q9(state.snapshot.g0r, state.snapshot.g1r,
                                  state.ramp_frame, gain_ramp_frames_);
    const int32_t left = lerp_q9(state.snapshot.g0l, state.snapshot.g1l,
                                 state.ramp_frame, gain_ramp_frames_);
    return fix_pcm
        ? mp2k_pack_fix_gain(static_cast<uint32_t>(std::max(0, right)),
                             static_cast<uint32_t>(std::max(0, left)))
        : mp2k_pack_stereo_gain(static_cast<uint32_t>(std::max(0, right)),
                                static_cast<uint32_t>(std::max(0, left)));
}

uint32_t Mp2kWallMixer::render(uint32_t frames, Mp2kWallStereoChunk& out) {
    out.frame_count = 0;
    if (wall_rate_ != kCanonicalRenderRate) return 0;
    const uint32_t count = static_cast<uint32_t>(
        std::min<std::size_t>(frames, kMaxChunkFrames));
    const uint64_t numerator = static_cast<uint64_t>(count) * kQ32One;
    if (numerator > std::numeric_limits<uint64_t>::max() - wall_remainder_)
        return fail(Mp2kWallCaptureResult::InvalidInput), 0;
    const uint64_t total = wall_remainder_ + numerator;
    const uint64_t delta = total / wall_rate_;
    const uint64_t remainder = total % wall_rate_;
    if (wall_cursor_q32_ > std::numeric_limits<uint64_t>::max() - delta)
        return fail(Mp2kWallCaptureResult::InvalidInput), 0;
    for (uint32_t i = 0; i < count; ++i) {
        int8_t route_a = 0, route_b = 0;
        if (output_enabled_) {
            uint32_t accumulator = 0;
            if (!seed_for_frame(accumulator)) {
                ++stats_.seed_exhaustions;
                stats_.fallback_required = true;
                output_enabled_ = false;
            }
            if (output_enabled_) {
                for (auto& voice : voices_) {
                    if (!voice.snapshot.on) continue;
                    bool fix_pcm = false;
                    const int32_t sample = sample_voice_q8(voice, fix_pcm);
                    accumulator = mp2k_packed_accumulate(
                        accumulator, packed_gain(voice, fix_pcm), sample);
                    if (voice.ramp_frame != UINT32_MAX) ++voice.ramp_frame;
                }
                accumulator = mp2k_saturate_packed_lanes(accumulator);
                mp2k_extract_dry_sample(accumulator, route_a, route_b);
            }
        }
        out.right[i] = static_cast<float>(route_a) / 128.0f * route_a_gain_;
        out.left[i] = static_cast<float>(route_b) / 128.0f * route_b_gain_;
        if (out.left[i] == 0.0f && out.right[i] == 0.0f) ++stats_.silent_frames;
        ++rendered_in_snapshot_;
    }
    wall_cursor_q32_ += delta;
    wall_remainder_ = remainder;
    out.frame_count = count;
    stats_.rendered_frames += count;
    stats_.active_voices = 0;
    for (const auto& voice : voices_)
        if (voice.snapshot.on) ++stats_.active_voices;
    return count;
}

}  // namespace gba
