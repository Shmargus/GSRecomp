#include "mp2k_wall_update_queue.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace gba {
namespace {

uint64_t hash_bytes(uint64_t hash, const uint8_t* bytes, uint32_t count) {
    if (!bytes) return hash ^ 0xBAD0'0000'0000'0001ull;
    for (uint32_t i = 0; i < count; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

}  // namespace

bool Mp2kWallUpdateQueue::AssetKey::operator==(
    const AssetKey& other) const {
    return kind == other.kind && hash == other.hash &&
           byte_count == other.byte_count && sample_count == other.sample_count &&
           loop_start == other.loop_start && looped == other.looped &&
           synth.kind == other.synth.kind && synth.base == other.synth.base &&
           synth.step == other.synth.step && synth.depth == other.synth.depth &&
           synth.initial_duty == other.synth.initial_duty;
}

uint64_t Mp2kWallUpdateQueue::asset_hash(
    const Mp2kWallAssetInput& input) {
    uint64_t hash = 1469598103934665603ull;
    hash ^= static_cast<uint8_t>(input.kind);
    hash *= 1099511628211ull;
    if (input.kind == Mp2kWallAssetKind::Synth && input.synth_payload) {
        hash = hash_bytes(hash, input.synth_payload, input.synth_payload_len);
    } else {
        hash = hash_bytes(hash, input.bytes, input.byte_count);
    }
    hash ^= input.sample_count;
    hash *= 1099511628211ull;
    hash ^= input.loop_start;
    hash *= 1099511628211ull;
    hash ^= input.looped ? 1u : 0u;
    return hash;
}

Mp2kWallUpdateQueue::AssetKey Mp2kWallUpdateQueue::make_key(
    const Mp2kWallAssetInput& input) {
    AssetKey key;
    key.kind = input.kind;
    key.hash = asset_hash(input);
    key.byte_count = input.byte_count;
    key.sample_count = input.sample_count;
    key.loop_start = input.loop_start;
    key.looped = input.looped;
    key.synth = input.synth;
    return key;
}

bool Mp2kWallUpdateQueue::same_voice(
    const Mp2kWallVoiceSnapshot& a, const Mp2kWallVoiceSnapshot& b) {
#define SAME(field) (a.field == b.field)
    return SAME(on) && SAME(ctype) && SAME(asset_index) &&
           SAME(asset_generation) && SAME(route) && SAME(pos_index) &&
           SAME(pos_frac) && SAME(step_q23) && SAME(size) &&
           SAME(loop_start) && SAME(looped) && SAME(compressed) &&
           SAME(reversed) && SAME(g0r) && SAME(g0l) && SAME(g1r) &&
           SAME(g1l) && SAME(env_now) && SAME(env_next) &&
           SAME(synth_kind) && SAME(synth_base) && SAME(synth_step) &&
           SAME(synth_depth) && SAME(synth_init_duty) &&
           SAME(synth_duty_pos) && SAME(synth_pos) &&
           SAME(synth_phase) && SAME(synth_step_render) &&
           SAME(synth_duty) && SAME(synth_duty_step);
#undef SAME
}

bool Mp2kWallUpdateQueue::same_global(const GuestState& a,
                                      const GuestState& b) {
    return a.pcm_rate == b.pcm_rate && a.reverb == b.reverb &&
           a.gain_ramp_frames == b.gain_ramp_frames &&
           a.route_a_gain == b.route_a_gain &&
           a.route_b_gain == b.route_b_gain;
}

bool Mp2kWallUpdateQueue::checked_add(uint64_t a, uint64_t b,
                                      uint64_t& out) {
    if (b > std::numeric_limits<uint64_t>::max() - a) return false;
    out = a + b;
    return true;
}

void Mp2kWallUpdateQueue::reset(uint64_t anchor_guest_cursor_q32) {
    events_.reset();
    head_ = 0;
    count_ = 0;
    anchor_guest_q32_ = anchor_guest_cursor_q32;
    next_event_sequence_ = 1;
    last_event_guest_q32_ = 0;
    have_last_event_ = false;
    previous_ = GuestState{};
    assets_ = {};
    consumed_guest_q32_ = anchor_guest_cursor_q32;
    queued_seed_words_ = 0;
    stats_ = Stats{};
}

bool Mp2kWallUpdateQueue::ensure_storage() {
    if (events_) return true;
    try {
        events_ = std::unique_ptr<Event[]>(new (std::nothrow) Event[kCapacity]);
    } catch (...) {
        events_.reset();
    }
    if (!events_) {
        fatal_overflow();
        return false;
    }
    return true;
}

double Mp2kWallUpdateQueue::estimated_remaining_seconds() const {
    return static_cast<double>(stats_.estimated_remaining_q32) /
           static_cast<double>(uint64_t{1} << 32);
}

void Mp2kWallUpdateQueue::set_previous(
    const Mp2kWallSnapshotInput& snapshot,
    const std::array<Mp2kWallVoiceSnapshot, Mp2kWallMixer::kMaxVoices>& voices) {
    previous_.valid = true;
    previous_.source_sequence = snapshot.sequence;
    previous_.guest_cursor_q32 = snapshot.guest_cursor_q32;
    previous_.pcm_rate = snapshot.pcm_rate;
    previous_.reverb = snapshot.reverb;
    previous_.gain_ramp_frames = snapshot.gain_ramp_frames;
    previous_.route_a_gain = snapshot.route_a_gain;
    previous_.route_b_gain = snapshot.route_b_gain;
    previous_.voices = voices;
}

bool Mp2kWallUpdateQueue::prime(const Mp2kWallSnapshotInput& snapshot,
                                const Mp2kWallMixer& mixer) {
    reset(snapshot.guest_cursor_q32);
    if (snapshot.sequence == UINT64_MAX || snapshot.asset_count >
        Mp2kWallMixer::kMaxAssets)
        return false;
    if (!ensure_storage()) return false;
    // Mixer update sequences continue the accepted snapshot sequence. Runtime
    // uses sequence zero for the normalized prime; focused callers may retain
    // their source sequence.
    next_event_sequence_ = snapshot.sequence + 1u;
    for (uint8_t i = 0; i < snapshot.asset_count; ++i) {
        AssetRecord& record = assets_[i];
        record.valid = true;
        record.key = make_key(snapshot.assets[i]);
        record.handle = mixer.asset_handle(i);
        if (!mixer.asset_valid(record.handle)) return false;
    }
    std::array<Mp2kWallVoiceSnapshot, Mp2kWallMixer::kMaxVoices> voices =
        snapshot.voices;
    for (auto& voice : voices) {
        if (!voice.on) continue;
        if (voice.asset_index >= snapshot.asset_count) return false;
        const AssetRecord& record = assets_[voice.asset_index];
        voice.asset_index = record.handle.slot;
        voice.asset_generation = record.handle.generation;
    }
    set_previous(snapshot, voices);
    return true;
}

bool Mp2kWallUpdateQueue::resolve_assets(
    const Mp2kWallSnapshotInput& snapshot, Mp2kWallMixer& mixer,
    std::array<Mp2kWallAssetHandle, Mp2kWallMixer::kMaxAssets>& handles) {
    handles = {};
    if (snapshot.asset_count > Mp2kWallMixer::kMaxAssets) return false;
    for (uint8_t i = 0; i < snapshot.asset_count; ++i) {
        const AssetKey key = make_key(snapshot.assets[i]);
        bool found = false;
        for (const auto& record : assets_) {
            if (record.valid && record.key == key &&
                mixer.asset_valid(record.handle)) {
                handles[i] = record.handle;
                found = true;
                break;
            }
        }
        if (found) continue;
        Mp2kWallAssetHandle handle{};
        const Mp2kWallCaptureResult result =
            mixer.stage_asset(snapshot.assets[i], handle);
        if (result != Mp2kWallCaptureResult::Accepted) {
            if (result == Mp2kWallCaptureResult::AssetOverflow)
                ++stats_.asset_overflows;
            return false;
        }
        std::size_t slot = 0;
        while (slot < assets_.size() && assets_[slot].valid) ++slot;
        if (slot == assets_.size()) {
            ++stats_.asset_overflows;
            return false;
        }
        assets_[slot] = {true, key, handle};
        handles[i] = handle;
    }
    return true;
}

bool Mp2kWallUpdateQueue::next_sequence(uint64_t& sequence) {
    if (next_event_sequence_ == UINT64_MAX) {
        fatal_overflow();
        return false;
    }
    sequence = next_event_sequence_++;
    return true;
}

void Mp2kWallUpdateQueue::fatal_overflow() {
    ++stats_.overflow;
    stats_.fallback_required = true;
}

bool Mp2kWallUpdateQueue::append_event(Event event) {
    if (!events_) {
        fatal_overflow();
        return false;
    }
    if (count_ == kCapacity) {
        fatal_overflow();
        return false;
    }
    if (have_last_event_ && event.guest_cursor_q32 < last_event_guest_q32_) {
        ++stats_.out_of_order;
        stats_.fallback_required = true;
        return false;
    }
    if (queued_seed_words_ > kMaxQueuedSeedWords ||
        event.seed_words.size() >
            kMaxQueuedSeedWords - queued_seed_words_) {
        fatal_overflow();
        return false;
    }
    const uint64_t event_cursor = event.guest_cursor_q32;
    const std::size_t seed_words = event.seed_words.size();
    const std::size_t tail = (head_ + count_) % kCapacity;
    events_[tail] = std::move(event);
    ++count_;
    queued_seed_words_ += seed_words;
    ++stats_.queued;
    stats_.high_water = std::max<uint64_t>(stats_.high_water, count_);
    last_event_guest_q32_ = event_cursor;
    have_last_event_ = true;
    stats_.estimated_remaining_q32 = last_event_guest_q32_ >=
        consumed_guest_q32_ ? last_event_guest_q32_ - consumed_guest_q32_ : 0;
    return true;
}

Mp2kWallCaptureResult Mp2kWallUpdateQueue::enqueue_snapshot(
    const Mp2kWallSnapshotInput& snapshot, Mp2kWallMixer& mixer) {
    if (!previous_.valid) {
        ++stats_.out_of_order;
        stats_.fallback_required = true;
        return Mp2kWallCaptureResult::OutOfOrder;
    }
    if (snapshot.sequence == UINT64_MAX) {
        ++stats_.stale;
        stats_.fallback_required = true;
        return Mp2kWallCaptureResult::OutOfOrder;
    }
    if (snapshot.sequence <= previous_.source_sequence) {
        ++stats_.stale;
        return Mp2kWallCaptureResult::OutOfOrder;
    }
    if (snapshot.guest_cursor_q32 < previous_.guest_cursor_q32) {
        ++stats_.out_of_order;
        stats_.fallback_required = true;
        return Mp2kWallCaptureResult::OutOfOrder;
    }
    // Capacity is a hard admission gate. Check it before resolving/staging
    // assets so a full queue cannot mutate the mixer epoch or arena before
    // failing closed.
    if (count_ == kCapacity) {
        fatal_overflow();
        return Mp2kWallCaptureResult::AssetOverflow;
    }
    std::array<Mp2kWallAssetHandle, Mp2kWallMixer::kMaxAssets> handles{};
    if (!resolve_assets(snapshot, mixer, handles)) {
        stats_.fallback_required = true;
        return Mp2kWallCaptureResult::AssetOverflow;
    }
    std::array<Mp2kWallVoiceSnapshot, Mp2kWallMixer::kMaxVoices> voices =
        snapshot.voices;
    for (auto& voice : voices) {
        if (!voice.on) continue;
        if (voice.asset_index >= snapshot.asset_count) {
            stats_.fallback_required = true;
            return Mp2kWallCaptureResult::StaleAsset;
        }
        voice.asset_index = handles[voice.asset_index].slot;
        voice.asset_generation = handles[voice.asset_index].generation;
    }
    // Keep one coherent event per verified producer block, even when the
    // voice image is unchanged: positions/gains remain tied to guest time and
    // intermediate publication IDs cannot disappear from the wall timeline.
    if (!ensure_storage() || count_ == kCapacity) {
        fatal_overflow();
        return Mp2kWallCaptureResult::AssetOverflow;
    }
    const GuestState current{true, snapshot.sequence,
                             snapshot.guest_cursor_q32, snapshot.pcm_rate,
                             snapshot.reverb, snapshot.gain_ramp_frames,
                             snapshot.route_a_gain, snapshot.route_b_gain,
                             voices};
    Event event{};
    if (!next_sequence(event.sequence)) return Mp2kWallCaptureResult::OutOfOrder;
    event.source_sequence = snapshot.sequence;
    event.guest_cursor_q32 = snapshot.guest_cursor_q32;
    event.pcm_rate = current.pcm_rate;
    event.reverb = current.reverb;
    event.gain_ramp_frames = current.gain_ramp_frames;
    event.route_a_gain = current.route_a_gain;
    event.route_b_gain = current.route_b_gain;
    event.voices = voices;
    if (snapshot.seed_valid) {
        if (snapshot.seed_word_count == 0 ||
            snapshot.seed_word_count > Mp2kWallMixer::kMaxProducerFrames ||
            snapshot.producer_block_frames == 0 ||
            snapshot.seed_word_count != snapshot.producer_block_frames) {
            stats_.fallback_required = true;
            return Mp2kWallCaptureResult::InvalidInput;
        }
        try {
            event.seed_words.assign(
                snapshot.seed_words.begin(),
                snapshot.seed_words.begin() + snapshot.seed_word_count);
        } catch (...) {
            stats_.fallback_required = true;
            return Mp2kWallCaptureResult::AssetOverflow;
        }
        event.producer_block_frames = snapshot.producer_block_frames;
        event.has_seed_block = true;
    }
    event.has_pcm_rate = previous_.pcm_rate != current.pcm_rate;
    event.has_route = previous_.route_a_gain != current.route_a_gain ||
                      previous_.route_b_gain != current.route_b_gain;
    event.has_reverb = previous_.reverb != current.reverb;
    event.has_gain_ramp = previous_.gain_ramp_frames !=
                          current.gain_ramp_frames;
    if (!append_event(event)) return Mp2kWallCaptureResult::OutOfOrder;
    set_previous(snapshot, voices);
    return Mp2kWallCaptureResult::Accepted;
}

bool Mp2kWallUpdateQueue::next_due_guest_cursor_q32(uint64_t& cursor) const {
    if (!events_ || count_ == 0) return false;
    cursor = events_[head_].guest_cursor_q32;
    return true;
}

bool Mp2kWallUpdateQueue::apply_due(Mp2kWallMixer& mixer,
                                    uint64_t due_guest_cursor_q32) {
    while (count_ != 0) {
        const Event& event = events_[head_];
        if (event.guest_cursor_q32 > due_guest_cursor_q32) break;
        Mp2kWallVoiceUpdate update{};
        update.sequence = event.sequence;
        update.guest_cursor_q32 = event.guest_cursor_q32;
        update.channel = Mp2kWallVoiceUpdate::kGlobal;
        update.has_full_snapshot = event.full_state;
        update.voices = event.voices;
        update.has_pcm_rate = event.has_pcm_rate;
        update.pcm_rate = event.pcm_rate;
        update.has_route = event.has_route;
        update.route_a_gain = event.route_a_gain;
        update.route_b_gain = event.route_b_gain;
        update.has_reverb = event.has_reverb;
        update.reverb = event.reverb;
        update.has_gain_ramp = event.has_gain_ramp;
        update.gain_ramp_frames = event.gain_ramp_frames;
        update.has_seed_block = event.has_seed_block;
        update.producer_block_frames = event.producer_block_frames;
        update.seed_word_count = static_cast<uint32_t>(event.seed_words.size());
        if (update.has_seed_block) {
            if (update.seed_word_count == 0 ||
                update.seed_word_count > Mp2kWallMixer::kMaxProducerFrames) {
                stats_.fallback_required = true;
                return false;
            }
            std::copy(event.seed_words.begin(), event.seed_words.end(),
                      update.seed_words.begin());
        }
        const Mp2kWallCaptureResult result = mixer.apply_voice_update(update);
        if (result != Mp2kWallCaptureResult::Accepted) {
            if (result == Mp2kWallCaptureResult::StaleAsset)
                ++stats_.stale;
            stats_.fallback_required = true;
            return false;
        }
        if (queued_seed_words_ < event.seed_words.size()) {
            stats_.fallback_required = true;
            return false;
        }
        head_ = (head_ + 1) % kCapacity;
        --count_;
        queued_seed_words_ -= event.seed_words.size();
        ++stats_.applied;
        consumed_guest_q32_ = event.guest_cursor_q32;
        stats_.estimated_remaining_q32 = count_ == 0 ? 0 :
            (last_event_guest_q32_ >= consumed_guest_q32_
                 ? last_event_guest_q32_ - consumed_guest_q32_ : 0);
    }
    return true;
}

}  // namespace gba
