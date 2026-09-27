# New .c files in this folder are picked up automatically on the next build.
file(GLOB PURR_TEST_SOURCES CONFIGURE_DEPENDS *.c)

add_executable(purr_tests ${PURR_TEST_SOURCES})
target_link_libraries(purr_tests PRIVATE purr)

add_test(NAME purr_tests COMMAND purr_tests)
