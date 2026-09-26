// turbo_audio_scheduler.cpp - bounded guest/wall-clock Turbo audio policy.

#include "turbo_audio_scheduler.h"

#include <limits>

namespace gba {

bool TurboAudioScheduler::checked_add(TurboAudioQ32 a, TurboAudioQ32 b,
                                      TurboAudioQ32& result) {
    const TurboAudioQ32 max = ~TurboAudioQ32{0};
    if (b > max - a) return false;
    result = a + b;
    return true;
}

void TurboAudioScheduler::clear_queue() {
    head_ = 0;
    count_ = 0;
    stats_.queue_depth = 0;
}

void TurboAudioScheduler::clear_ordering() {
    have_last_event_ = false;
    last_guest_start_q32_ = 0;
    last_wall_start_q32_ = 0;
    last_guest_end_q32_ = 0;
    last_wall_end_q32_ = 0;
}

void TurboAudioScheduler::bump_epoch() {
    if (epoch_ != ~uint64_t{0}) ++epoch_;
}

void TurboAudioScheduler::begin_epoch(TurboAudioQ32 guest_cursor_q32,
                                      TurboAudioQ32 wall_cursor_q32) {
    bump_epoch();
    guest_cursor_q32_ = guest_cursor_q32;
    wall_cursor_q32_ = wall_cursor_q32;
    stats_.guest_cursor_q32 = guest_cursor_q32_;
    stats_.wall_cursor_q32 = wall_cursor_q32_;
    // Keep fallback_required latched until the owner confirms handling.
    clear_queue();
    clear_ordering();
    ++stats_.resyncs;
}

void TurboAudioScheduler::reset(TurboAudioQ32 guest_cursor_q32,
                                TurboAudioQ32 wall_cursor_q32) {
    if (mode_ != TurboAudioMode::Off) ++stats_.mode_changes;
    mode_ = TurboAudioMode::Off;
    turbo_active_ = false;
    ++stats_.resets;
    begin_epoch(guest_cursor_q32, wall_cursor_q32);
}

void TurboAudioScheduler::resync(TurboAudioQ32 guest_cursor_q32,
                                 TurboAudioQ32 wall_cursor_q32) {
    begin_epoch(guest_cursor_q32, wall_cursor_q32);
}

void TurboAudioScheduler::set_mode(TurboAudioMode mode) {
    if (mode_ == mode) return;
    mode_ = mode;
    turbo_active_ = mode != TurboAudioMode::Off;
    ++stats_.mode_changes;
    begin_epoch(guest_cursor_q32_, wall_cursor_q32_);
}

void TurboAudioScheduler::enter_turbo(TurboAudioMode mode,
                                      TurboAudioQ32 guest_cursor_q32,
                                      TurboAudioQ32 wall_cursor_q32) {
    if (mode == TurboAudioMode::Off) {
        exit_turbo(guest_cursor_q32, wall_cursor_q32);
        return;
    }
    // Input pumps may report the same held state repeatedly. Do not treat
    // those reports as timeline boundaries or flush valid queued audio.
    if (turbo_active_ && mode_ == mode) return;
    mode_ = mode;
    turbo_active_ = true;
    ++stats_.mode_changes;
    begin_epoch(guest_cursor_q32, wall_cursor_q32);
}

void TurboAudioScheduler::exit_turbo(TurboAudioQ32 guest_cursor_q32,
                                     TurboAudioQ32 wall_cursor_q32) {
    if (!turbo_active_ && mode_ == TurboAudioMode::Off) return;
    mode_ = TurboAudioMode::Off;
    turbo_active_ = false;
    ++stats_.mode_changes;
    begin_epoch(guest_cursor_q32, wall_cursor_q32);
}

bool TurboAudioScheduler::advance_guest_q32(TurboAudioQ32 duration_q32) {
    TurboAudioQ32 next = 0;
    if (!checked_add(guest_cursor_q32_, duration_q32, next)) {
        clock_overflow();
        guest_cursor_q32_ = ~TurboAudioQ32{0};
        stats_.guest_cursor_q32 = guest_cursor_q32_;
        return false;
    }
    guest_cursor_q32_ = next;
    stats_.guest_cursor_q32 = guest_cursor_q32_;
    return true;
}

bool TurboAudioScheduler::advance_wall_q32(TurboAudioQ32 duration_q32) {
    TurboAudioQ32 next = 0;
    if (!checked_add(wall_cursor_q32_, duration_q32, next)) {
        clock_overflow();
        wall_cursor_q32_ = ~TurboAudioQ32{0};
        stats_.wall_cursor_q32 = wall_cursor_q32_;
        return false;
    }
    wall_cursor_q32_ = next;
    stats_.wall_cursor_q32 = wall_cursor_q32_;
    return true;
}

bool TurboAudioScheduler::non_droppable(
    TurboAudioEventClass event_class) {
    return event_class != TurboAudioEventClass::TransientSfx;
}

TurboAudioScheduler::CoalesceResult TurboAudioScheduler::can_coalesce(
    const TurboAudioEvent& older, const TurboAudioEvent& newer) {
    if (older.epoch != newer.epoch || older.stream_id != newer.stream_id ||
        older.event_class != newer.event_class ||
        older.event_type != TurboAudioEventType::PcmBlock ||
        newer.event_type != TurboAudioEventType::PcmBlock) {
        return CoalesceResult::No;
    }
    TurboAudioQ32 guest_end = 0;
    TurboAudioQ32 wall_end = 0;
    if (!checked_add(older.guest_start_q32, older.guest_duration_q32,
                     guest_end) ||
        !checked_add(older.wall_start_q32, older.wall_duration_q32,
                     wall_end)) {
        return CoalesceResult::ClockOverflow;
    }
    if (guest_end != newer.guest_start_q32 ||
        wall_end != newer.wall_start_q32)
        return CoalesceResult::No;
    TurboAudioQ32 unused = 0;
    if (!checked_add(older.guest_duration_q32, newer.guest_duration_q32,
                    unused) ||
        !checked_add(older.wall_duration_q32, newer.wall_duration_q32,
                     unused))
        return CoalesceResult::ClockOverflow;
    if (older.sample_frames >
        std::numeric_limits<uint32_t>::max() - newer.sample_frames)
        return CoalesceResult::ClockOverflow;
    return CoalesceResult::Yes;
}

bool TurboAudioScheduler::monotonic(const TurboAudioEvent& event) const {
    return !have_last_event_ ||
           (event.guest_start_q32 >= last_guest_start_q32_ &&
            event.wall_start_q32 >= last_wall_start_q32_);
}

bool TurboAudioScheduler::has_clock_gap(const TurboAudioEvent& event) const {
    return have_last_event_ &&
           (event.guest_start_q32 > last_guest_end_q32_ ||
            event.wall_start_q32 > last_wall_end_q32_);
}

void TurboAudioScheduler::remember_event(const TurboAudioEvent& event) {
    have_last_event_ = true;
    last_guest_start_q32_ = event.guest_start_q32;
    last_wall_start_q32_ = event.wall_start_q32;
    checked_add(event.guest_start_q32, event.guest_duration_q32,
                last_guest_end_q32_);
    checked_add(event.wall_start_q32, event.wall_duration_q32,
                last_wall_end_q32_);
}

TurboAudioPushResult TurboAudioScheduler::clock_overflow() {
    ++stats_.clock_overflows;
    ++stats_.fallback_requests;
    stats_.fallback_required = true;
    return TurboAudioPushResult::ClockOverflow;
}

TurboAudioPushResult TurboAudioScheduler::push(
    const TurboAudioEvent& event) {
    if (event.epoch != epoch_) {
        ++stats_.stale_epochs;
        return TurboAudioPushResult::StaleEpoch;
    }
    TurboAudioQ32 guest_end = 0;
    TurboAudioQ32 wall_end = 0;
    if (!checked_add(event.guest_start_q32, event.guest_duration_q32,
                     guest_end) ||
        !checked_add(event.wall_start_q32, event.wall_duration_q32,
                     wall_end)) {
        return clock_overflow();
    }
    (void)guest_end;
    (void)wall_end;
    if (!monotonic(event)) {
        ++stats_.out_of_order;
        return TurboAudioPushResult::OutOfOrder;
    }
    if (mode_ == TurboAudioMode::Muted) {
        ++stats_.muted_events;
        return TurboAudioPushResult::Muted;
    }

    const TurboAudioQ32 active_start =
        mode_ == TurboAudioMode::Decoupled ? event.wall_start_q32
                                           : event.guest_start_q32;
    const TurboAudioQ32 active_cursor =
        mode_ == TurboAudioMode::Decoupled ? wall_cursor_q32_
                                           : guest_cursor_q32_;
    if (active_start < active_cursor) ++stats_.late_events;
    if (has_clock_gap(event)) ++stats_.clock_gaps;

    if (count_ != 0) {
        const std::size_t tail = (head_ + count_ - 1) % kCapacity;
        const CoalesceResult coalesce = can_coalesce(queue_[tail], event);
        if (coalesce == CoalesceResult::ClockOverflow)
            return clock_overflow();
        if (coalesce == CoalesceResult::Yes) {
            checked_add(queue_[tail].guest_duration_q32,
                        event.guest_duration_q32,
                        queue_[tail].guest_duration_q32);
            checked_add(queue_[tail].wall_duration_q32,
                        event.wall_duration_q32,
                        queue_[tail].wall_duration_q32);
            queue_[tail].sample_frames += event.sample_frames;
            remember_event(queue_[tail]);
            ++stats_.coalesces;
            return TurboAudioPushResult::Coalesced;
        }
    }

    if (count_ == kCapacity) {
        if (non_droppable(event.event_class)) {
            ++stats_.fatal_overflows;
            ++stats_.fallback_requests;
            stats_.fallback_required = true;
            return TurboAudioPushResult::FatalOverflow;
        }
        ++stats_.drops;
        return TurboAudioPushResult::DroppedSfx;
    }

    const std::size_t tail = (head_ + count_) % kCapacity;
    queue_[tail] = event;
    ++count_;
    stats_.queue_depth = static_cast<uint32_t>(count_);
    if (stats_.queue_depth > stats_.queue_high_water)
        stats_.queue_high_water = stats_.queue_depth;
    remember_event(event);
    return TurboAudioPushResult::Accepted;
}

bool TurboAudioScheduler::due(const TurboAudioEvent& event) const {
    if (mode_ == TurboAudioMode::Decoupled)
        return event.wall_start_q32 <= wall_cursor_q32_;
    return event.guest_start_q32 <= guest_cursor_q32_;
}

bool TurboAudioScheduler::pop_due(TurboAudioEvent& event) {
    if (count_ == 0 || !due(queue_[head_])) return false;
    event = queue_[head_];
    head_ = (head_ + 1) % kCapacity;
    --count_;
    stats_.queue_depth = static_cast<uint32_t>(count_);
    ++stats_.emitted;
    return true;
}

}  // namespace gba
