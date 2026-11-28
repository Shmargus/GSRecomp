// Graphics-card time per frame step (launcher test option "GPU timing",
// GBARECOMP_GPU_TIMING=1; the developer and profiling launchers always set
// it). Steps, timed with GL timer queries on the card itself:
//   Game   - the game's own drawing (the present override) outside the
//            field renderer's passes: uploads, margins, effects, held
//            pictures;
//   Masks, FrontPass, SecondPass, Finish - the field renderer's sprite
//            window mask and window control, its two layer passes, and
//            resolve with world-map composition and shake;
//   Screen - the window's present: flicker blend, colours, screen filter,
//            scaling, touch buttons and menus.
// Each also keeps the processor time spent issuing it. Once a minute and at
// exit one "[gpu-time]" line goes to the session log, followed by a
// "[gpu-time] slow steps" line naming the slowest steps over 16.7 ms (card
// or processor) with their guest frame and time into the session. Results are read a few
// frames late and never waited for, so timing does not slow the frame.
// Off (the default) every call is a cheap no-op.
#pragma once

#include <cstdint>

namespace gbarecomp {

// Game is the game's drawing outside the field renderer's passes, which the
// renderer splits off into their own steps (FieldSceneRenderer::draw_frame).
// FrontPass keeps the pass's target setup and clears; its drawing is split
// into the sprites and each background layer (FrontSprites, FrontBg0..3).
enum class GpuTimingStep {
    Game = 0, Screen, Masks, FrontPass, SecondPass, Finish,
    FrontSprites, FrontBg0, FrontBg1, FrontBg2, FrontBg3, Count
};

// Call with the window's GL context current, around the step's GL work.
void gpu_timing_begin(GpuTimingStep step);
void gpu_timing_end(GpuTimingStep step);
// Inside an open step: end whatever is open and begin `next` (one query is
// open at a time). Does nothing when no step is open.
void gpu_timing_switch(GpuTimingStep next);

// Window, after the frame was handed to the screen: collect finished
// results and print the summary when a minute has passed.
void gpu_timing_frame_done();

// Print what has been gathered since the last line (game exit).
void gpu_timing_flush();

// The step most recently begun, whether or not timing is on, as a name
// ("front pass", ...); for gpu_fault_watch.h. Safe from any thread.
const char* gpu_timing_current_step();

struct GpuTimingScope {
    explicit GpuTimingScope(GpuTimingStep s) : step(s) { gpu_timing_begin(step); }
    ~GpuTimingScope() { gpu_timing_end(step); }
    GpuTimingStep step;
};

}  // namespace gbarecomp
