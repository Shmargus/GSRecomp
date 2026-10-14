// Golden Sun: The Lost Age -- Encounters: Normal / Half / Off / Double
// (settings page two), as in Golden Sun 1.
//
// The field's encounter step 0x080C9FD8 is Golden Sun 1's 0x0808AE74 with
// moved offsets (FACTS.md, 2026-10-05). It works out the step's amount
// (call_via at 0x080CA0CC, result in r0), loads the base 0x02000240 at
// 0x080CA0CE, adds the amount to the counter at base + 0x258 (0x02000498)
// and starts a battle once the counter reaches the threshold at
// [[0x0300006C] + 0x198].
//
// Off and Half redirect that one base load so base + 0x258 is a zero word
// in the ROM header (0x080000B8): the step reads 0 and its store to ROM is
// dropped, so the counter does not move. Off does that on every step, Half
// on every other step. Double adds the step's amount to the counter first,
// so the game's own add makes it twice.

#include <cstdint>
#include <cstdio>

#include "gba_bus.h"
#include "runtime.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"
#include "tla.h"

namespace {

constexpr uint32_t kBaseLiteralPc = 0x080CA0CEu;
constexpr uint32_t kBase = 0x02000240u;
constexpr uint32_t kCounterOffset = 0x258u;
constexpr uint32_t kZeroWord = 0x080000B8u;
constexpr const char* kOptionsKey = gbarecomp::kOptionEncounterRate;

constexpr int kNormal = 0, kHalf = 1, kOff = 2, kDouble = 3, kCount = 4;

int g_rate = kNormal;
bool g_skip_next = false;
bool g_logged = false;
int (*g_next_literal)(uint32_t, uint32_t, uint32_t*) = nullptr;

int encounter_literal(uint32_t pc, uint32_t original, uint32_t* out_value) {
    if (pc != kBaseLiteralPc || original != kBase)
        return g_next_literal ? g_next_literal(pc, original, out_value) : 0;
    if (!g_logged) {
        g_logged = true;
        std::fprintf(stderr, "[encounters] step amount=0x%X counter=0x%X rate=%d\n",
                     g_cpu.R[0],
                     gbarecomp::active_bus()
                         ? gbarecomp::active_bus()->read32(kBase + kCounterOffset)
                         : 0u,
                     g_rate);
    }
    bool drop = false;
    if (g_rate == kOff) {
        drop = true;
    } else if (g_rate == kHalf) {
        drop = g_skip_next;
        g_skip_next = !g_skip_next;
    } else if (g_rate == kDouble) {
        if (gba::GbaBus* b = gbarecomp::active_bus()) {
            const uint32_t counter = kBase + kCounterOffset;
            b->write32(counter, b->read32(counter) + g_cpu.R[0]);
        }
    }
    if (!drop) return 0;
    *out_value = kZeroWord - kCounterOffset;
    return 1;
}

}  // namespace

int tla_encounter_rate() { return g_rate; }
int tla_encounter_rate_count() { return kCount; }

void tla_set_encounter_rate(int rate) {
    if (rate < 0 || rate >= kCount) return;
    g_rate = rate;
    gbarecomp::game_option_set(kOptionsKey, rate);
}

// Chains in front of the literal hook already in RunOptions.
void tla_install_encounters(gbarecomp::RunOptions& options) {
    const int saved = gbarecomp::game_option_get(kOptionsKey, kNormal);
    g_rate = saved >= 0 && saved < kCount ? saved : kNormal;
    g_next_literal = options.thumb_literal_override;
    options.thumb_literal_override = encounter_literal;
}
