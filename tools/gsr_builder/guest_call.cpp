// guest_call.cpp -- see guest_call.h.
#include "guest_call.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "arm_decode.h"
#include "bus.h"
#include "cpu_state.h"
#include "interpreter.h"
#include "thumb_decode.h"

namespace gsr::builder {

namespace {
// Where a called routine returns to: never a real code address, so reaching
// it means the routine returned.
constexpr std::uint32_t kReturnSentinel = 0x0FFFFFF0u;
constexpr std::uint32_t kDma3 = 0x040000D4u;
}  // namespace

class FlatBus : public armv4t::Bus {
public:
    explicit FlatBus(GuestMachine& m) : m_(m) {}

    std::uint8_t* at(std::uint32_t addr, std::uint32_t n) {
        switch (addr >> 24) {
            case 0x02: {
                const std::uint32_t off = addr & 0x3FFFFu;
                return off + n <= m_.ewram_.size() ? &m_.ewram_[off] : nullptr;
            }
            case 0x03: {
                const std::uint32_t off = addr & 0x7FFFu;
                return off + n <= m_.iwram_.size() ? &m_.iwram_[off] : nullptr;
            }
            case 0x04: {
                const std::uint32_t off = addr & 0xFFFFFFu;
                return off + n <= m_.io_.size() ? &m_.io_[off] : nullptr;
            }
            default:
                return nullptr;
        }
    }
    const std::uint8_t* rom_at(std::uint32_t addr, std::uint32_t n) const {
        const std::uint32_t region = addr >> 24;
        if (region < 0x08 || region > 0x0D) return nullptr;
        const std::uint32_t off = addr & 0x01FFFFFFu;
        return off + n <= m_.rom_.size() ? &m_.rom_[off] : nullptr;
    }
    std::uint32_t read(std::uint32_t addr, std::uint32_t n) {
        const std::uint8_t* p = at(addr, n);
        if (!p) p = rom_at(addr, n);
        if (!p) return 0;  // unmapped reads: zero
        std::uint32_t v = 0;
        std::memcpy(&v, p, n);
        return v;
    }
    void write(std::uint32_t addr, std::uint32_t v, std::uint32_t n) {
        if (std::uint8_t* p = at(addr, n)) std::memcpy(p, &v, n);
        // ROM and unmapped writes are dropped.
        if (m_.instant_dma3_ && (addr >> 24) == 0x04u) {
            // The count/control word, or its control half, with the enable
            // bit set: copy now (emulate_stamp_builder.py's measured model).
            if ((addr == kDma3 + 8 && n == 4) || (addr == kDma3 + 10 && n == 2))
                run_dma3();
        }
    }
    void run_dma3() {
        std::uint32_t src = read(kDma3, 4), dst = read(kDma3 + 4, 4);
        std::uint32_t count = read(kDma3 + 8, 2);
        const std::uint32_t control = read(kDma3 + 10, 2);
        if (!(control & 0x8000u)) return;
        const std::uint32_t unit = (control & 0x0400u) ? 4u : 2u;
        if (count == 0) count = (dst >> 24) ? 0x10000u : 0x4000u;
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint32_t v = read(src + i * unit, unit);
            if (std::uint8_t* p = at(dst + i * unit, unit)) std::memcpy(p, &v, unit);
        }
    }

    std::uint8_t  read8 (std::uint32_t a) override { return static_cast<std::uint8_t>(read(a, 1)); }
    std::uint16_t read16(std::uint32_t a) override { return static_cast<std::uint16_t>(read(a & ~1u, 2)); }
    std::uint32_t read32(std::uint32_t a) override { return read(a & ~3u, 4); }
    void write8 (std::uint32_t a, std::uint8_t v) override { write(a, v, 1); }
    void write16(std::uint32_t a, std::uint16_t v) override { write(a & ~1u, v, 2); }
    void write32(std::uint32_t a, std::uint32_t v) override { write(a & ~3u, v, 4); }

private:
    GuestMachine& m_;
};

GuestMachine::GuestMachine(std::vector<std::uint8_t> rom)
    : rom_(std::move(rom)), ewram_(256 * 1024, 0), iwram_(32 * 1024, 0),
      io_(0x400, 0) {}

void GuestMachine::clear_memory() {
    std::fill(ewram_.begin(), ewram_.end(), 0);
    std::fill(iwram_.begin(), iwram_.end(), 0);
    std::fill(io_.begin(), io_.end(), 0);
}

void GuestMachine::stub_returning(std::uint32_t pc, std::uint32_t r0) {
    stubs_.push_back({pc & ~1u, r0});
}

void GuestMachine::poke32(std::uint32_t addr, std::uint32_t value) {
    FlatBus bus(*this);
    bus.write32(addr, value);
}

bool GuestMachine::call(std::uint32_t entry, const std::uint32_t (&args)[4],
                        std::uint32_t* r0_out, std::string* error,
                        std::uint32_t sp, std::uint64_t max_steps) {
    FlatBus bus(*this);
    armv4t::CPUState cpu{};
    for (int i = 0; i < 4; ++i) cpu.R[i] = args[i];
    cpu.R[13] = sp;
    cpu.R[14] = kReturnSentinel | (entry & 1u);
    cpu.R[15] = entry & ~1u;
    cpu.thumb = (entry & 1u) != 0;
    cpu.cpsr.t = cpu.thumb;
    cpu.cpsr.i = true;
    cpu.cpsr.f = true;
    cpu.cpsr.mode = static_cast<std::uint8_t>(armv4t::Mode::System);

    for (std::uint64_t n = 0; n < max_steps; ++n) {
        const std::uint32_t pc = cpu.R[15];
        if ((pc & ~1u) == kReturnSentinel) {
            if (r0_out) *r0_out = cpu.R[0];
            return true;
        }
        bool stubbed = false;
        for (const Stub& s : stubs_) {
            if (pc == s.pc) {
                cpu.R[0] = s.r0;
                // bx lr: the caller is Thumb when lr has bit 0 set.
                const std::uint32_t lr = cpu.R[14];
                cpu.thumb = (lr & 1u) != 0;
                cpu.cpsr.t = cpu.thumb;
                cpu.R[15] = lr & ~1u;
                stubbed = true;
                break;
            }
        }
        if (stubbed) continue;
        armv4t::Instr insn{};
        if (cpu.thumb) {
            insn = armv4t::ThumbDecoder::decode(bus.read16(pc), pc);
        } else {
            insn = armv4t::ArmDecoder::decode(bus.read32(pc), pc);
        }
        const auto r = armv4t::Interpreter::step(cpu, bus, insn);
        using R = armv4t::Interpreter::Result;
        if (r == R::Swi || r == R::Undefined || r == R::NotImplemented) {
            if (error) {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "stopped at %08X (%s)", pc,
                              r == R::Swi ? "SWI"
                              : r == R::Undefined ? "undefined instruction"
                                                  : "unsupported instruction");
                *error = buf;
            }
            return false;
        }
    }
    if (error) *error = "did not return in time";
    return false;
}

}  // namespace gsr::builder
