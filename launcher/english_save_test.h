#pragma once
#include "collection_storage.h"

namespace gsr_collection {
// Only the developer collection launcher creates these copies. The ROM and
// normal save/state paths remain language-specific; every test launch gets
// a fresh directory and never imports cross-revision savestates.
struct TestProgress { fs::path save, state_base; };
inline fs::path english_save_tests(const fs::path& collection) {
    return fs::absolute(collection).lexically_normal() / "Saves" / "English save tests";
}

inline bool create_english_save_test(const fs::path& collection, int game,
                                     TestProgress& result, std::string& error) {
    result = {};
    error.clear();
    try {
        if (game < 0 || game >= static_cast<int>(std::size(gsr_launcher::kGames)))
            throw std::runtime_error("Unknown game for English save testing.");
        const std::string stem = gsr_launcher::kGames[game].stem;
        const auto normal = managed_paths(collection, stem);
        const auto source = managed_paths(normal.root, stem, 0).save;
        if (!fs::is_regular_file(source) || fs::is_symlink(fs::symlink_status(source)))
            throw std::runtime_error("No English in-game save found for " + stem +
                ". Start the English game and save first.");
        const auto folder = english_save_tests(normal.root);
        fs::create_directories(folder);
        fs::path directory;
        do { directory = storage_detail::unused_path(folder / normal.state_base.filename()); }
        while (!fs::create_directory(directory));
        const auto state_base = directory / normal.state_base.filename();
        const auto save = fs::path(state_base.native() + fs::path(".sav").native());
        storage_detail::copy_preserving_time(source, save);
        result = {save, state_base};
        return true;
    } catch (const std::exception& failure) {
        error = "Could not prepare an English test save: " + std::string(failure.what());
        return false;
    }
}

// A child session accepts only an existing isolated copy, never a normal save
// supplied through the developer-only argument. It does not make another copy
// when retrying graphics startup or restarting inside the same session.
inline bool open_english_save_test(const fs::path& collection, int game,
                                   const fs::path& state_base,
                                   TestProgress& result, std::string& error) {
    result = {};
    error.clear();
    try {
        if (game < 0 || game >= static_cast<int>(std::size(gsr_launcher::kGames)))
            throw std::runtime_error("Unknown game for English save testing.");
        const auto normal = managed_paths(collection, gsr_launcher::kGames[game].stem);
        const auto base = state_base.lexically_normal();
        if (!state_base.is_absolute() || base.filename() != normal.state_base.filename() ||
            fs::canonical(base.parent_path()).parent_path() != fs::canonical(english_save_tests(normal.root)))
            throw std::runtime_error("The test save must be in its own English save tests folder.");
        const auto save = fs::path(base.native() + fs::path(".sav").native());
        if (!fs::is_regular_file(save) || fs::is_symlink(fs::symlink_status(save)))
            throw std::runtime_error("The English test save is missing or is not a regular file.");
        result = {save, base};
        return true;
    } catch (const std::exception& failure) {
        error = "Could not open the English test save: " + std::string(failure.what());
        return false;
    }
}
} // namespace gsr_collection
