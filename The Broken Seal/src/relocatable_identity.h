// relocatable_identity.h — code-only identity hashing for RAM code images.
//
// GS-011 established the defect class (docs/GS011_TRANSIENT_IMAGES.md,
// "Hashing data as if it were code"): an image's identity hash must cover
// bytes the copy's WRITER leaves alone, not the whole copied extent. Every
// fixed-address and relocatable image so far paid for that by trimming a
// trailing literal pool off the hashed extent. Func_b5138 self-relocates a
// jump table into the MIDDLE of its own extent (declared [[data_range]]
// blocks that are not at the tail), so trimming the extent cannot express
// the fix — the hash has to skip interior holes instead.
//
// This header is the shared, testable core of that skip: pure logic over a
// byte reader, no guest-bus dependency, so it can be exercised with a
// synthetic buffer in a unit test as well as with bus_read_u8 at runtime.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "sha1.h"

namespace gsr {

// A byte range excluded from a code-identity hash, expressed as [start, end)
// offsets from the image's base (i.e. from wherever it is currently
// resident) — the same offsets a declared [[data_range]] uses relative to
// the image's origin, since a relocatable image's data ranges move with the
// base by construction.
struct ByteRange {
    std::uint32_t start;  // inclusive
    std::uint32_t end;    // exclusive
};

inline bool byte_range_excludes(std::uint32_t offset, const ByteRange* ranges,
                                 unsigned len) {
    for (unsigned i = 0; i < len; ++i) {
        if (offset >= ranges[i].start && offset < ranges[i].end) return true;
    }
    return false;
}

// SHA-1 over [0, size) read through `read_byte(offset)`, skipping every
// offset covered by `excluded`. A byte inside an excluded range never
// affects the result; any byte outside one does. With excluded_len == 0 this
// is exactly a hash of the whole extent, so callers with no declared data
// range see no behavior change.
template <typename ByteReader>
std::string sha1_excluding(std::uint32_t size, ByteReader&& read_byte,
                            const ByteRange* excluded, unsigned excluded_len) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(size);
    for (std::uint32_t offset = 0; offset < size; ++offset) {
        if (byte_range_excludes(offset, excluded, excluded_len)) continue;
        bytes.push_back(static_cast<std::uint8_t>(read_byte(offset)));
    }
    return gba::sha1(bytes.data(), bytes.size()).hex();
}

// --- Page-epoch cache core (GS-cost: relocatable dispatch rehash) --------
//
// A relocatable image's identity hash above is correct but unconditional:
// callers who already proved it true for the current (image, base) pay a
// full SHA-1 again on every dispatch even when nothing changed. The
// fixed-address sibling mechanism (verified_ram_dispatch /
// g_verified_identity_cache in runner_main.cpp) solves the equivalent
// problem with a monotonic per-4KiB-page write counter: skip the hash while
// every page the image occupies still reads the same generation it did at
// the last verified hash.
//
// That is only sound if EVERY store that can change a tracked byte also
// bumps the page's generation. gbarecomp's write path
// (gba_bus.cpp / runtime_arm.h bus_write_u8/16/32) bumps the generation only
// for addresses inside a page the game runner has registered in
// g_ram_code_page_mask_*; a byte write to an unregistered page - including
// one made by DMA, which is otherwise routed through the very same
// bus_write* calls - never advances the epoch that page's cache entries
// compare against. Golden Sun's relocatable images are copied to stack-
// relative or allocator-relative IWRAM addresses that are NOT enumerable in
// advance (see kRelocatableCodeImages' "bounded only by the call graph"
// comments in runner_main.cpp), so the mask cannot be computed once at
// startup the way it is for the ~19 fixed-address images. The functions
// below let a caller register exactly the pages a base is about to be
// cached at, at the moment it starts trusting the cache for it - so mask
// coverage is proven for every base actually used, never guessed for bases
// that might be used.
//
// Everything here is pure (no guest-bus access), parameterized over the
// epoch/mask storage by templates, so it is exercisable with a synthetic
// page-epoch array in a unit test exactly like sha1_excluding above.

// Region-relative page bounds for [start, end). `mask` selects the
// mirroring window: 0x7FFF for IWRAM (region 0x03, 8 4KiB pages), 0x3FFFF
// for anything else (EWRAM, region 0x02, 64 4KiB pages) - the same split
// gba_bus.cpp's note_ram_code_write_bus uses.
inline void ram_range_page_bounds(std::uint32_t start, std::uint32_t end,
                                   std::uint32_t* mask, std::uint32_t* first,
                                   std::uint32_t* last) {
    const std::uint32_t region = start >> 24;
    *mask = (region == 0x03u) ? 0x00007FFFu : 0x0003FFFFu;
    *first = (start & *mask) >> 12;
    *last = ((end - 1u) & *mask) >> 12;
}

// True while every page-generation the cached [start, end) extent touches
// still equals what was recorded when the identity was last verified.
// `epoch_of(page_addr)` returns the live generation for the page containing
// `page_addr`; a real caller binds this to the guest's per-page counters.
template <typename EpochLookup>
bool ram_range_pages_current(std::uint32_t start, std::uint32_t end,
                              const std::array<unsigned int, 64>& page_epochs,
                              EpochLookup&& epoch_of) {
    if (end <= start) return false;
    std::uint32_t mask = 0, first = 0, last = 0;
    ram_range_page_bounds(start, end, &mask, &first, &last);
    for (std::uint32_t page = first; page <= last && page < 64u; ++page) {
        const std::uint32_t page_addr = (start & ~mask) | (page << 12);
        if (page_epochs[page] != epoch_of(page_addr)) return false;
    }
    return true;
}

// Snapshot the current per-page generations for [start, end) into
// `page_epochs`, clearing every page outside the extent first so a shrunk
// or moved cache entry cannot compare against a stale bit from a previous
// base.
template <typename EpochLookup>
void save_ram_range_page_epochs(std::uint32_t start, std::uint32_t end,
                                 std::array<unsigned int, 64>& page_epochs,
                                 EpochLookup&& epoch_of) {
    page_epochs.fill(0);
    if (end <= start) return;
    std::uint32_t mask = 0, first = 0, last = 0;
    ram_range_page_bounds(start, end, &mask, &first, &last);
    for (std::uint32_t page = first; page <= last && page < 64u; ++page) {
        const std::uint32_t page_addr = (start & ~mask) | (page << 12);
        page_epochs[page] = epoch_of(page_addr);
    }
}

// OR the pages [start, end) covers into the write-path masks, so every
// future write there is guaranteed to advance the epoch this cache compares
// against. Idempotent and monotonic (bits are only ever set), so calling it
// again for a base already registered is always safe.
inline void ram_range_register_mask(std::uint32_t start, std::uint32_t end,
                                     unsigned int* mask_iwram,
                                     unsigned long long* mask_ewram_lo,
                                     unsigned long long* mask_ewram_hi) {
    if (end <= start) return;
    const std::uint32_t last = end - 1u;
    const std::uint32_t region = start >> 24;
    if (region == 0x03u) {
        const std::uint32_t first_page = (start & 0x7FFFu) >> 12;
        const std::uint32_t last_page = (last & 0x7FFFu) >> 12;
        for (std::uint32_t page = first_page; page <= last_page && page < 8u;
             ++page) {
            *mask_iwram |= 1u << page;
        }
    } else if (region == 0x02u) {
        const std::uint32_t first_page = (start & 0x3FFFFu) >> 12;
        const std::uint32_t last_page = (last & 0x3FFFFu) >> 12;
        for (std::uint32_t page = first_page; page <= last_page && page < 64u;
             ++page) {
            if (page < 32u) *mask_ewram_lo |= 1ull << page;
            else *mask_ewram_hi |= 1ull << (page - 32u);
        }
    }
}

// Belt-and-suspenders DMA-bypass check, mirroring the fixed-address
// sibling's verified_identity_local_words_current: even with mask coverage
// proven above, re-read one or two words at the live PC's offset and compare
// them against the snapshot taken at the last full hash. Bytes inside a
// declared excluded (self-modified) range are skipped rather than compared,
// since those are expected to differ without invalidating the identity.
// Returns false (force a full recheck) whenever nothing conclusive could be
// checked, so a cache entry can never be trusted on the strength of an
// empty/OOB comparison.
template <typename ReadU32>
bool identity_local_words_current(std::uint32_t base, std::uint32_t pc,
                                   const std::uint8_t* snapshot,
                                   std::uint32_t snapshot_size,
                                   const ByteRange* excluded,
                                   unsigned excluded_len,
                                   ReadU32&& read_u32) {
    if (snapshot == nullptr || pc < base || (pc - base) >= snapshot_size) {
        return false;
    }
    const std::uint32_t offset = (pc - base) & ~3u;
    bool checked = false;
    for (std::uint32_t word = 0; word < 2u; ++word) {
        const std::uint32_t off = offset + word * 4u;
        if (off + 4u > snapshot_size) break;
        if (byte_range_excludes(off, excluded, excluded_len) ||
            byte_range_excludes(off + 3u, excluded, excluded_len)) {
            checked = true;
            continue;
        }
        const std::uint32_t live = read_u32(base + off);
        const std::uint32_t saved =
            static_cast<std::uint32_t>(snapshot[off]) |
            (static_cast<std::uint32_t>(snapshot[off + 1u]) << 8) |
            (static_cast<std::uint32_t>(snapshot[off + 2u]) << 16) |
            (static_cast<std::uint32_t>(snapshot[off + 3u]) << 24);
        if (live != saved) return false;
        checked = true;
    }
    return checked;
}

}  // namespace gsr
