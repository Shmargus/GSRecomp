// slim_pass.cpp -- see slim_pass.h. Each pass mirrors one rule of
// tools/slim_corpus.py, in the same order, with the same "leave anything
// that does not match exactly alone" policy. Plain string matching instead
// of std::regex: the corpus is ~500 MB of text.
#include "slim_pass.h"

#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace gbarecomp {
namespace {

using Lines = std::vector<std::string>;

std::string indent_of(const std::string& s) {
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(0, i);
}

bool starts_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}
bool ends_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() &&
           s.compare(s.size() - p.size(), p.size(), p) == 0;
}
bool is_word(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}
// `prefix` followed by one or more word characters, the whole string.
bool is_ident_with(const std::string& s, const std::string& prefix) {
    if (!starts_with(s, prefix) || s.size() == prefix.size()) return false;
    for (std::size_t i = prefix.size(); i < s.size(); ++i)
        if (!is_word(s[i])) return false;
    return true;
}
bool is_hex_u(const std::string& s) {  // 0x[0-9A-F]+u
    if (s.size() < 4 || s[0] != '0' || s[1] != 'x' || s.back() != 'u')
        return false;
    for (std::size_t i = 2; i + 1 < s.size(); ++i) {
        const char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}
bool is_digits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}
// Python's [^,\n]+ / [^,()\n]+ field checks.
bool no_comma(const std::string& s) {
    return !s.empty() && s.find_first_of(",\n") == std::string::npos;
}
bool no_comma_paren(const std::string& s) {
    return !s.empty() && s.find_first_of(",()\n") == std::string::npos;
}

// "a, b, c" as fields whose separators are exactly ", " and which contain
// no comma (the Python pattern ([^,\n]+), ([^,\n]+), ...).
bool split_fields(const std::string& s, std::size_t n, std::vector<std::string>* out) {
    out->clear();
    std::size_t pos = 0;
    for (std::size_t k = 0; k < n; ++k) {
        const std::size_t comma = s.find(',', pos);
        const bool last = k + 1 == n;
        if (last != (comma == std::string::npos)) return false;
        std::string field = s.substr(pos, last ? std::string::npos : comma - pos);
        if (!no_comma(field)) return false;
        out->push_back(field);
        if (!last) {
            if (comma + 1 >= s.size() || s[comma + 1] != ' ') return false;
            pos = comma + 2;
        }
    }
    return true;
}

void bump(std::map<std::string, long>* counts, const char* key) {
    if (counts) ++(*counts)[key];
}

// ind + "<cyc> += runtime_mem_cycles(<arg>, <w>u, <s>u)<tail>"
bool parse_cost(const std::string& line, const std::string& ind, std::string* cyc,
                std::string* rest_after_open) {
    if (!starts_with(line, ind)) return false;
    const std::string body = line.substr(ind.size());
    const std::size_t eq = body.find(" += runtime_mem_cycles(");
    if (eq == std::string::npos) return false;
    *cyc = body.substr(0, eq);
    if (!is_ident_with(*cyc, "_cyc_")) return false;
    *rest_after_open = body.substr(eq + std::string(" += runtime_mem_cycles(").size());
    return true;
}

// Rule 1.
void pass_boundary(Lines& in, std::map<std::string, long>* counts) {
    Lines out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        const std::string ind = indent_of(in[i]);
        if (in[i] == ind + "if (runtime_should_yield()) return;" && i + 1 < in.size() &&
            in[i + 1] == ind + "if (g_runtime_insn_trace) runtime_insn_fp();") {
            out.push_back(ind + "if (runtime_insn_boundary()) return;");
            bump(counts, "boundary");
            ++i;
            continue;
        }
        out.push_back(std::move(in[i]));
    }
    in.swap(out);
}

// Rule 1b.
void pass_begin(Lines& in, std::map<std::string, long>* counts) {
    Lines out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        const std::string& l0 = in[i];
        const std::string ind = indent_of(l0);
        const std::string p0 = ind + "g_cpu.R[15] = ";
        if (i + 4 < in.size() && starts_with(l0, p0) && ends_with(l0, ";")) {
            const std::string pc = l0.substr(p0.size(), l0.size() - p0.size() - 1);
            std::string cyc, rest, d;
            if (is_hex_u(pc) &&
                in[i + 1] == ind + "if (runtime_insn_boundary()) return;" &&
                starts_with(in[i + 2], ind + "uint32_t ") &&
                ends_with(in[i + 2], " = 1u;")) {
                cyc = in[i + 2].substr(ind.size() + 9,
                                       in[i + 2].size() - ind.size() - 9 - 6);
                const std::string p3 = ind + cyc + " = ";
                if (is_ident_with(cyc, "_cyc_") && starts_with(in[i + 3], p3) &&
                    ends_with(in[i + 3], "u;")) {
                    d = in[i + 3].substr(p3.size(), in[i + 3].size() - p3.size() - 2);
                    std::string w;
                    for (const char* cand : {"2", "4"}) {
                        if (in[i + 4] == ind + cyc + " += runtime_mem_cycles(" + pc +
                                             ", " + cand + "u, 1u) - 1u;")
                            w = cand;
                    }
                    if (is_digits(d) && !w.empty()) {
                        out.push_back(ind + "uint32_t " + cyc +
                                      " = runtime_insn_begin(" + pc + ", " + w + "u);");
                        out.push_back(ind + "if (" + cyc + " == 0u) return;");
                        out.push_back(ind + cyc + " += " + d + "u - 1u;");
                        bump(counts, "begin");
                        i += 4;
                        continue;
                    }
                }
            }
        }
        out.push_back(std::move(in[i]));
    }
    in.swap(out);
}

bool store_value_matches(const std::string& bits, const std::string& traced,
                         const std::string& stored) {
    if (bits == "32") return traced == stored;
    const std::string pt = "(uint32_t)";
    const std::string ps = "(uint" + bits + "_t)";
    if (!starts_with(traced, pt) || !starts_with(stored, ps)) return false;
    return traced.substr(pt.size()) == stored.substr(ps.size());
}

// Rule 5.
void pass_store(Lines& in, std::map<std::string, long>* counts) {
    Lines out;
    out.reserve(in.size());
    const std::string pre = "runtime_trace_event(RUNTIME_TRACE_MEM_WRITE, ";
    for (std::size_t i = 0; i < in.size(); ++i) {
        const std::string& l0 = in[i];
        const std::string ind = indent_of(l0);
        std::vector<std::string> f;
        if (i + 2 < in.size() && starts_with(l0, ind + pre) && ends_with(l0, "u);")) {
            const std::string mid =
                l0.substr(ind.size() + pre.size(),
                          l0.size() - ind.size() - pre.size() - 3);
            if (split_fields(mid, 4, &f) &&
                (f[3] == "1" || f[3] == "2" || f[3] == "4")) {
                const std::string &pc = f[0], &addr = f[1], &traced = f[2], &w = f[3];
                std::string bits;
                std::string vn;
                for (const char* b : {"32", "16", "8"}) {
                    const std::string p1 = ind + "bus_write_u" + b + "(" + addr + ", ";
                    const std::string& l1 = in[i + 1];
                    if (starts_with(l1, p1) && ends_with(l1, ");") &&
                        l1.size() > p1.size() + 2) {
                        bits = b;
                        vn = l1.substr(p1.size(), l1.size() - p1.size() - 2);
                        break;
                    }
                }
                std::string cyc, rest;
                if (!bits.empty() && vn.find('\n') == std::string::npos &&
                    parse_cost(in[i + 2], ind, &cyc, &rest) &&
                    ends_with(rest, ", " + w + "u, 2u);")) {
                    const std::string ea = rest.substr(0, rest.size() - (w.size() + 9));
                    if (is_ident_with(ea, "_ea_")) {
                        const char* width = bits == "32" ? "4" : bits == "16" ? "2" : "1";
                        if (w != width || !store_value_matches(bits, traced, vn)) {
                            bump(counts, "store_skipped");
                            out.push_back(std::move(in[i]));
                            out.push_back(std::move(in[i + 1]));
                            out.push_back(std::move(in[i + 2]));
                            i += 2;
                            continue;
                        }
                        out.push_back(ind + cyc + " += runtime_st_u" + bits + "(" + pc +
                                      ", " + addr + ", " + ea + ", " + traced + ");");
                        bump(counts, "store");
                        i += 2;
                        continue;
                    }
                }
            }
        }
        out.push_back(std::move(in[i]));
    }
    in.swap(out);
}

// "<_a_X & ~3u>, 4u, <0|1>u);" -> addr, seq
bool parse_block_cost(const std::string& rest, std::string* addr, std::string* seq) {
    for (const char* s : {"0", "1"}) {
        const std::string tail = std::string(", 4u, ") + s + "u);";
        if (ends_with(rest, tail)) {
            *addr = rest.substr(0, rest.size() - tail.size());
            *seq = s;
            if (!ends_with(*addr, " & ~3u")) return false;
            return is_ident_with(addr->substr(0, addr->size() - 6), "_a_");
        }
    }
    return false;
}

// Rule 7.
void pass_stm(Lines& in, std::map<std::string, long>* counts) {
    Lines out;
    out.reserve(in.size());
    const std::string pre = "runtime_trace_event(RUNTIME_TRACE_MEM_WRITE, ";
    for (std::size_t i = 0; i < in.size(); ++i) {
        const std::string ind = indent_of(in[i]);
        std::string cyc, rest, addr, seq;
        if (i + 2 < in.size() && parse_cost(in[i], ind, &cyc, &rest) &&
            parse_block_cost(rest, &addr, &seq)) {
            const std::string& l1 = in[i + 1];
            std::vector<std::string> f;
            if (starts_with(l1, ind + pre) && ends_with(l1, ", 4u);")) {
                const std::string mid =
                    l1.substr(ind.size() + pre.size(),
                              l1.size() - ind.size() - pre.size() - 6);
                if (split_fields(mid, 3, &f) && f[1] == addr &&
                    in[i + 2] == ind + "bus_write_u32(" + addr + ", " + f[2] + ");") {
                    out.push_back(ind + cyc + " += runtime_stm_u32(" + f[0] + ", " + addr +
                                  ", " + f[2] + ", " + seq + "u);");
                    bump(counts, "stm");
                    i += 2;
                    continue;
                }
            }
        }
        out.push_back(std::move(in[i]));
    }
    in.swap(out);
}

// Rule 6.
void pass_ldm(Lines& in, std::map<std::string, long>* counts) {
    Lines out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        const std::string ind = indent_of(in[i]);
        std::string cyc, rest, addr, seq;
        if (i + 1 < in.size() && parse_cost(in[i], ind, &cyc, &rest) &&
            parse_block_cost(rest, &addr, &seq)) {
            const std::string& l1 = in[i + 1];
            const std::string p1 = ind + "g_cpu.R[";
            const std::string tail = "] = bus_read_u32(" + addr + ");";
            if (starts_with(l1, p1) && ends_with(l1, tail)) {
                const std::string n = l1.substr(p1.size(), l1.size() - p1.size() - tail.size());
                if (is_digits(n)) {
                    out.push_back(ind + cyc + " += runtime_ldm_u32(" + addr + ", " + seq +
                                  "u, &g_cpu.R[" + n + "]);");
                    bump(counts, "ldm");
                    ++i;
                    continue;
                }
            }
        }
        out.push_back(std::move(in[i]));
    }
    in.swap(out);
}

// Rule 3.
void pass_refill(Lines& in, std::map<std::string, long>* counts) {
    Lines out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        const std::string ind = indent_of(in[i]);
        std::string cyc, rest;
        if (i + 1 < in.size() && parse_cost(in[i], ind, &cyc, &rest)) {
            for (const char* w : {"2", "4"}) {
                const std::string tail = std::string(", ") + w + "u, 0u) - 1u;";
                if (!ends_with(rest, tail)) continue;
                const std::string target = rest.substr(0, rest.size() - tail.size());
                if (!no_comma_paren(target)) break;
                if (in[i + 1] == ind + cyc + " += runtime_mem_cycles(" + target + " + " + w +
                                     "u, " + w + "u, 1u) - 1u;") {
                    out.push_back(ind + cyc + " += runtime_refill_cycles(" + target + ", " +
                                  w + "u);");
                    bump(counts, "refill");
                    ++i;
                    goto next;
                }
                break;
            }
        }
        out.push_back(std::move(in[i]));
    next:;
    }
    in.swap(out);
}

// Rule 2, anywhere in a line.
void pass_ns_delta(Lines& in, std::map<std::string, long>* counts) {
    const std::string open = "runtime_mem_cycles(";
    for (std::string& line : in) {
        std::size_t pos = 0;
        std::string result;
        bool changed = false;
        while (true) {
            const std::size_t at = line.find(open, pos);
            if (at == std::string::npos) break;
            const std::size_t arg = at + open.size();
            const std::size_t comma = line.find_first_of(",()\n", arg);
            bool matched = false;
            if (comma != std::string::npos && comma > arg && line[comma] == ',') {
                const std::string x = line.substr(arg, comma - arg);
                for (const char* w : {"2", "4"}) {
                    const std::string want = x + ", " + w + "u, 0u) - runtime_mem_cycles(" +
                                             x + ", " + w + "u, 1u)";
                    if (line.compare(arg, want.size(), want) == 0) {
                        result += line.substr(pos, at - pos);
                        result += "runtime_fetch_ns_delta(" + x + ", " + w + "u)";
                        pos = arg + want.size();
                        matched = changed = true;
                        bump(counts, "ns_delta");
                        break;
                    }
                }
            }
            if (!matched) {
                result += line.substr(pos, arg - pos);
                pos = arg;
            }
        }
        if (changed) {
            result += line.substr(pos);
            line.swap(result);
        }
    }
}

// Rule 4, line by line after the others (Python's final loop).
void pass_load(Lines& lines, std::map<std::string, long>* counts) {
    const long n = static_cast<long>(lines.size());
    auto at = [&](long j) -> std::string& {  // Python negative indexing
        return lines[static_cast<std::size_t>(j < 0 ? j + n : j)];
    };
    for (long i = 0; i < n; ++i) {
        const std::string ind = indent_of(lines[i]);
        std::string cyc, rest;
        if (!parse_cost(lines[i], ind, &cyc, &rest)) continue;
        std::string width;
        for (const char* w : {"1", "2", "4"}) {
            if (ends_with(rest, std::string(", ") + w + "u, 2u);")) width = w;
        }
        if (width.empty()) continue;
        const std::string ea = rest.substr(0, rest.size() - (width.size() + 9));
        if (!is_ident_with(ea, "_ea_")) continue;

        std::vector<long> prev{i - 1};
        {
            const std::string& p = at(i - 1);
            const std::size_t k = p.find_first_not_of(" \t\r\n\f\v");
            if (k != std::string::npos && p.compare(k, 15, "uint32_t _rot =") == 0)
                prev = {i - 2, i - 1};
        }
        std::string block;
        for (std::size_t k = 0; k < prev.size(); ++k) {
            if (k) block += "\n";
            block += at(prev[k]);
        }
        // Every bus_read_u32/16/8( in the block.
        std::vector<std::string> reads;
        for (std::size_t p = block.find("bus_read_u"); p != std::string::npos;
             p = block.find("bus_read_u", p + 1)) {
            for (const char* b : {"32", "16", "8"}) {
                if (block.compare(p + 10, std::strlen(b) + 1, std::string(b) + "(") == 0) {
                    reads.push_back(b);
                    break;
                }
            }
        }
        const std::string before = at(prev[0] - 1);
        const std::size_t lead = block.find_first_not_of(" \t\r\n\f\v");
        const bool if_else = lead != std::string::npos &&
            (block.compare(lead, 4, "else") == 0 || block.compare(lead, 2, "if") == 0);
        const char* want_w = reads.size() == 1
            ? (reads[0] == "32" ? "4" : reads[0] == "16" ? "2" : "1") : "";
        if (reads.size() != 1 || width != want_w ||
            block.find("bus_write") != std::string::npos ||
            block.find("(" + ea) == std::string::npos || if_else ||
            before.find("bus_read") != std::string::npos) {
            bump(counts, "load_skipped");
            continue;
        }
        const std::string bits = reads[0];
        const std::string call = "bus_read_u" + bits + "(";
        bool done = false;
        for (long j : prev) {
            std::string& line = at(j);
            // bus_read_uNN(EA) or bus_read_uNN(EA & ~1u / ~3u): every
            // occurrence in the line, as Python's subn.
            std::string out;
            std::size_t pos = 0;
            int replaced = 0;
            for (std::size_t p = line.find(call); p != std::string::npos;
                 p = line.find(call, p + 1)) {
                const std::size_t a = p + call.size();
                std::string arg;
                for (const char* suffix : {" & ~1u", " & ~3u", ""}) {
                    const std::string cand = ea + suffix;
                    if (line.compare(a, cand.size() + 1, cand + ")") == 0) {
                        arg = cand;
                        break;
                    }
                }
                if (arg.empty()) continue;
                out += line.substr(pos, p - pos);
                out += "runtime_ld_u" + bits + "(" + arg + ", " + ea + ")";
                pos = a + arg.size() + 1;
                p = pos - 1;
                ++replaced;
            }
            if (replaced) {
                out += line.substr(pos);
                line.swap(out);
                done = true;
                break;
            }
        }
        if (!done) {
            bump(counts, "load_skipped");
            continue;
        }
        lines[i] = ind + cyc + " += g_runtime_data_cost;";
        bump(counts, "load");
    }
}

}  // namespace

std::string slim_text(const std::string& text, std::map<std::string, long>* counts) {
    Lines lines;
    {
        std::size_t pos = 0;
        while (true) {
            const std::size_t nl = text.find('\n', pos);
            if (nl == std::string::npos) {
                lines.push_back(text.substr(pos));
                break;
            }
            lines.push_back(text.substr(pos, nl - pos));
            pos = nl + 1;
        }
    }
    pass_boundary(lines, counts);
    pass_begin(lines, counts);
    pass_store(lines, counts);
    pass_stm(lines, counts);
    pass_ldm(lines, counts);
    pass_refill(lines, counts);
    pass_ns_delta(lines, counts);
    pass_load(lines, counts);
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i) out += '\n';
        out += lines[i];
    }
    return out;
}

bool slim_directory(const std::string& dir, std::map<std::string, long>* counts,
                    std::string* error) {
    namespace fs = std::filesystem;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        const std::string name = entry.path().filename().string();
        if (!entry.is_regular_file() || !starts_with(name, "recompiled") ||
            !ends_with(name, ".cpp"))
            continue;
        std::ifstream in(entry.path(), std::ios::binary);
        std::stringstream buf;
        buf << in.rdbuf();
        if (!in && !in.eof()) {
            if (error) *error = "cannot read " + entry.path().string();
            return false;
        }
        std::string text = buf.str();
        const bool crlf = text.find("\r\n") != std::string::npos;
        if (crlf) {
            std::string lf;
            lf.reserve(text.size());
            for (std::size_t i = 0; i < text.size(); ++i)
                if (!(text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n'))
                    lf += text[i];
            text.swap(lf);
        }
        std::string slim = slim_text(text, counts);
        if (slim == text) continue;
        if (crlf) {
            std::string c;
            c.reserve(slim.size() + slim.size() / 16);
            for (char ch : slim) {
                if (ch == '\n') c += '\r';
                c += ch;
            }
            slim.swap(c);
        }
        std::ofstream out(entry.path(), std::ios::binary | std::ios::trunc);
        out.write(slim.data(), static_cast<std::streamsize>(slim.size()));
        if (!out) {
            if (error) *error = "cannot write " + entry.path().string();
            return false;
        }
    }
    if (ec) {
        if (error) *error = "cannot list " + dir + ": " + ec.message();
        return false;
    }
    return true;
}

}  // namespace gbarecomp
