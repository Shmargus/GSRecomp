// Tracker UI uses the shared UTF-8 menu translations. Proper names are
// decoded once from the selected ROM, using measured per-revision text IDs.
#pragma once
#include "gs_text_encode.h"
#include "host_config_ui.h"
#include <array>
#include <atomic>
#include <charconv>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace gsr::tracker_text {
struct Ids { std::array<std::uint16_t, 5> language; };
struct Token { const char* english; Ids ids; };
struct Names {
    std::span<const Ids> djinn, places, contents;
    std::span<const Token> tokens;
};
struct Config { Names names; std::size_t text_table, tree_table; unsigned language; };
inline const Config* config = nullptr;
inline std::unordered_map<std::string, std::string> names;
inline std::atomic<bool> ready{false};

template<class... Args> inline std::string format(const char* text, Args... args) {
    const int length = std::snprintf(nullptr, 0, text, args...);
    if (length < 0) return {};
    std::string result(static_cast<std::size_t>(length) + 1, '\0');
    std::snprintf(result.data(), result.size(), text, args...);
    result.resize(length);
    return result;
}

inline std::string utf8(const std::string& latin1) {
    std::string result;
    for (unsigned char c : latin1) {
        if (c < 0x20u) continue;  // name-prefix and line-format control codes
        if (c >= 0x80u) result += static_cast<char>(0xC0u | (c >> 6));
        result += static_cast<char>(c < 0x80u ? c : 0x80u | (c & 0x3Fu));
    }
    return result;
}

// Called only while the initial name cache is private to the game thread.
inline void add(mods::TextEncoder& decoder, const char* english, const Ids& ids) {
    const unsigned id = ids.language[config->language];
    if (!id) return;  // authored labels such as coin amounts and "Summon"
    std::string value;
    if (!decoder.decode(id, &value) || utf8(value).empty()) {
        std::fprintf(stderr, "[trackers] could not decode name text 0x%X\n", id);
        return;
    }
    names.insert_or_assign(english, utf8(value));
}

inline std::string name(const char* english) {
    if (ready.load(std::memory_order_acquire)) {
        const auto found = names.find(english);
        if (found != names.end()) return found->second;
    }
    return gbarecomp::ui_text(english);
}

// Translate prose first, then insert ROM-native names. A decorated/composed
// string must never become the key for a whole-string translation lookup.
inline std::string route(const char* english) {
    std::string result = gbarecomp::ui_text(english);
    std::size_t at = 0;
    while ((at = result.find('{', at)) != std::string::npos) {
        const auto end = result.find('}', at + 1);
        if (end == std::string::npos) break;
        const auto key = result.substr(at + 1, end - at - 1);
        const auto value = name(key.c_str());
        result.replace(at, end - at + 1, value);
        at += value.size();
    }
    return result;
}

inline std::string contents(const char* english) {
    const std::string_view text(english);
    unsigned amount = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), amount);
    const std::string_view suffix(parsed.ptr, text.data() + text.size());
    if (parsed.ec == std::errc{} && (suffix == " coin" || suffix == " coins"))
        return format(gbarecomp::ui_text(amount == 1 ? "%u coin" : "%u coins"), amount);
    return name(english);
}
} // namespace gsr::tracker_text
