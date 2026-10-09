// Earth Surge: a Psynergy-style cast, a jump attack with a white trail, and an
// earth explosion with a screen shake; see earth_surge.h.
//
// The cast plays through 0x080B9EC0 (non-summon mode) because src/mod_loader.cpp
// puts kind 9 in the move kind table; that presenter reads effect word 0x4001
// and starts Isaac's weapon swing after its prelude. This file only reacts.
//
// Arming: the resolver 0x080BE378 (called at 0x080B9BE6) keeps the action
// record at [0x03001E74] + 0x654, writes the move id (+0x4C) and the kind
// (+0x54, 0x080BF16C, from the kind table via 0x080BF11E..0x080BF136 or the
// other rules), then calls veneer 0x080772B8 at 0x080BF170 (bl pair, LR =
// 0x080BF175), whose ldr r4 / bx r4 lands on 0x08079EF8. Both functions have a
// generated function-entry hook and R14 is untouched by the veneer, so the
// hook sees LR 0x080BF175 at 0x08079EF8. Every path (table or not) joins at
// 0x080BF16C before that call. Nothing here depends on which presenter plays
// the move: the slash hiding and the burst are driven by the effect stamps
// alone.
//
// Threading: earth_surge_on_entry runs on the emulation thread and only reads
// guest memory and writes the atomics below. earth_surge_on_present runs on
// the thread that presents and draws (the same thread in a normal windowed
// run, inside gpu_field_present_override); the state it keeps in function-local
// statics is touched by that thread only. The two sides share nothing but the
// atomics g_armed / g_arm_frame / g_arm_generation / g_battler.
#include "earth_surge.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "effect_burst.h"
#include "field_scene_renderer.h"
#include "gba_bus.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"

extern "C" unsigned long long runtime_current_frame();

namespace gsr {
namespace {

constexpr std::uint32_t kEntryPc = 0x08079EF8u;
constexpr std::uint32_t kReturnPc = 0x080BF174u;  // LR & ~1 from the bl at 0x080BF170
constexpr std::uint32_t kStatePointerIwram = 0x1E74u;  // offset in IWRAM
constexpr std::uint32_t kRecordOffset = 0x654u;
constexpr std::uint32_t kBattlerOffset = 0x00u;   // attacker's battler id
constexpr std::uint32_t kMoveIdOffset = 0x4Cu;
constexpr std::uint32_t kKindOffset = 0x54u;
constexpr std::uint32_t kRecordEnd = 0x58u;  // one past the kind word
constexpr std::uint32_t kEwramBase = 0x02000000u;
constexpr std::uint32_t kEwramSize = 0x40000u;
constexpr std::uint32_t kEarthSurgeId = 18;
constexpr unsigned long long kGiveUpFrames = 300;
// The blow lands at the START of the slash: the burst spawns on the stamp
// frame this many frames after the first one (0 = the first stamp frame). If
// the slash ends sooner, it spawns on the first frame without stamps.
constexpr int kBurstDelayFromSlashStart = 2;

// Isaac's sprite is the OAM entry whose tile number (attr2 & 0x3FF) is
// 0x248 - 0x80 * battler id for the party battlers 0..3 (seen in play: Isaac
// 0x248, Garet 0x1C8, Ivan 0x148, Mia 0x0C8). The trail starts on the first
// frame its centre moves more than kTrailStartMove pixels from the previous
// frame and continues until the burst spawns.
constexpr std::uint32_t kSpriteTileBase = 0x248u;
constexpr std::uint32_t kSpriteTileStep = 0x80u;
constexpr std::uint32_t kPartyBattlers = 4;
constexpr float kTrailStartMove = 6.0f;  // px; the jump moves ~9-27 per frame

bool enabled() {
    static const bool on = [] {
        const char* project = std::getenv("GSR_STUDIO_MOD");
        if (project && project[0]) return false;
        const char* v = std::getenv("GSR_MOD_FIELD_TEST");
        return v && v[0] && !(v[0] == '0' && v[1] == '\0');
    }();
    return on;
}

std::uint32_t read_u32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

std::atomic<bool> g_armed{false};
std::atomic<std::uint64_t> g_arm_frame{0};
std::atomic<std::uint32_t> g_arm_generation{0};
std::atomic<std::uint32_t> g_battler{0xFFu};  // attacker's battler id at arming

}  // namespace

void earth_surge_on_entry(std::uint32_t entry_pc) {
    if (entry_pc != kEntryPc) return;
    if ((g_cpu.R[14] & ~1u) != kReturnPc) return;
    if (!enabled()) return;
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus || !bus->iwram_ptr() || !bus->ewram_ptr()) return;
    const std::uint32_t state = read_u32(bus->iwram_ptr() + kStatePointerIwram);
    const std::uint32_t action = state + kRecordOffset;
    if (state < kEwramBase || action < kEwramBase ||
        action - kEwramBase + kRecordEnd > kEwramSize)
        return;
    const std::uint8_t* rec = bus->ewram_ptr() + (action - kEwramBase);
    if (read_u32(rec + kMoveIdOffset) != kEarthSurgeId) return;

    const std::uint32_t battler = rec[kBattlerOffset];
    std::fprintf(stderr,
                 "[earth-surge] armed: kind %u (expect 9), battler %u (action "
                 "0x%08X)\n",
                 read_u32(rec + kKindOffset), battler, action);
    g_battler = battler;
    g_arm_frame = runtime_current_frame();
    ++g_arm_generation;
    g_armed = true;
}

// The slash is the stamps on the TEXT-layer canvas (the case
// effect_canvas_to_view can map). Stamps on an affine layer (the summon
// prelude / caster stance) are ignored entirely: they neither start the slash
// window nor end it, and are never hidden.
//
// Hides the game's own slash on the frames it is on screen, from the moment
// Earth Surge arms until the slash ends (the first frame without stamps after
// slash stamps were seen) or the give-up timeout. Present thread only; the
// renderer flag is set here and read by FieldSceneRenderer::draw on the same
// thread. The toggle off never arms, so this never touches the renderer then.
static void update_hidden_slash(FieldSceneRenderer& renderer, std::uint64_t frame,
                                int effect_layer, bool slash_now,
                                bool ignored_stamps) {
    static std::uint32_t hide_generation = 0;
    static bool hiding = false;
    static bool saw_stamps = false;
    static bool said = false;
    static std::uint64_t started = 0;

    const std::uint32_t generation = g_arm_generation.load();
    if (generation != hide_generation) {  // a new cast armed
        hide_generation = generation;
        hiding = true;
        saw_stamps = false;
        said = false;
        started = g_arm_frame.load();
    }
    if (!hiding) return;

    if (slash_now) saw_stamps = true;
    const bool timed_out = frame > started && frame - started > kGiveUpFrames;
    if (timed_out || (saw_stamps && !slash_now && !ignored_stamps)) {
        hiding = false;
        renderer.set_effect_hidden(false);
        std::fprintf(stderr,
                     "[earth-surge] game slash shown again (frame %llu%s)\n",
                     static_cast<unsigned long long>(frame),
                     timed_out ? ", timed out" : "");
        return;
    }
    if (!said && slash_now) {
        said = true;
        std::fprintf(stderr, "[earth-surge] hiding game slash (layer %d)\n",
                     effect_layer);
    }
    renderer.set_effect_hidden(slash_now);
}

// Finds the attacker's OAM entry by tile number and returns its centre in
// console pixels. Size and double-size come from attr0/attr1 (GBATEK):
//   shape (attr0 bits 14-15) x size (attr1 bits 14-15) -> width x height,
//   attr0 bit 8 = affine, bit 9 = double-size if affine, else disabled.
// x is 9-bit signed, y is 8-bit (wraps past 160). Regular objects are drawn
// in the top-left of their box, affine ones centred in it (twice as big when
// double-size); either way the centre of the box is the sprite's centre.
static bool find_sprite_centre(const std::uint8_t* oam, std::size_t oam_bytes,
                               std::uint32_t tile, float* cx, float* cy) {
    static const int kDim[3][4][2] = {
        {{8, 8}, {16, 16}, {32, 32}, {64, 64}},
        {{16, 8}, {32, 8}, {32, 16}, {64, 32}},
        {{8, 16}, {8, 32}, {16, 32}, {32, 64}},
    };
    if (!oam || oam_bytes < 1024) return false;
    for (int i = 0; i < 128; ++i) {
        const std::uint8_t* e = oam + i * 8;
        const unsigned a0 = e[0] | (e[1] << 8);
        const unsigned a1 = e[2] | (e[3] << 8);
        const unsigned a2 = e[4] | (e[5] << 8);
        if ((a2 & 0x3FFu) != tile) continue;
        const bool affine = (a0 & 0x100u) != 0;
        const bool bit9 = (a0 & 0x200u) != 0;
        if (!affine && bit9) continue;  // disabled
        const unsigned shape = (a0 >> 14) & 3u;
        if (shape == 3) continue;
        const int w = kDim[shape][(a1 >> 14) & 3u][0] * (affine && bit9 ? 2 : 1);
        const int h = kDim[shape][(a1 >> 14) & 3u][1] * (affine && bit9 ? 2 : 1);
        int x = static_cast<int>(a1 & 0x1FFu);
        if (x >= 256) x -= 512;
        int y = static_cast<int>(a0 & 0xFFu);
        if (y >= 160) y -= 256;
        *cx = static_cast<float>(x) + static_cast<float>(w) * 0.5f;
        *cy = static_cast<float>(y) + static_cast<float>(h) * 0.5f;
        return true;
    }
    return false;
}

// The white trail: while armed, once the attacker's sprite starts moving
// (more than kTrailStartMove px between frames), drop glows along its path
// every frame until the burst spawns. Present thread only.
static void update_trail(FieldSceneRenderer& renderer, std::uint64_t frame,
                         const std::uint8_t* oam, std::size_t oam_bytes) {
    static std::uint32_t seen_generation = 0;
    static bool have_prev = false;
    static float prev_x = 0, prev_y = 0;
    static bool trailing = false;
    static bool missing_said = false;

    const std::uint32_t generation = g_arm_generation.load();
    if (generation != seen_generation) {  // a new cast armed
        seen_generation = generation;
        have_prev = false;
        trailing = false;
        missing_said = false;
    }
    if (!g_armed.load()) return;

    const std::uint32_t battler = g_battler.load();
    float cx = 0, cy = 0;
    if (battler >= kPartyBattlers ||
        !find_sprite_centre(oam, oam_bytes,
                            kSpriteTileBase - kSpriteTileStep * battler, &cx,
                            &cy)) {
        if (!missing_said) {
            missing_said = true;
            std::fprintf(stderr,
                         "[earth-surge] no OAM entry for battler %u (frame "
                         "%llu); no trail yet\n",
                         battler, static_cast<unsigned long long>(frame));
        }
        have_prev = false;
        return;
    }
    // Console -> view, as in FieldSceneRenderer::effect_canvas_to_view.
    const float cam_x = static_cast<float>((240 - renderer.output_width()) / 2);
    const float cam_y = static_cast<float>((160 - renderer.output_height()) / 2);
    const float vx = cx - cam_x;
    const float vy = cy - cam_y;
    if (have_prev) {
        const float moved = std::hypot(vx - prev_x, vy - prev_y);
        if (!trailing && moved > kTrailStartMove) {
            trailing = true;
            std::fprintf(stderr,
                         "[earth-surge] trail starts (frame %llu, view "
                         "%.1f,%.1f)\n",
                         static_cast<unsigned long long>(frame), vx, vy);
        }
        if (trailing) renderer.add_trail(prev_x, prev_y, vx, vy);
    }
    have_prev = true;
    prev_x = vx;
    prev_y = vy;
}

void earth_surge_on_present(FieldSceneRenderer& renderer, std::uint64_t frame,
                            int effect_layer,
                            const std::vector<EffectSpark>& stamps,
                            const std::uint8_t* oam, std::size_t oam_bytes) {
    if (g_arm_generation.load() == 0) return;  // never armed (toggle off)
    // Classify this frame's stamps: a slash frame maps onto a text-layer
    // canvas; other stamps (affine layer) are ignored.
    const bool stamps_any = effect_layer >= 0 && !stamps.empty();
    float vx = 0, vy = 0;
    const bool slash_now =
        stamps_any && renderer.effect_canvas_to_view(stamps.back().x,
                                                     stamps.back().y, &vx, &vy);
    const bool ignored_stamps = stamps_any && !slash_now;
    update_hidden_slash(renderer, frame, effect_layer, slash_now, ignored_stamps);
    update_trail(renderer, frame, oam, oam_bytes);
    if (!g_armed.load()) return;

    // Present-thread state.
    static std::uint32_t seen_generation = ~0u;
    static bool have_stamp = false;
    static float view_x = 0, view_y = 0;
    static int canvas_x = 0, canvas_y = 0;
    static int stamp_frames = 0;
    static bool ignored_said = false;
    static int layer_index = -1;

    const std::uint32_t generation = g_arm_generation.load();
    if (generation != seen_generation) {
        seen_generation = generation;
        have_stamp = false;
        stamp_frames = 0;
    }
    const std::uint64_t armed_at = g_arm_frame.load();
    if (frame > armed_at && frame - armed_at > kGiveUpFrames) {
        g_armed = false;
        return;
    }

    if (slash_now) {
        const EffectSpark& last = stamps.back();
        have_stamp = true;
        view_x = vx;
        view_y = vy;
        canvas_x = last.x;
        canvas_y = last.y;
        layer_index = effect_layer;
        if (stamp_frames++ < kBurstDelayFromSlashStart) return;
    } else if (ignored_stamps) {
        if (!ignored_said) {
            ignored_said = true;
            std::fprintf(stderr,
                         "[earth-surge] ignoring stamps on effect layer %d "
                         "(not a text-layer canvas)\n",
                         effect_layer);
        }
        return;
    } else if (!have_stamp) {
        return;
    }
    // Reached on the delayed stamp frame, or on the first frame without
    // stamps if the slash ended before the delay.
    renderer.spawn_burst(view_x, view_y + kBurstOffsetY);
    std::fprintf(stderr,
                 "[earth-surge] burst at view %.1f,%.1f (canvas %d,%d, frame "
                 "%llu, effect layer %d, text-layer canvas yes)\n",
                 view_x, view_y, canvas_x, canvas_y,
                 static_cast<unsigned long long>(frame), layer_index);
    have_stamp = false;
    g_armed = false;
}

}  // namespace gsr
