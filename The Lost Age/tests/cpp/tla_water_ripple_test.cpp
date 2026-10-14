// Independent oracle: run the captured game's wave generator on the
// interpreter, extending its existing loop into the measured 240-row view.
// Usage: tla_water_ripple_test <ROM> <F12 folder> [more F12 folders]
// No ROM, save or captured-memory data is part of this test source.
#include "tla_water_ripple.h"
#include "bus.h"
#include "cpu_state.h"
#include "thumb_decode.h"
#include "arm_decode.h"
#include "interpreter.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static std::vector<std::uint8_t> load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    assert(f);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

struct BenchBus : armv4t::Bus {
    std::vector<std::uint8_t> memory, rom;
    std::uint8_t& byte(std::uint32_t a) {
        if (a >= 0x02000000 && a < 0x02040000) return memory[a - 0x02000000];
        if (a >= 0x03000000 && a < 0x03008000) return memory[0x40000 + a - 0x03000000];
        if (a >= 0x04000000 && a < 0x04000400) return memory[0x48000 + a - 0x04000000];
        assert(a >= 0x08000000 && a - 0x08000000 < rom.size());
        return rom[a - 0x08000000];
    }
    std::uint8_t read8(std::uint32_t a) override { return byte(a); }
    std::uint16_t read16(std::uint32_t a) override { return read8(a) | (read8(a + 1) << 8); }
    std::uint32_t read32(std::uint32_t a) override { return read16(a) | (std::uint32_t(read16(a + 2)) << 16); }
    void write8(std::uint32_t a, std::uint8_t v) override { byte(a) = v; }
    void write16(std::uint32_t a, std::uint16_t v) override { write8(a, v); write8(a + 1, v >> 8); }
    void write32(std::uint32_t a, std::uint32_t v) override { write16(a, v); write16(a + 2, v >> 16); }
};

int main(int argc, char** argv) {
    assert(argc >= 3);
    for (int file = 2; file < argc; ++file) {
        BenchBus bus;
        bus.rom = load(argv[1]);
        bus.memory = load(std::string(argv[file]) + "/memory.bin");
        const auto gpu = load(std::string(argv[file]) + "/gpu_frame.bin");
        gsr::FieldScene scene{};
        gsr::SceneMargins margins;
        scene.video_mode = 0;
        scene.layers[2].enabled = true;
        const std::size_t row_at = 16 + 0xC00 + 0x18000;
        assert(gpu.size() >= row_at + 160 * gsr::FieldScene::kLineIoBytes);
        for (int row = 0; row < 160; ++row) {
            std::copy_n(gpu.data() + row_at + row * gsr::FieldScene::kLineIoBytes,
                        gsr::FieldScene::kLineIoBytes, scene.row_io[row].data());
            scene.row_io_valid[row] = true;
        }
        const unsigned phase = bus.read8(0x0300122C);
        auto sine = [&](unsigned i) { return static_cast<std::int16_t>(bus.read16(0x080020A8 + i * 2)); };
        assert(tla_water_ripple(scene, margins, phase, sine, -40, 240));
        const auto native = scene.row_io;
        // The captured rows precede the next VBlank phase and camera update.
        bus.write8(0x0300122C, (phase - 1) & 255);
        const auto field = bus.read32(0x03000020);
        const unsigned y = native[0][0x1A] | (native[0][0x1B] << 8);
        bus.write16(field + 0x146, y - 40);
        // Both CMP r6,#160 instructions in the captured loop now use 240.
        assert(bus.read16(0x02008308) == 0x2EA0);
        assert(bus.read16(0x0200833C) == 0x2EA0);
        bus.write16(0x02008308, 0x2EF0);
        bus.write16(0x0200833C, 0x2EF0);
        armv4t::CPUState cpu{};
        cpu.cpsr.mode = unsigned(armv4t::Mode::System);
        cpu.cpsr.t = true;
        cpu.thumb = true;
        cpu.R[13] = 0x03007000;
        cpu.R[15] = 0x020082A4;
        int steps = 0;
        // Whole function's halfword count per iteration is a conservative
        // structural bound, including its two short sine/veneer calls.
        const int bound = (0x02008374 - 0x020082A4) / 2 * (240 + 1);
        while (cpu.R[15] != 0x02008340 && steps++ < bound) {
            const auto pc = cpu.R[15];
            const auto insn = cpu.thumb ? armv4t::ThumbDecoder::decode(bus.read16(pc), pc)
                                       : armv4t::ArmDecoder::decode(bus.read32(pc), pc);
            const auto result = armv4t::Interpreter::step(cpu, bus, insn);
            assert(result == armv4t::Interpreter::Result::Normal ||
                   result == armv4t::Interpreter::Result::Branched);
        }
        assert(cpu.R[15] == 0x02008340);
        for (int row = 0; row < 240; ++row)
            assert(margins.layers[2].scroll[row][0] == bus.read16(0x0200E340 + row * 2));
        assert(scene.row_io == native);
        scene.row_io[80][0x18] ^= 8;
        assert(!tla_water_ripple(scene, margins, phase, sine, -40, 240));
        assert(margins.layers[2].scroll.empty());
        scene.row_io = native;
        scene.row_io_valid[80] = false;
        assert(!tla_water_ripple(scene, margins, phase, sine, -40, 240));
        std::printf("%s: all 240 extended rows equal the game (%d steps); invalid rows rejected\n", argv[file], steps);
    }
}
