# End-to-end tests: e2e/<name>.purr is transpiled and compiled together with
# e2e/<name>_test.c and the shared test harness, then run.
function(purr_e2e_test name)
    set(target purrc_e2e_${name})
    add_executable(${target}
        "${PROJECT_SOURCE_DIR}/tests/test_main.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/e2e/${name}_test.c")
    target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/tests")
    # Generated code must compile without warnings, whatever the program does.
    target_compile_options(${target} PRIVATE -Werror)
    purr_add_game(${target} SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/e2e/${name}.purr" NAME game)
    add_test(NAME ${target} COMMAND ${target})
endfunction()

purr_e2e_test(move_clamp)
purr_e2e_test(structural)
purr_e2e_test(edge_cases)
purr_e2e_test(empty_world)
purr_e2e_test(defaults)

# Error tests: each errors/<name>.purr must fail to compile, and purrc's output
# must contain the text on the file's first line, written as `// expect: <text>`.
file(GLOB PURR_ERROR_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/errors/*.purr")
foreach(source IN LISTS PURR_ERROR_TESTS)
    get_filename_component(name "${source}" NAME_WE)
    add_test(NAME purrc_error_${name}
        COMMAND "${CMAKE_COMMAND}"
            "-DPURRC=$<TARGET_FILE:purrc>"
            "-DSOURCE=${source}"
            "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/errors"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/expect_error.cmake")
endforeach()
