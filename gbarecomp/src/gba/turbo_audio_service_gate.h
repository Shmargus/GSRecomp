// Once-per-guest-boundary guard for emulation-thread Turbo audio service.
#pragma once

#include <cstdint>

namespace gba {

class TurboAudioServiceGate {
public:
    void mark_pending() { pending_ = true; }
    void clear_pending() { pending_ = false; }
    bool pending() const { return pending_; }

    // Claim one service slot. A guest PPU frame may have several host seams;
    // the boundary generation distinguishes later guest progress in the same
    // PPU frame, while suppressing present/deferred/outer duplicates.
    bool claim(uint64_t guest_frame, uint64_t boundary_generation) {
        if (guest_frame == last_frame_ &&
            boundary_generation == last_generation_)
            return false;
        last_frame_ = guest_frame;
        last_generation_ = boundary_generation;
        return true;
    }

    // Consume an IRQ-marked request at the first safe seam. It still obeys
    // the once-per-frame/generation rule.
    bool take_pending(uint64_t guest_frame, uint64_t boundary_generation) {
        if (!pending_) return false;
        pending_ = false;
        return claim(guest_frame, boundary_generation);
    }

    void reset() {
        pending_ = false;
        last_frame_ = UINT64_MAX;
        last_generation_ = UINT64_MAX;
    }

private:
    bool pending_ = false;
    uint64_t last_frame_ = UINT64_MAX;
    uint64_t last_generation_ = UINT64_MAX;
};

}  // namespace gba
