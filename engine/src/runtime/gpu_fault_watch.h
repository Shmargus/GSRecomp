// Phone graphics-driver faults, written into the session log.
//
// On the Galaxy Tab A9+ (Adreno 619) the driver logs
// "IOCTL_KGSL_GPU_COMMAND failed: errno 71" when the card refuses our work;
// a run of them ended twice on 27 Nov 2026 with errno 35 and the game
// killed. Those lines go to Android's log only, not to ours. This copies
// them in as "[gpu-fault]" lines with the guest frame and the drawing step
// in progress (gpu_timing.h step names), adds the driver's own GL error
// messages (KHR_debug) and a context-reset check, and at the first fault of
// a session saves the last two seconds as a capture (as "Save capture"
// does), so the frames can be replayed on the PC. Always on, like the
// unpacker catcher: it only acts when the driver reports a fault.
// Android only; elsewhere these are no-ops.
#pragma once

namespace gbarecomp {

// Window, once per present on the GL thread: starts the watcher the first
// time, polls the reset status and asks for the capture after a fault.
void gpu_fault_watch_frame();

}  // namespace gbarecomp
