#pragma once
#include "collection_storage.h"
#include "collection_popup_text.h"
#include <optional>

namespace gsr_collection {
struct SaveLanguageImport {
    fs::path source, destination, declined;
    int game, language;
};

// Called only after a successful language-ROM build. Existing localized
// progress always wins; declining is remembered for this game/language.
inline std::optional<SaveLanguageImport> pending_save_language_import(
        const fs::path& collection, int game, int language) {
    if (game < 0 || game >= static_cast<int>(std::size(gsr_launcher::kGames)))
        throw std::runtime_error("Unknown game for save conversion.");
    const auto& identity = gsr_launcher::kGames[game];
    if (language < 0 || language >= gsr_launcher::rom_language_count(identity))
        throw std::runtime_error("Unknown language for save conversion.");
    if (language == 0) return std::nullopt;
    const auto target = managed_paths(collection, identity.stem, language);
    const auto source = managed_paths(collection, identity.stem, 0).save;
    const auto declined = target.root / "local" / "save-language-import" /
        fs::path(target.state_base.filename().native() + fs::path(".declined").native());
    if (storage_detail::path_present(target.save) || storage_detail::path_present(declined))
        return std::nullopt;
    std::error_code ec;
    if (!fs::is_regular_file(source, ec) || ec || fs::is_symlink(fs::symlink_status(source)))
        return std::nullopt;
    // Use the same battery-save file the English game uses, not a savestate.
    // Loading the copy in the other language invokes the game's own sanctuary
    // recovery (confirmed by Jimmy in both games); no save bytes are patched.
    return SaveLanguageImport{source, target.save, declined, game, language};
}

inline std::string save_language_import_message(const SaveLanguageImport& offer) {
    return popup_game_names(collection_popup_text(offer.language).save_question, offer.game);
}

inline bool finish_save_language_import(const SaveLanguageImport& offer, bool convert,
                                        std::string& error) {
    error.clear();
    try {
        if (convert) {
            // Never replace progress created while the question was open.
            if (storage_detail::path_present(offer.destination))
                throw std::runtime_error("A save for this language already exists. It was left unchanged.");
            storage_detail::copy_preserving_time(offer.source, offer.destination);
        } else {
            fs::create_directories(offer.declined.parent_path());
            std::ofstream answer(offer.declined, std::ios::trunc);
            answer << "declined\n";
            answer.close();
            if (!answer) throw std::runtime_error("Could not remember your save-conversion choice.");
        }
        return true;
    } catch (const std::exception& failure) {
        error = "Could not convert the English save: " + std::string(failure.what());
        return false;
    }
}
} // namespace gsr_collection
