#pragma once

#include <zlib.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace gsr_launcher {

// Small decoder for the launcher's non-interlaced, 8-bit RGB/RGBA assets.
// Validate chunk lengths, CRCs and allocation limits before trusting a file.
inline bool decode_png(const unsigned char* data, std::size_t size,
                       std::vector<unsigned char>* rgba, int* w, int* h) {
    constexpr std::size_t limit = 64 * 1024 * 1024;
    static const unsigned char signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (!data || !rgba || !w || !h || size < 8 || size > limit ||
        std::memcmp(data, signature, 8) != 0) return false;
    auto be32 = [](const unsigned char* p) {
        return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
               (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
    };
    std::size_t at = 8;
    std::uint32_t width = 0, height = 0;
    unsigned channels = 0;
    bool header = false, ended = false;
    std::vector<unsigned char> compressed;
    while (size - at >= 12) {
        const std::size_t len = be32(data + at);
        if (len > size - at - 12) return false;
        const unsigned char* type = data + at + 4;
        const unsigned char* body = data + at + 8;
        const auto crc = crc32(0, type, static_cast<uInt>(len + 4));
        if (crc != be32(body + len)) return false;
        if (!header && std::memcmp(type, "IHDR", 4) != 0) return false;
        if (std::memcmp(type, "IHDR", 4) == 0) {
            if (header || len != 13) return false;
            width = be32(body);
            height = be32(body + 4);
            if (!width || !height || width > 8192 || height > 8192 ||
                std::size_t(width) * height > limit / 4 || body[8] != 8 ||
                body[10] != 0 || body[11] != 0 || body[12] != 0) return false;
            channels = body[9] == 6 ? 4 : body[9] == 2 ? 3 : 0;
            if (!channels) return false;
            header = true;
        } else if (std::memcmp(type, "IDAT", 4) == 0) {
            if (len > limit - compressed.size()) return false;
            compressed.insert(compressed.end(), body, body + len);
        } else if (std::memcmp(type, "IEND", 4) == 0) {
            if (len != 0) return false;
            ended = true;
            break;
        } else if (std::memcmp(type, "tRNS", 4) == 0 ||
                   ((type[0] & 32) == 0 && std::memcmp(type, "PLTE", 4) != 0)) {
            return false;  // Unsupported transparency or critical format extension.
        }
        at += len + 12;
    }
    if (!header || !ended || compressed.empty()) return false;
    const std::size_t stride = std::size_t(width) * channels;
    std::vector<unsigned char> raw((stride + 1) * height);
    uLongf raw_size = static_cast<uLongf>(raw.size());
    if (uncompress(raw.data(), &raw_size, compressed.data(),
                   static_cast<uLong>(compressed.size())) != Z_OK || raw_size != raw.size())
        return false;
    std::vector<unsigned char> image(stride * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        const unsigned filter = raw[y * (stride + 1)];
        if (filter > 4) return false;
        const auto* source = raw.data() + y * (stride + 1) + 1;
        auto* row = image.data() + y * stride;
        const auto* previous = y ? image.data() + (y - 1) * stride : nullptr;
        for (std::size_t x = 0; x < stride; ++x) {
            const int a = x >= channels ? row[x - channels] : 0;
            const int b = previous ? previous[x] : 0;
            const int c = previous && x >= channels ? previous[x - channels] : 0;
            int value = source[x];
            if (filter == 1) value += a;
            if (filter == 2) value += b;
            if (filter == 3) value += (a + b) / 2;
            if (filter == 4) {
                const int p = a + b - c;
                const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                value += pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
            }
            row[x] = static_cast<unsigned char>(value);
        }
    }
    rgba->resize(std::size_t(width) * height * 4);
    for (std::size_t pixel = 0; pixel < std::size_t(width) * height; ++pixel) {
        for (unsigned k = 0; k < 3; ++k) (*rgba)[pixel * 4 + k] = image[pixel * channels + k];
        (*rgba)[pixel * 4 + 3] = channels == 4 ? image[pixel * 4 + 3] : 255;
    }
    *w = static_cast<int>(width);
    *h = static_cast<int>(height);
    return true;
}

}  // namespace gsr_launcher
