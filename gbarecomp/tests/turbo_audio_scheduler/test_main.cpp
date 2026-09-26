#include "turbo_audio_scheduler.h"
#include "turbo_audio_service_gate.h"

#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {

using gba::TurboAudioEvent;
using gba::TurboAudioEventClass;
using gba::TurboAudioEventType;
using gba::TurboAudioMode;
using gba::TurboAudioPushResult;
using gba::TurboAudioQ32;
using gba::TurboAudioScheduler;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

TurboAudioQ32 one_second() {
    return gba::kTurboAudioQ32OneSecond;
}

TurboAudioEvent event(uint64_t epoch, TurboAudioQ32 guest_start,
                      TurboAudioQ32 wall_start, TurboAudioQ32 guest_duration,
                      TurboAudioQ32 wall_duration,
                      TurboAudioEventClass event_class =
                          TurboAudioEventClass::Music,
                      TurboAudioEventType event_type =
                          TurboAudioEventType::PcmBlock,
                      uint16_t stream = 1, uint32_t sample_frames = 0) {
    TurboAudioEvent out;
    out.epoch = epoch;
    out.guest_start_q32 = guest_start;
    out.wall_start_q32 = wall_start;
    out.guest_duration_q32 = guest_duration;
    out.wall_duration_q32 = wall_duration;
    out.event_class = event_class;
    out.event_type = event_type;
    out.stream_id = stream;
    out.sample_frames = sample_frames;
    return out;
}

void test_q32_conversions_and_rate_change() {
    using namespace gba;
    expect(q32_seconds_from_frames(0, 44100) == 0,
           "zero frames did not convert to zero seconds");
    expect(q32_seconds_from_frames(44100, 44100) ==
               kTurboAudioQ32OneSecond,
           "one second frame conversion changed");
    expect(frames_from_q32_seconds(kTurboAudioQ32OneSecond, 44100) == 44100,
           "one second reverse conversion changed");
    expect(frames_from_q32_seconds(kTurboAudioQ32OneSecond / 2, 48000) ==
               24000,
           "half-second reverse conversion changed");
    expect(q32_seconds_from_frames(1024, 44100) !=
               q32_seconds_from_frames(1024, 48000),
           "sample-rate change did not change Q32 duration");
    const uint64_t max_u64 = std::numeric_limits<uint64_t>::max();
    const uint32_t max_rate = std::numeric_limits<uint32_t>::max();
    expect(q32_seconds_from_frames(max_u64, 1) == max_u64,
           "maximum frame conversion did not saturate");
    expect(q32_seconds_from_frames(max_u64, max_rate) == max_u64,
           "maximum frame/rate conversion did not saturate");
    expect(frames_from_q32_seconds(max_u64, max_rate) <= max_u64,
           "maximum reverse conversion overflowed");
    const uint64_t source_frames = 123456789;
    const uint64_t round_trip = frames_from_q32_seconds(
        q32_seconds_from_frames(source_frames, 48000), 48000);
    expect(round_trip <= source_frames && source_frames - round_trip <= 1,
           "Q32 frame round-trip exceeded one-frame truncation");
    expect(q32_seconds_from_frames(1, 0) == 0 &&
               frames_from_q32_seconds(kTurboAudioQ32OneSecond, 0) == 0,
           "zero-rate conversion was not safe");
    expect(q32_seconds_from_nanoseconds(1'000'000'000ull) ==
               kTurboAudioQ32OneSecond,
           "one second wall conversion changed");
    expect(q32_seconds_from_nanoseconds(500'000'000ull) ==
               kTurboAudioQ32OneSecond / 2,
           "half-second wall conversion changed");
    expect(q32_seconds_from_nanoseconds(max_u64) == max_u64,
           "maximum wall conversion did not saturate");
    expect(turbo_audio_multiplier_supported(2.0f, false) &&
               turbo_audio_multiplier_supported(4.0f, false) &&
               !turbo_audio_multiplier_supported(1.0f, false) &&
               !turbo_audio_multiplier_supported(8.0f, false) &&
               !turbo_audio_multiplier_supported(4.0f, true),
           "Turbo wall bound accepted an unsafe mode");
    expect(turbo_audio_env_allows_decoupled("decoupled", nullptr),
           "Turbo env did not request decoupled mode");
    expect(!turbo_audio_env_allows_decoupled("decoupled", "1") &&
               !turbo_audio_env_allows_decoupled("decoupled", "true") &&
               turbo_audio_env_allows_decoupled("decoupled", "0") &&
               !turbo_audio_env_allows_decoupled("0", nullptr),
           "strict-static Turbo env policy changed");
}

void test_one_to_one_and_boundary_due() {
    TurboAudioScheduler scheduler;
    scheduler.enter_turbo(TurboAudioMode::Coupled, 0, 0);
    const TurboAudioQ32 step = gba::q32_seconds_from_frames(10, 1000);

    for (uint64_t i = 0; i < 4; ++i) {
        expect(scheduler.push(event(scheduler.epoch(), i * step, i * step,
                                    step, step, TurboAudioEventClass::Music,
                                    TurboAudioEventType::PcmBlock,
                                    static_cast<uint16_t>(i + 1))) ==
                   TurboAudioPushResult::Accepted,
               "1x event was not accepted");
        scheduler.advance_guest_q32(step);
        scheduler.advance_wall_q32(step);
    }

    TurboAudioEvent out;
    for (int i = 0; i < 4; ++i)
        expect(scheduler.pop_due(out), "1x guest-clock event was not due");
    expect(scheduler.empty(), "1x queue retained emitted events");

    scheduler.resync(0, 0);
    const TurboAudioQ32 one = gba::kTurboAudioQ32OneSecond;
    expect(scheduler.push(event(scheduler.epoch(), one, one, 0, 0)) ==
               TurboAudioPushResult::Accepted,
           "boundary event was not accepted");
    scheduler.advance_guest_q32(one - 1);
    expect(!scheduler.pop_due(out), "event became due before guest boundary");
    scheduler.advance_guest_q32(1);
    expect(scheduler.pop_due(out), "event was not due at exact boundary");
}

void test_zero_duration_and_off_coupled_policy() {
    TurboAudioScheduler scheduler;
    const TurboAudioQ32 one = one_second();
    expect(scheduler.mode() == TurboAudioMode::Off &&
               !scheduler.turbo_active(),
           "new scheduler did not start in faithful Off mode");
    expect(scheduler.push(event(scheduler.epoch(), one, one * 4, 0, 0,
                                TurboAudioEventClass::Music,
                                TurboAudioEventType::Command, 8, 0)) ==
               TurboAudioPushResult::Accepted,
           "zero-duration control was rejected");
    expect(scheduler.advance_wall_q32(one * 8),
           "wall cursor unexpectedly overflowed");
    TurboAudioEvent out;
    expect(!scheduler.pop_due(out),
           "Off mode incorrectly used wall cursor for due events");
    expect(scheduler.advance_guest_q32(one),
           "guest cursor unexpectedly overflowed");
    expect(scheduler.pop_due(out), "Off mode did not use guest cursor");

    scheduler.enter_turbo(TurboAudioMode::Coupled, 0, 0);
    expect(scheduler.push(event(scheduler.epoch(), one, one * 4, 0, 0,
                                TurboAudioEventClass::Music,
                                TurboAudioEventType::PcmBlock, 8, 0)) ==
               TurboAudioPushResult::Accepted,
           "zero-length PCM marker was rejected");
    expect(scheduler.advance_wall_q32(one * 8),
           "Coupled wall cursor unexpectedly overflowed");
    expect(!scheduler.pop_due(out),
           "Coupled mode incorrectly used wall cursor for due events");
    expect(scheduler.advance_guest_q32(one),
           "Coupled guest cursor unexpectedly overflowed");
    expect(scheduler.pop_due(out), "Coupled mode did not use guest cursor");
}

void test_four_x_wall_clock_and_idle() {
    TurboAudioScheduler scheduler;
    scheduler.enter_turbo(TurboAudioMode::Decoupled, 0, 0);
    const TurboAudioQ32 guest_step =
        gba::q32_seconds_from_frames(4, 1000);
    const TurboAudioQ32 wall_step =
        gba::q32_seconds_from_frames(10, 1000);

    for (uint64_t i = 0; i < 4; ++i) {
        expect(scheduler.push(event(scheduler.epoch(), i * guest_step,
                                    i * wall_step, guest_step, wall_step,
                                    TurboAudioEventClass::Music,
                                    TurboAudioEventType::PcmBlock,
                                    static_cast<uint16_t>(i + 1))) ==
                   TurboAudioPushResult::Accepted,
               "4x event was not accepted");
        scheduler.advance_guest_q32(guest_step);
    }
    scheduler.advance_wall_q32(wall_step / 2);

    TurboAudioEvent out;
    expect(scheduler.pop_due(out), "first wall event was not due");
    expect(out.wall_start_q32 == 0, "wrong wall event released");
    expect(!scheduler.pop_due(out), "future wall event released early");

    scheduler.advance_wall_q32(wall_step * 3);
    int released = 0;
    while (scheduler.pop_due(out)) ++released;
    expect(released == 3, "wall clock did not release remaining events");

    scheduler.resync(0, 0);
    expect(scheduler.push(event(scheduler.epoch(), 0, one_second(), 0, 0)) ==
               TurboAudioPushResult::Accepted,
           "idle event was not accepted");
    scheduler.advance_guest_q32(one_second() * 10);
    expect(!scheduler.pop_due(out),
           "decoupled audio advanced during guest-only progress");
    scheduler.advance_wall_q32(one_second());
    expect(scheduler.pop_due(out), "wall audio stayed paused");
}

void test_epoch_order_and_stable_ties() {
    TurboAudioScheduler scheduler;
    scheduler.enter_turbo(TurboAudioMode::Coupled, 0, 0);
    const uint64_t old_epoch = scheduler.epoch();
    const TurboAudioEvent old = event(old_epoch, 0, 0, 0, 0);
    scheduler.resync(0, 0);
    expect(scheduler.push(old) == TurboAudioPushResult::StaleEpoch,
           "old epoch event was accepted");
    expect(scheduler.stats().stale_epochs == 1,
           "stale epoch was not counted");

    const TurboAudioQ32 t = one_second();
    expect(scheduler.push(event(scheduler.epoch(), t, t, 0, 0,
                                TurboAudioEventClass::Music,
                                TurboAudioEventType::NoteOn, 2)) ==
               TurboAudioPushResult::Accepted,
           "first tied event was not accepted");
    expect(scheduler.push(event(scheduler.epoch(), t, t, 0, 0,
                                TurboAudioEventClass::Music,
                                TurboAudioEventType::NoteOff, 2)) ==
               TurboAudioPushResult::Accepted,
           "second tied event was not accepted");
    expect(scheduler.push(event(scheduler.epoch(), t - 1, t, 0, 0)) ==
               TurboAudioPushResult::OutOfOrder,
           "out-of-order event was accepted");
    expect(scheduler.push(event(scheduler.epoch(), t + 1, t - 1, 0, 0)) ==
               TurboAudioPushResult::OutOfOrder,
           "wall-clock reversal was accepted");

    scheduler.advance_guest_q32(t);
    TurboAudioEvent out;
    expect(scheduler.pop_due(out) &&
               out.event_type == TurboAudioEventType::NoteOn,
           "equal-time insertion lost first event");
    expect(scheduler.pop_due(out) &&
               out.event_type == TurboAudioEventType::NoteOff,
           "equal-time insertion lost stable order");
    expect(scheduler.stats().out_of_order == 2,
           "out-of-order event was not counted");
}

void test_typed_coalesce_late_and_gaps() {
    TurboAudioScheduler scheduler;
    scheduler.enter_turbo(TurboAudioMode::Coupled, 0, 0);
    const TurboAudioQ32 t = one_second();
    expect(scheduler.push(event(scheduler.epoch(), 0, 0, t, t,
                                TurboAudioEventClass::Music,
                                TurboAudioEventType::PcmBlock, 7, 4)) ==
               TurboAudioPushResult::Accepted,
           "music block was not accepted");
    expect(scheduler.push(event(scheduler.epoch(), t, t, t, t,
                                TurboAudioEventClass::Music,
                                TurboAudioEventType::PcmBlock, 7, 5)) ==
               TurboAudioPushResult::Coalesced,
           "adjacent music blocks did not coalesce");
    expect(scheduler.push(event(scheduler.epoch(), t * 2, t * 2, 0, 0,
                                TurboAudioEventClass::Music,
                                TurboAudioEventType::NoteOn, 7)) ==
               TurboAudioPushResult::Accepted,
           "note-on was not accepted after PCM block");
    expect(scheduler.push(event(scheduler.epoch(), t * 2, t * 2, 0, 0,
                                TurboAudioEventClass::Music,
                                TurboAudioEventType::NoteOff, 7)) ==
               TurboAudioPushResult::Accepted,
           "note-off was not accepted after note-on");

    scheduler.advance_guest_q32(t * 10);
    TurboAudioEvent out;
    expect(scheduler.pop_due(out) && out.sample_frames == 9,
           "coalesced PCM count changed");
    expect(scheduler.pop_due(out) &&
               out.event_type == TurboAudioEventType::NoteOn,
           "note-on was coalesced or reordered");
    expect(scheduler.pop_due(out) &&
               out.event_type == TurboAudioEventType::NoteOff,
           "note-off was coalesced or reordered");

    scheduler.resync(0, 0);
    expect(scheduler.push(event(scheduler.epoch(), t, t, 0, 0)) ==
               TurboAudioPushResult::Accepted,
           "late-test event was not accepted");
    scheduler.advance_guest_q32(t * 2);
    expect(scheduler.push(event(scheduler.epoch(), t, t, 0, 0,
                                TurboAudioEventClass::Music,
                                TurboAudioEventType::NoteOn)) ==
               TurboAudioPushResult::Accepted,
           "late-test event was not accepted");
    expect(scheduler.push(event(scheduler.epoch(), t * 3, t * 3, 0, 0,
                                TurboAudioEventClass::Music,
                                TurboAudioEventType::NoteOff)) ==
               TurboAudioPushResult::Accepted,
           "gap-test event was not accepted");
    expect(scheduler.stats().late_events == 1,
           "late event was not counted");
    expect(scheduler.stats().clock_gaps == 1,
           "clock gap was not counted");
}

void fill_queue(TurboAudioScheduler& scheduler,
                TurboAudioEventClass event_class =
                    TurboAudioEventClass::Music) {
    for (std::size_t i = 0; i < TurboAudioScheduler::kCapacity; ++i) {
        expect(scheduler.push(event(
                   scheduler.epoch(), static_cast<TurboAudioQ32>(i * 2),
                   static_cast<TurboAudioQ32>(i * 2), 0, 0, event_class,
                   TurboAudioEventType::Command,
                   static_cast<uint16_t>(i + 1))) ==
                   TurboAudioPushResult::Accepted,
               "queue rejected an event before capacity");
    }
}

void test_overflow_fallback_and_sfx_drop() {
    TurboAudioScheduler scheduler;
    fill_queue(scheduler);
    expect(scheduler.stats().queue_high_water == TurboAudioScheduler::kCapacity,
           "queue high-water missed capacity");
    expect(scheduler.push(event(scheduler.epoch(), 1000, 1000, 0, 0,
                                TurboAudioEventClass::Essential,
                                TurboAudioEventType::Command, 999)) ==
               TurboAudioPushResult::FatalOverflow,
           "essential overflow was silently dropped");
    expect(scheduler.stats().fatal_overflows == 1 &&
               scheduler.stats().fallback_required &&
               scheduler.stats().fallback_requests == 1,
           "essential overflow did not request fallback");

    scheduler.resync(0, 0);
    fill_queue(scheduler);
    expect(scheduler.push(event(scheduler.epoch(), 1000, 1000, 0, 0,
                                TurboAudioEventClass::TransientSfx,
                                TurboAudioEventType::Command, 999)) ==
               TurboAudioPushResult::DroppedSfx,
           "SFX overflow was not explicitly dropped");
    expect(scheduler.stats().drops == 1 &&
               scheduler.stats().fallback_required,
           "fallback latch was silently cleared by resync or SFX drop");
    scheduler.acknowledge_fallback();
    expect(!scheduler.stats().fallback_required,
           "fallback acknowledgment did not clear safety latch");

    const uint64_t max_u64 = std::numeric_limits<uint64_t>::max();
    scheduler.resync(max_u64 - 1, 0);
    expect(!scheduler.advance_guest_q32(2),
           "Q32 cursor overflow was not reported");
    expect(scheduler.stats().clock_overflows == 1 &&
               scheduler.stats().fallback_required,
           "Q32 cursor overflow missed fatal telemetry");

    scheduler.acknowledge_fallback();
    scheduler.resync(0, 0);
    expect(scheduler.push(event(scheduler.epoch(), max_u64, 0, 1, 0)) ==
               TurboAudioPushResult::ClockOverflow,
           "event Q32 overflow was not reported");
    expect(scheduler.stats().clock_overflows == 2 &&
               scheduler.stats().fallback_required,
           "event Q32 overflow missed fallback telemetry");

    scheduler.reset();
    expect(scheduler.stats().fallback_required,
           "reset silently cleared unhandled fallback");
    scheduler.acknowledge_fallback();
    expect(!scheduler.stats().fallback_required,
           "fallback acknowledgment failed after reset");

    scheduler.resync(0, 0);
    expect(scheduler.push(event(scheduler.epoch(), 0, 0, 0, 0,
                                TurboAudioEventClass::Music,
                                TurboAudioEventType::PcmBlock, 12,
                                std::numeric_limits<uint32_t>::max())) ==
               TurboAudioPushResult::Accepted,
           "coalesce-overflow seed was not accepted");
    expect(scheduler.push(event(scheduler.epoch(), 0, 0, 0, 0,
                                TurboAudioEventClass::Music,
                                TurboAudioEventType::PcmBlock, 12, 1)) ==
               TurboAudioPushResult::ClockOverflow,
           "coalesce overflow was not fatal");
    expect(scheduler.stats().clock_overflows == 3 &&
               scheduler.stats().fallback_required,
           "coalesce overflow missed fallback telemetry");
    scheduler.acknowledge_fallback();

    scheduler.resync(0, 0);
    scheduler.enter_turbo(TurboAudioMode::Muted, 0, 0);
    expect(scheduler.push(event(scheduler.epoch(), 0, 0, 0, 0,
                                TurboAudioEventClass::Music)) ==
               TurboAudioPushResult::Muted,
           "Muted mode accepted new music event");
    expect(scheduler.stats().muted_events == 1,
           "Muted event was not counted");
}

void test_idempotent_enter_reset_and_queue_wrap() {
    TurboAudioScheduler scheduler;
    scheduler.enter_turbo(TurboAudioMode::Decoupled, 0, 0);
    const uint64_t active_epoch = scheduler.epoch();
    expect(scheduler.push(event(active_epoch, 100, 100, 0, 0)) ==
               TurboAudioPushResult::Accepted,
           "queued event for repeated enter was not accepted");
    const uint64_t resyncs = scheduler.stats().resyncs;
    scheduler.enter_turbo(TurboAudioMode::Decoupled, 999, 999);
    expect(scheduler.epoch() == active_epoch && scheduler.size() == 1 &&
               scheduler.stats().resyncs == resyncs &&
               scheduler.guest_cursor_q32() == 0 &&
               scheduler.wall_cursor_q32() == 0,
           "repeated active enter flushed queue or moved anchor");

    scheduler.resync(0, 0);
    for (std::size_t i = 0; i < TurboAudioScheduler::kCapacity; ++i)
        expect(scheduler.push(event(scheduler.epoch(), i * 2, i * 2, 0, 0,
                                    TurboAudioEventClass::Music,
                                    TurboAudioEventType::Command,
                                    static_cast<uint16_t>(i + 1))) ==
                   TurboAudioPushResult::Accepted,
               "wrap test initial push failed");
    scheduler.advance_wall_q32(gba::kTurboAudioQ32OneSecond * 100);
    TurboAudioEvent out;
    for (int i = 0; i < 10; ++i) {
        expect(scheduler.pop_due(out), "wrap test initial pop failed");
        expect(out.guest_start_q32 == static_cast<TurboAudioQ32>(i * 2),
               "wrap test changed FIFO order");
    }
    for (int i = 0; i < 10; ++i)
        expect(scheduler.push(event(scheduler.epoch(), 128 + i * 2,
                                    128 + i * 2, 0, 0,
                                    TurboAudioEventClass::Music,
                                    TurboAudioEventType::Command,
                                    static_cast<uint16_t>(100 + i))) ==
                   TurboAudioPushResult::Accepted,
               "wrap test refill failed");
    int remaining = 0;
    while (scheduler.pop_due(out)) ++remaining;
    expect(remaining == 64, "wrap test lost or duplicated events");

    const uint64_t before = scheduler.epoch();
    scheduler.set_mode(TurboAudioMode::Coupled);
    expect(scheduler.epoch() != before && scheduler.empty(),
           "mode switch did not start a new epoch");
    scheduler.reset();
    expect(scheduler.mode() == TurboAudioMode::Off && !scheduler.turbo_active(),
           "reset did not restore faithful Off mode");
}

void test_present_irq_pending_service_gate() {
    gba::TurboAudioServiceGate gate;
    gate.mark_pending();
    expect(gate.pending(), "IRQ present did not mark audio pending");
    expect(gate.take_pending(7, 100),
           "first post-IRQ seam did not claim audio service");
    expect(!gate.pending(), "post-IRQ service left pending set");
    expect(!gate.claim(7, 100),
           "outer boundary double-serviced the same guest step");

    // Non-IRQ present claims immediately; the outer seam skips it, then the
    // next guest frame gets one fresh claim.
    expect(gate.claim(8, 101), "non-IRQ present did not service audio");
    expect(!gate.claim(8, 101),
           "non-IRQ present and outer seam both serviced audio");
    expect(gate.claim(9, 102), "next guest frame stayed guarded");

    // A stale pending mark cannot create a second service in one generation.
    gate.mark_pending();
    expect(!gate.take_pending(9, 102),
           "stale IRQ pending mark bypassed generation guard");
    expect(!gate.pending(), "stale pending mark was not consumed");
}

}  // namespace

int main() {
    test_q32_conversions_and_rate_change();
    test_one_to_one_and_boundary_due();
    test_zero_duration_and_off_coupled_policy();
    test_four_x_wall_clock_and_idle();
    test_epoch_order_and_stable_ties();
    test_typed_coalesce_late_and_gaps();
    test_overflow_fallback_and_sfx_drop();
    test_idempotent_enter_reset_and_queue_wrap();
    test_present_irq_pending_service_gate();
    std::puts("Turbo audio scheduler tests passed");
    return 0;
}
