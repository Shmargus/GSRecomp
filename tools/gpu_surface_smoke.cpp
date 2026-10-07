// tools/gpu_surface_smoke.cpp -- does the GPU surface actually work on this
// machine?
//
// Shaders, integer textures and framebuffers are compiled and validated by the
// graphics DRIVER at runtime, so compiling this code proves nothing about
// whether it runs. This checks the things that can only fail on real hardware,
// in seconds, rather than finding them after a 15-20 minute game link.
//
// It checks, in order:
//   1. a GL context can be made and the entry points load;
//   2. an offscreen target can be created and is complete;
//   3. a textured quad samples its texture with the right orientation;
//   4. a 16-bit integer texture can be sampled from a shader -- which is how
//      the scene renderer will read map entries and palettes, and is the part
//      most likely to be missing on an old driver.
//
// Check 3 earns its place: an earlier version only checked the clear colour,
// which passed while the texture was never sampled at all and hid a real bug
// in the vertex attribute binding.
//
// Build:
//   g++ -O2 -std=gnu++17 -I gbarecomp/src/runtime \
//       -I C:/msys64/mingw64/include/SDL2 -o gpu_surface_smoke.exe \
//       tools/gpu_surface_smoke.cpp gbarecomp/src/runtime/gpu_surface.cpp \
//       -lmingw32 -lSDL2main -lSDL2 -lopengl32
//
// Not part of the product.
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_opengl.h>

#include <cstdio>
#include <cstdint>
#include <vector>

#include "gpu_surface.h"

namespace {

int failures = 0;

void check(bool ok, const char* what, const char* detail = nullptr) {
    std::printf("  [%s] %s", ok ? "ok  " : "FAIL", what);
    if (!ok && detail && *detail) std::printf("  -- %s", detail);
    std::printf("\n");
    if (!ok) ++failures;
}

// Reads the offscreen target back so the test checks real pixels rather than
// trusting that the draw happened.
std::vector<std::uint8_t> read_target(int w, int h) {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(w) * h * 4u);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    return pixels;
}

const char* kTexturedVertex =
    "#version 130\n"
    "in vec2 a_pos;\n"
    "in vec2 a_uv;\n"
    "in float a_depth;\n"
    "in vec4 a_tint;\n"
    "uniform mat4 u_transform;\n"
    "out vec2 v_uv;\n"
    "out vec4 v_tint;\n"
    "void main() {\n"
    "  v_uv = a_uv;\n"
    "  v_tint = a_tint;\n"
    "  gl_Position = u_transform * vec4(a_pos, a_depth, 1.0);\n"
    "}\n";

const char* kTexturedFragment =
    "#version 130\n"
    "in vec2 v_uv;\n"
    "in vec4 v_tint;\n"
    "uniform sampler2D u_texture;\n"
    "out vec4 o_colour;\n"
    "void main() { o_colour = texture(u_texture, v_uv) * v_tint; }\n";

// Deliberately declares its inputs in a DIFFERENT order from the textured
// shader above. If the surface did not pin attribute slots by name, the linker
// would assign them differently here and the sampled value would be wrong.
const char* kIntegerVertex =
    "#version 130\n"
    "in vec2 a_uv;\n"
    "in vec2 a_pos;\n"
    "uniform mat4 u_transform;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "  v_uv = a_uv;\n"
    "  gl_Position = u_transform * vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

const char* kIntegerFragment =
    "#version 130\n"
    "in vec2 v_uv;\n"
    "uniform usampler2D u_values;\n"
    "out vec4 o_colour;\n"
    "void main() {\n"
    "  uint v = texture(u_values, v_uv).r;\n"
    "  o_colour = vec4(float(v & 0xFFu) / 255.0,\n"
    "                  float((v >> 8) & 0xFFu) / 255.0, 0.0, 1.0);\n"
    "}\n";

constexpr int kW = 64, kH = 32;

// Screen-space projection over the target, y downward.
void target_transform(float* m) {
    for (int i = 0; i < 16; ++i) m[i] = 0.0f;
    m[0]  =  2.0f / kW;
    m[5]  = -2.0f / kH;
    m[10] = -1.0f;
    m[12] = -1.0f;
    m[13] =  1.0f;
    m[15] =  1.0f;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* win = SDL_CreateWindow("gpu surface smoke", 0, 0, 64, 64,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win) { std::fprintf(stderr, "window: %s\n", SDL_GetError()); return 1; }
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) { std::fprintf(stderr, "context: %s\n", SDL_GetError()); return 1; }

    std::printf("GL vendor   : %s\n", glGetString(GL_VENDOR));
    std::printf("GL renderer : %s\n", glGetString(GL_RENDERER));
    std::printf("GL version  : %s\n", glGetString(GL_VERSION));
    std::printf("GLSL        : %s\n\n",
                glGetString(GL_SHADING_LANGUAGE_VERSION));

    gbarecomp::GpuSurface gpu;
    const bool inited = gpu.init();
    check(inited, "entry points load and the built-in shader compiles",
          gpu.failure());
    if (!inited) {
        std::printf("\nThis machine cannot run the GPU path. The game would "
                    "keep its existing renderer.\n");
        SDL_GL_DeleteContext(ctx);
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }

    check(gpu.set_target_size(kW, kH), "offscreen target is complete");

    float transform[16];
    target_transform(transform);

    // ── a textured quad samples its texture correctly ───────────────────────
    {
        // Two texels side by side: red then green. Drawn across the whole
        // target, so the left half must come out red and the right green.
        const std::uint8_t texels[8] = {255, 0, 0, 255,  0, 255, 0, 255};
        gbarecomp::GpuTexture tex = gpu.create_texture(
            2, 1, gbarecomp::GpuTextureFormat::RGBA8, false);
        check(tex != 0, "an RGBA texture can be created");
        gpu.update_texture(tex, 0, 0, 2, 1, texels);

        char log[1024] = {};
        gbarecomp::GpuProgram prog = gpu.create_program(
            kTexturedVertex, kTexturedFragment, log, sizeof log);
        check(prog != 0, "a textured shader links", log);

        if (prog && tex) {
            gpu.set_uniform_mat4(prog, "u_transform", transform);
            gpu.set_uniform_int(prog, "u_texture", 0);

            gbarecomp::GpuQuad q;
            q.x = 0; q.y = 0; q.w = kW; q.h = kH;
            const gbarecomp::GpuTexture textures[1] = {tex};

            gpu.begin_frame(0.0f, 0.0f, 1.0f, 1.0f);   // blue, so a miss shows
            gpu.draw_quads(prog, &q, 1, textures, 1);
            const auto pixels = read_target(kW, kH);
            gpu.end_frame();

            check(pixels.size() == static_cast<std::size_t>(kW) * kH * 4,
                  "the target can be read back");
            const std::size_t left =
                (static_cast<std::size_t>(kH / 2) * kW + 8) * 4;
            const std::size_t right =
                (static_cast<std::size_t>(kH / 2) * kW + kW - 8) * 4;
            char detail[160];
            std::snprintf(detail, sizeof detail,
                          "left (%d,%d,%d) right (%d,%d,%d)",
                          pixels[left], pixels[left + 1], pixels[left + 2],
                          pixels[right], pixels[right + 1], pixels[right + 2]);
            check(pixels[left] > 200 && pixels[left + 1] < 60 &&
                  pixels[right + 1] > 200 && pixels[right] < 60,
                  "the texture is sampled with the right orientation", detail);
            gpu.destroy_program(prog);
        }
        gpu.destroy_texture(tex);
    }

    // ── a 16-bit integer texture can be sampled exactly ─────────────────────
    // This is the capability the scene renderer depends on: map entries and
    // palettes are 16-bit values a shader must read as numbers, not as
    // filtered colour.
    {
        char log[2048] = {};
        gbarecomp::GpuProgram prog = gpu.create_program(
            kIntegerVertex, kIntegerFragment, log, sizeof log);
        check(prog != 0, "a shader can sample a 16-bit integer texture", log);

        if (prog) {
            const std::uint16_t values[1] = {0x1234};
            gbarecomp::GpuTexture vt = gpu.create_texture(
                1, 1, gbarecomp::GpuTextureFormat::R16UI, false);
            check(vt != 0, "a 16-bit integer texture can be created");
            gpu.update_texture(vt, 0, 0, 1, 1, values);

            gpu.set_uniform_mat4(prog, "u_transform", transform);
            gpu.set_uniform_int(prog, "u_values", 0);

            gbarecomp::GpuQuad q;
            q.x = 0; q.y = 0; q.w = kW; q.h = kH;
            const gbarecomp::GpuTexture textures[1] = {vt};

            gpu.begin_frame(0, 0, 0, 1);
            gpu.draw_quads(prog, &q, 1, textures, 1);
            const auto pixels = read_target(kW, kH);
            gpu.end_frame();

            // 0x1234 in: low byte 0x34 out as red, high byte 0x12 as green.
            const int red = pixels[0], green = pixels[1];
            char detail[160];
            std::snprintf(detail, sizeof detail,
                          "expected red 0x34 green 0x12, got red 0x%02X "
                          "green 0x%02X", red, green);
            check(red == 0x34 && green == 0x12,
                  "the shader reads the exact 16-bit value", detail);

            gpu.destroy_texture(vt);
            gpu.destroy_program(prog);
        }
    }

    std::printf("\n%s\n", failures == 0
        ? "The GPU surface works on this machine."
        : "The GPU surface has problems on this machine; see above.");

    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return failures == 0 ? 0 : 1;
}
