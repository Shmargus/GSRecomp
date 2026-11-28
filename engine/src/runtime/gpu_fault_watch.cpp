#include "gpu_fault_watch.h"

#if defined(__ANDROID__) && defined(GBARECOMP_HAVE_SDL2)

#include <SDL.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "frame_history.h"
#include "gl_platform.h"
#include "gpu_timing.h"
#include "runtime_bus_bridge.h"

namespace gbarecomp {
namespace {

std::atomic<bool> g_started{false};
std::atomic<int> g_faults{0};
std::atomic<bool> g_capture_wanted{false};
bool g_capture_done = false;
std::atomic<int> g_debug_lines{0};

constexpr unsigned kDebugOutput = 0x92E0;
constexpr unsigned kSeverityHigh = 0x9146;
constexpr unsigned kSeverityMedium = 0x9147;
constexpr unsigned kNoError = 0;
constexpr int kDebugLineLimit = 60;  // GL messages kept per session

using DebugProc = void(APIENTRY*)(unsigned source, unsigned type, unsigned id,
                                  unsigned severity, int length, const char* message,
                                  const void* user);
using DebugMessageCallback = void(APIENTRY*)(DebugProc, const void*);
using GetResetStatus = unsigned(APIENTRY*)();
GetResetStatus g_reset_status = nullptr;
unsigned g_last_reset = kNoError;

void APIENTRY on_debug_message(unsigned source, unsigned type, unsigned id,
                               unsigned severity, int length, const char* message,
                               const void*) {
    if (severity != kSeverityHigh && severity != kSeverityMedium) return;
    const int n = g_debug_lines.fetch_add(1);
    if (n >= kDebugLineLimit) return;
    std::fprintf(stderr,
                 "[gpu-fault] driver message frame=%llu step=%s source=0x%X type=0x%X "
                 "id=%u severity=%s: %.*s%s\n",
                 runtime_current_frame(), gpu_timing_current_step(), source, type, id,
                 severity == kSeverityHigh ? "high" : "medium",
                 length > 0 ? length : static_cast<int>(std::strlen(message)), message,
                 n + 1 == kDebugLineLimit ? " (no more driver messages logged)" : "");
}

// Our own process's lines from the graphics driver (an app may read its own
// log). Each fault line is copied with the frame and step at that moment.
void watch_driver_log() {
    char command[128];
    std::snprintf(command, sizeof(command),
                  "logcat -v time -T 1 --pid=%d Adreno-GSL:V Adreno-ES20:V "
                  "AdrenoGLES:V libEGL:W *:S",
                  static_cast<int>(getpid()));
    FILE* pipe = popen(command, "r");
    if (!pipe) {
        std::fprintf(stderr, "[gpu-fault] cannot read the driver's log; faults not recorded\n");
        return;
    }
    std::fprintf(stderr, "[gpu-fault] watching the graphics driver's log\n");
    char line[1024];
    while (std::fgets(line, sizeof(line), pipe)) {
        // logcat's own banner and the line -T 1 replays from before start.
        if (std::strncmp(line, "--------- beginning", 19) == 0) continue;
        std::size_t len = std::strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
        const bool fault = std::strstr(line, "IOCTL_KGSL") != nullptr ||
                           std::strstr(line, "errno") != nullptr;
        if (fault) {
            const int n = g_faults.fetch_add(1) + 1;
            if (n == 1) g_capture_wanted.store(true);
            std::fprintf(stderr, "[gpu-fault] #%d frame=%llu step=%s | %s\n", n,
                         runtime_current_frame(), gpu_timing_current_step(), line);
        } else {
            std::fprintf(stderr, "[gpu-fault] driver frame=%llu step=%s | %s\n",
                         runtime_current_frame(), gpu_timing_current_step(), line);
        }
    }
    pclose(pipe);
}

void start() {
    std::thread(watch_driver_log).detach();
    if (SDL_GL_GetCurrentContext()) {
        const auto load = [](const char* a, const char* b) {
            void* p = SDL_GL_GetProcAddress(a);
            return p ? p : SDL_GL_GetProcAddress(b);
        };
        if (auto callback = reinterpret_cast<DebugMessageCallback>(
                load("glDebugMessageCallback", "glDebugMessageCallbackKHR"))) {
            callback(on_debug_message, nullptr);
            glEnable(kDebugOutput);
            std::fprintf(stderr, "[gpu-fault] graphics driver messages on\n");
        }
        g_reset_status = reinterpret_cast<GetResetStatus>(
            load("glGetGraphicsResetStatus", "glGetGraphicsResetStatusEXT"));
    }
}

}  // namespace

void gpu_fault_watch_frame() {
    if (!g_started.exchange(true)) start();
    if (g_reset_status) {
        const unsigned status = g_reset_status();
        if (status != g_last_reset) {
            g_last_reset = status;
            std::fprintf(stderr,
                         "[gpu-fault] context reset status 0x%X (0x8253 our work, 0x8254 "
                         "another app, 0x8255 unknown) frame=%llu step=%s\n",
                         status, runtime_current_frame(), gpu_timing_current_step());
        }
    }
    // The capture saves the frames before this one: the first fault's.
    if (!g_capture_done && g_capture_wanted.load()) {
        g_capture_done = true;
        frame_history_request_capture();
        std::fprintf(stderr,
                     "[gpu-fault] saving the last two seconds as a capture (first fault)\n");
    }
}

}  // namespace gbarecomp

#else

namespace gbarecomp {
void gpu_fault_watch_frame() {}
}  // namespace gbarecomp

#endif
