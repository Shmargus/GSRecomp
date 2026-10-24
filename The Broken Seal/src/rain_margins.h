#pragma once

#include <cstdint>
#include <vector>

#include "field_scene.h"
#include "object_artwork.h"

// The Vale storm's rain (Func_94820, F12 gpu_rewind_0050 and 0052).
//
// Each drop respawns at the object [[0x03001E70]]'s x and z plus
// (rand16 << 8) - 128 pixels (0x0809494C..0x0809497C; Func_4458 returns 16
// bits), so the drops fall uniformly over a 256x256-pixel square of ground.
// Repeating them 256 pixels in each direction tiles that square with the
// same density and phase. In the expanded view the square is centred on the
// view (golden_sun_rain_spawn_offset, runner_main.cpp); the game alone put
// its landings 128..383 rows below the camera top (F12 0053): a dry top. The
// drops below the screen are the game's own (the upper-Y override at
// 0x080948C8). A copy that would reach into
// the console's window is left out whole, so no streak is cut at its edge.
//
// Recognised by artwork, not tile slot: the streak (16x32) and its four
// splash frames (16x16) hash the same in 0050 (tiles 260, 268..280) and
// 0052 (tiles 324, 332..344). The capture's drop pool is a frame ahead of
// its OAM (0052: pool at frame N is OAM at N + 1), so the pool cannot name
// the slots.
inline bool golden_sun_rain_artwork(const gsr::FieldScene& scene,
                                    const gsr::SceneObject& object,
                                    const std::vector<std::uint8_t>& vram) {
    if (object.palette < 0 || object.width != 16 ||
        (object.height != 16 && object.height != 32)) return false;
    std::uint64_t hash = 0;
    if (!gsr::object_artwork_fingerprint_4bpp(scene, object, vram, &hash))
        return false;
    if (object.height == 32) return hash == UINT64_C(0x5a4c7ac23476b465);
    return hash == UINT64_C(0xa50f1d38ed5c48a7) ||
           hash == UINT64_C(0x12344c5e24f60145) ||
           hash == UINT64_C(0x6ae1d0a7e5342063) ||
           hash == UINT64_C(0x609b15688088d191);
}

// True when the scene has rain; `margins` then repeats it in every direction.
inline bool golden_sun_rain_margins(const gsr::FieldScene& scene,
                                    gsr::SceneMargins& margins,
                                    const std::vector<std::uint8_t>& vram) {
    margins.object_repeat = {};
    margins.object_repeat_whole = true;
    if (scene.video_mode != 0) return false;
    bool found = false;
    for (int slot = 0; slot < gsr::FieldScene::kObjects; ++slot) {
        const auto& o = scene.objects[slot];
        if (!o.present || o.parked || o.window || o.affine || !o.blended)
            continue;
        if (golden_sun_rain_artwork(scene, o, vram)) {
            margins.object_repeat[slot] = {256, 256};
            found = true;
        }
    }
    return found;
}
