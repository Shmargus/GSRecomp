// The run code's text box, both games (Jimmy, 8 November 2026): "Type a
// code" opens the system's own text box, so a code can be typed on the
// keyboard or pasted from the clipboard. It stays open until the code is
// valid or the player cancels. Where the system has none (Linux, for now),
// the letter grid (run_code.h Entry) remains.
#pragma once
#include <functional>
#include <string>

#include "run_code.h"

namespace gsr::code_box {

// The box's words, in the game's language.
struct Text {
    std::string title, prompt, invalid, ok, cancel;
};

bool available();
// True with *out filled when a valid code was entered; false on Cancel.
// Blocks the calling thread while the box is open.
bool ask(const Text& text, run_code::Run* out);

// New Game's "Type a seed" (new_game_flow.h Options::ask_code), its words
// through the game's translation; empty where there is no box.
inline std::function<bool(run_code::Run*)> asker(std::function<std::string(const char*)> translate) {
    if (!available()) return {};
    return [translate](run_code::Run* out) {
        const auto t = [&](const char* s) { return translate ? translate(s) : std::string(s); };
        return ask({t("Type a seed"), t("Type a friend's seed to play their run."), t("Not a valid seed."), t("OK"),
                    t("Cancel")},
                   out);
    };
}

}  // namespace gsr::code_box
