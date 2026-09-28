cmake_minimum_required(VERSION 3.25)

# Always build with clang, however CMake is invoked (presets, IDE profiles, plain `cmake -B`).
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/clang-toolchain.cmake")
endif()

project(PurrEngine LANGUAGES C)

if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang" OR NOT CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "GNU")
    message(FATAL_ERROR "PurrEngine builds with clang only (see AGENTS.md). "
                        "Found ${CMAKE_C_COMPILER_ID} (${CMAKE_C_COMPILER_FRONTEND_VARIANT} frontend).")
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

include(cmake/PurrFlags.cmake)
include(cmake/PurrLang.cmake)
include(cmake/WebTest.cmake)

option(PURR_PLATFORM "Build the platform layer and the demo. Downloads raylib." ON)

# What the purr command says it is, and where it looks for updates. Release
# builds get the version semantic-release picked (.github/workflows/build.yml);
# anything else is a development build, which never looks for updates.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" PURR_BASE_VERSION LIMIT_COUNT 1)
set(PURR_VERSION "${PURR_BASE_VERSION}-dev" CACHE STRING "purr's version")
set(PURR_CHANNEL "nightly" CACHE STRING "The update channel purr starts on: stable or nightly")
set(PURR_REPOSITORY "BlenMiner/PurrEngine" CACHE STRING "The GitHub repository purr updates from")

# The package (see cmake/package.cmake) is everything installed as the `purr`
# component: purr and purrls, the engine's headers and sources, and the
# prebuilt platform layer.
file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/package/VERSION" CONTENT "${PURR_VERSION}\n")
install(FILES "${CMAKE_BINARY_DIR}/package/VERSION" DESTINATION . COMPONENT purr)
if(NOT EMSCRIPTEN)
    # Editor support: highlighting, and the language server through purrls on PATH.
    install(DIRECTORY tools/purrlang-syntax DESTINATION editors COMPONENT purr)
    install(FILES tools/purrlang-lsp4ij/installed/template.json DESTINATION editors/purrlang-lsp4ij COMPONENT purr)
endif()

add_subdirectory(engine)

enable_testing()
add_subdirectory(tests)
add_subdirectory(compiler)

if(PURR_PLATFORM)
    add_subdirectory(platform)
    add_subdirectory(demo)
    add_subdirectory(sandbox) # Opens a window, so it needs the platform layer
endif()
