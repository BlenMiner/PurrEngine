# End-to-end tests: e2e/<name>.tide (or the SOURCES given) is transpiled and
# compiled together with e2e/<name>_test.c and the shared test harness, then run.
# WARNINGS are the ones tidec has to give, as for tide_add_game.
function(tide_e2e_test name)
    cmake_parse_arguments(ARG "" "" "SOURCES;WARNINGS" ${ARGN})
    set(sources "e2e/${name}.tide")
    if(ARG_SOURCES)
        set(sources ${ARG_SOURCES})
    endif()
    set(target tidec_e2e_${name})
    tide_add_game(${target} SOURCES ${sources} NAME game WARNINGS ${ARG_WARNINGS}
        HOST "${PROJECT_SOURCE_DIR}/tests/test_main.c" "${CMAKE_CURRENT_SOURCE_DIR}/e2e/${name}_test.c")
    target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/tests")
    add_test(NAME ${target} COMMAND ${target})
endfunction()

tide_e2e_test(move_clamp)
tide_e2e_test(structural)
tide_e2e_test(edge_cases WARNINGS "'settings' is never used" "system 'NeverRuns' never runs")
tide_e2e_test(empty_world WARNINGS "system 'PlayerMovement' never runs")
tide_e2e_test(defaults)
tide_e2e_test(default_value)
tide_e2e_test(math)
tide_e2e_test(input)
tide_e2e_test(crossplatform)
tide_e2e_test(evaluation_order)
tide_e2e_test(views)
tide_e2e_test(conditional)
tide_e2e_test(input_bounds)
tide_e2e_test(structs)
tide_e2e_test(methods)
tide_e2e_test(operators)
tide_e2e_test(events)
tide_e2e_test(local)
tide_e2e_test(enums)
tide_e2e_test(scenes)
tide_e2e_test(scenes_local)
tide_e2e_test(scenes_fallback)
tide_e2e_test(gui)
tide_e2e_test(blocks)
tide_e2e_test(loops)
tide_e2e_test(strings)
tide_e2e_test(show_values)
tide_e2e_test(text_fields)
tide_e2e_test(lists)
tide_e2e_test(crossplatform_text)
tide_e2e_test(devices)
tide_e2e_test(pause)
tide_e2e_test(net)
tide_e2e_test(net_no_input)
tide_e2e_test(net_ended)
tide_e2e_test(blend)
tide_e2e_test(big_world)
tide_e2e_test(crlf) # Windows line endings, kept by .gitattributes
tide_e2e_test(multi_file SOURCES e2e/multi_file/a_physics.tide e2e/multi_file/b_main.tide
    e2e/multi_file/c_combat.tide e2e/multi_file/d_items.tide)
tide_e2e_test(extern SOURCES e2e/extern.tide e2e/extern_noise.tide e2e/extern_c.c) # C functions, from the game's C file
tide_e2e_test(fails SOURCES e2e/fails.tide e2e/fails_c.c) # Errors as values, and T?
tide_e2e_test(constants SOURCES e2e/constants/a_tuning.tide e2e/constants/b_main.tide)
tide_e2e_test(settings)
tide_e2e_test(handover)

# An endless chain of events stops the program, naming the event: here past
# 1000 deep, so it stops quickly.
tide_add_game(tidec_e2e_chain_endless SOURCES e2e/chain_endless.tide NAME game
    HOST "${CMAKE_CURRENT_SOURCE_DIR}/e2e/chain_endless_main.c")
target_compile_definitions(tidec_e2e_chain_endless PRIVATE TIDE_MAX_CHAIN=1000u)
add_test(NAME tidec_e2e_chain_endless COMMAND tidec_e2e_chain_endless)
set_tests_properties(tidec_e2e_chain_endless PROPERTIES TIMEOUT 30
    PASS_REGULAR_EXPRESSION "an endless chain of events: P[io]ng is 1000 deep in one tick")

# Error tests: each errors/<name>.tide must fail to compile, and tidec's output
# must contain the text on the file's first line, written as `// expect: <text>`.
# A folder errors/<name>/ is one program of several files.
file(GLOB TIDE_ERROR_TESTS CONFIGURE_DEPENDS LIST_DIRECTORIES true "${CMAKE_CURRENT_SOURCE_DIR}/errors/*")
foreach(source IN LISTS TIDE_ERROR_TESTS)
    if(NOT IS_DIRECTORY "${source}" AND NOT source MATCHES "\\.tide$")
        continue()
    endif()
    get_filename_component(name "${source}" NAME_WE)
    add_test(NAME tidec_error_${name}
        COMMAND "${CMAKE_COMMAND}"
            "-DTIDEC=${TIDEC_PATH}"
            "-DSOURCE=${source}"
            "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/errors"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/expect_error.cmake")
endforeach()

# Schedule tests: `tidec --schedule` on schedule/<name>.tide must print
# schedule/<name>.txt exactly.
file(GLOB TIDE_SCHEDULE_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/schedule/*.tide")
foreach(source IN LISTS TIDE_SCHEDULE_TESTS)
    get_filename_component(name "${source}" NAME_WE)
    get_filename_component(dir "${source}" DIRECTORY)
    add_test(NAME tidec_schedule_${name}
        COMMAND "${CMAKE_COMMAND}"
            "-DTIDEC=${TIDEC_PATH}"
            "-DSOURCE=${source}"
            "-DEXPECTED=${dir}/${name}.txt"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/expect_schedule.cmake")
endforeach()
