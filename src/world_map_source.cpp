// world_map_source.cpp -- see world_map_source.h. Addresses and the piece
// format are in FACTS.md, 2026-09-25 ("The L-button zoom-out's broken left
// edge is the game's world-map ring running out").
#include "world_map_source.h"

#include <cstdio>
#include <cstring>

namespace gsr {
namespace {

constexpr std::uint32_t kMapNumberAddr = 0x02000408u;  // 2 on the world map
constexpr std::uint32_t kStatePointerAddr = 0x03001E70u;
constexpr std::uint32_t kDirectoryOffset = 0x110u;   // piece directory (ROM)
constexpr std::uint32_t kGridOffset = 0x138u;        // 16x16 piece ids
constexpr std::uint32_t kSlotOffset = 0x338u;        // 8 loaded piece ids
constexpr unsigned kSecondLayerIds = 0x140u;         // Func_114a0
constexpr std::uint32_t kEntryTableAddr = 0x02010000u;  // 4 tile bytes per id
constexpr std::uint32_t kRingAddr = 0x02020000u;     // the game's loaded pieces
constexpr std::size_t kPieceBytes = 1024;            // 16x16 cells x 4 bytes
constexpr std::size_t kMaxDecoded = 4096;

// The game's own decompressor, the ARM routine at ROM 0x08001B70 that
// Func_53e8 copies to RAM: a flag byte, then eight items, most significant
// flag first. 0 is a literal byte; 1 is a copy of (low nibble + 1) bytes from
// ((high nibble << 8) | next byte) back, where a zero nibble takes its length
// from one more byte (+ 0x11) and a zero distance ends the data.
bool decompress(const std::uint8_t* src, std::size_t avail,
                std::vector<std::uint8_t>* out) {
    out->clear();
    std::size_t pos = 0;
    for (;;) {
        if (pos >= avail) return false;
        const unsigned flags = src[pos++];
        for (int bit = 7; bit >= 0; --bit) {
            if (((flags >> bit) & 1u) == 0u) {
                if (pos >= avail) return false;
                out->push_back(src[pos++]);
            } else {
                if (pos + 2 > avail) return false;
                const unsigned b0 = src[pos], b1 = src[pos + 1];
                pos += 2;
                const std::size_t dist = ((b0 & 0xF0u) << 4) | b1;
                unsigned count = b0 & 0x0Fu;
                if (count == 0) {
                    if (dist == 0) return true;
                    if (pos >= avail) return false;
                    count = src[pos++] + 0x10u;
                }
                if (dist == 0 || dist > out->size()) return false;
                const std::size_t from = out->size() - dist;
                for (unsigned i = 0; i <= count; ++i)
                    out->push_back((*out)[from + i]);
            }
            if (out->size() > kMaxDecoded) return false;
        }
    }
}

struct Guest {
    const std::uint8_t* ewram;
    std::size_t ewram_bytes;
    const std::uint8_t* iwram;
    std::size_t iwram_bytes;

    const std::uint8_t* at(std::uint32_t addr, std::size_t n) const {
        if ((addr >> 24) == 0x02u) {
            const std::uint32_t off = addr - 0x02000000u;
            if (off + n <= ewram_bytes) return ewram + off;
        } else if ((addr >> 24) == 0x03u) {
            const std::uint32_t off = addr - 0x03000000u;
            if (off + n <= iwram_bytes) return iwram + off;
        }
        return nullptr;
    }
    bool u16(std::uint32_t addr, unsigned* v) const {
        const std::uint8_t* p = at(addr, 2);
        if (!p) return false;
        *v = p[0] | (p[1] << 8);
        return true;
    }
    bool u32(std::uint32_t addr, std::uint32_t* v) const {
        const std::uint8_t* p = at(addr, 4);
        if (!p) return false;
        *v = p[0] | (p[1] << 8) | (p[2] << 16) |
             (static_cast<std::uint32_t>(p[3]) << 24);
        return true;
    }
};

std::uint64_t fnv(std::uint64_t h, const std::uint8_t* p, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

}  // namespace

const std::vector<std::uint8_t>* WorldMapSource::piece(
        std::uint32_t directory, unsigned id, const std::uint8_t* rom,
        std::size_t rom_bytes) {
    if (directory != cached_directory_) {
        pieces_.clear();
        cached_directory_ = directory;
    }
    auto it = pieces_.find(id);
    if (it != pieces_.end())
        return it->second.empty() ? nullptr : &it->second;
    std::vector<std::uint8_t>& out = pieces_[id];
    const std::size_t dir = directory - 0x08000000u;
    const std::size_t slot = dir + static_cast<std::size_t>(id) * 4u;
    if (slot + 4 > rom_bytes) return nullptr;
    const std::uint32_t offset = rom[slot] | (rom[slot + 1] << 8) |
        (rom[slot + 2] << 16) | (static_cast<std::uint32_t>(rom[slot + 3]) << 24);
    const std::size_t src = dir + offset;
    std::vector<std::uint8_t> decoded;
    if (src >= rom_bytes ||
        !decompress(rom + src, rom_bytes - src, &decoded) ||
        decoded.size() < kPieceBytes) {
        return nullptr;  // `out` stays empty: remembered as a failure
    }
    out.assign(decoded.begin(), decoded.begin() + kPieceBytes);
    return &out;
}

bool WorldMapSource::update(const std::uint8_t* ewram, std::size_t ewram_bytes,
                            const std::uint8_t* iwram, std::size_t iwram_bytes,
                            const std::uint8_t* rom, std::size_t rom_bytes) {
    valid_ = false;
    if (!ewram || !iwram || !rom) return false;
    const Guest g{ewram, ewram_bytes, iwram, iwram_bytes};
    unsigned map_number = 0;
    if (!g.u16(kMapNumberAddr, &map_number) || map_number != 2u) return false;
    std::uint32_t state = 0, directory = 0;
    if (!g.u32(kStatePointerAddr, &state) ||
        !g.u32(state + kDirectoryOffset, &directory))
        return false;
    if ((directory >> 24) != 0x08u || directory - 0x08000000u >= rom_bytes)
        return false;
    const std::uint8_t* grid = g.at(state + kGridOffset, 256 * 2);
    const std::uint8_t* slots = g.at(state + kSlotOffset, 8 * 2);
    const std::uint8_t* entries = g.at(kEntryTableAddr, 0x10000);
    const std::uint8_t* ring = g.at(kRingAddr, 0x2000);
    if (!grid || !slots || !entries || !ring) return false;

    // The pieces the game itself has loaded must be exactly what we decode.
    for (int k = 0; k < 8; ++k) {
        const unsigned id = slots[k * 2] | (slots[k * 2 + 1] << 8);
        const std::vector<std::uint8_t>* p = piece(directory, id, rom, rom_bytes);
        const int layer = k >> 2, cy = (k >> 1) & 1, cx = k & 1;
        const std::uint8_t* dst =
            ring + (((layer * 2 + cy) * 32 + cx) * 64);
        bool same = p != nullptr;
        for (int r = 0; same && r < 16; ++r)
            same = std::memcmp(p->data() + r * 64, dst + r * 0x80, 64) == 0;
        if (!same) {
            if (!reported_mismatch_) {
                reported_mismatch_ = true;
                std::fprintf(stderr,
                    "[world-map] a loaded piece (slot %d, id %u) differs from "
                    "our unpacking; margins use the game's own map\n", k, id);
            }
            return false;
        }
    }

    std::uint64_t key = 0xCBF29CE484222325ull;
    key = fnv(key, reinterpret_cast<const std::uint8_t*>(&directory), 4);
    key = fnv(key, grid, 256 * 2);
    key = fnv(key, entries, 0x10000);
    if ((key == built_key_ || key == rejected_key_) && !tiles_.empty()) {
        valid_ = true;
        return true;
    }

    std::vector<std::uint8_t> tiles(
        static_cast<std::size_t>(kSideTiles) * kSideTiles * 2u, 0);
    for (int layer = 0; layer < 2; ++layer) {
        for (int gi = 0; gi < 256; ++gi) {
            const unsigned id = (grid[gi * 2] | (grid[gi * 2 + 1] << 8)) +
                                layer * kSecondLayerIds;
            const std::vector<std::uint8_t>* p =
                piece(directory, id, rom, rom_bytes);
            if (!p) return false;
            const int gx = gi & 15, gy = gi >> 4;
            for (int my = 0; my < 16; ++my) {
                for (int mx = 0; mx < 16; ++mx) {
                    const std::uint8_t* cell = p->data() + my * 64 + mx * 4;
                    const std::uint32_t entry_addr =
                        kEntryTableAddr + (cell[0] | (cell[1] << 8)) * 4u;
                    const std::uint8_t* e = g.at(entry_addr, 4);
                    if (!e) return false;
                    const int tx = gx * 32 + mx * 2;
                    const int ty = layer * kSideTiles + gy * 32 + my * 2;
                    std::uint8_t* row0 = &tiles[static_cast<std::size_t>(ty) * kSideTiles];
                    std::uint8_t* row1 = row0 + kSideTiles;
                    row0[tx] = e[0]; row0[tx + 1] = e[1];
                    row1[tx] = e[2]; row1[tx + 1] = e[3];
                }
            }
        }
    }
    previous_tiles_.swap(tiles_);
    previous_key_ = built_key_;
    tiles_.swap(tiles);
    built_key_ = key;
    ++generation_;
    valid_ = true;
    return true;
}

bool WorldMapSource::restore_previous() {
    if (previous_tiles_.empty()) return false;
    tiles_.swap(previous_tiles_);
    previous_tiles_.clear();
    rejected_key_ = built_key_;
    built_key_ = previous_key_;
    ++generation_;
    return true;
}

}  // namespace gsr
