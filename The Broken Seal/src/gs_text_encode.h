// Encoder for Golden Sun's Huffman-coded text, and the block rebuild that
// swaps edited lines into an extension of the in-memory ROM. C++ port of the
// reference tools/gs_text_encode.py (format: see its docstring; FACTS.md
// 2026-09-30). Pure functions over a ROM byte vector: no game state, no I/O.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace gsr::mods {

constexpr std::uint32_t kRomBase = 0x08000000u;
constexpr std::size_t kTextTable = 0x736B8;  // per 256-line block: data, lengths
constexpr std::size_t kTreeTable = 0x3842C;
constexpr std::uint32_t kLastTextLine = 0x29E1;  // real text ends here
constexpr std::size_t kMaxRomBytes = 16u * 1024u * 1024u;

class TextEncoder {
public:
    // The ROM must outlive the encoder and must not change while it is used
    // (the tree codes are cached).
    explicit TextEncoder(const std::vector<std::uint8_t>& rom) : rom_(rom) {}

    // Encodes ASCII text (each char is its own 12-bit code) followed by the
    // terminating 0, LSB first. False when a needed (previous, next)
    // character pair has no code in the game's trees; *why says which.
    bool encode(const std::string& text, std::vector<std::uint8_t>* out,
                std::string* why);

    // Stored start (ROM offset) and byte size of one line, as the game's
    // length table gives them. False if the table runs off the ROM.
    bool line_bytes(std::uint32_t number, std::size_t* start,
                    std::size_t* size) const;

private:
    using Path = std::vector<std::uint8_t>;
    bool codes_for(std::uint32_t prev, const std::map<std::uint32_t, Path>** out);

    const std::vector<std::uint8_t>& rom_;
    std::map<std::uint32_t, std::map<std::uint32_t, Path>> cache_;
};

// Rebuilds the 256-line (or shorter, for the last) block `block`: every
// line's bytes concatenated, unchanged lines copied byte-exact, lines in
// `edited` replaced by the given already-encoded bytes. The new data and new
// length table are appended (4-byte aligned) to `*rom` and the block's entry
// in the text table is pointed at them. False (ROM untouched) on failure.
bool rebuild_text_block(std::vector<std::uint8_t>* rom, std::uint32_t block,
                        const std::map<std::uint32_t,
                                       std::vector<std::uint8_t>>& edited,
                        std::string* why);

}  // namespace gsr::mods
