// One rule for on/off environment switches (GBARECOMP_*, GSR_*).
#pragma once
#include <cstdlib>
namespace gbarecomp {
// Off: empty, starting with '0', or false/off/no in any letter case.
// Unset gives `unset_value`. Anything else is on.
inline bool env_flag(const char* name, bool unset_value = false) {
    const char* v = std::getenv(name);
    if (v == nullptr) return unset_value;
    if (v[0] == '\0' || v[0] == '0') return false;
    static const char* const kOff[] = {"false", "off", "no"};
    for (const char* word : kOff) {
        const char* a = v;
        const char* b = word;
        while (*a != '\0' && *b != '\0') {
            char c = *a;
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (c != *b) break;
            ++a;
            ++b;
        }
        if (*a == '\0' && *b == '\0') return false;
    }
    return true;
}
}  // namespace gbarecomp
