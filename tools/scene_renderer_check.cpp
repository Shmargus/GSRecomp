// tools/scene_renderer_check.cpp -- does the GPU scene renderer draw the same
// background as the emulated hardware?
//
// Step 4's gate. For every recorded snapshot the GPU renderer accepts:
//
//   1. the emulated hardware draws the frame with objects switched off
//   2. the GPU renderer draws the same frame
//   3. compare the two pictures pixel by pixel
//
// Objects are switched off in the reference because the GPU renderer does not
// draw them yet and refuses any frame that has one. Comparing against a
// reference that included them would be comparing against the wrong picture.
//
// A small number of differing pixels is still a failure. The console's
// background rule is exact, and anything short of exact means the shader has a
// bug that will be much harder to find once objects and windows sit on top.
//
// Build:
//   g++ -O2 -std=gnu++17 -I src -I gbarecomp/src/gba -I gbarecomp/src/runtime \
//       -I gbarecomp/src/armv4t -I gbarecomp/src/debug \
//       -I C:/msys64/mingw64/include/SDL2 -o scene_renderer_check.exe \
//       tools/scene_renderer_check.cpp src/field_scene.cpp \
//       src/field_scene_renderer.cpp src/world_map_source.cpp \
//       src/effect_particles.cpp gbarecomp/src/runtime/gpu_surface.cpp \
//       gbarecomp/src/gba/gba_ppu.cpp -lmingw32 -lSDL2main -lSDL2 -lopengl32
//
// Usage: scene_renderer_check [logs-dir] [--limit N] [--dump path]
//        scene_renderer_check --replay <gpu_frame.bin> [--rom <file.gba>]
//        (see run_replay; --rom lets a world-map frame draw its margins
//        from the whole world map, as the game does)
//
// Not part of the product. Reads only host-written diagnostic files.
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_opengl.h>

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
#include "field_scene_renderer.h"
#include "gpu_surface.h"
#include "world_map_source.h"

// Offline there is no room buffer; the capture asks and must get an honest no.
namespace gsr {
bool room_buffer_rendering() { return false; }
}  // namespace gsr

namespace {

struct Snapshot {
    std::uint64_t frame = 0;
    std::map<std::string, std::vector<std::uint8_t>> sections;
};

bool load_snapshot(const char* path, Snapshot* out) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::uint8_t head[88];
    // Two writers share this 88-byte-header/24-byte-directory shape:
    // map_recorder.cpp's periodic GSRSNAP1 (one end-of-frame register read)
    // and runner_main.cpp's F9 GSRGPUF1 (one live frame, per-row tables
    // included -- see gpu_field_write_dump_if_requested's header comment).
    // Both are read the same way; only the magic differs.
    if (std::fread(head, 1, sizeof head, f) != sizeof head ||
        (std::memcmp(head, "GSRSNAP1", 8) != 0 &&
         std::memcmp(head, "GSRGPUF1", 8) != 0)) {
        std::fclose(f); return false;
    }
    std::uint32_t count = 0;
    std::memcpy(&count, head + 12, 4);
    std::memcpy(&out->frame, head + 16, 8);
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

void write_png(const char* path, int w, int h, const std::uint8_t* rgba);

// A signed 16-bit Q8.8 affine parameter and a signed 28-bit Q19.8 reference
// coordinate, read/written the same way gba_ppu.cpp's read_s16/read_s28_ref
// and this tool's own snapshot decoding already do -- restated here because
// --per-row-affine builds its own synthetic per-row tables rather than
// reading them from a snapshot.
inline std::int32_t read_s16_le(const std::uint8_t* p, std::size_t o) {
    std::int16_t v = static_cast<std::int16_t>(p[o] | (p[o + 1] << 8));
    return static_cast<std::int32_t>(v);
}

inline std::int32_t read_s28_ref_le(const std::uint8_t* p, std::size_t o) {
    std::uint32_t v = static_cast<std::uint32_t>(p[o]) |
                      (static_cast<std::uint32_t>(p[o + 1]) << 8) |
                      (static_cast<std::uint32_t>(p[o + 2]) << 16) |
                      (static_cast<std::uint32_t>(p[o + 3]) << 24);
    v &= 0x0FFFFFFFu;
    if (v & 0x08000000u) v |= 0xF0000000u;
    return static_cast<std::int32_t>(v);
}

inline void write_s16_le(std::uint8_t* p, std::size_t o, std::int32_t v) {
    p[o]     = static_cast<std::uint8_t>(v & 0xFF);
    p[o + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
}

// --per-row-affine: the default gate's snapshots hold only ONE end-of-frame
// register read, so it cannot exercise a shader that now reads PA/PC per row
// (from FieldScene::row_io) and the reference point per row (from
// FieldScene::row_affine). This mode builds, per accepted mode-1/2 snapshot,
// a synthetic 160-row line_io/affine_line_refs pair that varies PA/PD row to
// row by a deterministic non-linear pattern and reloads the x reference twice
// mid-frame, so nothing about it is reproducible from a single frame-level
// register value. Both sides -- the GPU renderer (via field_scene_capture's
// row tables) and the reference (gba::GbaPpu::render_captured_scene, which
// takes the identical two tables directly) -- are driven from the exact same
// synthetic data, so the comparison exercises the per-row plumbing, not
// whether the synthetic pattern resembles anything the game itself does.
int run_per_row_affine_check(const std::string& logs, int limit,
                             gsr::FieldSceneRenderer& renderer,
                             int kW, int kH) {
    std::printf("--per-row-affine: synthetic per-row PA/PD walk plus two "
                "mid-frame reference reloads (y=40, y=100), mode 1/2 "
                "snapshots only\n\n");

    std::string cmd = "dir /b /s \"" + logs + "\\snap_*.bin\" 2>nul";
    std::FILE* p = _popen(cmd.c_str(), "r");
    if (!p) { std::fprintf(stderr, "cannot scan %s\n", logs.c_str()); return 1; }

    constexpr std::size_t kLineIoBytes = gsr::FieldScene::kLineIoBytes;
    constexpr int kRows = gsr::FieldScene::kRows;

    int examined = 0, mode_matched = 0, drawn = 0, identical = 0, differing = 0;
    // Diagnostic: how many drawn frames actually had an affine layer (BG2 in
    // mode 1, BG2 or BG3 in mode 2) enabled -- an identical pass over frames
    // that never touch the affine path would be a hollow test, not evidence.
    int affine_layer_enabled = 0;
    std::map<std::string, int> declines;
    std::vector<std::string> worst;

    bool row_valid[kRows];
    for (int y = 0; y < kRows; ++y) row_valid[y] = true;

    char line[1024];
    while (std::fgets(line, sizeof line, p)) {
        std::size_t n = std::strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!n) continue;
        if (limit > 0 && drawn >= limit) break;
        ++examined;
        if ((examined % 100) == 0)
            std::printf("\r  %d examined, %d mode 1/2, %d drawn", examined,
                       mode_matched, drawn);

        Snapshot s;
        if (!load_snapshot(line, &s)) continue;
        auto it_io = s.sections.find("IO");
        auto it_vram = s.sections.find("VRAM");
        auto it_oam = s.sections.find("OAM");
        auto it_pal = s.sections.find("PAL");
        if (it_io == s.sections.end() || it_vram == s.sections.end() ||
            it_oam == s.sections.end() || it_pal == s.sections.end()) continue;
        std::vector<std::uint8_t> io = it_io->second;
        const auto& vram = it_vram->second;
        std::vector<std::uint8_t> oam = it_oam->second;
        const auto& pal = it_pal->second;
        if (io.size() < 0x60 || vram.size() < 0x18000 || pal.size() < 1024)
            continue;

        const unsigned dispcnt = static_cast<unsigned>(io[0] | (io[1] << 8));
        const int mode = static_cast<int>(dispcnt & 7u);
        if (mode != 1 && mode != 2) continue;
        ++mode_matched;

        std::uint8_t base_io[kLineIoBytes];
        std::memcpy(base_io, io.data(), kLineIoBytes);

        // a) 160-row line_io: each row starts as a copy of the snapshot's own
        // io, then PA/PD (BG2 at 0x20, BG3 at 0x30) are varied per row. PB/PC
        // are left as recorded: render_captured_scene reads PA/PC directly
        // per row for the within-row walk (gba_ppu.cpp:860-863), while PB/PD
        // only feed the reference accumulator built in step b below.
        std::vector<std::uint8_t> line_io(kLineIoBytes * kRows);
        for (int y = 0; y < kRows; ++y) {
            std::uint8_t* row = line_io.data() +
                static_cast<std::size_t>(y) * kLineIoBytes;
            std::memcpy(row, base_io, kLineIoBytes);
            for (int bg = 2; bg <= 3; ++bg) {
                const std::size_t off = (bg == 2) ? 0x20u : 0x30u;
                const std::int32_t base_pa = read_s16_le(base_io, off + 0x00);
                const std::int32_t base_pd = read_s16_le(base_io, off + 0x06);
                const std::int32_t pa = base_pa + (y * 3) % 17 - 8;
                const std::int32_t pd = base_pd + (y * 5) % 13 - 6;
                write_s16_le(row, off + 0x00, pa);
                write_s16_le(row, off + 0x06, pd);
            }
        }

        // b) matching 160x4 affine_line_refs (BG2 x/y, BG3 x/y): start from
        // the snapshot's own BG2X/Y and BG3X/Y (28-bit sign-extended, same as
        // gba_ppu.cpp's read_s28_ref), then walk row to row by the PB/PD of
        // the row just left -- PB is constant (left as recorded above) so
        // this choice only matters for PD, which is documented here rather
        // than guessed silently. Two mid-frame reloads (y=40, y=100) jump the
        // x reference by +0x2000, so the table cannot be reproduced by
        // extrapolating PB/PD from row 0 -- the case the frame-level read
        // got wrong.
        std::vector<std::int32_t> affine_refs(
            static_cast<std::size_t>(kRows) * 4u);
        std::int32_t ref2x = read_s28_ref_le(base_io, 0x28);
        std::int32_t ref2y = read_s28_ref_le(base_io, 0x2C);
        std::int32_t ref3x = read_s28_ref_le(base_io, 0x38);
        std::int32_t ref3y = read_s28_ref_le(base_io, 0x3C);
        for (int y = 0; y < kRows; ++y) {
            if (y > 0) {
                const std::uint8_t* prev = line_io.data() +
                    static_cast<std::size_t>(y - 1) * kLineIoBytes;
                ref2x += read_s16_le(prev, 0x22);
                ref2y += read_s16_le(prev, 0x26);
                ref3x += read_s16_le(prev, 0x32);
                ref3y += read_s16_le(prev, 0x36);
            }
            if (y == 40 || y == 100) { ref2x += 0x2000; ref3x += 0x2000; }
            std::int32_t* r = affine_refs.data() + static_cast<std::size_t>(y) * 4u;
            r[0] = ref2x; r[1] = ref2y; r[2] = ref3x; r[3] = ref3y;
        }

        // c) every row valid, both tables into field_scene_capture.
        gsr::FieldScene scene;
        gsr::field_scene_capture(&scene, s.frame, io.data(), oam.data(),
                                 pal.data(), line_io.data(), row_valid,
                                 affine_refs.data(), row_valid);

        renderer.upload_vram(vram.data(), vram.size());
        std::vector<std::uint16_t> palette(512);
        for (std::size_t i = 0; i < palette.size() && i * 2 + 1 < pal.size(); ++i)
            palette[i] = static_cast<std::uint16_t>(pal[i * 2] | (pal[i * 2 + 1] << 8));
        renderer.upload_palette(palette.data(), palette.size());

        renderer.set_output_size(kW, kH);
        if (!renderer.draw(scene)) {
            declines[renderer.declined_reason() ? renderer.declined_reason()
                                                : "unknown"]++;
            continue;
        }
        ++drawn;
        if (scene.layers[2].enabled || scene.layers[3].enabled)
            ++affine_layer_enabled;

        std::vector<std::uint8_t> gpu(static_cast<std::size_t>(kW) * kH * 4u);
        glBindTexture(GL_TEXTURE_2D, renderer.output_texture());
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, gpu.data());
        glBindTexture(GL_TEXTURE_2D, 0);

        // d) reference side: the same two synthetic tables, straight into
        // render_captured_scene.
        gba::GbaPpu ppu;
        std::vector<std::uint8_t> fb(ppu.render_bytes());
        ppu.render_captured_scene(fb.data(), line_io.data(), affine_refs.data(),
                                  vram.data(), oam.data(), pal.data());

        long long differing_pixels = 0;
        int min_x = kW, max_x = -1, min_y = kH, max_y = -1;
        int sample_x = -1, sample_y = -1;
        for (int y = 0; y < kH; ++y) {
            for (int x = 0; x < kW; ++x) {
                const std::size_t r = (static_cast<std::size_t>(y) * kW + x) * 3u;
                const std::size_t gf =
                    (static_cast<std::size_t>(kH - 1 - y) * kW + x) * 4u;
                if (std::abs(int(gpu[gf + 0]) - int(fb[r + 0])) > 1 ||
                    std::abs(int(gpu[gf + 1]) - int(fb[r + 1])) > 1 ||
                    std::abs(int(gpu[gf + 2]) - int(fb[r + 2])) > 1) {
                    ++differing_pixels;
                    if (x < min_x) min_x = x;
                    if (x > max_x) max_x = x;
                    if (y < min_y) min_y = y;
                    if (y > max_y) max_y = y;
                    if (sample_x < 0) { sample_x = x; sample_y = y; }
                }
            }
        }

        if (differing_pixels == 0) { ++identical; continue; }
        ++differing;
        if (worst.size() < 20) {
            const std::size_t gf =
                (static_cast<std::size_t>(kH - 1 - sample_y) * kW + sample_x) * 4u;
            const std::size_t rf =
                (static_cast<std::size_t>(sample_y) * kW + sample_x) * 3u;
            char buf[640];
            std::snprintf(buf, sizeof buf,
                          "%s (mode %d)\n      %lld of %d differ, box x %d..%d "
                          "y %d..%d\n      first at (%d,%d): gpu (%d,%d,%d) "
                          "hardware (%d,%d,%d)",
                          line, mode, differing_pixels, kW * kH,
                          min_x, max_x, min_y, max_y, sample_x, sample_y,
                          int(gpu[gf]), int(gpu[gf + 1]), int(gpu[gf + 2]),
                          int(fb[rf]), int(fb[rf + 1]), int(fb[rf + 2]));
            worst.push_back(buf);
        }
    }
    _pclose(p);

    std::printf("\r  %d examined, %d mode 1/2, %d drawn            \n\n",
               examined, mode_matched, drawn);
    std::printf("identical to the hardware : %d\n", identical);
    std::printf("differing                 : %d\n", differing);
    std::printf("  of those drawn, had an affine BG (2 or 3) enabled : %d\n",
               affine_layer_enabled);
    if (!declines.empty()) {
        std::printf("\nframes the renderer declined, and why:\n");
        for (const auto& kv : declines)
            std::printf("  %-44s %d\n", kv.first.c_str(), kv.second);
    }
    if (differing) {
        std::printf("\nfirst differences:\n");
        for (const auto& w : worst) std::printf("  %s\n", w.c_str());
    }
    return differing == 0 && drawn > 0 ? 0 : 1;
}

// Set by --rom: the cartridge, for the world map's margins. Empty = none.
std::vector<std::uint8_t> g_replay_rom;

// Set by --hide-bg N: which background the reference leaves out, or -1.
int g_replay_hide_bg = -1;
extern "C" bool g_hide_bg0;
extern "C" bool g_hide_bg1;
extern "C" bool g_hide_bg2;
extern "C" bool g_hide_bg3;

// --replay <file.bin>: replays ONE live-captured GSRGPUF1 frame (written by
// src/runner_main.cpp's F9 GPU field dump) against both the GPU scene
// renderer and the emulated compositor, driven by that frame's own per-row
// register/reference tables -- not the synthetic stand-in
// --per-row-affine uses, and not a single end-of-frame register read the way
// every other mode here is limited to. This is the reason the dump format
// exists at all: a per-scanline register write can only be told apart from
// a whole-frame one by replaying what the console actually did, row by row.
int run_replay(const std::string& path, gsr::FieldSceneRenderer& renderer,
               int kW, int kH) {
    Snapshot s;
    if (!load_snapshot(path.c_str(), &s)) {
        std::fprintf(stderr, "--replay: cannot read %s (missing, or not a "
                              "GSRSNAP1/GSRGPUF1 file)\n", path.c_str());
        return 1;
    }

    auto need = [&](const char* name) -> const std::vector<std::uint8_t>* {
        auto it = s.sections.find(name);
        return it == s.sections.end() ? nullptr : &it->second;
    };
    // IO is not read directly -- LINEIO already substitutes it per row, per
    // gpu_field_write_dump_if_requested's own contract (row_io_valid[y] ?
    // row_io[y] : io_bytes), so this replay never has to know the difference.
    const auto* scene_bytes = need("SCENE");
    const auto* oam         = need("OAM");
    const auto* pal         = need("PAL");
    const auto* vram        = need("VRAM");
    const auto* ewram       = need("EWRAM");
    const auto* line_io     = need("LINEIO");
    const auto* affine_raw  = need("AFFREF");

    if (!scene_bytes || !vram || !oam || !pal || !line_io || !affine_raw) {
        std::fprintf(stderr, "--replay: %s is missing a required section "
                              "(need SCENE, VRAM, OAM, PAL, LINEIO, AFFREF)\n",
                     path.c_str());
        return 1;
    }
    if (scene_bytes->size() != sizeof(gsr::FieldScene)) {
        std::fprintf(stderr, "--replay: %s SCENE section is %zu bytes, "
                              "expected %zu (built by a different scene "
                              "layout)\n",
                     path.c_str(), scene_bytes->size(), sizeof(gsr::FieldScene));
        return 1;
    }
    constexpr std::size_t kLineIoBytes = gsr::FieldScene::kLineIoBytes;
    constexpr int kRows = gsr::FieldScene::kRows;
    const std::size_t want_line_io =
        kLineIoBytes * static_cast<std::size_t>(kRows);
    const std::size_t want_affine =
        static_cast<std::size_t>(kRows) * 4u * sizeof(std::int32_t);
    if (line_io->size() != want_line_io || affine_raw->size() != want_affine) {
        std::fprintf(stderr, "--replay: %s LINEIO/AFFREF section size "
                              "mismatch\n", path.c_str());
        return 1;
    }

    // The game and this tool are built by the same pinned mingw g++
    // (AGENTS.md's toolchain pin), and FieldScene is a plain, trivially
    // copyable struct with no pointer this tool dereferences and no vtable
    // -- so a byte-for-byte copy reproduces exactly what runner_main.cpp
    // captured, without re-deriving anything from the raw registers.
    gsr::FieldScene scene;
    std::memcpy(&scene, scene_bytes->data(), sizeof(scene));

    const std::int32_t* affine_refs =
        reinterpret_cast<const std::int32_t*>(affine_raw->data());

    std::printf("--replay %s (frame %llu)\n\n", path.c_str(),
               static_cast<unsigned long long>(s.frame));
    std::printf("video mode %d%s\n", scene.video_mode,
               scene.forced_blank ? "  (forced blank)" : "");
    std::printf("scene model: valid=%d supported=%d%s%s\n", scene.valid,
               scene.supported,
               scene.supported ? "" : "  reason: ",
               scene.supported ? "" :
                   (scene.unsupported_reason ? scene.unsupported_reason
                                             : "(none given)"));
    for (int i = 0; i < 4; ++i) {
        const gsr::SceneLayer& L = scene.layers[i];
        if (!L.enabled) continue;
        std::printf("  BG%d: priority %d%s%s\n", i, L.priority,
                   L.affine ? ", affine" : "", L.mosaic ? ", mosaic" : "");
    }
    std::printf("WIN0V 0x%04x   BLDCNT 0x%04x\n", scene.effects.win0v,
               scene.effects.blend_control);

    // How many of the 160 rows actually differ from row 0 in each field --
    // the whole point of a live capture. A single end-of-frame register read
    // can never show this; a row that never changes never shows here either,
    // which is itself the honest answer for a frame that does not animate
    // mid-frame.
    auto row_io = [&](int y) {
        return line_io->data() + static_cast<std::size_t>(y) * kLineIoBytes;
    };
    auto row_affine = [&](int y) { return affine_refs + static_cast<std::size_t>(y) * 4; };
    int bldcnt_rows = 0, bldalpha_rows = 0, win0v_rows = 0;
    int bg2x_rows = 0, bg2y_rows = 0;
    for (int y = 1; y < kRows; ++y) {
        if (std::memcmp(row_io(y) + 0x50, row_io(0) + 0x50, 2) != 0) ++bldcnt_rows;
        if (std::memcmp(row_io(y) + 0x52, row_io(0) + 0x52, 2) != 0) ++bldalpha_rows;
        if (std::memcmp(row_io(y) + 0x44, row_io(0) + 0x44, 2) != 0) ++win0v_rows;
        if (row_affine(y)[0] != row_affine(0)[0]) ++bg2x_rows;
        if (row_affine(y)[1] != row_affine(0)[1]) ++bg2y_rows;
    }
    std::printf("rows differing from row 0: BLDCNT %d  BLDALPHA %d  WIN0V %d "
               " BG2X %d  BG2Y %d  (of %d)\n\n",
               bldcnt_rows, bldalpha_rows, win0v_rows, bg2x_rows, bg2y_rows,
               kRows - 1);

    // The emulated compositor, driven by the same per-row tables, at native
    // 240x160 -- the reference this replay checks the GPU renderer against.
    gba::GbaPpu ppu;
    std::vector<std::uint8_t> fb(ppu.render_bytes());
    // --hide-bg N asks the reference to leave one background out, which is
    // how a "which layer covers this pixel" question gets a measured answer
    // instead of a guess. gba_ppu.cpp already carries these switches.
    if (g_replay_hide_bg >= 0 && g_replay_hide_bg < 4) {
        bool* const flags[4] = {&g_hide_bg0, &g_hide_bg1, &g_hide_bg2,
                                &g_hide_bg3};
        *flags[g_replay_hide_bg] = true;
        std::printf("reference drawn with BG%d hidden\n", g_replay_hide_bg);
    }
    ppu.render_captured_scene(fb.data(), line_io->data(), affine_refs,
                              vram->data(), oam->data(), pal->data());
    if (g_replay_hide_bg >= 0 && g_replay_hide_bg < 4) {
        bool* const flags[4] = {&g_hide_bg0, &g_hide_bg1, &g_hide_bg2,
                                &g_hide_bg3};
        *flags[g_replay_hide_bg] = false;
    }

    renderer.upload_vram(vram->data(), vram->size());
    std::vector<std::uint16_t> palette(512);
    for (std::size_t i = 0; i < palette.size() && i * 2 + 1 < pal->size(); ++i)
        palette[i] = static_cast<std::uint16_t>((*pal)[i * 2] |
                                                 ((*pal)[i * 2 + 1] << 8));
    renderer.upload_palette(palette.data(), palette.size());
    if (ewram) renderer.upload_room(ewram->data(), ewram->size());
    renderer.set_room_source_enabled(true);
    // The runner knows whether a game window is open; offline, say so with
    // GSR_REPLAY_MENU=1 to check the menu-sprite confinement.
    renderer.set_menu_open(std::getenv("GSR_REPLAY_MENU") != nullptr);
    renderer.set_reveal_full(std::getenv("GSR_REPLAY_REVEAL_FULL") != nullptr);
    // The camera clamp's own limits (Func_10230 at 0x08010230): four 16.16
    // words at [0x03001E70] + 0xEC (min x), 0xF0 (min y), 0xF4 (max x),
    // 0xF8 (max y); the camera stays in [min, max - screen].
    if (const auto* iw = need("IWRAM"); iw && ewram && iw->size() > 0x1E74) {
        const std::uint32_t s = (*iw)[0x1E70] | ((*iw)[0x1E71] << 8) |
                                ((*iw)[0x1E72] << 16) |
                                (static_cast<std::uint32_t>((*iw)[0x1E73]) << 24);
        const std::uint32_t off = s - 0x02000000u;
        if ((s >> 24) == 2u && off + 0xFC <= ewram->size()) {
            auto w = [&](std::uint32_t o) {
                const auto* p = ewram->data() + off + o;
                return static_cast<std::int32_t>(p[0] | (p[1] << 8) |
                                                 (p[2] << 16) |
                                                 (static_cast<std::uint32_t>(p[3]) << 24));
            };
            std::printf("camera clamp @0x%08X: x %d..%d  y %d..%d (pixels)\n",
                        static_cast<unsigned>(s), w(0xEC) >> 16, w(0xF4) >> 16, w(0xF0) >> 16,
                        w(0xF8) >> 16);
        }
    }
    {
        static gsr::WorldMapSource world_map;
        const auto* iwram = need("IWRAM");
        if (ewram && iwram && !g_replay_rom.empty() &&
            world_map.update(ewram->data(), ewram->size(), iwram->data(),
                             iwram->size(), g_replay_rom.data(),
                             g_replay_rom.size())) {
            renderer.upload_world_map(world_map.tiles().data(),
                                      world_map.generation());
            std::printf("world map: whole map unpacked and checked\n");
            int compared = 0;
            const int agree = renderer.world_map_agreement(scene, &compared);
            std::printf("world map agreement %d%% over %d samples\n", agree,
                        compared);
        } else {
            renderer.upload_world_map(nullptr, 0);
            if (!g_replay_rom.empty())
                std::printf("world map: not used for this frame\n");
        }
    }
    // Exercise the widened-view effect sampling the same way the runner does,
    // so a replay shows what the player would see rather than the console
    // framing. Named from the scene's own registers: a battle's affine,
    // non-wrapping BG2.
    renderer.set_effect_renderer_enabled(true);
    {
        int effect_layer = -1;
        if (scene.video_mode == 1) {
            const gsr::SceneLayer& bg2 = scene.layers[2];
            // Same choice as the runner (runner_main.cpp).
            if (bg2.enabled && bg2.affine && !bg2.wraps)
                effect_layer = 2;
            else
                effect_layer = gsr::FieldSceneRenderer::find_effect_canvas_layer(
                    vram->data(), vram->size(), scene);
        }
        renderer.set_effect_layer(effect_layer);
        const auto stamps = s.sections.find("FXSTAMP");
        const auto artwork = s.sections.find("FXART");
        if (effect_layer >= 0 && stamps != s.sections.end() &&
            artwork != s.sections.end() && !stamps->second.empty() &&
            stamps->second.size() % sizeof(gsr::EffectSpark) == 0) {
            std::vector<gsr::EffectSpark> captured(
                stamps->second.size() / sizeof(gsr::EffectSpark));
            std::memcpy(captured.data(), stamps->second.data(), stamps->second.size());
            renderer.upload_effect_sparks(captured.data(),
                static_cast<int>(captured.size()), artwork->second.data(),
                artwork->second.size(), effect_layer);
        }
        // As the runner: with no sparks this still reads the tint fill.
        if (effect_layer >= 0)
            renderer.analyse_effect_canvas(vram->data(), vram->size(),
                                           scene.layers[effect_layer]);
    }

    renderer.set_output_size(kW, kH);
    if (!renderer.draw(scene)) {
        std::printf("GPU renderer declined this frame: %s\n",
                   renderer.declined_reason() ? renderer.declined_reason()
                                              : "unknown");
        return 1;
    }
    std::vector<std::uint8_t> gpu(static_cast<std::size_t>(kW) * kH * 4u);
    glBindTexture(GL_TEXTURE_2D, renderer.output_texture());
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, gpu.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    long long differing_pixels = 0;
    int min_x = kW, max_x = -1, min_y = kH, max_y = -1;
    int sample_x = -1, sample_y = -1;
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const std::size_t r = (static_cast<std::size_t>(y) * kW + x) * 3u;
            const std::size_t gf =
                (static_cast<std::size_t>(kH - 1 - y) * kW + x) * 4u;
            if (std::abs(int(gpu[gf + 0]) - int(fb[r + 0])) > 1 ||
                std::abs(int(gpu[gf + 1]) - int(fb[r + 1])) > 1 ||
                std::abs(int(gpu[gf + 2]) - int(fb[r + 2])) > 1) {
                ++differing_pixels;
                if (x < min_x) min_x = x;
                if (x > max_x) max_x = x;
                if (y < min_y) min_y = y;
                if (y > max_y) max_y = y;
                if (sample_x < 0) { sample_x = x; sample_y = y; }
            }
        }
    }

    std::printf("examined : 1\n");
    std::printf("identical: %d\n", differing_pixels == 0 ? 1 : 0);
    std::printf("differing: %d\n", differing_pixels == 0 ? 0 : 1);
    if (differing_pixels != 0) {
        const std::size_t gf =
            (static_cast<std::size_t>(kH - 1 - sample_y) * kW + sample_x) * 4u;
        const std::size_t rf =
            (static_cast<std::size_t>(sample_y) * kW + sample_x) * 3u;
        std::printf("  %lld of %d differ, box x %d..%d y %d..%d\n"
                   "  first at (%d,%d): gpu (%d,%d,%d) hardware (%d,%d,%d)\n",
                   differing_pixels, kW * kH, min_x, max_x, min_y, max_y,
                   sample_x, sample_y, int(gpu[gf]), int(gpu[gf + 1]),
                   int(gpu[gf + 2]), int(fb[rf]), int(fb[rf + 1]),
                   int(fb[rf + 2]));
    }

    // Both pictures at their own sizes, next to the input file: the GPU
    // renderer expanded to 360x240 (so any margin content shows too), and
    // the emulated compositor at native 240x160 (the ground truth the check
    // above just compared against).
    renderer.set_output_size(360, 240);
    if (renderer.draw(scene)) {
        std::vector<std::uint8_t> wide_gpu(
            static_cast<std::size_t>(360) * 240 * 4u);
        glBindTexture(GL_TEXTURE_2D, renderer.output_texture());
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     wide_gpu.data());
        glBindTexture(GL_TEXTURE_2D, 0);
        write_png((path + ".gpu.png").c_str(), 360, 240, wide_gpu.data());
        renderer.log_effect_state();
        std::printf("\nwrote %s.gpu.png (360x240, our renderer)\n",
                   path.c_str());
    } else {
        std::printf("\nGPU renderer declined the 360x240 draw: %s\n",
                   renderer.declined_reason() ? renderer.declined_reason()
                                              : "unknown");
    }
    // Why the widened margins look the way they do. A margin pixel can only
    // be filled from the reconstructed room (a field frame widens no regular
    // layer any other way, and BG0 never uses the room at all), so a black
    // one means the room had no answer there. This says which: past the
    // room's own edge, or inside it with a piece missing.
    // Margins exist only in the 360x240 draw above, never in the 240x160
    // comparison, so the margin extents are that draw's, not kW/kH's.
    if (renderer.room_known()) {
        const int lm = (360 - 240) / 2;
        const int tm = (240 - 160) / 2;
        std::printf("\nroom rect x %d..%d  y %d..%d, camera (%d,%d)\n",
                   renderer.room_min_x(), renderer.room_max_x(),
                   renderer.room_min_y(), renderer.room_max_y(),
                   renderer.room_camera_x(), renderer.room_camera_y());
        long long margin = 0, inside_rect = 0, answered = 0;
        for (int y = -tm; y < 160 + tm; ++y) {
            for (int x = -lm; x < 240 + lm; ++x) {
                if (x >= 0 && x < 240 && y >= 0 && y < 160) continue;
                ++margin;
                const int rx = renderer.room_camera_x() + x;
                const int ry = renderer.room_camera_y() + y;
                if (rx >= renderer.room_min_x() && rx < renderer.room_max_x() &&
                    ry >= renderer.room_min_y() && ry < renderer.room_max_y())
                    ++inside_rect;
                bool any = false;
                for (int bg = 1; bg < 4 && !any; ++bg) {
                    const gsr::SceneLayer& L = scene.layers[bg];
                    if (!L.enabled || L.affine) continue;
                    if (renderer.query_room_known(x, y, L.scroll_x, L.scroll_y))
                        any = true;
                }
                if (any) ++answered;
            }
        }
        std::printf("margin pixels %lld: %lld inside the room rect, "
                   "%lld at least one layer can draw (%.1f%%)\n",
                   margin, inside_rect, answered,
                   margin ? 100.0 * double(answered) / double(margin) : 0.0);

        // Per layer, and WHY it cannot draw. "some layer can" hides the case
        // that matters: the layer carrying the scenery refuses while another
        // one answers, and the pixel still comes out as bare backdrop.
        for (int bg = 1; bg < 4; ++bg) {
            const gsr::SceneLayer& L = scene.layers[bg];
            if (!L.enabled || L.affine) continue;
            long long ok = 0, neg_source = 0, off_grid = 0, other = 0;
            for (int y = -tm; y < 160 + tm; ++y) {
                for (int x = -lm; x < 240 + lm; ++x) {
                    if (x >= 0 && x < 240 && y >= 0 && y < 160) continue;
                    if (renderer.query_room_known(x, y, L.scroll_x,
                                                  L.scroll_y)) {
                        ++ok;
                        continue;
                    }
                    // The same tests the shader makes, in the same order.
                    const int sx = L.scroll_x + x;
                    const int sy = L.scroll_y + y;
                    if (sx < 0 || sy < 0) ++neg_source;
                    else if ((sx >> 4) >= 128 || (sy >> 4) >= 128) ++off_grid;
                    else ++other;
                }
            }
            int compared = 0;
            const int agree =
                renderer.room_layer_agreement(scene, bg, &compared);
            std::printf("  BG%d room agreement %d%% over %d tiles\n", bg,
                        agree, compared);
            std::printf("  BG%d (scroll %d,%d): draws %lld of %lld margin "
                       "pixels; refused %lld for a source before the map, "
                       "%lld past the grid, %lld for an unavailable tile\n",
                       bg, L.scroll_x, L.scroll_y, ok, margin, neg_source,
                       off_grid, other);
        }
    }

    renderer.set_output_size(kW, kH);

    std::vector<std::uint8_t> ref_rgba(
        static_cast<std::size_t>(kW) * kH * 4u, 255);
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            const std::size_t d =
                (static_cast<std::size_t>(kH - 1 - y) * kW + x) * 4u;
            const std::size_t srcp = (static_cast<std::size_t>(y) * kW + x) * 3u;
            ref_rgba[d + 0] = fb[srcp + 0];
            ref_rgba[d + 1] = fb[srcp + 1];
            ref_rgba[d + 2] = fb[srcp + 2];
        }
    write_png((path + ".ref.png").c_str(), kW, kH, ref_rgba.data());
    std::printf("wrote %s.ref.png (240x160, emulated compositor)\n",
               path.c_str());

    // The same two pictures the count above actually compared: our renderer
    // at the native 240x160 -- NOT the 360x240 draw written above, which is a
    // second, differently sized draw and so cannot be laid over the
    // reference -- and a red mask of exactly which pixels the loop counted.
    // Without these a replay reports a number with no way to see where it
    // came from, which is how a margin-only fault stayed invisible before.
    write_png((path + ".gpu240.png").c_str(), kW, kH, gpu.data());
    std::printf("wrote %s.gpu240.png (240x160, our renderer, the compared "
               "picture)\n", path.c_str());

    std::vector<std::uint8_t> mask(
        static_cast<std::size_t>(kW) * kH * 4u, 255);
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            const std::size_t r = (static_cast<std::size_t>(y) * kW + x) * 3u;
            const std::size_t d =
                (static_cast<std::size_t>(kH - 1 - y) * kW + x) * 4u;
            const bool differs =
                std::abs(int(gpu[d + 0]) - int(fb[r + 0])) > 1 ||
                std::abs(int(gpu[d + 1]) - int(fb[r + 1])) > 1 ||
                std::abs(int(gpu[d + 2]) - int(fb[r + 2])) > 1;
            mask[d + 0] = differs ? 255 : 0;
            mask[d + 1] = 0;
            mask[d + 2] = 0;
        }
    write_png((path + ".diff.png").c_str(), kW, kH, mask.data());
    std::printf("wrote %s.diff.png (240x160, red where the two differ)\n",
               path.c_str());

    return differing_pixels == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::string logs = "logs";
    std::string dump;
    std::string dump_wide;
    std::string dump_battle;
    std::string dump_battle_wide;
    std::string dump_overworld_wide;
    int limit = 0;
    // --room: source BG1-3 from the room's own tables (id grid + atlas)
    // instead of the console's wrapped tilemap, falling back per pixel.
    // --wide N: additionally draw each frame at N x native-height and check
    // the centre 240x160 stays pixel-identical while the extra width comes
    // from the room buffer (only if --room is also given; otherwise the
    // margin is whatever the video-memory-only path already drew there).
    // --tall N: the same, but N x native-width -- checks the top/bottom
    // bands instead of the left/right margins. --wide and --tall combine to
    // widen both axes at once.
    bool room = false;
    int wide = 0;
    int tall = 0;
    // --per-row-affine: a separate synthetic mode (see run_per_row_affine_check)
    // that exercises the per-row PA/PC/reference plumbing the recorded
    // snapshots cannot, since they hold only one end-of-frame register read.
    bool per_row_affine = false;
    // --replay <file.bin>: a separate mode (see run_replay) that replays ONE
    // live-captured GSRGPUF1 frame instead of scanning a logs directory.
    std::string replay;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--limit") == 0 && i + 1 < argc)
            limit = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--replay") == 0 && i + 1 < argc)
            replay = argv[++i];
        else if (std::strcmp(argv[i], "--rom") == 0 && i + 1 < argc) {
            std::FILE* f = std::fopen(argv[++i], "rb");
            if (f) {
                std::fseek(f, 0, SEEK_END);
                g_replay_rom.resize(static_cast<std::size_t>(std::ftell(f)));
                std::fseek(f, 0, SEEK_SET);
                if (std::fread(g_replay_rom.data(), 1, g_replay_rom.size(), f)
                        != g_replay_rom.size())
                    g_replay_rom.clear();
                std::fclose(f);
            }
        }
        else if (std::strcmp(argv[i], "--hide-bg") == 0 && i + 1 < argc)
            g_replay_hide_bg = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--dump") == 0 && i + 1 < argc)
            dump = argv[++i];
        else if (std::strcmp(argv[i], "--dump-wide") == 0 && i + 1 < argc)
            dump_wide = argv[++i];
        else if (std::strcmp(argv[i], "--dump-battle") == 0 && i + 1 < argc)
            dump_battle = argv[++i];
        else if (std::strcmp(argv[i], "--dump-battle-wide") == 0 &&
                 i + 1 < argc)
            dump_battle_wide = argv[++i];
        else if (std::strcmp(argv[i], "--dump-overworld-wide") == 0 &&
                 i + 1 < argc)
            dump_overworld_wide = argv[++i];
        else if (std::strcmp(argv[i], "--room") == 0)
            room = true;
        else if (std::strcmp(argv[i], "--per-row-affine") == 0)
            per_row_affine = true;
        else if (std::strcmp(argv[i], "--wide") == 0 && i + 1 < argc) {
            wide = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--tall") == 0 && i + 1 < argc) {
            tall = std::atoi(argv[++i]);
        } else if (argv[i][0] != '-') logs = argv[i];
    }

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* win = SDL_CreateWindow("scene renderer check", 0, 0, 64, 64,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    SDL_GLContext ctx = win ? SDL_GL_CreateContext(win) : nullptr;
    if (!ctx) { std::fprintf(stderr, "no GL context: %s\n", SDL_GetError()); return 1; }

    gbarecomp::GpuSurface surface;
    if (!surface.init()) {
        std::fprintf(stderr, "GPU surface: %s\n", surface.failure());
        return 1;
    }
    gsr::FieldSceneRenderer renderer;
    if (!renderer.init(&surface)) {
        std::fprintf(stderr, "scene renderer: %s\n", renderer.failure());
        return 1;
    }
    const int kW = gba::GbaPpu::kScreenWidth, kH = gba::GbaPpu::kScreenHeight;
    if (!renderer.set_output_size(kW, kH)) {
        std::fprintf(stderr, "could not size the output\n");
        return 1;
    }

    if (per_row_affine) {
        const int rc = run_per_row_affine_check(logs, limit, renderer, kW, kH);
        SDL_GL_DeleteContext(ctx);
        SDL_DestroyWindow(win);
        SDL_Quit();
        return rc;
    }

    if (!replay.empty()) {
        const int rc = run_replay(replay, renderer, kW, kH);
        SDL_GL_DeleteContext(ctx);
        SDL_DestroyWindow(win);
        SDL_Quit();
        return rc;
    }

    std::printf("comparing the GPU background against the emulated hardware\n"
                "at the native %dx%d viewport, objects included%s\n\n", kW, kH,
                room ? ", room buffer enabled for BG1-3" : "");

    std::string cmd = "dir /b /s \"" + logs + "\\snap_*.bin\" 2>nul";
    std::FILE* p = _popen(cmd.c_str(), "r");
    if (!p) { std::fprintf(stderr, "cannot scan %s\n", logs.c_str()); return 1; }

    int examined = 0, drawn = 0, identical = 0, differing = 0;
    int with_objects = 0, object_total = 0;
    std::map<std::string, int> declines;
    std::vector<std::string> worst;
    long long worst_pixels = -1;
    std::string worst_path;
    std::vector<std::uint8_t> worst_gpu, worst_ref;

    // Gate a) coverage: of the frames the renderer drew and matched exactly,
    // how many did the room buffer answer for every pixel of every enabled
    // field layer ("fully"), how many for some but not all ("partial",
    // falling back to video memory for the rest, which is why the picture is
    // still identical), and how many not at all (no EWRAM section, or the
    // room rect did not describe a real room).
    int room_no_ewram = 0, room_fully = 0, room_partial = 0, room_none = 0;
    int per_row_snapshots = 0;

    // Gate b): widened/heightened output, centre pixel-identity plus what
    // the margins actually contain. Left/right is the strip beside the
    // native 240x160 window, within its own row range; top/bottom is the
    // full-width band above and below it (see the reporting split below).
    long long wide_frames = 0, wide_center_differing = 0;
    long long wide_margin_lr_room_cells = 0, wide_margin_lr_fallback_cells = 0;
    long long wide_margin_tb_room_cells = 0, wide_margin_tb_fallback_cells = 0;
    bool dumped_wide = false;
    bool dumped_battle = false;
    bool dumped_battle_wide = false;
    bool dumped_overworld_wide = false;
    std::vector<std::string> wide_worst;

    char line[1024];
    while (std::fgets(line, sizeof line, p)) {
        std::size_t n = std::strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!n) continue;
        if (limit > 0 && drawn >= limit) break;
        ++examined;
        if ((examined % 100) == 0)
            std::printf("\r  %d examined, %d drawn", examined, drawn);

        Snapshot s;
        if (!load_snapshot(line, &s)) continue;
        auto it_io = s.sections.find("IO");
        auto it_vram = s.sections.find("VRAM");
        auto it_oam = s.sections.find("OAM");
        auto it_pal = s.sections.find("PAL");
        if (it_io == s.sections.end() || it_vram == s.sections.end() ||
            it_oam == s.sections.end() || it_pal == s.sections.end()) continue;
        std::vector<std::uint8_t> io = it_io->second;
        const auto& vram = it_vram->second;
        std::vector<std::uint8_t> oam = it_oam->second;
        const auto& pal = it_pal->second;
        if (io.size() < 0x60 || vram.size() < 0x18000 || pal.size() < 1024)
            continue;

        // Objects are drawn by both sides now, so the frame is compared exactly
        // as the game produced it.
        const unsigned dispcnt =
            static_cast<unsigned>(io[0] | (io[1] << 8));
        const std::vector<std::uint8_t>& oam_off = oam;

        // Per-row registers when the snapshot carries them. Captures taken
        // before 2026-09-16 have only the frame's last register read, and
        // then this falls back to it -- which is exactly the blindness that
        // let a renderer with a per-scanline fault pass this gate. A corpus
        // re-recorded with the current map_recorder gets checked row by row
        // instead, with no change to how it is run.
        const std::size_t kRows = gsr::FieldScene::kRows;
        const std::size_t kLineIo = gsr::FieldScene::kLineIoBytes;
        const std::uint8_t* row_io_table = nullptr;
        const std::int32_t* row_affine_table = nullptr;
        static std::vector<bool> row_valid_store;
        if (row_valid_store.size() != kRows)
            row_valid_store.assign(kRows, true);
        static std::vector<char> row_valid_flags;
        if (row_valid_flags.size() != kRows)
            row_valid_flags.assign(kRows, 1);
        const bool* row_valid =
            reinterpret_cast<const bool*>(row_valid_flags.data());
        {
            auto it_line = s.sections.find("LINEIO");
            auto it_aff = s.sections.find("AFFREF");
            // Distrust rows that describe a different frame than the memory
            // beside them: snapshots written on 2026-09-16 before the
            // recorder checked for this can carry the PREVIOUS frame's rows
            // (see map_recorder.cpp). DISPCNT is the witness -- if the row
            // copy disagrees with the register file, drive the comparison the
            // old way rather than from a scene that never existed.
            const bool rows_match_frame =
                it_line != s.sections.end() &&
                it_line->second.size() >= kRows * kLineIo &&
                (static_cast<unsigned>(it_line->second[0]) |
                 (static_cast<unsigned>(it_line->second[1]) << 8)) ==
                    (static_cast<unsigned>(io[0]) |
                     (static_cast<unsigned>(io[1]) << 8));
            if (rows_match_frame && it_aff != s.sections.end() &&
                it_aff->second.size() >= kRows * 4u * sizeof(std::int32_t)) {
                row_io_table = it_line->second.data();
                row_affine_table = reinterpret_cast<const std::int32_t*>(
                    it_aff->second.data());
                ++per_row_snapshots;
            }
        }

        gsr::FieldScene scene;
        gsr::field_scene_capture(&scene, s.frame, io.data(), oam.data(),
                                 pal.data(),
                                 row_io_table,
                                 row_io_table ? row_valid : nullptr,
                                 row_affine_table,
                                 row_affine_table ? row_valid : nullptr);

        renderer.upload_vram(vram.data(), vram.size());
        std::vector<std::uint16_t> palette(512);
        for (std::size_t i = 0; i < palette.size() && i * 2 + 1 < pal.size(); ++i)
            palette[i] = static_cast<std::uint16_t>(pal[i * 2] | (pal[i * 2 + 1] << 8));
        renderer.upload_palette(palette.data(), palette.size());

        bool this_room_valid = false;
        if (room) {
            auto it_ewram = s.sections.find("EWRAM");
            if (it_ewram != s.sections.end())
                this_room_valid =
                    renderer.upload_room(it_ewram->second.data(),
                                         it_ewram->second.size());
            renderer.set_room_source_enabled(true);
        }

        renderer.set_output_size(kW, kH);
        if (!renderer.draw(scene)) {
            declines[renderer.declined_reason() ? renderer.declined_reason()
                                                : "unknown"]++;
            continue;
        }
        ++drawn;
        int objects_here = 0;
        for (const auto& o : scene.objects) if (o.present) ++objects_here;
        if (objects_here > 0) ++with_objects;
        object_total += objects_here;

        // draw() has already cleared, drawn and restored state, so the finished
        // picture is sitting in the target texture. Read it straight out.
        std::vector<std::uint8_t> gpu(static_cast<std::size_t>(kW) * kH * 4u);
        glBindTexture(GL_TEXTURE_2D, renderer.output_texture());
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, gpu.data());
        glBindTexture(GL_TEXTURE_2D, 0);

        // The emulated hardware's picture, same frame, objects off.
        gba::GbaPpu ppu;
        std::vector<std::uint8_t> fb(ppu.render_bytes());
        if (row_io_table && row_affine_table) {
            // Drive the reference from the same per-row tables, so the two
            // sides disagree only where the renderer is actually wrong.
            ppu.render_captured_scene(fb.data(), row_io_table,
                                      row_affine_table, vram.data(),
                                      oam.data(), pal.data());
        } else
        ppu.render(fb.data(), static_cast<std::uint16_t>(dispcnt), io.data(),
                   vram.data(), oam_off.data(), pal.data());

        long long differing_pixels = 0;
        int min_x = kW, max_x = -1, min_y = kH, max_y = -1;
        int sample_x = -1, sample_y = -1;
        for (int y = 0; y < kH; ++y) {
            for (int x = 0; x < kW; ++x) {
                const std::size_t g = (static_cast<std::size_t>(y) * kW + x) * 4u;
                const std::size_t r = (static_cast<std::size_t>(y) * kW + x) * 3u;
                // The GPU target is bottom-up; the reference is top-down.
                const std::size_t gf =
                    (static_cast<std::size_t>(kH - 1 - y) * kW + x) * 4u;
                if (std::abs(int(gpu[gf + 0]) - int(fb[r + 0])) > 1 ||
                    std::abs(int(gpu[gf + 1]) - int(fb[r + 1])) > 1 ||
                    std::abs(int(gpu[gf + 2]) - int(fb[r + 2])) > 1) {
                    ++differing_pixels;
                    if (x < min_x) min_x = x;
                    if (x > max_x) max_x = x;
                    if (y < min_y) min_y = y;
                    if (y > max_y) max_y = y;
                    if (sample_x < 0) { sample_x = x; sample_y = y; }
                }
                (void)g;
            }
        }

        if (differing_pixels == 0) {
            ++identical;
            // A demonstration frame for a human to look at: the first
            // pixel-identical battle frame (mode 1 -- BG2 affine arena) that
            // actually has characters on screen and is not a transition
            // frame (those are often mid-fade, i.e. legitimately near-black),
            // native size, GPU output as drawn.
            if (!dump_battle.empty() && !dumped_battle &&
                scene.video_mode == 1 && objects_here > 0 &&
                std::string(line).find("modechange") == std::string::npos) {
                write_png((dump_battle + ".png").c_str(), kW, kH, gpu.data());
                std::printf("\ndumped battle/overworld frame to %s.png -- %s "
                           "(mode %d)\n",
                           dump_battle.c_str(), line, scene.video_mode);
                dumped_battle = true;
            }
            if (room && this_room_valid) {
                long long total_cells = 0, known_cells = 0;
                for (int bg = 1; bg <= 3; ++bg) {
                    const gsr::SceneLayer& L = scene.layers[bg];
                    if (!L.enabled) continue;
                    for (int y = 0; y < kH; ++y)
                        for (int x = 0; x < kW; ++x) {
                            ++total_cells;
                            if (renderer.query_room_known(x, y, L.scroll_x,
                                                          L.scroll_y))
                                ++known_cells;
                        }
                }
                if (total_cells == 0 || known_cells == total_cells) ++room_fully;
                else if (known_cells > 0) ++room_partial;
                else ++room_none;
            } else if (room) {
                ++room_no_ewram;
            }

            // Gate b): re-draw the same frame at a wider and/or taller
            // output through the same camera and check the centre stays
            // pixel-identical.
            if (wide > kW || tall > kH) {
                const int out_w = wide > kW ? wide : kW;
                const int out_h = tall > kH ? tall : kH;
                renderer.set_output_size(out_w, out_h);
                if (renderer.draw(scene)) {
                    ++wide_frames;
                    std::vector<std::uint8_t> wide_gpu(
                        static_cast<std::size_t>(out_w) * out_h * 4u);
                    glBindTexture(GL_TEXTURE_2D, renderer.output_texture());
                    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                                 wide_gpu.data());
                    glBindTexture(GL_TEXTURE_2D, 0);
                    const int center_x0 = (out_w - kW) / 2;
                    const int center_y0 = (out_h - kH) / 2;
                    long long frame_center_diff = 0;
                    int fsx = -1, fsy = -1;
                    for (int y = 0; y < kH; ++y) {
                        for (int x = 0; x < kW; ++x) {
                            const std::size_t wf =
                                (static_cast<std::size_t>(out_h - 1 - (center_y0 + y)) *
                                     out_w +
                                 (center_x0 + x)) * 4u;
                            const std::size_t r =
                                (static_cast<std::size_t>(y) * kW + x) * 3u;
                            if (std::abs(int(wide_gpu[wf + 0]) - int(fb[r + 0])) > 1 ||
                                std::abs(int(wide_gpu[wf + 1]) - int(fb[r + 1])) > 1 ||
                                std::abs(int(wide_gpu[wf + 2]) - int(fb[r + 2])) > 1) {
                                ++frame_center_diff;
                                if (fsx < 0) { fsx = x; fsy = y; }
                            }
                        }
                    }
                    // The battle arena at the expanded width, for a
                    // human to look at: whether the arena reaches the
                    // side margins cannot be read off a pixel count.
                    if (!dump_battle_wide.empty() && !dumped_battle_wide &&
                        scene.video_mode == 1 && objects_here > 4 &&
                        std::string(line).find("modechange") ==
                            std::string::npos) {
                        const unsigned win0v = scene.effects.win0v;
                        const int y1 = int((win0v >> 8) & 0xFFu);
                        const int y2 = int(win0v & 0xFFu);
                        if (y1 < kH && y1 < y2 && y2 >= kH / 2 && y2 <= kH) {
                            write_png((dump_battle_wide + ".png").c_str(),
                                      out_w, out_h, wide_gpu.data());
                            std::printf("\ndumped WIDE battle frame to "
                                        "%s.png -- %s (band rows %d..%d)\n",
                                        dump_battle_wide.c_str(), line,
                                        y1, y2);
                            dumped_battle_wide = true;
                        }
                    }
                    // The overworld at the expanded width: mode 2, whose
                    // two affine layers are the whole picture.
                    if (!dump_overworld_wide.empty() &&
                        !dumped_overworld_wide && scene.video_mode == 2 &&
                        objects_here > 2 &&
                        std::string(line).find("modechange") ==
                            std::string::npos) {
                        write_png((dump_overworld_wide + ".png").c_str(),
                                  out_w, out_h, wide_gpu.data());
                        std::printf("\ndumped WIDE overworld frame to %s.png"
                                    " -- %s\n",
                                    dump_overworld_wide.c_str(), line);
                        dumped_overworld_wide = true;
                    }
                    wide_center_differing += frame_center_diff;
                    if (frame_center_diff > 0 && wide_worst.size() < 20) {
                        const std::size_t wf =
                            (static_cast<std::size_t>(out_h - 1 - (center_y0 + fsy)) *
                                 out_w +
                             (center_x0 + fsx)) * 4u;
                        const std::size_t rf =
                            (static_cast<std::size_t>(fsy) * kW + fsx) * 3u;
                        char buf[320];
                        std::snprintf(buf, sizeof buf,
                                      "%s\n      %lld centre pixels differ, "
                                      "first at (%d,%d): gpu (%d,%d,%d) "
                                      "hardware (%d,%d,%d)",
                                      line, frame_center_diff, fsx, fsy,
                                      int(wide_gpu[wf]), int(wide_gpu[wf + 1]),
                                      int(wide_gpu[wf + 2]), int(fb[rf]),
                                      int(fb[rf + 1]), int(fb[rf + 2]));
                        wide_worst.push_back(buf);
                    }
                    // What the margins contain: for each margin pixel, ask
                    // every enabled field layer whether the room buffer
                    // supplied it, using the SAME console-space coordinate
                    // the shader itself uses (output pixel minus the
                    // centring offset -- query_room_known's documented
                    // contract). Any layer answering counts the cell as room
                    // content; none answering is backdrop/fallback. Reported
                    // as two bands: top/bottom (every column, outside the
                    // centre row range) and left/right (the centre row
                    // range only, outside the centre column range) -- the
                    // two axes the screenshots showed behaving differently.
                    long long frame_lr_room = 0, frame_lr_fallback = 0;
                    long long frame_tb_room = 0, frame_tb_fallback = 0;
                    for (int y = 0; y < out_h; ++y) {
                        const bool row_in_center =
                            y >= center_y0 && y < center_y0 + kH;
                        for (int x = 0; x < out_w; ++x) {
                            const bool col_in_center =
                                x >= center_x0 && x < center_x0 + kW;
                            if (row_in_center && col_in_center) continue;
                            bool any_known = false;
                            for (int bg = 1; bg <= 3 && !any_known; ++bg) {
                                const gsr::SceneLayer& L = scene.layers[bg];
                                if (!L.enabled) continue;
                                if (renderer.query_room_known(
                                        x - center_x0, y - center_y0,
                                        L.scroll_x, L.scroll_y))
                                    any_known = true;
                            }
                            if (row_in_center) {
                                if (any_known) ++frame_lr_room;
                                else ++frame_lr_fallback;
                            } else {
                                if (any_known) ++frame_tb_room;
                                else ++frame_tb_fallback;
                            }
                        }
                    }
                    wide_margin_lr_room_cells += frame_lr_room;
                    wide_margin_lr_fallback_cells += frame_lr_fallback;
                    wide_margin_tb_room_cells += frame_tb_room;
                    wide_margin_tb_fallback_cells += frame_tb_fallback;

                    // A demonstration frame for a human to look at: the first
                    // one where the room buffer supplies most of the margin
                    // (plenty of real room around the visible area, not just
                    // fallback). Written once, at the size actually drawn.
                    const long long frame_margin_room =
                        frame_lr_room + frame_tb_room;
                    const long long frame_margin_total = frame_margin_room +
                        frame_lr_fallback + frame_tb_fallback;
                    if (!dump_wide.empty() && !dumped_wide &&
                        frame_center_diff == 0 && frame_margin_total > 0 &&
                        frame_margin_room * 2 > frame_margin_total) {
                        write_png((dump_wide + ".png").c_str(), out_w, out_h,
                                 wide_gpu.data());
                        std::printf("\ndumped wide frame to %s.png -- %s\n"
                                   "  margin: %lld/%lld cells from the room "
                                   "buffer\n",
                                   dump_wide.c_str(), line,
                                   frame_margin_room, frame_margin_total);
                        dumped_wide = true;
                    }
                }
                renderer.set_output_size(kW, kH);
            }
            continue;
        }
        ++differing;
        if (worst_pixels < 0) {
            worst_pixels = differing_pixels;
            worst_path = line;
            worst_gpu = gpu;
            worst_ref.assign(fb.begin(), fb.end());
        }
        if (worst.size() < 20) {
            const std::size_t gf =
                (static_cast<std::size_t>(kH - 1 - sample_y) * kW + sample_x) * 4u;
            const std::size_t rf =
                (static_cast<std::size_t>(sample_y) * kW + sample_x) * 3u;
            char buf[640];
            std::snprintf(buf, sizeof buf,
                          "%s\n      %lld of %d differ, box x %d..%d y %d..%d"
                          "\n      first at (%d,%d): gpu (%d,%d,%d) "
                          "hardware (%d,%d,%d)",
                          line, differing_pixels, kW * kH,
                          min_x, max_x, min_y, max_y, sample_x, sample_y,
                          int(gpu[gf]), int(gpu[gf + 1]), int(gpu[gf + 2]),
                          int(fb[rf]), int(fb[rf + 1]), int(fb[rf + 2]));
            worst.push_back(buf);
            // Name the objects covering the first differing pixel, so a
            // failure points at a sprite rather than a coordinate.
            for (int i = 0; i < gsr::FieldScene::kObjects; ++i) {
                const auto& o = scene.objects[i];
                if (!o.present) continue;
                const int bw = (o.affine && o.double_size) ? o.width * 2 : o.width;
                const int bh = (o.affine && o.double_size) ? o.height * 2 : o.height;
                if (sample_x < o.x || sample_x >= o.x + bw ||
                    sample_y < o.y || sample_y >= o.y + bh) continue;
                char ob[320];
                std::snprintf(ob, sizeof ob,
                              "      slot %d: %dx%d at (%d,%d) pri %d tile %d "
                              "pal %d%s%s%s",
                              i, o.width, o.height, o.x, o.y, o.priority,
                              int(o.tile), o.palette,
                              o.affine ? " ROTATED" : "",
                              o.double_size ? " DOUBLE" : "",
                              o.blended ? " BLENDED" : "");
                worst.push_back(ob);
            }
        }
    }
    _pclose(p);

    std::printf("\r  %d examined, %d drawn            \n\n", examined, drawn);
    std::printf("snapshots with per-row registers : %d of %d%s\n",
               per_row_snapshots, examined,
               per_row_snapshots == 0
                   ? "  (pre-2026-09-16 corpus: this gate cannot see a "
                     "per-scanline fault)"
                   : "");
    std::printf("identical to the hardware : %d\n", identical);
    std::printf("  of those, frames with characters on screen : %d\n",
                with_objects);
    std::printf("  characters drawn in total                  : %d\n",
                object_total);
    std::printf("differing                 : %d\n", differing);
    if (!declines.empty()) {
        std::printf("\nframes the renderer declined, and why:\n");
        for (const auto& kv : declines)
            std::printf("  %-44s %d\n", kv.first.c_str(), kv.second);
    }
    if (room) {
        std::printf("\nroom buffer, native %dx%d, camera at the console's own "
                    "origin:\n", kW, kH);
        std::printf("  fully supplied by the room buffer   : %d\n", room_fully);
        std::printf("  partly supplied, rest fell back      : %d\n", room_partial);
        std::printf("  not supplied at all (fell back whole): %d\n", room_none);
        std::printf("  no EWRAM section in the snapshot     : %d\n", room_no_ewram);
    }
    if (wide > kW || tall > kH) {
        const int out_w = wide > kW ? wide : kW;
        const int out_h = tall > kH ? tall : kH;
        std::printf("\nroom buffer, widened/heightened output %dx%d through "
                    "the same camera:\n", out_w, out_h);
        std::printf("  frames drawn                         : %lld\n", wide_frames);
        std::printf("  centre 240x160 differing pixels total: %lld\n",
                    wide_center_differing);
        std::printf("  left/right margin cells from the room buffer : %lld\n",
                    wide_margin_lr_room_cells);
        std::printf("  left/right margin cells backdrop/fallback    : %lld\n",
                    wide_margin_lr_fallback_cells);
        std::printf("  top/bottom band cells from the room buffer   : %lld\n",
                    wide_margin_tb_room_cells);
        std::printf("  top/bottom band cells backdrop/fallback      : %lld\n",
                    wide_margin_tb_fallback_cells);
        if (wide_center_differing > 0) {
            std::printf("\nfirst centre differences (widened output):\n");
            for (const auto& w : wide_worst) std::printf("  %s\n", w.c_str());
        }
    }
    if (differing) {
        std::printf("\nfirst differences:\n");
        for (const auto& w : worst) std::printf("  %s\n", w.c_str());
        if (!dump.empty() && !worst_gpu.empty()) {
            write_png((dump + "_gpu.png").c_str(), kW, kH, worst_gpu.data());
            // The hardware picture beside it, converted to the same layout.
            std::vector<std::uint8_t> ref(
                static_cast<std::size_t>(kW) * kH * 4u, 255);
            for (int y = 0; y < kH; ++y)
                for (int x = 0; x < kW; ++x) {
                    const std::size_t d =
                        (static_cast<std::size_t>(kH - 1 - y) * kW + x) * 4u;
                    const std::size_t srcp =
                        (static_cast<std::size_t>(y) * kW + x) * 3u;
                    ref[d + 0] = worst_ref[srcp + 0];
                    ref[d + 1] = worst_ref[srcp + 1];
                    ref[d + 2] = worst_ref[srcp + 2];
                }
            write_png((dump + "_hardware.png").c_str(), kW, kH, ref.data());
            std::printf("\nworst case written to %s_gpu.png\n", dump.c_str());
        }
    }

    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return differing == 0 && drawn > 0 && wide_center_differing == 0 ? 0 : 1;
}

namespace {

// Minimal PNG writer, so a failure can be looked at rather than guessed about.
void write_png(const char* path, int w, int h, const std::uint8_t* rgba) {
    auto crc32 = [](const std::uint8_t* d, std::size_t n) {
        static std::uint32_t t[256];
        static bool built = false;
        if (!built) {
            for (std::uint32_t i = 0; i < 256; ++i) {
                std::uint32_t c = i;
                for (int k = 0; k < 8; ++k)
                    c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                t[i] = c;
            }
            built = true;
        }
        std::uint32_t c = 0xFFFFFFFFu;
        for (std::size_t i = 0; i < n; ++i) c = t[(c ^ d[i]) & 0xFF] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    };
    // Store-only deflate: no compression library needed for a diagnostic.
    std::vector<std::uint8_t> raw;
    for (int y = h - 1; y >= 0; --y) {           // flip back to top-down
        raw.push_back(0);
        const std::uint8_t* row = rgba + static_cast<std::size_t>(y) * w * 4;
        raw.insert(raw.end(), row, row + static_cast<std::size_t>(w) * 4);
    }
    std::vector<std::uint8_t> z;
    z.push_back(0x78); z.push_back(0x01);
    std::size_t pos = 0;
    while (pos < raw.size()) {
        const std::size_t block = std::min<std::size_t>(65535, raw.size() - pos);
        z.push_back(pos + block >= raw.size() ? 1 : 0);
        z.push_back(block & 0xFF); z.push_back((block >> 8) & 0xFF);
        z.push_back(~block & 0xFF); z.push_back((~block >> 8) & 0xFF);
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + block);
        pos += block;
    }
    std::uint32_t a = 1, b = 0;
    for (std::uint8_t v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
    const std::uint32_t adler = (b << 16) | a;
    for (int i = 3; i >= 0; --i) z.push_back((adler >> (i * 8)) & 0xFF);

    std::FILE* f = std::fopen(path, "wb");
    if (!f) return;
    const std::uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::fwrite(sig, 1, 8, f);
    auto chunk = [&](const char* type, const std::vector<std::uint8_t>& data) {
        std::uint8_t len[4] = {
            std::uint8_t((data.size() >> 24) & 0xFF),
            std::uint8_t((data.size() >> 16) & 0xFF),
            std::uint8_t((data.size() >> 8) & 0xFF),
            std::uint8_t(data.size() & 0xFF)};
        std::fwrite(len, 1, 4, f);
        std::vector<std::uint8_t> tc(type, type + 4);
        tc.insert(tc.end(), data.begin(), data.end());
        std::fwrite(tc.data(), 1, tc.size(), f);
        const std::uint32_t c = crc32(tc.data(), tc.size());
        std::uint8_t cb[4] = {std::uint8_t((c >> 24) & 0xFF),
                              std::uint8_t((c >> 16) & 0xFF),
                              std::uint8_t((c >> 8) & 0xFF),
                              std::uint8_t(c & 0xFF)};
        std::fwrite(cb, 1, 4, f);
    };
    std::vector<std::uint8_t> ihdr = {
        std::uint8_t((w >> 24) & 0xFF), std::uint8_t((w >> 16) & 0xFF),
        std::uint8_t((w >> 8) & 0xFF),  std::uint8_t(w & 0xFF),
        std::uint8_t((h >> 24) & 0xFF), std::uint8_t((h >> 16) & 0xFF),
        std::uint8_t((h >> 8) & 0xFF),  std::uint8_t(h & 0xFF),
        8, 6, 0, 0, 0};
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    std::fclose(f);
}

}  // namespace
