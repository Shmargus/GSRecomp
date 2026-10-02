cmake_minimum_required(VERSION 3.20)
project(GoldenSunRecompStarter LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Link-time (interprocedural) optimization for GoldenSunRecomp + the
# gbarecomp static libraries it links. Off for gsr_bootstrap and every
# third_party/* target (imgui, toml++, gba_suite, mgba) — those keep whatever
# flags they already had. Degrades gracefully: if the toolchain can't do IPO
# (checked below, not assumed), the build proceeds without it instead of
# failing. Override with -DGSR_ENABLE_LTO=OFF.
option(GSR_ENABLE_LTO
    "Enable link-time optimization for GoldenSunRecomp and gbarecomp targets"
    ON)
set(GSR_IPO_SUPPORTED FALSE)
if(GSR_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT GSR_IPO_SUPPORTED OUTPUT GSR_IPO_SUPPORT_ERROR)
    if(NOT GSR_IPO_SUPPORTED)
        message(STATUS
            "GSR_ENABLE_LTO is ON but this toolchain reports IPO/LTO "
            "unsupported (${GSR_IPO_SUPPORT_ERROR}); building without it.")
    endif()
endif()
# Parallel LTRANS (post-analysis codegen) job count for GCC's -flto=N on the
# final GoldenSunRecomp link. This does NOT bound the whole-program-analysis
# (WPA) pass that precedes it — WPA is single-threaded in GCC and is the
# actual peak-memory phase for a binary this size (~168 MB, ~400k alias
# thunks), so raising this number buys wall-clock time on the codegen phase
# only, not memory headroom. Left at a conservative fraction of logical
# cores rather than the full core count so LTRANS workers don't compound
# against that WPA peak on a 16 GB box. Set to "" to omit the explicit -flto=N
# override, or to 1 for a fully serial (lowest-memory) LTRANS. See the LTO
# section in the delivery report for the reasoning and how to size this up.
# build_lto_nosymbols.bat: the same optimised build without debug
# information, which every compile and the LTO link otherwise carry (the
# split-off .debug file is ~600 MB). The exe keeps its plain symbol table, so
# a crash report's exe offsets can still be matched to functions with nm.
# -g0 comes after the configuration's -g on every command line, so it wins.
option(GSR_DEBUG_SYMBOLS "Build with debug information" ON)
if(NOT GSR_DEBUG_SYMBOLS AND NOT MSVC)
    add_compile_options(-g0)
    add_link_options(-g0)
endif()

set(GSR_LTO_JOBS "8" CACHE STRING
    "Parallel LTRANS job count (-flto=N) for the GoldenSunRecomp link when LTO is enabled; empty = compiler default")

# This starter target proves the public repository builds without a ROM/BIOS.
# The real runner is intentionally not wired until GS-009, after upstream APIs,
# symbols, overlays, and hashes are pinned and reviewed.
add_executable(gsr_bootstrap src/bootstrap_main.cpp)

target_compile_definitions(gsr_bootstrap PRIVATE
    GSR_EXPECTED_ROM_SHA1="5c4695205413df7db52b9a184815a07783999971"
)

if(MSVC)
    target_compile_options(gsr_bootstrap PRIVATE /W4 /permissive-)
else()
    target_compile_options(gsr_bootstrap PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_test(NAME bootstrap_help COMMAND gsr_bootstrap --help)

# The spell-spark stamper. Host-side drawing of the effects the game clips to
# its own 128-wide canvas; see src/effect_particles.h for why that clip cannot
# be widened in place. Pure logic, no ROM and no graphics device, so it runs in
# the public suite alongside the Python checks.
add_executable(gsr_effect_particles_test
    tests/cpp/effect_particles_test.cpp
    src/effect_particles.cpp)
target_include_directories(gsr_effect_particles_test PRIVATE src)
add_test(NAME effect_particles COMMAND gsr_effect_particles_test)

# The Earth Surge mod test's particle burst (src/effect_burst.h): a pure CPU
# simulation, checked here without ROM or graphics.
add_executable(gsr_effect_burst_test
    tests/cpp/effect_burst_test.cpp
    src/effect_burst.cpp)
target_include_directories(gsr_effect_burst_test PRIVATE src)
add_test(NAME effect_burst COMMAND gsr_effect_burst_test)

# Reading the particles out of guest memory, which is the half that cannot be
# shared between effect systems -- they disagree on where the array is and what
# a position means. Its refusal path is the important part: position rules
# alone accepted a source on all three real captures, so the reader now has to
# agree with the canvas the game itself drew.
add_executable(gsr_effect_sources_test
    tests/cpp/effect_sources_test.cpp
    src/effect_sources.cpp
    src/effect_particles.cpp)
target_include_directories(gsr_effect_sources_test PRIVATE src)
add_test(NAME effect_sources COMMAND gsr_effect_sources_test)

find_package(Python3 3.10 COMPONENTS Interpreter REQUIRED)
add_test(
    NAME symbol_corpus_validation
    COMMAND ${Python3_EXECUTABLE} -m unittest discover
            -s ${CMAKE_SOURCE_DIR}/tests
            -p test_*.py
)
set_tests_properties(symbol_corpus_validation PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

add_test(
    NAME public_repository_audit
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/audit_public_repo.py
)

# GS-009 local runner. Generated translation units are derivative of the user's
# ROM and therefore stay under the ignored local/ boundary. Public/CI builds
# leave this option off and continue to build only the asset-free bootstrap.
option(GSR_BUILD_LOCAL_RUNNER
    "Build the private, generated-code Golden Sun runner"
    OFF)
set(GBARECOMP_ROOT "${CMAKE_SOURCE_DIR}/gbarecomp"
    CACHE PATH "Path to the pinned gbarecomp checkout")
# Root of the generated game code (every overlay, transient and variant
# folder below). scripts/build_lto.ps1 points it (and each folder setting)
# at the block-timing copy local/gs011_block (tools/block_timing.py), the
# default since 2026-09-30; -PerInstruction points it back at local/gs011.
set(GSR_GAME_CODE_ROOT "${CMAKE_SOURCE_DIR}/local/gs011"
    CACHE PATH "Root of the generated game code folders")
set(GSR_GENERATED_DIR "${CMAKE_SOURCE_DIR}/local/gs009/main"
    CACHE PATH "Private gba_recompile output directory")
set(GSR_TRANSIENT_FUNC2D5C_DIR
    "${GSR_GAME_CODE_ROOT}/transient_func2d5c"
    CACHE PATH "Private identity-prefixed Func_2d5c AOT variant output")
set(GSR_TRANSIENT_FUNCA37C_DIR
    "${GSR_GAME_CODE_ROOT}/transient_funca37c"
    CACHE PATH "Private identity-prefixed Func_a37c AOT variant output")
set(GSR_TRANSIENT_FUNC2808_03006000_DIR
    "${GSR_GAME_CODE_ROOT}/transient_func2808_03006000"
    CACHE PATH "Private identity-prefixed Func_2808 AOT output at 0x03006000")
set(GSR_TRANSIENT_FUNC6ABC_PIC_DIR
    "${GSR_GAME_CODE_ROOT}/transient_func6abc_pic"
    CACHE PATH "Private position-independent Func_6abc stack-thunk AOT output")
set(GSR_TRANSIENT_FUNC15E10_03003A84_DIR
    "${GSR_GAME_CODE_ROOT}/transient_func15e10_03003a84"
    CACHE PATH "Private identity-prefixed Func_15e10 AOT output at 0x03003a84")
set(GSR_TRANSIENT_FUNCA418_03003400_DIR
    "${GSR_GAME_CODE_ROOT}/transient_funca418_03003400"
    CACHE PATH "Private identity-prefixed Func_a418 AOT output at 0x03003400")
set(GSR_TRANSIENT_FUNC15AFC_03003A84_DIR
    "${GSR_GAME_CODE_ROOT}/transient_func15afc_03003a84"
    CACHE PATH "Private identity-prefixed Func_15afc AOT output at 0x03003a84")
# ONE position-independent corpus replaces the four per-base Func_2cf4 ones.
set(GSR_TRANSIENT_FUNC6B84_PIC_DIR
    "${GSR_GAME_CODE_ROOT}/transient_func6b84_pic"
    CACHE PATH "Private position-independent Func_6b84 RAM-image AOT output")
set(GSR_TRANSIENT_FUNC7994_PIC_DIR
    "${GSR_GAME_CODE_ROOT}/transient_func7994_pic"
    CACHE PATH "Private position-independent Func_7994 RAM-image AOT output")
set(GSR_TRANSIENT_FUNC9E7C_PIC_DIR
    "${GSR_GAME_CODE_ROOT}/transient_func9e7c_pic"
    CACHE PATH "Private position-independent Func_9e7c RAM-image AOT output")
set(GSR_TRANSIENT_FUNCA0F8_PIC_DIR
    "${GSR_GAME_CODE_ROOT}/transient_funca0f8_pic"
    CACHE PATH "Private position-independent Func_a0f8 RAM-image AOT output")
set(GSR_TRANSIENT_FUNC1B70_PIC_DIR
    "${GSR_GAME_CODE_ROOT}/transient_func1b70_pic"
    CACHE PATH "Private position-independent Func_1b70 RAM-image AOT output")
set(GSR_TRANSIENT_FUNC9BB8_PIC_DIR
    "${GSR_GAME_CODE_ROOT}/transient_func9bb8_pic"
    CACHE PATH "Private position-independent Func_9bb8 RAM-image AOT output")
set(GSR_TRANSIENT_FUNC15430_PIC_DIR
    "${GSR_GAME_CODE_ROOT}/transient_func15430_pic"
    CACHE PATH "Private position-independent Func_15430 RAM-image AOT output")
set(GSR_TRANSIENT_FUNC15570_PIC_DIR
    "${GSR_GAME_CODE_ROOT}/transient_func15570_pic"
    CACHE PATH "Private position-independent Func_15570 RAM-image AOT output")
set(GSR_TRANSIENT_FUNC158E8_PIC_DIR
    "${GSR_GAME_CODE_ROOT}/transient_func158e8_pic"
    CACHE PATH "Private position-independent Func_158e8 RAM-image AOT output")
set(GSR_TRANSIENT_FUNC2544_PIC_DIR
    "${GSR_GAME_CODE_ROOT}/transient_func2544_pic"
    CACHE PATH "Private position-independent Func_2544 RAM-image AOT output")
set(GSR_TRANSIENT_FUNC2CF4_PIC_DIR
    "${GSR_GAME_CODE_ROOT}/transient_func2cf4_pic"
    CACHE PATH "Private position-independent Func_2cf4 RAM-image AOT output")
set(GSR_TRANSIENT_FUNC1DC8_PIC_DIR
    "${GSR_GAME_CODE_ROOT}/transient_func1dc8_pic"
    CACHE PATH "Private position-independent Func_1dc8 RAM-image AOT output")
set(GSR_TRANSIENT_FUNC15AFC_PIC_DIR
    "${GSR_GAME_CODE_ROOT}/transient_func15afc_pic"
    CACHE PATH "Private position-independent Func_15afc RAM-image AOT output")
set(GSR_TRANSIENT_FUNC15E10_PIC_DIR
    "${GSR_GAME_CODE_ROOT}/transient_func15e10_pic"
    CACHE PATH "Private position-independent Func_15e10 RAM-image AOT output")
set(GSR_TRANSIENT_FUNC15D74_PIC_DIR
    "${GSR_GAME_CODE_ROOT}/transient_func15d74_pic"
    CACHE PATH "Private position-independent Func_15d74 RAM-image AOT output")
set(GSR_TRANSIENT_FUNC155D0_PIC_DIR
    "${GSR_GAME_CODE_ROOT}/transient_func155d0_pic"
    CACHE PATH "Private position-independent Func_155d0 RAM-image AOT output")
set(GSR_TRANSIENT_FUNC9BB8_SHORT_PIC_DIR
    "${GSR_GAME_CODE_ROOT}/transient_func9bb8_short_pic"
    CACHE PATH "Private position-independent Func_9bb8 short-copy RAM-image AOT output")
set(GSR_TRANSIENT_FUNC9BB8_CODE_PIC_DIR
    "${GSR_GAME_CODE_ROOT}/transient_func9bb8_code_pic"
    CACHE PATH "Private position-independent Func_9bb8 code-only RAM-image AOT output")
set(GSR_TRANSIENT_FUNCB5138_PIC_DIR
    "${GSR_GAME_CODE_ROOT}/transient_funcb5138_pic"
    CACHE PATH "Private position-independent Func_b5138 RAM-image AOT output")
set(GSR_TRANSIENT_FUNCF0024_PIC_DIR
    "${GSR_GAME_CODE_ROOT}/transient_funcf0024_pic"
    CACHE PATH "Private position-independent Func_f0024 (credits decoder) RAM-image AOT output")
set(GSR_TRANSIENT_FUNCA418_PIC_DIR
    "${GSR_GAME_CODE_ROOT}/transient_funca418_pic"
    CACHE PATH "Private position-independent Func_a418 RAM-image AOT output")
set(GSR_TRANSIENT_FUNC1EA8_PIC_DIR
    "${GSR_GAME_CODE_ROOT}/transient_func1ea8_pic"
    CACHE PATH "Private position-independent spell-canvas copy-and-fade RAM-image AOT output")
set(GSR_TRANSIENT_FUNC1EF8_PIC_DIR
    "${GSR_GAME_CODE_ROOT}/transient_func1ef8_pic"
    CACHE PATH "Private position-independent spell-canvas copy-and-halve RAM-image AOT output")
set(GSR_TRANSIENT_FUNC6C24_PIC_DIR
    "${GSR_GAME_CODE_ROOT}/transient_func6c24_pic"
    CACHE PATH "Private position-independent Func_6c24 RAM-image AOT output")
set(GSR_TRANSIENT_FUNC203C_PIC_DIR
    "${GSR_GAME_CODE_ROOT}/transient_func203c_pic"
    CACHE PATH "Private position-independent Func_203c RAM-image AOT output")
set(GSR_TRANSIENT_FUNC1F38_PIC_DIR
    "${GSR_GAME_CODE_ROOT}/transient_func1f38_pic"
    CACHE PATH "Private position-independent Func_1f38 RAM-image AOT output")
set(GSR_TRANSIENT_FUNC1FB8_PIC_DIR
    "${GSR_GAME_CODE_ROOT}/transient_func1fb8_pic"
    CACHE PATH "Private position-independent Func_1fb8 RAM-image AOT output")
set(GSR_TRANSIENT_FUNC2098_PIC_DIR
    "${GSR_GAME_CODE_ROOT}/transient_func2098_pic"
    CACHE PATH "Private position-independent Func_2098 RAM-image AOT output")
set(GSR_SYNTH_DC8_030008D4_DIR
    "${GSR_GAME_CODE_ROOT}/synth_dc8_030008d4"
    CACHE PATH "Private identity-prefixed synthesized Func_dc8 AOT output")
set(GSR_SYNTH_DC8_03000904_DIR
    "${GSR_GAME_CODE_ROOT}/synth_dc8_03000904"
    CACHE PATH "Private identity-prefixed third synthesized Func_dc8 AOT output")
set(GSR_SYNTH_DC8_030008EC_DIR
    "${GSR_GAME_CODE_ROOT}/synth_dc8_030008ec"
    CACHE PATH "Private identity-prefixed second synthesized Func_dc8 AOT output")
set(GSR_STAMPS_DIR
    "${GSR_GAME_CODE_ROOT}/stamps"
    CACHE PATH "Private position-independent Func_ed408 stamp routines (tools/build_stamp_variants.py)")
foreach(GSR_SLOT3_VARIANT 0 1 2 3 4)
    set(GSR_SYNTH_DC8C_${GSR_SLOT3_VARIANT}_DIR
        "${GSR_GAME_CODE_ROOT}/synth_dc8c_${GSR_SLOT3_VARIANT}"
        CACHE PATH "Private identity-prefixed Func_dc8 slot-3 variant AOT output")
endforeach()
foreach(GSR_SLOT2_VARIANT 0 1 2 3)
    set(GSR_SYNTH_DC8B_${GSR_SLOT2_VARIANT}_DIR
        "${GSR_GAME_CODE_ROOT}/synth_dc8b_${GSR_SLOT2_VARIANT}"
        CACHE PATH "Private identity-prefixed Func_dc8 slot-2 variant AOT output")
endforeach()
set(GSR_OVERLAY_ROOT
    "${GSR_GAME_CODE_ROOT}"
    CACHE PATH "Root containing private identity-prefixed overlay_<id> AOT outputs")

if(GSR_BUILD_LOCAL_RUNNER)
    if(NOT EXISTS "${GBARECOMP_ROOT}/CMakeLists.txt")
        message(FATAL_ERROR
            "GBARECOMP_ROOT is not a gbarecomp checkout: ${GBARECOMP_ROOT}")
    endif()
    if(NOT EXISTS "${GSR_GENERATED_DIR}/recompiled.h" OR
       NOT EXISTS "${GSR_GENERATED_DIR}/dispatch_table.cpp")
        message(FATAL_ERROR
            "GSR_GENERATED_DIR is not a complete private corpus: "
            "${GSR_GENERATED_DIR}")
    endif()
    if(NOT EXISTS "${GSR_TRANSIENT_FUNC2D5C_DIR}/recompiled.h" OR
       NOT EXISTS "${GSR_TRANSIENT_FUNC2D5C_DIR}/dispatch_table.cpp")
        message(FATAL_ERROR
            "GSR_TRANSIENT_FUNC2D5C_DIR is not a complete private corpus: "
            "${GSR_TRANSIENT_FUNC2D5C_DIR}")
    endif()
    if(NOT EXISTS "${GSR_TRANSIENT_FUNCA37C_DIR}/recompiled.h" OR
       NOT EXISTS "${GSR_TRANSIENT_FUNCA37C_DIR}/dispatch_table.cpp")
        message(FATAL_ERROR
            "GSR_TRANSIENT_FUNCA37C_DIR is not a complete private corpus: "
            "${GSR_TRANSIENT_FUNCA37C_DIR}")
    endif()
    if(NOT EXISTS "${GSR_TRANSIENT_FUNC2808_03006000_DIR}/recompiled.h" OR
       NOT EXISTS "${GSR_TRANSIENT_FUNC2808_03006000_DIR}/dispatch_table.cpp")
        message(FATAL_ERROR
            "GSR_TRANSIENT_FUNC2808_03006000_DIR is not a complete private corpus: "
            "${GSR_TRANSIENT_FUNC2808_03006000_DIR}")
    endif()
    if(NOT EXISTS "${GSR_TRANSIENT_FUNC6ABC_PIC_DIR}/recompiled.h" OR
       NOT EXISTS "${GSR_TRANSIENT_FUNC6ABC_PIC_DIR}/dispatch_table.cpp")
        message(FATAL_ERROR
            "GSR_TRANSIENT_FUNC6ABC_PIC_DIR is not a complete private corpus: "
            "${GSR_TRANSIENT_FUNC6ABC_PIC_DIR}")
    endif()
    if(NOT EXISTS "${GSR_TRANSIENT_FUNC15E10_03003A84_DIR}/recompiled.h" OR
       NOT EXISTS "${GSR_TRANSIENT_FUNC15E10_03003A84_DIR}/dispatch_table.cpp")
        message(FATAL_ERROR
            "GSR_TRANSIENT_FUNC15E10_03003A84_DIR is not a complete private corpus: "
            "${GSR_TRANSIENT_FUNC15E10_03003A84_DIR}")
    endif()
    if(NOT EXISTS "${GSR_TRANSIENT_FUNCA418_03003400_DIR}/recompiled.h" OR
       NOT EXISTS "${GSR_TRANSIENT_FUNCA418_03003400_DIR}/dispatch_table.cpp")
        message(FATAL_ERROR
            "GSR_TRANSIENT_FUNCA418_03003400_DIR is not a complete private corpus: "
            "${GSR_TRANSIENT_FUNCA418_03003400_DIR}")
    endif()
    if(NOT EXISTS "${GSR_TRANSIENT_FUNC15AFC_03003A84_DIR}/recompiled.h" OR
       NOT EXISTS "${GSR_TRANSIENT_FUNC15AFC_03003A84_DIR}/dispatch_table.cpp")
        message(FATAL_ERROR
            "GSR_TRANSIENT_FUNC15AFC_03003A84_DIR is not a complete private corpus: "
            "${GSR_TRANSIENT_FUNC15AFC_03003A84_DIR}")
    endif()
    if(NOT EXISTS "${GSR_TRANSIENT_FUNC6B84_PIC_DIR}/recompiled.h" OR
       NOT EXISTS "${GSR_TRANSIENT_FUNC6B84_PIC_DIR}/dispatch_table.cpp")
        message(FATAL_ERROR
            "GSR_TRANSIENT_FUNC6B84_PIC_DIR is not a complete private corpus: "
            "${GSR_TRANSIENT_FUNC6B84_PIC_DIR}")
    endif()
    if(NOT EXISTS "${GSR_TRANSIENT_FUNC7994_PIC_DIR}/recompiled.h" OR
       NOT EXISTS "${GSR_TRANSIENT_FUNC7994_PIC_DIR}/dispatch_table.cpp")
        message(FATAL_ERROR
            "GSR_TRANSIENT_FUNC7994_PIC_DIR is not a complete private corpus: "
            "${GSR_TRANSIENT_FUNC7994_PIC_DIR}")
    endif()
    if(NOT EXISTS "${GSR_TRANSIENT_FUNC9E7C_PIC_DIR}/recompiled.h" OR
       NOT EXISTS "${GSR_TRANSIENT_FUNC9E7C_PIC_DIR}/dispatch_table.cpp")
        message(FATAL_ERROR
            "GSR_TRANSIENT_FUNC9E7C_PIC_DIR is not a complete private corpus: "
            "${GSR_TRANSIENT_FUNC9E7C_PIC_DIR}")
    endif()
    if(NOT EXISTS "${GSR_TRANSIENT_FUNCA0F8_PIC_DIR}/recompiled.h" OR
       NOT EXISTS "${GSR_TRANSIENT_FUNCA0F8_PIC_DIR}/dispatch_table.cpp")
        message(FATAL_ERROR
            "GSR_TRANSIENT_FUNCA0F8_PIC_DIR is not a complete private corpus: "
            "${GSR_TRANSIENT_FUNCA0F8_PIC_DIR}")
    endif()
    if(NOT EXISTS "${GSR_TRANSIENT_FUNC1B70_PIC_DIR}/recompiled.h" OR
       NOT EXISTS "${GSR_TRANSIENT_FUNC1B70_PIC_DIR}/dispatch_table.cpp")
        message(FATAL_ERROR
            "GSR_TRANSIENT_FUNC1B70_PIC_DIR is not a complete private corpus: "
            "${GSR_TRANSIENT_FUNC1B70_PIC_DIR}")
    endif()
    if(NOT EXISTS "${GSR_TRANSIENT_FUNC9BB8_PIC_DIR}/recompiled.h" OR
       NOT EXISTS "${GSR_TRANSIENT_FUNC9BB8_PIC_DIR}/dispatch_table.cpp")
        message(FATAL_ERROR
            "GSR_TRANSIENT_FUNC9BB8_PIC_DIR is not a complete private corpus: "
            "${GSR_TRANSIENT_FUNC9BB8_PIC_DIR}")
    endif()
    if(NOT EXISTS "${GSR_TRANSIENT_FUNC15430_PIC_DIR}/recompiled.h" OR
       NOT EXISTS "${GSR_TRANSIENT_FUNC15430_PIC_DIR}/dispatch_table.cpp")
        message(FATAL_ERROR
            "GSR_TRANSIENT_FUNC15430_PIC_DIR is not a complete private corpus: "
            "${GSR_TRANSIENT_FUNC15430_PIC_DIR}")
    endif()
    if(NOT EXISTS "${GSR_TRANSIENT_FUNC15570_PIC_DIR}/recompiled.h" OR
       NOT EXISTS "${GSR_TRANSIENT_FUNC15570_PIC_DIR}/dispatch_table.cpp")
        message(FATAL_ERROR
            "GSR_TRANSIENT_FUNC15570_PIC_DIR is not a complete private corpus: "
            "${GSR_TRANSIENT_FUNC15570_PIC_DIR}")
    endif()
    if(NOT EXISTS "${GSR_TRANSIENT_FUNC158E8_PIC_DIR}/recompiled.h" OR
       NOT EXISTS "${GSR_TRANSIENT_FUNC158E8_PIC_DIR}/dispatch_table.cpp")
        message(FATAL_ERROR
            "GSR_TRANSIENT_FUNC158E8_PIC_DIR is not a complete private corpus: "
            "${GSR_TRANSIENT_FUNC158E8_PIC_DIR}")
    endif()
    if(NOT EXISTS "${GSR_TRANSIENT_FUNC2544_PIC_DIR}/recompiled.h" OR
       NOT EXISTS "${GSR_TRANSIENT_FUNC2544_PIC_DIR}/dispatch_table.cpp")
        message(FATAL_ERROR
            "GSR_TRANSIENT_FUNC2544_PIC_DIR is not a complete private corpus: "
            "${GSR_TRANSIENT_FUNC2544_PIC_DIR}")
    endif()
    if(NOT EXISTS "${GSR_TRANSIENT_FUNC2CF4_PIC_DIR}/recompiled.h" OR
       NOT EXISTS "${GSR_TRANSIENT_FUNC2CF4_PIC_DIR}/dispatch_table.cpp")
        message(FATAL_ERROR
            "GSR_TRANSIENT_FUNC2CF4_PIC_DIR is not a complete private corpus: "
            "${GSR_TRANSIENT_FUNC2CF4_PIC_DIR}")
    endif()
    if(NOT EXISTS "${GSR_TRANSIENT_FUNC1DC8_PIC_DIR}/recompiled.h" OR
       NOT EXISTS "${GSR_TRANSIENT_FUNC1DC8_PIC_DIR}/dispatch_table.cpp")
        message(FATAL_ERROR
            "GSR_TRANSIENT_FUNC1DC8_PIC_DIR is not a complete private corpus: "
            "${GSR_TRANSIENT_FUNC1DC8_PIC_DIR}")
    endif()
    if(NOT EXISTS "${GSR_TRANSIENT_FUNC15AFC_PIC_DIR}/recompiled.h" OR
       NOT EXISTS "${GSR_TRANSIENT_FUNC15AFC_PIC_DIR}/dispatch_table.cpp")
        message(FATAL_ERROR
            "GSR_TRANSIENT_FUNC15AFC_PIC_DIR is not a complete private corpus: "
            "${GSR_TRANSIENT_FUNC15AFC_PIC_DIR}")
    endif()
    if(NOT EXISTS "${GSR_TRANSIENT_FUNC15E10_PIC_DIR}/recompiled.h" OR
       NOT EXISTS "${GSR_TRANSIENT_FUNC15E10_PIC_DIR}/dispatch_table.cpp")
        message(FATAL_ERROR
            "GSR_TRANSIENT_FUNC15E10_PIC_DIR is not a complete private corpus: "
            "${GSR_TRANSIENT_FUNC15E10_PIC_DIR}")
    endif()
    if(NOT EXISTS "${GSR_TRANSIENT_FUNC15D74_PIC_DIR}/recompiled.h" OR
       NOT EXISTS "${GSR_TRANSIENT_FUNC15D74_PIC_DIR}/dispatch_table.cpp")
        message(FATAL_ERROR
            "GSR_TRANSIENT_FUNC15D74_PIC_DIR is not a complete private corpus: "
            "${GSR_TRANSIENT_FUNC15D74_PIC_DIR}")
    endif()
    if(NOT EXISTS "${GSR_TRANSIENT_FUNC155D0_PIC_DIR}/recompiled.h" OR
       NOT EXISTS "${GSR_TRANSIENT_FUNC155D0_PIC_DIR}/dispatch_table.cpp")
        message(FATAL_ERROR
            "GSR_TRANSIENT_FUNC155D0_PIC_DIR is not a complete private corpus: "
            "${GSR_TRANSIENT_FUNC155D0_PIC_DIR}")
    endif()
    if(NOT EXISTS "${GSR_TRANSIENT_FUNC9BB8_SHORT_PIC_DIR}/recompiled.h" OR
       NOT EXISTS "${GSR_TRANSIENT_FUNC9BB8_SHORT_PIC_DIR}/dispatch_table.cpp")
        message(FATAL_ERROR
            "GSR_TRANSIENT_FUNC9BB8_SHORT_PIC_DIR is not a complete private corpus: "
            "${GSR_TRANSIENT_FUNC9BB8_SHORT_PIC_DIR}")
    endif()
    if(NOT EXISTS "${GSR_TRANSIENT_FUNC9BB8_CODE_PIC_DIR}/recompiled.h" OR
       NOT EXISTS "${GSR_TRANSIENT_FUNC9BB8_CODE_PIC_DIR}/dispatch_table.cpp")
        message(FATAL_ERROR
            "GSR_TRANSIENT_FUNC9BB8_CODE_PIC_DIR is not a complete private corpus: "
            "${GSR_TRANSIENT_FUNC9BB8_CODE_PIC_DIR}")
    endif()
    if(NOT EXISTS "${GSR_TRANSIENT_FUNCB5138_PIC_DIR}/recompiled.h" OR
       NOT EXISTS "${GSR_TRANSIENT_FUNCB5138_PIC_DIR}/dispatch_table.cpp")
        message(FATAL_ERROR
            "GSR_TRANSIENT_FUNCB5138_PIC_DIR is not a complete private corpus: "
            "${GSR_TRANSIENT_FUNCB5138_PIC_DIR}")
    endif()
    if(NOT EXISTS "${GSR_TRANSIENT_FUNCF0024_PIC_DIR}/recompiled.h" OR
       NOT EXISTS "${GSR_TRANSIENT_FUNCF0024_PIC_DIR}/dispatch_table.cpp")
        message(FATAL_ERROR
            "GSR_TRANSIENT_FUNCF0024_PIC_DIR is not a complete private corpus: "
            "${GSR_TRANSIENT_FUNCF0024_PIC_DIR}")
    endif()
    if(NOT EXISTS "${GSR_TRANSIENT_FUNCA418_PIC_DIR}/recompiled.h" OR
       NOT EXISTS "${GSR_TRANSIENT_FUNCA418_PIC_DIR}/dispatch_table.cpp")
        message(FATAL_ERROR
            "GSR_TRANSIENT_FUNCA418_PIC_DIR is not a complete private corpus: "
            "${GSR_TRANSIENT_FUNCA418_PIC_DIR}")
    endif()
    if(NOT EXISTS "${GSR_TRANSIENT_FUNC1EA8_PIC_DIR}/recompiled.h" OR
       NOT EXISTS "${GSR_TRANSIENT_FUNC1EA8_PIC_DIR}/dispatch_table.cpp")
        message(FATAL_ERROR
            "GSR_TRANSIENT_FUNC1EA8_PIC_DIR is not a complete private corpus: "
            "${GSR_TRANSIENT_FUNC1EA8_PIC_DIR}")
    endif()
    if(NOT EXISTS "${GSR_TRANSIENT_FUNC1EF8_PIC_DIR}/recompiled.h" OR
       NOT EXISTS "${GSR_TRANSIENT_FUNC1EF8_PIC_DIR}/dispatch_table.cpp")
        message(FATAL_ERROR
            "GSR_TRANSIENT_FUNC1EF8_PIC_DIR is not a complete private corpus: "
            "${GSR_TRANSIENT_FUNC1EF8_PIC_DIR}")
    endif()
    if(NOT EXISTS "${GSR_TRANSIENT_FUNC6C24_PIC_DIR}/recompiled.h" OR
       NOT EXISTS "${GSR_TRANSIENT_FUNC6C24_PIC_DIR}/dispatch_table.cpp")
        message(FATAL_ERROR
            "GSR_TRANSIENT_FUNC6C24_PIC_DIR is not a complete private corpus: "
            "${GSR_TRANSIENT_FUNC6C24_PIC_DIR}")
    endif()
    if(NOT EXISTS "${GSR_TRANSIENT_FUNC203C_PIC_DIR}/recompiled.h" OR
       NOT EXISTS "${GSR_TRANSIENT_FUNC203C_PIC_DIR}/dispatch_table.cpp")
        message(FATAL_ERROR
            "GSR_TRANSIENT_FUNC203C_PIC_DIR is not a complete private corpus: "
            "${GSR_TRANSIENT_FUNC203C_PIC_DIR}")
    endif()
    if(NOT EXISTS "${GSR_TRANSIENT_FUNC1F38_PIC_DIR}/recompiled.h" OR
       NOT EXISTS "${GSR_TRANSIENT_FUNC1F38_PIC_DIR}/dispatch_table.cpp")
        message(FATAL_ERROR
            "GSR_TRANSIENT_FUNC1F38_PIC_DIR is not a complete private corpus: "
            "${GSR_TRANSIENT_FUNC1F38_PIC_DIR}")
    endif()
    if(NOT EXISTS "${GSR_TRANSIENT_FUNC1FB8_PIC_DIR}/recompiled.h" OR
       NOT EXISTS "${GSR_TRANSIENT_FUNC1FB8_PIC_DIR}/dispatch_table.cpp")
        message(FATAL_ERROR
            "GSR_TRANSIENT_FUNC1FB8_PIC_DIR is not a complete private corpus: "
            "${GSR_TRANSIENT_FUNC1FB8_PIC_DIR}")
    endif()
    if(NOT EXISTS "${GSR_TRANSIENT_FUNC2098_PIC_DIR}/recompiled.h" OR
       NOT EXISTS "${GSR_TRANSIENT_FUNC2098_PIC_DIR}/dispatch_table.cpp")
        message(FATAL_ERROR
            "GSR_TRANSIENT_FUNC2098_PIC_DIR is not a complete private corpus: "
            "${GSR_TRANSIENT_FUNC2098_PIC_DIR}")
    endif()
    if(NOT EXISTS "${GSR_SYNTH_DC8_030008D4_DIR}/recompiled.h" OR
       NOT EXISTS "${GSR_SYNTH_DC8_030008D4_DIR}/dispatch_table.cpp")
        message(FATAL_ERROR
            "GSR_SYNTH_DC8_030008D4_DIR is not a complete private corpus: "
            "${GSR_SYNTH_DC8_030008D4_DIR}")
    endif()
    if(NOT EXISTS "${GSR_SYNTH_DC8_03000904_DIR}/recompiled.h" OR
       NOT EXISTS "${GSR_SYNTH_DC8_03000904_DIR}/dispatch_table.cpp")
        message(FATAL_ERROR
            "GSR_SYNTH_DC8_03000904_DIR is not a complete private corpus: "
            "${GSR_SYNTH_DC8_03000904_DIR}")
    endif()
    if(NOT EXISTS "${GSR_SYNTH_DC8_030008EC_DIR}/recompiled.h" OR
       NOT EXISTS "${GSR_SYNTH_DC8_030008EC_DIR}/dispatch_table.cpp")
        message(FATAL_ERROR
            "GSR_SYNTH_DC8_030008EC_DIR is not a complete private corpus: "
            "${GSR_SYNTH_DC8_030008EC_DIR}")
    endif()
    if(NOT EXISTS "${GSR_STAMPS_DIR}/recompiled.h" OR
       NOT EXISTS "${GSR_STAMPS_DIR}/dispatch_table.cpp" OR
       NOT EXISTS "${GSR_STAMPS_DIR}/stamp_registry.cpp")
        message(FATAL_ERROR
            "GSR_STAMPS_DIR is not a complete private corpus (run "
            "tools/build_stamp_variants.py --recompile): ${GSR_STAMPS_DIR}")
    endif()
    file(GLOB GSR_STAMPS_SOURCES CONFIGURE_DEPENDS
        "${GSR_STAMPS_DIR}/recompiled.cpp"
        "${GSR_STAMPS_DIR}/recompiled_*.cpp"
        "${GSR_STAMPS_DIR}/dispatch_table.cpp"
        "${GSR_STAMPS_DIR}/stamp_registry.cpp")
    # tools/build_synthesized_dc8_slot2_variant.py --variant 0..3, then
    # gba_recompile --symbol-prefix gsr_synth_dc8b_<n>_.
    set(GSR_SYNTH_DC8B_SOURCES)
    foreach(GSR_SLOT2_VARIANT 0 1 2 3)
        set(GSR_SLOT2_DIR "${GSR_SYNTH_DC8B_${GSR_SLOT2_VARIANT}_DIR}")
        if(NOT EXISTS "${GSR_SLOT2_DIR}/recompiled.h" OR
           NOT EXISTS "${GSR_SLOT2_DIR}/dispatch_table.cpp")
            message(FATAL_ERROR
                "GSR_SYNTH_DC8B_${GSR_SLOT2_VARIANT}_DIR is not a complete "
                "private corpus: ${GSR_SLOT2_DIR}")
        endif()
        file(GLOB GSR_SLOT2_SOURCES CONFIGURE_DEPENDS
            "${GSR_SLOT2_DIR}/recompiled.cpp"
            "${GSR_SLOT2_DIR}/recompiled_*.cpp"
            "${GSR_SLOT2_DIR}/dispatch_table.cpp")
        list(APPEND GSR_SYNTH_DC8B_SOURCES ${GSR_SLOT2_SOURCES})
    endforeach()
    # tools/build_synthesized_dc8_slot3_variant.py --variant 0..4, then
    # gba_recompile --symbol-prefix gsr_synth_dc8c_<n>_.
    foreach(GSR_SLOT3_VARIANT 0 1 2 3 4)
        set(GSR_SLOT3_DIR "${GSR_SYNTH_DC8C_${GSR_SLOT3_VARIANT}_DIR}")
        if(NOT EXISTS "${GSR_SLOT3_DIR}/recompiled.h" OR
           NOT EXISTS "${GSR_SLOT3_DIR}/dispatch_table.cpp")
            message(FATAL_ERROR
                "GSR_SYNTH_DC8C_${GSR_SLOT3_VARIANT}_DIR is not a complete "
                "private corpus: ${GSR_SLOT3_DIR}")
        endif()
        file(GLOB GSR_SLOT3_SOURCES CONFIGURE_DEPENDS
            "${GSR_SLOT3_DIR}/recompiled.cpp"
            "${GSR_SLOT3_DIR}/recompiled_*.cpp"
            "${GSR_SLOT3_DIR}/dispatch_table.cpp")
        list(APPEND GSR_SYNTH_DC8B_SOURCES ${GSR_SLOT3_SOURCES})
    endforeach()
    file(STRINGS "${CMAKE_SOURCE_DIR}/config/usa/overlay-registry.inc"
        GSR_OVERLAY_REGISTRY_LINES REGEX "^GSR_OVERLAY\\(")
    set(GSR_OVERLAY_IDS)
    foreach(GSR_OVERLAY_LINE IN LISTS GSR_OVERLAY_REGISTRY_LINES)
        string(REGEX MATCH "^GSR_OVERLAY\\(([a-z0-9_]+)," _ "${GSR_OVERLAY_LINE}")
        if(NOT CMAKE_MATCH_1)
            message(FATAL_ERROR "Malformed overlay registry row: ${GSR_OVERLAY_LINE}")
        endif()
        list(APPEND GSR_OVERLAY_IDS "${CMAKE_MATCH_1}")
        set(GSR_OVERLAY_DIR "${GSR_OVERLAY_ROOT}/overlay_${CMAKE_MATCH_1}")
        if(NOT EXISTS "${GSR_OVERLAY_DIR}/recompiled.h" OR
           NOT EXISTS "${GSR_OVERLAY_DIR}/dispatch_table.cpp")
            message(FATAL_ERROR
                "Overlay registry corpus is missing or incomplete: ${GSR_OVERLAY_DIR}")
        endif()
    endforeach()

    # No Nintendo code in the program (Jimmy, 2026-09-24): the game runs in
    # no-BIOS mode. Turn this on only for a comparison build against the
    # real BIOS, with its locally generated recompiled output present.
    set(GBARECOMP_LINK_BIOS OFF CACHE BOOL
        "Link the locally generated recompiled BIOS (comparison builds only)")
    add_subdirectory("${GBARECOMP_ROOT}" gbarecomp_build EXCLUDE_FROM_ALL)
    target_compile_definitions(gbarecomp_runtime PRIVATE
        GBARECOMP_WINDOW_TITLE="Golden Sun Recompiled")
    file(GLOB GSR_GENERATED_SOURCES CONFIGURE_DEPENDS
        "${GSR_GENERATED_DIR}/recompiled_*.cpp"
        "${GSR_GENERATED_DIR}/dispatch_table.cpp"
        "${GSR_GENERATED_DIR}/symbol_map.cpp")
    file(GLOB GSR_TRANSIENT_FUNC2D5C_SOURCES CONFIGURE_DEPENDS
        "${GSR_TRANSIENT_FUNC2D5C_DIR}/recompiled.cpp"
        "${GSR_TRANSIENT_FUNC2D5C_DIR}/recompiled_*.cpp"
        "${GSR_TRANSIENT_FUNC2D5C_DIR}/dispatch_table.cpp")
    file(GLOB GSR_TRANSIENT_FUNCA37C_SOURCES CONFIGURE_DEPENDS
        "${GSR_TRANSIENT_FUNCA37C_DIR}/recompiled.cpp"
        "${GSR_TRANSIENT_FUNCA37C_DIR}/recompiled_*.cpp"
        "${GSR_TRANSIENT_FUNCA37C_DIR}/dispatch_table.cpp")
    file(GLOB GSR_TRANSIENT_FUNC2808_03006000_SOURCES CONFIGURE_DEPENDS
        "${GSR_TRANSIENT_FUNC2808_03006000_DIR}/recompiled.cpp"
        "${GSR_TRANSIENT_FUNC2808_03006000_DIR}/recompiled_*.cpp"
        "${GSR_TRANSIENT_FUNC2808_03006000_DIR}/dispatch_table.cpp")
    file(GLOB GSR_TRANSIENT_FUNC6ABC_PIC_SOURCES CONFIGURE_DEPENDS
        "${GSR_TRANSIENT_FUNC6ABC_PIC_DIR}/recompiled.cpp"
        "${GSR_TRANSIENT_FUNC6ABC_PIC_DIR}/recompiled_*.cpp"
        "${GSR_TRANSIENT_FUNC6ABC_PIC_DIR}/dispatch_table.cpp")
    file(GLOB GSR_TRANSIENT_FUNC15E10_03003A84_SOURCES CONFIGURE_DEPENDS
        "${GSR_TRANSIENT_FUNC15E10_03003A84_DIR}/recompiled.cpp"
        "${GSR_TRANSIENT_FUNC15E10_03003A84_DIR}/recompiled_*.cpp"
        "${GSR_TRANSIENT_FUNC15E10_03003A84_DIR}/dispatch_table.cpp")
    file(GLOB GSR_TRANSIENT_FUNCA418_03003400_SOURCES CONFIGURE_DEPENDS
        "${GSR_TRANSIENT_FUNCA418_03003400_DIR}/recompiled.cpp"
        "${GSR_TRANSIENT_FUNCA418_03003400_DIR}/recompiled_*.cpp"
        "${GSR_TRANSIENT_FUNCA418_03003400_DIR}/dispatch_table.cpp")
    file(GLOB GSR_TRANSIENT_FUNC15AFC_03003A84_SOURCES CONFIGURE_DEPENDS
        "${GSR_TRANSIENT_FUNC15AFC_03003A84_DIR}/recompiled.cpp"
        "${GSR_TRANSIENT_FUNC15AFC_03003A84_DIR}/recompiled_*.cpp"
        "${GSR_TRANSIENT_FUNC15AFC_03003A84_DIR}/dispatch_table.cpp")
    file(GLOB GSR_TRANSIENT_FUNC6B84_PIC_SOURCES CONFIGURE_DEPENDS
        "${GSR_TRANSIENT_FUNC6B84_PIC_DIR}/recompiled.cpp"
        "${GSR_TRANSIENT_FUNC6B84_PIC_DIR}/recompiled_*.cpp"
        "${GSR_TRANSIENT_FUNC6B84_PIC_DIR}/dispatch_table.cpp")
    file(GLOB GSR_TRANSIENT_FUNC7994_PIC_SOURCES CONFIGURE_DEPENDS
        "${GSR_TRANSIENT_FUNC7994_PIC_DIR}/recompiled.cpp"
        "${GSR_TRANSIENT_FUNC7994_PIC_DIR}/recompiled_*.cpp"
        "${GSR_TRANSIENT_FUNC7994_PIC_DIR}/dispatch_table.cpp")
    file(GLOB GSR_TRANSIENT_FUNC9E7C_PIC_SOURCES CONFIGURE_DEPENDS
        "${GSR_TRANSIENT_FUNC9E7C_PIC_DIR}/recompiled.cpp"
        "${GSR_TRANSIENT_FUNC9E7C_PIC_DIR}/recompiled_*.cpp"
        "${GSR_TRANSIENT_FUNC9E7C_PIC_DIR}/dispatch_table.cpp")
    file(GLOB GSR_TRANSIENT_FUNCA0F8_PIC_SOURCES CONFIGURE_DEPENDS
        "${GSR_TRANSIENT_FUNCA0F8_PIC_DIR}/recompiled.cpp"
        "${GSR_TRANSIENT_FUNCA0F8_PIC_DIR}/recompiled_*.cpp"
        "${GSR_TRANSIENT_FUNCA0F8_PIC_DIR}/dispatch_table.cpp")
    file(GLOB GSR_TRANSIENT_FUNC1B70_PIC_SOURCES CONFIGURE_DEPENDS
        "${GSR_TRANSIENT_FUNC1B70_PIC_DIR}/recompiled.cpp"
        "${GSR_TRANSIENT_FUNC1B70_PIC_DIR}/recompiled_*.cpp"
        "${GSR_TRANSIENT_FUNC1B70_PIC_DIR}/dispatch_table.cpp")
    file(GLOB GSR_TRANSIENT_FUNC9BB8_PIC_SOURCES CONFIGURE_DEPENDS
        "${GSR_TRANSIENT_FUNC9BB8_PIC_DIR}/recompiled.cpp"
        "${GSR_TRANSIENT_FUNC9BB8_PIC_DIR}/recompiled_*.cpp"
        "${GSR_TRANSIENT_FUNC9BB8_PIC_DIR}/dispatch_table.cpp")
    file(GLOB GSR_TRANSIENT_FUNC15430_PIC_SOURCES CONFIGURE_DEPENDS
        "${GSR_TRANSIENT_FUNC15430_PIC_DIR}/recompiled.cpp"
        "${GSR_TRANSIENT_FUNC15430_PIC_DIR}/recompiled_*.cpp"
        "${GSR_TRANSIENT_FUNC15430_PIC_DIR}/dispatch_table.cpp")
    file(GLOB GSR_TRANSIENT_FUNC15570_PIC_SOURCES CONFIGURE_DEPENDS
        "${GSR_TRANSIENT_FUNC15570_PIC_DIR}/recompiled.cpp"
        "${GSR_TRANSIENT_FUNC15570_PIC_DIR}/recompiled_*.cpp"
        "${GSR_TRANSIENT_FUNC15570_PIC_DIR}/dispatch_table.cpp")
    file(GLOB GSR_TRANSIENT_FUNC158E8_PIC_SOURCES CONFIGURE_DEPENDS
        "${GSR_TRANSIENT_FUNC158E8_PIC_DIR}/recompiled.cpp"
        "${GSR_TRANSIENT_FUNC158E8_PIC_DIR}/recompiled_*.cpp"
        "${GSR_TRANSIENT_FUNC158E8_PIC_DIR}/dispatch_table.cpp")
    file(GLOB GSR_TRANSIENT_FUNC2544_PIC_SOURCES CONFIGURE_DEPENDS
        "${GSR_TRANSIENT_FUNC2544_PIC_DIR}/recompiled.cpp"
        "${GSR_TRANSIENT_FUNC2544_PIC_DIR}/recompiled_*.cpp"
        "${GSR_TRANSIENT_FUNC2544_PIC_DIR}/dispatch_table.cpp")
    file(GLOB GSR_TRANSIENT_FUNC2CF4_PIC_SOURCES CONFIGURE_DEPENDS
        "${GSR_TRANSIENT_FUNC2CF4_PIC_DIR}/recompiled.cpp"
        "${GSR_TRANSIENT_FUNC2CF4_PIC_DIR}/recompiled_*.cpp"
        "${GSR_TRANSIENT_FUNC2CF4_PIC_DIR}/dispatch_table.cpp")
    file(GLOB GSR_TRANSIENT_FUNC1DC8_PIC_SOURCES CONFIGURE_DEPENDS
        "${GSR_TRANSIENT_FUNC1DC8_PIC_DIR}/recompiled.cpp"
        "${GSR_TRANSIENT_FUNC1DC8_PIC_DIR}/recompiled_*.cpp"
        "${GSR_TRANSIENT_FUNC1DC8_PIC_DIR}/dispatch_table.cpp")
    file(GLOB GSR_TRANSIENT_FUNC15AFC_PIC_SOURCES CONFIGURE_DEPENDS
        "${GSR_TRANSIENT_FUNC15AFC_PIC_DIR}/recompiled.cpp"
        "${GSR_TRANSIENT_FUNC15AFC_PIC_DIR}/recompiled_*.cpp"
        "${GSR_TRANSIENT_FUNC15AFC_PIC_DIR}/dispatch_table.cpp")
    file(GLOB GSR_TRANSIENT_FUNC15E10_PIC_SOURCES CONFIGURE_DEPENDS
        "${GSR_TRANSIENT_FUNC15E10_PIC_DIR}/recompiled.cpp"
        "${GSR_TRANSIENT_FUNC15E10_PIC_DIR}/recompiled_*.cpp"
        "${GSR_TRANSIENT_FUNC15E10_PIC_DIR}/dispatch_table.cpp")
    file(GLOB GSR_TRANSIENT_FUNC15D74_PIC_SOURCES CONFIGURE_DEPENDS
        "${GSR_TRANSIENT_FUNC15D74_PIC_DIR}/recompiled.cpp"
        "${GSR_TRANSIENT_FUNC15D74_PIC_DIR}/recompiled_*.cpp"
        "${GSR_TRANSIENT_FUNC15D74_PIC_DIR}/dispatch_table.cpp")
    file(GLOB GSR_TRANSIENT_FUNC155D0_PIC_SOURCES CONFIGURE_DEPENDS
        "${GSR_TRANSIENT_FUNC155D0_PIC_DIR}/recompiled.cpp"
        "${GSR_TRANSIENT_FUNC155D0_PIC_DIR}/recompiled_*.cpp"
        "${GSR_TRANSIENT_FUNC155D0_PIC_DIR}/dispatch_table.cpp")
    file(GLOB GSR_TRANSIENT_FUNC9BB8_SHORT_PIC_SOURCES CONFIGURE_DEPENDS
        "${GSR_TRANSIENT_FUNC9BB8_SHORT_PIC_DIR}/recompiled.cpp"
        "${GSR_TRANSIENT_FUNC9BB8_SHORT_PIC_DIR}/recompiled_*.cpp"
        "${GSR_TRANSIENT_FUNC9BB8_SHORT_PIC_DIR}/dispatch_table.cpp")
    file(GLOB GSR_TRANSIENT_FUNC9BB8_CODE_PIC_SOURCES CONFIGURE_DEPENDS
        "${GSR_TRANSIENT_FUNC9BB8_CODE_PIC_DIR}/recompiled.cpp"
        "${GSR_TRANSIENT_FUNC9BB8_CODE_PIC_DIR}/recompiled_*.cpp"
        "${GSR_TRANSIENT_FUNC9BB8_CODE_PIC_DIR}/dispatch_table.cpp")
    file(GLOB GSR_TRANSIENT_FUNCB5138_PIC_SOURCES CONFIGURE_DEPENDS
        "${GSR_TRANSIENT_FUNCB5138_PIC_DIR}/recompiled.cpp"
        "${GSR_TRANSIENT_FUNCB5138_PIC_DIR}/recompiled_*.cpp"
        "${GSR_TRANSIENT_FUNCB5138_PIC_DIR}/dispatch_table.cpp")
    file(GLOB GSR_TRANSIENT_FUNCF0024_PIC_SOURCES CONFIGURE_DEPENDS
        "${GSR_TRANSIENT_FUNCF0024_PIC_DIR}/recompiled.cpp"
        "${GSR_TRANSIENT_FUNCF0024_PIC_DIR}/recompiled_*.cpp"
        "${GSR_TRANSIENT_FUNCF0024_PIC_DIR}/dispatch_table.cpp")
    file(GLOB GSR_TRANSIENT_FUNCA418_PIC_SOURCES CONFIGURE_DEPENDS
        "${GSR_TRANSIENT_FUNCA418_PIC_DIR}/recompiled.cpp"
        "${GSR_TRANSIENT_FUNCA418_PIC_DIR}/recompiled_*.cpp"
        "${GSR_TRANSIENT_FUNCA418_PIC_DIR}/dispatch_table.cpp")
    file(GLOB GSR_TRANSIENT_FUNC1EA8_PIC_SOURCES CONFIGURE_DEPENDS
        "${GSR_TRANSIENT_FUNC1EA8_PIC_DIR}/recompiled.cpp"
        "${GSR_TRANSIENT_FUNC1EA8_PIC_DIR}/recompiled_*.cpp"
        "${GSR_TRANSIENT_FUNC1EA8_PIC_DIR}/dispatch_table.cpp")
    file(GLOB GSR_TRANSIENT_FUNC1EF8_PIC_SOURCES CONFIGURE_DEPENDS
        "${GSR_TRANSIENT_FUNC1EF8_PIC_DIR}/recompiled.cpp"
        "${GSR_TRANSIENT_FUNC1EF8_PIC_DIR}/recompiled_*.cpp"
        "${GSR_TRANSIENT_FUNC1EF8_PIC_DIR}/dispatch_table.cpp")
    file(GLOB GSR_TRANSIENT_FUNC6C24_PIC_SOURCES CONFIGURE_DEPENDS
        "${GSR_TRANSIENT_FUNC6C24_PIC_DIR}/recompiled.cpp"
        "${GSR_TRANSIENT_FUNC6C24_PIC_DIR}/recompiled_*.cpp"
        "${GSR_TRANSIENT_FUNC6C24_PIC_DIR}/dispatch_table.cpp")
    file(GLOB GSR_TRANSIENT_FUNC203C_PIC_SOURCES CONFIGURE_DEPENDS
        "${GSR_TRANSIENT_FUNC203C_PIC_DIR}/recompiled.cpp"
        "${GSR_TRANSIENT_FUNC203C_PIC_DIR}/recompiled_*.cpp"
        "${GSR_TRANSIENT_FUNC203C_PIC_DIR}/dispatch_table.cpp")
    file(GLOB GSR_TRANSIENT_FUNC1F38_PIC_SOURCES CONFIGURE_DEPENDS
        "${GSR_TRANSIENT_FUNC1F38_PIC_DIR}/recompiled.cpp"
        "${GSR_TRANSIENT_FUNC1F38_PIC_DIR}/recompiled_*.cpp"
        "${GSR_TRANSIENT_FUNC1F38_PIC_DIR}/dispatch_table.cpp")
    file(GLOB GSR_TRANSIENT_FUNC1FB8_PIC_SOURCES CONFIGURE_DEPENDS
        "${GSR_TRANSIENT_FUNC1FB8_PIC_DIR}/recompiled.cpp"
        "${GSR_TRANSIENT_FUNC1FB8_PIC_DIR}/recompiled_*.cpp"
        "${GSR_TRANSIENT_FUNC1FB8_PIC_DIR}/dispatch_table.cpp")
    file(GLOB GSR_TRANSIENT_FUNC2098_PIC_SOURCES CONFIGURE_DEPENDS
        "${GSR_TRANSIENT_FUNC2098_PIC_DIR}/recompiled.cpp"
        "${GSR_TRANSIENT_FUNC2098_PIC_DIR}/recompiled_*.cpp"
        "${GSR_TRANSIENT_FUNC2098_PIC_DIR}/dispatch_table.cpp")
    file(GLOB GSR_SYNTH_DC8_030008D4_SOURCES CONFIGURE_DEPENDS
        "${GSR_SYNTH_DC8_030008D4_DIR}/recompiled.cpp"
        "${GSR_SYNTH_DC8_030008D4_DIR}/recompiled_*.cpp"
        "${GSR_SYNTH_DC8_030008D4_DIR}/dispatch_table.cpp")
    file(GLOB GSR_SYNTH_DC8_03000904_SOURCES CONFIGURE_DEPENDS
        "${GSR_SYNTH_DC8_03000904_DIR}/recompiled.cpp"
        "${GSR_SYNTH_DC8_03000904_DIR}/recompiled_*.cpp"
        "${GSR_SYNTH_DC8_03000904_DIR}/dispatch_table.cpp")
    file(GLOB GSR_SYNTH_DC8_030008EC_SOURCES CONFIGURE_DEPENDS
        "${GSR_SYNTH_DC8_030008EC_DIR}/recompiled.cpp"
        "${GSR_SYNTH_DC8_030008EC_DIR}/recompiled_*.cpp"
        "${GSR_SYNTH_DC8_030008EC_DIR}/dispatch_table.cpp")
    set(GSR_PROACTIVE_OVERLAY_SOURCES)
    foreach(GSR_OVERLAY_ID IN LISTS GSR_OVERLAY_IDS)
        set(GSR_OVERLAY_DIR "${GSR_OVERLAY_ROOT}/overlay_${GSR_OVERLAY_ID}")
        file(GLOB GSR_ONE_OVERLAY_SOURCES CONFIGURE_DEPENDS
            "${GSR_OVERLAY_DIR}/recompiled.cpp"
            "${GSR_OVERLAY_DIR}/recompiled_*.cpp"
            "${GSR_OVERLAY_DIR}/dispatch_table.cpp")
        list(APPEND GSR_PROACTIVE_OVERLAY_SOURCES ${GSR_ONE_OVERLAY_SOURCES})
    endforeach()

    set(GSR_ENGINE_SOURCES
        src/runner_main.cpp
        src/function_tracer.cpp
        src/map_recorder.cpp
        src/obj_recorder.cpp
        src/room_buffer.cpp
        src/field_scene.cpp
        src/field_scene_renderer.cpp
        src/world_map_source.cpp
        src/effect_particles.cpp
        src/effect_sources.cpp
        src/effect_capture.cpp
        src/mod_loader.cpp
        src/gs_text_encode.cpp
        src/move_probe.cpp
        src/earth_surge.cpp
        src/effect_burst.cpp
        "${GBARECOMP_ROOT}/src/runtime/gpu_surface.cpp")
    set(GSR_GAME_SOURCES
        ${GSR_GENERATED_SOURCES}
        ${GSR_TRANSIENT_FUNC2D5C_SOURCES}
        ${GSR_TRANSIENT_FUNCA37C_SOURCES}
        ${GSR_TRANSIENT_FUNC2808_03006000_SOURCES}
        ${GSR_TRANSIENT_FUNC6ABC_PIC_SOURCES}
        ${GSR_TRANSIENT_FUNC15E10_03003A84_SOURCES}
        ${GSR_TRANSIENT_FUNCA418_03003400_SOURCES}
        ${GSR_TRANSIENT_FUNC15AFC_03003A84_SOURCES}
        ${GSR_TRANSIENT_FUNC1B70_PIC_SOURCES}
        ${GSR_TRANSIENT_FUNC9E7C_PIC_SOURCES}
        ${GSR_TRANSIENT_FUNCA0F8_PIC_SOURCES}
        ${GSR_TRANSIENT_FUNC7994_PIC_SOURCES}
        ${GSR_TRANSIENT_FUNC6B84_PIC_SOURCES}
        ${GSR_TRANSIENT_FUNC9BB8_PIC_SOURCES}
        ${GSR_TRANSIENT_FUNC15430_PIC_SOURCES}
        ${GSR_TRANSIENT_FUNC15570_PIC_SOURCES}
        ${GSR_TRANSIENT_FUNC158E8_PIC_SOURCES}
        ${GSR_TRANSIENT_FUNC2544_PIC_SOURCES}
        ${GSR_TRANSIENT_FUNC2CF4_PIC_SOURCES}
        ${GSR_TRANSIENT_FUNC1DC8_PIC_SOURCES}
        ${GSR_TRANSIENT_FUNC15AFC_PIC_SOURCES}
        ${GSR_TRANSIENT_FUNC15E10_PIC_SOURCES}
        ${GSR_TRANSIENT_FUNC15D74_PIC_SOURCES}
        ${GSR_TRANSIENT_FUNC155D0_PIC_SOURCES}
        ${GSR_TRANSIENT_FUNC9BB8_SHORT_PIC_SOURCES}
        ${GSR_TRANSIENT_FUNC9BB8_CODE_PIC_SOURCES}
        ${GSR_TRANSIENT_FUNCB5138_PIC_SOURCES}
        ${GSR_TRANSIENT_FUNCF0024_PIC_SOURCES}
        ${GSR_TRANSIENT_FUNCA418_PIC_SOURCES}
        ${GSR_TRANSIENT_FUNC1EA8_PIC_SOURCES}
        ${GSR_TRANSIENT_FUNC1EF8_PIC_SOURCES}
        ${GSR_TRANSIENT_FUNC6C24_PIC_SOURCES}
        ${GSR_TRANSIENT_FUNC203C_PIC_SOURCES}
        ${GSR_TRANSIENT_FUNC1F38_PIC_SOURCES}
        ${GSR_TRANSIENT_FUNC1FB8_PIC_SOURCES}
        ${GSR_TRANSIENT_FUNC2098_PIC_SOURCES}
        ${GSR_SYNTH_DC8_030008D4_SOURCES}
        ${GSR_SYNTH_DC8_03000904_SOURCES}
        ${GSR_SYNTH_DC8_030008EC_SOURCES}
        ${GSR_SYNTH_DC8B_SOURCES}
        ${GSR_STAMPS_SOURCES}
        ${GSR_PROACTIVE_OVERLAY_SOURCES})
    # GSR_SPLIT_GAME_CODE: the engine (our code only) is GoldenSunRecomp.exe
    # and the translated game code is GoldenSunGame.dll beside it, so a
    # release can ship the engine prebuilt and the player's PC builds only
    # the DLL from their ROM (ROADMAP.md, "Player builder"). Windows joins
    # the two when the game starts: the DLL exports the tables the engine
    # reads (GoldenSunGame.def, generated below) and the exe exports
    # everything the game code calls. The default since 2026-09-26 (Jimmy:
    # played through Vale, performs like the single LTO exe); OFF builds
    # the single exe.
    option(GSR_SPLIT_GAME_CODE "Build the game code as GoldenSunGame.dll" ON)
    if(GSR_SPLIT_GAME_CODE)
        add_executable(GoldenSunRecomp ${GSR_ENGINE_SOURCES})
    else()
        add_executable(GoldenSunRecomp
            ${GSR_ENGINE_SOURCES}
            ${GSR_GAME_SOURCES})
    endif()
    if(GSR_IPO_SUPPORTED)
        set_property(TARGET GoldenSunRecomp PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
    # Generated game code calls one shared copy of the RAM/ROM access fast
    # paths instead of inlining them at every load and store (runtime_arm.h,
    # GBARECOMP_OUTLINE_BUS). Same behaviour; much less code to compile and
    # link. Engine sources keep the inline copies.
    set(GSR_GAME_CODE_SOURCES ${GSR_GAME_SOURCES})
    list(FILTER GSR_GAME_CODE_SOURCES INCLUDE REGEX "/recompiled[^/]*\\.cpp$")
    set_property(SOURCE ${GSR_GAME_CODE_SOURCES} APPEND PROPERTY
        COMPILE_DEFINITIONS GBARECOMP_OUTLINE_BUS=1)
    target_include_directories(GoldenSunRecomp PRIVATE
        "${GSR_GENERATED_DIR}"
        "${GBARECOMP_ROOT}/src/armv4t"
        "${GBARECOMP_ROOT}/src/gba"
        "${GBARECOMP_ROOT}/src/runtime"
        "${GBARECOMP_ROOT}/src/debug"
        "${CMAKE_SOURCE_DIR}/config/usa")
    target_compile_definitions(GoldenSunRecomp PRIVATE
        GBARECOMP_DEFAULT_DEBUG_PORT=19843
        GBARECOMP_DEFAULT_GAME_CONFIG="game.toml")

    if(MSVC)
        target_link_libraries(GoldenSunRecomp PRIVATE
            gbarecomp_runtime gbarecomp_debug gbarecomp_gba
            gbarecomp_armv4t)
        target_link_options(GoldenSunRecomp PRIVATE /STACK:16777216)
    else()
        target_compile_options(GoldenSunRecomp PRIVATE
            -ffunction-sections -fdata-sections)
        if(GSR_SPLIT_GAME_CODE)
            # Every engine function the game code might call has to be in
            # the exe even when the engine itself never calls it.
            target_link_libraries(GoldenSunRecomp PRIVATE
                "-Wl,--whole-archive"
                gbarecomp_runtime gbarecomp_debug gbarecomp_gba
                gbarecomp_armv4t
                "-Wl,--no-whole-archive")
        else()
            target_link_libraries(GoldenSunRecomp PRIVATE
                "-Wl,--start-group"
                gbarecomp_runtime gbarecomp_debug gbarecomp_gba
                gbarecomp_armv4t
                "-Wl,--end-group")
        endif()
        target_link_options(GoldenSunRecomp PRIVATE -Wl,--gc-sections)
        if(MINGW OR WIN32)
            target_link_options(GoldenSunRecomp PRIVATE -Wl,--stack,16777216)
        endif()
        if(GSR_IPO_SUPPORTED AND GSR_LTO_JOBS)
            # -flto=N here overrides the plain -flto that
            # INTERPROCEDURAL_OPTIMIZATION already put on this link line,
            # asking GCC to run LTRANS across N parallel processes. See the
            # GSR_LTO_JOBS comment near the top of this file for why this
            # isn't set to the full core count.
            target_link_options(GoldenSunRecomp PRIVATE -flto=${GSR_LTO_JOBS})
        endif()
        # Incremental LTO (GCC 15+): the link splits the program into pieces
        # by a stable rule (-flto-partition=cache) and keeps each finished
        # piece in <build>/lto-cache. A later link recompiles only the pieces
        # whose code changed, so an engine-only change no longer redoes the
        # whole game's final optimisation. The first link after enabling this
        # is a full one. One build per tree at a time (the cache has no file
        # locking on Windows), which build_lto.ps1 already enforces.
        option(GSR_LTO_INCREMENTAL "Cache LTO link pieces between builds" ON)
        if(GSR_IPO_SUPPORTED AND GSR_LTO_INCREMENTAL AND
           CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
           CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 15)
            # Relative to the build directory, where the link runs: GCC's
            # LTO job list is a makefile, and an absolute path here would
            # carry this checkout's spaces into it ("overriding recipe for
            # target 'Sun'", 2026-09-26).
            file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/lto-cache")
            target_link_options(GoldenSunRecomp PRIVATE
                -flto-partition=cache
                -flto-incremental=lto-cache)
        endif()
    endif()

    if(GSR_SPLIT_GAME_CODE AND UNIX AND NOT APPLE)
        # Linux: the engine is GoldenSunRecomp and the game code is
        # libGoldenSunGame.so beside it. ELF needs none of the Windows
        # export/import lists below: the engine links against the library
        # (the player's builder later replaces it with one built from their
        # ROM, with the same symbols), and every engine symbol the library
        # uses is exported automatically because the library references it.
        # The engine is compiled -fPIC, not -fPIE, so it reaches the
        # library's tables through the GOT instead of copy relocations
        # sized from this build's copy.
        foreach(GSR_PIC_TARGET GoldenSunRecomp gbarecomp_armv4t gbarecomp_gba
                gbarecomp_debug gbarecomp_runtime)
            target_compile_options(${GSR_PIC_TARGET} PRIVATE -fPIC)
        endforeach()
        add_library(gsr_game_objects OBJECT ${GSR_GAME_SOURCES})
        set_target_properties(gsr_game_objects PROPERTIES
            INTERPROCEDURAL_OPTIMIZATION FALSE
            POSITION_INDEPENDENT_CODE ON)
        set(GSR_GAME_CODE_OPT "-Og" CACHE STRING
            "Optimisation flag for the game code in a split build")
        # -fno-semantic-interposition and -Bsymbolic: calls between game
        # functions bind inside the library instead of through the PLT.
        target_compile_options(gsr_game_objects PRIVATE
            ${GSR_GAME_CODE_OPT} -g0 -fno-semantic-interposition)
        target_include_directories(gsr_game_objects PRIVATE
            "${GSR_GENERATED_DIR}"
            "${GBARECOMP_ROOT}/src/armv4t"
            "${GBARECOMP_ROOT}/src/gba"
            "${GBARECOMP_ROOT}/src/runtime"
            "${GBARECOMP_ROOT}/src/debug"
            "${CMAKE_SOURCE_DIR}/config/usa")
        add_library(GoldenSunGame SHARED $<TARGET_OBJECTS:gsr_game_objects>)
        set_target_properties(GoldenSunGame PROPERTIES
            INTERPROCEDURAL_OPTIMIZATION FALSE
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
        target_link_options(GoldenSunGame PRIVATE -Wl,-Bsymbolic)
        target_link_libraries(GoldenSunRecomp PRIVATE GoldenSunGame)
        # The library is found beside the engine and nowhere else (no build
        # path baked in). No ENABLE_EXPORTS: only the engine symbols the
        # library calls are exported (about fifty runtime_*/g_* names; the
        # player's library, from the same SHA-1-gated ROM, calls the same
        # ones), so nothing the engine defines can interpose on the system's
        # graphics drivers. libstdc++ and libgcc are linked in, so the
        # engine runs on distros with an older GCC.
        set_target_properties(GoldenSunRecomp PROPERTIES
            BUILD_WITH_INSTALL_RPATH ON INSTALL_RPATH "$ORIGIN")
        target_link_options(GoldenSunRecomp PRIVATE -static-libstdc++ -static-libgcc)
    elseif(GSR_SPLIT_GAME_CODE AND NOT MSVC)
        # The tables the engine reads from the game code, by name. Every
        # dispatch_table*.cpp (and the stamps' stamp_registry.cpp) defines
        # them as `extern "C" const ... NAME`.
        set(GSR_GAME_DEF "${CMAKE_BINARY_DIR}/GoldenSunGame.def")
        set(GSR_GAME_DEF_TEXT "LIBRARY GoldenSunGame.dll\nEXPORTS\n")
        set(GSR_GAME_TABLE_FILES ${GSR_GAME_SOURCES})
        list(FILTER GSR_GAME_TABLE_FILES INCLUDE REGEX "/(dispatch_table|stamp_registry)[^/]*\\.cpp$")
        foreach(GSR_TABLE_FILE IN LISTS GSR_GAME_TABLE_FILES)
            file(STRINGS "${GSR_TABLE_FILE}" GSR_TABLE_LINES
                REGEX "^extern \"C\" const ")
            foreach(GSR_TABLE_LINE IN LISTS GSR_TABLE_LINES)
                if(GSR_TABLE_LINE MATCHES "([A-Za-z_][A-Za-z0-9_]*)(\\[\\])? =")
                    string(APPEND GSR_GAME_DEF_TEXT "    ${CMAKE_MATCH_1} DATA\n")
                endif()
            endforeach()
        endforeach()
        file(WRITE "${GSR_GAME_DEF}.tmp" "${GSR_GAME_DEF_TEXT}")
        file(COPY_FILE "${GSR_GAME_DEF}.tmp" "${GSR_GAME_DEF}" ONLY_IF_DIFFERENT)

        # The exe imports those tables from GoldenSunGame.dll through an
        # import library made from the .def alone, so the exe never waits
        # on (or depends on) the DLL being built.
        set(GSR_GAME_IMPLIB "${CMAKE_BINARY_DIR}/libGoldenSunGame_tables.a")
        find_program(GSR_DLLTOOL dlltool REQUIRED)
        add_custom_command(OUTPUT "${GSR_GAME_IMPLIB}"
            COMMAND "${GSR_DLLTOOL}" -d "${GSR_GAME_DEF}" -l "${GSR_GAME_IMPLIB}"
                    -D GoldenSunGame.dll
            DEPENDS "${GSR_GAME_DEF}"
            COMMENT "Import library for GoldenSunGame.dll's tables")
        add_custom_target(gsr_game_tables_implib DEPENDS "${GSR_GAME_IMPLIB}")
        add_dependencies(GoldenSunRecomp gsr_game_tables_implib)
        target_link_libraries(GoldenSunRecomp PRIVATE "${GSR_GAME_IMPLIB}")

        # The game code: no LTO (it calls shared engine helpers, which is
        # what LTO used to copy into it), compiled once and reused until the
        # translated code changes. Objects first, so the exe's export list
        # can be read from them before the exe links.
        add_library(gsr_game_objects OBJECT ${GSR_GAME_SOURCES})
        set_target_properties(gsr_game_objects PROPERTIES
            INTERPROCEDURAL_OPTIMIZATION FALSE)
        # This compile is what a player's PC waits for. Per file
        # (recompiled_005, 2026-09-26): -O2 -g 31.6 s, -O2 22.9 s, -O1
        # 14.5 s, -Og 8.3 s, -O0 5.6 s. No debug information: it made the DLL
        # 567 MB and nothing reads it.
        set(GSR_GAME_CODE_OPT "-Og" CACHE STRING
            "Optimisation flag for the game code in a split build")
        target_compile_options(gsr_game_objects PRIVATE
            ${GSR_GAME_CODE_OPT} -g0)
        target_include_directories(gsr_game_objects PRIVATE
            "${GSR_GENERATED_DIR}"
            "${GBARECOMP_ROOT}/src/armv4t"
            "${GBARECOMP_ROOT}/src/gba"
            "${GBARECOMP_ROOT}/src/runtime"
            "${GBARECOMP_ROOT}/src/debug"
            "${CMAKE_SOURCE_DIR}/config/usa")
        add_library(GoldenSunGame SHARED $<TARGET_OBJECTS:gsr_game_objects>)
        set_target_properties(GoldenSunGame PROPERTIES
            PREFIX ""
            INTERPROCEDURAL_OPTIMIZATION FALSE
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
        set(GSR_ENGINE_API_IMPLIB "${CMAKE_BINARY_DIR}/libGoldenSunRecomp_api.a")
        add_dependencies(GoldenSunGame GoldenSunRecomp)
        target_link_libraries(GoldenSunGame PRIVATE "${GSR_ENGINE_API_IMPLIB}")
        target_link_options(GoldenSunGame PRIVATE "${GSR_GAME_DEF}")

        # What the exe exports: every symbol the game objects use that the
        # engine defines (cmake/gsr_engine_exports.cmake), worked out right
        # before the exe links. Exporting everything instead fails under
        # LTO ("cannot export ...: symbol wrong type").
        # gcc-nm, not nm: the engine objects hold LTO bytecode, which only
        # the compiler's plugin can list.
        find_program(GSR_GCC_NM gcc-nm REQUIRED)
        set(GSR_ENGINE_EXPORTS "${CMAKE_BINARY_DIR}/GoldenSunRecomp_exports.def")
        set(GSR_ENGINE_EXPORTS_RSP "${CMAKE_BINARY_DIR}/GoldenSunRecomp_exports.rsp")
        file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/gsr_game_objects.txt"
            CONTENT "$<JOIN:$<TARGET_OBJECTS:gsr_game_objects>,\n>\n")
        file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/gsr_engine_inputs.txt"
            CONTENT "$<TARGET_FILE:gbarecomp_runtime>\n$<TARGET_FILE:gbarecomp_debug>\n$<TARGET_FILE:gbarecomp_gba>\n$<TARGET_FILE:gbarecomp_armv4t>\n$<JOIN:$<TARGET_OBJECTS:GoldenSunRecomp>,\n>\n")
        add_dependencies(GoldenSunRecomp gsr_game_objects)
        add_custom_command(TARGET GoldenSunRecomp PRE_LINK
            COMMAND "${CMAKE_COMMAND}"
                "-DNM=${GSR_GCC_NM}"
                "-DGAME_LIST=${CMAKE_BINARY_DIR}/gsr_game_objects.txt"
                "-DENGINE_LIST=${CMAKE_BINARY_DIR}/gsr_engine_inputs.txt"
                "-DOUT=${GSR_ENGINE_EXPORTS}"
                "-DOUT_RSP=${GSR_ENGINE_EXPORTS_RSP}"
                -P "${CMAKE_SOURCE_DIR}/cmake/gsr_engine_exports.cmake"
            # The DLL's import library comes from the same list. The one ld
            # writes with --out-implib under LTO leaves out exports the
            # engine itself never calls (2026-09-26).
            COMMAND "${GSR_DLLTOOL}" -d "${GSR_ENGINE_EXPORTS}"
                -l "${GSR_ENGINE_API_IMPLIB}" -D GoldenSunRecomp.exe
            BYPRODUCTS "${GSR_ENGINE_EXPORTS}" "${GSR_ENGINE_EXPORTS_RSP}"
                       "${GSR_ENGINE_API_IMPLIB}"
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
            VERBATIM)
        target_link_options(GoldenSunRecomp PRIVATE "${GSR_ENGINE_EXPORTS}"
            "@${GSR_ENGINE_EXPORTS_RSP}")
    endif()

    add_test(NAME runner_help COMMAND GoldenSunRecomp --help)

    # Split debug symbols out of the executable so the shipped binary stays
    # small while crash/hang stack traces can still resolve against the
    # separate .debug file (GNU objcopy debuglink convention).
    if(GSR_DEBUG_SYMBOLS AND CMAKE_BUILD_TYPE MATCHES "^(Debug|RelWithDebInfo)$")
        find_program(CMAKE_OBJCOPY objcopy)
        if(CMAKE_OBJCOPY)
            add_custom_command(TARGET GoldenSunRecomp POST_BUILD
                COMMAND "${CMAKE_OBJCOPY}" --only-keep-debug "$<TARGET_FILE:GoldenSunRecomp>" "$<TARGET_FILE:GoldenSunRecomp>.debug"
                COMMAND "${CMAKE_OBJCOPY}" --strip-all "$<TARGET_FILE:GoldenSunRecomp>"
                COMMAND "${CMAKE_OBJCOPY}" --add-gnu-debuglink="$<TARGET_FILE_NAME:GoldenSunRecomp>.debug" "$<TARGET_FILE:GoldenSunRecomp>"
                WORKING_DIRECTORY "$<TARGET_FILE_DIR:GoldenSunRecomp>"
                COMMENT "Splitting debug symbols for GoldenSunRecomp")
        else()
            message(STATUS "objcopy not found; skipping debug-symbol split for GoldenSunRecomp")
        endif()
    endif()

    # The player builder (tools/gsr_builder): translates the player's ROM
    # with gba_recompile and compiles the game code with the bundled GCC.
    set(GSR_BUILDER_SOURCES
        tools/gsr_builder/builder_main.cpp
        tools/gsr_builder/guest_call.cpp
        tools/gsr_builder/block_timing.cpp
        "${GBARECOMP_ROOT}/src/armv4t/interpreter.cpp"
        "${GBARECOMP_ROOT}/src/armv4t/arm_decode.cpp"
        "${GBARECOMP_ROOT}/src/armv4t/thumb_decode.cpp"
        "${GBARECOMP_ROOT}/src/armv4t/arm_ir.cpp"
        "${GBARECOMP_ROOT}/src/gba/sha1.cpp")

    # The launcher's logo (assets/launcher_logo.png), built into the exe as a
    # byte array so the developer and player launchers (Windows and Linux)
    # all show it without a loose image file. Rewritten only when the PNG
    # changes.
    set(GSR_LAUNCHER_LOGO "${CMAKE_SOURCE_DIR}/assets/launcher_logo.png")
    set(GSR_LAUNCHER_LOGO_H "${CMAKE_BINARY_DIR}/generated/launcher_logo.h")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${GSR_LAUNCHER_LOGO}")
    file(SHA1 "${GSR_LAUNCHER_LOGO}" gsr_logo_sha1)
    if(NOT EXISTS "${GSR_LAUNCHER_LOGO_H}" OR
       NOT gsr_logo_sha1 STREQUAL "${GSR_LAUNCHER_LOGO_SHA1}")
        file(READ "${GSR_LAUNCHER_LOGO}" gsr_logo_hex HEX)
        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," gsr_logo_bytes
            "${gsr_logo_hex}")
        file(WRITE "${GSR_LAUNCHER_LOGO_H}"
            "// Generated from assets/launcher_logo.png by CMakeLists.txt.\n"
            "#pragma once\n"
            "static const unsigned char kLauncherLogoPng[] = {${gsr_logo_bytes}};\n")
        set(GSR_LAUNCHER_LOGO_SHA1 "${gsr_logo_sha1}" CACHE INTERNAL
            "SHA-1 of the logo the generated header was written from")
    endif()

    # The release's version, the tag of its GitHub release (make_release sets
    # it). A launcher built as "dev" never offers updates (launcher_online.h).
    set(GSR_RELEASE_VERSION "dev" CACHE STRING
        "Release version (GitHub release tag) the launcher reports and updates from")

    if(WIN32)
        add_executable(GoldenSunLauncher WIN32
            src/launcher_main.cpp
            "${GBARECOMP_ROOT}/src/runtime/crash_handler.cpp")
        target_include_directories(GoldenSunLauncher PRIVATE
            "${GBARECOMP_ROOT}/src/runtime"
            "${CMAKE_BINARY_DIR}/generated")
        target_compile_definitions(GoldenSunLauncher PRIVATE
            UNICODE _UNICODE NOMINMAX
            GSR_RELEASE_VERSION="${GSR_RELEASE_VERSION}")
        target_link_libraries(GoldenSunLauncher PRIVATE bcrypt comdlg32 gdiplus
            shlwapi winhttp)
        # make_release.bat: a player launcher with only Pick ROM and Quit,
        # built into its own build tree so it never replaces the developer
        # launcher in the repository root.
        option(GSR_RELEASE_LAUNCHER
            "Build the player launcher (Pick ROM and Quit only)" OFF)
        # Where the developer launcher lands. The repository root by default;
        # a comparison build (build_nolto_test.bat) keeps its own copy in its
        # build tree so it never replaces the one you play from.
        set(GSR_LAUNCHER_OUTPUT_DIR "${CMAKE_SOURCE_DIR}" CACHE PATH
            "Output directory for the developer GoldenSunLauncher.exe")
        if(GSR_RELEASE_LAUNCHER)
            target_compile_definitions(GoldenSunLauncher PRIVATE
                GSR_RELEASE_LAUNCHER=1)
        else()
            set_target_properties(GoldenSunLauncher PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY "${GSR_LAUNCHER_OUTPUT_DIR}")
        endif()
        add_dependencies(GoldenSunRecomp GoldenSunLauncher)

        # The player builder (tools/gsr_builder): translates the player's
        # ROM with gba_recompile and compiles GoldenSunGame.dll with the
        # bundled GCC. make_release.bat ships it; static so it needs no DLLs.
        if(NOT MSVC)
            add_executable(gsr_builder ${GSR_BUILDER_SOURCES})
            target_include_directories(gsr_builder PRIVATE
                "${GBARECOMP_ROOT}/src/armv4t"
                "${GBARECOMP_ROOT}/src/gba")
            set_target_properties(gsr_builder PROPERTIES
                INTERPROCEDURAL_OPTIMIZATION FALSE)
            target_link_options(gsr_builder PRIVATE -municode -static)
        endif()
    elseif(UNIX AND NOT APPLE)
        # Linux release: the same builder, producing libGoldenSunGame.so.
        # libstdc++ and libgcc are linked in so it runs on any glibc distro.
        add_executable(gsr_builder ${GSR_BUILDER_SOURCES})
        target_include_directories(gsr_builder PRIVATE
            "${GBARECOMP_ROOT}/src/armv4t"
            "${GBARECOMP_ROOT}/src/gba")
        set_target_properties(gsr_builder PROPERTIES
            INTERPROCEDURAL_OPTIMIZATION FALSE)
        target_link_options(gsr_builder PRIVATE -static-libstdc++ -static-libgcc)

        # The Linux launcher (src/launcher_linux.cpp): its own SDL2 + ImGui
        # window, the same jobs as the Windows player launcher.
        find_package(ZLIB)
        find_package(Threads REQUIRED)
        if(TARGET gbarecomp_imgui AND ZLIB_FOUND)
            add_executable(GoldenSunLauncher
                src/launcher_linux.cpp
                "${GBARECOMP_ROOT}/src/gba/sha1.cpp"
                "${GBARECOMP_ROOT}/src/runtime/crash_handler.cpp")
            target_include_directories(GoldenSunLauncher PRIVATE
                "${GBARECOMP_ROOT}/src/runtime"
                "${GBARECOMP_ROOT}/src/gba"
                "${CMAKE_BINARY_DIR}/generated")
            set_target_properties(GoldenSunLauncher PROPERTIES
                INTERPROCEDURAL_OPTIMIZATION FALSE)
            target_compile_definitions(GoldenSunLauncher PRIVATE
                GSR_RELEASE_VERSION="${GSR_RELEASE_VERSION}")
            target_link_libraries(GoldenSunLauncher PRIVATE gbarecomp_imgui
                ZLIB::ZLIB Threads::Threads)
            target_link_options(GoldenSunLauncher PRIVATE
                -static-libstdc++ -static-libgcc)
        else()
            message(STATUS "GoldenSunLauncher (Linux) skipped: needs SDL2/ImGui and zlib")
        endif()
    endif()

endif()
