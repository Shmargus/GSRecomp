cmake_minimum_required(VERSION 3.20)
project(gbarecomp C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W3 /permissive- /Zc:__cplusplus)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter
        -ffunction-sections -fdata-sections)
endif()

# ─────────────────────────────────────────────────────────────────────────────
# SDL2 — soft dependency. When present we build host_window.cpp with the
# real SDL backend; otherwise it compiles as a no-op stub. CI and headless
# environments stay buildable either way.
#
# Avoid `find_package(SDL2 CONFIG)` on MSYS2: that target's
# INTERFACE_INCLUDE_DIRECTORIES is `-isystem /mingw64/include`, which
# breaks libstdc++'s `#include_next <stdlib.h>` chain.
# Avoid `C:/...` HINTS too: CMake-on-MSYS records them but mangles the
# emitted -I flag with the build dir prefix.
# ─────────────────────────────────────────────────────────────────────────────
set(GBARECOMP_SDL2_OK FALSE)
set(GBARECOMP_MINGW_PREFIX_UNIX "/c/msys64/mingw64" CACHE PATH
    "MSYS2 mingw64 install root, in Unix form, used for finding SDL2")

# Release builds bake SDL2 + libstdc++ + libgcc + libwinpthread into
# the executable so the shipped .exe has no DLL dependencies on a
# vanilla Windows install. Dev / debug builds keep the dynamic SDL2
# link so iteration is fast.
option(GBARECOMP_STATIC_RELEASE
    "Statically link SDL2 + the mingw C++ runtime into release exes"
    OFF)

# First-divergence co-simulation oracle (COSIM_ORACLE.md). When ON, compiles the
# cosim full-state hash + cycle-keyed park/step TCP engine and arms the runtime_tick
# checkpoint hook. Defined PUBLIC on gbarecomp_debug so it propagates through
# gbarecomp_runtime (runtime.cpp / runtime_bus_bridge.cpp) to the game exe. The
# force-interp backend (g_force_interp / GBARECOMP_FORCE_INTERP) is always compiled
# and independent of this option. OFF = byte-identical to a normal build.
option(GBA_COSIM
    "Build the first-divergence co-simulation oracle (state hash + TCP park/step)"
    OFF)

find_path(SDL2_INCLUDE_DIR
    NAMES SDL.h
    PATH_SUFFIXES SDL2 SDL2/include
    HINTS ${GBARECOMP_MINGW_PREFIX_UNIX}/include
)
if(GBARECOMP_STATIC_RELEASE)
    # Pin the static archives directly. CMake's find_library would
    # prefer libSDL2.dll.a on a default MSYS2 install. Try the Unix
    # form first (works under msys2 shells), fall back to the native
    # Windows path (works under PowerShell / cmd).
    set(SDL2_STATIC_LIB_CANDIDATES
        ${GBARECOMP_MINGW_PREFIX_UNIX}/lib/libSDL2.a
        C:/msys64/mingw64/lib/libSDL2.a)
    set(SDL2_LIBRARY "")
    foreach(_cand IN LISTS SDL2_STATIC_LIB_CANDIDATES)
        if(EXISTS "${_cand}")
            set(SDL2_LIBRARY      "${_cand}")
            string(REPLACE "libSDL2.a" "libSDL2main.a"
                   SDL2_MAIN_LIBRARY "${_cand}")
            break()
        endif()
    endforeach()
    if(SDL2_LIBRARY STREQUAL "" OR NOT EXISTS "${SDL2_MAIN_LIBRARY}")
        message(FATAL_ERROR
            "GBARECOMP_STATIC_RELEASE=ON but libSDL2.a / libSDL2main.a "
            "not found. Searched: ${SDL2_STATIC_LIB_CANDIDATES}")
    endif()
    # SDL2 static depends on a list of Windows system libs that
    # libSDL2.dll already pulls in transitively when dynamic.
    # (From `pkg-config --static --libs sdl2`.)
    # Use full paths to libSDL2.a + libSDL2main.a — `-lSDL2` would let
    # the linker prefer libSDL2.dll.a (dynamic import) over libSDL2.a.
    set(SDL2_STATIC_SYSDEPS
        -lmingw32
        ${SDL2_MAIN_LIBRARY}
        ${SDL2_LIBRARY}
        -lm
        -lkernel32 -luser32 -lgdi32 -lwinmm -limm32 -lole32 -loleaut32
        -lversion -luuid -ladvapi32 -lsetupapi -lshell32 -ldinput8
        -mwindows)
else()
    find_library(SDL2_LIBRARY
        NAMES SDL2
        HINTS ${GBARECOMP_MINGW_PREFIX_UNIX}/lib
    )
endif()

if(SDL2_INCLUDE_DIR AND SDL2_LIBRARY)
    set(GBARECOMP_SDL2_OK TRUE)
    set(SDL2_INCLUDE_DIRS ${SDL2_INCLUDE_DIR})
    if(GBARECOMP_STATIC_RELEASE)
        set(SDL2_LIBRARIES ${SDL2_STATIC_SYSDEPS})
    else()
        set(SDL2_LIBRARIES ${SDL2_LIBRARY})
    endif()
    message(STATUS "gbarecomp: SDL2 found — inc=${SDL2_INCLUDE_DIR} lib=${SDL2_LIBRARY} static=${GBARECOMP_STATIC_RELEASE}")
else()
    message(STATUS "gbarecomp: SDL2 NOT found — host_window will stub out (inc=${SDL2_INCLUDE_DIR} lib=${SDL2_LIBRARY})")
endif()

if(GBARECOMP_STATIC_RELEASE)
    # NOTE: -static / -static-libgcc / -static-libstdc++ MUST be set
    # via -DCMAKE_EXE_LINKER_FLAGS at configure time (see package_release.ps1).
    # add_link_options() here would only apply to targets defined in
    # gbarecomp's subdir, not to a parent project that includes us via
    # add_subdirectory() (e.g. MinishCapRecomp). The cache var works
    # across the whole tree.
endif()

# ─────────────────────────────────────────────────────────────────────────────
# armv4t — portable ARM7TDMI decoder, IR, codegen. Reusable for ARM9 later.
# Must NOT depend on src/gba or src/runtime.
# ─────────────────────────────────────────────────────────────────────────────
add_library(gbarecomp_armv4t STATIC
    src/armv4t/arm_decode.cpp
    src/armv4t/thumb_decode.cpp
    src/armv4t/arm_ir.cpp
    src/armv4t/arm_codegen.cpp
    src/armv4t/runtime_arm.cpp
    src/armv4t/symbol_lookup.cpp
    src/armv4t/interpreter.cpp
)
target_include_directories(gbarecomp_armv4t PUBLIC src/armv4t)
set_property(TARGET gbarecomp_armv4t PROPERTY POSITION_INDEPENDENT_CODE ON)

# ─────────────────────────────────────────────────────────────────────────────
# recompile_core — function discovery + per-function codegen, as a LIBRARY.
# Extracted from tools/gba_recompile so the SAME emitter serves both the
# offline corpus build (the tool) and the Stage-2 runtime self-healing
# recompiler (a dispatch miss → emit one function → gcc → DLL). Depends only
# on armv4t (decoder/IR/codegen); must NOT depend on src/gba or src/runtime.
# ─────────────────────────────────────────────────────────────────────────────
add_library(gbarecomp_recompile_core STATIC
    src/recompile/function_finder.cpp
    src/recompile/emit_function.cpp
)
target_include_directories(gbarecomp_recompile_core PUBLIC src/recompile)
target_link_libraries(gbarecomp_recompile_core PUBLIC gbarecomp_armv4t)
set_property(TARGET gbarecomp_recompile_core PROPERTY POSITION_INDEPENDENT_CODE ON)

# ─────────────────────────────────────────────────────────────────────────────
# gba — GBA-specific bus, IO, IRQ, DMA, timers, PPU, audio, input, save, BIOS.
# Aggressively GBA-specific. Depends on armv4t for CPU types.
# ─────────────────────────────────────────────────────────────────────────────
add_library(gbarecomp_gba STATIC
    src/gba/gba_memory.cpp
    src/gba/gba_bus.cpp
    src/gba/gba_io.cpp
    src/gba/gba_irq.cpp
    src/gba/gba_dma.cpp
    src/gba/gba_timers.cpp
    src/gba/gba_ppu.cpp
    src/gba/gba_vram_trace.cpp
    src/gba/gba_audio.cpp
    src/gba/gba_m4a.cpp
    src/gba/audio_shadow.cpp
    src/gba/turbo_audio_scheduler.cpp
    src/gba/audio_event_capture.cpp
    src/gba/mp2k_wall_mixer.cpp
    src/gba/mp2k_wall_update_queue.cpp
    src/gba/mp2k_shadow.cpp
    src/gba/gba_input.cpp
    src/gba/gba_save.cpp
    src/gba/gba_rtc.cpp
    src/gba/gba_bios.cpp
    src/gba/gba_scheduler.cpp
    src/gba/gba_rom_header.cpp
    src/gba/sha1.cpp
    src/gba/crc32.cpp
)
target_include_directories(gbarecomp_gba PUBLIC src/gba)
# src/debug for snapshot.h — the save-state SnapshotWriter/Reader are
# header-only, so this is an include-path dependency only (gbarecomp_gba
# does NOT link gbarecomp_debug; that would invert the layering).
target_include_directories(gbarecomp_gba PRIVATE src/debug)
target_link_libraries(gbarecomp_gba PUBLIC gbarecomp_armv4t)
set_property(TARGET gbarecomp_gba PROPERTY POSITION_INDEPENDENT_CODE ON)

# ─────────────────────────────────────────────────────────────────────────────
# heal_gate — P6 differential-gate primitives (snapshot/journal/diff). PURE:
# depends only on gbarecomp_gba (GbaBus + ArmCpuState), no runtime globals, so
# it unit-tests standalone and the runtime gate state machine layers on top.
# ─────────────────────────────────────────────────────────────────────────────
add_library(gbarecomp_heal_gate STATIC src/runtime/heal_gate.cpp)
target_include_directories(gbarecomp_heal_gate PUBLIC src/runtime)
target_link_libraries(gbarecomp_heal_gate PUBLIC gbarecomp_gba)
set_property(TARGET gbarecomp_heal_gate PROPERTY POSITION_INDEPENDENT_CODE ON)

# Identity-aware dispatch registry for statically translated RAM overlays.
# Pure game-thread logic: game integrations install it behind
# g_runtime_ram_dispatch_hook and drive activation from verified load events.
add_library(gbarecomp_ram_overlay STATIC
    src/runtime/ram_overlay_registry.cpp)
target_include_directories(gbarecomp_ram_overlay PUBLIC src/runtime)
set_property(TARGET gbarecomp_ram_overlay PROPERTY POSITION_INDEPENDENT_CODE ON)

# ─────────────────────────────────────────────────────────────────────────────
# runtime — what game binaries link against. Dispatch, host glue.
# ─────────────────────────────────────────────────────────────────────────────
# ── Dear ImGui (host config UI) ──────────────────────────────────────────────
# Only meaningful with SDL2, since both backends are SDL-based. Same
# local-checkout-then-pinned-fetch shape as toml++ below, so an offline tree
# with third_party/imgui builds without touching the network.
#
# Deliberately confined to the HOST side: the UI never runs on the guest path,
# so an ImGui bug cannot perturb emulation. Guest frames are still uploaded to
# the same texture as before; ImGui draws on top at present time.
set(GBARECOMP_IMGUI_OK FALSE)
if(GBARECOMP_SDL2_OK)
    find_path(GBARECOMP_IMGUI_DIR
        NAMES imgui.cpp
        HINTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui"
        NO_DEFAULT_PATH)
    if(NOT GBARECOMP_IMGUI_DIR)
        include(FetchContent)
        FetchContent_Declare(imgui
            GIT_REPOSITORY https://github.com/ocornut/imgui.git
            GIT_TAG 126d004f9e1eef062bf4b044b3b2faaf58d48c51)  # v1.91.9-docking
        FetchContent_MakeAvailable(imgui)
        set(GBARECOMP_IMGUI_DIR "${imgui_SOURCE_DIR}")
        message(STATUS "gbarecomp: fetched pinned Dear ImGui v1.91.9-docking")
    else()
        message(STATUS "gbarecomp: using local Dear ImGui at ${GBARECOMP_IMGUI_DIR}")
    endif()

    add_library(gbarecomp_imgui STATIC
        ${GBARECOMP_IMGUI_DIR}/imgui.cpp
        ${GBARECOMP_IMGUI_DIR}/imgui_draw.cpp
        ${GBARECOMP_IMGUI_DIR}/imgui_tables.cpp
        ${GBARECOMP_IMGUI_DIR}/imgui_widgets.cpp
        ${GBARECOMP_IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
        ${GBARECOMP_IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp
        # imgui_impl_opengl3: real multi-viewport support (panels torn off
        # into their own OS windows). imgui_impl_sdlrenderer2 has none
        # upstream. Uses its own function loader — no GLEW/GLAD needed.
        ${GBARECOMP_IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
    target_include_directories(gbarecomp_imgui PUBLIC
        ${GBARECOMP_IMGUI_DIR}
        ${GBARECOMP_IMGUI_DIR}/backends
        ${SDL2_INCLUDE_DIR})
    target_link_libraries(gbarecomp_imgui PUBLIC ${SDL2_LIBRARY})
    find_package(OpenGL)
    if(OPENGL_FOUND)
        target_link_libraries(gbarecomp_imgui PUBLIC OpenGL::GL)
    elseif(WIN32 OR MSYS)
        # mingw's OpenGL find module can miss opengl32 in some MSYS2
        # layouts; the import lib is always present alongside the other
        # Windows system libs, so fall back to linking it by name.
        target_link_libraries(gbarecomp_imgui PUBLIC opengl32)
    endif()
    set_property(TARGET gbarecomp_imgui PROPERTY POSITION_INDEPENDENT_CODE ON)
    # Upstream code, not ours: do not fail our warning budget on it.
    if(NOT MSVC)
        target_compile_options(gbarecomp_imgui PRIVATE -w)
    endif()
    set(GBARECOMP_IMGUI_OK TRUE)
endif()

add_library(gbarecomp_runtime STATIC
    src/runtime/runtime.cpp
    src/runtime/bios_hle.cpp
    src/runtime/generated_dispatch.cpp
    src/runtime/runtime_bus_bridge.cpp
    src/runtime/arm_cpu_bridge.cpp
    src/runtime/overlay_emit.cpp
    src/runtime/overlay_loader.cpp
    src/runtime/overlay_compile.cpp
    src/runtime/host_platform.cpp
    src/runtime/crash_handler.cpp
    src/runtime/host_window.cpp
    src/runtime/host_overlay.cpp
    src/runtime/host_config_ui.cpp
    src/runtime/frame_timing.cpp
    src/runtime/frame_interpolation.cpp
    src/runtime/color_lut.cpp
    src/runtime/asset_picker.cpp
    # Production abort stubs for runtime_dispatch_miss +
    # runtime_unimplemented_op. Test binaries that link only
    # gbarecomp_armv4t (not gbarecomp_runtime) supply their own.
    src/runtime/runtime_arm_default_aborts.cpp
    src/runtime/self_heal_cluster.cpp
)

# The recompiled BIOS (`gba_recompile --bios`, locally generated and
# gitignored: bios_recompiled.cpp plus its bios_dispatch_table.cpp) is linked
# only when GBARECOMP_LINK_BIOS is on and the output exists. Otherwise the
# empty no_bios_dispatch_table.cpp stands in and the build contains no BIOS
# code at all; it then runs only in no-BIOS mode (runtime_arm.h).
option(GBARECOMP_LINK_BIOS
       "Link the locally generated recompiled BIOS (BIOS-backed runs)" ON)
if(GBARECOMP_LINK_BIOS AND
   EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/generated_bios/bios_recompiled.cpp" AND
   EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/generated_bios/bios_dispatch_table.cpp")
    target_sources(gbarecomp_runtime PRIVATE
        src/runtime/generated_bios/bios_recompiled.cpp
        src/runtime/generated_bios/bios_dispatch_table.cpp)
    target_compile_definitions(gbarecomp_runtime PUBLIC
        GBARECOMP_HAVE_BIOS_RECOMP=1)
    message(STATUS "gbarecomp: linking the recompiled BIOS")
else()
    target_sources(gbarecomp_runtime PRIVATE
        src/runtime/no_bios_dispatch_table.cpp)
    message(STATUS "gbarecomp: no BIOS code linked; no-BIOS mode only")
endif()
target_include_directories(gbarecomp_runtime PUBLIC src/runtime)
# Bake the gbarecomp source root so the Stage-2 runtime compiler can find the
# overlay shim headers (src/runtime + src/armv4t) when it invokes g++ or the
# bundled tcc on a healed function. CMAKE_CURRENT_SOURCE_DIR is the gbarecomp
# worktree root even
# when a game project (MinishCapRecomp) pulls us in via add_subdirectory.
target_compile_definitions(gbarecomp_runtime PRIVATE
    GBARECOMP_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(gbarecomp_runtime PUBLIC
    gbarecomp_gba gbarecomp_debug gbarecomp_recompile_core
    gbarecomp_heal_gate gbarecomp_ram_overlay)
set_property(TARGET gbarecomp_runtime PROPERTY POSITION_INDEPENDENT_CODE ON)
if(GBARECOMP_SDL2_OK)
    target_compile_definitions(gbarecomp_runtime PUBLIC GBARECOMP_HAVE_SDL2)
    # Use plain -I (not -isystem) for SDL2 headers. -isystem on a path
    # that overlaps libstdc++'s search ordering breaks
    # `#include_next <stdlib.h>`.
    target_include_directories(gbarecomp_runtime PUBLIC ${SDL2_INCLUDE_DIRS})
    target_link_libraries(gbarecomp_runtime PUBLIC ${SDL2_LIBRARIES})
    if(GBARECOMP_IMGUI_OK)
        target_compile_definitions(gbarecomp_runtime PUBLIC GBARECOMP_HAVE_IMGUI)
        target_link_libraries(gbarecomp_runtime PUBLIC gbarecomp_imgui)
    endif()
endif()
if(WIN32 OR MSYS)
    # asset_picker uses GetOpenFileNameA (comdlg32) and MessageBoxA (user32).
    # host_platform's FramePacer uses timeBeginPeriod/timeEndPeriod (winmm).
    target_link_libraries(gbarecomp_runtime PUBLIC comdlg32 user32 winmm)
endif()

# ─────────────────────────────────────────────────────────────────────────────
# debug — TCP server + always-on ring buffers + snapshot. Always built.
# ─────────────────────────────────────────────────────────────────────────────
add_library(gbarecomp_debug STATIC
    src/debug/tcp_debug_server.cpp
    src/debug/trace_ring.cpp
    src/debug/snapshot.cpp
    # First-divergence co-sim engine + full-state hash. Empty TUs unless
    # GBA_COSIM is defined (guarded internally), so safe in the normal build.
    src/debug/cosim.cpp
    src/debug/cosim_state.cpp
)
target_include_directories(gbarecomp_debug PUBLIC src/debug)
# cosim_state.cpp consults the active bus via runtime_bus_bridge.h.
target_include_directories(gbarecomp_debug PRIVATE src/runtime)
target_link_libraries(gbarecomp_debug PUBLIC gbarecomp_gba)
set_property(TARGET gbarecomp_debug PROPERTY POSITION_INDEPENDENT_CODE ON)
if(WIN32 OR MSYS)
    target_link_libraries(gbarecomp_debug PUBLIC ws2_32)
endif()
# Propagate GBA_COSIM PUBLIC so the cosim files here, the runtime_tick hook in
# gbarecomp_runtime, and runtime.cpp's cosim_init() all compile the oracle in.
if(GBA_COSIM)
    target_compile_definitions(gbarecomp_debug PUBLIC GBA_COSIM)
    message(STATUS "gbarecomp: GBA_COSIM ON — first-divergence oracle compiled in")
endif()

# ─────────────────────────────────────────────────────────────────────────────
# LTO — applied only to gbarecomp's own static libraries, driven by the
# parent project's GSR_ENABLE_LTO/GSR_IPO_SUPPORTED (set and IPO-checked in
# the top-level CMakeLists.txt before add_subdirectory(GBARECOMP_ROOT ...)).
# Both variables are unset/false when gbarecomp is configured standalone, so
# this is a no-op there. Deliberately excludes gbarecomp_imgui: that library
# is upstream Dear ImGui sources (third_party/imgui), not ours to LTO here.
# ─────────────────────────────────────────────────────────────────────────────
if(GSR_ENABLE_LTO AND GSR_IPO_SUPPORTED)
    foreach(GSR_LTO_TARGET IN ITEMS
            gbarecomp_armv4t gbarecomp_recompile_core gbarecomp_gba
            gbarecomp_heal_gate gbarecomp_ram_overlay gbarecomp_runtime
            gbarecomp_debug)
        set_property(TARGET ${GSR_LTO_TARGET} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endforeach()
endif()

# ─────────────────────────────────────────────────────────────────────────────
# tools — small CLI utilities. Each is a separate executable.
# ─────────────────────────────────────────────────────────────────────────────
function(target_link_gbarecomp_runtime_stack target_name)
    if(MINGW OR MSYS)
        target_link_libraries(${target_name} PRIVATE
            -Wl,--start-group
            gbarecomp_armv4t gbarecomp_gba gbarecomp_runtime gbarecomp_debug
            -Wl,--end-group)
    else()
        target_link_libraries(${target_name} PRIVATE
            gbarecomp_armv4t gbarecomp_gba gbarecomp_runtime gbarecomp_debug)
    endif()
endfunction()

add_executable(gba_scan tools/gba_scan/main.cpp)
target_link_libraries(gba_scan PRIVATE gbarecomp_armv4t gbarecomp_gba)

# A local toml++ checkout remains useful for offline development, but it is
# intentionally ignored.  Fresh clones/worktrees must still be able to build
# gba_recompile, so resolve the pinned upstream dependency into the build tree
# when the local single-header checkout is absent.
find_path(GBARECOMP_TOMLPP_INCLUDE_DIR
    NAMES toml.hpp
    HINTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tomlpp"
    NO_DEFAULT_PATH)
if(NOT GBARECOMP_TOMLPP_INCLUDE_DIR)
    include(FetchContent)
    set(TOMLPLUSPLUS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(TOMLPLUSPLUS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(tomlplusplus
        GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
        GIT_TAG 30172438cee64926dc41fdd9c11fb3ba5b2ba9de)
    FetchContent_MakeAvailable(tomlplusplus)
    set(GBARECOMP_TOMLPP_INCLUDE_DIR "${tomlplusplus_SOURCE_DIR}")
    message(STATUS
        "gbarecomp: fetched pinned toml++ v3.4.0 for gba_recompile")
else()
    message(STATUS
        "gbarecomp: using local toml++ at ${GBARECOMP_TOMLPP_INCLUDE_DIR}")
endif()

add_executable(gba_recompile
    tools/gba_recompile/main.cpp
    tools/gba_recompile/config.cpp
    tools/gba_recompile/slim_pass.cpp)
target_include_directories(gba_recompile PRIVATE
    tools/gba_recompile
    src/armv4t
    "${GBARECOMP_TOMLPP_INCLUDE_DIR}")
target_link_libraries(gba_recompile PRIVATE
    gbarecomp_recompile_core gbarecomp_armv4t gbarecomp_gba)
# Self-contained on MinGW: the Golden Sun player builder ships this exe
# and runs it on PCs without the MinGW runtime DLLs.
if(MINGW)
    target_link_options(gba_recompile PRIVATE -static)
endif()

add_executable(symbol_import tools/symbol_import/main.cpp)
target_link_libraries(symbol_import PRIVATE gbarecomp_armv4t)

# test_rom_runner — direct-boots a recompiled jsmolka gba-suite test
# ROM and reports r12 at exit. Two targets per ROM (ARM + THUMB):
# each ROM is recompiled at build time via gba_recompile and linked
# into its own runner exe. Output is the test_rom_runner main
# (shared) + the per-ROM generated body.

set(GBA_SUITE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/gba_suite)

# Recompile arm.gba at build time.
set(TEST_ROM_ARM_OUTDIR ${CMAKE_CURRENT_BINARY_DIR}/test_rom_arm_gen)
set(TEST_ROM_ARM_DISPATCH ${TEST_ROM_ARM_OUTDIR}/dispatch_table.cpp)
set(TEST_ROM_ARM_BODY     ${TEST_ROM_ARM_OUTDIR}/recompiled.cpp)
set(TEST_ROM_ARM_HEADER   ${TEST_ROM_ARM_OUTDIR}/recompiled.h)
file(MAKE_DIRECTORY ${TEST_ROM_ARM_OUTDIR})

if(EXISTS ${GBA_SUITE_ROOT}/arm/arm.gba)
    add_custom_command(
        OUTPUT  ${TEST_ROM_ARM_DISPATCH} ${TEST_ROM_ARM_BODY}
                ${TEST_ROM_ARM_HEADER}
        COMMAND $<TARGET_FILE:gba_recompile>
                --rom ${GBA_SUITE_ROOT}/arm/arm.gba
                --entry 0x080000C0
                --out ${TEST_ROM_ARM_OUTDIR}
        DEPENDS gba_recompile
                ${GBA_SUITE_ROOT}/arm/arm.gba
        COMMENT "Recompiling gba-suite arm.gba"
        VERBATIM
    )

    add_executable(test_rom_arm
        tools/test_rom_runner/main.cpp
        ${TEST_ROM_ARM_DISPATCH}
        ${TEST_ROM_ARM_BODY}
    )
    target_include_directories(test_rom_arm PRIVATE
        ${TEST_ROM_ARM_OUTDIR}
        src/armv4t)
    target_link_gbarecomp_runtime_stack(test_rom_arm)
    add_test(NAME test_rom_arm COMMAND test_rom_arm
        ${GBA_SUITE_ROOT}/arm/arm.gba)
else()
    message(STATUS "gbarecomp: gba-suite arm.gba not found; "
                   "skipping test_rom_arm target")
endif()

# Recompile thumb.gba at build time.
set(TEST_ROM_THUMB_OUTDIR ${CMAKE_CURRENT_BINARY_DIR}/test_rom_thumb_gen)
set(TEST_ROM_THUMB_DISPATCH ${TEST_ROM_THUMB_OUTDIR}/dispatch_table.cpp)
set(TEST_ROM_THUMB_BODY     ${TEST_ROM_THUMB_OUTDIR}/recompiled.cpp)
set(TEST_ROM_THUMB_HEADER   ${TEST_ROM_THUMB_OUTDIR}/recompiled.h)
file(MAKE_DIRECTORY ${TEST_ROM_THUMB_OUTDIR})

if(EXISTS ${GBA_SUITE_ROOT}/thumb/thumb.gba)
    add_custom_command(
        OUTPUT  ${TEST_ROM_THUMB_DISPATCH} ${TEST_ROM_THUMB_BODY}
                ${TEST_ROM_THUMB_HEADER}
        COMMAND $<TARGET_FILE:gba_recompile>
                --rom ${GBA_SUITE_ROOT}/thumb/thumb.gba
                --entry 0x080000C0
                --out ${TEST_ROM_THUMB_OUTDIR}
        DEPENDS gba_recompile
                ${GBA_SUITE_ROOT}/thumb/thumb.gba
        COMMENT "Recompiling gba-suite thumb.gba"
        VERBATIM
    )

    add_executable(test_rom_thumb
        tools/test_rom_runner/main.cpp
        ${TEST_ROM_THUMB_DISPATCH}
        ${TEST_ROM_THUMB_BODY}
    )
    target_include_directories(test_rom_thumb PRIVATE
        ${TEST_ROM_THUMB_OUTDIR}
        src/armv4t)
    target_link_gbarecomp_runtime_stack(test_rom_thumb)
    add_test(NAME test_rom_thumb COMMAND test_rom_thumb
        ${GBA_SUITE_ROOT}/thumb/thumb.gba)
else()
    message(STATUS "gbarecomp: gba-suite thumb.gba not found; "
                   "skipping test_rom_thumb target")
endif()

# bios_smoke — Phase 2.1 canary. Loads bios/gba_bios.bin, runs the
# interpreter from PC=0 for N instructions, prints a trace.
add_executable(bios_smoke tools/bios_smoke/main.cpp
                          tools/bios_smoke/dispatch_stub.cpp)
target_link_gbarecomp_runtime_stack(bios_smoke)
if(WIN32 OR MSYS)
    target_link_libraries(bios_smoke PRIVATE ws2_32)
endif()

# ─────────────────────────────────────────────────────────────────────────────
# tests — per-subsystem. Run with ctest.
# ─────────────────────────────────────────────────────────────────────────────
enable_testing()

add_executable(decoder_smoke tests/armv4t/decoder_smoke.cpp)
target_link_libraries(decoder_smoke PRIVATE gbarecomp_armv4t)
add_test(NAME decoder_smoke COMMAND decoder_smoke)

add_executable(function_finder_tests tests/recompile/function_finder_test.cpp)
target_link_libraries(function_finder_tests PRIVATE gbarecomp_recompile_core)
add_test(NAME function_finder_tests COMMAND function_finder_tests)

# Position-independent RAM code images (gba_recompile --relocatable-image).
add_executable(relocatable_image_tests
    tests/relocatable_image/relocatable_image_test.cpp)
target_link_libraries(relocatable_image_tests PRIVATE gbarecomp_recompile_core)
add_test(NAME relocatable_image_tests COMMAND relocatable_image_tests)

# Self-heal proposal clustering — pure logic, links only the standalone
# clustering TU (no runtime/bus/overlay dependencies).
add_executable(selfheal_cluster_test
    tests/selfheal/cluster_test.cpp
    src/runtime/self_heal_cluster.cpp)
target_include_directories(selfheal_cluster_test PRIVATE src/runtime)
add_test(NAME selfheal_cluster_test COMMAND selfheal_cluster_test)

add_executable(interpreter_smoke tests/armv4t/interpreter_smoke.cpp)
target_link_libraries(interpreter_smoke PRIVATE gbarecomp_armv4t)
add_test(NAME interpreter_smoke COMMAND interpreter_smoke)

# Stage-2 self-heal over MUTABLE (IWRAM/EWRAM) code: owned-snapshot compile +
# content-verified (CRC32) dispatch. Drives the real worker-thread compile
# pipeline (needs a real g++ — see gxx_path() in overlay_compile.cpp); no
# ROM/BIOS bytes, every function body is synthetic.
add_executable(ram_heal_tests
    tests/selfheal/ram_heal_test.cpp
    tools/bios_smoke/dispatch_stub.cpp)
target_link_gbarecomp_runtime_stack(ram_heal_tests)
add_test(NAME ram_heal_tests COMMAND ram_heal_tests)
set_tests_properties(ram_heal_tests PROPERTIES TIMEOUT 120)

add_executable(armv4t_tests tests/armv4t/test_main.cpp)
target_link_libraries(armv4t_tests PRIVATE gbarecomp_armv4t)
add_test(NAME armv4t_tests COMMAND armv4t_tests)

# CRASH-03 provenance: pool-LDM probe depth-8 write rings, savestate-epoch
# tags, and payload-free window CRC at arm + auth-epoch boundaries.
add_executable(pool_ldm_probe_tests
    tests/armv4t/pool_ldm_probe_test.cpp
    tools/bios_smoke/dispatch_stub.cpp)
target_link_gbarecomp_runtime_stack(pool_ldm_probe_tests)
add_test(NAME pool_ldm_probe_tests COMMAND pool_ldm_probe_tests)
set_tests_properties(pool_ldm_probe_tests PROPERTIES TIMEOUT 120)

add_executable(thumb_tests tests/thumb/test_main.cpp)
target_link_libraries(thumb_tests PRIVATE gbarecomp_armv4t)
add_test(NAME thumb_tests COMMAND thumb_tests)

add_executable(bus_tests
    tests/bus/test_main.cpp
    tools/bios_smoke/dispatch_stub.cpp)
target_include_directories(bus_tests PRIVATE src/debug)
target_link_gbarecomp_runtime_stack(bus_tests)
add_test(NAME bus_tests COMMAND bus_tests)

add_executable(dma_tests
    tests/dma/test_main.cpp
    tools/bios_smoke/dispatch_stub.cpp)
target_include_directories(dma_tests PRIVATE src/debug)
target_link_gbarecomp_runtime_stack(dma_tests)
add_test(NAME dma_tests COMMAND dma_tests)

add_executable(timer_tests tests/timers/test_main.cpp)
target_link_libraries(timer_tests PRIVATE gbarecomp_gba)
add_test(NAME timer_tests COMMAND timer_tests)

add_executable(irq_tests tests/irq/test_main.cpp)
target_link_libraries(irq_tests PRIVATE gbarecomp_gba)
add_test(NAME irq_tests COMMAND irq_tests)

add_executable(ppu_smoke_tests tests/ppu_smoke/test_main.cpp)
target_include_directories(ppu_smoke_tests PRIVATE src/runtime src/debug)
target_link_libraries(ppu_smoke_tests PRIVATE gbarecomp_gba)
add_test(NAME ppu_smoke_tests COMMAND ppu_smoke_tests)

add_executable(view_config_tests tests/view_config/test_main.cpp)
target_include_directories(view_config_tests PRIVATE src/runtime)
add_test(NAME view_config_tests COMMAND view_config_tests)

# Overlay primitives behind the in-window rebind menu (hand-written font data).
add_executable(host_overlay_tests
    tests/host_overlay/overlay_test.cpp
    src/runtime/host_overlay.cpp)
target_include_directories(host_overlay_tests PRIVATE src/runtime)
add_test(NAME host_overlay_tests COMMAND host_overlay_tests)

add_executable(presentation_layout_tests tests/presentation_layout/test_main.cpp)
target_include_directories(presentation_layout_tests PRIVATE src/runtime)
add_test(NAME presentation_layout_tests COMMAND presentation_layout_tests)

add_executable(frame_interpolation_tests
    tests/frame_interpolation/test_main.cpp
    src/runtime/frame_interpolation.cpp)
target_include_directories(frame_interpolation_tests PRIVATE src/runtime)
add_test(NAME frame_interpolation_tests COMMAND frame_interpolation_tests)

add_executable(frame_timing_tests
    tests/frame_timing/test_main.cpp
    src/runtime/frame_timing.cpp)
target_include_directories(frame_timing_tests PRIVATE src/runtime)
add_test(NAME frame_timing_tests COMMAND frame_timing_tests)

# VFX-FLICKER-02: header-only temporal frame-blend ("LCD ghosting") math,
# tested against synthetic buffers only (no ROM, no SDL).
add_executable(temporal_blend_tests tests/temporal_blend/test_main.cpp)
target_include_directories(temporal_blend_tests PRIVATE src/runtime)
add_test(NAME temporal_blend_tests COMMAND temporal_blend_tests)

add_executable(audio_drc_tests tests/audio_drc/test_main.cpp)
target_include_directories(audio_drc_tests PRIVATE src/runtime)
add_test(NAME audio_drc_tests COMMAND audio_drc_tests)

# Stage-1/experimental runtime Turbo audio policy and bounded guest/wall-clock
# metadata scheduler. Synthetic scheduler tests stay independent of SDL.
add_executable(turbo_audio_scheduler_tests
    tests/turbo_audio_scheduler/test_main.cpp
    src/gba/turbo_audio_scheduler.cpp)
target_include_directories(turbo_audio_scheduler_tests PRIVATE src/gba)
add_test(NAME turbo_audio_scheduler_tests COMMAND turbo_audio_scheduler_tests)

# Stage-3b pure MP2K wall mixer. Snapshot-only synthetic tests; no live
# runtime, SDL callback, or guest-memory wiring.
add_executable(mp2k_wall_mixer_tests
    tests/mp2k_wall_mixer/test_main.cpp)
target_include_directories(mp2k_wall_mixer_tests PRIVATE src/gba)
target_link_libraries(mp2k_wall_mixer_tests PRIVATE gbarecomp_gba)
add_test(NAME mp2k_wall_mixer_tests COMMAND mp2k_wall_mixer_tests)

add_executable(mp2k_wall_update_queue_tests
    tests/mp2k_wall_update_queue/test_main.cpp)
target_include_directories(mp2k_wall_update_queue_tests PRIVATE src/gba)
target_link_libraries(mp2k_wall_update_queue_tests PRIVATE gbarecomp_gba)
add_test(NAME mp2k_wall_update_queue_tests COMMAND mp2k_wall_update_queue_tests)

# Stage-2 audio capture: bounded metadata only; no live playback wiring.
add_executable(audio_event_capture_tests
    tests/audio_event_capture/test_main.cpp
    src/gba/audio_event_capture.cpp)
target_include_directories(audio_event_capture_tests PRIVATE src/gba)
add_test(NAME audio_event_capture_tests COMMAND audio_event_capture_tests)

# Synthetic judge-publication seam; no live ROM/gameplay integration.
add_executable(audio_event_capture_synthetic_seam_tests
    tests/audio_event_capture/integration_test.cpp
    tools/bios_smoke/dispatch_stub.cpp)
target_link_gbarecomp_runtime_stack(audio_event_capture_synthetic_seam_tests)
add_test(NAME audio_event_capture_synthetic_seam_tests
         COMMAND audio_event_capture_synthetic_seam_tests)

add_executable(mp2k_shadow_tests tests/mp2k_shadow/test_main.cpp)
target_link_libraries(mp2k_shadow_tests PRIVATE gbarecomp_gba)
add_test(NAME mp2k_shadow_tests COMMAND mp2k_shadow_tests)

# Synthetic hot-path benchmark. It compares probe-off/on output and verifier
# state in one process; no ROM/BIOS data or live runtime wiring.
add_executable(mp2k_shadow_benchmark tests/mp2k_shadow/benchmark.cpp)
target_link_libraries(mp2k_shadow_benchmark PRIVATE gbarecomp_gba)
add_test(NAME mp2k_shadow_benchmark COMMAND mp2k_shadow_benchmark)

# P6 differential-gate primitives (offline; full-RAM snapshot/restore/diff).
add_executable(heal_gate_tests
    tests/heal_gate/heal_gate_test.cpp
    tests/heal_gate/runtime_stubs.cpp)
target_link_libraries(heal_gate_tests PRIVATE gbarecomp_heal_gate)
add_test(NAME heal_gate_tests COMMAND heal_gate_tests)

add_executable(ram_overlay_registry_tests
    tests/ram_overlay/registry_test.cpp
    tests/codegen/stubs.cpp)
target_include_directories(ram_overlay_registry_tests PRIVATE
    tests/codegen
    src/armv4t)
target_link_libraries(ram_overlay_registry_tests PRIVATE
    gbarecomp_ram_overlay
    gbarecomp_armv4t)
add_test(NAME ram_overlay_registry_tests COMMAND ram_overlay_registry_tests)

add_executable(ram_image_probe_tests
    tests/ram_overlay/ram_image_probe_test.cpp
    tests/codegen/stubs.cpp)
target_include_directories(ram_image_probe_tests PRIVATE
    tests/codegen
    src/armv4t)
target_link_libraries(ram_image_probe_tests PRIVATE gbarecomp_armv4t)
add_test(NAME ram_image_probe_tests COMMAND ram_image_probe_tests)

# ─────────────────────────────────────────────────────────────────────
# codegen_tests — L1 per-IrOp diff harness.
#
# Architecture:
#   1. tests/codegen/test_cases.cpp lists per-IrOp test cases.
#   2. gen_codegen_tests.exe reads that list, invokes the same
#      ArmCodegen used by tools/gba_recompile, and emits one C
#      function per case into ${CMAKE_BINARY_DIR}/codegen_tests/
#      test_funcs.cpp.
#   3. codegen_tests.exe links test_main.cpp + test_cases.cpp +
#      stubs.cpp + the generated test_funcs.cpp against
#      gbarecomp_armv4t. It runs both the interpreter and the
#      generated function per case and diffs the resulting state.
#
# Per PRINCIPLES.md "Interpreter is informative, never load-bearing":
# the interpreter is the diff-oracle, never a runtime fallback. Any
# divergence is a real codegen bug to fix in arm_codegen.cpp.
# ─────────────────────────────────────────────────────────────────────
add_executable(gen_codegen_tests
    tests/codegen/gen_codegen_tests.cpp
    tests/codegen/test_cases.cpp
)
target_include_directories(gen_codegen_tests PRIVATE
    tests/codegen
    src/armv4t)
target_link_libraries(gen_codegen_tests PRIVATE gbarecomp_armv4t)

set(CODEGEN_TESTS_OUTDIR
    ${CMAKE_CURRENT_BINARY_DIR}/codegen_tests)
set(CODEGEN_TESTS_GENFILE
    ${CODEGEN_TESTS_OUTDIR}/test_funcs.cpp)

file(MAKE_DIRECTORY ${CODEGEN_TESTS_OUTDIR})

add_custom_command(
    OUTPUT  ${CODEGEN_TESTS_GENFILE}
    COMMAND gen_codegen_tests ${CODEGEN_TESTS_GENFILE}
    DEPENDS gen_codegen_tests
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/codegen/test_cases.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/codegen/test_cases.h
    COMMENT "Emitting L1 codegen test functions"
    VERBATIM
)

add_executable(codegen_tests
    tests/codegen/test_main.cpp
    tests/codegen/test_cases.cpp
    tests/codegen/stubs.cpp
    ${CODEGEN_TESTS_GENFILE}
)
target_include_directories(codegen_tests PRIVATE
    tests/codegen
    src/armv4t)
target_link_libraries(codegen_tests PRIVATE gbarecomp_armv4t)
add_test(NAME codegen_tests COMMAND codegen_tests)

# ─────────────────────────────────────────────────────────────────────────────
# selfheal_stage2_bios_identical — Stage-2 GFP1 byte-identity gate. Proves a
# healed-to-native BIOS function (0xBA4 exclude fixture) executes instruction-
# for-instruction identically to the fully-static corpus (static == bridge ==
# heal_cold == heal_warm). Heavy + needs the gitignored ROM/BIOS + a built
# MinishCapRecomp, so it is OPT-IN: pass -DGBARECOMP_SELFHEAL_MC_DIR=<MC dir>.
# Skips (ctest "Skipped") if the assets are absent.
# ─────────────────────────────────────────────────────────────────────────────
if(GBARECOMP_SELFHEAL_MC_DIR)
    find_program(BASH_EXE bash HINTS C:/msys64/usr/bin /usr/bin)
    if(BASH_EXE)
        add_test(NAME selfheal_stage2_bios_identical
            COMMAND ${BASH_EXE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/selfheal/stage2_bios_identical.sh)
        set_tests_properties(selfheal_stage2_bios_identical PROPERTIES
            ENVIRONMENT "GBARECOMP_ROOT=${CMAKE_CURRENT_SOURCE_DIR};MC_DIR=${GBARECOMP_SELFHEAL_MC_DIR}"
            SKIP_RETURN_CODE 77
            TIMEOUT 2400)
        # selfheal_coverage_sweep — drives the built MC from every savestate with
        # the self-heal recompiler on and asserts FULLY_STATIC for each scene
        # (the doctrine's "done"). Surfaces remaining finder gaps; doubles as a
        # coverage regression guard. Same opt-in / SKIP-77 contract.
        add_test(NAME selfheal_coverage_sweep
            COMMAND ${BASH_EXE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/selfheal/coverage_sweep.sh)
        set_tests_properties(selfheal_coverage_sweep PROPERTIES
            ENVIRONMENT "GBARECOMP_ROOT=${CMAKE_CURRENT_SOURCE_DIR};MC_DIR=${GBARECOMP_SELFHEAL_MC_DIR}"
            SKIP_RETURN_CODE 77
            TIMEOUT 2400)
    else()
        message(STATUS "gbarecomp: bash not found; skipping "
                       "selfheal_stage2_bios_identical + selfheal_coverage_sweep")
    endif()
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Oracle binary — opt-in (-DGBARECOMP_BUILD_ORACLE=ON). Embeds libmgba
# (MPL-2.0). Per PRINCIPLES.md the native build has zero copyleft deps;
# the oracle binary is the ONLY place libmgba may be linked, and it
# runs as a SEPARATE PROCESS over TCP.
# ─────────────────────────────────────────────────────────────────────────────
option(GBARECOMP_BUILD_ORACLE
       "Build the mGBA-backed oracle (requires third_party/mgba/build/libmgba.a)"
       OFF)
if(GBARECOMP_BUILD_ORACLE)
    add_subdirectory(oracle)
endif()

# ─────────────────────────────────────────────────────────────────────────────
# runtime.cmake — game projects (e.g. MinishCapRecomp) include this to add
# a runnable target that links generated C against the runtime libs.
# ─────────────────────────────────────────────────────────────────────────────
configure_file(
    cmake/runtime.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/runtime.cmake
    @ONLY
)
