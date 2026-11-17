#include "gpu_present.h"

#include <atomic>

#if defined(__ANDROID__) && defined(GBARECOMP_HAVE_SDL2)

#include <SDL.h>

#include <algorithm>
#include <cstdio>

#include "gl_platform.h"
#include "temporal_blend.h"

namespace gbarecomp {
namespace {

std::atomic<bool> g_accepting{false};
bool g_offered = false;
GpuPresentFrame g_frame;

// The overlay layer as the runner last set it.
struct Overlay {
    const unsigned char* rgba = nullptr;
    int width = 0, height = 0;
    int dirty_x = 0, dirty_y = 0, dirty_w = 0, dirty_h = 0;
    bool visible = false;
} g_overlay;

// GL draw state and two frames of flicker history.
struct State {
    bool tried = false;
    bool ready = false;
    GLuint program = 0, vao = 0, vbo = 0, fbo_read = 0, fbo_draw = 0;
    GLint u_cur = -1, u_prev = -1, u_prev2 = -1, u_mode = -1, u_weight = -1;
    GLint u_source = -1, u_size = -1, u_overlay = -1, u_overlay_on = -1;
    GLuint overlay = 0;
    int overlay_w = 0, overlay_h = 0;
    GLuint history[2] = {0, 0};  // [0] frame N-1, [1] frame N-2
    int history_w = 0, history_h = 0;
    int history_count = 0;       // real previous frames held (0..2)
} g;

const char* kVertex =
    "#version 300 es\n"
    "layout(location = 0) in vec2 a_pos;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "  v_uv = vec2(a_pos.x * 0.5 + 0.5, 0.5 - a_pos.y * 0.5);\n"  // top-down
    "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

// Match temporal_blend.h. Mode 1 blends a pixel when it matches N-2 and
// differs from N-1. Mode 2 blends every pixel with N-1. Use exact 8-bit
// comparisons and nearest-neighbor sampling.
const char* kFragment =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "uniform sampler2D u_cur;\n"
    "uniform sampler2D u_prev;\n"
    "uniform sampler2D u_prev2;\n"
    "uniform int u_mode;\n"
    "uniform float u_weight;\n"
    "uniform vec4 u_source;\n"  // x0, y0, x1, y1 in frame pixels, top-down
    "uniform ivec2 u_size;\n"
    "uniform sampler2D u_overlay;\n"  // rows top-down
    "uniform int u_overlay_on;\n"
    "in vec2 v_uv;\n"
    "out vec4 o_colour;\n"
    "void main() {\n"
    "  vec2 p = mix(u_source.xy, u_source.zw, v_uv);\n"
    "  ivec2 t = clamp(ivec2(floor(p)), ivec2(0), u_size - 1);\n"
    "  ivec2 top_down = t;\n"
    "  t.y = u_size.y - 1 - t.y;\n"  // GL rows are bottom-up
    "  vec3 c = texelFetch(u_cur, t, 0).rgb;\n"
    "  if (u_mode == 1) {\n"
    "    vec3 a = texelFetch(u_prev, t, 0).rgb;\n"
    "    vec3 b = texelFetch(u_prev2, t, 0).rgb;\n"
    "    if (all(equal(c, b)) && any(notEqual(c, a)))\n"
    "      c = c * (1.0 - u_weight) + a * u_weight;\n"
    "  } else if (u_mode == 2) {\n"
    "    c = c * (1.0 - u_weight) + texelFetch(u_prev, t, 0).rgb * u_weight;\n"
    "  }\n"
    "  if (u_overlay_on != 0) {\n"
    "    vec4 o = texelFetch(u_overlay, top_down, 0);\n"
    "    c = mix(c, o.rgb, o.a);\n"
    "  }\n"
    "  o_colour = vec4(c, 1.0);\n"
    "}\n";

GLuint compile(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[gpu-present] shader failed: %s\n", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

bool init() {
    if (g.tried) return g.ready;
    g.tried = true;
    const GLuint vs = compile(GL_VERTEX_SHADER, kVertex);
    const GLuint fs = compile(GL_FRAGMENT_SHADER, kFragment);
    if (!vs || !fs) return false;
    g.program = glCreateProgram();
    glAttachShader(g.program, vs);
    glAttachShader(g.program, fs);
    glLinkProgram(g.program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(g.program, GL_LINK_STATUS, &ok);
    if (!ok) {
        std::fprintf(stderr, "[gpu-present] program failed to link\n");
        return false;
    }
    g.u_cur = glGetUniformLocation(g.program, "u_cur");
    g.u_prev = glGetUniformLocation(g.program, "u_prev");
    g.u_prev2 = glGetUniformLocation(g.program, "u_prev2");
    g.u_mode = glGetUniformLocation(g.program, "u_mode");
    g.u_weight = glGetUniformLocation(g.program, "u_weight");
    g.u_source = glGetUniformLocation(g.program, "u_source");
    g.u_size = glGetUniformLocation(g.program, "u_size");
    g.u_overlay = glGetUniformLocation(g.program, "u_overlay");
    g.u_overlay_on = glGetUniformLocation(g.program, "u_overlay_on");

    GLint previous_vao = 0, previous_buffer = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previous_vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previous_buffer);
    static const float kQuad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    glGenVertexArrays(1, &g.vao);
    glGenBuffers(1, &g.vbo);
    glBindVertexArray(g.vao);
    glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kQuad), kQuad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindVertexArray(static_cast<GLuint>(previous_vao));
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(previous_buffer));
    glGenFramebuffers(1, &g.fbo_read);
    glGenFramebuffers(1, &g.fbo_draw);
    g.ready = true;
    std::fprintf(stderr, "[gpu-present] drawing the card's picture directly\n");
    return true;
}

bool ensure_history(int w, int h) {
    if (g.history_w == w && g.history_h == h && g.history[0]) return true;
    GLint previous = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
    for (GLuint& t : g.history) {
        if (t) glDeleteTextures(1, &t);
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
    }
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous));
    g.history_w = w;
    g.history_h = h;
    g.history_count = 0;
    return g.history[0] && g.history[1];
}

}  // namespace

void gpu_present_set_accepting(bool accepting) { g_accepting.store(accepting); }
bool gpu_present_accepting() { return g_accepting.load(); }

void gpu_present_offer(const GpuPresentFrame& frame) {
    g_frame = frame;
    g_offered = frame.texture != 0 && frame.width > 0 && frame.height > 0;
}
void gpu_present_withdraw() { g_offered = false; }
const GpuPresentFrame* gpu_present_current() { return g_offered ? &g_frame : nullptr; }

void gpu_present_reset_history() { g.history_count = 0; }

void gpu_present_set_overlay(const unsigned char* rgba, int width, int height,
                             int dirty_x, int dirty_y, int dirty_w, int dirty_h,
                             bool visible) {
    g_overlay.rgba = rgba;
    g_overlay.width = width;
    g_overlay.height = height;
    // Dirty regions add up until a draw uploads them.
    if (dirty_w > 0 && dirty_h > 0) {
        if (g_overlay.dirty_w > 0) {
            const int x0 = std::min(g_overlay.dirty_x, dirty_x);
            const int y0 = std::min(g_overlay.dirty_y, dirty_y);
            const int x1 = std::max(g_overlay.dirty_x + g_overlay.dirty_w, dirty_x + dirty_w);
            const int y1 = std::max(g_overlay.dirty_y + g_overlay.dirty_h, dirty_y + dirty_h);
            dirty_x = x0; dirty_y = y0; dirty_w = x1 - x0; dirty_h = y1 - y0;
        }
        g_overlay.dirty_x = dirty_x;
        g_overlay.dirty_y = dirty_y;
        g_overlay.dirty_w = dirty_w;
        g_overlay.dirty_h = dirty_h;
    }
    g_overlay.visible = visible && rgba && width > 0 && height > 0;
}

namespace {
// The layer's texture, sized to the frame, with this frame's changes. The
// caller has the unit it samples from active.
bool upload_overlay(int w, int h) {
    if (!g_overlay.rgba || g_overlay.width != w || g_overlay.height != h) return false;
    const bool fresh = !g.overlay || g.overlay_w != w || g.overlay_h != h;
    if (fresh) {
        if (g.overlay) glDeleteTextures(1, &g.overlay);
        glGenTextures(1, &g.overlay);
        glBindTexture(GL_TEXTURE_2D, g.overlay);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
        g.overlay_w = w;
        g.overlay_h = h;
        g_overlay.dirty_x = g_overlay.dirty_y = 0;
        g_overlay.dirty_w = w;
        g_overlay.dirty_h = h;
    } else {
        glBindTexture(GL_TEXTURE_2D, g.overlay);
    }
    if (g_overlay.dirty_w > 0 && g_overlay.dirty_h > 0) {
        GLint row_length = 0, skip_rows = 0, skip_pixels = 0, alignment = 4, unpack = 0;
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row_length);
        glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skip_rows);
        glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skip_pixels);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, w);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, g_overlay.dirty_y);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, g_overlay.dirty_x);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, g_overlay.dirty_x, g_overlay.dirty_y,
                        g_overlay.dirty_w, g_overlay.dirty_h, GL_RGBA, GL_UNSIGNED_BYTE,
                        g_overlay.rgba);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, row_length);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, skip_rows);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, skip_pixels);
        glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(unpack));
        g_overlay.dirty_w = g_overlay.dirty_h = 0;
    }
    return true;
}
}  // namespace

bool gpu_present_draw(SDL_Renderer* renderer, const GpuPresentFrame& frame,
                      const SDL_Rect& source, const SDL_Rect& dest, int blend_index) {
    if (!renderer || dest.w <= 0 || dest.h <= 0) return false;
    int out_w = 0, out_h = 0;
    if (SDL_GetRendererOutputSize(renderer, &out_w, &out_h) != 0 || out_h <= 0) return false;
    // Flush SDL's queued draws and make its GL context current.
    SDL_RenderFlush(renderer);
    if (!init()) return false;
    if (!ensure_history(frame.width, frame.height)) return false;

    // Restore GL state on exit because SDL caches it.
    GLint program = 0, vao = 0, active = 0, viewport[4] = {};
    GLint draw_fbo = 0, read_fbo = 0, unit_tex[4] = {};
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
    for (int u = 0; u < 4; ++u) {
        glActiveTexture(GL_TEXTURE0 + u);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &unit_tex[u]);
    }
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    const GLboolean blend = glIsEnabled(GL_BLEND);
    const GLboolean depth = glIsEnabled(GL_DEPTH_TEST);

    const int index = std::clamp(blend_index, 0, kTemporalBlendItemCount - 1);
    const bool selective = temporal_blend_index_is_selective(index);
    const int required = selective ? 2 : 1;
    int mode = 0;
    if (index != 0 && g.history_count >= required) mode = selective ? 1 : 2;

    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glUseProgram(g.program);
    glBindVertexArray(g.vao);
    const GLuint textures[3] = {frame.texture, g.history[0], g.history[1]};
    for (int u = 0; u < 3; ++u) {
        glActiveTexture(GL_TEXTURE0 + u);
        glBindTexture(GL_TEXTURE_2D, textures[u]);
    }
    glUniform1i(g.u_cur, 0);
    glUniform1i(g.u_prev, 1);
    glUniform1i(g.u_prev2, 2);
    glUniform1i(g.u_mode, mode);
    glUniform1f(g.u_weight, temporal_blend_weight_for_index(index));
    glUniform2i(g.u_size, frame.width, frame.height);
    glActiveTexture(GL_TEXTURE3);
    const bool overlay = g_overlay.visible && upload_overlay(frame.width, frame.height);
    glUniform1i(g.u_overlay, 3);
    glUniform1i(g.u_overlay_on, overlay ? 1 : 0);

    // Draw to the window framebuffer.
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(draw_fbo));
    glViewport(dest.x, out_h - (dest.y + dest.h), dest.w, dest.h);
    glUniform4f(g.u_source, static_cast<float>(source.x), static_cast<float>(source.y),
                static_cast<float>(source.x + source.w),
                static_cast<float>(source.y + source.h));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // History for the blend: N-1 becomes N-2, this frame becomes N-1, kept
    // as drawn by the renderer (unblended), as the copied path does.
    if (index != 0) {
        std::swap(g.history[0], g.history[1]);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g.fbo_read);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               frame.texture, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g.fbo_draw);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               g.history[0], 0);
        glBlitFramebuffer(0, 0, frame.width, frame.height, 0, 0, frame.width, frame.height,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
        if (g.history_count < 2) ++g.history_count;
    } else {
        g.history_count = 0;
    }

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(draw_fbo));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(read_fbo));
    for (int u = 3; u >= 0; --u) {
        glActiveTexture(GL_TEXTURE0 + u);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(unit_tex[u]));
    }
    glActiveTexture(static_cast<GLenum>(active));
    glBindVertexArray(static_cast<GLuint>(vao));
    glUseProgram(static_cast<GLuint>(program));
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    if (blend) glEnable(GL_BLEND);
    if (depth) glEnable(GL_DEPTH_TEST);
    return true;
}

}  // namespace gbarecomp

#else  // desktop: the copied path only

namespace gbarecomp {
void gpu_present_set_accepting(bool) {}
bool gpu_present_accepting() { return false; }
void gpu_present_offer(const GpuPresentFrame&) {}
void gpu_present_withdraw() {}
const GpuPresentFrame* gpu_present_current() { return nullptr; }
bool gpu_present_draw(SDL_Renderer*, const GpuPresentFrame&, const SDL_Rect&,
                      const SDL_Rect&, int) { return false; }
void gpu_present_reset_history() {}
void gpu_present_set_overlay(const unsigned char*, int, int, int, int, int, int, bool) {}
}  // namespace gbarecomp

#endif
