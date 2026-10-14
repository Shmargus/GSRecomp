// Game-specific layout for the shared complete-world-map source.
#pragma once

#include "../../engine/src/runtime/world_map_source.h"

namespace gsr {

class WorldMapSource : public gbarecomp::WorldMapSource {
public:
    static constexpr int kSideTiles = 1024;

    WorldMapSource()
        : gbarecomp::WorldMapSource({0x02000428u, 0x03000020u, 0x114u, 0x148u, 0x948u,
                                    0x02010000u, 0x02020000u, 32, 0x400u, true}) {}
};

}  // namespace gsr
