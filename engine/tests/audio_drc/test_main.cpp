#define RECOMP_AUDIO_DRC_IMPL
#include "recomp_audio_drc.h"
#include "frame_timing.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

namespace {

void expect(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

rab_config test_config(double source_rate, double host_rate) {
    rab_config cfg;
    rab_config_defaults(&cfg);
    cfg.channels = 1;
    cfg.source_rate = source_rate;
    cfg.host_rate = host_rate;
    cfg.target_ms = 100.0;
    cfg.ring_ms = 6000.0;
    cfg.kp = 0.0;  // isolate the requested clock ratio from the fill servo
    cfg.slew_pp_per_s = 0.0;
    return cfg;
}

double pull_delta(rab_bridge& bridge, int frames) {
    const double before = bridge.out_pos;
    std::vector<int16_t> out(static_cast<std::size_t>(frames));
    rab_pull(&bridge, out.data(), frames);
    return bridge.out_pos - before;
}

}  // namespace

int main() {
    {
        rab_config cfg = test_config(65536.0, 48000.0);
        rab_bridge bridge{};
        expect(rab_init(&bridge, &cfg) == 0, "host-rate bridge did not initialize");
        std::vector<int16_t> source(100000, 1000);
        rab_push(&bridge, source.data(), static_cast<int>(source.size()));

        const double expected = 1000.0 * 65536.0 / 48000.0;
        expect(std::abs(pull_delta(bridge, 1000) - expected) < 1e-9,
               "65536 to 48000 conversion changed the source cursor incorrectly");
        expect(bridge.stats.overflow_drops == 0,
               "host-rate conversion dropped source frames");
        rab_free(&bridge);
    }

    {
        rab_config cfg = test_config(1000.0, 1000.0);
        rab_bridge bridge{};
        expect(rab_init(&bridge, &cfg) == 0, "timing bridge did not initialize");
        std::vector<int16_t> source(5000, 2000);
        rab_push(&bridge, source.data(), static_cast<int>(source.size()));

        expect(std::abs(pull_delta(bridge, 1000) - 1000.0) < 1e-9,
               "faithful timing did not consume one source frame per output frame");

        const double enhanced_scale = gbarecomp::kEnhancedAudioRateScale;
        expect(std::abs(enhanced_scale - 1.0045623779296875) < 1e-15,
               "enhanced audio clock ratio changed");
        rab_set_rate_scale(&bridge, enhanced_scale);
        expect(std::abs(bridge.rate_scale - enhanced_scale) < 1e-15,
               "enhanced audio clock ratio was not applied");
        const double enhanced_delta = pull_delta(bridge, 1000);
        expect(std::abs(enhanced_delta - 1000.0 * enhanced_scale) < 1e-9,
               "enhanced timing did not resample the guest audio clock");

        rab_set_rate_scale(&bridge, std::numeric_limits<double>::quiet_NaN());
        expect(bridge.rate_scale == 1.0,
               "invalid audio clock ratio did not fall back to faithful timing");
        expect(bridge.stats.overflow_drops == 0,
               "enhanced timing dropped source frames");
        rab_push(&bridge, source.data(), 256);
        rab_reset(&bridge);
        expect(bridge.in_count == 0 && bridge.out_pos == bridge.half - 1,
               "audio bridge reset did not discard the old guest timeline");
        expect(rab_fill_ms(&bridge) == 0.0 && bridge.primed == 0,
               "audio bridge reset retained queued audio");
        expect(std::abs(bridge.rate_scale - 1.0) < 1e-15,
               "audio bridge reset changed the selected clock scale");
        rab_free(&bridge);
    }

    std::puts("audio DRC tests passed");
    return 0;
}
