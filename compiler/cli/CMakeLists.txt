# tide: the command users run to build, run and upgrade games. It runs tidec's
# front end in-process and calls clang (see AGENTS.md, Packaging and releases).
add_executable(tide_cli main.c build.c editors.c fetch.c libraries.c packages_cmd.c release.c serve.c sha256.c sys.c
    toolchain.c upgrade.c "${CMAKE_CURRENT_SOURCE_DIR}/../lsp/folders.c" "${CMAKE_CURRENT_SOURCE_DIR}/../lsp/json.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../lsp/packages.c")
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

# Versions and picking releases, which platform libraries are for, what
# tide.packages and git's lists of branches and tags say, without the network,
# and Android apps as Android checks them.
add_executable(tide_cli_tests tests/release_test.c tests/libraries_test.c tests/packages_test.c tests/apk_test.c
    release.c libraries.c fetch.c sys.c ${TIDE_APK_SOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/../lsp/json.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../lsp/packages.c" "${PROJECT_SOURCE_DIR}/tests/test_main.c")
target_include_directories(tide_cli_tests PRIVATE . ../lsp "${PROJECT_SOURCE_DIR}/tests" "${PROJECT_SOURCE_DIR}/platform/src/rtc")
target_link_libraries(tide_cli_tests PRIVATE tide tidec_core)
if(NOT WIN32)
    target_link_libraries(tide_cli_tests PRIVATE Threads::Threads)
endif()
add_test(NAME tide_cli_tests COMMAND tide_cli_tests)

# Games with packages: in folders, and from git through a folder that stands
# in for git hosts.
add_test(NAME tide_cli_packages
    COMMAND "${CMAKE_COMMAND}" "-DTIDE=$<TARGET_FILE:tide_cli>" "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/packages_test"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/packages_test.cmake")

# tide editors, with fake editors where their installers put them.
add_test(NAME tide_cli_editors
    COMMAND "${CMAKE_COMMAND}" "-DTIDE=$<TARGET_FILE:tide_cli>" "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/editors_test"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/editors_test.cmake")

# Packages build clang and lld into tide, so users need no compiler of their
# own (see cmake/LLVM.cmake).
option(TIDE_EMBED_LLVM "Build clang and lld into tide, from LLVM's static libraries" OFF)
if(TIDE_EMBED_LLVM)
    include("${PROJECT_SOURCE_DIR}/cmake/LLVM.cmake")
    target_link_libraries(tide_cli PRIVATE tide_cc)
    target_compile_definitions(tide_cli PRIVATE TIDE_EMBEDDED_CLANG "TIDE_LLVM_VERSION=\"${TIDE_LLVM_VERSION}\"")
endif()
