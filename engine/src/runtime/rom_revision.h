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

// Revisions after the first that a row carries.
inline constexpr unsigned kLocalized = 4;

struct Row {
    std::uint32_t base;
    std::uint32_t localized[kLocalized];
};
struct OverlayRow {
    const char* overlay;
    std::uint32_t base;
    std::uint32_t localized[kLocalized];
};

// The index of `sha1` in `sha1s`; aborts on a revision the build does not know.
template <std::size_t N>
unsigned identify(const char* const (&sha1s)[N], const char* sha1, const char* tag) {
    static_assert(N == kLocalized + 1, "one SHA-1 per revision");
    for (unsigned i = 0; i < N; ++i)
        if (std::strcmp(sha1s[i], sha1) == 0) return i;
    std::fprintf(stderr, "[%s] the game library was generated from ROM %s, "
                         "which this build does not know\n", tag, sha1);
    std::abort();
}

[[noreturn]] inline void unmapped(const char* tag, const char* revision_name,
                                  const char* what, std::uint32_t base) {
    std::fprintf(stderr, "[%s] %s 0x%08X has no measured %s counterpart\n",
                 tag, what, base, revision_name);
    std::abort();
}

// `rows` sorted by base (generated address tables are).
template <std::size_t N>
const Row* find_sorted(const Row (&rows)[N], std::uint32_t base) {
    const Row* it = std::lower_bound(std::begin(rows), std::end(rows), base,
        [](const Row& row, std::uint32_t value) { return row.base < value; });
    return it != std::end(rows) && it->base == base ? it : nullptr;
}

template <std::size_t N>
const Row* find(const Row (&rows)[N], std::uint32_t base) {
    for (const Row& row : rows)
        if (row.base == base) return &row;
    return nullptr;
}

template <std::size_t N>
const OverlayRow* find(const OverlayRow (&rows)[N], const char* overlay, std::uint32_t base) {
    for (const OverlayRow& row : rows)
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
