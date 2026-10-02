# tide_add_game(<target> [SOURCES <file.tide|file.c>...] [HOST <file.c>...] [NAME <name>]
#               [TITLE <title>] [STATS] [LAYOUT] [WARNINGS <text>...])
#
# Builds a Tide game as the program <target>. The game is every .tide file
# in the current source folder and its subfolders (new ones are picked up by
# the next build), or only the files listed after SOURCES. Its C files, which
# define its extern functions, compile with it: every .c file in the folder
# but the HOST ones, or the .c files listed after SOURCES.
#
# Without HOST, the game is the whole program: it opens a window titled TITLE,
# or the game's title setting, or <target>, and runs (see
# platform/include/tide/run.h). STATS shows the frame rate, ping, bandwidth
# (what goes over the network each second, up and down), tick and entity
# count in a corner. On the web it's <target>.html.
#
# With HOST, those C files are the program instead, for tests and custom hosts.
# They include the game's generated header, <name>.h (NAME defaults to
# <target>), and drive the game through its API (see AGENTS.md). LAYOUT also
# describes the game's data layout, as tide run does for hot reloading
# (tide_game_layout, see engine/include/tide/layout.h).
#
# The generated files land in the build tree and are regenerated whenever a
# .tide file or tidec changes. Every game is also listed in
# TIDE_GAMES_MANIFEST, so editors know which files belong together.
#
# tidec's warnings are errors here (cmake/run_tidec.cmake), as the compiler's
# are. WARNINGS lists the ones a game means to have, such as tests of what
# tidec generates for programs it warns about: each is part of a warning's
# text, and has to be there.

set(TIDE_GAMES_MANIFEST "${PROJECT_SOURCE_DIR}/build/tools/games.txt")
set(TIDE_RUN_TIDEC "${CMAKE_CURRENT_LIST_DIR}/run_tidec.cmake")

function(tide_add_game target)
    cmake_parse_arguments(ARG "STATS;LAYOUT" "NAME;TITLE" "SOURCES;HOST;WARNINGS" ${ARGN})
    if("SOURCES" IN_LIST ARG_KEYWORDS_MISSING_VALUES)
        message(FATAL_ERROR "tide_add_game(${target}): list the game's .tide files after SOURCES, or leave "
                            "SOURCES out to use every .tide file in ${CMAKE_CURRENT_SOURCE_DIR} and its subfolders")
    endif()
    set(name "${target}")
    if(ARG_NAME)
        set(name "${ARG_NAME}")
    endif()
    # TITLE goes over the game's title setting; the target's name only stands in for it
    set(title_field "game_name")
    set(title "${target}")
    if(ARG_TITLE)
        set(title_field "title")
        set(title "${ARG_TITLE}")
    endif()

    set(sources "")
    set(c_sources "")
    if(ARG_SOURCES)
        foreach(file IN LISTS ARG_SOURCES)
            get_filename_component(path "${file}" ABSOLUTE)
            if(path MATCHES "\\.c$")
                list(APPEND c_sources "${path}")
            else()
                list(APPEND sources "${path}")
            endif()
        endforeach()
        set(manifest_paths ${sources})
    else()
        # CONFIGURE_DEPENDS: every build checks for new or deleted files first.
        file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.tide")
        if(NOT sources)
            message(FATAL_ERROR "tide_add_game(${target}): there are no .tide files in ${CMAKE_CURRENT_SOURCE_DIR} "
                                "or its subfolders. Add one, or list the game's files after SOURCES")
        endif()
        # Not in hidden folders or build/, where tide keeps what it makes (.tide/),
        # generated C included.
        file(GLOB_RECURSE found CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.c")
        foreach(path IN LISTS found)
            file(RELATIVE_PATH relative "${CMAKE_CURRENT_SOURCE_DIR}" "${path}")
            if(NOT relative MATCHES "(^|/)\\." AND NOT relative MATCHES "^build/")
                list(APPEND c_sources "${path}")
            endif()
        endforeach()
        foreach(file IN LISTS ARG_HOST)
            get_filename_component(path "${file}" ABSOLUTE)
            list(REMOVE_ITEM c_sources "${path}")
        endforeach()
        # The folder rather than its files, so editors see new files before the next build.
        set(manifest_paths "${CMAKE_CURRENT_SOURCE_DIR}/")
    endif()

    set(layout "")
    if(ARG_LAYOUT)
        set(layout --layout)
    endif()

    set(out_dir "${CMAKE_CURRENT_BINARY_DIR}/tide/${target}")
    set(out_c "${out_dir}/${name}.c")
    set(out_h "${out_dir}/${name}.h")
    # The warnings it means to have, written only when they change, so tidec
    # runs again when they do.
    set(expected "${CMAKE_CURRENT_BINARY_DIR}/tide/${target}.warnings")
    list(JOIN ARG_WARNINGS "\n" warnings)
    set(written "-")
    if(EXISTS "${expected}")
        file(READ "${expected}" written)
    endif()
    if(NOT written STREQUAL warnings)
        file(WRITE "${expected}" "${warnings}")
    endif()
    add_custom_command(
        OUTPUT "${out_c}" "${out_h}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${out_dir}"
        COMMAND "${CMAKE_COMMAND}" "-DTIDEC=${TIDEC_PATH}" "-DEXPECTED=${expected}" -P "${TIDE_RUN_TIDEC}"
            -- ${sources} -o "${out_dir}" --name "${name}" ${layout}
        DEPENDS ${TIDEC_DEPENDS} ${sources} "${expected}" "${TIDE_RUN_TIDEC}"
        COMMENT "tidec ${target}"
        VERBATIM)

    if(ARG_HOST)
        add_executable(${target} ${ARG_HOST} "${out_c}" "${out_h}" ${c_sources})
        target_link_libraries(${target} PRIVATE tide)
    else()
        if(NOT TARGET tide_platform)
            message(FATAL_ERROR "tide_add_game(${target}): a game without HOST opens a window, which needs the "
                                "platform layer (TIDE_PLATFORM=ON, and platform/ added before this directory)")
        endif()
        set(stats false)
        if(ARG_STATS)
            set(stats true)
        endif()
        set(main "${out_dir}/main.c")
        file(CONFIGURE OUTPUT "${main}" @ONLY CONTENT [[
// Generated by tide_add_game for @target@. Do not edit.
#include "@name@.h"
#include "tide/run.h"

int main(int argc, char **argv)
{
    tide_run(&(tide_run_desc){.@title_field@ = "@title@", .stats = @stats@, .argc = argc, .argv = argv});
}
]])
        add_executable(${target} "${main}" "${out_c}" "${out_h}" ${c_sources})
        target_link_libraries(${target} PRIVATE tide_platform)
        tide_web_page(${target})
    endif()
    target_include_directories(${target} PRIVATE "${out_dir}")

    # `cmake --build --preset <preset> --target <target>_schedule` prints which
    # systems can run at the same time, and why the others wait.
    add_custom_target(${target}_schedule
        COMMAND ${TIDEC_COMMAND} ${sources} --schedule --name "${name}"
        DEPENDS ${TIDEC_DEPENDS}
        VERBATIM
        USES_TERMINAL)

    foreach(path IN LISTS manifest_paths)
        set_property(GLOBAL APPEND PROPERTY TIDE_GAME_LINES "${target}\t${path}")
    endforeach()
    get_property(scheduled GLOBAL PROPERTY TIDE_GAMES_MANIFEST_SCHEDULED)
    if(NOT scheduled)
        set_property(GLOBAL PROPERTY TIDE_GAMES_MANIFEST_SCHEDULED TRUE)
        cmake_language(DEFER DIRECTORY "${PROJECT_SOURCE_DIR}" CALL tide_write_games_manifest)
    endif()
endfunction()

# Lists every game and its files, one "<game>\t<path>" per line, once all the
# games are known. A path ending in '/' is a folder: every .tide file in it and
# its subfolders. tidels reads it to analyze a game's files together.
function(tide_write_games_manifest)
    get_property(lines GLOBAL PROPERTY TIDE_GAME_LINES)
    list(JOIN lines "\n" content)
    file(CONFIGURE OUTPUT "${TIDE_GAMES_MANIFEST}" CONTENT "${content}\n" @ONLY)
endfunction()
