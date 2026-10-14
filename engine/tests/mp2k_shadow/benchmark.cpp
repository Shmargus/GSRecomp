#include "mp2k_shadow.h"
#include "gba_audio.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <vector>

#if defined(_WIN32)
#include <cstdlib>
#else
#include <cstdlib>
#endif

[[noreturn]] void benchmark_fail(const char* expression, const char* file,
                                 int line) {
    std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", expression, file,
                 line);
    std::exit(1);
}

#define CHECK(expression) \
    do { \
        if (!(expression)) benchmark_fail(#expression, __FILE__, __LINE__); \
    } while (false)

namespace gba {

// Test-only access to the deterministic voice state. This makes the
// benchmark sensitive to sample fetches, not only the delayed producer bus.
struct Mp2kShadowWallFixture {
    static uint64_t voice_hash(const Mp2kShadow& shadow) {
        uint64_t hash = 0x84222325cbf29ce4ull;
        for (const auto& voice : shadow.voices_) {
            hash ^= static_cast<uint64_t>(voice.on);
            hash = (hash << 7) ^ voice.pos_index;
            hash = (hash << 7) ^ voice.pos_frac;
            hash = (hash << 7) ^ static_cast<uint32_t>(voice.last_sample_x2);
            hash = (hash << 7) ^ static_cast<uint32_t>(voice.chk_sample_x2);
            uint32_t bits = 0;
            std::memcpy(&bits, &voice.chk_hold, sizeof(bits));
            hash = (hash << 7) ^ bits;
        }
        return hash;
    }

    static int32_t first_last_sample(const Mp2kShadow& shadow) {
        return shadow.voices_[0].last_sample_x2;
    }
};

}  // namespace gba

namespace {

void set_probe(bool enabled) {
#if defined(_WIN32)
    _putenv_s("GBARECOMP_AUDIO_PROBE", enabled ? "1" : "");
#else
    if (enabled) setenv("GBARECOMP_AUDIO_PROBE", "1", 1);
    else unsetenv("GBARECOMP_AUDIO_PROBE");
#endif
}

void set_shadow_request(bool enabled) {
#if defined(_WIN32)
    _putenv_s("GBARECOMP_AUDIO_SHADOW", enabled ? "1" : "");
#else
    if (enabled) setenv("GBARECOMP_AUDIO_SHADOW", "1", 1);
    else unsetenv("GBARECOMP_AUDIO_SHADOW");
#endif
}

void put32(std::vector<uint8_t>& bytes, std::size_t off, uint32_t value) {
    bytes[off + 0] = static_cast<uint8_t>(value);
    bytes[off + 1] = static_cast<uint8_t>(value >> 8);
    bytes[off + 2] = static_cast<uint8_t>(value >> 16);
    bytes[off + 3] = static_cast<uint8_t>(value >> 24);
}

uint64_t mix_hash(uint64_t hash, uint64_t value) {
    hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
    return hash;
}

uint64_t float_bits(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

struct RunResult {
    uint64_t hash = 0;
    bool trace_active = false;
    uint64_t elapsed_ns = 0;
};

RunResult run(bool probe) {
    set_probe(probe);

    std::vector<uint8_t> rom(0x140, 0);
    // Synthetic Camelot PWM payload. No ROM/BIOS bytes are used.
    rom[0x110] = 0;
    rom[0x111] = 0x40;
    rom[0x112] = 0x08;
    rom[0x113] = 0x10;
    rom[0x114] = 0x40;
    std::vector<uint8_t> ewram(0x400, 0);
    std::vector<uint8_t> iwram(0x8000, 0);
    const uint32_t sound_info = 0x03001000u;
    const std::size_t si = sound_info & 0x7FFFu;
    put32(iwram, 0x7FF0u, sound_info);
    put32(iwram, si, gba::kMp2kMagicBase);
    iwram[si + 0x06] = gba::kMp2kMaxChans;
    iwram[si + 0x0B] = 7;
    put32(iwram, si + 0x10, 8);
    put32(iwram, si + 0x14, 65536);
    for (int ch = 0; ch < gba::kMp2kMaxChans; ++ch) {
        const std::size_t base = si + gba::kSoundChansOff +
            static_cast<std::size_t>(ch) * gba::kSoundChanStride;
        iwram[base + 0x00] = 0x80;
        iwram[base + 0x01] = 0;
        iwram[base + 0x02] = 127;
        iwram[base + 0x03] = 127;
        iwram[base + 0x04] = 0xFF;
        iwram[base + 0x05] = 0xC0;
        iwram[base + 0x06] = 0x80;
        iwram[base + 0x07] = 0x40;
        put32(iwram, base + 0x18, 0);
        put32(iwram, base + 0x1C, 0);
        put32(iwram, base + 0x20, 0x00004000u);
        put32(iwram, base + 0x24, 0x08000100u);
    }
    std::array<uint8_t, 32> block{};
    for (std::size_t i = 0; i < block.size(); ++i)
        block[i] = static_cast<uint8_t>(0x12u + i);
    std::copy(block.begin(), block.end(), ewram.begin());

    gba::MemView mem{rom.data(), rom.size(), ewram.data(), ewram.size(),
                     iwram.data(), iwram.size()};
    gba::Mp2kShadow shadow;
    shadow.init({gba::Mp2kSig{0, 0x08000101u}});

    constexpr uint32_t kRenders = 65536;
    uint64_t hash = 0x123456789abcdef0ull;
    const auto start = std::chrono::steady_clock::now();
    float route_a = 0.0f, route_b = 0.0f;
    for (uint32_t render = 0; render < kRenders; render += 8) {
        const uint64_t block_id = 100u + render / 8u;
        shadow.frame_hook(mem, render, 0x08000101u,
                          gba::Mp2kShadow::HookPhase::PreMix, block_id);
        shadow.producer_interleaved_block(mem, 0x02000000u, 8, block_id);
        for (uint32_t sample = 0; sample < 8; ++sample) {
            const bool ready = shadow.render(mem, render + sample,
                                             route_a, route_b);
            hash = mix_hash(hash, ready ? 1u : 0u);
            hash = mix_hash(hash, float_bits(route_a));
            hash = mix_hash(hash, float_bits(route_b));
        }
    }
    const auto finish = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        finish - start).count();
    hash = mix_hash(hash, shadow.producer_blocks_judged());
    hash = mix_hash(hash, shadow.producer_blocks_rejected());
    hash = mix_hash(hash, shadow.probation_failures());
    hash = mix_hash(hash, shadow.signed_gate_seen() ? 1u : 0u);
    hash = mix_hash(hash, shadow.signed_gate_passed() ? 1u : 0u);
    const auto& diff = shadow.producer_diff_trace();
    bool trace_active = false;
    for (const auto& voice : diff.voices)
        trace_active = trace_active || voice.active;
    return RunResult{hash, trace_active,
                     static_cast<uint64_t>(elapsed)};
}

RunResult run_pcm8_rom() {
    set_probe(false);
    std::vector<uint8_t> rom(0x2000, 0);
    const std::size_t wave = 0x100;
    // Looping PCM8 archive entry: 1024 samples, deterministic saw pattern.
    rom[wave + 2] = 0xC0;
    put32(rom, wave + 8, 0);
    put32(rom, wave + 12, 1024);
    for (std::size_t i = 0; i < 1024; ++i)
        rom[wave + 16 + i] = static_cast<uint8_t>(i * 37u + 11u);

    std::vector<uint8_t> ewram(0x400, 0);
    std::vector<uint8_t> iwram(0x8000, 0);
    const uint32_t sound_info = 0x03001000u;
    const std::size_t si = sound_info & 0x7FFFu;
    put32(iwram, 0x7FF0u, sound_info);
    put32(iwram, si, gba::kMp2kMagicBase);
    iwram[si + 0x06] = gba::kMp2kMaxChans;
    iwram[si + 0x0B] = 7;
    put32(iwram, si + 0x10, 224);
    put32(iwram, si + 0x14, 13379);
    for (int ch = 0; ch < gba::kMp2kMaxChans; ++ch) {
        const std::size_t base = si + gba::kSoundChansOff +
            static_cast<std::size_t>(ch) * gba::kSoundChanStride;
        iwram[base + 0x00] = 0x80;
        iwram[base + 0x01] = 0;
        iwram[base + 0x02] = static_cast<uint8_t>(96 + ch);
        iwram[base + 0x03] = static_cast<uint8_t>(80 + ch);
        iwram[base + 0x04] = 0xFF;
        iwram[base + 0x05] = 0xC0;
        iwram[base + 0x06] = 0x80;
        iwram[base + 0x07] = 0x40;
        put32(iwram, base + 0x18, 0);
        put32(iwram, base + 0x1C, 0);
        put32(iwram, base + 0x20, 13379 + static_cast<uint32_t>(ch * 311));
        put32(iwram, base + 0x24, 0x100);
    }

    gba::MemView mem{rom.data(), rom.size(), ewram.data(), ewram.size(),
                     iwram.data(), iwram.size()};
    gba::Mp2kShadow shadow;
    shadow.init({gba::Mp2kSig{0, 0x08000101u}});
    shadow.frame_hook(mem, 0, 0x08000101u,
                      gba::Mp2kShadow::HookPhase::PreMix, 1);

    constexpr uint32_t kRenders = 65536;
    uint64_t hash = 0x1f123bb5a7d44011ull;
    float route_a = 0.0f, route_b = 0.0f;
    const auto start = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < kRenders; ++i) {
        const bool ready = shadow.render(mem, i, route_a, route_b);
        hash = mix_hash(hash, ready ? 1u : 0u);
        hash = mix_hash(hash, float_bits(route_a));
        hash = mix_hash(hash, float_bits(route_b));
        hash = mix_hash(hash, gba::Mp2kShadowWallFixture::voice_hash(shadow));
    }
    const auto finish = std::chrono::steady_clock::now();
    hash = mix_hash(hash, shadow.hooks());
    hash = mix_hash(hash, shadow.bad_waves());
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        finish - start).count();
    return RunResult{hash, false, static_cast<uint64_t>(elapsed)};
}

bool ram_mutation_changes_output() {
    set_probe(false);
    std::vector<uint8_t> rom(0x100, 0);
    std::vector<uint8_t> ewram(0x400, 0);
    std::vector<uint8_t> iwram(0x8000, 0);
    const uint32_t sound_info = 0x03001000u;
    const std::size_t si = sound_info & 0x7FFFu;
    put32(iwram, 0x7FF0u, sound_info);
    put32(iwram, si, gba::kMp2kMagicBase);
    iwram[si + 0x06] = 1;
    iwram[si + 0x0B] = 7;
    put32(iwram, si + 0x10, 8);
    put32(iwram, si + 0x14, 65536);
    const std::size_t base = si + gba::kSoundChansOff;
    iwram[base + 0x00] = 0x80;
    iwram[base + 0x02] = iwram[base + 0x03] = 127;
    iwram[base + 0x04] = 0xFF;
    iwram[base + 0x05] = 0xC0;
    iwram[base + 0x06] = 0x80;
    iwram[base + 0x07] = 0x40;
    put32(iwram, base + 0x20, 65536);
    put32(iwram, base + 0x24, 0x03000200u);
    const std::size_t wave = 0x200;
    iwram[wave + 2] = 0xC0;
    put32(iwram, wave + 8, 0);
    put32(iwram, wave + 12, 4);
    iwram[wave + 16] = 10;
    iwram[wave + 17] = 20;
    iwram[wave + 18] = 30;
    iwram[wave + 19] = 40;
    gba::MemView mem{rom.data(), rom.size(), ewram.data(), ewram.size(),
                     iwram.data(), iwram.size()};
    gba::Mp2kShadow shadow;
    shadow.init({gba::Mp2kSig{0, 0x08000101u}});
    shadow.frame_hook(mem, 0, 0x08000101u,
                      gba::Mp2kShadow::HookPhase::PreMix, 1);
    float a = 0.0f, b = 0.0f;
    shadow.render(mem, 0, a, b);
    const int32_t before = gba::Mp2kShadowWallFixture::first_last_sample(shadow);
    iwram[wave + 18] = 99;
    shadow.render(mem, 1, a, b);
    const int32_t after = gba::Mp2kShadowWallFixture::first_last_sample(shadow);
    return before != after;
}

struct FilterResult {
    uint64_t slow_ns = 0;
    uint64_t fast_ns = 0;
    uint64_t accepted = 0;
};

FilterResult run_filter_benchmark() {
    std::vector<uint8_t> rom(0x100, 0), ewram(0x100, 0), iwram(0x8000, 0);
    const uint32_t sound_info = 0x03001000u;
    const std::size_t si = sound_info & 0x7FFFu;
    auto put32_local = [&](std::size_t off, uint32_t value) {
        iwram[off + 0] = static_cast<uint8_t>(value);
        iwram[off + 1] = static_cast<uint8_t>(value >> 8);
        iwram[off + 2] = static_cast<uint8_t>(value >> 16);
        iwram[off + 3] = static_cast<uint8_t>(value >> 24);
    };
    put32_local(gba::kSoundInfoPtr & 0x7FFFu, sound_info);
    put32_local(si, gba::kMp2kMagicBase);
    iwram[si + 0x06] = gba::kMp2kMaxChans;
    iwram[si + 0x0B] = 7;
    put32_local(si + 0x10, 8);
    put32_local(si + 0x14, 13379);
    gba::GbaAudio audio;
    audio.configure_shadow({gba::Mp2kSig{0, 0x08000101u}},
                            rom.data(), rom.size(), ewram.data(), ewram.size(),
                            iwram.data(), iwram.size(), false, false);
    gba::MemView mem{rom.data(), rom.size(), ewram.data(), ewram.size(),
                     iwram.data(), iwram.size()};
    constexpr uint32_t kStores = 1'000'000;
    volatile uint64_t sink = 0;
    auto old_slow_predicate = [&] {
        uint32_t current_si = 0;
        const uint8_t* head = nullptr;
        bool relevant = false;
        if (mem.u32(gba::kSoundInfoPtr, current_si) &&
            (head = mem.slice(current_si, 0x50))) {
            const uint32_t block =
                static_cast<uint32_t>(head[0x10]) |
                (static_cast<uint32_t>(head[0x11]) << 8) |
                (static_cast<uint32_t>(head[0x12]) << 16) |
                (static_cast<uint32_t>(head[0x13]) << 24);
            const uint32_t channels = current_si + gba::kSoundChansOff;
            const uint32_t channels_end = channels +
                gba::kMp2kMaxChans * gba::kSoundChanStride;
            const uint32_t ring = current_si + 0x350u;
            const uint32_t ring_end = ring + block * 9u + 32u;
            const uint32_t addr = 0x03005000u;
            relevant = (addr >= current_si && addr < current_si + 0x50u) ||
                (addr >= channels && addr < channels_end) ||
                (addr >= ring && addr < ring_end);
        }
        return relevant;
    };
    auto start = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < kStores; ++i)
        sink += old_slow_predicate() ? 1u : 0u;
    const uint64_t slow_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count());
    start = std::chrono::steady_clock::now();
    uint64_t accepted = 0;
    for (uint32_t i = 0; i < kStores; ++i)
        accepted += audio.mp2k_write_may_be_relevant(
            0x08000001u, 0x03005000u, 1) ? 1u : 0u;
    const uint64_t fast_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count());
    sink += accepted;
    CHECK(sink == accepted);
    return FilterResult{slow_ns, fast_ns, accepted};
}

struct HookFilterResult {
    uint64_t old_miss_ns = 0;
    uint64_t fast_miss_ns = 0;
    uint64_t old_hit_ns = 0;
    uint64_t fast_hit_ns = 0;
    uint64_t old_miss = 0;
    uint64_t fast_miss = 0;
    uint64_t old_hit = 0;
    uint64_t fast_hit = 0;
};

HookFilterResult run_hook_filter_benchmark() {
    set_shadow_request(true);
    std::vector<uint8_t> rom(0x100, 0), ewram(0x100, 0), iwram(0x8000, 0);
    const uint32_t sound_info = 0x03001000u;
    const std::size_t si = sound_info & 0x7FFFu;
    put32(iwram, gba::kSoundInfoPtr & 0x7FFFu, sound_info);
    put32(iwram, si, gba::kMp2kMagicBase);
    iwram[si + 0x06] = gba::kMp2kMaxChans;
    iwram[si + 0x0B] = 7;
    put32(iwram, si + 0x10, 8);
    put32(iwram, si + 0x14, 65536);
    const std::vector<gba::Mp2kSig> sigs{
        {0, 0x08000101u}, {0, 0x08000201u},
        {0, 0x08000301u}, {0, 0x08000401u},
    };
    gba::Mp2kShadow old_shadow;
    old_shadow.init(sigs);
    gba::GbaAudio audio;
    audio.configure_shadow(sigs, rom.data(), rom.size(), ewram.data(),
                           ewram.size(), iwram.data(), iwram.size(),
                           true, false);
    constexpr uint32_t kChecks = 10'000'000;
    constexpr uint32_t kMiss = 0x08005001u;
    constexpr uint32_t kHit = 0x08000101u;
    volatile uint64_t sink = 0;
    auto measure = [&](bool fast, uint32_t key, uint64_t& accepted) {
        accepted = 0;
        const auto start = std::chrono::steady_clock::now();
        for (uint32_t i = 0; i < kChecks; ++i) {
            const bool match = fast
                ? audio.mp2k_frame_hook_may_be_relevant(key)
                : old_shadow.matches_hook(key);
            accepted += match ? 1u : 0u;
        }
        sink += accepted;
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start).count());
    };
    HookFilterResult result{};
    result.old_miss_ns = measure(false, kMiss, result.old_miss);
    result.fast_miss_ns = measure(true, kMiss, result.fast_miss);
    result.old_hit_ns = measure(false, kHit, result.old_hit);
    result.fast_hit_ns = measure(true, kHit, result.fast_hit);
    CHECK(result.old_miss == result.fast_miss && result.old_miss == 0);
    CHECK(result.old_hit == result.fast_hit && result.old_hit == kChecks);
    CHECK(sink == kChecks * 2ull);
    set_shadow_request(false);
    return result;
}

}  // namespace

int main() {
    // Independent golden from the pre-optimization, behavior-preserving
    // baseline. Never replace this with a hash generated by the current run.
    constexpr uint64_t kPcm8BaselineHash = 0x7b6b161e3083ed13ull;
    const RunResult off = run(false);
    const RunResult on = run(true);
    const RunResult pcm = run_pcm8_rom();
    CHECK(ram_mutation_changes_output());
    const FilterResult filter = run_filter_benchmark();
    CHECK(filter.accepted == 0);
    set_probe(false);

    // Probe diagnostics must not alter audible output or verifier state.
    CHECK(off.hash == on.hash);
    CHECK(pcm.hash == kPcm8BaselineHash);
    // Probe-off must not retain the full per-sample voice trace.
    CHECK(!off.trace_active);
    const HookFilterResult hooks = run_hook_filter_benchmark();

    std::cout << "mp2k_shadow_benchmark renders=65536"
              << " probe_off_ns=" << off.elapsed_ns
              << " probe_on_ns=" << on.elapsed_ns
              << " pcm8_12v_ns=" << pcm.elapsed_ns
              << " pcm8_hash=0x" << std::hex << pcm.hash
              << " hash=0x" << off.hash << std::dec
              << " filter_slow_ns=" << filter.slow_ns
              << " filter_fast_ns=" << filter.fast_ns
              << " hook_old_miss_ns=" << hooks.old_miss_ns
              << " hook_fast_miss_ns=" << hooks.fast_miss_ns
              << " hook_old_hit_ns=" << hooks.old_hit_ns
              << " hook_fast_hit_ns=" << hooks.fast_hit_ns << "\n";
    return 0;
}
