// turbo_audio_scheduler.h - bounded, guest/wall-clock Turbo audio policy.
//
// This component schedules immutable audio-event metadata. It does not read
// guest memory or own PCM. Live Turbo wiring remains explicit and default-off.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace gba {

// Every cursor and event timestamp is Q32 seconds. Keeping one unit for both
// clocks prevents a sample-count or CPU-tick value from being misread as wall
// time at a policy boundary.
using TurboAudioQ32 = uint64_t;
constexpr TurboAudioQ32 kTurboAudioQ32OneSecond =
    (TurboAudioQ32{1} << 32);
constexpr TurboAudioQ32 kTurboAudioQ32FractionMask =
    kTurboAudioQ32OneSecond - 1;

constexpr TurboAudioQ32 q32_seconds_from_nanoseconds(uint64_t nanoseconds) {
    constexpr uint64_t kNsPerSecond = 1'000'000'000ull;
    const uint64_t whole = nanoseconds / kNsPerSecond;
    const uint64_t remainder = nanoseconds % kNsPerSecond;
    if (whole > (std::numeric_limits<uint64_t>::max() >> 32))
        return std::numeric_limits<uint64_t>::max();
    const uint64_t whole_q32 = whole << 32;
    const uint64_t fraction_q32 =
        (remainder << 32) / kNsPerSecond;
    return fraction_q32 > std::numeric_limits<uint64_t>::max() - whole_q32
        ? std::numeric_limits<uint64_t>::max()
        : whole_q32 + fraction_q32;
}

// Live wall playback stays bounded to the validated 2x/4x Turbo choices.
// Uncapped mode has no wall-production bound and must fail closed.
constexpr bool turbo_audio_multiplier_supported(float multiplier,
                                                bool uncapped) {
    return !uncapped && multiplier >= 2.0f && multiplier <= 4.0f;
}

// Strict-static acceptance owns the faithful route even when a caller starts
// the runtime directly with inherited environment variables. Keep this policy
// beside the Turbo bound so launcher and direct-runner paths agree.
inline bool turbo_audio_env_allows_decoupled(const char* turbo_env,
                                             const char* strict_static_env) {
    const bool requested = turbo_env &&
        std::strcmp(turbo_env, "decoupled") == 0;
    const bool strict = strict_static_env && strict_static_env[0] &&
                        strict_static_env[0] != '0';
    return requested && !strict;
}

// Saturating frame/rate conversion. Rate zero means "no usable clock".
constexpr TurboAudioQ32 q32_seconds_from_frames(uint64_t frames,
                                                uint32_t rate) {
    if (rate == 0) return 0;
    const uint64_t whole = frames / rate;
    const uint64_t remainder = frames % rate;
    if (whole > (std::numeric_limits<uint64_t>::max() >> 32))
        return std::numeric_limits<uint64_t>::max();
    const uint64_t whole_q32 = whole << 32;
    const uint64_t fraction_q32 =
        (remainder << 32) / static_cast<uint64_t>(rate);
    return fraction_q32 > std::numeric_limits<uint64_t>::max() - whole_q32
        ? std::numeric_limits<uint64_t>::max()
        : whole_q32 + fraction_q32;
}

constexpr uint64_t frames_from_q32_seconds(TurboAudioQ32 seconds,
                                            uint32_t rate) {
    if (rate == 0) return 0;
    const uint64_t whole_seconds = seconds >> 32;
    const uint64_t fractional_seconds =
        seconds & kTurboAudioQ32FractionMask;
    if (whole_seconds > std::numeric_limits<uint64_t>::max() / rate)
        return std::numeric_limits<uint64_t>::max();
    const uint64_t whole_frames = whole_seconds * rate;
    const uint64_t fractional_frames =
        (fractional_seconds * static_cast<uint64_t>(rate)) >> 32;
    return fractional_frames >
               std::numeric_limits<uint64_t>::max() - whole_frames
        ? std::numeric_limits<uint64_t>::max()
        : whole_frames + fractional_frames;
}

// Off is the faithful guest-clock path. Coupled keeps audio tied to the guest
// clock while Turbo is held. Muted suppresses new scheduled events.
// Decoupled releases events against wall time, independent of guest progress.
enum class TurboAudioMode : uint8_t {
    Off,
    Coupled,
    Muted,
    Decoupled,
};

// Class controls overflow safety. Essential and Music must never be silently
// discarded; a full queue reports FatalOverflow and requests fallback.
enum class TurboAudioEventClass : uint8_t {
    Essential,
    Music,
    TransientSfx,
};

// Only PcmBlock events may coalesce. Note/command boundaries stay distinct.
enum class TurboAudioEventType : uint8_t {
    PcmBlock,
    NoteOn,
    NoteOff,
    Command,
};

// Zero-duration/zero-frame events are valid control markers. They do not
// advance either clock and are never split; Stage 2 decides how to consume
// them. Zero-length PCM metadata follows the same no-time-advance rule.

enum class TurboAudioPushResult : uint8_t {
    Accepted,
    Coalesced,
    DroppedSfx,
    FatalOverflow,
    ClockOverflow,
    StaleEpoch,
    OutOfOrder,
    Muted,
};

// Event carries timing metadata only. The scheduler copies it on push, so a
// caller may safely reuse or mutate its input object afterwards. Within one
// epoch, both guest_start_q32 and wall_start_q32 must be nondecreasing in push
// order; equal timestamps retain stable FIFO order.
struct TurboAudioEvent {
    uint64_t epoch = 0;
    TurboAudioQ32 guest_start_q32 = 0;
    TurboAudioQ32 wall_start_q32 = 0;
    TurboAudioQ32 guest_duration_q32 = 0;
    TurboAudioQ32 wall_duration_q32 = 0;
    uint32_t sample_frames = 0;
    uint16_t stream_id = 0;
    TurboAudioEventClass event_class = TurboAudioEventClass::Music;
    TurboAudioEventType event_type = TurboAudioEventType::PcmBlock;
};

// Bounded telemetry. Counters are monotonic until the scheduler is destroyed;
// fallback_required is a persistent safety latch cleared only by explicit
// acknowledge_fallback().
struct TurboAudioStats {
    uint64_t drops = 0;             // Transient SFX dropped at queue-full.
    uint64_t coalesces = 0;
    uint64_t resyncs = 0;
    uint64_t stale_epochs = 0;
    uint64_t out_of_order = 0;
    uint64_t late_events = 0;
    uint64_t clock_gaps = 0;
    uint64_t clock_overflows = 0;
    uint64_t fatal_overflows = 0;
    uint64_t fallback_requests = 0;
    uint64_t muted_events = 0;
    uint64_t mode_changes = 0;
    uint64_t resets = 0;
    uint64_t emitted = 0;
    bool fallback_required = false;
    uint32_t queue_depth = 0;
    uint32_t queue_high_water = 0;
    TurboAudioQ32 guest_cursor_q32 = 0;
    TurboAudioQ32 wall_cursor_q32 = 0;
};

class TurboAudioScheduler {
public:
    // Fixed storage keeps the policy bounded and deterministic. This is
    // metadata capacity, not a PCM-buffer size.
    static constexpr std::size_t kCapacity = 64;

    TurboAudioScheduler() = default;

    TurboAudioMode mode() const { return mode_; }
    bool turbo_active() const { return turbo_active_; }
    uint64_t epoch() const { return epoch_; }
    TurboAudioQ32 guest_cursor_q32() const { return guest_cursor_q32_; }
    TurboAudioQ32 wall_cursor_q32() const { return wall_cursor_q32_; }
    std::size_t size() const { return count_; }
    bool empty() const { return count_ == 0; }
    const TurboAudioStats& stats() const { return stats_; }

    // Timeline reset, suitable for startup/state reset. Returns to faithful
    // Off mode and starts a new event epoch. It does not clear an unhandled
    // fallback latch; acknowledge_fallback() is the only clear operation.
    void reset(TurboAudioQ32 guest_cursor_q32 = 0,
               TurboAudioQ32 wall_cursor_q32 = 0);

    // Timeline discontinuity, suitable for savestate load. Keeps the chosen
    // mode but drops events from the old guest timeline and bumps epoch.
    void resync(TurboAudioQ32 guest_cursor_q32,
                TurboAudioQ32 wall_cursor_q32);

    // Mode boundaries clear queued events and bump epoch. set_mode is the
    // direct form; enter_turbo/exit_turbo also set anchor cursors.
    void set_mode(TurboAudioMode mode);
    void enter_turbo(TurboAudioMode mode, TurboAudioQ32 guest_cursor_q32,
                     TurboAudioQ32 wall_cursor_q32);
    void exit_turbo(TurboAudioQ32 guest_cursor_q32,
                    TurboAudioQ32 wall_cursor_q32);

    // Repeated enter_turbo for the same active mode is idempotent: anchors are
    // accepted only on a real boundary, so queued events survive repeated
    // input-pump reports.
    bool advance_guest_q32(TurboAudioQ32 duration_q32);
    bool advance_wall_q32(TurboAudioQ32 duration_q32);

    // Safety latch for a fatal queue/clock condition. Timeline changes do not
    // clear it; the owner must acknowledge fallback handling explicitly.
    void acknowledge_fallback() { stats_.fallback_required = false; }

    // Event.epoch must equal epoch(). Adjacent same-stream PcmBlock events
    // coalesce when both clocks are contiguous; full storage drops only SFX.
    TurboAudioPushResult push(const TurboAudioEvent& event);

    // Copies and removes the oldest event only when its active clock is due.
    // Stage 1 pops whole events; Stage 2 will consume their duration/frame
    // fields when it wires real audio payloads.
    bool pop_due(TurboAudioEvent& event);

    // Ownership is the emulation thread. The SDL callback must never call
    // this object; live wiring can add an explicit handoff later. Therefore
    // this scaffold needs no atomic SPSC queue yet.

private:
    enum class CoalesceResult : uint8_t {
        No,
        Yes,
        ClockOverflow,
    };

    static CoalesceResult can_coalesce(const TurboAudioEvent& older,
                                       const TurboAudioEvent& newer);
    static bool non_droppable(TurboAudioEventClass event_class);
    static bool checked_add(TurboAudioQ32 a, TurboAudioQ32 b,
                            TurboAudioQ32& result);
    void clear_queue();
    void clear_ordering();
    void bump_epoch();
    TurboAudioPushResult clock_overflow();
    void begin_epoch(TurboAudioQ32 guest_cursor_q32,
                     TurboAudioQ32 wall_cursor_q32);
    void remember_event(const TurboAudioEvent& event);
    bool monotonic(const TurboAudioEvent& event) const;
    bool has_clock_gap(const TurboAudioEvent& event) const;
    bool due(const TurboAudioEvent& event) const;

    std::array<TurboAudioEvent, kCapacity> queue_{};
    std::size_t head_ = 0;
    std::size_t count_ = 0;
    TurboAudioMode mode_ = TurboAudioMode::Off;
    bool turbo_active_ = false;
    uint64_t epoch_ = 1;
    TurboAudioQ32 guest_cursor_q32_ = 0;
    TurboAudioQ32 wall_cursor_q32_ = 0;
    bool have_last_event_ = false;
    TurboAudioQ32 last_guest_start_q32_ = 0;
    TurboAudioQ32 last_wall_start_q32_ = 0;
    TurboAudioQ32 last_guest_end_q32_ = 0;
    TurboAudioQ32 last_wall_end_q32_ = 0;
    TurboAudioStats stats_{};
};

}  // namespace gba
