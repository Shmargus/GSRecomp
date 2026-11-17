// bench_main.cpp: runs a game library the phone built, without the app, for
// checks over adb. Not part of build_apk.py: build it by hand with the NDK's
// clang++ for aarch64-linux-android28, push it with adb and run:
//
//   gsr_bench <game library> <engine> --rom <rom> --no-window --frames N ...
//
// It does what the app's game process does (launcher/android_main.cpp):
// loads the library, which brings the engine plug-in, and runs
// gsr_game_main on a 16 MiB thread.
#include <dlfcn.h>
#include <pthread.h>

#include <cstdio>

namespace {
using GameMain = int (*)(int, char**, void*, void**);
struct Run {
    GameMain game_main;
    int argc;
    char** argv;
    int code = 1;
};
}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: gsr_bench <game library> <engine> [game arguments]\n");
        return 2;
    }
    void* handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        std::fprintf(stderr, "dlopen: %s\n", dlerror());
        return 127;
    }
    auto game_main = reinterpret_cast<GameMain>(dlsym(handle, "gsr_game_main"));
    if (!game_main) {
        std::fprintf(stderr, "no gsr_game_main\n");
        return 127;
    }
    Run run{game_main, argc - 2, argv + 2};
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 16u << 20);
    pthread_t thread;
    pthread_create(&thread, &attributes, [](void* data) -> void* {
        auto* r = static_cast<Run*>(data);
        r->code = r->game_main(r->argc, r->argv, nullptr, nullptr);
        return nullptr;
    }, &run);
    pthread_join(thread, nullptr);
    return run.code;
}
