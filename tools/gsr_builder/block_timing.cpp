// Block timing for the player build: the same rewrite tools/block_timing.py
// does for the developer build (read that file's header for what and why).
// Players must get the same game code as the developer build: the release
// engine exports only what that code calls (cmake/gsr_engine_exports.cmake),
// and per-instruction code calls functions the block-timing code does not.
//
// Keep the two in step: tools/block_timing.py is the reference, and the
// output here must be byte-for-byte the same for the same input.
#include "block_timing.h"

#include <cctype>
#include <fstream>
#include <set>
#include <sstream>
#include <vector>

namespace gsr {
namespace {

const char* const kPrelude = R"(
// Block timing (tools/block_timing.py): inline memory timing.
extern "C" uint8_t g_runtime_mem_cost[16][2][2];
extern "C" uint32_t g_runtime_mem_cost_key;
extern "C" const uint8_t* g_runtime_waitcnt_live;
// always_inline: plain `static inline` was left out of line in the big
// generated functions and was 20% of the heaviest Nereid frames
// (logs/session_20260930_190323.hostprof.txt).
// Below the cart (BIOS, EWRAM, IWRAM, IO, PAL, VRAM, OAM) the cost does not
// depend on WAITCNT or S/N: GbaBus::access_cycles (gba_bus.cpp) gives EWRAM
// 3 / 6, PAL and VRAM 1 / 2, everything else 1, for 16- / 32-bit. With a
// constant address that folds to a constant.
static inline __attribute__((always_inline))
uint32_t gsr_bt_mem_cycles(uint32_t a, uint32_t w, uint32_t s) {
    const uint32_t region = (a >> 24) & 0xFu;
    if (region < 0x8u) {
        if (region == 0x2u) return w == 4u ? 6u : 3u;
        if (region == 0x5u || region == 0x6u) return w == 4u ? 2u : 1u;
        return 1u;
    }
    const uint8_t* wc = g_runtime_waitcnt_live;
    if (wc && (uint32_t)(wc[0] | (wc[1] << 8)) == g_runtime_mem_cost_key)
        return g_runtime_mem_cost[region][w == 4u][s != 0u];
    return runtime_mem_cycles(a, w, s);
}
static inline __attribute__((always_inline))
uint32_t gsr_bt_insn_fetch(uint32_t pc, uint32_t w) {
    g_cpu.R[15] = pc;
    return gsr_bt_mem_cycles(pc, w, 1u);
}
// runtime_arm.cpp's arm_cond_passes, inline.
static inline __attribute__((always_inline))
int gsr_bt_cond_passes(unsigned cond) {
    const uint32_t n = cpsr_n();
    const uint32_t z = cpsr_z();
    const uint32_t c = cpsr_c();
    const uint32_t v = cpsr_v();
    switch (cond & 0xFu) {
        case 0x0: return z != 0;
        case 0x1: return z == 0;
        case 0x2: return c != 0;
        case 0x3: return c == 0;
        case 0x4: return n != 0;
        case 0x5: return n == 0;
        case 0x6: return v != 0;
        case 0x7: return v == 0;
        case 0x8: return (c != 0) && (z == 0);
        case 0x9: return (c == 0) || (z != 0);
        case 0xA: return n == v;
        case 0xB: return n != v;
        case 0xC: return (z == 0) && (n == v);
        case 0xD: return (z != 0) || (n != v);
        case 0xE: return 1;
        default:  return 0;
    }
}
)";

// Python's str.isspace / \s for the characters generated code contains.
bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
           c == '\v';
}
bool is_word(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}
bool is_hex(char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }

std::string rstrip(const std::string& s) {
    std::size_t e = s.size();
    while (e > 0 && is_space(s[e - 1])) --e;
    return s.substr(0, e);
}
std::size_t indent_end(const std::string& s) {
    std::size_t i = 0;
    while (i < s.size() && is_space(s[i])) ++i;
    return i;
}

// ^\S.*\)\s*\{\s*$
bool is_func_header(const std::string& s) {
    if (s.empty() || is_space(s[0])) return false;
    std::string r = rstrip(s);
    if (r.empty() || r.back() != '{') return false;
    r = rstrip(r.substr(0, r.size() - 1));
    return !r.empty() && r.back() == ')';
}

// ^\s*L_[0-9A-Fa-f]+:\s*$ ; returns the label name (without the colon).
bool label_name(const std::string& s, std::string* name) {
    std::size_t i = indent_end(s);
    if (s.compare(i, 2, "L_") != 0) return false;
    std::size_t j = i + 2;
    while (j < s.size() && is_hex(s[j])) ++j;
    if (j == i + 2 || j >= s.size() || s[j] != ':') return false;
    for (std::size_t k = j + 1; k < s.size(); ++k)
        if (!is_space(s[k])) return false;
    *name = s.substr(i, j - i);
    return true;
}

// ^(\s*)uint32_t (_cyc_\w+) = runtime_insn_begin\((0x[0-9A-Fa-f]+u), (\d+u)\);$
bool parse_begin(const std::string& s, std::string* indent, std::string* var,
                 std::string* pc, std::string* width) {
    std::size_t i = indent_end(s);
    *indent = s.substr(0, i);
    static const std::string kType = "uint32_t ";
    if (s.compare(i, kType.size(), kType) != 0) return false;
    i += kType.size();
    if (s.compare(i, 5, "_cyc_") != 0) return false;
    std::size_t j = i + 5;
    while (j < s.size() && is_word(s[j])) ++j;
    if (j == i + 5) return false;
    *var = s.substr(i, j - i);
    static const std::string kCall = " = runtime_insn_begin(";
    if (s.compare(j, kCall.size(), kCall) != 0) return false;
    j += kCall.size();
    if (s.compare(j, 2, "0x") != 0) return false;
    std::size_t k = j + 2;
    while (k < s.size() && is_hex(s[k])) ++k;
    if (k == j + 2 || k >= s.size() || s[k] != 'u') return false;
    *pc = s.substr(j, k + 1 - j);
    k += 1;
    if (s.compare(k, 2, ", ") != 0) return false;
    k += 2;
    std::size_t m = k;
    while (m < s.size() && std::isdigit(static_cast<unsigned char>(s[m]))) ++m;
    if (m == k || m >= s.size() || s[m] != 'u') return false;
    *width = s.substr(k, m + 1 - k);
    return s.compare(m + 1, std::string::npos, ");") == 0;
}

// ^\s*if \(runtime_insn_boundary\(\)\) return;$
bool is_boundary(const std::string& s) {
    return s.compare(indent_end(s), std::string::npos,
                     "if (runtime_insn_boundary()) return;") == 0;
}

void replace_all(std::string& s, const std::string& from, const std::string& to,
                 std::size_t* count = nullptr) {
    std::size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
        if (count) ++*count;
    }
}

// runtime_mem_cycles(A, W, 0u|1u) -> gsr_bt_mem_cycles(A, W, ...), as
// inline_mem_cycles in the Python tool.
std::string inline_mem_cycles(const std::string& line, BlockTimingStats& st) {
    static const std::string kName = "runtime_mem_cycles(";
    std::string out;
    std::size_t i = 0;
    const std::size_t n = kName.size();
    while (true) {
        const std::size_t j = line.find(kName, i);
        if (j == std::string::npos) {
            out += line.substr(i);
            break;
        }
        if (j > 0 && is_word(line[j - 1])) {
            out += line.substr(i, j + n - i);
            i = j + n;
            continue;
        }
        std::size_t k = j + n;
        int depth = 1;
        std::size_t start = k;
        std::vector<std::string> args;
        while (k < line.size() && depth) {
            const char c = line[k];
            if (c == '(') ++depth;
            else if (c == ')') --depth;
            else if (c == ',' && depth == 1) {
                args.push_back(line.substr(start, k - start));
                start = k + 1;
            }
            ++k;
        }
        if (depth) {
            ++st.mem_cycles_kept;
            out += line.substr(i);
            break;
        }
        args.push_back(line.substr(start, k - 1 - start));
        out += line.substr(i, j - i);
        std::string third = args.size() == 3 ? args[2] : std::string();
        // Python: args[2].strip()
        std::size_t a = 0, b = third.size();
        while (a < b && is_space(third[a])) ++a;
        while (b > a && is_space(third[b - 1])) --b;
        third = third.substr(a, b - a);
        if (args.size() == 3 && (third == "0u" || third == "1u")) {
            out += "gsr_bt_mem_cycles(";
            ++st.mem_cycles_inline;
        } else {
            out += kName;
            ++st.mem_cycles_kept;
        }
        i = j + n;
    }
    return out;
}

}  // namespace

bool block_timing_transform(const std::string& text, std::string* out,
                            BlockTimingStats& st) {
    std::vector<std::string> lines;
    {
        std::size_t start = 0;
        while (true) {
            const std::size_t nl = text.find('\n', start);
            if (nl == std::string::npos) {
                lines.push_back(text.substr(start));
                break;
            }
            lines.push_back(text.substr(start, nl - start));
            start = nl + 1;
        }
    }
    std::set<std::string> loop_labels;
    for (const std::string& line : lines) {
        if (line.find("goto L_") == std::string::npos ||
            line.find("case ") != std::string::npos)
            continue;
        std::size_t p = 0;
        while ((p = line.find("goto L_", p)) != std::string::npos) {
            // \bgoto (L_[0-9A-Fa-f]+);
            if (p > 0 && is_word(line[p - 1])) { p += 7; continue; }
            std::size_t q = p + 7;
            while (q < line.size() && is_hex(line[q])) ++q;
            if (q > p + 7 && q < line.size() && line[q] == ';')
                loop_labels.insert(line.substr(p + 5, q - (p + 5)));
            p = q;
        }
    }
    std::vector<std::string> result;
    result.reserve(lines.size());
    bool keep_next = false;
    std::string indent, var, pc, width, name;
    for (std::size_t i = 0; i < lines.size();) {
        std::string line = lines[i];
        if (is_func_header(line)) {
            keep_next = true;
        } else if (label_name(line, &name)) {
            if (loop_labels.count(name)) keep_next = true;
            else ++st.entry_labels;
        }
        if (parse_begin(line, &indent, &var, &pc, &width) && i + 1 < lines.size() &&
            lines[i + 1] == indent + "if (" + var + " == 0u) return;") {
            if (keep_next) {
                keep_next = false;
                ++st.kept;
                result.push_back(line);
                result.push_back(lines[i + 1]);
            } else {
                ++st.merged;
                result.push_back(indent + "uint32_t " + var + " = gsr_bt_insn_fetch(" +
                                 pc + ", " + width + ");");
            }
            i += 2;
            continue;
        }
        if (is_boundary(line)) {
            if (keep_next) {
                keep_next = false;
                ++st.kept_boundary;
            } else {
                ++st.dropped_boundary;
                i += 1;
                continue;
            }
        }
        if (line.find("runtime_insn_boundary()") != std::string::npos ||
            line.find("runtime_insn_begin(") != std::string::npos)
            keep_next = false;
        if (line.find("runtime_tick(") != std::string::npos)
            replace_all(line, "runtime_tick(", "runtime_tick_deferred(", &st.ticks);
        if (line.find("runtime_mem_cycles(") != std::string::npos)
            line = inline_mem_cycles(line, st);
        if (line.find("arm_cond_passes(") != std::string::npos)
            replace_all(line, "arm_cond_passes(", "gsr_bt_cond_passes(", &st.cond_inline);
        result.push_back(line);
        i += 1;
    }
    std::string joined;
    for (std::size_t i = 0; i < result.size(); ++i) {
        if (i) joined += '\n';
        joined += result[i];
    }
    if (joined.find("gsr_bt_") != std::string::npos) {
        static const std::string kAnchor = "#include \"recompiled.h\"\n";
        const std::size_t at = joined.find(kAnchor);
        if (at == std::string::npos) return false;
        joined.insert(at + kAnchor.size(), kPrelude);
        ++st.preludes;
    }
    *out = std::move(joined);
    return true;
}

}  // namespace gsr
