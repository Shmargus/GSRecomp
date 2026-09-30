// guest_call.h -- run one routine from the player's ROM on the gbarecomp
// ARM interpreter, against a plain GBA memory map (ROM, EWRAM, IWRAM, IO).
//
// The player builder uses this where it needs exactly what the game's own
// code produces from the ROM: unpacking the compressed overlays with the
// game's decompressor, and generating the particle stamp routines with the
// game's own builder. The routine comes from the player's ROM at build time;
// none of it is part of this program.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace gsr::builder {

class GuestMachine {
public:
    explicit GuestMachine(std::vector<std::uint8_t> rom);

    std::vector<std::uint8_t>& ewram() { return ewram_; }
    std::vector<std::uint8_t>& iwram() { return iwram_; }
    const std::vector<std::uint8_t>& rom() const { return rom_; }

    // Zero EWRAM, IWRAM and IO.
    void clear_memory();

    // Stand-in for a routine the called code uses: when execution reaches
    // `pc`, r0 is set to `r0` and the routine returns to its caller (lr)
    // instead of running.
    void stub_returning(std::uint32_t pc, std::uint32_t r0);

    // DMA3 copies at once when its enable bit is written (the stamp
    // builder's DMA-copied template runs). Off by default.
    void set_instant_dma3(bool on) { instant_dma3_ = on; }

    // Write one word into guest memory (EWRAM/IWRAM), e.g. a stack argument.
    void poke32(std::uint32_t addr, std::uint32_t value);

    // Call the routine at `entry` (bit 0 set = Thumb) with r0..r3 = args and
    // sp = `sp`. Returns true when it returns to the caller within
    // `max_steps` instructions; `*r0_out` receives r0. `error` says why
    // otherwise.
    bool call(std::uint32_t entry, const std::uint32_t (&args)[4],
              std::uint32_t* r0_out, std::string* error,
              std::uint32_t sp = 0x03007F00u,
              std::uint64_t max_steps = 200000000ull);

private:
    struct Stub {
        std::uint32_t pc;
        std::uint32_t r0;
    };
    std::vector<std::uint8_t> rom_;
    std::vector<std::uint8_t> ewram_;
    std::vector<std::uint8_t> iwram_;
    std::vector<std::uint8_t> io_;
    std::vector<Stub> stubs_;
    bool instant_dma3_ = false;
    friend class FlatBus;
};

}  // namespace gsr::builder
