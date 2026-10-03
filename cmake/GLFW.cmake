# GLFW: desktop windows, their OpenGL contexts and input, for the platform
# layer (see AGENTS.md, Rendering and platform). Downloaded at configure time,
# pinned to a release. Web and Android builds have windows of their own
# (platform/web/canvas.c, platform/android/activity.c) and download nothing.
include(FetchContent)

FetchContent_Declare(glfw
    URL https://github.com/glfw/glfw/releases/download/3.4/glfw-3.4.zip
    URL_HASH SHA256=b5ec004b2712fd08e8861dc271428f048775200a2df719ccf575143ba749a3e9
    DOWNLOAD_EXTRACT_TIMESTAMP ON)

# A function, so these settings stay local to GLFW.
function(tide_add_glfw)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    # Its objects go into the platform layer's library, so that's the one
    # library games link, on every platform (see platform/CMakeLists.txt).
    set(GLFW_LIBRARY_TYPE OBJECT CACHE STRING "" FORCE)
    # X11 only on Linux: Wayland needs wayland-scanner and its protocols to
    # build, and X11 windows run under it.
    set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
    # GLFW uses POSIX and compiler extensions on some platforms.
    set(CMAKE_C_EXTENSIONS ON)

    FetchContent_MakeAvailable(glfw)

    # Third-party code: its warnings aren't ours to fix.
    target_compile_options(glfw PRIVATE -w)
endfunction()

tide_add_glfw()
