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
            "-DPURRC=$<TARGET_FILE:purrc>"
            "-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
            "-DSOURCE=${source}"
            "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/errors"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/expect_error.cmake")
endforeach()
