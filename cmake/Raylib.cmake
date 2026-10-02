# raylib: windows, input and rendering for the platform layer (see AGENTS.md,
# Rendering and platform). Downloaded at configure time, pinned to a release.
include(FetchContent)

# Web and Android builds only download it (see tide_add_raylib_custom).
if(TIDE_WEB OR TIDE_ANDROID)
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

# Web and Android builds compile raylib themselves, since its own build assumes
# Emscripten for the web and the NDK's own setup for Android. rcore.c gets our
# platform backend (platform/web/raylib/rcore_web_tide.c, or
# platform/android/raylib/rcore_android_tide.c) in place of raylib's, and rlgl
# uses OpenGL ES 3: on the web, the page's JavaScript implements it on WebGL 2
# (platform/web/tide.js). Audio and 3D models aren't built: the engine doesn't
# use them yet.
function(tide_add_raylib_custom define backend)
    FetchContent_MakeAvailable(raylib)
    set(src "${raylib_SOURCE_DIR}/src")

    file(READ "${src}/rcore.c" rcore)
    set(original "#elif defined(PLATFORM_MEMORY)
    #include \"platforms/rcore_memory.c\"")
    string(REPLACE "${original}" "#elif defined(${define})
    #include \"${backend}\"
${original}"
        patched "${rcore}")
    if(patched STREQUAL rcore)
        message(FATAL_ERROR "raylib's rcore.c changed: update the patch in cmake/Raylib.cmake")
    endif()
    set(patched_file "${CMAKE_BINARY_DIR}/raylib-tide/rcore.c")
    if(EXISTS "${patched_file}")
        file(READ "${patched_file}" existing)
    endif()
    if(NOT existing STREQUAL patched)
        file(WRITE "${patched_file}" "${patched}")
    endif()

    add_library(raylib STATIC "${patched_file}" "${src}/rshapes.c" "${src}/rtextures.c" "${src}/rtext.c")
    target_compile_definitions(raylib PRIVATE
        ${define} GRAPHICS_API_OPENGL_ES3 SUPPORT_MODULE_RMODELS=0 SUPPORT_MODULE_RAUDIO=0)
    target_include_directories(raylib PUBLIC "${src}")
    set_target_properties(raylib PROPERTIES C_EXTENSIONS ON)
    target_compile_options(raylib PRIVATE -w)
endfunction()

if(TIDE_WEB)
    tide_add_raylib_custom(PLATFORM_WEB_TIDE rcore_web_tide.c)
    target_include_directories(raylib PRIVATE "${PROJECT_SOURCE_DIR}/platform/web/raylib" "${PROJECT_SOURCE_DIR}/platform/web"
        "${PROJECT_SOURCE_DIR}/platform/web/include")
elseif(TIDE_ANDROID)
    # The backend is the app's way in too (ANativeActivity_onCreate), and
    # hands fingers to the platform layer (platform/src/native.h).
    tide_add_raylib_custom(PLATFORM_ANDROID_TIDE rcore_android_tide.c)
    target_include_directories(raylib PRIVATE "${PROJECT_SOURCE_DIR}/platform/android/raylib"
        "${PROJECT_SOURCE_DIR}/platform/src" "${PROJECT_SOURCE_DIR}/engine/include")
    target_link_libraries(raylib PUBLIC android log EGL GLESv3)
else()
    tide_add_raylib()
endif()
