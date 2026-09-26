// world_map_source.h -- Golden Sun's whole world map, unpacked host-side.
//
// The game keeps only a 512x512-pixel ring of the world map in memory, refilled
// in 256-pixel pieces around the camera, so it guarantees about 128 map pixels
// either side of it. That is exactly what the console's L-button zoom-out
// needs; the expanded view needs more, and past the ring the game's memory
// holds leftover pieces (FACTS.md, 2026-09-25). This unpacks every piece the
// same way the game does -- piece table and directory from the game's own
// state, compressed bytes from the cartridge, tile entries from the game's own
// table -- so the renderer can draw the margins from the real world.
//
// Each frame the pieces the game itself has loaded are compared with what this
// unpacked; any difference and the map is not used that frame.
#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace gsr {

class WorldMapSource {
public:
    static constexpr int kSideTiles = 512;  // per axis, per layer (4096 px)

    // Reads the world-map state from `ewram` (GBA 0x02000000) and `iwram`
    // (0x03000000), decoding pieces from `rom` (0x08000000). Safe to call
    // every frame; outside the world map it just returns false.
    bool update(const std::uint8_t* ewram, std::size_t ewram_bytes,
                const std::uint8_t* iwram, std::size_t iwram_bytes,
                const std::uint8_t* rom, std::size_t rom_bytes);

    bool valid() const { return valid_; }
    // Changes whenever tiles() changes, so a caller uploads only then.
    std::uint32_t generation() const { return generation_; }
    // kSideTiles wide, 2 * kSideTiles tall: rows 0..511 are BG3's world
    // (the game's first layer), rows 512..1023 BG2's. One affine tile index
    // per byte, exactly what the console's map would hold there.
    const std::vector<std::uint8_t>& tiles() const { return tiles_; }

private:
    const std::vector<std::uint8_t>* piece(std::uint32_t directory,
                                           unsigned id,
                                           const std::uint8_t* rom,
                                           std::size_t rom_bytes);

    bool valid_ = false;
    std::uint32_t generation_ = 0;
    std::uint64_t built_key_ = 0;
    std::uint32_t cached_directory_ = 0;
    std::vector<std::uint8_t> tiles_;
    // Decoded pieces, 1024 bytes each (16x16 cells of 4 bytes), by id.
    std::unordered_map<unsigned, std::vector<std::uint8_t>> pieces_;
    bool reported_mismatch_ = false;
};

}  // namespace gsr
