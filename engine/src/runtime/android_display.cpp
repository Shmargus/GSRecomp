// Android only: display calls shared by the games and the launcher.
//
// gsr_cached_display_dpi: Dear ImGui's SDL2 backend refreshes its monitor list every
// frame (ImGui_ImplSDL2_UpdateMonitors from ImGui_ImplSDL2_NewFrame), and on
// Android SDL_GetDisplayDPI is a Java call (SDLActivity.getDisplayDPI). A phone
// profile measured it at 1.1% of the game thread with no menu open.
// engine/CMakeLists.txt points that one backend file's SDL_GetDisplayDPI here.
// The answer is kept per display and asked again only when the display's
// bounds change (rotation, a folding phone), which SDL reads natively.
#include "android_display.h"

#include <SDL.h>
#include <SDL_system.h>
#include <jni.h>

#include <cstdio>
#include <cstring>

extern "C" int gsr_cached_display_dpi(int display, float* ddpi, float* hdpi, float* vdpi) {
    struct Entry {
        SDL_Rect bounds;
        int result;
        float d, h, v;
        bool valid;
    };
    static Entry cache[8] = {};
    if (display < 0 || display >= int(sizeof(cache) / sizeof(cache[0])))
        return SDL_GetDisplayDPI(display, ddpi, hdpi, vdpi);
    SDL_Rect bounds{};
    SDL_GetDisplayBounds(display, &bounds);
    Entry& e = cache[display];
    if (!e.valid || std::memcmp(&bounds, &e.bounds, sizeof bounds) != 0) {
        e.d = e.h = e.v = 0.0f;
        e.result = SDL_GetDisplayDPI(display, &e.d, &e.h, &e.v);
        e.bounds = bounds;
        e.valid = true;
    }
    if (ddpi) *ddpi = e.d;
    if (hdpi) *hdpi = e.h;
    if (vdpi) *vdpi = e.v;
    return e.result;
}

// Tells Android the game's frame rate; otherwise the screen can run at 120 Hz
// under a 60 Hz game.
void android_set_frame_rate(double hz) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (!env || !activity) return;
    jclass type = env->GetObjectClass(activity);
    jmethodID method = type ? env->GetMethodID(type, "setFrameRate", "(F)V") : nullptr;
    if (method) env->CallVoidMethod(activity, method, static_cast<jfloat>(hz));
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (type) env->DeleteLocalRef(type);
    env->DeleteLocalRef(activity);
    std::fprintf(stderr, "[frame-rate] asked the system for %.3f Hz\n", hz);
}
