// gpu_surface.h -- a small, game-agnostic OpenGL surface for host rendering.
//
// Step 3 of the modern renderer plan. This is the generic half of that work and
// belongs to the platform core: it knows about shaders, textures, render
// targets and quads, and nothing whatsoever about any game's map, rooms or
// actors. Game-specific drawing lives in the game repository and drives this.
//
// It deliberately does NOT create a window or a GL context. The host window
// already runs an SDL renderer on the "opengl" driver, and Dear ImGui already
// issues its own GL calls beside it at present time. This attaches to that
// existing context and is careful to leave every piece of GL state it touches
// exactly as it found it, because SDL's renderer batches its own drawing and
// would otherwise be corrupted.
//
// Availability is a runtime question, not a build-time one: a machine may give
// us a context too old for shaders and framebuffers. `init()` reports that
// honestly and the caller keeps using whatever it was using before. There is
// no partial mode.
#pragma once

#include <cstddef>
#include <cstdint>

namespace gbarecomp {

// Opaque handles. Zero always means "nothing".
using GpuTexture = unsigned;
using GpuProgram = unsigned;

enum class GpuTextureFormat {
    // Integer formats, read in a shader with usampler2D and texelFetch. These
    // carry numbers -- memory bytes, tile indices, map entries, palettes -- and
    // must never be filtered or normalised.
    R8UI,      // one byte per texel
    R16UI,     // one 16-bit unsigned integer per texel
    // A normalised colour format, read with sampler2D.
    RGBA8,     // four bytes per texel: finished colour
};

enum class GpuBlendMode { Off, Alpha, Additive };

// One textured quad in whatever space the caller's shader works in.
struct GpuQuad {
    float x = 0, y = 0, w = 0, h = 0;          // destination rectangle
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;      // source rectangle, 0..1
    float depth = 0;                            // smaller draws in front
    float tint[4] = {1, 1, 1, 1};
};

class GpuSurface {
public:
    GpuSurface() = default;
    ~GpuSurface();
    GpuSurface(const GpuSurface&) = delete;
    GpuSurface& operator=(const GpuSurface&) = delete;

    // Loads the GL entry points from the current context and checks the
    // version is new enough. Returns false when this host cannot support the
    // path at all; the caller must then carry on with its previous renderer.
    // Safe to call more than once.
    bool init();
    bool ready() const { return ready_; }

    // Why init() failed, for diagnostics. Never shown to a player.
    const char* failure() const { return failure_; }

    // ── resources ───────────────────────────────────────────────────────────

    // Returns zero when the current context cannot allocate the requested size.
    GpuTexture create_texture(int width, int height, GpuTextureFormat format,
                              bool filtered = false);
    void update_texture(GpuTexture texture, int x, int y, int width, int height,
                        const void* pixels);
    void destroy_texture(GpuTexture texture);

    // Compiles a vertex and fragment shader pair. Returns 0 on failure and
    // writes the compiler's own message into `log`, which is what makes a
    // shader mistake findable.
    GpuProgram create_program(const char* vertex_source,
                              const char* fragment_source,
                              char* log, std::size_t log_bytes);
    void destroy_program(GpuProgram program);

    // Uniform setters, by name. A name the program does not use is ignored
    // rather than treated as an error: shaders legitimately drop unused work.
    void set_uniform_int(GpuProgram p, const char* name, int value);
    void set_uniform_float(GpuProgram p, const char* name, float value);
    void set_uniform_vec2(GpuProgram p, const char* name, float x, float y);
    void set_uniform_vec4(GpuProgram p, const char* name,
                          float x, float y, float z, float w);
    // Column-major 4x4, the layout GL expects.
    void set_uniform_mat4(GpuProgram p, const char* name, const float* m);

    // Read back the values a diagnostic needs without rebinding the program.
    bool get_uniform_int(GpuProgram p, const char* name, int* value);
    bool get_uniform_vec2(GpuProgram p, const char* name, float* values);

    // On-demand, payload-free audit of an R8UI upload. Logs actual storage,
    // filtering, the named unit's binding, and comparison with the CPU bytes.
    // No readback is attempted unless format and dimensions match exactly.
    void log_texture_r8ui(GpuTexture texture, unsigned unit,
                          int width, int height, const std::uint8_t* expected);

    // ── drawing ─────────────────────────────────────────────────────────────

    // Creates or resizes the offscreen target everything is drawn into. Kept
    // separate from the window so output size is a free choice.
    bool set_target_size(int width, int height);
    int  target_width() const { return target_w_; }
    int  target_height() const { return target_h_; }
    GpuTexture target_texture() const { return target_tex_; }

    // Begins drawing into the offscreen target, saving the GL state that SDL
    // and ImGui rely on. Every begin must be matched by an end.
    void begin_frame(float clear_r, float clear_g, float clear_b, float clear_a);
    void end_frame();

    // Blending: Off, Alpha (src alpha, one minus src alpha) or Additive (src
    // alpha, one). Between begin_frame and end_frame only; begin/end save and
    // restore the blend enable and blend function, so the caller need not.
    // set_blend_alpha(bool) is shorthand for Alpha / Off.
    void set_blend_mode(GpuBlendMode mode);
    void set_blend_alpha(bool enabled);

    // Draws quads with the given program. Textures are bound to units 0..n-1
    // in the order given, so a shader samples tile art, map and palette
    // together. Must be called between begin_frame and end_frame.
    void draw_quads(GpuProgram program, const GpuQuad* quads, std::size_t count,
                    const GpuTexture* textures, std::size_t texture_count);

    // Redirects the framebuffer's colour attachment to a texture the caller
    // owns, for multi-pass rendering into scratch targets without a second
    // framebuffer object. Pass 0 to point back at the surface's own target
    // texture. Only valid between begin_frame and end_frame; the depth
    // attachment, if any, is left exactly as it was.
    void bind_color_target(GpuTexture texture);

    // Clears whichever texture is currently the colour attachment. Separate
    // from begin_frame's own clear because a multi-pass caller redirects the
    // attachment more than once per frame and each pass wants its own clear
    // colour.
    void clear_color_target(float r, float g, float b, float a);

    // Depth testing, for callers doing more than simple back-to-front
    // painting (a two-pass peel, for instance). The depth buffer is a
    // renderbuffer attached alongside the colour attachment, sized to the
    // current target and (re)created the first time this is enabled or after
    // set_target_size changes the size. Smaller depth wins (GL_LESS).
    // Disabling turns the GL depth test off again; the renderbuffer itself is
    // kept rather than freed, so toggling within a frame is cheap.
    void set_depth_enabled(bool enabled);

    // Clears the depth buffer. A no-op before set_depth_enabled(true) has ever
    // run, since there is then nothing to clear.
    void clear_depth(float value = 1.0f);

    // Draws the finished target into the currently bound framebuffer, scaled to
    // the given rectangle in pixels. Used to put the frame on screen.
    void present_target(int x, int y, int width, int height,
                        int viewport_width, int viewport_height,
                        bool filtered);

    // Reads a texture's pixels back to host memory as top-down RGB888 (three
    // bytes per pixel, row 0 first, no padding) -- the layout the rest of the
    // host already presents. GL stores a texture bottom-up and this format is
    // RGBA, so this flips rows and drops the alpha channel; `out_rgb` must
    // hold at least width*height*3 bytes. This is a full GPU round trip:
    // only call it where a caller has already decided that cost is worth
    // paying (see MODIFICATIONS.md).
    void read_texture_rgb(GpuTexture texture, int width, int height,
                          std::uint8_t* out_rgb);

    // The same readback split in two, so the CPU does not sit waiting for
    // the GPU to finish drawing. begin_texture_readback queues a copy of the
    // texture into host-readable memory and returns at once (false when the
    // context cannot do that, OpenGL 3.2 sync objects being required).
    // finish_texture_readback delivers the copy queued by the PREVIOUS begin,
    // in read_texture_rgb's layout -- normally long finished by then -- and
    // returns false if there is none of this size or the GPU did not finish
    // it in time. A caller presenting the result runs one frame behind.
    bool begin_texture_readback(GpuTexture texture, int width, int height);
    bool finish_texture_readback(int width, int height, std::uint8_t* out_rgb);

private:
    bool load_entry_points();
    // Keeps the depth buffer the same size as the colour attachment.
    void resize_depth_to_target();
    void release();

    bool ready_ = false;
    const char* failure_ = "not initialised";

    unsigned vao_ = 0, vbo_ = 0;
    unsigned fbo_ = 0;
    GpuTexture target_tex_ = 0;
    int target_w_ = 0, target_h_ = 0;

    unsigned depth_rb_ = 0;
    int depth_w_ = 0, depth_h_ = 0;

    GpuProgram blit_program_ = 0;
    std::size_t vbo_capacity_ = 0;

    // Saved GL state, restored by end_frame.
    struct SavedState;
    SavedState* saved_ = nullptr;
    bool in_frame_ = false;

    // Per-program uniform locations and last values set, so a setter costs
    // one GL call (or none, when the value is unchanged) instead of a program
    // query, two program switches, a name lookup and the set itself.
    struct UniformSlot;
    struct UniformCache;
    UniformCache* uniforms_ = nullptr;

    // Two pixel-pack buffers used in turn by begin/finish_texture_readback.
    struct Readback;
    Readback* readback_ = nullptr;
    UniformSlot* uniform_slot(GpuProgram p, const char* name);
    void forget_program_uniforms(GpuProgram p);
};

}  // namespace gbarecomp
