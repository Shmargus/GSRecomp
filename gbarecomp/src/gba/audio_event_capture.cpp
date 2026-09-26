#include "audio_event_capture.h"

#include <algorithm>
#include <new>
#include <utility>

namespace gba {

void AudioEventCapture::set_enabled(bool enabled) {
    if (!enabled) {
        if (!enabled_ && !queue_) return;
        enabled_ = false;
        queue_.reset();
        reset();
        return;
    }
    if (enabled_ && queue_) return;

    std::unique_ptr<AudioCaptureEvent[]> queue(
        new (std::nothrow) AudioCaptureEvent[kCapacity]);
    if (!queue) {
        enabled_ = false;
        queue_.reset();
        allocation_failed_ = true;
        ++stats_.fatal_overflows;
        stats_.fallback_required = true;
        return;
    }
    queue_ = std::move(queue);
    enabled_ = true;
    allocation_failed_ = false;
    reset();
}

void AudioEventCapture::reset() {
    head_ = 0;
    count_ = 0;
    have_last_time_ = false;
    last_time_q32_ = 0;
    next_sequence_ = 0;
    if (epoch_ != std::numeric_limits<uint64_t>::max()) ++epoch_;
    ++stats_.resets;
    stats_.queue_depth = 0;
}

bool AudioEventCapture::pop(AudioCaptureEvent& event) {
    if (count_ == 0 || !queue_) return false;
    event = queue_[head_];
    head_ = (head_ + 1) % kCapacity;
    --count_;
    stats_.queue_depth = static_cast<uint32_t>(count_);
    return true;
}

AudioCaptureResult AudioEventCapture::push(const AudioCaptureEvent& event) {
    if (!enabled_ || !queue_) return AudioCaptureResult::Disabled;
    if (event.epoch != epoch_) {
        ++stats_.stale_epochs;
        return AudioCaptureResult::StaleEpoch;
    }
    if (have_last_time_ && event.guest_time_q32 < last_time_q32_) {
        ++stats_.out_of_order;
        stats_.fallback_required = true;
        return AudioCaptureResult::OutOfOrder;
    }
    if (count_ == kCapacity) {
        // Unknown is deliberately fail-closed too: a real audio event must
        // never disappear silently before evidence classifies it.
        ++stats_.fatal_overflows;
        stats_.fallback_required = true;
        return AudioCaptureResult::FatalOverflow;
    }
    if (next_sequence_ == std::numeric_limits<uint64_t>::max()) {
        ++stats_.fatal_overflows;
        stats_.fallback_required = true;
        return AudioCaptureResult::FatalOverflow;
    }
    AudioCaptureEvent stored = event;
    stored.sequence = next_sequence_++;
    queue_[(head_ + count_) % kCapacity] = stored;
    ++count_;
    have_last_time_ = true;
    last_time_q32_ = event.guest_time_q32;
    ++stats_.accepted;
    stats_.queue_depth = static_cast<uint32_t>(count_);
    stats_.queue_high_water = std::max(stats_.queue_high_water,
                                       stats_.queue_depth);
    return AudioCaptureResult::Accepted;
}

void AudioEventCapture::note_result(AudioCaptureResult result) {
    switch (result) {
        case AudioCaptureResult::Disabled:
        case AudioCaptureResult::Accepted:
            return;
        default:
            // The producer methods already account and latch the failure;
            // this explicit acknowledgement keeps hook call sites honest.
            stats_.fallback_required = true;
            return;
    }
}

bool AudioEventCapture::q32_seconds_from_cycles(
    uint64_t cycles, uint32_t system_hz, TurboAudioQ32& out) {
    if (system_hz == 0) {
        out = 0;
        return false;
    }
    const uint64_t whole = cycles / system_hz;
    const uint64_t remainder = cycles % system_hz;
    if (whole > (std::numeric_limits<uint64_t>::max() >> 32)) {
        out = std::numeric_limits<uint64_t>::max();
        return false;
    }
    const uint64_t whole_q32 = whole << 32;
    // Portable 32-step binary long division. Avoid relying on a compiler's
    // wider integer type or an intermediate remainder shift.
    uint64_t fraction_q32 = 0;
    uint64_t scaled_remainder = remainder;
    for (int bit = 31; bit >= 0; --bit) {
        scaled_remainder <<= 1;
        fraction_q32 <<= 1;
        if (scaled_remainder >= system_hz) {
            scaled_remainder -= system_hz;
            fraction_q32 |= 1u;
        }
    }
    if (fraction_q32 > std::numeric_limits<uint64_t>::max() - whole_q32) {
        out = std::numeric_limits<uint64_t>::max();
        return false;
    }
    out = whole_q32 + fraction_q32;
    return true;
}

AudioCaptureResult AudioEventCapture::timestamp(
    uint64_t cycles, TurboAudioQ32& out) const {
    return timestamp(Timestamp{cycles, 0}, out);
}

AudioCaptureResult AudioEventCapture::timestamp(
    Timestamp stamp, TurboAudioQ32& out) const {
    return q32_seconds_from_cycles(stamp.cycle_start, kSystemHz, out)
        ? AudioCaptureResult::Accepted : AudioCaptureResult::ClockOverflow;
}

AudioCaptureResult AudioEventCapture::fatal_clock() {
    ++stats_.clock_overflows;
    ++stats_.fatal_overflows;
    stats_.fallback_required = true;
    return AudioCaptureResult::ClockOverflow;
}

AudioCaptureResult AudioEventCapture::fatal_asset() {
    ++stats_.unsupported_assets;
    ++stats_.fatal_overflows;
    stats_.fallback_required = true;
    return AudioCaptureResult::UnsupportedAsset;
}

AudioCaptureResult AudioEventCapture::push_timestamped(
    AudioCaptureEvent event, uint64_t cycles, uint64_t cycle_start,
    uint32_t ordinal) {
    if (!enabled_) return AudioCaptureResult::Disabled;
    const AudioCaptureResult time_result = timestamp(cycles,
                                                     event.guest_time_q32);
    if (time_result != AudioCaptureResult::Accepted) return fatal_clock();
    if (cycle_start == UINT64_MAX) cycle_start = cycles;
    const AudioCaptureResult start_result = timestamp(
        Timestamp{cycle_start, ordinal}, event.guest_start_q32);
    if (start_result != AudioCaptureResult::Accepted) return fatal_clock();
    event.epoch = epoch_;
    event.ordinal = ordinal;
    return push(event);
}

AudioCaptureResult AudioEventCapture::record_mp2k_channel(
    uint64_t cycles, uint32_t pc, uint32_t addr, uint32_t value,
    uint32_t before, uint32_t width, uint8_t channel, uint16_t field,
    uint8_t thumb, AudioCaptureClass classification) {
    AudioCaptureEvent event{};
    event.classification = classification;
    event.kind = AudioCaptureKind::Mp2kChannelWrite;
    event.channel = channel;
    event.width = static_cast<uint8_t>(std::min<uint32_t>(width, 0xFFu));
    event.flags = thumb ? 1u : 0u;
    event.pc = pc;
    event.address = addr;
    event.value = value;
    event.before = before;
    event.aux0 = field;
    return push_timestamped(event, cycles);
}

AudioCaptureResult AudioEventCapture::record_psg_register(
    uint64_t cycles, uint32_t off, uint32_t value, uint8_t width,
    bool trigger, uint32_t before, uint32_t ordinal) {
    AudioCaptureEvent event{};
    const bool touches_fifo_control = off < 0x084 && off + width > 0x082;
    event.kind = touches_fifo_control
        ? AudioCaptureKind::DirectFifoControl
        : AudioCaptureKind::PsgRegisterWrite;
    event.width = width;
    event.flags = trigger ? 1u : 0u;
    event.address = off;
    event.value = value;
    event.before = before;
    return push_timestamped(event, cycles, UINT64_MAX, ordinal);
}

AudioCaptureResult AudioEventCapture::record_psg_wave_ram(
    uint64_t cycles, uint32_t offset, uint8_t bank, uint32_t value,
    uint32_t before, uint8_t width, uint32_t ordinal) {
    AudioCaptureEvent event{};
    event.kind = AudioCaptureKind::PsgWaveRamWrite;
    event.channel = 3;
    event.width = width;
    event.address = offset;
    event.value = value;
    event.before = before;
    event.aux0 = bank;
    return push_timestamped(event, cycles, UINT64_MAX, ordinal);
}

AudioCaptureResult AudioEventCapture::record_direct_fifo_cpu_word(
    uint64_t cycles, uint8_t fifo, uint32_t value, uint8_t width) {
    AudioCaptureEvent event{};
    event.kind = AudioCaptureKind::DirectFifoCpuWord;
    event.channel = fifo;
    event.width = width;
    event.value = value;
    return push_timestamped(event, cycles);
}

AudioCaptureResult AudioEventCapture::record_direct_fifo_dma_word(
    uint64_t cycles, uint8_t fifo, uint32_t source_addr, uint32_t value,
    uint32_t ordinal) {
    AudioCaptureEvent event{};
    event.kind = AudioCaptureKind::DirectFifoDmaWord;
    event.channel = fifo;
    event.width = 4;
    event.address = source_addr;
    event.value = value;
    return push_timestamped(event, cycles, UINT64_MAX, ordinal);
}

AudioCaptureResult AudioEventCapture::record_direct_fifo_timer(
    uint64_t cycles, uint8_t timer, uint8_t active_fifo_mask,
    uint32_t ordinal) {
    AudioCaptureEvent event{};
    event.kind = AudioCaptureKind::DirectFifoTimer;
    event.channel = timer;
    event.aux0 = active_fifo_mask;
    return push_timestamped(event, cycles, UINT64_MAX, ordinal);
}

AudioCaptureResult AudioEventCapture::record_direct_fifo_consume(
    uint64_t cycles, uint8_t fifo, uint32_t source_addr, uint8_t byte_index,
    int8_t sample, uint32_t ordinal) {
    AudioCaptureEvent event{};
    event.kind = AudioCaptureKind::DirectFifoConsume;
    event.channel = fifo;
    event.width = 1;
    event.address = source_addr;
    event.value = static_cast<uint8_t>(sample);
    event.aux0 = byte_index;
    return push_timestamped(event, cycles, UINT64_MAX, ordinal);
}

AudioCaptureResult AudioEventCapture::record_producer_block(
    uint64_t cycle_start, uint64_t cycle_end, uint64_t block_id,
    uint8_t route, uint32_t sample_rate, uint32_t frame_count,
    uint32_t guest_start_addr, uint32_t ordinal,
    uint64_t guest_start_cursor, uint32_t guest_cursor_rate) {
    AudioCaptureEvent event{};
    event.kind = AudioCaptureKind::Mp2kProducerBlock;
    event.block_id = block_id;
    event.route = route;
    event.sample_rate = sample_rate;
    event.frame_count = frame_count;
    event.address = guest_start_addr;
    if (guest_start_cursor != UINT64_MAX) {
        event.guest_start_cursor = guest_start_cursor;
        event.guest_cursor_rate = guest_cursor_rate;
    }
    return push_timestamped(event, cycle_end, cycle_start, ordinal);
}

AudioCaptureResult AudioEventCapture::record_asset_metadata(
    uint64_t cycles, uint32_t asset_id, const uint8_t* bytes,
    std::size_t size, AudioCaptureClass classification) {
    if (!enabled_) return AudioCaptureResult::Disabled;
    if (!bytes || size > kMaxAssetBytes) return fatal_asset();
    uint32_t hash = 2166136261u;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    AudioCaptureEvent event{};
    event.classification = classification;
    event.kind = AudioCaptureKind::AssetMetadata;
    event.width = static_cast<uint8_t>(size);
    event.aux0 = asset_id;
    event.aux1 = hash;
    return push_timestamped(event, cycles);
}

}  // namespace gba
