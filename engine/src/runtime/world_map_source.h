// world_map_source.h -- Whole world maps, unpacked host-side.
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

namespace gbarecomp {

struct WorldMapLayout {
    std::uint32_t map_number_addr;
    std::uint32_t state_pointer_addr;
    std::uint32_t directory_offset;
    std::uint32_t grid_offset;
    std::uint32_t slot_offset;
    std::uint32_t entry_table_addr;
    std::uint32_t ring_addr;
    int grid_side;
    unsigned second_layer_ids;
    bool zero_offset_is_empty = false;
};

// The game's own decompressor, the ARM routine at ROM 0x08001B70 (see
// world_map_source.cpp). False on a malformed stream or past `max_bytes`.
bool lz_decompress(const std::uint8_t* src, std::size_t avail,
                   std::vector<std::uint8_t>* out, std::size_t max_bytes);

class WorldMapSource {
public:
    explicit WorldMapSource(WorldMapLayout layout) : layout_(layout) {}

    int side_tiles() const { return layout_.grid_side * 32; }

    // Reads the world-map state from `ewram` (GBA 0x02000000) and `iwram`
    // (0x03000000), decoding pieces from `rom` (0x08000000). Safe to call
    // every frame; outside the world map it just returns false.
    bool update(const std::uint8_t* ewram, std::size_t ewram_bytes,
                const std::uint8_t* iwram, std::size_t iwram_bytes,
                const std::uint8_t* rom, std::size_t rom_bytes);

    bool valid() const { return valid_; }
    // Changes whenever tiles() changes, so a caller uploads only then.
    std::uint32_t generation() const { return generation_; }
    // side_tiles() wide, 2 * side_tiles() tall: BG3's world (the game's first
    // layer), then BG2's. One affine tile index per byte, exactly what the
    // console's map would hold there.
    const std::vector<std::uint8_t>& tiles() const { return tiles_; }
    // side_tiles() / 2 square, one byte per 16-pixel cell of the first layer:
    // the top byte of the cell's word, which the game keeps per-cell
    // attributes in (The Lost Age reads its climate there). Follows tiles().
    const std::vector<std::uint8_t>& cell_attributes() const { return attributes_; }

    // Go back to the map built before the latest rebuild, and keep it while
    // the game's memory stays as it is now. For a rebuild that disagrees
    // with the screen: cutscenes and the pause menu reuse part of the tile
    // table at 0x02010000 while the screen still shows the old world
    // (FACTS.md 2026-10-07). False when there is no earlier map.
    bool restore_previous();

private:
    WorldMapLayout layout_;

    const std::vector<std::uint8_t>* piece(std::uint32_t directory,
                                           unsigned id,
                                           const std::uint8_t* rom,
                                           std::size_t rom_bytes);

    bool valid_ = false;
    std::uint32_t generation_ = 0;
    std::uint64_t built_key_ = 0;
    std::uint64_t previous_key_ = 0;
    std::uint64_t rejected_key_ = 0;
    std::vector<std::uint8_t> previous_tiles_;
    std::vector<std::uint8_t> previous_attributes_;
    std::uint32_t cached_directory_ = 0;
    std::vector<std::uint8_t> tiles_;
    std::vector<std::uint8_t> attributes_;
    // Decoded pieces, 1024 bytes each (16x16 cells of 4 bytes), by id.
    std::unordered_map<unsigned, std::vector<std::uint8_t>> pieces_;
    bool reported_mismatch_ = false;
};

}  // namespace gbarecomp
