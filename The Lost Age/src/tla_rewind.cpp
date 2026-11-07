// Golden Sun: The Lost Age -- F12 saves the last 2 seconds (always on).
//
// The ring and the folder it writes are the engine's shared F12 history
// (frame_history.h, logs/tla_rewind_NNNN/): each presented picture, its
// renderer snapshot (tla_gpu_field_snapshot), the rows' registers and the
// memory at the press, so the session's input recording can replay the
// moment on the bench. "F12 SAVED" shows for 3 seconds afterwards.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <new>

#include "frame_history.h"
#include "runtime_bus_bridge.h"
#include "tla.h"
#include "../../shared_game/unpacker_guard.h"

namespace {

gbarecomp::FrameHistory g_history("tla_rewind_");

bool present(uint8_t* rgb, uint32_t width, uint32_t height) {
    // A backwards guest clock (a state load) starts a new history in
    // FrameHistory::record; never reuse a pre-load picture for the spell
    // setup hold below.
    const bool have_last = g_history.count() &&
        runtime_current_frame() >= g_history.back(0).guest_frame;
    auto frame = gbarecomp::FrameHistory::capture();
    frame.battle_2x = tla_battle_speed_active();
    try {
        frame.scene = tla_gpu_field_snapshot(width, height, &frame.scene_frame);
    } catch (const std::bad_alloc&) {
        static bool reported = false;
        if (!reported) {
            reported = true;
            std::fprintf(stderr, "[tla] F12: renderer snapshot allocation failed; keeping images only\n");
        }
    }
    const bool held_frame = have_last && g_history.back(0).width == width &&
        g_history.back(0).height == height && tla_battle_spell_setup_frame();
    const bool synchronous = held_frame || tla_settings_page_overlay_visible() ||
        tla_gs_save_import_visible();
    bool drawn = tla_gpu_field_present(rgb, width, height, frame, synchronous);
    if (held_frame) {
        const auto& last = g_history.back(0);
        std::copy(last.rgb.begin(), last.rgb.end(), rgb);
        frame = last;
        frame.rgb.clear();
        frame.held_frame = true;
        drawn = true;
    }
    // The indicator belongs to the delivered picture, not the live guest.
    if (frame.battle_2x) {
        tla_paint_text(rgb, width, height, 4, 4, ">>", 5);
        drawn = true;
    }
    drawn = tla_settings_page_paint(rgb, width, height) || drawn;
    drawn = tla_gs_save_import_paint(rgb, width, height) || drawn;
    drawn = tla_enemy_hp_bar_paint(rgb, width, height) || drawn;
    drawn = tla_custom_battle_paint(rgb, width, height) || drawn;
    drawn = tla_retreat_travel_paint(rgb, width, height) || drawn;
    drawn = tla_radial_psynergy_paint(rgb, width, height) || drawn;
    drawn = tla_save_files_paint(rgb, width, height) || drawn;
    drawn = tla_new_game_paint(rgb, width, height) || drawn;
    g_history.record(rgb, width, height, std::move(frame));

    std::string unpacker_note;
    const auto notice = g_history.update_report(
        gsr::unpacker_guard_capture_due(&unpacker_note) ? &unpacker_note : nullptr);
    // After recording, so the notice is not in the saved frames.
    if (notice != gbarecomp::FrameHistory::ReportNotice::None) {
        tla_paint_text(rgb, width, height, static_cast<int>(width) / 2 - 9 * 6 / 2,
                       4, "F12 SAVED");
        drawn = true;
    }
    return drawn;
}

}  // namespace

// The runner's own 5x7 letters, white with a dark shadow, one cell every
// `advance` pixels.
void tla_paint_text(uint8_t* rgb, uint32_t width, uint32_t height, int x0,
                    int y0, const char* text, int advance) {
    static const struct { char c; uint8_t rows[7]; } kGlyphs[] = {
        {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
        {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
        {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
        {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
        {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
        {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
        {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
        {'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}},
        {'>', {0x10, 0x18, 0x0C, 0x06, 0x0C, 0x18, 0x10}},
    };
    for (int pass = 0; pass < 2; ++pass) {  // shadow first, then letter
        const int d = pass == 0 ? 1 : 0;
        const uint8_t v = pass == 0 ? 0x28 : 0xF8;
        for (int i = 0; text[i]; ++i) {
            const uint8_t* rows = nullptr;
            for (const auto& g : kGlyphs)
                if (g.c == text[i]) rows = g.rows;
            if (!rows) continue;
            for (int y = 0; y < 7; ++y)
                for (int x = 0; x < 5; ++x) {
                    if (!(rows[y] & (0x10 >> x))) continue;
                    const int px = x0 + i * advance + x + d, py = y0 + y + d;
                    if (px < 0 || py < 0 || px >= static_cast<int>(width) ||
                        py >= static_cast<int>(height))
                        continue;
                    uint8_t* p = rgb + (static_cast<size_t>(py) * width + px) * 3u;
                    p[0] = p[1] = p[2] = v;
                }
        }
    }
}

void tla_rewind_reset() {
    g_history.clear();
}

void tla_install_rewind() {
    gbarecomp::set_frame_present_override_hook(present);
}
