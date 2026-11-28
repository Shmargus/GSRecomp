// OpenGL platform headers and helpers.
//
// PC: desktop OpenGL 3.0+, headers from <GL/gl.h> (entry points newer than
// 1.1 are fetched through SDL). Android: OpenGL ES 3.1, headers from
// <GLES3/gl31.h> (3.1 adds the texture size queries we use). Our shaders
// are written for desktop GLSL 1.30; gl_shader_source() rewrites the version
// line for OpenGL ES 3.0 and adds the precision declarations ES requires.
// Everything else in the shaders is the common subset of both languages.
#pragma once

#include <string>

#if defined(__ANDROID__)
#  include <GLES3/gl31.h>  // 3.1: texture size queries
#  ifndef APIENTRY
#    define APIENTRY GL_APIENTRY
#  endif
#  define GBARECOMP_GLES 1
#else
#  if defined(_WIN32)
#    include <windows.h>  // GL/gl.h needs WINGDIAPI and APIENTRY
#  endif
#  include <GL/gl.h>
#endif

namespace gbarecomp {

// Choose the SDL renderer whose context the engine uses for raw GL.
inline const char* gl_sdl_render_driver() {
#if defined(GBARECOMP_GLES)
    return "opengles2";  // SDL's name; Android hands it an ES 3 context
#else
    return "opengl";
#endif
}

// The GLSL version line Dear ImGui's OpenGL backend should use.
inline const char* gl_imgui_glsl_version() {
#if defined(GBARECOMP_GLES)
    return "#version 300 es";
#else
    return "#version 130";
#endif
}

// Convert desktop GLSL to the current platform's GLSL.
inline std::string gl_shader_source(const char* source) {
    std::string s = source ? source : "";
    static const char kDesktop[] = "#version 130\n";
#if defined(GBARECOMP_GLES)
    static const char kEs[] =
        "#version 300 es\n"
        "precision highp float;\n"
        "precision highp int;\n"
        "precision highp sampler2D;\n"
        "precision highp usampler2D;\n"
        "precision highp isampler2D;\n"
        // Direct fetch for coordinates already proved in-bounds against a
        // constant or framebuffer size, avoiding a per-fetch textureSize().
        "vec4 gsr_raw_fetch(sampler2D s, ivec2 p, int l) { return texelFetch(s, p, l); }\n"
        "uvec4 gsr_raw_fetch(usampler2D s, ivec2 p, int l) { return texelFetch(s, p, l); }\n"
        "ivec4 gsr_raw_fetch(isampler2D s, ivec2 p, int l) { return texelFetch(s, p, l); }\n"
        // A texel read outside its texture returns zero, as desktop drivers
        // do. ES leaves it undefined, and on the Galaxy Tab A9+ (Adreno 619)
        // a stray read became a GPU memory fault: a run of them ended with
        // the driver refusing work (errno 35) and the game killed, twice on
        // 27 Nov 2026. The read itself stays inside the texture.
        "ivec2 gsr_fetch_at(ivec2 p, ivec2 size) { return clamp(p, ivec2(0), max(size - 1, ivec2(0))); }\n"
        "bool gsr_fetch_in(ivec2 p, ivec2 size) { return all(greaterThanEqual(p, ivec2(0))) && all(lessThan(p, size)); }\n"
        "vec4 gsr_texel_fetch(sampler2D s, ivec2 p, int l) {\n"
        "  ivec2 z = textureSize(s, l); vec4 v = texelFetch(s, gsr_fetch_at(p, z), l);\n"
        "  return gsr_fetch_in(p, z) ? v : vec4(0.0); }\n"
        "uvec4 gsr_texel_fetch(usampler2D s, ivec2 p, int l) {\n"
        "  ivec2 z = textureSize(s, l); uvec4 v = texelFetch(s, gsr_fetch_at(p, z), l);\n"
        "  return gsr_fetch_in(p, z) ? v : uvec4(0u); }\n"
        "ivec4 gsr_texel_fetch(isampler2D s, ivec2 p, int l) {\n"
        "  ivec2 z = textureSize(s, l); ivec4 v = texelFetch(s, gsr_fetch_at(p, z), l);\n"
        "  return gsr_fetch_in(p, z) ? v : ivec4(0); }\n"
        "#define texelFetch gsr_texel_fetch\n";
    if (s.compare(0, sizeof(kDesktop) - 1, kDesktop) == 0)
        s.replace(0, sizeof(kDesktop) - 1, kEs);
#else
    static const char kDesktopPreamble[] =
        "#version 130\n"
        "#define gsr_raw_fetch texelFetch\n";
    if (s.compare(0, sizeof(kDesktop) - 1, kDesktop) == 0)
        s.replace(0, sizeof(kDesktop) - 1, kDesktopPreamble);
#endif
    return s;
}

}  // namespace gbarecomp
