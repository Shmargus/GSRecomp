#pragma once

#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace gsr_player {

inline constexpr char kBuilderVersion[] = "2";

struct BuildRecord {
    std::string rom_sha1, builder, engine_sha1;
    std::optional<std::string> library_sha1;
};

inline constexpr std::string_view kRomField = "rom_sha1=";
inline constexpr std::string_view kBuilderField = "builder=";
inline constexpr std::string_view kEngineField = "engine_sha1=";
inline constexpr std::string_view kLibraryField = "library_sha1=";

inline BuildRecord read_build_record(const std::string& text) {
    BuildRecord record;
    std::istringstream input(text);
    for (std::string line; std::getline(input, line);) {
        if (line.starts_with(kRomField)) record.rom_sha1 = line.substr(kRomField.size());
        else if (line.starts_with(kBuilderField)) record.builder = line.substr(kBuilderField.size());
        else if (line.starts_with(kEngineField)) record.engine_sha1 = line.substr(kEngineField.size());
        else if (line.starts_with(kLibraryField)) record.library_sha1 = line.substr(kLibraryField.size());
    }
    return record;
}

inline std::string write_build_record(const BuildRecord& record) {
    std::string text;
    text.append(kRomField).append(record.rom_sha1).append("\n")
        .append(kBuilderField).append(record.builder).append("\n")
        .append(kEngineField).append(record.engine_sha1).append("\n");
    if (record.library_sha1)
        text.append(kLibraryField).append(*record.library_sha1).append("\n");
    return text;
}

}  // namespace gsr_player
