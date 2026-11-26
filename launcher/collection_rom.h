#pragma once

#include "collection_build.h"
#include "collection_storage.h"
#include "emulator_save.h"
#include "collection_session.h"
#include "collection_preferences.h"
#include "launcher_common.h"
#include "launcher_game.h"
#include <SDL.h>
#include <cerrno>
#include <cstdlib>
#include <iterator>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <SDL_syswm.h>
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#ifdef __ANDROID__
#include "android_host.h"
#endif

namespace gsr_launcher {
bool sha1_file(const std::filesystem::path&, std::string*);
}

namespace gsr_collection {
namespace rom_detail {
inline void trim_newlines(std::string& value) {
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) value.pop_back();
}
// The game's ROM check, the same one its session makes (launcher_common.h),
// except that it accepts only the language the game's flag is set to.
// `any_rom` is that game's "Skip the ROM check" setting.
inline bool validate(const fs::path& path, int choice, bool any_rom, std::string& error,
                     int language, bool same_language = false) {
    return gsr_launcher::validate_rom(path, &error, gsr_launcher::kGames[choice], any_rom,
                                      language, same_language);
}
#ifndef _WIN32
inline fs::path program(const char* name) {
    const char* environment = std::getenv("PATH");
    if (!environment) return {};
    const std::string paths(environment);
    for (std::size_t begin = 0; begin <= paths.size();) {
        const auto end = paths.find(':', begin);
        const auto directory = paths.substr(begin, end == std::string::npos ? end : end - begin);
        const auto candidate = fs::path(directory.empty() ? "." : directory) / name;
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec) && access(candidate.c_str(), X_OK) == 0) return fs::absolute(candidate);
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return {};
}
inline bool run_picker(const fs::path& executable, std::vector<std::string> arguments,
                       std::string& output, std::string& error) {
    arguments.insert(arguments.begin(), executable.string());
    std::vector<char*> argv;
    for (auto& argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    int descriptors[2];
    if (pipe(descriptors) != 0) { error = "Cannot open the ROM file chooser."; return false; }
    const pid_t child = fork();
    if (child == 0) {
        close(descriptors[0]);
        if (dup2(descriptors[1], STDOUT_FILENO) < 0) _exit(127);
        close(descriptors[1]);
        const int quiet = open("/dev/null", O_WRONLY);
        if (quiet >= 0) { dup2(quiet, STDERR_FILENO); close(quiet); }
        execv(executable.c_str(), argv.data());
        _exit(127);
    }
    close(descriptors[1]);
    if (child < 0) { close(descriptors[0]); error = "Cannot start the ROM file chooser."; return false; }
    char buffer[4096];
    bool read_failed = false;
    for (;;) {
        const auto count = read(descriptors[0], buffer, sizeof(buffer));
        if (count > 0) output.append(buffer, static_cast<std::size_t>(count));
        else if (count < 0 && errno == EINTR) continue;
        else { read_failed = count < 0; break; }
    }
    close(descriptors[0]);
    int status = 0;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    if (read_failed || waited < 0 || !WIFEXITED(status) || WEXITSTATUS(status) > 1) {
        error = "The ROM file chooser failed."; return false;
    }
    if (WEXITSTATUS(status) == 1) return false;
    trim_newlines(output);
    return !output.empty();
}
#endif
// What a file window lists first: `name`, files ending in `extensions`.
struct FileKind {
    const char* name;
    std::vector<std::string> extensions;
    const char* android_copy;  // the picked file's copy in local (Android)
};
inline const FileKind& rom_files() {
    static const FileKind kind{"Golden Sun ROM", {"gba"}, "picked-rom.gba"};
    return kind;
}
inline std::string patterns(const FileKind& kind, const char* separator, bool upper) {
    std::string out;
    for (const auto& extension : kind.extensions) {
        out += (out.empty() ? "*." : separator + std::string("*.")) + extension;
        if (upper) {
            std::string capital = extension;
            for (char& c : capital) if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
            out += separator + std::string("*.") + capital;
        }
    }
    return out;
}
inline bool pick(SDL_Window* window, const fs::path& folder, int choice, int language,
                 fs::path& selected, std::string& error, const char* step = "",
                 const char* what = nullptr, const FileKind& kind = rom_files()) {
    const auto& game = gsr_launcher::kGames[choice];
    // `step` names which of the two "Skip the ROM check" file windows this is;
    // `what`, when given, names the ROM asked for instead of `language`'s.
    const std::string rom = what ? std::string(what)
                                 : std::string(game.stem) + " ROM (" + game.rom_names[language] + ")";
    const std::string title = *step ? std::string(step) + rom : "Select " + rom;
    const std::string listed = std::string(kind.name) + " (" + patterns(kind, ", ", false) + ")";
#ifdef _WIN32
    SDL_SysWMinfo info{};
    SDL_VERSION(&info.version);
    if (!window || !SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_WINDOWS) {
        error = "Cannot attach the ROM file chooser to the launcher window."; return false;
    }
    std::vector<wchar_t> filename(32768, L'\0');
    const auto initial = folder.wstring();
    const std::wstring caption(title.begin(), title.end());
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = info.info.win.window;
    const std::string filter_text = listed + '\0' + patterns(kind, ";", false) + '\0' + "All files" + '\0' + "*.*" + '\0';
    const std::wstring filter(filter_text.begin(), filter_text.end());
    dialog.lpstrFilter = filter.c_str();
    dialog.lpstrFile = filename.data();
    dialog.nMaxFile = static_cast<DWORD>(filename.size());
    dialog.lpstrInitialDir = initial.c_str();
    dialog.lpstrTitle = caption.c_str();
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&dialog)) {
        const auto code = CommDlgExtendedError();
        if (code) error = "The ROM file chooser failed (" + std::to_string(code) + ").";
        return false;
    }
    selected = filename.data();
    return true;
#elif defined(__ANDROID__)
    // The system file picker copies the file into the collection; the
    // caller checks it and imports it as the PC launcher does.
    (void)window;
    (void)folder;
    const fs::path copy = gsr_android::storage_root() / "local" / kind.android_copy;
    const std::string picked = gsr_android::pick_file(title, copy);
    if (picked.empty()) return false;
    selected = fs::path(picked);
    return true;
#else
    (void)window;
    std::string output;
    const auto zenity = program("zenity");
    bool accepted;
    if (!zenity.empty()) accepted = run_picker(zenity, {"--file-selection", "--title=" + title,
        "--file-filter=" + listed + " | " + patterns(kind, " ", true), "--file-filter=All files | *",
        "--filename=" + folder.string() + "/"}, output, error);
    else {
        const auto kdialog = program("kdialog");
        if (kdialog.empty()) { error = "No ROM file chooser was found. Install zenity or kdialog and try again."; return false; }
        accepted = run_picker(kdialog, {"--title", title, "--getopenfilename", folder.string(),
            patterns(kind, " ", true) + "|" + listed + "\n*|All files"}, output, error);
    }
    if (!accepted) return false;
    selected = fs::path(reinterpret_cast<const char8_t*>(output.c_str()));
    return true;
#endif
}
// Pads a ROM that is slightly short of a full cartridge.
// IPS patchers that honour a patch's truncation record can leave a ROM short
// (Maheo Hack 2.0: one byte short of 16 MiB, measured). Patchers that ignore
// the record keep the clean ROM's zero there, and that 16 MiB file is the
// supported one. So a ROM under 16 MiB is padded with zeros, in a copy in the
// collection's local folder. When the copy is a supported ROM of `choice`,
// `selected` names the copy.
inline void pad_short_rom(const fs::path& root, int choice, fs::path& selected) {
    constexpr std::uintmax_t kCartridge = 0x01000000;
    std::error_code ec;
    const auto size = fs::file_size(selected, ec);
    if (ec || size >= kCartridge) return;
    std::vector<char> bytes(kCartridge, 0);
    {
        std::ifstream input(selected, std::ios::binary);
        if (!input || !input.read(bytes.data(), static_cast<std::streamsize>(size))) return;
    }
    const fs::path padded = root / "local" / "padded-rom.gba";
    fs::create_directories(padded.parent_path(), ec);
    {
        std::ofstream output(padded, std::ios::binary | std::ios::trunc);
        if (!output || !output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()))) return;
    }
    std::string sha1;
    if (gsr_launcher::sha1_file(padded, &sha1) && gsr_launcher::accepts_rom(gsr_launcher::kGames[choice], sha1))
        selected = padded;
    else
        fs::remove(padded, ec);
}
} // namespace rom_detail

// The Romhack button's "Load another romhack". The player picks a supported
// romhack's ROM, which then:
//   - becomes the game's revision (rom_language), and
//   - is copied into ROMs under the hack's own name, with its own saves
//     (import_selected_rom).
// `revision` is the hack's row.
inline bool load_romhack(SDL_Window* window, const fs::path& collection, int choice,
                         int& revision, std::string& error) {
    error.clear();
    revision = -1;
    if (choice < 0 || choice > 1) { error = "Unknown collection game."; return false; }
    if (collection.empty()) { error = "Cannot locate the collection launcher folder."; return false; }
    try {
        const auto root = fs::absolute(collection).lexically_normal();
        const auto& game = gsr_launcher::kGames[choice];
        const std::string what = std::string(game.stem) + " romhack ROM";
        fs::path selected;
        if (!rom_detail::pick(window, root / "ROMs", choice, 0, selected, error, "", what.c_str())) return false;
        rom_detail::pad_short_rom(root, choice, selected);
        std::string sha1;
        const int found = gsr_launcher::sha1_file(selected, &sha1) ? gsr_launcher::rom_revision_of(game, sha1) : -1;
        if (!gsr_launcher::rom_is_romhack(game, found)) {
            std::string supported;
            for (int r = 0; r < gsr_launcher::rom_revision_count(game); ++r)
                if (gsr_launcher::rom_is_romhack(game, r)) supported += std::string("\n") + game.rom_names[r];
            error = std::string("That is not a supported ") + game.stem + " romhack.\n\nSupported romhacks:" +
                    supported + "\n\nPatch the USA/Europe ROM with the hack's own patch file, then pick the result.";
            return false;
        }
        const int previous = rom_language(root, choice);
        if (!set_rom_language(root, found, choice)) { error = "Could not save the ROM choice."; return false; }
        ManagedFiles managed;
        if (!import_selected_rom(root, selected, game.stem, managed, error)) {
            set_rom_language(root, previous, choice);
            return false;
        }
        revision = found;
        return true;
    } catch (const std::exception& e) { error = "Cannot load the romhack: " + std::string(e.what()); return false; }
}

// The launcher's "Import emulator save". The player picks a battery save
// (mGBA, VisualBoyAdvance, RetroArch...). After the player agrees, it becomes
// the save of its own game's chosen revision. The save it replaces is kept in
// Saves/Imported. `done` says what happened, for the closing message.
inline bool import_emulator_save(SDL_Window* window, const fs::path& collection,
                                 std::string& done, std::string& error) {
    error.clear();
    done.clear();
    if (collection.empty()) { error = "Cannot locate the collection launcher folder."; return false; }
    try {
        const auto root = fs::absolute(collection).lexically_normal();
        static const rom_detail::FileKind kSaves{"Emulator save", {"sav", "srm"}, "picked-save.sav"};
        fs::path selected;
        if (!rom_detail::pick(window, root / "Saves", 0, 0, selected, error, "", "emulator save", kSaves)) return false;
        std::vector<std::uint8_t> file, image;
        if (!emulator_save::read_bytes(selected, file)) { error = "Cannot read " + storage_detail::text(selected); return false; }
        const int game = emulator_save::flash_image(file, image, error);
        if (game < 0) return false;
        const auto& identity = gsr_launcher::kGames[game];
        const int revision = rom_language(root, game);
        const auto files = managed_paths(root, identity.stem, revision);
        const std::string target = std::string(identity.stem) + " (" + identity.rom_names[revision] + ")";
        const std::string question = "This is a save from " + std::string(identity.stem) + ".\n\n"
            "Use it as your " + target + " save? Your current save is kept in Saves/Imported.\n\n"
            "Loading a savestate made before this brings back the old save.";
        const SDL_MessageBoxButtonData buttons[] = {
            {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Use this save"},
            {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel"}};
        const SDL_MessageBoxData dialog{SDL_MESSAGEBOX_INFORMATION, window, "Import emulator save",
                                        question.c_str(), 2, buttons, nullptr};
        int answer = 0;
        if (SDL_ShowMessageBox(&dialog, &answer) != 0 || answer != 1) return false;
        emulator_save::install(image, files.save, files);
        done = "Your " + target + " save is now the emulator save.";
        return true;
    } catch (const std::exception& e) { error = "Cannot import the save: " + std::string(e.what()); return false; }
}

// `ask`: always open the file window (titled with `step`), ignoring the ROM
// used last time ("Skip the ROM check" builds ask for the ROM to play).
inline bool prepare_rom(SDL_Window* window, const fs::path& collection, int choice,
                        fs::path& rom, std::string& error, bool ask = false,
                        const char* step = "") {
    error.clear();
    rom.clear();
    if (choice < 0 || choice > 1) { error = "Unknown collection game."; return false; }
    if (collection.empty()) { error = "Cannot locate the collection launcher folder."; return false; }
    try {
        const auto root = fs::absolute(collection).lexically_normal();
        const auto cache = gsr_session::game_root(root, choice) / "local" / "launcher-rom.txt";
        const bool any_rom = gsr_session::load_launcher_audio_settings(
            gsr_session::game_root(root, choice)).any_rom;
        int language = rom_language(root,choice);
        fs::path selected;
        std::ifstream remembered;
        if (!ask) remembered.open(cache, std::ios::binary);
        if (remembered) {
            std::string text{std::istreambuf_iterator<char>(remembered), std::istreambuf_iterator<char>()};
            rom_detail::trim_newlines(text);
            std::string ignored;
            const fs::path candidate(reinterpret_cast<const char8_t*>(text.c_str()));
            if (!remembered.bad() && !text.empty() && rom_detail::validate(candidate, choice, any_rom, ignored, language)) selected = candidate;
        }
        if (remembered.is_open()) remembered.close();
        if (selected.empty() && !ask) {
            const auto managed = managed_paths(root, choice == 0 ? "The Broken Seal" : "The Lost Age");
            std::string ignored;
            if (rom_detail::validate(managed.rom, choice, any_rom, ignored, language)) selected = managed.rom;
        }
        const bool picked = selected.empty();
        if (picked && !rom_detail::pick(window, root / "ROMs", choice, language, selected, error, step)) return false;
        if (picked) rom_detail::pad_short_rom(root, choice, selected);
        if (!rom_detail::validate(selected, choice, any_rom, error, language, picked)) return false;
        // A picked ROM that is the language's other revision (Reloaded under
        // the English flag, or the clean ROM while Reloaded is chosen)
        // switches to it: its own ROM, saves and game build. The caller sees
        // the change in rom_language.
        if (picked) {
            const auto& game = gsr_launcher::kGames[choice];
            std::string sha1;
            const int found = gsr_launcher::sha1_file(selected, &sha1) ? gsr_launcher::rom_revision_of(game, sha1) : -1;
            if (found >= 0 && found != language &&
                gsr_launcher::rom_language_row(game, found) == gsr_launcher::rom_language_row(game, language)) {
                if (!set_rom_language(root, found, choice)) { error = "Could not save the ROM choice."; return false; }
                language = found;
            }
        }
        ManagedFiles managed;
        if (!import_selected_rom(root, selected, choice == 0 ? "The Broken Seal" : "The Lost Age", managed, error)) return false;
        if (!rom_detail::validate(managed.rom, choice, any_rom, error, language)) return false;
        fs::create_directories(cache.parent_path());
        std::ofstream output(cache, std::ios::binary | std::ios::trunc);
        const auto text = storage_detail::text(managed.rom) + "\n";
        if (!output || !output.write(text.data(), static_cast<std::streamsize>(text.size()))) {
            error = "Cannot remember the selected ROM."; return false;
        }
        output.close();
        if (!output) { error = "Cannot remember the selected ROM."; return false; }
        rom = managed.rom;
        return true;
    } catch (const std::exception& e) { error = "Cannot prepare the ROM: " + std::string(e.what()); return false; }
}

// With "Skip the ROM check" on, the clean ROM to build with, picked now, first
// thing after Build (collection_build.h); otherwise `rom` stays empty.
inline bool clean_build_rom(SDL_Window* window, const fs::path& collection, int choice,
                            fs::path& rom, std::string& error) {
    error.clear();
    rom.clear();
    if (!skip_rom_check_on(collection, choice)) return true;
    const auto message = clean_build_rom_message(collection, choice, true);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, gsr_launcher::kGames[choice].title,
                             message.c_str(), window);
    fs::path clean;
    if (!rom_detail::pick(window, collection / "ROMs", choice, rom_language(collection, choice),
                          clean, error, "Step 1 of 2 - your CLEAN, unpatched ")) return false;
    if (!check_clean_build_rom(clean, collection, choice, error)) return false;
    rom = clean;
    return true;
}
} // namespace gsr_collection
