# End-to-end tests: e2e/<name>.purr (or the SOURCES given) is transpiled and
# compiled together with e2e/<name>_test.c and the shared test harness, then run.
function(purr_e2e_test name)
    cmake_parse_arguments(ARG "" "" "SOURCES" ${ARGN})
    set(sources "e2e/${name}.purr")
    if(ARG_SOURCES)
        set(sources ${ARG_SOURCES})
    endif()
    set(target purrc_e2e_${name})
    purr_add_game(${target} SOURCES ${sources} NAME game
        HOST "${PROJECT_SOURCE_DIR}/tests/test_main.c" "${CMAKE_CURRENT_SOURCE_DIR}/e2e/${name}_test.c")
    target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/tests")
    # Generated code must compile without warnings, whatever the program does.
    target_compile_options(${target} PRIVATE -Werror)
    add_test(NAME ${target} COMMAND ${target})
endfunction()

purr_e2e_test(move_clamp)
purr_e2e_test(structural)
purr_e2e_test(edge_cases)
purr_e2e_test(empty_world)
purr_e2e_test(defaults)
purr_e2e_test(math)
purr_e2e_test(input)
purr_e2e_test(crossplatform)
purr_e2e_test(evaluation_order)
purr_e2e_test(views)
purr_e2e_test(conditional)
purr_e2e_test(input_bounds)
purr_e2e_test(structs)
purr_e2e_test(methods)
purr_e2e_test(operators)
purr_e2e_test(events)
purr_e2e_test(local)
purr_e2e_test(enums)
purr_e2e_test(scenes)
purr_e2e_test(scenes_local)
purr_e2e_test(gui)
purr_e2e_test(blocks)
purr_e2e_test(loops)
purr_e2e_test(strings)
purr_e2e_test(text_fields)
purr_e2e_test(lists)
purr_e2e_test(crossplatform_text)
purr_e2e_test(devices)
purr_e2e_test(pause)
purr_e2e_test(net)
purr_e2e_test(net_no_input)
purr_e2e_test(blend)
purr_e2e_test(crlf) # Windows line endings, kept by .gitattributes
purr_e2e_test(multi_file SOURCES e2e/multi_file/a_physics.purr e2e/multi_file/b_main.purr
    e2e/multi_file/c_combat.purr e2e/multi_file/d_items.purr)

# Error tests: each errors/<name>.purr must fail to compile, and purrc's output
# must contain the text on the file's first line, written as `// expect: <text>`.
# A folder errors/<name>/ is one program of several files.
file(GLOB PURR_ERROR_TESTS CONFIGURE_DEPENDS LIST_DIRECTORIES true "${CMAKE_CURRENT_SOURCE_DIR}/errors/*")
foreach(source IN LISTS PURR_ERROR_TESTS)
    if(NOT IS_DIRECTORY "${source}" AND NOT source MATCHES "\\.purr$")
        continue()
    endif()
    get_filename_component(name "${source}" NAME_WE)
    add_test(NAME purrc_error_${name}
        COMMAND "${CMAKE_COMMAND}"
            "-DPURRC=${PURRC_PATH}"
            "-DSOURCE=${source}"
            "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/errors"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/expect_error.cmake")
endforeach()

# Schedule tests: `purrc --schedule` on schedule/<name>.purr must print
# schedule/<name>.txt exactly.
file(GLOB PURR_SCHEDULE_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/schedule/*.purr")
foreach(source IN LISTS PURR_SCHEDULE_TESTS)
    get_filename_component(name "${source}" NAME_WE)
    get_filename_component(dir "${source}" DIRECTORY)
    add_test(NAME purrc_schedule_${name}
        COMMAND "${CMAKE_COMMAND}"
            "-DPURRC=${PURRC_PATH}"
            "-DSOURCE=${source}"
            "-DEXPECTED=${dir}/${name}.txt"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/expect_schedule.cmake")
endforeach()
