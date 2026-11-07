// Radial Psynergy menu, one copy for both games. A mappable hotkey opens a
// ring of the party's field Psynergy icons; Left/Right cycles, A casts, the
// L/R buttons and the L2/R2 hotkeys make the highlighted one that button's
// field shortcut, B closes. Each Psynergy appears once; whoever in the party
// knows it and has the most PP casts it.
//
// Everything runs through the games' own field shortcut (FACTS "Radial
// Psynergy menu", 4 November 2026): both games keep L and R shortcut words in
// the save data (id | caster << 10) and read them in their field button poll,
// which hands the word to the shortcut cast routine. That routine queues the
// cast exactly as the Psynergy menu does. Here the poll's entry is the moment
// the player has control: the radial opens there and casts through that
// routine. L2/R2 are our own words in game_options.ini.
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include "auto_field_psynergy_labels.h"
#include "gba_bus.h"
#include "gba_ppu.h"
#include "game_hotkeys.h"
#include "game_options.h"
#include "gs_text_encode.h"
#include "retreat_travel.h"
#include "runtime.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"

namespace gsr::radial_psynergy {

// Auto Field Psynergy: A with nothing to talk to or check casts the field
// Psynergy the game itself would react to there. Every question goes to the
// game's own routines (FACTS "Auto Field Psynergy" in both games, November
// 2026), asked from the field poll's entry; the game files hold the calls.
struct AutoCast {
    std::uint32_t a_mask;          // halfword: the game's A button mask
    std::uint32_t pressed;         // word: buttons pressed this frame
    unsigned events;               // field + events: the room's event table
    bool (*a_acts)();              // A would talk or check something
    std::uint32_t (*find)(std::uint32_t category, unsigned kind);  // event or 0
    std::uint32_t categories[4];   // what the cast itself searches
    unsigned category_count;
    std::uint32_t anywhere;        // event word bit: reacts anywhere in the room
};

// A game routine's r0, or 0 when it did not return.
inline std::uint32_t guest(std::uint32_t pc, std::uint32_t r0 = 0, std::uint32_t r1 = 0,
                           std::uint32_t r2 = 0) {
    std::uint32_t result = 0;
    if (!gbarecomp::guest_call(pc | 1u, r0, r1, r2, 0, &result)) return 0;
    return result;
}

// A game's measured places (The Broken Seal: runner_main.cpp, The Lost Age:
// tla_radial_psynergy.cpp).
struct Game {
    std::uint32_t poll;            // field button poll entry; returns nonzero when it acted
    std::uint32_t field;           // the field state pointer the poll checks first (0: it returns)
    unsigned special;              // byte at field + special: the poll's every-button mode (0: none)
    std::uint32_t cast;            // shortcut cast routine(word)
    std::uint32_t dispatcher;      // field Psynergy dispatcher(id | caster << 10)
    std::uint32_t sound;           // play sound(id)
    std::uint32_t abilities;       // ability records
    unsigned ability_size;         // 16 / 12 bytes
    unsigned ability_count;        // the getter's bound
    unsigned kind_offset;          // field effect kind byte, 0 = none
    bool icon_halfword;            // icon index at +4: byte or halfword
    std::uint32_t icons;           // icon pointer table
    unsigned icon_count;
    std::uint32_t icon_palette;    // 16 BGR555 colours
    std::uint32_t records;         // character record 0
    std::uint32_t joined;          // flag bank byte: bit n = character n joined
    std::uint32_t order;           // party order bytes
    std::uint32_t shortcut_l, shortcut_r;
    std::uint32_t name_text;       // text line of ability 0's name
    std::size_t text_table, tree_table;
    void (*prepare_font)();        // the runner's menu font, before the box shows
    const AutoCast* auto_cast = nullptr;  // null: no Auto Field Psynergy
};

// Measured in both games (FACTS): 0x14C-byte character records, current HP
// +0x38, current PP +0x3A, 32 learned-ability words from +0x58 (id in the low
// 14 bits). Ability records: target byte +0 (2 = one ally: the heals, which
// the field menu casts on a party member, not here), icon +4, PP cost +9.
constexpr unsigned kRecordSize = 0x14C, kPp = 0x3A;
constexpr unsigned kAbilities = 0x58, kAbilitySlots = 32;
constexpr unsigned kTargetAlly = 2, kIconOffset = 4, kCostOffset = 9;
constexpr std::uint32_t kIdMask = 0x3FFFu;
// A shortcut / dispatcher word: id | caster << 10.
constexpr std::uint32_t kWordId = 0x3FFu;
constexpr unsigned kCasters = 8;

// Hotkeys (F1 > Hotkeys), unbound by default: an unbound menu is no feature.
constexpr const char* kOptionL2 = "PsynergyShortcutL2";
constexpr const char* kOptionR2 = "PsynergyShortcutR2";
// The last field Psynergy the party cast, any way: the ring opens on it.
constexpr const char* kOptionLast = "PsynergyLastCast";
enum Hotkey : unsigned { kMenu = 1, kL2 = 2, kR2 = 4 };

// GBA KEYINPUT bits.
constexpr unsigned kA = 1, kB = 2, kRight = 16, kLeft = 32, kR = 256, kL = 512;

struct Entry {
    unsigned id = 0, cost = 0;
    std::string name;
    std::array<std::uint8_t, 256> pixels{};  // 16x16 palette indices, 0 clear
};

inline const Game* game = nullptr;
inline std::mutex lock;                     // entries/cursor/open: emulation thread vs painter
inline std::vector<Entry> entries;
inline int cursor = 0;
inline bool open = false;
inline bool swallowing = false;
inline std::uint16_t held = 0;
inline unsigned requests = 0;               // hotkey presses for the next poll
// Chosen in the menu, done at the next poll (game memory is touched there).
inline unsigned cast_id = 0, set_l_id = 0, set_r_id = 0;
inline std::vector<unsigned> sounds;
inline bool busy = false;
inline std::atomic<std::uint32_t> frames{0};         // guest frames (keyinput runs once a frame)
inline std::uint32_t opened_at = 0;                  // frames when the ring opened
// Turning: the ring is drawn spin_from steps behind the cursor at spin_at,
// easing to none over kSpinFrames, so Left/Right turn it instead of jumping.
inline double spin_from = 0;
inline std::uint32_t spin_at = 0;
// A chosen feel, not a measured game timing.
constexpr unsigned kSpinFrames = 8;

inline double spin_offset(std::uint32_t now) {
    const double t = std::min(1.0, (now - spin_at) / double(kSpinFrames));
    const double left = 1 - t;
    return spin_from * left * left * left;
}
inline std::array<std::uint16_t, 16> palette{};
inline std::atomic<unsigned> hotkey_down{0}, hotkey_held{0};

// --- the game's icon decoder ------------------------------------------------
// The games' icon unpacker (The Broken Seal ARM 0x08015AFC, The Lost Age the
// same 0x278 bytes at 0x08038B18): LSB-first bits, a 16-entry move-to-front
// history (two words of nibbles), 256 pixel indices. Checked equal to the
// original routine for all 160 / 260 Psynergy icons and 371 TLA item icons.
inline bool decode_icon(gba::GbaBus& b, std::uint32_t src, std::array<std::uint8_t, 256>& out) {
    std::uint32_t ip = 1, at = src;
    if (src & 3u) {
        const std::uint32_t w = b.read32(src & ~3u);
        at = (src & ~3u) + 4u;
        ip = (0x80000000u | (w >> 1)) >> ((src & 3u) * 8u - 1u);
    }
    auto bit = [&]() -> bool {
        bool c = ip & 1u;
        ip >>= 1;
        if (!ip) {
            const std::uint32_t w = b.read32(at);
            at += 4u;
            c = w & 1u;
            ip = 0x80000000u | (w >> 1);
        }
        return c;
    };
    auto ror = [](std::uint32_t x, unsigned k) { return k ? (x >> k) | (x << (32u - k)) : x; };
    std::uint32_t high = 0xFEDCBA98u, low = 0x76543210u, rest = 0, value = 0;
    unsigned n = 0;
    // Nibble k of the low history moves to the front.
    auto from_low = [&](unsigned k) {
        rest = (low >> (4u * k + 4u)) | (low << (32u - 4u * k));
        value = (low >> (4u * k)) & 0xFu;
        low = value | ror(rest, 28u - 4u * k);
    };
    // Nibble k of the high history: taken out, the rest closes up.
    auto from_high = [&](unsigned k) {
        value = (high >> (4u * k)) & 0xFu;
        rest = k < 7u ? ror((high >> (4u * k + 4u)) | (high << (32u - 4u * k)), 28u - 4u * k)
                      : high << 4;
    };
    for (int guard = 0; guard < 4096; ++guard) {
        // One code: 0 repeats the current value; 1 then a choice of history slot.
        bool front_from_high = false;
        if (!bit()) {
            value = low & 0xFu;
            if (n >= out.size()) return false;
            out[n++] = static_cast<std::uint8_t>(value);
            continue;
        }
        if (!bit()) {
            if (!bit()) from_low(1);
            else if (bit()) from_low(3);
            else from_low(2);
        } else if (!bit()) {
            if (bit()) from_low(5);
            else from_low(4);
        } else if (!bit()) {
            from_low(6);
        } else if (!bit()) {
            if (bit()) { value = high & 0xFu; rest = high & ~0xFu; front_from_high = true; }
            else { low = ror(low, 28); value = low & 0xFu; }
        } else {
            front_from_high = true;
            if (!bit()) from_high(1);
            else if (!bit()) { if (!bit()) from_high(2); else from_high(3); }
            else if (!bit()) from_high(4);
            else if (!bit()) { if (!bit()) from_high(5); else from_high(6); }
            else if (bit()) return n == out.size();
            else from_high(7);
        }
        if (front_from_high) {
            high = rest | (low >> 28);
            low = value | (low << 4);
        }
        if (n >= out.size()) return false;
        out[n++] = static_cast<std::uint8_t>(value);
    }
    return false;
}

// --- the party --------------------------------------------------------------
inline std::uint32_t record(unsigned who) { return game->records + who * kRecordSize; }
inline std::uint32_t ability(unsigned id) { return game->abilities + id * game->ability_size; }

inline bool knows(gba::GbaBus& b, unsigned who, unsigned id) {
    for (unsigned i = 0; i < kAbilitySlots; ++i)
        if ((b.read16(record(who) + kAbilities + i * 4u) & kIdMask) == id) return true;
    return false;
}

// The party in order: the order bytes, one per joined character.
inline std::vector<unsigned> party(gba::GbaBus& b) {
    unsigned count = 0;
    const unsigned flags = b.read8(game->joined);
    for (unsigned i = 0; i < kCasters; ++i) count += (flags >> i) & 1u;
    std::vector<unsigned> out;
    for (unsigned i = 0; i < count; ++i) {
        const unsigned who = b.read8(game->order + i);
        if (who < kCasters) out.push_back(who);
    }
    return out;
}

// Whoever in the party knows `id` and has the most PP (first in order on a
// tie). Members who are down count too (Jimmy, 4 November 2026: Challenge
// Mode's left-out heroes stay down); the shortcut check the games make
// (0x08091814 / 0x080D2414) asks only joined and knows.
inline int caster(gba::GbaBus& b, unsigned id) {
    int best = -1;
    unsigned best_pp = 0;
    for (const unsigned who : party(b)) {
        if (!knows(b, who, id)) continue;
        const unsigned pp = b.read16(record(who) + kPp);
        if (best < 0 || pp > best_pp) { best = static_cast<int>(who); best_pp = pp; }
    }
    return best;
}

// Castable outside battle: the game's field effect kind is set and the
// target is not one ally (heals).
inline bool field_psynergy(gba::GbaBus& b, unsigned id) {
    if (!id || id >= game->ability_count) return false;
    return b.read8(ability(id) + game->kind_offset) != 0 && b.read8(ability(id)) != kTargetAlly;
}

inline std::string name_of(gba::GbaBus& b, unsigned id) {
    if (!b.rom_ptr()) return {};
    mods::TextEncoder text({b.rom_ptr(), b.rom_size()}, game->text_table, game->tree_table);
    std::string name;
    if (!text.decode(game->name_text + id, &name, 40)) return {};
    return name;
}

// The party's field Psynergy, each once, in id order.
inline std::vector<unsigned> castable(gba::GbaBus& b) {
    std::vector<unsigned> ids;
    for (const unsigned who : party(b))
        for (unsigned i = 0; i < kAbilitySlots; ++i) {
            const unsigned id = b.read16(record(who) + kAbilities + i * 4u) & kIdMask;
            if (field_psynergy(b, id) && caster(b, id) >= 0) ids.push_back(id);
        }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

inline std::vector<Entry> build(gba::GbaBus& b) {
    std::vector<Entry> out;
    for (const unsigned id : castable(b)) {
        Entry e;
        e.id = id;
        e.cost = b.read8(ability(id) + kCostOffset);
        e.name = name_of(b, id);
        unsigned icon = game->icon_halfword ? b.read16(ability(id) + kIconOffset)
                                            : b.read8(ability(id) + kIconOffset);
        if (icon >= game->icon_count) icon = 0;  // as the games' icon routines do
        if (!decode_icon(b, b.read32(game->icons + icon * 4u), e.pixels)) e.pixels.fill(0);
        out.push_back(std::move(e));
    }
    return out;
}

inline std::uint32_t word(gba::GbaBus& b, unsigned id) {
    const int who = caster(b, id);
    return who < 0 ? 0u : ((id & kWordId) | (static_cast<unsigned>(who) << 10));
}

// Which buttons hold `id` now, for the info box.
inline std::string buttons_for(gba::GbaBus& b, unsigned id) {
    std::string out;
    auto add = [&](bool on, const char* label) {
        if (!on) return;
        if (!out.empty()) out += ' ';
        out += label;
    };
    add((b.read16(game->shortcut_l) & kWordId) == id, "L");
    add((b.read16(game->shortcut_r) & kWordId) == id, "R");
    add(static_cast<unsigned>(gbarecomp::game_option_get(kOptionL2, 0)) == id, "L2");
    add(static_cast<unsigned>(gbarecomp::game_option_get(kOptionR2, 0)) == id, "R2");
    return out;
}

// Auto Field Psynergy, from the poll's entry: the shortcut word for A, or 0
// when A would talk, check or meet nothing that reacts. Room-wide events are
// left out, so A never casts just for being in a room.
inline std::uint32_t auto_word(gba::GbaBus& b, std::uint32_t field) {
    const AutoCast& a = *game->auto_cast;
    if (!(b.read16(a.a_mask) & b.read32(a.pressed))) return 0;
    if (!b.read32(field + a.events) || a.a_acts()) return 0;
    for (const unsigned id : castable(b)) {
        const unsigned kind = b.read8(ability(id) + game->kind_offset);
        for (unsigned i = 0; i < a.category_count; ++i) {
            const std::uint32_t event = a.find(a.categories[i], kind);
            if (event && !(b.read32(event) & a.anywhere)) return word(b, id);
        }
    }
    return 0;
}

// --- hooks ------------------------------------------------------------------
inline void reset() {
    std::lock_guard<std::mutex> guard(lock);
    open = swallowing = false;
    entries.clear();
    cursor = 0; held = 0; requests = 0; spin_from = 0;
    cast_id = set_l_id = set_r_id = 0;
    sounds.clear();
}

inline void hotkey(unsigned bit, bool down) {
    const unsigned before = hotkey_held.load();
    const unsigned now = down ? before | bit : before & ~bit;
    hotkey_held.store(now);
    if (down && !(before & bit)) hotkey_down.fetch_or(bit);
}

// Before the host window: the three hotkeys, unbound.
inline void install(const Game& g) {
    game = &g;
    reset();
    auto& keys = gbarecomp::game_hotkeys();
    keys.push_back({"PsynergyMenu", "Psynergy Menu", "", nullptr,
                    [](bool down) { hotkey(kMenu, down); }});
    keys.push_back({"PsynergyL2", "Psynergy Shortcut L2", "", nullptr,
                    [](bool down) { hotkey(kL2, down); }});
    keys.push_back({"PsynergyR2", "Psynergy Shortcut R2", "", nullptr,
                    [](bool down) { hotkey(kR2, down); }});
}

inline void play(unsigned id) { sounds.push_back(id); }

// Once a frame, before the game reads its buttons.
inline std::uint16_t keyinput(std::uint16_t value) {
    if (!game) return value;
    ++frames;
    namespace rt = gsr::retreat_travel;
    const unsigned down_keys = hotkey_down.exchange(0);
    const auto pressed = static_cast<std::uint16_t>(~value & 0x3FFu);
    std::lock_guard<std::mutex> guard(lock);
    if (open) {
        const unsigned down = pressed & ~held;
        held = pressed;
        const int count = static_cast<int>(entries.size());
        const unsigned id = count ? entries[cursor].id : 0u;
        // B, the menu hotkey again, or a cast closes it (Jimmy, 4 November
        // 2026); Start does not.
        if ((down & kB) || (down_keys & kMenu)) {
            open = false; swallowing = true; play(rt::kBack);
        } else if (down & kA) {
            cast_id = id;
            open = false; swallowing = true; play(rt::kSelect);
        } else if (down & (kL | kR)) {
            ((down & kL) ? set_l_id : set_r_id) = id;
            play(rt::kSelect);
        } else if (down_keys & (kL2 | kR2)) {
            if (down_keys & kL2) gbarecomp::game_option_set(kOptionL2, static_cast<int>(id));
            if (down_keys & kR2) gbarecomp::game_option_set(kOptionR2, static_cast<int>(id));
            play(rt::kSelect);
        } else if (count > 1 && (down & (kLeft | kRight))) {
            cursor = (down & kLeft) ? (cursor + count - 1) % count : (cursor + 1) % count;
            // Held from where it is now, so quick presses keep it turning.
            const std::uint32_t now = frames.load();
            spin_from = spin_offset(now) + ((down & kLeft) ? -1.0 : 1.0);
            spin_at = now;
            play(rt::kMove);
        }
        return 0x3FFu;
    }
    if (swallowing) {
        if (pressed) return 0x3FFu;
        swallowing = false;
    }
    // A press waits for this frame's field poll only; elsewhere it lapses.
    requests = down_keys;
    return value;
}

// Function entry. True when the poll was answered here. Game routines run
// with the lock released.
inline bool on_entry(std::uint32_t pc) {
    if (!game) return false;
    // Every field cast reaches the dispatcher with id | caster << 10: the
    // Psynergy menu, L/R, L2/R2 and the ring. Caster 8 and up are the game's
    // own requests (word 0x2000 | id), not the party's.
    if (pc == game->dispatcher) {
        const std::uint32_t word = g_cpu.R[0];
        if (((word >> 10) & 0xFu) < kCasters &&
            static_cast<unsigned>(gbarecomp::game_option_get(kOptionLast, 0)) != (word & kWordId))
            gbarecomp::game_option_set(kOptionLast, static_cast<int>(word & kWordId));
        return false;
    }
    if (busy || pc != game->poll) return false;
    auto* b = gbarecomp::active_bus();
    if (!b) return false;
    // Only where the poll itself would read the shortcut buttons.
    const std::uint32_t field = b->read32(game->field);
    if (!field || (game->special && b->read8(field + game->special))) return false;
    std::vector<unsigned> play_now;
    unsigned cast = 0, set_l = 0, set_r = 0, asked = 0;
    bool was_open = false;
    {
        std::lock_guard<std::mutex> guard(lock);
        play_now.swap(sounds);
        cast = cast_id; set_l = set_l_id; set_r = set_r_id;
        cast_id = set_l_id = set_r_id = 0;
        asked = requests;
        requests = 0;
        was_open = open;
    }
    busy = true;
    const auto sound = [&](unsigned id) {
        gbarecomp::guest_call(game->sound | 1u, id, 0, 0, 0, nullptr);
    };
    for (const unsigned s : play_now) sound(s);
    // L and R: the game's own shortcut words, the caster chosen now.
    if (set_l) if (const auto w = word(*b, set_l)) b->write16(game->shortcut_l, static_cast<std::uint16_t>(w));
    if (set_r) if (const auto w = word(*b, set_r)) b->write16(game->shortcut_r, static_cast<std::uint16_t>(w));
    std::uint32_t w = cast ? word(*b, cast) : 0u;
    if (!cast && !was_open && (asked & kMenu)) {
        auto built = build(*b);
        const bool any = !built.empty();
        if (any) {
            for (int i = 0; i < 16; ++i) palette[i] = b->read16(game->icon_palette + i * 2u);
            if (game->prepare_font) game->prepare_font();
        }
        {
            std::lock_guard<std::mutex> guard(lock);
            entries = std::move(built);
            const auto last = static_cast<unsigned>(gbarecomp::game_option_get(kOptionLast, 0));
            const auto it = std::find_if(entries.begin(), entries.end(),
                [&](const Entry& e) { return e.id == last; });
            cursor = it == entries.end() ? 0 : static_cast<int>(it - entries.begin());
            held = 0x3FFu;  // buttons already down do not act in the menu
            open = any;
            opened_at = frames.load();
            spin_from = 0;
        }
        sound(any ? retreat_travel::kSelect : retreat_travel::kBack);
    } else if (!cast && !was_open && (asked & (kL2 | kR2))) {
        const unsigned id = static_cast<unsigned>(gbarecomp::game_option_get(
            (asked & kL2) ? kOptionL2 : kOptionR2, 0));
        if (field_psynergy(*b, id)) {
            w = word(*b, id);
            if (!w) sound(retreat_travel::kBack);
        }
    } else if (!cast && !was_open && !asked && game->auto_cast &&
               gbarecomp::game_option_get(auto_field_psynergy::kOptionKey, 0)) {
        w = auto_word(*b, field);
    }
    bool answered = false;
    std::uint32_t result = 0;
    if (w && gbarecomp::guest_call(game->cast | 1u, w, 0, 0, 0, &result)) {
        gbarecomp::guest_return_now(pc, result);
        answered = true;
    }
    busy = false;
    return answered;
}

// --- painting ---------------------------------------------------------------
// Info box: native tiles, centred under the ring.
constexpr int kBoxTileX = 4, kBoxTileY = 17, kBoxTiles = 22, kBoxRows = 3;
constexpr int kCentreX = 120, kCentreY = 74;
constexpr int kMaxRadiusX = 108, kMaxRadiusY = 50, kSpacing = 22;

// box(tile_x, tile_y, tiles, rows, content): the runner's game window, which
// calls content(draw(s, x, y), width(s), inner_x, inner_y) in the game font.
// Native coordinates; the expanded view's margins are added here for the
// icons and by the runner for the box.
template <class Box>
bool paint(std::uint8_t* rgb, std::uint32_t width, std::uint32_t height, Box box) {
    std::lock_guard<std::mutex> guard(lock);
    if (!open || entries.empty() || !game) return false;
    int ox = 0, oy = 0;
    if (const gba::GbaPpu* ppu = gbarecomp::active_ppu()) {
        ox = static_cast<int>(ppu->view_extra_left());
        oy = static_cast<int>(ppu->view_extra_top());
    }
    // A native pixel's RGB, null off the picture.
    auto at = [&](int x, int y) -> std::uint8_t* {
        x += ox; y += oy;
        if (x < 0 || y < 0 || x >= static_cast<int>(width) || y >= static_cast<int>(height))
            return nullptr;
        return rgb + (static_cast<std::size_t>(y) * width + x) * 3u;
    };
    auto put = [&](int x, int y, std::uint8_t index) {
        std::uint8_t* p = index ? at(x, y) : nullptr;
        if (!p) return;
        const std::uint16_t c = palette[index & 0xFu];
        const unsigned r = c & 31u, g = (c >> 5) & 31u, bl = (c >> 10) & 31u;
        p[0] = static_cast<std::uint8_t>((r << 3) | (r >> 2));
        p[1] = static_cast<std::uint8_t>((g << 3) | (g >> 2));
        p[2] = static_cast<std::uint8_t>((bl << 3) | (bl >> 2));
    };
    // The pulse runs at half the battle target cursor's beat (it bobs once
    // every 8 frames, FACTS "The cursor bobs", F12 0062; Jimmy found that
    // too quick, 4 November 2026). Counted in game frames, so it pauses and
    // speeds up with the game. The highlighted
    // frame brightens and dims and its icon grows by up to two pixels a side.
    constexpr double kPi = 3.14159265358979;
    constexpr unsigned kBeat = 16;
    const double wave = 0.5 - 0.5 * std::cos(2 * kPi * (frames.load() % kBeat) / kBeat);
    // Each icon in its own box with cut corners: half-dark inside, light frame.
    auto icon = [&](const Entry& e, int cx, int cy, int size, bool selected) {
        const int half = size / 2 + 2;
        // Only the highlighted frame pulses; the others stay a steady grey.
        const int light = selected ? 176 + static_cast<int>(79 * wave) : 144;
        const auto edge = static_cast<std::uint8_t>(light);
        for (int y = -half; y < half; ++y)
            for (int x = -half; x < half; ++x) {
                // Distance into the box from its nearest corner, per axis.
                const int dx = std::min(x + half, half - 1 - x);
                const int dy = std::min(y + half, half - 1 - y);
                const int corner = dx < 3 && dy < 3 ? dx + dy : 99;
                if (corner < 2) continue;  // outside the rounded corner
                const bool frame = corner == 2 || dx == 0 || dy == 0;
                std::uint8_t* q = at(cx + x, cy + y);
                if (!q) continue;
                if (frame) { q[0] = q[1] = q[2] = edge; continue; }
                q[0] = static_cast<std::uint8_t>(q[0] / 2);
                q[1] = static_cast<std::uint8_t>(q[1] / 2);
                q[2] = static_cast<std::uint8_t>(q[2] / 2);
            }
        const int left = cx - size / 2, top = cy - size / 2;
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
                put(left + x, top + y, e.pixels[(y * 16 / size) * 16 + x * 16 / size]);
    };
    // The ring turns so the highlighted icon sits at the top, drawn double.
    const int count = static_cast<int>(entries.size());
    const double radius = std::max(28.0, count * kSpacing / (2.0 * kPi));
    const double rx = std::min<double>(kMaxRadiusX, radius);
    const double ry = std::min<double>(kMaxRadiusY, radius);
    // Opening: every icon swirls in together from beyond the screen edge,
    // half a turn, easing out over kOpenFrames game frames. The length is a
    // chosen feel, not a measured game timing.
    constexpr unsigned kOpenFrames = 18;
    constexpr double kSwirl = kPi;
    const double t = std::min(1.0, (frames.load() - opened_at) / double(kOpenFrames));
    const double ease = 1 - (1 - t) * (1 - t) * (1 - t);
    // Far enough out that the start is off any view (the widest is 480 wide).
    const double reach = 1 + (300.0 / std::min(rx, ry) - 1) * (1 - ease);
    const double turn = kSwirl * (1 - ease);
    const double spin = spin_offset(frames.load());
    // Steps from the top, wrapped to the nearer way round.
    auto steps = [&](int i) {
        double d = std::fmod(i - cursor + spin, double(count));
        if (d > count / 2.0) d -= count;
        if (d < -count / 2.0) d += count;
        return d;
    };
    auto place = [&](int i, int& x, int& y) {
        const double a = -kPi / 2 + 2 * kPi * steps(i) / count + turn;
        x = kCentreX + static_cast<int>(std::lround(rx * reach * std::cos(a)));
        y = kCentreY + static_cast<int>(std::lround(ry * reach * std::sin(a)));
    };
    // An icon grows as it turns into the top and shrinks as it leaves, so
    // the big one travels with the ring; nearer the top draws later.
    auto nearness = [&](int i) { return std::max(0.0, 1 - std::fabs(steps(i))); };
    std::vector<int> order(count);
    for (int i = 0; i < count; ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        if ((a == cursor) != (b == cursor)) return b == cursor;
        return nearness(a) < nearness(b);
    });
    for (const int i : order) {
        int x, y;
        place(i, x, y);
        const double near = nearness(i);
        const bool selected = i == cursor;
        const double grow = 8 + (selected ? 2 * wave : 0);
        icon(entries[i], x, y, 16 + 2 * static_cast<int>(std::lround(grow * near)), selected);
    }
    // Name, PP and the buttons it is on.
    const Entry& e = entries[cursor];
    std::string keys;
    if (auto* b = gbarecomp::active_bus()) keys = buttons_for(*b, e.id);
    box(kBoxTileX, kBoxTileY, kBoxTiles, kBoxRows, [&](auto& draw, auto& text_width, int x, int y) {
        const int inner = (kBoxTiles - 2) * 8;
        draw(e.name, x + 4, y);
        draw("PP " + std::to_string(e.cost), x + inner / 2 + 8, y);
        if (!keys.empty()) draw(keys, x + inner - 4 - static_cast<int>(text_width(keys)), y);
    });
    return true;
}

}  // namespace gsr::radial_psynergy
