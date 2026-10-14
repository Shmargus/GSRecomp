// Golden Sun: The Lost Age -- text the runner draws in place of the game's
// own lines.
//
// The text decoder 0x0803B094 takes each decoded character at 0x0803B13C
// (`adds r7,r0,#0`), with the line's text ID at [sp+0x30] (FACTS.md,
// 2026-10-03). When a feature has a replacement for that ID, every character
// comes from the replacement instead, then the 0 that ends the line.
// Features answer through tla_*_text (src/tla.h), asked in order on the first
// character of each line.

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <string>

#include "gba_bus.h"
#include "gba_ppu.h"
#include "runtime.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"
#include "tla.h"

namespace {

constexpr uint32_t kDecodedCharPc = 0x0803B13Cu;
constexpr uint32_t kDecoderTextIdStackOffset = 0x30u;

bool replacement(uint32_t text_id, std::string& out) {
    return tla_settings_page_text(text_id, out) ||
           tla_text_speed_text(text_id, out) ||
           tla_hard_mode_text(text_id, out) ||
           tla_gs_save_import_text(text_id, out);
}

bool g_active = false;
uint32_t g_id = 0;
std::string g_text;
size_t g_pos = 0;

int (*g_next_alu_immediate)(uint32_t, uint32_t, uint32_t*) = nullptr;

// The window, in the game's own pieces: menu font tiles 16..23 are its
// corners and edges and 32 its inside, drawn with background palette bank
// 15 (the battle's party window in F12 0047, BG0 map 0x06002000).
constexpr unsigned char kTopLeft = 16, kTop = 17, kTopRight = 18;
constexpr unsigned char kBottomLeft = 19, kBottom = 20, kBottomRight = 21;
constexpr unsigned char kLeft = 22, kRight = 23, kFill = 32;
constexpr uint32_t kWindowBank = 0x05000000u + 15u * 32u;
// Bank 15 as F12 0047 held it; used until the live bank is seen holding the
// window palette (letter 0x7FFF at index 1 and shadow 0 at index 3).
constexpr uint16_t kDefaultWindowPalette[16] = {
    0x44E0, 0x7FFF, 0x318C, 0x0000, 0x45A0, 0x3980, 0x3160, 0x2940,
    0x4DC1, 0x55E1, 0x5E21, 0x294A, 0x5294, 0x001F, 0x03FF, 0x7C00};
uint16_t g_palette[16] = {};
bool g_palette_known = false;

std::function<void(int, int, uint8_t)> menu_pixel_writer(
    uint8_t* rgb, uint32_t width, uint32_t height) {
    if (gba::GbaBus* b = gbarecomp::active_bus()) {
        uint16_t bank[16];
        for (uint32_t i = 0; i < 16u; ++i) bank[i] = b->read16(kWindowBank + i * 2u);
        if (bank[1] == 0x7FFFu && bank[3] == 0u) {
            std::copy(bank, bank + 16, g_palette);
            g_palette_known = true;
        }
    }
    const uint16_t* palette = g_palette_known ? g_palette : kDefaultWindowPalette;
    // In the expanded view the console's picture sits inside the margins.
    int ox = 0, oy = 0;
    if (const gba::GbaPpu* ppu = gbarecomp::active_ppu()) {
        ox = static_cast<int>(ppu->view_extra_left());
        oy = static_cast<int>(ppu->view_extra_top());
    }
    return [=](int x, int y, uint8_t index) {
        x += ox;
        y += oy;
        if (index == 0 || x < 0 || y < 0 || x >= static_cast<int>(width) ||
            y >= static_cast<int>(height))
            return;
        const uint16_t c = palette[index & 0xFu];
        uint8_t* p = rgb + (static_cast<size_t>(y) * width + x) * 3u;
        const unsigned r = c & 31u, g = (c >> 5) & 31u, bl = (c >> 10) & 31u;
        p[0] = static_cast<uint8_t>((r << 3) | (r >> 2));
        p[1] = static_cast<uint8_t>((g << 3) | (g >> 2));
        p[2] = static_cast<uint8_t>((bl << 3) | (bl >> 2));
    };
}

int text_alu_immediate(uint32_t pc, uint32_t original, uint32_t* out_value) {
    if (pc != kDecodedCharPc)
        return g_next_alu_immediate ? g_next_alu_immediate(pc, original, out_value) : 0;
    gba::GbaBus* b = gbarecomp::active_bus();
    if (!b) return 0;
    const uint32_t id = b->read32(g_cpu.R[13] + kDecoderTextIdStackOffset);
    if (!g_active || id != g_id) {
        g_id = id;
        g_pos = 0;
        g_text.clear();
        g_active = replacement(id, g_text);
        if (!g_active) return 0;
    }
    uint32_t wanted = 0u;
    if (g_pos < g_text.size())
        wanted = static_cast<unsigned char>(g_text[g_pos++]);
    else
        g_active = false;
    // r7 = r0 + imm, so this imm makes r7 the wanted character.
    *out_value = original + (wanted - g_cpu.R[0]);
    return 1;
}

}  // namespace

// The menu font: asset 0x13 of the table 0x08680000 (0x08013300), 8x8 4bpp,
// tile = character code; advance per character 0x0805F484, indexed by code -
// 0x20 (read by the glyph routine 0x080416CC). Copied out of the ROM on first
// use.
uint8_t tla_menu_glyph(unsigned char code, int gx, int gy) {
    static std::array<uint8_t, 256 * 32> tiles{};
    static bool loaded = false;
    if (!loaded) {
        gba::GbaBus* b = gbarecomp::active_bus();
        if (!b) return 0u;
        const uint32_t font = b->read32(0x08680000u + 0x13u * 4u);
        for (uint32_t i = 0; i < tiles.size(); ++i) tiles[i] = b->read8(font + i);
        loaded = true;
    }
    const uint8_t byte = tiles[code * 32u + gy * 4 + gx / 2];
    return static_cast<uint8_t>((byte >> (4 * (gx & 1))) & 0xFu);
}

uint32_t tla_menu_text_width(const std::string& text) {
    gba::GbaBus* b = gbarecomp::active_bus();
    uint32_t width = 0;
    for (const char c : text) {
        const auto code = static_cast<unsigned char>(c);
        if (b && code >= 0x20u) width += b->read8(0x0805F484u + code - 0x20u);
    }
    return width;
}

// Letter and shadow pixels only; whatever is behind shows around them, as it
// does behind the game's own text.
void tla_menu_draw_text(const std::string& text, int x, int y,
                        const std::function<void(int, int, uint8_t)>& put) {
    for (const char c : text) {
        const auto code = static_cast<unsigned char>(c);
        if (code < 0x20u) continue;
        for (int gy = 0; gy < 8; ++gy) {
            for (int gx = 0; gx < 8; ++gx) {
                const uint8_t v = tla_menu_glyph(code, gx, gy);
                if (v == kTlaGlyphLetter || v == kTlaGlyphShadow) put(x + gx, y + gy, v);
            }
        }
        x += static_cast<int>(tla_menu_text_width(std::string(1, c)));
    }
}

void tla_menu_paint_window(uint8_t* rgb, uint32_t width, uint32_t height,
                           int tile_x, int tile_y, int tiles_w, int tiles_h) {
    const auto put = menu_pixel_writer(rgb, width, height);
    for (int ty = 0; ty < tiles_h; ++ty) {
        for (int tx = 0; tx < tiles_w; ++tx) {
            const bool top = ty == 0, bottom = ty == tiles_h - 1;
            const bool left = tx == 0, right = tx == tiles_w - 1;
            const unsigned char code =
                top ? (left ? kTopLeft : right ? kTopRight : kTop)
                : bottom ? (left ? kBottomLeft : right ? kBottomRight : kBottom)
                : left ? kLeft : right ? kRight : kFill;
            for (int gy = 0; gy < 8; ++gy)
                for (int gx = 0; gx < 8; ++gx)
                    put((tile_x + tx) * 8 + gx, (tile_y + ty) * 8 + gy,
                        tla_menu_glyph(code, gx, gy));
        }
    }
}

void tla_menu_paint_text(uint8_t* rgb, uint32_t width, uint32_t height,
                         const std::string& text, int x, int y) {
    tla_menu_draw_text(text, x, y, menu_pixel_writer(rgb, width, height));
}

void TlaValueCaption::arm(uint32_t text_id, const char* word, bool selected) {
    text = selected ? std::string("< ") + word + " >" : std::string(word);
    id = text_id;
    armed = true;
}

bool TlaValueCaption::take(uint32_t text_id, std::string& out) {
    if (!armed || text_id != id) return false;
    armed = false;
    out = text;
    return true;
}

uint32_t TlaValueCaption::draw_x() const {
    return kTlaValueCentreX - tla_menu_text_width(text) / 2u;
}

// Chains in front of the immediate hook already in RunOptions.
void tla_install_text(gbarecomp::RunOptions& options) {
    g_next_alu_immediate = options.thumb_alu_immediate_override;
    options.thumb_alu_immediate_override = text_alu_immediate;
}
