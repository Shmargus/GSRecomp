// Golden Sun: The Lost Age -- a second sprite page for the people on the
// field in the expanded view (FACTS.md, 2026-10-16, "OBJ tile allocator").
//
// The game hands out its 32 KB of sprite memory through 0x080142D4 (owner
// r0, bytes r1): a first fit, 0x08014174, over a 0x200-byte block map at
// 0x02003410 (64 bytes a block, 0xFF free, else the owner), the grant kept in
// the owner table 0x020036E0 (96 x {u16 bytes, u16 byte offset}). In the
// widened view the people in the margins take that memory too: Madra filled
// it to 506 of 512 blocks, and the menu icons and dialogue portrait asked
// next got nothing (F12 tla_rewind_0037..0039, 0046).
//
// Here the character builder's own request (0x08021FA2, returning to
// 0x08021FA5) is granted from the engine's second page instead
// (gba_bus.h set_obj_extra, byte offsets 0x8000..0xFFFF, tiles 0x400..0x7FF),
// so menus, portraits, spells and shadows keep the console's 32 KB. The
// grant lives only in the owner table, as any other: freeing, room changes
// and save states need nothing more. The builder copies the artwork to
// 0x06010000 + offset (0x08021FA4) and keeps the tile's low 10 bits in the
// sprite (`ands` 0x3FF, 0x08021FC4); the renderer gets bit 10 back from the
// sprite's record (tla_sprite_memory_tile).
//
// Inert in the console's 240x160 view, with the CPU renderer (Direct3D
// compatibility mode), and with the launcher's "Console sprite memory".

#include <cstdint>
#include <array>
#include <cstdio>
#include <cstdlib>

#include "gba_bus.h"
#include "runtime.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"
#include "tla.h"

namespace {

constexpr std::uint32_t kOwnerTable = 0x020036E0u;
constexpr std::uint32_t kBlockMap = 0x02003410u;
constexpr std::uint32_t kMapNumber = 0x02000428u;
constexpr unsigned kOwners = 0x60, kBlocks = 0x200, kBlockBytes = 64;
// The page's byte offsets: after the console's 32 KB, up to the owner
// table's "none" value 0xFFFF.
constexpr std::uint32_t kPageStart = 0x8000u, kPageEnd = 0x10000u;
// 0x080142D4 after its first fit: `cmp r5, #-1; beq` returns 0 (no tile).
// r5 the offset or -1, r6 bytes, r7 the owner's table entry; the caller's
// return address is at sp + 16 (`push {r5-r7, lr}`, `push {r8}`).
constexpr std::uint32_t kAllocResult = 0x0801431Au;
constexpr std::uint32_t kBuilderReturn = 0x08021FA5u;

bool g_on = false;        // granting from the page this frame
unsigned g_peak = 0;      // most page blocks in use at once this session
unsigned g_logged_map = 0xFFFFu;  // full-page / full-console notes, once a map

gba::GbaBus* bus() { return gbarecomp::active_bus(); }

bool trace_enabled() {
    static const bool on = [] {
        const char* e = std::getenv("TLA_SPRITE_MEMORY_TRACE");
        return e && e[0] && e[0] != '0';
    }();
    return on;
}

// Log allocator results after our branch hook has made its decision. This
// catches creation before an F12 history starts, without dumping artwork.
struct AllocationTrace {
    gba::GbaBus& b;
    unsigned owner;
    std::uint32_t bytes, caller;
    bool enabled = trace_enabled();
    ~AllocationTrace() {
        if (!enabled) return;
        std::fprintf(stderr,
            "[sprite-alloc] map=%u owner=%u bytes=%u caller=%08X "
            "previous=%u:%04X result=%08X source=%08X\n",
            static_cast<unsigned>(b.read16(kMapNumber)), owner, bytes, caller,
            static_cast<unsigned>(b.read16(kOwnerTable + owner * 4u)),
            static_cast<unsigned>(b.read16(kOwnerTable + owner * 4u + 2u)),
            g_cpu.R[5], g_cpu.R[8]);
    }
};

void trace_record(gba::GbaBus& b, unsigned owner, std::uint32_t record,
                  std::uint16_t attr2) {
    if (!trace_enabled() || owner >= kOwners) return;
    struct Seen {
        std::uint32_t record = 0, grant = 0;
        std::uint16_t attr2 = 0, map = 0;
    };
    static std::array<Seen, kOwners> seen{};
    const std::uint32_t grant = b.read32(kOwnerTable + owner * 4u);
    const std::uint16_t map = b.read16(kMapNumber);
    auto& last = seen[owner];
    if (last.record == record && last.grant == grant &&
        last.attr2 == attr2 && last.map == map) return;
    last = {record, grant, attr2, map};
    std::fprintf(stderr,
        "[sprite-tile] map=%u owner=%u record=%08X attr2=%04X "
        "grant=%u:%04X\n", static_cast<unsigned>(map), owner, record,
        static_cast<unsigned>(attr2), grant & 0xFFFFu, grant >> 16);
}

std::uint32_t round_up(std::uint32_t bytes) {
    return (bytes + kBlockBytes - 1) / kBlockBytes * kBlockBytes;
}

// The page's live grants are the owner table's entries past the console's
// 32 KB (a released entry holds bytes <= 0x10 or offset 0xFFFF).
bool page_grant(gba::GbaBus& b, unsigned owner, std::uint32_t* from,
                std::uint32_t* to) {
    const std::uint32_t entry = kOwnerTable + owner * 4u;
    const std::uint32_t bytes = b.read16(entry), offset = b.read16(entry + 2u);
    if (bytes <= 0x10u || offset == 0xFFFFu || offset < kPageStart) return false;
    *from = offset;
    *to = offset + round_up(bytes);
    return true;
}

// First fit in the page, every other owner's grant excluded; -1 when full.
// `used` gets the page bytes already granted.
std::int32_t page_first_fit(gba::GbaBus& b, unsigned skip, std::uint32_t bytes,
                            std::uint32_t* used) {
    *used = 0;
    std::uint32_t from[kOwners], to[kOwners];
    unsigned n = 0;
    for (unsigned owner = 0; owner < kOwners; ++owner)
        if (owner != skip && page_grant(b, owner, &from[n], &to[n])) {
            *used += to[n] - from[n];
            ++n;
        }
    const std::uint32_t need = round_up(bytes);
    for (std::uint32_t at = kPageStart; at + need <= kPageEnd;) {
        std::uint32_t clash = 0;
        for (unsigned i = 0; i < n; ++i)
            if (at < to[i] && from[i] < at + need && to[i] > clash) clash = to[i];
        if (!clash) return static_cast<std::int32_t>(at);
        at = clash;
    }
    return -1;
}

void note_once(gba::GbaBus& b, const char* what, unsigned owner,
               std::uint32_t bytes, std::uint32_t caller) {
    const unsigned map = b.read16(kMapNumber);
    if (map == g_logged_map) return;
    g_logged_map = map;
    std::fprintf(stderr,
                 "[sprite-memory] map %u: %s (owner 0x%02X, %u bytes, "
                 "caller 0x%08X)\n",
                 map, what, owner, bytes, caller);
}

}  // namespace

int tla_sprite_memory_branch(std::uint32_t pc, std::uint32_t original,
                             std::uint32_t* out_decision) {
    if (pc != kAllocResult) return -1;
    gba::GbaBus* b = bus();
    if (!b) return 0;
    const std::uint32_t entry = g_cpu.R[7];
    const unsigned owner = (entry - kOwnerTable) / 4u;
    const std::uint32_t bytes = g_cpu.R[6];
    const std::uint32_t caller = b->read32(g_cpu.R[13] + 16u);
    if (entry < kOwnerTable || owner >= kOwners) return 0;
    const AllocationTrace allocation_trace{*b, owner, bytes, caller};
    // Battles build their fighters with the same builder, but only field
    // sprites tell the renderers their page (F12 tla_rewind_0049: the party
    // and King Scorpion granted 0x8000..0xBFFF, drawn from the console's).
    const bool field = b->read16(kMapNumber) > 1u && b->read32(kTlaBattleSlot) == 0u;
    if (caller != kBuilderReturn || !g_on || !field) {
        // Measure whether the console's 32 KB still runs out.
        if (original && g_on) note_once(*b, "console sprite memory full", owner, bytes, caller);
        return 0;
    }
    // An equal-size grant the game reuses stays where it is: moving it every
    // pass left OAM pointing at the last frame's spot, which the next owner
    // had taken (F12 tla_rewind_0050, Felix in sand: the sand sprites hopped
    // round the page every frame).
    if (b->read16(entry) == bytes && b->read16(entry + 2u) == g_cpu.R[5])
        return 0;
    std::uint32_t used = 0;
    const std::int32_t at = page_first_fit(*b, owner, bytes, &used);
    if (at < 0) {
        note_once(*b, "second sprite page full", owner, bytes, caller);
        return 0;  // the console's grant (or none), as before
    }
    // Hand back the console blocks the first fit just took for this owner;
    // its earlier grant was released before the first fit ran.
    if (!original)
        for (unsigned i = 0; i < kBlocks; ++i)
            if (b->read8(kBlockMap + i) == owner) b->write8(kBlockMap + i, 0xFFu);
    g_cpu.R[5] = static_cast<std::uint32_t>(at);
    const unsigned in_use = (used + round_up(bytes)) / kBlockBytes;
    if (in_use > g_peak) {
        g_peak = in_use;
        std::fprintf(stderr,
                     "[sprite-memory] map %u: second page peak %u of %u blocks "
                     "(%u tiles)\n",
                     static_cast<unsigned>(b->read16(kMapNumber)), in_use,
                     (kPageEnd - kPageStart) / kBlockBytes, in_use * 2u);
    }
    *out_decision = 0u;  // not taken: the grant is stored and returned
    return 1;
}

bool tla_sprite_memory_frame(bool expanded) {
    static const bool off = [] {
        const char* e = std::getenv("TLA_CONSOLE_SPRITE_MEMORY");
        return e && e[0] && e[0] != '0';
    }();
    gba::GbaBus* b = bus();
    if (!b) return false;
    g_on = expanded && !off;
    // Once on, the page stays mapped so earlier grants keep their artwork.
    // The renderers keep asking while the page is mapped, grants or not:
    // OAM shows the people for a frame after a menu opens or closes and
    // releases every grant (F12 tla_rewind_0052 frames 8 and 110).
    if (g_on) b->set_obj_extra(true);
    return b->obj_extra();
}

int tla_sprite_memory_tile(std::uint32_t record, std::uint16_t attr2) {
    gba::GbaBus* b = bus();
    if (!b || ((record >> 24) != 0x02u && (record >> 24) != 0x03u)) return -1;
    const unsigned owner = b->read8(record + 0x10u);
    trace_record(*b, owner, record, attr2);
    std::uint32_t from = 0, to = 0;
    if (owner >= kOwners || !page_grant(*b, owner, &from, &to)) return -1;
    // The sprite's 10 bits must fall inside its owner's grant.
    const unsigned tile = 0x400u | (attr2 & 0x3FFu);
    return tile >= from / 32u && tile < to / 32u ? static_cast<int>(tile) : -1;
}
