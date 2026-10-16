// Golden Sun: The Lost Age -- the expanded view's game side: the camera
// keeps the wider view inside the room, and characters and objects are drawn
// in the margins (FACTS.md, 2026-10-05). Golden Sun 1's design, re-found on
// this ROM. Everything here is inert in the native 240x160 view.
//
// Camera. The field camera is stored at S+0xE4/+0xE8 (S = [0x03000020]) by
// the per-frame follow (0x0802AE9C/0x0802AE9E) and the room-entry clamp
// (0x0802B006/0x0802B010). Both keep the console's 240x160 inside
// [S+0xEC, S+0xF4 - 240] x [S+0xF0, S+0xF8 - 160]. Here each stored value is
// pulled in from the room's edges by up to the margin, so the margins show
// room rather than the black past its edge; a room narrower than the view
// gets half the spare width per side, which centres it. The title screen
// runs the field camera too, as in Golden Sun 1, and is left alone: its room
// loads as map number 1, but the number then follows the highlighted save
// (F12 0022: 2 with Felix's World Map save highlighted, and the inset moved
// the logo 60 left and 40 up). So the number is taken when the room loads,
// at the room clamp's store, and kept until the next room.
//
// Characters. 0x08023940..0x08023958 skips a character whose screen position
// is outside x -32..272, y -32..224 (four literals, 16.16); the builders
// 0x080220F0 and 0x08022318 then hide a body past x 239 or y 159 and a
// shadow past y 159 (`cmp #0xEF/#0x9F; bgt`). Both are widened to the view
// plus kSlack around it, so characters are already drawn when they scroll
// in.
//
// Rotated objects (the world map's towns, caves and lighthouses). The affine
// builder 0x0802254C projects the object to [sp+0x1C] (x) / [sp+0x20] (y) and
// skips it unless x + 0x20 <= 0x130 (`bls` 0x08022592) and y + 0x20 <= 0xF0
// (`bls` 0x0802259C): x -32..272, y -32..208. Both are widened like the
// characters' (measured from F6 state 5 walking left on the world map: the
// object went at x 255; FACTS.md, 2026-10-08). Its record gets attr0/attr1
// at +4 like the characters' builders, and the placement is recorded at
// 0x080226B4: x = [sl] - sb + r0, y = [sl+4] - fp + [sp].
//
// Before that the projection 0x08015778 hides anything beyond the far depth
// [0x030011E8] (1020; near [0x030011E4] 255), which on the console is about
// its top row, so objects in the top margin went early (F12 0064/0065, top
// right). On the world map (map 2, every world-map F12) in the expanded view
// the far test (`bgt` 0x080157A2) never hides; the widened y test above
// decides what fits the view instead.
//
// Real positions. The console's object Y is 8 bits and X 9: 180 and -76 are
// the same Y, and once objects are placed outside its screen any raw value
// can mean either. Each builder pass records the object's real X and Y by
// its record; the scene capture asks for them (gba::g_ws_obj_attr_x_provider
// and _y_provider) and gets them back from the record that holds that raw
// position, or placed it there in the last few frames.

#include <algorithm>
#include <array>
#include <cstdint>

#include "gba_bus.h"
#include "gba_ppu.h"
#include "obj_placements.h"
#include "runtime.h"
#include "view_config.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"
#include "tla.h"

namespace {

constexpr uint32_t kFieldStatePointer = 0x03000020u;
constexpr uint32_t kMapNumber = 0x02000428u;

constexpr uint32_t kFollowStoreX = 0x0802AE9Cu, kFollowStoreY = 0x0802AE9Eu;
constexpr uint32_t kClampStoreX = 0x0802B006u, kClampStoreY = 0x0802B010u;

constexpr uint32_t kPreCullOffsetX = 0x08023940u;  // 0x001FFFFF: 32 << 16, - 1
constexpr uint32_t kPreCullRangeX = 0x08023944u;   // 0x012FFFFE
constexpr uint32_t kPreCullMinY = 0x0802394Cu;     // 0xFFE00000: -32 << 16
constexpr uint32_t kPreCullMaxY = 0x08023954u;     // 0x00DFFFFF: 224 << 16, - 1

// The Lash rope's segments (0x080D9B80, 8x8 OBJs, tile 0x18C; F4 state 3
// walking right): the same four literals, loaded at these four `ldr`s,
// x in r0 and y in r2 (16.16); the segment is placed when the last test,
// `bgt` 0x080D9C80, is not taken, its record at r5 + 0x14 (attrs at +4).
constexpr uint32_t kRopeOffsetX = 0x080D9C68u, kRopeRangeX = 0x080D9C70u;
constexpr uint32_t kRopeMinY = 0x080D9C76u, kRopeMaxY = 0x080D9C7Cu;
constexpr uint32_t kRopePlaced = 0x080D9C80u;
// The Lash rope's coil (10 rotated 8x8 OBJs, tile 0x1B6; F2 state 1 of
// 2026-10-09, a write watch on a piece's attrs caught 0x080D8B2C): the same
// literals and tests in its own loop, x in r0, y in r2, record r5 + 0x14.
// Nothing recorded its placement, so in the bottom margin its raw Y read as
// above the screen and the coil vanished (F12 0107/0108, 0112/0113).
constexpr uint32_t kCoilOffsetX = 0x080D8AE0u, kCoilRangeX = 0x080D8AEAu;
constexpr uint32_t kCoilMinY = 0x080D8AF0u, kCoilMaxY = 0x080D8AF6u;
constexpr uint32_t kCoilPlaced = 0x080D8AFAu;

// Builder tests: body x (r6), body y (r4), shadow y (r4).
constexpr uint32_t kBodyX1 = 0x080222B2u, kBodyY1 = 0x080222B6u, kShadowY1 = 0x080221C8u;
constexpr uint32_t kBodyX2 = 0x080224ECu, kBodyY2 = 0x080224F0u, kShadowY2 = 0x08022402u;

// Affine builder tests (r3 = x + 0x20, y + 0x20) and its record store.
constexpr uint32_t kAffineX = 0x08022592u, kAffineY = 0x0802259Cu;
constexpr uint32_t kAffinePlaced = 0x080226B4u;
constexpr uint32_t kNearClip = 0x0801579Cu;
constexpr uint32_t kFarClip = 0x080157A2u;
constexpr uint16_t kWorldMap = 2u;

// Draw order. Every object record goes into one of 256 depth lists
// (heads 0x02003010, occupancy bits 0x03001190), which the OAM flush walks
// from 255 down, so a deeper key is drawn in front. 0x080140D8 (characters,
// key = ground y + 0x3E from the builders) clamps the key to 0..239 (`bls`
// 0x080140DE after `cmp r1,#0xEF`); 0x08014128 clamps to 0..255 (`ble`
// 0x0801412E). Below the console's screen every key is 239, and the objects
// there stack in the order they were filed: Air's Rock's pillars in the
// bottom margin, the upper pillar over the lower (F12 tla_rewind_0026, all
// six statues and pillars in list 239; 0027, same room 151 rows lower: keys
// 31 and 47, drawn right). In the expanded view both spread the view's own
// key range, -top .. 239 + bottom, over the 256 lists, keeping their order
// (keys closer than about 1.6 can share a list).
constexpr uint32_t kDepthClampCharacters = 0x080140DEu;
constexpr uint32_t kDepthClampAll = 0x0801412Eu;

// How far past the view's edge characters are still drawn.
constexpr int kSlack = 32;

struct View { int left = 0, right = 0, top = 0, bottom = 0; };

gba::GbaBus* bus() { return gbarecomp::active_bus(); }

View view() {
    View v;
    if (const gba::GbaPpu* ppu = gbarecomp::active_ppu()) {
        v.left = static_cast<int>(ppu->view_extra_left());
        v.right = static_cast<int>(ppu->view_extra_right());
        v.top = static_cast<int>(ppu->view_extra_top());
        v.bottom = static_cast<int>(ppu->view_extra_bottom());
    }
    return v;
}

bool expanded(const View& v) { return v.left | v.right | v.top | v.bottom; }

// The field, not the title screen (map number 1) or a battle.
bool field_running(gba::GbaBus* b) {
    return b->read16(kMapNumber) > 1u && b->read32(kTlaBattleSlot) == 0u;
}

// ---- Camera -------------------------------------------------------------

int32_t camera_value(gba::GbaBus* b, bool x, int32_t value, const View& v) {
    const uint32_t s = b->read32(kFieldStatePointer);
    if ((s >> 24) != 0x02u) return value;
    const int32_t lo = static_cast<int32_t>(b->read32(s + (x ? 0xECu : 0xF0u)));
    const int32_t edge = static_cast<int32_t>(b->read32(s + (x ? 0xF4u : 0xF8u)));
    const int32_t size = x ? 240 : 160;
    const int64_t spare = (static_cast<int64_t>(edge) - lo) - int64_t(size) * 65536;
    if (spare <= 0) return value;
    const int64_t before = int64_t(x ? v.left : v.top) * 65536;
    const int64_t after = int64_t(x ? v.right : v.bottom) * 65536;
    // Fit asymmetric margins exactly; when the room is smaller than the
    // visible view, center its available travel without leaving native bounds.
    const auto inset = gbarecomp::resolve_view_camera_insets(spare, before, after);
    const int32_t min = static_cast<int32_t>(int64_t(lo) + inset.before);
    const int32_t max = static_cast<int32_t>(int64_t(edge) - int64_t(size) * 65536 -
        inset.after);
    return std::min(std::max(value, min), max);
}

RuntimeMemWriteOverrideHook g_next_write = nullptr;
// The last y store the camera rule saw: what the game asked for and what was
// written (16.16). tla_camera_hold_y compares it with the live camera.
int32_t g_camera_y_requested = 0, g_camera_y_written = 0;
bool g_camera_y_seen = false;
// The map number when the current room was loaded (room clamp store).
uint16_t g_room_map = 0;

// A scene that widens the room's bounds around the old ones mid-room keeps
// the camera's offset from where the game asks (F12 tla_rewind_0044, the
// Tremor scene: bounds X 32..608 became 0..1024 at f101; the rule's limit
// 248 lifted and the camera jumped 120 to the game's 368 in one frame, past
// what the game redraws of its maps, then shook around 368..373). Bounds
// that move or shrink in the same map (0014 f89, 0024 f13, 0030 f34: other
// parts of the room) and a new map take the rule again (Jimmy, 2026-10-16:
// hold steady, shake included).
struct CameraHold {
    bool valid = false, holding = false;
    uint16_t map = 0;
    int32_t lo = 0, edge = 0;            // bounds at the last store
    int32_t held_lo = 0, held_edge = 0;  // bounds when the hold began
    int32_t offset = 0;                  // written - requested
};
CameraHold g_hold[2];  // x, y

int32_t held_camera(gba::GbaBus* b, bool x, int32_t requested, int32_t ruled) {
    const uint32_t s = b->read32(kFieldStatePointer);
    CameraHold& h = g_hold[x ? 0 : 1];
    if ((s >> 24) != 0x02u) { h.valid = false; return ruled; }
    const int32_t lo = static_cast<int32_t>(b->read32(s + (x ? 0xECu : 0xF0u)));
    const int32_t edge = static_cast<int32_t>(b->read32(s + (x ? 0xF4u : 0xF8u)));
    const uint16_t map = b->read16(kMapNumber);
    if (!h.valid || map != h.map) {
        h = CameraHold{true, false, map, lo, edge};
    } else {
        if (!h.holding && (lo != h.lo || edge != h.edge) && lo <= h.lo && edge >= h.edge) {
            h.holding = true;
            h.held_lo = h.lo;
            h.held_edge = h.edge;
        }
        if (h.holding && (lo > h.held_lo || edge < h.held_edge ||
                          (lo == h.held_lo && edge == h.held_edge)))
            h.holding = false;
        h.lo = lo;
        h.edge = edge;
    }
    if (!h.holding) {
        h.offset = ruled - requested;
        return ruled;
    }
    const int32_t size = (x ? 240 : 160) * 65536;
    return std::clamp(requested + h.offset, lo, std::max(lo, edge - size));
}

int camera_store(uint32_t pc, uint32_t addr, uint32_t requested, uint32_t width,
                 uint32_t* out_value) {
    const bool x = pc == kFollowStoreX || pc == kClampStoreX;
    const bool y = pc == kFollowStoreY || pc == kClampStoreY;
    if ((x || y) && width == 4u && out_value) {
        gba::GbaBus* b = bus();
        if (b && pc == kClampStoreX) g_room_map = b->read16(kMapNumber);
        const View v = view();
        int32_t written = static_cast<int32_t>(requested);
        if (b && expanded(v) && field_running(b) && g_room_map > 1u)
            written = held_camera(b, x, static_cast<int32_t>(requested),
                camera_value(b, x, static_cast<int32_t>(requested), v));
        if (y) {
            g_camera_y_requested = static_cast<int32_t>(requested);
            g_camera_y_written = written;
            g_camera_y_seen = true;
        }
        *out_value = static_cast<uint32_t>(written);
        return *out_value != requested ? 1 : 0;
    }
    return g_next_write ? g_next_write(pc, addr, requested, width, out_value) : 0;
}


// ---- Characters ---------------------------------------------------------

int (*g_next_literal)(uint32_t, uint32_t, uint32_t*) = nullptr;

int precull_literal(uint32_t pc, uint32_t original, uint32_t* out_value) {
    if (pc == kRopeOffsetX || pc == kCoilOffsetX) pc = kPreCullOffsetX;
    else if (pc == kRopeRangeX || pc == kCoilRangeX) pc = kPreCullRangeX;
    else if (pc == kRopeMinY || pc == kCoilMinY) pc = kPreCullMinY;
    else if (pc == kRopeMaxY || pc == kCoilMaxY) pc = kPreCullMaxY;
    const bool ours = (pc == kPreCullOffsetX && original == 0x001FFFFFu) ||
                      (pc == kPreCullRangeX && original == 0x012FFFFEu) ||
                      (pc == kPreCullMinY && original == 0xFFE00000u) ||
                      (pc == kPreCullMaxY && original == 0x00DFFFFFu);
    if (!ours) return g_next_literal ? g_next_literal(pc, original, out_value) : 0;
    const View v = view();
    gba::GbaBus* b = bus();
    if (!expanded(v) || !b || !field_running(b)) return 0;
    const uint32_t left = static_cast<uint32_t>(32 + v.left + kSlack);
    const uint32_t right = static_cast<uint32_t>(272 + v.right + kSlack);
    const uint32_t offset = (left << 16) - 1u;
    switch (pc) {
    case kPreCullOffsetX: *out_value = offset; break;
    case kPreCullRangeX: *out_value = offset + (right << 16) - 1u; break;
    case kPreCullMinY:
        *out_value = static_cast<uint32_t>(-(32 + v.top + kSlack)) << 16;
        break;
    default:
        *out_value = (static_cast<uint32_t>(224 + v.bottom + kSlack) << 16) - 1u;
        break;
    }
    return 1;
}

// Where each object record was last placed (engine field/obj_placements.h).
using Placement = gbarecomp::ObjPlacements::Placement;
gbarecomp::ObjPlacements g_placements;

void sync_placement_scene(gba::GbaBus* b) {
    g_placements.sync(runtime_current_frame(), b->read32(kFieldStatePointer),
                      b->read32(kTlaBattleSlot), b->read16(kMapNumber));
}

void note_placed(uint32_t record, int x, int y) {
    gba::GbaBus* b = bus();
    if (!b) return;
    sync_placement_scene(b);
    // The record's full tile now, while its sprite-memory grant is current.
    const uint16_t attr2 = b->read16(record + 8u);
    const int tile = tla_sprite_memory_tile(record, attr2);
    g_placements.note(*b, runtime_current_frame(), record, x, y,
                      tile >= 0 ? tile : attr2 & 0x3FF);
}

// A game window is open this frame (set by tla_widescreen_frame).
bool g_window_open = false;

// The menus build their own sprites (priority 0) with the field's character
// builder, which sees them 256 rows up: the Psynergy menu's icons at raw Y
// 50..98 were recorded at -206..-158 and vanished (F12 tla_rewind_0023,
// 2026-10-15; the console shows them at the raw rows). With a window open
// those sprites take the console's own reading, given here: the scene's
// fallback keeps a sprite near last frame's place, and the icons park at raw
// Y 168 (-88) before sliding in at 50..98, so it chose -206..-158 again
// (F12 tla_rewind_0029 frames 44..47).
bool menu_sprite(uint16_t attr2) {
    return g_window_open && ((attr2 >> 10) & 3u) == 0u;
}

int (*g_next_branch)(uint32_t, uint32_t, uint32_t*) = nullptr;

// F12 0001/0002: the world-map descriptor loader at 0x0200B6A8
// unloads the cave when its focus-relative X crosses -240 world units,
// although its projected sprite is still in the left margin. Its two
// camera modes use X +/-160 or +/-240 and Z -300..200 or -450..300.
// Grow each existing world-space span in proportion to the added screen
// pixels. The projected builder still decides which sprites are visible.
// Harvested copies configure these four branches at their packed origins;
// the code generator restores the image base before calling the hook.
int world_range_branch(uint32_t pc, uint32_t original, uint32_t* out) {
    if (pc < 0x02000000u || pc >= 0x02040000u) return -1;
    const uint32_t live_pc = g_runtime_image_base + (pc - 0x02000000u);
    uint32_t signature = 0;
    int coordinate = 0, bound = 0, margin = 0;
    const View v = view();
    switch (live_pc) {
    case 0x0200B768u:
        signature = 0xDC00428Du; coordinate = 5; bound = 1; margin = -v.left;
        break;
    case 0x0200B770u:
        signature = 0xDB004295u; coordinate = 5; bound = 2; margin = v.right;
        break;
    case 0x0200B778u:
        signature = 0xDC00429Fu; coordinate = 7; bound = 3; margin = -v.top;
        break;
    case 0x0200B780u:
        signature = 0xDB004287u; coordinate = 7; bound = 0; margin = v.bottom;
        break;
    default: return -1;
    }
    gba::GbaBus* b = bus();
    if (!b || !expanded(v) || !field_running(b) ||
        b->read16(kMapNumber) != kWorldMap ||
        b->read32(live_pc - 2u) != signature ||
        b->read32(0x0200B8C8u) != 0xFF600000u ||
        b->read32(0x0200B8CCu) != 0xFED40000u) return 0;
    const uint32_t sp = g_cpu.R[13];
    const bool x = coordinate == 5;
    const int64_t span = x
        ? int64_t(static_cast<int32_t>(b->read32(sp + 8u))) -
            static_cast<int32_t>(b->read32(sp + 12u))
        : int64_t(static_cast<int32_t>(b->read32(sp))) -
            static_cast<int32_t>(b->read32(sp + 4u));
    if (span <= 0) return 0;
    const int64_t edge = int64_t(static_cast<int32_t>(g_cpu.R[bound])) +
                         span * margin / (x ? 240 : 160);
    const int64_t at = static_cast<int32_t>(g_cpu.R[coordinate]);
    const bool minimum = live_pc == 0x0200B768u || live_pc == 0x0200B778u;
    *out = (minimum ? at > edge : at < edge) ? 1u : 0u;
    return *out != original;
}

// The affine builder: `bls` taken keeps the object; the record's placement.
int affine_branch(uint32_t pc, uint32_t original, uint32_t* out_decision) {
    const View v = view();
    gba::GbaBus* b = bus();
    if (!expanded(v) || !b || !field_running(b)) return 0;
    if (pc == kAffinePlaced) {
        const uint32_t sl = g_cpu.R[10];
        const int x = static_cast<int32_t>(b->read32(sl)) -
                      static_cast<int32_t>(g_cpu.R[9]) +
                      static_cast<int32_t>(g_cpu.R[0]);
        const int y = static_cast<int32_t>(b->read32(sl + 4u)) -
                      static_cast<int32_t>(g_cpu.R[11]) +
                      static_cast<int32_t>(b->read32(g_cpu.R[13]));
        note_placed(g_cpu.R[7], x, y);
        return 0;
    }
    const int at = static_cast<int32_t>(g_cpu.R[3]) - 0x20;
    const bool keep = pc == kAffineX
        ? at >= -(32 + v.left + kSlack) && at <= 272 + v.right + kSlack
        : at >= -(32 + v.top + kSlack) && at <= 208 + v.bottom + kSlack;
    *out_decision = keep ? 1u : 0u;
    return *out_decision != original ? 1 : 0;
}

// The depth key's clamp (see kDepthClampCharacters): r1 becomes the key's
// list in the expanded range and the clamp is skipped.
int depth_branch(uint32_t original, uint32_t* out_decision) {
    const View v = view();
    gba::GbaBus* b = bus();
    if (!expanded(v) || !b || !field_running(b)) return 0;
    const int64_t low = -v.top, span = 240 + v.top + v.bottom;
    const int64_t key = static_cast<int32_t>(g_cpu.R[1]);
    g_cpu.R[1] = static_cast<uint32_t>(
        std::clamp<int64_t>((key - low) * 256 / span, 0, 255));
    *out_decision = 1u;  // taken: past the clamp
    return *out_decision != original;
}

int builder_branch(uint32_t pc, uint32_t original, uint32_t* out_decision) {
    if (const int result = world_range_branch(pc, original, out_decision);
        result >= 0) return result;
    if (const int result = tla_sprite_memory_branch(pc, original, out_decision);
        result >= 0) return result;
    if (pc == kDepthClampCharacters || pc == kDepthClampAll)
        return depth_branch(original, out_decision);
    if (pc == kAffineX || pc == kAffineY || pc == kAffinePlaced)
        return affine_branch(pc, original, out_decision);
    if (pc == kRopePlaced || pc == kCoilPlaced) {
        gba::GbaBus* b = bus();
        if (original == 0u && b && expanded(view()) && field_running(b))
            note_placed(g_cpu.R[5] + 0x14u,
                        (static_cast<int32_t>(g_cpu.R[0]) >> 16) - 4,
                        (static_cast<int32_t>(g_cpu.R[2]) >> 16) - 4);
        return 0;
    }
    if (pc == kNearClip || pc == kFarClip) {
        gba::GbaBus* b = bus();
        if (!expanded(view()) || !b || b->read16(kMapNumber) != kWorldMap ||
            !field_running(b))
            return 0;
        // The old depth planes fit the console view. The widened builder
        // tests the projected position instead. Keep points behind the camera
        // and zero reciprocal denominators hidden (projection shifts depth 11).
        const int32_t depth = static_cast<int32_t>(g_cpu.R[1]);
        *out_decision = pc == kNearClip &&
            (depth <= 0 || (static_cast<uint32_t>(depth) >> 11) == 0) ? 1u : 0u;
        return *out_decision != original;
    }
    const bool body_x = pc == kBodyX1 || pc == kBodyX2;
    const bool body_y = pc == kBodyY1 || pc == kBodyY2;
    const bool shadow_y = pc == kShadowY1 || pc == kShadowY2;
    if (!body_x && !body_y && !shadow_y)
        return g_next_branch ? g_next_branch(pc, original, out_decision) : 0;
    const View v = view();
    gba::GbaBus* b = bus();
    if (!expanded(v) || !b || !field_running(b)) return 0;
    const int x = static_cast<int32_t>(g_cpu.R[6]);
    const int y = static_cast<int32_t>(g_cpu.R[4]);
    // `bgt` skips the object: taken when past the widened edge. A menu
    // sprite's shadow (its record follows the body's, r7 + 0x1C) keeps the
    // console's own test: setting a Djinn put three Djinn shadows at row 210,
    // which the console drops and the widened test drew in the bottom margin
    // (F12 tla_rewind_0035 frames 53..119, bodies priority 0 at -86/-83).
    const bool menu_shadow = shadow_y && menu_sprite(b->read16(g_cpu.R[7] + 8u));
    const bool cull = body_x ? x > 239 + v.right + kSlack
                             : y > 159 + (menu_shadow ? 0 : v.bottom + kSlack);
    if (!cull && (body_y || shadow_y)) {
        // Every placement is recorded, so a record that moves back above
        // row 160 stops answering with its old Y. The body's X is r6; the
        // shadow's is worked out from the builder's stack as the game does
        // next (0x080221D8 / 0x08022412).
        const uint32_t sp = g_cpu.R[13];
        uint32_t record = g_cpu.R[7];
        int ox = x;
        if (shadow_y) {
            record += 0x1Cu;
            ox = pc == kShadowY1
                     ? (static_cast<int32_t>(b->read32(sp + 0xCu)) >> 16) - 8
                     : (static_cast<int32_t>(b->read32(sp + 8u)) >> 16) -
                           static_cast<int32_t>(b->read32(sp + 0x10u));
        }
        note_placed(record, ox, y);
    }
    *out_decision = cull ? 1u : 0u;
    return *out_decision != original ? 1 : 0;
}

// The placement an OAM entry came from, or null. Every raw value is
// ambiguous once characters are placed outside the console's screen: the
// widened pre-cull admits NPCs whose sprite sits as far as ~112 rows above
// the view, and raw Y 144 then reads as row 144, in the bottom margin
// (F12 0033, 2026-10-05: an NPC above the view drawn at the bottom left).
const Placement* placement_for(uint16_t attr0, uint16_t attr1, uint16_t attr2) {
    gba::GbaBus* b = bus();
    if (!b || !field_running(b)) return nullptr;
    sync_placement_scene(b);
    return g_placements.find(*b, runtime_current_frame(), attr0, attr1, attr2);
}

extern "C" int tla_obj_y_provider(int /*oam_index*/, uint16_t attr0,
                                  uint16_t attr1, uint16_t attr2, int* out_y) {
    if (menu_sprite(attr2)) {
        const int raw = attr0 & 0xFF;
        *out_y = raw >= 160 ? raw - 256 : raw;
        return 1;
    }
    const Placement* p = placement_for(attr0, attr1, attr2);
    if (!p) return 0;
    *out_y = p->y;
    return 1;
}

extern "C" int tla_obj_x_provider(int /*oam_index*/, uint16_t attr0,
                                  uint16_t attr1, uint16_t attr2, int* out_x) {
    if (menu_sprite(attr2)) {
        const int raw = attr1 & 0x1FF;
        *out_x = (raw & 0x100) ? raw - 0x200 : raw;
        return 1;
    }
    const Placement* p = placement_for(attr0, attr1, attr2);
    if (!p) return 0;
    *out_x = p->x;
    return 1;
}

// A field person's full tile number, from the record that placed it: its
// owner's current grant when the tile is in it, else the tile it had when
// placed if OAM still shows that one. The grant comes first: sand puffs are
// noted while their grant is being renewed and read as console tiles (F12
// tla_rewind_0051 frame 115, owners 0x0A and 0x13). The noted tile covers a
// grant released since: opening a menu frees every person's tiles a frame
// before OAM drops them (tla_rewind_0047 frame 105).
extern "C" int tla_obj_tile_provider(int /*oam_index*/, uint16_t attr0,
                                     uint16_t attr1, uint16_t attr2) {
    const Placement* p = placement_for(attr0, attr1, attr2);
    if (!p) return -1;
    if (const int now = tla_sprite_memory_tile(p->record, attr2); now >= 0)
        return now;
    if (p->tile >= 0 && ((p->tile ^ attr2) & 0x3FF) == 0 && (p->tile & 0x400))
        return p->tile;
    return -1;
}

}  // namespace

// Rows the camera rule holds the camera below (+) or above (-) where the game
// put it, while the camera is still where the rule wrote it; 0 once another
// routine has moved it (Air's Rock's tornado cutscene pans with its own
// stores and goes to the room's top, F12 0016/0018/0019, 2026-10-14).
int tla_camera_hold_y() {
    gba::GbaBus* b = bus();
    if (!b || !g_camera_y_seen) return 0;
    const uint32_t s = b->read32(kFieldStatePointer);
    if ((s >> 24) != 0x02u) return 0;
    if (static_cast<int32_t>(b->read32(s + 0xE8u)) != g_camera_y_written) return 0;
    return static_cast<int>(
        (int64_t(g_camera_y_written) - int64_t(g_camera_y_requested)) / 65536);
}

// Called by the graphics card's capture (src/tla_gpu_field.cpp) each frame:
// run_game resets the engine's object hooks at start-up.
void tla_widescreen_frame(bool window_open) {
    g_window_open = window_open;
    const bool on = expanded(view());
    gba::g_ws_obj_attr_y_provider = on ? tla_obj_y_provider : nullptr;
    gba::g_ws_obj_attr_x_provider = on ? tla_obj_x_provider : nullptr;
    gba::g_ws_obj_tile_provider =
        tla_sprite_memory_frame(on) ? tla_obj_tile_provider : nullptr;
}

// Chains in front of the hooks already installed.
void tla_install_widescreen(gbarecomp::RunOptions& options) {
    g_next_write = g_runtime_mem_write_override;
    g_runtime_mem_write_override = camera_store;
    g_next_literal = options.thumb_literal_override;
    options.thumb_literal_override = precull_literal;
    g_next_branch = options.conditional_branch_override;
    options.conditional_branch_override = builder_branch;
}
