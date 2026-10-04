// launcher_online.h: what the Windows and Linux launchers share for their
// two online features, both of which only run when the player agrees:
//
//   * Updates: on start the launcher asks GitHub for the project's newest
//     release (pre-releases included). When its tag differs from the tag this
//     launcher was built as (GSR_RELEASE_VERSION, set by make_release) and it
//     has a download for this platform, the player is told and can open the
//     release's GitHub page. Nothing is downloaded by the launcher.
//   * Bug reports: the "Send report" button uploads a report archive to the
//     project's report service (tools/report_service/worker.js). Its address
//     is not in the source: only the official release scripts build it in
//     (GSR_REPORT_HOST). A launcher built without it has no Send button and
//     offers the bug report form instead, so a fork never sends reports to
//     the project by accident.
//
// Plain C++ only: the HTTP and file work is platform code in each launcher.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#ifndef GSR_RELEASE_VERSION
#define GSR_RELEASE_VERSION "dev"
#endif
#ifndef GSR_REPORT_HOST
#define GSR_REPORT_HOST ""
#endif

namespace gsr_online {

constexpr const char* kReleaseVersion = GSR_RELEASE_VERSION;
constexpr const char* kReleasesApiHost = "api.github.com";
constexpr const char* kReleasesApiPath = "/repos/Shmargus/GSRecomp/releases?per_page=1";
constexpr const char* kReleasesPage = "https://github.com/Shmargus/GSRecomp/releases";
constexpr const char* kReportHost = GSR_REPORT_HOST;
constexpr const char* kReportPath = "/report";
constexpr const char* kUserAgent = "GoldenSunLauncher";

// A development build ("dev") never offers updates.
inline bool update_checks_enabled() {
    return std::strcmp(kReleaseVersion, "dev") != 0 && kReleaseVersion[0] != '\0';
}

// Only a launcher built with the report service's address can send reports.
inline bool report_upload_enabled() { return kReportHost[0] != '\0'; }

// ---------------------------------------------------------------- JSON
// Just enough JSON for GitHub's release list: objects, arrays, strings
// (with escapes), numbers, true/false/null.
struct Json {
    enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
    bool boolean = false;
    double number = 0;
    std::string text;
    std::vector<Json> items;
    std::map<std::string, Json> fields;

    const Json* get(const std::string& key) const {
        auto it = fields.find(key);
        return it == fields.end() ? nullptr : &it->second;
    }
    std::string str(const std::string& key) const {
        const Json* v = get(key);
        return v && v->kind == String ? v->text : std::string();
    }
    double num(const std::string& key) const {
        const Json* v = get(key);
        return v && v->kind == Number ? v->number : 0;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& s) : s_(s) {}
    bool parse(Json* out) {
        if (!value(out, 0)) return false;
        ws();
        return pos_ == s_.size();
    }

private:
    const std::string& s_;
    std::size_t pos_ = 0;

    void ws() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\n' ||
                                    s_[pos_] == '\r' || s_[pos_] == '\t'))
            ++pos_;
    }
    bool literal(const char* word) {
        const std::size_t n = std::strlen(word);
        if (s_.compare(pos_, n, word) != 0) return false;
        pos_ += n;
        return true;
    }
    static void append_utf8(std::string* out, unsigned cp) {
        if (cp < 0x80) {
            out->push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    bool hex4(unsigned* cp) {
        if (pos_ + 4 > s_.size()) return false;
        unsigned v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[pos_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
            else return false;
        }
        *cp = v;
        return true;
    }
    bool string(std::string* out) {
        if (pos_ >= s_.size() || s_[pos_] != '"') return false;
        ++pos_;
        while (pos_ < s_.size()) {
            const char c = s_[pos_++];
            if (c == '"') return true;
            if (c != '\\') {
                out->push_back(c);
                continue;
            }
            if (pos_ >= s_.size()) return false;
            const char e = s_[pos_++];
            switch (e) {
            case '"': out->push_back('"'); break;
            case '\\': out->push_back('\\'); break;
            case '/': out->push_back('/'); break;
            case 'b': out->push_back('\b'); break;
            case 'f': out->push_back('\f'); break;
            case 'n': out->push_back('\n'); break;
            case 'r': out->push_back('\r'); break;
            case 't': out->push_back('\t'); break;
            case 'u': {
                unsigned cp = 0;
                if (!hex4(&cp)) return false;
                if (cp >= 0xD800 && cp < 0xDC00 && pos_ + 6 <= s_.size() &&
                    s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
                    pos_ += 2;
                    unsigned low = 0;
                    if (!hex4(&low)) return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                }
                append_utf8(out, cp);
                break;
            }
            default: return false;
            }
        }
        return false;
    }
    bool value(Json* out, int depth) {
        if (depth > 64) return false;
        ws();
        if (pos_ >= s_.size()) return false;
        const char c = s_[pos_];
        if (c == '{') {
            out->kind = Json::Object;
            ++pos_;
            ws();
            if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
            for (;;) {
                ws();
                std::string key;
                if (!string(&key)) return false;
                ws();
                if (pos_ >= s_.size() || s_[pos_++] != ':') return false;
                Json v;
                if (!value(&v, depth + 1)) return false;
                out->fields[key] = std::move(v);
                ws();
                if (pos_ >= s_.size()) return false;
                if (s_[pos_] == ',') { ++pos_; continue; }
                if (s_[pos_] == '}') { ++pos_; return true; }
                return false;
            }
        }
        if (c == '[') {
            out->kind = Json::Array;
            ++pos_;
            ws();
            if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }
            for (;;) {
                Json v;
                if (!value(&v, depth + 1)) return false;
                out->items.push_back(std::move(v));
                ws();
                if (pos_ >= s_.size()) return false;
                if (s_[pos_] == ',') { ++pos_; continue; }
                if (s_[pos_] == ']') { ++pos_; return true; }
                return false;
            }
        }
        if (c == '"') {
            out->kind = Json::String;
            return string(&out->text);
        }
        if (literal("true")) { out->kind = Json::Bool; out->boolean = true; return true; }
        if (literal("false")) { out->kind = Json::Bool; return true; }
        if (literal("null")) { out->kind = Json::Null; return true; }
        const std::size_t start = pos_;
        while (pos_ < s_.size() && std::strchr("+-0123456789.eE", s_[pos_])) ++pos_;
        if (pos_ == start) return false;
        out->kind = Json::Number;
        out->number = std::strtod(s_.substr(start, pos_ - start).c_str(), nullptr);
        return true;
    }
};

// ---------------------------------------------------------------- releases

struct Release {
    std::string tag;         // e.g. v0.2-test
    std::string title;       // the release's name
    std::string page;        // its GitHub page
    std::string notes;       // its release notes (GitHub markdown)
    std::string asset_name;  // this platform's zip
    std::string asset_url;
    std::uint64_t asset_size = 0;
};

// The newest release in GitHub's reply to kReleasesApiPath, with the zip for
// this platform: on Linux the asset with "linux" in its name, on Windows the
// .zip without it.
inline bool parse_newest_release(const std::string& reply, bool linux_build, Release* out,
                                 std::string* error) {
    Json root;
    if (!JsonParser(reply).parse(&root) || root.kind != Json::Array) {
        *error = "GitHub's answer could not be read.";
        return false;
    }
    if (root.items.empty()) {
        *error = "There are no releases yet.";
        return false;
    }
    const Json& rel = root.items.front();
    out->tag = rel.str("tag_name");
    out->title = rel.str("name");
    out->page = rel.str("html_url");
    out->notes = rel.str("body");
    const Json* assets = rel.get("assets");
    if (out->tag.empty() || !assets || assets->kind != Json::Array) {
        *error = "The newest release has no downloads.";
        return false;
    }
    for (const Json& a : assets->items) {
        std::string name = a.str("name");
        std::string lower = name;
        for (char& c : lower)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        const bool is_zip = lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".zip") == 0;
        const bool is_linux = lower.find("linux") != std::string::npos;
        if (!is_zip || is_linux != linux_build) continue;
        out->asset_name = name;
        out->asset_url = a.str("browser_download_url");
        out->asset_size = static_cast<std::uint64_t>(a.num("size"));
        return !out->asset_url.empty();
    }
    *error = std::string("The newest release has no download for ") +
             (linux_build ? "Linux." : "Windows.");
    return false;
}

inline bool is_newer_release(const Release& r) {
    return update_checks_enabled() && !r.tag.empty() && r.tag != kReleaseVersion;
}

// Release notes (GitHub markdown) as plain text for display: headings keep
// their text, bold and code marks go, list items get a bullet (two spaces of
// indent per nesting level), [text](url) becomes text.
inline std::string notes_to_plain_text(const std::string& markdown) {
    std::string text;
    for (std::size_t i = 0; i < markdown.size(); ++i)
        if (markdown[i] != '\r') text.push_back(markdown[i]);
    std::string out;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        std::size_t indent = 0;
        while (indent < line.size() && (line[indent] == ' ' || line[indent] == '\t')) ++indent;
        std::string body = line.substr(indent);
        if (!body.empty() && body[0] == '#') {
            std::size_t h = 0;
            while (h < body.size() && body[h] == '#') ++h;
            while (h < body.size() && body[h] == ' ') ++h;
            body = body.substr(h);
            indent = 0;
        } else if (body.size() > 1 && (body[0] == '-' || body[0] == '*') && body[1] == ' ') {
            body = "\xE2\x80\xA2 " + body.substr(2);
            indent = (indent / 2) * 2;
        } else {
            indent = 0;
        }
        // Inline marks: ** __ ` and [text](url).
        std::string clean;
        for (std::size_t i = 0; i < body.size(); ++i) {
            const char c = body[i];
            if (c == '`') continue;
            if ((c == '*' || c == '_') && i + 1 < body.size() && body[i + 1] == c) {
                ++i;
                continue;
            }
            if (c == '[') {
                const std::size_t close = body.find("](", i);
                const std::size_t paren = close == std::string::npos ? close : body.find(')', close);
                if (paren != std::string::npos) {
                    clean += body.substr(i + 1, close - i - 1);
                    i = paren;
                    continue;
                }
            }
            clean.push_back(c);
        }
        out += std::string(indent, ' ') + clean + "\n";
        if (end == text.size()) break;
    }
    // At most one blank line in a row, no blank lines at either end.
    std::string collapsed;
    int newlines = 0;
    for (char c : out) {
        newlines = c == '\n' ? newlines + 1 : 0;
        if (newlines <= 2) collapsed.push_back(c);
    }
    const std::size_t first = collapsed.find_first_not_of("\n");
    if (first == std::string::npos) return std::string();
    const std::size_t last = collapsed.find_last_not_of(" \n");
    return collapsed.substr(first, last - first + 1);
}

// ---------------------------------------------------------------- reports

// For the X-GSR-Description header: UTF-8, every byte outside
// [A-Za-z0-9-_.~ ] percent-encoded (the service decodes it).
inline std::string percent_encode(const std::string& s, std::size_t max_bytes = 1500) {
    static const char* digits = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s.substr(0, max_bytes)) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(digits[c >> 4]);
            out.push_back(digits[c & 0xF]);
        }
    }
    return out;
}

}  // namespace gsr_online
