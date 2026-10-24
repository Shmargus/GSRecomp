#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

// Alhafra's port with the camera on Briggs's ship (map 98, F12
// tla_rewind_0131: camera 664,720, "Ahoy, Briggs!"). The room grid still holds
// the east end of the town's land, grid cells x28..38 / y33..43 (pixels
// 448..624 / 528..704; BG3 grass 5/6/9/0x17, BG2 empty below row 38, the same
// grid in 0040 and 0053). No capture's console view shows it, and in the
// widened view it stood as a lone green square in the top-left margin of the
// open sea. Jimmy's choice (2026-10-23): while the camera's left edge is past
// that land (x >= 624, so none of it can be on the console's screen), the
// margins draw those cells as the open water beside them (column 39, BG3
// water 2 / BG2 empty in rows 33..43). Only the margins' copy of memory
// changes; the game's own memory is untouched.
inline bool tla_alhafra_ship_view(const std::uint8_t* ewram, std::size_t bytes,
                                  std::vector<std::uint8_t>* out) {
    constexpr std::size_t kMapId = 0x428u;
    constexpr std::size_t kCameraX = 0x30E36u;  // integer half, 16.16
    constexpr std::size_t kGrid = 0x10000u;     // 128x128 u32 ids
    constexpr int kBg2Column = 64;              // BG2's region starts at x 1024
    constexpr int kFirstX = 28, kLastX = 38, kFirstY = 33, kLastY = 43;
    constexpr int kWaterX = kLastX + 1;
    constexpr unsigned kLandRightEdge = (kLastX + 1) * 16;
    if (!ewram || bytes < kGrid + 128u * 128u * 4u || bytes <= kCameraX + 1)
        return false;
    auto u16 = [&](std::size_t at) {
        return unsigned(ewram[at]) | (unsigned(ewram[at + 1]) << 8);
    };
    if (u16(kMapId) != 98u || u16(kCameraX) < kLandRightEdge) return false;
    out->assign(ewram, ewram + bytes);
    auto cell = [&](int x, int y) { return kGrid + std::size_t(y * 128 + x) * 4u; };
    for (int y = kFirstY; y <= kLastY; ++y)
        for (int x = kFirstX; x <= kLastX; ++x)
            for (int region : {0, kBg2Column})
                std::memcpy(out->data() + cell(region + x, y),
                            ewram + cell(region + kWaterX, y), 4);
    return true;
}
