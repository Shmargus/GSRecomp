// ROM-free integration check: the real launcher starts this executable in
// place of a builder, so we inspect the limits seen by a child process.
#include "../../../launcher/collection_build.h"
#include <chrono>
#include <cstdlib>
#include <iostream>

namespace fs = std::filesystem;
using namespace gsr_collection;

static fs::path own_executable() {
#ifdef _WIN32
    wchar_t path[32768];
    DWORD count = GetModuleFileNameW(nullptr, path, 32768);
    return std::wstring(path, count);
#else
    return fs::read_symlink("/proc/self/exe");
#endif
}

static bool inspect(bool reduced) {
    return !reduced || build_limits_active(build_resources());
}

static int descendant(bool reduced) {
    const auto executable = own_executable();
#ifdef _WIN32
    std::wstring command = L"\"" + executable.wstring() + L"\" --inspect " + (reduced ? L"1" : L"0");
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) return 1;
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD result = 1; GetExitCodeProcess(process.hProcess, &result);
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    return int(result);
#else
    const pid_t child = fork();
    if (child == 0) {
        execl(executable.c_str(), executable.c_str(), "--inspect", reduced ? "1" : "0", static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    if (child < 0 || waitpid(child, &status, 0) != child) return 1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--inspect") return inspect(std::string(argv[2]) == "1") ? 0 : 1;
    if (argc > 2 && std::string(argv[1]) == "--rom") {
        const bool reduced = argc > 3 && std::string(argv[3]) == "--low-resources";
        if (!inspect(reduced) || descendant(reduced)) {
            std::cout << "@error Limits missing in builder or descendant\n"; return 1;
        }
        if (fs::path(argv[2]).filename() == "cancel.rom") {
            std::cout << "@stage Waiting for cancellation\n" << std::flush;
            std::this_thread::sleep_for(std::chrono::seconds(30));
        }
        std::cout << (reduced ? "limits verified\n" : "normal verified\n") << "@done\n";
        return 0;
    }

    const auto root = fs::temp_directory_path() / ("GS build resources $ test " +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root / "games");
    auto fail = [&](const std::string& reason) {
        std::cerr << reason << "\nEvidence: " << root << '\n'; return 1;
    };
    for (int game = 0; game < 2; ++game) {
        const auto builder = gsr_session::game_root(root, game) / "builder";
        fs::create_directories(builder);
#ifdef _WIN32
        const auto target = builder / "gsr_builder.exe";
#else
        const auto target = builder / "gsr_builder";
#endif
        fs::copy_file(own_executable(), target);
        fs::permissions(target, fs::perms::owner_all, fs::perm_options::add);
    }
    const auto rom = root / "not a real $ ROM.rom";
    std::ofstream(rom).put('x');
    {
    CollectionBuild build;
    auto wait = [&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (build.snapshot().running && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (build.snapshot().running) build.cancel();
        return build.snapshot();
    };
    for (int game = 0; game < 2; ++game) for (bool reduced : {false, true}) {
        std::string error;
        if (!build.start(root, game, rom, error, reduced)) return fail(error);
        const auto state = wait();
        if (!state.succeeded) return fail(state.error);
        std::ifstream log(root / "local/collection-build.log");
        const std::string output{std::istreambuf_iterator<char>(log), {}};
        if (output.find(reduced ? "limits verified" : "normal verified") == std::string::npos)
            return fail("Wrong build mode");
    }
    const auto cancel_rom = root / "cancel.rom";
    std::ofstream(cancel_rom).put('x');
    std::string error;
    if (!build.start(root, 0, cancel_rom, error, true)) return fail(error);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (build.snapshot().stage != "Waiting for cancellation" &&
            build.snapshot().running && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (build.snapshot().stage != "Waiting for cancellation") return fail("Cancellation fixture failed to start");
    if (build.start(root, 1, rom, error, true)) return fail("Concurrent build was accepted");
    build.cancel();
    const auto cancelled = wait();
    if (cancelled.succeeded || cancelled.running || cancelled.error != "Build cancelled.") return fail("Cancellation failed");
    if (!build.start(root, 1, rom, error, true) || !wait().succeeded) return fail("Build lock not released");
#ifdef _WIN32
    const auto developer = root / "developer";
    fs::create_directories(developer / "scripts");
    std::ofstream(developer / "scripts/build_dev.ps1") << R"ps(
param([string]$Game, [int]$LauncherProcessId, [switch]$SkipLauncher,
      [int]$CpuPercent=90, [int]$RamPercent=90, [switch]$CpuCapInherited)
if (!$SkipLauncher -or $LauncherProcessId -le 0) { exit 2 }
if ($Game -notin @('BrokenSeal','LostAge')) { exit 3 }
if ($CpuCapInherited) {
    if ($CpuPercent -ne 50 -or $RamPercent -ne 50) { exit 4 }
} elseif ($CpuPercent -ne 90 -or $RamPercent -ne 90) { exit 5 }
Write-Output '@done'
)ps";
    for (int game = 0; game < 2; ++game) for (bool reduced : {false, true}) {
        if (!build.start(developer, game, {}, error, reduced) || !wait().succeeded)
            return fail("Developer budget forwarding failed");
    }
#else
    const char* prior_path = std::getenv("PATH");
    const std::string saved_path = prior_path ? prior_path : "";
    setenv("PATH", "/missing-gs-test-tools", 1);
    const bool started = build.start(root, 0, rom, error, true);
    const auto unavailable = started ? wait() : BuildSnapshot{};
    setenv("PATH", saved_path.c_str(), 1);
    if (!started || unavailable.succeeded || unavailable.running || unavailable.error.find("systemd") == std::string::npos)
        return fail("Unavailable limits backend did not fail safely");
    if (!build.start(root, 0, rom, error, false) || !wait().succeeded)
        return fail("Normal mode failed after unavailable limits backend");
#endif
    }
    // Test folders contain only this test's own executable and synthetic files.
    fs::remove_all(root);
    std::cout << "Both games: normal/reduced modes, descendant limits, cancellation, serialization and restart pass\n";
}
