// gpu_surface.cpp -- see gpu_surface.h.
//
// Entry points are loaded through SDL rather than a loader library. Windows'
// opengl32 exports only OpenGL 1.1, so everything newer -- shaders, buffers,
// vertex arrays, framebuffers -- has to be fetched at runtime. Dear ImGui
// ships its own loader for its own use; reaching into that would couple us to
// its internals, and the set we need is small enough to fetch directly.
#include "gpu_surface.h"

#include <SDL.h>

#include <cstdio>
#include <cstring>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif
#include <GL/gl.h>

namespace gbarecomp {
namespace {

// ── constants newer than the 1.1 headers Windows ships ──────────────────────
constexpr GLenum kArrayBuffer          = 0x8892;
constexpr GLenum kStreamDraw           = 0x88E0;
constexpr GLenum kFragmentShader       = 0x8B30;
constexpr GLenum kVertexShader         = 0x8B31;
constexpr GLenum kCompileStatus        = 0x8B81;
constexpr GLenum kLinkStatus           = 0x8B82;
constexpr GLenum kInfoLogLength        = 0x8B84;
constexpr GLenum kFramebuffer          = 0x8D40;
constexpr GLenum kColorAttachment0     = 0x8CE0;
constexpr GLenum kFramebufferComplete  = 0x8CD5;
constexpr GLenum kFramebufferBinding   = 0x8CA6;
constexpr GLenum kTexture0             = 0x84C0;
constexpr GLenum kActiveTextureEnum    = 0x84E0;
constexpr GLenum kArrayBufferBinding   = 0x8894;
constexpr GLenum kVertexArrayBinding   = 0x85B5;
constexpr GLenum kCurrentProgram       = 0x8B8D;
constexpr GLenum kR8UI                 = 0x8232;
constexpr GLenum kR16UI                = 0x8234;
constexpr GLenum kRedInteger           = 0x8D94;
constexpr GLenum kClampToEdge          = 0x812F;
constexpr GLenum kRenderbuffer         = 0x8D41;
constexpr GLenum kDepthAttachment      = 0x8D00;
constexpr GLenum kDepthComponent24     = 0x81A6;
constexpr GLenum kRenderbufferBinding  = 0x8CA7;
// Pixel-transfer state. Each of these silently changes which bytes GL
// reads from -- or writes to -- the pointer a caller hands it.
constexpr GLenum kUnpackRowLength      = 0x0CF2;
constexpr GLenum kUnpackSkipRows       = 0x0CF3;
constexpr GLenum kUnpackSkipPixels     = 0x0CF4;
constexpr GLenum kUnpackImageHeight    = 0x806E;
constexpr GLenum kUnpackSkipImages     = 0x806D;
constexpr GLenum kPackRowLength        = 0x0D02;
constexpr GLenum kPackSkipRows         = 0x0D03;
constexpr GLenum kPackSkipPixels       = 0x0D04;
constexpr GLenum kPixelUnpackBuffer    = 0x88EC;
constexpr GLenum kPixelPackBuffer      = 0x88EB;
constexpr GLenum kPixelUnpackBufferBinding = 0x88EF;
constexpr GLenum kPixelPackBufferBinding   = 0x88ED;
constexpr GLenum kBlendDstRgb          = 0x80C8;
constexpr GLenum kBlendSrcRgb          = 0x80C9;
constexpr GLenum kBlendDstAlpha        = 0x80CA;
constexpr GLenum kBlendSrcAlpha        = 0x80CB;

// ── the entry points we need ────────────────────────────────────────────────
#define GPU_GL_FUNCTIONS(X)                                                    \
    X(GLuint,  CreateShader,   (GLenum))                                       \
    X(void,    ShaderSource,    (GLuint, GLsizei, const char* const*, const GLint*)) \
    X(void,    CompileShader,   (GLuint))                                      \
    X(void,    GetShaderiv,     (GLuint, GLenum, GLint*))                      \
    X(void,    GetShaderInfoLog,(GLuint, GLsizei, GLsizei*, char*))            \
    X(void,    DeleteShader,    (GLuint))                                      \
    X(GLuint,  CreateProgram,   (void))                                        \
    X(void,    AttachShader,    (GLuint, GLuint))                              \
    X(void,    LinkProgram,     (GLuint))                                      \
    X(void,    GetProgramiv,    (GLuint, GLenum, GLint*))                      \
    X(void,    GetProgramInfoLog,(GLuint, GLsizei, GLsizei*, char*))           \
    X(void,    DeleteProgram,   (GLuint))                                      \
    X(void,    UseProgram,      (GLuint))                                      \
    X(GLint,   GetUniformLocation,(GLuint, const char*))                       \
    X(void,    GetUniformiv,    (GLuint, GLint, GLint*))                       \
    X(void,    GetUniformfv,    (GLuint, GLint, GLfloat*))                     \
    X(void,    Uniform1i,       (GLint, GLint))                                \
    X(void,    Uniform1f,       (GLint, GLfloat))                              \
    X(void,    Uniform2f,       (GLint, GLfloat, GLfloat))                     \
    X(void,    Uniform4f,       (GLint, GLfloat, GLfloat, GLfloat, GLfloat))   \
    X(void,    UniformMatrix4fv,(GLint, GLsizei, GLboolean, const GLfloat*))   \
    X(void,    GenVertexArrays, (GLsizei, GLuint*))                            \
    X(void,    BindVertexArray, (GLuint))                                      \
    X(void,    DeleteVertexArrays,(GLsizei, const GLuint*))                    \
    X(void,    GenBuffers,      (GLsizei, GLuint*))                            \
    X(void,    BindBuffer,      (GLenum, GLuint))                              \
    X(void,    BufferData,      (GLenum, ptrdiff_t, const void*, GLenum))      \
    X(void,    BufferSubData,   (GLenum, ptrdiff_t, ptrdiff_t, const void*))   \
    X(void,    DeleteBuffers,   (GLsizei, const GLuint*))                      \
    X(void,    VertexAttribPointer,(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*)) \
    X(void,    EnableVertexAttribArray,(GLuint))                               \
    X(void,    GenFramebuffers, (GLsizei, GLuint*))                            \
    X(void,    BindFramebuffer, (GLenum, GLuint))                              \
    X(void,    FramebufferTexture2D,(GLenum, GLenum, GLenum, GLuint, GLint))   \
    X(GLenum,  CheckFramebufferStatus,(GLenum))                                \
    X(void,    DeleteFramebuffers,(GLsizei, const GLuint*))                    \
    X(void,    ActiveTexture,   (GLenum))                                      \
    X(void,    BindAttribLocation,(GLuint, GLuint, const char*))                \
    X(void,    GenRenderbuffers,(GLsizei, GLuint*))                            \
    X(void,    BindRenderbuffer,(GLenum, GLuint))                              \
    X(void,    RenderbufferStorage,(GLenum, GLenum, GLsizei, GLsizei))         \
    X(void,    FramebufferRenderbuffer,(GLenum, GLenum, GLenum, GLuint))       \
    X(void,    DeleteRenderbuffers,(GLsizei, const GLuint*))                    \
    X(void,    BlendFuncSeparate,(GLenum, GLenum, GLenum, GLenum))

#define GPU_DECLARE(ret, name, args) ret (APIENTRY* gl##name) args = nullptr;
GPU_GL_FUNCTIONS(GPU_DECLARE)
#undef GPU_DECLARE

bool load_all() {
    bool ok = true;
#define GPU_LOAD(ret, name, args)                                              \
    gl##name = reinterpret_cast<ret (APIENTRY*) args>(                         \
        SDL_GL_GetProcAddress("gl" #name));                                    \
    if (!gl##name) ok = false;
    GPU_GL_FUNCTIONS(GPU_LOAD)
#undef GPU_LOAD
    return ok;
}

// Every quad becomes two triangles; each vertex carries position, texture
// coordinate, depth and tint.
constexpr int kFloatsPerVertex = 9;
constexpr int kVerticesPerQuad = 6;

const char* kBlitVertex =
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

const char* kBlitFragment =
    "#version 130\n"
    "in vec2 v_uv;\n"
    "in vec4 v_tint;\n"
    "uniform sampler2D u_texture;\n"
    "out vec4 o_colour;\n"
    "void main() {\n"
    "  o_colour = texture(u_texture, v_uv) * v_tint;\n"
    "}\n";

// Orthographic projection over a width x height rectangle with y downward,
// column-major, which is what GL expects.
void ortho(float* m, float width, float height) {
    std::memset(m, 0, sizeof(float) * 16);
    m[0]  =  2.0f / width;
    m[5]  = -2.0f / height;
    m[10] = -1.0f;
    m[12] = -1.0f;
    m[13] =  1.0f;
    m[15] =  1.0f;
}

}  // namespace

struct GpuSurface::SavedState {
    GLint framebuffer = 0;
    GLint program = 0;
    GLint array_buffer = 0;
    GLint vertex_array = 0;
    GLint active_texture = 0;
    GLint viewport[4] = {0, 0, 0, 0};
    GLboolean blend = GL_FALSE;
    GLboolean depth_test = GL_FALSE;
    GLboolean scissor = GL_FALSE;
    GLboolean depth_mask = GL_TRUE;
    // SDL's renderer remembers the clear colour it last set and skips setting
    // it again, so a colour left behind here becomes its window clear colour
    // (the margins around the picture).
    GLfloat clear_color[4] = {0, 0, 0, 1};
    GLint blend_src_rgb = GL_ONE, blend_dst_rgb = GL_ZERO;
    GLint blend_src_alpha = GL_ONE, blend_dst_alpha = GL_ZERO;
};

GpuSurface::~GpuSurface() { release(); }

bool GpuSurface::init() {
    if (ready_) return true;

    if (!SDL_GL_GetCurrentContext()) {
        failure_ = "no OpenGL context is current on this thread";
        return false;
    }
    if (!load_all()) {
        failure_ = "this OpenGL context is missing shader, buffer or "
                   "framebuffer support";
        return false;
    }
    const GLubyte* version = glGetString(GL_VERSION);
    if (!version) {
        failure_ = "the OpenGL context reported no version";
        return false;
    }

    saved_ = new SavedState();

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glGenFramebuffers(1, &fbo_);
    if (!vao_ || !vbo_ || !fbo_) {
        failure_ = "could not create the vertex array, buffer or framebuffer";
        release();
        return false;
    }

    char log[1024] = {};
    blit_program_ = create_program(kBlitVertex, kBlitFragment, log, sizeof log);
    if (!blit_program_) {
        std::fprintf(stderr, "gpu_surface: blit shader failed: %s\n", log);
        failure_ = "the built-in shader did not compile on this driver";
        release();
        return false;
    }

    ready_ = true;
    failure_ = nullptr;
    return true;
}

void GpuSurface::release() {
    if (blit_program_) { glDeleteProgram(blit_program_); blit_program_ = 0; }
    if (target_tex_) { glDeleteTextures(1, &target_tex_); target_tex_ = 0; }
    if (depth_rb_) { glDeleteRenderbuffers(1, &depth_rb_); depth_rb_ = 0; }
    if (fbo_) { glDeleteFramebuffers(1, &fbo_); fbo_ = 0; }
    if (vbo_) { glDeleteBuffers(1, &vbo_); vbo_ = 0; }
    if (vao_) { glDeleteVertexArrays(1, &vao_); vao_ = 0; }
    delete saved_;
    saved_ = nullptr;
    ready_ = false;
}

// ── resources ───────────────────────────────────────────────────────────────

GpuTexture GpuSurface::create_texture(int width, int height,
                                      GpuTextureFormat format, bool filtered) {
    if (width <= 0 || height <= 0) return 0;
    GLint max_size = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_size);
    if (max_size <= 0 || width > max_size || height > max_size) return 0;

    GLuint tex = 0;
    glGenTextures(1, &tex);
    if (!tex) return 0;
    GLint previous = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
    glBindTexture(GL_TEXTURE_2D, tex);
    const GLint filter = filtered ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, kClampToEdge);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, kClampToEdge);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    switch (format) {
    case GpuTextureFormat::R8UI:
        glTexImage2D(GL_TEXTURE_2D, 0, kR8UI, width, height, 0, kRedInteger,
                     GL_UNSIGNED_BYTE, nullptr);
        break;
    case GpuTextureFormat::R16UI:
        glTexImage2D(GL_TEXTURE_2D, 0, kR16UI, width, height, 0, kRedInteger,
                     GL_UNSIGNED_SHORT, nullptr);
        break;
    case GpuTextureFormat::RGBA8:
    default:
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, nullptr);
        break;
    }
    GLint actual_width = 0, actual_height = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &actual_width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &actual_height);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous));
    if (actual_width != width || actual_height != height) {
        glDeleteTextures(1, &tex);
        return 0;
    }
    return tex;
}

void GpuSurface::update_texture(GpuTexture texture, int x, int y,
                                int width, int height, const void* pixels) {
    if (!texture || width <= 0 || height <= 0 || !pixels) return;
    GLint previous = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
    glBindTexture(GL_TEXTURE_2D, texture);
    // GL reads this upload through whatever pixel-store state is current, and
    // this surface shares its context with SDL's renderer and with ImGui, both
    // of which upload textures of their own. A row length or a skip left behind
    // by either makes the driver read a larger span than the caller's buffer
    // holds, past the end of it; a transfer buffer left bound turns `pixels`
    // into an offset into that buffer instead. Nothing in an offline harness
    // reproduces this -- nothing else there touches GL -- and the first of the
    // two crashed the game inside the driver three seconds into a field screen.
    // So pin every setting this transfer depends on, and put back what we found.
    GLint prev_alignment = 4, prev_row_length = 0, prev_skip_rows = 0;
    GLint prev_skip_pixels = 0, prev_image_height = 0, prev_skip_images = 0;
    GLint prev_unpack_buffer = 0;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_alignment);
    glGetIntegerv(kUnpackRowLength, &prev_row_length);
    glGetIntegerv(kUnpackSkipRows, &prev_skip_rows);
    glGetIntegerv(kUnpackSkipPixels, &prev_skip_pixels);
    glGetIntegerv(kUnpackImageHeight, &prev_image_height);
    glGetIntegerv(kUnpackSkipImages, &prev_skip_images);
    glGetIntegerv(kPixelUnpackBufferBinding, &prev_unpack_buffer);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(kUnpackRowLength, 0);
    glPixelStorei(kUnpackSkipRows, 0);
    glPixelStorei(kUnpackSkipPixels, 0);
    glPixelStorei(kUnpackImageHeight, 0);
    glPixelStorei(kUnpackSkipImages, 0);
    if (prev_unpack_buffer) glBindBuffer(kPixelUnpackBuffer, 0);
    // The format is recovered from the texture's own storage, so callers do
    // not have to repeat it and cannot contradict it.
    GLint internal = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT,
                             &internal);
    if (internal == static_cast<GLint>(kR8UI)) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, kRedInteger,
                        GL_UNSIGNED_BYTE, pixels);
    } else if (internal == static_cast<GLint>(kR16UI)) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, kRedInteger,
                        GL_UNSIGNED_SHORT, pixels);
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, GL_RGBA,
                        GL_UNSIGNED_BYTE, pixels);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, prev_alignment);
    glPixelStorei(kUnpackRowLength, prev_row_length);
    glPixelStorei(kUnpackSkipRows, prev_skip_rows);
    glPixelStorei(kUnpackSkipPixels, prev_skip_pixels);
    glPixelStorei(kUnpackImageHeight, prev_image_height);
    glPixelStorei(kUnpackSkipImages, prev_skip_images);
    if (prev_unpack_buffer)
        glBindBuffer(kPixelUnpackBuffer,
                     static_cast<GLuint>(prev_unpack_buffer));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous));
}

void GpuSurface::destroy_texture(GpuTexture texture) {
    if (texture) glDeleteTextures(1, &texture);
}

GpuProgram GpuSurface::create_program(const char* vertex_source,
                                      const char* fragment_source,
                                      char* log, std::size_t log_bytes) {
    if (log && log_bytes) log[0] = 0;
    auto compile = [&](GLenum type, const char* source) -> GLuint {
        GLuint shader = glCreateShader(type);
        if (!shader) return 0;
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);
        GLint ok = 0;
        glGetShaderiv(shader, kCompileStatus, &ok);
        if (!ok) {
            if (log && log_bytes)
                glGetShaderInfoLog(shader, static_cast<GLsizei>(log_bytes),
                                   nullptr, log);
            glDeleteShader(shader);
            return 0;
        }
        return shader;
    };

    GLuint vs = compile(kVertexShader, vertex_source);
    if (!vs) return 0;
    GLuint fs = compile(kFragmentShader, fragment_source);
    if (!fs) { glDeleteShader(vs); return 0; }

    GLuint program = glCreateProgram();
    if (!program) { glDeleteShader(vs); glDeleteShader(fs); return 0; }
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    // Pin the vertex inputs to the slots draw_quads fills. Without this the
    // linker assigns them in whatever order it likes, and a shader that
    // declares its inputs in a different order silently reads the wrong data
    // -- which is exactly what the smoke test caught. A shader that does not
    // use one of these names simply ignores the binding.
    glBindAttribLocation(program, 0, "a_pos");
    glBindAttribLocation(program, 1, "a_uv");
    glBindAttribLocation(program, 2, "a_depth");
    glBindAttribLocation(program, 3, "a_tint");
    glLinkProgram(program);
    GLint ok = 0;
    glGetProgramiv(program, kLinkStatus, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok) {
        if (log && log_bytes)
            glGetProgramInfoLog(program, static_cast<GLsizei>(log_bytes),
                                nullptr, log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

void GpuSurface::destroy_program(GpuProgram program) {
    if (program) glDeleteProgram(program);
}

// Uniform setters bind the program, set, and put the previous one back, so a
// caller can prepare several programs without tracking what is current.
#define GPU_WITH_PROGRAM(p, body)                                              \
    do {                                                                       \
        if (!(p)) break;                                                       \
        GLint previous = 0;                                                    \
        glGetIntegerv(kCurrentProgram, &previous);                             \
        glUseProgram(p);                                                       \
        body;                                                                  \
        glUseProgram(static_cast<GLuint>(previous));                           \
    } while (false)

void GpuSurface::set_uniform_int(GpuProgram p, const char* name, int v) {
    GPU_WITH_PROGRAM(p, {
        const GLint loc = glGetUniformLocation(p, name);
        if (loc >= 0) glUniform1i(loc, v);
    });
}
void GpuSurface::set_uniform_float(GpuProgram p, const char* name, float v) {
    GPU_WITH_PROGRAM(p, {
        const GLint loc = glGetUniformLocation(p, name);
        if (loc >= 0) glUniform1f(loc, v);
    });
}
void GpuSurface::set_uniform_vec2(GpuProgram p, const char* name,
                                  float x, float y) {
    GPU_WITH_PROGRAM(p, {
        const GLint loc = glGetUniformLocation(p, name);
        if (loc >= 0) glUniform2f(loc, x, y);
    });
}
void GpuSurface::set_uniform_vec4(GpuProgram p, const char* name,
                                  float x, float y, float z, float w) {
    GPU_WITH_PROGRAM(p, {
        const GLint loc = glGetUniformLocation(p, name);
        if (loc >= 0) glUniform4f(loc, x, y, z, w);
    });
}
void GpuSurface::set_uniform_mat4(GpuProgram p, const char* name,
                                  const float* m) {
    GPU_WITH_PROGRAM(p, {
        const GLint loc = glGetUniformLocation(p, name);
        if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_FALSE, m);
    });
}
#undef GPU_WITH_PROGRAM

bool GpuSurface::get_uniform_int(GpuProgram p, const char* name, int* value) {
    if (!ready_ || !p || !value) return false;
    const GLint loc = glGetUniformLocation(p, name);
    if (loc < 0) return false;
    glGetUniformiv(p, loc, value);
    return true;
}

bool GpuSurface::get_uniform_vec2(GpuProgram p, const char* name, float* values) {
    if (!ready_ || !p || !values) return false;
    const GLint loc = glGetUniformLocation(p, name);
    if (loc < 0) return false;
    glGetUniformfv(p, loc, values);
    return true;
}

void GpuSurface::log_texture_r8ui(GpuTexture texture, unsigned unit,
                                 int width, int height,
                                 const std::uint8_t* expected) {
    if (!ready_) return;
    const GLenum prior_error = glGetError();
    while (glGetError() != GL_NO_ERROR) {}
    GLint active = 0, bound = 0, actual_w = 0, actual_h = 0, format = 0;
    GLint min_filter = 0, mag_filter = 0, max_size = 0;
    glGetIntegerv(kActiveTextureEnum, &active);
    glActiveTexture(kTexture0 + unit);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_size);
    glBindTexture(GL_TEXTURE_2D, texture);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &actual_w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &actual_h);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &format);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &min_filter);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &mag_filter);

    bool read = false;
    std::size_t nonzero = 0, differing = 0;
    if (texture && expected && width > 0 && height > 0 &&
        actual_w == width && actual_h == height && format == GLint(kR8UI)) {
        std::vector<std::uint8_t> pixels(std::size_t(width) * height);
        GLint alignment = 0, row_length = 0, skip_rows = 0, skip_pixels = 0;
        GLint pack_buffer = 0;
        glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
        glGetIntegerv(kPackRowLength, &row_length);
        glGetIntegerv(kPackSkipRows, &skip_rows);
        glGetIntegerv(kPackSkipPixels, &skip_pixels);
        glGetIntegerv(kPixelPackBufferBinding, &pack_buffer);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(kPackRowLength, 0);
        glPixelStorei(kPackSkipRows, 0);
        glPixelStorei(kPackSkipPixels, 0);
        if (pack_buffer) glBindBuffer(kPixelPackBuffer, 0);
        glGetTexImage(GL_TEXTURE_2D, 0, kRedInteger, GL_UNSIGNED_BYTE, pixels.data());
        glPixelStorei(GL_PACK_ALIGNMENT, alignment);
        glPixelStorei(kPackRowLength, row_length);
        glPixelStorei(kPackSkipRows, skip_rows);
        glPixelStorei(kPackSkipPixels, skip_pixels);
        if (pack_buffer) glBindBuffer(kPixelPackBuffer, GLuint(pack_buffer));
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            nonzero += pixels[i] != 0;
            differing += pixels[i] != expected[i];
        }
        read = true;
    }
    glBindTexture(GL_TEXTURE_2D, GLuint(bound));
    glActiveTexture(GLenum(active));
    const GLenum error = glGetError();
    std::fprintf(stderr,
        "[gpu] texture audit: texture=%u unit=%u bound=%d expected=%dx%d "
        "actual=%dx%d max=%d format=0x%x min/mag=0x%x/0x%x "
        "read=%d nonzero=%zu differing=%zu prior_error=0x%x error=0x%x\n",
        texture, unit, bound, width, height, actual_w, actual_h, max_size,
        format, min_filter, mag_filter, read && error == GL_NO_ERROR,
        nonzero, differing, prior_error, error);
}

// ── drawing ─────────────────────────────────────────────────────────────────

bool GpuSurface::set_target_size(int width, int height) {
    if (!ready_ || width <= 0 || height <= 0) return false;
    if (width == target_w_ && height == target_h_ && target_tex_) return true;

    if (target_tex_) { glDeleteTextures(1, &target_tex_); target_tex_ = 0; }
    target_tex_ = create_texture(width, height, GpuTextureFormat::RGBA8, false);
    if (!target_tex_) return false;

    GLint previous_fbo = 0;
    glGetIntegerv(kFramebufferBinding, &previous_fbo);
    glBindFramebuffer(kFramebuffer, fbo_);
    glFramebufferTexture2D(kFramebuffer, kColorAttachment0, GL_TEXTURE_2D,
                           target_tex_, 0);
    const GLenum status = glCheckFramebufferStatus(kFramebuffer);
    glBindFramebuffer(kFramebuffer, static_cast<GLuint>(previous_fbo));
    if (status != kFramebufferComplete) {
        glDeleteTextures(1, &target_tex_);
        target_tex_ = 0;
        return false;
    }
    target_w_ = width;
    target_h_ = height;
    return true;
}

void GpuSurface::begin_frame(float r, float g, float b, float a) {
    if (!ready_ || !target_tex_ || in_frame_) return;
    // SDL's renderer batches its own drawing and ImGui assumes the state it
    // left behind. Save everything we are about to change.
    glGetIntegerv(kFramebufferBinding, &saved_->framebuffer);
    glGetIntegerv(kCurrentProgram, &saved_->program);
    glGetIntegerv(kArrayBufferBinding, &saved_->array_buffer);
    glGetIntegerv(kVertexArrayBinding, &saved_->vertex_array);
    glGetIntegerv(kActiveTextureEnum, &saved_->active_texture);
    glGetIntegerv(GL_VIEWPORT, saved_->viewport);
    saved_->blend = glIsEnabled(GL_BLEND);
    saved_->depth_test = glIsEnabled(GL_DEPTH_TEST);
    saved_->scissor = glIsEnabled(GL_SCISSOR_TEST);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &saved_->depth_mask);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, saved_->clear_color);
    glGetIntegerv(kBlendSrcRgb, &saved_->blend_src_rgb);
    glGetIntegerv(kBlendDstRgb, &saved_->blend_dst_rgb);
    glGetIntegerv(kBlendSrcAlpha, &saved_->blend_src_alpha);
    glGetIntegerv(kBlendDstAlpha, &saved_->blend_dst_alpha);

    glBindFramebuffer(kFramebuffer, fbo_);
    glViewport(0, 0, target_w_, target_h_);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT);
    in_frame_ = true;
}

void GpuSurface::end_frame() {
    if (!ready_ || !in_frame_) return;
    glBindFramebuffer(kFramebuffer, static_cast<GLuint>(saved_->framebuffer));
    glUseProgram(static_cast<GLuint>(saved_->program));
    glBindBuffer(kArrayBuffer, static_cast<GLuint>(saved_->array_buffer));
    glBindVertexArray(static_cast<GLuint>(saved_->vertex_array));
    glActiveTexture(static_cast<GLenum>(saved_->active_texture));
    glViewport(saved_->viewport[0], saved_->viewport[1],
               saved_->viewport[2], saved_->viewport[3]);
    if (saved_->blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (saved_->depth_test) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (saved_->scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    glDepthMask(saved_->depth_mask);
    glClearColor(saved_->clear_color[0], saved_->clear_color[1],
                 saved_->clear_color[2], saved_->clear_color[3]);
    glBlendFuncSeparate(static_cast<GLenum>(saved_->blend_src_rgb),
                        static_cast<GLenum>(saved_->blend_dst_rgb),
                        static_cast<GLenum>(saved_->blend_src_alpha),
                        static_cast<GLenum>(saved_->blend_dst_alpha));
    in_frame_ = false;
}

void GpuSurface::set_blend_mode(GpuBlendMode mode) {
    if (!ready_ || !in_frame_) return;
    if (mode == GpuBlendMode::Off) {
        glDisable(GL_BLEND);
        return;
    }
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, mode == GpuBlendMode::Additive
                                  ? GL_ONE
                                  : GL_ONE_MINUS_SRC_ALPHA);
}

void GpuSurface::set_blend_alpha(bool enabled) {
    set_blend_mode(enabled ? GpuBlendMode::Alpha : GpuBlendMode::Off);
}

void GpuSurface::bind_color_target(GpuTexture texture) {
    if (!ready_) return;
    const GLuint tex = texture ? texture : target_tex_;
    GLint previous_fbo = 0;
    glGetIntegerv(kFramebufferBinding, &previous_fbo);
    glBindFramebuffer(kFramebuffer, fbo_);
    glFramebufferTexture2D(kFramebuffer, kColorAttachment0, GL_TEXTURE_2D, tex, 0);
    glBindFramebuffer(kFramebuffer, static_cast<GLuint>(previous_fbo));
}

void GpuSurface::clear_color_target(float r, float g, float b, float a) {
    if (!ready_ || !in_frame_) return;
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT);
}

// The depth buffer must match the colour attachment's size at ALL times, not
// only while the depth test is on. A driver is free to restrict drawing and
// clearing to the overlap of differently sized attachments WITHOUT reporting
// the framebuffer incomplete, and this one does: a target widened while the
// depth buffer still held the previous width silently lost every pixel past
// the old width, in passes that had the depth test switched off entirely.
void GpuSurface::resize_depth_to_target() {
    if (!ready_ || !depth_rb_) return;
    if (depth_w_ == target_w_ && depth_h_ == target_h_) return;
    GLint previous_rb = 0;
    glGetIntegerv(kRenderbufferBinding, &previous_rb);
    glBindRenderbuffer(kRenderbuffer, depth_rb_);
    glRenderbufferStorage(kRenderbuffer, kDepthComponent24, target_w_,
                          target_h_);
    glBindRenderbuffer(kRenderbuffer, static_cast<GLuint>(previous_rb));
    depth_w_ = target_w_;
    depth_h_ = target_h_;
}

void GpuSurface::set_depth_enabled(bool enabled) {
    if (!ready_) return;
    resize_depth_to_target();
    if (enabled) {
        if (!depth_rb_ || depth_w_ != target_w_ || depth_h_ != target_h_) {
            if (!depth_rb_) glGenRenderbuffers(1, &depth_rb_);
            GLint previous_rb = 0;
            glGetIntegerv(kRenderbufferBinding, &previous_rb);
            glBindRenderbuffer(kRenderbuffer, depth_rb_);
            glRenderbufferStorage(kRenderbuffer, kDepthComponent24, target_w_,
                                  target_h_);
            glBindRenderbuffer(kRenderbuffer, static_cast<GLuint>(previous_rb));

            GLint previous_fbo = 0;
            glGetIntegerv(kFramebufferBinding, &previous_fbo);
            glBindFramebuffer(kFramebuffer, fbo_);
            glFramebufferRenderbuffer(kFramebuffer, kDepthAttachment,
                                      kRenderbuffer, depth_rb_);
            glBindFramebuffer(kFramebuffer, static_cast<GLuint>(previous_fbo));
            depth_w_ = target_w_;
            depth_h_ = target_h_;
        }
        glDepthMask(GL_TRUE);
        glDepthFunc(GL_LESS);
        glEnable(GL_DEPTH_TEST);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
}

void GpuSurface::clear_depth(float value) {
    if (!ready_ || !depth_rb_ || !in_frame_) return;
    glClearDepth(static_cast<GLdouble>(value));
    glDepthMask(GL_TRUE);
    glClear(GL_DEPTH_BUFFER_BIT);
}

void GpuSurface::draw_quads(GpuProgram program, const GpuQuad* quads,
                            std::size_t count, const GpuTexture* textures,
                            std::size_t texture_count) {
    if (!ready_ || !program || !quads || count == 0) return;

    std::vector<float> vertices;
    vertices.reserve(count * kVerticesPerQuad * kFloatsPerVertex);
    for (std::size_t i = 0; i < count; ++i) {
        const GpuQuad& q = quads[i];
        const float corners[6][4] = {
            {q.x,       q.y,       q.u0, q.v0},
            {q.x + q.w, q.y,       q.u1, q.v0},
            {q.x + q.w, q.y + q.h, q.u1, q.v1},
            {q.x,       q.y,       q.u0, q.v0},
            {q.x + q.w, q.y + q.h, q.u1, q.v1},
            {q.x,       q.y + q.h, q.u0, q.v1},
        };
        for (const auto& c : corners) {
            vertices.push_back(c[0]);
            vertices.push_back(c[1]);
            vertices.push_back(c[2]);
            vertices.push_back(c[3]);
            vertices.push_back(q.depth);
            vertices.push_back(q.tint[0]);
            vertices.push_back(q.tint[1]);
            vertices.push_back(q.tint[2]);
            vertices.push_back(q.tint[3]);
        }
    }

    glBindVertexArray(vao_);
    glBindBuffer(kArrayBuffer, vbo_);
    const std::size_t bytes = vertices.size() * sizeof(float);
    if (bytes > vbo_capacity_) {
        glBufferData(kArrayBuffer, static_cast<ptrdiff_t>(bytes),
                     vertices.data(), kStreamDraw);
        vbo_capacity_ = bytes;
    } else {
        glBufferSubData(kArrayBuffer, 0, static_cast<ptrdiff_t>(bytes),
                        vertices.data());
    }

    const GLsizei stride = kFloatsPerVertex * sizeof(float);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<const void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<const void*>(2 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<const void*>(4 * sizeof(float)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride,
                          reinterpret_cast<const void*>(5 * sizeof(float)));

    for (std::size_t i = 0; i < texture_count; ++i) {
        glActiveTexture(static_cast<GLenum>(kTexture0 + i));
        glBindTexture(GL_TEXTURE_2D, textures[i]);
    }
    glActiveTexture(kTexture0);

    glUseProgram(program);
    glDrawArrays(GL_TRIANGLES, 0,
                 static_cast<GLsizei>(count * kVerticesPerQuad));
}

void GpuSurface::present_target(int x, int y, int width, int height,
                                int viewport_width, int viewport_height,
                                bool filtered) {
    if (!ready_ || !target_tex_ || width <= 0 || height <= 0) return;

    GLint previous_texture = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);
    glBindTexture(GL_TEXTURE_2D, target_tex_);
    const GLint filter = filtered ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous_texture));

    float transform[16];
    ortho(transform, static_cast<float>(viewport_width),
          static_cast<float>(viewport_height));
    set_uniform_mat4(blit_program_, "u_transform", transform);
    set_uniform_int(blit_program_, "u_texture", 0);

    GpuQuad quad;
    quad.x = static_cast<float>(x);
    quad.y = static_cast<float>(y);
    quad.w = static_cast<float>(width);
    quad.h = static_cast<float>(height);
    // The offscreen target is drawn bottom-up by GL, so flip it here rather
    // than everywhere that draws into it.
    quad.v0 = 1.0f;
    quad.v1 = 0.0f;
    const GpuTexture textures[1] = {target_tex_};
    draw_quads(blit_program_, &quad, 1, textures, 1);
}

void GpuSurface::read_texture_rgb(GpuTexture texture, int width, int height,
                                  std::uint8_t* out_rgb) {
    if (!ready_ || !texture || width <= 0 || height <= 0 || !out_rgb) return;

    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4u);
    GLint previous_texture = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    // Same hazard as update_texture, in the other direction: a stale pack row
    // length or a bound pack buffer would have GL write outside `rgba`.
    GLint prev_alignment = 4, prev_row_length = 0, prev_skip_rows = 0;
    GLint prev_skip_pixels = 0, prev_pack_buffer = 0;
    glGetIntegerv(GL_PACK_ALIGNMENT, &prev_alignment);
    glGetIntegerv(kPackRowLength, &prev_row_length);
    glGetIntegerv(kPackSkipRows, &prev_skip_rows);
    glGetIntegerv(kPackSkipPixels, &prev_skip_pixels);
    glGetIntegerv(kPixelPackBufferBinding, &prev_pack_buffer);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(kPackRowLength, 0);
    glPixelStorei(kPackSkipRows, 0);
    glPixelStorei(kPackSkipPixels, 0);
    if (prev_pack_buffer) glBindBuffer(kPixelPackBuffer, 0);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glPixelStorei(GL_PACK_ALIGNMENT, prev_alignment);
    glPixelStorei(kPackRowLength, prev_row_length);
    glPixelStorei(kPackSkipRows, prev_skip_rows);
    glPixelStorei(kPackSkipPixels, prev_skip_pixels);
    if (prev_pack_buffer)
        glBindBuffer(kPixelPackBuffer, static_cast<GLuint>(prev_pack_buffer));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous_texture));

    // GL's row 0 is the bottom of the image; flip while dropping alpha so the
    // result matches the reference compositor's top-down RGB888 layout.
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* src =
            rgba.data() + static_cast<std::size_t>(height - 1 - y) * width * 4u;
        std::uint8_t* dst = out_rgb + static_cast<std::size_t>(y) * width * 3u;
        for (int x = 0; x < width; ++x) {
            dst[x * 3 + 0] = src[x * 4 + 0];
            dst[x * 3 + 1] = src[x * 4 + 1];
            dst[x * 3 + 2] = src[x * 4 + 2];
        }
    }
}

}  // namespace gbarecomp
