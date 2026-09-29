# The platform layer: a window, the frame loop and input devices, on raylib.
# Hosts and views link it; the simulation never does (see AGENTS.md).
include("${PROJECT_SOURCE_DIR}/cmake/Raylib.cmake")

file(GLOB PURR_PLATFORM_SOURCES CONFIGURE_DEPENDS src/*.c)

add_library(purr_platform STATIC ${PURR_PLATFORM_SOURCES})
target_include_directories(purr_platform PUBLIC include)
# Private: hosts go through purr/platform.h only. A file with a game's generated
# header can't include raylib.h, whose names clash with components.
target_link_libraries(purr_platform PUBLIC purr PRIVATE raylib)
if(WIN32)
    target_link_libraries(purr_platform PUBLIC ws2_32) # UDP, for sessions
endif()

if(PURR_WEB)
    # The page's JavaScript (web/purr.js) provides the canvas, input and frames
    # (web/purr_web.h), and implements the GL functions rlgl imports by name.
    # It allocates strings for glGetString with malloc.
    target_include_directories(purr_platform PUBLIC web)
    target_link_options(purr_platform INTERFACE -Wl,--allow-undefined -Wl,--export=malloc -Wl,--export=free)
endif()

# purr_web_page(<target> [SHELL <page.html>])
#
# Web builds: makes <target>.html next to <target>.wasm, one self-contained
# page with the program and web/purr.js inside, which opens straight from disk.
# SHELL is the page around the canvas (default web/shell.html). Does nothing in
# native builds, so it can be called unconditionally.
set(PURR_WEB_SHELL "${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html" CACHE INTERNAL "")
set(PURR_WEB_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/web/purr.js" CACHE INTERNAL "")
function(purr_web_page target)
    if(NOT PURR_WEB)
        return()
    endif()
    cmake_parse_arguments(ARG "" "SHELL" "" ${ARGN})
    if(ARG_SHELL)
        get_filename_component(shell "${ARG_SHELL}" ABSOLUTE)
    else()
        set(shell "${PURR_WEB_SHELL}")
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${PURR_NODE}" "${PROJECT_SOURCE_DIR}/cmake/web_page.mjs"
            "$<TARGET_FILE:${target}>" "${shell}" "${PURR_WEB_SCRIPT}"
            "$<TARGET_FILE_DIR:${target}>/${target}.html"
        COMMENT "Web page for ${target}"
        VERBATIM)
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${shell}" "${PURR_WEB_SCRIPT}")
endfunction()

# In the package, the platform layer is prebuilt: purr links games with it and
# raylib, native or web.
install(DIRECTORY include/purr DESTINATION include COMPONENT purr)
install(FILES web/shell.html DESTINATION web COMPONENT purr)
if(PURR_WEB)
    set(PURR_PACKAGE_LIBS lib/web)
    install(FILES web/purr.js DESTINATION web COMPONENT purr)
    # The C library purr links web games with, from cmake/wasi-toolchain.cmake:
    # only what wasm32-wasip1 C programs use.
    install(DIRECTORY "${PURR_WASI_SYSROOT}/include/wasm32-wasip1" DESTINATION wasi/sysroot/include COMPONENT purr)
    install(FILES "${PURR_WASI_SYSROOT}/lib/wasm32-wasip1/crt1-command.o" "${PURR_WASI_SYSROOT}/lib/wasm32-wasip1/libc.a"
        "${PURR_WASI_SYSROOT}/lib/wasm32-wasip1/libm.a" DESTINATION wasi/sysroot/lib/wasm32-wasip1 COMPONENT purr)
    install(FILES "${PURR_WASI_BUILTINS}" DESTINATION wasi RENAME libclang_rt.builtins.a COMPONENT purr)
elseif(MINGW)
    # purr builds Windows games for MinGW, with this C runtime (see
    # cmake/mingw-toolchain.cmake): every header, and only the libraries games
    # link (compiler/cli/build.c).
    set(PURR_PACKAGE_LIBS lib/native)
    set(mingw_target "${PURR_MINGW_SYSROOT}/x86_64-w64-mingw32")
    install(DIRECTORY "${mingw_target}/include" DESTINATION mingw/sysroot/x86_64-w64-mingw32 COMPONENT purr)
    set(mingw_libs crt2.o crtbegin.o crtend.o)
    foreach(lib IN ITEMS mingw32 mingwex moldname msvcrt kernel32 user32 gdi32 shell32 advapi32 opengl32 winmm)
        list(APPEND mingw_libs lib${lib}.a)
    endforeach()
    list(TRANSFORM mingw_libs PREPEND "${mingw_target}/lib/")
    install(FILES ${mingw_libs} DESTINATION mingw/sysroot/x86_64-w64-mingw32/lib COMPONENT purr)
    install(FILES "${PURR_MINGW_BUILTINS}" DESTINATION mingw COMPONENT purr)
elseif(NOT WIN32)
    set(PURR_PACKAGE_LIBS lib/native)
endif()
# On Windows, only the MinGW build's platform layer goes in the package.
if(PURR_PACKAGE_LIBS)
    install(FILES $<TARGET_FILE:purr_platform> $<TARGET_FILE:raylib> DESTINATION ${PURR_PACKAGE_LIBS} COMPONENT purr)
endif()

if(NOT PURR_WEB)
    # Sessions over real UDP, on this machine.
    add_executable(purr_platform_udp tests/udp.c "${PROJECT_SOURCE_DIR}/tests/test_main.c")
    target_include_directories(purr_platform_udp PRIVATE "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(purr_platform_udp PRIVATE purr_platform)
    add_test(NAME purr_platform_udp COMMAND purr_platform_udp)
endif()

if(PURR_WEB)
    # Keys by physical position on the web, where the typed character would be wrong.
    add_executable(purr_platform_web_keys tests/web_keys.c)
    target_link_libraries(purr_platform_web_keys PRIVATE purr_platform)
    purr_web_page(purr_platform_web_keys SHELL web/test_shell.html)
    purr_add_web_test(purr_platform_web_keys TARGET purr_platform_web_keys)
endif()
