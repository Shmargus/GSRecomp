// tools/ppu_bench.cpp -- standalone renderer bench + equivalence harness.
//
// Build (not part of any CMake target; compiles in seconds):
//
//   g++ -O2 -std=gnu++17 -I gbarecomp/src/gba -I gbarecomp/src/runtime //       -I gbarecomp/src/armv4t -I gbarecomp/src/debug //       -o ppu_bench.exe tools/ppu_bench.cpp gbarecomp/src/gba/gba_ppu.cpp
//
// Usage: ppu_bench <snap.bin> [iterations] [--hooks|--battle|--native]
//   --hooks   installs a field tilemap source that declines every sample and
//             a margin policy with no flags, as the field runs.
//   --battle  as --hooks, with Golden Sun's battle margin policy (pillarbox
//             top and bottom).
//
// To prove a renderer change draws identically, build a second binary from a
// copy of gba_ppu.cpp without the change and compare the printed CRCs across
// several snapshots and all three modes. That is how the fully-pillarboxed
// row skip was accepted (FACTS.md, 2026-09-13).
//
// Kept in the repo because it was written, lost and rewritten once already.
//
// Standalone renderer bench/equivalence harness.
//
// Links the REAL gbarecomp/src/gba/gba_ppu.cpp and drives GbaPpu::render with
// a scene taken verbatim from a recorded GSRSNAP1 snapshot (IO, VRAM, OAM,
// PAL), so the frame it times is one the game actually produced rather than
// synthetic data. Prints milliseconds per frame and a CRC32 of the finished
// framebuffer: the CRC is the equivalence check a renderer change has to
// leave untouched.
//
// Not part of the product. Reads only host-written diagnostic files.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <string>
#include <vector>
#include <map>
#include <cstring>
#include <cstdlib>

#include "gba_ppu.h"

namespace {

struct Snapshot {
    std::uint64_t frame = 0;
    std::string tag;
    std::map<std::string, std::vector<std::uint8_t>> sections;
};

bool load_snapshot(const char* path, Snapshot* out) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) { std::fprintf(stderr, "cannot open %s\n", path); return false; }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<std::uint8_t> d(static_cast<std::size_t>(size));
    if (std::fread(d.data(), 1, d.size(), f) != d.size()) {
        std::fclose(f); std::fprintf(stderr, "short read %s\n", path); return false;
    }
    std::fclose(f);
    if (d.size() < 88 || std::memcmp(d.data(), "GSRSNAP1", 8) != 0) {
        std::fprintf(stderr, "%s: bad magic\n", path); return false;
    }
    std::uint32_t count = 0;
    std::memcpy(&count, d.data() + 12, 4);
    std::memcpy(&out->frame, d.data() + 16, 8);
    out->tag.assign(reinterpret_cast<const char*>(d.data() + 24),
                    strnlen(reinterpret_cast<const char*>(d.data() + 24), 64));
    std::size_t off = 24 + 64;
    for (std::uint32_t i = 0; i < count; ++i) {
        char name[9] = {};
        std::memcpy(name, d.data() + off, 8);
        std::uint64_t s_off = 0, s_len = 0;
        std::memcpy(&s_off, d.data() + off + 8, 8);
        std::memcpy(&s_len, d.data() + off + 16, 8);
        if (s_off + s_len <= d.size()) {
            out->sections[name].assign(d.begin() + static_cast<long>(s_off),
                                       d.begin() + static_cast<long>(s_off + s_len));
        }
        off += 8 + 16;
    }
    return true;
}

std::uint32_t crc32_of(const std::uint8_t* p, std::size_t n) {
    static std::uint32_t table[256];
    static bool built = false;
    if (!built) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        built = true;
    }
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

const std::vector<std::uint8_t>* sec(const Snapshot& s, const char* n) {
    auto it = s.sections.find(n);
    return it == s.sections.end() ? nullptr : &it->second;
}

}  // namespace

// Golden Sun installs a field tilemap source and a margin policy for every
// frame, battle included, where the source declines every sample. The
// renderer still pays the indirect call per sample. Stubs with the same
// answers isolate that cost from the drawing itself.
extern "C" int decline_source(int, int, int, std::uint16_t*) { return 0; }
extern "C" void decline_begin(int) {}
extern "C" unsigned zero_policy(std::uint16_t, const std::uint8_t*) { return 0u; }
// Golden Sun's battle margin policy: pillarbox top and bottom (kPillarboxTop |
// kPillarboxBottom == 1<<2 | 1<<3), left and right left open.
extern "C" unsigned battle_policy(std::uint16_t, const std::uint8_t*) { return 0xCu; }

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
            "usage: bench <snap.bin> [iterations] [--native]\n");
        return 2;
    }
    const int iters = argc > 2 ? std::atoi(argv[2]) : 200;
    bool native = false;
    bool hooks = false;
    bool battle = false;
    for (int i = 3; i < argc; ++i) {
        if (std::strcmp(argv[i], "--native") == 0) native = true;
        if (std::strcmp(argv[i], "--hooks") == 0) hooks = true;
        if (std::strcmp(argv[i], "--battle") == 0) { hooks = true; battle = true; }
    }
    if (hooks) {
        gba::g_ws_field_tilemap_source = &decline_source;
        gba::g_ws_field_tilemap_source_begin = &decline_begin;
        gba::g_ws_margin_policy = battle ? &battle_policy : &zero_policy;
    }

    Snapshot snap;
    if (!load_snapshot(argv[1], &snap)) return 1;
    const auto* io = sec(snap, "IO");
    const auto* vram = sec(snap, "VRAM");
    const auto* oam = sec(snap, "OAM");
    const auto* pal = sec(snap, "PAL");
    if (!io || !vram || !oam || !pal) {
        std::fprintf(stderr, "snapshot is missing IO/VRAM/OAM/PAL\n");
        return 1;
    }

    gba::GbaPpu ppu;
    if (!native) ppu.set_view_margins(60, 60, 40, 40);
    const std::uint32_t w = ppu.render_width(), h = ppu.render_height();
    std::vector<std::uint8_t> fb(ppu.render_bytes());
    std::uint16_t dispcnt = 0;
    std::memcpy(&dispcnt, io->data(), 2);

    // One untimed render so first-touch page faults on the framebuffer and any
    // lazily built table are not charged to the measurement.
    ppu.render(fb.data(), dispcnt, io->data(), vram->data(), oam->data(),
               pal->data());
    const std::uint32_t crc = crc32_of(fb.data(), fb.size());

    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i) {
        ppu.render(fb.data(), dispcnt, io->data(), vram->data(), oam->data(),
                   pal->data());
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double us = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          t1 - t0).count() / 1000.0 / iters;

    std::printf("%-44s frame=%llu tag=%-22s %ux%u  %8.0f us/frame  crc=%08X\n",
                argv[1], static_cast<unsigned long long>(snap.frame),
                snap.tag.c_str(), w, h, us, crc, battle ? "  [battle]" : hooks ? "  [hooks]" : "");
    return 0;
}
