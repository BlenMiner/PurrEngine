# The platform layer: a window, the frame loop and input devices, on raylib.
# Hosts and views link it; the simulation never does (see AGENTS.md).
include("${PROJECT_SOURCE_DIR}/cmake/Raylib.cmake")

file(GLOB PURR_PLATFORM_SOURCES CONFIGURE_DEPENDS src/*.c)

add_library(purr_platform STATIC ${PURR_PLATFORM_SOURCES})
target_include_directories(purr_platform PUBLIC include)
# Private: hosts go through purr/platform.h only. A file with a game's generated
# header can't include raylib.h, whose names clash with components.
target_link_libraries(purr_platform PUBLIC purr PRIVATE raylib)

# purr_web_page(<target> [SHELL <page.html>])
#
# Web builds: <target> becomes one self-contained <target>.html, which opens
# straight from disk. SHELL is the page around the canvas (default web/shell.html).
# Does nothing in native builds, so it can be called unconditionally.
set(PURR_WEB_SHELL "${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html" CACHE INTERNAL "")
function(purr_web_page target)
    if(NOT EMSCRIPTEN)
        return()
    endif()
    cmake_parse_arguments(ARG "" "SHELL" "" ${ARGN})
    if(ARG_SHELL)
        get_filename_component(shell "${ARG_SHELL}" ABSOLUTE)
    else()
        set(shell "${PURR_WEB_SHELL}")
    endif()
    set_target_properties(${target} PROPERTIES SUFFIX ".html")
    target_link_options(${target} PRIVATE "SHELL:--shell-file ${shell}" -sSINGLE_FILE=1)
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${shell}")
endfunction()

# In the package, the platform layer is prebuilt: purr links games with it and
# raylib, native or web.
install(DIRECTORY include/purr DESTINATION include COMPONENT purr)
install(FILES web/shell.html DESTINATION web COMPONENT purr)
if(EMSCRIPTEN)
    set(PURR_PACKAGE_LIBS lib/web)
else()
    set(PURR_PACKAGE_LIBS lib/native)
endif()
install(FILES $<TARGET_FILE:purr_platform> $<TARGET_FILE:raylib> DESTINATION ${PURR_PACKAGE_LIBS} COMPONENT purr)

if(EMSCRIPTEN)
    # Keys by physical position on the web, where raylib alone gets them wrong.
    add_executable(purr_platform_web_keys tests/web_keys.c)
    target_link_libraries(purr_platform_web_keys PRIVATE purr_platform)
    purr_web_page(purr_platform_web_keys SHELL web/test_shell.html)
    purr_add_web_test(purr_platform_web_keys TARGET purr_platform_web_keys)
endif()
