// tile_ring_redraw.h -- repaint a field's tile rings after a camera jump.
//
// Both Golden Sun games keep each of three field layers in a 256x256 tile
// ring in video memory and, per frame, redraw only the strip the camera
// scrolls onto: one column when a layer's x crosses an 8-pixel tile, one
// two-tile row when its y crosses 16 pixels. A camera that jumps further
// (our view-size inset, on a switch to or from Wide) leaves the tiles in
// between from the old place until the camera leaves again (The Broken Seal
// F12 gpu_rewind_0055). Called on entry to the game's camera follow, this
// repaints every ring with the game's own row writer, which fills 16 cells
// (32 tiles, the ring's width) of a two-tile row: 16 calls per layer. Only
// after a jump of more than one strip within the same map, and only for the
// field (its state in EWRAM, video mode 0). The addresses are the game's,
// measured per game (The Broken Seal runner_main.cpp, The Lost Age
// tla_widescreen.cpp); see each game's FACTS.md.
#pragma once

#include <array>
#include <cstdint>
#include <cstdlib>

#include "gba_bus.h"
#include "runtime.h"
#include "runtime_bus_bridge.h"

namespace gsr {

struct TileRingLayout {
    std::uint32_t state_pointer;  // holds the field state S
    std::uint32_t first_layer;    // S + this = layer 0's x, y (16.16)
    std::uint32_t layer_stride;
    std::uint32_t row_writer;     // THUMB (layer, tile x, tile y)
    std::uint32_t map_number;     // u16
    bool signed_tiles;            // round negatives as the follow does
};

class TileRingRedraw {
public:
    explicit TileRingRedraw(TileRingLayout layout) : layout_(layout) {}

    void on_follow_entry() {
        gba::GbaBus* bus = gbarecomp::active_bus();
        if (!bus) return;
        const std::uint32_t s = bus->read32(layout_.state_pointer);
        if ((s >> 24) != 0x02u || (bus->read16(0x04000000u) & 7u) != 0u) {
            map_ = 0xFFFFu;
            return;
        }
        const std::uint16_t map = bus->read16(layout_.map_number);
        std::array<std::array<std::int32_t, 2>, 3> now{};
        bool jumped = false;
        for (int n = 0; n < 3; ++n) {
            const std::uint32_t at = s + layout_.first_layer +
                layout_.layer_stride * static_cast<std::uint32_t>(n);
            now[n] = {tile(bus->read32(at)), tile(bus->read32(at + 4u))};
            if (std::abs(now[n][0] - tiles_[n][0]) > 1 ||
                std::abs((now[n][1] >> 1) - (tiles_[n][1] >> 1)) > 1)
                jumped = true;
        }
        if (jumped && map == map_) {
            for (int n = 0; n < 3; ++n)
                for (int row = 0; row < 32; row += 2)
                    gbarecomp::guest_call(layout_.row_writer | 1u, static_cast<std::uint32_t>(n),
                                          static_cast<std::uint32_t>(now[n][0]),
                                          static_cast<std::uint32_t>(now[n][1] + row), 0u,
                                          nullptr);
        }
        map_ = map;
        tiles_ = now;
    }

private:
    // A 16.16 position's 8-pixel tile, as the game's follow computes it.
    std::int32_t tile(std::uint32_t v) const {
        if (!layout_.signed_tiles) return static_cast<std::int32_t>(v >> 19);
        const std::int32_t s = static_cast<std::int32_t>(v);
        return (s < 0 ? s + 0x7FFFF : s) >> 19;
    }

    TileRingLayout layout_;
    std::uint16_t map_ = 0xFFFFu;
    std::array<std::array<std::int32_t, 2>, 3> tiles_{};
};

}  // namespace gsr
