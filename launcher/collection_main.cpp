// One collection launcher owns selection, preparation and both game sessions.
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include "launcher_png.h"
#include "collection_update.h"
#include "collection_storage.h"
#include "collection_settings.h"
#include "collection_session.h"
#include "collection_preferences.h"
#include "collection_view_settings.h"
#include "adventure_settings.h"
#include "music_mod_settings.h"
#include "launcher_common.h"
#include "launcher_test_policy.h"
#include "collection_build.h"
#include "collection_build_state.h"
#include "collection_rom.h"
#include "launcher_build_progress.h"
#include "../engine/src/gba/sha1.h"
#include "../engine/src/runtime/game_options.h"
#include "../engine/src/runtime/cartridge_art.h"
#include "../engine/src/runtime/handover.h"
#include "../engine/src/runtime/seamless.h"
#include "../engine/src/runtime/soft_reset.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <dwmapi.h>
#include <SDL_syswm.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace gsr_launcher {
// The build check hashes ~1 GB of generated code; Windows' hardware-assisted
// SHA-1 measured ~4x faster than reading through istreambuf into gba::sha1.
// Safe to call from several threads at once.
bool sha1_file(const fs::path& path, std::string* out) {
#ifdef _WIN32
    static const BCRYPT_ALG_HANDLE algorithm = [] {
        BCRYPT_ALG_HANDLE handle = nullptr;
        return BCryptOpenAlgorithmProvider(&handle, BCRYPT_SHA1_ALGORITHM, nullptr, 0) < 0 ? nullptr : handle;
    }();
    if (!algorithm) return false;
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    std::vector<UCHAR> buffer(1 << 20);
    for (DWORD read = 0; ok;) {
        if (!ReadFile(file, buffer.data(), DWORD(buffer.size()), &read, nullptr)) ok = false;
        else if (!read) break;
        else ok = BCryptHashData(hash, buffer.data(), read, 0) >= 0;
    }
    UCHAR digest[20];
    ok = ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    CloseHandle(file);
    if (!ok) return false;
    char hex[41];
    for (int i = 0; i < 20; ++i) std::snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    *out = hex;
    return true;
#else
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return false;
    std::vector<char> bytes(std::size_t(input.tellg()));
    input.seekg(0);
    if (!input.read(bytes.data(), std::streamsize(bytes.size()))) return false;
    *out = gba::sha1(bytes.data(), bytes.size()).hex();
    return true;
#endif
}
}

namespace {

#ifdef _WIN32
bool theme_title_bar(SDL_Window* window) {
    SDL_SysWMinfo info{};
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_WINDOWS) return false;
    const HWND handle = info.info.win.window;
    const BOOL dark = TRUE;
    const COLORREF caption = RGB(10, 23, 25), text = RGB(241, 194, 78), border = RGB(37, 53, 52);
    // Windows SDK DWMWINDOWATTRIBUTE values; older MinGW headers omit these.
    // Unsupported attributes simply retain the system's normal title bar.
    constexpr DWORD immersive_dark = 20, border_color = 34, caption_color = 35, text_color = 36;
    DwmSetWindowAttribute(handle, immersive_dark, &dark, sizeof(dark));
    const HRESULT caption_result = DwmSetWindowAttribute(handle, caption_color, &caption, sizeof(caption));
    const HRESULT text_result = DwmSetWindowAttribute(handle, text_color, &text, sizeof(text));
    DwmSetWindowAttribute(handle, border_color, &border, sizeof(border));
    return SUCCEEDED(caption_result) && SUCCEEDED(text_result);
}
#endif

fs::path executable_file() {
#ifdef _WIN32
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    return length && length < buffer.size() ? fs::path(std::wstring(buffer.data(), length)) : fs::path{};
#else
    std::error_code ec;
    const auto executable = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path{} : executable;
#endif
}

fs::path executable_directory() { return executable_file().parent_path(); }

struct Texture {
    SDL_Texture* texture = nullptr;
    SDL_Texture* glow = nullptr;
    int width = 0, height = 0;
    gbarecomp::cartridge_art::Glow glow_info;  // cartridges only (cartridge_art.h)
    std::vector<unsigned char> pixels;         // cartridges only: the cut-out RGBA
    ~Texture() {
        if (texture) SDL_DestroyTexture(texture);
        if (glow) SDL_DestroyTexture(glow);
    }
    void make_glow(SDL_Renderer* renderer, const std::vector<unsigned char>& rgba) {
        glow_info = gbarecomp::cartridge_art::make_glow(rgba, width, height);
        const int w = glow_info.width + 2 * glow_info.padding;
        const int h = glow_info.height + 2 * glow_info.padding;
        glow = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
        if (glow && SDL_UpdateTexture(glow, nullptr, glow_info.rgba.data(), w * 4) != 0) {
            SDL_DestroyTexture(glow); glow = nullptr;
        }
        if (glow) {
            SDL_SetTextureBlendMode(glow, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(glow, SDL_ScaleModeLinear);
        }
        glow_info.rgba.clear();
        pixels = rgba;
    }
    // The cut-out cartridge as a 32-bit BMP, for a game's black cover.
    bool save_bmp(const fs::path& path) const {
        if (pixels.empty()) return false;
        SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormatFrom(
            const_cast<unsigned char*>(pixels.data()), width, height, 32, width * 4,
            SDL_PIXELFORMAT_ABGR8888);
        const bool saved = surface && SDL_SaveBMP(surface, path.string().c_str()) == 0;
        if (surface) SDL_FreeSurface(surface);
        return saved;
    }
    bool load(SDL_Renderer* renderer, const fs::path& path, bool cartridge = false) {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) return false;
        const auto size = input.tellg();
        if (size <= 0 || size > 64 * 1024 * 1024) return false;
        std::vector<unsigned char> bytes(static_cast<std::size_t>(size)), rgba;
        input.seekg(0);
        if (!input.read(reinterpret_cast<char*>(bytes.data()), size) ||
            !gsr_launcher::decode_png(bytes.data(), bytes.size(), &rgba, &width, &height)) return false;
        // Cutout exports may include large transparent margins. Fit the actual
        // visible cartridge/logo, rather than the export canvas, into the layout.
        int left = width, top = height, right = -1, bottom = -1;
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
            if (rgba[(std::size_t(y) * width + x) * 4 + 3] > 0) {
                left = std::min(left, x); top = std::min(top, y);
                right = std::max(right, x); bottom = std::max(bottom, y);
            }
        }
        if (right < left || bottom < top) return false;
        const int cropped_width = right - left + 1, cropped_height = bottom - top + 1;
        if (cropped_width != width || cropped_height != height) {
            std::vector<unsigned char> cropped(std::size_t(cropped_width) * cropped_height * 4);
            for (int y = 0; y < cropped_height; ++y)
                std::memcpy(cropped.data() + std::size_t(y) * cropped_width * 4,
                            rgba.data() + (std::size_t(y + top) * width + left) * 4, std::size_t(cropped_width) * 4);
            rgba.swap(cropped); width = cropped_width; height = cropped_height;
        }
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, width, height);
        if (!texture) return false;
        if (SDL_UpdateTexture(texture, nullptr, rgba.data(), width * 4) != 0) {
            SDL_DestroyTexture(texture); texture = nullptr; return false;
        }
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(texture, cartridge ? SDL_ScaleModeNearest : SDL_ScaleModeLinear);
        if (cartridge) make_glow(renderer, rgba);
        return true;
    }
};

std::vector<fs::path> asset_directories(const fs::path& root) {
    std::vector<fs::path> result{root / "assets"};
#ifdef GSR_COLLECTION_ASSET_DIR
    result.emplace_back(GSR_COLLECTION_ASSET_DIR);
#endif
    result.emplace_back(fs::path(__FILE__).parent_path().parent_path().parent_path() / "assets");
    return result;
}

fs::path collection_root() {
    const auto directory = executable_directory();
    for (auto at = directory; !at.empty(); at = at.parent_path()) {
        if (fs::is_directory(at / "games") ||
            (fs::is_directory(at / "The Broken Seal") && fs::is_directory(at / "The Lost Age")))
            return at;
        if (at == at.parent_path()) break;
    }
    return directory;
}

bool runtime_saves_in_saves(const fs::path& root, int choice, std::string& error) {
    // The engine file: on Windows the plug-in beside the program
    // (launcher_game.h), on Linux the program itself.
    for (const auto& candidate : {gsr_session::engine(root, choice).parent_path() /
                                  gsr_launcher::kGames[choice].engine}) {
        if (!fs::is_regular_file(candidate)) continue;
        std::ifstream stream(candidate, std::ios::binary);
        const std::string marker = "GBARECOMP_STATE_BASE";
        char chunk[65536];
        std::string tail;
        while (stream) {
            stream.read(chunk, sizeof(chunk));
            tail.append(chunk, static_cast<std::size_t>(stream.gcount()));
            if (tail.find(marker) != std::string::npos) return true;
            if (tail.size() >= marker.size()) tail.erase(0, tail.size() - marker.size() + 1);
        }
        error = std::string(choice == 0 ? "The Broken Seal" : "The Lost Age") +
            " needs an updated game build before playing. Rebuild it so savestates are written only into Saves.";
        return false;
    }
    return true; // Missing games retain their normal preparation flow.
}

bool game_built(const fs::path& root, int choice, std::stop_token stop = {}) {
    std::error_code ec;
    // Packaged games check the release fingerprint; developer trees check the build record.
    if (fs::is_directory(root / "builder", ec) || root.parent_path().filename() == "games")
        return gsr_launcher::game_code_ready(root, gsr_launcher::kGames[choice],
                                             gsr_session::load_launcher_audio_settings(root).any_rom);
    return gsr_collection::developer_build_ready(root, choice, gsr_session::engine(root, choice), stop);
}

// A finished packaged build: record which release it was built for, so
// game_built accepts it.
void stamp_packaged_build(const fs::path& root, int choice) {
    std::error_code ec;
    if (!fs::is_directory(root / "builder", ec) && root.parent_path().filename() != "games") return;
    const auto& game = gsr_launcher::kGames[choice];
    std::ofstream(root / game.release_stamp, std::ios::binary | std::ios::trunc)
        << gsr_launcher::release_fingerprint(root, game);
}

// Hashing the generated game code can take seconds on a cold disk. Keep the
// cartridge window responsive while checking, and cancel promptly on close.
class BuildReadiness {
    std::array<std::atomic<int>, 2> results_{{-1, -1}};
    std::jthread worker_;
public:
    void refresh(const fs::path& root) {
        worker_.request_stop();
        if (worker_.joinable()) worker_.join();
        for (auto& result : results_) result.store(-1);
        worker_ = std::jthread([this, root](std::stop_token stop) {
            for (int game = 0; game < 2; ++game) {
                if (stop.stop_requested()) return;
                bool ready = false;
                try { ready = game_built(gsr_session::game_root(root, game), game, stop); }
                catch (const std::exception&) {}
                if (stop.stop_requested()) return;
                results_[game].store(ready ? 1 : 0);
            }
        });
    }
    int result(int game) const { return results_[game].load(); }
};

const char* build_label(const fs::path& game, int choice, bool built) {
    if (built) return "Ready to play";
    std::error_code ec;
    const auto engine = gsr_session::engine(game, choice);
    return fs::exists(engine, ec) ||
        fs::exists(game / (choice ? "TLAGame.build.txt" : "GoldenSunGame.build.txt"), ec) ||
        fs::exists(game / (choice ? "TLAGame.release.txt" : "GoldenSunGame.release.txt"), ec) ||
        fs::exists(engine.parent_path() / "collection-build.txt", ec) ||
        fs::exists(gsr_session::developer_engine(game, choice).parent_path() / "collection-build.txt", ec)
        ? "Needs rebuilding" : "Not built yet";
}

#ifdef _WIN32
// CreateProcess quoting must also preserve a directory's trailing backslash.
std::wstring quote_argument(const std::wstring& value) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') { ++slashes; continue; }
        result.append(c == L'\"' ? slashes * 2 + 1 : slashes, L'\\');
        result += c;
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    return result + L"\"";
}
#endif

class Child {
#ifdef _WIN32
    HANDLE process_ = nullptr;
#else
    pid_t pid_ = -1;
#endif
    // The game session started last, and how its process ended (-1: unknown).
    int started_game_ = -1;
    int exit_code_ = -1;
    bool finished_ = false;
public:
    ~Child() {
#ifdef _WIN32
        if (process_) CloseHandle(process_);
#else
        if (pid_ > 0) waitpid(pid_, nullptr, WNOHANG);
#endif
    }
    bool running() {
#ifdef _WIN32
        if (!process_) return false;
        if (WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) return true;
        DWORD code = 0;
        exit_code_ = GetExitCodeProcess(process_, &code) ? static_cast<int>(code) : -1;
        CloseHandle(process_); process_ = nullptr;
#else
        if (pid_ <= 0) return false;
        int status = 0;
        const auto result = waitpid(pid_, &status, WNOHANG);
        if (result == 0 || (result < 0 && errno == EINTR)) return true;
        exit_code_ = result == pid_ && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        pid_ = -1;
#endif
        finished_ = true;
        return false;
    }
    // Once per finished session: the game it was started for and its exit code.
    bool take_exit(int& game, int& code) {
        if (!finished_) return false;
        finished_ = false; game = started_game_; code = exit_code_;
        return true;
    }
    bool start(const fs::path& root, int choice, std::string& error,
               const gsr_collection::GameSettings* settings = nullptr, int seamless_slot = -1) {
        if (running()) return false;
        finished_ = false;
        const auto executable = executable_file();
        const auto game = gsr_session::game_root(root, choice);
        std::error_code ec;
        if (!fs::is_regular_file(executable, ec)) {
            error = "The collection launcher is missing. Please extract the complete release.";
            return false;
        }
        if (!fs::is_directory(game, ec)) {
            error = "This game's files are missing. Please extract the complete release.";
            return false;
        }
#ifdef _WIN32
        std::wstring arguments = quote_argument(executable.wstring()) +
            L" --session-game " + (choice ? std::wstring(L"LostAge") : std::wstring(L"BrokenSeal")) +
            L" --collection-play --collection-update-checked --collection-root " + quote_argument(root.wstring());
        if (settings && settings->direct3d) arguments += L" --direct3d";
        if (seamless_slot >= 0) arguments += L" --seamless-slot " + std::to_wstring(seamless_slot);
#ifdef GSR_COLLECTION_DEV
        if (settings) {
            const auto tests = gsr_collection::serialize_tests(*settings);
            arguments += L" --collection-tests " + std::wstring(tests.begin(), tests.end());
        }
#endif
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        if (!CreateProcessW(executable.c_str(), arguments.data(), nullptr, nullptr, FALSE, 0, nullptr,
                            game.c_str(), &startup, &info)) {
            error = "Could not start the game session (Windows error " + std::to_string(GetLastError()) + ").";
            return false;
        }
        CloseHandle(info.hThread); process_ = info.hProcess;
        started_game_ = choice;
#else
        int status_pipe[2];
        if (pipe(status_pipe) != 0) { error = std::strerror(errno); return false; }
        if (fcntl(status_pipe[1], F_SETFD, FD_CLOEXEC) == -1) {
            error = std::strerror(errno); close(status_pipe[0]); close(status_pipe[1]); return false;
        }
        const std::string program = executable.string(), directory = game.string();
        std::vector<std::string> arguments{program, "--session-game", choice ? "LostAge" : "BrokenSeal",
            "--collection-play", "--collection-update-checked", "--collection-root", root.string()};
#ifdef GSR_COLLECTION_DEV
        if (settings) { arguments.push_back("--collection-tests"); arguments.push_back(gsr_collection::serialize_tests(*settings)); }
#endif
        if (seamless_slot >= 0) { arguments.push_back("--seamless-slot"); arguments.push_back(std::to_string(seamless_slot)); }
        std::vector<char*> argv;
        for (auto& argument : arguments) argv.push_back(argument.data());
        argv.push_back(nullptr);
        pid_ = fork();
        if (pid_ == 0) {
            close(status_pipe[0]);
            if (chdir(directory.c_str()) == 0) execv(program.c_str(), argv.data());
            const int failure = errno;
            const auto ignored = write(status_pipe[1], &failure, sizeof(failure)); (void)ignored;
            _exit(127);
        }
        const int fork_error = errno;
        close(status_pipe[1]);
        if (pid_ < 0) { close(status_pipe[0]); error = std::strerror(fork_error); return false; }
        int failure = 0; ssize_t bytes;
        do { bytes = read(status_pipe[0], &failure, sizeof(failure)); } while (bytes < 0 && errno == EINTR);
        close(status_pipe[0]);
        if (bytes > 0) {
            while (waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {}
            pid_ = -1; error = "Could not start the game session: " + std::string(std::strerror(failure)); return false;
        }
        started_game_ = choice;
#endif
        error.clear();
        return true;
    }
};

struct Box {
    ImVec2 min, max;
    bool contains(float x, float y) const { return x >= min.x && x < max.x && y >= min.y && y < max.y; }
};

void centered_text(ImDrawList* draw, ImFont* font, float size, float center, float top,
                   ImU32 color, const char* text) {
    const auto measured = font->CalcTextSizeA(size, FLT_MAX, 0, text);
    draw->AddText(font, size, {center - measured.x / 2, top}, color, text);
}

void split_controls(ImDrawList* draw, ImFont* font, float size, float center, float top,
                    ImU32 color, const char* left, const char* right, float scale) {
    const float gap = 18 * scale;
    const float left_width = font->CalcTextSizeA(size, FLT_MAX, 0, left).x;
    draw->AddText(font, size, {center - gap - left_width, top}, color, left);
    draw->AddText(font, size, {center + gap, top}, color, right);
}

void golden_title(ImDrawList* draw, ImFont* font, float size, float center, float top, const char* text) {
    const auto measured = font->CalcTextSizeA(size, FLT_MAX, 0, text);
    const ImVec2 position{center - measured.x / 2, top};
    const float edge = size / 30.0f;
    draw->AddText(font, size, {position.x, top + 3 * edge}, IM_COL32(0, 0, 0, 220), text);
    for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x)
        if (x || y) draw->AddText(font, size, {position.x + x * edge, top + y * edge}, IM_COL32(68, 25, 16, 255), text);
    const ImU32 colors[] = {IM_COL32(255, 241, 170, 255), IM_COL32(241, 194, 78, 255), IM_COL32(198, 134, 41, 255)};
    for (int band = 0; band < 3; ++band) {
        const ImVec4 clip{0, top + measured.y * band / 3, FLT_MAX, top + measured.y * (band + 1) / 3};
        draw->AddText(font, size, position, colors[band], text, nullptr, 0, &clip);
    }
}

float grain(int x, int y) {
    Uint32 value = Uint32(x) * 374761393u + Uint32(y) * 668265263u;
    value = (value ^ (value >> 13)) * 1274126177u;
    return float((value ^ (value >> 16)) & 65535u) / 65535;
}

float cloud(float x, float y) {
    const int ix = int(std::floor(x)), iy = int(std::floor(y));
    const float dx = x - ix, dy = y - iy;
    const float sx = dx * dx * (3 - 2 * dx), sy = dy * dy * (3 - 2 * dy);
    const float a = grain(ix, iy) * (1 - sx) + grain(ix + 1, iy) * sx;
    const float b = grain(ix, iy + 1) * (1 - sx) + grain(ix + 1, iy + 1) * sx;
    return a * (1 - sy) + b * sy;
}

bool make_background(SDL_Renderer* renderer, Texture& background) {
    background.width = 410; background.height = 300;
    std::vector<unsigned char> pixels(std::size_t(background.width) * background.height * 4);
    for (int y = 0; y < background.height; ++y) for (int x = 0; x < background.width; ++x) {
        // Green (The Broken Seal) eases into blue (The Lost Age) across the middle half.
        const float across = std::clamp((float(x) / (background.width - 1) - 0.25f) * 2, 0.0f, 1.0f);
        const float blue = across * across * (3 - 2 * across);
        const float mist = 0.65f * cloud(x / 48.0f, y / 48.0f) + 0.35f * cloud(x / 12.0f, y / 12.0f);
        const float mottling = (mist - 0.5f) * 15 + (grain(x, y) - 0.5f) * 5;
        const float light = std::sin(float(y) / background.height * 3.14159265f) * 5;
        const float bases[] = {12.0f, 38.0f - 14.0f * blue, 31.0f + 15.0f * blue};
        const auto at = (std::size_t(y) * background.width + x) * 4;
        for (int c = 0; c < 3; ++c) pixels[at + c] = static_cast<unsigned char>(std::clamp(bases[c] + mottling + light, 0.0f, 255.0f));
        pixels[at + 3] = 255;
    }
    background.texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, background.width, background.height);
    if (!background.texture || SDL_UpdateTexture(background.texture, nullptr, pixels.data(), background.width * 4) != 0) return false;
    SDL_SetTextureScaleMode(background.texture, SDL_ScaleModeLinear);
    return true;
}

struct InsertionPose { float offset, opacity; };
float smootherstep(float t) {
    const double x = std::clamp(double(t), 0.0, 1.0);
    return float(x * x * x * (x * (x * 6 - 15) + 10));
}
float startup_opacity(float progress, bool fade_only) {
    if (progress < 0) return 1;
    // Let the cartridge lift before fading the whole window into the desktop.
    const float begin = fade_only ? 0.0f : 0.55f;
    return 1 - smootherstep((progress - begin) / (1 - begin));
}
InsertionPose insertion_pose(float progress, float drop, float scale) {
    const float lift = 44 * scale;
    // Ease into the lift and turn directly into the descent. Both segments
    // meet with zero velocity/acceleration, so there is no snap on return.
    if (progress < 0.36f) return {-lift * smootherstep(progress / 0.36f), 1};
    const float t = std::clamp((progress - 0.36f) / 0.64f, 0.0f, 1.0f);
    return {-lift + (drop + lift) * smootherstep(t), 1 - smootherstep((t - 0.18f) / 0.82f)};
}

ImFont* load_font(ImGuiIO& io, bool heading) {
#ifdef _WIN32
    const auto windows = fs::path(std::getenv("WINDIR") ? std::getenv("WINDIR") : "C:/Windows");
    const auto path = windows / "Fonts" / (heading ? "georgiab.ttf" : "segoeui.ttf");
#else
    const auto path = fs::path("/usr/share/fonts/truetype/dejavu") / (heading ? "DejaVuSerif-Bold.ttf" : "DejaVuSans.ttf");
#endif
    std::error_code ec;
    if (fs::is_regular_file(path, ec)) {
        if (auto* font = io.Fonts->AddFontFromFileTTF(path.string().c_str(), heading ? 32.0f : 20.0f)) return font;
    }
    ImFontConfig config; config.SizePixels = heading ? 32.0f : 20.0f;
    return io.Fonts->AddFontDefault(&config);
}

// Sets (or, given an empty value, removes) a variable the games started from
// here inherit.
void set_environment(const char* name, const fs::path& value) {
#ifdef _WIN32
    const std::wstring wide_name(name, name + std::strlen(name));
    SetEnvironmentVariableW(wide_name.c_str(), value.empty() ? nullptr : value.c_str());
#else
    if (value.empty()) unsetenv(name); else setenv(name, value.c_str(), 1);
#endif
}

bool capture_frame(SDL_Renderer* renderer, const fs::path& path) {
    int width = 0, height = 0;
    if (SDL_GetRendererOutputSize(renderer, &width, &height) != 0) return false;
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!surface) return false;
    const bool captured = SDL_RenderReadPixels(renderer, nullptr, surface->format->format, surface->pixels, surface->pitch) == 0;
    const bool saved = captured && SDL_SaveBMP(surface, path.string().c_str()) == 0;
    SDL_FreeSurface(surface);
    return saved;
}

int run(SDL_Window* window, SDL_Renderer* renderer, const fs::path& root, const fs::path& preview,
        int preview_choice = 0, float preview_insertion = -1, int preview_settings = -1) {
    if (preview.empty()) {
        std::error_code error;
        for (const char* folder : {"ROMs", "Saves"}) {
            fs::create_directories(root / folder, error);
            if (error) {
                const std::string message = std::string("Could not create ") + folder + ": " + error.message();
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Golden Sun Recompiled", message.c_str(), window);
                return 1;
            }
        }
        for (const char* game : {"The Broken Seal", "The Lost Age"}) {
            std::string message;
            if (!gsr_collection::sync_legacy_states(gsr_collection::managed_paths(root, game), message)) {
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Golden Sun Recompiled", message.c_str(), window);
                return 1;
            }
        }
    }
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImFont* body = load_font(io, false);
    ImFont* heading = load_font(io, true);
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);
    Texture logo, cartridges[2], background;
    if (!make_background(renderer, background)) {
        std::fprintf(stderr, "Cannot create launcher background: %s\n", SDL_GetError());
        ImGui_ImplSDLRenderer2_Shutdown(); ImGui_ImplSDL2_Shutdown(); ImGui::DestroyContext();
        return 1;
    }
    const auto directories = asset_directories(root);
    const char* filenames[] = {"launcher_logo.png", "cartridge_broken_seal.png", "cartridge_lost_age.png"};
    Texture* textures[] = {&logo, &cartridges[0], &cartridges[1]};
    for (int i = 0; i < 3; ++i) {
        for (const auto& directory : directories) if (textures[i]->load(renderer, directory / filenames[i], i > 0)) break;
        if (!textures[i]->texture) {
            const std::string message = std::string("Cannot load assets/") + filenames[i] + ". Please extract the complete release.";
            std::fprintf(stderr, "%s\n", message.c_str());
            if (preview.empty()) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Golden Sun Recompiled", message.c_str(), window);
            ImGui_ImplSDLRenderer2_Shutdown(); ImGui_ImplSDL2_Shutdown(); ImGui::DestroyContext();
            return 1;
        }
    }
    std::vector<SDL_GameController*> controllers;
    auto add_controller = [&](int index) {
        if (SDL_IsGameController(index)) if (auto* controller = SDL_GameControllerOpen(index)) controllers.push_back(controller);
    };
    for (int i = 0; i < SDL_NumJoysticks(); ++i) add_controller(i);
    Child child;
    gsr_collection::CollectionBuild builder;
    BuildReadiness readiness;
    std::array<bool, 2> built{};
    std::array<bool, 2> checking_builds{};
    auto refresh_builds = [&] {
        if (preview.empty()) {
            checking_builds = {true, true};
            readiness.refresh(root);
        } else for (int game = 0; game < 2; ++game)
            built[game] = game_built(gsr_session::game_root(root, game), game);
    };
    refresh_builds();
    int building_choice = -1;
    int build_choice = -1;
    bool play_after_build = false;
    // When both games need building, the player may build the other one
    // too: it is built straight after, then play_game starts.
    int then_build = -1, play_game = -1;
    bool checking_build_result = false;
    auto settings = preview.empty() ? gsr_collection::load_settings(root) : gsr_collection::CollectionSettings{};
    std::array<gsr_session::LauncherAudioSettings, 2> preferences{};
    std::array<gsr_collection::ViewSettings, 2> view_settings{};
    if (preview.empty()) for (int game = 0; game < 2; ++game)
        preferences[game] = gsr_session::load_launcher_audio_settings(gsr_session::game_root(root, game));
    // Music mod (music_mod_settings.h): found recordings and the saved choice,
    // read at start and whenever Settings opens. A game whose folder holds
    // recordings and that has no saved choice yet asks once.
    const auto options_file = [&](int game) {
        return gsr_session::engine(gsr_session::game_root(root, game), game).parent_path() / "game_options.ini";
    };
    std::array<bool, 2> music_found{}, music_on{}, music_ask{};
    const auto refresh_music = [&] {
        for (int game = 0; game < 2; ++game) {
            music_found[game] = gsr_collection::music_mod_available(root, game);
            const auto choice = gsr_collection::music_mod_choice(options_file(game));
            music_on[game] = music_found[game] && choice.value_or(false);
            music_ask[game] = music_found[game] && !choice;
        }
    };
    if (preview.empty()) refresh_music();
    bool request_music_prompt = music_ask[0] || music_ask[1];
    // Open (or about to open): the launcher behind it takes no input, so the
    // answering click or button never also starts or builds a game.
    bool music_prompt_open = request_music_prompt;
    bool settings_open = preview_settings >= 0;
    int settings_game = preview_settings >= 0 ? preview_settings : 0;
    bool request_settings = settings_open;
    Box settings_box{};
    gsr_collection::ReleaseCheck updates(preview.empty());
    // What's New: the newest release's notes, loaded when first opened.
    gsr_collection::ReleaseNotes notes;
    bool notes_open = false, request_notes = false;
    Box notes_box{};
    int selected = preview.empty() ? 0 : preview_choice, axis_direction = 0;
    int pending_choice = -1;
    int play_choice = -1;
    int insertion_choice = -1;
    double insertion_started = 0;
    float insertion_scale = 1;
    constexpr double insertion_duration = 1.08;
    constexpr double fade_duration = 0.36;
    bool fade_only = false;
    bool native_fade = preview.empty();
    float window_opacity = 1;
    bool alive = true, hidden = false;
    std::string error;
    Box cartridge_boxes[2]{};
    const Uint64 start = SDL_GetPerformanceCounter();
    int result = 0;
    int preview_frames = 0;
    while (alive) {
        if (preview.empty()) for (int game = 0; game < 2; ++game) {
            const int ready = readiness.result(game);
            if (ready >= 0) { built[game] = ready == 1; checking_builds[game] = false; }
        }
        const auto build = builder.snapshot();
        if (building_choice >= 0 && build.finished) {
            const int choice = building_choice;
            if (!checking_build_result) {
                if (build.succeeded) stamp_packaged_build(gsr_session::game_root(root, choice), choice);
                refresh_builds(); checking_build_result = true;
            }
            if (!build.succeeded || !checking_builds[choice]) {
                building_choice = -1; checking_build_result = false;
                if (!build.succeeded || !built[choice]) {
                    error = build.error.empty() ? "The build did not finish. Please try again." : build.error;
                    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Game build", error.c_str(), window);
                } else if (then_build >= 0) {
                    build_choice = then_build; then_build = -1;
                    continue;
                } else if (play_after_build) pending_choice = play_game;
                play_after_build = false; then_build = play_game = -1;
            }
        }
        if (hidden && !child.running()) {
            // Seamless mode: The Broken Seal ended after saving its Clear Data
            // (the session exits with the game's code); go straight on to The
            // Lost Age, keeping the window hidden. Failing that, show the window.
            int finished_game = -1, finished_code = -1;
            const bool finished = child.take_exit(finished_game, finished_code);
            if (finished && finished_code == gbarecomp::soft_reset::kExitCode &&
                finished_game >= 0 && finished_game < 2 && preview.empty()) {
                error.clear();
                if (child.start(root, finished_game, error, &settings[finished_game])) {
                    SDL_Delay(30); continue;
                }
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Soft Reset", error.c_str(), window);
            }
            const int seamless_slot = finished && finished_game == 0
                ? gbarecomp::seamless::slot_of_exit_code(finished_code) : -1;
            if (seamless_slot >= 0 && preview.empty()) {
                error.clear();
                // The Lost Age shows its cartridge, bouncing as in this
                // selector, on its black cover while it starts (cartridge_art.h).
                std::error_code temp_error;
                const fs::path cover = fs::temp_directory_path(temp_error) / "gsr_cover_cartridge.bmp";
                const bool cover_art = !temp_error && cartridges[1].save_bmp(cover);
                if (cover_art) {
                    set_environment(gbarecomp::cartridge_art::kCoverArtEnv, cover);
                    set_environment(gbarecomp::cartridge_art::kCoverGameEnv, "1");
                }
                const bool started = built[1] && runtime_saves_in_saves(gsr_session::game_root(root, 1), 1, error) &&
                    child.start(root, 1, error, &settings[1], seamless_slot);
                if (cover_art) {
                    set_environment(gbarecomp::cartridge_art::kCoverArtEnv, {});
                    set_environment(gbarecomp::cartridge_art::kCoverGameEnv, {});
                }
                if (started) {
                    selected = 1;
                    SDL_Delay(30); continue;
                }
                if (error.empty()) error = "The Lost Age is not built yet.";
#ifdef _WIN32
                gbarecomp::handover::quit();  // release the waiting Broken Seal program
#endif
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Seamless mode", error.c_str(), window);
            }
            SDL_SetWindowOpacity(window, 1); window_opacity = 1;
            hidden = false; SDL_ShowWindow(window); SDL_RaiseWindow(window); axis_direction = 0;
            SDL_FlushEvents(SDL_KEYDOWN, SDL_KEYUP);
            SDL_FlushEvents(SDL_CONTROLLERAXISMOTION, SDL_CONTROLLERBUTTONUP);
        }
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) alive = false;
            if (event.type == SDL_CONTROLLERDEVICEADDED) add_controller(event.cdevice.which);
            if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
                controllers.erase(std::remove_if(controllers.begin(), controllers.end(), [&](auto* controller) {
                    if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller)) != event.cdevice.which) return false;
                    SDL_GameControllerClose(controller); return true;
                }), controllers.end());
            }
            if (hidden) continue;
            if (build.running || building_choice >= 0) {
                if ((event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) ||
                    (event.type == SDL_CONTROLLERBUTTONDOWN && event.cbutton.button == SDL_CONTROLLER_BUTTON_B))
                    { builder.cancel(); play_after_build = false; then_build = -1; }
                continue;
            }
            if (music_prompt_open) continue;
            if (notes_open) {
                if ((event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) ||
                    (event.type == SDL_CONTROLLERBUTTONDOWN && event.cbutton.button == SDL_CONTROLLER_BUTTON_B)) {
                    notes_open = false;
                }
                continue;
            }
            if (insertion_choice < 0 && pending_choice < 0 && !settings_open &&
                event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT &&
                notes_box.contains(float(event.button.x), float(event.button.y))) {
                if (preview.empty()) notes.request();
                notes_open = request_notes = true; play_choice = -1; continue;
            }
            if (settings_open) {
                if ((event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) ||
                    (event.type == SDL_CONTROLLERBUTTONDOWN && event.cbutton.button == SDL_CONTROLLER_BUTTON_B)) {
                    settings_open = false;
                }
                continue;
            }
            if (insertion_choice < 0 && pending_choice < 0 &&
                ((event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_F2) ||
                 (event.type == SDL_CONTROLLERBUTTONDOWN && event.cbutton.button == SDL_CONTROLLER_BUTTON_START) ||
                 (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT &&
                  settings_box.contains(float(event.button.x), float(event.button.y))))) {
                settings_game = selected; settings_open = request_settings = true; play_choice = -1; continue;
            }
            if (insertion_choice >= 0 || pending_choice >= 0) {
                if ((event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) ||
                    (event.type == SDL_CONTROLLERBUTTONDOWN && event.cbutton.button == SDL_CONTROLLER_BUTTON_B)) alive = false;
                continue;
            }
            if (event.type == SDL_KEYDOWN && !event.key.repeat) {
                const auto key = event.key.keysym.sym;
                if (key == SDLK_LEFT || key == SDLK_a) selected = 0;
                if (key == SDLK_RIGHT || key == SDLK_d) selected = 1;
                if (play_choice < 0 && (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE)) play_choice = selected;
                if (key == SDLK_ESCAPE) alive = false;
            }
            if (event.type == SDL_CONTROLLERBUTTONDOWN) {
                if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT) selected = 0;
                if (event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) selected = 1;
                if (play_choice < 0 && event.cbutton.button == SDL_CONTROLLER_BUTTON_A) play_choice = selected;
                if (event.cbutton.button == SDL_CONTROLLER_BUTTON_B) alive = false;
            }
            if (event.type == SDL_CONTROLLERAXISMOTION && event.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) {
                const int direction = event.caxis.value > 16000 ? 1 : event.caxis.value < -16000 ? -1 : 0;
                if (direction && direction != axis_direction) selected = direction > 0 ? 1 : 0;
                axis_direction = direction;
            }
            if (event.type == SDL_MOUSEMOTION) {
                for (int i = 0; i < 2; ++i) if (cartridge_boxes[i].contains(float(event.motion.x), float(event.motion.y))) selected = i;
            }
            if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
                for (int i = 0; i < 2; ++i) if (cartridge_boxes[i].contains(float(event.button.x), float(event.button.y))) {
                    selected = i; if (play_choice < 0) play_choice = i;
                }
            }
        }
        if (!alive) break;
        if (play_choice >= 0 && preview.empty()) {
            pending_choice = play_choice;
            play_choice = -1;
            error.clear();
        }
        gsr_online::Release release;
        // Use one completion snapshot so a check finishing between these calls
        // cannot launch the child before presenting the available release.
        const bool updates_done = updates.done();
        if (updates_done && updates.take(release)) {
            const std::string message = "A new Golden Sun Recompiled release (" + release.tag +
                ") is available. Download the newest release?";
            const SDL_MessageBoxButtonData buttons[] = {
                {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Open download"},
                {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Not now"}
            };
            const SDL_MessageBoxData dialog{SDL_MESSAGEBOX_INFORMATION, window, "New release available",
                                           message.c_str(), 2, buttons, nullptr};
            int answer = 0;
            if (SDL_ShowMessageBox(&dialog, &answer) == 0 && answer == 1) {
                pending_choice = -1;
                if (SDL_OpenURL(gsr_online::kReleasesPage) != 0)
                    error = std::string("Could not open the release page: ") + SDL_GetError();
            }
        }
        // An unbuilt game also waits for the other game's check, to know
        // whether to offer building both.
        if (pending_choice >= 0 && updates_done && !checking_builds[pending_choice] &&
            (built[pending_choice] || !checking_builds[1 - pending_choice])) {
            const int choice = pending_choice; pending_choice = -1;
            const auto game = gsr_session::game_root(root, choice);
            if (!built[choice]) {
                build_choice = play_game = choice;
                play_after_build = true;
                then_build = -1;
                if (!built[1 - choice]) {
                    const char* names[] = {"The Broken Seal", "The Lost Age"};
                    const std::string message = std::string(names[choice]) + " needs to be built before it can start. " +
                        names[1 - choice] + " needs building too. Build both now?";
                    const std::string both = "Build both", one = std::string("Only ") + names[choice];
                    const SDL_MessageBoxButtonData buttons[] = {
                        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, both.c_str()},
                        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, one.c_str()}
                    };
                    const SDL_MessageBoxData dialog{SDL_MESSAGEBOX_INFORMATION, window, "Game build",
                                                   message.c_str(), 2, buttons, nullptr};
                    int answer = 0;
                    if (SDL_ShowMessageBox(&dialog, &answer) == 0 && answer == 1) then_build = 1 - choice;
                }
            } else {
                fs::path rom;
                if (gsr_collection::prepare_rom(window, root, choice, rom, error) &&
                    runtime_saves_in_saves(game, choice, error)) {
                    fade_only = false;
                    insertion_choice = selected = choice;
                    insertion_started = double(SDL_GetPerformanceCounter() - start) / SDL_GetPerformanceFrequency();
                    insertion_scale = gbarecomp::cartridge_art::pulse(insertion_started);
                }
                if (!error.empty()) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Game preparation", error.c_str(), window);
            }
        }
        if (build_choice >= 0 && !build.running && building_choice < 0 && preview.empty()) {
            const int choice = build_choice; build_choice = -1;
            fs::path rom;
            error.clear();
            if (gsr_collection::prepare_rom(window, root, choice, rom, error)) {
                if (builder.start(root, choice, rom, error)) {
                    building_choice = selected = choice;
                    continue;
                }
            }
            play_after_build = false; then_build = play_game = -1;
            if (!error.empty()) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Game build", error.c_str(), window);
        }
        const double elapsed = double(SDL_GetPerformanceCounter() - start) / SDL_GetPerformanceFrequency();
        const double transition_duration = fade_only ? fade_duration : insertion_duration;
        if (insertion_choice >= 0 && elapsed - insertion_started >= transition_duration) {
            const int choice = insertion_choice;
            insertion_choice = -1;
            if (child.start(root, choice, error, &settings[choice])) { hidden = true; SDL_HideWindow(window); }
        }
        if (hidden) { SDL_Delay(30); continue; }
        const float transition_progress = preview_insertion >= 0 ? preview_insertion :
            insertion_choice >= 0 ? std::clamp(float((elapsed - insertion_started) / transition_duration), 0.0f, 1.0f) : -1;
        const float opacity = startup_opacity(transition_progress, fade_only);
        if (native_fade && opacity != window_opacity) {
            native_fade = SDL_SetWindowOpacity(window, opacity) == 0;
            window_opacity = opacity;
        }
        if (pending_choice >= 0) selected = pending_choice;
        ImGui_ImplSDLRenderer2_NewFrame(); ImGui_ImplSDL2_NewFrame(); ImGui::NewFrame();
        const float width = io.DisplaySize.x, height = io.DisplaySize.y;
        const float scale = std::min(width / 820.0f, height / 600.0f);
        const float center = width / 2, footer = height - 104 * scale;
        auto* draw = ImGui::GetBackgroundDrawList();
        draw->AddImage(reinterpret_cast<ImTextureID>(background.texture), {0, 0}, {width, height});
        draw->AddRectFilledMultiColor({0, 0}, {width, 200 * scale}, IM_COL32(4, 9, 13, 150), IM_COL32(4, 9, 13, 150), 0, 0);
        const bool starting = preview_insertion >= 0 || insertion_choice >= 0;
        const bool checking = (pending_choice >= 0 && (!updates.done() || checking_builds[pending_choice] ||
                                (!built[pending_choice] && checking_builds[1 - pending_choice]))) ||
                              checking_build_result;
        const float logo_width = std::min({width * 0.68f, 400 * scale, 140 * scale * logo.width / logo.height});
        const float logo_height = logo_width * logo.height / logo.width;
        const float logo_top = 14 * scale;
        draw->AddImage(reinterpret_cast<ImTextureID>(logo.texture), {center - logo_width / 2, logo_top}, {center + logo_width / 2, logo_top + logo_height});
        golden_title(draw, heading, 24 * scale, center, 172 * scale, "Choose Your Adventure");
        const double seconds = preview.empty() ? elapsed : 1.0;
        const float pulse = gbarecomp::cartridge_art::pulse(seconds);
        const int inserting = preview_insertion >= 0 ? preview_choice : fade_only ? -1 : insertion_choice;
        const float insertion_progress = transition_progress;
        for (int i = 0; i < 2; ++i) {
            const float x = width * (i ? 0.75f : 0.25f);
            cartridge_boxes[i] = {{i ? center : 0, 208 * scale}, {i ? width : center, footer}};
            golden_title(draw, heading, 29 * scale, x, 223 * scale, i ? "The Lost Age" : "The Broken Seal");
            const Texture& image = cartridges[i];
            const float base_width = std::min(width * 0.36f, 184 * scale * image.width / image.height);
            const float starting_scale = preview_insertion >= 0 ? pulse : insertion_scale;
            const float sprite_scale = inserting == i ? 1 + (starting_scale - 1) * (1 - smootherstep(insertion_progress / 0.4f)) :
                selected == i ? pulse : 1;
            const float image_width = base_width * sprite_scale;
            const float image_height = image_width * image.height / image.width;
            // Leave room for the higher lift without crossing the game title.
            float image_y = 369 * scale;
            float opacity = inserting >= 0 && inserting != i ? 1 - 0.5f * smootherstep(insertion_progress / 0.25f) : 1.0f;
            if (inserting == i) {
                const auto pose = insertion_pose(insertion_progress, height - image_y + image_height / 2, scale);
                image_y += pose.offset; opacity = pose.opacity;
            }
            // Full width: with no divider, a glow cut at the middle would show a hard edge.
            draw->PushClipRect({0, 200 * scale}, {width, footer}, true);
            // The selected cartridge's glow, motes and bounce (cartridge_art.h,
            // shared with the game's black cover); otherwise the cartridge alone.
            namespace art = gbarecomp::cartridge_art;
            const art::Glow no_glow;
            for (const art::Piece& piece : art::selected(i, x, image_y, image_width, image_height,
                     selected == i && image.glow ? image.glow_info : no_glow, scale, seconds, opacity)) {
                const ImU32 color = IM_COL32(piece.r, piece.g, piece.b, piece.a);
                if (piece.kind == art::Piece::Dot)
                    draw->AddCircleFilled({piece.x0, piece.y0}, piece.x1, color, piece.segments);
                else
                    draw->AddImage(reinterpret_cast<ImTextureID>(piece.kind == art::Piece::Image ? image.texture : image.glow),
                                   {piece.x0, piece.y0}, {piece.x1, piece.y1}, {0, 0}, {1, 1}, color);
            }
            draw->PopClipRect();
            if (!starting) {
                const auto game = gsr_session::game_root(root, i);
                centered_text(draw, body, 16 * scale, x, 477 * scale,
                    built[i] ? IM_COL32(188, 218, 195, 255) : IM_COL32(241, 194, 78, 255),
                    build.running && building_choice == i ? "Building..." : checking_builds[i] ? "Checking build..." : build_label(game, i, built[i]));
            }
        }
        if (build.running) {
            const std::string title = "Building " + std::string(building_choice ? "The Lost Age" : "The Broken Seal");
            std::string note = title + (build.stage.empty() ? "..." : ": " + build.stage);
            if (body->CalcTextSizeA(16 * scale, FLT_MAX, 0, note.c_str()).x > width - 72 * scale) {
                while (!note.empty() && body->CalcTextSizeA(16 * scale, FLT_MAX, 0, (note + "...").c_str()).x > width - 72 * scale)
                    note.pop_back();
                note += "...";
            }
            centered_text(draw, body, 16 * scale, center, height - 91 * scale,
                          IM_COL32(241, 194, 78, 255), note.c_str());
            const float left = 36 * scale, right = width - left;
            const float top = height - 65 * scale, bottom = top + 15 * scale;
            draw->AddRectFilled({left, top}, {right, bottom}, IM_COL32(22, 13, 35, 255), 6 * scale);
            if (build.total > 0) {
                const float fraction = gsr_launcher::progress_fraction(build.done, build.total);
                draw->AddRectFilled({left, top}, {left + (right - left) * fraction, bottom},
                                    IM_COL32(172, 118, 220, 220), 6 * scale);
            } else {
                const float phase = float(0.5 - 0.5 * std::cos(elapsed));
                const float segment = (right - left) / 4;
                const float at = left + (right - left - segment) * phase;
                draw->AddRectFilled({at, top}, {at + segment, bottom}, IM_COL32(172, 118, 220, 220), 6 * scale);
            }
        } else if (starting || checking)
            centered_text(draw, body, 18 * scale, center, height - 70 * scale, IM_COL32(221, 218, 195, 255),
                          starting ? "Starting your adventure..." :
                          pending_choice >= 0 && !updates.done() ? "Checking for updates..." : "Checking game build...");
        if (build.running)
            split_controls(draw, body, 16 * scale, center, height - 43 * scale, IM_COL32(188, 198, 199, 255),
                           "Keep this window open", "Esc / B to cancel", scale);
        if (!error.empty()) centered_text(draw, body, 14 * scale, center, height - 22 * scale, IM_COL32(243, 174, 153, 255), error.c_str());
        // What's New, in the titles' gold, bottom right; hidden while a build
        // or a start uses the bottom of the window.
        notes_box = {};
        if (!build.running && !starting && !checking) {
            const std::string version = std::string("Version ") + gsr_online::kReleaseVersion;
            const float version_size = 14 * scale;
            const ImVec2 version_text = body->CalcTextSizeA(version_size, FLT_MAX, 0, version.c_str());
            if (error.empty())
                draw->AddText(body, version_size, {18 * scale, height - 16 * scale - version_text.y},
                              IM_COL32(188, 198, 199, 255), version.c_str());
            const char* label = "What's New!";
            const float size = 18 * scale;
            const ImVec2 text = heading->CalcTextSizeA(size, FLT_MAX, 0, label);
            const ImVec2 at{width - 18 * scale - text.x, height - 16 * scale - text.y};
            notes_box = {{at.x - 8 * scale, at.y - 6 * scale}, {width, height}};
            golden_title(draw, heading, size, at.x + text.x / 2, at.y, label);
            if (!settings_open && !notes_open &&
                ImGui::IsMouseHoveringRect(notes_box.min, notes_box.max, false)) {
                draw->AddLine({at.x, at.y + text.y + 1 * scale}, {at.x + text.x, at.y + text.y + 1 * scale},
                              IM_COL32(241, 194, 78, 255), 1.2f * scale);
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }
        }
        // A generous hit target surrounds the small, vector-drawn gold cog.
        settings_box = {{width - 54 * scale, 12 * scale}, {width - 12 * scale, 54 * scale}};
        ImGui::SetNextWindowPos(settings_box.min);
        ImGui::SetNextWindowSize({42 * scale, 42 * scale});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
        ImGui::Begin("##settings_cog", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        if (ImGui::InvisibleButton("Settings", {42 * scale, 42 * scale}) && !starting && !checking && !build.running) {
            settings_game = selected; settings_open = request_settings = true;
        }
        const bool cog_hovered = ImGui::IsItemHovered() || ImGui::IsItemFocused();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Settings (F2 / Start)");
        auto* cog = ImGui::GetWindowDrawList();
        const ImVec2 cog_center{width - 33 * scale, 33 * scale};
        const ImU32 bronze = IM_COL32(117, 67, 17, 255);
        const ImU32 gold = cog_hovered ? IM_COL32(255, 215, 97, 255) : IM_COL32(228, 177, 53, 255);
        if (cog_hovered) {
            cog->AddCircleFilled(cog_center, 20 * scale, IM_COL32(255, 193, 61, 22));
            cog->AddCircleFilled(cog_center, 16 * scale, IM_COL32(255, 219, 112, 18));
        }
        // Thin teeth and a bevelled annulus keep the icon light at small sizes.
        for (int tooth = 0; tooth < 8; ++tooth) {
            const float angle = tooth * 6.2831853f / 8;
            const ImVec2 radial{std::cos(angle), std::sin(angle)}, tangent{-radial.y, radial.x};
            ImVec2 points[4];
            for (int corner = 0; corner < 4; ++corner) {
                const float r = (corner < 2 ? 8.5f : 13.0f) * scale;
                const float t = (corner == 0 || corner == 3 ? -1.4f : 1.4f) * scale;
                points[corner] = {cog_center.x + radial.x * r + tangent.x * t, cog_center.y + radial.y * r + tangent.y * t};
            }
            cog->AddConvexPolyFilled(points, 4, bronze);
            cog->AddLine(points[0], points[3], IM_COL32(255, 232, 151, 255), 0.85f * scale);
            cog->AddLine(points[3], points[2], gold, 1.4f * scale);
            cog->AddLine(points[1], points[2], gold, 1.0f * scale);
        }
        cog->AddCircle({cog_center.x, cog_center.y + 0.7f * scale}, 9 * scale, bronze, 48, 3.0f * scale);
        for (int segment = 0; segment < 48; ++segment) {
            const float a0 = segment * 6.2831853f / 48, a1 = (segment + 1) * 6.2831853f / 48;
            const float shine = (1 - std::sin((a0 + a1) / 2)) / 2;
            const ImU32 metal = IM_COL32(int(174 + 81 * shine), int(114 + 112 * shine), int(27 + 105 * shine), 255);
            cog->AddLine({cog_center.x + std::cos(a0) * 9 * scale, cog_center.y + std::sin(a0) * 9 * scale},
                         {cog_center.x + std::cos(a1) * 9 * scale, cog_center.y + std::sin(a1) * 9 * scale}, metal, 2.1f * scale);
        }
        cog->AddCircle(cog_center, 3.7f * scale, bronze, 32, 1.2f * scale);
        cog->PathArcTo(cog_center, 3.7f * scale, 3.45f, 5.5f, 16);
        cog->PathStroke(IM_COL32(255, 231, 150, 255), 0, 0.8f * scale);
        const ImVec2 glint{cog_center.x - 5.8f * scale, cog_center.y - 6.9f * scale};
        cog->AddLine({glint.x - 2 * scale, glint.y}, {glint.x + 2 * scale, glint.y}, IM_COL32(255, 247, 206, 240), 0.75f * scale);
        cog->AddLine({glint.x, glint.y - 1.5f * scale}, {glint.x, glint.y + 1.5f * scale}, IM_COL32(255, 247, 206, 240), 0.75f * scale);
        ImGui::End(); ImGui::PopStyleVar();
        if (request_settings) {
            if (preview.empty()) for (int game = 0; game < 2; ++game)
                view_settings[game] = gsr_collection::load_view_settings(
                    gsr_session::engine(gsr_session::game_root(root, game), game).parent_path());
            if (preview.empty()) gbarecomp::game_options_load((gsr_session::engine(gsr_session::game_root(root, 0), 0).parent_path() /
                                                               "game_options.ini").string());
            if (preview.empty()) refresh_music();
            ImGui::OpenPopup("Settings"); request_settings = false;
        }
        ImGui::SetNextWindowPos({center, height / 2}, ImGuiCond_Always, {0.5f, 0.5f});
#ifdef GSR_COLLECTION_DEV
        const float settings_height = 510 * scale;
#else
        const float settings_height = std::max(360.0f, 360 * scale);
#endif
        ImGui::SetNextWindowSize({std::min(width - 32, 550 * scale), std::min(height - 32, settings_height)}, ImGuiCond_Always);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, {0.04f, 0.09f, 0.10f, 0.99f});
        ImGui::PushStyleColor(ImGuiCol_Border, {0.70f, 0.53f, 0.20f, 0.75f});
        ImGui::PushStyleColor(ImGuiCol_CheckMark, {0.95f, 0.76f, 0.31f, 1});
        ImGui::PushStyleColor(ImGuiCol_Button, {0.24f, 0.22f, 0.12f, 1});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.40f, 0.32f, 0.14f, 1});
        ImGui::PushStyleColor(ImGuiCol_Header, {0.35f, 0.28f, 0.12f, 1});
        ImGui::PushStyleColor(ImGuiCol_FrameBg, {0.12f, 0.20f, 0.20f, 1});
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, {0.25f, 0.30f, 0.22f, 1});
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, {0.35f, 0.28f, 0.12f, 1});
        ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, {0.01f, 0.02f, 0.03f, 0.65f});
        ImGui::PushFont(body);
        if (ImGui::BeginPopupModal("Settings", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) {
            if (!settings_open) ImGui::CloseCurrentPopup();
            ImGui::TextColored({0.95f, 0.76f, 0.31f, 1}, "Adventure settings");
            for (int game = 0; game < 2; ++game) {
                if (game) ImGui::SameLine();
                if (ImGui::Selectable(game ? "The Lost Age" : "The Broken Seal", settings_game == game, ImGuiSelectableFlags_DontClosePopups,
                                      {ImGui::GetContentRegionAvail().x / (game ? 1 : 2) - (game ? 0 : 8), 28 * scale})) settings_game = selected = game;
            }
            ImGui::Separator();
            const float reserved = 48 * scale;
            ImGui::BeginChild("Settings options", {0, -reserved});
            auto& game = settings[settings_game];
            if (settings_game == 0) {
                // Seamless mode lives in The Broken Seal's own game_options.ini,
                // the file next to its program (loaded when this page opens, and
                // again before each write so the game's other keys survive).
                const auto options_file = (gsr_session::engine(gsr_session::game_root(root, 0), 0).parent_path() /
                                           "game_options.ini").string();
                bool seamless_on = gbarecomp::game_option_get(gbarecomp::seamless::kOptionKey, 0) != 0;
                if (ImGui::Checkbox("Seamless mode", &seamless_on) && preview.empty()) {
                    gbarecomp::game_options_load(options_file);
                    gbarecomp::game_option_set(gbarecomp::seamless::kOptionKey, seamless_on ? 1 : 0);
                    if (seamless_on) {
                        const std::string warning = std::string(
                            "Build The Lost Age before you finish The Broken Seal: Seamless mode can only "
                            "go on to The Lost Age when it is ready to play.") +
                            (built[1] ? "" : "\n\nThe Lost Age is not built yet.");
                        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Seamless mode", warning.c_str(), window);
                    }
                }
                ImGui::TextWrapped("At the end of the game, skip the credits, save to the file you are playing and go straight on to The Lost Age.");
                ImGui::Separator();
            }
            if (settings_game == 1) {
                if (ImGui::Checkbox("Import Golden Sun save files", &preferences[1].gs_save_import) &&
                    preview.empty() && !gsr_session::save_launcher_audio_settings(
                        gsr_session::game_root(root, 1), preferences[1]))
                    error = "Could not save the import preference.";
                ImGui::Separator();
            }
            if (ImGui::Checkbox("Skip the ROM check", &preferences[settings_game].any_rom) &&
                preview.empty() && !gsr_session::save_launcher_audio_settings(
                    gsr_session::game_root(root, settings_game), preferences[settings_game]))
                error = "Could not save the ROM check preference.";
            ImGui::TextWrapped("Accept a ROM other than the supported USA/Europe one, such as a translation or a hack. "
                               "The game is made for the supported ROM; another one may not work.");
            ImGui::Separator();
            ImGui::BeginDisabled(!music_found[settings_game]);
            if (ImGui::Checkbox("Enable Music Mod", &music_on[settings_game]) && preview.empty() &&
                !gsr_collection::set_music_mod(options_file(settings_game), music_on[settings_game]))
                error = "Could not save the Music mod setting.";
            ImGui::EndDisabled();
            if (music_found[settings_game])
                ImGui::TextWrapped(settings_game == 1
                    ? "Play the recordings in Music/TLA instead of the game's music, and those in Music/TBS for the songs "
                      "carried over from The Broken Seal. Sound effects stay the same. Also in game under F1 > Audio."
                    : "Play the recordings in Music/TBS instead of the game's music. Sound effects stay the same. "
                      "Also in game under F1 > Audio.");
            else
                ImGui::TextWrapped(settings_game == 1
                    ? "No music found. Put recordings in Music/TLA (or Music/TBS) next to the launcher to use a music mod."
                    : "No music found. Put recordings in Music/TBS next to the launcher to use a music mod.");
            ImGui::Separator();
#ifdef _WIN32
            ImGui::TextColored({0.95f, 0.76f, 0.31f, 1}, "Graphics");
            if (ImGui::Checkbox("Use Direct3D", &game.direct3d) && preview.empty() && !gsr_collection::save_settings(root, settings))
                error = "Could not save graphics settings.";
            ImGui::TextWrapped("Leave this off. Direct3D is only for computers where the game does not "
                               "start with OpenGL; some enhancements, screen filters and fixes are "
                               "unavailable with it.");
#else
            ImGui::TextWrapped("Graphics use OpenGL on Linux.");
#endif
            auto& view = view_settings[settings_game];
            bool view_changed = ImGui::Combo("View", &view.mode,
                gbarecomp::kViewModeLabels, gbarecomp::kViewModeCount);
            ImGui::BeginDisabled(view.mode == 0);
            view_changed |= ImGui::Combo("Aspect ratio", &view.aspect,
                gbarecomp::kViewAspectLabels, gbarecomp::kViewAspectCount);
            ImGui::EndDisabled();
            if (view.mode != 0 && view.aspect == gbarecomp::ViewAspectMinimalSharpPixels) view.sharp_pixels = true;
            ImGui::BeginDisabled(view.mode != 0 && view.aspect == gbarecomp::ViewAspectMinimalSharpPixels);
            view_changed |= ImGui::Checkbox("Sharp pixels", &view.sharp_pixels);
            ImGui::EndDisabled();
            ImGui::TextWrapped("Aspect ratios fit inside the selected view. Window follows resizing; Minimal Sharp Pixels adds only enough scenery to fill the window.");
            if (view_changed && preview.empty() &&
                !gsr_collection::save_view_settings(
                    gsr_session::engine(gsr_session::game_root(root, settings_game), settings_game).parent_path(), view))
                error = "Could not save view settings.";
// Commented out: TBS guide options hidden for now.
// #ifdef GSR_COLLECTION_DEV
//             // Quest Tracker, Guide and Area collectibles: developer builds only.
//             if (settings_game == 0)
//                 gsr_collection::adventure_settings(
//                     gsr_session::engine(gsr_session::game_root(root, settings_game), settings_game).parent_path(),
//                     preview.empty(), error);
// #endif
#ifdef GSR_COLLECTION_DEV
            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
#ifdef _WIN32
            ImGui::TextColored({0.95f, 0.76f, 0.31f, 1}, "Game options");
            auto& preference = preferences[settings_game];
            bool preferences_changed = ImGui::Checkbox("Enhanced Options", &preference.enhanced_options);
            ImGui::BeginDisabled(game.direct3d);
            preferences_changed |= ImGui::Checkbox("Draw the field with the graphics card", &preference.gpu_field);
            ImGui::BeginDisabled(!preference.gpu_field);
            preferences_changed |= ImGui::Checkbox("Graphics card only", &preference.gpu_field_only);
            ImGui::EndDisabled(); ImGui::EndDisabled();
            if (!preference.gpu_field) preference.gpu_field_only = false;
            preferences_changed |= ImGui::Checkbox("Copy session ID to clipboard", &preference.copy_session_id_to_clipboard);
            bool build_changed = false;
            if (settings_game == 1) {
                build_changed = ImGui::Checkbox("Pre-built RAM code", &preference.prebuilt_ram_code);
                ImGui::TextWrapped("Off skips about 90%% of the game-code compile: RAM code the game ran in "
                                   "play sessions then compiles while playing, as in player releases. "
                                   "Changing it needs a rebuild.");
            }
            if ((preferences_changed || build_changed) && preview.empty() &&
                !gsr_session::save_launcher_audio_settings(gsr_session::game_root(root, settings_game), preference))
                error = "Could not save game options.";
            else if (build_changed && preview.empty() && building_choice < 0) refresh_builds();
            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
#endif
            ImGui::TextColored({0.95f, 0.76f, 0.31f, 1}, "Developer diagnostics");
            ImGui::Checkbox("Test variables", &game.test_variables);
            ImGui::TextWrapped("Cost probe, host profiler and present cadence are always on in developer launches. Other diagnostics reset when this launcher closes; self-heal RAM can stay enabled independently.");
            ImGui::Spacing();
            for (std::size_t i = 0; i < gsr_collection::test_options.size(); ++i) {
                const auto& option = gsr_collection::test_options[i];
                const bool supported = option.games & (1u << settings_game);
                const bool always_on = gsr::is_developer_performance_option(option.environment);
                ImGui::BeginDisabled(!supported || always_on || (!game.test_variables && i != 0));
                bool shown = supported && (always_on || game.tests[i]);
                if (ImGui::Checkbox(option.label, &shown) && supported) game.tests[i] = shown;
                if (!supported) {
                    ImGui::SameLine();
                    ImGui::TextDisabled(option.games == 1 ? "(The Broken Seal only)" : "(The Lost Age only)");
                }
                if (i == gsr_collection::test_index("TLA_GPU_FIELD_CHECK") &&
                    ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Enables the GPU field renderer for this check. Unavailable with Direct3D.");
                ImGui::EndDisabled();
            }
#endif
            ImGui::EndChild();
            ImGui::Separator();
            ImGui::BeginDisabled(checking_builds[settings_game] || building_choice >= 0);
            if (ImGui::Button(built[settings_game] ? "Rebuild game" : "Build game")) {
                build_choice = settings_game;
                play_after_build = false;
                settings_open = false; ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Done", {90 * scale, 0})) { settings_open = false; ImGui::CloseCurrentPopup(); }
            ImGui::SameLine(); ImGui::TextDisabled("Esc / B to return");
            ImGui::EndPopup();
        }
        settings_open = ImGui::IsPopupOpen("Settings");
        if (request_notes) { ImGui::OpenPopup("What's New"); request_notes = false; }
        ImGui::SetNextWindowPos({center, height / 2}, ImGuiCond_Always, {0.5f, 0.5f});
        ImGui::SetNextWindowSize({std::min(width - 32, 470 * scale), std::min(height - 32, 400 * scale)}, ImGuiCond_Always);
        if (ImGui::BeginPopupModal("What's New", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) {
            if (!notes_open) ImGui::CloseCurrentPopup();
            const auto status = notes.status();
            std::string tag, text;
            if (status == gsr_collection::ReleaseNotes::Status::Ready) notes.read(tag, text);
            ImGui::TextColored({0.95f, 0.76f, 0.31f, 1}, "%s", tag.empty() ? "What's New" : ("What's new in " + tag).c_str());
            ImGui::Separator();
            ImGui::BeginChild("Release notes", {0, -48 * scale});
            ImGui::PushTextWrapPos(0);
            if (status == gsr_collection::ReleaseNotes::Status::Ready)
                ImGui::TextUnformatted(text.empty() ? "This release has no notes." : text.c_str());
            else if (status == gsr_collection::ReleaseNotes::Status::Failed)
                ImGui::TextUnformatted("The changelog could not be loaded. Check your internet connection, "
                                       "or open the release page.");
            else
                ImGui::TextUnformatted("Loading the latest changelog...");
            ImGui::PopTextWrapPos();
            ImGui::EndChild();
            ImGui::Separator();
            if (ImGui::Button("Open release page") && SDL_OpenURL(gsr_online::kReleasesPage) != 0)
                error = std::string("Could not open the release page: ") + SDL_GetError();
            ImGui::SameLine();
            if (ImGui::Button("Done", {90 * scale, 0})) { notes_open = false; ImGui::CloseCurrentPopup(); }
            ImGui::SameLine(); ImGui::TextDisabled("Esc / B to return");
            ImGui::EndPopup();
        }
        notes_open = ImGui::IsPopupOpen("What's New");
        // First launch with a music mod in place: ask once, remember the answer.
        if (request_music_prompt && !settings_open && !notes_open) {
            ImGui::OpenPopup("Music mod"); request_music_prompt = false;
        }
        ImGui::SetNextWindowPos({center, height / 2}, ImGuiCond_Always, {0.5f, 0.5f});
        ImGui::SetNextWindowSize({std::min(width - 32, 470 * scale), 0}, ImGuiCond_Always);
        if (ImGui::BeginPopupModal("Music mod", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::TextColored({0.95f, 0.76f, 0.31f, 1}, "Music mod found");
            ImGui::Separator();
            const std::string games = music_ask[0] && music_ask[1] ? "The Broken Seal and The Lost Age"
                                    : music_ask[0] ? "The Broken Seal" : "The Lost Age";
            ImGui::TextWrapped("The Music folder holds recordings for %s. Play them instead of the game's own music? "
                               "Sound effects stay the same. You can change this at any time in the cogwheel settings, "
                               "or in game under F1 > Audio.", games.c_str());
            ImGui::Separator();
            const auto answer = [&](bool on) {
                for (int game = 0; game < 2; ++game) {
                    if (!music_ask[game]) continue;
                    if (!gsr_collection::set_music_mod(options_file(game), on))
                        error = "Could not save the Music mod setting.";
                    music_on[game] = on;
                    music_ask[game] = false;
                }
                ImGui::CloseCurrentPopup();
            };
            if (ImGui::Button("Enable Music Mod")) answer(true);
            ImGui::SameLine();
            if (ImGui::Button("Keep the original music")) answer(false);
            ImGui::EndPopup();
        }
        music_prompt_open = request_music_prompt || ImGui::IsPopupOpen("Music mod");
        ImGui::PopFont(); ImGui::PopStyleColor(10);
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 10, 22, 32, 255); SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
        // Compositors without window opacity still get a smooth fade to black.
        // Preview captures use this same fallback without touching desktop opacity.
        if (!native_fade && opacity < 1) {
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, Uint8(255 * (1 - opacity)));
            SDL_RenderFillRect(renderer, nullptr);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        }
        if (!preview.empty() && ++preview_frames >= 3) {
            if (!capture_frame(renderer, preview)) { std::fprintf(stderr, "Preview capture failed: %s\n", SDL_GetError()); result = 1; }
            alive = false;
        }
        SDL_RenderPresent(renderer);
        SDL_Delay(8);
    }
    for (auto* controller : controllers) SDL_GameControllerClose(controller);
    ImGui_ImplSDLRenderer2_Shutdown(); ImGui_ImplSDL2_Shutdown(); ImGui::DestroyContext();
    return result;
}
} // namespace

int main(int argc, char** argv) {
    SDL_SetMainReady();
    if (argc >= 2 && std::string(argv[1]) == "--session-game") {
        if (argc < 3) return 1;
        try {
            if (std::string(argv[2]) == "BrokenSeal") return gsr_session_main(0, argc, argv);
            if (std::string(argv[2]) == "LostAge") return gsr_session_main(1, argc, argv);
        } catch (const std::exception& error) {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Golden Sun Recompiled", error.what(), nullptr);
        }
        return 1;
    }
#ifdef _WIN32
    // Everything this launcher starts finds it by this (handover.h).
    SetEnvironmentVariableW(gbarecomp::handover::kLauncherEnv,
                            std::to_wstring(GetCurrentProcessId()).c_str());
#endif
    fs::path preview;
    int preview_choice = 0;
    float preview_insertion = -1;
    int preview_settings = -1;
    if (argc >= 3 && std::string(argv[1]) == "--preview") {
        preview = fs::path(reinterpret_cast<const char8_t*>(argv[2]));
        for (int i = 3; i < argc; i += 2) {
            if (i + 1 >= argc) return 1;
            const std::string option = argv[i];
            if (option == "--choice" && (std::string(argv[i + 1]) == "0" || std::string(argv[i + 1]) == "1")) preview_choice = argv[i + 1][0] - '0';
            else if (option == "--settings" && (std::string(argv[i + 1]) == "0" || std::string(argv[i + 1]) == "1")) preview_settings = argv[i + 1][0] - '0';
            else if (option == "--insertion") {
                char* end = nullptr;
                preview_insertion = std::strtof(argv[i + 1], &end);
                if (end == argv[i + 1] || *end || !std::isfinite(preview_insertion) || preview_insertion < 0 || preview_insertion > 1) return 1;
            } else return 1;
        }
    } else if (argc > 1) { std::fprintf(stderr, "Usage: GoldenSunCollection [--preview PATH.bmp [--choice 0|1] [--insertion 0..1] [--settings 0|1]]\n"); return 1; }
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) { std::fprintf(stderr, "%s\n", SDL_GetError()); return 1; }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    SDL_Window* window = SDL_CreateWindow("Golden Sun Recompiled", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          820, 600, SDL_WINDOW_RESIZABLE | (preview.empty() ? SDL_WINDOW_SHOWN : SDL_WINDOW_HIDDEN));
    if (!window) { std::fprintf(stderr, "%s\n", SDL_GetError()); SDL_Quit(); return 1; }
    SDL_SetWindowMinimumSize(window, 640, 470);
#ifdef _WIN32
    theme_title_bar(window);
#endif
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) { std::fprintf(stderr, "%s\n", SDL_GetError()); SDL_DestroyWindow(window); SDL_Quit(); return 1; }
    const int result = run(window, renderer, collection_root(), preview, preview_choice, preview_insertion, preview_settings);
    SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); SDL_Quit();
    return result;
}
