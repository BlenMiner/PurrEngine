# raylib: windows, input and rendering for the platform layer (see AGENTS.md,
# Rendering and platform). Downloaded at configure time, pinned to a release.
include(FetchContent)

FetchContent_Declare(raylib
    URL https://github.com/raysan5/raylib/archive/refs/tags/6.0.tar.gz
    URL_HASH SHA256=2b3ee1e2120c7a0796b33062c7e9a694dd8a8caa56a96319ac8c8ecf54a90d0b
    DOWNLOAD_EXTRACT_TIMESTAMP ON)

# A function, so these settings stay local to raylib.
function(purr_add_raylib)
    set(BUILD_EXAMPLES OFF)
    # Never CUSTOMIZE_BUILD: in raylib 6.0 it reads every `#define X 0` in
    # config.h as ON, which among other things stops EndDrawing from presenting
    # frames and polling window events. raylib's own defaults are fine.
    # raylib and GLFW use POSIX and compiler extensions on some platforms.
    set(CMAKE_C_EXTENSIONS ON)
    # WebGL 2 on the web (raylib defaults to WebGL 1).
    if(EMSCRIPTEN)
        set(GRAPHICS GRAPHICS_API_OPENGL_ES3)
    endif()

    FetchContent_MakeAvailable(raylib)

    # Third-party code: its warnings aren't ours to fix.
    target_compile_options(raylib PRIVATE -w)
    if(TARGET glfw)
        target_compile_options(glfw PRIVATE -w)
    endif()
endfunction()

purr_add_raylib()
