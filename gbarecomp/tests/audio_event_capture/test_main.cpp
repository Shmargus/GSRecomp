#include "audio_event_capture.h"

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <limits>

using gba::AudioCaptureClass;
using gba::AudioCaptureEvent;
using gba::AudioCaptureKind;
using gba::AudioCaptureResult;
using gba::AudioEventCapture;

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

int main() {
    static_assert(sizeof(AudioEventCapture) < 256,
                  "capture queue must stay lazy outside diagnostics");
    AudioEventCapture capture;
    CHECK(capture.record_psg_register(0, 0x065, 0x80, 1, true) ==
           AudioCaptureResult::Disabled);

    capture.set_enabled(true);
    const uint64_t epoch = capture.epoch();
    for (uint8_t channel = 0; channel < 12; ++channel)
        CHECK(capture.record_mp2k_channel(
                   0, 0x100, 0x200, channel, 0x10, 4, channel, 0x24,
                   1) == AudioCaptureResult::Accepted);
    CHECK(capture.record_psg_register(1, 0x065, 0x80, 1, true) ==
           AudioCaptureResult::Accepted);
    CHECK(capture.record_psg_wave_ram(2, 3, 1, 0xAB, 0xAA) ==
           AudioCaptureResult::Accepted);
    CHECK(capture.record_direct_fifo_cpu_word(3, 0, 0x12345678) ==
           AudioCaptureResult::Accepted);
    CHECK(capture.record_direct_fifo_dma_word(4, 1, 0x02000000,
                                               0xCAFEBABE) ==
           AudioCaptureResult::Accepted);
    uint8_t asset[] = {1, 2, 3};
    CHECK(capture.record_asset_metadata(5, 7, asset, sizeof(asset)) ==
           AudioCaptureResult::Accepted);
    CHECK(capture.size() == 17);

    AudioCaptureEvent event{};
    CHECK(capture.pop(event) && event.epoch == epoch &&
           event.kind == AudioCaptureKind::Mp2kChannelWrite &&
           event.channel == 0);
    bool saw_asset = false;
    while (capture.pop(event)) {
        if (event.kind == AudioCaptureKind::AssetMetadata) {
            saw_asset = true;
            CHECK(event.aux0 == 7 && event.aux1 == 0x56CF37ABu &&
                  event.width == 3);
        }
    }
    CHECK(saw_asset);
    CHECK(capture.empty());

    // Epoch mismatch rejects stale synthetic events.
    AudioCaptureEvent stale{};
    stale.epoch = epoch;
    stale.guest_time_q32 = 0;
    capture.reset();
    CHECK(capture.epoch() != epoch);
    CHECK(capture.push(stale) == AudioCaptureResult::StaleEpoch);

    // Stable equal-time insertion; reverse time is rejected.
    CHECK(capture.record_psg_register(100, 0x060, 1, 1, false) ==
           AudioCaptureResult::Accepted);
    CHECK(capture.record_psg_register(99, 0x061, 2, 1, false) ==
           AudioCaptureResult::OutOfOrder);

    // Q32 cycle conversion has exact zero and bounded overflow behavior.
    // These literals are independently computed fixed-point references.
    constexpr gba::TurboAudioQ32 kQ32OneSecond = 0x0000000100000000ull;
    constexpr gba::TurboAudioQ32 kQ32HalfSecond = 0x0000000080000000ull;
    gba::TurboAudioQ32 q32 = 0;
    CHECK(AudioEventCapture::q32_seconds_from_cycles(
        0, AudioEventCapture::kSystemHz, q32) && q32 == 0);
    CHECK(AudioEventCapture::q32_seconds_from_cycles(
        AudioEventCapture::kSystemHz, AudioEventCapture::kSystemHz, q32) &&
           q32 == kQ32OneSecond);
    CHECK(AudioEventCapture::q32_seconds_from_cycles(
        AudioEventCapture::kSystemHz + AudioEventCapture::kSystemHz / 2,
        AudioEventCapture::kSystemHz, q32) &&
           q32 == kQ32OneSecond + kQ32HalfSecond);
    CHECK(AudioEventCapture::q32_seconds_from_cycles(
        AudioEventCapture::kSystemHz - 1u, AudioEventCapture::kSystemHz,
        q32) && q32 == kQ32OneSecond - 256u);
    CHECK(AudioEventCapture::q32_seconds_from_cycles(
        std::numeric_limits<uint32_t>::max(),
        std::numeric_limits<uint32_t>::max(), q32) &&
           q32 == kQ32OneSecond);
    CHECK(!AudioEventCapture::q32_seconds_from_cycles(1, 0, q32));
    CHECK(!AudioEventCapture::q32_seconds_from_cycles(
        std::numeric_limits<uint64_t>::max(), 1, q32));
    CHECK(q32 == std::numeric_limits<uint64_t>::max());

    // Unsupported dynamic data fails closed without retaining a pointer.
    CHECK(capture.record_asset_metadata(200, 8, nullptr, 0) ==
           AudioCaptureResult::UnsupportedAsset);
    CHECK(capture.stats().fallback_required);
    capture.reset();
    CHECK(capture.stats().fallback_required);
    capture.acknowledge_fallback();
    CHECK(!capture.stats().fallback_required);
    CHECK(capture.record_producer_block(200, 300, 9, 1, 13379, 16,
                                         0x03001000, 4, 224, 13379) ==
           AudioCaptureResult::Accepted);
    // kSystemHz is 2^24, so integer cycle timestamps map exactly by 2^8.
    CHECK(capture.pop(event) &&
           event.kind == AudioCaptureKind::Mp2kProducerBlock &&
           event.block_id == 9 && event.route == 1 &&
           event.sample_rate == 13379 && event.frame_count == 16 &&
           event.address == 0x03001000 && event.ordinal == 4 &&
           event.guest_start_cursor == 224 &&
           event.guest_cursor_rate == 13379 &&
           event.guest_start_q32 == (200ull << 8) &&
           event.guest_time_q32 == (300ull << 8));

    // Every real event requests fallback at a full bounded queue, including
    // events still classified Unknown.
    AudioEventCapture full;
    full.set_enabled(true);
    AudioCaptureEvent unknown{};
    unknown.epoch = full.epoch();
    for (std::size_t i = 0; i < AudioEventCapture::kCapacity; ++i) {
        unknown.aux0 = static_cast<uint32_t>(i);
        CHECK(full.push(unknown) == AudioCaptureResult::Accepted);
    }
    unknown.aux0 = 0xFFFFu;
    CHECK(full.push(unknown) == AudioCaptureResult::FatalOverflow);
    unknown.classification = AudioCaptureClass::Essential;
    CHECK(full.push(unknown) == AudioCaptureResult::FatalOverflow);
    CHECK(full.stats().fallback_required);
    CHECK(full.stats().queue_high_water == AudioEventCapture::kCapacity);

    return 0;
}
