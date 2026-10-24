// Shared GLSL strings; the game supplies its decoded map's side in tiles.
#pragma once

// u_world_cell_art / u_world_art: other artwork for some cells
// (FieldSceneRenderer::upload_world_cell_art). Per 16-pixel cell, the set it
// is drawn from, 255 for video memory; each set's character blocks 2-3
// (VRAM 0x8000-0xFFFF) and, in palette row 2 + set, its colours.
#define GBARECOMP_WORLD_MAP_UNIFORMS \
    "uniform usampler2D u_world_map;\n" \
    "uniform int u_use_world;\n" \
    "uniform int u_world_side;\n" \
    "uniform usampler2D u_world_cell_art;\n" \
    "uniform usampler2D u_world_art;\n" \
    "uniform int u_world_art_sets;\n"

// Starts an if/else around the existing affine-ring sampling path. Sets
// `world_colour_row` (declared by the caller, 0) for a cell with other art.
#define GBARECOMP_WORLD_MAP_SAMPLE_BEGIN \
    "    bool world_row = u_use_world != 0 && outside_native &&\n" \
    "        (row_dispcnt & 7u) == 2u &&\n" \
    "        (((row_u16(reg_row, 0x0E) >> 8) & 0x1Fu) == 8u) &&\n" \
    "        (((row_u16(reg_row, 0x0C) >> 8) & 0x1Fu) == 10u);\n" \
    "    if (world_row) {\n" \
    "      ivec2 wt = ivec2((tex_x >> 3) & (u_world_side - 1),\n" \
    "        ((tex_y >> 3) & (u_world_side - 1)) + (u_layer == 2 ? u_world_side : 0));\n" \
    "      int tile = int(texelFetch(u_world_map, wt, 0).r);\n" \
    "      int at = bg_char_base + tile * 64 + (tex_y & 7) * 8 + (tex_x & 7);\n" \
    "      int art = 255;\n" \
    "      if (u_world_art_sets != 0 && bg_char_base >= 0x8000) {\n" \
    "        int cells = u_world_side >> 1;\n" \
    "        art = int(texelFetch(u_world_cell_art, ivec2((tex_x >> 4) & (cells - 1),\n" \
    "          (tex_y >> 4) & (cells - 1)), 0).r);\n" \
    "      }\n" \
    "      if (art < u_world_art_sets) {\n" \
    "        int a = art * 0x8000 + at - 0x8000;\n" \
    "        index = int(texelFetch(u_world_art, ivec2(a % 1024, a / 1024), 0).r);\n" \
    "        world_colour_row = 2 + art;\n" \
    "      } else {\n" \
    "        index = int(vram_byte(at));\n" \
    "      }\n" \
    "    } else {\n"
