// Game-specific layout for the shared complete-world-map source.
#pragma once

#include "../../engine/src/runtime/world_map_source.h"

namespace gsr {

class WorldMapSource : public gbarecomp::WorldMapSource {
public:
    static constexpr int kSideTiles = 512;

    WorldMapSource()
        : gbarecomp::WorldMapSource({0x02000408u, 0x03001E70u, 0x110u, 0x138u, 0x338u,
                                    0x02010000u, 0x02020000u, 16, 0x140u}) {}
};

}  // namespace gsr
