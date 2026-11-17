#pragma once

// Which ROM revision a game library was generated from, and where a
// first-revision (USA/Europe) address, overlay address or text ID is in the
// loaded one. Generic: each game supplies its revision list and its measured
// tables (The Broken Seal src/rom_version.cpp, The Lost Age
// src/tla_rom_version.cpp); nothing game-specific lives here.
//
// A table row holds the first revision's value and its counterpart in every
// later revision, in revision order. Lookups abort loudly when a value has
// no measured counterpart: a guessed address is worse than a stop.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

namespace gbarecomp::rom_revision {

// A row carries a value for each revision after the first; K is how many.
// Each game's tables name their own width (a revision added to one game
// leaves the other's tables alone).
template <unsigned K>
struct RowN {
    std::uint32_t base;
    std::uint32_t localized[K];
};
template <unsigned K>
struct OverlayRowN {
    const char* overlay;
    std::uint32_t base;
    std::uint32_t localized[K];
};
// The five-language width (USA/Europe plus four localized revisions).
inline constexpr unsigned kLocalized = 4;
using Row = RowN<kLocalized>;
using OverlayRow = OverlayRowN<kLocalized>;

// One row of a game's revision list (its *rom_revisions.inc):
//   constexpr Revision kRevisions[] = {
//   #define GSR_ROM_REVISION(code, name, sha1, stamp_builder, stamp_allocator, language) \
//       {code, name, sha1, language},
//   #include "rom_revisions.inc"
//   #undef GSR_ROM_REVISION
//   };
// `language` is the code of the language row the revision plays as (a
// patched ROM plays as the language it patches).
struct Revision {
    const char* code;
    const char* name;
    const char* sha1;
    const char* language;
};

// The index of `sha1` in the list; aborts on a revision the build does not
// know. The caller's tables carry N - 1 localized values.
template <std::size_t N>
unsigned identify(const Revision (&revisions)[N], const char* sha1, const char* tag) {
    for (unsigned i = 0; i < N; ++i)
        if (std::strcmp(revisions[i].sha1, sha1) == 0) return i;
    std::fprintf(stderr, "[%s] the game library was generated from ROM %s, "
                         "which this build does not know\n", tag, sha1);
    std::abort();
}

// The row of the language revision `index` plays as.
template <std::size_t N>
unsigned language_row(const Revision (&revisions)[N], unsigned index, const char* tag) {
    for (unsigned i = 0; i < N; ++i)
        if (std::strcmp(revisions[i].code, revisions[index].language) == 0) return i;
    std::fprintf(stderr, "[%s] %s plays as unknown language %s\n", tag,
                 revisions[index].name, revisions[index].language);
    std::abort();
}

[[noreturn]] inline void unmapped(const char* tag, const char* revision_name,
                                  const char* what, std::uint32_t base) {
    std::fprintf(stderr, "[%s] %s 0x%08X has no measured %s counterpart\n",
                 tag, what, base, revision_name);
    std::abort();
}

// `row`'s value for revision `index` (not the first); aborts when the row
// is missing, i.e. `base` has no measured counterpart.
template <typename Row>
std::uint32_t counterpart(const Row* row, unsigned index, const char* tag,
                          const char* revision_name, const char* what,
                          std::uint32_t base) {
    if (!row) unmapped(tag, revision_name, what, base);
    return row->localized[index - 1];
}

// `rows` sorted by base (generated address tables are).
template <unsigned K, std::size_t N>
const RowN<K>* find_sorted(const RowN<K> (&rows)[N], std::uint32_t base) {
    const RowN<K>* it = std::lower_bound(std::begin(rows), std::end(rows), base,
        [](const RowN<K>& row, std::uint32_t value) { return row.base < value; });
    return it != std::end(rows) && it->base == base ? it : nullptr;
}

template <unsigned K, std::size_t N>
const RowN<K>* find(const RowN<K> (&rows)[N], std::uint32_t base) {
    for (const RowN<K>& row : rows)
        if (row.base == base) return &row;
    return nullptr;
}

template <unsigned K, std::size_t N>
const OverlayRowN<K>* find(const OverlayRowN<K> (&rows)[N], const char* overlay, std::uint32_t base) {
    for (const OverlayRowN<K>& row : rows)
        if (row.base == base && std::strcmp(row.overlay, overlay) == 0) return &row;
    return nullptr;
}

}  // namespace gbarecomp::rom_revision

// The value of `expr` (a lookup of a constant), computed once per call site.
#define GBARECOMP_ONCE_U32(expr)                                        \
    ([]() -> std::uint32_t {                                            \
        static const std::uint32_t gbarecomp_once_value = (expr);       \
        return gbarecomp_once_value;                                    \
    }())
