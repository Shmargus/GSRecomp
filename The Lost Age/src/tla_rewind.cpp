// Golden Sun: The Lost Age -- F12 saves the last 2 seconds (always on).
//
// F12 saves the shared frame history under logs/tla_rewind_NNNN/. It holds
// presented frames, renderer snapshots, scanline registers and memory at
// the key press, so the recorded input can replay that moment. Show
// "F12 SAVED" for 3 seconds.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <new>

#include "frame_history.h"
#include "gpu_present.h"
#include "host_overlay.h"
#include "runtime_bus_bridge.h"
#include "tla.h"
#include "../../shared_game/battle_speed_level.h"
#include "../../shared_game/unpacker_guard.h"

namespace {

gbarecomp::FrameHistory g_history("tla_rewind_");
// Phones: what the runner paints (the battle HP bar and ">>", the Quality of
// Life menu, the Golden Sun save import, the Custom Battle panel, the radial
// menu, the new-game questions and the notice) over the card's picture when
// the frame is shown straight from the card (none of them reads the picture).
gbarecomp::GpuOverlayLayer g_overlay_layer;

bool present(uint8_t* rgb, uint32_t width, uint32_t height) {
    // A backwards guest clock (a state load) starts a new history in
    // FrameHistory::record; never reuse a pre-load picture for the spell
    // setup hold below.
    const bool have_last = g_history.count() &&
        runtime_current_frame() >= g_history.back(0).guest_frame;
    auto frame = gbarecomp::FrameHistory::capture();
    frame.battle_2x = tla_battle_speed_active();
    // A torn spell picture, and Retreat's native map running unseen, show
    // the last picture again: drawn again on the card from its snapshot, or
    // copied from the kept picture when the card did not draw it.
    const bool same_size = have_last && g_history.back(0).width == width &&
        g_history.back(0).height == height;
    const bool last_on_card = same_size && g_history.back(0).gpu_drawn &&
        g_history.back(0).scene;
    const bool held_frame = same_size &&
        (last_on_card || !g_history.back(0).rgb.empty()) && tla_battle_spell_setup_frame();
    const bool retreat_hold = tla_retreat_hold_frame();
    const bool held_on_card = (held_frame || retreat_hold) && last_on_card;
    if (held_on_card) {
        frame.scene = g_history.back(0).scene;
        frame.scene_frame = g_history.back(0).scene_frame;
    } else {
        try {
            frame.scene = tla_gpu_field_snapshot(width, height, &frame.scene_frame);
        } catch (const std::bad_alloc&) {
            static bool reported = false;
            if (!reported) {
                reported = true;
                std::fprintf(stderr, "[tla] F12: renderer snapshot allocation failed; keeping images only\n");
            }
        }
    }
    // When the picture is copied back, held pictures and the menus wait for
    // the card: the queued copy would show the menus a frame late, and would
    // alternate a held picture with the one queued before it.
    const bool synchronous = (held_frame || retreat_hold ||
                              tla_settings_page_overlay_visible() ||
                              tla_gs_save_import_visible()) &&
        !gbarecomp::gpu_present_accepting();
    // Only painters that read the picture need it copied back from the card.
    // Everything else goes to the overlay layer (g_overlay_layer), so a phone
    // can show the card's picture directly (gpu_present.h). Copying back made
    // the game wait on the GPU for over half its time on an Adreno 619 tablet
    // (TLA FACTS "Tablet session profile"). The card turns red save files
    // red itself (FieldSceneRenderer::set_window_red).
    const bool cpu_paint = (held_frame || retreat_hold) && !held_on_card;
    bool drawn = tla_gpu_field_present(rgb, width, height, frame, synchronous, !cpu_paint,
                                       held_on_card ? g_history.back(0).scene.get() : nullptr);
    const bool direct = gbarecomp::gpu_present_current() != nullptr;
    if (held_on_card && drawn && frame.gpu_drawn) {
        frame.held_frame = true;
    } else if (held_frame && !g_history.back(0).rgb.empty()) {
        const auto& last = g_history.back(0);
        std::copy(last.rgb.begin(), last.rgb.end(), rgb);
        frame = last;
        frame.rgb.clear();
        frame.held_frame = true;
        drawn = true;
    }
    const bool card_drawn = frame.gpu_drawn;  // red save files done on the card
    // The indicator belongs to the delivered picture, not the live guest.
    std::uint8_t* const layer = direct ? g_overlay_layer.begin(width, height) : rgb;
    if (frame.battle_2x) {
        tla_paint_text(layer, width, height, 4, 4,
                       gsr::battle_speed_level::indicator(), 5);
        drawn = true;
    }
    drawn = tla_settings_page_paint(layer, width, height) || drawn;
    drawn = tla_gs_save_import_paint(layer, width, height) || drawn;
    drawn = tla_enemy_hp_bar_paint(layer, width, height) || drawn;
    // The Custom Battle panel shades only inside its own window, which it
    // paints first, so it reads the layer's pixels, never the picture's.
    drawn = tla_custom_battle_paint(layer, width, height) || drawn;
    drawn = tla_retreat_travel_paint(layer, direct || card_drawn ? nullptr : rgb,
                                     width, height) || drawn;
    drawn = tla_radial_psynergy_paint(layer, width, height) || drawn;
    drawn = tla_save_files_paint(layer, card_drawn ? nullptr : rgb, width, height) || drawn;
    drawn = tla_new_game_paint(layer, width, height) || drawn;
    g_history.record(direct ? nullptr : rgb, width, height, std::move(frame));

    std::string unpacker_note;
    const auto notice = g_history.update_report(
        gsr::unpacker_guard_capture_due(&unpacker_note) ? &unpacker_note : nullptr);
    // After recording, so the notice is not in the saved frames.
    if (notice != gbarecomp::FrameHistory::ReportNotice::None) {
        tla_paint_text(layer, width, height, static_cast<int>(width) / 2 - 9 * 6 / 2,
                       4, "F12 SAVED");
        drawn = true;
    }
    if (direct) g_overlay_layer.finish();
    return drawn;
}

}  // namespace

// The engine's 5x7 letters (host_overlay.h), white with a dark shadow, one
// cell every `advance` pixels.
void tla_paint_text(uint8_t* rgb, uint32_t width, uint32_t height, int x0,
                    int y0, const char* text, int advance) {
    for (int pass = 0; pass < 2; ++pass) {  // shadow first, then letter
        const int d = pass == 0 ? 1 : 0;
        const uint8_t v = pass == 0 ? 0x28 : 0xF8;
        for (int i = 0; text[i]; ++i) {
            const uint8_t* rows = gbarecomp::overlay_glyph(text[i]);
            for (int y = 0; y < 7; ++y)
                for (int x = 0; x < 5; ++x) {
                    if (!(rows[y] & (0x80 >> x))) continue;
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
