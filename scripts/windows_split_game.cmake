# Link a MinGW engine plug-in to its translated game DLL. The caller owns
# the game object target and selects which sources define the exported tables.
function(gsr_windows_split_game prefix engine game engine_dll table_files)
    set(game_objects "${prefix}_game_objects")
    set(game_dll "${game}.dll")
    set(game_def "${CMAKE_BINARY_DIR}/${game}.def")
    set(game_def_text "LIBRARY ${game_dll}\nEXPORTS\n")
    foreach(table_file IN LISTS table_files)
        file(STRINGS "${table_file}" table_lines REGEX "^extern \"C\" const ")
        foreach(line IN LISTS table_lines)
            if(line MATCHES "([A-Za-z_][A-Za-z0-9_]*)(\\[\\])? =")
                string(APPEND game_def_text "    ${CMAKE_MATCH_1} DATA\n")
            endif()
        endforeach()
    endforeach()
    file(WRITE "${game_def}.tmp" "${game_def_text}")
    file(COPY_FILE "${game_def}.tmp" "${game_def}" ONLY_IF_DIFFERENT)

    string(TOUPPER "${prefix}" tool_prefix)
    find_program(${tool_prefix}_DLLTOOL dlltool REQUIRED)
    find_program(${tool_prefix}_GCC_NM gcc-nm REQUIRED)
    set(dlltool "${${tool_prefix}_DLLTOOL}")

    # The engine can import the tables before the game DLL has been linked.
    set(game_implib "${CMAKE_BINARY_DIR}/lib${game}_tables.a")
    add_custom_command(OUTPUT "${game_implib}"
        COMMAND "${dlltool}" -d "${game_def}" -l "${game_implib}" -D "${game_dll}"
        DEPENDS "${game_def}"
        COMMENT "Import library for ${game_dll}'s tables")
    add_custom_target(${prefix}_game_tables_implib DEPENDS "${game_implib}")
    add_dependencies(${engine} ${prefix}_game_tables_implib)
    target_link_libraries(${engine} PRIVATE "${game_implib}")

    set(engine_implib "${CMAKE_BINARY_DIR}/lib${engine}_api.a")
    add_library(${game} SHARED $<TARGET_OBJECTS:${game_objects}>)
    set_target_properties(${game} PROPERTIES
        PREFIX ""
        INTERPROCEDURAL_OPTIMIZATION FALSE
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_dependencies(${game} ${engine})
    target_link_libraries(${game} PRIVATE "${engine_implib}")
    target_link_options(${game} PRIVATE "${game_def}")

    # gcc-nm can read LTO objects. Export only the engine symbols the game
    # uses, and generate the game's import library from that same list.
    set(game_list "${CMAKE_BINARY_DIR}/${game_objects}.txt")
    set(engine_list "${CMAKE_BINARY_DIR}/${prefix}_engine_inputs.txt")
    set(engine_def "${CMAKE_BINARY_DIR}/${engine}_exports.def")
    set(engine_rsp "${CMAKE_BINARY_DIR}/${engine}_exports.rsp")
    file(GENERATE OUTPUT "${game_list}"
        CONTENT "$<JOIN:$<TARGET_OBJECTS:${game_objects}>,\n>\n")
    file(GENERATE OUTPUT "${engine_list}"
        CONTENT "$<TARGET_FILE:gbarecomp_runtime>\n$<TARGET_FILE:gbarecomp_debug>\n$<TARGET_FILE:gbarecomp_gba>\n$<TARGET_FILE:gbarecomp_armv4t>\n$<JOIN:$<TARGET_OBJECTS:${engine}>,\n>\n")
    add_dependencies(${engine} ${game_objects})
    add_custom_command(TARGET ${engine} PRE_LINK
        COMMAND "${CMAKE_COMMAND}"
            "-DNM=${${tool_prefix}_GCC_NM}"
            "-DGAME_LIST=${game_list}"
            "-DENGINE_LIST=${engine_list}"
            "-DOUT=${engine_def}"
            "-DOUT_RSP=${engine_rsp}"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/engine_exports.cmake"
        COMMAND "${dlltool}" -d "${engine_def}" -l "${engine_implib}" -D "${engine_dll}"
        BYPRODUCTS "${engine_def}" "${engine_rsp}" "${engine_implib}"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        VERBATIM)
    target_link_options(${engine} PRIVATE "${engine_def}" "@${engine_rsp}")
endfunction()
