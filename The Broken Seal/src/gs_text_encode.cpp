#include "gs_text_encode.h"

#include <algorithm>
#include <cstdio>

namespace gsr::mods {
namespace {

std::uint32_t rd32(const std::vector<std::uint8_t>& r, std::size_t p) {
    return static_cast<std::uint32_t>(r[p]) |
           (static_cast<std::uint32_t>(r[p + 1]) << 8) |
           (static_cast<std::uint32_t>(r[p + 2]) << 16) |
           (static_cast<std::uint32_t>(r[p + 3]) << 24);
}

void wr32(std::vector<std::uint8_t>& r, std::size_t p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) r[p + i] = static_cast<std::uint8_t>(v >> (8 * i));
}

// Reads a preorder tree bit string LSB first.
struct TreeBits {
    const std::vector<std::uint8_t>& rom;
    std::size_t addr;
    std::size_t pos = 0;
    bool ok = true;
    int bit() {
        const std::size_t at = addr + (pos >> 3);
        if (at >= rom.size()) { ok = false; return 0; }
        const int b = (rom[at] >> (pos & 7)) & 1;
        ++pos;
        return b;
    }
};

}  // namespace

bool TextEncoder::codes_for(std::uint32_t prev,
                            const std::map<std::uint32_t, Path>** out) {
    auto found = cache_.find(prev);
    if (found != cache_.end()) { *out = &found->second; return true; }

    const std::size_t entry = kTreeTable + (prev >> 8) * 8;
    if (entry + 8 > rom_.size()) return false;
    const std::uint32_t base = rd32(rom_, entry);
    const std::uint32_t offsets = rd32(rom_, entry + 4);
    if (base < kRomBase || offsets < kRomBase) return false;
    const std::size_t off_at = offsets - kRomBase + (prev & 0xFF) * 2;
    if (off_at + 2 > rom_.size()) return false;
    const std::size_t off = rom_[off_at] | (rom_[off_at + 1] << 8);
    const std::size_t tree = base - kRomBase + off;

    std::map<std::uint32_t, Path> codes;
    TreeBits bits{rom_, tree};
    std::size_t leaves = 0;
    bool bad = false;
    // Preorder: bit 1 = leaf, 0 = internal node with two children.
    struct Walk {
        const std::vector<std::uint8_t>& rom;
        TreeBits& bits;
        std::size_t tree;
        std::size_t& leaves;
        bool& bad;
        std::map<std::uint32_t, Path>& codes;
        void go(Path& path, int depth) {
            if (bad || !bits.ok || depth > 512) { bad = true; return; }
            if (bits.bit()) {
                const std::size_t index = leaves++;
                const std::size_t p = tree - (index + (index >> 1));
                if (p < 2 || p - 1 >= rom.size()) { bad = true; return; }
                const unsigned hi = rom[p - 1], lo = rom[p - 2];
                const std::uint32_t ch = (index & 1)
                                             ? (((hi & 0x0Fu) << 8) | lo)
                                             : ((hi << 4) | (lo >> 4));
                codes[ch] = path;
                return;
            }
            path.push_back(0);
            go(path, depth + 1);
            path.back() = 1;
            go(path, depth + 1);
            path.pop_back();
        }
    } walk{rom_, bits, tree, leaves, bad, codes};
    Path path;
    walk.go(path, 0);
    if (bad || !bits.ok) return false;
    auto inserted = cache_.emplace(prev, std::move(codes));
    *out = &inserted.first->second;
    return true;
}

bool TextEncoder::encode(const std::string& text,
                         std::vector<std::uint8_t>* out, std::string* why) {
    std::vector<std::uint8_t> bytes;
    unsigned acc = 0;
    int n = 0;
    std::uint32_t prev = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        const std::uint32_t c =
            i < text.size() ? static_cast<unsigned char>(text[i]) : 0u;
        const std::map<std::uint32_t, Path>* codes = nullptr;
        if (!codes_for(prev, &codes)) {
            if (why) *why = "no tree for previous character " +
                            std::to_string(prev);
            return false;
        }
        const auto code = codes->find(c);
        if (code == codes->end()) {
            if (why) *why = "no code for pair (" + std::to_string(prev) + ", " +
                            std::to_string(c) + ")";
            return false;
        }
        for (const std::uint8_t b : code->second) {
            acc |= static_cast<unsigned>(b) << n;
            if (++n == 8) {
                bytes.push_back(static_cast<std::uint8_t>(acc));
                acc = 0;
                n = 0;
            }
        }
        prev = c;
    }
    if (n) bytes.push_back(static_cast<std::uint8_t>(acc));
    *out = std::move(bytes);
    return true;
}

bool TextEncoder::line_bytes(std::uint32_t number, std::size_t* start,
                             std::size_t* size) const {
    const std::size_t entry = kTextTable + (number >> 8) * 8;
    if (entry + 8 > rom_.size()) return false;
    const std::uint32_t data = rd32(rom_, entry);
    const std::uint32_t lengths = rd32(rom_, entry + 4);
    if (data < kRomBase || lengths < kRomBase) return false;
    std::size_t p = lengths - kRomBase;
    std::size_t s = data - kRomBase;
    for (std::uint32_t i = 0; i < (number & 0xFF); ++i) {
        while (p < rom_.size() && rom_[p] == 0xFF) { s += 0xFF; ++p; }
        if (p >= rom_.size()) return false;
        s += rom_[p++];
    }
    std::size_t sz = 0;
    while (p < rom_.size() && rom_[p] == 0xFF) { sz += 0xFF; ++p; }
    if (p >= rom_.size()) return false;
    sz += rom_[p];
    if (s + sz > rom_.size()) return false;
    *start = s;
    *size = sz;
    return true;
}

bool rebuild_text_block(std::vector<std::uint8_t>* rom, std::uint32_t block,
                        const std::map<std::uint32_t,
                                       std::vector<std::uint8_t>>& edited,
                        std::string* why) {
    auto fail = [&](const char* m) {
        if (why) *why = m;
        return false;
    };
    if (!rom) return fail("no rom");
    const std::uint32_t first = block * 256u;
    if (first > kLastTextLine) return fail("block is past the real text");
    const std::uint32_t lines =
        std::min<std::uint32_t>(256u, kLastTextLine + 1u - first);
    const std::size_t entry = kTextTable + block * 8u;
    if (entry + 8 > rom->size()) return fail("text table entry out of range");

    std::vector<std::uint8_t> data, lengths;
    {
        TextEncoder reader(*rom);
        for (std::uint32_t i = 0; i < lines; ++i) {
            const std::uint32_t number = first + i;
            const auto edit = edited.find(number);
            std::size_t size;
            if (edit != edited.end()) {
                data.insert(data.end(), edit->second.begin(),
                            edit->second.end());
                size = edit->second.size();
            } else {
                std::size_t start;
                if (!reader.line_bytes(number, &start, &size))
                    return fail("could not read an existing line");
                data.insert(data.end(), rom->begin() + start,
                            rom->begin() + start + size);
            }
            std::size_t left = size;
            while (left >= 0xFF) { lengths.push_back(0xFF); left -= 0xFF; }
            lengths.push_back(static_cast<std::uint8_t>(left));
        }
    }

    auto align4 = [](std::size_t v) { return (v + 3) & ~std::size_t(3); };
    const std::size_t data_at = align4(rom->size());
    const std::size_t lengths_at = align4(data_at + data.size());
    const std::size_t end = lengths_at + lengths.size();
    if (end > kMaxRomBytes) return fail("ROM extension would exceed 16 MB");
    rom->resize(end, 0);
    std::copy(data.begin(), data.end(), rom->begin() + data_at);
    std::copy(lengths.begin(), lengths.end(), rom->begin() + lengths_at);
    wr32(*rom, entry, kRomBase + static_cast<std::uint32_t>(data_at));
    wr32(*rom, entry + 4, kRomBase + static_cast<std::uint32_t>(lengths_at));
    return true;
}

}  // namespace gsr::mods
