// screen_filter.cpp -- see screen_filter.h.
//
// Four GLSL filters run at the final present on SDL's "opengl" renderer:
//
//   LCD3x      by Gigaherz, public domain (libretro glsl-shaders, handheld/).
//   xBR-lv2    by Hyllian, MIT licence (libretro glsl-shaders, xbr/). The
//              licence notice is reproduced verbatim above the shader source.
//   CRT Lottes by Timothy Lottes, public domain (libretro glsl-shaders, crt/).
//   ScaleFX    by Sp00kyFox, MIT licence (libretro glsl-shaders, scalefx/). A
//              five-pass chain; the notice is reproduced verbatim above it.
//
// All are ported from libretro's slang-less GLSL to `#version 130`. The libretro
// parameter/#pragma machinery is gone; the algorithms and their default
// constants are unchanged. The first three are one fragment pass; ScaleFX runs
// its passes through float/8-bit FBO textures (see kSfxPasses).
//
// The shaders work in "source pixel" units. The graphics-card renderer's
// texture can be the 1x picture enlarged by an integer native scale, so every
// sample goes through src_texel(), which reads the centre of a 1x pixel with
// nearest filtering -- for an integer-enlarged texture that is exactly the 1x
// picture.
//
// GL entry points are fetched through SDL_GL_GetProcAddress, the same way
// gpu_surface.cpp does it. That loader sits in gpu_surface.cpp's anonymous
// namespace and carries a much larger set than this file needs, so this file
// loads its own small set rather than widening that header.
#include "screen_filter.h"

#if defined(GBARECOMP_HAVE_SDL2)

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

// -- constants newer than the 1.1 headers Windows ships ----------------------
constexpr GLenum kArrayBuffer        = 0x8892;
constexpr GLenum kArrayBufferBinding = 0x8894;
constexpr GLenum kStaticDraw         = 0x88E4;
constexpr GLenum kFragmentShader     = 0x8B30;
constexpr GLenum kVertexShader       = 0x8B31;
constexpr GLenum kCompileStatus      = 0x8B81;
constexpr GLenum kLinkStatus         = 0x8B82;
constexpr GLenum kInfoLogLength      = 0x8B84;
constexpr GLenum kTexture0           = 0x84C0;
constexpr GLenum kActiveTextureEnum  = 0x84E0;
constexpr GLenum kVertexArrayBinding = 0x85B5;
constexpr GLenum kCurrentProgram     = 0x8B8D;
constexpr GLenum kClampToBorder      = 0x812D;
constexpr GLenum kFramebuffer        = 0x8D40;
constexpr GLenum kReadFramebuffer    = 0x8CA8;
constexpr GLenum kDrawFramebuffer    = 0x8CA9;
constexpr GLenum kDrawFramebufferBinding = 0x8CA6;
constexpr GLenum kReadFramebufferBinding = 0x8CAA;
constexpr GLenum kColorAttachment0   = 0x8CE0;
constexpr GLenum kFramebufferComplete = 0x8CD5;
constexpr GLenum kRgba8              = 0x8058;
constexpr GLenum kRgba16f            = 0x881A;

#define SF_GL_FUNCTIONS(X)                                                     \
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
    X(void,    Uniform1i,       (GLint, GLint))                                \
    X(void,    Uniform2f,       (GLint, GLfloat, GLfloat))                     \
    X(void,    Uniform4f,       (GLint, GLfloat, GLfloat, GLfloat, GLfloat))   \
    X(void,    GenVertexArrays, (GLsizei, GLuint*))                            \
    X(void,    BindVertexArray, (GLuint))                                      \
    X(void,    DeleteVertexArrays,(GLsizei, const GLuint*))                    \
    X(void,    GenBuffers,      (GLsizei, GLuint*))                            \
    X(void,    BindBuffer,      (GLenum, GLuint))                              \
    X(void,    BufferData,      (GLenum, ptrdiff_t, const void*, GLenum))      \
    X(void,    DeleteBuffers,   (GLsizei, const GLuint*))                      \
    X(void,    VertexAttribPointer,(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*)) \
    X(void,    EnableVertexAttribArray,(GLuint))                               \
    X(void,    ActiveTexture,   (GLenum))                                      \
    X(void,    BindAttribLocation,(GLuint, GLuint, const char*))

#define SF_DECLARE(ret, name, args) ret (APIENTRY* gl##name) args = nullptr;
SF_GL_FUNCTIONS(SF_DECLARE)
#undef SF_DECLARE

// Framebuffer objects (GL 3.0): loaded separately so a driver without them
// only loses ScaleFX, not the single-pass filters.
#define SF_FBO_FUNCTIONS(X)                                                    \
    X(void,    GenFramebuffers, (GLsizei, GLuint*))                            \
    X(void,    BindFramebuffer, (GLenum, GLuint))                              \
    X(void,    DeleteFramebuffers,(GLsizei, const GLuint*))                    \
    X(void,    FramebufferTexture2D,(GLenum, GLenum, GLenum, GLuint, GLint))   \
    X(GLenum,  CheckFramebufferStatus,(GLenum))

#define SF_DECLARE(ret, name, args) ret (APIENTRY* gl##name) args = nullptr;
SF_FBO_FUNCTIONS(SF_DECLARE)
#undef SF_DECLARE

bool load_fbo() {
    bool ok = true;
#define SF_LOAD(ret, name, args)                                               \
    gl##name = reinterpret_cast<ret (APIENTRY*) args>(                         \
        SDL_GL_GetProcAddress("gl" #name));                                    \
    if (!gl##name) ok = false;
    SF_FBO_FUNCTIONS(SF_LOAD)
#undef SF_LOAD
    return ok;
}

bool load_all() {
    bool ok = true;
#define SF_LOAD(ret, name, args)                                               \
    gl##name = reinterpret_cast<ret (APIENTRY*) args>(                         \
        SDL_GL_GetProcAddress("gl" #name));                                    \
    if (!gl##name) ok = false;
    SF_GL_FUNCTIONS(SF_LOAD)
#undef SF_LOAD
    return ok;
}

// -- shaders -----------------------------------------------------------------
const char* kVertexSource =
    "#version 130\n"
    "in vec2 a_pos;\n"
    "in vec2 a_uv;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "  v_uv = a_uv;\n"
    "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

// Same as kVertexSource but v_uv.y runs bottom-to-top: used when rendering into
// an FBO texture, so texture coordinate t equals v_uv and the image's top row is
// texture row 0 (what the sampling passes expect).
const char* kVertexSourceFbo =
    "#version 130\n"
    "in vec2 a_pos;\n"
    "in vec2 a_uv;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "  v_uv = vec2(a_uv.x, 1.0 - a_uv.y);\n"
    "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

// Shared by the LCD3x, xBR and CRT Lottes fragment shaders (and ScaleFX's copy). v_uv runs 0..1 across the destination,
// top-left origin, so v_uv * SourceSize is a position in source pixels.
const char* kFragmentCommon =
    "#version 130\n"
    "uniform sampler2D Texture;\n"
    "uniform vec2 SourceSize;\n"
    "uniform vec4 TexRect;\n"
    "uniform vec2 OutputSize;\n"
    "in vec2 v_uv;\n"
    "out vec4 FragColor;\n"
    "\n"
    "// Centre of the 1x pixel containing `pixel`, mapped into TexRect, nearest.\n"
    "vec4 src_texel(vec2 pixel) {\n"
    "  vec2 p = clamp(floor(pixel), vec2(0.0), SourceSize - vec2(1.0));\n"
    "  vec2 t = (p + vec2(0.5)) / SourceSize;\n"
    "  return texture(Texture, mix(TexRect.xy, TexRect.zw, t));\n"
    "}\n";

// LCD3x -- Author: Gigaherz. License: Public domain.
// (libretro glsl-shaders, handheld/shaders/lcd3x.glsl; defaults 16.0 and 4.0)
const char* kLcd3xFragment = R"GLSL(
#define brighten_scanlines 16.0
#define brighten_lcd 4.0

const vec3 offsets = vec3(3.141592654) * vec3(1.0/2.0,1.0/2.0 - 2.0/3.0,1.0/2.0-4.0/3.0);

void main()
{
    vec2 omega = vec2(3.141592654) * vec2(2.0) * SourceSize;
    vec3 res = src_texel(v_uv * SourceSize).xyz;

    vec2 angle = v_uv * omega;

    float yfactor = (brighten_scanlines + sin(angle.y)) / (brighten_scanlines + 1.0);
    vec3 xfactors = (brighten_lcd + sin(angle.x + offsets)) / (brighten_lcd + 1.0);

    vec3 color = yfactor * xfactors * res;

    FragColor = vec4(color.x, color.y, color.z, 1.0);
}
)GLSL";

// xBR-lv2 -- the licence notice below is reproduced verbatim from the upstream
// shader (libretro glsl-shaders, xbr/shaders/xbr-lv2.glsl).
//
//    Hyllian's xBR-lv2 Shader
//
//    Copyright (C) 2011-2016 Hyllian - sergiogdb@gmail.com
//
//    Permission is hereby granted, free of charge, to any person obtaining a copy
//    of this software and associated documentation files (the "Software"), to deal
//    in the Software without restriction, including without limitation the rights
//    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//    copies of the Software, and to permit persons to whom the Software is
//    furnished to do so, subject to the following conditions:
//
//    The above copyright notice and this permission notice shall be included in
//    all copies or substantial portions of the Software.
//
//    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
//    THE SOFTWARE.
//
//    Incorporates some of the ideas from SABR shader. Thanks to Joshua Street.
//
// Port notes: the vertex stage's t1..t7 neighbour coordinates are computed in
// the fragment stage as whole-pixel offsets through src_texel(); CORNER_C and
// SMOOTH_TIPS (the upstream defaults) are kept, the "small details" branch
// (default off) and the parameter pragmas are dropped.
const char* kXbrFragment = R"GLSL(
#define XBR_SCALE 3.0
#define XBR_EQ_THRESHOLD 15.0
#define XBR_LV2_COEFFICIENT 2.0
#define lv2_cf XBR_LV2_COEFFICIENT

const vec3 rgbw          = vec3(14.352, 28.176, 5.472);

const vec4 delta   = vec4(1.0/XBR_SCALE, 1.0/XBR_SCALE, 1.0/XBR_SCALE, 1.0/XBR_SCALE);
const vec4 delta_l = vec4(0.5/XBR_SCALE, 1.0/XBR_SCALE, 0.5/XBR_SCALE, 1.0/XBR_SCALE);
const vec4 delta_u = delta_l.yxwz;

const  vec4 Ao = vec4( 1.0, -1.0, -1.0, 1.0 );
const  vec4 Bo = vec4( 1.0,  1.0, -1.0,-1.0 );
const  vec4 Co = vec4( 1.5,  0.5, -0.5, 0.5 );
const  vec4 Ax = vec4( 1.0, -1.0, -1.0, 1.0 );
const  vec4 Bx = vec4( 0.5,  2.0, -0.5,-2.0 );
const  vec4 Cx = vec4( 1.0,  1.0, -0.5, 0.0 );
const  vec4 Ay = vec4( 1.0, -1.0, -1.0, 1.0 );
const  vec4 By = vec4( 2.0,  0.5, -2.0,-0.5 );
const  vec4 Cy = vec4( 2.0,  0.0, -1.0, 0.5 );
const  vec4 Ci = vec4(0.25, 0.25, 0.25, 0.25);

// Difference between vector components.
vec4 df(vec4 A, vec4 B)
{
    return vec4(abs(A-B));
}

// Compare two vectors and return their components are different.
vec4 diff(vec4 A, vec4 B)
{
    return vec4(notEqual(A, B));
}

// Determine if two vector components are equal based on a threshold.
vec4 eq(vec4 A, vec4 B)
{
    return (step(df(A, B), vec4(XBR_EQ_THRESHOLD)));
}

// Determine if two vector components are NOT equal based on a threshold.
vec4 neq(vec4 A, vec4 B)
{
    return (vec4(1.0, 1.0, 1.0, 1.0) - eq(A, B));
}

// Weighted distance.
vec4 wd(vec4 a, vec4 b, vec4 c, vec4 d, vec4 e, vec4 f, vec4 g, vec4 h)
{
    return (df(a,b) + df(a,c) + df(d,e) + df(d,f) + 4.0*df(g,h));
}

float c_df(vec3 c1, vec3 c2)
{
      vec3 df = abs(c1 - c2);
      return df.r + df.g + df.b;
}

void main()
{
    vec4 edri, edr, edr_l, edr_u, px; // px = pixel, edr = edge detection rule
    vec4 irlv0, irlv1, irlv2l, irlv2u, block_3d;
    vec4 fx, fx_l, fx_u; // inequations of straight lines.

    vec2 sp  = v_uv * SourceSize;
    vec2 fp  = fract(sp);
    vec2 bp  = floor(sp);

    vec3 A1 = src_texel(bp + vec2(-1.0, -2.0)).xyz;
    vec3 B1 = src_texel(bp + vec2( 0.0, -2.0)).xyz;
    vec3 C1 = src_texel(bp + vec2( 1.0, -2.0)).xyz;
    vec3 A  = src_texel(bp + vec2(-1.0, -1.0)).xyz;
    vec3 B  = src_texel(bp + vec2( 0.0, -1.0)).xyz;
    vec3 C  = src_texel(bp + vec2( 1.0, -1.0)).xyz;
    vec3 D  = src_texel(bp + vec2(-1.0,  0.0)).xyz;
    vec3 E  = src_texel(bp + vec2( 0.0,  0.0)).xyz;
    vec3 F  = src_texel(bp + vec2( 1.0,  0.0)).xyz;
    vec3 G  = src_texel(bp + vec2(-1.0,  1.0)).xyz;
    vec3 H  = src_texel(bp + vec2( 0.0,  1.0)).xyz;
    vec3 I  = src_texel(bp + vec2( 1.0,  1.0)).xyz;
    vec3 G5 = src_texel(bp + vec2(-1.0,  2.0)).xyz;
    vec3 H5 = src_texel(bp + vec2( 0.0,  2.0)).xyz;
    vec3 I5 = src_texel(bp + vec2( 1.0,  2.0)).xyz;
    vec3 A0 = src_texel(bp + vec2(-2.0, -1.0)).xyz;
    vec3 D0 = src_texel(bp + vec2(-2.0,  0.0)).xyz;
    vec3 G0 = src_texel(bp + vec2(-2.0,  1.0)).xyz;
    vec3 C4 = src_texel(bp + vec2( 2.0, -1.0)).xyz;
    vec3 F4 = src_texel(bp + vec2( 2.0,  0.0)).xyz;
    vec3 I4 = src_texel(bp + vec2( 2.0,  1.0)).xyz;

    vec4 b  = vec4(dot(B ,rgbw), dot(D ,rgbw), dot(H ,rgbw), dot(F ,rgbw));
    vec4 c  = vec4(dot(C ,rgbw), dot(A ,rgbw), dot(G ,rgbw), dot(I ,rgbw));
    vec4 d  = b.yzwx;
    vec4 e  = vec4(dot(E,rgbw));
    vec4 f  = b.wxyz;
    vec4 g  = c.zwxy;
    vec4 h  = b.zwxy;
    vec4 i  = c.wxyz;

    vec4 i4, i5, h5, f4;

    i4 = vec4(dot(I4,rgbw), dot(C1,rgbw), dot(A0,rgbw), dot(G5,rgbw));
    i5 = vec4(dot(I5,rgbw), dot(C4,rgbw), dot(A1,rgbw), dot(G0,rgbw));
    h5 = vec4(dot(H5,rgbw), dot(F4,rgbw), dot(B1,rgbw), dot(D0,rgbw));
    f4 = h5.yzwx;

    // These inequations define the line below which interpolation occurs.
    fx   = (Ao*fp.y+Bo*fp.x);
    fx_l = (Ax*fp.y+Bx*fp.x);
    fx_u = (Ay*fp.y+By*fp.x);

    irlv1 = irlv0 = diff(e,f) * diff(e,h);

    // CORNER_C
    irlv1     = (irlv0  * ( neq(f,b) * neq(f,c) + neq(h,d) * neq(h,g) + eq(e,i) * (neq(f,f4) * neq(f,i4) + neq(h,h5) * neq(h,i5)) + eq(e,g) + eq(e,c)) );

    irlv2l = diff(e,g) * diff(d,g);
    irlv2u = diff(e,c) * diff(b,c);

    vec4 fx45i = clamp((fx   + delta   -Co - Ci)/(2.0*delta  ), 0.0, 1.0);
    vec4 fx45  = clamp((fx   + delta   -Co     )/(2.0*delta  ), 0.0, 1.0);
    vec4 fx30  = clamp((fx_l + delta_l -Cx     )/(2.0*delta_l), 0.0, 1.0);
    vec4 fx60  = clamp((fx_u + delta_u -Cy     )/(2.0*delta_u), 0.0, 1.0);

    vec4 wd1, wd2;
    wd1 = wd( e, c,  g, i, h5, f4, h, f);
    wd2 = wd( h, d, i5, f, i4,  b, e, i);

    edri  = step(wd1, wd2) * irlv0;
    edr   = step(wd1 + vec4(0.1, 0.1, 0.1, 0.1), wd2) * step(vec4(0.5, 0.5, 0.5, 0.5), irlv1);
    edr_l = step( lv2_cf*df(f,g), df(h,c) ) * irlv2l * edr;
    edr_u = step( lv2_cf*df(h,c), df(f,g) ) * irlv2u * edr;

    fx45  = edr   * fx45;
    fx30  = edr_l * fx30;
    fx60  = edr_u * fx60;
    fx45i = edri  * fx45i;

    px = step(df(e,f), df(e,h));

    // SMOOTH_TIPS
    vec4 maximos = max(max(fx30, fx60), max(fx45, fx45i));

    vec3 res1 = E;
    res1 = mix(res1, mix(H, F, px.x), maximos.x);
    res1 = mix(res1, mix(B, D, px.z), maximos.z);

    vec3 res2 = E;
    res2 = mix(res2, mix(F, B, px.y), maximos.y);
    res2 = mix(res2, mix(D, H, px.w), maximos.w);

    vec3 res = mix(res1, res2, step(c_df(E, res1), c_df(E, res2)));

    FragColor = vec4(res, 1.0);
}
)GLSL";

// CRT Lottes -- PUBLIC DOMAIN CRT STYLED SCAN-LINE SHADER by Timothy Lottes
// (libretro glsl-shaders, crt/shaders/crt-lottes.glsl). Upstream's header says
// "Please take and use, change, or whatever."
//
// Port notes: parameters keep their upstream default values as #defines; the
// GL_ES / SIMPLE_LINEAR_GAMMA branches and the pragma lines are dropped. Every
// sample goes through src_texel(); reads outside the picture return black, as
// upstream's clamp-to-border wrap does. TextureSize == InputSize here, so the
// upstream size ratios in main() are 1.
const char* kLottesFragment = R"GLSL(
#define hardScan -8.0
#define hardPix -3.0
#define warpX 0.031
#define warpY 0.041
#define maskDark 0.5
#define maskLight 1.5
#define scaleInLinearGamma 1.0
#define shadowMask 3.0
#define brightBoost 1.0
#define hardBloomPix -1.5
#define hardBloomScan -2.0
#define bloomAmount 0.15
#define shape 2.0

//Uncomment to reduce instructions with simpler linearization
//(fixes HD3000 Sandy Bridge IGP)
//#define SIMPLE_LINEAR_GAMMA
#define DO_BLOOM

// ------------- //

// sRGB to Linear.
// Assuming using sRGB typed textures this should not be needed.
float ToLinear1(float c)
{
    if (scaleInLinearGamma == 0.) 
        return c;
    
    return(c<=0.04045) ? c/12.92 : pow((c + 0.055)/1.055, 2.4);
}

vec3 ToLinear(vec3 c)
{
    if (scaleInLinearGamma==0.) 
        return c;
    
    return vec3(ToLinear1(c.r), ToLinear1(c.g), ToLinear1(c.b));
}

// Linear to sRGB.
// Assuming using sRGB typed textures this should not be needed.
float ToSrgb1(float c)
{
    if (scaleInLinearGamma == 0.) 
        return c;
    
    return(c<0.0031308 ? c*12.92 : 1.055*pow(c, 0.41666) - 0.055);
}

vec3 ToSrgb(vec3 c)
{
    if (scaleInLinearGamma == 0.) 
        return c;
    
    return vec3(ToSrgb1(c.r), ToSrgb1(c.g), ToSrgb1(c.b));
}

// Nearest emulated sample given floating point position and texel offset.
// Also zero's off screen.
vec3 Fetch(vec2 pos,vec2 off){
  vec2 px = floor(pos*SourceSize+off);
  // Upstream's texture wrap is clamp-to-border (black): off-screen reads are black.
  if (px.x < 0.0 || px.y < 0.0 || px.x >= SourceSize.x || px.y >= SourceSize.y)
    return vec3(0.0);
  return ToLinear(brightBoost * src_texel(px).rgb);
}

// Distance in emulated pixels to nearest texel.
vec2 Dist(vec2 pos)
{
    pos = pos*SourceSize;
    
    return -((pos - floor(pos)) - vec2(0.5));
}
    
// 1D Gaussian.
float Gaus(float pos, float scale)
{
    return exp2(scale*pow(abs(pos), shape));
}

// 3-tap Gaussian filter along horz line.
vec3 Horz3(vec2 pos, float off)
{
    vec3 b    = Fetch(pos, vec2(-1.0, off));
    vec3 c    = Fetch(pos, vec2( 0.0, off));
    vec3 d    = Fetch(pos, vec2( 1.0, off));
    float dst = Dist(pos).x;

    // Convert distance to weight.
    float scale = hardPix;
    float wb = Gaus(dst-1.0,scale);
    float wc = Gaus(dst+0.0,scale);
    float wd = Gaus(dst+1.0,scale);

    // Return filtered sample.
    return (b*wb+c*wc+d*wd)/(wb+wc+wd);
}

// 5-tap Gaussian filter along horz line.
vec3 Horz5(vec2 pos,float off){
    vec3 a = Fetch(pos,vec2(-2.0, off));
    vec3 b = Fetch(pos,vec2(-1.0, off));
    vec3 c = Fetch(pos,vec2( 0.0, off));
    vec3 d = Fetch(pos,vec2( 1.0, off));
    vec3 e = Fetch(pos,vec2( 2.0, off));
    
    float dst = Dist(pos).x;
    // Convert distance to weight.
    float scale = hardPix;
    float wa = Gaus(dst - 2.0, scale);
    float wb = Gaus(dst - 1.0, scale);
    float wc = Gaus(dst + 0.0, scale);
    float wd = Gaus(dst + 1.0, scale);
    float we = Gaus(dst + 2.0, scale);
    
    // Return filtered sample.
    return (a*wa+b*wb+c*wc+d*wd+e*we)/(wa+wb+wc+wd+we);
}
  
// 7-tap Gaussian filter along horz line.
vec3 Horz7(vec2 pos,float off)
{
    vec3 a = Fetch(pos, vec2(-3.0, off));
    vec3 b = Fetch(pos, vec2(-2.0, off));
    vec3 c = Fetch(pos, vec2(-1.0, off));
    vec3 d = Fetch(pos, vec2( 0.0, off));
    vec3 e = Fetch(pos, vec2( 1.0, off));
    vec3 f = Fetch(pos, vec2( 2.0, off));
    vec3 g = Fetch(pos, vec2( 3.0, off));

    float dst = Dist(pos).x;
    // Convert distance to weight.
    float scale = hardBloomPix;
    float wa = Gaus(dst - 3.0, scale);
    float wb = Gaus(dst - 2.0, scale);
    float wc = Gaus(dst - 1.0, scale);
    float wd = Gaus(dst + 0.0, scale);
    float we = Gaus(dst + 1.0, scale);
    float wf = Gaus(dst + 2.0, scale);
    float wg = Gaus(dst + 3.0, scale);

    // Return filtered sample.
    return (a*wa+b*wb+c*wc+d*wd+e*we+f*wf+g*wg)/(wa+wb+wc+wd+we+wf+wg);
}
  
// Return scanline weight.
float Scan(vec2 pos, float off)
{
    float dst = Dist(pos).y;

    return Gaus(dst + off, hardScan);
}
  
// Return scanline weight for bloom.
float BloomScan(vec2 pos, float off)
{
    float dst = Dist(pos).y;
    
    return Gaus(dst + off, hardBloomScan);
}

// Allow nearest three lines to effect pixel.
vec3 Tri(vec2 pos)
{
    vec3 a = Horz3(pos,-1.0);
    vec3 b = Horz5(pos, 0.0);
    vec3 c = Horz3(pos, 1.0);
    
    float wa = Scan(pos,-1.0); 
    float wb = Scan(pos, 0.0);
    float wc = Scan(pos, 1.0);
    
    return a*wa + b*wb + c*wc;
}
  
// Small bloom.
vec3 Bloom(vec2 pos)
{
    vec3 a = Horz5(pos,-2.0);
    vec3 b = Horz7(pos,-1.0);
    vec3 c = Horz7(pos, 0.0);
    vec3 d = Horz7(pos, 1.0);
    vec3 e = Horz5(pos, 2.0);

    float wa = BloomScan(pos,-2.0);
    float wb = BloomScan(pos,-1.0); 
    float wc = BloomScan(pos, 0.0);
    float wd = BloomScan(pos, 1.0);
    float we = BloomScan(pos, 2.0);

    return a*wa+b*wb+c*wc+d*wd+e*we;
}
  
// Distortion of scanlines, and end of screen alpha.
vec2 Warp(vec2 pos)
{
    pos  = pos*2.0-1.0;    
    pos *= vec2(1.0 + (pos.y*pos.y)*warpX, 1.0 + (pos.x*pos.x)*warpY);
    
    return pos*0.5 + 0.5;
}
  
// Shadow mask.
vec3 Mask(vec2 pos)
{
    vec3 mask = vec3(maskDark, maskDark, maskDark);
  
    // Very compressed TV style shadow mask.
    if (shadowMask == 1.0) 
    {
        float line = maskLight;
        float odd = 0.0;
        
        if (fract(pos.x*0.166666666) < 0.5) odd = 1.0;
        if (fract((pos.y + odd) * 0.5) < 0.5) line = maskDark;  
        
        pos.x = fract(pos.x*0.333333333);

        if      (pos.x < 0.333) mask.r = maskLight;
        else if (pos.x < 0.666) mask.g = maskLight;
        else                    mask.b = maskLight;
        mask*=line;  
    } 

    // Aperture-grille.
    else if (shadowMask == 2.0) 
    {
        pos.x = fract(pos.x*0.333333333);

        if      (pos.x < 0.333) mask.r = maskLight;
        else if (pos.x < 0.666) mask.g = maskLight;
        else                    mask.b = maskLight;
    } 

    // Stretched VGA style shadow mask (same as prior shaders).
    else if (shadowMask == 3.0) 
    {
        pos.x += pos.y*3.0;
        pos.x  = fract(pos.x*0.166666666);

        if      (pos.x < 0.333) mask.r = maskLight;
        else if (pos.x < 0.666) mask.g = maskLight;
        else                    mask.b = maskLight;
    }

    // VGA style shadow mask.
    else if (shadowMask == 4.0) 
    {
        pos.xy  = floor(pos.xy*vec2(1.0, 0.5));
        pos.x  += pos.y*3.0;
        pos.x   = fract(pos.x*0.166666666);

        if      (pos.x < 0.333) mask.r = maskLight;
        else if (pos.x < 0.666) mask.g = maskLight;
        else                    mask.b = maskLight;
    }

    return mask;
}

void main()
{
    // TextureSize == InputSize here, so the upstream size ratios are 1.
    vec2 pos = Warp(v_uv);
    vec3 outColor = Tri(pos);

    //Add Bloom
    outColor.rgb += Bloom(pos)*bloomAmount;

    if (shadowMask > 0.0)
        outColor.rgb *= Mask(gl_FragCoord.xy * 1.000001);
    
    FragColor = vec4(ToSrgb(outColor.rgb), 1.0);
} 
)GLSL";

// ScaleFX -- the licence notice below is reproduced verbatim from the upstream
// shaders (libretro glsl-shaders, scalefx/shaders/scalefx-pass0..4.glsl; the
// notice is identical in all five). ScaleFX by Sp00kyFox, 2017-03-01.
//
//    Copyright (c) 2016 Sp00kyFox - ScaleFX@web.de
//
//    Permission is hereby granted, free of charge, to any person obtaining a copy
//    of this software and associated documentation files (the "Software"), to deal
//    in the Software without restriction, including without limitation the rights
//    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//    copies of the Software, and to permit persons to whom the Software is
//    furnished to do so, subject to the following conditions:
//
//    The above copyright notice and this permission notice shall be included in
//    all copies or substantial portions of the Software.
//
//    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
//    THE SOFTWARE.
//
// Port notes: the five passes follow scalefx.glslp (see kSfxPasses below). The
// vertex stages' t1..t4 coordinates are unused by the desktop fragment path
// (textureOffset), so they are dropped; v_uv replaces TEX0 and the SFX_*
// parameters keep their upstream defaults (SFX_CLR 0.5, SFX_SAA 1.0, SFX_SCN 1.0).
// Uniforms follow libretro: TextureSize/InputSize describe the pass's Source,
// OutputSize its own render target.
const char* kSfxCommon =
    "#version 130\n"
    "uniform vec2 OutputSize;\n"
    "uniform vec2 TextureSize;\n"
    "uniform vec2 InputSize;\n"
    "uniform sampler2D Texture;\n"
    "in vec2 v_uv;\n"
    "out vec4 FragColor;\n"
    "#define Source Texture\n"
    "#define vTexCoord v_uv\n"
    "#define SourceSize vec4(TextureSize, 1.0 / TextureSize)\n"
    "#define outsize vec4(OutputSize, 1.0 / OutputSize)\n";

// Step 0: the shown region at exactly 1x, so the passes read clean textures.
const char* kSfxCopyFragment = R"GLSL(
void main()
{
    FragColor = vec4(src_texel(v_uv * SourceSize).rgb, 1.0);
}
)GLSL";

const char* kSfxPass0 = R"GLSL(

// Reference: http://www.compuphase.com/cmetric.htm
float dist(vec3 A, vec3 B)
{
	float r = 0.5 * (A.r + B.r);
	vec3 d = A - B;
	vec3 c = vec3(2. + r, 4., 3. - r);

	return sqrt(dot(c*d, d)) / 3.;
}

void main()
{
	/*	grid		metric

		A B C		x y z
		  E F		  o w
	*/

#define TEX(x, y) textureOffset(Source, vTexCoord, ivec2(x, y)).rgb
	// read texels
	vec3 A = TEX(-1,-1);
	vec3 B = TEX( 0,-1);
	vec3 C = TEX( 1,-1);
	vec3 E = TEX( 0, 0);
	vec3 F = TEX( 1, 0);
	// output
	FragColor = vec4(dist(E,A), dist(E,B), dist(E,C), dist(E,F));
} 
)GLSL";

const char* kSfxPass1 = R"GLSL(

#define SFX_CLR 0.5
#define SFX_SAA 1.0

// corner strength
float str(float d, vec2 a, vec2 b){
	float diff = a.x - a.y;
	float wght1 = max(SFX_CLR - d, 0.) / SFX_CLR;
	float wght2 = clamp((1.-d) + (min(a.x, b.x) + a.x > min(a.y, b.y) + a.y ? diff : -diff), 0., 1.);
	return (SFX_SAA == 1. || 2.*d < a.x + a.y) ? (wght1 * wght2) * (a.x * a.y) : 0.;
}

void main()
{
	/*	grid		metric		pattern

		A B		x y z		x y
		D E F		  o w		w z
		G H I
	*/

#define TEX(x, y) textureOffset(Source, vTexCoord, ivec2(x, y))

	// metric data
	vec4 A = TEX(-1,-1), B = TEX( 0,-1);
	vec4 D = TEX(-1, 0), E = TEX( 0, 0), F = TEX( 1, 0);
	vec4 G = TEX(-1, 1), H = TEX( 0, 1), I = TEX( 1, 1);

	// corner strength
	vec4 res;
	res.x = str(D.z, vec2(D.w, E.y), vec2(A.w, D.y));
	res.y = str(F.x, vec2(E.w, E.y), vec2(B.w, F.y));
	res.z = str(H.z, vec2(E.w, H.y), vec2(H.w, I.y));
	res.w = str(H.x, vec2(D.w, H.y), vec2(G.w, G.y));
		
	FragColor = res;
} 
)GLSL";

const char* kSfxPass2 = R"GLSL(

uniform sampler2D PassPrev2Texture;

#define PassOutput0 PassPrev2Texture

#define LE(x, y) (1. - step(y, x))
#define GE(x, y) (1. - step(x, y))
#define LEQ(x, y) step(x, y)
#define GEQ(x, y) step(y, x)
#define NOT(x) (1. - (x))

// corner dominance at junctions
vec4 dom(vec3 x, vec3 y, vec3 z, vec3 w){
	return 2. * vec4(x.y, y.y, z.y, w.y) - (vec4(x.x, y.x, z.x, w.x) + vec4(x.z, y.z, z.z, w.z));
}

// necessary but not sufficient junction condition for orthogonal edges
float clear(vec2 crn, vec2 a, vec2 b){
	return (crn.x >= max(min(a.x, a.y), min(b.x, b.y))) && (crn.y >= max(min(a.x, b.y), min(b.x, a.y))) ? 1. : 0.;
}

void main()
{
	/*	grid		metric		pattern

		A B C		x y z		x y
		D E F		  o w		w z
		G H I
	*/

	#define TEXm(x, y) textureOffset(PassOutput0, vTexCoord, ivec2(x, y))
	#define TEXs(x, y) textureOffset(Source, vTexCoord, ivec2(x, y))

	// metric data
	vec4 A = TEXm(-1,-1), B = TEXm( 0,-1);
	vec4 D = TEXm(-1, 0), E = TEXm( 0, 0), F = TEXm( 1, 0);
	vec4 G = TEXm(-1, 1), H = TEXm( 0, 1), I = TEXm( 1, 1);	

	// strength data
	vec4 As = TEXs(-1,-1), Bs = TEXs( 0,-1), Cs = TEXs( 1,-1);
	vec4 Ds = TEXs(-1, 0), Es = TEXs( 0, 0), Fs = TEXs( 1, 0);
	vec4 Gs = TEXs(-1, 1), Hs = TEXs( 0, 1), Is = TEXs( 1, 1);

	// strength & dominance junctions
	vec4 jSx = vec4(As.z, Bs.w, Es.x, Ds.y), jDx = dom(As.yzw, Bs.zwx, Es.wxy, Ds.xyz);
	vec4 jSy = vec4(Bs.z, Cs.w, Fs.x, Es.y), jDy = dom(Bs.yzw, Cs.zwx, Fs.wxy, Es.xyz);
	vec4 jSz = vec4(Es.z, Fs.w, Is.x, Hs.y), jDz = dom(Es.yzw, Fs.zwx, Is.wxy, Hs.xyz);
	vec4 jSw = vec4(Ds.z, Es.w, Hs.x, Gs.y), jDw = dom(Ds.yzw, Es.zwx, Hs.wxy, Gs.xyz);

	// majority vote for ambiguous dominance junctions
	vec4 zero4 = vec4(0.);
	vec4 jx = min(GE(jDx, zero4) * (LEQ(jDx.yzwx, zero4) * LEQ(jDx.wxyz, zero4) + GE(jDx + jDx.zwxy, jDx.yzwx + jDx.wxyz)), 1.);
	vec4 jy = min(GE(jDy, zero4) * (LEQ(jDy.yzwx, zero4) * LEQ(jDy.wxyz, zero4) + GE(jDy + jDy.zwxy, jDy.yzwx + jDy.wxyz)), 1.);
	vec4 jz = min(GE(jDz, zero4) * (LEQ(jDz.yzwx, zero4) * LEQ(jDz.wxyz, zero4) + GE(jDz + jDz.zwxy, jDz.yzwx + jDz.wxyz)), 1.);
	vec4 jw = min(GE(jDw, zero4) * (LEQ(jDw.yzwx, zero4) * LEQ(jDw.wxyz, zero4) + GE(jDw + jDw.zwxy, jDw.yzwx + jDw.wxyz)), 1.);

	// inject strength without creating new contradictions
	vec4 res;
	res.x = min(jx.z + NOT(jx.y) * NOT(jx.w) * GE(jSx.z, 0.) * (jx.x + GE(jSx.x + jSx.z, jSx.y + jSx.w)), 1.);
	res.y = min(jy.w + NOT(jy.z) * NOT(jy.x) * GE(jSy.w, 0.) * (jy.y + GE(jSy.y + jSy.w, jSy.x + jSy.z)), 1.);
	res.z = min(jz.x + NOT(jz.w) * NOT(jz.y) * GE(jSz.x, 0.) * (jz.z + GE(jSz.x + jSz.z, jSz.y + jSz.w)), 1.);
	res.w = min(jw.y + NOT(jw.x) * NOT(jw.z) * GE(jSw.y, 0.) * (jw.w + GE(jSw.y + jSw.w, jSw.x + jSw.z)), 1.);	

	// single pixel & end of line detection
	res = min(res * (vec4(jx.z, jy.w, jz.x, jw.y) + NOT(res.wxyz * res.yzwx)), 1.);

	// output

	vec4 clr;
	clr.x = clear(vec2(D.z, E.x), vec2(D.w, E.y), vec2(A.w, D.y));
	clr.y = clear(vec2(F.x, E.z), vec2(E.w, E.y), vec2(B.w, F.y));
	clr.z = clear(vec2(H.z, I.x), vec2(E.w, H.y), vec2(H.w, I.y));
	clr.w = clear(vec2(H.x, G.z), vec2(D.w, H.y), vec2(G.w, G.y));

	vec4 h = vec4(min(D.w, A.w), min(E.w, B.w), min(E.w, H.w), min(D.w, G.w));
	vec4 v = vec4(min(E.y, D.y), min(E.y, F.y), min(H.y, I.y), min(H.y, G.y));

	vec4 or   = GE(h + vec4(D.w, E.w, E.w, D.w), v + vec4(E.y, E.y, H.y, H.y));	// orientation
	vec4 hori = LE(h, v) * clr;	// horizontal edges
	vec4 vert = GE(h, v) * clr;	// vertical edges

	FragColor = (res + 2. * hori + 4. * vert + 8. * or) / 15.;
} 
)GLSL";

const char* kSfxPass3 = R"GLSL(

#define SFX_SCN 1.0

// extract first bool4 from float4 - corners
bvec4 loadCorn(vec4 x){
	return bvec4(floor(mod(x*15. + 0.5, 2.)));
}

// extract second bool4 from float4 - horizontal edges
bvec4 loadHori(vec4 x){
	return bvec4(floor(mod(x*7.5 + 0.25, 2.)));
}

// extract third bool4 from float4 - vertical edges
bvec4 loadVert(vec4 x){
	return bvec4(floor(mod(x*3.75 + 0.125, 2.)));
}

// extract fourth bool4 from float4 - orientation
bvec4 loadOr(vec4 x){
	return bvec4(floor(mod(x*1.875 + 0.0625, 2.)));
}

void main()
{
	/*	grid		corners		mids		

		  B		x   y	  	  x
		D E F				w   y
		  H		w   z	  	  z
	*/
#define TEX(x, y) textureOffset(Source, vTexCoord, ivec2(x, y))

	// read data
	vec4 E = TEX( 0, 0);
	vec4 D = TEX(-1, 0), D0 = TEX(-2, 0), D1 = TEX(-3, 0);
	vec4 F = TEX( 1, 0), F0 = TEX( 2, 0), F1 = TEX( 3, 0);
	vec4 B = TEX( 0,-1), B0 = TEX( 0,-2), B1 = TEX( 0,-3);
	vec4 H = TEX( 0, 1), H0 = TEX( 0, 2), H1 = TEX( 0, 3);
	// extract data
	bvec4 Ec = loadCorn(E), Eh = loadHori(E), Ev = loadVert(E), Eo = loadOr(E);
	bvec4 Dc = loadCorn(D),	Dh = loadHori(D), Do = loadOr(D), D0c = loadCorn(D0), D0h = loadHori(D0), D1h = loadHori(D1);
	bvec4 Fc = loadCorn(F),	Fh = loadHori(F), Fo = loadOr(F), F0c = loadCorn(F0), F0h = loadHori(F0), F1h = loadHori(F1);
	bvec4 Bc = loadCorn(B),	Bv = loadVert(B), Bo = loadOr(B), B0c = loadCorn(B0), B0v = loadVert(B0), B1v = loadVert(B1);
	bvec4 Hc = loadCorn(H),	Hv = loadVert(H), Ho = loadOr(H), H0c = loadCorn(H0), H0v = loadVert(H0), H1v = loadVert(H1);

	
	// lvl1 corners (hori, vert)
	bool lvl1x = Ec.x && (Dc.z || Bc.z || SFX_SCN == 1.);
	bool lvl1y = Ec.y && (Fc.w || Bc.w || SFX_SCN == 1.);
	bool lvl1z = Ec.z && (Fc.x || Hc.x || SFX_SCN == 1.);
	bool lvl1w = Ec.w && (Dc.y || Hc.y || SFX_SCN == 1.);

	// lvl2 mid (left, right / up, down)
	bvec2 lvl2x = bvec2((Ec.x && Eh.y) && Dc.z, (Ec.y && Eh.x) && Fc.w);
	bvec2 lvl2y = bvec2((Ec.y && Ev.z) && Bc.w, (Ec.z && Ev.y) && Hc.x);
	bvec2 lvl2z = bvec2((Ec.w && Eh.z) && Dc.y, (Ec.z && Eh.w) && Fc.x);
	bvec2 lvl2w = bvec2((Ec.x && Ev.w) && Bc.z, (Ec.w && Ev.x) && Hc.y);

	// lvl3 corners (hori, vert)
	bvec2 lvl3x = bvec2(lvl2x.y && (Dh.y && Dh.x) && Fh.z, lvl2w.y && (Bv.w && Bv.x) && Hv.z);
	bvec2 lvl3y = bvec2(lvl2x.x && (Fh.x && Fh.y) && Dh.w, lvl2y.y && (Bv.z && Bv.y) && Hv.w);
	bvec2 lvl3z = bvec2(lvl2z.x && (Fh.w && Fh.z) && Dh.x, lvl2y.x && (Hv.y && Hv.z) && Bv.x);
	bvec2 lvl3w = bvec2(lvl2z.y && (Dh.z && Dh.w) && Fh.y, lvl2w.x && (Hv.x && Hv.w) && Bv.y);

	// lvl4 corners (hori, vert)
	bvec2 lvl4x = bvec2((Dc.x && Dh.y && Eh.x && Eh.y && Fh.x && Fh.y) && (D0c.z && D0h.w), (Bc.x && Bv.w && Ev.x && Ev.w && Hv.x && Hv.w) && (B0c.z && B0v.y));
	bvec2 lvl4y = bvec2((Fc.y && Fh.x && Eh.y && Eh.x && Dh.y && Dh.x) && (F0c.w && F0h.z), (Bc.y && Bv.z && Ev.y && Ev.z && Hv.y && Hv.z) && (B0c.w && B0v.x));
	bvec2 lvl4z = bvec2((Fc.z && Fh.w && Eh.z && Eh.w && Dh.z && Dh.w) && (F0c.x && F0h.y), (Hc.z && Hv.y && Ev.z && Ev.y && Bv.z && Bv.y) && (H0c.x && H0v.w));
	bvec2 lvl4w = bvec2((Dc.w && Dh.z && Eh.w && Eh.z && Fh.w && Fh.z) && (D0c.y && D0h.x), (Hc.w && Hv.x && Ev.w && Ev.x && Bv.w && Bv.x) && (H0c.y && H0v.z));

	// lvl5 mid (left, right / up, down)
	bvec2 lvl5x = bvec2(lvl4x.x && (F0h.x && F0h.y) && (D1h.z && D1h.w), lvl4y.x && (D0h.y && D0h.x) && (F1h.w && F1h.z));
	bvec2 lvl5y = bvec2(lvl4y.y && (H0v.y && H0v.z) && (B1v.w && B1v.x), lvl4z.y && (B0v.z && B0v.y) && (H1v.x && H1v.w));
	bvec2 lvl5z = bvec2(lvl4w.x && (F0h.w && F0h.z) && (D1h.y && D1h.x), lvl4z.x && (D0h.z && D0h.w) && (F1h.x && F1h.y));
	bvec2 lvl5w = bvec2(lvl4x.y && (H0v.x && H0v.w) && (B1v.z && B1v.y), lvl4w.y && (B0v.w && B0v.x) && (H1v.y && H1v.z));

	// lvl6 corners (hori, vert)
	bvec2 lvl6x = bvec2(lvl5x.y && (D1h.y && D1h.x), lvl5w.y && (B1v.w && B1v.x));
	bvec2 lvl6y = bvec2(lvl5x.x && (F1h.x && F1h.y), lvl5y.y && (B1v.z && B1v.y));
	bvec2 lvl6z = bvec2(lvl5z.x && (F1h.w && F1h.z), lvl5y.x && (H1v.y && H1v.z));
	bvec2 lvl6w = bvec2(lvl5z.y && (D1h.z && D1h.w), lvl5w.x && (H1v.x && H1v.w));

	
	// subpixels - 0 = E, 1 = D, 2 = D0, 3 = F, 4 = F0, 5 = B, 6 = B0, 7 = H, 8 = H0

	vec4 crn;
	crn.x = (lvl1x && Eo.x || lvl3x.x && Eo.y || lvl4x.x && Do.x || lvl6x.x && Fo.y) ? 5. : (lvl1x || lvl3x.y && !Eo.w || lvl4x.y && !Bo.x || lvl6x.y && !Ho.w) ? 1. : lvl3x.x ? 3. : lvl3x.y ? 7. : lvl4x.x ? 2. : lvl4x.y ? 6. : lvl6x.x ? 4. : lvl6x.y ? 8. : 0.;
	crn.y = (lvl1y && Eo.y || lvl3y.x && Eo.x || lvl4y.x && Fo.y || lvl6y.x && Do.x) ? 5. : (lvl1y || lvl3y.y && !Eo.z || lvl4y.y && !Bo.y || lvl6y.y && !Ho.z) ? 3. : lvl3y.x ? 1. : lvl3y.y ? 7. : lvl4y.x ? 4. : lvl4y.y ? 6. : lvl6y.x ? 2. : lvl6y.y ? 8. : 0.;
	crn.z = (lvl1z && Eo.z || lvl3z.x && Eo.w || lvl4z.x && Fo.z || lvl6z.x && Do.w) ? 7. : (lvl1z || lvl3z.y && !Eo.y || lvl4z.y && !Ho.z || lvl6z.y && !Bo.y) ? 3. : lvl3z.x ? 1. : lvl3z.y ? 5. : lvl4z.x ? 4. : lvl4z.y ? 8. : lvl6z.x ? 2. : lvl6z.y ? 6. : 0.;
	crn.w = (lvl1w && Eo.w || lvl3w.x && Eo.z || lvl4w.x && Do.w || lvl6w.x && Fo.z) ? 7. : (lvl1w || lvl3w.y && !Eo.x || lvl4w.y && !Ho.w || lvl6w.y && !Bo.x) ? 1. : lvl3w.x ? 3. : lvl3w.y ? 5. : lvl4w.x ? 2. : lvl4w.y ? 8. : lvl6w.x ? 4. : lvl6w.y ? 6. : 0.;

	vec4 mid;
	mid.x = (lvl2x.x &&  Eo.x || lvl2x.y &&  Eo.y || lvl5x.x &&  Do.x || lvl5x.y &&  Fo.y) ? 5. : lvl2x.x ? 1. : lvl2x.y ? 3. : lvl5x.x ? 2. : lvl5x.y ? 4. : (Ec.x && Dc.z && Ec.y && Fc.w) ? ( Eo.x ?  Eo.y ? 5. : 3. : 1.) : 0.;
	mid.y = (lvl2y.x && !Eo.y || lvl2y.y && !Eo.z || lvl5y.x && !Bo.y || lvl5y.y && !Ho.z) ? 3. : lvl2y.x ? 5. : lvl2y.y ? 7. : lvl5y.x ? 6. : lvl5y.y ? 8. : (Ec.y && Bc.w && Ec.z && Hc.x) ? (!Eo.y ? !Eo.z ? 3. : 7. : 5.) : 0.;
	mid.z = (lvl2z.x &&  Eo.w || lvl2z.y &&  Eo.z || lvl5z.x &&  Do.w || lvl5z.y &&  Fo.z) ? 7. : lvl2z.x ? 1. : lvl2z.y ? 3. : lvl5z.x ? 2. : lvl5z.y ? 4. : (Ec.z && Fc.x && Ec.w && Dc.y) ? ( Eo.z ?  Eo.w ? 7. : 1. : 3.) : 0.;
	mid.w = (lvl2w.x && !Eo.x || lvl2w.y && !Eo.w || lvl5w.x && !Bo.x || lvl5w.y && !Ho.w) ? 1. : lvl2w.x ? 5. : lvl2w.y ? 7. : lvl5w.x ? 6. : lvl5w.y ? 8. : (Ec.w && Hc.y && Ec.x && Bc.z) ? (!Eo.w ? !Eo.x ? 1. : 5. : 7.) : 0.;

	// ouput
	FragColor = (crn + 9. * mid) / 80.;
} 
)GLSL";

const char* kSfxPass4 = R"GLSL(

uniform sampler2D PassPrev5Texture;

// extract corners
vec4 loadCrn(vec4 x){
	return floor(mod(x*80. + 0.5, 9.));
}

// extract mids
vec4 loadMid(vec4 x){
	return floor(mod(x*8.888888 + 0.055555, 9.));
}

void main()
{
	/*	grid		corners		mids

		  B		x   y	  	  x
		D E F				w   y
		  H		w   z	  	  z
	*/

	// read data
	vec4 E = texture(Source, vTexCoord);

	// extract data
	vec4 crn = loadCrn(E);
	vec4 mid = loadMid(E);

	// determine subpixel
	vec2 fp = floor(3.0 * fract(vTexCoord * SourceSize.xy));
	float sp = fp.y == 0. ? (fp.x == 0. ? crn.x : fp.x == 1. ? mid.x : crn.y) : (fp.y == 1. ? (fp.x == 0. ? mid.w : fp.x == 1. ? 0. : mid.y) : (fp.x == 0. ? crn.w : fp.x == 1. ? mid.z : crn.z));

	// output coordinate - 0 = E, 1 = D, 2 = D0, 3 = F, 4 = F0, 5 = B, 6 = B0, 7 = H, 8 = H0
	vec2 res = sp == 0. ? vec2(0.,0.) : sp == 1. ? vec2(-1.,0.) : sp == 2. ? vec2(-2.,0.) : sp == 3. ? vec2(1.,0.) : sp == 4. ? vec2(2.,0.) : sp == 5. ? vec2(0,-1) : sp == 6. ? vec2(0.,-2.) : sp == 7. ? vec2(0.,1.) : vec2(0.,2.);

	// ouput
	FragColor = texture(PassPrev5Texture, vTexCoord + res / SourceSize.xy);
} 
)GLSL";

// -- state -------------------------------------------------------------------
struct Program {
    GLuint id = 0;
    bool   tried = false;  // compile attempted (and logged on failure)
    GLint  u_texture = -1;
    GLint  u_source_size = -1;
    GLint  u_tex_rect = -1;
    GLint  u_output_size = -1;
    GLint  u_texture_size = -1;  // ScaleFX passes
    GLint  u_input_size = -1;    // ScaleFX passes
};

// ScaleFX, exactly as libretro's scalefx.glslp: five passes, all nearest, scale
// type "source". Passes 0-3 are 1x of their Source; pass 4 is 3x and is drawn
// straight into the destination viewport. float_framebuffer is set on passes 0
// and 1 only. Textures: 0 = the original (the 1x copy), k + 1 = pass k's output.
// `source` indexes that list; `aux` is the extra texture (-1 none) that the pass
// reads under `aux_name` (pass 2: PassPrev2 = pass 0's output, pass 4: PassPrev5
// = the original).
struct SfxPass {
    const char* name;
    const char* fragment;
    GLenum      format;     // render target format (unused for the last pass)
    int         source;
    int         aux;
    const char* aux_name;
};
const SfxPass kSfxPasses[5] = {
    {"ScaleFX pass 0", kSfxPass0, kRgba16f, 0, -1, nullptr},
    {"ScaleFX pass 1", kSfxPass1, kRgba16f, 1, -1, nullptr},
    {"ScaleFX pass 2", kSfxPass2, kRgba8,   2,  1, "PassPrev2Texture"},
    {"ScaleFX pass 3", kSfxPass3, kRgba8,   3, -1, nullptr},
    {"ScaleFX pass 4", kSfxPass4, kRgba8,   4,  0, "PassPrev5Texture"},
};

struct State {
    bool        loaded = false;      // load_all() attempted
    bool        loaded_ok = false;
    SDL_GLContext context = nullptr; // context the GL objects belong to
    GLuint      vao = 0;
    GLuint      vbo = 0;
    bool        geometry_ready = false;
    Program     programs[3];         // Lcd3x, Xbr, CrtLottes
    // ScaleFX
    bool        fbo_loaded = false;  // load_fbo() attempted
    bool        fbo_ok = false;
    bool        sfx_failed = false;  // FBO functions or a program unavailable
    Program     sfx_copy;
    Program     sfx[5];
    GLuint      sfx_fbo = 0;
    GLuint      sfx_tex[5] = {0, 0, 0, 0, 0};
    int         sfx_w = 0;           // 1x size the textures are allocated for
    int         sfx_h = 0;
};

State& state() {
    static State s;
    return s;
}

void log_info(const char* what, bool is_program, GLuint object) {
    GLint bytes = 0;
    if (is_program) glGetProgramiv(object, kInfoLogLength, &bytes);
    else            glGetShaderiv(object, kInfoLogLength, &bytes);
    std::vector<char> text(bytes > 1 ? static_cast<std::size_t>(bytes) : 1u, 0);
    if (bytes > 1) {
        if (is_program)
            glGetProgramInfoLog(object, static_cast<GLsizei>(text.size()),
                                nullptr, text.data());
        else
            glGetShaderInfoLog(object, static_cast<GLsizei>(text.size()),
                               nullptr, text.data());
    }
    std::fprintf(stderr, "screen_filter: %s failed:\n%s\n", what, text.data());
    std::fflush(stderr);
}

GLuint compile(GLenum type, const char* const* sources, GLsizei count,
               const char* what) {
    GLuint shader = glCreateShader(type);
    if (!shader) return 0;
    glShaderSource(shader, count, sources, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, kCompileStatus, &ok);
    if (!ok) {
        log_info(what, false, shader);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

// Builds one program. Logs the GL info log once on failure; the caller marks
// the program as tried so it is never rebuilt.
void build_program(Program* p, const char* fragment_body, const char* name,
                   const char* vertex = kVertexSource,
                   const char* common = kFragmentCommon) {
    p->tried = true;
    const char* vs_sources[1] = {vertex};
    const char* fs_sources[2] = {common, fragment_body};
    char what[64];
    std::snprintf(what, sizeof(what), "%s vertex shader", name);
    GLuint vs = compile(kVertexShader, vs_sources, 1, what);
    if (!vs) return;
    std::snprintf(what, sizeof(what), "%s fragment shader", name);
    GLuint fs = compile(kFragmentShader, fs_sources, 2, what);
    if (!fs) { glDeleteShader(vs); return; }
    GLuint program = glCreateProgram();
    if (!program) { glDeleteShader(vs); glDeleteShader(fs); return; }
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "a_pos");
    glBindAttribLocation(program, 1, "a_uv");
    glLinkProgram(program);
    GLint ok = 0;
    glGetProgramiv(program, kLinkStatus, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok) {
        std::snprintf(what, sizeof(what), "%s program link", name);
        log_info(what, true, program);
        glDeleteProgram(program);
        return;
    }
    p->id = program;
    p->u_texture = glGetUniformLocation(program, "Texture");
    p->u_source_size = glGetUniformLocation(program, "SourceSize");
    p->u_tex_rect = glGetUniformLocation(program, "TexRect");
    p->u_output_size = glGetUniformLocation(program, "OutputSize");
    p->u_texture_size = glGetUniformLocation(program, "TextureSize");
    p->u_input_size = glGetUniformLocation(program, "InputSize");
}

// Two triangles covering clip space. Each vertex: x, y, u, v with (u,v) = 0,0
// at the top-left of the destination.
void build_geometry(State* s) {
    static const float quad[24] = {
        -1.0f,  1.0f, 0.0f, 0.0f,
        -1.0f, -1.0f, 0.0f, 1.0f,
         1.0f, -1.0f, 1.0f, 1.0f,
        -1.0f,  1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 0.0f,
    };
    glGenVertexArrays(1, &s->vao);
    glGenBuffers(1, &s->vbo);
    glBindVertexArray(s->vao);
    glBindBuffer(kArrayBuffer, s->vbo);
    glBufferData(kArrayBuffer, sizeof(quad), quad, kStaticDraw);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<const void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<const void*>(2 * sizeof(float)));
    s->geometry_ready = s->vao != 0 && s->vbo != 0;
}

// Builds the ScaleFX copy step and its five passes. Any failure marks ScaleFX
// as failed for good (the cause is logged once by build_program).
void build_scalefx(State* s) {
    build_program(&s->sfx_copy, kSfxCopyFragment, "ScaleFX copy", kVertexSourceFbo);
    bool ok = s->sfx_copy.id != 0;
    for (int i = 0; i < 5; ++i) {
        build_program(&s->sfx[i], kSfxPasses[i].fragment, kSfxPasses[i].name,
                      i == 4 ? kVertexSource : kVertexSourceFbo, kSfxCommon);
        if (!s->sfx[i].id) ok = false;
    }
    if (!ok) s->sfx_failed = true;
}

// (Re)allocates the five ScaleFX textures at the 1x size, on texture unit
// `unit_index`. Only reallocates when the size changed.
bool ensure_sfx_chain(State* s, int w, int h, GLint unit_index) {
    if (!s->sfx_fbo) glGenFramebuffers(1, &s->sfx_fbo);
    if (!s->sfx_fbo) return false;
    if (s->sfx_w == w && s->sfx_h == h && s->sfx_tex[0]) return true;
    glActiveTexture(kTexture0 + static_cast<GLenum>(unit_index));
    for (int i = 0; i < 5; ++i) {
        if (!s->sfx_tex[i]) glGenTextures(1, &s->sfx_tex[i]);
        if (!s->sfx_tex[i]) return false;
        glBindTexture(GL_TEXTURE_2D, s->sfx_tex[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        // libretro's default wrap: clamp to border (transparent black).
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, static_cast<GLint>(kClampToBorder));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, static_cast<GLint>(kClampToBorder));
        const GLenum format = i == 0 ? kRgba8 : kSfxPasses[i - 1].format;
        glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(format), w, h, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, nullptr);
    }
    s->sfx_w = w;
    s->sfx_h = h;
    return true;
}

// Runs the ScaleFX copy step and passes. `unit` is the texture unit the SDL
// texture is bound to; `src_unit`/`aux_unit` are two other units this uses.
// The last pass draws into the framebuffer that was bound on entry
// (`final_fbo`) at `dest`. The caller resets the active texture unit.
bool run_scalefx(State* s, GLint unit, GLint src_unit, GLint aux_unit,
                 const SDL_Rect& source, int tex_w, int tex_h, float texw,
                 float texh, int pixel_w, int pixel_h, const SDL_Rect& dest,
                 int out_h, GLuint final_fbo) {
    if (!ensure_sfx_chain(s, pixel_w, pixel_h, src_unit)) return false;
    const float u0 = static_cast<float>(source.x) / tex_w * texw;
    const float v0 = static_cast<float>(source.y) / tex_h * texh;
    const float u1 = static_cast<float>(source.x + source.w) / tex_w * texw;
    const float v1 = static_cast<float>(source.y + source.h) / tex_h * texh;
    const float fw = static_cast<float>(pixel_w);
    const float fh = static_cast<float>(pixel_h);

    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glBindVertexArray(s->vao);

    // Copy step: the SDL texture region at exactly 1x into texture 0.
    glBindFramebuffer(kFramebuffer, s->sfx_fbo);
    glFramebufferTexture2D(kFramebuffer, kColorAttachment0, GL_TEXTURE_2D,
                           s->sfx_tex[0], 0);
    if (glCheckFramebufferStatus(kFramebuffer) != kFramebufferComplete) return false;
    glViewport(0, 0, pixel_w, pixel_h);
    const Program& c = s->sfx_copy;
    glUseProgram(c.id);
    glUniform1i(c.u_texture, unit - static_cast<GLint>(kTexture0));
    glUniform2f(c.u_source_size, fw, fh);
    glUniform4f(c.u_tex_rect, u0, v0, u1, v1);
    glUniform2f(c.u_output_size, fw, fh);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    for (int i = 0; i < 5; ++i) {
        const SfxPass& d = kSfxPasses[i];
        const Program& p = s->sfx[i];
        const bool last = i == 4;
        if (last) {
            glBindFramebuffer(kFramebuffer, final_fbo);
            glViewport(dest.x, out_h - (dest.y + dest.h), dest.w, dest.h);
        } else {
            glFramebufferTexture2D(kFramebuffer, kColorAttachment0, GL_TEXTURE_2D,
                                   s->sfx_tex[i + 1], 0);
            if (glCheckFramebufferStatus(kFramebuffer) != kFramebufferComplete)
                return false;
            glViewport(0, 0, pixel_w, pixel_h);
        }
        glUseProgram(p.id);
        glActiveTexture(kTexture0 + static_cast<GLenum>(src_unit));
        glBindTexture(GL_TEXTURE_2D, s->sfx_tex[d.source]);
        glUniform1i(p.u_texture, src_unit);
        if (d.aux >= 0) {
            glActiveTexture(kTexture0 + static_cast<GLenum>(aux_unit));
            glBindTexture(GL_TEXTURE_2D, s->sfx_tex[d.aux]);
            glUniform1i(glGetUniformLocation(p.id, d.aux_name), aux_unit);
        }
        glUniform2f(p.u_texture_size, fw, fh);
        glUniform2f(p.u_input_size, fw, fh);
        glUniform2f(p.u_output_size, last ? static_cast<float>(dest.w) : fw,
                    last ? static_cast<float>(dest.h) : fh);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    return true;
}

}  // namespace

bool screen_filter_draw(SDL_Renderer* renderer, SDL_Texture* texture,
                        ScreenFilter filter, const SDL_Rect& source,
                        int pixel_w, int pixel_h, const SDL_Rect& dest) {
    if (filter == ScreenFilter::Off || !renderer || !texture) return false;
    if (pixel_w < 1 || pixel_h < 1 || source.w < 1 || source.h < 1 ||
        dest.w < 1 || dest.h < 1)
        return false;
    State& s = state();
    if (s.loaded && !s.loaded_ok) return false;
    const bool sfx = filter == ScreenFilter::ScaleFx;
    Program& prog = sfx ? s.sfx_copy
                  : s.programs[filter == ScreenFilter::Lcd3x ? 0
                             : filter == ScreenFilter::Xbr ? 1 : 2];
    if (sfx && s.sfx_failed) return false;
    if (prog.tried && !prog.id) return false;

    SDL_RendererInfo info{};
    if (SDL_GetRendererInfo(renderer, &info) != 0 || !info.name ||
        std::strcmp(info.name, "opengl") != 0)
        return false;
    // `dest` is in renderer-output pixels and the draw below is raw GL, so
    // SDL's logical size (the native 240x160 path) does not apply to it.
    int out_w = 0, out_h = 0;
    if (SDL_GetRendererOutputSize(renderer, &out_w, &out_h) != 0 ||
        out_w < 1 || out_h < 1)
        return false;
    int tex_w = 0, tex_h = 0;
    if (SDL_QueryTexture(texture, nullptr, nullptr, &tex_w, &tex_h) != 0 ||
        tex_w < 1 || tex_h < 1)
        return false;

    // Commit SDL's batched draws first (and make its GL context current):
    // everything below talks to GL directly.
    SDL_RenderFlush(renderer);
    SDL_GLContext context = SDL_GL_GetCurrentContext();
    if (!context) return false;

    if (!s.loaded) {
        s.loaded = true;
        s.loaded_ok = load_all();
        if (!s.loaded_ok) {
            std::fprintf(stderr,
                         "screen_filter: required GL functions unavailable; "
                         "filters disabled\n");
            std::fflush(stderr);
            return false;
        }
    }
    if (s.context && s.context != context) return false;
    if (sfx && !s.fbo_loaded) {
        s.fbo_loaded = true;
        s.fbo_ok = load_fbo();
        if (!s.fbo_ok) {
            s.sfx_failed = true;
            std::fprintf(stderr,
                         "screen_filter: framebuffer functions unavailable; "
                         "ScaleFX disabled\n");
            std::fflush(stderr);
        }
    }
    if (sfx && s.sfx_failed) return false;

    // Back up everything touched, the way imgui_impl_opengl3 does: SDL's
    // renderer caches its own GL state and would not notice a change.
    GLint prev_active = 0, prev_texture = 0, prev_program = 0;
    GLint prev_array_buffer = 0, prev_vao = 0;
    GLint prev_viewport[4] = {0, 0, 0, 0};
    const GLboolean prev_scissor = glIsEnabled(GL_SCISSOR_TEST);
    const GLboolean prev_blend = glIsEnabled(GL_BLEND);
    glGetIntegerv(kActiveTextureEnum, &prev_active);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_texture);
    glGetIntegerv(kCurrentProgram, &prev_program);
    glGetIntegerv(kArrayBufferBinding, &prev_array_buffer);
    glGetIntegerv(kVertexArrayBinding, &prev_vao);
    glGetIntegerv(GL_VIEWPORT, prev_viewport);
    // ScaleFX also moves the framebuffer bindings and uses two more texture
    // units, so back up those too (units 0-2 cover any pair besides SDL's).
    GLint prev_draw_fbo = 0, prev_read_fbo = 0, prev_unit_tex[3] = {0, 0, 0};
    if (sfx) {
        glGetIntegerv(kDrawFramebufferBinding, &prev_draw_fbo);
        glGetIntegerv(kReadFramebufferBinding, &prev_read_fbo);
        for (int u = 0; u < 3; ++u) {
            glActiveTexture(kTexture0 + static_cast<GLenum>(u));
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_unit_tex[u]);
        }
        glActiveTexture(static_cast<GLenum>(prev_active));
    }

    float texw = 1.0f, texh = 1.0f;
    if (SDL_GL_BindTexture(texture, &texw, &texh) != 0) return false;
    GLint unit = 0;
    glGetIntegerv(kActiveTextureEnum, &unit);
    GLint prev_min = 0, prev_mag = 0;
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &prev_min);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &prev_mag);

    bool drawn = false;
    // A rectangle texture (coordinates in pixels, not 0..1) cannot be read
    // through sampler2D.
    if (texw <= 1.0001f && texh <= 1.0001f) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

        if (!s.context) s.context = context;
        if (!s.geometry_ready) build_geometry(&s);
        if (!prog.tried) {
            if (sfx)
                build_scalefx(&s);
            else if (filter == ScreenFilter::Lcd3x)
                build_program(&prog, kLcd3xFragment, "LCD3x");
            else if (filter == ScreenFilter::Xbr)
                build_program(&prog, kXbrFragment, "xBR");
            else
                build_program(&prog, kLottesFragment, "CRT Lottes");
        }
        if (sfx) {
            if (prog.id && s.geometry_ready && !s.sfx_failed) {
                GLint src_unit = 0, aux_unit = 0, found = 0;
                for (GLint u = 0; u < 3 && found < 2; ++u) {
                    if (u == unit - static_cast<GLint>(kTexture0)) continue;
                    (found++ == 0 ? src_unit : aux_unit) = u;
                }
                drawn = run_scalefx(&s, unit, src_unit, aux_unit, source, tex_w,
                                    tex_h, texw, texh, pixel_w, pixel_h, dest,
                                    out_h, static_cast<GLuint>(prev_draw_fbo));
                glActiveTexture(static_cast<GLenum>(unit));
            }
        } else if (prog.id && s.geometry_ready) {
            const float u0 = static_cast<float>(source.x) / tex_w * texw;
            const float v0 = static_cast<float>(source.y) / tex_h * texh;
            const float u1 = static_cast<float>(source.x + source.w) / tex_w * texw;
            const float v1 = static_cast<float>(source.y + source.h) / tex_h * texh;

            glViewport(dest.x, out_h - (dest.y + dest.h), dest.w, dest.h);
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_BLEND);
            glUseProgram(prog.id);
            glUniform1i(prog.u_texture, unit - static_cast<GLint>(kTexture0));
            glUniform2f(prog.u_source_size, static_cast<float>(pixel_w),
                        static_cast<float>(pixel_h));
            glUniform4f(prog.u_tex_rect, u0, v0, u1, v1);
            glUniform2f(prog.u_output_size, static_cast<float>(dest.w),
                        static_cast<float>(dest.h));
            glBindVertexArray(s.vao);
            glDrawArrays(GL_TRIANGLES, 0, 6);
            drawn = true;
        }
    }

    // Restore, newest first. The texture's own filter parameters go back while
    // it is still bound.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, prev_min);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, prev_mag);
    if (sfx) {
        glBindFramebuffer(kDrawFramebuffer, static_cast<GLuint>(prev_draw_fbo));
        glBindFramebuffer(kReadFramebuffer, static_cast<GLuint>(prev_read_fbo));
    }
    glBindVertexArray(static_cast<GLuint>(prev_vao));
    glBindBuffer(kArrayBuffer, static_cast<GLuint>(prev_array_buffer));
    glUseProgram(static_cast<GLuint>(prev_program));
    glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2],
               prev_viewport[3]);
    if (prev_scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (prev_blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    SDL_GL_UnbindTexture(texture);
    glActiveTexture(static_cast<GLenum>(unit));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev_texture));
    if (sfx) {
        for (int u = 0; u < 3; ++u) {
            glActiveTexture(kTexture0 + static_cast<GLenum>(u));
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev_unit_tex[u]));
        }
    }
    glActiveTexture(static_cast<GLenum>(prev_active));
    return drawn;
}

void screen_filter_shutdown() {
    State& s = state();
    // GL objects can only be freed from the context that made them. If that
    // context is not current (or already gone) the driver reclaims them with
    // the context, so just forget them.
    if (s.loaded_ok && s.context && SDL_GL_GetCurrentContext() == s.context) {
        for (Program& p : s.programs)
            if (p.id) glDeleteProgram(p.id);
        if (s.sfx_copy.id) glDeleteProgram(s.sfx_copy.id);
        for (Program& p : s.sfx)
            if (p.id) glDeleteProgram(p.id);
        if (s.fbo_ok) {
            for (GLuint t : s.sfx_tex)
                if (t) glDeleteTextures(1, &t);
            if (s.sfx_fbo) glDeleteFramebuffers(1, &s.sfx_fbo);
        }
        if (s.vbo) glDeleteBuffers(1, &s.vbo);
        if (s.vao) glDeleteVertexArrays(1, &s.vao);
    }
    s = State{};
}

}  // namespace gbarecomp

#else  // !GBARECOMP_HAVE_SDL2

namespace gbarecomp {

bool screen_filter_draw(SDL_Renderer*, SDL_Texture*, ScreenFilter,
                        const SDL_Rect&, int, int, const SDL_Rect&) {
    return false;
}

void screen_filter_shutdown() {}

}  // namespace gbarecomp

#endif
