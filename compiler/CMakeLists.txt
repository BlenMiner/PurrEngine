# purrc: the PurrLang transpiler. It's a build-time tool, so it doesn't link the engine.
file(GLOB PURRC_SOURCES CONFIGURE_DEPENDS src/*.c)

add_executable(purrc ${PURRC_SOURCES})
target_link_libraries(purrc PRIVATE purr_flags)
# purrc reads the device member lists from purr/devices.h so PurrLang and C agree.
target_include_directories(purrc PRIVATE "${PROJECT_SOURCE_DIR}/engine/include")

if(EMSCRIPTEN)
    # In web builds purrc is WebAssembly too, run under Node during the build
    # (CMake adds the emulator automatically). It needs the real file system.
    target_link_options(purrc PRIVATE -sNODERAWFS=1)
endif()

add_subdirectory(tests)
