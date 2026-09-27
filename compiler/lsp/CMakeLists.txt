# purrls: the PurrLang language server. Editors run it for completion,
# diagnostics, hovers, go to definition and semantic highlighting.
add_library(purrls_core STATIC analysis.c cdefs.c folders.c json.c server.c)
target_link_libraries(purrls_core PUBLIC purrc_core)
target_include_directories(purrls_core PUBLIC .)
# "Go to definition" on built-ins opens the engine headers they're defined in.
target_compile_definitions(purrls_core PRIVATE "PURR_ENGINE_INCLUDE_DIR=\"${PROJECT_SOURCE_DIR}/engine/include\"")

add_executable(purrls main.c)
target_link_libraries(purrls PRIVATE purrls_core)
# Which files make up each game, for analysing them together (see cmake/PurrLang.cmake).
target_compile_definitions(purrls PRIVATE "PURR_GAMES_MANIFEST=\"${PURR_GAMES_MANIFEST}\"")

if(WIN32)
    # Editors keep purrls running, and Windows won't overwrite a running
    # program. It does allow renaming one, so move it aside before linking.
    add_custom_command(TARGET purrls PRE_LINK
        COMMAND "${CMAKE_COMMAND}" "-DEXE=$<TARGET_FILE:purrls>" -P "${CMAKE_CURRENT_SOURCE_DIR}/move_aside.cmake"
        VERBATIM)
endif()

# Editors run a copy at a fixed path, build/tools/purrls, so they get the
# latest server whichever build tree or IDE profile built it.
set(PURRLS_EDITOR_COPY "${PROJECT_SOURCE_DIR}/build/tools/purrls${CMAKE_EXECUTABLE_SUFFIX}")
add_custom_command(TARGET purrls POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${PROJECT_SOURCE_DIR}/build/tools"
    COMMAND "${CMAKE_COMMAND}" "-DEXE=${PURRLS_EDITOR_COPY}" -P "${CMAKE_CURRENT_SOURCE_DIR}/move_aside.cmake"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:purrls>" "${PURRLS_EDITOR_COPY}"
    VERBATIM)

add_executable(purrls_tests tests/lsp_test.c "${PROJECT_SOURCE_DIR}/tests/test_main.c")
target_include_directories(purrls_tests PRIVATE "${PROJECT_SOURCE_DIR}/tests")
target_link_libraries(purrls_tests PRIVATE purrls_core purr)
add_test(NAME purrls_tests COMMAND purrls_tests)

# The real program over stdin and stdout, as an editor runs it.
add_test(NAME purrls_stdio
    COMMAND "${CMAKE_COMMAND}" "-DPURRLS=$<TARGET_FILE:purrls>" "-DOUT=${CMAKE_CURRENT_BINARY_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/stdio_test.cmake")
