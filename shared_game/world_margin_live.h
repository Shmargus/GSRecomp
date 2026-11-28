// The world map's live margins, both games: the margins draw the
// settled world map again EVERY frame, with this frame's sprites, sprite art
// and sprite colours, instead of one held photo that froze everything out
// there during menus, dialogue and Psynergy (in The Broken Seal, sparkles
// stopped at the old top edge, and so did the caravan dialogue). The photo
// stays the fallback. Each game passes its own capture type (The Broken
// Seal's GpuFieldCapture, The Lost Age's Capture); both carry these fields.
#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <new>
#include <vector>

#include "field_scene.h"
#include "frame_history.h"
#include "held_palette_effect.h"

namespace gsr {

// A screen fade under way: a BLDCNT brightness effect with a non-zero BLDY.
// A world-map frame taken mid-fade would hold a darkened copy.
inline bool world_map_fading(const std::vector<std::uint8_t>& io) {
    if (io.size() < 0x56u) return false;
    const unsigned bldcnt = io[0x50] | (static_cast<unsigned>(io[0x51]) << 8);
    return ((bldcnt >> 6) & 3u) >= 2u && (io[0x54] & 0x1Fu) != 0u;
}

// Rows in the world map's mode 2 and rows in another mode in one frame: a
// shutter, a battle-flash band, or a transition (nothing to hold).
inline bool world_rows_mixed(const FieldScene& scene) {
    bool has_m2 = false, has_other = false;
    for (int y = 0; y < FieldScene::kRows; ++y) {
        if (!scene.row_io_valid[y]) continue;
        ((scene.row_io[y][0] & 7u) == 2u ? has_m2 : has_other) = true;
    }
    return has_m2 && has_other;
}

// `mix` becomes the settled capture with this frame's sprite state, and the
// same guest memory the main draw just used, so state read from it sees one
// input per frame. False (nothing to draw) when either capture is short, or
// on an allocation failure.
template <class Capture>
bool live_world_margin_capture(const Capture& settled, const Capture& live,
                               Capture& mix, bool tint_background) {
    if (!settled.valid || live.oam.size() < 1024u || live.pal.size() < 1024u ||
        live.vram.size() < 0x18000u || settled.vram.size() < 0x18000u)
        return false;
    try {
        mix = settled;
        mix.effects = {};
        mix.ewram = live.ewram;
        mix.iwram = live.iwram;
        mix.scene.objects = live.scene.objects;
        // A game window's own sprites stay out of the margins, as the main
        // draw keeps them in the console's window (FieldSceneRenderer's
        // menu_sprite rule: window open, priority 0). The settled capture is
        // mode 2 with no window, so that rule cannot see them here: The Lost Age's
        // Djinn screen over the world map parks three priority-0 Djinn at
        // y 170 (read as -86), drawn in the top margin.
        if (live.window_open)
            for (auto& o : mix.scene.objects)
                if (o.priority == 0) o.present = false;
        mix.scene.object_transforms = live.scene.object_transforms;
        mix.scene.one_dimensional_objects = live.scene.one_dimensional_objects;
        // A spell can tint the map's palette. Apply that before compositing
        // with the live sprites, whose colours already match the native view.
        const auto first_colour = tint_background ? 0 : 256;
        std::copy(live.scene.palette.begin() + first_colour, live.scene.palette.end(),
                  mix.scene.palette.begin() + first_colour);
        mix.oam = live.oam;
        if (mix.pal.size() >= 1024u)
            std::copy(live.pal.begin() + first_colour * 2, live.pal.begin() + 0x400,
                      mix.pal.begin() + first_colour * 2);
        // Sprite art: the console's, plus the extra sprite page past it when
        // the capture carries one (GbaBus::kObjExtraVram). Retreat's particles
        // on The Lost Age's world map draw from that page (tile 0x4A0..);
        // the settled frame's stale copy garbled them.
        mix.vram.resize(live.vram.size());
        std::copy(live.vram.begin() + 0x10000, live.vram.end(), mix.vram.begin() + 0x10000);
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

// The settled world-map view the margins hold while a menu, dialogue or
// Psynergy covers the map, and the palette it was taken with. Each game
// decides which frames count as settled; this owns the rest.
template <class Capture>
class WorldMarginHold {
public:
    // A settled world-map frame to hold from now on.
    void remember(const std::shared_ptr<const Capture>& settled) {
        settled_ = settled;
        palette_.save(settled->pal);
        cache_valid_ = false;
    }
    // Off the world map, or a savestate load: the held view is stale.
    void forget() {
        settled_ = {};
        palette_ = {};
        cache_valid_ = false;
    }
    // Whether a spell has recoloured the map's own palette since it settled.
    bool changes_background(const std::vector<std::uint8_t>& pal) const {
        return palette_.changes_background(pal);
    }
    // Draws the margin picture for a world-map frame under a menu: the held
    // map with this frame's sprites, else the held map alone. `draw(capture)`
    // draws one capture as the margin picture. When `target_ready` is true and
    // nothing visible in the margin picture changed since the last draw (e.g.
    // a paused menu over the world map), reuses the already-rendered target
    // without redrawing or re-uploading VRAM. False: nothing held, or neither
    // drew.
    template <class Draw>
    bool draw_margin(const Capture& live, bool tint_background, Draw&& draw,
                     bool target_ready = false) {
        if (!settled_ || !settled_->valid) return false;
        if (target_ready && cache_valid_ && cached_settled_ == settled_.get() &&
            cached_tint_ == tint_background && margin_inputs_match(live, tint_background)) {
            gbarecomp::frame_trace_note_held_scene(live.scene.frame,
                                                   settled_->scene.frame);
            return true;
        }
        static Capture mix;
        const bool mixed_ok =
            live_world_margin_capture(*settled_, live, mix, tint_background);
        const bool drawn =
            (mixed_ok && draw(static_cast<const Capture&>(mix))) ||
            draw(*settled_);
        if (drawn) {
            if (mixed_ok)
                save_cache(live, tint_background);
            else
                cache_valid_ = false;
            gbarecomp::frame_trace_note_held_scene(live.scene.frame,
                                                   settled_->scene.frame);
        } else {
            cache_valid_ = false;
        }
        return drawn;
    }

private:
    static bool same_object(const SceneObject& a, const SceneObject& b) {
        if (a.present != b.present) return false;
        if (!a.present) return true;
        return a.x == b.x && a.y == b.y && a.width == b.width &&
               a.height == b.height && a.tile == b.tile &&
               a.palette == b.palette && a.priority == b.priority &&
               a.hflip == b.hflip && a.vflip == b.vflip &&
               a.affine == b.affine && a.double_size == b.double_size &&
               a.affine_index == b.affine_index && a.blended == b.blended &&
               a.window == b.window && a.mosaic == b.mosaic &&
               a.parked == b.parked;
    }

    // Collects the filtered non-window sprites, used palette banks, and exact
    // OBJ VRAM tile bytes they reference so priority-0 menu cursor animations
    // do not force a full world-map redraw.
    static void collect_obj_fingerprint(
        const Capture& live,
        std::array<SceneObject, FieldScene::kObjects>& objects_out,
        std::vector<std::uint8_t>& pal_bytes,
        std::vector<std::uint8_t>& tile_bytes,
        bool& any_affine) {
        objects_out = live.scene.objects;
        if (live.window_open)
            for (auto& o : objects_out)
                if (o.priority == 0) o.present = false;
        pal_bytes.clear();
        tile_bytes.clear();
        any_affine = false;
        std::uint32_t pal_banks = 0;
        bool any_8bit = false;
        const bool one_d = live.scene.one_dimensional_objects;
        const std::size_t vram_size = live.vram.size();
        for (const SceneObject& o : objects_out) {
            if (!o.present) continue;
            if (o.affine) any_affine = true;
            if (!o.window) {
                if (o.palette < 0) any_8bit = true;
                else if (o.palette < 16) pal_banks |= (1u << o.palette);
            }
            if (vram_size <= 0x10000u || o.width <= 0 || o.height <= 0) continue;
            const bool eight_bit = o.palette < 0;
            const int step = eight_bit ? 2 : 1;
            const int bytes_per_tile = eight_bit ? 64 : 32;
            const int tw = o.width >> 3;
            const int th = o.height >> 3;
            const int page = static_cast<int>(o.tile & 0x400u);
            for (int ty = 0; ty < th; ++ty) {
                for (int tx = 0; tx < tw; ++tx) {
                    int t = one_d
                        ? static_cast<int>(o.tile) + (ty * tw + tx) * step
                        : static_cast<int>(o.tile) + ty * 32 + tx * step;
                    t = page | (t & 0x3FF);
                    const std::size_t off = 0x10000u + static_cast<std::size_t>(t) * 32u;
                    if (off + static_cast<std::size_t>(bytes_per_tile) <= vram_size) {
                        tile_bytes.insert(tile_bytes.end(),
                                          live.vram.begin() + off,
                                          live.vram.begin() + off + bytes_per_tile);
                    }
                }
            }
        }
        if (live.pal.size() >= 1024u) {
            if (any_8bit) {
                pal_bytes.assign(live.pal.begin() + 512, live.pal.begin() + 1024);
            } else if (pal_banks != 0) {
                for (int b = 0; b < 16; ++b) {
                    if ((pal_banks & (1u << b)) == 0) continue;
                    const std::size_t off = 512u + static_cast<std::size_t>(b) * 32u;
                    pal_bytes.insert(pal_bytes.end(),
                                     live.pal.begin() + off,
                                     live.pal.begin() + off + 32u);
                }
            }
        }
    }

    bool margin_inputs_match(const Capture& live, bool tint_background) {
        if (live.oam.size() < 1024u || live.pal.size() < 1024u ||
            live.vram.size() < 0x18000u)
            return false;
        if (tint_background) {
            if (cached_bg_pal_.size() != 512u ||
                !std::equal(cached_bg_pal_.begin(), cached_bg_pal_.end(),
                            live.pal.begin()))
                return false;
        }
        if (live.scene.one_dimensional_objects != cached_1d_) return false;
        bool any_affine = false;
        collect_obj_fingerprint(live, scratch_objects_, scratch_obj_pal_,
                                scratch_obj_tiles_, any_affine);
        for (std::size_t i = 0; i < FieldScene::kObjects; ++i)
            if (!same_object(scratch_objects_[i], cached_objects_[i]))
                return false;
        if (any_affine && live.scene.object_transforms != cached_transforms_)
            return false;
        if (scratch_obj_pal_ != cached_obj_pal_) return false;
        if (scratch_obj_tiles_ != cached_obj_tiles_) return false;
        return true;
    }

    void save_cache(const Capture& live, bool tint_background) {
        cached_settled_ = settled_.get();
        cached_tint_ = tint_background;
        cached_1d_ = live.scene.one_dimensional_objects;
        if (tint_background && live.pal.size() >= 512u)
            cached_bg_pal_.assign(live.pal.begin(), live.pal.begin() + 512);
        else
            cached_bg_pal_.clear();
        bool any_affine = false;
        collect_obj_fingerprint(live, cached_objects_, cached_obj_pal_,
                                cached_obj_tiles_, any_affine);
        if (any_affine)
            cached_transforms_ = live.scene.object_transforms;
        cache_valid_ = true;
    }

    std::shared_ptr<const Capture> settled_;
    HeldPaletteEffect palette_;
    bool cache_valid_ = false;
    const Capture* cached_settled_ = nullptr;
    bool cached_tint_ = false;
    bool cached_1d_ = false;
    std::array<SceneObject, FieldScene::kObjects> cached_objects_{};
    std::array<std::array<float, 4>, FieldScene::kTransforms> cached_transforms_{};
    std::vector<std::uint8_t> cached_bg_pal_;
    std::vector<std::uint8_t> cached_obj_pal_;
    std::vector<std::uint8_t> cached_obj_tiles_;
    std::array<SceneObject, FieldScene::kObjects> scratch_objects_{};
    std::vector<std::uint8_t> scratch_obj_pal_;
    std::vector<std::uint8_t> scratch_obj_tiles_;
};

}  // namespace gsr
