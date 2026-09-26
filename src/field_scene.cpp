// field_scene.cpp -- see field_scene.h.
//
// Every register offset and bit meaning below is GBA hardware, not a Golden
// Sun guess. The Golden Sun part is only which rooms and modes are supported,
// and where the room and camera come from (FACTS.md).
#include "field_scene.h"

#include <cstring>

#include "gba_ppu.h"
#include "room_buffer.h"
#include "widescreen_policy.h"

namespace gsr {
namespace {

inline unsigned rd16(const std::uint8_t* p, std::size_t o) {
    return static_cast<unsigned>(p[o] | (p[o + 1] << 8));
}

// A signed 16-bit Q8.8 affine parameter (BGxPA/PB/PC/PD), and a signed 28-bit
// Q19.8 reference coordinate (BGxX/Y, sign-extended from bit 27) -- the same
// hardware format gba_ppu.cpp's read_s16/read_s28_ref decode, restated here
// because this file already independently decodes every other register
// rather than calling into the PPU.
inline int read_s16(const std::uint8_t* p, std::size_t o) {
    return static_cast<int>(static_cast<std::int16_t>(rd16(p, o)));
}

inline std::int32_t read_s28_ref(const std::uint8_t* p, std::size_t o) {
    std::uint32_t v = static_cast<std::uint32_t>(p[o]) |
                      (static_cast<std::uint32_t>(p[o + 1]) << 8) |
                      (static_cast<std::uint32_t>(p[o + 2]) << 16) |
                      (static_cast<std::uint32_t>(p[o + 3]) << 24);
    v &= 0x0FFFFFFFu;
    if (v & 0x08000000u) v |= 0xF0000000u;
    return static_cast<std::int32_t>(v);
}

// The GBA's object sizes, indexed by shape then size code.
constexpr int kObjWidth[3][4]  = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}};
constexpr int kObjHeight[3][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}};

void capture_layers(FieldScene* s, const std::uint8_t* io) {
    const int mode = s->video_mode;
    // A background's own DISPCNT bit only matters for the layers that mode
    // actually has. render_scanline_internal never calls a draw function for
    // the others regardless of their enable bit: mode 0 has all four
    // (regular); mode 1 has BG0/BG1 (regular) and BG2 (affine) -- BG3 is
    // never drawn; mode 2 has only BG2/BG3 (affine) -- BG0/BG1 are never
    // drawn (gba_ppu.cpp:901-913).
    auto layer_exists_in_mode = [&](int bg) {
        if (mode == 0) return true;
        if (mode == 1) return bg <= 2;
        if (mode == 2) return bg >= 2;
        return false;
    };
    for (int bg = 0; bg < 4; ++bg) {
        SceneLayer& L = s->layers[bg];
        L.enabled = layer_exists_in_mode(bg) &&
                    ((s->dispcnt >> (8 + bg)) & 1) != 0;
        const unsigned cnt = rd16(io, 0x08u + static_cast<std::size_t>(bg) * 2u);
        L.priority         = static_cast<int>(cnt & 3u);
        L.char_base        = ((cnt >> 2) & 3u) * 0x4000u;
        L.mosaic           = ((cnt >> 6) & 1u) != 0;
        L.eight_bit_colour = ((cnt >> 7) & 1u) != 0;
        L.screen_base      = ((cnt >> 8) & 0x1Fu) * 0x800u;
        L.wraps            = ((cnt >> 13) & 1u) != 0;
        L.size_code        = (cnt >> 14) & 3u;
        // Mode 0 is four regular layers; mode 1 makes BG2 affine; mode 2 makes
        // BG2 and BG3 affine.
        L.affine = (mode == 1 && bg == 2) || (mode == 2 && bg >= 2);
        // Taken UNMASKED: Golden Sun writes scroll values wider than the
        // hardware's 9 bits and the excess carries the layer's region of the
        // shared map grid (FACTS.md, "Per-layer sourcing").
        L.scroll_x = static_cast<int>(rd16(io, 0x10u + static_cast<std::size_t>(bg) * 4u));
        L.scroll_y = static_cast<int>(rd16(io, 0x12u + static_cast<std::size_t>(bg) * 4u));

        if (L.affine) {
            // BG2's parameters start at 0x20, BG3's at 0x30 -- only bg 2 or 3
            // ever reach here (mode 1 makes BG2 affine, mode 2 makes BG2 and
            // BG3 affine).
            const std::size_t param_off = (bg == 2) ? 0x20u : 0x30u;
            L.affine_pa = read_s16(io, param_off + 0x00u);
            L.affine_pb = read_s16(io, param_off + 0x02u);
            L.affine_pc = read_s16(io, param_off + 0x04u);
            L.affine_pd = read_s16(io, param_off + 0x06u);
            L.affine_ref_x = read_s28_ref(io, param_off + 0x08u);
            L.affine_ref_y = read_s28_ref(io, param_off + 0x0Cu);
        }
    }
}

void capture_objects(FieldScene* s, const std::uint8_t* oam) {
    for (int i = 0; i < FieldScene::kObjects; ++i) {
        SceneObject& O = s->objects[i];
        O = SceneObject{};
        const std::size_t base = static_cast<std::size_t>(i) * 8u;
        const unsigned a0 = rd16(oam, base + 0);
        const unsigned a1 = rd16(oam, base + 2);
        const unsigned a2 = rd16(oam, base + 4);

        const bool affine = (a0 & 0x0100u) != 0;
        const bool double_or_hidden = (a0 & 0x0200u) != 0;
        // A non-affine slot with the hide bit set draws nothing at all.
        if (!affine && double_or_hidden) continue;

        const unsigned mode = (a0 >> 10) & 3u;
        if (mode == 3u) continue;               // an invalid mode draws nothing

        const unsigned shape = (a0 >> 14) & 3u;
        const unsigned size  = (a1 >> 14) & 3u;
        if (shape == 3u) continue;              // reserved shape

        O.present = true;
        O.affine = affine;
        O.double_size = affine && double_or_hidden;
        O.mosaic = ((a0 >> 12) & 1u) != 0;
        O.blended = (mode == 1u);
        O.window  = (mode == 2u);
        O.width  = kObjWidth[shape][size];
        O.height = kObjHeight[shape][size];

        // Y is 8 bits and wraps; X is 9 bits and is signed -- the hardware
        // fallback rules, used verbatim below whenever no provider answers.
        // These mirror gba_ppu.cpp's render_scanline_wide (resolve_sy /
        // resolve_sx) exactly, including the asymmetry between the two
        // axes: Y's fallback threshold is 160 (an 8-bit field, so values
        // 160..255 are read as negative), X's is the top bit of a 9-bit
        // field (0x100), which is a different-looking test but agrees with
        // "x >= 240" for every value that is natively visible -- 240..255
        // is off-screen either way, so raw x in that range is the only
        // place the two rules could differ, and it never differs in where
        // it places the sprite (off-screen), only in the never-observed
        // sign of that off-screen number.
        const int raw_y = static_cast<int>(a0 & 0xFFu);
        const int raw_x = static_cast<int>(a1 & 0x1FFu);
        int y = 0, x = 0;
        bool y_trusted = false, x_trusted = false;
        int provided_y = 0;
        if (gba::g_ws_obj_attr_y_provider &&
            gba::g_ws_obj_attr_y_provider(i, static_cast<std::uint16_t>(a0),
                                     static_cast<std::uint16_t>(a1),
                                     static_cast<std::uint16_t>(a2),
                                     &provided_y)) {
            y = provided_y;
            y_trusted = true;
        } else {
            y = (raw_y >= 160) ? raw_y - 256 : raw_y;
        }
        int provided_x = 0;
        if (gba::g_ws_obj_attr_x_provider &&
            gba::g_ws_obj_attr_x_provider(i, static_cast<std::uint16_t>(a0),
                                     static_cast<std::uint16_t>(a1),
                                     static_cast<std::uint16_t>(a2),
                                     &provided_x)) {
            x = provided_x;
            x_trusted = true;
        } else {
            x = (raw_x & 0x100) ? raw_x - 0x200 : raw_x;
            // A raw X of 256+ read as negative matters on hardware only when
            // the sprite reaches back onto the screen from the left. When,
            // read that way, it would stay hidden even in the widened left
            // margin, the only thing it can mean is a sprite the game placed
            // past the right edge: world-map towns once their visibility
            // tests admit the right margin (FACTS.md, 2026-09-24). Reading it
            // as positive cannot move anything that would otherwise show.
            const int box_w = O.double_size ? 2 * O.width : O.width;
            if ((raw_x & 0x100) &&
                x + box_w <= -gsr::widescreen::kExpandedExtraX) {
                x = raw_x;
            }
        }
        O.x = x;
        O.y = y;
        O.trusted = x_trusted && y_trusted;
        O.parked = gsr::widescreen::golden_sun_obj_is_parked(
            static_cast<std::uint16_t>(a0), static_cast<std::uint16_t>(a1));

        if (affine) {
            O.affine_index = static_cast<int>((a1 >> 9) & 0x1Fu);
        } else {
            O.hflip = ((a1 >> 12) & 1u) != 0;
            O.vflip = ((a1 >> 13) & 1u) != 0;
        }

        O.tile     = a2 & 0x3FFu;
        O.priority = static_cast<int>((a2 >> 10) & 3u);
        const bool eight_bit = ((a0 >> 13) & 1u) != 0;
        O.palette  = eight_bit ? -1 : static_cast<int>((a2 >> 12) & 0xFu);
    }
}

// The transform groups live in the spare two bytes of every fourth object
// entry: group g's four values are at object slots g*4 + 0..3, byte 6 of each.
void capture_transforms(FieldScene* s, const std::uint8_t* oam) {
    for (int g = 0; g < FieldScene::kTransforms; ++g) {
        for (int k = 0; k < 4; ++k) {
            const std::size_t at =
                static_cast<std::size_t>(g) * 32u + static_cast<std::size_t>(k) * 8u + 6u;
            const std::int16_t raw = static_cast<std::int16_t>(rd16(oam, at));
            s->object_transforms[g][k] = static_cast<float>(raw) / 256.0f;
        }
    }
}

void capture_effects(FieldScene* s, const std::uint8_t* io) {
    SceneEffects& e = s->effects;
    e.win0       = ((s->dispcnt >> 13) & 1u) != 0;
    e.win1       = ((s->dispcnt >> 14) & 1u) != 0;
    e.obj_window = ((s->dispcnt >> 15) & 1u) != 0;
    e.win0h = rd16(io, 0x40);
    e.win1h = rd16(io, 0x42);
    e.win0v = rd16(io, 0x44);
    e.win1v = rd16(io, 0x46);
    e.winin  = rd16(io, 0x48);
    e.winout = rd16(io, 0x4A);
    e.mosaic = rd16(io, 0x4C);
    e.blend_control    = rd16(io, 0x50);
    e.blend_alpha      = rd16(io, 0x52);
    e.blend_brightness = rd16(io, 0x54);
}

// Rows are uniform when no captured row differs from row 0 across the register
// window a draw depends on. Golden Sun's battle frames measure uniform
// (FACTS.md, 2026-09-12), but this is checked per frame rather than assumed.
bool rows_are_uniform(const FieldScene* s) {
    int first = -1;
    for (int y = 0; y < FieldScene::kRows; ++y) {
        if (!s->row_io_valid[y]) continue;
        if (first < 0) { first = y; continue; }
        if (std::memcmp(s->row_io[first].data(), s->row_io[y].data(),
                        FieldScene::kLineIoBytes) != 0) {
            return false;
        }
    }
    // The affine reference legitimately advances every row by the hardware's
    // own walk, so it is not part of this test; a renderer reads it per row.
    return true;
}

}  // namespace

bool field_scene_frame_supported(unsigned dispcnt, const char** why) {
    const int mode = static_cast<int>(dispcnt & 7u);
    // Modes 0-2 are tile-based: 0 is four regular layers, 1 makes BG2
    // affine, 2 makes BG2 and BG3 affine. Modes 3-5 are bitmap modes that
    // redefine BG2 as a raw framebuffer instead of a tilemap; the emulated
    // hardware itself renders no background for them either
    // (gba_ppu.cpp's render_scanline_internal only branches on mode 0/1/2),
    // and the measured 2,429-snapshot corpus never uses them (FACTS.md), so
    // refusing is honest, not a guess.
    if (mode > 2) {
        if (why) *why = "the video mode uses a bitmap background";
        return false;
    }
    // Room knowledge is checked by the capture, which can see the room
    // buffer.
    if (why) *why = nullptr;
    return true;
}

bool field_scene_capture(FieldScene* out,
                         std::uint64_t frame,
                         const std::uint8_t* io,
                         const std::uint8_t* oam,
                         const std::uint8_t* pal,
                         const std::uint8_t* row_io_table,
                         const bool* row_io_valid,
                         const std::int32_t* row_affine_table,
                         const bool* row_affine_valid) {
    *out = FieldScene{};
    out->frame = frame;
    if (!io || !oam || !pal) {
        out->unsupported_reason = "the frame's memory was not available";
        return false;
    }

    out->dispcnt = rd16(io, 0x00);
    out->video_mode = static_cast<int>(out->dispcnt & 7u);
    out->forced_blank = ((out->dispcnt >> 7) & 1u) != 0;
    out->one_dimensional_objects = ((out->dispcnt >> 6) & 1u) != 0;
    out->valid = true;
    // Raw copy of the same end-of-frame registers the frame-level fields
    // above are decoded from, for the renderer's per-row fallback (see
    // field_scene.h).
    std::memcpy(out->io_bytes.data(), io, FieldScene::kLineIoBytes);

    capture_layers(out, io);
    capture_objects(out, oam);
    capture_transforms(out, oam);
    capture_effects(out, io);

    for (std::size_t i = 0; i < out->palette.size(); ++i)
        out->palette[i] = static_cast<std::uint16_t>(rd16(pal, i * 2u));

    if (row_io_table && row_io_valid) {
        for (int y = 0; y < FieldScene::kRows; ++y) {
            out->row_io_valid[y] = row_io_valid[y];
            if (!row_io_valid[y]) continue;
            std::memcpy(out->row_io[y].data(),
                        row_io_table + static_cast<std::size_t>(y) *
                                       FieldScene::kLineIoBytes,
                        FieldScene::kLineIoBytes);
        }
    }
    if (row_affine_table && row_affine_valid) {
        for (int y = 0; y < FieldScene::kRows; ++y) {
            out->row_affine_valid[y] = row_affine_valid[y];
            if (!row_affine_valid[y]) continue;
            for (int k = 0; k < 4; ++k)
                out->row_affine[y][k] =
                    row_affine_table[static_cast<std::size_t>(y) * 4u + k];
        }
    }
    out->rows_uniform = rows_are_uniform(out);

    const char* why = nullptr;
    if (!field_scene_frame_supported(out->dispcnt, &why)) {
        out->unsupported_reason = why;
        out->supported = false;
        return false;
    }
    // A field frame is only drawable when the room buffer actually holds this
    // room. Without it there is no world outside the console's own window, and
    // inventing one is the defect this project already removed (D-008).
    if (!room_buffer_rendering()) {
        out->unsupported_reason = "the room buffer has no reconstruction for "
                                  "this room";
        out->supported = false;
        return false;
    }
    out->supported = true;
    return true;
}


namespace {

inline void wr16(std::uint8_t* p, std::size_t o, unsigned v) {
    p[o]     = static_cast<std::uint8_t>(v & 0xFFu);
    p[o + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
}

// Recovers the shape and size codes from a decoded width/height. Each valid
// pair occurs once in the hardware's table, so this is exact.
bool object_shape_size(int w, int h, unsigned* shape, unsigned* size) {
    for (unsigned sh = 0; sh < 3; ++sh)
        for (unsigned sz = 0; sz < 4; ++sz)
            if (kObjWidth[sh][sz] == w && kObjHeight[sh][sz] == h) {
                *shape = sh; *size = sz; return true;
            }
    return false;
}

}  // namespace

void field_scene_write_back(const FieldScene& scene,
                            std::uint8_t* io,
                            std::uint8_t* oam,
                            std::uint8_t* pal) {
    if (!io || !oam || !pal) return;

    wr16(io, 0x00, scene.dispcnt);

    for (int bg = 0; bg < 4; ++bg) {
        const SceneLayer& L = scene.layers[bg];
        unsigned cnt = 0;
        cnt |= static_cast<unsigned>(L.priority) & 3u;
        cnt |= ((L.char_base / 0x4000u) & 3u) << 2;
        cnt |= (L.mosaic ? 1u : 0u) << 6;
        cnt |= (L.eight_bit_colour ? 1u : 0u) << 7;
        cnt |= ((L.screen_base / 0x800u) & 0x1Fu) << 8;
        cnt |= (L.wraps ? 1u : 0u) << 13;
        cnt |= (L.size_code & 3u) << 14;
        wr16(io, 0x08u + static_cast<std::size_t>(bg) * 2u, cnt);
        wr16(io, 0x10u + static_cast<std::size_t>(bg) * 4u,
             static_cast<unsigned>(L.scroll_x) & 0xFFFFu);
        wr16(io, 0x12u + static_cast<std::size_t>(bg) * 4u,
             static_cast<unsigned>(L.scroll_y) & 0xFFFFu);

        if (L.affine) {
            const std::size_t param_off = (bg == 2) ? 0x20u : 0x30u;
            wr16(io, param_off + 0x00u, static_cast<unsigned>(L.affine_pa) & 0xFFFFu);
            wr16(io, param_off + 0x02u, static_cast<unsigned>(L.affine_pb) & 0xFFFFu);
            wr16(io, param_off + 0x04u, static_cast<unsigned>(L.affine_pc) & 0xFFFFu);
            wr16(io, param_off + 0x06u, static_cast<unsigned>(L.affine_pd) & 0xFFFFu);
            const std::uint32_t refx = static_cast<std::uint32_t>(L.affine_ref_x);
            const std::uint32_t refy = static_cast<std::uint32_t>(L.affine_ref_y);
            io[param_off + 0x08u] = static_cast<std::uint8_t>(refx & 0xFFu);
            io[param_off + 0x09u] = static_cast<std::uint8_t>((refx >> 8) & 0xFFu);
            io[param_off + 0x0Au] = static_cast<std::uint8_t>((refx >> 16) & 0xFFu);
            io[param_off + 0x0Bu] = static_cast<std::uint8_t>((refx >> 24) & 0xFFu);
            io[param_off + 0x0Cu] = static_cast<std::uint8_t>(refy & 0xFFu);
            io[param_off + 0x0Du] = static_cast<std::uint8_t>((refy >> 8) & 0xFFu);
            io[param_off + 0x0Eu] = static_cast<std::uint8_t>((refy >> 16) & 0xFFu);
            io[param_off + 0x0Fu] = static_cast<std::uint8_t>((refy >> 24) & 0xFFu);
        }
    }

    const SceneEffects& e = scene.effects;
    wr16(io, 0x40, e.win0h);
    wr16(io, 0x42, e.win1h);
    wr16(io, 0x44, e.win0v);
    wr16(io, 0x46, e.win1v);
    wr16(io, 0x48, e.winin);
    wr16(io, 0x4A, e.winout);
    wr16(io, 0x4C, e.mosaic);
    wr16(io, 0x50, e.blend_control);
    wr16(io, 0x52, e.blend_alpha);
    wr16(io, 0x54, e.blend_brightness);

    for (int i = 0; i < FieldScene::kObjects; ++i) {
        const SceneObject& O = scene.objects[i];
        const std::size_t base = static_cast<std::size_t>(i) * 8u;
        if (!O.present) {
            // Hide the slot explicitly. Zeroing it would leave a visible
            // 8x8 object at the top-left instead.
            wr16(oam, base + 0, 0x0200u);
            wr16(oam, base + 2, 0u);
            wr16(oam, base + 4, 0u);
            continue;
        }
        unsigned shape = 0, size = 0;
        if (!object_shape_size(O.width, O.height, &shape, &size)) {
            wr16(oam, base + 0, 0x0200u);
            continue;
        }
        unsigned a0 = static_cast<unsigned>(O.y & 0xFF);
        a0 |= (O.affine ? 1u : 0u) << 8;
        a0 |= ((O.affine ? O.double_size : false) ? 1u : 0u) << 9;
        a0 |= (O.blended ? 1u : (O.window ? 2u : 0u)) << 10;
        a0 |= (O.mosaic ? 1u : 0u) << 12;
        a0 |= ((O.palette < 0) ? 1u : 0u) << 13;
        a0 |= (shape & 3u) << 14;

        unsigned a1 = static_cast<unsigned>(O.x) & 0x1FFu;
        if (O.affine) {
            a1 |= (static_cast<unsigned>(O.affine_index < 0 ? 0 : O.affine_index)
                   & 0x1Fu) << 9;
        } else {
            a1 |= (O.hflip ? 1u : 0u) << 12;
            a1 |= (O.vflip ? 1u : 0u) << 13;
        }
        a1 |= (size & 3u) << 14;

        unsigned a2 = O.tile & 0x3FFu;
        a2 |= (static_cast<unsigned>(O.priority) & 3u) << 10;
        if (O.palette >= 0) a2 |= (static_cast<unsigned>(O.palette) & 0xFu) << 12;

        wr16(oam, base + 0, a0);
        wr16(oam, base + 2, a1);
        wr16(oam, base + 4, a2);
        // Bytes 6-7 are not this object's; they carry a transform group, and
        // are written below from the description's own copy.
    }

    for (int g = 0; g < FieldScene::kTransforms; ++g) {
        for (int k = 0; k < 4; ++k) {
            const std::size_t at =
                static_cast<std::size_t>(g) * 32u +
                static_cast<std::size_t>(k) * 8u + 6u;
            const float value = scene.object_transforms[g][k] * 256.0f;
            const int rounded = static_cast<int>(
                value < 0.0f ? value - 0.5f : value + 0.5f);
            wr16(oam, at, static_cast<unsigned>(rounded) & 0xFFFFu);
        }
    }

    for (std::size_t i = 0; i < scene.palette.size(); ++i)
        wr16(pal, i * 2u, scene.palette[i]);
}

}  // namespace gsr
