# The platform layer: a window, the frame loop and input devices, on raylib.
# Hosts and views link it; the simulation never does (see AGENTS.md).
include("${PROJECT_SOURCE_DIR}/cmake/Raylib.cmake")

file(GLOB TIDE_PLATFORM_SOURCES CONFIGURE_DEPENDS src/*.c)
if(NOT TIDE_WEB)
    # WebRTC for rooms on desktop (src/rtc/rtc.h); the web has the browser's.
    file(GLOB TIDE_RTC_SOURCES CONFIGURE_DEPENDS src/rtc/*.c)
    list(APPEND TIDE_PLATFORM_SOURCES ${TIDE_RTC_SOURCES})
endif()

add_library(tide_platform STATIC ${TIDE_PLATFORM_SOURCES})
target_include_directories(tide_platform PUBLIC include)
# Private: hosts go through tide/platform.h only. A file with a game's generated
# header can't include raylib.h, whose names clash with components.
target_link_libraries(tide_platform PUBLIC tide PRIVATE raylib)
if(WIN32)
    target_link_libraries(tide_platform PUBLIC ws2_32 secur32) # UDP, for sessions; TLS, for the relay
elseif(APPLE)
    target_link_libraries(tide_platform PUBLIC "-framework Security") # TLS, for the relay
endif()
# Hot reloading loads the game's library (src/reload.c).
target_link_libraries(tide_platform PUBLIC ${CMAKE_DL_LIBS})

if(TIDE_WEB)
    # The page's JavaScript (web/tide.js) provides the canvas, input and frames
    # (web/tide_web.h), and implements the GL functions rlgl imports by name.
    # It allocates strings for glGetString with malloc.
    target_include_directories(tide_platform PUBLIC web)
    target_link_options(tide_platform INTERFACE -Wl,--allow-undefined -Wl,--export=malloc -Wl,--export=free)
endif()

# tide_web_page(<target> [SHELL <page.html>])
#
# Web builds: makes <target>.html next to <target>.wasm, one self-contained
# page with the program and web/tide.js inside, which opens straight from disk.
# SHELL is the page around the canvas (default web/shell.html). Does nothing in
# native builds, so it can be called unconditionally.
set(TIDE_WEB_SHELL "${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html" CACHE INTERNAL "")
set(TIDE_WEB_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/web/tide.js" CACHE INTERNAL "")
function(tide_web_page target)
    if(NOT TIDE_WEB)
        return()
    endif()
    cmake_parse_arguments(ARG "" "SHELL" "" ${ARGN})
    if(ARG_SHELL)
        get_filename_component(shell "${ARG_SHELL}" ABSOLUTE)
    else()
        set(shell "${TIDE_WEB_SHELL}")
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${TIDE_NODE}" "${PROJECT_SOURCE_DIR}/cmake/web_page.mjs"
            "$<TARGET_FILE:${target}>" "${shell}" "${TIDE_WEB_SCRIPT}"
            "$<TARGET_FILE_DIR:${target}>/${target}.html"
        COMMENT "Web page for ${target}"
        VERBATIM)
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${shell}" "${TIDE_WEB_SCRIPT}")
endfunction()

# In the package, the platform layer is prebuilt: tide links games with it and
# raylib, native or web.
install(DIRECTORY include/tide DESTINATION include COMPONENT tide)
install(FILES web/shell.html DESTINATION web COMPONENT tide)
if(TIDE_WEB)
    set(TIDE_PACKAGE_LIBS lib/web)
    install(FILES web/tide.js DESTINATION web COMPONENT tide)
    # The C library tide links web games with, from cmake/wasi-toolchain.cmake:
    # only what wasm32-wasip1 C programs use.
    install(DIRECTORY "${TIDE_WASI_SYSROOT}/include/wasm32-wasip1" DESTINATION wasi/sysroot/include COMPONENT tide)
    install(FILES "${TIDE_WASI_SYSROOT}/lib/wasm32-wasip1/crt1-command.o" "${TIDE_WASI_SYSROOT}/lib/wasm32-wasip1/libc.a"
        "${TIDE_WASI_SYSROOT}/lib/wasm32-wasip1/libm.a" DESTINATION wasi/sysroot/lib/wasm32-wasip1 COMPONENT tide)
    install(FILES "${TIDE_WASI_BUILTINS}" DESTINATION wasi RENAME libclang_rt.builtins.a COMPONENT tide)
elseif(MINGW)
    # tide builds Windows games for MinGW, with this C runtime (see
    # cmake/mingw-toolchain.cmake): every header, and only the libraries games
    # link (compiler/cli/build.c).
    set(TIDE_PACKAGE_LIBS lib/native)
    set(mingw_target "${TIDE_MINGW_SYSROOT}/x86_64-w64-mingw32")
    install(DIRECTORY "${mingw_target}/include" DESTINATION mingw/sysroot/x86_64-w64-mingw32 COMPONENT tide)
    set(mingw_libs crt2.o dllcrt2.o crtbegin.o crtend.o)
    foreach(lib IN ITEMS mingw32 mingwex moldname msvcrt kernel32 user32 gdi32 shell32 advapi32 opengl32 winmm ws2_32
                         secur32)
        list(APPEND mingw_libs lib${lib}.a)
    endforeach()
    list(TRANSFORM mingw_libs PREPEND "${mingw_target}/lib/")
    install(FILES ${mingw_libs} DESTINATION mingw/sysroot/x86_64-w64-mingw32/lib COMPONENT tide)
    install(FILES "${TIDE_MINGW_BUILTINS}" DESTINATION mingw COMPONENT tide)
elseif(NOT WIN32)
    set(TIDE_PACKAGE_LIBS lib/native)
endif()
# On Windows, only the MinGW build's platform layer goes in the package.
if(TIDE_PACKAGE_LIBS)
    install(FILES $<TARGET_FILE:tide_platform> $<TARGET_FILE:raylib> DESTINATION ${TIDE_PACKAGE_LIBS} COMPONENT tide)
endif()

# Carrying a world over to a build whose data layout changed, for hot
# reloading: on layouts written by hand, and on a game's, as tidec describes it.
tide_add_game(tide_platform_migrate SOURCES tests/migrate.tide NAME game LAYOUT
    HOST tests/migrate.c "${PROJECT_SOURCE_DIR}/tests/test_main.c")
target_include_directories(tide_platform_migrate PRIVATE "${PROJECT_SOURCE_DIR}/tests")
target_link_libraries(tide_platform_migrate PRIVATE tide_platform)
add_test(NAME tide_platform_migrate COMMAND tide_platform_migrate)

if(NOT TIDE_WEB)
    # The WebRTC stack (src/rtc), piece by piece.
    add_executable(tide_platform_rtc tests/rtc.c "${PROJECT_SOURCE_DIR}/tests/test_main.c")
    target_include_directories(tide_platform_rtc PRIVATE "${PROJECT_SOURCE_DIR}/tests" src)
    target_link_libraries(tide_platform_rtc PRIVATE tide_platform)
    add_test(NAME tide_platform_rtc COMMAND tide_platform_rtc)
    set_tests_properties(tide_platform_rtc PROPERTIES ENVIRONMENT TIDE_RTC_LOCAL=1) # Sockets on loopback only

    # Everything in it that reads what other machines send, fed real messages
    # broken at random. With TIDE_SANITIZE, AddressSanitizer watches it too.
    add_executable(tide_platform_rtc_fuzz tests/rtc_fuzz.c)
    target_include_directories(tide_platform_rtc_fuzz PRIVATE src)
    target_link_libraries(tide_platform_rtc_fuzz PRIVATE tide_platform)
    add_test(NAME tide_platform_rtc_fuzz COMMAND tide_platform_rtc_fuzz 2000)
    set_tests_properties(tide_platform_rtc_fuzz PROPERTIES ENVIRONMENT TIDE_RTC_LOCAL=1)

    # Its crypto against Node's (OpenSSL's), on random inputs every run.
    add_executable(tide_platform_rtc_vectors tests/rtc_vectors.c)
    target_include_directories(tide_platform_rtc_vectors PRIVATE src)
    target_link_libraries(tide_platform_rtc_vectors PRIVATE tide_platform)
    if(TIDE_NODE)
        add_test(NAME tide_platform_rtc_vectors
            COMMAND "${TIDE_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/rtc_vectors.mjs"
                $<TARGET_FILE:tide_platform_rtc_vectors> "${CMAKE_CURRENT_BINARY_DIR}")
    endif()

    # Rooms on desktop, through a relay on this machine: two desktop players,
    # and a browser's WebRTC against ours, both ways round (tests/rooms.mjs).
    add_executable(tide_platform_rooms tests/rooms_native.c)
    target_link_libraries(tide_platform_rooms PRIVATE tide_platform)
    if(TIDE_NODE)
        add_test(NAME tide_platform_rooms
            COMMAND "${TIDE_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/rooms.mjs" "${TIDE_BROWSER}"
                $<TARGET_FILE:tide_platform_rooms> "${CMAKE_CURRENT_BINARY_DIR}/tide_platform_rooms-browser"
                "${TIDE_FIREFOX}")
        set_tests_properties(tide_platform_rooms PROPERTIES TIMEOUT 180)
    endif()

    # Sessions over real UDP, on this machine.
    add_executable(tide_platform_udp tests/udp.c "${PROJECT_SOURCE_DIR}/tests/test_main.c")
    target_include_directories(tide_platform_udp PRIVATE "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(tide_platform_udp PRIVATE tide_platform)
    add_test(NAME tide_platform_udp COMMAND tide_platform_udp)
endif()

if(TIDE_WEB)
    # Keys by physical position on the web, where the typed character would be wrong.
    add_executable(tide_platform_web_keys tests/web_keys.c)
    target_link_libraries(tide_platform_web_keys PRIVATE tide_platform)
    tide_web_page(tide_platform_web_keys SHELL web/test_shell.html)
    tide_add_web_test(tide_platform_web_keys TARGET tide_platform_web_keys)

    # Two players in a room: a relay on this machine, and WebRTC between two
    # frames of one page, in real time rather than the other tests' virtual time.
    add_executable(tide_platform_web_rooms tests/web_rooms.c)
    target_link_libraries(tide_platform_web_rooms PRIVATE tide_platform)
    tide_web_page(tide_platform_web_rooms SHELL tests/web_rooms.html)
    add_test(NAME tide_platform_web_rooms
        COMMAND "${TIDE_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/web_rooms.mjs" "${TIDE_BROWSER}"
            "$<TARGET_FILE_DIR:tide_platform_web_rooms>/tide_platform_web_rooms.html"
            "${CMAKE_CURRENT_BINARY_DIR}/tide_platform_web_rooms-browser")
    set_tests_properties(tide_platform_web_rooms PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:" TIMEOUT 120)
endif()
