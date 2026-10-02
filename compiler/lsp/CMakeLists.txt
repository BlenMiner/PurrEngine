# tidels: the Tide language server. Editors run it for completion,
# diagnostics, hovers, go to definition and semantic highlighting.
add_library(tidels_core STATIC analysis.c cdefs.c folders.c json.c packages.c server.c)
target_link_libraries(tidels_core PUBLIC tidec_core)
target_include_directories(tidels_core PUBLIC .)
# "Go to definition" on built-ins opens the engine headers they're defined in.
target_compile_definitions(tidels_core PRIVATE "TIDE_ENGINE_INCLUDE_DIR=\"${PROJECT_SOURCE_DIR}/engine/include\"")

add_executable(tidels main.c)
target_link_libraries(tidels PRIVATE tidels_core)
install(TARGETS tidels RUNTIME DESTINATION bin COMPONENT tide)
# Which files make up each game, for analysing them together (see cmake/Tide.cmake).
target_compile_definitions(tidels PRIVATE "TIDE_GAMES_MANIFEST=\"${TIDE_GAMES_MANIFEST}\"")

if(WIN32)
    # Editors keep tidels running, and Windows won't overwrite a running
    # program. It does allow renaming one, so move it aside before linking.
    add_custom_command(TARGET tidels PRE_LINK
        COMMAND "${CMAKE_COMMAND}" "-DEXE=$<TARGET_FILE:tidels>" -P "${CMAKE_CURRENT_SOURCE_DIR}/move_aside.cmake"
        VERBATIM)
endif()

# Editors run a copy at a fixed path, build/tools/tidels, so they get the
# latest server whichever build tree or IDE profile built it.
set(TIDELS_EDITOR_COPY "${PROJECT_SOURCE_DIR}/build/tools/tidels${CMAKE_EXECUTABLE_SUFFIX}")
add_custom_command(TARGET tidels POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${PROJECT_SOURCE_DIR}/build/tools"
    COMMAND "${CMAKE_COMMAND}" "-DEXE=${TIDELS_EDITOR_COPY}" -P "${CMAKE_CURRENT_SOURCE_DIR}/move_aside.cmake"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:tidels>" "${TIDELS_EDITOR_COPY}"
    VERBATIM)

add_executable(tidels_tests tests/lsp_test.c "${PROJECT_SOURCE_DIR}/tests/test_main.c")
target_include_directories(tidels_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests")
target_link_libraries(tidels_tests PRIVATE tidels_core tide)
add_test(NAME tidels_tests COMMAND tidels_tests)

# The real program over stdin and stdout, as an editor runs it.
add_test(NAME tidels_stdio
    COMMAND "${CMAKE_COMMAND}" "-DTIDELS=$<TARGET_FILE:tidels>" "-DOUT=${CMAKE_CURRENT_BINARY_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/stdio_test.cmake")
