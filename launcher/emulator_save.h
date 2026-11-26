#pragma once

// Reads battery saves made by emulators (mGBA, VisualBoyAdvance, RetroArch
// .srm...).
//
// Both games keep a 64 KiB flash chip. mGBA's Golden Sun .sav is that image
// byte for byte (measured). Other emulators may write it differently:
//   - A larger file (a 128 KiB chip, with data after it): the first 64 KiB is
//     kept when nothing after it is a Golden Sun record.
//   - A shorter file: it is padded with erased flash (0xFF), as the engine
//     itself loads one (gba_save.cpp load_flash_bytes).
//
// The game a save belongs to comes from its own records:
//   - The Broken Seal checksums each 0x1000-byte sector
//     (The Lost Age/src/gs_save.cpp).
//   - The Lost Age checksums each 0x3000-byte group (tla_save_update.cpp).
// Measured on 12 saves of both games and every revision: each matched exactly
// one of the two rules.
#include "collection_storage.h"
#include "../shared_game/save_record.h"
#include <cstdint>
#include <iterator>

namespace gsr_collection::emulator_save {
constexpr std::size_t kFlashBytes = 0x10000;

namespace detail {
namespace record = gsr::save_files;
inline int broken_seal_records(const std::uint8_t* image) {
    int count = 0;
    for (std::size_t at = 0; at + 0x1000 <= kFlashBytes; at += 0x1000) {
        const auto* p = image + at;
        count += p[7] < 16 && record::valid(p, 0xFE8);
    }
    return count;
}
inline int lost_age_records(const std::uint8_t* image) {
    int count = 0;
    for (std::size_t at = 0; at + 0x3000 <= kFlashBytes; at += 0x3000) {
        const auto* p = image + at;
        count += p[7] < 3 && record::valid(p, 0x3000 - record::kRecordHeader) && (p[10] | p[11]) != 0;
    }
    return count;
}
}  // namespace detail

// `file`'s 64 KiB flash image in `image`, and its game: 0 The Broken Seal,
// 1 The Lost Age, -1 (with `error`) when it is not a usable Golden Sun save.
inline int flash_image(const std::vector<std::uint8_t>& file, std::vector<std::uint8_t>& image,
                       std::string& error) {
    image.assign(kFlashBytes, 0xFF);
    std::copy_n(file.begin(), std::min(file.size(), kFlashBytes), image.begin());
    for (std::size_t at = kFlashBytes; at + 7 <= file.size(); at += 0x1000)
        if (gsr::save_files::present(file.data() + at)) {
            error = "This save holds more than one Golden Sun save area, so it is not clear which one to use.";
            return -1;
        }
    const int broken_seal = detail::broken_seal_records(image.data());
    const int lost_age = detail::lost_age_records(image.data());
    if (broken_seal > 0 && lost_age == 0) return 0;
    if (lost_age > 0 && broken_seal == 0) return 1;
    error = "This file is not a Golden Sun save, or it has no saved game in it.";
    return -1;
}

inline bool read_bytes(const fs::path& path, std::vector<std::uint8_t>& out) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    out.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return !input.bad();
}

// Make `image` the save at `target` (one of `files`' saves). A different save
// already there is kept first in Saves/Imported, as every import does.
inline void install(const std::vector<std::uint8_t>& image, const fs::path& target, const ManagedFiles& files) {
    using namespace storage_detail;
    fs::create_directories(target.parent_path());
    const auto staged = unused_path(fs::path(target.native() + fs::path(".emulator.tmp").native()));
    try {
        {
            std::ofstream output(staged, std::ios::binary | std::ios::trunc);
            if (!output || !output.write(reinterpret_cast<const char*>(image.data()),
                                         static_cast<std::streamsize>(image.size())))
                throw std::runtime_error("Cannot write " + text(staged));
        }
        if (path_present(target) && !same_bytes(staged, target)) preserve_conflict(target, files);
        copy_preserving_time(staged, target, true);
    } catch (...) {
        std::error_code ignored;
        fs::remove(staged, ignored);
        throw;
    }
    fs::remove(staged);
}

// At game start: a save larger than 64 KiB (an emulator's, brought in with its
// ROM) becomes its 64 KiB image, the original kept in Saves/Imported. False,
// with `error`, when it is not a usable save of `game`.
inline bool repair_oversized(const ManagedFiles& files, const fs::path& save, int game, std::string& error) {
    error.clear();
    try {
        std::vector<std::uint8_t> file, image;
        if (!read_bytes(save, file)) { error = "Cannot read " + storage_detail::text(save); return false; }
        const int found = flash_image(file, image, error);
        if (found < 0) return false;
        if (found != game) {
            error = std::string("This save is from ") + (found ? "The Lost Age" : "The Broken Seal") + ".";
            return false;
        }
        install(image, save, files);
        return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
}  // namespace gsr_collection::emulator_save
