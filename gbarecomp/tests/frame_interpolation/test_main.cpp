#include "frame_interpolation.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

int failures = 0;

void expect(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

void put16(std::vector<std::uint8_t>& v, std::size_t off, std::uint16_t x) {
    v[off] = static_cast<std::uint8_t>(x);
    v[off + 1] = static_cast<std::uint8_t>(x >> 8);
}

std::uint16_t get16(const std::vector<std::uint8_t>& v, std::size_t off) {
    return static_cast<std::uint16_t>(v[off]) |
           static_cast<std::uint16_t>(v[off + 1] << 8);
}

void put32(std::vector<std::uint8_t>& v, std::size_t off, std::uint32_t x) {
    v[off] = static_cast<std::uint8_t>(x);
    v[off + 1] = static_cast<std::uint8_t>(x >> 8);
    v[off + 2] = static_cast<std::uint8_t>(x >> 16);
    v[off + 3] = static_cast<std::uint8_t>(x >> 24);
}

struct Fixture {
    std::vector<std::uint8_t> io = std::vector<std::uint8_t>(0x400, 0);
    std::vector<std::uint8_t> vram = std::vector<std::uint8_t>(0x18000, 0x5A);
    std::vector<std::uint8_t> oam = std::vector<std::uint8_t>(0x400, 0);
    std::vector<std::uint8_t> pal = std::vector<std::uint8_t>(0x400, 0x33);
    std::vector<std::uint8_t> line_io = std::vector<std::uint8_t>(
        gbarecomp::FrameInterpolation::kScreenHeight *
        gbarecomp::FrameInterpolation::kLineIoBytes, 0);
    std::array<bool, gbarecomp::FrameInterpolation::kScreenHeight>
        line_valid{};
    std::vector<std::int32_t> affine_refs = std::vector<std::int32_t>(
        gbarecomp::FrameInterpolation::kScreenHeight * 4u, 0);
    std::array<bool, gbarecomp::FrameInterpolation::kScreenHeight>
        affine_valid{};

    void capture(gbarecomp::FrameInterpolation& f, std::uint16_t dispcnt = 2) {
        for (std::size_t y = 0; y < line_valid.size(); ++y) {
            std::copy_n(io.data(),
                        gbarecomp::FrameInterpolation::kLineIoBytes,
                        line_io.data() + y *
                            gbarecomp::FrameInterpolation::kLineIoBytes);
            put16(line_io, y * gbarecomp::FrameInterpolation::kLineIoBytes,
                  dispcnt);
        }
        line_valid.fill(true);
        affine_valid.fill(true);
        f.capture(dispcnt, io.data(), io.size(), vram.data(), vram.size(),
                  oam.data(), oam.size(), pal.data(), pal.size(),
                  line_io.data(), line_valid.data(), affine_refs.data(),
                  affine_valid.data());
        f.set_current_endpoint_verified(true);
    }
};

void test_midpoint_math() {
    gbarecomp::FrameInterpolation f;
    Fixture x;
    put16(x.io, 0x10, 511);
    put16(x.io, 0x20, static_cast<std::uint16_t>(-256));
    put32(x.io, 0x28, 0x0FFFFF00u);  // -256 in signed 28-bit form
    x.affine_refs[0] = -256;
    x.io[0x60] = 0xA5;
    x.capture(f);

    put16(x.io, 0x10, 1);
    put16(x.io, 0x20, 256);
    put32(x.io, 0x28, 0x00000100u);
    x.affine_refs[0] = 256;
    x.oam[0] = 7;  // sprites are deliberately held at current, not interpolated
    x.capture(f);

    std::vector<std::uint8_t> out;
    const char* reason = nullptr;
    expect(f.build_midpoint_io(out, &reason), "valid pair interpolates");
    expect((get16(out, 0x10) & 0x1FFu) == 0, "scroll wraps by shortest path");
    expect(static_cast<std::int16_t>(get16(out, 0x20)) == 0,
           "signed affine matrix midpoint");
    expect((get16(out, 0x28) | (static_cast<std::uint32_t>(get16(out, 0x2A)) << 16)) == 0,
           "signed affine reference midpoint");
    expect(out[0x60] == 0xA5, "unrelated IO remains current");
    gbarecomp::FrameInterpolation::Midpoint raster;
    expect(f.build_midpoint(raster), "raster midpoint builds");
    expect((get16(raster.line_io, 0x10) & 0x1FFu) == 0,
           "per-line scroll uses midpoint");
    expect(raster.affine_line_refs[0] == 0,
           "hidden affine reference uses midpoint");
}

void test_fallbacks_and_reset() {
    gbarecomp::FrameInterpolation f;
    Fixture x;
    std::vector<std::uint8_t> out;
    const char* reason = nullptr;

    x.capture(f);
    expect(!f.build_midpoint_io(out, &reason), "one frame has no history");
    x.capture(f, 3);
    expect(!f.build_midpoint_io(out, &reason), "DISPCNT change falls back");

    f.reset();
    x = Fixture{};
    x.capture(f);
    x.vram[9] ^= 1;
    x.capture(f);
    expect(!f.build_midpoint_io(out, &reason), "VRAM change falls back");

    f.reset();
    expect(!f.has_current(), "reset clears interpolation history");
}

void test_unverified_endpoint_falls_back() {
    gbarecomp::FrameInterpolation f;
    Fixture x;
    x.capture(f);
    x.capture(f);
    f.set_current_endpoint_verified(false);
    std::vector<std::uint8_t> out;
    expect(!f.build_midpoint_io(out), "unverified canonical endpoint falls back");
}

void test_stable_sprite_identity() {
    gbarecomp::FrameInterpolation f;
    Fixture x;
    // Same tile/palette/shape in the same OAM slot, moving through the wrapped
    // coordinate boundary: x=510 -> 2 and y=254 -> 2 midpoint at zero.
    put16(x.oam, 0, 254);
    put16(x.oam, 2, 510);
    put16(x.oam, 4, 7);
    x.capture(f);
    put16(x.oam, 0, 2);
    put16(x.oam, 2, 2);
    x.capture(f);
    gbarecomp::FrameInterpolation::Midpoint midpoint;
    expect(f.build_midpoint(midpoint), "stable sprite interpolates");
    expect((get16(midpoint.oam, 0) & 0xFFu) == 0,
           "sprite y wraps by shortest path");
    expect((get16(midpoint.oam, 2) & 0x1FFu) == 0,
           "sprite x wraps by shortest path");

    // A tile change means new identity/content. Keep the newer position.
    f.reset();
    put16(x.oam, 0, 10);
    put16(x.oam, 2, 10);
    put16(x.oam, 4, 7);
    x.capture(f);
    put16(x.oam, 0, 30);
    put16(x.oam, 2, 30);
    put16(x.oam, 4, 8);
    x.capture(f);
    expect(f.build_midpoint(midpoint), "sprite transition does not reject frame");
    expect((get16(midpoint.oam, 0) & 0xFFu) == 30,
           "changed sprite stays at current y");
    expect((get16(midpoint.oam, 2) & 0x1FFu) == 30,
           "changed sprite stays at current x");

    // A same-looking sprite can still be a cut or slot reuse. Large jumps
    // remain at the newer endpoint instead of gliding across the screen.
    f.reset();
    put16(x.oam, 0, 10);
    put16(x.oam, 2, 10);
    put16(x.oam, 4, 7);
    x.capture(f);
    put16(x.oam, 0, 80);
    put16(x.oam, 2, 100);
    x.capture(f);
    expect(f.build_midpoint(midpoint), "large sprite jump does not reject frame");
    expect((get16(midpoint.oam, 0) & 0xFFu) == 80,
           "large sprite y jump stays current");
    expect((get16(midpoint.oam, 2) & 0x1FFu) == 100,
           "large sprite x jump stays current");
}

}  // namespace

int main() {
    test_midpoint_math();
    test_fallbacks_and_reset();
    test_unverified_endpoint_falls_back();
    test_stable_sprite_identity();
    if (failures) return 1;
    std::puts("frame interpolation tests passed");
    return 0;
}
