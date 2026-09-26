// runtime_bus_bridge.h — public surface for binding the active bus
// to the recompiled-code runtime.

#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace gba { class GbaBus; }
namespace gba { class GbaPpu; }

// Count of PPU VBlank-start events (scanline 159->160), incremented in
// runtime_tick. The debug step-one-frame primitive stops on its increment
// so the recomp's TCP `step` parks at VBlank-start, matching the
// interpreter and mGBA oracles. Defined in runtime_bus_bridge.cpp.
extern "C" unsigned long long g_runtime_vblank_starts;

// Current guest frame index (the PPU's frame_count()). Unlike
// g_runtime_vblank_starts, this is the same frame key used by the runtime's
// frame-phase CSV and remains correct after a savestate load.
extern "C" unsigned long long runtime_current_frame();

namespace gbarecomp {

// Install the active bus pointer. Subsequent bus_read_u*/bus_write_u*
// calls from generated code (declared in src/armv4t/runtime_arm.h)
// will delegate to this bus.
void set_active_bus(gba::GbaBus* bus);
void set_active_ppu(gba::GbaPpu* ppu);

// Present-time snapshot hook. Called at VBlank start after the faithful
// scanline framebuffer is latched and before VBlank DMA/IRQ work mutates video
// state. Empty disables it.
void set_frame_snapshot_hook(std::function<void()> hook);

// A second, independent observer of that same VBlank-start boundary. Kept
// separate from set_frame_snapshot_hook because that slot belongs to the
// frame-interpolation feature (installed once, unconditionally, from inside
// run_game()) and setting it again from game code would silently replace
// interpolation's own capture. Fired immediately after the snapshot hook
// above, from the same call site, so both see identical, not-yet-DMA'd
// state. Intended for a game-side renderer that needs to copy memory out
// before this frame's VBlank work can mutate it; do no GPU work here, since
// callers may fire this from deep inside guest instruction dispatch rather
// than from the host's present call. Empty disables it.
void set_scene_capture_hook(std::function<void()> hook);

// Optional whole-frame presentation override, consulted by the plain
// (non-interpolated, non-supersampled) present path immediately before it
// shows the canonical framebuffer. `rgb` already holds that frame's
// canonical picture, sized `width`*`height`*3 (RGB888, matching
// GbaPpu::render_width()/render_height() at the point of the call, which is
// already the game's current Expanded View size). Returning true after
// overwriting `rgb` in place presents that picture instead; returning false
// (including when no hook is installed) leaves the canonical picture
// untouched. Never called for frame-interpolated or native-supersampled
// frames. Empty hook (default) never overrides anything, so leaving this
// unset costs nothing on the present path.
void set_frame_present_override_hook(
    std::function<bool(std::uint8_t* rgb, std::uint32_t width,
                       std::uint32_t height)> hook);

// Invokes the hook installed by set_frame_present_override_hook(), if any.
// False (with `rgb` untouched) when no hook is installed. Not for use
// outside the present path in runtime.cpp.
bool invoke_frame_present_override_hook(std::uint8_t* rgb, std::uint32_t width,
                                        std::uint32_t height);

// Savestate-load hook. Called once a savestate has fully applied (guest PC,
// frame_count() and all other snapshot state already updated) — after every
// do_savestate_load() success, regardless of trigger (hotkey, TCP debug
// command, or the --load-state startup arg; see runtime.cpp's shared
// do_savestate_load lambda). A load can move runtime_current_frame()
// discontinuously (backward, or forward by however much guest time the save
// itself recorded), which any observer treating frame numbers as elapsed-time
// deltas within an open span must resynchronize against. Empty disables it.
void set_savestate_load_hook(std::function<void()> hook);

// Fires the hook installed by set_savestate_load_hook(), if any. Called from
// runtime.cpp's do_savestate_load() right after a load applies. Not for use
// outside that one call site.
void notify_savestate_loaded();

// Safe outer emulation boundary. The callback runs after one top-level guest
// dispatch, never from an audio/IRQ hook. It is cleared before runner locals
// captured by the callback leave scope.
void runtime_set_guest_step_boundary_hook(std::function<void()> hook);

// Turbo render-skip gate. Called at most once per guest frame, exactly when
// that frame's scanline counter wraps to 0 — i.e. BEFORE that frame's own
// HBlank scanline rendering starts, with the newly-started frame's index.
// Returning false skips that frame's PPU pixel work (scanline rendering and
// widescreen margin rows) only; every guest-observable PPU effect (HBlank/
// VBlank timing, DISPSTAT, VCOUNT, IRQs, HBlank/VBlank DMA, per-frame state
// latches) still runs unconditionally every frame regardless of the return
// value. Empty hook (default) always renders.
void runtime_set_frame_render_gate_hook(
    std::function<bool(unsigned long long)> hook);

// Retrieve the currently-bound bus / ppu, or nullptr if none.
gba::GbaBus* active_bus();
gba::GbaPpu* active_ppu();

// Minimal, dependency-free 8-bit RGB PNG writer (defined in runtime.cpp;
// the same writer run_game() uses for --dump-png). Exposed so game-owned
// tooling can save a screenshot from outside this translation unit, e.g.
// against gba::GbaPpu::latched_framebuffer()/render_width()/render_height().
bool write_png(const std::string& path, const std::uint8_t* rgb,
               std::uint32_t w, std::uint32_t h);


// Host profiler: mark the region where the frame limiter is deliberately
// asleep, so those samples are not recorded.
//
// Without this the profile is dominated by the pacer's own wait -- 57.1% of
// session_20260917_195930 sat in ntdll, and PERFORMANCE.md records the same
// trap costing a session on 2026-09-13, when 27% of samples looked like
// stalls and were the limiter working correctly. A profile taken to explain
// frames that are OVER budget must not be filled by the frames that came in
// under it. No-op unless GBARECOMP_HOST_PROF is set.
void host_prof_begin_idle();
void host_prof_end_idle();

}  // namespace gbarecomp
