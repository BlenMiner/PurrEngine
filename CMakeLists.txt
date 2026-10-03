cmake_minimum_required(VERSION 3.25)
# Static libraries listed once more only for linkers that need it: Apple's
# warns about a repeated one, as a program that links the engine and the
# platform layer would get otherwise. CMake 3.29 and later.
if(POLICY CMP0156)
    cmake_policy(SET CMP0156 NEW)
endif()

# Always build with clang, however CMake is invoked (presets, IDE profiles, plain `cmake -B`).
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/clang-toolchain.cmake")
endif()

project(Tide LANGUAGES C)

# AppleClang is Apple's build of clang.
if(NOT CMAKE_C_COMPILER_ID MATCHES "^(Apple)?Clang$" OR NOT CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "GNU")
    message(FATAL_ERROR "Tide builds with clang only (see AGENTS.md). "
                        "Found ${CMAKE_C_COMPILER_ID} (${CMAKE_C_COMPILER_FRONTEND_VARIANT} frontend).")
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Web builds: WebAssembly through clang and wasi-libc (cmake/wasi-toolchain.cmake).
if(CMAKE_SYSTEM_NAME STREQUAL "WASI")
    set(TIDE_WEB ON)
    set(CMAKE_EXECUTABLE_SUFFIX .wasm) # CMake's WASI platform leaves it empty
    # Threads, which older clang doesn't take from the target's name. Only when
    # compiling: older clang warns that -pthread does nothing for a link. And
    # WebAssembly's SIMD, whose operations give the same results on every
    # machine (relaxed SIMD's don't, and stays off: see AGENTS.md, Determinism).
    add_compile_options(-pthread -msimd128)
else()
    set(TIDE_WEB OFF)
endif()
# Android builds: through clang with Android's NDK (cmake/android-toolchain.cmake).
# Games are shared libraries the app loads, which link the engine's static
# libraries, so everything is position-independent.
if(CMAKE_C_COMPILER_TARGET MATCHES "android")
    set(TIDE_ANDROID ON)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
else()
    set(TIDE_ANDROID OFF)
endif()

include(cmake/TideFlags.cmake)
include(cmake/Tide.cmake)

# For testing: AddressSanitizer and UndefinedBehaviorSanitizer on everything,
# like the WebRTC stack's fuzzing (platform/tests/rtc_fuzz.c). Linux and macOS.
option(TIDE_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(TIDE_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined)
endif()
include(cmake/WebTest.cmake)
find_program(TIDE_NODE NAMES node) # Tests that run Node: web builds need it anyway

option(TIDE_PLATFORM "Build the platform layer and the demo. Desktop builds download GLFW." ON)
option(TIDE_TOOLS_ONLY "Build only tidec, as web builds do for their host (cmake/HostTidec.cmake)" OFF)

# What the tide command says it is, and where it looks for updates. Release
# builds get the version semantic-release picked (.github/workflows/build.yml);
# anything else is a development build, which never looks for updates. tidec
# has it too, for `#if TIDE_0_3_OR_NEWER`, so web and Android builds' host tidec
# gets the same (cmake/HostTidec.cmake).
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" TIDE_BASE_VERSION LIMIT_COUNT 1)
set(TIDE_VERSION "${TIDE_BASE_VERSION}-dev" CACHE STRING "tide's version")

# tide_add_game runs tidec: this build's, or in web and Android builds a native
# one, since a WebAssembly or Android tidec can't run during the build.
if(TIDE_WEB OR TIDE_ANDROID)
    include(cmake/HostTidec.cmake)
else()
    set(TIDEC_COMMAND tidec)
    set(TIDEC_DEPENDS tidec)
    set(TIDEC_PATH "$<TARGET_FILE:tidec>")
endif()

if(TIDE_TOOLS_ONLY)
    add_subdirectory(compiler)
    return()
endif()

set(TIDE_CHANNEL "nightly" CACHE STRING "The update channel tide starts on: stable or nightly")
set(TIDE_REPOSITORY "BlenMiner/tide-engine" CACHE STRING "The GitHub repository tide updates from")

# The package (see cmake/package.cmake) is everything installed as the `tide`
# component: tide and tidels, the engine's headers and sources, and the
# prebuilt platform layer.
file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/package/VERSION" CONTENT "${TIDE_VERSION}\n")
install(FILES "${CMAKE_BINARY_DIR}/package/VERSION" DESTINATION . COMPONENT tide)
if(NOT TIDE_WEB AND NOT TIDE_ANDROID)
    add_subdirectory(tools) # Editor support
endif()

add_subdirectory(engine)

enable_testing()
add_subdirectory(tests)
add_subdirectory(compiler)

# The relay (relay/), on Node's own test runner, wherever there's Node.
if(TIDE_NODE)
    add_test(NAME relay COMMAND "${TIDE_NODE}" --test "${CMAKE_CURRENT_SOURCE_DIR}/relay/relay.test.mjs")
endif()

if(TIDE_PLATFORM)
    add_subdirectory(platform)
    add_subdirectory(demo)
    add_subdirectory(sandbox) # Opens a window, so it needs the platform layer
    add_subdirectory(bench)   # Ticks on the platform layer's threads
endif()
