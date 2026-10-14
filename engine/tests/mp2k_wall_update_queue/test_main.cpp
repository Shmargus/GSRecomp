#include "mp2k_wall_update_queue.h"

#include <array>
#include <cstdint>
#include <cstdio>

using namespace gba;

namespace {

int fail(const char* expression, int line) {
    std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
    return 1;
}

#define CHECK(condition) \
    do { if (!(condition)) return fail(#condition, __LINE__); } while (false)

Mp2kWallSnapshotInput snapshot(const uint8_t* bytes, uint32_t count,
                               uint64_t sequence, uint64_t cursor) {
    Mp2kWallSnapshotInput input{};
    input.sequence = sequence;
    input.guest_cursor_q32 = cursor;
    input.render_rate = Mp2kWallMixer::kCanonicalRenderRate;
    input.pcm_rate = Mp2kWallMixer::kCanonicalRenderRate;
    input.asset_count = 1;
    input.assets[0].bytes = bytes;
    input.assets[0].byte_count = count;
    input.assets[0].sample_count = count;
    input.assets[0].looped = true;
    auto& voice = input.voices[0];
    voice.on = true;
    voice.asset_index = 0;
    voice.route = Mp2kWallRoute::Both;
    voice.size = count;
    voice.looped = true;
    voice.step_q23 = kMp2kFracOne;
    voice.g0r = voice.g0l = voice.g1r = voice.g1l = 1.0f;
    return input;
}

}  // namespace

int main() {
    const std::array<uint8_t, 2> pcm{127, 64};
    Mp2kWallMixer mixer;
    Mp2kWallUpdateQueue queue;
    auto first = snapshot(pcm.data(), pcm.size(), 1, 0);
    CHECK(mixer.begin_snapshot(first) == Mp2kWallCaptureResult::Accepted);
    CHECK(queue.prime(first, mixer));
    CHECK(queue.empty());

    // Zero-seed MP2K mode renders voices without producer PCM and does not
    // enter the producer-seed exhaustion path.
    Mp2kWallStereoChunk chunk{};
    CHECK(mixer.render(1, chunk) == 1);
    CHECK(mixer.stats().seed_exhaustions == 0);
    CHECK(chunk.left[0] != 0.0f || chunk.right[0] != 0.0f);

    // Full producer-seed mode carries the next verified accumulator block
    // through the guest-timeline queue, so a wall renderer can replace the
    // guest MP2K block without borrowing guest memory or exhausting its seed.
    std::array<uint32_t, 2> seed_words{0x01000000u, 0x02000000u};
    auto seeded_first = snapshot(pcm.data(), pcm.size(), 10, 0);
    seeded_first.seed_valid = true;
    seeded_first.seed_word_count = 2;
    seeded_first.producer_block_frames = 2;
    seeded_first.seed_words[0] = seed_words[0];
    seeded_first.seed_words[1] = seed_words[1];
    Mp2kWallMixer seeded_mixer;
    Mp2kWallUpdateQueue seeded_queue;
    CHECK(seeded_mixer.begin_snapshot(seeded_first) ==
          Mp2kWallCaptureResult::Accepted);
    CHECK(seeded_queue.prime(seeded_first, seeded_mixer));
    CHECK(seeded_mixer.render(2, chunk) == 2);
    CHECK(seeded_mixer.stats().seed_exhaustions == 0);
    auto seeded_next = seeded_first;
    seeded_next.sequence = 11;
    seeded_next.guest_cursor_q32 = uint64_t{2} << 32;
    seeded_next.seed_words[0] = 0x03000000u;
    seeded_next.seed_words[1] = 0x04000000u;
    CHECK(seeded_queue.enqueue_snapshot(seeded_next, seeded_mixer) ==
          Mp2kWallCaptureResult::Accepted);
    CHECK(seeded_queue.apply_due(seeded_mixer,
                                 seeded_next.guest_cursor_q32));
    CHECK(seeded_mixer.render(2, chunk) == 2);
    CHECK(seeded_mixer.stats().seed_exhaustions == 0);

    // Guest updates stay queued until their canonical guest time arrives.
    auto note_off = first;
    note_off.sequence = 2;
    note_off.guest_cursor_q32 = uint64_t{1} << 32;
    note_off.voices[0].on = false;
    CHECK(queue.enqueue_snapshot(note_off, mixer) ==
          Mp2kWallCaptureResult::Accepted);
    CHECK(queue.size() == 1);
    CHECK(queue.apply_due(mixer, note_off.guest_cursor_q32 - 1));
    CHECK(mixer.stats().active_voices == 1);
    CHECK(queue.apply_due(mixer, note_off.guest_cursor_q32));
    CHECK(mixer.stats().active_voices == 0);

    // A wall interval can split exactly at a note deadline.
    Mp2kWallMixer segmented_mixer;
    Mp2kWallUpdateQueue segmented_queue;
    auto segmented_first = snapshot(pcm.data(), pcm.size(), 50, 0);
    CHECK(segmented_mixer.begin_snapshot(segmented_first) ==
          Mp2kWallCaptureResult::Accepted);
    CHECK(segmented_queue.prime(segmented_first, segmented_mixer));
    auto segmented_off = segmented_first;
    segmented_off.sequence = 51;
    segmented_off.guest_cursor_q32 = 2u * 65536u;
    segmented_off.voices[0].on = false;
    CHECK(segmented_queue.enqueue_snapshot(segmented_off, segmented_mixer) ==
          Mp2kWallCaptureResult::Accepted);
    CHECK(segmented_mixer.render(2, chunk) == 2);
    CHECK(segmented_queue.apply_due(segmented_mixer,
                                    segmented_off.guest_cursor_q32));
    CHECK(segmented_mixer.stats().active_voices == 0);
    CHECK(segmented_mixer.render(2, chunk) == 2);

    // Several verified producer publications can arrive before the next wall
    // service. Keep every ordered block; one service applies all due events.
    Mp2kWallMixer burst_mixer;
    Mp2kWallUpdateQueue burst_queue;
    auto burst_first = snapshot(pcm.data(), pcm.size(), 100, 0);
    CHECK(burst_mixer.begin_snapshot(burst_first) ==
          Mp2kWallCaptureResult::Accepted);
    CHECK(burst_queue.prime(burst_first, burst_mixer));
    for (uint64_t i = 1; i <= 3; ++i) {
        auto publication = burst_first;
        publication.sequence = 100 + i;
        publication.guest_cursor_q32 = i * 65536u;
        publication.voices[0].pos_index = static_cast<uint32_t>(i & 1u);
        CHECK(burst_queue.enqueue_snapshot(publication, burst_mixer) ==
              Mp2kWallCaptureResult::Accepted);
    }
    CHECK(burst_queue.size() == 3);
    CHECK(burst_queue.stats().applied == 0);
    CHECK(burst_queue.apply_due(burst_mixer, 3u * 65536u));
    CHECK(burst_queue.empty());
    CHECK(burst_queue.stats().applied == 3);

    // Source publication IDs may skip when an upstream observer misses an
    // unrelated block.  Queue event IDs stay contiguous and the accepted
    // source gap must not spuriously latch fallback or reorder the timeline.
    Mp2kWallMixer gap_mixer;
    Mp2kWallUpdateQueue gap_queue;
    auto gap_first = snapshot(pcm.data(), pcm.size(), 200, 0);
    CHECK(gap_mixer.begin_snapshot(gap_first) ==
          Mp2kWallCaptureResult::Accepted);
    CHECK(gap_queue.prime(gap_first, gap_mixer));
    auto gap_update = gap_first;
    gap_update.sequence = 202;
    gap_update.guest_cursor_q32 = uint64_t{2} << 32;
    gap_update.voices[0].pos_index = 1;
    CHECK(gap_queue.enqueue_snapshot(gap_update, gap_mixer) ==
          Mp2kWallCaptureResult::Accepted);
    CHECK(gap_queue.size() == 1 && gap_queue.stats().stale == 0 &&
          gap_queue.stats().out_of_order == 0 &&
          !gap_queue.stats().fallback_required);
    uint64_t gap_due = 0;
    CHECK(gap_queue.next_due_guest_cursor_q32(gap_due) &&
          gap_due == (uint64_t{2} << 32));
    CHECK(gap_queue.apply_due(gap_mixer, gap_due));
    CHECK(gap_queue.empty() && gap_queue.stats().applied == 1);

    // An unchanged publication still occupies one coherent block event so
    // guest positions remain ordered across accelerated production.
    auto unchanged = note_off;
    unchanged.sequence = 3;
    unchanged.guest_cursor_q32 += uint64_t{1} << 32;
    CHECK(queue.enqueue_snapshot(unchanged, mixer) ==
          Mp2kWallCaptureResult::Accepted);
    CHECK(queue.size() == 1);

    // Global state is ordered separately from voice state.
    auto global = unchanged;
    global.sequence = 4;
    global.guest_cursor_q32 += uint64_t{1} << 32;
    global.pcm_rate = 32768;
    global.route_a_gain = 0.5f;
    CHECK(queue.enqueue_snapshot(global, mixer) ==
          Mp2kWallCaptureResult::Accepted);
    CHECK(queue.size() == 2);
    CHECK(queue.apply_due(mixer, global.guest_cursor_q32));
    CHECK(mixer.stats().pcm_rate == 32768);

    // Repeated source publication is stale; backward guest time is ordered
    // failure. Both are fail-closed and counted.
    CHECK(queue.enqueue_snapshot(global, mixer) ==
          Mp2kWallCaptureResult::OutOfOrder);
    auto backwards = global;
    backwards.sequence = 5;
    backwards.guest_cursor_q32 = 1;
    CHECK(queue.enqueue_snapshot(backwards, mixer) ==
          Mp2kWallCaptureResult::OutOfOrder);
    CHECK(queue.stats().stale != 0 && queue.stats().out_of_order != 0);

    // A new immutable asset is staged once, then referenced by generation in
    // the compact event (the queue retains no borrowed pointer).
    Mp2kWallUpdateQueue asset_queue;
    Mp2kWallMixer asset_mixer;
    auto asset_first = snapshot(pcm.data(), pcm.size(), 10, 0);
    CHECK(asset_mixer.begin_snapshot(asset_first) ==
          Mp2kWallCaptureResult::Accepted);
    CHECK(asset_queue.prime(asset_first, asset_mixer));
    const std::array<uint8_t, 2> pcm2{32, 16};
    auto asset_update = snapshot(pcm2.data(), pcm2.size(), 11,
                                uint64_t{1} << 32);
    CHECK(asset_queue.enqueue_snapshot(asset_update, asset_mixer) ==
          Mp2kWallCaptureResult::Accepted);
    CHECK(asset_mixer.stats().assets_staged == 1);
    CHECK(asset_queue.apply_due(asset_mixer, asset_update.guest_cursor_q32));

    // More than the old 12-slot voice count is valid: immutable assets reuse
    // by content hash and never overwrite a live epoch slot.
    std::array<std::array<uint8_t, 2>, 16> many_bytes{};
    auto many = asset_update;
    many.sequence = 12;
    many.guest_cursor_q32 = uint64_t{2} << 32;
    many.voices[0].pos_index = 1;
    many.asset_count = 16;
    for (uint8_t i = 0; i < many.asset_count; ++i) {
        many_bytes[i] = {static_cast<uint8_t>(i + 1),
                         static_cast<uint8_t>(i + 2)};
        many.assets[i].bytes = many_bytes[i].data();
        many.assets[i].byte_count = 2;
        many.assets[i].sample_count = 2;
        many.assets[i].looped = true;
    }
    CHECK(asset_queue.enqueue_snapshot(many, asset_mixer) ==
          Mp2kWallCaptureResult::Accepted);
    CHECK(asset_mixer.stats().assets_staged >= 16 - 1);
    const uint64_t staged = asset_mixer.stats().assets_staged;
    many.sequence = 13;
    many.guest_cursor_q32 = uint64_t{3} << 32;
    many.voices[0].pos_index = 0;
    CHECK(asset_queue.enqueue_snapshot(many, asset_mixer) ==
          Mp2kWallCaptureResult::Accepted);
    CHECK(asset_mixer.stats().assets_staged == staged);

    // Fixed queue fails closed at capacity; no heap growth or silent drop.
    Mp2kWallUpdateQueue full_queue;
    Mp2kWallMixer full_mixer;
    auto base = snapshot(pcm.data(), pcm.size(), 20, 0);
    CHECK(full_mixer.begin_snapshot(base) == Mp2kWallCaptureResult::Accepted);
    CHECK(full_queue.prime(base, full_mixer));
    CHECK(full_queue.capacity() == Mp2kWallUpdateQueue::kCapacity);
    for (uint64_t i = 1; i <= Mp2kWallUpdateQueue::kCapacity; ++i) {
        auto update = base;
        update.sequence = 20 + i;
        update.guest_cursor_q32 = i;
        update.voices[0].pos_index = static_cast<uint32_t>(i % 2);
        CHECK(full_queue.enqueue_snapshot(update, full_mixer) ==
              Mp2kWallCaptureResult::Accepted);
    }
    auto overflow = base;
    overflow.sequence = 21 + Mp2kWallUpdateQueue::kCapacity;
    overflow.guest_cursor_q32 = Mp2kWallUpdateQueue::kCapacity + 1;
    overflow.voices[0].pos_index = 1;
    CHECK(full_queue.enqueue_snapshot(overflow, full_mixer) ==
          Mp2kWallCaptureResult::AssetOverflow);
    CHECK(full_queue.stats().fallback_required);
    CHECK(full_queue.stats().high_water == Mp2kWallUpdateQueue::kCapacity);
    CHECK(full_queue.estimated_remaining_seconds() > 0.0);

    // Capacity failure must be checked before staging a new immutable asset.
    // The old queued events remain intact, no asset slot/arena bytes leak, and
    // the bounded queue fails closed exactly once.
    const uint64_t staged_before_full = full_mixer.stats().assets_staged;
    const uint64_t overflow_before_unique = full_queue.stats().overflow;
    const std::array<uint8_t, 2> unique_pcm{0x11, 0x22};
    auto unique_overflow = overflow;
    unique_overflow.assets[0].bytes = unique_pcm.data();
    unique_overflow.assets[0].byte_count = unique_pcm.size();
    unique_overflow.assets[0].sample_count = unique_pcm.size();
    unique_overflow.voices[0].size = unique_pcm.size();
    CHECK(full_queue.enqueue_snapshot(unique_overflow, full_mixer) ==
          Mp2kWallCaptureResult::AssetOverflow);
    CHECK(full_queue.size() == Mp2kWallUpdateQueue::kCapacity &&
          full_mixer.stats().assets_staged == staged_before_full &&
          full_queue.stats().fallback_required &&
          full_queue.stats().overflow == overflow_before_unique + 1);
    uint64_t first_due = 0;
    CHECK(full_queue.next_due_guest_cursor_q32(first_due) && first_due == 1);

    // Reset invalidates the old mixer epoch; queued events cannot revive it.
    Mp2kWallUpdateQueue stale_queue;
    Mp2kWallMixer stale_mixer;
    auto stale_first = snapshot(pcm.data(), pcm.size(), 40, 0);
    CHECK(stale_mixer.begin_snapshot(stale_first) ==
          Mp2kWallCaptureResult::Accepted);
    CHECK(stale_queue.prime(stale_first, stale_mixer));
    auto stale_update = stale_first;
    stale_update.sequence = 41;
    stale_update.guest_cursor_q32 = 1;
    stale_update.voices[0].on = false;
    CHECK(stale_queue.enqueue_snapshot(stale_update, stale_mixer) ==
          Mp2kWallCaptureResult::Accepted);
    stale_mixer.reset();
    CHECK(!stale_queue.apply_due(stale_mixer, 1));
    CHECK(stale_queue.stats().fallback_required);

    return 0;
}
