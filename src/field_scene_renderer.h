// field_scene_renderer.h -- draws a described frame with the graphics card.
//
// Step 4 of docs/NATIVE_SCENE_RENDERER_PLAN.md, Golden Sun's half. It takes a
// FieldScene (what the game asked for) plus the frame's video memory, and
// draws it through gbarecomp's generic GpuSurface.
//
// How the tiles reach the card. Video memory and the palettes are uploaded as
// they are -- raw bytes and raw 15-bit colours -- and the shader does the whole
// lookup itself: map entry, tile, pixel, palette, colour. Nothing is decoded on
// the processor. That is the difference between this and the offline preview
// tool, which rebuilt a whole room into one picture and took 173-399 ms to do
// it. Here an upload is 96 KB when video memory changes and nothing at all when
// it does not.
//
// What this draws today: Modes 0-2's background layers, regular and affine,
// and objects, faithfully, at the native viewport, including fades, alpha
// blending, semi-transparent objects, and windows (WIN0, WIN1 and the object
// window) -- window and background scroll registers are read per scanline,
// exactly as the console does, so a frame that rewrites one mid-frame (the
// room-transition iris rewrites WIN0H every scanline to carve its shrinking
// circle) draws correctly too. The room buffer only ever sources a regular
// field layer (BG1-3 outside battle/overworld); an affine layer always draws
// from video memory. The bitmap modes (3-5) and mosaic are the remaining
// pieces; the renderer reports what it cannot yet draw rather than drawing
// it wrongly, and the caller falls back to the emulated hardware path for
// the whole frame.
#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

#include "effect_particles.h"
#include "field_scene.h"
#include "gpu_surface.h"

namespace gsr {

class FieldSceneRenderer {
public:
    FieldSceneRenderer() = default;
    ~FieldSceneRenderer();
    FieldSceneRenderer(const FieldSceneRenderer&) = delete;
    FieldSceneRenderer& operator=(const FieldSceneRenderer&) = delete;

    // Compiles the shaders and creates the textures. `surface` must already be
    // initialised and outlive this renderer. False means this host cannot run
    // the path; `failure()` says why and the caller keeps its old renderer.
    bool init(gbarecomp::GpuSurface* surface);
    bool ready() const { return ready_; }
    const char* failure() const { return failure_; }

    // Chooses the output size. The native viewport is 240x160; anything wider
    // shows more of the world through the same camera, which is the point of
    // the whole exercise.
    bool set_output_size(int width, int height);

    // Uploads video memory and palettes. Call when they have changed; the
    // renderer does not guess. `vram_bytes` is normally 96 KB.
    void upload_vram(const std::uint8_t* vram, std::size_t vram_bytes);
    // Video memory for room tiles drawn OUTSIDE the console's 240x160:
    // the same bytes, except that a character block a game window has
    // borrowed keeps what it held before the window opened. Optional;
    // upload_vram() also fills it, so without this call both agree.
    void upload_room_vram(const std::uint8_t* vram, std::size_t vram_bytes);
    void upload_palette(const std::uint16_t* palette, std::size_t entries);

    // Selects the field's background source: video memory only (default --
    // what the pixel-identity gate compares against, and what BG0, the
    // lighting layer, always uses -- FACTS.md, "the lighting layer cannot be
    // reconstructed by the room buffer's method"), or the room's own tables
    // for BG1-3, falling back to video memory per pixel wherever the room
    // buffer does not know the answer.
    void set_room_source_enabled(bool enabled);

    // Supplement the game's canvas with captured sparks beyond its edges.
    // Off by default; disabling this preserves the original picture.
    void set_effect_renderer_enabled(bool enabled);

    // Stretch the effect's canvas across the whole view. Off: the effect
    // keeps the size the game drew it at, and the view is simply wider than
    // the artwork that exists. On: the same artwork covers the full width,
    // magnified to do so. There is no third option -- past canvas column 117
    // of 127 there is nothing drawn, so 124 pixels of a 360-wide view have no
    // artwork in existence to show (FACTS.md, 2026-09-18).
    void set_effect_span_enabled(bool enabled);

    // Name the spell-effect layer for this frame, so it can be sampled
    // through the widened view instead of the console window. -1 for none.
    void set_effect_layer(int layer);

    // Stamp the captured artwork at canvas coordinates. The GPU samples it
    // with the original layer transform. Returns false if no ink was stamped.
    bool upload_effect_sparks(const EffectSpark* sparks, int count,
                              const std::uint8_t* art, std::size_t art_bytes,
                              int effect_layer);

    // After upload_effect_sparks: find where the effect layer's map shows
    // the game's 128x128 canvas (a wrapping 256x256 affine layer with it at
    // the map origin, or a 256x256 8-bit regular layer with it anywhere) and
    // read the canvas's fill (its value wherever no spark landed). Then the
    // margins draw fill plus our sparks instead of the wrapped map, so the
    // spell neither repeats nor stops at its canvas edge. `vram` is the
    // frame's video memory.
    //
    // find_effect_canvas_layer picks the layer for set_effect_layer: BG2 or
    // BG1, whichever shows the canvas that way, else -1.
    void analyse_effect_canvas(const std::uint8_t* vram, std::size_t vram_bytes,
                               const SceneLayer& layer);
    static int find_effect_canvas_layer(const std::uint8_t* vram,
                                        std::size_t vram_bytes,
                                        const FieldScene& scene);

    // F12-only readback: distinguish disabled drawing from a lost GPU upload.
    void log_effect_state() const;

    bool room_source_enabled() const { return room_source_enabled_; }

    // The world map's whole tile layout (WorldMapSource::tiles(): 512 wide,
    // BG3 rows 0..511 then BG2 rows 512..1023), used for world-map pixels
    // OUTSIDE the console's 240x160, where the game's own map ring may hold
    // leftovers. Null turns it off. Re-uploads only when `generation` moves.
    void upload_world_map(const std::uint8_t* tiles, std::uint32_t generation);

    // Loads this frame's room tables -- rect, camera, id grid, atlas -- from
    // an EWRAM byte buffer. `ewram[0]` must be GBA address 0x02000000,
    // whether that is the live guest's EWRAM or a recorded snapshot's
    // "EWRAM" section (src/map_recorder.cpp). The four addresses this reads
    // and the per-layer rule draw() applies with them are documented in
    // src/room_buffer.h, with their evidence in FACTS.md; nothing here is a
    // new address. Safe to call every frame. Returns false ("not known")
    // when the rect does not describe a real room -- draw() then falls back
    // to video memory for every pixel of every field layer, same as any
    // other per-pixel miss.
    bool upload_room(const std::uint8_t* ewram, std::size_t ewram_bytes);
    bool room_known() const { return room_valid_; }

    // False until the game has loaded its first room -- the map number at
    // 0x02000408 is still 0. Nothing on screen then belongs to a world, so no
    // layer may be widened into the margins. Set by upload_room(), which reads
    // that address whether or not the room rect itself is usable.
    bool world_loaded() const { return world_loaded_; }

    // Diagnostic mirror of the room-buffer branch the background shader
    // runs, in plain C++, using the room data the last upload_room() call
    // accepted. `screen_x`/`screen_y` are console-space pixels (0..239,
    // 0..159 at the native viewport; can run negative or past 240/160 for a
    // widened output), `scroll_x`/`scroll_y` are the layer's own UNMASKED
    // HOFS/VOFS (FieldScene::SceneLayer::scroll_x/scroll_y). Lets a checker
    // count what the shader will draw without a GPU readback. Kept in sync
    // with the shader by hand; a mismatch between the two would show up as
    // a differing pixel in the pixel-identity gate.
    bool query_room_known(int screen_x, int screen_y, int scroll_x,
                          int scroll_y) const;

    // The reconstructed room's own rectangle and the game's camera inside
    // it, both in room pixels, as the last accepted upload_room() read them.
    // Diagnostic only: a checker asking "is this margin pixel black because
    // the room ENDS here, or because the reconstruction is missing a piece
    // of it" needs the rect to tell those two apart.
    int room_min_x() const { return room_min_x_; }
    int room_max_x() const { return room_max_x_; }
    int room_min_y() const { return room_min_y_; }
    int room_max_y() const { return room_max_y_; }
    int room_camera_x() const { return room_camera_x_; }
    int room_camera_y() const { return room_camera_y_; }

    // Draws one described frame. Returns false when the scene contains
    // something this renderer cannot reproduce, having drawn nothing -- the
    // caller must then use the emulated hardware path for the WHOLE frame.
    // There is no correct way to draw part of it (FACTS.md).
    bool draw(const FieldScene& scene);

    // The finished picture, for presenting or for comparison.
    gbarecomp::GpuTexture output_texture() const;
    int output_width() const { return output_w_; }
    int output_height() const { return output_h_; }

    // Why the last draw() declined. Null when it succeeded.
    const char* declined_reason() const { return declined_; }

private:
    gbarecomp::GpuSurface* surface_ = nullptr;
    bool ready_ = false;
    const char* failure_ = "not initialised";
    const char* declined_ = nullptr;

    gbarecomp::GpuProgram background_ = 0;
    gbarecomp::GpuProgram object_ = 0;
    gbarecomp::GpuProgram resolve_ = 0;
    // Two more one-purpose passes windows need: one marks where the mode-2
    // "window" objects are opaque, the other turns that plus WIN0/WIN1 into
    // the per-pixel 6-bit control byte the peel and resolve passes read.
    gbarecomp::GpuProgram obj_window_ = 0;
    gbarecomp::GpuProgram window_ = 0;
    gbarecomp::GpuTexture vram_ = 0;
    gbarecomp::GpuTexture room_vram_ = 0;
    gbarecomp::GpuTexture palette_ = 0;
    // Room buffer: 128x128 metatile ids and their 4-entry-per-id atlas, both
    // uploaded whole on upload_room() (see room_buffer.h for the addresses).
    // The host-side copies exist only so query_room_known() can answer
    // without a GPU readback; the shader reads its own copy from the GPU
    // textures below.
    gbarecomp::GpuTexture id_grid_ = 0;
    gbarecomp::GpuTexture atlas_ = 0;
    // This frame's per-row registers (FieldScene::kLineIoBytes bytes x
    // FieldScene::kRows), uploaded fresh every draw() call. The background
    // and window shaders read window and scroll state through this instead
    // of a frame-level uniform, which is what lets a frame that rewrites a
    // register mid-frame (the room-transition iris rewrites WIN0H every
    // scanline) draw correctly instead of being refused.
    gbarecomp::GpuTexture row_io_ = 0;
    // This frame's per-row affine reference points (8 halves x
    // FieldScene::kRows): the console's internal accumulator for BG2 and BG3,
    // which its register file does not hold. An affine layer is sampled
    // through this, never through a frame-level reference -- the overworld
    // and every battle arena rewrite the transform per scanline.
    gbarecomp::GpuTexture row_affine_ = 0;
    gbarecomp::GpuTexture world_map_ = 0;
    bool world_map_valid_ = false;
    std::uint32_t world_map_generation_ = 0;
    bool room_source_enabled_ = false;
    bool room_valid_ = false;
    // Our spell-effect canvas: the host-side buffer, its texture, and which
    // layer (if any) the next draw() should take from it.
    gbarecomp::GpuTexture effect_canvas_ = 0;
    EffectCanvas effect_pixels_;
    bool effect_enabled_ = false;
    int effect_layer_ = -1;
    bool effect_span_ = false;
    bool effect_have_sparks_ = false;
    bool effect_wrap_canvas_ = false;
    bool effect_text_canvas_ = false;
    int effect_canvas_map_x_ = 0, effect_canvas_map_y_ = 0;
    int effect_fill_ = 0;
    // Default true: a caller that never uploads a room (the offline checks)
    // must behave exactly as before this flag existed.
    bool world_loaded_ = true;
    int room_min_x_ = 0, room_max_x_ = 0, room_min_y_ = 0, room_max_y_ = 0;
    int room_camera_x_ = 0, room_camera_y_ = 0;
    std::vector<std::uint16_t> room_ids_;    // 128*128, low 12 bits
    std::vector<std::uint16_t> room_atlas_;  // 4096*4, id-major
    // Two-layer depth peel: every quad is drawn twice, once recording the
    // frontmost ("top") candidate at each pixel and once recording the
    // runner-up ("second"), then a resolve pass applies the console's alpha
    // blend / brightness rule exactly as the reference compositor does. See
    // field_scene_renderer.cpp for why painter's order alone cannot do this.
    gbarecomp::GpuTexture top_ = 0;
    gbarecomp::GpuTexture second_ = 0;
    gbarecomp::GpuTexture obj_window_mask_ = 0;
    gbarecomp::GpuTexture window_control_ = 0;
    int output_w_ = 0, output_h_ = 0;
};

}  // namespace gsr
