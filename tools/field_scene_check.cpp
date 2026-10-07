// tools/field_scene_check.cpp -- step 2 of the modern renderer plan.
//
// Proves the frame description in src/field_scene.* is complete, by driving
// the REAL renderer from it and checking the picture does not change.
//
// For every recorded snapshot:
//
//   1. render it the normal way            -> checksum A
//   2. capture a FieldScene from it
//   3. rebuild the hardware state from that description alone
//   4. render the rebuilt state            -> checksum B
//
// A == B means the description carries everything the renderer reads. A != B
// means the capture dropped something, and the snapshot that failed says which
// frame to look at.
//
// Video memory is not part of the description on purpose (tile art stays on
// the graphics card and is refreshed when the game writes it), so it is passed
// through unchanged. This checks the registers, objects and palettes.
//
// Build:
//   g++ -O2 -std=gnu++17 -I src -I gbarecomp/src/gba -I gbarecomp/src/runtime \
//       -I gbarecomp/src/armv4t -I gbarecomp/src/debug \
//       -o field_scene_check.exe tools/field_scene_check.cpp \
//       src/field_scene.cpp gbarecomp/src/gba/gba_ppu.cpp
//
// Usage: field_scene_check [logs-dir] [--all]
//        --all also checks frames the scene model does not claim to support,
//        which is how the description gets extended to new cases.
//
// Not part of the product. Reads only host-written diagnostic files.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "gba_ppu.h"
#include "field_scene.h"

// The room buffer is a runtime thing and is not linked here. The capture asks
// it whether this room is reconstructed; offline it never is, so answer no and
// let the harness decide what to check via --all.
namespace gsr {
bool room_buffer_rendering() { return false; }
}  // namespace gsr

namespace {

struct Snapshot {
    std::uint64_t frame = 0;
    std::string tag;
    std::map<std::string, std::vector<std::uint8_t>> sections;
};

bool load_snapshot(const char* path, Snapshot* out) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::uint8_t head[88];
    if (std::fread(head, 1, sizeof head, f) != sizeof head ||
        std::memcmp(head, "GSRSNAP1", 8) != 0) { std::fclose(f); return false; }
    std::uint32_t count = 0;
    std::memcpy(&count, head + 12, 4);
    std::memcpy(&out->frame, head + 16, 8);
    out->tag.assign(reinterpret_cast<const char*>(head + 24),
                    strnlen(reinterpret_cast<const char*>(head + 24), 64));
    if (count > 64) { std::fclose(f); return false; }
    std::vector<std::pair<std::string, std::pair<std::uint64_t, std::uint64_t>>> dir;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint8_t e[24];
        if (std::fread(e, 1, sizeof e, f) != sizeof e) { std::fclose(f); return false; }
        char name[9] = {};
        std::memcpy(name, e, 8);
        std::uint64_t off = 0, len = 0;
        std::memcpy(&off, e + 8, 8);
        std::memcpy(&len, e + 16, 8);
        dir.push_back({name, {off, len}});
    }
    for (const auto& d : dir) {
        auto& v = out->sections[d.first];
        v.resize(static_cast<std::size_t>(d.second.second));
        if (std::fseek(f, static_cast<long>(d.second.first), SEEK_SET) != 0 ||
            std::fread(v.data(), 1, v.size(), f) != v.size()) v.clear();
    }
    std::fclose(f);
    return true;
}

std::uint32_t crc32_of(const std::uint8_t* p, std::size_t n) {
    static std::uint32_t table[256];
    static bool built = false;
    if (!built) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        built = true;
    }
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// True for register offsets the renderer never reads, so a difference there
// cannot explain a changed picture. DISPSTAT and VCOUNT (0x04-0x07) are status
// registers the frame's drawing does not consult.
bool register_affects_drawing(std::size_t off) {
    return off < 0x04u || off >= 0x08u;
}

// Names the first register the rebuild got wrong, so a failure points at a
// field rather than just a checksum. Only registers the renderer actually
// reads are considered: reporting the earliest differing byte instead pointed
// at DISPSTAT every time and hid the real cause.
std::string first_difference(const std::uint8_t* a, const std::uint8_t* b,
                             std::size_t n, const char* what) {
    const bool is_io = std::strcmp(what, "IO") == 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (is_io && !register_affects_drawing(i)) continue;
        if (a[i] != b[i]) {
            char buf[128];
            std::snprintf(buf, sizeof buf, "%s+0x%02X: %02X -> %02X",
                          what, static_cast<unsigned>(i), a[i], b[i]);
            return buf;
        }
    }
    return {};
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string logs = "logs";
    bool check_all = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--all") == 0) check_all = true;
        else if (argv[i][0] != '-') logs = argv[i];
    }

    std::string cmd = "dir /b /s \"" + logs + "\\snap_*.bin\" 2>nul";
    std::FILE* p = _popen(cmd.c_str(), "r");
    if (!p) { std::fprintf(stderr, "cannot scan %s\n", logs.c_str()); return 1; }

    std::printf("driving the renderer from the frame description alone\n");
    std::printf("checking %s frames in %s\n\n",
                check_all ? "ALL" : "supported (Mode 0-2 tile)", logs.c_str());

    int examined = 0, checked = 0, passed = 0, failed = 0, skipped = 0;
    std::map<std::string, int> skip_reasons;
    std::vector<std::string> failures;

    char line[1024];
    while (std::fgets(line, sizeof line, p)) {
        std::size_t n = std::strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!n) continue;
        ++examined;
        if ((examined % 200) == 0) std::printf("\r  %d examined, %d checked",
                                               examined, checked);

        Snapshot s;
        if (!load_snapshot(line, &s)) continue;
        auto it_io = s.sections.find("IO");
        auto it_vram = s.sections.find("VRAM");
        auto it_oam = s.sections.find("OAM");
        auto it_pal = s.sections.find("PAL");
        if (it_io == s.sections.end() || it_vram == s.sections.end() ||
            it_oam == s.sections.end() || it_pal == s.sections.end()) continue;

        std::vector<std::uint8_t> io = it_io->second;
        const std::vector<std::uint8_t>& vram = it_vram->second;
        std::vector<std::uint8_t> oam = it_oam->second;
        std::vector<std::uint8_t> pal = it_pal->second;
        if (io.size() < 0x60 || oam.size() < 1024 || pal.size() < 1024) continue;

        const unsigned dispcnt =
            static_cast<unsigned>(io[0] | (io[1] << 8));
        const char* why = nullptr;
        if (!check_all && !gsr::field_scene_frame_supported(dispcnt, &why)) {
            ++skipped;
            skip_reasons[why ? why : "unsupported"]++;
            continue;
        }

        // 1. the picture as it is drawn today
        gba::GbaPpu ppu;
        std::vector<std::uint8_t> fb_a(ppu.render_bytes());
        ppu.render(fb_a.data(), static_cast<std::uint16_t>(dispcnt), io.data(),
                   vram.data(), oam.data(), pal.data());
        const std::uint32_t crc_a = crc32_of(fb_a.data(), fb_a.size());

        // 2 and 3. describe the frame, then rebuild the hardware state from
        // the description alone.
        gsr::FieldScene scene;
        gsr::field_scene_capture(&scene, s.frame, io.data(), oam.data(),
                                 pal.data(), nullptr, nullptr, nullptr, nullptr);
        // Start the rebuilt register file EMPTY, not as a copy. If the
        // description fails to carry a field the renderer reads, the rebuild
        // leaves it zero and the picture changes -- which is the whole point.
        // A copy here would let every uncaptured field pass through unnoticed
        // and the check would prove nothing.
        std::vector<std::uint8_t> io_b(io.size(), 0);
        std::vector<std::uint8_t> oam_b(oam.size(), 0);
        std::vector<std::uint8_t> pal_b(pal.size(), 0);
        // Object slots carry affine parameters for OTHER slots in their last
        // two bytes, and those are tile data rather than object description.
        // Carry them across explicitly so this tests the description, not the
        // affine tables.
        for (std::size_t i = 6; i + 1 < oam.size(); i += 8) {
            oam_b[i] = oam[i];
            oam_b[i + 1] = oam[i + 1];
        }
        gsr::field_scene_write_back(scene, io_b.data(), oam_b.data(),
                                    pal_b.data());

        // 4. the picture drawn from the rebuilt state
        gba::GbaPpu ppu_b;
        std::vector<std::uint8_t> fb_b(ppu_b.render_bytes());
        const std::uint16_t dispcnt_b =
            static_cast<std::uint16_t>(io_b[0] | (io_b[1] << 8));
        ppu_b.render(fb_b.data(), dispcnt_b, io_b.data(), vram.data(),
                     oam_b.data(), pal_b.data());
        const std::uint32_t crc_b = crc32_of(fb_b.data(), fb_b.size());

        ++checked;
        if (crc_a == crc_b) { ++passed; continue; }

        ++failed;
        if (failures.size() < 12) {
            std::string detail = first_difference(io.data(), io_b.data(),
                                                  0x60, "IO");
            if (detail.empty())
                detail = first_difference(oam.data(), oam_b.data(), 1024, "OAM");
            if (detail.empty())
                detail = first_difference(pal.data(), pal_b.data(), 1024, "PAL");
            if (detail.empty()) detail = "state identical, picture differs";
            char buf[512];
            std::snprintf(buf, sizeof buf, "%s\n      mode %u  %08X -> %08X  %s",
                          line, dispcnt & 7u, crc_a, crc_b, detail.c_str());
            failures.push_back(buf);
        }
    }
    _pclose(p);

    std::printf("\r  %d examined, %d checked           \n\n", examined, checked);
    std::printf("identical pictures : %d\n", passed);
    std::printf("different pictures : %d\n", failed);
    if (skipped) {
        std::printf("skipped            : %d\n", skipped);
        for (const auto& kv : skip_reasons)
            std::printf("    %-40s %d\n", kv.first.c_str(), kv.second);
    }

    if (failed) {
        std::printf("\nthe description is incomplete. First failures:\n\n");
        for (const auto& f : failures) std::printf("  %s\n\n", f.c_str());
        return 1;
    }
    if (checked == 0) {
        std::printf("\nnothing was checked.\n");
        return 1;
    }
    std::printf("\nthe description carries everything the renderer reads,"
                " across %d frames.\n", checked);
    return 0;
}
