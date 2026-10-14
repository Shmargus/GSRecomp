#pragma once

#include "collection_storage.h"
#include "collection_session.h"
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

namespace gsr_launcher {
bool sha1_file(const std::filesystem::path&, std::string*);
}

namespace gsr_collection {
namespace rom_detail {
inline void trim_newlines(std::string& value) {
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) value.pop_back();
}
inline bool validate(const fs::path& path, int choice, std::string& error) {
    const char* hashes[] = {"5c4695205413df7db52b9a184815a07783999971", "b500663220cb9bf56b9f8e8c0c544f1d6fa3a824"};
    std::error_code ec;
    std::string digest;
    if (!fs::is_regular_file(path, ec) || !gsr_launcher::sha1_file(path, &digest)) {
        error = "The ROM could not be read.";
        return false;
    }
    if (digest != hashes[choice]) {
        error = std::string("Select the required ") + (choice == 0 ? "The Broken Seal" : "The Lost Age") +
            " USA/Europe ROM.\n\nExpected SHA-1:\n" + hashes[choice] + "\n\nFound SHA-1:\n" + digest;
        return false;
    }
    return true;
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
inline bool pick(SDL_Window* window, const fs::path& folder, int choice, fs::path& selected, std::string& error) {
    const std::string title = std::string("Select ") + (choice == 0 ? "The Broken Seal" : "The Lost Age") + " ROM";
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
    dialog.lpstrFilter = L"Golden Sun ROM (*.gba)\0*.gba\0All files\0*.*\0\0";
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
#else
    (void)window;
    std::string output;
    const auto zenity = program("zenity");
    bool accepted;
    if (!zenity.empty()) accepted = run_picker(zenity, {"--file-selection", "--title=" + title,
        "--file-filter=Golden Sun ROM (*.gba) | *.gba *.GBA", "--file-filter=All files | *",
        "--filename=" + folder.string() + "/"}, output, error);
    else {
        const auto kdialog = program("kdialog");
        if (kdialog.empty()) { error = "No ROM file chooser was found. Install zenity or kdialog and try again."; return false; }
        accepted = run_picker(kdialog, {"--title", title, "--getopenfilename", folder.string(),
            "*.gba *.GBA|Golden Sun ROM (*.gba)"}, output, error);
    }
    if (!accepted) return false;
    selected = fs::path(reinterpret_cast<const char8_t*>(output.c_str()));
    return true;
#endif
}
} // namespace rom_detail

inline bool prepare_rom(SDL_Window* window, const fs::path& collection, int choice,
                        fs::path& rom, std::string& error) {
    error.clear();
    rom.clear();
    if (choice < 0 || choice > 1) { error = "Unknown collection game."; return false; }
    if (collection.empty()) { error = "Cannot locate the collection launcher folder."; return false; }
    try {
        const auto root = fs::absolute(collection).lexically_normal();
        const auto cache = gsr_session::game_root(root, choice) / "local" / "launcher-rom.txt";
        fs::path selected;
        std::ifstream remembered(cache, std::ios::binary);
        if (remembered) {
            std::string text{std::istreambuf_iterator<char>(remembered), std::istreambuf_iterator<char>()};
            rom_detail::trim_newlines(text);
            std::string ignored;
            const fs::path candidate(reinterpret_cast<const char8_t*>(text.c_str()));
            if (!remembered.bad() && !text.empty() && rom_detail::validate(candidate, choice, ignored)) selected = candidate;
        }
        if (remembered.is_open()) remembered.close();
        if (selected.empty()) {
            const auto managed = managed_paths(root, choice == 0 ? "The Broken Seal" : "The Lost Age");
            std::string ignored;
            if (rom_detail::validate(managed.rom, choice, ignored)) selected = managed.rom;
        }
        if (selected.empty() && !rom_detail::pick(window, root / "ROMs", choice, selected, error)) return false;
        if (!rom_detail::validate(selected, choice, error)) return false;
        ManagedFiles managed;
        if (!import_selected_rom(root, selected, choice == 0 ? "The Broken Seal" : "The Lost Age", managed, error)) return false;
        if (!rom_detail::validate(managed.rom, choice, error)) return false;
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
} // namespace gsr_collection
