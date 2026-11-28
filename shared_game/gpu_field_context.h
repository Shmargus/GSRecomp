// The graphics card renderer each game draws its field with, both games.
// Created on first use at present time, where the window's GL context is
// already current (gpu_surface.h attaches to it, never makes its own), and
// torn down by the present-shutdown hook before the GL window goes.
#pragma once

#include <cstdio>
#include <memory>

#include "field_scene_renderer.h"
#include "gpu_surface.h"
#include "runtime_bus_bridge.h"

namespace gsr {

// `Extra`: what a game keeps beside the renderer (The Lost Age's draw state).
template <class Extra>
struct GpuFieldContext {
    gbarecomp::GpuSurface surface;
    FieldSceneRenderer renderer;
    Extra extra{};
};

// The context, or null when the renderer could not start (the reason is
// printed once). `on_shutdown` runs first when the window closes (drop
// queued picture copies); `on_ready` runs once after a successful start.
template <class Extra>
GpuFieldContext<Extra>* gpu_field_context(void (*on_shutdown)(),
                                          void (*on_ready)(FieldSceneRenderer&)) {
    static std::unique_ptr<GpuFieldContext<Extra>> context;
    static bool attempted = false;
    static bool ok = false;
    static void (*shutdown)() = nullptr;
    shutdown = on_shutdown;
    if (!context) {
        context = std::make_unique<GpuFieldContext<Extra>>();
        gbarecomp::set_frame_present_shutdown_hook([] {
            if (shutdown) shutdown();
            context.reset();  // renderer, then PBOs/fences, before the GL window
            attempted = ok = false;
        });
    }
    if (!attempted) {
        attempted = true;
        ok = context->surface.init() && context->renderer.init(&context->surface);
        if (ok) {
            if (on_ready) on_ready(context->renderer);
        } else {
            std::fprintf(stderr,
                         "[gpu-field] renderer unavailable (%s); staying on the "
                         "console compositor\n",
                         !context->surface.ready() ? context->surface.failure()
                                                   : context->renderer.failure());
        }
    }
    return ok ? context.get() : nullptr;
}

}  // namespace gsr
