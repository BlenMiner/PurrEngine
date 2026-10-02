# C against Tide: each concept written both ways, as fast as each language
# allows, and timed on the same work, on one thread and on every core (see
# versus.h and AGENTS.md, Benchmarks). `versus_<concept> --help` lists a run's
# options. Tests run each one --quick, and fail when the ways end differently.
add_library(tide_versus STATIC versus.c)
target_include_directories(tide_versus PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(tide_versus PUBLIC tide tide_platform) # tide_platform_jobs
# --save adds a run's results to bench/results/versus.jsonl.
target_compile_definitions(tide_versus PRIVATE TIDE_BENCH_RESULTS="${CMAKE_CURRENT_SOURCE_DIR}/../results")

set(versus_concepts "")

# versus_<concept>: the game in the .tide files given, and <concept>/main.c,
# which runs it beside the C ways in <concept>/<concept>.c.
function(versus_add concept)
    tide_add_game(versus_${concept} SOURCES ${ARGN} NAME ${concept} HOST ${concept}/main.c ${concept}/${concept}.c)
    target_link_libraries(versus_${concept} PRIVATE tide_versus)
    add_test(NAME versus_${concept} COMMAND versus_${concept} --quick)
    set(versus_concepts ${versus_concepts} versus_${concept} PARENT_SCOPE)
endfunction()

versus_add(particles particles/particles.tide)
versus_add(sand ../sand/sand.tide) # The game itself
versus_add(life life/life.tide)
versus_add(churn churn/churn.tide)
versus_add(events events/events.tide)
versus_add(nbody nbody/nbody.tide)

# Every one in turn, in full: cmake --build --preset release --target versus
if(NOT TIDE_WEB)
    set(commands "")
    foreach(program IN LISTS versus_concepts)
        list(APPEND commands COMMAND ${program})
    endforeach()
    add_custom_target(versus ${commands} DEPENDS ${versus_concepts} USES_TERMINAL VERBATIM)
endif()
