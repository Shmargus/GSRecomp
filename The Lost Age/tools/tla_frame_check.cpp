// tla_frame_check -- draws one saved frame three ways, offline.
//
//   tla_frame_check <F12 folder or frame.bin> [out prefix] [ROM] [--check-views]
//
// Reads the folder's gpu_frame.bin (src/tla_gpu_field.cpp: the registers,
// objects, palettes, video memory and per-row tables the graphics card was
// handed). TLAGPUF2 rewind frames carry their own memory, room copies,
// placements and effect history; legacy TLAGPUF1 uses the folder's memory.bin.
// Writes
//   <prefix>_console.ppm  the console compositor (GbaPpu::render_captured_scene)
//   <prefix>_gpu.ppm      the graphics card at 240x160
//   <prefix>_wide.ppm     the graphics card at 360x240 with the room source
// and prints how many native pixels differ (compared as 5-bit colours) and
// what the room source made of the room. Needs a GL context, from a hidden
// window; nothing of the game runs.

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

#include "field_scene.h"
#include "field_scene_renderer.h"
#include "world_map_source.h"
#include "gba_ppu.h"
#include "gpu_surface.h"
#include "tla_water_ripple.h"
#include "tla_tornado_margins.h"
#include "tla_screen_effect_margins.h"
#include "tla_ship_water_margins.h"
#include "tla_room_palette.h"
#include "tla_frame_capture.h"

namespace {

bool read_file(const std::string& path, std::vector<std::uint8_t>* out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    out->resize(static_cast<std::size_t>(std::ftell(f)));
    std::fseek(f, 0, SEEK_SET);
    const bool ok = std::fread(out->data(), 1, out->size(), f) == out->size();
    std::fclose(f);
    return ok;
}

struct Frame {
    std::uint64_t frame = 0;
    const std::uint8_t *io, *oam, *pal, *vram, *row_io, *row_io_valid;
    const std::uint8_t *row_affine, *row_affine_valid;
};

void write_ppm(const std::string& path, const std::uint8_t* rgb, int w, int h) {
    if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fprintf(f, "P6\n%d %d\n255\n", w, h);
        std::fwrite(rgb, 1, static_cast<std::size_t>(w) * h * 3, f);
        std::fclose(f);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: tla_frame_check <F12 folder or frame.bin> [out prefix] [ROM] [--check-views]\n");
        return 2;
    }
    const std::filesystem::path input = argv[1];
    std::error_code path_error;
    const bool folder = std::filesystem::is_directory(input, path_error);
    const auto frame_path = folder ? input / "gpu_frame.bin" : input;
    const auto dir = folder ? input : input.parent_path();
    const std::string prefix = argc > 2 ? argv[2]
        : folder ? (input / "check").string() : input.string() + ".check";
    const bool check_views = argc > 4 && std::strcmp(argv[4], "--check-views") == 0;
    std::vector<std::uint8_t> gpu_bytes, memory;
    if (!read_file(frame_path.string(), &gpu_bytes)) {
        std::fprintf(stderr, "cannot read %s\n", frame_path.string().c_str());
        return 1;
    }
    tla_frame::View saved;
    if (!tla_frame::parse(gpu_bytes, &saved)) {
        std::fprintf(stderr, "frame is not a valid TLAGPUF1/TLAGPUF2 file\n");
        return 1;
    }
    Frame fr{saved.frame, saved.find("IO").data, saved.find("OAM").data,
        saved.find("PAL").data, saved.find("VRAM").data,
        saved.find("ROWIO").data, saved.find("ROWVALID").data,
        saved.find("AFFREF").data, saved.find("AFFVALID").data};
    const auto state = saved.find("STATE"), objects = saved.find("OBJECTS");
    const auto room_vram = saved.find("ROOMVRAM"), room_pal = saved.find("ROOMPAL");
    const auto fx_stamps = saved.find("FXSTAMP"), fx_art = saved.find("FXART");
    std::vector<gsr::EffectSpark> stamps;
    std::vector<std::uint16_t> room_palette;
    if (!saved.legacy) {
        const auto ewram = saved.find("EWRAM"), iwram = saved.find("IWRAM");
        if (ewram.size != 0x40000 || iwram.size != 0x8000 ||
            state.size != 8 || objects.size != 128 * 12 ||
            (room_vram.size && room_vram.size != 0x18000) ||
            (room_pal.size && room_pal.size != 0x400) ||
            fx_stamps.size % 36 ||
            fx_stamps.size / 36 > std::size_t(std::numeric_limits<int>::max()) ||
            !tla_frame::unpack_stamps(saved, &stamps)) {
            std::fprintf(stderr, "invalid saved memory, room or effect sections\n");
            return 1;
        }
        memory.assign(ewram.data, ewram.data + ewram.size);
        memory.insert(memory.end(), iwram.data, iwram.data + iwram.size);
        room_palette.resize(room_pal.size / 2);
        for (std::size_t i = 0; i < room_palette.size(); ++i)
            room_palette[i] = std::uint16_t(room_pal.data[i * 2]) |
                (std::uint16_t(room_pal.data[i * 2 + 1]) << 8);
    } else {
        if (!read_file((dir / "memory.bin").string(), &memory)) memory.clear();
    }
    const bool have_memory = memory.size() >= 0x40000;
    const std::uint32_t state_flags = saved.legacy ? 0 : tla_frame::read32(state.data);
    const unsigned ripple_phase = saved.legacy
        ? (memory.size() >= 0x4122D ? memory[0x4122C] : 0)
        : tla_frame::read32(state.data + 4);
    std::vector<std::uint8_t> rom;
    if (argc > 3 && (!read_file(argv[3], &rom) || rom.size() < 0x20A8 + 1024 * 2)) {
        std::fprintf(stderr, "cannot read the water-ripple asset from ROM\n");
        return 1;
    }

    // bool tables in the file are one byte each, as the PPU's.
    bool io_valid[gsr::FieldScene::kRows], affine_valid[gsr::FieldScene::kRows];
    std::array<std::int32_t, gsr::FieldScene::kRows * 4> affine{};
    for (std::size_t i = 0; i < affine.size(); ++i)
        affine[i] = static_cast<std::int32_t>(tla_frame::read32(fr.row_affine + i * 4));
    for (int y = 0; y < gsr::FieldScene::kRows; ++y) {
        io_valid[y] = fr.row_io_valid[y] != 0;
        affine_valid[y] = fr.row_affine_valid[y] != 0;
    }
    gsr::FieldScene scene{};
    gsr::SceneMargins margins;
    gsr::field_scene_capture(&scene, fr.frame, fr.io, fr.oam, fr.pal, fr.row_io,
                             io_valid,
                             affine.data(),
                             affine_valid);
    if (!saved.legacy) {
        for (std::size_t i = 0; i < scene.objects.size(); ++i) {
            const auto* p = objects.data + i * 12;
            scene.objects[i].x = static_cast<std::int32_t>(tla_frame::read32(p));
            scene.objects[i].y = static_cast<std::int32_t>(tla_frame::read32(p + 4));
            // Bit 0 parked; bits 16+ the drawn tile number (0 in older files).
            const std::uint32_t flags = tla_frame::read32(p + 8);
            scene.objects[i].parked = (flags & 1u) != 0;
            if (flags >> 16) scene.objects[i].tile = flags >> 16;
        }
        if ((state_flags & 2u) == 0u) {
            scene.supported = false;
            scene.unsupported_reason = "saved source was unsupported";
        }
    }
    std::printf("frame %llu: DISPCNT %04X, described %s%s%s\n",
                static_cast<unsigned long long>(fr.frame), scene.dispcnt,
                scene.supported ? "yes" : "no",
                scene.unsupported_reason ? ", " : "",
                scene.unsupported_reason ? scene.unsupported_reason : "");

    // The console compositor, from the same rows.
    std::vector<std::uint8_t> rows(gsr::FieldScene::kRows * gsr::FieldScene::kLineIoBytes);
    for (int y = 0; y < gsr::FieldScene::kRows; ++y)
        std::memcpy(rows.data() + y * gsr::FieldScene::kLineIoBytes,
                    io_valid[y] ? fr.row_io + y * gsr::FieldScene::kLineIoBytes : fr.io,
                    gsr::FieldScene::kLineIoBytes);
    std::vector<std::uint8_t> console(240 * 160 * 3);
    {
        static gba::GbaPpu ppu;
        ppu.render_captured_scene(console.data(), rows.data(),
                                  affine.data(),
                                  fr.vram, fr.oam, fr.pal);
        write_ppm(prefix + "_console.ppm", console.data(), 240, 160);
    }

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_Window* win = SDL_CreateWindow("tla_frame_check", 0, 0, 64, 64,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    SDL_GLContext ctx = win ? SDL_GL_CreateContext(win) : nullptr;
    if (!ctx) {
        std::fprintf(stderr, "no GL context: %s\n", SDL_GetError());
        return 1;
    }
    int status = 0;
    {
        gbarecomp::GpuSurface surface;
        gsr::FieldSceneRenderer renderer;
        if (!surface.init() || !renderer.init(&surface)) {
            std::fprintf(stderr, "renderer: %s\n",
                         !surface.ready() ? surface.failure() : renderer.failure());
            status = 1;
        } else {
            gsr::WorldMapSource world_map;
            const auto camhold = saved.find("CAMHOLD");
            const int camera_hold_y = camhold.size == 4
                ? static_cast<std::int32_t>(tla_frame::read32(camhold.data)) : 0;
            // Each size starts from the saved frame: the tornado rule rewrites
            // rows (tla_tornado_margins).
            const gsr::FieldScene saved_scene = scene;
            auto draw = [&](int w, int h, bool room, const std::string& name,
                            std::vector<std::uint8_t>* out) {
                if (!renderer.set_output_size(w, h)) return false;
                scene = saved_scene;
                margins.layers[2].scroll.clear();
                tla_tornado_margins(scene, margins,
                    room && memory.size() >= 0x42A
                        ? unsigned(memory[0x428]) | (unsigned(memory[0x429]) << 8) : 0,
                    -(h - 160) / 2, room ? camera_hold_y : 0);
                // The game's iris, when the frame saved one (16 bytes).
                if (const auto iris = saved.find("IRIS"); iris.size == 16) {
                    float v[4];
                    std::memcpy(v, iris.data, sizeof v);
                    margins.iris = {true, v[0], v[1], v[2], v[3]};
                    std::printf("iris: game's own\n");
                }
                // The second sprite page follows when the file has one.
                std::vector<std::uint8_t> vram(fr.vram, fr.vram + 0x18000);
                const auto objx = saved.find("OBJX");
                if (objx.size == 0x8000) vram.insert(vram.end(), objx.data, objx.data + objx.size);
                const bool haze = tla_screen_effect_margins(scene, margins,
                    room && memory.size() >= 0x42A
                        ? unsigned(memory[0x428]) | (unsigned(memory[0x429]) << 8) : 0,
                    vram);
                bool port = false;
                if (room && !rom.empty() && (!saved.legacy || memory.size() >= 0x4122D)) {
                    const auto sine = [&](unsigned index) {
                        const auto at = 0x20A8u + index * 2u;
                        return static_cast<std::int16_t>(rom[at] | (rom[at + 1] << 8));
                    };
                    const bool ripple = tla_water_ripple(scene, margins, ripple_phase,
                                                         sine, -(h - 160) / 2, h);
                    std::printf("water ripple: %s\n", ripple ? "matched" : "not matched");
                    port = tla_port_wave(scene, margins, ripple_phase,
                                         sine, -(h - 160) / 2, h);
                    std::printf("port wave: %s\n", port ? "matched" : "not matched");
                }
                std::vector<std::uint8_t> haze_vram;
                if ((haze && tla_haze_fill_rows(vram, scene, &haze_vram)) ||
                    (port && tla_haze_fill_rows(vram, scene, &haze_vram, 2))) {
                    std::printf("haze rows: refilled\n");
                    vram.swap(haze_vram);
                }
                if (tla_ship_water_margins(scene, margins,
                        room && memory.size() >= 0x42A
                            ? unsigned(memory[0x428]) | (unsigned(memory[0x429]) << 8) : 0,
                        vram)) {
                    std::printf("ship water: repeated\n");
                }
                renderer.upload_vram(vram.data(), vram.size());
                renderer.upload_palette(scene.palette.data(), scene.palette.size());
                if (!saved.legacy) {
                    if (room_vram.size)
                        renderer.upload_room_vram(room_vram.data, room_vram.size);
                    if (!room_palette.empty()) {
                        auto colours = room_palette;
                        if (memory.size() >= 0x48000)
                            tla_restore_room_palette((state_flags & 1u) != 0u,
                                memory.data(), 0x40000, memory.data() + 0x40000,
                                0x8000, &colours);
                        renderer.upload_room_palette(colours.data(), colours.size());
                    }
                    renderer.set_menu_open((state_flags & 1u) != 0u);
                }
                renderer.set_room_source_enabled(room);
                if (room && have_memory) {
                    const bool known = renderer.upload_room(memory.data(), memory.size());
                    std::printf("room: %s, world loaded %s, rect x %d..%d y %d..%d, "
                                "camera %d,%d\n",
                                known ? "known" : "not known",
                                renderer.world_loaded() ? "yes" : "no",
                                renderer.room_min_x(), renderer.room_max_x(),
                                renderer.room_min_y(), renderer.room_max_y(),
                                renderer.room_camera_x(), renderer.room_camera_y());
                    for (int bg = 1; bg <= 3; ++bg) {
                        int compared = 0;
                        const int agree = renderer.room_layer_agreement(scene, bg, &compared);
                        std::printf("  BG%d room agreement %d%% of %d tiles\n", bg,
                                    agree, compared);
                    }
                }
                if (room && memory.size() >= 0x48000 &&
                    world_map.update(memory.data(), 0x40000, memory.data() + 0x40000,
                        0x8000, rom.data(), rom.size())) {
                    renderer.upload_world_map(world_map.tiles().data(), world_map.generation());
                    int compared = 0;
                    const int agreement = renderer.world_map_agreement(scene, &compared);
                    std::printf("world map agreement %d%% of %d tiles\n", agreement, compared);
                } else {
                    renderer.upload_world_map(nullptr, 0);
                }
                if (!saved.legacy) {
                    renderer.set_effect_renderer_enabled(room);
                    int effect_layer = -1;
                    if (room && scene.video_mode == 1) {
                        const auto& bg2 = scene.layers[2];
                        effect_layer = bg2.enabled && bg2.affine && !bg2.wraps
                            ? 2 : gsr::FieldSceneRenderer::find_effect_canvas_layer(
                                fr.vram, 0x18000, scene);
                    }
                    renderer.set_effect_layer(effect_layer);
                    if (effect_layer >= 0) {
                        if (!stamps.empty())
                            renderer.upload_effect_sparks(stamps.data(), int(stamps.size()),
                                fx_art.data, fx_art.size, effect_layer);
                        renderer.analyse_effect_canvas(fr.vram, 0x18000,
                            scene.layers[effect_layer]);
                    }
                }
                renderer.set_scene_margins(&margins);
                if (!renderer.draw(scene)) {
                    std::printf("%s: declined, %s\n", name.c_str(),
                                renderer.declined_reason());
                    return false;
                }
                out->resize(static_cast<std::size_t>(w) * h * 3);
                surface.read_texture_rgb(renderer.output_texture(), w, h, out->data());
                write_ppm(prefix + "_" + name + ".ppm", out->data(), w, h);
                return true;
            };
            std::vector<std::uint8_t> gpu, wide;
            if (draw(240, 160, false, "gpu", &gpu)) {
                int differ = 0;
                for (int i = 0; i < 240 * 160; ++i)
                    for (int c = 0; c < 3; ++c)
                        if ((gpu[i * 3 + c] >> 3) != (console[i * 3 + c] >> 3)) {
                            ++differ;
                            break;
                        }
                std::printf("native: %d of 38400 pixels differ from the console\n",
                            differ);
            } else if (!saved.legacy) {
                status = 1;
            }
            if (!draw(360, 240, true, "wide", &wide) && !saved.legacy) status = 1;
            if (!saved.legacy &&
                (saved.width != 360 || saved.height != 240) &&
                (saved.width != 240 || saved.height != 160)) {
                std::vector<std::uint8_t> actual;
                if (!draw(int(saved.width), int(saved.height), true, "saved_view", &actual))
                    status = 1;
            }
            if (check_views) {
                const int views[][2] = {{320,240},{360,225},{360,203},
                    {480,320},{480,270},{320,180},{383,160},{240,160}};
                for (const auto& view : views) {
                    const int w = view[0], h = view[1];
                    std::vector<std::uint8_t> resized;
                    const std::string name = "view_" + std::to_string(w) + "x" + std::to_string(h);
                    if (gpu.empty() || !draw(w, h, w != 240 || h != 160, name, &resized)) {
                        std::printf("check-views %dx%d: draw failed\n", w, h);
                        status = 1;
                        continue;
                    }
                    int bytes = 0, pixels = 0;
                    const int left = (w - 240) / 2, top = (h - 160) / 2;
                    for (int y = 0; y < 160; ++y)
                        for (int x = 0; x < 240; ++x) {
                            bool exceeds = false;
                            for (int c = 0; c < 3; ++c) {
                                const int delta = std::abs(int(resized[((y + top) * w + x + left) * 3 + c]) -
                                    int(gpu[(y * 240 + x) * 3 + c]));
                                bytes += delta != 0;
                                exceeds |= delta > 1;
                            }
                            pixels += exceeds;
                        }
                    std::printf("check-views %dx%d: byte mismatches %d, pixels delta>1 %d\n", w, h, bytes, pixels);
                    if (pixels) status = 1;
                }
            }
            // A battle frame again with Golden Sun 1's spell-effect layer
            // handling (its gpu_field_present_override): the layer carrying
            // the spell's canvas is named, and its margins draw the canvas's
            // fill instead of being cut off or repeating.
            if (scene.video_mode == 1) {
                const gsr::SceneLayer& bg2 = scene.layers[2];
                int effect_layer =
                    (bg2.enabled && bg2.affine && !bg2.wraps)
                        ? 2
                        : gsr::FieldSceneRenderer::find_effect_canvas_layer(
                              fr.vram, 0x18000, scene);
                std::printf("effect layer %d (BG2 %s%s%s)\n", effect_layer,
                            bg2.enabled ? "on" : "off",
                            bg2.affine ? ", affine" : "",
                            bg2.wraps ? ", wraps" : "");
                renderer.set_effect_renderer_enabled(true);
                renderer.set_effect_layer(effect_layer);
                if (effect_layer >= 0)
                    renderer.analyse_effect_canvas(fr.vram, 0x18000,
                                                   scene.layers[effect_layer]);
                std::vector<std::uint8_t> fx;
                draw(360, 240, true, "wide_fx", &fx);
            }
        }
    }
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return status;
}
