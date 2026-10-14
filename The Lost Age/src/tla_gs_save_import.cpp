// Direct GS1 save transfer through TLA's existing link-data importer.
#include "tla.h"
#include "gs_save.h"
#include "gba_bus.h"
#include "runtime.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"
#include <SDL.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <SDL_syswm.h>
#endif

namespace {
namespace fs = std::filesystem;
enum class Stage { Idle, File, Slots, Error, Finish };
Stage g_stage = Stage::Idle;
bool g_enabled = true, g_headless = false, g_import = false;
unsigned g_row = 0;
std::uint16_t g_held = 0x3FF;
std::vector<gs_save::ClearSlot> g_slots;
std::string g_error;
struct PickerResult {
    fs::path path;
    std::string error;
    std::atomic<bool> done{false};
};
std::shared_ptr<PickerResult> g_picker;
unsigned long long g_test_ready = 0;
int (*g_next_literal)(std::uint32_t, std::uint32_t, std::uint32_t*) = nullptr;
int (*g_next_alu)(std::uint32_t, std::uint32_t, std::uint32_t*) = nullptr;

fs::path utf8_path(const char* value) {
    return fs::path(reinterpret_cast<const char8_t*>(value));
}
std::vector<gs_save::ClearSlot> read_slots(const fs::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input || input.tellg() != std::streamoff(gs_save::kFileBytes)) return {};
    std::vector<std::uint8_t> bytes(gs_save::kFileBytes);
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()))) return {};
    return gs_save::cleared_slots(bytes);
}

fs::path detect_save(const fs::path& base) {
    std::vector<fs::path> folders;
    std::ifstream cache("local/gs-save-path.txt");
    std::string previous;
    if (std::getline(cache, previous) && !previous.empty()) {
        auto path = utf8_path(previous.c_str());
        if (!read_slots(path).empty()) return path;
        folders.push_back(path.parent_path());
    }
#ifdef _WIN32
    wchar_t documents[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, documents)))
        folders.emplace_back(documents);
#else
    if (const char* home = std::getenv("HOME")) folders.push_back(fs::path(home) / "Documents");
#endif
    std::error_code ec;
    folders.push_back(fs::current_path(ec));
    folders.push_back(base);
    for (const auto& folder : folders) {
        std::vector<fs::path> candidates;
        for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            std::string extension = it->path().extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(),
                           [](unsigned char c) { return char(std::tolower(c)); });
            if (extension == ".sav") candidates.push_back(it->path());
        }
        // Stable choice when more than one GS1 file exists; validate contents,
        // because TLA saves share the CAMELOT signature.
        std::sort(candidates.begin(), candidates.end());
        for (const auto& path : candidates)
            if (!read_slots(path).empty()) return fs::absolute(path, ec);
        ec.clear();
    }
    return {};
}

#ifndef _WIN32
std::string shell_quote(const std::string& value) {
    std::string out = "'";
    for (char c : value) out += c == '\'' ? "'\\''" : std::string(1, c);
    return out + "'";
}
#endif

void begin_picker() {
    g_stage = Stage::File;
    g_slots.clear();
    g_row = 0;
    g_held = 0x3FF;
    g_import = false;
    g_picker = std::make_shared<PickerResult>();
    g_test_ready = runtime_current_frame();
    if (g_headless) {
        if (const char* file = std::getenv("TLA_GS_SAVE_TEST_FILE")) g_picker->path = utf8_path(file);
        if (const char* wait = std::getenv("TLA_GS_SAVE_TEST_WAIT_FRAMES"))
            g_test_ready += std::strtoull(wait, nullptr, 10);
        g_picker->done.store(true, std::memory_order_release);
        return;
    }
    fs::path base;
    if (char* dir = SDL_GetBasePath()) { base = utf8_path(dir); SDL_free(dir); }
#ifdef _WIN32
    HWND owner = nullptr;
    SDL_SysWMinfo info{};
    SDL_VERSION(&info.version);
    if (SDL_Window* window = SDL_GetKeyboardFocus();
        window && SDL_GetWindowWMInfo(window, &info)) owner = info.info.win.window;
#endif
    // Only the private result is shared. Guest memory, SDL and audio remain
    // on the game thread. Closing TLA does not wait for an open file dialog.
    std::thread([result = g_picker, base
#ifdef _WIN32
                 , owner
#endif
    ] {
        try {
            const fs::path detected = detect_save(base);
            if (!detected.empty()) {
                const auto text = detected.u8string();
                std::fprintf(stderr, "[gs-save-import] Preselected %s\n", reinterpret_cast<const char*>(text.c_str()));
            }
#ifdef _WIN32
            wchar_t path[32768]{};
            const std::wstring initial = detected.wstring();
            std::copy_n(initial.c_str(), std::min(initial.size(), std::size(path) - 1), path);
            const std::wstring folder = detected.parent_path().wstring();
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = owner;
            ofn.lpstrFilter = L"Golden Sun save (*.sav)\0*.sav\0All files\0*.*\0";
            ofn.lpstrFile = path;
            ofn.nMaxFile = static_cast<DWORD>(std::size(path));
            ofn.lpstrInitialDir = folder.empty() ? nullptr : folder.c_str();
            ofn.lpstrTitle = L"Select your Golden Sun save file";
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameW(&ofn)) result->path = fs::path(path);
            else if (CommDlgExtendedError()) result->error = "Could not open the file browser.";
#else
            std::string command;
            if (std::system("command -v zenity >/dev/null 2>&1") == 0)
                command = "zenity --file-selection --title='Select your Golden Sun save file' --file-filter='Golden Sun save | *.sav *.SAV' --filename=" + shell_quote(detected.string()) + " 2>/dev/null";
            else if (std::system("command -v kdialog >/dev/null 2>&1") == 0)
                command = "kdialog --getopenfilename " + shell_quote(detected.empty() ? "." : detected.string()) + " '*.sav *.SAV|Golden Sun save' 2>/dev/null";
            else result->error = "Install zenity or kdialog to browse.";
            if (!command.empty()) {
                if (FILE* pipe = popen(command.c_str(), "r")) {
                    std::string path;
                    char buf[4096];
                    while (std::fgets(buf, sizeof(buf), pipe)) path += buf;
                    if (pclose(pipe) == 0) {
                        while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) path.pop_back();
                        result->path = fs::path(path);
                    }
                } else result->error = "Could not open the file browser.";
            }
#endif
        } catch (const std::exception&) {
            result->error = "Could not open the file browser.";
        }
        result->done.store(true, std::memory_order_release);
    }).detach();
}

void fail(const std::string& message) {
    g_error = message;
    g_stage = Stage::Error;
    std::fprintf(stderr, "[gs-save-import] %s\n", message.c_str());
}
void menu_frame(gba::GbaBus& bus) {
    const auto keys = bus.read16(0x03001154u);
    const auto down = std::uint16_t(keys & ~g_held);
    g_held = keys;
    if (g_stage == Stage::File && runtime_current_frame() >= g_test_ready &&
        g_picker->done.load(std::memory_order_acquire)) {
        if (!g_picker->error.empty()) fail(g_picker->error);
        else if (g_picker->path.empty()) g_stage = Stage::Finish;
        else {
            g_slots = read_slots(g_picker->path);
            if (g_slots.empty()) fail("No Golden Sun Clear Data in this file.");
            else {
                g_stage = Stage::Slots;
                if (!g_headless) {
                    std::error_code ec;
                    fs::create_directories("local", ec);
                    std::ofstream cache("local/gs-save-path.txt");
                    const auto path = fs::absolute(g_picker->path, ec).u8string();
                    cache << reinterpret_cast<const char*>(path.c_str()) << '\n';
                } else if (const char* slot = std::getenv("TLA_GS_SAVE_TEST_SLOT")) {
                    auto it = std::find_if(g_slots.begin(), g_slots.end(), [&](const auto& s) {
                        return s.slot == std::strtoul(slot, nullptr, 10);
                    });
                    if (it == g_slots.end()) fail("That slot has no Golden Sun Clear Data.");
                    else g_row = unsigned(it - g_slots.begin());
                }
                std::fprintf(stderr, "[gs-save-import] %zu cleared slots offered at frame %llu.\n",
                             g_slots.size(), runtime_current_frame());
            }
        }
        return; // Opening click must not also select a slot.
    }
    if (g_stage == Stage::Slots) {
        if (down & 0x40u) g_row = (g_row + unsigned(g_slots.size()) - 1) % unsigned(g_slots.size());
        if (down & 0x80u) g_row = (g_row + 1) % unsigned(g_slots.size());
        if (down & 1u) { g_import = true; g_stage = Stage::Finish; }
        else if (down & 2u) g_stage = Stage::Finish;
    } else if (g_stage == Stage::Error && (down & 3u)) g_stage = Stage::Finish;
}

int import_literal(std::uint32_t pc, std::uint32_t original, std::uint32_t* value) {
    if (g_enabled && pc == 0x080381F8u && original == 0x0804D7FDu &&
        g_cpu.R[14] == 0x020084DBu && g_cpu.R[0] == 0) begin_picker();
    if (g_stage != Stage::Idle && pc == 0x0804D1DAu && original == 0x03001150u) {
        *value = 0x080000ACu; // Existing measured no-keys stand-in.
        return 1;
    }
    return g_next_literal ? g_next_literal(pc, original, value) : 0;
}
int import_alu(std::uint32_t pc, std::uint32_t original, std::uint32_t* value) {
    if (g_stage != Stage::Idle) {
        if (gba::GbaBus* bus = gbarecomp::active_bus()) {
            if (pc == 0x0804D1EAu) {
                menu_frame(*bus);
                g_cpu.R[2] = g_stage == Stage::Finish ? 1 : 0;
            } else if (pc == 0x0804D94Au && bus->read32(g_cpu.R[13] + 24) == 0x020084DBu) {
                // After its frame loop and window cleanup, use the original
                // epilogue. Saved r8 is SP+4; saved caller PC is SP+24.
                if (g_import) {
                    const auto& slot = g_slots[g_row];
                    std::memcpy(bus->ewram_ptr() + 0x13000, slot.transfer.data(), slot.transfer.size());
                    bus->write32(g_cpu.R[13] + 4, 0xFFFFFFFFu);
                    std::fprintf(stderr, "[gs-save-import] Imported Clear Data slot %u (level %u).\n", slot.slot, slot.level);
                }
                bus->write32(g_cpu.R[13] + 24, g_import ? 0x02008525u : 0x020084B7u);
                g_stage = Stage::Idle;
                g_picker.reset();
            }
        }
    }
    return g_next_alu ? g_next_alu(pc, original, value) : 0;
}
} // namespace

void tla_install_gs_save_import(gbarecomp::RunOptions& options, bool headless) {
    const char* enabled = std::getenv("TLA_GS_SAVE_IMPORT");
    g_enabled = !enabled || std::strcmp(enabled, "0") != 0;
    g_headless = headless;
    g_next_literal = options.thumb_literal_override;
    options.thumb_literal_override = import_literal;
    g_next_alu = options.thumb_alu_immediate_override;
    options.thumb_alu_immediate_override = import_alu;
}
bool tla_gs_save_import_text(std::uint32_t id, std::string& out) {
    if (!g_enabled || id != 0x24u) return false;
    out = "Load a Golden Sun save file?";
    return true;
}
bool tla_gs_save_import_paint(std::uint8_t* rgb, std::uint32_t width, std::uint32_t height) {
    if (g_stage == Stage::Idle) return false;
    // Cover the original method chooser on the native 30x20 tile canvas.
    tla_menu_paint_window(rgb, width, height, 0, 0, 30, 20);
    auto text = [&](const std::string& label, int y) {
        tla_menu_paint_text(rgb, width, height, label,
                            (240 - int(tla_menu_text_width(label))) / 2, y);
    };
    text("Golden Sun Clear Data", 16);
    if (g_stage == Stage::Slots || (g_stage == Stage::Finish && g_import)) {
        for (unsigned i = 0; i < g_slots.size(); ++i) {
            const auto& s = g_slots[i];
            const std::string prefix = std::string(i == g_row ? "> " : "  ") +
                "Slot " + std::to_string(s.slot) + ": ";
            const std::string suffix = "  Lv." + std::to_string(s.level);
            std::string name = s.name;
            while (!name.empty() && tla_menu_text_width(prefix + name + suffix) > 208)
                name.pop_back();
            const std::string label = prefix + name + suffix;
            tla_menu_paint_text(rgb, width, height, label, 16, 48 + int(i) * 24);
        }
        text("A: Select   B: Cancel", 136);
    } else if (g_stage == Stage::Error) {
        text(g_error, 64);
        text("A / B: Back", 136);
    } else {
        text("Choose a Golden Sun save file", 64);
        text("in the file browser.", 80);
    }
    return true;
}
