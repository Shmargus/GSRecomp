#pragma once
// Fallen heroes in the menu (F1 Game page, both games): the field menu's
// party window (one window, a column per hero: name, HP, PP) shows a
// knocked-out hero's column see-through, at the opacity the player chose
// (100% = as the game draws it, 0% = a gap). Only the picture changes: the
// hero stays in the party, so Items and Djinn still reach them.
//
// The window (measured in both games, with the same
// frame pieces in both): a window record in use with y 0,
// height 5, width 6n+1 for n heroes (n <= 4), right-aligned (x + width 30),
// whose BG0 frame has the column joins (tiles `join_top`/`join_bottom`) at
// x+6, x+12, ... Column i is party order i; its tiles are x+1+6i..x+5+6i.
// A join or outer edge fades only when every column beside it fades. Every
// other in-use window matching the shape in 112 The Broken Seal captures
// (a dialogue box) has no joins there.
#include <cstdint>
#include <string>

#include "game_options.h"
#include "gba_bus.h"
#include "host_config_ui.h"

namespace gsr::party_window_fade {

struct Game {
    std::uint32_t window_block_slot;  // [slot] = the window system's block
    std::uint32_t window_map;         // block + this: BG0 map shadow, 32 a row
    std::uint32_t window_records;     // block + this: records, 0x24 each
    int window_record_count;
    std::uint16_t join_top, join_bottom;  // frame pieces where columns meet
    std::uint32_t joined;             // flag bank: bit id = hero id joined
    std::uint32_t order;              // party order bytes
    std::uint32_t hero_records;       // hero record 0
    std::uint32_t hero_record_size;
    std::uint32_t hp_offset;          // current HP, u16; 0 = knocked out
};

// Record fields (s16 tiles) and the in-use bit (measured).
constexpr std::uint32_t kRecordSize = 0x24u, kWidth = 8u, kHeight = 0xAu,
                        kX = 0xCu, kY = 0xEu, kFlags = 0x16u;
constexpr int kColumnTiles = 6, kMaxColumns = 4, kScreenTiles = 30, kHeightTiles = 5;
constexpr const char* kOptionKey = "FallenHeroOpacity";  // percent, 100 = off

inline const Game* game = nullptr;
inline void install(const Game& g) { game = &g; }

inline int opacity_percent() {
    const int v = gbarecomp::game_option_get(kOptionKey, 100);
    return v < 0 ? 0 : v > 100 ? 100 : v;
}

struct Fade {
    std::uint32_t columns = 0;  // console tile columns, bit n = pixels 8n..8n+7
    int row0 = 0, row1 = 0;     // tile rows
};

// This frame's fade, from the game's memory (emulation thread).
inline Fade find(gba::GbaBus& b) {
    Fade fade;
    if (!game || opacity_percent() >= 100) return fade;
    const std::uint32_t block = b.read32(game->window_block_slot);
    if ((block >> 24) != 0x02u) return fade;
    int heroes = 0;
    for (unsigned id = 0; id < 8u; ++id) heroes += (b.read8(game->joined + id / 8u) >> (id & 7u)) & 1u;
    const auto tile = [&](int x, int y) {
        return static_cast<std::uint16_t>(
            b.read16(block + game->window_map + std::uint32_t(y * 32 + x) * 2u) & 0x3FFu);
    };
    for (int i = 0; i < game->window_record_count; ++i) {
        const std::uint32_t r = block + game->window_records + std::uint32_t(i) * kRecordSize;
        if ((b.read8(r + kFlags) & 1u) == 0) continue;
        const int w = static_cast<std::int16_t>(b.read16(r + kWidth));
        const int h = static_cast<std::int16_t>(b.read16(r + kHeight));
        const int x = static_cast<std::int16_t>(b.read16(r + kX));
        const int y = static_cast<std::int16_t>(b.read16(r + kY));
        if (y != 0 || h != kHeightTiles || x < 0 || x + w != kScreenTiles || (w - 1) % kColumnTiles) continue;
        const int n = (w - 1) / kColumnTiles;
        if (n < 1 || n > kMaxColumns || n > heroes) continue;
        bool joins = true;
        for (int k = 1; k < n && joins; ++k)
            joins = tile(x + k * kColumnTiles, y) == game->join_top &&
                    tile(x + k * kColumnTiles, y + h - 1) == game->join_bottom;
        if (!joins) continue;
        bool down[kMaxColumns] = {};
        for (int c = 0; c < n; ++c) {
            const unsigned id = b.read8(game->order + std::uint32_t(c));
            down[c] = id < 8u &&
                b.read16(game->hero_records + id * game->hero_record_size + game->hp_offset) == 0;
            if (!down[c]) continue;
            for (int t = 1; t < kColumnTiles; ++t)
                fade.columns |= 1u << (x + c * kColumnTiles + t);
        }
        for (int k = 0; k <= n; ++k)  // joins and outer edges
            if ((k == 0 || down[k - 1]) && (k == n || down[k]))
                fade.columns |= 1u << (x + k * kColumnTiles);
        fade.row0 = y;
        fade.row1 = y + h - 1;
        return fade;
    }
    return fade;
}

// Hands `fade` to the field renderer (FieldSceneRenderer::set_window_fade)
// at the player's opacity.
template <class Renderer>
inline void apply(Renderer& renderer, const Fade& fade) {
    renderer.set_window_fade(fade.columns, fade.row0, fade.row1, opacity_percent() / 100.0f);
}

// F1 Game page.
inline void draw() {
    int v = opacity_percent();
    if (gbarecomp::config_ui_slider_int("Opacity for Dead Heroes", &v, 0, 100, "%d%%"))
        gbarecomp::game_option_set(kOptionKey, v);
}

}  // namespace gsr::party_window_fade
