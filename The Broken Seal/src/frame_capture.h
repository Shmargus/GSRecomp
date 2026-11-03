#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "effect_capture.h"
#include "field_scene.h"

namespace gsr {

struct GpuFieldCapture {
    bool valid = false;
    gsr::CapturedEffectFrame effects;
    gsr::FieldScene scene;
    std::vector<std::uint8_t> vram;   // bus->vram_ptr(), 96 KB
    // The same, except that while a game window is open BG0's character
    // block holds what it held before the window opened (see
    // build_room_vram). FieldSceneRenderer::upload_room_vram; room tiles
    // in the margins read it.
    std::vector<std::uint8_t> room_vram;
    // Likewise the palette: the field's background colours restored while a
    // window is open, else empty.
    std::vector<std::uint16_t> room_palette;
    // bus->ewram_ptr(), 256 KB: the room rect/camera/id-grid/atlas tables
    // FieldSceneRenderer::upload_room reads (addresses in room_buffer.h),
    // taken from the live guest, not the offline recorded snapshots the
    // check tool uses.
    std::vector<std::uint8_t> ewram;
    // bus->iwram_ptr(), 32 KB. Not read by the renderer at all -- this is for
    // the F12 dump alone, and it is here because the spell effects cannot be
    // read any other way. Golden Sun composes an effect in a 16 KB buffer on
    // its own IWRAM heap and RELOCATES the routine that stamps particles into
    // that buffer to just behind it, so the routine exists only at runtime:
    // its address is never a ROM literal and the disassembly has no symbol for
    // it (FACTS.md, 2026-09-18). A write trace can say where it wrote but
    // never what instructions it is. Capturing IWRAM captures the routine
    // itself, to be disassembled offline.
    std::vector<std::uint8_t> iwram;
    // The cartridge, for WorldMapSource: the world map's pieces are unpacked
    // from it (world_map_source.h). Constant for the session.
    const std::uint8_t* rom = nullptr;
    std::size_t rom_bytes = 0;
    // Raw register/object/palette memory, for gpu_field_present_override()'s
    // F12 dump only (see its header comment). `scene` already carries a
    // decoded summary of IO/OAM/PAL, but a one-frame dump hands the replay
    // tool the exact memory the hardware compositor reads, not a re-encoding
    // of it, so these are separate full copies rather than derived from
    // `scene` at dump time.
    std::vector<std::uint8_t> io;     // bus->io().raw(), 0x400 bytes
    std::vector<std::uint8_t> oam;    // bus->oam_ptr(), 1024 bytes
    std::vector<std::uint8_t> pal;    // bus->pal_ptr(), 1024 bytes
    // game_window_open() at this frame's capture (world-map margin hold).
    bool window_open = false;
};

std::string gpu_field_dump_bytes(const GpuFieldCapture& capture);
void paint_bug_report_notice(std::uint8_t* rgb, std::uint32_t width,
                             std::uint32_t height, const char* text);

}  // namespace gsr
