# tidec: the Tide transpiler. It's a build-time tool, so it doesn't link the engine.
# Everything but main.c is a library, which the language server shares.
file(GLOB TIDEC_SOURCES CONFIGURE_DEPENDS src/*.c)
list(REMOVE_ITEM TIDEC_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/main.c")

# Web and Android builds use a native tidec instead (cmake/HostTidec.cmake),
# and the language server and tide run natively, so only native builds have them.
if(NOT TIDE_WEB AND NOT TIDE_ANDROID)
    add_library(tidec_core STATIC ${TIDEC_SOURCES})
    target_link_libraries(tidec_core PUBLIC tide_flags)
    # tidec reads the device member lists from tide/devices.h so Tide and C agree.
    target_include_directories(tidec_core PUBLIC src "${PROJECT_SOURCE_DIR}/engine/include")

    add_executable(tidec src/main.c)
    target_link_libraries(tidec PRIVATE tidec_core)

    # Android apps (cli/apk.c), which tide makes itself, for this repo's
    # Android builds (tide_android_app). Its crypto is the platform layer's.
    set(TIDE_APK_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/cli/apk.c"
        "${PROJECT_SOURCE_DIR}/platform/src/rtc/p256.c" "${PROJECT_SOURCE_DIR}/platform/src/rtc/cert.c"
        "${PROJECT_SOURCE_DIR}/platform/src/rtc/crypto.c" "${PROJECT_SOURCE_DIR}/engine/src/sha256.c")
    set(TIDE_APK_SOURCES ${TIDE_APK_SOURCES} PARENT_SCOPE)
    add_executable(tide_apk cli/apk_main.c ${TIDE_APK_SOURCES})
    target_include_directories(tide_apk PRIVATE cli "${PROJECT_SOURCE_DIR}/platform/src/rtc" "${PROJECT_SOURCE_DIR}/engine/include")
    target_link_libraries(tide_apk PRIVATE tide_flags)
    if(TIDE_TOOLS_ONLY)
        return()
    endif()
    add_subdirectory(lsp)
    add_subdirectory(cli)
endif()

add_subdirectory(tests)
