# Split build (GSR_SPLIT_GAME_CODE): writes the .def of everything
# GoldenSunRecomp.exe must export for GoldenSunGame.dll, namely every symbol
# the game-code objects use but do not define, that the engine defines.
#
#   cmake -DNM=<gcc-nm> -DGAME_LIST=<file> -DENGINE_LIST=<file> -DOUT=<def> -P this
#
# GAME_LIST / ENGINE_LIST hold one object or archive path per line.
#
# Kept to string searches: the game objects hold hundreds of thousands of
# symbols, and comparing CMake lists item by item took many minutes.

foreach(var NM GAME_LIST ENGINE_LIST OUT)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "gsr_engine_exports.cmake: ${var} is not set")
    endif()
endforeach()

# Run nm over every path in a list file (through a response file: hundreds
# of paths overflow the Windows command line) and return its output.
function(gsr_nm list_file extra_args out_text)
    file(STRINGS "${list_file}" paths)
    set(rsp "${list_file}.rsp")
    set(text "")
    foreach(p IN LISTS paths)
        string(APPEND text "\"${p}\"\n")
    endforeach()
    file(WRITE "${rsp}" "${text}")
    execute_process(
        COMMAND "${NM}" -P ${extra_args} "@${rsp}"
        OUTPUT_VARIABLE nm_out
        ERROR_VARIABLE nm_err
        RESULT_VARIABLE nm_rc)
    if(NOT nm_rc EQUAL 0)
        message(FATAL_ERROR "nm failed (${nm_rc}): ${nm_err}")
    endif()
    set(${out_text} "\n${nm_out}" PARENT_SCOPE)
endfunction()

# Names the game code uses from outside itself. Its own functions and
# tables (gf_*, gsr_*, k*) are cross-references between game objects, so
# they are skipped before the (few) remaining names are looked up.
gsr_nm("${GAME_LIST}" "-u" game_undefined_text)
string(REGEX MATCHALL "\n[A-Za-z_][A-Za-z0-9_@$.]* U" undefined_lines
       "${game_undefined_text}")
set(candidates "")
foreach(line IN LISTS undefined_lines)
    string(REGEX REPLACE "^\n([^ ]+) U$" "\\1" name "${line}")
    if(NOT name MATCHES "^(gf_|gsr_|k[A-Z])")
        list(APPEND candidates "${name}")
    endif()
endforeach()
list(REMOVE_DUPLICATES candidates)

# Keep the ones the engine defines (a defined-symbol line is "name T ...",
# "name D ...", and so on; anything but U, w, v).
gsr_nm("${ENGINE_LIST}" "--defined-only" engine_text)
# Variables are marked DATA, so their import library entry is the address
# slot only; a code thunk under a variable's name would be read as the
# variable.
set(exports "")
foreach(name IN LISTS candidates)
    string(REGEX MATCH "\n${name} ([A-TX-Zabd-uxyz]) " hit "${engine_text}")
    if(hit)
        if(CMAKE_MATCH_1 MATCHES "^[Tt]$")
            list(APPEND exports "${name}")
        else()
            list(APPEND exports "${name} DATA")
        endif()
    endif()
endforeach()
list(SORT exports)
list(LENGTH candidates n_candidates)
list(LENGTH exports n)

# No LIBRARY line: given to the exe's own link, it marks the output a DLL
# and Windows refuses to start it. dlltool gets the name from -D instead.
set(text "EXPORTS\n")
foreach(name IN LISTS exports)
    string(APPEND text "    ${name}\n")
endforeach()
file(WRITE "${OUT}.tmp" "${text}")
file(COPY_FILE "${OUT}.tmp" "${OUT}" ONLY_IF_DIFFERENT)

# The .def alone does not keep LTO from discarding functions the engine
# never calls itself: they were exported with the address 0xC0000000 and the
# DLL crashed while starting (2026-09-26). --undefined marks each one as
# needed from outside the LTO code. OUT_RSP is a gcc response file on the
# exe's link line.
if(DEFINED OUT_RSP)
    set(rsp "")
    foreach(entry IN LISTS exports)
        string(REGEX REPLACE " DATA$" "" name "${entry}")
        string(APPEND rsp "-Wl,--undefined=${name}\n")
    endforeach()
    file(WRITE "${OUT_RSP}.tmp" "${rsp}")
    file(COPY_FILE "${OUT_RSP}.tmp" "${OUT_RSP}" ONLY_IF_DIFFERENT)
endif()
message(STATUS "GoldenSunRecomp.exe exports ${n} of ${n_candidates} symbols the game code uses from outside itself")
