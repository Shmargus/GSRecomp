// field_scene.h -- what the game asked to be drawn this frame.
//
// Step 1 of docs/NATIVE_SCENE_RENDERER_PLAN.md. Nothing here draws anything.
// It captures, once per frame, everything a renderer needs in order to produce
// that frame WITHOUT reading emulated memory again while it draws.
//
// Why this exists before any GPU code. The existing renderer decides each
// pixel while the frame is still being emulated, reading live registers as it
// goes. A renderer that draws the whole frame at the end instead needs the
// frame handed to it as data. Capturing that data first, and proving the old
// renderer can be driven from it alone, is what shows the description is
// complete -- before a single triangle depends on it.
//
// Two things are deliberately NOT copied here:
//
//   * Video memory (96 KB) and its tile art. Copying it every frame would cost
//     more than it saves. The renderer keeps it on the graphics card and
//     refreshes it when the game writes, which is a renderer concern.
//   * The room's reconstructed map. `room_buffer.cpp` already owns that and
//     rebuilds it on room entry; the scene only needs to say WHICH room, so
//     the renderer knows whether what it already uploaded is still current.
//
// What IS copied is small and changes every frame: the display and layer
// registers, each row's own copy of them where the game moved one mid-frame,
// the object table, and both palettes. That is 3 KB or so per frame.
//
// The per-row tables are not new work: the PPU already latches every visible
// row's IO and affine reference (FACTS.md, "The seams a GPU renderer needs
// already exist"). This just takes a copy that outlives the frame.
#pragma once

#include <array>
#include <cstdint>

namespace gsr {

// One background layer, as the game set it up for this frame.
struct SceneLayer {
    bool     enabled = false;
    int      priority = 0;         // 0 = front, 3 = back
    unsigned char_base = 0;        // byte offset into video memory
    unsigned screen_base = 0;      // byte offset into video memory
    bool     eight_bit_colour = false;
    bool     mosaic = false;
    bool     wraps = false;        // affine layers only
    unsigned size_code = 0;        // 0-3, meaning depends on regular/affine
    int      scroll_x = 0;         // UNMASKED, as the game wrote it
    int      scroll_y = 0;
    bool     affine = false;
    // Set only when `affine` is true: BG2PA/PB/PC/PD or BG3PA/PB/PC/PD,
    // signed 16-bit Q8.8, exactly as the hardware stores them
    // (gba_ppu.cpp, read_s16). Zero for a regular layer.
    int      affine_pa = 0, affine_pb = 0, affine_pc = 0, affine_pd = 0;
    // The reference point BG2X/Y or BG3X/Y, signed 28-bit Q19.8, exactly as
    // written (gba_ppu.cpp, read_s28_ref) -- the frame's own value; the
    // console's internal accumulator walks this by pb/pd every scanline,
    // which the renderer reproduces per pixel rather than per row. Zero for
    // a regular layer.
    std::int32_t affine_ref_x = 0, affine_ref_y = 0;
};

// One object (sprite) slot, decoded from the object table.
struct SceneObject {
    bool     present = false;      // false = this slot draws nothing
    int      x = 0, y = 0;         // screen position, sign-resolved
    int      width = 0, height = 0;
    unsigned tile = 0;
    int      palette = 0;          // -1 when the object is 8-bit colour
    int      priority = 0;
    bool     hflip = false, vflip = false;
    bool     blended = false;      // object mode 1
    bool     window = false;       // object mode 2: shapes the object window
    bool     mosaic = false;
    bool     affine = false;
    int      affine_index = -1;    // parameter group, or -1
    bool     double_size = false;
    // True only when BOTH x and y came from the widescreen provenance
    // providers (g_ws_obj_attr_x_provider / g_ws_obj_attr_y_provider in
    // gba_ppu.h), the same test gba_ppu.cpp's expanded object path uses
    // (`sprite_trusted = sx_trusted && sy_trusted`). An untrusted object's
    // position is only the hardware sign-wrap fallback, which Golden Sun
    // sometimes uses to park a sprite off the visible 240x160 window on
    // purpose; a renderer must confine it to that authentic window, exactly
    // as emit_obj does, rather than draw it floating in the margins.
    bool     trusted = false;

    // True only for a slot sitting on Golden Sun's parked-sprite sentinel
    // (src/widescreen_policy.h). Such a slot is invisible on hardware and
    // must stay invisible when the view is widened; every other object,
    // authenticated or not, is drawn where it really is. Replaces `trusted`
    // as the renderer's clip test, 2026-09-17: the providers only
    // authenticate FIELD sprites, so clipping on `trusted` cut every battle
    // monster and effect sprite off at the console's old screen edge.
    bool     parked = false;
};

// The windows and colour effects, which decide what is visible and how two
// layers combine.
struct SceneEffects {
    bool     win0 = false, win1 = false, obj_window = false;
    unsigned win0h = 0, win0v = 0, win1h = 0, win1v = 0;
    unsigned winin = 0, winout = 0;
    unsigned blend_control = 0;    // BLDCNT
    unsigned blend_alpha = 0;      // BLDALPHA
    unsigned blend_brightness = 0; // BLDY
    unsigned mosaic = 0;           // MOSAIC
};

// Everything the game asked for, for one frame.
struct FieldScene {
    static constexpr int kRows = 160;
    static constexpr int kObjects = 128;
    static constexpr std::size_t kLineIoBytes = 0x58;

    bool     valid = false;
    std::uint64_t frame = 0;

    unsigned dispcnt = 0;
    int      video_mode = 0;
    bool     forced_blank = false;
    bool     one_dimensional_objects = false;

    std::array<SceneLayer, 4>  layers{};
    std::array<SceneObject, kObjects> objects{};
    SceneEffects effects{};

    // The 32 object transform groups, as PA, PB, PC, PD already scaled from
    // the hardware's 8.8 fixed point. A rotated or scaled object names one of
    // these; they are stored in the spare bytes of the object table, four
    // entries apart, which is why they are a separate array rather than part
    // of SceneObject.
    static constexpr int kTransforms = 32;
    std::array<std::array<float, 4>, kTransforms> object_transforms{};

    // Palettes, copied whole: 256 background entries then 256 object entries,
    // as 15-bit colours.
    std::array<std::uint16_t, 512> palette{};

    // Each row's own registers, for the rows where the game moved one
    // mid-frame. `row_io_valid[y]` says whether row y was captured. A row
    // that was NOT captured (the display was off, or the game never wrote a
    // per-row copy) falls back to `io_bytes` below -- the same end-of-frame
    // register file the frame-level fields (`layers`, `effects`, `dispcnt`)
    // were decoded from, so the fallback reproduces exactly what those
    // fields already say.
    std::array<std::array<std::uint8_t, kLineIoBytes>, kRows> row_io{};
    std::array<bool, kRows> row_io_valid{};
    std::array<std::uint8_t, kLineIoBytes> io_bytes{};
    // Four int32 per row: BG2 x/y then BG3 x/y, as latched for that row.
    std::array<std::array<std::int32_t, 4>, kRows> row_affine{};
    std::array<bool, kRows> row_affine_valid{};
    // True when no row differs from row 0 in any field a draw call depends on.
    // Golden Sun's battle frames measure this way (FACTS.md, 2026-09-12).
    // The renderer no longer needs this to decide whether it can draw a
    // frame at all -- window and scroll registers are read per row from
    // `row_io`/`io_bytes` regardless -- but it stays as a description of
    // the frame for diagnostics.
    bool rows_uniform = true;

    // Which room this frame belongs to, so a renderer can tell whether the
    // room it already uploaded is still the right one. Zero means the room is
    // not known, which is not an error -- it means fall back.
    std::uint32_t room_key = 0;
    int      room_width = 0, room_height = 0;
    int      camera_x = 0, camera_y = 0;   // room pixels, integer part

    // False whenever this frame uses something the scene model does not yet
    // describe. The caller must then use the emulated hardware path for the
    // WHOLE frame; there is no correct way to draw part of it (FACTS.md).
    bool supported = false;
    // Why not, for diagnostics. Never shown to the player.
    const char* unsupported_reason = nullptr;
};

// Captures the frame that has just finished rendering. `io`, `oam` and `pal`
// are the emulated register file, object table and palette memory; the per-row
// tables come from the PPU's own latched copies.
//
// Returns false, with `out->supported` false and a reason set, when the frame
// is not one the scene model describes.
bool field_scene_capture(FieldScene* out,
                         std::uint64_t frame,
                         const std::uint8_t* io,
                         const std::uint8_t* oam,
                         const std::uint8_t* pal,
                         const std::uint8_t* row_io_table,
                         const bool* row_io_valid,
                         const std::int32_t* row_affine_table,
                         const bool* row_affine_valid);

// True when the scene model can draw this frame. Kept separate so the runner
// can ask before paying for a capture.
bool field_scene_frame_supported(unsigned dispcnt, const char** why);

// Writes the description back out as hardware state: register file, object
// table and palette memory.
//
// This exists to TEST the description, not to run the game. If the capture
// drops any field the renderer reads, the rebuilt state differs from the
// original and the rendered picture changes -- which is the gate for step 2 of
// docs/NATIVE_SCENE_RENDERER_PLAN.md. Video memory is not rebuilt: tile art is
// deliberately not part of the description (see the header comment), so the
// caller passes the original through.
//
// `io` must already hold the frame's register file; only the fields the
// description owns are overwritten, so unrelated hardware registers (timers,
// DMA, sound) keep their values.
void field_scene_write_back(const FieldScene& scene,
                            std::uint8_t* io,
                            std::uint8_t* oam,
                            std::uint8_t* pal);

}  // namespace gsr
