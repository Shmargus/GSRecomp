#include "frame_capture.h"

#include <algorithm>
#include <cstring>

namespace gsr {

// GSRGPUF1 follows map_recorder.cpp's GSRSNAP1 layout: an 88-byte header,
// then 24-byte directory entries (8-byte name, u64 offset, u64 length),
// then the section bytes.

std::string gpu_field_dump_bytes(const GpuFieldCapture& capture) {
    const gsr::FieldScene& scene = capture.scene;
    constexpr std::size_t kLineIoBytes = gsr::FieldScene::kLineIoBytes;
    constexpr int kRows = gsr::FieldScene::kRows;

    // LINEIO: row y's own registers where the console latched one, else the
    // frame's end-of-frame register file -- substituted here so the replay
    // tool never has to know the difference (field_scene.h's own contract
    // for row_io/row_io_valid/io_bytes).
    std::vector<std::uint8_t> line_io(kLineIoBytes * static_cast<std::size_t>(kRows));
    for (int y = 0; y < kRows; ++y) {
        const std::uint8_t* src = scene.registers_for_row(y);
        std::memcpy(line_io.data() + static_cast<std::size_t>(y) * kLineIoBytes,
                   src, kLineIoBytes);
    }

    // AFFREF: row y's own latched BG2/BG3 reference point where the console
    // captured one, else the frame-level reference walked by PB/PD.
    std::vector<std::int32_t> affine_refs(static_cast<std::size_t>(kRows) * 4u);
    for (int y = 0; y < kRows; ++y) {
        for (int bg = 2; bg <= 3; ++bg) {
            const auto reference = scene.affine_reference_for_row(y, bg);
            const auto offset = static_cast<std::size_t>(y) * 4u + (bg - 2) * 2u;
            std::copy(reference.begin(), reference.end(), affine_refs.data() + offset);
        }
    }

    struct Section {
        const char* name;
        const std::uint8_t* data;
        std::uint64_t length;
    };
    const Section sections[] = {
        {"SCENE",  reinterpret_cast<const std::uint8_t*>(&scene), sizeof(scene)},
        {"IO",     capture.io.data(), capture.io.size()},
        {"OAM",    capture.oam.data(), capture.oam.size()},
        {"PAL",    capture.pal.data(), capture.pal.size()},
        {"VRAM",   capture.vram.data(), capture.vram.size()},
        {"EWRAM",  capture.ewram.data(), capture.ewram.size()},
        {"IWRAM",  capture.iwram.data(), capture.iwram.size()},
        {"LINEIO", line_io.data(), line_io.size()},
        {"AFFREF", reinterpret_cast<const std::uint8_t*>(affine_refs.data()),
                   affine_refs.size() * sizeof(std::int32_t)},
        {"FXSTAMP", reinterpret_cast<const std::uint8_t*>(
                        capture.effects.stamps.data()),
                    capture.effects.stamps.size() * sizeof(gsr::EffectSpark)},
        {"FXART", capture.effects.artwork.data(),
                  capture.effects.artwork.size()},
    };
    constexpr std::size_t kSectionCount = sizeof(sections) / sizeof(sections[0]);

    auto put_u32 = [](std::string& out, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFFu));
    };
    auto put_u64 = [](std::string& out, std::uint64_t v) {
        for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFFu));
    };

    // Zero-initialized already covers "rest zero", including offset 8-11
    // (GSRSNAP1's version field, unused here).
    std::string header(88, '\0');
    std::memcpy(&header[0], "GSRGPUF1", 8);
    // offset 12: section count
    {
        std::string cnt;
        put_u32(cnt, static_cast<std::uint32_t>(kSectionCount));
        std::memcpy(&header[12], cnt.data(), 4);
    }
    // offset 16: frame number
    {
        std::string frm;
        put_u64(frm, scene.frame);
        std::memcpy(&header[16], frm.data(), 8);
    }

    std::string table;
    std::string data;
    std::uint64_t data_off = 88 + kSectionCount * 24u;
    for (const Section& s : sections) {
        std::string name_field(8, '\0');
        std::memcpy(&name_field[0], s.name,
                   std::min(std::strlen(s.name), std::size_t{8}));
        table += name_field;
        put_u64(table, data_off);
        put_u64(table, s.length);
        if (s.length) data.append(reinterpret_cast<const char*>(s.data), s.length);
        data_off += s.length;
    }

    std::string out;
    out.reserve(header.size() + table.size() + data.size());
    out += header;
    out += table;
    out += data;
    return out;
}

// "BUG REPORT SAVED" at the top right, in a small built-in font (the game's
// menu font is only read once a menu has opened), white on a dark box.
void paint_bug_report_notice(std::uint8_t* rgb, std::uint32_t width,
                             std::uint32_t height, const char* text) {
    struct Glyph { char c; const char* rows[7]; };
    static const Glyph kGlyphs[] = {
        {'A', {".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
        {'B', {"####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."}},
        {'C', {".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."}},
        {'D', {"####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."}},
        {'E', {"#####", "#....", "#....", "####.", "#....", "#....", "#####"}},
        {'G', {".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".####"}},
        {'H', {"#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
        {'I', {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "#####"}},
        {'L', {"#....", "#....", "#....", "#....", "#....", "#....", "#####"}},
        {'O', {".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
        {'P', {"####.", "#...#", "#...#", "####.", "#....", "#....", "#...."}},
        {'R', {"####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"}},
        {'S', {".####", "#....", "#....", ".###.", "....#", "....#", "####."}},
        {'T', {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."}},
        {'U', {"#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
        {'V', {"#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."}},
    };
    const int text_w = static_cast<int>(std::strlen(text)) * 6 - 1;
    const int box_w = text_w + 8, box_h = 7 + 8;
    const int x0 = static_cast<int>(width) - box_w - 4, y0 = 4;
    auto put = [&](int x, int y, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        if (x < 0 || y < 0 || x >= static_cast<int>(width) ||
            y >= static_cast<int>(height)) return;
        std::uint8_t* p = rgb + (static_cast<std::size_t>(y) * width + x) * 3u;
        p[0] = r; p[1] = g; p[2] = b;
    };
    for (int y = 0; y < box_h; ++y)
        for (int x = 0; x < box_w; ++x) {
            const bool edge = x == 0 || y == 0 || x == box_w - 1 || y == box_h - 1;
            if (edge) put(x0 + x, y0 + y, 200, 160, 255);
            else put(x0 + x, y0 + y, 24, 14, 40);
        }
    int cx = x0 + 4;
    for (const char* s = text; *s; ++s, cx += 6) {
        for (const Glyph& g : kGlyphs) {
            if (g.c != *s) continue;
            for (int ry = 0; ry < 7; ++ry)
                for (int rx = 0; rx < 5; ++rx)
                    if (g.rows[ry][rx] == '#') put(cx + rx, y0 + 4 + ry, 255, 255, 255);
        }
    }
}

}  // namespace gsr
