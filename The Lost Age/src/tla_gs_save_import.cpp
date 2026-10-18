// Direct GS1 save transfer through TLA's existing link-data importer.
#include "tla.h"
#include "env_flag.h"
#include "gs_save.h"
#include "gba_bus.h"
#include "runtime.h"
#include "runtime_arm.h"
#include "runtime_bus_bridge.h"
#include "seamless.h"
#include "host_config_ui.h"
#include "tla_save_update.h"
#ifdef GBARECOMP_HAVE_IMGUI
#include "imgui.h"
#endif
#include <SDL.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
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
bool g_enabled = true, g_headless = false;
struct PickerResult {
    fs::path path;
    std::string error;
    std::atomic<bool> done{false};
};
struct SaveSelection {
    std::shared_ptr<PickerResult> picker;
    std::vector<gs_save::ClearSlot> slots;
    unsigned row = 0;
};
enum class PickerStatus { Waiting, Cancelled, Ready, Error };
enum class ImportStage { Idle, File, Slots, Error, Finish };
struct ImportMenu {
    ImportStage stage = ImportStage::Idle;
    SaveSelection source;
    std::string error;
    std::uint16_t held = 0x3FF;
    bool accepted = false;
    unsigned long long ready_frame = 0;
} g_import_menu;
enum class UpdateStage { Idle, File, Slots, Destination, Confirm, Updated, Error };
struct UpdateMenu {
    UpdateStage stage = UpdateStage::Idle;
    SaveSelection source;
    std::string error;
    std::vector<std::uint8_t> flash;
    std::vector<tla_save_update::Slot> destinations;
    unsigned destination = 0;
    unsigned viewport = 0;
} g_update_menu;

bool update_active() { return g_update_menu.stage != UpdateStage::Idle; }
int (*g_next_literal)(std::uint32_t, std::uint32_t, std::uint32_t*) = nullptr;
int (*g_next_alu)(std::uint32_t, std::uint32_t, std::uint32_t*) = nullptr;

// Seamless mode (engine seamless.h): the launcher started this run as The
// Broken Seal ended, with its save file and slot. No key is pressed for the
// player; each step the game would wait for is taken directly:
// - the intro: the game starts in room 0 entrance 1 (0x080C99B4); right after
//   (0x080C99D6 calls 0x08038348) the start room becomes the title, room 1
//   entrance 1, as Start in the intro asks for (0x080D2AA4(1, 1));
// - the title routine 0x0804D5AC takes its own no-saves path and returns New
//   Game (cmp r0,#0 at 0x0804D5C0 compared with r0 itself);
// - Felix is named as usual;
// - "Load a Golden Sun save file?": its message (0x08038040 with r0 = 0x24)
//   and window close (0x08038140) do not run, and Yes/No (0x08038378) returns
//   0, Yes (engine guest_return_now);
// - the import takes that file and slot without asking.
// The picture stays black (engine set_screen_veil) except while Felix is
// named, and again from the end of the import, so only the naming shows.
// FACTS.md 2026-10-15, "Seamless mode" and "Seamless mode: straight to the
// naming".
enum class Seamless { Off, Intro, Title, Naming, Question, Close, Done };
Seamless g_seamless = Seamless::Off;
// The Broken Seal's save, from the launcher; empty when it gave none.
fs::path g_broken_seal_save;
unsigned g_seamless_slot = 0;
constexpr std::uint32_t kStartRoomSetPc = 0x08038348u;  // first call after it
constexpr std::uint32_t kRoomAddr = 0x02000420u;        // u16, entrance +2
constexpr std::uint32_t kEntranceAddr = 0x02000422u;
constexpr std::uint16_t kIntroRoom = 0u, kTitleRoom = 1u, kStartEntrance = 1u;
constexpr std::uint32_t kTitleNoSavesTestPc = 0x0804D5C0u;
constexpr std::uint32_t kNamingEntryPc = 0x080382F8u;  // title code's naming call
constexpr std::uint32_t kMessagePc = 0x08038040u;      // r0 = text ID
constexpr std::uint32_t kQuestionText = 0x24u;
constexpr std::uint32_t kYesNoPc = 0x08038378u;        // 0 = Yes
constexpr std::uint32_t kMessageClosePc = 0x08038140u;

// While the picture is covered: the frame it was covered at. Each covered
// stretch (start to the naming; the question to the end of the import) ran
// within 555 frames: that was the shortest whole Seamless session, naming
// included (logs/session_20261015_181714, 555 frames). Past 600 frames
// (10 s) a step was missed: the picture comes back and the player goes on
// by hand instead of facing a black screen.
constexpr unsigned long long kNotCovered = ~0ull;
constexpr unsigned long long kCoverLimitFrames = 600;
unsigned long long g_covered_at = kNotCovered;

void cover(bool immediately = false) {
    gbarecomp::set_screen_veil(true, immediately);
    g_covered_at = runtime_current_frame();
}
void uncover() {
    gbarecomp::set_screen_veil(false);
    g_covered_at = kNotCovered;
}

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
    // Then The Broken Seal's save in this collection (the launcher's).
    if (!g_broken_seal_save.empty() && !read_slots(g_broken_seal_save).empty())
        return g_broken_seal_save;
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

std::shared_ptr<PickerResult> browse_for_save(SDL_Window* owner_window = nullptr) {
    auto result = std::make_shared<PickerResult>();
    if (g_headless) {
        if (const char* file = std::getenv("TLA_GS_SAVE_TEST_FILE")) result->path = utf8_path(file);
        result->done.store(true, std::memory_order_release);
        return result;
    }
    fs::path base;
    if (char* dir = SDL_GetBasePath()) { base = utf8_path(dir); SDL_free(dir); }
#ifdef _WIN32
    HWND owner = nullptr;
    SDL_SysWMinfo info{};
    SDL_VERSION(&info.version);
    if (SDL_Window* window = owner_window ? owner_window : SDL_GetKeyboardFocus();
        window && SDL_GetWindowWMInfo(window, &info)) owner = info.info.win.window;
#endif
    // Only the private result is shared. Guest memory, SDL and audio remain
    // on the game thread. Closing TLA does not wait for an open file dialog.
    std::thread([result, base
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
    return result;
}

PickerStatus poll_picker(SaveSelection& source, std::string& error) {
    if (!source.picker->done.load(std::memory_order_acquire)) return PickerStatus::Waiting;
    if (!source.picker->error.empty()) error = source.picker->error;
    else if (source.picker->path.empty()) return PickerStatus::Cancelled;
    else {
        source.slots = read_slots(source.picker->path);
        if (!source.slots.empty()) return PickerStatus::Ready;
        error = "No Golden Sun Clear Data in this file.";
    }
    return PickerStatus::Error;
}

void begin_import() {
    g_import_menu = {};
    g_import_menu.stage = ImportStage::File;
    g_import_menu.ready_frame = runtime_current_frame();
    if (g_seamless == Seamless::Close) g_seamless = Seamless::Done;
    // Only the new-game workflow may take Seamless's file automatically.
    if (g_seamless == Seamless::Done && !g_broken_seal_save.empty()) {
        auto result = std::make_shared<PickerResult>();
        result->path = g_broken_seal_save;
        result->done.store(true, std::memory_order_release);
        g_import_menu.source.picker = std::move(result);
    } else {
        g_import_menu.source.picker = browse_for_save();
        if (g_headless) {
            if (const char* wait = std::getenv("TLA_GS_SAVE_TEST_WAIT_FRAMES"))
                g_import_menu.ready_frame += std::strtoull(wait, nullptr, 10);
        }
    }
}

void fail(const std::string& message) {
    if (g_seamless != Seamless::Off) uncover();
    g_import_menu.error = message;
    g_import_menu.stage = ImportStage::Error;
    std::fprintf(stderr, "[gs-save-import] %s\n", message.c_str());
}
void menu_frame(gba::GbaBus& bus) {
    auto& menu = g_import_menu;
    auto& source = menu.source;
    const auto keys = bus.read16(0x03001154u);
    const auto down = std::uint16_t(keys & ~menu.held);
    menu.held = keys;
    if (menu.stage == ImportStage::File && runtime_current_frame() >= menu.ready_frame) {
        switch (poll_picker(source, menu.error)) {
        case PickerStatus::Waiting:
            return;
        case PickerStatus::Cancelled:
            menu.stage = ImportStage::Finish;
            return;
        case PickerStatus::Error:
            fail(menu.error);
            return;
        case PickerStatus::Ready:
            break;
        }
        if (g_seamless == Seamless::Done) {
            auto it = std::find_if(source.slots.begin(), source.slots.end(), [](const auto& s) {
                return s.slot == g_seamless_slot;
            });
            if (it == source.slots.end()) fail("That slot has no Golden Sun Clear Data.");
            else {
                source.row = unsigned(it - source.slots.begin());
                menu.accepted = true;
                menu.stage = ImportStage::Finish;
            }
        } else {
            menu.stage = ImportStage::Slots;
            if (!g_headless) {
                std::error_code ec;
                fs::create_directories("local", ec);
                std::ofstream cache("local/gs-save-path.txt");
                const auto path = fs::absolute(source.picker->path, ec).u8string();
                cache << reinterpret_cast<const char*>(path.c_str()) << '\n';
            } else if (const char* slot = std::getenv("TLA_GS_SAVE_TEST_SLOT")) {
                auto it = std::find_if(source.slots.begin(), source.slots.end(), [&](const auto& s) {
                    return s.slot == std::strtoul(slot, nullptr, 10);
                });
                if (it == source.slots.end()) fail("That slot has no Golden Sun Clear Data.");
                else source.row = unsigned(it - source.slots.begin());
            }
            std::fprintf(stderr, "[gs-save-import] %zu cleared slots offered at frame %llu.\n",
                         source.slots.size(), runtime_current_frame());
        }
        return; // Opening click must not also select a slot.
    }
    if (menu.stage == ImportStage::Slots) {
        const auto count = unsigned(source.slots.size());
        if (down & 0x40u) source.row = (source.row + count - 1) % count;
        if (down & 0x80u) source.row = (source.row + 1) % count;
        if (down & 1u) { menu.accepted = true; menu.stage = ImportStage::Finish; }
        else if (down & 2u) menu.stage = ImportStage::Finish;
    } else if (menu.stage == ImportStage::Error && (down & 3u)) menu.stage = ImportStage::Finish;
}

int import_literal(std::uint32_t pc, std::uint32_t original, std::uint32_t* value) {
    if (!update_active() && g_enabled && pc == 0x080381F8u && original == 0x0804D7FDu &&
        g_cpu.R[14] == 0x020084DBu && g_cpu.R[0] == 0) begin_import();
    if (g_import_menu.stage != ImportStage::Idle && pc == 0x0804D1DAu && original == 0x03001150u) {
        *value = 0x080000ACu; // Existing measured no-keys stand-in.
        return 1;
    }
    return g_next_literal ? g_next_literal(pc, original, value) : 0;
}
int import_alu(std::uint32_t pc, std::uint32_t original, std::uint32_t* value) {
    auto& menu = g_import_menu;
    if (pc == kTitleNoSavesTestPc && original == 0u && g_seamless == Seamless::Title) {
        *value = g_cpu.R[0];
        g_seamless = Seamless::Naming;
        std::fprintf(stderr, "[seamless] New Game\n");
        return 1;
    }
    if (menu.stage != ImportStage::Idle) {
        if (gba::GbaBus* bus = gbarecomp::active_bus()) {
            if (pc == 0x0804D1EAu) {
                menu_frame(*bus);
                g_cpu.R[2] = menu.stage == ImportStage::Finish ? 1 : 0;
            } else if (pc == 0x0804D94Au && bus->read32(g_cpu.R[13] + 24) == 0x020084DBu) {
                // After its frame loop and window cleanup, use the original
                // epilogue. Saved r8 is SP+4; saved caller PC is SP+24.
                if (menu.accepted) {
                    const auto& slot = menu.source.slots[menu.source.row];
                    std::memcpy(bus->ewram_ptr() + 0x13000, slot.transfer.data(), slot.transfer.size());
                    bus->write32(g_cpu.R[13] + 4, 0xFFFFFFFFu);
                    std::fprintf(stderr, "[gs-save-import] Imported Clear Data slot %u (level %u).\n", slot.slot, slot.level);
                }
                bus->write32(g_cpu.R[13] + 24, menu.accepted ? 0x02008525u : 0x020084B7u);
                menu.stage = ImportStage::Idle;
                if (g_seamless == Seamless::Done) uncover();
                menu.source.picker.reset();
            }
        }
    }
    return g_next_alu ? g_next_alu(pc, original, value) : 0;
}
// Manual updates are a host menu over a paused game. Only the selected
// battery-save record is replaced; the live guest stays untouched.
bool (*g_next_pause_poll)(bool*) = nullptr;
bool g_update_pause_owned = false, g_update_was_paused = false;

bool update_pause_poll(bool* paused) {
    const bool previous_changed = g_next_pause_poll && g_next_pause_poll(paused);
    if (update_active()) {
        if (!g_update_pause_owned) {
            g_update_was_paused = *paused;
            g_update_pause_owned = true;
        }
        *paused = true;
        return true;
    }
    if (g_update_pause_owned) {
        g_update_pause_owned = false;
        if (!previous_changed) *paused = g_update_was_paused;
        return true;
    }
    return previous_changed;
}

#ifdef GBARECOMP_HAVE_IMGUI
void (*g_next_extra_draw)() = nullptr;
bool (*g_next_extra_keyboard)() = nullptr;

void close_update() {
    g_update_menu = {};
    ImGui::CloseCurrentPopup();
}

void draw_update_action() {
    ImGui::BeginDisabled(g_import_menu.stage != ImportStage::Idle || update_active());
    if (ImGui::Button("Update from The Broken Seal...")) {
        g_update_menu = {};
        g_update_menu.stage = UpdateStage::File;
        const auto* viewport = ImGui::GetWindowViewport();
        g_update_menu.viewport = viewport->ID;
        // The SDL2 backend stores a window ID here, not an SDL_Window*.
        const auto window_id = static_cast<Uint32>(reinterpret_cast<std::uintptr_t>(viewport->PlatformHandle));
        g_update_menu.source.picker = browse_for_save(SDL_GetWindowFromID(window_id));
    }
    ImGui::EndDisabled();
    ImGui::TextUnformatted("Choose TBS Clear Data and a saved TLA slot to update.");
    ImGui::TextUnformatted("Available before Isaac's party joins. Your current game stays paused.");
}

bool update_wants_keyboard() {
    return update_active() || (g_next_extra_keyboard && g_next_extra_keyboard());
}

void read_destinations() {
    auto& menu = g_update_menu;
    auto* bus = gbarecomp::active_bus();
    menu.flash = bus ? bus->save().flash_bytes() : std::vector<std::uint8_t>{};
    menu.destinations = tla_save_update::slots(menu.flash);
    if (menu.destinations.empty()) {
        menu.error = "No saved TLA adventures were found. Save in the game first.";
        menu.stage = UpdateStage::Error;
    } else menu.stage = UpdateStage::Destination;
}

void commit_update() {
    auto& menu = g_update_menu;
    try {
        auto* bus = gbarecomp::active_bus();
        std::vector<std::uint8_t> updated;
        if (!bus || !g_update_pause_owned) {
            menu.error = "The game could not be paused. Nothing was changed.";
        } else if (bus->save().flash_bytes() != menu.flash) {
            menu.error = "The save changed. Please choose the destination again.";
        } else if (tla_save_update::prepare(
                       {bus->rom_ptr(), bus->rom_ptr() + bus->rom_size()},
                       {bus->ewram_ptr(), bus->ewram_ptr() + 0x40000},
                       {bus->iwram_ptr(), bus->iwram_ptr() + 0x8000},
                       menu.flash, menu.destination, menu.source.slots[menu.source.row], updated, menu.error) &&
                   gbarecomp::update_flash_save(menu.flash, updated, menu.error)) {
            menu.stage = UpdateStage::Updated;
            return;
        }
    } catch (const std::exception&) {
        menu.error = "The update could not finish.";
    }
    menu.stage = UpdateStage::Error;
}

void draw_update_dialog() {
    auto& menu = g_update_menu;
    if (g_next_extra_draw) g_next_extra_draw();
    if (!update_active()) return;
    constexpr const char* title = "Update from The Broken Seal";
    if (!ImGui::IsPopupOpen(title)) ImGui::OpenPopup(title);
    // F1 has its own OS window in windowed OpenGL play. The extra-draw
    // callback otherwise creates this modal in the game window behind it.
    const auto* viewport = ImGui::FindViewportByID(menu.viewport);
    if (!viewport) viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    if (menu.stage == UpdateStage::File) {
        switch (poll_picker(menu.source, menu.error)) {
        case PickerStatus::Waiting:
            break;
        case PickerStatus::Cancelled:
            close_update();
            ImGui::EndPopup();
            return;
        case PickerStatus::Error:
            menu.stage = UpdateStage::Error;
            break;
        case PickerStatus::Ready:
            menu.stage = UpdateStage::Slots;
            break;
        }
    }
    if (menu.stage == UpdateStage::File) {
        ImGui::TextUnformatted("Choose your TBS .sav file in the file browser.");
    } else if (menu.stage == UpdateStage::Slots) {
        ImGui::TextUnformatted("Which TBS Clear Data slot should be imported?");
        for (unsigned i = 0; i < menu.source.slots.size(); ++i) {
            const auto& s = menu.source.slots[i];
            const auto label = "TBS Slot " + std::to_string(s.slot) + ": " +
                               s.name + "  Lv." + std::to_string(s.level);
            if (ImGui::Selectable(label.c_str(), menu.source.row == i)) menu.source.row = i;
        }
        if (ImGui::Button("Next: choose TLA slot")) read_destinations();
    } else if (menu.stage == UpdateStage::Destination) {
        ImGui::TextUnformatted("Which saved TLA adventure should receive it?");
        for (const auto& s : menu.destinations) {
            const auto label = "TLA Slot " + std::to_string(s.slot) + ": " + s.name +
                "  Lv." + std::to_string(s.level) + (s.post_reunion ? " (Post-Reunion Data)" : "");
            ImGui::BeginDisabled(s.post_reunion);
            if (ImGui::Selectable(label.c_str())) {
                menu.destination = s.slot;
                menu.stage = UpdateStage::Confirm;
            }
            ImGui::EndDisabled();
        }
        ImGui::TextUnformatted("Post-Reunion Data cannot be updated.");
        if (ImGui::Button("Back")) menu.stage = UpdateStage::Slots;
    } else if (menu.stage == UpdateStage::Confirm) {
        ImGui::Text("Import TBS Slot %u into TLA Slot %u?", menu.source.slots[menu.source.row].slot, menu.destination);
        ImGui::TextUnformatted("This replaces the transferred TBS data in that saved adventure.");
        ImGui::TextUnformatted("A backup of the save file will be kept before updating.");
        ImGui::TextUnformatted("Your running game and unsaved progress will stay unchanged.");
        ImGui::TextUnformatted("Load the updated slot to use it. Saving over it from this session");
        ImGui::TextUnformatted("will replace this update with your running game's data.");
        if (ImGui::Button("Update saved slot")) commit_update();
        ImGui::SameLine();
        if (ImGui::Button("Back")) menu.stage = UpdateStage::Destination;
    } else if (menu.stage == UpdateStage::Updated) {
        ImGui::Text("TLA Slot %u was updated.", menu.destination);
        ImGui::TextUnformatted("Load that slot to use the updated data.");
        ImGui::TextUnformatted("Your current game and unsaved progress have not changed.");
    } else if (menu.stage == UpdateStage::Error) {
        ImGui::TextUnformatted(menu.error.c_str());
    }
    if (menu.stage != UpdateStage::File && ImGui::Button(
            menu.stage == UpdateStage::Updated || menu.stage == UpdateStage::Error ? "Close" : "Cancel")) close_update();
    ImGui::EndPopup();
}
#endif
} // namespace

void tla_install_gs_save_import(gbarecomp::RunOptions& options, bool headless) {
    g_enabled = gbarecomp::env_flag("TLA_GS_SAVE_IMPORT", true);
    g_headless = headless;
    namespace sm = gbarecomp::seamless;
    const char* file = std::getenv(sm::kSaveEnv);
    const char* slot = std::getenv(sm::kSlotEnv);
    if (file && *file) g_broken_seal_save = utf8_path(file);
    if (file && *file && slot && *slot) {
        g_seamless_slot = unsigned(std::strtoul(slot, nullptr, 10));
        g_seamless = Seamless::Intro;
        g_enabled = true;  // Seamless mode is an import
        cover(true);
        std::fprintf(stderr, "[seamless] importing slot %u\n", g_seamless_slot);
    }
    g_next_literal = options.thumb_literal_override;
    options.thumb_literal_override = import_literal;
    g_next_alu = options.thumb_alu_immediate_override;
    options.thumb_alu_immediate_override = import_alu;
#ifdef GBARECOMP_HAVE_IMGUI
    if (!headless) {
        g_next_pause_poll = options.pause_request_poll;
        options.pause_request_poll = update_pause_poll;
        g_next_extra_draw = gbarecomp::g_config_ui_extra_draw;
        g_next_extra_keyboard = gbarecomp::g_config_ui_extra_wants_keyboard;
        gbarecomp::g_config_ui_extra_draw = draw_update_dialog;
        gbarecomp::g_config_ui_extra_wants_keyboard = update_wants_keyboard;
        gbarecomp::config_ui_set_game_page(draw_update_action);
    }
#endif
}
bool tla_gs_save_import_text(std::uint32_t id, std::string& out) {
    if (!g_enabled || id != kQuestionText) return false;
    out = "Load a Golden Sun save file?";
    return true;
}
void tla_gs_save_import_on_entry(std::uint32_t pc) {
    if (g_covered_at != kNotCovered &&
        runtime_current_frame() - g_covered_at > kCoverLimitFrames) {
        std::fprintf(stderr, "[seamless] a step did not come within %llu frames; "
                     "showing the game, the player goes on by hand\n", kCoverLimitFrames);
        uncover();
        g_seamless = Seamless::Off;
        return;
    }
    switch (g_seamless) {
    case Seamless::Intro:
        if (pc == kStartRoomSetPc && bus_read_u16(kRoomAddr) == kIntroRoom &&
            bus_read_u16(kEntranceAddr) == kStartEntrance) {
            bus_write_u16(kRoomAddr, kTitleRoom);
            g_seamless = Seamless::Title;
            std::fprintf(stderr, "[seamless] intro skipped\n");
        }
        break;
    case Seamless::Title:  // no saves: the title routine never runs
    case Seamless::Naming:
        if (pc == kNamingEntryPc) {
            g_seamless = Seamless::Naming;
            uncover();
        } else if (pc == kMessagePc && g_cpu.R[0] == kQuestionText &&
                   g_seamless == Seamless::Naming) {
            cover();
            gbarecomp::guest_return_now(pc, 0);
            g_seamless = Seamless::Question;
        }
        break;
    case Seamless::Question:
        if (pc == kYesNoPc) {
            gbarecomp::guest_return_now(pc, 0);
            g_seamless = Seamless::Close;
            std::fprintf(stderr, "[seamless] Golden Sun save: Yes\n");
        }
        break;
    case Seamless::Close:
        if (pc == kMessageClosePc) gbarecomp::guest_return_now(pc, 0);
        break;
    default:
        break;
    }
}
bool tla_gs_save_import_visible() { return g_import_menu.stage != ImportStage::Idle; }

bool tla_gs_save_import_paint(std::uint8_t* rgb, std::uint32_t width, std::uint32_t height) {
    const auto& menu = g_import_menu;
    if (menu.stage == ImportStage::Idle) return false;
    // Cover the original method chooser on the native 30x20 tile canvas.
    tla_menu_paint_window(rgb, width, height, 0, 0, 30, 20);
    auto text = [&](const std::string& label, int y) {
        tla_menu_paint_text(rgb, width, height, label,
                            (240 - int(tla_menu_text_width(label))) / 2, y);
    };
    text("Golden Sun Clear Data", 16);
    if (menu.stage == ImportStage::Slots || (menu.stage == ImportStage::Finish && menu.accepted)) {
        for (unsigned i = 0; i < menu.source.slots.size(); ++i) {
            const auto& s = menu.source.slots[i];
            const std::string prefix = std::string(i == menu.source.row ? "> " : "  ") +
                "Slot " + std::to_string(s.slot) + ": ";
            const std::string suffix = "  Lv." + std::to_string(s.level);
            std::string name = s.name;
            while (!name.empty() && tla_menu_text_width(prefix + name + suffix) > 208)
                name.pop_back();
            const std::string label = prefix + name + suffix;
            tla_menu_paint_text(rgb, width, height, label, 16, 48 + int(i) * 24);
        }
        text("A: Select   B: Cancel", 136);
    } else if (menu.stage == ImportStage::Error) {
        text(menu.error, 64);
        text("A / B: Back", 136);
    } else {
        text("Choose a Golden Sun save file", 64);
        text("in the file browser.", 80);
    }
    return true;
}
