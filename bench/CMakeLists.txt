# Benchmarks: games that stress the engine, played by bots on one machine
# (see AGENTS.md, Benchmarks). `bench_<name> --help` lists a run's options.
# Tests run each one --quick, and fail when a world goes wrong, a bot can't
# join, or the match ends with another hash than every platform agrees on.
add_library(tide_bench STATIC bench.c)
target_include_directories(tide_bench PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(tide_bench PUBLIC tide tide_platform) # tide_platform_jobs
# --save adds a run's results to results/<name>.jsonl, in the repo, to compare
# with later (node bench/compare.mjs).
target_compile_definitions(tide_bench PRIVATE TIDE_BENCH_RESULTS="${CMAKE_CURRENT_SOURCE_DIR}/results")

tide_add_game(bench_sand SOURCES sand/sand.tide NAME sand HOST sand/main.c)
target_link_libraries(bench_sand PRIVATE tide_bench)
add_test(NAME bench_sand COMMAND bench_sand --quick --expect 0x4f8c29e1545be234)
# Sand to play, with its views and the mouse: `sand` in a window, sand.html on
# the web, which the docs site shows (docs/guide/sand.md).
tide_add_game(sand SOURCES sand/sand.tide sand/play.tide TITLE "Tide sand")

tide_add_game(bench_gallery SOURCES gallery/gallery.tide NAME gallery HOST gallery/main.c)
target_link_libraries(bench_gallery PRIVATE tide_bench)
add_test(NAME bench_gallery COMMAND bench_gallery --quick --expect 0x5963c41ae016415f)
