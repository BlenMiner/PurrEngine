# purrc: the PurrLang transpiler. It's a build-time tool, so it doesn't link the engine.
# Everything but main.c is a library, which the language server shares.
file(GLOB PURRC_SOURCES CONFIGURE_DEPENDS src/*.c)
list(REMOVE_ITEM PURRC_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/main.c")

# Web builds use a native purrc instead (cmake/HostPurrc.cmake), and the
# language server and purr run natively, so only native builds have them.
if(NOT PURR_WEB)
    add_library(purrc_core STATIC ${PURRC_SOURCES})
    target_link_libraries(purrc_core PUBLIC purr_flags)
    # purrc reads the device member lists from purr/devices.h so PurrLang and C agree.
    target_include_directories(purrc_core PUBLIC src "${PROJECT_SOURCE_DIR}/engine/include")

    add_executable(purrc src/main.c)
    target_link_libraries(purrc PRIVATE purrc_core)
    if(PURR_TOOLS_ONLY)
        return()
    endif()
    add_subdirectory(lsp)
    add_subdirectory(cli)
endif()

add_subdirectory(tests)
