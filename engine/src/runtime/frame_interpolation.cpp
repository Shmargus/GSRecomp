#include "frame_interpolation.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace gbarecomp {
namespace {

std::uint16_t read16(const std::vector<std::uint8_t>& v, std::size_t off) {
    return static_cast<std::uint16_t>(v[off]) |
           static_cast<std::uint16_t>(v[off + 1] << 8);
}

std::uint32_t read32(const std::vector<std::uint8_t>& v, std::size_t off) {
    return static_cast<std::uint32_t>(v[off]) |
           (static_cast<std::uint32_t>(v[off + 1]) << 8) |
           (static_cast<std::uint32_t>(v[off + 2]) << 16) |
           (static_cast<std::uint32_t>(v[off + 3]) << 24);
}

void write16(std::vector<std::uint8_t>& v, std::size_t off,
             std::uint16_t value) {
    v[off] = static_cast<std::uint8_t>(value);
    v[off + 1] = static_cast<std::uint8_t>(value >> 8);
}

void write32(std::vector<std::uint8_t>& v, std::size_t off,
             std::uint32_t value) {
    v[off] = static_cast<std::uint8_t>(value);
    v[off + 1] = static_cast<std::uint8_t>(value >> 8);
    v[off + 2] = static_cast<std::uint8_t>(value >> 16);
    v[off + 3] = static_cast<std::uint8_t>(value >> 24);
}

std::int32_t sign_extend_28(std::uint32_t value) {
    value &= 0x0FFFFFFFu;
    if (value & 0x08000000u) value |= 0xF0000000u;
    return static_cast<std::int32_t>(value);
}

std::uint16_t midpoint_scroll(std::uint16_t a, std::uint16_t b) {
    const int av = a & 0x1FF;
    const int bv = b & 0x1FF;
    int delta = (bv - av + 256) & 0x1FF;
    delta -= 256;
    return static_cast<std::uint16_t>((av + delta / 2) & 0x1FF);
}

int wrapped_delta(std::uint16_t a, std::uint16_t b, unsigned bits) {
    const int modulus = 1 << bits;
    const int mask = modulus - 1;
    int delta = (static_cast<int>(b & mask) - static_cast<int>(a & mask) +
                 modulus / 2) & mask;
    delta -= modulus / 2;
    return delta;
}

std::uint16_t midpoint_wrapped(std::uint16_t a, int delta, unsigned bits) {
    const int mask = (1 << bits) - 1;
    return static_cast<std::uint16_t>((static_cast<int>(a & mask) +
                                      delta / 2) & mask);
}

std::int32_t midpoint_signed(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(
        (static_cast<std::int64_t>(a) + static_cast<std::int64_t>(b)) / 2);
}

bool equal_range(const std::vector<std::uint8_t>& a,
                 const std::vector<std::uint8_t>& b) {
    return a.size() == b.size() && a == b;
}

}  // namespace

void FrameInterpolation::capture(std::uint16_t dispcnt,
                                 const std::uint8_t* io, std::size_t io_size,
                                 const std::uint8_t* vram, std::size_t vram_size,
                                 const std::uint8_t* oam, std::size_t oam_size,
                                 const std::uint8_t* pal, std::size_t pal_size,
                                 const std::uint8_t* line_io,
                                 const bool* line_io_valid,
                                 const std::int32_t* affine_line_refs,
                                 const bool* affine_line_ref_valid) {
    auto next = std::make_unique<Snapshot>();
    next->dispcnt = dispcnt;
    if (io && io_size) next->io.assign(io, io + io_size);
    if (vram && vram_size) next->vram.assign(vram, vram + vram_size);
    if (oam && oam_size) next->oam.assign(oam, oam + oam_size);
    if (pal && pal_size) next->pal.assign(pal, pal + pal_size);
    if (line_io && line_io_valid) {
        next->line_io.assign(line_io,
            line_io + kScreenHeight * kLineIoBytes);
        next->line_io_valid.assign(line_io_valid,
            line_io_valid + kScreenHeight);
    }
    if (affine_line_refs && affine_line_ref_valid) {
        next->affine_line_refs.assign(affine_line_refs,
            affine_line_refs + kScreenHeight * 4u);
        next->affine_line_ref_valid.assign(affine_line_ref_valid,
            affine_line_ref_valid + kScreenHeight);
    }
    previous_ = std::move(current_);
    current_ = std::move(next);
}

void FrameInterpolation::reset() {
    previous_.reset();
    current_.reset();
}

void FrameInterpolation::set_current_endpoint_verified(bool verified) {
    if (current_) current_->endpoint_verified = verified;
}

bool FrameInterpolation::build_midpoint_io(std::vector<std::uint8_t>& out,
                                           const char** reason) const {
    Midpoint midpoint;
    if (!build_midpoint(midpoint, reason)) return false;
    out = std::move(midpoint.io);
    return true;
}

bool FrameInterpolation::build_midpoint(Midpoint& out,
                                        const char** reason) const {
    auto fail = [&](const char* why) {
        if (reason) *reason = why;
        return false;
    };
    if (!previous_ || !current_) return fail("history not ready");
    if (!previous_->endpoint_verified || !current_->endpoint_verified)
        return fail("canonical endpoint not verified");
    if (previous_->io.size() < 0x40 || current_->io.size() < 0x40)
        return fail("incomplete IO snapshot");
    if (previous_->dispcnt != current_->dispcnt)
        return fail("DISPCNT changed");
    for (std::size_t off : {std::size_t{0x08}, std::size_t{0x0A},
                            std::size_t{0x0C}, std::size_t{0x0E}}) {
        if (read16(previous_->io, off) != read16(current_->io, off))
            return fail("BG control changed");
    }
    if (!equal_range(previous_->vram, current_->vram))
        return fail("VRAM changed");
    if (!equal_range(previous_->pal, current_->pal))
        return fail("palette changed");

    out.io = current_->io;
    for (std::size_t off = 0x10; off <= 0x1E; off += 2) {
        write16(out.io, off, midpoint_scroll(read16(previous_->io, off),
                                            read16(current_->io, off)));
    }
    for (std::size_t off : {std::size_t{0x20}, std::size_t{0x22},
                            std::size_t{0x24}, std::size_t{0x26},
                            std::size_t{0x30}, std::size_t{0x32},
                            std::size_t{0x34}, std::size_t{0x36}}) {
        const auto a = static_cast<std::int16_t>(read16(previous_->io, off));
        const auto b = static_cast<std::int16_t>(read16(current_->io, off));
        write16(out.io, off, static_cast<std::uint16_t>(
            static_cast<std::int16_t>(midpoint_signed(a, b))));
    }
    for (std::size_t off : {std::size_t{0x28}, std::size_t{0x2C},
                            std::size_t{0x38}, std::size_t{0x3C}}) {
        const std::int32_t a = sign_extend_28(read32(previous_->io, off));
        const std::int32_t b = sign_extend_28(read32(current_->io, off));
        write32(out.io, off, static_cast<std::uint32_t>(midpoint_signed(a, b)) &
                          0x0FFFFFFFu);
    }

    // Raster effects are captured at the point each scanline was rendered.
    // Interpolate motion registers line-by-line; all other state is held at
    // the newer endpoint. Missing line state is unsafe because final IO cannot
    // reconstruct HBlank DMA/IRQ changes.
    const std::size_t line_bytes = kScreenHeight * kLineIoBytes;
    if (previous_->line_io.size() != line_bytes ||
        current_->line_io.size() != line_bytes ||
        previous_->line_io_valid.size() != kScreenHeight ||
        current_->line_io_valid.size() != kScreenHeight)
        return fail("raster state unavailable");
    out.line_io = current_->line_io;
    for (std::size_t y = 0; y < kScreenHeight; ++y) {
        if (!previous_->line_io_valid[y] || !current_->line_io_valid[y])
            return fail("incomplete raster state");
        const std::size_t base = y * kLineIoBytes;
        const auto line16 = [&](const Snapshot& s, std::size_t off) {
            return read16(s.line_io, base + off);
        };
        if (line16(*previous_, 0) != line16(*current_, 0))
            return fail("per-line DISPCNT changed");
        for (std::size_t off : {std::size_t{0x08}, std::size_t{0x0A},
                                std::size_t{0x0C}, std::size_t{0x0E}}) {
            if (line16(*previous_, off) != line16(*current_, off))
                return fail("per-line BG control changed");
        }
        for (std::size_t off = 0x10; off <= 0x1E; off += 2) {
            write16(out.line_io, base + off,
                    midpoint_scroll(line16(*previous_, off),
                                    line16(*current_, off)));
        }
        for (std::size_t off : {std::size_t{0x20}, std::size_t{0x22},
                                std::size_t{0x24}, std::size_t{0x26},
                                std::size_t{0x30}, std::size_t{0x32},
                                std::size_t{0x34}, std::size_t{0x36}}) {
            const auto a = static_cast<std::int16_t>(line16(*previous_, off));
            const auto b = static_cast<std::int16_t>(line16(*current_, off));
            write16(out.line_io, base + off, static_cast<std::uint16_t>(
                static_cast<std::int16_t>(midpoint_signed(a, b))));
        }
    }

    const std::size_t ref_count = kScreenHeight * 4u;
    if (previous_->affine_line_refs.size() != ref_count ||
        current_->affine_line_refs.size() != ref_count ||
        previous_->affine_line_ref_valid.size() != kScreenHeight ||
        current_->affine_line_ref_valid.size() != kScreenHeight)
        return fail("affine raster state unavailable");
    out.affine_line_refs = current_->affine_line_refs;
    for (std::size_t y = 0; y < kScreenHeight; ++y) {
        if (!previous_->affine_line_ref_valid[y] ||
            !current_->affine_line_ref_valid[y])
            return fail("incomplete affine raster state");
        for (std::size_t i = 0; i < 4; ++i) {
            const std::size_t at = y * 4u + i;
            out.affine_line_refs[at] = midpoint_signed(
                previous_->affine_line_refs[at],
                current_->affine_line_refs[at]);
        }
    }

    // OAM slot is the hardware's stable sprite identity. Move only sprites
    // whose non-position attributes are unchanged; transitions stay at the
    // newer endpoint, avoiding tile/palette/shape ghosting.
    out.oam = current_->oam;
    if (previous_->oam.size() == current_->oam.size() &&
        out.oam.size() >= 0x400) {
        for (std::size_t off = 0; off < 0x400; off += 8) {
            const auto a0 = read16(previous_->oam, off);
            const auto b0 = read16(current_->oam, off);
            const auto a1 = read16(previous_->oam, off + 2);
            const auto b1 = read16(current_->oam, off + 2);
            const auto a2 = read16(previous_->oam, off + 4);
            const auto b2 = read16(current_->oam, off + 4);
            const int dy = wrapped_delta(a0, b0, 8);
            const int dx = wrapped_delta(a1, b1, 9);
            if ((a0 & 0xFF00u) != (b0 & 0xFF00u) ||
                (a1 & 0xFE00u) != (b1 & 0xFE00u) || a2 != b2 ||
                // Affine sprites can change through matrix parameters stored
                // in other OAM slots. OBJ-window/prohibited modes are also
                // unsafe to move independently of their masking effect.
                (b0 & 0x0100u) != 0 || (b0 & 0x0C00u) >= 0x0800u ||
                std::abs(dx) > 16 || std::abs(dy) > 16)
                continue;
            write16(out.oam, off, static_cast<std::uint16_t>(
                (b0 & 0xFF00u) | midpoint_wrapped(a0, dy, 8)));
            write16(out.oam, off + 2, static_cast<std::uint16_t>(
                (b1 & 0xFE00u) | midpoint_wrapped(a1, dx, 9)));
        }
    }
    if (reason) *reason = nullptr;
    return true;
}

}  // namespace gbarecomp
