// frame_interpolation.h — verified 2x presentation-state interpolation.
//
// Guest execution remains at the exact GBA cadence. This helper only keeps
// read-only presentation snapshots and builds a midpoint IO image for a second
// host present. The faithful framebuffer remains the oracle.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace gbarecomp {

class FrameInterpolation {
public:
    static constexpr std::size_t kScreenHeight = 160;
    static constexpr std::size_t kLineIoBytes = 0x58;

    struct Snapshot {
        std::uint16_t dispcnt = 0;
        std::vector<std::uint8_t> io;
        std::vector<std::uint8_t> vram;
        std::vector<std::uint8_t> oam;
        std::vector<std::uint8_t> pal;
        std::vector<std::uint8_t> line_io;
        std::vector<std::uint8_t> line_io_valid;
        std::vector<std::int32_t> affine_line_refs;
        std::vector<std::uint8_t> affine_line_ref_valid;
        bool endpoint_verified = false;
    };

    struct Midpoint {
        std::vector<std::uint8_t> io;
        std::vector<std::uint8_t> oam;
        std::vector<std::uint8_t> line_io;
        std::vector<std::int32_t> affine_line_refs;
    };

    void capture(std::uint16_t dispcnt,
                 const std::uint8_t* io, std::size_t io_size,
                 const std::uint8_t* vram, std::size_t vram_size,
                 const std::uint8_t* oam, std::size_t oam_size,
                 const std::uint8_t* pal, std::size_t pal_size,
                 const std::uint8_t* line_io = nullptr,
                 const bool* line_io_valid = nullptr,
                 const std::int32_t* affine_line_refs = nullptr,
                 const bool* affine_line_ref_valid = nullptr);
    void reset();

    bool has_current() const { return current_ != nullptr; }
    const Snapshot* current() const { return current_.get(); }
    void set_current_endpoint_verified(bool verified);

    // Builds alpha=0.5 state for BG scroll and affine registers. Everything
    // else remains at the current frame's value. False means the caller must
    // duplicate the canonical frame instead.
    bool build_midpoint_io(std::vector<std::uint8_t>& out,
                           const char** reason = nullptr) const;
    bool build_midpoint(Midpoint& out, const char** reason = nullptr) const;

private:
    std::unique_ptr<Snapshot> previous_;
    std::unique_ptr<Snapshot> current_;
};

}  // namespace gbarecomp
