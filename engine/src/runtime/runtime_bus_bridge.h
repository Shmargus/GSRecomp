// runtime_bus_bridge.h: public surface for binding the active bus
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

// A second, independent observer of that same VBlank-start boundary.
//
// It is separate from set_frame_snapshot_hook because that slot belongs to the
// frame-interpolation feature. Interpolation installs it once, unconditionally,
// from inside run_game(). Setting it again from game code would silently
// replace interpolation's own capture.
//
// It fires immediately after the snapshot hook above, from the same call site.
// So both see identical state that has not been through DMA yet.
//
// It is meant for a game-side renderer that needs to copy memory out before
// this frame's VBlank work can change it. Do no GPU work here: callers may
// fire this from deep inside guest instruction dispatch rather than from the
// host's present call. An empty hook disables it.
void set_scene_capture_hook(std::function<void()> hook);

// Optional whole-frame presentation override. The plain present path
// (non-interpolated, non-supersampled) consults it immediately before it shows
// the canonical framebuffer.
//
// `rgb` already holds that frame's canonical picture, sized
// `width`*`height`*3 (RGB888). That matches GbaPpu::render_width() and
// render_height() at the point of the call, which is already the game's
// current Expanded View size.
//   - Returning true, after overwriting `rgb` in place, presents that picture
//     instead.
//   - Returning false (including when no hook is installed) leaves the
//     canonical picture untouched.
//
// Installing a hook turns native supersampling off for the session, since that
// output would bypass the hook. The empty hook (the default) never overrides
// anything, so leaving it unset costs nothing on the present path.
void set_frame_present_override_hook(
    std::function<bool(std::uint8_t* rgb, std::uint32_t width,
                       std::uint32_t height)> hook);

// Invokes the hook installed by set_frame_present_override_hook(), if any.
// False (with `rgb` untouched) when no hook is installed. Not for use
// outside the present path in runtime.cpp.
bool invoke_frame_present_override_hook(std::uint8_t* rgb, std::uint32_t width,
                                        std::uint32_t height);
bool has_frame_present_override_hook();

// Release game-owned presentation resources while the window's GL context
// still exists. The window consumes this callback once, before teardown.
void set_frame_present_shutdown_hook(std::function<void()> hook);
void notify_frame_present_shutdown();

// Called after any successful savestate load, once the guest PC and frame
// counter are restored. Frame numbers can jump in either direction. Reset
// any observer that measures elapsed time from a previous frame. Empty
// disables the hook.
void set_savestate_load_hook(std::function<void()> hook);

// Fires the hook installed by set_savestate_load_hook(), if any. Called from
// runtime.cpp's do_savestate_load() right after a load applies. Not for use
// outside that one call site.
void notify_savestate_loaded();

// Safe outer emulation boundary. The callback runs after one top-level guest
// dispatch, never from an audio/IRQ hook. It is cleared before runner locals
// captured by the callback leave scope.
void runtime_set_guest_step_boundary_hook(std::function<void()> hook);

// Runs at scanline 0 before the new frame's pixel work. Return false to
// skip drawing that frame and its margins. PPU timing, status registers,
// IRQs, DMA and per-frame latches still run. An empty hook always draws.
void runtime_set_frame_render_gate_hook(
    std::function<bool(unsigned long long)> hook);

// A game rule can hold every F1 cheat off (Golden Sun's Challenge Mode). While
// it does:
//   - Infinite HP / PP are not applied.
//   - Each cheat option reads as its normal value.
//   - The player's choices are kept, but not applied.
//   - F1 shows the Cheats page at its normal values, unavailable.
// The game sets this each frame. It is false by default.
void set_cheats_blocked(bool blocked);
bool cheats_blocked();
// A cheat's game_options.ini value, or `normal` while cheats are blocked.
// Every cheat option reads through this.
int cheat_option_get(const char* key, int normal);

// The F1 "Flicker reduction" choice (temporal_blend.h index), published by
// the host window each presented frame so a game's own renderer can apply
// the same setting (FieldSceneRenderer::set_flicker_smoothing).
void set_flicker_reduction_index(int index);
int flicker_reduction_index();

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
// Without this, the profile is dominated by the pacer's own wait (in one
// session 57% of samples sat in ntdll). That looks like stalls, but it is the
// limiter working correctly. A profile taken to explain frames that are OVER
// budget must not be filled by the frames that came in under it.
// No-op unless GBARECOMP_HOST_PROF is set.
void host_prof_begin_idle();
void host_prof_end_idle();

}  // namespace gbarecomp
