// Golden Sun: The Lost Age -- running a piece of the game's own code again
// on the host, without touching the game.
//
// The spark capture (src/tla_spell_sparks.cpp) lets the game's routine draw
// what it clipped away: it copies the CPU, steps the interpreter through the
// routine on this bus, and reads back what it wrote.
// Reads come from EWRAM, IWRAM and ROM. IWRAM is a private copy, taken on
// the first write to it (the routines patch their own code and use the
// stack); other writes land in an overlay the later reads see. A DMA3 the
// routine starts is done here, on this bus. Anything else ends the run: the
// game's state and decisions never change.
#pragma once

#include <bitset>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "arm_cpu_bridge.h"
#include "arm_decode.h"
#include "bus.h"
#include "gba_bus.h"
#include "interpreter.h"
#include "runtime_arm.h"
#include "thumb_decode.h"

struct TlaDryRunBus final : armv4t::Bus {
    static constexpr std::uint32_t kIwram = 0x03000000u, kIwramBytes = 0x8000u;
    static constexpr std::uint32_t kEwram = 0x02000000u, kEwramBytes = 0x40000u;
    static constexpr std::uint32_t kPageShift = 8;

    explicit TlaDryRunBus(gba::GbaBus* bus)
        : ewram(bus->ewram_ptr()), iwram(bus->iwram_ptr()),
          rom(bus->rom_ptr()), rom_bytes(bus->rom_size()) {}

    const std::uint8_t* ewram;
    const std::uint8_t* iwram;
    const std::uint8_t* rom;
    std::size_t rom_bytes;
    std::vector<std::uint8_t> iwram_copy;  // empty until IWRAM is written
    std::unordered_map<std::uint32_t, std::uint8_t> written;
    std::bitset<(kEwramBytes >> kPageShift)> ewram_dirty;
    bool failed = false;
    bool rom_written = false;  // a write over ROM bytes (a list the caller replaced)

    // The run's own IWRAM, copied on first use: a caller may also prepare
    // bytes in it before the run (a scratch canvas).
    std::uint8_t* own_iwram(std::uint32_t a) {
        if (iwram_copy.empty()) iwram_copy.assign(iwram, iwram + kIwramBytes);
        return iwram_copy.data() + (a - kIwram);
    }

    std::uint8_t read8(std::uint32_t a) override {
        if (a - kIwram < kIwramBytes)
            return iwram_copy.empty() ? iwram[a - kIwram] : iwram_copy[a - kIwram];
        if (a - kEwram < kEwramBytes) {
            if (ewram_dirty[(a - kEwram) >> kPageShift])
                if (const auto it = written.find(a); it != written.end()) return it->second;
            return ewram[a - kEwram];
        }
        if (const auto it = written.find(a); it != written.end()) return it->second;
        if (a >= 0x08000000u && a - 0x08000000u < rom_bytes)
            return rom[a - 0x08000000u];
        failed = true;
        return 0;
    }
    std::uint16_t read16(std::uint32_t a) override {
        a &= ~1u;
        if (a - kIwram < kIwramBytes ||
            (!rom_written && a >= 0x08000000u && a - 0x08000000u + 1u < rom_bytes)) {
            const std::uint8_t* p = a - kIwram < kIwramBytes
                ? (iwram_copy.empty() ? iwram : iwram_copy.data()) + (a - kIwram)
                : rom + (a - 0x08000000u);
            return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
        }
        return static_cast<std::uint16_t>(read8(a) | (read8(a + 1) << 8));
    }
    std::uint32_t read32(std::uint32_t a) override {
        a &= ~3u;
        if (a - kIwram < kIwramBytes ||
            (!rom_written && a >= 0x08000000u && a - 0x08000000u + 3u < rom_bytes)) {
            const std::uint8_t* p = a - kIwram < kIwramBytes
                ? (iwram_copy.empty() ? iwram : iwram_copy.data()) + (a - kIwram)
                : rom + (a - 0x08000000u);
            std::uint32_t v;
            std::memcpy(&v, p, 4);
            return v;
        }
        return std::uint32_t(read16(a)) | (std::uint32_t(read16(a + 2)) << 16);
    }
    void write8(std::uint32_t a, std::uint8_t v) override {
        if (a - kIwram < kIwramBytes) { *own_iwram(a) = v; return; }
        if (a - kEwram < kEwramBytes) ewram_dirty[(a - kEwram) >> kPageShift] = true;
        else if (a >= 0x08000000u) rom_written = true;
        written[a] = v;
    }
    void write16(std::uint32_t a, std::uint16_t v) override {
        a &= ~1u;
        write8(a, static_cast<std::uint8_t>(v));
        write8(a + 1, static_cast<std::uint8_t>(v >> 8));
    }
    void write32(std::uint32_t a, std::uint32_t v) override {
        a &= ~3u;
        write16(a, static_cast<std::uint16_t>(v));
        write16(a + 2, static_cast<std::uint16_t>(v >> 16));
        // DMA3 started by its count/control word: an immediate copy (the
        // rasterizer restores its self-patched loop from a template).
        if (a == 0x040000DCu && (v & 0x80000000u) != 0u) {
            const std::uint32_t src = read32(0x040000D4u);
            const std::uint32_t dst = read32(0x040000D8u);
            const bool words = (v & 0x04000000u) != 0u;
            const std::uint32_t count = (v & 0xFFFFu) ? (v & 0xFFFFu) : 0x10000u;
            for (std::uint32_t i = 0; i < count && !failed; ++i) {
                if (words) write32(dst + i * 4u, read32(src + i * 4u));
                else write16(dst + i * 2u, read16(src + i * 2u));
            }
        }
    }
};

// Steps `cpu` on `dry` until `stop(cpu)` returns true (stopped), the run
// fails, or `max_steps` pass. True when stopped. `stop` sees each pc before
// it runs and may move it on (a test treated as passed); it is asked again
// at the new pc.
template <typename Stop>
bool tla_dry_run(armv4t::CPUState& cpu, TlaDryRunBus& dry, int max_steps, Stop stop) {
    // Decoded instructions by address, kept while the bytes there match
    // (the rasterizer patches its own code before its loops run).
    struct Decoded { std::uint32_t bits; bool thumb; armv4t::Instr insn; };
    std::unordered_map<std::uint32_t, Decoded> decoded;
    for (int step = 0; step < max_steps; ++step) {
        std::uint32_t at;
        do {
            at = cpu.R[15];
            if (stop(cpu)) return true;
        } while (cpu.R[15] != at);
        const std::uint32_t bits = cpu.thumb ? dry.read16(at) : dry.read32(at);
        if (dry.failed) return false;
        auto it = decoded.find(at);
        if (it == decoded.end() || it->second.bits != bits ||
            it->second.thumb != cpu.thumb) {
            const armv4t::Instr fresh =
                cpu.thumb ? armv4t::ThumbDecoder::decode(static_cast<std::uint16_t>(bits), at)
                          : armv4t::ArmDecoder::decode(bits, at);
            it = decoded.insert_or_assign(at, Decoded{bits, cpu.thumb, fresh}).first;
        }
        const armv4t::Instr& insn = it->second.insn;
        const auto r = armv4t::Interpreter::step(cpu, dry, insn);
        if (dry.failed || (r != armv4t::Interpreter::Result::Normal &&
                           r != armv4t::Interpreter::Result::Branched))
            return false;
    }
    return false;
}
