# New .c files in this folder are picked up automatically on the next build.
file(GLOB TIDE_TEST_SOURCES CONFIGURE_DEPENDS *.c)

add_executable(tide_tests ${TIDE_TEST_SOURCES})
target_link_libraries(tide_tests PRIVATE tide)

add_test(NAME tide_tests COMMAND tide_tests)
