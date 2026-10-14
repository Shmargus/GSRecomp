// ram_overlay_registry.h -- identity-aware dispatch for statically translated
// code images that occupy the same guest RAM addresses at different times.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace gbarecomp {

using RamOverlayFunction = void (*)(void);

struct RamOverlayDispatchEntry {
    uint32_t pc;
    uint8_t thumb;
    RamOverlayFunction fn;
};

struct RamOverlayImageView {
    std::string_view identity;
    uint32_t runtime_start;
    uint32_t runtime_end;
    const RamOverlayDispatchEntry* entries;
    std::size_t entry_count;
};

// Game-thread-only registry used behind RuntimeRamDispatchHook. Registered
// images are immutable. Activating an image atomically replaces every active
// image whose declared runtime range overlaps it, so a shared (PC, mode) can
// never resolve through stale identity state.
class RamOverlayRegistry {
public:
    bool register_image(const RamOverlayImageView& image,
                        std::string* error = nullptr);
    bool activate(std::string_view identity, std::string* error = nullptr);

    // Invalidate active images touched by the half-open overwrite range.
    // Registered definitions remain available for a later verified reload.
    std::size_t invalidate_range(uint32_t runtime_start,
                                 uint32_t runtime_end);

    // Resolves the active image's native function for exactly (pc, mode), or
    // nullptr on unknown identity/entry. The runtime hook invokes the result
    // at its own tail-transfer site.
    RamOverlayFunction resolve(uint32_t pc, bool thumb) const;

    // Convenience invoke-and-status wrapper for non-tail callers.
    bool dispatch(uint32_t pc, bool thumb) const;

    bool is_active(std::string_view identity) const;
    std::string active_identity(uint32_t pc) const;
    std::size_t active_count() const { return active_identities_.size(); }
    void clear_active() { active_identities_.clear(); }

private:
    struct RegisteredImage {
        std::string identity;
        uint32_t runtime_start;
        uint32_t runtime_end;
        std::vector<RamOverlayDispatchEntry> entries;
    };

    const RegisteredImage* find_image(std::string_view identity) const;

    std::vector<RegisteredImage> images_;
    std::vector<std::string> active_identities_;
};

}  // namespace gbarecomp
