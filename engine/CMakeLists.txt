# New .c files under src/ are picked up automatically on the next build.
file(GLOB_RECURSE TIDE_ENGINE_SOURCES CONFIGURE_DEPENDS src/*.c)

add_library(tide STATIC ${TIDE_ENGINE_SOURCES})
target_include_directories(tide PUBLIC include)
target_link_libraries(tide PUBLIC tide_flags)

# In the package, the engine is sources: tide compiles them with each game.
install(DIRECTORY include/tide DESTINATION include COMPONENT tide)
install(FILES ${TIDE_ENGINE_SOURCES} DESTINATION src/engine COMPONENT tide)
