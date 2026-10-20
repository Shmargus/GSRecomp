#pragma once

// F12 renderer snapshots. TLAGPUF2 has a 32-byte little-endian header and
// 24-byte section entries (name[8], offset u64, length u64). No host pointers
// or C++ object layouts are stored. TLAGPUF1 remains readable for old reports.
#include <cstring>
#include <algorithm>
#include <initializer_list>
#include <string>
#include <vector>
#include <utility>
#include "effect_particles.h"

namespace tla_frame {
inline void u32(std::string& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(char(v >> (i * 8)));
}
inline void u64(std::string& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(char(v >> (i * 8)));
}
inline std::uint32_t read32(const std::uint8_t* p) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= std::uint32_t(p[i]) << (i * 8);
    return v;
}
inline std::uint64_t read64(const std::uint8_t* p) {
    return read32(p) | (std::uint64_t(read32(p + 4)) << 32);
}
struct Section {
    std::string name;
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
};
inline std::string encode(std::uint64_t frame, std::uint32_t width,
                          std::uint32_t height,
                          std::initializer_list<Section> parts) {
    std::string out("TLAGPUF2", 8);
    u32(out, static_cast<std::uint32_t>(parts.size()));
    u32(out, 0);
    u64(out, frame);
    u32(out, width);
    u32(out, height);
    std::uint64_t offset = 32 + parts.size() * 24;
    for (const auto& part : parts) {
        out.append(part.name.substr(0, 8));
        out.append(8 - std::min(part.name.size(), std::size_t(8)), '\0');
        u64(out, offset);
        u64(out, part.size);
        offset += part.size;
    }
    out.reserve(static_cast<std::size_t>(offset));
    for (const auto& part : parts)
        if (part.size) out.append(reinterpret_cast<const char*>(part.data), part.size);
    return out;
}
struct View {
    std::uint64_t frame = 0;
    std::uint32_t width = 360, height = 240;
    bool legacy = false;
    std::vector<Section> sections;
    Section find(const char* name) const {
        for (const auto& s : sections) if (s.name == name) return s;
        return {};
    }
};
inline bool parse(const std::vector<std::uint8_t>& bytes, View* out) {
    View result;
    if (bytes.size() < 16) return false;
    const auto* p = bytes.data();
    if (std::memcmp(p, "TLAGPUF1", 8) == 0) {
        result.legacy = true;
        result.frame = read64(p + 8);
        std::size_t offset = 16;
        for (const auto& part : {Section{"IO", nullptr, 0x400},
                 Section{"OAM", nullptr, 0x400}, Section{"PAL", nullptr, 0x400},
                 Section{"VRAM", nullptr, 0x18000}, Section{"ROWIO", nullptr, 160 * 0x58},
                 Section{"ROWVALID", nullptr, 160}, Section{"AFFREF", nullptr, 160 * 16},
                 Section{"AFFVALID", nullptr, 160}}) {
            if (part.size > bytes.size() - offset) return false;
            result.sections.push_back({part.name, p + offset, part.size});
            offset += part.size;
        }
    } else if (std::memcmp(p, "TLAGPUF2", 8) == 0) {
        if (bytes.size() < 32) return false;
        const std::uint32_t count = read32(p + 8);
        if (count > (bytes.size() - 32) / 24) return false;
        const std::size_t table_end = 32 + std::size_t(count) * 24;
        result.frame = read64(p + 16);
        result.width = read32(p + 24);
        result.height = read32(p + 28);
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto* entry = p + 32 + std::size_t(i) * 24;
            std::size_t n = 0;
            while (n < 8 && entry[n]) ++n;
            std::string name(reinterpret_cast<const char*>(entry), n);
            const std::uint64_t offset = read64(entry + 8), size = read64(entry + 16);
            if (name.empty() || result.find(name.c_str()).data ||
                offset < table_end || offset > bytes.size() || size > bytes.size() - offset)
                return false;
            result.sections.push_back({name, p + std::size_t(offset), std::size_t(size)});
        }
    } else return false;
    for (const auto& expected : {Section{"IO", nullptr, 0x400},
             Section{"OAM", nullptr, 0x400}, Section{"PAL", nullptr, 0x400},
             Section{"VRAM", nullptr, 0x18000}, Section{"ROWIO", nullptr, 160 * 0x58},
             Section{"ROWVALID", nullptr, 160}, Section{"AFFREF", nullptr, 160 * 16},
             Section{"AFFVALID", nullptr, 160}})
        if (result.find(expected.name.c_str()).size != expected.size) return false;
    if (!result.legacy) {
        if (result.width < 240 || result.height < 160 ||
            result.width > 480 || result.height > 320) return false;
        for (const auto& expected : {Section{"EWRAM", nullptr, 0x40000},
                 Section{"IWRAM", nullptr, 0x8000}, Section{"OBJECTS", nullptr, 128 * 12},
                 Section{"STATE", nullptr, 8}})
            if (result.find(expected.name.c_str()).size != expected.size) return false;
        // Menu-open frames copy the whole video memory, the second sprite
        // page included (build_room_vram), since that page was added.
        const auto room_vram = result.find("ROOMVRAM").size;
        if (room_vram != 0 && room_vram != 0x18000 && room_vram != 0x20000)
            return false;
        if (result.find("ROOMPAL").size != 0 && result.find("ROOMPAL").size != 0x400)
            return false;
    }
    *out = std::move(result);
    return true;
}
// Explicit 36-byte records: x, y, size, width, height, blend, flip bits, art offset.
inline std::string pack_stamps(const std::vector<gsr::EffectSpark>& stamps) {
    std::string out;
    out.reserve(stamps.size() * 36);
    for (const auto& s : stamps) {
        u32(out, std::uint32_t(s.x)); u32(out, std::uint32_t(s.y));
        u32(out, std::uint32_t(s.size_index));
        u32(out, std::uint32_t(s.width)); u32(out, std::uint32_t(s.height));
        u32(out, static_cast<std::uint32_t>(s.blend));
        u32(out, (s.flip_x ? 1u : 0u) | (s.flip_y ? 2u : 0u));
        u64(out, s.art_offset == std::numeric_limits<std::size_t>::max()
            ? UINT64_MAX : std::uint64_t(s.art_offset));
    }
    return out;
}
inline bool unpack_stamps(const View& frame, std::vector<gsr::EffectSpark>* out) {
    const auto part = frame.find("FXSTAMP"), art = frame.find("FXART");
    if (part.size % 36) return false;
    out->clear();
    for (std::size_t at = 0; at < part.size; at += 36) {
        const auto* p = part.data + at;
        gsr::EffectSpark s;
        s.x = std::int32_t(read32(p)); s.y = std::int32_t(read32(p + 4));
        s.size_index = std::int32_t(read32(p + 8));
        s.width = std::int32_t(read32(p + 12)); s.height = std::int32_t(read32(p + 16));
        const auto blend = read32(p + 20), flip = read32(p + 24);
        const auto offset = read64(p + 28);
        if (blend > static_cast<std::uint32_t>(gsr::EffectBlend::Copy) || flip > 3 ||
            s.width < 0 || s.height < 0 || (offset != UINT64_MAX && offset > art.size))
            return false;
        s.blend = static_cast<gsr::EffectBlend>(blend);
        s.flip_x = (flip & 1) != 0; s.flip_y = (flip & 2) != 0;
        s.art_offset = offset == UINT64_MAX ? std::numeric_limits<std::size_t>::max()
                                           : static_cast<std::size_t>(offset);
        out->push_back(s);
    }
    return true;
}
}  // namespace tla_frame
