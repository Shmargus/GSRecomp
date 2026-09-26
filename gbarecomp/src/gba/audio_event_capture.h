// Bounded, emulation-thread-owned audio event capture.
//
// This is observability only. It stores fixed-size metadata and snapshots,
// never guest pointers or PCM buffers. The canonical audio path may call it
// at existing MMIO/MP2K/DMA hook points; disabled capture is a no-op.
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>

#include "turbo_audio_scheduler.h"

namespace gba {

enum class AudioCaptureClass : uint8_t {
    Unknown,
    Essential,
};

enum class AudioCaptureKind : uint8_t {
    Mp2kChannelWrite,
    Mp2kProducerBlock,
    PsgRegisterWrite,
    PsgWaveRamWrite,
    DirectFifoCpuWord,
    DirectFifoDmaWord,
    DirectFifoTimer,
    DirectFifoConsume,
    DirectFifoControl,
    AssetMetadata,
};

enum class AudioCaptureResult : uint8_t {
    Disabled,
    Accepted,
    FatalOverflow,
    ClockOverflow,
    StaleEpoch,
    OutOfOrder,
    UnsupportedAsset,
};

struct AudioCaptureEvent {
    uint64_t epoch = 0;
    TurboAudioQ32 guest_time_q32 = 0;
    TurboAudioQ32 guest_start_q32 = 0;
    uint64_t sequence = 0;
    uint32_t ordinal = 0;
    AudioCaptureClass classification = AudioCaptureClass::Unknown;
    AudioCaptureKind kind = AudioCaptureKind::PsgRegisterWrite;
    uint8_t channel = 0xFF;
    uint8_t width = 0;
    uint16_t flags = 0;
    uint32_t pc = 0;
    uint32_t address = 0;
    uint32_t value = 0;
    uint32_t before = 0;
    uint32_t aux0 = 0;
    uint32_t aux1 = 0;
    uint64_t block_id = 0;
    uint32_t sample_rate = 0;
    uint32_t frame_count = 0;
    uint64_t guest_start_cursor = 0;
    uint32_t guest_cursor_rate = 0;
    uint8_t route = 0xFF;
};

struct AudioCaptureStats {
    uint64_t accepted = 0;
    uint64_t fatal_overflows = 0;
    uint64_t clock_overflows = 0;
    uint64_t stale_epochs = 0;
    uint64_t out_of_order = 0;
    uint64_t unsupported_assets = 0;
    uint64_t resets = 0;
    uint32_t queue_depth = 0;
    uint32_t queue_high_water = 0;
    bool fallback_required = false;
};

class AudioEventCapture {
public:
    // Metadata only. This is deliberately bounded and preallocated.
    static constexpr std::size_t kCapacity = 2048;
    static constexpr std::size_t kMaxAssetBytes = 64;
    static constexpr uint32_t kSystemHz = 16777216u;

    AudioEventCapture() = default;

    bool enabled() const { return enabled_; }
    void set_enabled(bool enabled);
    bool allocation_failed() const { return allocation_failed_; }
    uint64_t epoch() const { return epoch_; }
    std::size_t size() const { return count_; }
    bool empty() const { return count_ == 0; }
    const AudioCaptureStats& stats() const { return stats_; }

    // Starts a new capture epoch and drops queued events. An unacknowledged
    // fatal condition survives reset, so fallback handling cannot be missed.
    void reset();
    void acknowledge_fallback() { stats_.fallback_required = false; }

    // Copies the oldest immutable event out of the queue. A full queue is a
    // fatal condition for both Essential and still-unclassified real events.
    bool pop(AudioCaptureEvent& event);
    AudioCaptureResult push(const AudioCaptureEvent& event);
    // Callers must feed back every result. Fatal and ordering failures latch
    // fallback; this method is idempotent because push already counts them.
    void note_result(AudioCaptureResult result);

    // Safe Q32 guest time conversion. No floating point or guest memory.
    static bool q32_seconds_from_cycles(uint64_t cycles, uint32_t system_hz,
                                        TurboAudioQ32& out);

    struct Timestamp {
        uint64_t cycle_start = 0;
        uint32_t ordinal = 0;
    };

    AudioCaptureResult record_mp2k_channel(
        uint64_t cycles, uint32_t pc, uint32_t addr, uint32_t value,
        uint32_t before, uint32_t width, uint8_t channel, uint16_t field,
        uint8_t thumb, AudioCaptureClass classification =
                           AudioCaptureClass::Unknown);
    AudioCaptureResult record_psg_register(
        uint64_t cycles, uint32_t off, uint32_t value, uint8_t width,
        bool trigger, uint32_t before = 0, uint32_t ordinal = 0);
    AudioCaptureResult record_psg_wave_ram(
        uint64_t cycles, uint32_t offset, uint8_t bank, uint32_t value,
        uint32_t before, uint8_t width = 1, uint32_t ordinal = 0);
    AudioCaptureResult record_direct_fifo_cpu_word(
        uint64_t cycles, uint8_t fifo, uint32_t value, uint8_t width = 4);
    AudioCaptureResult record_direct_fifo_dma_word(
        uint64_t cycles, uint8_t fifo, uint32_t source_addr, uint32_t value,
        uint32_t ordinal = 0);
    AudioCaptureResult record_direct_fifo_timer(
        uint64_t cycles, uint8_t timer, uint8_t active_fifo_mask,
        uint32_t ordinal = 0);
    AudioCaptureResult record_direct_fifo_consume(
        uint64_t cycles, uint8_t fifo, uint32_t source_addr, uint8_t byte_index,
        int8_t sample, uint32_t ordinal = 0);
    AudioCaptureResult record_producer_block(
        uint64_t cycle_start, uint64_t cycle_end, uint64_t block_id,
        uint8_t route, uint32_t sample_rate, uint32_t frame_count,
        uint32_t guest_start_addr, uint32_t ordinal = 0,
        uint64_t guest_start_cursor = UINT64_MAX,
        uint32_t guest_cursor_rate = 0);

    // Dynamic assets are accepted only as bounded copied metadata. A null,
    // oversized, or otherwise unsupported payload fails closed and latches
    // fallback; no pointer is retained.
    AudioCaptureResult record_asset_metadata(
        uint64_t cycles, uint32_t asset_id, const uint8_t* bytes,
        std::size_t size, AudioCaptureClass classification =
                              AudioCaptureClass::Unknown);

private:
    AudioCaptureResult timestamp(uint64_t cycles, TurboAudioQ32& out) const;
    AudioCaptureResult timestamp(Timestamp stamp, TurboAudioQ32& out) const;
    AudioCaptureResult fatal_clock();
    AudioCaptureResult fatal_asset();
    AudioCaptureResult push_timestamped(AudioCaptureEvent event,
                                        uint64_t cycles,
                                        uint64_t cycle_start = UINT64_MAX,
                                        uint32_t ordinal = 0);

    // Capture is diagnostic/future PSG observability only. Keep the normal
    // GbaAudio object small; allocate the bounded queue only on explicit
    // capture enable, never from the SDL callback or canonical audio path.
    std::unique_ptr<AudioCaptureEvent[]> queue_;
    std::size_t head_ = 0;
    std::size_t count_ = 0;
    bool enabled_ = false;
    bool allocation_failed_ = false;
    uint64_t epoch_ = 1;
    bool have_last_time_ = false;
    TurboAudioQ32 last_time_q32_ = 0;
    uint64_t next_sequence_ = 0;
    AudioCaptureStats stats_{};
};

}  // namespace gba
