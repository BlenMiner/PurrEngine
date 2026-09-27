# purrc: the PurrLang transpiler. It's a build-time tool, so it doesn't link the engine.
# Everything but main.c is a library, which the language server shares.
file(GLOB PURRC_SOURCES CONFIGURE_DEPENDS src/*.c)
list(REMOVE_ITEM PURRC_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/main.c")

add_library(purrc_core STATIC ${PURRC_SOURCES})
target_link_libraries(purrc_core PUBLIC purr_flags)
# purrc reads the device member lists from purr/devices.h so PurrLang and C agree.
target_include_directories(purrc_core PUBLIC src "${PROJECT_SOURCE_DIR}/engine/include")

add_executable(purrc src/main.c)
target_link_libraries(purrc PRIVATE purrc_core)

if(EMSCRIPTEN)
    # In web builds purrc is WebAssembly too, run under Node during the build
    # (CMake adds the emulator automatically). It needs the real file system.
    target_link_options(purrc PRIVATE -sNODERAWFS=1)
else()
    # The language server is for editors, so only native builds have it.
    add_subdirectory(lsp)
endif()

add_subdirectory(tests)
