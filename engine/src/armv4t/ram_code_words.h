#pragma once
// Exact RAM-code write generations.
//
// The per-page epochs in runtime_arm.h advance on any write to a 4 KB page
// that holds RAM code, so data sharing a page with copied code (TLA battle
// data, the IWRAM stack beside 0x03007D88) made every identity check redo its
// full byte compare or CRC: ~15-20% of TLA Turbo battle time, session
// 20261030_115820. A checker that knows its exact byte range registers it
// here; the page's word epoch then advances only when a store touches a
// registered 4-byte word. Kept out of runtime_arm.h so generated code does
// not rebuild.
#include <cstdint>

extern "C" {
// Advanced by runtime_note_ram_code_write when a store touches a registered
// word, and for every page when a savestate replaces RAM.
extern unsigned int g_ram_code_word_epoch_ewram[64];
extern unsigned int g_ram_code_word_epoch_iwram[8];

// Marks every word overlapping [start, end) as code. Call before reading
// ram_code_word_epoch for the range. Registrations are never cleared.
void runtime_register_ram_code_words(uint32_t start, uint32_t end);

// The same for bytes a "nothing matched here" result depends on, kept apart
// so watching them never invalidates a code check. The caller registers the
// range's pages (g_ram_code_page_mask_*) too, so their stores are noted.
extern unsigned int g_ram_watch_word_epoch_ewram[64];
extern unsigned int g_ram_watch_word_epoch_iwram[8];
void runtime_register_ram_watch_words(uint32_t start, uint32_t end);
}

// The word epoch of the 4 KB page holding `addr` (EWRAM or IWRAM).
inline unsigned int ram_code_word_epoch(uint32_t addr) {
    return (addr >> 24) == 0x03u
        ? g_ram_code_word_epoch_iwram[(addr & 0x00007FFFu) >> 12]
        : g_ram_code_word_epoch_ewram[(addr & 0x0003FFFFu) >> 12];
}

// Sum of the watch-word epochs over [start, end)'s pages. Epochs only grow,
// so any watched store in the range changes the sum.
inline unsigned long long ram_watch_word_signature(uint32_t start, uint32_t end) {
    const bool iwram = (start >> 24) == 0x03u;
    const unsigned int* epochs =
        iwram ? g_ram_watch_word_epoch_iwram : g_ram_watch_word_epoch_ewram;
    const uint32_t mask = iwram ? 0x00007FFFu : 0x0003FFFFu;
    unsigned long long sum = 0;
    for (uint32_t page = (start & mask) >> 12; page <= ((end - 1u) & mask) >> 12; ++page)
        sum += epochs[page];
    return sum;
}
