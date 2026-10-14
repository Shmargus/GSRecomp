#include "gba_ppu.h"
#include "snapshot.h"
#include "view_config.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

namespace {

void store16(uint8_t* dst, uint16_t value) {
    dst[0] = static_cast<uint8_t>(value);
    dst[1] = static_cast<uint8_t>(value >> 8);
}

void expect_pixel(const uint8_t* actual,
                  uint8_t r,
                  uint8_t g,
                  uint8_t b,
                  const char* label) {
    if (actual[0] == r && actual[1] == g && actual[2] == b) return;
    std::fprintf(stderr,
                 "%s: expected RGB(%u,%u,%u), got RGB(%u,%u,%u)\n",
                 label, r, g, b, actual[0], actual[1], actual[2]);
    std::exit(1);
}

struct Fixture {
    std::array<uint8_t, 0x400> io{};
    std::array<uint8_t, 0x18000> vram{};
    std::array<uint8_t, 0x400> oam{};
    std::array<uint8_t, 0x400> pal{};
    std::array<uint8_t, gba::GbaPpu::kFramebufferBytes> rgb{};
    gba::GbaPpu ppu;
};

int test_positive_obj_x(int raw_x, int* out_x) {
    if (raw_x >= 0x100 && raw_x < 288 && out_x) {
        *out_x = raw_x;
        return 1;
    }
    return 0;
}

int test_mode0_obj_x(int raw_x, int* out_x) {
    // Synthetic town adapter: only reinterpret the 8 raw values needed to
    // cover a 24px right margin. Everything else keeps hardware's signed X.
    if (raw_x >= 0x100 && raw_x < 264 && out_x) {
        *out_x = raw_x;
        return 1;
    }
    return 0;
}

int g_obj_attr_provider_calls = 0;
int test_obj_attr_x(int index, uint16_t attr0, uint16_t attr1,
                    uint16_t attr2, int* out_x) {
    ++g_obj_attr_provider_calls;
    if (index != 0 || attr0 != 0 || attr1 != 10 || attr2 != 0 || !out_x)
        return 0;
    *out_x = 260;
    return 1;
}

int g_obj_y_provider_calls = 0;
int test_obj_y_provider(int raw_y, int* out_y) {
    ++g_obj_y_provider_calls;
    if (raw_y >= 160 && raw_y < 200 && out_y) {
        *out_y = raw_y;
        return 1;
    }
    return 0;
}

int g_margin_provider_action = gba::kWsTilemapReplace;

unsigned g_margin_diagnostics_callbacks = 0;
gba::WsMarginDiagnostics g_margin_diagnostics_last{};

void test_margin_diagnostics(const gba::WsMarginDiagnostics& diagnostics) {
    ++g_margin_diagnostics_callbacks;
    g_margin_diagnostics_last = diagnostics;
}

int g_vertical_tilemap_calls = 0;
int g_vertical_tilemap_vertical_calls = 0;
int g_vertical_tilemap_action = gba::kWsTilemapReplace;
int test_vertical_tilemap(int bg, int, int screen_y, uint16_t* out_entry) {
    ++g_vertical_tilemap_calls;
    if (screen_y < 0 || screen_y >= static_cast<int>(gba::GbaPpu::kScreenHeight))
        ++g_vertical_tilemap_vertical_calls;
    if (bg != 0 || !out_entry ||
        (screen_y >= 0 && screen_y < static_cast<int>(gba::GbaPpu::kScreenHeight)))
        return gba::kWsTilemapUnavailable;
    if (g_vertical_tilemap_action == gba::kWsTilemapReplace)
        *out_entry = 1u; // Authored tile 1, distinct from the wrapped tile 0.
    return g_vertical_tilemap_action;
}

int test_margin_tilemap(int bg, int, int, uint16_t* out_entry) {
    if (bg != 0 || !out_entry) return 0;
    *out_entry = 0;
    return g_margin_provider_action;
}

int g_bg_x_provider_calls = 0;

std::array<uint8_t, 0x28000> g_field_atlas{};
bool g_field_atlas_scene_valid = false;

void store32(uint8_t* dst, uint32_t value) {
    dst[0] = static_cast<uint8_t>(value);
    dst[1] = static_cast<uint8_t>(value >> 8);
    dst[2] = static_cast<uint8_t>(value >> 16);
    dst[3] = static_cast<uint8_t>(value >> 24);
}

int floor_div8(int value) {
    return value >= 0 ? value / 8 : -(((-value) + 7) / 8);
}

int test_golden_sun_field_tilemap(int bg, int hw_x, int screen_y,
                                  uint16_t* out_entry) {
    if (!g_field_atlas_scene_valid || !out_entry || bg < 1 || bg > 3 ||
        (hw_x >= 0 && hw_x < 240) || screen_y < 0 || screen_y >= 160) {
        return gba::kWsTilemapUnavailable;
    }
    constexpr int hofs = 115;
    constexpr int vofs = 239;
    const int tile_x = floor_div8(hofs + hw_x);
    const int tile_y = floor_div8(vofs + screen_y);
    const uint32_t map_x = static_cast<uint32_t>(tile_x >> 1) & 127u;
    const uint32_t map_y = static_cast<uint32_t>(tile_y >> 1) & 127u;
    const size_t map_off = 0x10000u +
        (static_cast<size_t>(map_y) * 128u + map_x) * 4u;
    const uint32_t id = static_cast<uint32_t>(g_field_atlas[map_off]) |
        (static_cast<uint32_t>(g_field_atlas[map_off + 1u]) << 8) |
        (static_cast<uint32_t>(g_field_atlas[map_off + 2u]) << 16) |
        (static_cast<uint32_t>(g_field_atlas[map_off + 3u]) << 24);
    const size_t raw_off = 0x20000u + static_cast<size_t>(id & 0xFFFu) * 8u +
        static_cast<size_t>(static_cast<uint32_t>(tile_y) & 1u) * 4u +
        static_cast<size_t>(static_cast<uint32_t>(tile_x) & 1u) * 2u;
    *out_entry = static_cast<uint16_t>(g_field_atlas[raw_off]) |
        static_cast<uint16_t>(g_field_atlas[raw_off + 1u] << 8);
    return gba::kWsTilemapReplace;
}

int test_bg_x_provider(int bg, int output_x, int, int* out_hw_x) {
    ++g_bg_x_provider_calls;
    if (bg != 0) return 0;
    if (output_x == 0 && out_hw_x) {
        *out_hw_x = 0;
        return 1;
    }
    if (output_x == 24) return -1;
    return 0;
}

unsigned g_margin_policy_calls = 0;
bool g_test_mode0_field_scene = false;

int test_field_bg_x_provider(int bg, int output_x, int, int*) {
    // Synthetic Golden Sun Mode 0 adapter: BG0 is screen-space/UI content and
    // field BG1-3 margins fail closed until a measured map provider exists.
    if (bg == 0 && (output_x < 24 || output_x >= 264)) return -1;
    if (g_test_mode0_field_scene && bg >= 1 && bg <= 3 &&
        (output_x < 24 || output_x >= 264)) return -1;
    return 0;
}

unsigned test_margin_policy(uint16_t dispcnt, const uint8_t* io) {
    ++g_margin_policy_calls;
    if (!io || (dispcnt & 0x0080u) != 0 || (dispcnt & 0xE000u) != 0) {
        return gba::kWsMarginPillarboxBoth;
    }
    const auto read_io16 = [&](std::size_t offset) {
        return static_cast<uint16_t>(io[offset]) |
               static_cast<uint16_t>(io[offset + 1u] << 8);
    };
    if ((dispcnt & 0x0007u) == 2u) {
        constexpr uint16_t required = 0xA000u;  // wrap + 512x512.
        return (dispcnt & 0x0C00u) == 0x0C00u &&
                       (read_io16(0x0C) & 0xE000u) == required &&
                       (read_io16(0x0E) & 0xE000u) == required
                   ? 0u
                   : gba::kWsMarginPillarboxBoth;
    }
    if ((dispcnt & 0x0007u) == 0u) {
        if ((dispcnt & 0x0E00u) != 0x0E00u) {
            return gba::kWsMarginPillarboxBoth;
        }
        return read_io16(0x14) == read_io16(0x18) &&
                       read_io16(0x14) == read_io16(0x1C) &&
                       read_io16(0x16) == read_io16(0x1A) &&
                       read_io16(0x16) == read_io16(0x1E)
                   ? 0u
                   : gba::kWsMarginPillarboxBoth;
    }
    return gba::kWsMarginPillarboxBoth;
}

void test_alpha_native_domain_and_green_precision() {
    Fixture f;
    // Mode 0 BG0, 256-color tile 0 at character base 0, map at 0x800.
    const uint16_t dispcnt = 0x0100;
    store16(&f.io[0x08], 0x0180); // 256 colors, screen base block 1.
    f.vram[0] = 1;                // First pixel uses palette entry 1.
    store16(&f.vram[0x800], 0);   // Tile-map entry 0.

    // Top: RGB5(10,20,5), hidden green low bit set. Bottom: RGB5(20,5,25).
    store16(&f.pal[2], static_cast<uint16_t>(
        0x8000 | (5 << 10) | (20 << 5) | 10));
    store16(&f.pal[0], static_cast<uint16_t>(
        (25 << 10) | (5 << 5) | 20));
    store16(&f.io[0x50], 0x2041); // BG0 first, alpha, backdrop second.
    store16(&f.io[0x52], 0x100B); // EVA=11, EVB=16.

    f.ppu.render(f.rgb.data(), dispcnt, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    // Native-domain result is RGB5(27,19,28), expanded after blending.
    expect_pixel(f.rgb.data(), 222, 156, 231, "alpha native-domain rounding");
}

void test_brightness_native_domain_and_green_precision() {
    Fixture f;
    const uint16_t source = static_cast<uint16_t>(
        0x8000 | (3 << 10) | (10 << 5) | 5);
    store16(&f.pal[0], source);
    store16(&f.io[0x54], 7);

    // Backdrop first target, brighten effect. Native result RGB5(16,19,15).
    store16(&f.io[0x50], 0x00A0);
    f.ppu.render(f.rgb.data(), 0, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(f.rgb.data(), 132, 156, 123, "brighten native-domain rounding");

    // Same source and coefficient, darken effect. Native result RGB5(3,6,2).
    store16(&f.io[0x50], 0x00E0);
    f.ppu.render(f.rgb.data(), 0, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(f.rgb.data(), 24, 49, 16, "darken native-domain rounding");
}

void test_hblank_affine_reference_reload() {
    Fixture f;
    // Mode 1 BG2: make texture row 0 red and row 1 blue. A HBlank write of
    // BG2Y=0 must make the next line begin at row 0 even with PD=1.0; the old
    // renderer wrongly added screen_y*PD again and sampled row 1.
    const uint16_t dispcnt = 0x0401;
    store16(&f.io[0x0C], 0x0100); // screen base block 1, 128x128.
    store16(&f.io[0x20], 0x0100); // PA=1.0
    store16(&f.io[0x26], 0x0100); // PD=1.0
    f.vram[0x800] = 0;
    for (unsigned x = 0; x < 8; ++x) {
        f.vram[x] = 1;
        f.vram[8 + x] = 2;
    }
    store16(&f.pal[2], 0x001F); // red
    store16(&f.pal[4], 0x7C00); // blue

    f.ppu.reload_affine_references(f.io.data());
    f.ppu.reload_affine_reference(2, true, 0);
    f.ppu.render_scanline(1, dispcnt, f.io.data(), f.vram.data(),
                          f.oam.data(), f.pal.data());
    const uint8_t* line1 = f.ppu.latched_framebuffer() +
        gba::GbaPpu::kScreenWidth * 3u;
    expect_pixel(line1, 255, 0, 0, "HBlank affine reference reload");
}

void test_affine_hidden_state_roundtrip() {
    Fixture f;
    const uint16_t dispcnt = 0x0401;
    store16(&f.io[0x0C], 0x0100);
    store16(&f.io[0x20], 0x0100);
    store16(&f.io[0x26], 0x0100);
    f.vram[0x800] = 0;
    std::fill_n(&f.vram[0], 8, 1);
    std::fill_n(&f.vram[8], 8, 2);
    store16(&f.pal[2], 0x001F);
    store16(&f.pal[4], 0x7C00);

    f.ppu.reload_affine_references(f.io.data());
    f.ppu.render_scanline(0, dispcnt, f.io.data(), f.vram.data(),
                          f.oam.data(), f.pal.data()); // hidden Y advances to 1
    gbarecomp::debug::SnapshotWriter writer;
    f.ppu.serialize_affine_state(writer);

    gba::GbaPpu restored;
    gbarecomp::debug::SnapshotReader reader(writer.buffer().data(),
                                             writer.size());
    restored.deserialize_affine_state(reader);
    if (!reader.ok() || reader.remaining() != 0) {
        std::fprintf(stderr, "affine hidden-state snapshot did not roundtrip\n");
        std::exit(1);
    }
    restored.render_scanline(5, dispcnt, f.io.data(), f.vram.data(),
                             f.oam.data(), f.pal.data());
    const uint8_t* line5 = restored.latched_framebuffer() +
        5u * gba::GbaPpu::kScreenWidth * 3u;
    expect_pixel(line5, 0, 0, 255, "affine hidden-state roundtrip");
}

void test_native_scene_supersampling() {
    Fixture f;
    // A deterministic 256-colour BG0 with a red/blue checker at hardware
    // pixel granularity. Native output must keep each logical sample crisp,
    // while the affine-capable compositor is exercised at a wider stride.
    const uint16_t dispcnt = 0x0100;
    store16(&f.io[0x08], 0x0180); // BG0 256-colour, screen base 1.
    store16(&f.vram[0x800], 0);
    for (unsigned i = 0; i < 64; ++i)
        f.vram[i] = static_cast<uint8_t>((i & 1u) ? 2u : 1u);
    store16(&f.pal[0], 0);
    store16(&f.pal[2], 0x001F); // red
    store16(&f.pal[4], 0x7C00); // blue

    std::vector<uint8_t> native(
        static_cast<std::size_t>(gba::GbaPpu::kScreenWidth * 2u) *
        (gba::GbaPpu::kScreenHeight * 2u) * 3u, 0);
    f.ppu.render_native(native.data(), 2, dispcnt, f.io.data(), f.vram.data(),
                        f.oam.data(), f.pal.data());
    const std::size_t native_stride =
        static_cast<std::size_t>(gba::GbaPpu::kScreenWidth * 2u) * 3u;
    // Every pair of native samples maps to one logical source pixel for a
    // regular BG. Check two distinct source colours and both row/column
    // bounds, which also catches a 960px compositor-buffer overrun.
    const uint8_t* p0 = native.data() + 0 * 3u;
    const uint8_t* p1 = native.data() + 1 * 3u;
    const uint8_t* p2 = native.data() + 2 * 3u;
    expect_pixel(p0, 255, 0, 0, "native BG sample 0");
    expect_pixel(p1, 255, 0, 0, "native BG horizontal duplicate");
    expect_pixel(p2, 0, 0, 255, "native BG sample 1");
    for (unsigned y = 0; y < 8; ++y) {
        for (unsigned x = 0; x < 8; ++x)
            f.vram[y * 8u + x] = static_cast<uint8_t>((y & 1u) ? 2u : 1u);
    }
    f.ppu.render_native(native.data(), 2, dispcnt, f.io.data(), f.vram.data(),
                        f.oam.data(), f.pal.data());
    expect_pixel(native.data(), 255, 0, 0, "native BG vertical sample 0");
    expect_pixel(native.data() + native_stride, 255, 0, 0,
                 "native BG vertical duplicate");
    expect_pixel(native.data() + 2u * native_stride, 0, 0, 255,
                 "native BG vertical sample 1");
    f.ppu.latch_native_scene_state(dispcnt, f.io.data(), f.vram.data(),
                                   f.oam.data(), f.pal.data());
    f.vram[0] = 2; // Live memory changes after the VBlank boundary.
    std::fill(native.begin(), native.end(), 0);
    if (!f.ppu.render_native_latched(native.data(), 2)) {
        std::fprintf(stderr, "native latch was not available\n");
        std::exit(1);
    }
    expect_pixel(native.data(), 255, 0, 0,
                 "native render ignored VBlank-latched source");
    const uint8_t* last = native.data() +
        (gba::GbaPpu::kScreenHeight * 2u - 1u) * native_stride +
        (gba::GbaPpu::kScreenWidth * 2u - 1u) * 3u;
    expect_pixel(last, 0, 0, 255, "native BG bottom-right");

    // Scale 4 uses the separate native/compositor width envelope rather than
    // widening the established 480px view limit.
    std::vector<uint8_t> native4(
        static_cast<std::size_t>(gba::GbaPpu::kScreenWidth * 4u) *
        (gba::GbaPpu::kScreenHeight * 4u) * 3u, 0);
    f.ppu.render_native(native4.data(), 4, dispcnt, f.io.data(), f.vram.data(),
                        f.oam.data(), f.pal.data());
    const uint8_t* last4 = native4.data() +
        (static_cast<std::size_t>(gba::GbaPpu::kScreenHeight * 4u) - 1u) *
            gba::GbaPpu::kScreenWidth * 4u * 3u +
        (gba::GbaPpu::kScreenWidth * 4u - 1u) * 3u;
    expect_pixel(last4, 0, 0, 255, "native 4x BG bottom-right");

    std::vector<uint8_t> native10(
        static_cast<std::size_t>(gba::GbaPpu::kScreenWidth * 10u) *
        (gba::GbaPpu::kScreenHeight * 10u) * 3u, 0);
    f.ppu.render_native(native10.data(), 10, dispcnt, f.io.data(), f.vram.data(),
                        f.oam.data(), f.pal.data());
    const uint8_t* last10 = native10.data() +
        (static_cast<std::size_t>(gba::GbaPpu::kScreenHeight * 10u) - 1u) *
            gba::GbaPpu::kScreenWidth * 10u * 3u +
        (gba::GbaPpu::kScreenWidth * 10u - 1u) * 3u;
    expect_pixel(last10, 0, 0, 255, "native 10x BG bottom-right");

    // Preserve per-scanline register effects in the native snapshot. A
    // final-state-only compositor would apply line 1's darken setting to both
    // lines and fail this check.
    Fixture raster;
    store16(&raster.pal[0], 0x001F); // red backdrop
    store16(&raster.io[0x50], 0x00A0); // brighten backdrop
    store16(&raster.io[0x54], 16);
    raster.ppu.render_scanline(0, 0, raster.io.data(), raster.vram.data(),
                               raster.oam.data(), raster.pal.data());
    store16(&raster.io[0x50], 0x00E0); // darken backdrop
    raster.ppu.render_scanline(1, 0, raster.io.data(), raster.vram.data(),
                               raster.oam.data(), raster.pal.data());
    raster.ppu.latch_native_scene_state(0, raster.io.data(),
                                        raster.vram.data(), raster.oam.data(),
                                        raster.pal.data());
    std::vector<uint8_t> raster_native(
        static_cast<std::size_t>(gba::GbaPpu::kScreenWidth * 2u) *
        (gba::GbaPpu::kScreenHeight * 2u) * 3u, 0);
    if (!raster.ppu.render_native_latched(raster_native.data(), 2)) {
        std::fprintf(stderr, "native raster latch was not available\n");
        std::exit(1);
    }
    expect_pixel(raster_native.data(), 255, 255, 255,
                 "native per-line register row 0");
    expect_pixel(raster_native.data() + 2u * native_stride, 0, 0, 0,
                 "native per-line register row 1");
}

void test_extended_view_geometry_and_clamp() {
    gba::GbaPpu ppu;
    ppu.set_view_margins(24, 24, 0, 0);
    if (ppu.render_width() != 288 || ppu.render_height() != 160 ||
        ppu.view_extra_left() != 24 || ppu.view_extra_right() != 24) {
        std::fprintf(stderr, "extended-view 288x160 geometry mismatch\n");
        std::exit(1);
    }

    ppu.set_view_margins(72, 72, 0, 0);
    if (ppu.render_width() != 384 || ppu.view_extra_left() != 72 ||
        ppu.view_extra_right() != 72) {
        std::fprintf(stderr, "extended-view 384x160 geometry mismatch\n");
        std::exit(1);
    }

    ppu.set_view_margins(120, 120, 0, 0);
    if (ppu.render_width() != 480 ||
        ppu.render_width() != gba::GbaPpu::kMaxRenderWidth) {
        std::fprintf(stderr, "extended-view 480x160 capacity mismatch\n");
        std::exit(1);
    }

    ppu.set_view_margins(1000, 1000, 1000, 1000);
    if (ppu.render_width() != gba::GbaPpu::kMaxRenderWidth ||
        ppu.render_height() != gba::GbaPpu::kMaxRenderHeight ||
        ppu.view_extra_top() != gba::GbaPpu::kMaxExtraY ||
        ppu.view_extra_bottom() != gba::GbaPpu::kMaxExtraY) {
        std::fprintf(stderr, "extended-view clamp mismatch\n");
        std::exit(1);
    }

    ppu.set_view_margins(60, 60, 40, 40);
    if (ppu.render_width() != 360 || ppu.render_height() != 240 ||
        ppu.view_extra_left() != 60 || ppu.view_extra_right() != 60 ||
        ppu.view_extra_top() != 40 || ppu.view_extra_bottom() != 40) {
        std::fprintf(stderr, "extended-view 360x240 geometry mismatch\n");
        std::exit(1);
    }
}

void test_extended_view_capability_policy() {
    using gbarecomp::resolve_view_geometry;
    constexpr uint32_t kEngineMax = gba::GbaPpu::kMaxRenderWidth;

    auto g = resolve_view_geometry(288, 240, false, kEngineMax);
    if (g.width != 240 || g.extra_left != 0 || g.extra_right != 0) {
        std::fprintf(stderr, "unsupported extended view was not inert\n");
        std::exit(1);
    }
    g = resolve_view_geometry(288, 320, false, kEngineMax);
    if (g.width != 288 || g.extra_left != 24 || g.extra_right != 24) {
        std::fprintf(stderr, "opted-in 288x160 geometry mismatch\n");
        std::exit(1);
    }
    g = resolve_view_geometry(368, 320, false, kEngineMax);
    if (g.width != 320 || g.extra_left != 40 || g.extra_right != 40) {
        std::fprintf(stderr, "per-game maximum was not enforced\n");
        std::exit(1);
    }
    g = resolve_view_geometry(384, 480, false, kEngineMax);
    if (g.width != 384 || g.extra_left != 72 || g.extra_right != 72) {
        std::fprintf(stderr, "opted-in 384x160 geometry mismatch\n");
        std::exit(1);
    }
    g = resolve_view_geometry(480, 480, false, kEngineMax);
    if (g.width != 480 || g.extra_left != 120 || g.extra_right != 120) {
        std::fprintf(stderr, "opted-in 480x160 geometry mismatch\n");
        std::exit(1);
    }
    g = resolve_view_geometry(600, 600, false, kEngineMax);
    if (g.width != 480 || g.extra_left != 120 || g.extra_right != 120) {
        std::fprintf(stderr, "480x160 engine capacity was not enforced\n");
        std::exit(1);
    }
    g = resolve_view_geometry(288, 240, true, kEngineMax);
    if (g.width != 288) {
        std::fprintf(stderr, "development override did not bypass capability\n");
        std::exit(1);
    }
    g = resolve_view_geometry(285, 320, false, kEngineMax);
    if (g.width != 285 || g.extra_left != 22 || g.extra_right != 23) {
        std::fprintf(stderr, "odd extended-view split mismatch\n");
        std::exit(1);
    }

    const int legacy_extra[] = {0, 20, 22, 24, 40, 72, 120};
    const int expected_width[] = {240, 280, 284, 288, 320, 384, 480};
    for (std::size_t i = 0; i < std::size(legacy_extra); ++i) {
        int width = 0;
        if (!gbarecomp::legacy_extra_to_view_width(legacy_extra[i], &width) ||
            width != expected_width[i]) {
            std::fprintf(stderr, "legacy widescreen conversion mismatch\n");
            std::exit(1);
        }
    }
    int ignored = 0;
    if (gbarecomp::legacy_extra_to_view_width(-1, &ignored) ||
        gbarecomp::legacy_extra_to_view_width(
            std::numeric_limits<int>::max(), &ignored)) {
        std::fprintf(stderr, "legacy widescreen overflow was accepted\n");
        std::exit(1);
    }
}

void test_resize_driven_view_policy() {
    using gbarecomp::resize_driven_view_width;
    struct Case { int w; int h; uint32_t max; uint32_t expected; };
    const Case cases[] = {
        {720, 480, 480, 240}, {3440, 1440, 480, 382},
        {2560, 1080, 480, 379}, {1920, 1080, 320, 284},
        {800, 1200, 480, 240}, {10000, 1000, 480, 480},
        {0, 0, 480, 240},
    };
    for (const Case& c : cases) {
        if (resize_driven_view_width(c.w, c.h, c.max, 480) != c.expected) {
            std::fprintf(stderr, "resize-driven view policy mismatch\n");
            std::exit(1);
        }
    }
}

void test_extended_view_preserves_authentic_center() {
    Fixture f;
    const uint16_t dispcnt = 0x0100;  // Mode 0, BG0.
    store16(&f.io[0x08], 0x0180);     // 256 colors, screen block 1.
    store16(&f.io[0x10], 13);         // Non-tile-aligned horizontal scroll.
    store16(&f.io[0x12], 5);          // Non-tile-aligned vertical scroll.

    // Give every texel and palette entry a deterministic nontrivial value so
    // the comparison covers tile selection, scrolling, and RGB conversion.
    for (std::size_t i = 0; i < 64; ++i) f.vram[i] = static_cast<uint8_t>(i + 1);
    for (std::size_t i = 0; i < 32u * 32u; ++i)
        store16(&f.vram[0x800 + i * 2], static_cast<uint16_t>(i & 1u));
    for (unsigned i = 1; i < 256; ++i)
        store16(&f.pal[i * 2], static_cast<uint16_t>(
            (i & 31u) | (((i * 3u) & 31u) << 5) | (((i * 7u) & 31u) << 10)));

    std::vector<uint8_t> authentic(gba::GbaPpu::kFramebufferBytes, 0);
    f.ppu.render(authentic.data(), dispcnt, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());

    std::vector<uint8_t> wide(gba::GbaPpu::kMaxFramebufferBytes, 0);
    const std::size_t authentic_stride = gba::GbaPpu::kScreenWidth * 3u;
    for (const uint32_t extra : {24u, 72u, 120u}) {
        f.ppu.set_view_margins(extra, extra, 0, 0);
        std::fill(wide.begin(), wide.end(), 0);
        f.ppu.render(wide.data(), dispcnt, f.io.data(), f.vram.data(),
                     f.oam.data(), f.pal.data());
        const std::size_t wide_stride = f.ppu.render_width() * 3u;
        for (uint32_t y = 0; y < gba::GbaPpu::kScreenHeight; ++y) {
            const uint8_t* got = wide.data() +
                y * wide_stride + extra * 3u;
            const uint8_t* expected = authentic.data() + y * authentic_stride;
            if (std::memcmp(got, expected, authentic_stride) != 0) {
                std::fprintf(stderr,
                             "extended-view %ux160 center differs from "
                             "authentic row %u\n",
                             f.ppu.render_width(), y);
                std::exit(1);
            }
        }
    }
}

void test_expanded_view_vertical_ppu_paths() {
    Fixture f;
    constexpr uint16_t bg_dispcnt = 0x0100u;
    store16(&f.io[0x08], 0x0180u); // BG0: 256-colour, screen block 1.

    // Give each map row a distinct 8bpp tile/palette entry. This makes signed
    // logical-Y sampling in the top/bottom margins observable without any
    // game-specific provider.
    for (uint32_t row = 0; row < 32; ++row) {
        const uint8_t tile = static_cast<uint8_t>(row + 1u);
        std::fill_n(f.vram.data() + static_cast<std::size_t>(tile) * 64u,
                    64u, tile);
        for (uint32_t col = 0; col < 32; ++col)
            store16(&f.vram[0x800u + (row * 32u + col) * 2u], tile);
    }
    for (uint32_t row = 0; row < 32; ++row) {
        const uint16_t color = row == 27u ? 0x001Fu :
            row == 24u ? 0x03E0u :
            static_cast<uint16_t>((row + 1u) & 31u);
        store16(&f.pal[(row + 1u) * 2u], color);
    }

    std::vector<uint8_t> authentic(gba::GbaPpu::kFramebufferBytes, 0);
    f.ppu.set_view_margins(0, 0, 0, 0);
    f.ppu.render(authentic.data(), bg_dispcnt, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());

    f.ppu.set_view_margins(60, 60, 40, 40);
    std::vector<uint8_t> wide(gba::GbaPpu::kMaxFramebufferBytes, 0);
    f.ppu.render(wide.data(), bg_dispcnt, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    const std::size_t wide_stride =
        static_cast<std::size_t>(f.ppu.render_width()) * 3u;
    for (uint32_t y = 0; y < gba::GbaPpu::kScreenHeight; ++y) {
        const uint8_t* got = wide.data() +
            (static_cast<std::size_t>(y) + 40u) * wide_stride + 60u * 3u;
        const uint8_t* expected = authentic.data() +
            static_cast<std::size_t>(y) * gba::GbaPpu::kScreenWidth * 3u;
        if (std::memcmp(got, expected,
                        gba::GbaPpu::kScreenWidth * 3u) != 0) {
            std::fprintf(stderr, "vertical expanded center changed at row %u\n", y);
            std::exit(1);
        }
    }
    expect_pixel(wide.data() + 60u * 3u, 255, 0, 0,
                 "top vertical margin did not use signed BG Y");
    expect_pixel(wide.data() + 239u * wide_stride + 60u * 3u,
                 0, 255, 0, "bottom vertical margin did not use signed BG Y");

    // Exercise the runtime's one-scanline-at-a-time path. The VBlank snapshot
    // must fill only the host rows outside the 160 authentic lines.
    f.ppu.reset();
    f.ppu.set_view_margins(60, 60, 40, 40);
    for (uint32_t y = 0; y < gba::GbaPpu::kScreenHeight; ++y)
        f.ppu.render_scanline(y, bg_dispcnt, f.io.data(), f.vram.data(),
                              f.oam.data(), f.pal.data());
    f.ppu.latch_native_scene_state(bg_dispcnt, f.io.data(), f.vram.data(),
                                   f.oam.data(), f.pal.data());
    f.ppu.mark_framebuffer_latched();
    const uint8_t* latched = f.ppu.latched_framebuffer();
    expect_pixel(latched + 60u * 3u, 255, 0, 0,
                 "latched top vertical margin missing");
    expect_pixel(latched + 239u * wide_stride + 60u * 3u,
                 0, 255, 0, "latched bottom vertical margin missing");

    // OAM Y remains hardware-signed, but is evaluated against signed logical
    // rows so sprites can cross either newly visible edge.
    f.ppu.reset();
    f.ppu.set_view_margins(60, 60, 40, 40);
    for (std::size_t i = 0; i < 128; ++i)
        store16(&f.oam[i * 8u], 0x0200u); // Disable every OBJ.
    std::fill_n(f.vram.data() + 0x10000u, 2048u, static_cast<uint8_t>(0x11));
    store16(&f.pal[0x202], 0x001Fu); // OBJ palette index 1 = red.
    store16(&f.oam[2], static_cast<uint16_t>(3u << 14)); // 64x64 at X=0.
    store16(&f.oam[4], 0u);
    store16(&f.oam[0], 224u); // Hardware Y=-32; line 0 is output row 8.
    f.ppu.render(wide.data(), 0x1040u, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(wide.data() + 8u * wide_stride + 60u * 3u, 255, 0, 0,
                 "top vertical margin did not render signed OBJ Y");
    store16(&f.oam[0], 150u); // 64px sprite reaches logical Y=199.
    std::fill(wide.begin(), wide.end(), 0);
    f.ppu.render(wide.data(), 0x1040u, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(wide.data() + 239u * wide_stride + 60u * 3u,
                 255, 0, 0, "bottom vertical margin did not render OBJ Y");
}

void test_expanded_view_vertical_tilemap_provider() {
    Fixture f;
    constexpr uint16_t dispcnt = 0x0100u;
    store16(&f.io[0x08], 0x0180u); // BG0: 256-colour, screen block 1.
    std::fill_n(f.vram.begin(), 64u, static_cast<uint8_t>(1));
    std::fill_n(f.vram.begin() + 64u, 64u, static_cast<uint8_t>(2));
    for (uint32_t i = 0; i < 32u * 32u; ++i)
        store16(&f.vram[0x800u + i * 2u], 0u);
    store16(&f.pal[0], 0x0000u); // Unavailable samples expose backdrop.
    store16(&f.pal[2], 0x001Fu); // Wrapped tile 0 = red.
    store16(&f.pal[4], 0x7C00u); // Provider tile 1 = blue.

    f.ppu.set_view_margins(0, 0, 40, 40);
    std::vector<uint8_t> wide(gba::GbaPpu::kMaxFramebufferBytes, 0);
    gba::g_ws_margin_policy = nullptr;
    gba::g_ws_pillarbox = 0;
    gba::g_ws_pillarbox_top = 0;
    gba::g_ws_pillarbox_bottom = 0;
    gba::g_ws_tilemap_provider = test_vertical_tilemap;
    g_vertical_tilemap_calls = 0;
    g_vertical_tilemap_vertical_calls = 0;
    g_vertical_tilemap_action = gba::kWsTilemapReplace;
    f.ppu.render(wide.data(), dispcnt, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    if (g_vertical_tilemap_calls == 0 ||
        g_vertical_tilemap_vertical_calls == 0) {
        std::fprintf(stderr, "vertical tilemap provider was not queried\n");
        std::exit(1);
    }
    const std::size_t stride = static_cast<std::size_t>(f.ppu.render_width()) * 3u;
    expect_pixel(wide.data(), 0, 0, 255,
                 "vertical tilemap replacement missing at top");
    expect_pixel(wide.data() + 40u * stride, 255, 0, 0,
                 "vertical tilemap provider changed native center");
    expect_pixel(wide.data() + 239u * stride, 0, 0, 255,
                 "vertical tilemap replacement missing at bottom");

    // An unavailable provider must fail closed instead of drawing the wrapped
    // native map seam in either synthetic edge region.
    g_vertical_tilemap_action = gba::kWsTilemapUnavailable;
    store16(&f.pal[0], 0x03E0u); // Green backdrop makes fallback visible.
    std::fill(wide.begin(), wide.end(), 0);
    f.ppu.render(wide.data(), dispcnt, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(wide.data(), 0, 255, 0,
                 "unavailable top tilemap provider leaked wrapped map");
    expect_pixel(wide.data() + 239u * stride, 0, 255, 0,
                 "unavailable bottom tilemap provider leaked wrapped map");
    expect_pixel(wide.data() + 40u * stride, 255, 0, 0,
                 "unavailable tilemap provider changed native center");

    g_vertical_tilemap_action = gba::kWsTilemapReplace;
    gba::g_ws_tilemap_provider = nullptr;
}

void test_expanded_view_obj_y_provider() {
    Fixture f;
    for (std::size_t i = 0; i < 128; ++i)
        store16(&f.oam[i * 8u], 0x0200u); // Disable every OBJ.
    store16(&f.oam[2], static_cast<uint16_t>(3u << 14)); // 64x64 at X=0.
    store16(&f.oam[4], 0u);
    std::fill_n(f.vram.begin() + 0x10000u, 2048u, static_cast<uint8_t>(0x11));
    store16(&f.pal[0x202], 0x001Fu); // OBJ palette index 1 = red.
    store16(&f.oam[0], 180u); // Raw Y=180 is hardware-signed Y=-76.
    constexpr uint16_t dispcnt = 0x1040u; // OBJ + 1D tile mapping.

    f.ppu.set_view_margins(0, 0, 40, 40);
    std::vector<uint8_t> wide(gba::GbaPpu::kMaxFramebufferBytes, 0);
    gba::g_ws_obj_y_provider = nullptr;
    g_obj_y_provider_calls = 0;
    f.ppu.render(wide.data(), dispcnt, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    const std::size_t stride = static_cast<std::size_t>(f.ppu.render_width()) * 3u;
    expect_pixel(wide.data() + 220u * stride, 0, 0, 0,
                 "raw OBJ Y unexpectedly escaped signed hardware range");
    if (g_obj_y_provider_calls != 0) {
        std::fprintf(stderr, "OBJ Y provider ran while uninstalled\n");
        std::exit(1);
    }

    gba::g_ws_obj_y_provider = test_obj_y_provider;
    std::fill(wide.begin(), wide.end(), 0);
    f.ppu.render(wide.data(), dispcnt, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    if (g_obj_y_provider_calls == 0) {
        std::fprintf(stderr, "expanded renderer skipped OBJ Y provider\n");
        std::exit(1);
    }
    expect_pixel(wide.data() + 220u * stride, 255, 0, 0,
                 "OBJ Y provider did not expose positive raw Y");

    // The faithful native path remains hardware-signed and never calls the
    // expanded-only provider.
    const int calls_before_native = g_obj_y_provider_calls;
    f.ppu.set_view_margins(0, 0, 0, 0);
    std::fill(f.rgb.begin(), f.rgb.end(), 0);
    f.ppu.render(f.rgb.data(), dispcnt, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    if (g_obj_y_provider_calls != calls_before_native) {
        std::fprintf(stderr, "native renderer called OBJ Y provider\n");
        std::exit(1);
    }
    expect_pixel(f.rgb.data(), 0, 0, 0,
                 "native renderer accepted expanded OBJ Y");

    gba::g_ws_obj_y_provider = nullptr;
}

void test_expanded_margin_policy_pillarboxes_rejected_scene() {
    Fixture f;
    // Synthetic Mode 2 scene: both affine layers use wrapped 512x512 maps.
    const uint16_t dispcnt = 0x0C02u;
    const uint16_t bgcnt2 = 0xA100u;       // wrap + 512x512, priority 0.
    const uint16_t bgcnt3 = 0xA101u;       // wrap + 512x512, priority 1.
    store16(&f.io[0x0C], bgcnt2);
    store16(&f.io[0x0E], bgcnt3);
    store16(&f.io[0x20], 0x0100u);         // BG2 PA = 1.0.
    store16(&f.io[0x26], 0x0100u);         // BG2 PD = 1.0.
    store16(&f.io[0x30], 0x0100u);         // BG3 PA = 1.0.
    store16(&f.io[0x36], 0x0100u);         // BG3 PD = 1.0.

    // Affine maps are 64x64 bytes. Put their entries in screen block 1 and
    // make tile 1 an opaque red tile. The green backdrop makes an accidental
    // non-black margin obvious when the policy rejects the scene.
    std::fill_n(&f.vram[0x800], 4096, static_cast<uint8_t>(1));
    std::fill_n(&f.vram[64], 64, static_cast<uint8_t>(1));
    store16(&f.pal[0], 0x03E0u);
    store16(&f.pal[2], 0x001Fu);

    std::vector<uint8_t> authentic(gba::GbaPpu::kFramebufferBytes, 0);
    f.ppu.set_view_margins(0, 0, 0, 0);
    f.ppu.render(authentic.data(), dispcnt, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());

    gba::g_ws_margin_policy = test_margin_policy;
    g_margin_policy_calls = 0;
    f.ppu.set_view_margins(24, 24, 0, 0);
    std::vector<uint8_t> wide(gba::GbaPpu::kMaxFramebufferBytes, 0);

    // A wide snapshot does not carry presentation margins. Loading it
    // therefore sets the legacy flag as a conservative interim default. The
    // installed per-scanline policy must still be authoritative: this valid
    // field frame has a zero policy result and must reopen both margins.
    gbarecomp::debug::SnapshotWriter stale_state;
    f.ppu.serialize(stale_state);
    gbarecomp::debug::SnapshotReader stale_reader(
        stale_state.buffer().data(), stale_state.size());
    f.ppu.deserialize(stale_reader);
    if (!stale_reader.ok() || stale_reader.remaining() != 0 ||
        gba::g_ws_pillarbox != 1) {
        std::fprintf(stderr,
                     "wide save/load did not install stale legacy pillarbox\n");
        std::exit(1);
    }
    f.ppu.render(wide.data(), dispcnt, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    if (g_margin_policy_calls != gba::GbaPpu::kScreenHeight) {
        std::fprintf(stderr, "margin policy was not called per expanded scanline\n");
        std::exit(1);
    }
    expect_pixel(wide.data(), 255, 0, 0, "valid expanded left margin");
    expect_pixel(wide.data() + 287u * 3u, 255, 0, 0,
                 "valid expanded right margin");
    for (uint32_t y = 0; y < gba::GbaPpu::kScreenHeight; ++y) {
        const uint8_t* got = wide.data() +
            (static_cast<std::size_t>(y) * f.ppu.render_width() + 24u) * 3u;
        const uint8_t* expected = authentic.data() +
            static_cast<std::size_t>(y) * gba::GbaPpu::kScreenWidth * 3u;
        if (std::memcmp(got, expected,
                        gba::GbaPpu::kScreenWidth * 3u) != 0) {
            std::fprintf(stderr,
                         "valid expanded policy changed center row %u\n", y);
            std::exit(1);
        }
    }

    // Break the measured configuration while leaving BG2's visible center
    // layer intact. The policy must black both margins and preserve center.
    store16(&f.io[0x0E], 0x6101u);         // BG3 wraps at 256x256, not 512.
    g_margin_policy_calls = 0;
    std::fill(wide.begin(), wide.end(), 0);
    f.ppu.render(wide.data(), dispcnt, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    if (g_margin_policy_calls != gba::GbaPpu::kScreenHeight) {
        std::fprintf(stderr, "rejected margin policy call count mismatch\n");
        std::exit(1);
    }
    expect_pixel(wide.data(), 0, 0, 0, "rejected expanded left margin");
    expect_pixel(wide.data() + 287u * 3u, 0, 0, 0,
                 "rejected expanded right margin");
    for (uint32_t y = 0; y < gba::GbaPpu::kScreenHeight; ++y) {
        const uint8_t* got = wide.data() +
            (static_cast<std::size_t>(y) * f.ppu.render_width() + 24u) * 3u;
        const uint8_t* expected = authentic.data() +
            static_cast<std::size_t>(y) * gba::GbaPpu::kScreenWidth * 3u;
        if (std::memcmp(got, expected,
                        gba::GbaPpu::kScreenWidth * 3u) != 0) {
            std::fprintf(stderr,
                         "rejected expanded policy changed center row %u\n", y);
            std::exit(1);
        }
    }
    // Null callback retains the historical global-flag behavior.
    gba::g_ws_margin_policy = nullptr;
    gba::g_ws_pillarbox = 1;
    std::fill(wide.begin(), wide.end(), 0);
    f.ppu.render(wide.data(), dispcnt, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(wide.data(), 0, 0, 0, "null-policy legacy left margin");
    expect_pixel(wide.data() + 287u * 3u, 0, 0, 0,
                 "null-policy legacy right margin");
    gba::g_ws_pillarbox = 0;
    gba::g_ws_pillarbox_left = 0;
    gba::g_ws_pillarbox_right = 0;
}

void test_expanded_mode0_field_policy_and_margins() {
    Fixture f;
    // Synthetic Mode 0 town/dungeon field: BG1/BG2/BG3 all use the same
    // populated 256x256 text layer and the measured equal scroll pairs.
    const uint16_t dispcnt = 0x0F00u; // BG0 + BG1/BG2/BG3 enabled.
    constexpr uint16_t bgcnt = 0x0180u; // 256-colour, screen block 1.
    constexpr uint16_t bg0cnt = 0x0284u; // BG0: char base 1, map block 2.
    store16(&f.io[0x08], bg0cnt);         // BG0CNT.
    store16(&f.io[0x0A], bgcnt);         // BG1CNT.
    store16(&f.io[0x0C], bgcnt);         // BG2CNT.
    store16(&f.io[0x0E], bgcnt);         // BG3CNT.
    store16(&f.io[0x14], 0u);
    store16(&f.io[0x16], 0u);
    store16(&f.io[0x18], 0u);
    store16(&f.io[0x1A], 0u);
    store16(&f.io[0x1C], 0u);
    store16(&f.io[0x1E], 0u);

    std::fill_n(f.vram.begin(), 64, static_cast<uint8_t>(1));
    std::fill_n(f.vram.begin() + 64, 64, static_cast<uint8_t>(2));
    std::fill_n(f.vram.begin() + 128, 64, static_cast<uint8_t>(3));
    for (unsigned row = 0; row < 32; ++row) {
        for (unsigned col = 0; col < 32; ++col) {
            const uint16_t tile = col == 30 ? 1u : (col == 31 ? 2u : 0u);
            store16(&f.vram[0x800 + (row * 32u + col) * 2u], tile);
        }
    }
    std::fill_n(f.vram.begin() + 0x4000, 64, static_cast<uint8_t>(4));
    store16(&f.pal[0], 0x0000u); // Black backdrop exposes cut-off margins.
    store16(&f.pal[2], 0x001Fu); // Central field tile: red.
    store16(&f.pal[4], 0x03E0u); // Untrusted ring tile: green.
    store16(&f.pal[6], 0x03FFu); // Untrusted ring tile: yellow.
    store16(&f.pal[8], 0x7C00u); // Distinct opaque blue BG0 tile.

    std::vector<uint8_t> authentic(gba::GbaPpu::kFramebufferBytes, 0);
    f.ppu.set_view_margins(0, 0, 0, 0);
    f.ppu.render(authentic.data(), dispcnt, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());

    gba::g_ws_margin_policy = test_margin_policy;
    gba::g_ws_bg_x_provider = test_field_bg_x_provider;
    gba::g_ws_bg_x_provider_layers = 0xFu; // BG0 + BG1/BG2/BG3.
    g_test_mode0_field_scene = true;
    gba::g_ws_pillarbox = 0;
    gba::g_ws_pillarbox_left = 0;
    gba::g_ws_pillarbox_right = 0;
    f.ppu.set_view_margins(24, 24, 0, 0);
    std::vector<uint8_t> wide(gba::GbaPpu::kMaxFramebufferBytes, 0);
    g_margin_policy_calls = 0;

    // Loading a wide snapshot leaves the legacy pillarbox flag set until the
    // next policy call. The field policy and BG0 suppression must both recover
    // on that first render.
    gbarecomp::debug::SnapshotWriter field_state;
    f.ppu.serialize(field_state);
    gbarecomp::debug::SnapshotReader field_reader(
        field_state.buffer().data(), field_state.size());
    f.ppu.deserialize(field_reader);
    if (!field_reader.ok() || field_reader.remaining() != 0 ||
        gba::g_ws_pillarbox != 1) {
        std::fprintf(stderr, "Mode0 save/load did not install stale pillarbox\n");
        std::exit(1);
    }
    f.ppu.render(wide.data(), dispcnt, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    if (g_margin_policy_calls != gba::GbaPpu::kScreenHeight) {
        std::fprintf(stderr, "Mode0 margin policy call count mismatch\n");
        std::exit(1);
    }
    // BG0 is blue and visibly wins the authentic center. All untrusted BG1-3
    // ring samples must be suppressed in both margins, even where the ring
    // happens to contain distinct-looking tile data.
    expect_pixel(wide.data() + 24u * 3u, 0, 0, 255,
                 "Mode0 distinct BG0 center");
    for (uint32_t x = 0; x < 24u; ++x) {
        expect_pixel(wide.data() + x * 3u, 0, 0, 0,
                     "Mode0 left field margin cutoff");
    }
    for (uint32_t x = 264u; x < 288u; ++x) {
        expect_pixel(wide.data() + x * 3u, 0, 0, 0,
                     "Mode0 right field margin cutoff");
    }
    for (uint32_t y = 0; y < gba::GbaPpu::kScreenHeight; ++y) {
        const uint8_t* got = wide.data() +
            (static_cast<std::size_t>(y) * f.ppu.render_width() + 24u) * 3u;
        const uint8_t* expected = authentic.data() +
            static_cast<std::size_t>(y) * gba::GbaPpu::kScreenWidth * 3u;
        if (std::memcmp(got, expected,
                        gba::GbaPpu::kScreenWidth * 3u) != 0) {
            std::fprintf(stderr, "Mode0 expanded center changed at row %u\n", y);
            std::exit(1);
        }
    }

    // Windowed/transition-like frames fail closed even with the same field
    // layers; BG0 must not leak through the black margins.
    std::fill(wide.begin(), wide.end(), 0);
    f.ppu.render(wide.data(), dispcnt | 0x2000u, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(wide.data(), 0, 0, 0, "Mode0 transition left margin");
    expect_pixel(wide.data() + 287u * 3u, 0, 0, 0,
                 "Mode0 transition right margin");

    // The field signature must fail closed if one enabled layer is missing or
    // if the camera scrolls no longer match.
    store16(&f.io[0x18], 118u);
    std::fill(wide.begin(), wide.end(), 0);
    f.ppu.render(wide.data(), dispcnt, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(wide.data(), 0, 0, 0, "Mode0 mismatched scroll left margin");
    expect_pixel(wide.data() + 287u * 3u, 0, 0, 0,
                 "Mode0 mismatched scroll right margin");

    store16(&f.io[0x18], 117u);
    std::fill(wide.begin(), wide.end(), 0);
    f.ppu.render(wide.data(), dispcnt & ~0x0200u, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(wide.data(), 0, 0, 0, "Mode0 disabled BG1 left margin");
    expect_pixel(wide.data() + 287u * 3u, 0, 0, 0,
                 "Mode0 disabled BG1 right margin");

    gba::g_ws_margin_policy = nullptr;
    gba::g_ws_bg_x_provider = nullptr;
    gba::g_ws_bg_x_provider_layers = 0xFu;
    g_test_mode0_field_scene = false;
    gba::g_ws_pillarbox = 0;
    gba::g_ws_pillarbox_left = 0;
    gba::g_ws_pillarbox_right = 0;
}

void test_extended_bg_sample_remap_is_opt_in_and_native_inert() {
    Fixture f;
    const uint16_t dispcnt = 0x0100;  // Mode 0, BG0.
    store16(&f.io[0x08], 0x0180);     // 256 colors, screen block 1.
    std::fill_n(&f.vram[0], 64, 1);   // Tile 0: red.
    std::fill_n(&f.vram[64], 64, 2);  // Tile 1: blue.
    store16(&f.vram[0x800], 0);       // Authentic hardware X=0.
    store16(&f.vram[0x800 + 29 * 2], 1);  // Wrapped wide X=-24.
    store16(&f.pal[0], 0x03E0);       // Green backdrop.
    store16(&f.pal[2], 0x001F);       // Red tile 0.
    store16(&f.pal[4], 0x7C00);       // Blue tile 1.

    gba::g_ws_bg_x_provider = test_bg_x_provider;
    gba::g_ws_bg_x_provider_layers = 0xFu;
    g_bg_x_provider_calls = 0;
    f.ppu.render(f.rgb.data(), dispcnt, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    if (g_bg_x_provider_calls != 0) {
        std::fprintf(stderr, "native renderer called wide BG remap provider\n");
        std::exit(1);
    }
    expect_pixel(f.rgb.data(), 255, 0, 0,
                 "native BG changed by wide remap provider");

    f.ppu.set_view_margins(24, 24, 0, 0);
    std::vector<uint8_t> wide(gba::GbaPpu::kMaxFramebufferBytes, 0);
    f.ppu.render(wide.data(), dispcnt, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    if (g_bg_x_provider_calls == 0) {
        std::fprintf(stderr, "wide renderer did not call BG remap provider\n");
        std::exit(1);
    }
    expect_pixel(wide.data(), 255, 0, 0,
                 "wide BG remap did not sample authentic X");
    expect_pixel(wide.data() + 24u * 3u, 0, 255, 0,
                 "wide BG suppress did not expose backdrop");

    g_bg_x_provider_calls = 0;
    gba::g_ws_bg_x_provider_layers = 0;
    f.ppu.render(wide.data(), dispcnt, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    if (g_bg_x_provider_calls != 0) {
        std::fprintf(stderr, "wide renderer ignored BG provider layer mask\n");
        std::exit(1);
    }
    gba::g_ws_bg_x_provider_layers = 0xFu;
    gba::g_ws_bg_x_provider = nullptr;
}

void test_extended_view_snapshot_latch_policy() {
    constexpr std::size_t kPpuHeaderBytes = 3u * 4u + 2u + 8u + 1u;
    constexpr std::size_t kSnapshotBytes =
        kPpuHeaderBytes + gba::GbaPpu::kFramebufferBytes;

    // Native serialization remains the historical fixed header followed by the
    // contiguous 240x160 latch, byte for byte.
    auto native = std::make_unique<gba::GbaPpu>();
    uint8_t* native_latch = const_cast<uint8_t*>(native->latched_framebuffer());
    for (std::size_t i = 0; i < gba::GbaPpu::kFramebufferBytes; ++i)
        native_latch[i] = static_cast<uint8_t>((i * 17u + 11u) & 0xFFu);
    native->mark_framebuffer_latched();
    gbarecomp::debug::SnapshotWriter native_writer;
    native->serialize(native_writer);
    std::vector<uint8_t> historical(kSnapshotBytes, 0);
    historical[kPpuHeaderBytes - 1u] = 1u;
    std::memcpy(historical.data() + kPpuHeaderBytes, native_latch,
                gba::GbaPpu::kFramebufferBytes);
    if (native_writer.buffer() != historical) {
        std::fprintf(stderr, "native PPU snapshot bytes changed\n");
        std::exit(1);
    }

    // A 480-wide latch must serialize the authentic center row-by-row, not the
    // first 240x160 bytes of its wider row-major allocation.
    auto wide = std::make_unique<gba::GbaPpu>();
    wide->set_view_margins(120, 120, 0, 0);
    uint8_t* wide_latch = const_cast<uint8_t*>(wide->latched_framebuffer());
    for (uint32_t y = 0; y < wide->render_height(); ++y) {
        for (uint32_t x = 0; x < wide->render_width(); ++x) {
            for (uint32_t channel = 0; channel < 3; ++channel) {
                wide_latch[(static_cast<std::size_t>(y) * wide->render_width() + x) *
                               3u + channel] =
                    static_cast<uint8_t>((y * 7u + x * 3u + channel) & 0xFFu);
            }
        }
    }
    wide->mark_framebuffer_latched();
    gbarecomp::debug::SnapshotWriter wide_writer;
    wide->serialize(wide_writer);
    if (wide_writer.size() != kSnapshotBytes) {
        std::fprintf(stderr, "wide PPU snapshot layout size changed\n");
        std::exit(1);
    }
    const uint8_t* payload = wide_writer.buffer().data() + kPpuHeaderBytes;
    constexpr std::size_t kNativeStride = gba::GbaPpu::kScreenWidth * 3u;
    for (uint32_t y = 0; y < gba::GbaPpu::kScreenHeight; ++y) {
        const uint8_t* expected = wide_latch +
            (static_cast<std::size_t>(y) * wide->render_width() + 120u) * 3u;
        if (std::memcmp(payload + y * kNativeStride, expected,
                        kNativeStride) != 0) {
            std::fprintf(stderr, "wide snapshot center crop failed at row %u\n", y);
            std::exit(1);
        }
    }

    // Native loads retain the stored center latch. Wide loads consume the same
    // fixed payload but invalidate it because no serialized margin pixels exist.
    auto native_loaded = std::make_unique<gba::GbaPpu>();
    gba::g_ws_pillarbox = 0;
    gba::g_ws_pillarbox_left = 7;
    gba::g_ws_pillarbox_right = 9;
    gbarecomp::debug::SnapshotReader native_reader(
        wide_writer.buffer().data(), wide_writer.size());
    native_loaded->deserialize(native_reader);
    if (!native_reader.ok() || native_reader.remaining() != 0 ||
        !native_loaded->has_latched_framebuffer() ||
        std::memcmp(native_loaded->latched_framebuffer(), payload,
                    gba::GbaPpu::kFramebufferBytes) != 0) {
        std::fprintf(stderr, "wide-to-native snapshot latch restore failed\n");
        std::exit(1);
    }
    if (gba::g_ws_pillarbox != 0 || gba::g_ws_pillarbox_left != 7 ||
        gba::g_ws_pillarbox_right != 9) {
        std::fprintf(stderr, "native snapshot load changed margin policy\n");
        std::exit(1);
    }

    auto wide_loaded = std::make_unique<gba::GbaPpu>();
    wide_loaded->set_view_margins(120, 120, 0, 0);
    gba::g_ws_authored_margin_layers = 0;
    gba::g_ws_pillarbox = 0;
    gba::g_ws_pillarbox_left = 1;
    gba::g_ws_pillarbox_right = 1;
    gbarecomp::debug::SnapshotReader wide_reader(
        wide_writer.buffer().data(), wide_writer.size());
    wide_loaded->deserialize(wide_reader);
    if (!wide_reader.ok() || wide_reader.remaining() != 0 ||
        wide_loaded->has_latched_framebuffer() || gba::g_ws_pillarbox != 1 ||
        gba::g_ws_pillarbox_left != 0 || gba::g_ws_pillarbox_right != 0) {
        std::fprintf(stderr, "wide snapshot presentation latch was not invalidated\n");
        std::exit(1);
    }
    gba::g_ws_pillarbox = 0;

    // A self-sufficient game provider can explicitly authorize immediate
    // margin reconstruction from restored guest state. This must not weaken
    // the established default used by MMZ and the generic sidecar above.
    auto authored_loaded = std::make_unique<gba::GbaPpu>();
    authored_loaded->set_view_margins(120, 120, 0, 0);
    gba::g_ws_authored_margin_layers = 1;
    gba::g_ws_pillarbox = 0;
    gbarecomp::debug::SnapshotReader authored_reader(
        wide_writer.buffer().data(), wide_writer.size());
    authored_loaded->deserialize(authored_reader);
    if (!authored_reader.ok() || authored_reader.remaining() != 0 ||
        authored_loaded->has_latched_framebuffer() || gba::g_ws_pillarbox != 0) {
        std::fprintf(stderr,
                     "authored margin provider was pillarboxed after restore\n");
        std::exit(1);
    }
    gba::g_ws_authored_margin_layers = 0;
}

void test_extended_view_obj_x_is_explicitly_opt_in() {
    Fixture f;
    for (std::size_t i = 0; i < 128; ++i)
        store16(&f.oam[i * 8], 0x0200);  // Disable every OBJ.
    store16(&f.oam[0], 0x0000);          // Enable OBJ 0 at Y=0.
    store16(&f.oam[2], 0x0100);          // Raw 9-bit X=256.
    store16(&f.oam[4], 0x0000);          // 4bpp tile 0, OBJ palette 0.
    f.vram[0x10000] = 0x11;              // First two texels use color 1.
    store16(&f.pal[0x202], 0x001F);      // OBJ color 1 = red.

    gba::g_ws_obj_x_provider = test_positive_obj_x;
    f.ppu.render(f.rgb.data(), 0x1000, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    // The faithful renderer must ignore the extended-view provider.
    expect_pixel(f.rgb.data(), 0, 0, 0, "faithful OBJ X remained signed");

    f.ppu.set_view_margins(24, 24, 0, 0);
    std::vector<uint8_t> wide(gba::GbaPpu::kMaxFramebufferBytes, 0);
    f.ppu.render(wide.data(), 0x1000, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    const std::size_t extended_x = (256u + 24u) * 3u;
    expect_pixel(wide.data() + extended_x, 255, 0, 0,
                 "opted-in extended OBJ X");
    gba::g_ws_obj_x_provider = nullptr;
}

void test_extended_view_obj_covers_full_288_margin() {
    Fixture f;
    for (std::size_t i = 0; i < 128; ++i)
        store16(&f.oam[i * 8], 0x0200);  // Disable every OBJ.
    store16(&f.oam[0], 0x0000);          // Enable OBJ 0 at Y=0.
    store16(&f.oam[4], 0x0000);          // 4bpp tile 0, OBJ palette 0.
    f.vram[0x10000] = 0x11;              // First two texels use color 1.
    store16(&f.pal[0x202], 0x001F);      // OBJ color 1 = red.
    f.ppu.set_view_margins(24, 24, 0, 0);
    std::vector<uint8_t> wide(gba::GbaPpu::kMaxFramebufferBytes, 0);

    // Raw X=263 is the last possible sprite origin in a 288-wide view. The
    // faithful signed decode sees it as -249, so it must not leak into the
    // expanded output without an explicitly installed game adapter.
    store16(&f.oam[2], 263);
    gba::g_ws_obj_x_provider = nullptr;
    f.ppu.render(wide.data(), 0x1000, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(wide.data() + 287u * 3u, 0, 0, 0,
                 "unadapted OBJ X stayed hardware-signed");

    // The old signed range still reaches x=255 (output 279), proving the
    // missing span is exactly the outer eight pixels, not a whole-side cull.
    store16(&f.oam[2], 255);
    std::fill(wide.begin(), wide.end(), 0);
    f.ppu.render(wide.data(), 0x1000, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(wide.data() + 279u * 3u, 255, 0, 0,
                 "signed OBJ reached old right edge");
    expect_pixel(wide.data() + 287u * 3u, 0, 0, 0,
                 "signed OBJ left outer eight pixels empty");

    // The measured field adapter's [256,264) envelope reaches the exact
    // right edge, while raw X=264 stays outside and remains hidden.
    gba::g_ws_obj_x_provider = test_mode0_obj_x;
    store16(&f.oam[2], 263);
    std::fill(wide.begin(), wide.end(), 0);
    f.ppu.render(wide.data(), 0x1000, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(wide.data() + 287u * 3u, 255, 0, 0,
                 "adapted OBJ reached rightmost 24px margin");

    store16(&f.oam[2], 264);
    std::fill(wide.begin(), wide.end(), 0);
    f.ppu.render(wide.data(), 0x1000, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(wide.data() + 287u * 3u, 0, 0, 0,
                 "OBJ adapter exceeded right margin envelope");

    // The left edge already uses the authentic signed range: raw 0x1E8 is
    // hardware X=-24 and lands on output column zero.
    store16(&f.oam[2], 0x01E8);
    std::fill(wide.begin(), wide.end(), 0);
    f.ppu.render(wide.data(), 0x1000, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(wide.data(), 255, 0, 0,
                 "hardware-signed OBJ reached leftmost 24px margin");

    // A rejected/transition scene still blackens the expanded margins even
    // when an adapter is installed, so the widened OBJ interpretation cannot
    // leak sprites through pillarbox policy.
    store16(&f.oam[2], 263);
    gba::g_ws_margin_policy = test_margin_policy;
    std::fill(wide.begin(), wide.end(), 0);
    f.ppu.render(wide.data(), 0x1000, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(wide.data() + 287u * 3u, 0, 0, 0,
                 "pillarbox policy hid adapted OBJ margin");

    gba::g_ws_margin_policy = nullptr;
    gba::g_ws_obj_x_provider = nullptr;
    gba::g_ws_pillarbox = 0;
    gba::g_ws_pillarbox_left = 0;
    gba::g_ws_pillarbox_right = 0;
}

void test_extended_view_obj_attr_x_is_explicitly_opt_in() {
    Fixture f;
    for (std::size_t i = 0; i < 128; ++i)
        store16(&f.oam[i * 8], 0x0200);
    store16(&f.oam[0], 0x0000);
    store16(&f.oam[2], 10);
    store16(&f.oam[4], 0);
    f.vram[0x10000] = 0x11;
    store16(&f.pal[0x202], 0x001F);

    g_obj_attr_provider_calls = 0;
    gba::g_ws_obj_attr_x_provider = test_obj_attr_x;
    f.ppu.render(f.rgb.data(), 0x1000, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    if (g_obj_attr_provider_calls != 0) {
        std::fprintf(stderr, "native renderer called OBJ attribute provider\n");
        std::exit(1);
    }
    expect_pixel(f.rgb.data() + 10u * 3u, 255, 0, 0,
                 "native OBJ moved by attribute provider");

    f.ppu.set_view_margins(24, 24, 0, 0);
    std::vector<uint8_t> wide(gba::GbaPpu::kMaxFramebufferBytes, 0);
    f.ppu.render(wide.data(), 0x1000, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    if (g_obj_attr_provider_calls == 0) {
        std::fprintf(stderr, "wide renderer skipped OBJ attribute provider\n");
        std::exit(1);
    }
    expect_pixel(wide.data() + (24u + 260u) * 3u, 255, 0, 0,
                 "attribute-aware HUD OBJ placement");
    gba::g_ws_obj_attr_x_provider = nullptr;
}

void test_extended_view_extends_nearest_window_edge() {
    Fixture f;
    store16(&f.io[0x08], 0x0180);  // BG0 256-color, screen block 1.
    std::fill_n(f.vram.begin(), 64, static_cast<uint8_t>(1));
    store16(&f.vram[0x800], 0);
    store16(&f.pal[2], 0x001F);     // Red.
    store16(&f.io[0x40], 0x00F0);   // WIN0 X=[0,240).
    store16(&f.io[0x44], 0x00A0);   // WIN0 Y=[0,160).
    store16(&f.io[0x48], 0x0001);   // WIN0 enables BG0.
    store16(&f.io[0x4A], 0x0000);   // WINOUT disables everything.
    f.ppu.set_view_margins(24, 24, 0, 0);
    gba::g_ws_tilemap_provider = test_margin_tilemap;
    std::vector<uint8_t> wide(gba::GbaPpu::kMaxFramebufferBytes, 0);
    f.ppu.render(wide.data(), 0x2100, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(wide.data(), 255, 0, 0,
                 "left margin inherited visible window edge");
    expect_pixel(wide.data() + (287u * 3u), 255, 0, 0,
                 "right margin inherited visible window edge");

    // kWsTilemapUnavailable must suppress the wrapped entry itself. Make the
    // resident tile 0 opaque red and the backdrop green so this does not rely
    // on tile 0 being transparent; an unavailable margin must show only the
    // backdrop, while the authentic center remains unchanged.
    g_margin_provider_action = gba::kWsTilemapUnavailable;
    store16(&f.pal[0], 0x03E0);     // Green backdrop.
    std::fill(wide.begin(), wide.end(), 0);
    f.ppu.render(wide.data(), 0x2100, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(wide.data(), 0, 255, 0,
                 "unavailable provider leaked wrapped left tile");
    expect_pixel(wide.data() + (287u * 3u), 0, 255, 0,
                 "unavailable provider leaked wrapped right tile");
    expect_pixel(wide.data() + 24u * 3u, 255, 0, 0,
                 "unavailable provider changed native center");
    g_margin_provider_action = gba::kWsTilemapReplace;
    store16(&f.pal[0], 0x0000);     // Restore the black test backdrop.

    // A game can explicitly retain a wrapped entry for an intentionally
    // tiled effect without weakening the default fail-closed margin policy.
    std::fill_n(f.vram.begin() + 64, 64, static_cast<uint8_t>(2));
    store16(&f.pal[4], 0x03E0);     // Green.
    store16(&f.vram[0x800 + 29 * 2], 1);  // hx=-24 wraps to tile column 29.
    g_margin_provider_action = gba::kWsTilemapKeepWrapped;
    std::fill(wide.begin(), wide.end(), 0);
    f.ppu.render(wide.data(), 0x2100, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(wide.data(), 0, 255, 0,
                 "provider did not retain authored wrapped overlay");
    g_margin_provider_action = gba::kWsTilemapReplace;

    // With both authentic edges outside a smaller iris, margins inherit the
    // masked edge instead of bypassing WINOUT.
    store16(&f.io[0x40], 0x32BE);   // WIN0 X=[50,190).
    std::fill(wide.begin(), wide.end(), 0);
    f.ppu.render(wide.data(), 0x2100, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(wide.data(), 0, 0, 0,
                 "left margin inherited masked iris edge");
    expect_pixel(wide.data() + (287u * 3u), 0, 0, 0,
                 "right margin inherited masked iris edge");

    // Inverse apertures can make the authentic edge visible while the center
    // is hidden. Extending that edge would leak scenery around a closed wipe.
    store16(&f.io[0x48], 0x0000);
    store16(&f.io[0x4A], 0x0001);
    std::fill(wide.begin(), wide.end(), 0);
    f.ppu.render(wide.data(), 0x2100, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(wide.data(), 0, 0, 0,
                 "inverse aperture failed margin closed");
    expect_pixel(wide.data() + (287u * 3u), 0, 0, 0,
                 "inverse aperture failed right margin closed");

    // A narrow guest mask between the edge and center probes is still a
    // non-uniform scanline. The former edge/center-only classifier missed it
    // and extended visible WINOUT into both margins.
    store16(&f.io[0x40], 0x1428);   // WIN0 X=[20,40), covers no edge or center.
    store16(&f.io[0x48], 0x0000);   // WIN0 disables BG0.
    store16(&f.io[0x4A], 0x0001);   // WINOUT enables BG0.
    store16(&f.pal[0], 0x03E0);      // Nonblack green backdrop.
    std::vector<uint8_t> authentic(gba::GbaPpu::kFramebufferBytes, 0);
    f.ppu.set_view_margins(0, 0, 0, 0);
    f.ppu.render(authentic.data(), 0x2100, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    f.ppu.set_view_margins(24, 24, 0, 0);
    std::fill(wide.begin(), wide.end(), 0);
    f.ppu.render(wide.data(), 0x2100, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(wide.data(), 0, 0, 0,
                 "narrow off-center mask failed left margin closed");
    expect_pixel(wide.data() + (287u * 3u), 0, 0, 0,
                 "narrow off-center mask failed right margin closed");
    for (uint32_t row = 0; row < gba::GbaPpu::kScreenHeight; ++row) {
        const uint8_t* got = wide.data() +
            (row * f.ppu.render_width() + f.ppu.view_extra_left()) * 3u;
        const uint8_t* expected = authentic.data() +
            row * gba::GbaPpu::kScreenWidth * 3u;
        if (std::memcmp(got, expected,
                        gba::GbaPpu::kScreenWidth * 3u) != 0) {
            std::fprintf(stderr,
                         "narrow mask changed authentic center row %u\n", row);
            std::exit(1);
        }
    }

    // Minish's full-room buffers are independent of native 240px HUD/dialog
    // windows. Its separate opt-in may reconstruct only the regular-BG
    // margins while leaving the authentic center masked exactly as authored.
    store16(&f.io[0x40], 0x32BE);   // Non-uniform native window.
    store16(&f.io[0x48], 0x0000);   // Disable BG0 inside WIN0.
    store16(&f.io[0x4A], 0x0000);   // Disable BG0 in WINOUT too.
    gba::g_ws_authored_margin_layers = 1;
    std::fill(wide.begin(), wide.end(), 0);
    f.ppu.render(wide.data(), 0x2100, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(wide.data(), 255, 0, 0,
                 "authored provider did not reconstruct left margin");
    expect_pixel(wide.data() + (287u * 3u), 255, 0, 0,
                 "authored provider did not reconstruct right margin");
    expect_pixel(wide.data() + ((24u + 100u) * 3u), 0, 255, 0,
                 "authored margin policy changed native window center");
    gba::g_ws_authored_margin_layers = 0;
    gba::g_ws_tilemap_provider = nullptr;
    g_margin_provider_action = gba::kWsTilemapReplace;
}

void test_golden_sun_field_atlas_provider() {
    Fixture f;
    constexpr uint16_t dispcnt = 0x0E00u; // Mode 0, BG1/BG2/BG3.
    constexpr uint16_t bgcnt = 0x0180u;   // 256-colour, 256x256.
    store16(&f.io[0x0A], bgcnt);
    store16(&f.io[0x0C], bgcnt);
    store16(&f.io[0x0E], bgcnt);
    for (uint32_t off : {0x14u, 0x18u, 0x1Cu}) store16(&f.io[off], 115u);
    for (uint32_t off : {0x16u, 0x1Au, 0x1Eu}) store16(&f.io[off], 239u);

    // Central ring data is red; the atlas provider supplies blue left and
    // green right margin entries. Each BG gets the same deterministic tile
    // source, while priority ordering remains the stock PPU path.
    std::fill_n(f.vram.begin(), 64, static_cast<uint8_t>(1));
    std::fill_n(f.vram.begin() + 64, 64, static_cast<uint8_t>(2));
    std::fill_n(f.vram.begin() + 128, 64, static_cast<uint8_t>(3));
    for (uint32_t row = 0; row < 32; ++row)
        for (uint32_t col = 0; col < 32; ++col)
            store16(&f.vram[0x800u + (row * 32u + col) * 2u], 0u);
    store16(&f.pal[0], 0x0000u);
    store16(&f.pal[2], 0x001Fu); // red
    store16(&f.pal[4], 0x7C00u); // blue
    store16(&f.pal[6], 0x03E0u); // green

    std::fill(g_field_atlas.begin(), g_field_atlas.end(), 0);
    const auto set_map_region = [](uint32_t tile_x, uint32_t id) {
        for (uint32_t y = 0; y < 160u; y += 8u) {
            const uint32_t tile_y = static_cast<uint32_t>(floor_div8(239 +
                static_cast<int>(y)));
            const size_t off = 0x10000u +
                ((static_cast<size_t>((tile_y >> 1) & 127u) * 128u) +
                 ((tile_x >> 1) & 127u)) * 4u;
            store32(&g_field_atlas[off], id);
        }
    };
    for (uint32_t x = 11; x <= 14; ++x) set_map_region(x, 7u);
    for (uint32_t x = 47; x <= 50; ++x) set_map_region(x, 8u);
    for (uint32_t sub_y = 0; sub_y < 2; ++sub_y)
        for (uint32_t sub_x = 0; sub_x < 2; ++sub_x) {
            store16(&g_field_atlas[0x20000u + 7u * 8u +
                                   sub_y * 4u + sub_x * 2u], 1u);
            store16(&g_field_atlas[0x20000u + 8u * 8u +
                                   sub_y * 4u + sub_x * 2u], 2u);
        }

    gba::g_ws_margin_policy = test_margin_policy;
    gba::g_ws_tilemap_provider = test_golden_sun_field_tilemap;
    gba::g_ws_bg_x_provider = nullptr;
    gba::g_ws_pillarbox = 0;
    gba::g_ws_pillarbox_left = 0;
    gba::g_ws_pillarbox_right = 0;
    g_field_atlas_scene_valid = true;
    f.ppu.set_view_margins(24, 24, 0, 0);
    std::vector<uint8_t> wide(gba::GbaPpu::kMaxFramebufferBytes, 0);
    f.ppu.render(wide.data(), dispcnt, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(wide.data() + 24u * 3u, 255, 0, 0,
                 "atlas provider center changed");
    expect_pixel(wide.data() + 0u * 3u, 0, 0, 255,
                 "atlas provider left margin missing");
    expect_pixel(wide.data() + 287u * 3u, 0, 255, 0,
                 "atlas provider right margin missing");

    // A transition/save-load invalidation closes margins while preserving the
    // provider hook; reauthorizing the field scene reopens both sides.
    gbarecomp::debug::SnapshotWriter writer;
    f.ppu.serialize(writer);
    gbarecomp::debug::SnapshotReader reader(writer.buffer().data(),
                                             writer.size());
    f.ppu.deserialize(reader);
    g_field_atlas_scene_valid = false;
    std::fill(wide.begin(), wide.end(), 0);
    f.ppu.render(wide.data(), dispcnt, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(wide.data(), 0, 0, 0, "atlas transition left not closed");
    expect_pixel(wide.data() + 287u * 3u, 0, 0, 0,
                 "atlas transition right not closed");
    g_field_atlas_scene_valid = true;
    std::fill(wide.begin(), wide.end(), 0);
    f.ppu.render(wide.data(), dispcnt, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    expect_pixel(wide.data(), 0, 0, 255,
                 "atlas provider did not recover after transition");
    expect_pixel(wide.data() + 287u * 3u, 0, 255, 0,
                 "atlas provider right did not recover after transition");

    g_field_atlas_scene_valid = false;
    gba::g_ws_tilemap_provider = nullptr;
    gba::g_ws_margin_policy = nullptr;
}

void test_expanded_margin_diagnostics_attribute_final_source() {
    Fixture f;
    constexpr uint16_t dispcnt = 0x0100u; // Mode 0, BG0.
    store16(&f.io[0x08], 0x0180u);        // 256-colour map at block 1.
    std::fill_n(f.vram.begin(), 64u, static_cast<uint8_t>(1));
    store16(&f.vram[0x800u], 0u);
    store16(&f.pal[0], 0x03E0u);          // Green backdrop.
    store16(&f.pal[2], 0x001Fu);          // Red field tile.

    f.ppu.set_view_margins(24, 24, 0, 0);
    gba::g_ws_margin_policy = nullptr;
    gba::g_ws_pillarbox = 0;
    gba::g_ws_pillarbox_left = 0;
    gba::g_ws_pillarbox_right = 0;
    gba::g_ws_tilemap_provider = test_margin_tilemap;
    gba::g_ws_margin_diagnostics = test_margin_diagnostics;
    g_margin_diagnostics_callbacks = 0;
    g_margin_provider_action = gba::kWsTilemapReplace;
    std::vector<uint8_t> wide(gba::GbaPpu::kMaxFramebufferBytes, 0);
    f.ppu.render(wide.data(), dispcnt, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    constexpr uint64_t kHorizontalMarginPixels = 24u * 160u * 2u;
    if (g_margin_diagnostics_callbacks != 1u ||
        g_margin_diagnostics_last.margin_pixels != kHorizontalMarginPixels ||
        g_margin_diagnostics_last.left_margin_pixels != 24u * 160u ||
        g_margin_diagnostics_last.right_margin_pixels != 24u * 160u ||
        g_margin_diagnostics_last.provider_results[0]
            [gba::kWsMarginProviderReplace] != kHorizontalMarginPixels ||
        g_margin_diagnostics_last.final_selected[gba::kWsMarginTraceBg0]
            [gba::kWsMarginSourceProviderReplace] != kHorizontalMarginPixels ||
        g_margin_diagnostics_last.horizontal_final_selected[1]
            [gba::kWsMarginTraceBg0]
            [gba::kWsMarginSourceProviderReplace] != 24u * 160u) {
        std::fprintf(stderr, "margin replace diagnostics attribution mismatch\n");
        std::exit(1);
    }

    g_margin_provider_action = gba::kWsTilemapKeepWrapped;
    f.ppu.render(wide.data(), dispcnt, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    if (g_margin_diagnostics_last.provider_results[0]
            [gba::kWsMarginProviderKeepWrapped] != kHorizontalMarginPixels ||
        g_margin_diagnostics_last.final_selected[gba::kWsMarginTraceBg0]
            [gba::kWsMarginSourceProviderKeepWrapped] !=
                kHorizontalMarginPixels) {
        std::fprintf(stderr, "margin keep-wrapped diagnostics mismatch\n");
        std::exit(1);
    }

    g_margin_provider_action = gba::kWsTilemapUnavailable;
    f.ppu.render(wide.data(), dispcnt, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    if (g_margin_diagnostics_last.provider_results[0]
            [gba::kWsMarginProviderUnavailable] != kHorizontalMarginPixels ||
        g_margin_diagnostics_last.final_selected[gba::kWsMarginTraceBackdrop]
            [gba::kWsMarginSourceBackdrop] != kHorizontalMarginPixels ||
        g_margin_diagnostics_last.horizontal_final_selected[1]
            [gba::kWsMarginTraceBackdrop]
            [gba::kWsMarginSourceBackdrop] != 24u * 160u) {
        std::fprintf(stderr, "margin unavailable diagnostics mismatch\n");
        std::exit(1);
    }

    const unsigned callbacks_before_native = g_margin_diagnostics_callbacks;
    f.ppu.set_view_margins(0, 0, 0, 0);
    f.ppu.render(f.rgb.data(), dispcnt, f.io.data(), f.vram.data(),
                 f.oam.data(), f.pal.data());
    if (g_margin_diagnostics_callbacks != callbacks_before_native) {
        std::fprintf(stderr, "native render invoked margin diagnostics\n");
        std::exit(1);
    }
    gba::g_ws_margin_diagnostics = nullptr;
    gba::g_ws_tilemap_provider = nullptr;
    g_margin_provider_action = gba::kWsTilemapReplace;
}

} // namespace

int main() {
    test_alpha_native_domain_and_green_precision();
    test_brightness_native_domain_and_green_precision();
    test_hblank_affine_reference_reload();
    test_affine_hidden_state_roundtrip();
    test_native_scene_supersampling();
    test_extended_view_geometry_and_clamp();
    test_extended_view_capability_policy();
    test_resize_driven_view_policy();
    test_extended_view_preserves_authentic_center();
    test_expanded_view_vertical_ppu_paths();
    test_expanded_view_vertical_tilemap_provider();
    test_expanded_view_obj_y_provider();
    test_expanded_margin_policy_pillarboxes_rejected_scene();
    test_expanded_mode0_field_policy_and_margins();
    test_extended_bg_sample_remap_is_opt_in_and_native_inert();
    test_extended_view_snapshot_latch_policy();
    test_extended_view_obj_x_is_explicitly_opt_in();
    test_extended_view_obj_covers_full_288_margin();
    test_extended_view_obj_attr_x_is_explicitly_opt_in();
    test_extended_view_extends_nearest_window_edge();
    test_golden_sun_field_atlas_provider();
    test_expanded_margin_diagnostics_attribute_final_source();
    std::puts("ppu_smoke_tests: PASS");
    return 0;
}
