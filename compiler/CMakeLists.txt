# purrc: the PurrLang transpiler. It's a build-time tool, so it doesn't link the engine.
file(GLOB PURRC_SOURCES CONFIGURE_DEPENDS src/*.c)

add_executable(purrc ${PURRC_SOURCES})
target_link_libraries(purrc PRIVATE purr_flags)

add_subdirectory(tests)
