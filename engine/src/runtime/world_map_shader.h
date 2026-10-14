// Shared GLSL strings; the game supplies its decoded map's side in tiles.
#pragma once

#define GBARECOMP_WORLD_MAP_UNIFORMS \
    "uniform usampler2D u_world_map;\n" \
    "uniform int u_use_world;\n" \
    "uniform int u_world_side;\n"

// Starts an if/else around the existing affine-ring sampling path.
#define GBARECOMP_WORLD_MAP_SAMPLE_BEGIN \
    "    bool world_row = u_use_world != 0 && outside_native &&\n" \
    "        (row_dispcnt & 7u) == 2u &&\n" \
    "        (((row_u16(reg_row, 0x0E) >> 8) & 0x1Fu) == 8u) &&\n" \
    "        (((row_u16(reg_row, 0x0C) >> 8) & 0x1Fu) == 10u);\n" \
    "    if (world_row) {\n" \
    "      ivec2 wt = ivec2((tex_x >> 3) & (u_world_side - 1),\n" \
    "        ((tex_y >> 3) & (u_world_side - 1)) + (u_layer == 2 ? u_world_side : 0));\n" \
    "      int tile = int(texelFetch(u_world_map, wt, 0).r);\n" \
    "      index = int(vram_byte(bg_char_base + tile * 64\n" \
    "        + (tex_y & 7) * 8 + (tex_x & 7)));\n" \
    "    } else {\n"
