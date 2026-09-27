# New .c files under src/ are picked up automatically on the next build.
file(GLOB_RECURSE PURR_ENGINE_SOURCES CONFIGURE_DEPENDS src/*.c)

add_library(purr STATIC ${PURR_ENGINE_SOURCES})
target_include_directories(purr PUBLIC include)
target_link_libraries(purr PUBLIC purr_flags)
