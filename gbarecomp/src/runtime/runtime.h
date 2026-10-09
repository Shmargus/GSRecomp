// runtime.h — STUB. Glue that game binaries link against.
//
// Owns lifecycle: ROM load + hash verify, BIOS load, CPU + bus + PPU
// init, scheduler start, debug server start, main loop. The generated
// C from gba_recompile expects this header to provide the dispatch
// entry points and host-platform helpers it calls into.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gbarecomp {

// Optional host tooling, polled only by the ordinary runner at clean outer
// dispatch boundaries. Game adapters own IPC/authentication and snapshot paths.
enum class ToolingCommand { Play, Pause, Step, SaveCheckpoint, RestoreCheckpoint };
enum class ToolingPhase { Idle, Acknowledged, Completed, Ended };
struct ToolingRequest {
    std::uint64_t id = 0;
    ToolingCommand command = ToolingCommand::Pause;
    std::string checkpoint_path;
};
struct ToolingState {
    std::uint64_t frame = 0;
    bool paused = false;
    bool stepping = false;
    bool menu_paused = false;
};

// Per-game built-in defaults baked into a game runner at compile time.
// Lets a standalone release .exe (e.g. MinishCapRecomp.exe) ship
// without a sibling game.toml — the runtime falls back to these
// values when no TOML is found and no CLI override is supplied.
//
// All fields are optional. A null pointer / 0 means "no built-in" and
// the runtime keeps whatever it would have used otherwise (BIOS
// constants from GbaBios, empty ROM hash that forces the user to
// provide --config or --rom-sha1).
struct RunOptions {
    const char*   builtin_game_name = nullptr;
    const char*   builtin_rom_sha1  = nullptr;
    std::uint32_t builtin_rom_crc32 = 0;

    // True: run in no-BIOS mode unless `--bios <path>` is given on the
    // command line (GBARECOMP_NO_BIOS still overrides either way). False
    // keeps the BIOS-backed default. A build without the recompiled BIOS
    // (GBARECOMP_LINK_BIOS off) always runs without one.
    bool no_bios_by_default = false;

    // Optional read-only game observer for exact, hash-gated function-entry
    // research. Installed only when the loaded ROM SHA-1 equals
    // builtin_rom_sha1, and chained with the runtime's existing debug hook.
    // It must never alter guest state; nullptr keeps the normal fast path.
    void (*function_entry_observer)(std::uint32_t entry_pc) = nullptr;

    // Optional game-owned value overrides for the exact THUMB ALU-immediate
    // and literal-load sites the game's recompiler config opts in. Installed
    // at startup under the same ROM-identity gate as function_entry_observer;
    // nullptr leaves every generated site with its original value.
    int (*thumb_alu_immediate_override)(std::uint32_t instruction_pc,
                                        std::uint32_t original_value,
                                        std::uint32_t* out_value) = nullptr;
    int (*thumb_literal_override)(std::uint32_t instruction_pc,
                                  std::uint32_t original_value,
                                  std::uint32_t* out_value) = nullptr;
    int (*conditional_branch_override)(std::uint32_t instruction_pc,
                                       std::uint32_t original_decision,
                                       std::uint32_t* out_decision) = nullptr;
    // Optional game-owned SWI skip (runtime_arm.h RuntimeSwiOverride),
    // installed under the same ROM-identity gate.
    int (*swi_override)(std::uint32_t return_pc,
                        std::uint32_t swi_num) = nullptr;

    // Optional game-owned pause request, polled once per host input pump
    // (same cadence as the Pause hotkey). Lets game-owned tooling (e.g. a
    // function-call tracer) drive the SAME host_paused gate the hotkey
    // uses, instead of adding a second, competing pause path. Return true
    // and write the desired paused state to *out_paused to request a
    // change; return false to make no request this pump. nullptr keeps the
    // normal fast path (one null-pointer check).
    bool (*pause_request_poll)(bool* out_paused) = nullptr;

    bool (*tooling_active)() = nullptr;
    bool (*tooling_poll)(ToolingRequest* request) = nullptr;
    void (*tooling_status)(const ToolingState& state, std::uint64_t request_id,
                           ToolingPhase phase, const char* error) = nullptr;
    bool (*tooling_menu_paused)() = nullptr;

    // A game-owned menu drawn by the game over the paused picture (Golden
    // Sun's cheat menu). game_menu_toggle runs when the Cheat Menu hotkey
    // is pressed; the game then asks for the pause itself through
    // pause_request_poll. keyinput_filter sees each pump's keys before the
    // guest and returns what the guest gets, so a menu can take the keys
    // and keep them from the game. paused_overlay paints over a copy of the
    // frozen frame on every pump while the host is paused. All nullptr by
    // default, which leaves the run loop exactly as it was.
    void (*game_menu_toggle)() = nullptr;
    std::uint16_t (*keyinput_filter)(std::uint16_t keyinput) = nullptr;
    void (*paused_overlay)(std::uint8_t* rgb, std::uint32_t width,
                           std::uint32_t height) = nullptr;

    // Extended view is a game-owned enhancement capability, not a generic
    // emulator toggle. Values above the native 240x160 are the opt-in; the
    // default therefore makes stale config/environment settings inert.
    std::uint16_t max_view_width = 240;
    std::uint16_t max_view_height = 160;

    // A separate extended-view policy for games whose logical width follows
    // the live host-window aspect ratio. This ceiling is deliberately not
    // max_view_width: opting into resize-driven view does not also authorize
    // fixed --view-width modes. The launcher/CLI must explicitly opt in with
    // --resize-view; an accompanying --view-width may seed the initial
    // windowed aspect, while adaptive fullscreen follows the host display.
    std::uint16_t max_resize_view_width = 240;
    std::uint16_t max_resize_view_height = 160;
    bool resize_driven_view = false;

    // Optional game-owned view callback. Called after startup and every fixed
    // or adaptive geometry transition with all four margins; native mode
    // passes zero margins so a game can disable its expanded content path.
    void (*extended_view_init)(std::uint32_t extra_left,
                               std::uint32_t extra_right,
                               std::uint32_t extra_top,
                               std::uint32_t extra_bottom) = nullptr;

    // Optional game-owned ROM patch. Called once after the ROM is loaded and
    // its SHA-1 verified, before the bus sees it. It may change bytes and may
    // GROW the vector (data appended past the original end is served at
    // 0x08000000 + offset; keep the total within 16 MB). For data the game
    // reads at run time only (tables, text), never code or literal pools,
    // because the translated code has those built in.
    void (*rom_patch)(std::vector<std::uint8_t>* rom) = nullptr;

    // ---- pre-boot launcher identity (launcher_seam.h, RECOMP_LAUNCHER builds) --
    // Consumed by the recomp-ui launcher seam a game's main() runs BEFORE
    // run_game(); the runtime itself never reads these. All optional.
    const char* launcher_region = nullptr;      // display region, e.g. "USA"
    // The game's default game.toml path (GBARECOMP_DEFAULT_GAME_CONFIG). The
    // seam reads its [rom].path / [bios].path to PREFILL the launcher when no
    // rom.cfg / bios.cfg sidecar exists yet, so a first run isn't blank.
    const char* launcher_game_config = nullptr;
    const char* launcher_save_path = nullptr;   // explicit save file (game.toml
                                                // [save].path); null => <rom>.sav
                                                // derived from the seeded ROM
    // Keep an implemented extended-view mode out of the public launcher while
    // it is still being profiled. Explicit CLI/TOML opt-ins remain available.
    // Defaults true so existing games (including MMZ) retain today's UI.
    bool launcher_expose_widescreen = true;
    // Show recomp-ui's Adaptive view toggle for this game. Runtime support
    // (resize_driven_view + max_resize_view_width) is necessary but not
    // sufficient: every title must explicitly opt into the launcher surface
    // after its live-resize presentation has been validated.
    bool launcher_expose_adaptive_view = false;
    // Allow the in-game F1 menu to expose verified 2x scene interpolation.
    // Default false keeps unsupported games and acceptance builds faithful.
    bool frame_interpolation_available = false;
    // Allow the in-game menu to select exact 60/120 Hz host pacing. This is
    // presentation-only, default-off, and forcibly unavailable in strict runs.
    bool enhanced_timing_available = false;
    // Opt-in host-side native presentation compositor. Forced off for
    // strict/static and framebuffer-capture runs.
    bool native_renderer_available = false;
    // >240/160 offers the launcher's fixed expanded-view toggle, mapped to
    // --view-width/--view-height when enabled. 0/240/160 = no expanded
    // surface shown. Games with MULTIPLE extended widths/heights use the
    // aspect vocabulary below instead (takes precedence when set).
    std::uint16_t widescreen_view_width = 0;
    std::uint16_t widescreen_view_height = 0;
    // Game-supplied aspect vocabulary for the launcher's aspect cycle
    // (EXPERIMENTAL-tagged). labels/view_widths/view_heights are parallel
    // arrays of num_aspects entries; index 0 must be the native 240x160 view.
    // The committed index maps to --view-width/--view-height at that index.
    // e.g. Mega Man Zero: {"3:2 (Native)","9:5 (288 px)","12:5 (384 px)",
    // "6:2 (480 px)"} / {240, 288, 384, 480}.
    const char* const*   launcher_aspect_labels = nullptr;
    const std::uint16_t* launcher_aspect_view_widths = nullptr;
    const std::uint16_t* launcher_aspect_view_heights = nullptr;
    int                  launcher_num_aspects = 0;
    // Box-art image path relative to the assets dir staged next to the exe;
    // null => the launcher's default "assets/img/boxart.tga". Multi-variant
    // repos stage one file per variant (e.g. "assets/img/boxart_firered.tga").
    const char* launcher_boxart = nullptr;
};

int run_game(int argc, char** argv, const RunOptions& opts = {});

}  // namespace gbarecomp
