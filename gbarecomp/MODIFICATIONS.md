# This is a modified copy of gbarecomp

Upstream is [`mstan/gbarecomp`](https://github.com/mstan/gbarecomp) by Matthew
Stan, licensed PolyForm Noncommercial 1.0.0 (`LICENSE`, unmodified). This
directory is a **changed** copy of it, carried inside
[GoldenSunRecomp](https://github.com/Shmargus/GoldenSunRecompiled) so that a
checkout of that project always describes an engine that builds the game.

Do not mistake this for upstream. It diverged from upstream commit `af51d0e`
(2026-07-17) and has not been merged back.

## What was changed

12 commits, roughly 47,000 added lines across 130 files. By area:

- **PPU** (`src/gba/gba_ppu.{cpp,h}`) — the expanded view: per-layer
  background sample remapping for regular and affine layers, with the window
  registers asked about the source pixel and an opt-out for layers whose
  placement the game adapter owns; whole-frame assembly at VBlank
  (`g_ws_defer_native_rows`) for frames that magnify a layer; margin
  reconstruction policy hooks; a per-row field-source preparation seam whose
  callback receives the render row's DISPCNT and IO snapshot; and the
  diagnostic dumps the Golden Sun work is measured with. The PPU cost
  attribution in the runtime bridge includes VBlank-rendered expanded rows.
  The wide scanline compositor also builds only the columns a row actually
  draws, asks its row-constant margin questions once per row, and converts a
  colour to RGB888 once at output rather than on every submitted candidate --
  a third off the cost of an expanded frame, pixel-identical, and general to
  any game using the expanded view.
  When no hardware window is enabled, the wide renderer fills the constant
  per-column controls directly; active-window handling is unchanged. The
  isolated full-layer Tret harness case measured a further 3.1% saving, with
  matching before/after pixels (2026-09-14; not a live-game speedup claim).
  Compact wide-path pixel records also reduce the windowed auto-attack/Ray
  harness times by 2.31%/2.66%, with 20 matching framebuffer comparisons.
  The native compositor and pixel ordering are unchanged.
- **Host profiler** (`src/runtime/runtime_bus_bridge.cpp`) — records the first
  caller outside `msvcrt.dll` for CRT samples using Windows x64 unwind metadata,
  allowing the remaining environment-search hotspot to be attributed in a new
  capture. Records contain only addresses/counts, not stack or environment
  contents; sampling remains opt-in. Compile-checked, not yet gameplay-tested.
- **GPU surface** (`src/runtime/gpu_surface.cpp`) — rejects texture requests
  above the active context's measured `GL_MAX_TEXTURE_SIZE` and verifies level-0
  storage before returning a handle, so callers can recover from failed
  allocations instead of treating an empty texture as valid.
- **VRAM trace** (`src/gba/gba_vram_trace.cpp`) — tile and text tracing used to
  identify what the game draws where.
- **Diagnostic settings** (`src/gba/gba_io.cpp`, `src/gba/gba_bus.cpp`) —
  cache the launch-time audio-DMA and unmapped-access logging flags on first
  use, avoiding repeated environment scans while preserving presence-based
  enablement and existing logging limits.
- **ARM runtime** (`src/armv4t/runtime_arm.{cpp,h}`,
  `src/runtime/runtime_bus_bridge.cpp`, `src/runtime/overlay_loader.cpp`) —
  native hand-off at bridge call boundaries, multi-variant healed-code cache,
  background warm-load, asynchronous RAM cache lookup during warm-up,
  removal of a dispatch recursion cap, and RAM overlay work.
- **Audio** (`src/gba/gba_audio.*`, `src/gba/mp2k_shadow.*`,
  `src/gba/mp2k_wall_mixer.cpp`, `src/gba/turbo_audio_scheduler.cpp`) — the
  MP2K shadow mixer and its scheduling.
- **Host** (`src/runtime/host_window.cpp`, `src/runtime/host_config_ui.*`,
  `src/runtime/runtime.cpp`, `src/runtime/crash_handler.cpp`) — presentation,
  the configuration UI, crash reporting, and an opt-in `GSR_OBJ_RECORD`
  guest-frame marker drawn into a presentation copy for sprite diagnostics.
- **Screen filters** (`src/runtime/screen_filter.{h,cpp}`) — test-only LCD3x,
  xBR, CRT Lottes and ScaleFX (multi-pass) GLSL filters at the final present
  on the OpenGL renderer, behind the developer launcher's `GSR_SCREEN_FILTERS`
  box; nothing runs when it is off.
- **Environment switches** (`src/armv4t/env_flag.h`) — on/off switches share
  one parser: empty, `0`, `false`, `off` and `no` are off, anything else is on.
- **Tests and tools** — new suites under `tests/` covering the above, and
  options in `tools/gba_recompile/`.
- **Removed** (2026-09-17) — upstream's Pokémon FireRed widescreen
  experiment, `src/debug/ws_sidecar.*` and `src/debug/ws_provenance.*` with
  their `GBARECOMP_WS_SC_*`/`GBARECOMP_WS_PROBE_*` environment options and
  `docs/WIDESCREEN_STEPC_PLAN.md`. Both were armed only by FireRed program
  counters and were never used by Golden Sun, which solves the same problem
  from reconstructed room data instead. `GBARECOMP_WS_WIP` is kept: it still
  lets a game exceed its declared maximum view size. Deleting them also takes
  the sidecar's per-frame hooks out of the runtime loop and its veto out of
  the present-in-place decision.

The game's own rules and findings live outside this directory, in the parent
repository's `AGENTS.md`, `ROADMAP.md` and `FACTS.md`. Anything here is meant to
be general to ARMv4T, GBA hardware or recompilation, and is a candidate to send
upstream.

## What was left out of this copy

Present upstream, not carried here because none of it is needed to build:

- `.github/` and `.claude/` — upstream's CI config and editor skills.
- `oracle/` — the mGBA-backed co-simulation oracle. It is off by default
  (`GBARECOMP_BUILD_ORACLE=OFF`) and needs a `libmgba` this repository does not
  carry.
- `third_party/mgba` — mGBA's own source. `THIRD_PARTY_ATTRIBUTION.md` (kept
  intact, as upstream wrote it) says mGBA is vendored at that path; in this copy
  it is not. The files here that are derived from mGBA are
  `src/runtime/bios_hle.{h,cpp}`; they remain under MPL-2.0
  (<https://mozilla.org/MPL/2.0/>) and mGBA's source is at
  <https://github.com/mgba-emu/mgba>.

Upstream's own documentation is still here: `README.md`, `PRINCIPLES.md`,
`docs/`, and the rest.

## Skip rows that can only be black (2026-09-13)

`src/gba/gba_ppu.cpp`, `render_scanline_wide`. A row on which every column is
pillarboxed used to run the full BG and sprite pipeline across the canvas and
then have the output loop overwrite it with black. It now probes one column
per class (left margin, right margin, native), and when all three are
pillarboxed it memsets the row and returns before any sampling. The shortcut
stands down while margin diagnostics or the row census are armed, since both
want per-column records.

General engine behaviour, not Golden Sun specific -- any game whose margin
policy pillarboxes a whole edge benefits. Verified pixel-identical with
`tools/ppu_bench.cpp` over five recorded scenes in three provider modes;
20-25% faster on frames with a vertical pillarbox, unchanged without one.

## A generic OpenGL surface for host rendering (2026-09-14)

New: `src/runtime/gpu_surface.{h,cpp}`. A small, game-agnostic OpenGL surface —
shaders, textures (8-bit, RGBA and 16-bit integer), an offscreen render target,
and batched textured quads. It knows nothing about any game.

It deliberately creates no window and no context. The host window already runs
its SDL renderer on the "opengl" driver and Dear ImGui already issues its own
GL beside it at present time, so this attaches to that existing context and
saves and restores every piece of GL state it touches — otherwise SDL's batched
drawing would be corrupted.

Entry points are fetched through `SDL_GL_GetProcAddress` rather than a loader
library, because Windows' `opengl32` exports only OpenGL 1.1. ImGui ships a
loader for its own use; depending on it would couple us to its internals, and
the set needed here is small.

Availability is a runtime question: `init()` reports failure honestly when a
context cannot support shaders or framebuffers, and the caller keeps whatever
renderer it had. There is no partial mode.

Verified on real hardware with `tools/gpu_surface_smoke.cpp` (AMD Radeon RX
7800 XT, GL 4.6 compatibility, GLSL 4.60): entry points load, the offscreen
target is complete, a textured quad samples with the correct orientation, and a
shader reads an exact 16-bit integer texel — the capability the Golden Sun
scene renderer needs for map entries and palettes.

That smoke test found a real defect before anything depended on it: quads are
filled into vertex attribute slots 0-3, but nothing bound the shaders' inputs
to those slots, so the linker assigned them freely and a shader whose inputs
were declared in a different order read the wrong data. `create_program` now
pins `a_pos`, `a_uv`, `a_depth` and `a_tint` to slots 0-3 before linking, and
the smoke test declares its inputs in a deliberately different order to keep
that honest.

## Depth testing and a redirectable colour attachment (2026-09-15)

`src/runtime/gpu_surface.{h,cpp}`. Two additions, still game-agnostic: a depth
renderbuffer with `set_depth_enabled`/`clear_depth`, and
`bind_color_target(texture)` to point the surface's one framebuffer object at
any texture the caller owns instead of its own target, so a caller can do
multi-pass rendering into scratch textures without a second FBO. Both are
attached to the existing `fbo_`; `bind_color_target` only ever touches the
colour attachment, leaving the depth attachment (if any) alone, and vice
versa. The depth renderbuffer is sized to the current target, kept in step by
`resize_depth_to_target()` (see the entry below -- the lazy, enabled-only
resize described here was the defect that entry fixes).

`GL_DEPTH_WRITEMASK` joins the state `begin_frame`/`end_frame` save and
restore, since these additions are the first thing in this file that ever
writes it.

Checked, not assumed: this file's own `ortho()` (used only for `present_target`
blitting, always with depth 0) needed no change. Golden Sun's scene renderer,
the first real caller of depth testing, found its own copy of the same
ortho shape had the z row backwards for that purpose -- see that project's
`field_scene_renderer.cpp` for the fix and why it was never visible before
depth was actually used for anything.

Driving caller and verification: `docs/NATIVE_SCENE_RENDERER_PLAN.md` step 5,
alpha blending and semi-transparent objects, in the Golden Sun repository.

## Depth renderbuffer resized whenever the target changes, not only when depth testing turns on (2026-09-15)

`src/runtime/gpu_surface.{h,cpp}`. The resize above was, in fact, lazy in a way
that lost pixels: it only ran inside `set_depth_enabled(true)`, so a caller
that changed `set_target_size()` and then drew with depth testing OFF kept
rendering into a framebuffer whose colour attachment was the new size and
whose depth attachment was still the old one. New private
`resize_depth_to_target()` keeps the two in step unconditionally, called at
the top of `set_depth_enabled()` regardless of `enabled`.

Real driver behaviour this makes safe to forget: differently sized colour and
depth attachments are not reported `GL_FRAMEBUFFER_INCOMPLETE` on the hardware
this was found on. The renderable area is silently restricted to the
overlap of the two attachments instead, so every pixel outside the smaller
one draws and clears as if untouched -- with no error, no validation
failure, nothing to catch in a log. Found via Golden Sun's widened scene
renderer, which turns depth testing off for two full-screen passes (the
window-control texture and the object-window mask) before ever turning it on
for the two-pass depth peel later in the same frame; on the first frame that
widened the output after a native-sized one, those two off-depth passes ran
against a depth attachment still sized from the native frame. Verified with
`tools/scene_renderer_check.cpp --room --wide 360` over the Golden Sun
snapshot corpus -- see that project's `FACTS.md`, 2026-09-15, for the
before/after pixel counts.
## The scanline compositor can be told to stop drawing (2026-09-16)

`GbaPpu::set_native_raster_enabled(bool)`, default on, so an engine nobody
calls it from behaves exactly as before.

Off, `render_scanline()` still does all of its bookkeeping -- it records that
row's register file and DISPCNT into `line_io_`, stores the row's affine
reference, and advances the affine accumulators by PB/PD -- and simply does
not call `render_scanline_internal()` or `render_scanline_wide()`.
`mark_framebuffer_latched()` likewise skips the deferred native rows and the
vertical margin rows, which are also drawing. The expanded branch still calls
`g_ws_margin_policy`, because that is how a game adapter learns what the frame
is rather than part of putting pixels down.

The split matters because the recorded per-row state is what a host renderer
reads: a host drawing the frame itself through the frame-present override
still needs every register the raster saw, and would otherwise have to run a
whole compositor it then throws away. Two uses, both general:

- a host that draws frames some other way pays for the raster once, not twice;
- a host whose renderer can refuse a frame shows what it refused, instead of
  the picture from here silently standing in for it. A fallback that always
  works is indistinguishable from a renderer that always works.

Found this way in GoldenSunRecomp: its GPU renderer was quietly handing frames
back and the fallback hid every one of them. See that project's `FACTS.md`,
2026-09-16.
## Game value overrides can be installed at startup (2026-09-22)

`RunOptions::thumb_alu_immediate_override` and
`RunOptions::thumb_literal_override`, default null. `run_game` clears
`g_runtime_thumb_alu_imm_override` / `g_runtime_thumb_literal_override` at
start, and until now a game could only set them again from
`extended_view_init`, which runs at startup only when the view is expanded.
The new fields are installed after the ROM-identity check, under the same
gate as `function_entry_observer`, so a game's reviewed override sites work
in the native 240x160 view too. Null keeps every generated site at its
original value, exactly as before.

Found this way in GoldenSunRecomp: the fourth Message-speed choice needs its
overrides in every view mode. See that project's `FACTS.md`, 2026-09-22.

## A 64 KB flash chip reports a 64 KB device ID (2026-09-22)

`GbaSave::configure_flash` reported Macronix MX29L010 (`0xC2`/`0x09`, a
1 Mbit part) for both chip sizes. The 512 Kbit-only SDK driver
(`FLASH_V12x`) does not list `0x09`, so games using it found no usable save
chip and never wrote a save. A 64 KB chip now reports MX29L512 (`0xC2`/`0x1C`),
which that driver lists; 1 Mbit chips are unchanged. The command set the
controller models (byte program, 4 KB sector erase, chip erase) is the one
that part uses.

Found this way in GoldenSunRecomp (`FLASH_V123`). See that project's
`FACTS.md`, 2026-09-22.

## Walking speed changed by game code is saved (2026-09-22)

`host_window.cpp` records the walking-speed multiplier `config.ini` last
held (`Backend::persisted_player_speed`). When the live value
(`runtime_get_mem_write_override_enabled`) differs from it outside the F1
menu, the next pump writes `[Cheats]` through the same `write_cheats_ini`.
Game code can therefore offer the setting in its own menus without a second
save path. Found this way in GoldenSunRecomp (settings page two).

## View mode requested by game code (2026-09-23)

`runtime_request_view_mode(mode)` (`runtime_arm.cpp`) queues a fixed view
mode. The next `HostWindow::pump` takes it with
`runtime_take_view_mode_request`, sets `cfg.fixed_view_mode` and raises
`enhancements_changed`, so it goes through the F1 menu's own apply path:
the view-mode event and the `config.ini` write. After the apply step every
pump publishes the mode in effect (`runtime_publish_view_mode`), read with
`runtime_get_view_mode`. Found this way in GoldenSunRecomp (settings page
two, Screen row).

## SWI override (2026-09-23)

`RunOptions::swi_override(return_pc, swi_num)`, default null, installed
into `g_runtime_swi_override` under the same ROM-identity gate as the other
value overrides and cleared by `run_game`. `runtime_swi` calls it first;
returning 1 drops that SWI (the guest resumes at the return address with
its registers unchanged, no BIOS entry, no HLE). The SWI log does not record
dropped calls. Found this way in GoldenSunRecomp (battle speed-up: one
frame wait's Halt is dropped every other frame in battle).

## No Slowdown requested by game code (2026-09-23)

`runtime_request_no_slowdown(on)` (`runtime_arm.cpp`) queues Enhanced
Timing and CPU Overclock (10x) both on or both off. The next
`HostWindow::pump` sets `cfg.enhanced_timing` (only where the game allows
it) and `cfg.overclock_index`, raises `enhancements_changed`, and so applies
and saves them through the F1 menu's path. Every pump publishes whether both
are on (`runtime_get_no_slowdown`). Found this way in GoldenSunRecomp
(settings page two, No Slowdown row).

## HBlank DMA table latch (2026-09-23)

`gba::g_hblank_dma_latch` (`gba_io.h`), default false. While set, an
HBlank-timed DMA channel with an incrementing source copies its source table
(`word_count * unit * 160` bytes) at the first visible line's trigger, or
when it starts mid-frame, and reads that copy for the rest of the frame.
Game code sets it while it runs two frames of guest work per VBlank, when a
per-line table can be rewritten while it is being scanned out. Found this
way in GoldenSunRecomp (battle speed-up: background tearing in battle).

## Video write observer (2026-09-23)

`vram_trace::set_video_write_observer` (`gba_vram_trace.h`), default null.
Receives writer PC, destination, byte count, DMA source and channel for
every CPU write into palette RAM or VRAM (palette writes reach it through
`note_palette_cpu_write`, called from `gba_bus.cpp`) and once per DMA
transfer whose destination is palette RAM or VRAM. Payload-free. Found this
way in GoldenSunRecomp (recorder for battle BG1 writes, to find the updates
lost at battle speed 2x).

## BIOS inventory: boot/play split and protected reads (2026-09-24)

`GBARECOMP_BIOS_PC_LOG` output gains a `phase` column: `boot` for a BIOS
PC seen only before the first instruction at or above `0x08000000`, `play`
only after it, `both` for either. New `GBARECOMP_BIOS_READ_LOG=path`
(`gba_bus.cpp`, saved at exit by `gba::bios_read_log_save_file`) counts
every data read of `0x0000..0x3FFF` made while BIOS access is locked, keyed
by reading PC, address and width (`pc,addr,width,count`; at most 100,000
keys, overflow counted). Addresses and counts only, never the returned
value. Both default off. Found this way in GoldenSunRecomp (step 1 of
running without a BIOS: what the game needs from it after boot).

## Deep unmatched returns unwind instead of nesting (2026-09-24)

`runtime_call_should_return` (`runtime_arm.cpp`) reports "return" for an
unmatched target once 256 call-return frames (a quarter of the stack) are
live above the IRQ floor. Generated code then returns from the C frame with
R15 set; every generated call site above sees the mismatch, cancels its own
frame and returns, and the dispatch loop (or `runtime_irq`'s re-dispatch
loop) continues at R15. Below the threshold nothing changes. Found this way
in GoldenSunRecomp: the decompressor `Func_2544` calls a bit-refill helper
that rewrites its own return address (`sub lr, pc, #0x11c; ...; bx lr`),
so each refill nested one frame deeper; a large decompression (the world
map's R-button view) overflowed the 1024-entry stack and aborted.

## No-BIOS mode (2026-09-24)

`--no-bios` / `GBARECOMP_NO_BIOS=1` (default off; `runtime_set_no_bios`,
`g_runtime_no_bios`, `runtime_arm.h`). No BIOS file is resolved, loaded or
executed: boot uses `bios_hle_boot_skip(0x08000000)` plus POSTFLG = 1 and the
post-boot open-bus word; `runtime_irq` pushes r0-r3, r12, LR_irq on the IRQ
stack, sets r0 = 0x04000000 and LR = 0x138 and jumps to the handler at
[0x03FFFFFC]; dispatching 0x138 pops them and returns from the exception;
`runtime_swi` services Halt and Stop by writing HALTCNT, every other SWI
through the HLE hook, and aborts on one without a stand-in; dispatching
0x1B4 or 0x170 finishes the SWI exit a BIOS-backed Halt wait left on the
stacks, so existing savestates resume; any other PC below 0x4000 aborts.
Protected BIOS reads return the value hardware latches at each exit
(`GbaBus::set_bios_open_bus`, `g_runtime_bios_open_bus_hook`). The overlay
loader gets no BIOS image. Written from GBATEK's descriptions and the
register/stack layout, not from running BIOS code. Found this way in
GoldenSunRecomp: after boot the game needs only IRQ entry, Halt, Stop and
CpuSet (BIOS_REMOVAL_PLAN.md, step 1). The BIOS-backed path is unchanged.

## Building without the BIOS; no-BIOS by default for a game (2026-09-24)

CMake option `GBARECOMP_LINK_BIOS` (default ON, upstream behaviour): the
locally generated `generated_bios/bios_recompiled.cpp` and
`bios_dispatch_table.cpp` are linked only when it is on and both exist;
otherwise the empty `src/runtime/no_bios_dispatch_table.cpp` is linked and
`GBARECOMP_HAVE_BIOS_RECOMP` is not defined, so the build holds no BIOS code
and `run_game` runs only in no-BIOS mode (an explicit `--bios` or
`GBARECOMP_NO_BIOS=0` then fails with a message). `bios_dispatch_table.cpp`
is no longer committed (it lists every BIOS function's address and name).
`RunOptions::no_bios_by_default` makes no-BIOS the default unless `--bios`
is given on the command line. GoldenSunRecomp sets both: LINK_BIOS off in its
root CMakeLists and `no_bios_by_default = true` in its runner.

## Game-owned menu over the paused picture (2026-09-25)

A new rebindable hotkey `HK_GAME_MENU` (config.ini `[KeyMap] CheatMenu=`,
shown as "Cheat Menu" in the F1 Hotkeys tab, default F11, keyboard or
controller) sets `HostWindow::Events::game_menu`. Three `RunOptions` hooks,
all null by default: `game_menu_toggle` runs on that press;
`keyinput_filter` sees every pump's keys before the guest and input
recording and returns what they get; `paused_overlay` paints over a copy of
the frozen frame on each pump of the paused loop, which then presents the
copy. The game asks for the pause itself through the existing
`pause_request_poll`. Infinite HP/PP switched by game code
(`runtime_set_infinite_hp/pp`) are now saved to `[Cheats]` the same way a
walking-speed change from game code already was. GoldenSunRecomp's cheat
menu uses all of it (`src/cheat_menu.h`).

## Sprites are one layer for blending; non-transparent sprites take BLDCNT (2026-09-25)

Two compositor rules in `gba_ppu.cpp` (both scanline paths), mirrored in
GoldenSunRecomp's GPU renderer:

- At each pixel only the frontmost sprite takes part in blending; a sprite
  is never the "layer below" another sprite. Before, the sprite behind was
  kept as the second target, so a semi-transparent sprite over another
  sprite found no blend partner and fell through to the brightness effect.
- An OBJ is a first target when it is semi-transparent (always) or when
  BLDCNT selects the OBJ layer; only a semi-transparent OBJ forces alpha.
  Before, only semi-transparent OBJs were ever first targets, so a plain
  sprite never brightened or darkened, and any OBJ forced alpha.

GBATEK, "LCD I/O Color Special Effects". Found on Golden Sun's defend and
target-selection flashes (FACTS.md, 2026-09-25).


## A yield unwind hides the resume PC (2026-09-29)

Generated code calls a callee in C and continues only if `R15` equals its
return address. When a scheduler yield unwinds the host stack, a stale call
site (left live by code whose trampoline returned elsewhere, such as the
overlay unpacker) could see `R15` equal to the yield PC and keep running old
C code on new state. `runtime_should_yield()` now stores the real PC in
`g_yield_resume_pc` and leaves `R15 = 0xFFFFFFF0` (outside the GBA bus) so no
site matches and every frame cancels itself. `runtime_yield_restore_pc()`
puts the PC back right after each `runtime_dispatch()` in the run loop
(`runtime.cpp`, both branches), the IRQ drive loop (`runtime_irq`), the
force-interp step and `test_rom_runner`. The `kUnwindDepth` unwind in
`runtime_call_should_return` is unchanged: there the target PC is meant to
match. See FACTS.md "Door+Turbo crash".


## Fast IWRAM store observer gated inline (2026-09-29)

The fast-path `bus_write_u32/16/8_inline` stores called
`g_runtime_fast_iwram_write_observer` twice (START and COMMITTED) through a
function pointer for every IWRAM store, though most stores are stack traffic
the game's observer ignores. `runtime_arm.h` now declares
`g_runtime_fast_iwram_watch[8]` (defined in `runtime_arm.cpp`, zero by
default): a 512-bit bitmap over IWRAM at 64-byte granularity, bit
`(off >> 6) & 63` of word `off >> 12` with `off = addr & 0x7FFF`. The inline
stores call the observer only when it is installed and a block the store
touches (first and last byte, so a straddling store is covered) is set;
START/COMMITTED pairing is unchanged when it is called. The game owner fills
the bitmap when it installs the observer (set every block for a debug store
trace); `runtime.cpp` clears it together with the observer. The observer
keeps its own precise range check, so the bitmap only has to be a superset.
With the bitmap all zero an installed observer is never called.


## Game-owned ROM patch hook (2026-09-30)

`RunOptions::rom_patch` (`runtime.h`) is an optional
`void (*)(std::uint8_t* rom, std::size_t size)`. `run_game` calls it once, in
`runtime.cpp` just before `bus.set_rom(...)`, after the ROM has been read, its
size checked and its SHA-1 verified against the expected value, so the patched
bytes can never fail verification. The bus keeps a pointer to that same buffer
and no other copy is made, so every later ROM read sees the patch. It is meant
for data the game reads at run time only (tables, text). Code and literal
pools are built into the translated code and must not be patched here.

## Alpha blending switch and blend-function save/restore in the GPU surface (2026-09-30)

`src/runtime/gpu_surface.{h,cpp}`. New `set_blend_alpha(bool)`: on enables
`GL_BLEND` with `glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)`, off
disables it; only valid between `begin_frame` and `end_frame`. The surface had
no blend control, and a caller drawing translucent quads over its finished
picture (a host particle burst) needs it. `begin_frame`/`end_frame` now also
save and restore the blend function (`GL_BLEND_SRC_RGB`/`DST_RGB`/
`SRC_ALPHA`/`DST_ALPHA`, restored with `glBlendFuncSeparate`), because the
surface now changes it and SDL's renderer relies on its own. `glBlendFuncSeparate`
(GL 1.4, not in Windows' 1.1 headers) is added to the entry points
`load_entry_points` fetches. Nothing changes for callers that never call
`set_blend_alpha`; no game-code file includes these headers.

Addendum, same day: `set_blend_mode(GpuBlendMode::Off/Alpha/Additive)`. Additive
is `glBlendFunc(GL_SRC_ALPHA, GL_ONE)`, for glowing particles that brighten
where they overlap; `set_blend_alpha(bool)` is now shorthand for Alpha/Off. The
save/restore above already covers both modes.

## The ROM patch hook may grow the ROM (2026-09-30)

`RunOptions::rom_patch` now takes the ROM vector,
`void (*)(std::vector<std::uint8_t>*)`, so a game patch can append data (new
text, for instance) past the original end. It is still called in `runtime.cpp`
immediately before `bus.set_rom(rom.data(), rom.size())`; nothing captured
`rom.data()` or `rom.size()` earlier (they are used only to read the file,
check its size and hash, and parse the header, all before the patch). The bus,
the fast ROM path (`g_fast_rom`/`g_fast_rom_size`, taken from `bus->rom_size()`)
and the audio/overlay users all index by `addr & 0x01FFFFFF` against the
buffer's size, so appended bytes at offset N are served at `0x08000000 + N`
(and its mirrors) with no further change. Grow only when needed and stay
within 16 MB.

## CPU overclock ceiling raised to 50x (2026-09-30)

`kOverclockFactors` (`src/runtime/host_config_ui.h`) is now {1, 50}: the On
setting (the game's "No Slowdown") pins the guest CPU at up to 50x GBA cycles
per frame instead of 10x. Saved `CpuOverclock=` values above 1 still map to On,
so an existing config.ini picks up 50x. The game HALTs once its frame's work is
done, so the ceiling only matters on frames that need more than 10x.

## Window horizontal edges follow mGBA (2026-09-30)

`gba_ppu.cpp`'s two `win_h_in` rules used GBATEK's "X2 > 240 or X1 > X2
-> X2 = 240". They now follow mGBA (`video-software.c`, `GBA_REG_WIN0H` and
`_breakWindow`): X1 > 240 and X1 > X2 reads X1 = 0; X2 > 240 clamps to 240
(and X1 with it); X1 > X2 then wraps to [X1,240) plus [0,X2). Golden Sun's
Boreas summon writes WIN0H 0xFFF1 for the full width; the old rule made the
window empty and hid the summon. Vertical edges are unchanged (mGBA's
vertical window is a per-scanline on/off latch; not needed so far).

## Block timing hooks (2026-09-30)

For the block-timing game code (`tools/block_timing.py` in the game repo):
`runtime_arm.h` declares `g_runtime_deferred_cycles`,
`runtime_tick_deferred` (inline add), `runtime_insn_fetch` (R15 + fetch
cost, no yield check) and `runtime_flush_deferred`. `runtime_tick` first
takes the deferred cycles; `runtime_insn_boundary`, `runtime_dispatch`,
`runtime_swi`, `runtime_msr_cpsr`, `runtime_idle_backedge` and the run loop
after each top-level dispatch pay them. Every load/store helper the game
code calls pays them before an access to 0x04000000-0x07FFFFFF, with the
interrupt check held (`g_tick_irq_held`) so an IRQ is never taken in the
middle of an instruction. With normal game code the deferred count is
always 0 and behaviour is unchanged.

## Memory timing table (2026-09-30)

`runtime_mem_cycles` (`runtime_bus_bridge.cpp`) answers from a table of
`GbaBus::access_cycles` per region x width x S/N, rebuilt whenever WAITCNT
differs from the value it was built with. Data accesses (`sequential ==
2`) use the table unless `data_access_cycles` would apply its prefetch
adjustment (below-cart access, cart code running, WAITCNT prefetch on),
which still goes to the bus. Results are identical; only the virtual call
and switch per access are gone. The rebuild and the prefetch call are
kept out of line so the common path does not save registers.
The table is exported (`g_runtime_mem_cost`, `g_runtime_mem_cost_key` =
the WAITCNT it was built for, `g_runtime_waitcnt_live` = the active bus's
live WAITCNT bytes, set by `set_active_bus`) for the block-timing game code,
which reads it inline and calls `runtime_mem_cycles` when the key differs.

## Volume and Mute are saved (2026-10-01)

The F1 menu's Audio tab (Volume, Mute) changed only the running session:
nothing wrote it, and `runtime.cpp` applied `--volume` (default 100) at
every start, so each run began at 100%. `host_window.cpp` now keeps them in
their own `config.ini` section, `[Audio]` `Volume=0..100` and
`Mute=true|false`, read in `load_input_config` and written by
`write_audio_ini` whenever they differ from what was last recorded -- from
the menu, or from the Volume Up/Down hotkeys. `runtime.cpp` applies
`--volume` only when it is actually passed (`volume_given`), and then it
still wins for that run.

## Unpacker-slot journal keeps its events (2026-10-03)

The `[unpacker-slot]` journal in `runtime_arm.cpp` had one 64-entry ring, and a
decompressor writing over `0x03002000` filled it with CPU stores, so a player
crash report lost the copies, entries and returns that explain it (FACTS.md
2026-10-03). CPU stores now have their own 32-entry ring and every other kind
a 256-entry ring; the dump merges both by sequence number. Each `enter` also
records r0, r1, r6, sp, Golden Sun's IWRAM allocator pointer (`0x03001E54`)
and whether the dispatch resumed a yield. A new `yield_resume` kind records
yield restores whose resume PC is in the slot (repeats collapse into a count).
The dump ends with the last 16 `runtime_dispatch` targets, kept by
`runtime_dispatch` in a 16-entry ring (one store per dispatch).

## RAM fallback probe and resume marks on the dispatch ring (2026-10-08)

`runtime_arm_default_aborts.cpp` exports `g_runtime_ram_fallback_probe`, a
null-by-default hook that `runtime_mutable_ram_code_miss` and
`runtime_dispatch_miss` call first, with the pc execution will continue at,
the pc that was asked for, the mode and which path it is. The game installs
it only for its "Catch unpacker faults" test toggle. The 16-entry dispatch
ring in `runtime_arm.cpp` now also keeps, per entry, whether that dispatch
resumed a scheduler yield (one byte store per dispatch), and
`runtime_recent_dispatch_copy` returns the ring oldest first.

## Unmatched-return hook (2026-10-08)

`runtime_arm.cpp` exports `g_runtime_unmatched_return_hook`, null by default.
`runtime_call_should_return` calls it with the target when a return idiom
matches no live call-return entry, after the existing return hook and probe
and before the deep-unwind check. A nonzero answer makes the call report
"return": the hook has already set g_cpu (R15 included) and the call-return
stack to where the guest really continues, and the generated call sites above
cancel or continue by comparing R15, as after a yield. The game uses it for
its unpacker safety net (`src/unpacker_guard.h`).
