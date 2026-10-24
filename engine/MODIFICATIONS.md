# This is a modified copy of gbarecomp

Upstream is [`mstan/gbarecomp`](https://github.com/mstan/gbarecomp) by Matthew
Stan, licensed PolyForm Noncommercial 1.0.0 (`LICENSE`, unmodified). This
directory is a **changed** copy of it, carried inside
[Golden Sun Recompiled](https://github.com/Shmargus/GSRecomp) so that a
checkout of that project describes the one engine both games use. It diverged
from upstream commit `af51d0e` (2026-07-17) and has not been merged back.

Game-specific code (addresses, hooks, ROM configuration) lives in the game
folders. Engine changes are meant to be general to ARMv4T, GBA hardware,
recompilation or the host, and are candidates to send upstream. The public
overview and build instructions are in the root `README.md` and `BUILDING.md`.

## What this copy changes

### Recompiler and generated code

- **Guaranteed tail jumps.** Every guest transfer that never returns (B,
  computed jumps, non-return BX/POP/LDR PC, fall-through, and the mid-function
  alias entry stubs in `gba_recompile`) is emitted as
  `GBARECOMP_TAIL_TRANSFER(call)` (`runtime_arm_types.h`; one helper,
  `emit_tail_transfer`, in `arm_codegen`). Under GCC 15+ in C++ it is
  `[[gnu::musttail]] return call`, a host jump at any optimisation level;
  elsewhere (tcc heal overlays, older GCC) it stays call-and-return. At `-Og`
  GCC otherwise kept every `call(); return;` frame and a copied map
  decompressor grew the native stack until it crashed. Existing generated
  sources must be regenerated to receive the new statements.
- **Dynamic-operand support** in the translator, and extra options in
  `tools/gba_recompile/`.
- **Deep unmatched returns unwind instead of nesting.**
  `runtime_call_should_return` (`runtime_arm.cpp`) reports "return" for an
  unmatched target once 256 call-return frames (a quarter of the stack) are
  live above the IRQ floor; generated call sites above see the R15 mismatch,
  cancel their frames and return, and the dispatch loop continues at R15. This
  fixed a bit-refill helper that rewrites its own return address and nested one
  frame deeper per refill.
- **A yield unwind hides the resume PC.** `runtime_should_yield()` stores the
  real PC in `g_yield_resume_pc` and leaves `R15 = 0xFFFFFFF0` (outside the GBA
  bus), so a stale call site cannot match the yield PC and keep running old C
  code on new state. `runtime_yield_restore_pc()` restores it after each
  `runtime_dispatch()` in the run loop, the IRQ drive loop and the force-interp
  step.
- **Block timing hooks.** `runtime_arm.h` declares `g_runtime_deferred_cycles`,
  `runtime_tick_deferred`, `runtime_insn_fetch` and `runtime_flush_deferred`, so
  generated code with batched block timing can defer cycle charges; the tick,
  dispatch, SWI, MSR, idle-backedge, run-loop and load/store paths pay them
  (the latter with the interrupt check held, `g_tick_irq_held`). With ordinary
  code the deferred count is 0.
- **Memory timing table.** `runtime_mem_cycles` (`runtime_bus_bridge.cpp`)
  answers from a table of `GbaBus::access_cycles` per region x width x S/N,
  rebuilt when WAITCNT changes, unless the bus's prefetch adjustment applies.
  Exported as `g_runtime_mem_cost`, `g_runtime_mem_cost_key` and
  `g_runtime_waitcnt_live`.
- **Fast IWRAM store observer gated inline.** `g_runtime_fast_iwram_watch[8]`
  is a 512-bit bitmap over IWRAM at 64-byte granularity. The inline
  `bus_write_u32/16/8_inline` stores call the installed observer only when a
  block they touch is set; all zero means it is never called. The game fills
  the bitmap when it installs the observer.
- **Idle skip for screen-line waits.** `is_idle_safe_read` accepts
  DISPSTAT/VCOUNT, so a loop polling the scanline counter is skipped up to the
  next PPU event like a RAM poll.
- **Host calls into game routines.** `gbarecomp::guest_call` (`runtime.h`) runs
  one game routine from host code like a `bl`, returns r0 and restores every
  register. While it runs, `runtime_should_yield` never unwinds: a HALT is
  waited out one presented frame at a time, and a HALT no interrupt can end
  (IE 0) or a window close abandons the call. `guest_return_now`, called from a
  function-entry observer, makes that routine return at once with a given r0.

### ARM runtime, RAM code and overlays

- **Hand-off and caches.** Native hand-off at bridge call boundaries, a
  multi-variant healed-code cache, background warm-load, asynchronous RAM cache
  lookup during warm-up, and removal of a dispatch recursion cap
  (`runtime_arm.{cpp,h}`, `runtime_bus_bridge.cpp`, `overlay_loader.cpp`).
  Overlay caches use the `engine-v2` layout. `overlay_compile_one`'s exact-file
  load-only path is used once per backend instead of enumerating the cache
  folder on the game thread, and `overlay_drain_ready` logs per-routine details
  only for requested work, not cached loads.
- **RAM handoff after a direct guard miss.** When a direct call's fixed-code
  guard fails, `runtime_mutable_ram_code_miss` checks runtime-healed code, then
  consults the game-owned verified RAM-image resolver before bridging into the
  interpreter. Re-entry is guarded by a linked PC/mode resolution stack and by
  comparing `g_runtime_cycles + g_runtime_deferred_cycles`, so only re-entry
  with no guest progress is rejected.
- **Healed RAM conditional-branch hooks.** The overlay loader installs a
  condition callback (`overlay_condition_passes`) that forwards live Thumb/ARM RAM conditional `B`
  instructions to the game's existing hook with packed bank-relative context,
  so cached native routines do not skip the game's overrides. Other
  instructions, BL, AL/NV and non-RAM locations keep the original behaviour.
- **Game RAM layout.** `armv4t/game_ram_layout.h`: `g_game_ram_layout`, set by
  the game's runner, holds the unpacker slot and allocator pointer, RAM-image
  window, OAM shadow, IWRAM heap and pool-LDM probe region. Zero means inert, so
  no game address lives in the engine. The Infinite HP/PP party layout defaults
  to 0 slots; each game calls `runtime_set_party_layout`.
- **Unpacker-slot journal.** CPU stores to the unpacker slot have their own
  32-entry ring and other event kinds a 256-entry ring, merged by sequence
  number in the dump. `enter` events record r0, r1, r6, sp, the allocator
  pointer and whether a yield was resumed. The dump ends with the last 16
  `runtime_dispatch` targets (`runtime_recent_dispatch_copy`, oldest first).
- **Fallback probe and unmatched-return hook.** `g_runtime_ram_fallback_probe`
  (null by default) is called first by `runtime_mutable_ram_code_miss` and
  `runtime_dispatch_miss` with the pc execution continues at, the pc requested,
  the mode and the path. `g_runtime_unmatched_return_hook` is called by
  `runtime_call_should_return` when a return idiom matches no live call-return
  entry; a nonzero answer means the hook already set g_cpu and the call-return
  stack to where the guest really continues.
- **Sprite and placement records** (`field/`). The shared `field_obj_is_parked`
  matches the complete blank attr0/attr1 pair, not coordinates plus a
  non-affine status. `field/obj_placements.h` stores `uses_tile_grant` (default
  false) with each placement snapshot so only builders whose records carry a
  character-allocation byte consult that table. Placement records can accept
  outgoing sprite attributes from a builder hook, so a pose change keeps the
  right sprite-page identity.

### BIOS and no-BIOS mode

- **No-BIOS mode.** `--no-bios` / `GBARECOMP_NO_BIOS=1` (`runtime_set_no_bios`,
  `g_runtime_no_bios`): no BIOS file is resolved, loaded or executed. Boot uses
  `bios_hle_boot_skip(0x08000000)`, POSTFLG = 1 and the post-boot open-bus word.
  `runtime_irq` pushes r0-r3, r12, LR_irq, sets r0 = 0x04000000 and
  LR = 0x138 and jumps to the handler at [0x03FFFFFC]; dispatching 0x138 pops
  them and returns from the exception. `runtime_swi` services Halt and Stop by
  writing HALTCNT and every other SWI through the HLE hook, and aborts on one
  without a stand-in. Dispatching 0x1B4 or 0x170 finishes a SWI exit that a
  BIOS-backed Halt wait left on the stacks, so older savestates resume. Any
  other PC below 0x4000 aborts. Protected BIOS reads return the value hardware
  latches at each exit (`GbaBus::set_bios_open_bus`,
  `g_runtime_bios_open_bus_hook`). Written from GBATEK's descriptions, not from
  running BIOS code. The BIOS-backed path is unchanged.
- **Building without the BIOS.** CMake option `GBARECOMP_LINK_BIOS` (default ON,
  upstream behaviour). The locally generated `generated_bios/` sources are
  linked only when it is on and both exist; otherwise the empty
  `src/runtime/no_bios_dispatch_table.cpp` is linked and
  `GBARECOMP_HAVE_BIOS_RECOMP` is undefined, so `run_game` runs only in no-BIOS
  mode (`--bios` or `GBARECOMP_NO_BIOS=0` then fails with a message).
  `bios_dispatch_table.cpp` is no longer committed.
  `RunOptions::no_bios_by_default` makes no-BIOS the default unless `--bios` is
  given; both games set it and build with `GBARECOMP_LINK_BIOS` off.
- **BIOS inventory logs.** `GBARECOMP_BIOS_PC_LOG` output has a `phase` column
  (`boot`, `play` or `both`, split at the first instruction at or above
  0x08000000). `GBARECOMP_BIOS_READ_LOG=path` (`gba_bus.cpp`) counts data reads
  of 0x0000-0x3FFF made while BIOS access is locked, keyed by reading PC, address
  and width (`pc,addr,width,count`; at most 100,000 keys). Counts only, never
  values. Both default off.
- **SWI override.** `RunOptions::swi_override(return_pc, swi_num)` (default
  null, `g_runtime_swi_override`): `runtime_swi` calls it first, and returning 1
  drops that SWI (guest resumes at the return address, no BIOS entry, no HLE).

### PPU, wider views and the field renderer

- **Expanded view in the PPU** (`gba/gba_ppu.{cpp,h}`). Per-layer background
  sample remapping for regular and affine layers, with the window registers
  asked about the source pixel and an opt-out for layers whose placement the
  game adapter owns; whole-frame assembly at VBlank (`g_ws_defer_native_rows`)
  for frames that magnify a layer; margin reconstruction policy hooks; a
  per-row field-source preparation seam whose callback receives the row's
  DISPCNT and IO snapshot. `GBARECOMP_WS_WIP` lets a game exceed its declared
  maximum view size.
- **Faster wide compositor.** It builds only the columns a row draws, asks
  row-constant margin questions once per row, converts colour to RGB888 once at
  output and uses compact pixel records; pixel-identical. A row whose left
  margin, right margin and native columns are all pillarboxed is memset before
  any sampling (not while margin diagnostics or the row census are armed).
- **Native raster can be switched off.** `GbaPpu::set_native_raster_enabled(
  bool)`, default on. Off, `render_scanline()` still records each row's register
  file and DISPCNT into `line_io_` and advances the affine accumulators, but does
  not draw (`render_scanline_internal()`, `render_scanline_wide()`, deferred
  native and vertical-margin rows). A host that draws frames itself pays for the
  raster once, and a frame its renderer refuses shows as refused instead of
  being hidden by a fallback picture.
- **Blending rules.** In both scanline paths only the frontmost sprite takes part
  in blending. An OBJ is a first target when semi-transparent or when BLDCNT
  selects the OBJ layer; only a semi-transparent OBJ forces alpha (GBATEK, "LCD
  I/O Color Special Effects").
- **Window horizontal edges follow mGBA** (`video-software.c`, `GBA_REG_WIN0H`,
  `_breakWindow`): X1 > 240 and X1 > X2 reads X1 = 0; X2 > 240 clamps to 240
  (and X1 with it); X1 > X2 wraps to [X1,240) plus [0,X2). Vertical edges are
  unchanged.
- **HBlank DMA table latch.** `gba::g_hblank_dma_latch` (`gba_io.h`, default
  false): while set, an HBlank-timed DMA channel with an incrementing source
  copies its table (`word_count * unit * 160` bytes) at the first visible line
  or when it starts mid-frame, and reads that copy for the rest of the frame.
- **Video write observer.** `vram_trace::set_video_write_observer`
  (`gba_vram_trace.h`, default null) receives writer PC, destination, byte
  count, DMA source and channel for every CPU write into palette RAM or VRAM and
  once per DMA transfer into either. Payload-free.
- **Shared field scene and GPU field renderer.** `src/field/` holds the one
  field scene model, GPU field renderer, spell sparks, bursts and battle view
  both games compile. Each game's `src/field_game_profile.cpp` supplies the
  addresses and switches rules that exist in one game only
  (`field_game_profile.h`). Room-grid wrapping is a profile choice shared by the
  CPU room queries and the GPU shader.
- **Margin layers.** Margin layers can expose validated repeat dimensions
  without replacing the native map. `SceneMarginLayer` has `scroll_sides`
  (default false; the per-row margin scroll table then also positions pixels
  beside the native window) and `rest_clip_y` (default zero; a nonzero
  game-supplied offset feeds the renderer's existing `u_rest_dy` room-silhouette
  clip). The margin repeat width continues horizontal-only room patches at both
  ends of the room, outside the native window, when the game gives a nonzero
  measured repeat.
- **Repeating-layer margins.** `field/scene_repeat.h` (header only):
  `find_layer_repeat` finds a regular layer's whole-screen repeat from its tile
  map (each shown row equals the row `period` further down, shifted at most one
  period sideways, at least two periods shown; one-tile layers ignored);
  `continue_layer_repeat` fills margin scroll rows from the nearest shown
  period; `continue_repeating_layers` applies it to every layer no game rule
  claimed, except room scenery the room source fills. It needs the profile's
  `margin_scroll_rows`. `tools/scene_repeat_bench.cpp` is an offline bench over
  saved captures (built per game; not in CMake).
- **Sprite-effect continuation.** `SceneMargins` can supply a two-axis
  continuation period per object, outside the raw `FieldScene` capture format.
  The renderer draws translated copies with the same artwork, animation, depth
  and blend state, clipped away from the original 240x160 window. Parked and
  menu sprites and object-window masks are not extended. (The earlier
  `object_position` mechanism was removed; games use the normal position
  provider.)
- **GPU surface** (`runtime/gpu_surface.{h,cpp}`). A game-agnostic OpenGL helper
  with no window or context of its own: it attaches to the context the host
  already runs and saves and restores every piece of GL state it touches.
  Shaders, textures (8-bit, RGBA, 16-bit integer), an offscreen target, batched
  textured quads; entry points load through `SDL_GL_GetProcAddress`. `init()`
  reports failure honestly and the caller keeps its previous renderer. Also:
  `create_program` pins `a_pos`, `a_uv`, `a_depth` and `a_tint` to attribute
  slots 0-3; a depth renderbuffer (`set_depth_enabled`, `clear_depth`) that is
  resized whenever the target changes (`resize_depth_to_target()`, because
  drivers silently clip to the overlap of mismatched attachments);
  `bind_color_target(texture)`; `set_blend_mode(GpuBlendMode::Off/Alpha/
  Additive)` (`begin_frame`/`end_frame` save and restore the blend function and
  `GL_DEPTH_WRITEMASK`); texture requests above `GL_MAX_TEXTURE_SIZE` are
  rejected and level-0 storage is verified; uniform location/value caching.
- **Delayed GPU readback.** One shared `TextureReadbackQueue` does fenced PBO
  copies for both games, carrying owned game captures and `HistoryFrame` state
  with each picture; failed reads and failed frames use matched fallback state.
  The 50 ms fence-wait ceiling remains, so this is overlap, not guaranteed
  non-blocking transfer. Savestates discard queued identity (known issue: a
  state loaded into a larger view than it was made in can miss graphics). A
  presentation shutdown hook releases each game's renderer, fences and PBOs
  before `HostWindow` destroys its context.
- **Screen filters** (`runtime/screen_filter.{h,cpp}`). LCD3x, xBR, CRT Lottes
  and ScaleFX (multi-stage) GLSL filters at the final present on the OpenGL
  renderer, behind `GSR_SCREEN_FILTERS` (set by every launcher except under
  Direct3D). Nothing runs when it is off.
- **Release badge and cover cartridge.** The host reads `edition_badge.txt`
  (caption and ASCII flourish) and `edition_badge.bmp` beside the executable;
  when both exist F1 reserves space at the bottom right for them.
  `runtime/cartridge_art.h` holds the launcher's cartridge glow and idle
  animation as plain draw pieces (ImGui in the launcher, SDL in the engine);
  when `GSR_COVER_CARTRIDGE` names a 32-bit BMP (`GSR_COVER_CARTRIDGE_GAME` sets
  the glow colour), `draw_screen_veil` draws it centred over the black cover.

### Audio and music mods

- **Music mods** (`runtime/host_local_music.{h,cpp}`, `runtime/local_music_m4a.h`):
  WAV (16-bit PCM) or FLAC recordings, decoded with dr_flac
  (`third_party/dr_libs`, pinned commit d422edf, fetched by CMake when no
  local copy exists), play in place of a game's songs. `GBARECOMP_MUSIC_DIR`
  lists folders like PATH; folder i's album track NN (file
  `<disc>-<NN> <title>.flac|.wav`) is track i * `kLocalMusicFolderTracks` + NN.
  Each folder's `loops.txt` (`<track> <from> <to>` in file frames, or
  `<track> once`) sets the loop jump, applied before resampling so the
  stream stays continuous; no entry repeats the whole file. The first quarter
  second of every recording is decoded at startup, the rest streams from a
  worker thread. Missing or unreadable files keep the game's own music.
- `local_music_m4a.h` lets a game hand one m4a music player to those
  recordings: the game supplies the player's addresses, its song table and
  the two gain-saturation sites (a branch and `MOVS #255`); while a recording
  plays, that player's voices get gain 0, so the song keeps its timing, fades
  and pauses but is silent. The `MusicMod` option in `game_options.ini` is
  read live; F1 > Audio has "Enable Music Mod", greyed out unless
  `local_music_available()`.
- **Audio core** (`gba/gba_audio.*`). Canonical GBA audio with stereo output.
  The experimental native MP2K/shadow/wall mixers and independent Turbo audio
  were removed.
- **Volume and Mute are saved.** `config.ini` `[Audio]` holds `Volume=0..100`
  and `Mute=true|false`, written whenever they change (menu or Volume Up/Down
  hotkeys). `--volume` is applied only when actually given, and then wins for
  that run.

### Host window, input, hotkeys and menus

- **F1 preferences.** `host_window.cpp` reads and writes `[Video]` window
  scale, fullscreen, V-Sync, linear/integer scaling, screen filter and FPS
  display in `config.ini`, preserving other sections. Filter preferences
  survive launches that cannot enable filters.
- **Game-owned hotkeys.** `runtime/game_hotkeys.h` lets a game register extra
  actions before the host is created. `host_window.cpp` appends their names,
  labels and defaults after the 15 built-in actions, using the standard
  capture, keyboard/modifier/controller dispatch and INI persistence;
  `host_config_ui.h` sizes its binding vectors to the registered count. Games
  without registrations keep the 15.
- **Cheat/game menu hooks.** Rebindable hotkey `HK_GAME_MENU` (`[KeyMap]
  CheatMenu=`, "Cheat Menu" in F1 Hotkeys, default F11) sets
  `HostWindow::Events::game_menu`. Three `RunOptions` hooks, all null by
  default: `game_menu_toggle` (on that press), `keyinput_filter` (sees every
  pump's keys before the guest and input recording, returns what they get) and
  `paused_overlay` (paints over a copy of the frozen frame on each pump of the
  paused loop). The game requests the pause through `pause_request_poll`.
  Infinite HP/PP set by game code (`runtime_set_infinite_hp/pp`) are saved to
  `[Cheats]`.
- **Two bindings per gameplay button.** F1 Controls offers primary and
  alternate bindings per GBA button (`keybinds.ini` `[player1_pad]` plus
  `[player1_pad_alt]`). Input ORs both sources before KEYINPUT, with the same
  stick dead zone and sector rules. Capture conflicts remove from both columns;
  clear affects one column; defaults clear alternates.
- **Shared controls.** When `GBARECOMP_CONTROLS_DIR` is set, `HostWindow` reads
  and writes shared `keybinds.ini` and `config.ini` `[KeyMap]`/`[KeyMap.Pad]`
  there, leaving other settings per game. The first game launched seeds missing
  shared files from its legacy bindings.
- **Soft Reset.** F1's Hotkeys page has an initially unbound `SoftReset`
  action (keyboard/controller, shared `[KeyMap]`/`[KeyMap.Pad]`). The runtime
  flushes saves and exits with the action code from `soft_reset.h`; on Windows
  the collection restarts the same game; on Linux the running launcher process
  requeues its ROM and clears the Seamless slot variable, so a reset after a
  hand-over boots the second game without repeating an import.
- **Game settings page and No Slowdown.** Games can add controls to F1's Game
  page through `config_ui_set_game_page`. `kOverclockFactors`
  (`host_config_ui.h`) is {1, 50}: On (the game's "No Slowdown") allows up to
  50x GBA cycles per frame; saved values above 1 map to On.
- **Settings requested by game code.** A walking speed set by game code is
  saved: `Backend::persisted_player_speed` records what `config.ini` last held,
  and a differing live multiplier (`runtime_get_mem_write_override_enabled`)
  outside the F1 menu makes the next pump write `[Cheats]`.
  `runtime_request_view_mode(mode)` queues a fixed view mode that the next
  `HostWindow::pump` applies through the F1 menu's own path (view-mode event and
  `config.ini` write), then publishes the mode in effect
  (`runtime_publish_view_mode` / `runtime_get_view_mode`).
  `runtime_request_no_slowdown(on)` switches Enhanced Timing (where the game
  allows it) and CPU Overclock together, published via
  `runtime_get_no_slowdown`.
- **One window for both games.** `HostWindow::give_window` makes the next
  `open()` use a window the player program already has;
  `keep_window_on_close` makes `close()` leave it open and `take_kept_window`
  hands it back; a given window is never destroyed by the engine. End-of-game
  jobs register through `at_game_exit` (`armv4t/game_exit.h`) instead of
  `std::atexit`; a plug-in's player program calls `run_game_exit()`.
- **Games as plug-ins (Windows).** Each game's engine builds as a DLL
  (`GoldenSunEngine.dll`, `TLAEngine.dll`; CMake targets keep the names
  `GoldenSunRecomp`/`TLARecomp`) exporting `gsr_game_main(argc, argv,
  window_in, window_out)` from `runtime/game_module.cpp`;
  `runtime/player_main.cpp` is the program under the old exe name (16 MB stack)
  that loads it. Linux is unchanged (engine = program).
- **Seamless hand-over.** `runtime/handover.h` uses a named pipe
  (`\\.\pipe\gsr-handover-<launcher pid>`, `GSR_HANDOVER_LAUNCHER`) so a game
  ending with a Seamless code keeps the window and the second game's launch is
  sent to the waiting program instead of starting a new exe. Any failure falls
  back to a separate start.
- **Skip the ROM check.** With `GSR_ANY_ROM` on (`armv4t/any_rom.h`), `run_game`
  logs a ROM SHA-1 mismatch and goes on instead of refusing to start.
- **Speed-state logging.** `HostWindow` logs the guest frame plus
  held/toggle/active fast-forward state on source transitions.

### Saves and ROM data

- **Flash chip IDs.** `GbaSave::configure_flash` reports Macronix MX29L512
  (`0xC2`/`0x1C`) for a 64 KB chip, because the 512 Kbit-only SDK driver does
  not list the 1 Mbit ID `0x09` and found no usable chip; 1 Mbit chips still
  report MX29L010 (`0xC2`/`0x09`).
- **Paused saved-slot updates.** `update_flash_save` replaces a verified flash
  snapshot, rejecting stale data and unfinished commands, backing up all slots
  before writing, using the normal save persistence path and restoring the
  in-memory image on failure. No game addresses or import rules in the engine.
- **Game-owned ROM patch hook.** `RunOptions::rom_patch`,
  `void (*)(std::vector<std::uint8_t>*)`, is called once in `runtime.cpp`
  immediately before `bus.set_rom(...)`, after the SHA-1 check, so patched
  bytes never fail verification. The game may append data past the original end
  (served at `0x08000000 + N` and its mirrors; stay within 16 MB). It is for
  data read at run time; code and literal pools are built into translated code
  and must not be patched here.
- **Value overrides installed at startup.**
  `RunOptions::thumb_alu_immediate_override` and `thumb_literal_override`
  (default null) are installed after the ROM-identity check, under the same
  gate as `function_entry_observer`, so reviewed override sites work in every
  view mode. Null keeps each generated site at its original value.

### Crash reports, F12 captures and diagnostics

- **Shared crash-state writer.** `memory_snapshot.cpp` owns
  `gsr_install_crash_snapshot_writer` (callback for `crash_memory.bin` and, with
  the F1 Crash log option, `crash_trail.csv`); both runners install it right
  after `crash_handler_install`. It keeps the GSRMEM1 serialization and a
  20000-instruction tail.
- **Event-time context.** A shared helper writes `crash_context.txt` /
  `capture_context.txt` with live frame, CPU, CPSR, resume PC, outstanding guest
  returns and diagnostic build time (the returns are not a full backtrace). The
  F12 capture write path adds timing, scene/held/GPU availability and file-write
  completeness to `capture_info.txt`, and `index.csv` appends `image_written`.
  Failed saves keep partial evidence, log their folder and return failure;
  memory and CSV tail writers check stream and close errors.
- **Host profiler** (`runtime_bus_bridge.cpp`). While the target is suspended
  only registers and phase/frame atomics are read; the earlier in-process
  Windows x64 caller unwind (it could block behind the loader lock) was removed.
  Direct-function/phase/frame samples remain opt-in; dumps mark caller tracing
  unavailable.
- **Other diagnostics.** `gba/gba_vram_trace.cpp` (tile and text tracing); an
  opt-in `GSR_OBJ_RECORD` guest-frame marker drawn into a presentation copy for
  sprite diagnostics; audio-DMA and unmapped-access logging flags cached on
  first use (`gba_io.cpp`, `gba_bus.cpp`); a generic audio-bridge probe;
  `GBARECOMP_DEMO_INPUT` modes `walk`, `campaign` and the menu masher.
- **Environment switches** (`armv4t/env_flag.h`). On/off switches share one
  parser: empty, `0`, `false`, `off` and `no` are off, anything else is on.

### Game hooks (what a game can install)

All null or off by default and installed by the game's runner. `RunOptions`:
`function_entry_observer`, `thumb_alu_immediate_override`,
`thumb_literal_override`, `swi_override`, `rom_patch`, `game_menu_toggle`,
`keyinput_filter`, `paused_overlay`, `pause_request_poll`,
`no_bios_by_default`, the frame-present override. Globals and setters:
`g_runtime_fast_iwram_write_observer`, `g_runtime_ram_fallback_probe`,
`g_runtime_unmatched_return_hook`, `g_runtime_bios_open_bus_hook`,
`g_game_ram_layout`, the RAM-image resolver, the conditional-branch hook,
`vram_trace::set_video_write_observer`, `gba::g_hblank_dma_latch`,
`runtime_request_view_mode`, `runtime_request_no_slowdown`,
`runtime_set_infinite_hp/pp`, `runtime_set_party_layout`. Also
`game_hotkeys.h`, `config_ui_set_game_page`, the field game profile,
`SceneMargins` providers and `gbarecomp::guest_call` / `guest_return_now`.

### Build and tests

- Every translated game file includes `armv4t/runtime_arm.h`, so editing it
  rebuilds all generated code in both games; any engine change rebuilds both.
- `GBARECOMP_LINK_BIOS` (above); the `third_party/dr_libs` fetch;
  `runtime/host_local_music.{h,cpp}` belongs to `gbarecomp_runtime`.
- GCC 15+ is wanted for the guaranteed tail jumps; Linux packaging selects and
  bundles it.
- `scripts/build_dev.ps1` runs 20 engine tests before each developer game
  build. Suites under `tests/` cover the areas above; `ram_heal_test.cpp`,
  `ram_overlay/registry_test.cpp`, `ppu_smoke/test_main.cpp` and
  `armv4t/pool_ldm_probe_test.cpp` were updated to current contracts.

### Removed from upstream

- Upstream's Pokemon FireRed widescreen experiment: `src/debug/ws_sidecar.*`,
  `src/debug/ws_provenance.*`, their `GBARECOMP_WS_SC_*`/`GBARECOMP_WS_PROBE_*`
  options and `docs/WIDESCREEN_STEPC_PLAN.md`. They were armed only by FireRed
  program counters. `GBARECOMP_WS_WIP` is kept.
- Upstream's Mega Man Zero demo-input tracks (`campaign-combat`, `-traverse`,
  `-safe`, `-clear`) and `GBARECOMP_BOSS_STRATEGY`.
- The `GBARECOMP_AUDIO_PROBE` entry/bridge register traces (they hard-coded
  Golden Sun sound-routine addresses).
- `runtime/heal_gate.{h,cpp}` and `tests/heal_gate/` (an unused gate, archived),
  with `gbarecomp_heal_gate` in `CMakeLists.txt` and the `GbaBus`
  `BusWriteObserver` hook.
- `PRINCIPLES.md` references to upstream's Minish Cap and template paths were
  replaced with this collection's equivalents.

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
