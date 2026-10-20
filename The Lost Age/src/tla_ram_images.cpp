// Golden Sun: The Lost Age -- routines the game copies into RAM and runs.
//
// Several wrappers allocate a buffer (or carve one off the stack), DMA a fixed
// ROM routine into it, call it and free it (config/tla/reloc/*.toml has the
// evidence for each). The buffer address comes from the game's allocator, so
// the same routine runs at different RAM addresses. Each routine is
// translated once as a position-independent image (gba_recompile
// --relocatable-image); this file recognizes a live copy and dispatches into
// that image.
//
// Recognition is exact: the live bytes must equal the ROM routine byte for
// byte. A verified base is cached until a write lands on one of its 4 KiB
// pages (the engine's per-page write epochs).
//
// The sound mixer in the boot IWRAM copy also patches its own loops; each
// patched spot has a fixed set of contents, translated ahead of time as
// fixed-address variants (tools/tla_mixer_variants.py) and recognized the
// same way against their expected bytes.
//
// Everything else the game ran from RAM in play sessions -- overlays,
// stack copies, patched code -- is harvested from the engine's self-heal
// cache (tools/tla_harvest.py) and packed the same way, as Golden Sun 1
// pre-built its RAM code.
//
// Battle and Psynergy effects draw through blitters the game assembles from
// template pieces (builder 0x08196404). Every routine it can build is
// translated ahead of time (tools/tla_battle_blitters.py), packed at a fixed
// stride into a few images; a live routine is matched by its exact bytes at
// any base, like a copied routine.
//
// Installing a RAM dispatch hook changes two engine defaults, both restored
// here: unmatched RAM code still reaches the self-heal overlay tier, and a
// fixed RAM function on a written page still runs behind its own CRC guard.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "gba_bus.h"
#include "overlay_loader.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"
#include "tla.h"
// One copy for both games (TLA's CMakeLists.txt): the data unpacker's
// safety net and its test options.
#include "../../The Broken Seal/src/unpacker_guard.h"

struct DispatchEntry { uint32_t addr; uint8_t thumb; void (*fn)(void); };

// The image list is generated (scripts/tla_recompile.ps1): TLA_RELOC(name,
// rom source) and TLA_SKIP(name, start, end) for config/tla/reloc/*.toml,
// TLA_VARIANT(name) for the mixer variants, TLA_FAMILY(name) for the packed
// battle blitters. Skips of one image are listed in ascending order.
#define TLA_RELOC(name, src)                                \
    extern "C" const DispatchEntry name##_kDispatchTable[]; \
    extern "C" const unsigned name##_kDispatchTableLen;     \
    extern "C" const uint32_t name##_kImageOrigin;          \
    extern "C" const uint32_t name##_kImageSize;
#define TLA_VARIANT(name) \
    TLA_RELOC(name, 0u)   \
    extern "C" const uint8_t name##_kExpected[];
#define TLA_FAMILY(name)                              TLA_RELOC(name, 0u)                               extern "C" const uint8_t name##_kExpected[];      extern "C" const uint32_t name##_kSizes[];        extern "C" const unsigned name##_kCount;          extern "C" const uint32_t name##_kStride;
#ifdef TLA_PLAYER_RELEASE
#define TLA_HARVEST(name)
#else
#define TLA_HARVEST(name) TLA_FAMILY(name) extern "C" const uint8_t name##_kStarts[];
#endif
#define TLA_SKIP(name, start, end)
#include "tla_reloc_images.inc"
#undef TLA_SKIP
#undef TLA_FAMILY
#undef TLA_HARVEST
#undef TLA_VARIANT
#undef TLA_RELOC

namespace {

struct Image {
    const char* name;
    uint32_t rom_src;  // the copy's ROM source (config/tla/reloc/<name>.toml)
    const DispatchEntry* table;
    const unsigned* table_len;
    const uint32_t* origin;
    const uint32_t* size;
    // Mixer variants: the bytes to match, and only at their own origin.
    const uint8_t* expected;
};

#define TLA_RELOC(name, src)                                         \
    {#name, src, name##_kDispatchTable, &name##_kDispatchTableLen, \
     &name##_kImageOrigin, &name##_kImageSize, nullptr},
#define TLA_VARIANT(name)                                            \
    {#name, 0u, name##_kDispatchTable, &name##_kDispatchTableLen,  \
     &name##_kImageOrigin, &name##_kImageSize, name##_kExpected},
#define TLA_FAMILY(name)
#define TLA_HARVEST(name)
#define TLA_SKIP(name, start, end)
const Image kImages[] = {
#include "tla_reloc_images.inc"
};
#undef TLA_SKIP
#undef TLA_FAMILY
#undef TLA_HARVEST
#undef TLA_VARIANT
#undef TLA_RELOC
constexpr unsigned kImageCount = sizeof(kImages) / sizeof(kImages[0]);

// Areas a routine writes inside its own copy (data; "# identity-skip" in its
// config): not compared.
struct Skip { const char* name; uint32_t start, end; };
#define TLA_RELOC(name, src)
#define TLA_VARIANT(name)
#define TLA_FAMILY(name)
#define TLA_HARVEST(name)
#define TLA_SKIP(name, start, end) {#name, start, end},
const Skip kSkips[] = {
#include "tla_reloc_images.inc"
    {nullptr, 0u, 0u},
};
#undef TLA_SKIP
#undef TLA_FAMILY
#undef TLA_HARVEST
#undef TLA_VARIANT
#undef TLA_RELOC

// Each image's skips, gathered once: a byte check runs on every RAM dispatch
// that misses the cache, against every candidate image (in one battle, ~90,000
// times a frame), and finding the skips by name there cost most of the frame.
const std::vector<const Skip*>& skips_of(unsigned index) {
    static const std::vector<std::vector<const Skip*>> all = [] {
        std::vector<std::vector<const Skip*>> v(kImageCount);
        for (const Skip& s : kSkips) {
            if (!s.name) continue;
            for (unsigned i = 0; i < kImageCount; ++i) {
                if (std::strcmp(s.name, kImages[i].name) == 0) v[i].push_back(&s);
            }
        }
        return v;
    }();
    return all[index];
}

bool bytes_match(unsigned index, const uint8_t* live, const uint8_t* want,
                 uint32_t size) {
    uint32_t pos = 0;
    for (const Skip* s : skips_of(index)) {
        if (s->start > pos && std::memcmp(live + pos, want + pos, s->start - pos))
            return false;
        pos = s->end;
    }
    return pos >= size || std::memcmp(live + pos, want + pos, size - pos) == 0;
}

struct Candidate { uint16_t image; uint8_t thumb; uint32_t offset; RuntimeGuestFn fn; };
// Keyed by the instruction at the entry: (1<<32 | halfword) for Thumb,
// the word for ARM. A live pc can only belong to an image whose entry at the
// same offset holds the same instruction.
std::unordered_map<uint64_t, std::vector<Candidate>> g_by_opcode;

struct Placement {
    bool valid = false;
    uint32_t base = 0;
    unsigned epoch_first = 0;
    unsigned epoch_last = 0;
};
Placement g_placement[kImageCount];
unsigned g_logged[kImageCount];

uint8_t* live_ptr(uint32_t addr, uint32_t len) {
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus) return nullptr;
    if (addr >= 0x03000000u && addr + len <= 0x03008000u)
        return bus->iwram_ptr() + (addr - 0x03000000u);
    if (addr >= 0x02000000u && addr + len <= 0x02040000u)
        return bus->ewram_ptr() + (addr - 0x02000000u);
    return nullptr;
}

const uint8_t* rom_ptr(uint32_t addr, uint32_t len) {
    gba::GbaBus* bus = gbarecomp::active_bus();
    if (!bus || addr < 0x08000000u) return nullptr;
    const uint32_t off = addr - 0x08000000u;
    if (off + len > bus->rom_size()) return nullptr;
    return bus->rom_ptr() + off;
}

unsigned page_epoch(uint32_t addr) {
    const uint32_t page = (addr & 0x0003FFFFu) >> 12;
    return (addr >> 24) == 0x03u ? g_ram_code_page_epoch_iwram[page & 7u]
                                 : g_ram_code_page_epoch_ewram[page & 63u];
}

// Make writes to these pages advance their epochs (see ram_code_write_affects).
void register_pages(uint32_t start, uint32_t end) {
    for (uint32_t a = start & ~0xFFFu; a < end; a += 0x1000u) {
        const uint32_t page = (a & 0x0003FFFFu) >> 12;
        if ((a >> 24) == 0x03u) {
            g_ram_code_page_mask_iwram |= 1u << (page & 7u);
        } else if (page < 32u) {
            g_ram_code_page_mask_ewram_lo |= 1ull << page;
        } else {
            g_ram_code_page_mask_ewram_hi |= 1ull << (page - 32u);
        }
    }
}

bool resident_at(unsigned index, uint32_t base) {
    const Image& image = kImages[index];
    const uint32_t size = *image.size;
    Placement& p = g_placement[index];
    if (p.valid && p.base == base &&
        p.epoch_first == page_epoch(base) &&
        p.epoch_last == page_epoch(base + size - 1u)) {
        return true;
    }
    const uint8_t* live = live_ptr(base, size);
    const uint8_t* want =
        image.expected ? image.expected : rom_ptr(image.rom_src, size);
    if (!live || !want || !bytes_match(index, live, want, size)) return false;
    register_pages(base, base + size);
    p.valid = true;
    p.base = base;
    p.epoch_first = page_epoch(base);
    p.epoch_last = page_epoch(base + size - 1u);
    if (g_logged[index] < 8u) {
        ++g_logged[index];
        std::fprintf(stderr, "[tla-ram-image] %s verified at 0x%08X\n",
                     image.name, base);
    }
    return true;
}

// Entries at bytes the byte check skips (state words, code the routine
// patches or copies into itself): their live opcode is not the ROM's, so
// they are tried for any opcode.
std::vector<Candidate> g_any_opcode;

bool skipped(unsigned index, uint32_t offset) {
    for (const Skip* s : skips_of(index)) {
        if (offset >= s->start && offset < s->end) return true;
    }
    return false;
}

RuntimeGuestFn image_dispatch(uint32_t pc, int thumb) {
    const uint8_t* live = live_ptr(pc, thumb ? 2u : 4u);
    if (!live) return nullptr;
    uint64_t key;
    if (thumb) {
        uint16_t hw;
        std::memcpy(&hw, live, 2);
        key = (uint64_t{1} << 32) | hw;
    } else {
        uint32_t w;
        std::memcpy(&w, live, 4);
        key = w;
    }
    auto it = g_by_opcode.find(key);
    if (it != g_by_opcode.end()) {
        // The cached base first: a routine is entered many times per copy.
        for (const Candidate& c : it->second) {
            const Placement& p = g_placement[c.image];
            if (p.valid && p.base == pc - c.offset &&
                resident_at(c.image, p.base)) {
                g_runtime_image_base = p.base;
                return c.fn;
            }
        }
        for (const Candidate& c : it->second) {
            const uint32_t base = pc - c.offset;
            if ((base & 3u) != 0u) continue;
            if (resident_at(c.image, base)) {
                g_runtime_image_base = base;
                return c.fn;
            }
        }
    }
    for (const Candidate& c : g_any_opcode) {
        if (c.thumb != (thumb ? 1u : 0u)) continue;
        const uint32_t base = pc - c.offset;
        if ((base & 3u) != 0u || !resident_at(c.image, base)) continue;
        g_runtime_image_base = base;
        return c.fn;
    }
    return nullptr;
}

// Built on the first RAM dispatch: the ROM is on the bus by then.
bool build_index() {
    for (unsigned i = 0; i < kImageCount; ++i) {
        const Image& image = kImages[i];
        if (image.expected) continue;  // variants: by address, see below
        for (unsigned e = 0; e < *image.table_len; ++e) {
            const DispatchEntry& d = image.table[e];
            const uint32_t offset = d.addr - *image.origin;
            // The walk can also emit functions outside the copied bytes
            // (targets it followed past the end); those are not image code.
            if (offset >= *image.size) continue;
            const uint8_t* rom = rom_ptr(image.rom_src + offset, 4u);
            if (!rom) {
                g_by_opcode.clear();
                g_any_opcode.clear();
                return false;
            }
            if (skipped(i, offset)) {
                g_any_opcode.push_back(
                    Candidate{static_cast<uint16_t>(i), d.thumb, offset, d.fn});
                continue;
            }
            uint64_t key;
            if (d.thumb) {
                uint16_t hw;
                std::memcpy(&hw, rom, 2);
                key = (uint64_t{1} << 32) | hw;
            } else {
                uint32_t w;
                std::memcpy(&w, rom, 4);
                key = w;
            }
            g_by_opcode[key].push_back(
                Candidate{static_cast<uint16_t>(i), d.thumb, offset, d.fn});
        }
    }
    return true;
}

// A mixer variant covering pc whose expected bytes are live at its origin.
RuntimeGuestFn variant_dispatch(uint32_t pc, int thumb) {
    for (unsigned i = 0; i < kImageCount; ++i) {
        const Image& image = kImages[i];
        if (!image.expected) continue;
        const uint32_t origin = *image.origin;
        if (pc - origin >= *image.size) continue;
        if (!resident_at(i, origin)) continue;
        const DispatchEntry* t = image.table;
        for (unsigned e = 0; e < *image.table_len; ++e) {
            if (t[e].addr == pc && t[e].thumb == (thumb ? 1u : 0u)) {
                g_runtime_image_base = origin;
                return t[e].fn;
            }
        }
    }
    return nullptr;
}

// Battle blitters: routine v of a family sits at origin + v * stride in its
// image, sizes[v] bytes, padded to the stride.
struct Family {
    const char* name;
    const DispatchEntry* table;
    const unsigned* table_len;
    const uint32_t* origin;
    const uint8_t* expected;
    const uint32_t* sizes;
    const unsigned* count;
    const uint32_t* stride;
    // Harvested RAM code (tools/tla_harvest.py): where each routine's own
    // bytes start in its slot (its address mod 4); null for the blitters.
    const uint8_t* starts;
};
#define TLA_RELOC(name, src)
#define TLA_VARIANT(name)
#define TLA_SKIP(name, start, end)
#define TLA_FAMILY(name)                                                        {#name, name##_kDispatchTable, &name##_kDispatchTableLen,                  &name##_kImageOrigin, name##_kExpected, name##_kSizes, &name##_kCount,      &name##_kStride, nullptr},
#ifdef TLA_PLAYER_RELEASE
#define TLA_HARVEST(name)
#else
#define TLA_HARVEST(name) {#name, name##_kDispatchTable, &name##_kDispatchTableLen, &name##_kImageOrigin, name##_kExpected, name##_kSizes, &name##_kCount, &name##_kStride, name##_kStarts},
#endif
const Family kFamilies[] = {
#include "tla_reloc_images.inc"
    {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr},
};
#undef TLA_FAMILY
#undef TLA_HARVEST
#undef TLA_SKIP
#undef TLA_VARIANT
#undef TLA_RELOC
constexpr unsigned kFamilyCount =
    sizeof(kFamilies) / sizeof(kFamilies[0]) - 1u;

struct Member {
    uint16_t family;
    uint16_t routine;
    uint32_t offset;
    RuntimeGuestFn fn;
    uint32_t order;
};
// One instruction puts up to 15,858 entries in a single measured bucket.
// Eight opening bytes reduce the captured searches sharply. Short tails
// keep the instruction index; full routine verification still decides identity.
constexpr unsigned kMemberPrefixSize = sizeof(uint64_t);
std::unordered_map<uint64_t, std::vector<Member>> g_members_by_prefix[2];
std::unordered_map<uint64_t, std::vector<Member>> g_members_by_opcode;
// The last base each routine was verified at.
// Family/routine indices are dense and fixed by the generated image tables.
// Keep the verification result directly at those indices: this lookup runs
// even on a cache hit, so hashing the packed indices repeats needless work.
std::vector<std::vector<Placement>> g_member_placement;
unsigned g_members_logged = 0;
// The routine last dispatched at each entry address: checked first, since a
// built routine is entered many times while it lives.
std::unordered_map<uint64_t, const Member*> g_member_hit;

uint64_t opcode_key(const uint8_t* p, int thumb) {
    if (thumb) {
        uint16_t hw;
        std::memcpy(&hw, p, 2);
        return (uint64_t{1} << 32) | hw;
    }
    uint32_t w;
    std::memcpy(&w, p, 4);
    return w;
}

void build_family_index() {
    uint32_t order = 0;
    g_member_placement.resize(kFamilyCount);
    for (unsigned f = 0; f < kFamilyCount; ++f) {
        const Family& fam = kFamilies[f];
        g_member_placement[f].resize(*fam.count);
        for (unsigned e = 0; e < *fam.table_len; ++e) {
            const DispatchEntry& d = fam.table[e];
            const uint32_t rel = d.addr - *fam.origin;
            const uint32_t routine = rel / *fam.stride;
            const uint32_t offset = rel % *fam.stride;
            if (routine >= *fam.count || offset >= fam.sizes[routine]) continue;
            if (fam.starts && offset < fam.starts[routine]) continue;
            const uint8_t* at = fam.expected + rel;
            const Member m{
                static_cast<uint16_t>(f), static_cast<uint16_t>(routine),
                offset, d.fn, order++};
            if (fam.sizes[routine] - offset >= kMemberPrefixSize) {
                uint64_t prefix;
                std::memcpy(&prefix, at, sizeof(prefix));
                g_members_by_prefix[d.thumb ? 1 : 0][prefix].push_back(m);
            } else {
                g_members_by_opcode[opcode_key(at, d.thumb)].push_back(m);
            }
        }
    }
}

bool member_resident(const Member& m, uint32_t base) {
    const Family& fam = kFamilies[m.family];
    const uint32_t size = fam.sizes[m.routine];
    const uint32_t start = fam.starts ? fam.starts[m.routine] : 0u;
    Placement& p = g_member_placement[m.family][m.routine];
    if (p.valid && p.base == base && p.epoch_first == page_epoch(base) &&
        p.epoch_last == page_epoch(base + size - 1u)) {
        return true;
    }
    const uint8_t* live = live_ptr(base + start, size - start);
    if (!live ||
        std::memcmp(live, fam.expected + m.routine * *fam.stride + start,
                    size - start) != 0)
        return false;
    register_pages(base, base + size);
    p.valid = true;
    p.base = base;
    p.epoch_first = page_epoch(base);
    p.epoch_last = page_epoch(base + size - 1u);
    if (g_members_logged < 16u) {
        ++g_members_logged;
        std::fprintf(stderr, "[tla-ram-image] %s routine %u verified at 0x%08X\n",
                     fam.name, m.routine, base);
    }
    return true;
}

RuntimeGuestFn family_dispatch(uint32_t pc, int thumb) {
    const uint8_t* live = live_ptr(pc, thumb ? 2u : 4u);
    if (!live) return nullptr;
    // Image addresses are origin-relative: routine v sits v strides in.
    auto enter = [](const Member& m, uint32_t base) {
        g_runtime_image_base = base - m.routine * *kFamilies[m.family].stride;
        return m.fn;
    };
    const uint64_t at = (uint64_t{pc} << 1) | (thumb ? 1u : 0u);
    auto hit = g_member_hit.find(at);
    if (hit != g_member_hit.end() &&
        member_resident(*hit->second, pc - hit->second->offset))
        return enter(*hit->second, pc - hit->second->offset);
    const std::vector<Member>* prefix_members = nullptr;
    if (const uint8_t* opening = live_ptr(pc, kMemberPrefixSize)) {
        uint64_t prefix;
        std::memcpy(&prefix, opening, sizeof(prefix));
        const auto& index = g_members_by_prefix[thumb ? 1 : 0];
        auto it = index.find(prefix);
        if (it != index.end()) prefix_members = &it->second;
    }
    const std::vector<Member>* short_members = nullptr;
    auto short_it = g_members_by_opcode.find(opcode_key(live, thumb));
    if (short_it != g_members_by_opcode.end()) short_members = &short_it->second;
    // Merge the two buckets in their original generated order, preserving
    // which entry wins if more than one complete routine matches.
    size_t i = 0, j = 0;
    while ((prefix_members && i < prefix_members->size()) ||
           (short_members && j < short_members->size())) {
        const bool take_prefix = prefix_members && i < prefix_members->size() &&
            (!short_members || j == short_members->size() ||
             (*prefix_members)[i].order < (*short_members)[j].order);
        const Member& m = take_prefix ? (*prefix_members)[i++] : (*short_members)[j++];
        const uint32_t base = pc - m.offset;
        if ((base & 3u) != 0u || !member_resident(m, base)) continue;
        g_member_hit[at] = &m;
        return enter(m, base);
    }
    return nullptr;
}

// What a dispatch the unpacker guard redid runs: nothing.
void unpacker_redone() {}

RuntimeGuestFn tla_ram_dispatch(uint32_t pc, int thumb) {
    static bool indexed = false;
    if (!indexed) {
        indexed = build_index();
        if (indexed) build_family_index();
    }
    // The boot IWRAM copy (0x03000100..0x03001100, FACTS.md) is fixed code in
    // the main translation, apart from the mixer's patched spots; no copied
    // routine was ever seen there.
    if (pc >= 0x03000100u && pc < 0x03001100u) {
        if (RuntimeGuestFn fn = variant_dispatch(pc, thumb)) return fn;
        if (RuntimeGuestFn fn = family_dispatch(pc, thumb)) return fn;
    } else if (RuntimeGuestFn fn = image_dispatch(pc, thumb)) {
        return fn;
    } else if (RuntimeGuestFn fn = family_dispatch(pc, thumb)) {
        return fn;
    }
    if (runtime_has_static_entry(pc, thumb)) return nullptr;
    // Same tier runtime_dispatch consults when no hook is installed.
    return gbarecomp::overlay_resolve(pc, thumb);
}

// The data unpacker (The Broken Seal's Func_2808, 0x08012B74 here) is copied
// behind a `bx pc` stub; its ARM entry is watched like The Broken Seal's
// (src/unpacker_guard.h). The guard's own first-word test keeps this cheap.
void unpacker_watch_return(uint32_t return_pc, uint32_t) {
    gsr::unpacker_guard_note_return(return_pc);
}

RuntimeGuestFn tla_ram_dispatch_watched(uint32_t pc, int thumb) {
    if (thumb) return tla_ram_dispatch(pc, thumb);
    gsr::unpacker_guard_note_entry(pc);
    const RuntimeGuestFn fn = tla_ram_dispatch(pc, thumb);
    if (gsr::g_unpacker_guard_pending &&
        gsr::unpacker_guard_note_dispatch(pc, fn, fn, gsr::RamResolvePath::None))
        return &unpacker_redone;  // redone; g_cpu is at the caller
    return fn;
}

// Every generated RAM function checks its own bytes (runtime_ram_code_guard)
// on entry, so a written page never needs a second identity check here.
int tla_ram_identity_confirmed(uint32_t, int) { return 1; }

}  // namespace

void tla_install_ram_images() {
    g_runtime_ram_dispatch_hook = tla_ram_dispatch_watched;
    g_runtime_call_return_hook = unpacker_watch_return;
    gsr::unpacker_guard_install(0x08012B74u);  // Func_2808 (FACTS.md 2026-10-14)
    g_runtime_ram_identity_confirmed_hook = tla_ram_identity_confirmed;
}
