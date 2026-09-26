// Golden Sun's narrow, evidence-backed expanded-view policy.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace gsr::widescreen {

// This numeric contract mirrors gbarecomp's generic margin-policy flags while
// keeping the game policy pure and independently testable.
inline constexpr unsigned kPillarboxLeft = 1u << 0;
inline constexpr unsigned kPillarboxRight = 1u << 1;
inline constexpr unsigned kPillarboxBoth = kPillarboxLeft | kPillarboxRight;
inline constexpr unsigned kPillarboxTop = 1u << 2;
inline constexpr unsigned kPillarboxBottom = 1u << 3;
inline constexpr unsigned kPillarboxVertical =
    kPillarboxTop | kPillarboxBottom;
inline constexpr unsigned kPillarboxAll =
    kPillarboxBoth | kPillarboxVertical;
inline constexpr std::uint32_t kNativeWidth = 240u;
inline constexpr std::uint32_t kNativeHeight = 160u;

// The 360x240 expanded option is deliberately bounded here even though the
// generic PPU can support a larger horizontal envelope.  These constants are
// presentation policy, not guest camera coordinates: the atlas provider may
// read only the measured 60px/40px margins around the authentic canvas.
inline constexpr std::int32_t kExpandedExtraX = 60;
inline constexpr std::int32_t kExpandedExtraY = 40;

// The guest culls an actor by its OWN reference point, not by the rectangle
// its sprite ends up occupying, so an actor whose point has just left the view
// can still owe it several columns of sprite. Stopping the widened bands
// exactly at the view edge therefore drops those sprites a moment early --
// visible as NPCs and shadows winking out at the right-hand edge. Carry the
// bands ~10% of the view further out (2026-09-06, asked for by the user after
// watching NPCs vanish at the right).
//
// Horizontal only. The vertical band cannot be extended past the view's own
// bottom row: OAM Y is eight bits, so a row below 240+ex_bottom shares its
// byte with a row in the top margin, and admitting more of them would put
// stray sprites there instead. See the OAM read-back in render_scanline_wide.
inline constexpr std::int32_t kExpandedObjSlackX =
    (static_cast<std::int32_t>(kNativeWidth) + 2 * kExpandedExtraX) / 10;
inline constexpr std::int32_t kExpandedWidth =
    static_cast<std::int32_t>(kNativeWidth) + 2 * kExpandedExtraX;
inline constexpr std::int32_t kExpandedHeight =
    static_cast<std::int32_t>(kNativeHeight) + 2 * kExpandedExtraY;

// The exact conditional branches below are the measured guest viewport
// reject decisions. Every route is operand-aware: a guest decision is changed
// only when its measured operand is in the newly visible viewport band.
enum class GoldenSunViewportBranchSite : std::uint8_t {
    B27E = 0,  // BGT to the normal Func_b168 writer path.
    B324,       // BGT to the normal Func_b168 writer path.
    B328,       // BGT to the normal Func_b168 writer path.
    B3D2,       // BGE, Func_b388 lower-X bound.
    B3DC,       // BLE, Func_b388 upper-X bound.
    B3E6,       // BGE, Func_b388 lower-Y bound.
    B3EC,       // BLE, Func_b388 upper-Y bound.
    C6FA,       // BLS, Func_c62c upper-X reject route.
    C702,       // BLE, Func_c62c lower-Y reject route.
    C708,       // BGT, Func_c62c upper-Y reject route.
    Count,
};

struct GoldenSunViewportBranchReview {
    std::uint32_t pc;
    bool forced_decision;
};

inline constexpr std::array<GoldenSunViewportBranchReview,
                             static_cast<std::size_t>(
                                 GoldenSunViewportBranchSite::Count)>
    kGoldenSunViewportBranchReviews{{
        {0x0800B27Eu, false},
        {0x0800B324u, false},
        {0x0800B328u, false},
        {0x0800B3D2u, true},
        {0x0800B3DCu, true},
        {0x0800B3E6u, true},
        {0x0800B3ECu, true},
        {0x0800C6FAu, true},
        {0x0800C702u, false},
        {0x0800C708u, false},
    }};

inline constexpr int golden_sun_viewport_branch_site_index(
    std::uint32_t pc) {
    for (std::size_t i = 0; i < kGoldenSunViewportBranchReviews.size();
         ++i) {
        if (kGoldenSunViewportBranchReviews[i].pc == pc) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

inline constexpr bool golden_sun_expanded_view_active(
    std::uint32_t width, std::uint32_t height, std::uint32_t extra_left,
    std::uint32_t extra_right, std::uint32_t extra_top,
    std::uint32_t extra_bottom) {
    return width == static_cast<std::uint32_t>(kExpandedWidth) &&
           height == static_cast<std::uint32_t>(kExpandedHeight) &&
           extra_left == static_cast<std::uint32_t>(kExpandedExtraX) &&
           extra_right == static_cast<std::uint32_t>(kExpandedExtraX) &&
           extra_top == static_cast<std::uint32_t>(kExpandedExtraY) &&
           extra_bottom == static_cast<std::uint32_t>(kExpandedExtraY);
}

// Func_b388's stock padded actor bounds are X [-32, 272] and Y [-32, 208].
// Keep the lower bounds axis-specific: the guest's X and Y comparisons do not
// share padding, so horizontal expansion must not enlarge the vertical band.
inline constexpr std::uint32_t golden_sun_actor_precull_negative_x_padding(
    std::uint32_t extra_left) {
    return 32u + extra_left;
}

inline constexpr std::uint32_t golden_sun_actor_precull_negative_y_padding(
    std::uint32_t extra_top) {
    return 32u + extra_top;
}

inline constexpr std::uint32_t golden_sun_actor_precull_right_half_limit(
    std::uint32_t extra_right) {
    return 136u + ((extra_right +
                    static_cast<std::uint32_t>(kExpandedObjSlackX) + 1u) / 2u);
}

inline constexpr std::uint32_t golden_sun_actor_precull_bottom_limit(
    std::uint32_t extra_bottom) {
    return 208u + extra_bottom;
}

// Func_c62c's fixed-point bounds. The X operand is already x + 32 in
// unsigned 16.16 form; the Y lower bound is signed -32px in 16.16 form.
inline constexpr std::uint32_t golden_sun_field_list_x_upper_literal(
    std::uint32_t extra_right) {
    return 0x012FFFFEu +
           ((extra_right + static_cast<std::uint32_t>(kExpandedObjSlackX))
            << 16);
}

inline constexpr std::uint32_t golden_sun_field_list_y_lower_literal(
    std::uint32_t extra_top) {
    return 0xFFE00000u - (extra_top << 16);
}

// Returns nonzero only when this exact reviewed branch is enabled for the
// strict expanded geometry and an output slot is available. `compared_value`
// is the measured operand at the branch: r4 for B324 and r6 for B27E/B328;
// the other routes consume the operands documented at their exact PCs.
inline constexpr bool golden_sun_expanded_viewport_branch_override(
    std::uint32_t pc, std::uint32_t original_decision,
    std::uint32_t width, std::uint32_t height, std::uint32_t extra_left,
    std::uint32_t extra_right, std::uint32_t extra_top,
    std::uint32_t extra_bottom, std::int32_t compared_value,
    std::uint32_t* out_decision) {
    if (!out_decision || !golden_sun_expanded_view_active(
            width, height, extra_left, extra_right, extra_top,
            extra_bottom)) {
        return false;
    }
    const int index = golden_sun_viewport_branch_site_index(pc);
    if (index < 0) return false;
    const bool object_x_route = pc == 0x0800B324u;
    // B328 is an alternate writer route. Its widened fall-through can create
    // an unauthenticated positive-Y OAM entry, so preserve the guest decision
    // here. A narrower parent-correlated helper below handles the one proven
    // route without making this generic policy depend on runtime state.
    const bool object_y_route = pc == 0x0800B27Eu;
    if (object_x_route) {
        // B322 compares r4 against 239; admit x=240 out to the view's right
        // edge plus the actor-reference slack above (240..335 as measured).
        if (compared_value < static_cast<std::int32_t>(kNativeWidth) ||
            compared_value >= static_cast<std::int32_t>(
                kNativeWidth + extra_right +
                static_cast<std::uint32_t>(kExpandedObjSlackX)) ||
            !original_decision) {
            return false;
        }
        *out_decision = 0u;
        return true;
    }
    if (object_y_route) {
        // B27C compares r6 against 159; admit only y=160..199.
        if (compared_value < static_cast<std::int32_t>(kNativeHeight) ||
            compared_value >= static_cast<std::int32_t>(
                kNativeHeight + extra_bottom) || !original_decision) {
            return false;
        }
        *out_decision = 0u;
        return true;
    }
    // Func_b388 lower-X/lower-Y: admit only the new lower band. A stock
    // accepted decision is never rewritten.
    if (pc == 0x0800B3D2u) {
        const std::int64_t lower = -static_cast<std::int64_t>(
            golden_sun_actor_precull_negative_x_padding(extra_left));
        const std::int64_t stock_lower = -32;
        if (!original_decision &&
            static_cast<std::int64_t>(compared_value) >= lower &&
            static_cast<std::int64_t>(compared_value) < stock_lower) {
            *out_decision = 1u;
            return true;
        }
        return false;
    }
    if (pc == 0x0800B3E6u) {
        const std::int64_t lower = -static_cast<std::int64_t>(
            golden_sun_actor_precull_negative_y_padding(extra_top));
        const std::int64_t stock_lower = -32;
        if (!original_decision &&
            static_cast<std::int64_t>(compared_value) >= lower &&
            static_cast<std::int64_t>(compared_value) < stock_lower) {
            *out_decision = 1u;
            return true;
        }
        return false;
    }
    // Func_b388 upper-X/upper-Y: admit only the new upper band. A stock
    // accepted decision is never rewritten.
    if (pc == 0x0800B3DCu) {
        const std::int64_t upper = 2 * static_cast<std::int64_t>(
            golden_sun_actor_precull_right_half_limit(extra_right));
        if (!original_decision && compared_value > 272 &&
            static_cast<std::int64_t>(compared_value) <= upper) {
            *out_decision = 1u;
            return true;
        }
        return false;
    }
    if (pc == 0x0800B3ECu) {
        const std::int64_t upper = static_cast<std::int64_t>(
            golden_sun_actor_precull_bottom_limit(extra_bottom));
        if (!original_decision && compared_value > 208 &&
            static_cast<std::int64_t>(compared_value) <= upper) {
            *out_decision = 1u;
            return true;
        }
        return false;
    }
    // Func_c62c upper-X: retain the unsigned lower behavior and widen only
    // the interval above the stock literal.
    if (pc == 0x0800C6FAu) {
        const std::uint32_t operand = static_cast<std::uint32_t>(
            compared_value);
        constexpr std::uint32_t stock_upper = 0x012FFFFEu;
        const std::uint32_t widened_upper =
            golden_sun_field_list_x_upper_literal(extra_right);
        // The lower side of this fixed-point X operand is signed.  Preserve
        // the guest's existing decision, but admit the newly visible
        // negative band as well as the established unsigned upper band.
        const std::int64_t lower =
            -(static_cast<std::int64_t>(extra_left) << 16);
        const bool lower_band =
            static_cast<std::int64_t>(compared_value) >= lower &&
            compared_value < 0;
        const bool upper_band = operand > stock_upper &&
            operand <= widened_upper;
        if (!original_decision && (lower_band || upper_band)) {
            *out_decision = 1u;
            return true;
        }
        return false;
    }
    // Func_c62c lower-Y: this is a direct reject branch, so bypass only a
    // taken reject whose signed operand is above the expanded lower bound.
    if (pc == 0x0800C702u) {
        const std::int64_t lower = static_cast<std::int64_t>(
            static_cast<std::int32_t>(
                golden_sun_field_list_y_lower_literal(extra_top)));
        if (original_decision &&
            static_cast<std::int64_t>(compared_value) > lower) {
            *out_decision = 0u;
            return true;
        }
        return false;
    }
    // Func_c62c upper-Y: bypass a taken reject through the expanded upper
    // boundary; values above it remain rejected.
    if (pc == 0x0800C708u) {
        const std::int64_t upper = (208ll + extra_bottom) << 16;
        if (original_decision &&
            static_cast<std::int64_t>(compared_value) <= upper) {
            *out_decision = 0u;
            return true;
        }
        return false;
    }
    return false;
}

// B328 computes the body's own top-left after B27E's separate +0x0C path.
// A rejected B27E does not reject that body: its computed Y may still be in
// the expanded view (session 20260906_162111: 225 -> 199, down to 222 -> 196).
// Require the same current execution identity, not acceptance of the parent.
// Keep this predicate pure: the
// runner supplies the current execution identity and the exact parent record,
// while diagnostics remain observational and are not required for behavior.
struct GoldenSunObjB328ParentMatch {
    bool valid = false;
    std::uint32_t staging_address = 0;
    std::uint64_t frame = UINT64_MAX;
    std::uint32_t call_depth = 0;
    std::uint32_t call_return_pc = 0;
    std::uint32_t original_decision = 0;
    std::uint32_t final_decision = 0;
    bool overridden = false;
};

inline constexpr bool golden_sun_b328_parent_override(
    bool expanded_view_active, std::uint32_t original_decision,
    std::int32_t operand, std::uint32_t staging_address,
    std::uint64_t frame, std::uint32_t call_depth,
    std::uint32_t call_return_pc,
    const GoldenSunObjB328ParentMatch& parent,
    std::uint32_t* out_decision) {
    if (!out_decision || !expanded_view_active || original_decision != 1u ||
        operand < static_cast<std::int32_t>(kNativeHeight) ||
        operand >= static_cast<std::int32_t>(kNativeHeight + kExpandedExtraY) ||
        !parent.valid || parent.staging_address != staging_address ||
        parent.frame != frame || parent.call_depth != call_depth ||
        parent.call_return_pc != call_return_pc ||
        parent.original_decision != 1u ||
        !((parent.final_decision == 0u && parent.overridden) ||
          (parent.final_decision == 1u && !parent.overridden))) {
        return false;
    }
    *out_decision = 0u;
    return true;
}

// The expanded renderer may reinterpret only the raw OBJ-X values that the
// proven field cull can emit. Mode-2/overworld objects remain hardware-signed
// until their producer/cull route is independently authenticated.
inline constexpr bool golden_sun_field_obj_x_authorized(
    bool authenticated_field, int raw_x, std::uint32_t extra_right) {
    if (!authenticated_field || extra_right == 0u || raw_x < 0x100) {
        return false;
    }
    return raw_x < static_cast<int>(kNativeWidth + extra_right);
}

// Measured OAM shadow range: 128 slots, 8 bytes each, with end exclusive.
inline constexpr std::uint32_t kGoldenSunOamShadowStart = 0x0300347Cu;
inline constexpr std::uint32_t kGoldenSunOamShadowEnd = 0x0300387Cu;
inline constexpr std::uint32_t kGoldenSunOamShadowSlotBytes = 8u;
inline constexpr std::size_t kGoldenSunOamShadowSlotCount =
    (kGoldenSunOamShadowEnd - kGoldenSunOamShadowStart) /
    kGoldenSunOamShadowSlotBytes;
inline constexpr std::uint32_t kGoldenSunOamStart = 0x07000000u;
inline constexpr std::uint32_t kGoldenSunOamBytes = 1024u;

// The game builds its sprite list in more than one place and uploads
// whichever is current, so watching a single address misses whole frames.
// Measured from the upload DMAs themselves (source, destination 0x07000000,
// 1024 bytes): `session_20260906_131954` took 177 of 256 uploads from
// kGoldenSunOamShadowStart and 79 from the address below;
// `session_20260906_113648` split 196/60. A third address, 0x03002000,
// supplied 10 of 256 in `session_20260906_105904`, but that range is reused
// for unrelated purposes (actor records, a transient code image) and is
// deliberately NOT recognised here without evidence of its geometry.
//
// Size and stride are not assumed: the upload is 1024 bytes to OAM, which is
// the hardware's 128 slots of 8 bytes.
inline constexpr std::uint32_t kGoldenSunOamShadowAltStart = 0x03005AE0u;
inline constexpr std::uint32_t kGoldenSunOamShadowAltEnd =
    kGoldenSunOamShadowAltStart + kGoldenSunOamBytes;

// Base address of whichever sprite table `address` falls in, or 0.
inline constexpr std::uint32_t golden_sun_oam_shadow_table_base(
    std::uint32_t address) {
    if (address >= kGoldenSunOamShadowStart && address < kGoldenSunOamShadowEnd)
        return kGoldenSunOamShadowStart;
    if (address >= kGoldenSunOamShadowAltStart &&
        address < kGoldenSunOamShadowAltEnd)
        return kGoldenSunOamShadowAltStart;
    return 0u;
}

// Slot index within whichever table `address` belongs to, or -1. The two
// tables share one slot numbering because only one of them is uploaded per
// frame; a record carries its sprite's exact ATTR0/1/2 as identity, so a
// record left behind by the other table can never be applied to the wrong
// sprite -- it simply fails to match.
inline constexpr int golden_sun_oam_shadow_slot_in_any_table(
    std::uint32_t address, std::uint32_t byte_in_slot) {
    const std::uint32_t base = golden_sun_oam_shadow_table_base(address);
    if (base == 0u) return -1;
    const std::uint32_t offset = address - base;
    if ((offset % kGoldenSunOamShadowSlotBytes) != byte_in_slot) return -1;
    return static_cast<int>(offset / kGoldenSunOamShadowSlotBytes);
}

// This is the only transfer that makes the shadow provenance visible to the
// renderer. The alternate table is observed in uploads but has no
// authenticated writer route, so copying the primary pending array for that
// transfer would attach coordinates to the wrong OAM image. Keep the
// descriptor contract exact so another DMA cannot publish a partially
// updated or unrelated image.
inline constexpr bool golden_sun_obj_provenance_dma_handoff(
    std::uint32_t source, std::uint32_t destination, std::uint32_t bytes,
    std::uint16_t control) {
    return source == kGoldenSunOamShadowStart &&
           destination == kGoldenSunOamStart && bytes == kGoldenSunOamBytes &&
           (control & 0x0060u) == 0u;
}

inline constexpr int golden_sun_oam_shadow_slot(std::uint32_t attr0_address) {
    if (attr0_address < kGoldenSunOamShadowStart ||
        attr0_address >= kGoldenSunOamShadowEnd ||
        (attr0_address - kGoldenSunOamShadowStart) %
                kGoldenSunOamShadowSlotBytes != 0u) {
        return -1;
    }
    return static_cast<int>((attr0_address - kGoldenSunOamShadowStart) /
                            kGoldenSunOamShadowSlotBytes);
}

// The branch PCs are the instructions after the two measured `cmp r6,#159`
// instructions.  The branch body owns the corresponding OAM ATTR0 store:
// B27E writes the second word and B328 writes the first word of the slot.
inline constexpr std::uint32_t golden_sun_obj_y_provenance_offset(
    std::uint32_t branch_pc) {
    return branch_pc == 0x0800B27Eu ? 16u
         : branch_pc == 0x0800B328u ? 4u
                                    : UINT32_MAX;
}

// B27E and B328 are BGT rejects. The fall-through path writes ATTR0, whether
// the guest condition was originally false or the expanded policy forced it
// false. Keep both paths on the same logical-Y provenance route.
inline constexpr bool golden_sun_obj_y_branch_writes_oam(
    std::uint32_t branch_pc, std::uint32_t original_decision,
    bool overridden, std::uint32_t resulting_decision) {
    if (branch_pc != 0x0800B27Eu && branch_pc != 0x0800B328u) return false;
    return (overridden ? resulting_decision : original_decision) == 0u;
}

inline constexpr bool golden_sun_obj_y_provenance_address(
    std::uint32_t branch_pc, std::uint32_t r7,
    std::uint32_t* out_address) {
    const std::uint32_t offset =
        golden_sun_obj_y_provenance_offset(branch_pc);
    if (!out_address || offset == UINT32_MAX || r7 > UINT32_MAX - offset)
        return false;
    *out_address = r7 + offset;
    return true;
}

// Func_b168's widened writer stores ATTR1 at R7+6 on the B324/B328 route,
// immediately after ATTR0 at R7+4. Resolve the owning ATTR0 base so the
// address can be mapped to an aligned OAM slot while retaining X from r4.
// Golden Sun hides an unused sprite by moving it off the visible screen
// rather than clearing its enable bit, and it parks every one of them at a
// SINGLE position: raw `attr1 & 0x1FF == 192`, `attr0 & 0xFF == 192`, which
// resolves to (192, -64) -- just above the screen. Measured 2026-09-11 over
// logs/objrec_20260911_101026: 19,264 of 22,528 published OAM slots across 180
// field frames sit on exactly that pair, 85.5% of the table and about 107 of
// the 128 live entries every frame. It is one sentinel, not a spread, which is
// what makes an exact test safe.
//
// This matters only for the expanded view. -64 is off-screen on hardware, but
// it is INSIDE a 40-row top margin, so without this test the parked dummies
// rain into the widened picture.
inline constexpr unsigned kGoldenSunObjParkRawX = 192u;
inline constexpr unsigned kGoldenSunObjParkRawY = 192u;

inline constexpr bool golden_sun_obj_is_parked(std::uint16_t attr0,
                                               std::uint16_t attr1) {
    return (attr1 & 0x01FFu) == kGoldenSunObjParkRawX &&
           (attr0 & 0x00FFu) == kGoldenSunObjParkRawY;
}

inline constexpr bool golden_sun_obj_x_provenance_address(
    std::uint32_t branch_pc, std::uint32_t r7,
    std::uint32_t* out_attr0_address) {
    if (!out_attr0_address || branch_pc != 0x0800B324u ||
        r7 > UINT32_MAX - 4u) {
        return false;
    }
    *out_attr0_address = r7 + 4u;
    return true;
}

enum class GoldenSunObjYResolution : std::uint8_t {
    Canonical = 0,
    SignedProvenance,
};

// Signed writer provenance is the only expanded-Y override. Without a fresh
// matching record, preserve canonical GBA 8-bit Y wrapping: e.g. logical
// Y=-65 truncates to raw 191 and must stay above the canvas, not become +191.
inline constexpr GoldenSunObjYResolution golden_sun_obj_y_resolution(
    bool signed_provenance_matches) {
    if (signed_provenance_matches) {
        return GoldenSunObjYResolution::SignedProvenance;
    }
    return GoldenSunObjYResolution::Canonical;
}

// Raw OBJ Y values are ambiguous whenever their signed logical counterpart
// crosses the 8-bit hardware boundary: values >=160 can be widened positive
// rows, while negative logical rows can truncate below 160 and wrap positive
// in an unprovenanced fallback. Keep this predicate diagnostic-only; it does
// not select a rendering path.
inline constexpr bool golden_sun_obj_y_alias_candidate(int raw_y,
                                                        int logical_y) {
    return (raw_y >= 160 && logical_y >= 160) ||
           (raw_y < 160 && logical_y < 0);
}

// One measured NPC crossed the 8-bit OBJ-Y boundary as 162/-94, 161/-95,
// 160/-96, then 159/+159.  Keep that evidence as a pure, fail-closed state
// machine.  It is intentionally independent of the guest/OAM state and is
// applied only by the final Expanded renderer seam. The final-Y provider may
// call this once per scanline, so the state records the last raw/canonical
// result to distinguish an identical same-frame sample from a mutation.
struct GoldenSunObjYEdgeAliasState {
    bool valid = false;
    std::uint8_t count = 0;
    int slot = -1;
    std::uint32_t target_address = 0;
    std::uint64_t auth_epoch = 0;
    std::uint64_t last_frame = UINT64_MAX;
    std::uint16_t attr0_non_y = 0;
    std::uint16_t attr1 = 0;
    std::uint16_t attr2 = 0;
    int last_raw_y = 0;
    int last_canonical_y = 0;
};

struct GoldenSunObjYEdgeAliasSample {
    bool expanded_active = false;
    bool sprite_active = false;
    bool exact_provenance = false;
    int slot = -1;
    std::uint32_t target_address = 0;
    std::uint64_t auth_epoch = 0;
    std::uint64_t frame = UINT64_MAX;
    int raw_y = 0;
    int canonical_y = 0;
    std::uint16_t attr0 = 0;
    std::uint16_t attr1 = 0;
    std::uint16_t attr2 = 0;
};

struct GoldenSunObjYEdgeAliasStep {
    GoldenSunObjYEdgeAliasState state{};
    bool activated = false;
};

inline constexpr std::uint16_t golden_sun_obj_y_edge_alias_attr0_non_y(
    std::uint16_t attr0) {
    return static_cast<std::uint16_t>(attr0 & 0xFF00u);
}

inline constexpr bool golden_sun_obj_y_edge_alias_identity_matches(
    const GoldenSunObjYEdgeAliasState& state,
    const GoldenSunObjYEdgeAliasSample& sample) {
    return state.valid && state.slot == sample.slot &&
           state.target_address == sample.target_address &&
           state.auth_epoch == sample.auth_epoch &&
           state.attr0_non_y == golden_sun_obj_y_edge_alias_attr0_non_y(
               sample.attr0) &&
           state.attr1 == sample.attr1 && state.attr2 == sample.attr2;
}

inline constexpr bool golden_sun_obj_y_edge_alias_frame_contiguous(
    std::uint64_t previous, std::uint64_t current) {
    // The captured transition advances by 1, 2, 1 guest frames.  A larger
    // gap is a different observation and must not complete the latch.
    return current > previous && current - previous <= 2u;
}

inline constexpr GoldenSunObjYEdgeAliasStep golden_sun_obj_y_edge_alias_step(
    const GoldenSunObjYEdgeAliasState& previous,
    const GoldenSunObjYEdgeAliasSample& sample) {
    GoldenSunObjYEdgeAliasStep result{};
    result.state = previous;
    const auto reset = [&]() {
        result.state = {};
    };
    if (!sample.expanded_active || !sample.sprite_active ||
        sample.exact_provenance || sample.slot < 0 ||
        sample.target_address == 0u || sample.frame == UINT64_MAX) {
        reset();
        return result;
    }

    constexpr int kFirstRaw = 162;
    constexpr int kFirstCanonical = -94;
    if (!previous.valid) {
        if (sample.raw_y != kFirstRaw || sample.canonical_y != kFirstCanonical)
            return result;
        result.state.valid = true;
        result.state.count = 1u;
        result.state.slot = sample.slot;
        result.state.target_address = sample.target_address;
        result.state.auth_epoch = sample.auth_epoch;
        result.state.last_frame = sample.frame;
        result.state.last_raw_y = sample.raw_y;
        result.state.last_canonical_y = sample.canonical_y;
        result.state.attr0_non_y =
            golden_sun_obj_y_edge_alias_attr0_non_y(sample.attr0);
        result.state.attr1 = sample.attr1;
        result.state.attr2 = sample.attr2;
        return result;
    }

    if (!golden_sun_obj_y_edge_alias_identity_matches(previous, sample) ||
        (sample.frame == previous.last_frame &&
         (sample.raw_y != previous.last_raw_y ||
          sample.canonical_y != previous.last_canonical_y)) ||
        (sample.frame != previous.last_frame &&
         !golden_sun_obj_y_edge_alias_frame_contiguous(
             previous.last_frame, sample.frame))) {
        reset();
        return result;
    }

    // The final-Y provider runs once per scanline. Repeated observations of
    // one frame are the same sample, not a frame-gap violation.
    if (sample.frame == previous.last_frame) {
        return result;
    }

    const int expected_raw = previous.count == 1u ? 161
        : previous.count == 2u ? 160
        : previous.count == 3u ? 159 : -1;
    const int expected_canonical = previous.count == 1u ? -95
        : previous.count == 2u ? -96
        : previous.count == 3u ? 159 : 0;
    if (sample.raw_y != expected_raw ||
        sample.canonical_y != expected_canonical) {
        reset();
        return result;
    }
    if (previous.count == 3u) {
        result.state = {};
        result.activated = true;
        return result;
    }
    result.state.last_frame = sample.frame;
    result.state.last_raw_y = sample.raw_y;
    result.state.last_canonical_y = sample.canonical_y;
    ++result.state.count;
    return result;
}

// Historical cadence predicate retained for focused diagnostics/tests. The
// runtime's visible provenance validity is now bounded by exact OAM identity,
// not by this short frame-age window.
inline constexpr bool golden_sun_obj_provenance_frame_fresh(
    std::uint64_t writer_frame, std::uint64_t render_frame) {
    if (writer_frame == UINT64_MAX || render_frame < writer_frame) {
        return false;
    }
    return render_frame - writer_frame <= 2u;
}

// A signed placement may outlive the producer-frame cadence, but it must
// never outlive the exact OAM image it describes.  The three attributes are
// the complete identity consumed by the OBJ renderer (ATTR0/1/2); a changed
// or reused slot therefore fails closed and keeps canonical GBA wrapping.
inline constexpr bool golden_sun_obj_provenance_attrs_match(
    bool expected_valid, std::uint16_t expected_attr0,
    std::uint16_t expected_attr1, std::uint16_t expected_attr2,
    std::uint16_t attr0, std::uint16_t attr1, std::uint16_t attr2) {
    return expected_valid && expected_attr0 == attr0 &&
           expected_attr1 == attr1 && expected_attr2 == attr2;
}

inline constexpr std::uint16_t kDispcntModeMask = 0x0007u;
inline constexpr std::uint16_t kDispcntMode0 = 0x0000u;
inline constexpr std::uint16_t kDispcntMode2 = 0x0002u;
inline constexpr std::uint16_t kDispcntForcedBlank = 0x0080u;
inline constexpr std::uint16_t kDispcntBg1 = 0x0200u;
inline constexpr std::uint16_t kDispcntBg2 = 0x0400u;
inline constexpr std::uint16_t kDispcntBg3 = 0x0800u;
inline constexpr std::uint16_t kDispcntObj = 0x1000u;
inline constexpr std::uint16_t kDispcntWindows = 0xE000u;

inline constexpr std::uint16_t kBgcntWrap = 0x2000u;
inline constexpr std::uint16_t kBgcntSizeMask = 0xC000u;
inline constexpr std::uint16_t kBgcntSize256 = 0x0000u;
inline constexpr std::uint16_t kBgcntSize512 = 0x8000u;

// Mode 0's BG0 is measured as a screen-space/UI layer in State1. Keep it
// inside the authentic 240-pixel canvas when field margins are widened.
inline constexpr bool golden_sun_suppress_bg0_margin(
    int bg, int output_x, std::uint32_t extra_left,
    std::uint32_t extra_right) {
    if (bg != 0) return false;
    const int first = static_cast<int>(extra_left);
    const int last = first + static_cast<int>(kNativeWidth);
    // Keep the right-margin argument in the contract so the helper remains
    // correct for asymmetric views; the output-side check is simply outside
    // the central native interval on either side.
    (void)extra_right;
    return output_x < first || output_x >= last;
}

// Full expanded-view form. BG0 is screen-space/UI composition in the measured
// field scene, so it remains inside the centered native rectangle on both
// axes. Keep the four-argument overload above for the existing horizontal
// runner seam until the generic runtime carries Y margins as well.
inline constexpr bool golden_sun_suppress_bg0_margin(
    int bg, int output_x, int output_y, std::uint32_t extra_left,
    std::uint32_t extra_right, std::uint32_t extra_top,
    std::uint32_t extra_bottom) {
    if (bg != 0) return false;
    const int first_x = static_cast<int>(extra_left);
    const int last_x = first_x + static_cast<int>(kNativeWidth);
    const int first_y = static_cast<int>(extra_top);
    const int last_y = first_y + static_cast<int>(kNativeHeight);
    (void)extra_right;
    (void)extra_bottom;
    return output_x < first_x || output_x >= last_x ||
           output_y < first_y || output_y >= last_y;
}

// Conservative fallback for callers without the measured atlas provider:
// keep every untrusted field sample inside the authentic 240px canvas. The
// runtime adapter uses the pure lookup below when live field tables are valid.
inline constexpr bool golden_sun_suppress_mode0_field_margin(
    int bg, int output_x, std::uint32_t extra_left,
    std::uint32_t extra_right) {
    if (bg < 1 || bg > 3) return false;
    const int native_first = static_cast<int>(extra_left);
    const int native_last = native_first + static_cast<int>(kNativeWidth);
    // Keep the right-margin argument in the contract for symmetric/asymmetric
    // view callers; the native interval itself defines both margin sides.
    (void)extra_right;
    return output_x < native_first || output_x >= native_last;
}

inline constexpr std::uint16_t read_io16(const std::uint8_t* io,
                                          std::size_t offset) {
    return static_cast<std::uint16_t>(io[offset]) |
           static_cast<std::uint16_t>(io[offset + 1u] << 8);
}

// Return zero only for measured field raster configurations. Mode 2 is the
// overworld signature: both affine layers enabled, both layers wrapping
// 512x512 maps, and no WIN0/WIN1/OBJ-window. Mode 0 is the town/dungeon
// signature: BG1/BG2/BG3 are enabled, all three layer scroll pairs match, and
// no WIN0/WIN1/OBJ-window is active. The scroll equality is only a scene
// classifier; it does not synthesize tiles or change guest camera/culling.
// Mode 1 battle and all unknown configurations remain pillarboxed.
enum class GoldenSunWidePolicyReason : std::uint8_t {
    AuthorizedMode2,
    AuthorizedMode0,
    AuthorizedMode0SplitScroll,
    MissingIo,
    ForcedBlank,
    WindowControl,
    UnsupportedMode,
    Mode2Layers,
    Mode2Geometry,
    Mode0Layers,
    Mode0Geometry,
    Mode0ScrollMismatch,
    Mode0SplitLayers,
    Mode0SplitGeometry,
    Mode0SplitScrollMismatch,
};

inline constexpr bool golden_sun_wide_policy_authorized(
    GoldenSunWidePolicyReason reason) {
    return reason == GoldenSunWidePolicyReason::AuthorizedMode2 ||
           reason == GoldenSunWidePolicyReason::AuthorizedMode0 ||
           reason == GoldenSunWidePolicyReason::AuthorizedMode0SplitScroll;
}

inline constexpr GoldenSunWidePolicyReason golden_sun_wide_margin_policy_reason(
    std::uint16_t dispcnt, const std::uint8_t* io) {
    if (!io) return GoldenSunWidePolicyReason::MissingIo;
    if ((dispcnt & kDispcntForcedBlank) != 0)
        return GoldenSunWidePolicyReason::ForcedBlank;
    if ((dispcnt & kDispcntWindows) != 0)
        return GoldenSunWidePolicyReason::WindowControl;

    const std::uint16_t mode = dispcnt & kDispcntModeMask;
    if (mode == kDispcntMode2) {
        const std::uint16_t required_layers = kDispcntBg2 | kDispcntBg3;
        if ((dispcnt & required_layers) != required_layers)
            return GoldenSunWidePolicyReason::Mode2Layers;
        const std::uint16_t required = kBgcntWrap | kBgcntSize512;
        const std::uint16_t bg2cnt = read_io16(io, 0x0Cu);
        const std::uint16_t bg3cnt = read_io16(io, 0x0Eu);
        if ((bg2cnt & (kBgcntWrap | kBgcntSizeMask)) != required ||
            (bg3cnt & (kBgcntWrap | kBgcntSizeMask)) != required)
            return GoldenSunWidePolicyReason::Mode2Geometry;
        return GoldenSunWidePolicyReason::AuthorizedMode2;
    }

    if (mode == kDispcntMode0) {
        constexpr std::uint16_t field_layers =
            kDispcntBg1 | kDispcntBg2 | kDispcntBg3;
        if ((dispcnt & field_layers) != field_layers)
            return GoldenSunWidePolicyReason::Mode0Layers;
        // State1 measures all three field layers as 256x256 text BGs. Do not
        // widen a different regular-BG geometry under the same scroll pattern.
        const std::uint16_t bg1cnt = read_io16(io, 0x0Au);
        const std::uint16_t bg2cnt = read_io16(io, 0x0Cu);
        const std::uint16_t bg3cnt = read_io16(io, 0x0Eu);
        if ((bg1cnt & kBgcntSizeMask) != kBgcntSize256 ||
            (bg2cnt & kBgcntSizeMask) != kBgcntSize256 ||
            (bg3cnt & kBgcntSizeMask) != kBgcntSize256)
            return GoldenSunWidePolicyReason::Mode0Geometry;
        // HOFS/VOFS are 9-bit GBA registers. Compare the effective hardware
        // scroll, not unused high bits retained by a host IO snapshot.
        const auto read_scroll = [&](std::size_t offset) {
            return static_cast<std::uint16_t>(read_io16(io, offset) & 0x01FFu);
        };
        const std::uint16_t bg1_hofs = read_scroll(0x14u);
        const std::uint16_t bg1_vofs = read_scroll(0x16u);
        const std::uint16_t bg2_hofs = read_scroll(0x18u);
        const std::uint16_t bg2_vofs = read_scroll(0x1Au);
        const std::uint16_t bg3_hofs = read_scroll(0x1Cu);
        const std::uint16_t bg3_vofs = read_scroll(0x1Eu);
        if (bg1_hofs != bg2_hofs || bg1_hofs != bg3_hofs ||
            bg1_vofs != bg2_vofs || bg1_vofs != bg3_vofs)
            return GoldenSunWidePolicyReason::Mode0ScrollMismatch;
        return GoldenSunWidePolicyReason::AuthorizedMode0;
    }

    return GoldenSunWidePolicyReason::UnsupportedMode;
}

// McCoy Palace's measured Mode-0 class is deliberately distinct from the
// equal-scroll field class above. The exact BG CNT values and raw scroll
// deltas are part of the authentication: transitions that merely happen to
// match one relation must not become a Palace policy. The runner additionally
// requires a complete clean frame before using this result.
inline constexpr std::uint16_t kGoldenSunPalaceBg1Cnt = 0x0709u;
inline constexpr std::uint16_t kGoldenSunPalaceBg2Cnt = 0x060Au;
inline constexpr std::uint16_t kGoldenSunPalaceBg3Cnt = 0x0503u;

// Exact payload-free fingerprints measured for McCoy Palace's restored
// split-scroll tables.  A fingerprint is necessary but not sufficient: the
// runner also requires a complete clean split-scroll frame and invalidates the
// authorization on any subsequent table write.
inline constexpr std::uint32_t kGoldenSunPalaceMapCrc = 0x0aef4a71u;
inline constexpr std::uint32_t kGoldenSunPalaceRawCrc = 0x11020c66u;
// Bilibin's measured pair (0x7ac260c4/0x060ecd2c) has no independently
// measured raw 0xffff/filler ownership profile. Keep it fail-closed until an
// exact profile can be established; it must not inherit Palace authorization.
// The measured Palace fingerprint is dominated by metatile 0x026 outside
// the active room (14,068/16,384 cells). Treat that fill as out-of-room in
// the split-scroll provider; all other IDs remain subject to the raw-entry
// sentinel and finite atlas checks below.
inline constexpr std::uint16_t kGoldenSunPalaceNoMapId = 0x026u;

inline constexpr bool golden_sun_palace_table_fingerprint_matches(
    std::uint32_t map_crc, std::uint32_t raw_crc) {
    return map_crc == kGoldenSunPalaceMapCrc &&
           raw_crc == kGoldenSunPalaceRawCrc;
}

inline constexpr bool golden_sun_palace_table_authorization_allowed(
    bool complete_clean_split_scroll, bool table_written,
    GoldenSunWidePolicyReason reason, std::uint32_t map_crc,
    std::uint32_t raw_crc) {
    return complete_clean_split_scroll && !table_written &&
           reason == GoldenSunWidePolicyReason::AuthorizedMode0SplitScroll &&
           golden_sun_palace_table_fingerprint_matches(map_crc, raw_crc);
}

inline constexpr GoldenSunWidePolicyReason
golden_sun_mode0_split_scroll_policy_reason(
    std::uint16_t dispcnt, const std::uint8_t* io) {
    if (!io) return GoldenSunWidePolicyReason::MissingIo;
    if ((dispcnt & kDispcntForcedBlank) != 0)
        return GoldenSunWidePolicyReason::ForcedBlank;
    if ((dispcnt & kDispcntWindows) != 0)
        return GoldenSunWidePolicyReason::WindowControl;
    if ((dispcnt & kDispcntModeMask) != kDispcntMode0)
        return GoldenSunWidePolicyReason::UnsupportedMode;

    constexpr std::uint16_t field_layers =
        kDispcntBg1 | kDispcntBg2 | kDispcntBg3;
    if ((dispcnt & field_layers) != field_layers)
        return GoldenSunWidePolicyReason::Mode0SplitLayers;
    if (read_io16(io, 0x0Au) != kGoldenSunPalaceBg1Cnt ||
        read_io16(io, 0x0Cu) != kGoldenSunPalaceBg2Cnt ||
        read_io16(io, 0x0Eu) != kGoldenSunPalaceBg3Cnt)
        return GoldenSunWidePolicyReason::Mode0SplitGeometry;

    const std::uint16_t bg1_hofs = read_io16(io, 0x14u);
    const std::uint16_t bg2_hofs = read_io16(io, 0x18u);
    const std::uint16_t bg3_hofs = read_io16(io, 0x1Cu);
    const std::uint16_t bg1_vofs = read_io16(io, 0x16u);
    const std::uint16_t bg2_vofs = read_io16(io, 0x1Au);
    const std::uint16_t bg3_vofs = read_io16(io, 0x1Eu);
    const bool effective_h_equal =
        (bg1_hofs & 0x01FFu) == (bg2_hofs & 0x01FFu) &&
        (bg1_hofs & 0x01FFu) == (bg3_hofs & 0x01FFu);
    const bool raw_h_split =
        bg1_hofs == bg3_hofs &&
        static_cast<std::uint16_t>(bg2_hofs - bg1_hofs) == 0x0200u;
    const bool raw_v_split =
        bg2_vofs == bg3_vofs &&
        static_cast<std::uint16_t>(bg1_vofs - bg2_vofs) == 0x0180u;
    if (!effective_h_equal || !raw_h_split || !raw_v_split)
        return GoldenSunWidePolicyReason::Mode0SplitScrollMismatch;
    return GoldenSunWidePolicyReason::AuthorizedMode0SplitScroll;
}

inline constexpr bool golden_sun_mode0_split_scroll_authorized(
    std::uint16_t dispcnt, const std::uint8_t* io) {
    return golden_sun_mode0_split_scroll_policy_reason(dispcnt, io) ==
        GoldenSunWidePolicyReason::AuthorizedMode0SplitScroll;
}

// Authentication is delayed by one full rendered frame. A row that fails the
// exact split-scroll predicate immediately clears the candidate, so a short
// transition cannot authorize the following frame.
struct GoldenSunMode0SplitScrollFrame {
    static constexpr std::uint64_t kUnsetFrame = UINT64_MAX;

    void reset() {
        frame = kUnsetFrame;
        clean_rows = 0;
        complete = false;
        authorized = false;
    }

    // Returns whether the row may use a previously completed clean frame.
    // `expected_rows` is the authentic guest raster height. Host-only signed
    // margin rows are synthesized after these rows and are not observations.
    bool observe(std::uint64_t sample_frame, bool clean_row,
                 std::uint32_t expected_rows) {
        if (expected_rows == 0u) {
            reset();
            return false;
        }
        if (frame != sample_frame) {
            const bool previous_complete = complete;
            frame = sample_frame;
            clean_rows = 0;
            complete = false;
            authorized = previous_complete;
        }
        if (!clean_row) {
            clean_rows = 0;
            complete = false;
            authorized = false;
            return false;
        }
        if (clean_rows < expected_rows) ++clean_rows;
        if (clean_rows >= expected_rows) complete = true;
        return authorized;
    }

    std::uint64_t frame = kUnsetFrame;
    std::uint32_t clean_rows = 0;
    bool complete = false;
    bool authorized = false;
};

// Field maps whose BG1/BG2/BG3 scroll registers disagree (see
// Mode0ScrollMismatch above) are not McCoy Palace's exact fingerprint, and no
// per-room offset between the layers is proven stable (a prior attempt to
// live-measure and reconstruct one produced visibly misaligned margins and
// was removed). Rather than guess a cross-layer relationship, only the
// field's own terrain/gameplay layer is widened, using its own raw scroll
// directly -- exactly the technique the already-correct equal-scroll path
// uses for every layer when they all agree. The other two layers contribute
// nothing outside the native 240x160 canvas.
//
// The terrain layer is the lowest-indexed regular BG among BG1..BG3 that
// DISPCNT currently reports enabled, so the choice is derived from live
// layer-enable state each frame rather than a fixed literal. Every measured
// field scene requires BG1/BG2/BG3 all enabled to reach this classification
// (see the Mode0Layers gate above), so this resolves to BG1 in every case
// proven so far; a future room where that is not true would need its own
// measurement before this helper could return anything else.
inline constexpr int golden_sun_field_terrain_bg(std::uint16_t dispcnt) {
    if ((dispcnt & kDispcntBg1) != 0) return 1;
    if ((dispcnt & kDispcntBg2) != 0) return 2;
    if ((dispcnt & kDispcntBg3) != 0) return 3;
    return 0;
}

inline constexpr unsigned golden_sun_wide_margin_policy(
    std::uint16_t dispcnt, const std::uint8_t* io) {
    return golden_sun_wide_policy_authorized(
        golden_sun_wide_margin_policy_reason(dispcnt, io)) ? 0u : kPillarboxAll;
}

// Golden Sun's Mode 0 field renderer keeps a 32x32 tile ring in VRAM, but its
// own row/column writers resolve the real map through these EWRAM tables:
//   0x02010000: 128x128 u32 metatile IDs (low 12 bits)
//   0x02020000: 4096 entries, each four u16 8x8 tile entries (2x2)
// The helper is deliberately a pure read-only lookup. `ewram` is the live
// EWRAM image beginning at 0x02000000; no guest state or camera is changed.
// The row/column writers copy the raw u16 entry verbatim.  0xffff is not a
// usable field tile: it selects tile 0x3ff with palette bank 0xf and exposes
// the stale VRAM tile seen in the widescreen margin.  Do not reject map IDs
// based on their value; the raw entry is the measured unavailable sentinel.
inline constexpr std::uint16_t kGoldenSunFieldUnavailableTile = 0xffffu;

// Optional metadata kept separate from the rendered entry so game-owned
// presentation policy can identify an unavailable raw entry (notably 0xffff)
// without changing the lookup result.
struct GoldenSunFieldTilemapMetadata {
    std::uint16_t map_id = 0;
    std::uint16_t raw_entry = 0;
    std::int32_t tile_x = 0;
    std::int32_t tile_y = 0;
    std::uint32_t map_x = 0;
    std::uint32_t map_y = 0;
    bool has_raw_entry = false;
};

inline constexpr bool golden_sun_palace_out_of_room(
    const GoldenSunFieldTilemapMetadata& metadata) {
    return metadata.has_raw_entry &&
           metadata.map_id == kGoldenSunPalaceNoMapId;
}
// State1's complete 128x128 field table proves metatile 0x017/raw 0xF200 is
// the un-authored fill: it occupies 13,531/16,384 cells and forms the solid
// rows/columns outside the active 32x32 room. Metatile 0x003 is authored room
// data and must not be suppressed. The equal-scroll field uses BG3 as the
// coordinate-level boundary and suppresses BG1/BG2 there too; the separate
// split-scroll Palace path does not use BG3 as a cross-layer boundary.
inline constexpr std::uint16_t kGoldenSunFieldNoMapId = 0x017u;
inline constexpr std::uint16_t kGoldenSunFieldNoMapTile = 0xF200u;

// Bilibin's own [wide-field-map] table census (session 20260825_161027,
// frame=519890, crc=7ac260c4) measured top=017:13531,01a:1195,003:147,067:69.
// 0x01a is the second-largest occupant after the known 0x017 fill, and its
// occurrences visually correlate with the repeating menu/font-tile leak seen
// in that same session's screenshot. Its exact paired raw value was not
// separately measured, so this rejects the metatile ID alone rather than an
// (id, raw) pair. 0x003 (authored room data, per above) and 0x067 (only 69
// occurrences — too sparse to distinguish fill from legitimate scattered
// content) are deliberately left unsuppressed.
inline constexpr std::uint16_t kGoldenSunFieldNoMapId2 = 0x01Au;

inline constexpr bool golden_sun_field_bg3_no_map(
    const GoldenSunFieldTilemapMetadata& metadata) {
    if (!metadata.has_raw_entry) return false;
    if (metadata.map_id == kGoldenSunFieldNoMapId &&
        metadata.raw_entry == kGoldenSunFieldNoMapTile) {
        return true;
    }
    return metadata.map_id == kGoldenSunFieldNoMapId2;
}

// A raw 0xffff entry is also unavailable even though the map/atlas lookup
// reached a valid raw-table slot.  Keep this separate from malformed-source
// cases: the provider may repair only an authenticated BG3 margin sample whose
// metadata identifies one of these measured unavailable entries.
inline constexpr bool golden_sun_field_bg3_unavailable(
    const GoldenSunFieldTilemapMetadata& metadata) {
    return metadata.has_raw_entry &&
        (metadata.raw_entry == kGoldenSunFieldUnavailableTile ||
         golden_sun_field_bg3_no_map(metadata));
}

// The same source-validity rule applies to every field layer.  BG3 remains
// the cross-layer boundary oracle in the runner, but a BG1/BG2 lookup can also
// land on an unavailable atlas cell while BG3 at that coordinate is authored.
// Keep this predicate separate from the BG3-named compatibility helper so a
// caller cannot accidentally treat layer-local invalid data as valid terrain.
inline constexpr bool golden_sun_field_atlas_unavailable(
    const GoldenSunFieldTilemapMetadata& metadata) {
    return golden_sun_field_bg3_unavailable(metadata);
}

// Resolve a BG3 margin sample by walking toward the authentic 240px canvas in
// 8px steps when the atlas returned one of the measured unavailable entries.
// `resolve` is a presentation-only callback supplied by the caller; it must
// return the atlas entry and metadata for the requested hardware X.  Native
// edge coordinates are included so a completely dummy 24px margin can reuse
// the first authored tile on the canvas.  The helper never writes guest RAM.
template <typename Resolve>
inline bool golden_sun_field_bg3_inward_entry(
    int hw_x, std::uint32_t native_width, std::uint32_t margin,
    Resolve&& resolve, std::uint16_t* out_entry,
    GoldenSunFieldTilemapMetadata* out_metadata = nullptr) {
    if (!out_entry || native_width == 0u || margin == 0u) return false;

    const bool left = hw_x < 0;
    if (!left && hw_x < static_cast<int>(native_width)) return false;
    const int limit = static_cast<int>(margin);
    if ((left && hw_x < -limit) ||
        (!left && hw_x >= static_cast<int>(native_width) + limit)) {
        return false;
    }

    const int direction = left ? 1 : -1;
    const int first = hw_x;
    // Continue through the authentic row after crossing the native edge. A
    // dummy can occupy the edge tile itself, so clamping there would repeat
    // the same unavailable sample forever and leave the margin transparent.
    // The last reachable native candidate is 8px before the opposite edge:
    // left: 0,8,...,native_width-8; right: native_width-1,...,7.
    const int max_distance = limit + static_cast<int>(native_width) - 8;
    for (int distance = 8; distance <= max_distance; distance += 8) {
        int candidate = first + direction * distance;
        // Keep the remaining margin samples in the resolver's search order;
        // after crossing the margin, reject any non-native coordinate.
        if (distance > limit &&
            (candidate < 0 || candidate >= static_cast<int>(native_width))) {
            continue;
        }

        std::uint16_t entry = 0;
        GoldenSunFieldTilemapMetadata metadata;
        if (resolve(candidate, &entry, &metadata) &&
            !golden_sun_field_bg3_unavailable(metadata)) {
            *out_entry = entry;
            if (out_metadata) *out_metadata = metadata;
            return true;
        }
    }
    return false;
}

// Vertical counterpart to the horizontal BG3 repair above. The callback is
// given a candidate screen Y and must resolve the same hardware X. It walks
// inward in 8px steps, crossing the native edge so an unavailable top/bottom
// margin can reuse an authored native row. No guest memory is written.
template <typename Resolve>
inline bool golden_sun_field_bg3_inward_entry_vertical(
    int hw_x, int screen_y, std::uint32_t native_height,
    std::uint32_t margin, Resolve&& resolve, std::uint16_t* out_entry,
    GoldenSunFieldTilemapMetadata* out_metadata = nullptr) {
    if (!out_entry || native_height == 0u || margin == 0u) return false;

    const bool top = screen_y < 0;
    if (!top && screen_y < static_cast<int>(native_height)) return false;
    const int limit = static_cast<int>(margin);
    if ((top && screen_y < -limit) ||
        (!top && screen_y >= static_cast<int>(native_height) + limit)) {
        return false;
    }

    const int direction = top ? 1 : -1;
    const int first = screen_y;
    const int max_distance = limit + static_cast<int>(native_height) - 8;
    for (int distance = 8; distance <= max_distance; distance += 8) {
        const int candidate = first + direction * distance;
        if (distance > limit &&
            (candidate < 0 || candidate >= static_cast<int>(native_height))) {
            continue;
        }

        std::uint16_t entry = 0;
        GoldenSunFieldTilemapMetadata metadata;
        if (resolve(candidate, &entry, &metadata) &&
            !golden_sun_field_bg3_unavailable(metadata)) {
            *out_entry = entry;
            if (out_metadata) *out_metadata = metadata;
            return true;
        }
    }
    (void)hw_x;
    return false;
}

// Two-dimensional BG3 repair used by a renderer that can request both
// horizontal and vertical margins. The resolver receives the candidate
// hardware X/Y pair. For a single-axis margin this is exactly the directional
// 8px search above; corners first try each axis independently, then walk the
// Cartesian inward candidates so both coordinates can reach the native canvas.
template <typename Resolve>
inline bool golden_sun_field_bg3_inward_entry_2d(
    int hw_x, int screen_y, std::uint32_t native_width,
    std::uint32_t native_height, std::uint32_t margin_x,
    std::uint32_t margin_y, Resolve&& resolve, std::uint16_t* out_entry,
    GoldenSunFieldTilemapMetadata* out_metadata = nullptr) {
    if (!out_entry || native_width == 0u || native_height == 0u) return false;

    const int native_last_x = static_cast<int>(native_width);
    const int native_last_y = static_cast<int>(native_height);
    const bool outside_x = hw_x < 0 || hw_x >= native_last_x;
    const bool outside_y = screen_y < 0 || screen_y >= native_last_y;
    if (!outside_x && !outside_y) return false;
    if ((hw_x < -static_cast<int>(margin_x)) ||
        (hw_x >= native_last_x + static_cast<int>(margin_x)) ||
        (screen_y < -static_cast<int>(margin_y)) ||
        (screen_y >= native_last_y + static_cast<int>(margin_y))) {
        return false;
    }

    if (outside_x && !outside_y) {
        return golden_sun_field_bg3_inward_entry(
            hw_x, native_width, margin_x,
            [&](int candidate, std::uint16_t* entry,
                GoldenSunFieldTilemapMetadata* metadata) {
                return resolve(candidate, screen_y, entry, metadata);
            },
            out_entry, out_metadata);
    }
    if (!outside_x && outside_y) {
        return golden_sun_field_bg3_inward_entry_vertical(
            hw_x, screen_y, native_height, margin_y,
            [&](int candidate, std::uint16_t* entry,
                GoldenSunFieldTilemapMetadata* metadata) {
                return resolve(hw_x, candidate, entry, metadata);
            },
            out_entry, out_metadata);
    }

    // A corner has two independent inward directions. Preserve the existing
    // horizontal-first order, then vertical-first, before trying combinations
    // that move both coordinates toward the native rectangle.
    if (margin_x != 0u) {
        if (golden_sun_field_bg3_inward_entry(
                hw_x, native_width, margin_x,
                [&](int candidate, std::uint16_t* entry,
                    GoldenSunFieldTilemapMetadata* metadata) {
                    return resolve(candidate, screen_y, entry, metadata);
                },
                out_entry, out_metadata)) {
            return true;
        }
    }
    if (margin_y != 0u) {
        if (golden_sun_field_bg3_inward_entry_vertical(
                hw_x, screen_y, native_height, margin_y,
                [&](int candidate, std::uint16_t* entry,
                    GoldenSunFieldTilemapMetadata* metadata) {
                    return resolve(hw_x, candidate, entry, metadata);
                },
                out_entry, out_metadata)) {
            return true;
        }
    }

    // If each one-axis candidate is unavailable, try the inward Cartesian
    // product. The bounded margins keep this loop small and deterministic.
    const bool top = screen_y < 0;
    const int y_direction = top ? 1 : -1;
    const bool left = hw_x < 0;
    const int x_direction = left ? 1 : -1;
    const int max_x_distance = static_cast<int>(margin_x) +
        native_last_x - 8;
    const int max_y_distance = static_cast<int>(margin_y) +
        native_last_y - 8;
    for (int y_distance = 8; y_distance <= max_y_distance;
         y_distance += 8) {
        const int candidate_y = screen_y + y_direction * y_distance;
        if (y_distance > static_cast<int>(margin_y) &&
            (candidate_y < 0 || candidate_y >= native_last_y)) {
            continue;
        }
        for (int x_distance = 8; x_distance <= max_x_distance;
             x_distance += 8) {
            const int candidate_x = hw_x + x_direction * x_distance;
            if (x_distance > static_cast<int>(margin_x) &&
                (candidate_x < 0 || candidate_x >= native_last_x)) {
                continue;
            }
            std::uint16_t entry = 0;
            GoldenSunFieldTilemapMetadata metadata;
            if (resolve(candidate_x, candidate_y, &entry, &metadata) &&
                !golden_sun_field_bg3_unavailable(metadata)) {
                *out_entry = entry;
                if (out_metadata) *out_metadata = metadata;
                return true;
            }
        }
    }
    return false;
}

// Per-cell "authored" bitmap for the field's 128x128 metatile table (EWRAM
// 0x02010000..0x02020000, one u32 entry per cell). A bit is set only when
// the guest has actually written that cell since the map was last reset.
// This replaces relying on a single measured sentinel ID/value to detect
// un-authored fill: that approach only recognised the exact 0x017/0xF200
// pair seen in one room and let other rooms' different residual fill IDs
// (e.g. Bilibin's 0x01a) through as visible junk. Kept as a pure,
// independently testable value type; the runner owns the one live instance
// and mirrors EWRAM writes into it only while an expanded view is active.
class GoldenSunFieldAuthoredMap {
public:
    static constexpr std::uint32_t kCells = 128u * 128u;
    static constexpr std::uint32_t kCellBytes = 4u;
    // Absolute guest address of the metatile table (0x02010000).
    static constexpr std::uint32_t kAbsBase = 0x02010000u;
    static constexpr std::uint64_t kAbsEnd =
        static_cast<std::uint64_t>(kAbsBase) + kCells * kCellBytes;

    void reset() { bits_.fill(0u); }

    // `address` is a full guest address (e.g. a bus write address) and
    // `size` the access width in bytes. Marks every metatile cell the
    // access overlaps as authored.
    void mark_write(std::uint32_t address, std::uint32_t size) {
        if (size == 0u) return;
        const std::uint64_t first = address;
        const std::uint64_t last = first + size;
        if (last <= kAbsBase || first >= kAbsEnd) return;
        const std::uint64_t lo = first > kAbsBase ? first : kAbsBase;
        const std::uint64_t hi = last < kAbsEnd ? last : kAbsEnd;
        const std::uint32_t first_cell = static_cast<std::uint32_t>(
            (lo - kAbsBase) / kCellBytes);
        const std::uint32_t last_cell = static_cast<std::uint32_t>(
            (hi - 1u - kAbsBase) / kCellBytes);
        for (std::uint32_t cell = first_cell;
             cell <= last_cell && cell < kCells; ++cell) {
            bits_[cell >> 3] = static_cast<std::uint8_t>(
                bits_[cell >> 3] | (1u << (cell & 7u)));
        }
    }

    bool authored(std::uint32_t map_x, std::uint32_t map_y) const {
        const std::uint32_t cell = map_y * 128u + map_x;
        if (cell >= kCells) return false;
        return (bits_[cell >> 3] & (1u << (cell & 7u))) != 0u;
    }

    // Diagnostic-only census of writes observed since the last reset. This is
    // deliberately separate from the map contents: a savestate may restore a
    // populated EWRAM table while this bitmap correctly starts empty.
    std::uint32_t authored_count() const {
        std::uint32_t count = 0;
        for (std::uint8_t byte : bits_) {
            for (unsigned bit = 0; bit < 8u; ++bit)
                count += (byte >> bit) & 1u;
        }
        return count;
    }

private:
    std::array<std::uint8_t, kCells / 8u> bits_{};
};

inline bool golden_sun_field_tilemap_entry(
    std::uint16_t dispcnt, const std::uint8_t* io, std::size_t io_size,
    const std::uint8_t* ewram, std::size_t ewram_size, int bg, int hw_x,
    int screen_y, std::uint16_t* out_entry,
    GoldenSunFieldTilemapMetadata* out_metadata = nullptr,
    const GoldenSunFieldAuthoredMap* authored = nullptr,
    bool allow_native_x = false, bool allow_split_scroll = false,
    bool allow_mismatch_terrain = false) {
    // `allow_split_scroll` is reserved for the separately authenticated Palace
    // class. It does not relax any coordinate, source-bound, or sentinel check.
    // `allow_mismatch_terrain` permits the Mode0ScrollMismatch policy reason
    // to reach this lookup for the field's terrain layer only (see
    // golden_sun_field_terrain_bg); every layer, including the terrain layer,
    // always reads its own raw scroll register directly. There is no
    // cross-layer reconstruction: a prior attempt at one guessed an unproven
    // delta and produced visibly wrong margins.
    constexpr std::size_t kMapBase = 0x10000u;
    constexpr std::size_t kRawBase = 0x20000u;
    constexpr std::size_t kMapBytes = 128u * 128u * 4u;
    constexpr std::size_t kRawBytes = 4096u * 8u;
    constexpr std::size_t kRequiredEwram = kRawBase + kRawBytes;
    constexpr std::size_t kRequiredIo = 0x20u;
    if (!out_entry || !io || io_size < kRequiredIo || !ewram ||
        ewram_size < kRequiredEwram || bg < 1 || bg > 3 ||
        hw_x < -kExpandedExtraX ||
        hw_x >= static_cast<int>(kNativeWidth) + kExpandedExtraX ||
        screen_y < -kExpandedExtraY ||
        screen_y >= static_cast<int>(kNativeHeight) + kExpandedExtraY ||
        (!allow_native_x && hw_x >= 0 &&
         hw_x < static_cast<int>(kNativeWidth) && screen_y >= 0 &&
         screen_y < static_cast<int>(kNativeHeight))) {
        return false;
    }
    const GoldenSunWidePolicyReason policy_reason =
        golden_sun_wide_margin_policy_reason(dispcnt, io);
    if (policy_reason != GoldenSunWidePolicyReason::AuthorizedMode0 &&
        !(allow_split_scroll &&
          golden_sun_mode0_split_scroll_policy_reason(dispcnt, io) ==
              GoldenSunWidePolicyReason::AuthorizedMode0SplitScroll) &&
        !(allow_mismatch_terrain &&
          policy_reason == GoldenSunWidePolicyReason::Mode0ScrollMismatch)) {
        return false;
    }

    const std::size_t scroll_off = 0x14u +
        static_cast<std::size_t>(bg - 1) * 4u;
    // Keep the raw 16-bit register values. The guest's hardware samples mask
    // them to 9 bits, while these high bits select the field atlas region.
    const std::int32_t hofs = static_cast<std::int32_t>(
        read_io16(io, scroll_off));
    const std::int32_t vofs = static_cast<std::int32_t>(
        read_io16(io, scroll_off + 2u));
    const std::int32_t abs_x = hofs + static_cast<std::int32_t>(hw_x);
    const std::int32_t abs_y = vofs + static_cast<std::int32_t>(screen_y);
    const auto floor_div8 = [](std::int32_t value) {
        if (value >= 0) return value / 8;
        return -static_cast<std::int32_t>(
            (-static_cast<std::int64_t>(value) + 7) / 8);
    };
    const std::int32_t tile_x = floor_div8(abs_x);
    const std::int32_t tile_y = floor_div8(abs_y);
    // The provider's map is a finite 128x128 authored table. Do not apply
    // GBA's 9-bit/wrap behavior to this logical atlas lookup: negative or
    // beyond-table tile coordinates are out-of-room and must fail closed.
    if (tile_x < 0 || tile_y < 0 || tile_x >= 256 || tile_y >= 256) {
        return false;
    }
    const std::uint32_t map_x = static_cast<std::uint32_t>(tile_x / 2);
    const std::uint32_t map_y = static_cast<std::uint32_t>(tile_y / 2);
    if (out_metadata) {
        out_metadata->tile_x = tile_x;
        out_metadata->tile_y = tile_y;
        out_metadata->map_x = map_x;
        out_metadata->map_y = map_y;
    }
    // A cell the guest has not written since the current area loaded is
    // untrustworthy: it may be residue from whatever previously occupied
    // this EWRAM table (e.g. a different room's fill, or another feature's
    // scratch use). Reject it before reading map/raw content.
    if (authored && !authored->authored(map_x, map_y)) return false;
    const std::size_t map_off = kMapBase +
        (static_cast<std::size_t>(map_y) * 128u + map_x) * 4u;
    if (map_off < kMapBase || map_off + 4u > kMapBase + kMapBytes ||
        map_off + 4u > ewram_size) {
        return false;
    }
    const std::uint32_t map_word =
        static_cast<std::uint32_t>(ewram[map_off]) |
        (static_cast<std::uint32_t>(ewram[map_off + 1u]) << 8) |
        (static_cast<std::uint32_t>(ewram[map_off + 2u]) << 16) |
        (static_cast<std::uint32_t>(ewram[map_off + 3u]) << 24);
    const std::uint32_t id = map_word & 0x0FFFu;
    if (out_metadata)
        out_metadata->map_id = static_cast<std::uint16_t>(id);
    const std::size_t raw_off = kRawBase +
        static_cast<std::size_t>(id) * 8u +
        (static_cast<std::size_t>(static_cast<std::uint32_t>(tile_y) & 1u) *
         4u) +
        (static_cast<std::size_t>(static_cast<std::uint32_t>(tile_x) & 1u) *
         2u);
    if (raw_off < kRawBase || raw_off + 2u > kRawBase + kRawBytes ||
        raw_off + 2u > ewram_size) {
        return false;
    }
    const std::uint16_t raw_entry =
        static_cast<std::uint16_t>(ewram[raw_off]) |
        static_cast<std::uint16_t>(ewram[raw_off + 1u] << 8);
    if (out_metadata) {
        out_metadata->raw_entry = raw_entry;
        out_metadata->has_raw_entry = true;
    }
    if (raw_entry == kGoldenSunFieldUnavailableTile ||
        (allow_split_scroll && id == kGoldenSunPalaceNoMapId)) {
        return false;
    }
    *out_entry = raw_entry;
    return true;
}

// Palace's restored table contains a large 0x026 residual region.  A raw
// entry can be finite and non-sentinel while still belonging to a disconnected
// island of that residual image.  Keep the presentation boundary conservative:
// seed each layer from independently valid samples in the authentic canvas,
// then flood bounded per-seed windows through non-0x026 map cells. This is
// pure table metadata; it does not use authored-write ownership or a bitmap.
class GoldenSunPalaceActiveRegion {
public:
    static constexpr std::size_t kWidth = 128u;
    static constexpr std::size_t kHeight = 128u;
    static constexpr std::size_t kCellCount = kWidth * kHeight;
    // A 16px metatile bounds the atlas graph. These are the exact conservative
    // ceil margins for Expanded's 60px horizontal and 40px vertical view.
    static constexpr std::size_t kMaxHorizontalCells = 4u;
    static constexpr std::size_t kMaxVerticalCells = 3u;

    GoldenSunPalaceActiveRegion() { reset(); }

    void reset() {
        for (auto& layer : reachable_) layer.fill(0u);
        for (auto& layer : seeded_) layer.fill(0u);
        built_.fill(false);
        seed_counts_.fill(0u);
    }

    bool build(std::uint16_t dispcnt, const std::uint8_t* io,
               std::size_t io_size, const std::uint8_t* ewram,
               std::size_t ewram_size, bool exact_table_authorized) {
        reset();
        if (!exact_table_authorized ||
            golden_sun_mode0_split_scroll_policy_reason(dispcnt, io) !=
                GoldenSunWidePolicyReason::AuthorizedMode0SplitScroll ||
            !io || io_size < 0x20u || !ewram ||
            ewram_size < 0x20000u + 4096u * 8u) {
            return false;
        }

        // The 8px sampling interval is half a metatile, so every visible
        // 16px map cell gets an opportunity to seed each independently
        // scrolled layer. Include the authentic right/bottom edge explicitly;
        // otherwise the last 7px would make the expanded edge need an extra
        // graph cell beyond the ceil(60/16) and ceil(40/16) bounds.
        for (int bg = 1; bg <= 3; ++bg) {
            std::array<std::uint8_t, kCellCount> seeds{};
            for (int screen_y = 0;;) {
                for (int hw_x = 0;;) {
                    std::uint16_t entry = 0;
                    GoldenSunFieldTilemapMetadata metadata;
                    const bool resolved = golden_sun_field_tilemap_entry(
                        dispcnt, io, io_size, ewram, ewram_size, bg, hw_x,
                        screen_y, &entry, &metadata, nullptr,
                        true, true);
                    if (resolved && metadata.has_raw_entry &&
                        metadata.map_id != kGoldenSunPalaceNoMapId &&
                        metadata.raw_entry != kGoldenSunFieldUnavailableTile &&
                        metadata.map_x < kWidth && metadata.map_y < kHeight) {
                        const std::size_t cell =
                            static_cast<std::size_t>(metadata.map_y) * kWidth +
                            metadata.map_x;
                        seeds[cell] = 1u;
                        seeded_[static_cast<std::size_t>(bg - 1)][cell] = 1u;
                        reachable_[static_cast<std::size_t>(bg - 1)][cell] = 1u;
                        ++seed_counts_[static_cast<std::size_t>(bg - 1)];
                    }
                    if (hw_x == static_cast<int>(kNativeWidth) - 1) break;
                    hw_x = std::min(hw_x + 8,
                                    static_cast<int>(kNativeWidth) - 1);
                }
                if (screen_y == static_cast<int>(kNativeHeight) - 1) break;
                screen_y = std::min(screen_y + 8,
                                    static_cast<int>(kNativeHeight) - 1);
            }
            if (seed_counts_[static_cast<std::size_t>(bg - 1)] == 0u)
                continue;

            auto& layer = reachable_[static_cast<std::size_t>(bg - 1)];
            for (std::size_t cell = 0; cell < kCellCount; ++cell) {
                if (seeds[cell] == 0u) continue;
                struct QueueNode {
                    std::uint16_t cell;
                    std::int8_t dx;
                    std::int8_t dy;
                };
                constexpr std::size_t kWindowWidth =
                    2u * kMaxHorizontalCells + 1u;
                constexpr std::size_t kWindowHeight =
                    2u * kMaxVerticalCells + 1u;
                std::array<QueueNode, kWindowWidth * kWindowHeight> queue{};
                std::array<std::uint8_t, kWindowWidth * kWindowHeight> seen{};
                std::size_t head = 0;
                std::size_t tail = 0;
                queue[tail++] = {static_cast<std::uint16_t>(cell), 0, 0};
                seen[kMaxVerticalCells * kWindowWidth +
                     kMaxHorizontalCells] = 1u;
                while (head < tail) {
                    const QueueNode node = queue[head++];
                    const std::size_t current = node.cell;
                    const std::size_t x = current % kWidth;
                    const std::size_t y = current / kWidth;
                    const auto visit = [&](std::size_t nx, std::size_t ny,
                                           std::int8_t dx,
                                           std::int8_t dy) {
                        if (dx < -static_cast<std::int8_t>(kMaxHorizontalCells) ||
                            dx > static_cast<std::int8_t>(kMaxHorizontalCells) ||
                            dy < -static_cast<std::int8_t>(kMaxVerticalCells) ||
                            dy > static_cast<std::int8_t>(kMaxVerticalCells))
                            return;
                        const std::size_t local =
                            static_cast<std::size_t>(dy +
                                static_cast<std::int8_t>(kMaxVerticalCells)) *
                                kWindowWidth + static_cast<std::size_t>(dx +
                                static_cast<std::int8_t>(kMaxHorizontalCells));
                        if (seen[local] != 0u || palace_map_id(
                                ewram, ewram_size, nx, ny) ==
                                kGoldenSunPalaceNoMapId) return;
                        seen[local] = 1u;
                        const std::size_t next = ny * kWidth + nx;
                        layer[next] = 1u;
                        queue[tail++] = {static_cast<std::uint16_t>(next),
                                         dx, dy};
                    };
                    if (x != 0u) visit(x - 1u, y, node.dx - 1, node.dy);
                    if (x + 1u < kWidth)
                        visit(x + 1u, y, node.dx + 1, node.dy);
                    if (y != 0u) visit(x, y - 1u, node.dx, node.dy - 1);
                    if (y + 1u < kHeight)
                        visit(x, y + 1u, node.dx, node.dy + 1);
                }
            }
            built_[static_cast<std::size_t>(bg - 1)] = true;
        }
        return built_[0] && built_[1] && built_[2];
    }

    bool reachable(int bg, std::uint32_t map_x, std::uint32_t map_y) const {
        if (bg < 1 || bg > 3 || map_x >= kWidth || map_y >= kHeight)
            return false;
        const std::size_t layer = static_cast<std::size_t>(bg - 1);
        return built_[layer] &&
               reachable_[layer][static_cast<std::size_t>(map_y) * kWidth +
                                  map_x] != 0u;
    }

    // A seed is a cell observed directly in the authentic 240x160 canvas.
    // Reachable-but-unseeded cells are only admitted because the bounded
    // Palace margin envelope connected them to a native seed.
    bool seeded(int bg, std::uint32_t map_x, std::uint32_t map_y) const {
        if (bg < 1 || bg > 3 || map_x >= kWidth || map_y >= kHeight)
            return false;
        const std::size_t layer = static_cast<std::size_t>(bg - 1);
        return built_[layer] &&
               seeded_[layer][static_cast<std::size_t>(map_y) * kWidth +
                              map_x] != 0u;
    }

    bool layer_built(int bg) const {
        return bg >= 1 && bg <= 3 && built_[static_cast<std::size_t>(bg - 1)];
    }

private:
    static std::uint16_t palace_map_id(const std::uint8_t* ewram,
                                       std::size_t ewram_size,
                                       std::size_t map_x,
                                       std::size_t map_y) {
        const std::size_t off = 0x10000u + (map_y * kWidth + map_x) * 4u;
        if (!ewram || off + 4u > ewram_size) return kGoldenSunPalaceNoMapId;
        return static_cast<std::uint16_t>(
            (static_cast<std::uint32_t>(ewram[off]) |
             (static_cast<std::uint32_t>(ewram[off + 1u]) << 8) |
             (static_cast<std::uint32_t>(ewram[off + 2u]) << 16) |
             (static_cast<std::uint32_t>(ewram[off + 3u]) << 24)) & 0x0FFFu);
    }

    std::array<std::array<std::uint8_t, kCellCount>, 3> reachable_{};
    std::array<std::array<std::uint8_t, kCellCount>, 3> seeded_{};
    std::array<bool, 3> built_{};
    std::array<std::uint32_t, 3> seed_counts_{};
};

// Resolve only coordinates that are either exact pre-truncation provenance or
// outside the hardware sign ambiguity.  A false result means the caller must
// keep the sprite; no signed value is guessed.
inline constexpr bool golden_sun_obj_resolve_oam_x(
    std::uint32_t raw9, bool exact, int exact_x, int* out_x) {
    if (!out_x) return false;
    const std::uint32_t raw = raw9 & 0x1FFu;
    if (exact) {
        if ((static_cast<std::uint32_t>(exact_x) & 0x1FFu) != raw)
            return false;
        *out_x = exact_x;
        return true;
    }
    if (raw >= 256u) return false;
    *out_x = static_cast<int>(raw);
    return true;
}

inline constexpr bool golden_sun_obj_resolve_oam_y(
    std::uint32_t raw8, bool exact, int exact_y, int* out_y) {
    if (!out_y) return false;
    const std::uint32_t raw = raw8 & 0xFFu;
    if (exact) {
        if ((static_cast<std::uint32_t>(exact_y) & 0xFFu) != raw)
            return false;
        *out_y = exact_y;
        return true;
    }
    if (raw >= 128u) return false;
    *out_y = static_cast<int>(raw);
    return true;
}

// Standard GBA OBJ shape (ATTR0 bits 14-15) x size (ATTR1 bits 14-15) pixel
// table. WIDE-01_NPC_IDENTITY.md's "Sprite extent" section observed exactly
// four combinations across every session mined for that document
// (shape=0,size=1 -> 16x16; shape=0,size=2 -> 32x32; shape=1,size=0 -> 16x8;
// shape=2,size=3 -> 32x64); the full standard table is included so any
// shape/size decode stays well-defined, not just the observed four.
inline constexpr void golden_sun_obj_half_extent(unsigned shape,
                                                  unsigned size,
                                                  int* out_half_w,
                                                  int* out_half_h) {
    constexpr int kWidths[3][4] = {
        {8, 16, 32, 64},   // shape 0: square
        {16, 32, 32, 64},  // shape 1: wide
        {8, 8, 16, 32},    // shape 2: tall
    };
    constexpr int kHeights[3][4] = {
        {8, 16, 32, 64},
        {8, 8, 16, 32},
        {16, 32, 32, 64},
    };
    const unsigned s = shape > 2u ? 0u : shape;
    const unsigned z = size > 3u ? 0u : size;
    if (out_half_w) *out_half_w = kWidths[s][z] / 2;
    if (out_half_h) *out_half_h = kHeights[s][z] / 2;
}

// Decode the standard, non-affine OBJ size.  The culler uses the real
// top-left origin and full dimensions; returning false keeps unusual/invalid
// shapes visible rather than guessing.
inline constexpr bool golden_sun_obj_dimensions(unsigned shape,
                                                unsigned size,
                                                int* out_width,
                                                int* out_height) {
    constexpr int kWidths[3][4] = {
        {8, 16, 32, 64},
        {16, 32, 32, 64},
        {8, 8, 16, 32},
    };
    constexpr int kHeights[3][4] = {
        {8, 16, 32, 64},
        {8, 8, 16, 32},
        {16, 32, 32, 64},
    };
    if (!out_width || !out_height || shape >= 3u || size >= 4u)
        return false;
    *out_width = kWidths[shape][size];
    *out_height = kHeights[shape][size];
    return true;
}

// ---- sprite placement outcomes -------------------------------------------
//
// Why this vocabulary exists as policy rather than as loose strings in
// runner_main.cpp: the OBJ position the PPU reads back is truncated (X to 9
// bits, Y to 8), and a view taller than 160 rows cannot be addressed by 8
// bits at all -- rows -104..199 need 304 values and a byte names 256, so the
// bottom margin and the band above the view share bytes (measured
// 2026-09-06; see ROADMAP.md). The only escape is to take the guest's own
// full-precision coordinate at the moment it computes it, before the
// truncation, and treat OAM as an identity tag instead of a position. That
// is what golden_sun_obj_resolve_oam_x/y above check, and these outcomes
// name every way that lookup can end.
//
// The recorder (src/obj_recorder.cpp) counts committed sprites by outcome so
// coverage can be measured before the renderer is rewired to trust the
// table. `Exact` and `PairedBody` are the two that carry a full-precision
// position; everything else falls back to the truncated byte and is where
// vertical wrap comes from.
enum class GoldenSunObjPlacementOutcome : std::uint8_t {
    // Full-precision position recovered for this sprite's own record.
    Exact = 0,
    // A shadow, positioned from its paired body record (body at record+0x00,
    // shadow at record+0x0C -- see golden_sun_obj_record_identity).
    PairedBody,
    // The committed sprite could not be traced back to a guest actor record
    // at all. This is the outcome that matters most: it is the population an
    // object buffer would have to grow to cover.
    SourceUnavailable,
    // Traced to a record, but that record carried no usable coordinate this
    // frame (stale epoch, wrong frame, or the truncated bytes disagreed).
    PlacementUnavailable,
    // Rotation/scaling sprites need transformed bounds; not attempted.
    AffineOrDoubleSize,
    // ATTR shape/size decoded outside the standard table.
    InvalidShapeSize,
    Count,
};

inline constexpr const char* golden_sun_obj_placement_outcome_name(
    GoldenSunObjPlacementOutcome outcome) {
    switch (outcome) {
    case GoldenSunObjPlacementOutcome::Exact: return "exact-placement";
    case GoldenSunObjPlacementOutcome::PairedBody:
        return "paired-body-placement";
    case GoldenSunObjPlacementOutcome::SourceUnavailable:
        return "source-unavailable";
    case GoldenSunObjPlacementOutcome::PlacementUnavailable:
        return "placement-unavailable";
    case GoldenSunObjPlacementOutcome::AffineOrDoubleSize:
        return "affine-or-double-size";
    case GoldenSunObjPlacementOutcome::InvalidShapeSize:
        return "invalid-shape-size";
    case GoldenSunObjPlacementOutcome::Count: break;
    }
    return "unknown";
}

// Recover a sprite's own full-precision coordinate from a paired sprite's
// exact one plus its own truncated OAM field.
//
// A shadow carries no coordinate of its own: the guest computes it from the
// body it belongs to, and only the truncated result reaches OAM. Knowing the
// body's exact row does not by itself place the shadow, which sits some rows
// below it -- so take the difference between the two in the truncated field,
// read it as signed, and add it back. The result truncates to exactly the
// byte OAM carries, and it is unique so long as the true offset between the
// pair is within half the field's range. A shadow is drawn touching the
// sprite it belongs to, so that holds with enormous margin; the recorder
// reports the largest offset it actually sees so the assumption stays
// measured rather than assumed.
//
// `bits` is the width of the truncated field: 8 for OAM Y, 9 for OAM X.
inline constexpr int golden_sun_obj_paired_coordinate(int exact_paired,
                                                      std::uint32_t raw,
                                                      unsigned bits) {
    const std::uint32_t mask = (1u << bits) - 1u;
    const std::uint32_t half = 1u << (bits - 1u);
    const std::uint32_t delta =
        (raw - static_cast<std::uint32_t>(exact_paired)) & mask;
    return exact_paired + (delta >= half
                               ? static_cast<int>(delta) -
                                     static_cast<int>(mask + 1u)
                               : static_cast<int>(delta));
}

// A sprite is safe to draw from the table -- and therefore immune to the
// 8-bit vertical wrap -- only in these two outcomes.
inline constexpr bool golden_sun_obj_placement_is_exact(
    GoldenSunObjPlacementOutcome outcome) {
    return outcome == GoldenSunObjPlacementOutcome::Exact ||
           outcome == GoldenSunObjPlacementOutcome::PairedBody;
}

}  // namespace gsr::widescreen
