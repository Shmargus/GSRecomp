// New Game onboarding, both games (Jimmy, 7 November 2026;
// docs/NEXT_PRIORITY_MODES.md section 6): one question at a time, each
// highlighted option explained by the scrolling help bar.
//   Names (after the hero's name) -> Difficulty -> Randomize -> Stats ->
//   Scaling -> Shop prices -> Fair Play (a tick per randomized thing) ->
//   Code (text box, else letter grid) ->
//   Challenge -> picks ->
//   Last choices (The Broken Seal: Seamless Mode, Skip Prologue; The Lost
//   Age: skip Venus Lighthouse and Idejima) ->
//   Autosave, the last question in both games.
// Follow-ups appear only when they matter. B goes back one question. The
// logic is here; each game paints View with its own window and font and
// applies Result.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "challenge_format.h"
#include "run_code.h"

namespace gsr::new_game {

enum class Stage {
    Names, Difficulty, Randomize, Stats, Scaling, Prices, FairPlay, Code, CodeEntry, Challenge, Picks,
    Extras, Autosave, Done
};
enum Difficulty { kNormal, kHard, kScaled, kHardScaled };
enum Random { kWeapons, kArmor, kItems, kCoins, kPsynergy, kRandomCount };
// Fair Play's switches (Jimmy, 10 November 2026): each strives for a fair
// progression in its own thing, and shows only when that thing is randomized.
enum Fair { kFairItems, kFairCoins, kFairShops, kFairPsynergy, kFairCount };
constexpr const char* kFairLabels[kFairCount] = {"Items", "Coins", "Shops", "Psynergy"};
constexpr const char* kFairHelp[kFairCount] = {
    "Healing items and gear are spread through the journey, and gear gets stronger as you go.",
    "Coin spots grow with the journey, as the game's own do; no jackpots.",
    "New stock is around each shop's usual level.",
    "A starting hero learns Cure or Ply at level 1.",
};

// What differs between the games.
struct Options {
    // Random Treasure (The Broken Seal): the Items and Coins rows, and
    // Weapons and Armor change what chests and gifts give, with the stat
    // shuffle a follow-up (Jimmy, 7 November 2026). Without it (The Lost
    // Age, for now) Weapons and Armor are the stat shuffle.
    bool items_and_coins = true;
    bool extras = false;           // the Last choices page
    // Its rows: Seamless Mode (The Broken Seal only), then the game's skip.
    bool extras_seamless = true;
    const char* skip_label = "Skip Prologue and Sol Sanctum";
    const char* skip_help = "Begin on the world map outside Vale, with Garet, as the journey starts.";
    bool seamless = false;         // Seamless Mode's current setting
    bool autosave = false;         // Autosave's current setting (autosave.h)
    bool transferred = false;      // data from Golden Sun: no randomizing, no challenge
    int challenge_sizes = 3;       // Solo, Duo, Triple (+ No 2nd party)
    std::vector<unsigned> heroes;  // pick order, character ids
    std::function<std::string(unsigned)> hero_name;
    std::function<std::uint32_t()> fresh_seed;
    std::function<std::string(const char*)> text;  // the game's translation
    // "Type a seed" in the system's text box (code_box.h): true with a valid
    // code, false on Cancel. Unset: the letter grid.
    std::function<bool(run_code::Run*)> ask_code;
};

struct Result {
    int difficulty = kNormal;
    bool random[kRandomCount] = {};
    bool stats = false;  // the ticked pieces' stats shuffle too
    bool scaling = false, bargains = false;
    bool fair[kFairCount] = {true, true, true, true};  // Fair Play's ticks, all on to start
    // A switch shows, and its tick counts, only while its thing is
    // randomized (shops go with Items).
    bool fair_shown(Fair f) const {
        return f == kFairPsynergy ? random[kPsynergy] : f == kFairCoins ? random[kCoins] : random[kItems];
    }
    bool fair_on(Fair f) const { return fair_shown(f) && fair[f]; }
    std::uint32_t seed = 0;
    gsr::challenge::Config challenge;
    bool seamless = false, skip_opening = false;
    bool autosave = false;
    bool name_others = false;  // the Names answer

    bool any_random() const {
        for (const bool r : random)
            if (r) return true;
        return false;
    }
    run_code::Run run() const {
        namespace rc = run_code;
        rc::Run r{seed, 0};
        if (random[kWeapons]) r.choices |= rc::kWeapons;
        if (random[kArmor]) r.choices |= rc::kArmor;
        if (random[kItems]) r.choices |= rc::kItems;
        if (random[kCoins]) r.choices |= rc::kCoins;
        if (random[kPsynergy]) r.choices |= rc::kPsynergy;
        const bool equip = random[kWeapons] || random[kArmor];
        if (equip && stats) r.choices |= rc::kStats;
        if (equip && stats && scaling) r.choices |= rc::kScaling;
        if (random[kItems] && bargains) r.choices |= rc::kBargains;
        if (fair_on(kFairPsynergy)) r.choices |= rc::kFairPsynergy;
        if (fair_on(kFairItems)) r.choices |= rc::kFairItems;
        if (fair_on(kFairCoins)) r.choices |= rc::kFairCoins;
        if (fair_on(kFairShops)) r.choices |= rc::kFairShops;
        return r;
    }
    void take(const run_code::Run& r) {
        namespace rc = run_code;
        seed = r.seed;
        random[kWeapons] = r.choices & rc::kWeapons;
        random[kArmor] = r.choices & rc::kArmor;
        random[kItems] = r.choices & rc::kItems;
        random[kCoins] = r.choices & rc::kCoins;
        random[kPsynergy] = r.choices & rc::kPsynergy;
        stats = r.choices & rc::kStats;
        scaling = r.choices & rc::kScaling;
        bargains = r.choices & rc::kBargains;
        fair[kFairPsynergy] = r.choices & rc::kFairPsynergy;
        fair[kFairItems] = r.choices & rc::kFairItems;
        fair[kFairCoins] = r.choices & rc::kFairCoins;
        fair[kFairShops] = r.choices & rc::kFairShops;
    }
};

// One row: a label, an optional box (-1 none, 0 empty, 1 ticked; locked
// rows draw greyed) and the help line.
struct Row {
    std::string label;
    int box = -1;
    bool locked = false;
    std::string help;
};

struct View {
    std::string title, subtitle;  // subtitle: a second title line (the code)
    std::vector<Row> rows;
    int cursor = 0;
    int gap_before = -1;          // a little space above this row (Continue, Begin)
    const run_code::Entry* entry = nullptr;  // the letter grid instead of rows
};

class Flow {
public:
    // The Names question alone, after the hero's name.
    void open_names(const Options& o) {
        options_ = o;
        result_ = {};
        open_ = true;
        stage_ = Stage::Names;
        history_.clear();
        cursor_ = 0;
    }
    // The rest, from Difficulty.
    void open(const Options& o) {
        options_ = o;
        result_ = {};
        result_.seamless = o.seamless;
        result_.autosave = o.autosave;
        result_.seed = o.fresh_seed ? o.fresh_seed() : 1u;
        open_ = true;
        history_.clear();
        go(Stage::Difficulty);
    }
    bool is_open() const { return open_; }
    Stage stage() const { return stage_; }
    const Result& result() const { return result_; }
    // Frames are counted by the game; this says when the cursor last moved.
    bool moved() {
        const bool m = moved_;
        moved_ = false;
        return m;
    }

    // New presses (KEYINPUT bits, active high).
    void keys(std::uint16_t down) {
        constexpr std::uint16_t kA = 1, kB = 2, kStart = 8, kRight = 0x10, kLeft = 0x20, kUp = 0x40, kDown = 0x80;
        if (!open_) return;
        if (stage_ == Stage::CodeEntry) {
            auto& e = entry_;
            const int dx = (down & kRight) ? 1 : (down & kLeft) ? -1 : 0;
            const int dy = (down & kDown) ? 1 : (down & kUp) ? -1 : 0;
            if (dx || dy) e.move(dx, dy);
            run_code::Run run;
            switch (e.press(down & kA, down & kB, down & kStart, &run)) {
            case run_code::Result::Accepted:
                result_.take(run);
                go(Stage::Challenge);  // the code holds the follow-ups' answers
                return;
            case run_code::Result::Random:
                if (options_.fresh_seed) result_.seed = options_.fresh_seed();
                back();
                return;
            case run_code::Result::Cancel: back(); return;
            default: return;
            }
        }
        const int rows = static_cast<int>(view().rows.size());
        if (rows > 0 && (down & (kUp | kDown))) {
            cursor_ = (cursor_ + ((down & kDown) ? 1 : rows - 1)) % rows;
            moved_ = true;
        }
        if (down & kB) return back();
        if (down & kStart) {
            if (stage_ == Stage::Randomize || stage_ == Stage::Extras) {
                cursor_ = rows - 1;  // Continue / Begin
                return choose();
            }
        }
        if (down & kA) choose();
    }

    View view() const {
        View v;
        v.cursor = cursor_;
        const auto t = [&](const char* s) { return options_.text ? options_.text(s) : std::string(s); };
        const auto row = [&](const char* label, const char* help, int box = -1, bool locked = false) {
            v.rows.push_back({t(label), box, locked, t(help)});
        };
        switch (stage_) {
        case Stage::Names:
            v.title = t("Name the other characters?");
            row("Yes", "Name each of the other heroes now, one after another.");
            row("No", "Keep their usual names and go on.");
            break;
        case Stage::Difficulty:
            v.title = t("Difficulty");
            row("Normal", "The game as it was made.");
            row("Hard", "Enemies get more HP, Attack and Defence. Change it in Options.");
            row("Scaled", "Every enemy weaker than your party rises to its level, as in the Arena.");
            row("Hard Scaled", "Hard's stronger enemies, and every weaker one rises to your party's level.");
            break;
        case Stage::Randomize: {
            v.title = t("Randomize?");
            const bool locked = options_.transferred;
            if (options_.items_and_coins) {
                row("Weapons", "Weapon finds become random weapons from the whole game, hidden anywhere.",
                    result_.random[kWeapons], locked);
                row("Armor", "Armour finds become random pieces from the whole game, hidden anywhere.",
                    result_.random[kArmor], locked);
            } else {
                row("Weapons", "Every weapon gets another weapon's stats and a random unleash.",
                    result_.random[kWeapons], locked);
                row("Armor", "Every armour piece gets another piece's stats.", result_.random[kArmor], locked);
            }
            if (options_.items_and_coins) {
                row("Items", "Chests, gifts and shops change; story items stay.",
                    result_.random[kItems], locked);
                row("Coins", "Every coin spot gets a new amount, from 1 coin to the coin cap.",
                    result_.random[kCoins], locked);
            }
            row("Psynergy", "Random battle Psynergy at random levels. Field Psynergy comes in time.",
                result_.random[kPsynergy], locked);
            row("Continue", locked ? "Not with data transferred from Golden Sun." : "Go on with these choices.");
            v.gap_before = static_cast<int>(v.rows.size()) - 1;
            break;
        }
        case Stage::Stats:
            v.title = t("Shuffle stats?");
            row("Off", "Every piece keeps its own stats and unleash.");
            row("On", "Every weapon also gets another weapon's stats and a random unleash; armour another piece's stats.");
            break;
        case Stage::Scaling:
            v.title = t("Equipment Scaling");
            row("Off", "Any weapon or armour piece can end up far stronger or weaker than usual.");
            row("On", "Each piece keeps its usual strength; its new extras scale to match.");
            break;
        case Stage::Prices:
            v.title = t("Shop prices");
            row("Normal", "Shops charge the game's own prices.");
            row("Bargains", "Every item costs 5 to 1000 coins, cheaper the earlier it usually appears.");
            break;
        case Stage::FairPlay:
            v.title = t("Fair Play");
            for (const Fair f : fair_rows()) row(kFairLabels[f], kFairHelp[f], result_.fair[f]);
            row("Continue", "Go on with these choices.");
            v.gap_before = static_cast<int>(v.rows.size()) - 1;
            break;
        case Stage::Code:
            v.title = t("Seed");
            v.subtitle = run_code::spaced(run_code::encode(result_.run()));
            row("Play this seed", "Share it: anyone who types it at New Game gets the same run.");
            row("Type a seed", "Type a friend's seed to play their run.");
            row("New seed", "Roll a different run with the same choices.");
            break;
        case Stage::CodeEntry:
            v.entry = &entry_;
            break;
        case Stage::Challenge: {
            v.title = t("Challenge run?");
            const char* help = options_.transferred ? "Not with data transferred from Golden Sun."
                                                    : "Only the heroes you choose fight; the others stay down.";
            row("No", "Play with the whole party.");
            row("Solo", help);
            row("Duo", help);
            row("Triple", help);
            if (options_.challenge_sizes > 3) row("No 2nd party", help);
            if (options_.transferred)
                for (std::size_t i = 1; i < v.rows.size(); ++i) v.rows[i].locked = true;
            break;
        }
        case Stage::Picks: {
            const int size = result_.challenge.size;
            static constexpr const char* kSizes[] = {"Solo", "Duo", "Triple", "No 2nd party"};
            v.title = t(kSizes[std::max(0, size_ - 1)]);
            v.subtitle = t(size_ == 1 ? "Choose your hero." : "Choose your heroes.");
            for (const unsigned id : options_.heroes) {
                std::string name = options_.hero_name ? options_.hero_name(id) : std::to_string(id);
                for (int i = 0; i < size; ++i)
                    if (result_.challenge.picks[static_cast<std::size_t>(i)] == id)
                        name = std::to_string(i + 1) + ". " + name;
                v.rows.push_back({name, -1, false, t("The first one chosen leads.")});
            }
            break;
        }
        case Stage::Extras:
            v.title = t("Last choices");
            if (options_.extras_seamless)
                row("Seamless Mode", "At the end, save and go straight on to The Lost Age. Change it with F1.",
                    result_.seamless);
            row(options_.skip_label, options_.skip_help, result_.skip_opening);
            row("Continue", "Go on with these choices.");
            v.gap_before = static_cast<int>(v.rows.size()) - 1;  // above Continue
            break;
        case Stage::Autosave:
            v.title = t("Autosave");
            row("Off", "Only your own saves. Change it with F1.");
            row("On", "Each time you enter a new area, save to the Autosave file at the top of the list. Change it with F1.");
            break;
        case Stage::Done: break;
        }
        return v;
    }

private:
    bool random_any() const { return result_.any_random(); }
    void go(Stage s) {
        if (stage_ != s && open_ && stage_ != Stage::Done) history_.push_back({stage_, cursor_});
        stage_ = s;
        cursor_ = 0;
        moved_ = true;
        if (s == Stage::CodeEntry) entry_.open();
        // Start each question on its current answer.
        switch (s) {
        case Stage::Stats: cursor_ = result_.stats; break;
        case Stage::Scaling: cursor_ = result_.scaling; break;
        case Stage::Prices: cursor_ = result_.bargains; break;
        case Stage::Difficulty: cursor_ = result_.difficulty; break;
        case Stage::Autosave: cursor_ = result_.autosave; break;
        default: break;
        }
    }
    void finish() {
        open_ = false;
        stage_ = Stage::Done;
    }
    void back() {
        if (stage_ == Stage::Picks && result_.challenge.size > 0) {
            result_.challenge.picks[static_cast<std::size_t>(--result_.challenge.size)] = gsr::challenge::kNone;
            return;
        }
        if (history_.empty()) return;  // naming is over: nothing to go back to
        const auto [s, c] = history_.back();
        history_.pop_back();
        stage_ = s;
        cursor_ = c;
        moved_ = true;
    }
    // Fair Play's rows: the switches whose thing is randomized.
    std::vector<Fair> fair_rows() const {
        std::vector<Fair> rows;
        for (int f = 0; f < kFairCount; ++f)
            if (result_.fair_shown(static_cast<Fair>(f))) rows.push_back(static_cast<Fair>(f));
        return rows;
    }
    // The question after `from`, skipping the ones that do not matter.
    Stage next(Stage from) const {
        const bool equip = result_.random[kWeapons] || result_.random[kArmor];
        switch (from) {
        case Stage::Difficulty: return options_.transferred ? Stage::Challenge : Stage::Randomize;
        case Stage::Randomize:
            if (equip) return options_.items_and_coins ? Stage::Stats : Stage::Scaling;
            [[fallthrough]];
        case Stage::Stats:
            if (equip && result_.stats) return Stage::Scaling;
            [[fallthrough]];
        case Stage::Scaling:
            if (result_.random[kItems]) return Stage::Prices;
            [[fallthrough]];
        case Stage::Prices:
            if (!fair_rows().empty()) return Stage::FairPlay;
            [[fallthrough]];
        case Stage::FairPlay:
            if (random_any()) return Stage::Code;
            [[fallthrough]];
        case Stage::Code:
        case Stage::CodeEntry: return Stage::Challenge;
        case Stage::Challenge:
        case Stage::Picks: return options_.extras ? Stage::Extras : Stage::Autosave;
        case Stage::Extras: return Stage::Autosave;
        default: return Stage::Done;
        }
    }
    void advance() {
        const Stage n = next(stage_);
        if (n == Stage::Done) return finish();
        go(n);
    }
    void choose() {
        const int c = cursor_;
        switch (stage_) {
        case Stage::Names:
            result_.name_others = c == 0;
            return finish();
        case Stage::Difficulty:
            result_.difficulty = c;
            return advance();
        case Stage::Randomize: {
            const int rows = options_.items_and_coins ? 5 : 3;
            if (c < rows) {
                if (options_.transferred) return;
                static constexpr Random kAll[] = {kWeapons, kArmor, kItems, kCoins, kPsynergy};
                static constexpr Random kSome[] = {kWeapons, kArmor, kPsynergy};
                const Random r = options_.items_and_coins ? kAll[c] : kSome[c];
                result_.random[r] = !result_.random[r];
                return;
            }
            // Without Random Treasure the ticks are the stat shuffle.
            if (!options_.items_and_coins) result_.stats = result_.random[kWeapons] || result_.random[kArmor];
            return advance();
        }
        case Stage::Stats: result_.stats = c == 1; return advance();
        case Stage::Scaling: result_.scaling = c == 1; return advance();
        case Stage::Prices: result_.bargains = c == 1; return advance();
        case Stage::FairPlay: {
            const std::vector<Fair> rows = fair_rows();
            if (c >= static_cast<int>(rows.size())) return advance();
            result_.fair[rows[static_cast<std::size_t>(c)]] = !result_.fair[rows[static_cast<std::size_t>(c)]];
            return;
        }
        case Stage::Code:
            if (c == 1) {
                if (!options_.ask_code) return go(Stage::CodeEntry);
                run_code::Run run;
                moved_ = true;
                if (!options_.ask_code(&run)) return;
                result_.take(run);
                return go(Stage::Challenge);  // the code holds the follow-ups' answers
            }
            if (c == 2) {
                if (options_.fresh_seed) result_.seed = options_.fresh_seed();
                moved_ = true;
                return;
            }
            return advance();
        case Stage::Challenge:
            if (c > 0 && options_.transferred) return;
            result_.challenge = {};
            size_ = c;
            if (c == 0) return advance();
            return go(Stage::Picks);
        case Stage::Picks: {
            auto& ch = result_.challenge;
            const auto id = static_cast<std::uint8_t>(options_.heroes[static_cast<std::size_t>(c)]);
            for (int i = 0; i < ch.size; ++i)
                if (ch.picks[static_cast<std::size_t>(i)] == id) return;
            ch.picks[static_cast<std::size_t>(ch.size++)] = id;
            if (ch.size == size_) return advance();
            return;
        }
        case Stage::Extras: {
            const int skip = options_.extras_seamless ? 1 : 0;
            if (c < skip) {
                result_.seamless = !result_.seamless;
                // A challenge cannot go on to The Lost Age.
                if (result_.seamless && result_.challenge.size > 0) result_.seamless = false;
                return;
            }
            if (c == skip) { result_.skip_opening = !result_.skip_opening; return; }
            return advance();
        }
        case Stage::Autosave:
            result_.autosave = c == 1;
            return finish();
        default: return;
        }
    }

    Options options_;
    Result result_;
    bool open_ = false, moved_ = false;
    Stage stage_ = Stage::Done;
    int cursor_ = 0, size_ = 0;
    std::vector<std::pair<Stage, int>> history_;
    run_code::Entry entry_;
};

}  // namespace gsr::new_game
