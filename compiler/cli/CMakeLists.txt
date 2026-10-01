# tide: the command users run to build, run and upgrade games. It runs tidec's
# front end in-process and calls clang (see AGENTS.md, Packaging and releases).
add_executable(tide_cli main.c build.c editors.c libraries.c release.c serve.c sha256.c sys.c toolchain.c upgrade.c
    "${CMAKE_CURRENT_SOURCE_DIR}/../lsp/folders.c" "${CMAKE_CURRENT_SOURCE_DIR}/../lsp/json.c")
target_include_directories(tide_cli PRIVATE . ../lsp)
target_link_libraries(tide_cli PRIVATE tidec_core)
# The engine library is already the `tide` target; the program is still called tide.
set_target_properties(tide_cli PROPERTIES OUTPUT_NAME tide)
target_compile_definitions(tide_cli PRIVATE
    "TIDE_VERSION=\"${TIDE_VERSION}\""
    "TIDE_CHANNEL=\"${TIDE_CHANNEL}\""
    "TIDE_REPOSITORY=\"${TIDE_REPOSITORY}\"")
if(WIN32)
    target_link_libraries(tide_cli PRIVATE ws2_32) # Serves the page of tide run --web
else()
    find_package(Threads REQUIRED)
    target_link_libraries(tide_cli PRIVATE Threads::Threads) # Reads the terminal while a game runs
endif()
install(TARGETS tide_cli RUNTIME DESTINATION bin COMPONENT tide)

# Versions and picking releases, without the network, and which platform
# libraries are for.
add_executable(tide_cli_tests tests/release_test.c tests/libraries_test.c release.c libraries.c
    "${CMAKE_CURRENT_SOURCE_DIR}/../lsp/json.c" "${PROJECT_SOURCE_DIR}/tests/test_main.c")
target_include_directories(tide_cli_tests PRIVATE . ../lsp "${PROJECT_SOURCE_DIR}/tests")
target_link_libraries(tide_cli_tests PRIVATE tide)
add_test(NAME tide_cli_tests COMMAND tide_cli_tests)

# Packages build clang and lld into tide, so users need no compiler of their
# own (see cmake/LLVM.cmake).
option(TIDE_EMBED_LLVM "Build clang and lld into tide, from LLVM's static libraries" OFF)
if(TIDE_EMBED_LLVM)
    include("${PROJECT_SOURCE_DIR}/cmake/LLVM.cmake")
    target_link_libraries(tide_cli PRIVATE tide_cc)
    target_compile_definitions(tide_cli PRIVATE TIDE_EMBEDDED_CLANG "TIDE_LLVM_VERSION=\"${TIDE_LLVM_VERSION}\"")
endif()
