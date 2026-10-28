# Windows programs run with UTF-8 as their code page (activeCodePage in
# windows_utf8.manifest, Windows 10 1903 and later), so argv, getenv and the
# narrow file, process and library calls take the same UTF-8 paths as
# std::filesystem. The collection then works from a folder with any name
# (measured 2026-10-27: from a Cyrillic folder every narrow call failed
# without it and worked with it). Older Windows ignore the setting.
#
#   include(../scripts/windows_utf8.cmake)
#   gsr_windows_utf8(<target>...)   # executables only; others are skipped

set(GSR_WINDOWS_UTF8_MANIFEST "${CMAKE_CURRENT_LIST_DIR}/windows_utf8.manifest")
if(WIN32 AND NOT MSVC)
    enable_language(RC)
endif()

function(gsr_windows_utf8)
    if(NOT WIN32 OR MSVC)
        return()
    endif()
    # Our manifest replaces MinGW's default-manifest.o: the linker keeps the
    # first manifest resource, and ours comes before it.
    set(rc "${CMAKE_BINARY_DIR}/generated/windows_utf8.rc")
    file(CONFIGURE OUTPUT "${rc}"
        CONTENT "1 24 \"${GSR_WINDOWS_UTF8_MANIFEST}\"\n")
    set_source_files_properties("${rc}" PROPERTIES
        OBJECT_DEPENDS "${GSR_WINDOWS_UTF8_MANIFEST}")
    foreach(target IN LISTS ARGN)
        if(TARGET ${target})
            get_target_property(type ${target} TYPE)
            if(type STREQUAL "EXECUTABLE")
                target_sources(${target} PRIVATE "${rc}")
            endif()
        endif()
    endforeach()
endfunction()
