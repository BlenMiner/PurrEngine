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

add_subdirectory(engine)

enable_testing()
add_subdirectory(tests)
add_subdirectory(compiler)

if(PURR_PLATFORM)
    add_subdirectory(platform)
    add_subdirectory(demo)
    add_subdirectory(sandbox) # Opens a window, so it needs the platform layer
endif()
