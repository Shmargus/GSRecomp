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
#include "tile_ring_redraw.h"
#include "tla.h"

namespace {

constexpr uint32_t kFieldStatePointer = 0x03000020u;
constexpr uint32_t kMapNumber = 0x02000428u;

constexpr uint32_t kFollowStoreX = 0x0802AE9Cu, kFollowStoreY = 0x0802AE9Eu;
constexpr uint32_t kClampStoreX = 0x0802B006u, kClampStoreY = 0x0802B010u;
constexpr uint32_t kCameraYLimit = 0x0802AE80u;

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

// Climbing vines use the same four cull literals in their own segment loop.
// F12 0081 f119: 0x080DAAB0 rejects the head at y=-40 before its -8 anchor
// adjustment, leaving the five lower pieces. Its record is also r5+0x14.
constexpr uint32_t kVineOffsetX = 0x080DABA4u, kVineRangeX = 0x080DABACu;
constexpr uint32_t kVineMinY = 0x080DABB4u, kVineMaxY = 0x080DABBAu;
constexpr uint32_t kVinePlaced = 0x080DABBEu;

// Field effect sprites (0x080EB01C, 27 callers 0x080D1E0E..0x080EBB3A; the
// summon tablet's letters, F12 0155): record r7, attrs at +4, screen x/y
// 16.16 at +0xC/+0x10, box w/h at +0x20/+0x22. r5/r6 = box left/top; the
// sprite is dropped when r5 + w < 0 (`blt`), r5 > 239 (`bgt`), r6 + h <= 0
// (`ble`), r6 > 159 (`bgt`), the console's screen exactly (measured: left
// 239 drawn, 240 dropped; top -7 drawn, -8 dropped). The letters' loop ran to
// x 270 and was cut at the screen's right edge.
constexpr uint32_t kEffectLeft = 0x080EB22Au, kEffectRight = 0x080EB22Eu;
constexpr uint32_t kEffectTop = 0x080EB236u, kEffectBottom = 0x080EB23Au;

// ROM 0x0801401C computes one four-halfword OBJ transform. At 0x0801403E
// it rejects allocation 32 and returns slot 31, even if that slot belongs
// to a different rotation (0085: the bottom vine tips). Reuse byte-identical
// completed transforms so margin pieces do not exhaust the hardware slots.
constexpr uint32_t kMatrixCapacity = 0x0801403Eu;
constexpr uint32_t kVineMatrixReturn = 0x080DAC3Cu;
constexpr uint32_t kMatrixCount = 0x030011FCu, kMatrixData = 0x020054D0u;
struct MatrixPending {
    uint32_t return_pc = 0, depth = 0, index = 0;
} g_matrix_pending;
RuntimeCallReturnHook g_next_call_return = nullptr;

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

// A battle draws its objects with the same character builders (veneers
// 0x08020010 / 0x08020018). Eclipse's dragon (F12 tla_rewind_0140) is 32
// such objects, one per 32x32 cell of a 256x160 picture: a cell past x 239
// is skipped at 0x080222B2 / 0x080224EC and its record keeps the last x it
// was drawn at (239), so the wing ended at the console's right edge. In a
// battle only the sideways tests are widened: its rows are a band, and an
// 8-bit Y past row 159 would read as the top.
bool battle_running(gba::GbaBus* b) {
    return b->read32(kTlaBattleSlot) != 0u;
}

// ---- Camera -------------------------------------------------------------

// The rule for room bounds lo..edge (16.16).
int32_t camera_value_in(bool x, int32_t value, const View& v, int32_t lo, int32_t edge) {
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

int32_t camera_value(gba::GbaBus* b, bool x, int32_t value, const View& v) {
    const uint32_t s = b->read32(kFieldStatePointer);
    if ((s >> 24) != 0x02u) return value;
    return camera_value_in(x, value, v,
        static_cast<int32_t>(b->read32(s + (x ? 0xECu : 0xF0u))),
        static_cast<int32_t>(b->read32(s + (x ? 0xF4u : 0xF8u))));
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
//
// The hold lets go as the game's camera travels: each pixel the request
// moves past the farthest it has been since the hold began takes one pixel
// off the offset, so walking eases the view back to the game's camera and a
// shake (inside the range already travelled) does not. Jupiter Lighthouse
// (map 257, F12 0141) began a hold in ordinary play, bounds 16..800 never
// changed back, and Felix stayed 120 px off centre (Jimmy, 2026-10-24:
// catch up while he walks, no jump).
//
// Bounds that shrink inside the old ones while the game's own camera does
// not jump keep the old bounds for the expanded view instead (`kept`):
// stepping off Lemuria's dock stairs (map 201, F12 0136 f24) narrows y
// 352..928 to 750..928, and the rule moved the camera 71 rows in one frame
// and blacked out the scenery above 750 (Jimmy, 2026-10-23: keep the camera
// still; a 16 px/frame glide was rejected). 0024/0030 (another part of the
// room) do not narrow inside the old bounds; 0014 does, but as a new room
// behind a fade (below). The game's own camera jumping more than it moves in
// a frame (16 px, r5 at 0x0802AE7E) past its new clamp, bounds that grow past
// the kept ones, or bounds back to them end it.
struct CameraHold {
    bool valid = false, holding = false;
    uint16_t map = 0;
    int32_t lo = 0, edge = 0;            // bounds at the last store
    int32_t held_lo = 0, held_edge = 0;  // bounds when the hold began
    int32_t travel_lo = 0, travel_hi = 0;  // requests seen since then
    int32_t offset = 0;                  // written - requested
    bool kept = false;
    int32_t kept_lo = 0, kept_edge = 0;  // the wider bounds still in force
    bool have_requested = false;
    int32_t requested = 0;               // the game's last request

    // New bounds that end the hold: no longer around the held ones, or the
    // held ones again.
    bool hold_ends(int32_t new_lo, int32_t new_edge) const {
        return new_lo > held_lo || new_edge < held_edge ||
               (new_lo == held_lo && new_edge == held_edge);
    }
    // New bounds that end the kept ones: grown past them, or them again.
    bool kept_ends(int32_t new_lo, int32_t new_edge) const {
        return new_lo < kept_lo || new_edge > kept_edge ||
               (new_lo == kept_lo && new_edge == kept_edge);
    }
};
CameraHold g_hold[2];  // x, y

int camera_y_limit(uint32_t original, uint32_t* out_decision) {
    gba::GbaBus* b = bus();
    const CameraHold& h = g_hold[1];
    if (!b || !expanded(view()) || !field_running(b) || g_room_map <= 1u ||
        !h.valid || !(h.holding || h.kept) || b->read16(kMapNumber) != h.map)
        return 0;
    const uint32_t s = b->read32(kFieldStatePointer);
    if ((s >> 24) != 0x02u) return 0;
    const int32_t lo = static_cast<int32_t>(b->read32(s + 0xF0u));
    const int32_t edge = static_cast<int32_t>(b->read32(s + 0xF8u));
    if (h.kept ? h.kept_ends(lo, edge) : h.hold_ends(lo, edge))
        return 0;  // the store will release the hold
    // The game's limiter reads the previous *written* camera into r2, then
    // compares r2-r7 with r5 (16 pixels). Undo our held offset for that read,
    // so the store applies it once, rather than feeding it back every frame.
    // Keep the game's own limiter, including its two branch paths.
    g_cpu.R[2] -= static_cast<uint32_t>(h.offset);
    g_cpu.R[3] = g_cpu.R[2] - g_cpu.R[7];
    *out_decision = static_cast<int32_t>(g_cpu.R[3]) <=
                    static_cast<int32_t>(g_cpu.R[5]);
    return *out_decision != original;
}

int32_t held_camera(gba::GbaBus* b, bool x, int32_t requested, int32_t ruled,
                    const View& v) {
    const uint32_t s = b->read32(kFieldStatePointer);
    CameraHold& h = g_hold[x ? 0 : 1];
    if ((s >> 24) != 0x02u) { h.valid = false; return ruled; }
    const int32_t lo = static_cast<int32_t>(b->read32(s + (x ? 0xECu : 0xF0u)));
    const int32_t edge = static_cast<int32_t>(b->read32(s + (x ? 0xF4u : 0xF8u)));
    const uint16_t map = b->read16(kMapNumber);
    if (!h.valid || map != h.map) {
        h = CameraHold{true, false, map, lo, edge};
    } else {
        const int32_t size = (x ? 240 : 160) << 16;
        constexpr int32_t kGameStep = 16 << 16;
        // Where the game's last request lands inside the new bounds: its own
        // clamp moving it is not a jump.
        const int32_t settled = std::min(std::max(h.requested, lo),
                                         std::max(lo, edge - size));
        const bool jumped = !h.have_requested ||
            std::abs(int64_t(requested) - settled) > kGameStep;
        // Back to the kept bounds: the rule there is where the kept camera
        // already is, so neither hold starts.
        const bool returned = h.kept && lo == h.kept_lo && edge == h.kept_edge;
        if (h.kept && (h.kept_ends(lo, edge) || jumped))
            h.kept = false;
        if (h.holding && h.hold_ends(lo, edge))
            h.holding = false;
        const bool changed = lo != h.lo || edge != h.edge;
        // Only in plain view: 0014 f89 sets a new room's bounds inside the
        // old 0..512 with the field layers off (DISPCNT 0x0140, faded out);
        // 0136 f24 has them on (0x1F40).
        const bool field_shown = (b->read16(0x04000000u) & 0x0C00u) == 0x0C00u;
        if (!h.kept && !h.holding && !returned && !jumped && changed && field_shown &&
            lo >= h.lo && edge <= h.edge) {
            h.kept = true;
            h.kept_lo = h.lo;
            h.kept_edge = h.edge;
        }
        if (!h.holding && !h.kept && !returned && changed &&
            lo <= h.lo && edge >= h.edge) {
            h.holding = true;
            h.held_lo = h.lo;
            h.held_edge = h.edge;
            h.travel_lo = h.travel_hi = requested;
        }
        h.lo = lo;
        h.edge = edge;
    }
    h.have_requested = true;
    h.requested = requested;
    if (h.kept) {
        const int32_t kept = camera_value_in(x, requested, v, h.kept_lo, h.kept_edge);
        h.offset = kept - requested;
        return kept;
    }
    if (h.holding) {
        int64_t travelled = 0;
        if (requested > h.travel_hi) {
            travelled = int64_t(requested) - h.travel_hi;
            h.travel_hi = requested;
        } else if (requested < h.travel_lo) {
            travelled = int64_t(h.travel_lo) - requested;
            h.travel_lo = requested;
        }
        const int64_t left = std::max<int64_t>(0, std::abs(int64_t(h.offset)) - travelled);
        h.offset = static_cast<int32_t>(h.offset < 0 ? -left : left);
        if (h.offset == 0) h.holding = false;
    }
    if (!h.holding) {
        h.offset = ruled - requested;
        return ruled;
    }
    // Preserve the cutscene offset within the expanded view's existing room
    // limits. Native-only limits let a hold with zero offset bypass the
    // margin rule entirely (0082: right edge1075 beyond room edge1024).
    return camera_value(b, x, requested + h.offset, v);
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
                camera_value(b, x, static_cast<int32_t>(requested), v), v);
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
    if (pc == kRopeOffsetX || pc == kCoilOffsetX || pc == kVineOffsetX) pc = kPreCullOffsetX;
    else if (pc == kRopeRangeX || pc == kCoilRangeX || pc == kVineRangeX) pc = kPreCullRangeX;
    else if (pc == kRopeMinY || pc == kCoilMinY || pc == kVineMinY) pc = kPreCullMinY;
    else if (pc == kRopeMaxY || pc == kCoilMaxY || pc == kVineMaxY) pc = kPreCullMaxY;
    const bool ours = (pc == kPreCullOffsetX && original == 0x001FFFFFu) ||
                      (pc == kPreCullRangeX && original == 0x012FFFFEu) ||
                      (pc == kPreCullMinY && original == 0xFFE00000u) ||
                      (pc == kPreCullMaxY && original == 0x00DFFFFFu);
    if (!ours) return g_next_literal ? g_next_literal(pc, original, out_value) : 0;
    const View v = view();
    gba::GbaBus* b = bus();
    if (!expanded(v) || !b) return 0;
    if (!field_running(b) &&
        !(battle_running(b) && (pc == kPreCullOffsetX || pc == kPreCullRangeX)))
        return 0;
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

void note_placed(uint32_t record, int x, int y, int placed_attr0 = -1,
                 bool uses_tile_grant = false) {
    gba::GbaBus* b = bus();
    if (!b) return;
    sync_placement_scene(b);
    // The record's full tile now, while its sprite-memory grant is current.
    const uint16_t attr2 = b->read16(record + 8u);
    const int tile = uses_tile_grant ? tla_sprite_memory_tile(record, attr2) : -1;
    g_placements.note(*b, runtime_current_frame(), record, x, y,
                      tile >= 0 ? tile : attr2 & 0x3FF, placed_attr0,
                      uses_tile_grant);
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

void matrix_return(uint32_t pc, uint32_t depth) {
    if (g_next_call_return) g_next_call_return(pc, depth);
    if (!g_matrix_pending.return_pc ||
        (pc & ~1u) != g_matrix_pending.return_pc ||
        depth + 1u != g_matrix_pending.depth)
        return;
    const auto pending = g_matrix_pending;
    g_matrix_pending = {};
    gba::GbaBus* b = bus();
    if (!b || !expanded(view()) || !field_running(b) ||
        g_cpu.R[0] != pending.index || pending.index >= 32u ||
        b->read8(kMatrixCount) != pending.index + 1u)
        return;
    const uint32_t at = kMatrixData + pending.index * 8u;
    const uint32_t first = b->read32(at), second = b->read32(at + 4u);
    for (uint32_t i = 0; i < pending.index; ++i) {
        const uint32_t previous = kMatrixData + i * 8u;
        if (b->read32(previous) != first || b->read32(previous + 4u) != second)
            continue;
        // The caller has not filed its OBJ yet. Return the identical earlier
        // transform and release only this last, still-unreferenced slot.
        g_cpu.R[0] = i;
        b->write8(kMatrixCount, static_cast<uint8_t>(pending.index));
        return;
    }
}

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
        // This builder uses the same tile-owner records as character bodies.
        // World-map landmarks in F12 0092 occupy the second sprite page.
        note_placed(g_cpu.R[7], x, y, -1, true);
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

// Apojii's Rainbow Water emitter (0096) masks X/Y before queueing its
// sprite. At the existing depth hook its descriptor is still record-0x14
// and r8/r9 retain the emitter's quantized camera. Keep the full placement,
// including real negative rows, rather than guessing from its wrapped byte.
void note_rainbow_placement() {
    if (g_cpu.R[14] != 0x02009759u) return;
    gba::GbaBus* b = bus();
    if (!b || !expanded(view()) || !field_running(b) ||
        b->read16(kMapNumber) != 154u ||
        b->read32(0x020096F0u) != 0x14232000u ||
        b->read32(0x02009700u) != 0x3C080040u ||
        b->read32(0x02009718u) != 0x33FF005Bu ||
        b->read32(0x02009744u) != 0x19834658u ||
        b->read32(0x02009C40u) != 0x080001E9u)
        return;
    const uint32_t record = g_cpu.R[0];
    if (record < 0x02000014u || record > 0x0203FFF4u) return;
    const uint32_t descriptor = record - 0x14u;
    const int x = (static_cast<int32_t>(b->read32(descriptor) - g_cpu.R[9]) >> 16) - 8;
    const int y = (static_cast<int32_t>(b->read32(descriptor + 8u) - g_cpu.R[8] -
                                        b->read32(descriptor + 4u)) >> 16) - 8;
    // Refuse a stale/reused record, even with a matching caller image.
    if ((b->read16(record + 4u) & 0xFFu) != (static_cast<unsigned>(y) & 0xFFu) ||
        (b->read16(record + 6u) & 0x1FFu) != (static_cast<unsigned>(x) & 0x1FFu))
        return;
    note_placed(record, x, y);
}

bool jump_pad_image(gba::GbaBus* b) {
    return b && field_running(b) &&
        b->read32(0x0200B1CCu) == 0x401E33FFu &&
        b->read32(0x0200B1F0u) == 0x30181C28u &&
        b->read32(0x0200B1FAu) == 0x1A7C4641u &&
        b->read32(0x0200B21Cu) == 0x3024401Cu &&
        b->read32(0x0200B244u) == 0x990262ECu &&
        b->read32(0x0200B26Cu) == 0xFFFF0000u &&
        b->read32(0x0200BC98u) == 0x080001E9u;
}

// F12 0108/0109 and 0112/0113 carry the same pad and sparkle emitters
// in different rooms. Identify the drawing code, not the room number.
bool sparkle_image(gba::GbaBus* b) {
    return jump_pad_image(b) &&
        b->read32(0x0200B56Au) == 0x46901B57u &&
        b->read32(0x0200B5B4u) == 0x2380D821u &&
        b->read32(0x0200B5C4u) == 0x2300401Fu &&
        b->read32(0x0200B5F4u) == 0xF0004641u;
}

struct SparkleMarginRandom {
    uint32_t record = 0, seed = 0;
};
SparkleMarginRandom g_sparkle_margin_random;
// ROM 0x08014878..0x08014898: the sparkle's third random call chooses
// its artwork. An extra margin sprite must not advance the game's RNG.
constexpr uint32_t kSparkleRandomState = 0x030011BCu;

// F12 0103/0104: the pad emitter still rejects X outside -15..255,
// and each section's Y outside -47..191. Extend those measured intervals
// by the actual view margins, preserving the original unsigned tests.
int jump_pad_range_branch(uint32_t pc, uint32_t original, uint32_t* out) {
    if (pc < 0x02000000u || pc >= 0x02040000u) return -1;
    const uint32_t live_pc = g_runtime_image_base + (pc - 0x02000000u);
    uint32_t signature = 0;
    switch (live_pc) {
    case 0x0200B1BEu: signature = 0xD845429Au; break;
    case 0x0200B1C6u: signature = 0xD8182BEEu; break;
    case 0x0200B208u: signature = 0xD8202BEEu; break;
    case 0x0200B5ACu: signature = 0xD82542A6u; break;
    case 0x0200B5B4u: signature = 0xD8212BEEu; break;
    default: return -1;
    }
    gba::GbaBus* b = bus();
    const View v = view();
    if (!expanded(v) || !jump_pad_image(b) ||
        b->read32(live_pc - 2u) != signature) return 0;
    const bool sparkle = live_pc == 0x0200B5ACu || live_pc == 0x0200B5B4u;
    if (sparkle && !sparkle_image(b)) return 0;
    if (live_pc == 0x0200B5ACu) g_sparkle_margin_random = {};
    if (live_pc == 0x0200B1BEu || live_pc == 0x0200B5ACu) {
        *out = g_cpu.R[sparkle ? 6 : 2] + static_cast<uint32_t>(v.left) >
               g_cpu.R[sparkle ? 4 : 3] + static_cast<uint32_t>(v.left + v.right);
    } else {
        *out = g_cpu.R[3] + static_cast<uint32_t>(v.top) >
               238u + static_cast<uint32_t>(v.top + v.bottom);
    }
    if (sparkle && original && !*out)
        g_sparkle_margin_random = {g_cpu.R[9] + 0x10u,
                                   b->read32(kSparkleRandomState)};
    return *out != original;
}

// The sparkle queue retains full X in sl, but has already masked Y.
// Its original instructions add random*10 >> 16, then subtract 10.
// Recover that measured 0..9 jitter relative to the descriptor's full Y;
// this distinguishes bottom rows from their identical wrapped top bytes.
void note_sparkle_placement() {
    if (g_cpu.R[14] != 0x0200B5FBu) return;
    gba::GbaBus* b = bus();
    if (!expanded(view()) || !sparkle_image(b)) return;
    const uint32_t record = g_cpu.R[0];
    if (record < 0x02000010u || record > 0x0203FFF4u) return;
    const uint32_t descriptor = record - 0x10u;
    if (descriptor != g_cpu.R[9]) return;
    if (g_sparkle_margin_random.record == record) {
        b->write32(kSparkleRandomState, g_sparkle_margin_random.seed);
        g_sparkle_margin_random = {};
    }
    const uint32_t field = b->read32(kFieldStatePointer);
    if (field < 0x02000000u || field > 0x0203FF18u) return;
    const uint32_t camera_y = b->read32(field + 0xE8u) & 0xFFFF0000u;
    const int base_y = (static_cast<int32_t>(b->read32(descriptor + 8u) -
        camera_y - b->read32(descriptor + 4u)) >> 16) - 10;
    const uint32_t jitter = (b->read16(record + 4u) -
                             static_cast<uint32_t>(base_y)) & 0xFFu;
    const int x = static_cast<int32_t>(g_cpu.R[10]);
    if (jitter >= 10u || (b->read16(record + 6u) & 0x1FFu) !=
                         (static_cast<unsigned>(x) & 0x1FFu)) return;
    note_placed(record, x, base_y + static_cast<int>(jitter));
}

// F12 0099/0100: map 164's jumping pads queue the cap and stem separately,
// after masking their coordinates. r7/r8 still retain the full projection.
void note_jump_pad_placement() {
    const uint32_t caller = g_cpu.R[14];
    if (caller != 0x0200B1FBu && caller != 0x0200B24Du) return;
    gba::GbaBus* b = bus();
    if (!expanded(view()) || !jump_pad_image(b)) return;
    const bool cap = caller == 0x0200B1FBu;
    const uint32_t record = g_cpu.R[0], offset = cap ? 0x18u : 0x24u;
    if (record < 0x02000000u + offset || record > 0x0203FFF0u) return;
    // The queue's prologue has already replaced r5 with this record.
    const uint32_t descriptor = record - offset;
    const uint32_t field = b->read32(kFieldStatePointer);
    if (field < 0x02000000u || field > 0x0203FF18u) return;
    const uint32_t linked = b->read32(descriptor + 0x14u);
    if (linked && (linked < 0x02000000u || linked > 0x0203FFF4u)) return;
    const uint32_t camera_x = b->read32(field + 0xE4u) & 0xFFFF0000u;
    const int x = (static_cast<int32_t>(b->read32(linked ? linked : descriptor) -
                                        camera_x) >> 16) - 8;
    const int y = (static_cast<int32_t>(g_cpu.R[7] - g_cpu.R[8]) >> 16) +
                  (cap ? -8 : 8);
    if ((b->read16(record + 4u) & 0xFFu) != (static_cast<unsigned>(y) & 0xFFu) ||
        (b->read16(record + 6u) & 0x1FFu) != (static_cast<unsigned>(x) & 0x1FFu))
        return;
    // Use the normal position provider: F12 0102 also has caps wrapped
    // above the view after they move back inside the original screen.
    note_placed(record, x, y);
}

// F12 tla_rewind_0144/0145 (map 255, Jupiter Lighthouse): the room's own
// Thumb routine (0x0200925C there) draws a top piece over each pillar in its
// list (0x0200ABDC, count 0x0200ABDA), 16x16, or 32x64 while its mode word
// (0x0200ACDC) is nonzero. It skips a piece whose x + 7 (+ 0x17) is above
// 334 (350) unsigned, or whose row is <= -16 (-64) or > 239 (175): the
// console's screen. A pillar at the top edge (0144: base row 0, top row -16)
// kept its base and lost its top. Extend those tests by the view's margins
// and record the placement, since a wrapped row cannot tell -16 from 240.
// Map 264 (F12 0148) loads the same code at 0x02009A78: only its two BLs
// and its pool's data addresses differ. So the code is found from the
// branch, by its own words relative to its start, not by room or address.
struct PillarTopSite {
    uint32_t offset;    // from the routine's start
    uint16_t opcode;
    bool x_test, low, tall;
};
constexpr PillarTopSite kPillarTopSites[] = {
    {0xB6u, 0xD85Eu, true, false, false},   // x + 7 against 334
    {0xBEu, 0xDD5Au, false, true, false},   // row <= -16
    {0xC2u, 0xDC58u, false, false, false},  // row > 239
    {0xF4u, 0xD83Fu, true, false, true},    // x + 0x17 against 350
    {0xFCu, 0xDD3Bu, false, true, true},    // row <= -64
    {0x100u, 0xDC39u, false, false, true},  // row > 175
};

bool pillar_top_image(gba::GbaBus* b, uint32_t start) {
    return b->read32(start) == 0x465FB5E0u &&
        b->read32(start + 0xB4u) == 0xD85E428Bu &&
        b->read32(start + 0xBCu) == 0xDD5A4294u &&
        b->read32(start + 0xC0u) == 0xDC582CEFu &&
        b->read32(start + 0xF2u) == 0xD83F428Bu &&
        b->read32(start + 0xFAu) == 0xDD3B4294u &&
        b->read32(start + 0xFEu) == 0xDC392CAFu &&
        b->read32(start + 0x194u) == 0xFFFF0000u &&
        b->read32(start + 0x19Cu) == 0x020036E0u &&
        b->read32(start + 0x1B0u) == 0x40002000u &&
        b->read32(start + 0x1B4u) == 0xC000A000u;
}

int pillar_top_branch(uint32_t pc, uint32_t original, uint32_t* out) {
    if (pc < 0x02000000u || pc >= 0x02040000u) return -1;
    const uint32_t live_pc = g_runtime_image_base + (pc - 0x02000000u);
    gba::GbaBus* b = bus();
    if (!b || live_pc < 0x02000100u || live_pc >= 0x0203FE00u) return -1;
    const PillarTopSite* site = nullptr;
    const uint16_t opcode = b->read16(live_pc);
    for (const PillarTopSite& s : kPillarTopSites)
        if (s.opcode == opcode && pillar_top_image(b, live_pc - s.offset)) {
            site = &s;
            break;
        }
    if (!site) return -1;
    const bool x_test = site->x_test, low = site->low, tall = site->tall;
    const View v = view();
    if (!expanded(v) || !field_running(b)) return 0;
    const int row = static_cast<int32_t>(g_cpu.R[4]);
    if (x_test) {
        *out = g_cpu.R[3] + static_cast<uint32_t>(v.left) >
               (tall ? 350u : 334u) + static_cast<uint32_t>(v.left + v.right);
    } else if (low) {
        *out = row <= -(tall ? 64 : 16) - v.top;
    } else {
        *out = row > (tall ? 175 : 239) + v.bottom;
        // Kept: r0 is the piece's x (r1 - 8), r6 its record; the routine
        // writes 0x2000 | row (0xA000 | row when tall) next.
        if (!*out)
            note_placed(g_cpu.R[6], static_cast<int32_t>(g_cpu.R[0]), row,
                        (tall ? 0xA000 : 0x2000) | (row & 0xFF));
    }
    return *out != original;
}

// Field effect sprites (kEffectLeft..kEffectBottom): taken drops the sprite.
// The view's margins widen each test; a sprite kept past the last one is
// placed, so its real position is recorded.
int effect_branch(uint32_t pc, uint32_t original, uint32_t* out_decision) {
    const View v = view();
    gba::GbaBus* b = bus();
    if (!expanded(v) || !b || !field_running(b)) return 0;
    const int r3 = static_cast<int32_t>(g_cpu.R[3]);
    const int left = static_cast<int32_t>(g_cpu.R[5]);
    const int top = static_cast<int32_t>(g_cpu.R[6]);
    bool drop = false;
    switch (pc) {
    case kEffectLeft: drop = r3 < -(v.left + kSlack); break;
    case kEffectRight: drop = left > 239 + v.right + kSlack; break;
    case kEffectTop: drop = r3 <= -(v.top + kSlack); break;
    default: drop = top > 159 + v.bottom + kSlack; break;
    }
    if (pc == kEffectBottom && !drop) note_placed(g_cpu.R[7], left, top);
    *out_decision = drop ? 1u : 0u;
    return *out_decision != original ? 1 : 0;
}

int builder_branch(uint32_t pc, uint32_t original, uint32_t* out_decision) {
    if (pc == kEffectLeft || pc == kEffectRight || pc == kEffectTop ||
        pc == kEffectBottom)
        return effect_branch(pc, original, out_decision);
    if (pc == kMatrixCapacity) {
        g_matrix_pending = {};
        gba::GbaBus* b = bus();
        if (original == 0u && b && expanded(view()) && field_running(b) &&
            (b->read32(g_cpu.R[13] + 24u) & ~1u) == kVineMatrixReturn) {
            // Seven saved words; LR is the last (ROM 0x0801401C..24).
            g_matrix_pending = {b->read32(g_cpu.R[13] + 24u) & ~1u,
                                runtime_call_stack_depth(), g_cpu.R[2]};
        }
        return 0;
    }
    if (pc == kCameraYLimit) return camera_y_limit(original, out_decision);
    if (const int result = jump_pad_range_branch(pc, original, out_decision);
        result >= 0) return result;
    if (const int result = world_range_branch(pc, original, out_decision);
        result >= 0) return result;
    if (const int result = pillar_top_branch(pc, original, out_decision);
        result >= 0) return result;
    if (const int result = tla_sprite_memory_branch(pc, original, out_decision);
        result >= 0) return result;
    if (pc == kDepthClampCharacters) {
        note_rainbow_placement();
        note_jump_pad_placement();
        note_sparkle_placement();
    }
    if (pc == kDepthClampCharacters || pc == kDepthClampAll)
        return depth_branch(original, out_decision);
    if (pc == kAffineX || pc == kAffineY || pc == kAffinePlaced)
        return affine_branch(pc, original, out_decision);
    if (pc == kRopePlaced || pc == kCoilPlaced || pc == kVinePlaced) {
        gba::GbaBus* b = bus();
        if (original == 0u && b && expanded(view()) && field_running(b)) {
            const int anchor = pc == kVinePlaced ? 8 : 4;
            const int x = (static_cast<int32_t>(g_cpu.R[0]) >> 16) - anchor;
            const int y = (static_cast<int32_t>(g_cpu.R[2]) >> 16) - anchor;
            // The vine builder writes 0x2000 | Y, then sets affine only
            // when r7 (rotation) is nonzero (ROM 0x080DABC0..0x080DAC34).
            // Snapshot that outgoing kind, not the previous rotation's:
            // 0087 f30 loses the lagging non-affine section at Y261 once
            // its live record becomes affine again, falling back to Y5.
            const int attr0 = pc == kVinePlaced
                ? 0x2000 | (y & 0xFF) | (g_cpu.R[7] ? 0x100 : 0)
                : -1;
            note_placed(g_cpu.R[5] + 0x14u,
                        x, y, attr0);
        }
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
    // Back at the console's size, people granted from the second sprite page
    // keep it until the game grants them again, so their placements are still
    // recorded for the tile lookup; the console's own tests decide (F12
    // tla_rewind_0117: two people striped after switching to Original).
    const bool wide = expanded(v);
    if (body_x && wide && b && battle_running(b)) {
        *out_decision = static_cast<int32_t>(g_cpu.R[6]) > 239 + v.right + kSlack;
        return *out_decision != original ? 1 : 0;
    }
    if (!b || !field_running(b) || (!wide && !b->obj_extra())) return 0;
    const int x = static_cast<int32_t>(g_cpu.R[6]);
    const int y = static_cast<int32_t>(g_cpu.R[4]);
    // `bgt` skips the object: taken when past the widened edge. A menu
    // sprite's shadow (its record follows the body's, r7 + 0x1C) keeps the
    // console's own test: setting a Djinn put three Djinn shadows at row 210,
    // which the console drops and the widened test drew in the bottom margin
    // (F12 tla_rewind_0035 frames 53..119, bodies priority 0 at -86/-83).
    const bool menu_shadow = shadow_y && menu_sprite(b->read16(g_cpu.R[7] + 8u));
    const bool cull = !wide ? original != 0
                    : body_x ? x > 239 + v.right + kSlack
                             : y > 159 + (menu_shadow ? 0 : v.bottom + kSlack);
    if (!cull && (body_y || shadow_y)) {
        // Every placement is recorded, so a record that moves back above
        // row 160 stops answering with its old Y. The body's X is r6; the
        // shadow's is worked out from the builder's stack as the game does
        // next (0x080221D8 / 0x08022412).
        const uint32_t sp = g_cpu.R[13];
        uint32_t record = g_cpu.R[7];
        int ox = x;
        int placed_attr0 = -1;
        if (body_y) {
            // This hook runs before the builder writes attr0. It sets the
            // affine/double-size bits from this stack word at 0x080222D6
            // or 0x08022510. Remember the outgoing pose, not the previous
            // pose still in the record: OAM can show it a frame later.
            const uint32_t mode = b->read32(sp + (pc == kBodyY1 ? 0x10u : 0xCu));
            placed_attr0 = (b->read16(record + 4u) & 0xFC00u) |
                           ((mode & 0xFFu) << 8) | (static_cast<unsigned>(y) & 0xFFu);
        }
        if (shadow_y) {
            record += 0x1Cu;
            ox = pc == kShadowY1
                     ? (static_cast<int32_t>(b->read32(sp + 0xCu)) >> 16) - 8
                     : (static_cast<int32_t>(b->read32(sp + 8u)) >> 16) -
                           static_cast<int32_t>(b->read32(sp + 0x10u));
        }
        note_placed(record, ox, y, placed_attr0, body_y);
    }
    if (!wide) return 0;
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
    if (!p || !p->uses_tile_grant) return -1;
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

// The wider room bounds the camera rule still uses on each axis while a
// narrowing is kept (CameraHold::kept), whole pixels; false when none is.
bool tla_camera_kept_bounds(bool kept[2], int lo[2], int edge[2]) {
    gba::GbaBus* b = bus();
    bool any = false;
    for (int n = 0; n < 2; ++n) {
        const CameraHold& h = g_hold[n];
        kept[n] = b && expanded(view()) && field_running(b) && h.valid && h.kept &&
                  b->read16(kMapNumber) == h.map;
        lo[n] = h.kept_lo >> 16;
        edge[n] = h.kept_edge >> 16;
        any = any || kept[n];
    }
    return any;
}

// The camera follow 0x0802AD84 keeps the three field layers at [S + 0x108 +
// 0x38 * n] (x, y; 16.16; S = [0x03000020]; F12 0113..0115: n = 0, 1, 2
// match BG3, BG2, BG1's scroll) and redraws only the strip it scrolls onto:
// column writer 0x0802AC90 (bl 0x0802AF16) on an 8-pixel x tile, row writer
// 0x0802ABBC (bl 0x0802AF38/44, 16 cells, the ring's width) on a 16-pixel y
// row; tiles are `lsr #19` (0x0802AEEC/F8). Repaint after a jump:
// tile_ring_redraw.h.
namespace {
gsr::TileRingRedraw g_tile_rings({kFieldStatePointer, 0x108u, 0x38u, 0x0802ABBCu, kMapNumber,
                                  false});
}  // namespace

void tla_widescreen_on_entry(std::uint32_t pc) {
    if (pc == 0x0802AD84u) g_tile_rings.on_follow_entry();
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
    g_matrix_pending = {};
    g_next_call_return = g_runtime_call_return_hook;
    g_runtime_call_return_hook = matrix_return;
    g_next_write = g_runtime_mem_write_override;
    g_runtime_mem_write_override = camera_store;
    g_next_literal = options.thumb_literal_override;
    options.thumb_literal_override = precull_literal;
    g_next_branch = options.conditional_branch_override;
    options.conditional_branch_override = builder_branch;
}
