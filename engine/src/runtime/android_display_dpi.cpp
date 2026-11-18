// Android only. Dear ImGui's SDL2 backend refreshes its monitor list every
// frame (ImGui_ImplSDL2_UpdateMonitors from ImGui_ImplSDL2_NewFrame), and on
// Android SDL_GetDisplayDPI is a Java call (SDLActivity.getDisplayDPI). Phone
// profile, 18 November 2026: 1.1% of the game thread in Vale with no menu
// open (GS1 FACTS "Phone profile after the field renderer plumbing fix").
// engine/CMakeLists.txt points that one backend file's SDL_GetDisplayDPI here.
// The answer is kept per display and asked again only when the display's
// bounds change (rotation, a folding phone), which SDL reads natively.
#include <SDL.h>

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
