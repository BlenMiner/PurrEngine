# raylib: windows, input and rendering for the platform layer (see AGENTS.md,
# Rendering and platform). Downloaded at configure time, pinned to a release.
include(FetchContent)

# Web builds only download it (see tide_add_raylib_web).
if(TIDE_WEB)
    set(_tide_raylib_no_cmake SOURCE_SUBDIR tide-builds-it)
endif()
FetchContent_Declare(raylib
    URL https://github.com/raysan5/raylib/archive/refs/tags/6.0.tar.gz
    URL_HASH SHA256=2b3ee1e2120c7a0796b33062c7e9a694dd8a8caa56a96319ac8c8ecf54a90d0b
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    ${_tide_raylib_no_cmake})

# A function, so these settings stay local to raylib.
function(tide_add_raylib)
    set(BUILD_EXAMPLES OFF)
    # Never CUSTOMIZE_BUILD: in raylib 6.0 it reads every `#define X 0` in
    # config.h as ON, which among other things stops EndDrawing from presenting
    # frames and polling window events. raylib's own defaults are fine.
    # raylib and GLFW use POSIX and compiler extensions on some platforms.
    set(CMAKE_C_EXTENSIONS ON)

    FetchContent_MakeAvailable(raylib)

    # Third-party code: its warnings aren't ours to fix.
    target_compile_options(raylib PRIVATE -w)
    if(TARGET glfw)
        target_compile_options(glfw PRIVATE -w)
    endif()
endfunction()

# Web builds compile raylib themselves, since its own build assumes Emscripten.
# rcore.c gets our platform backend (platform/web/raylib/rcore_web_tide.c) in
# place of raylib's, and rlgl uses OpenGL ES 3, which the page's JavaScript
# implements on WebGL 2 (platform/web/tide.js). Audio and 3D models aren't
# built: the engine doesn't use them yet.
function(tide_add_raylib_web)
    FetchContent_MakeAvailable(raylib)
    set(src "${raylib_SOURCE_DIR}/src")

    file(READ "${src}/rcore.c" rcore)
    set(original "#elif defined(PLATFORM_MEMORY)\n    #include \"platforms/rcore_memory.c\"")
    string(REPLACE "${original}" "#elif defined(PLATFORM_WEB_TIDE)\n    #include \"rcore_web_tide.c\"\n${original}"
        patched "${rcore}")
    if(patched STREQUAL rcore)
        message(FATAL_ERROR "raylib's rcore.c changed: update the patch in cmake/Raylib.cmake")
    endif()
    set(patched_file "${CMAKE_BINARY_DIR}/raylib-web/rcore.c")
    if(EXISTS "${patched_file}")
        file(READ "${patched_file}" existing)
    endif()
    if(NOT existing STREQUAL patched)
        file(WRITE "${patched_file}" "${patched}")
    endif()

    add_library(raylib STATIC "${patched_file}" "${src}/rshapes.c" "${src}/rtextures.c" "${src}/rtext.c")
    target_compile_definitions(raylib PRIVATE
        PLATFORM_WEB_TIDE GRAPHICS_API_OPENGL_ES3 SUPPORT_MODULE_RMODELS=0 SUPPORT_MODULE_RAUDIO=0)
    target_include_directories(raylib
        PUBLIC "${src}"
        PRIVATE "${PROJECT_SOURCE_DIR}/platform/web/raylib" "${PROJECT_SOURCE_DIR}/platform/web"
                "${PROJECT_SOURCE_DIR}/platform/web/include")
    set_target_properties(raylib PROPERTIES C_EXTENSIONS ON)
    target_compile_options(raylib PRIVATE -w)
endfunction()

if(TIDE_WEB)
    tide_add_raylib_web()
else()
    tide_add_raylib()
endif()
