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
if(NOT WIN32 AND NOT TIDE_WEB AND NOT TIDE_ANDROID) # Android's are in its C library
    # Threads for ticks (src/jobs.c)
    find_package(Threads REQUIRED)
    target_link_libraries(tide_platform PUBLIC Threads::Threads)
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

# tide_android_app(<target> [LABEL <name>])
#
# Android builds: makes the shared library <target> an app, <target>.apk next
# to it, which runs the program's main() in Android's NativeActivity
# (platform/android/raylib/rcore_android_tide.c). Its ID is dev.tide.<target>,
# and it's signed with a key every build of this repo shares
# (build/android-debug.pem, made the first time), so a new build installs
# over the last. Does nothing in other builds, so it can be called
# unconditionally.
set(TIDE_ANDROID_KEY "${PROJECT_SOURCE_DIR}/build/android-debug.pem")
if(TIDE_ANDROID)
    # Made once, before any app, so apps built at once share it.
    add_custom_target(tide_android_key COMMAND "${TIDE_APK_PATH}" --key "${TIDE_ANDROID_KEY}" VERBATIM)
    add_dependencies(tide_android_key tidec_host)
endif()
function(tide_android_app target)
    if(NOT TIDE_ANDROID)
        return()
    endif()
    cmake_parse_arguments(ARG "" "LABEL" "" ${ARGN})
    set(label "${target}")
    if(ARG_LABEL)
        set(label "${ARG_LABEL}")
    endif()
    # The activity's way in is in raylib, which nothing in the program calls.
    target_link_options(${target} PRIVATE -Wl,-u,ANativeActivity_onCreate)
    string(REPLACE "-" "_" id "${target}")
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${TIDE_APK_PATH}" "$<TARGET_FILE_DIR:${target}>/${target}.apk" --package "dev.tide.${id}"
            --label "${label}" --lib "${target}" --key "${TIDE_ANDROID_KEY}" --min ${TIDE_ANDROID_API} --debuggable
            --icon "${PROJECT_SOURCE_DIR}/tools/tide-vscode/icon.png"
            "${TIDE_ANDROID_ABI}=$<TARGET_FILE:${target}>"
        COMMENT "Android app ${target}.apk"
        VERBATIM)
    add_dependencies(${target} tide_android_key)
endfunction()

# In the package, the platform layer is prebuilt: tide links games with it and
# raylib, native or web.
install(DIRECTORY include/tide DESTINATION include COMPONENT tide)
install(FILES web/shell.html DESTINATION web COMPONENT tide)
if(TIDE_WEB)
    set(TIDE_PACKAGE_LIBS lib/web)
    install(FILES web/tide.js DESTINATION web COMPONENT tide)
    # The C library tide links web games with, from cmake/wasi-toolchain.cmake:
    # only what its target's C programs use (threads are in libc.a). Games are
    # C, so not the C++ headers.
    install(DIRECTORY "${TIDE_WASI_SYSROOT}/include/${TIDE_WASI_TARGET}" DESTINATION wasi/sysroot/include COMPONENT tide
        PATTERN c++ EXCLUDE)
    install(FILES "${TIDE_WASI_SYSROOT}/lib/${TIDE_WASI_TARGET}/crt1-command.o"
        "${TIDE_WASI_SYSROOT}/lib/${TIDE_WASI_TARGET}/libc.a" "${TIDE_WASI_SYSROOT}/lib/${TIDE_WASI_TARGET}/libm.a"
        DESTINATION wasi/sysroot/lib/${TIDE_WASI_TARGET} COMPONENT tide)
    install(FILES "${TIDE_WASI_BUILTINS}" DESTINATION wasi RENAME libclang_rt.builtins.a COMPONENT tide)
elseif(MINGW)
    # tide builds Windows games for MinGW, with this C runtime (see
    # cmake/mingw-toolchain.cmake): every C header, since a game's C can include
    # any of Windows', and only the libraries games link (compiler/cli/build.c).
    # Not the C++ headers (the Windows Runtime's C++ library's too), nor the
    # interface definitions and type libraries, which only MIDL and C++ read.
    set(TIDE_PACKAGE_LIBS lib/native)
    set(mingw_target "${TIDE_MINGW_SYSROOT}/x86_64-w64-mingw32")
    install(DIRECTORY "${mingw_target}/include" DESTINATION mingw/sysroot/x86_64-w64-mingw32 COMPONENT tide
        PATTERN c++ EXCLUDE PATTERN wrl EXCLUDE PATTERN wrl.h EXCLUDE PATTERN "*.hxx" EXCLUDE PATTERN "*.idl" EXCLUDE
        PATTERN "*.tlb" EXCLUDE)
    set(mingw_libs crt2.o dllcrt2.o crtbegin.o crtend.o)
    foreach(lib IN ITEMS mingw32 mingwex moldname msvcrt kernel32 user32 gdi32 shell32 advapi32 opengl32 winmm ws2_32
                         secur32)
        list(APPEND mingw_libs lib${lib}.a)
    endforeach()
    list(TRANSFORM mingw_libs PREPEND "${mingw_target}/lib/")
    install(FILES ${mingw_libs} DESTINATION mingw/sysroot/x86_64-w64-mingw32/lib COMPONENT tide)
    install(FILES "${TIDE_MINGW_BUILTINS}" DESTINATION mingw COMPONENT tide)
elseif(TIDE_ANDROID)
    # tide builds Android games against the NDK the user has (see
    # compiler/cli/android.c), so only the platform layer for each CPU, and
    # the icon apps get when their game has none.
    set(TIDE_PACKAGE_LIBS lib/android/${TIDE_ANDROID_ABI})
    install(FILES "${PROJECT_SOURCE_DIR}/tools/tide-vscode/icon.png" DESTINATION lib/android COMPONENT tide)
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

# Ticks on every core, against ticks on one thread
tide_add_game(tide_platform_jobs SOURCES tests/jobs.tide NAME game
    HOST tests/jobs.c "${PROJECT_SOURCE_DIR}/tests/test_main.c")
target_include_directories(tide_platform_jobs PRIVATE "${PROJECT_SOURCE_DIR}/tests")
target_link_libraries(tide_platform_jobs PRIVATE tide_platform)
add_test(NAME tide_platform_jobs COMMAND tide_platform_jobs)
set_tests_properties(tide_platform_jobs PROPERTIES TIMEOUT 120)

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
    # Node runs it on this machine, so not on Android.
    if(NOT TIDE_ANDROID)
        add_executable(tide_platform_rtc_vectors tests/rtc_vectors.c)
        target_include_directories(tide_platform_rtc_vectors PRIVATE src)
        target_link_libraries(tide_platform_rtc_vectors PRIVATE tide_platform)
        if(TIDE_NODE)
            add_test(NAME tide_platform_rtc_vectors
                COMMAND "${TIDE_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/rtc_vectors.mjs"
                    $<TARGET_FILE:tide_platform_rtc_vectors> "${CMAKE_CURRENT_BINARY_DIR}")
        endif()
    endif()

    # Rooms on desktop, through a relay on this machine: two desktop players,
    # and a browser's WebRTC against ours, both ways round (tests/rooms.mjs).
    add_executable(tide_platform_rooms tests/rooms_native.c)
    target_link_libraries(tide_platform_rooms PRIVATE tide_platform)
    if(TIDE_NODE AND NOT TIDE_ANDROID)
        add_test(NAME tide_platform_rooms
            COMMAND "${TIDE_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/rooms.mjs" "${TIDE_BROWSER}"
                $<TARGET_FILE:tide_platform_rooms> "${CMAKE_CURRENT_BINARY_DIR}/tide_platform_rooms-browser"
                "${TIDE_FIREFOX}")
        set_tests_properties(tide_platform_rooms PROPERTIES TIMEOUT 180)
    endif()

    # On Android, rooms need an app around them: the relay's TLS is Java's,
    # which a program run from a shell has none of.
    if(TIDE_ANDROID)
        add_library(tide_platform_rooms_app SHARED tests/rooms_native.c)
        target_link_libraries(tide_platform_rooms_app PRIVATE tide_platform)
        tide_android_app(tide_platform_rooms_app LABEL "Tide rooms")
    endif()

    # Sessions over real UDP, on this machine.
    add_executable(tide_platform_udp tests/udp.c "${PROJECT_SOURCE_DIR}/tests/test_main.c")
    target_include_directories(tide_platform_udp PRIVATE "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(tide_platform_udp PRIVATE tide_platform)
    add_test(NAME tide_platform_udp COMMAND tide_platform_udp)
endif()

# The renderer: shapes, text and the GUI, read back from an offscreen render.
# `tide_platform_draw --bench` (`?bench` on the web) times frames of many shapes.
add_executable(tide_platform_draw tests/draw.c)
target_link_libraries(tide_platform_draw PRIVATE tide_platform)
if(TIDE_WEB)
    tide_web_page(tide_platform_draw SHELL web/test_shell.html)
    tide_add_web_test(tide_platform_draw TARGET tide_platform_draw)
elseif(NOT TIDE_ANDROID)
    # A hidden window, so it needs a desktop session with working OpenGL.
    add_test(NAME tide_platform_draw COMMAND tide_platform_draw)
    set_tests_properties(tide_platform_draw PROPERTIES LABELS gpu)
endif()

if(TIDE_WEB)
    # Keys by physical position on the web, where the typed character would be wrong.
    add_executable(tide_platform_web_keys tests/web_keys.c)
    target_link_libraries(tide_platform_web_keys PRIVATE tide_platform)
    tide_web_page(tide_platform_web_keys SHELL web/test_shell.html)
    tide_add_web_test(tide_platform_web_keys TARGET tide_platform_web_keys)

    # Fingers on the canvas: the touchscreen's, never the mouse's.
    add_executable(tide_platform_web_touch tests/web_touch.c)
    target_link_libraries(tide_platform_web_touch PRIVATE tide_platform)
    tide_web_page(tide_platform_web_touch SHELL web/test_shell.html)
    tide_add_web_test(tide_platform_web_touch TARGET tide_platform_web_touch)

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
    # Host migration in the browser: the host's frame leaves, and the two
    # others carry on in the room, one of them hosting it.
    add_test(NAME tide_platform_web_rooms_handover
        COMMAND "${TIDE_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/web_rooms.mjs" "${TIDE_BROWSER}"
            "$<TARGET_FILE_DIR:tide_platform_web_rooms>/tide_platform_web_rooms.html"
            "${CMAKE_CURRENT_BINARY_DIR}/tide_platform_web_rooms_handover-browser" handover)
    set_tests_properties(tide_platform_web_rooms_handover PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:" TIMEOUT 120)
    # ...and a match its host ended stays ended, for a player who missed the goodbye.
    add_test(NAME tide_platform_web_rooms_ended
        COMMAND "${TIDE_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/web_rooms.mjs" "${TIDE_BROWSER}"
            "$<TARGET_FILE_DIR:tide_platform_web_rooms>/tide_platform_web_rooms.html"
            "${CMAKE_CURRENT_BINARY_DIR}/tide_platform_web_rooms_ended-browser" ended)
    set_tests_properties(tide_platform_web_rooms_ended PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:" TIMEOUT 120)

    # Ticks on threads in a browser: a cross-origin isolated page, whose
    # threads are workers sharing its memory, against ticks on one thread.
    tide_add_game(tide_platform_web_threads SOURCES tests/jobs.tide NAME game HOST tests/web_threads.c)
    target_link_libraries(tide_platform_web_threads PRIVATE tide_platform)
    tide_web_page(tide_platform_web_threads SHELL tests/web_threads.html)
    add_test(NAME tide_platform_web_threads
        COMMAND "${TIDE_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/web_threads.mjs" "${TIDE_BROWSER}"
            "$<TARGET_FILE_DIR:tide_platform_web_threads>/tide_platform_web_threads.html"
            "${CMAKE_CURRENT_BINARY_DIR}/tide_platform_web_threads-browser")
    set_tests_properties(tide_platform_web_threads PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:" TIMEOUT 120)
endif()
