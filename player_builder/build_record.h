#pragma once

#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace gsr_player {

// Bump this when a build or link change is outside build_inputs_sha1.
// Libraries built under another version are rebuilt.
inline constexpr char kBuilderVersion[] = "3";

struct BuildRecord {
    std::string rom_sha1, builder, engine_sha1;
    std::optional<std::string> library_sha1;
    // Everything the compile and link read (builder_main.cpp
    // build_inputs_sha1). Records from before 0.8.4 have none.
    std::optional<std::string> inputs_sha1;
};

inline constexpr std::string_view kRomField = "rom_sha1=";
inline constexpr std::string_view kBuilderField = "builder=";
inline constexpr std::string_view kEngineField = "engine_sha1=";
inline constexpr std::string_view kLibraryField = "library_sha1=";
inline constexpr std::string_view kInputsField = "inputs_sha1=";

inline BuildRecord read_build_record(const std::string& text) {
    BuildRecord record;
    std::istringstream input(text);
    for (std::string line; std::getline(input, line);) {
        if (line.starts_with(kRomField)) record.rom_sha1 = line.substr(kRomField.size());
        else if (line.starts_with(kBuilderField)) record.builder = line.substr(kBuilderField.size());
        else if (line.starts_with(kEngineField)) record.engine_sha1 = line.substr(kEngineField.size());
        else if (line.starts_with(kLibraryField)) record.library_sha1 = line.substr(kLibraryField.size());
        else if (line.starts_with(kInputsField)) record.inputs_sha1 = line.substr(kInputsField.size());
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
    if (record.inputs_sha1)
        text.append(kInputsField).append(*record.inputs_sha1).append("\n");
    return text;
}

}  // namespace gsr_player
