// android_main.cpp: the app's entry point (Android only).
//
// Each launcher activity loads libmain.so and calls SDL_main with:
//
//   launcher                         the collection window
//   session [options] -- <args>      a game session (collection_main's
//                                    --session-game arguments)
//   game    [options] -- <library> <args>
//                                    the game: the game library the phone
//                                    built, then the game's own arguments
//
// The parent can pass --env NAME=VALUE, --unset NAME, --output FILE
// (for stdout and stderr) and --cwd DIR.
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_system.h>

#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>
#include <jni.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "android_host.h"

extern "C" int main(int argc, char** argv);  // collection_main.cpp

namespace {

namespace fs = std::filesystem;

void log_error(const std::string& text) {
    __android_log_print(ANDROID_LOG_ERROR, "GSRecomp", "%s", text.c_str());
    std::fprintf(stderr, "%s\n", text.c_str());
}

void report_exit(int code) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (!env || !activity) return;
    jclass type = env->GetObjectClass(activity);
    if (jmethodID set = env->GetMethodID(type, "setExitCode", "(I)V"))
        env->CallVoidMethod(activity, set, code);
    env->DeleteLocalRef(type);
    env->DeleteLocalRef(activity);
}

// Load the game library and its engine plug-in, then run the game as
// player_main.cpp does on desktop.
int run_game(std::vector<std::string> args) {
    if (args.empty()) {
        log_error("No game library given.");
        return 1;
    }
    const std::string library = args.front();
    void* handle = dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        log_error(std::string("Could not load the game: ") + dlerror());
        return 127;
    }
    using GameMain = int (*)(int, char**, void*, void**);
    auto game_main = reinterpret_cast<GameMain>(dlsym(handle, "gsr_game_main"));
    if (!game_main) {
        log_error("The game library has no gsr_game_main.");
        return 127;
    }
    // The game's own arguments follow the library; argv[0] is the engine, as
    // a PC session starts the game program.
    std::vector<char*> argv;
    for (std::size_t i = 1; i < args.size(); ++i) argv.push_back(args[i].data());
    argv.push_back(nullptr);
    const int argc = static_cast<int>(argv.size()) - 1;

    // Give the game a 16 MiB stack, matching the Windows link settings.
    // SDL runs on a Java thread with about 1 MiB. The Broken Seal's Camelot
    // intro overflows that smaller stack.
    struct Run {
        GameMain game_main;
        int argc;
        char** argv;
        int code = 1;
    } run{game_main, argc, argv.data()};
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 16u << 20);
    pthread_t thread;
    const int started = pthread_create(&thread, &attributes, [](void* data) -> void* {
        auto* r = static_cast<Run*>(data);
        r->code = r->game_main(r->argc, r->argv, nullptr, nullptr);
        return nullptr;
    }, &run);
    pthread_attr_destroy(&attributes);
    if (started != 0) {
        log_error("Could not start the game's thread.");
        return 1;
    }
    pthread_join(thread, nullptr);
    return run.code;
}

}  // namespace

extern "C" __attribute__((visibility("default")))
int SDL_main(int argc, char** argv) {
    // SDL puts "app_process" first (SDL_android.c); GsrActivity's
    // arguments follow, the part first.
    const std::string part = argc > 1 ? argv[1] : "launcher";
    std::vector<std::string> args(argv + std::min(argc, 2), argv + argc);
    std::string output, cwd;
    std::size_t at = 0;
    for (; at < args.size(); ++at) {
        const std::string& a = args[at];
        if (a == "--") { ++at; break; }
        if (a == "--env" && at + 1 < args.size()) {
            const std::string& entry = args[++at];
            const std::size_t equals = entry.find('=');
            if (equals != std::string::npos)
                setenv(entry.substr(0, equals).c_str(), entry.substr(equals + 1).c_str(), 1);
        } else if (a == "--unset" && at + 1 < args.size()) {
            unsetenv(args[++at].c_str());
        } else if (a == "--output" && at + 1 < args.size()) {
            output = args[++at];
        } else if (a == "--cwd" && at + 1 < args.size()) {
            cwd = args[++at];
        }
    }
    args.erase(args.begin(), args.begin() + static_cast<std::ptrdiff_t>(at));

    const fs::path root = gsr_android::storage_root();
    std::error_code ec;
    fs::create_directories(root / "local", ec);
    if (output.empty()) output = (root / "local" / (part + ".log")).string();
    std::freopen(output.c_str(), part == "game" ? "a" : "w", stderr);
    std::freopen(output.c_str(), "a", stdout);
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    // Forward Back as a key so the game can ask before quitting.
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    int code = 0;
    if (part == "launcher") {
        std::string error;
        if (!gsr_android::prepare_collection(&error)) {
            log_error(error);
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Golden Sun Recompiled",
                                     error.c_str(), nullptr);
            return 1;
        }
        if (chdir(root.c_str()) != 0) log_error("Could not enter " + root.string());
        char name[] = "GoldenSunCollection";
        char* launcher_argv[] = {name, nullptr};
        code = main(1, launcher_argv);
    } else if (part == "session") {
        if (chdir(root.c_str()) != 0) log_error("Could not enter " + root.string());
        std::vector<std::string> full = {"GoldenSunCollection"};
        full.insert(full.end(), args.begin(), args.end());
        std::vector<char*> session_argv;
        for (std::string& a : full) session_argv.push_back(a.data());
        session_argv.push_back(nullptr);
        code = main(static_cast<int>(full.size()), session_argv.data());
    } else if (part == "game") {
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) log_error("Could not enter " + cwd);
        code = run_game(args);
    } else {
        log_error("Unknown part: " + part);
        code = 1;
    }
    std::fflush(stdout);
    std::fflush(stderr);
    report_exit(code);
    return code;
}
