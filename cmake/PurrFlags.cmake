# Flags shared by every PurrEngine target. The engine links this publicly,
# so anything that links the engine gets the same flags.
add_library(purr_flags INTERFACE)

target_compile_options(purr_flags INTERFACE
    -Wall
    -Wextra
    -Wpedantic

    # Determinism (see AGENTS.md). Order matters: -fno-fast-math resets
    # contraction to clang's default ("on"), so -ffp-contract=off must follow it.
    -fno-fast-math
    -ffp-contract=off
)

if(WIN32)
    # Silences MSVC CRT warnings that push non-portable *_s functions.
    target_compile_definitions(purr_flags INTERFACE _CRT_SECURE_NO_WARNINGS)
endif()

if(EMSCRIPTEN)
    # Web builds: programs exit with main's return value (tests and purrc rely on
    # it), worlds live in static memory, and memory may grow. Single-threaded:
    # threads need a cross-origin isolated page (see AGENTS.md, Platforms).
    target_link_options(purr_flags INTERFACE
        -sEXIT_RUNTIME=1
        -sINITIAL_MEMORY=64MB
        -sALLOW_MEMORY_GROWTH=1
        -sSTACK_SIZE=1MB
    )
endif()
