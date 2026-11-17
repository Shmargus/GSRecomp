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
#if defined(GBARECOMP_GLES)
    static const char kDesktop[] = "#version 130\n";
    static const char kEs[] =
        "#version 300 es\n"
        "precision highp float;\n"
        "precision highp int;\n"
        "precision highp sampler2D;\n"
        "precision highp usampler2D;\n"
        "precision highp isampler2D;\n";
    if (s.compare(0, sizeof(kDesktop) - 1, kDesktop) == 0)
        s.replace(0, sizeof(kDesktop) - 1, kEs);
#endif
    return s;
}

}  // namespace gbarecomp
