# Flags shared by every Tide target. The engine links this publicly,
# so anything that links the engine gets the same flags.
add_library(tide_flags INTERFACE)

target_compile_options(tide_flags INTERFACE
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
    target_compile_definitions(tide_flags INTERFACE _CRT_SECURE_NO_WARNINGS)
endif()

if(UNIX AND NOT TIDE_WEB)
    # Linux keeps the C math library separate. The engine avoids its functions
    # (see AGENTS.md, Determinism), but clang still calls sqrtf for its error
    # case, and tests use the library for reference values.
    target_link_libraries(tide_flags INTERFACE m)
endif()

if(TIDE_WEB)
    # Web builds: a 1 MB stack like native threads' smallest, and memory that
    # grows as needed (wasi-libc's malloc). Single-threaded: threads need a
    # cross-origin isolated page (see AGENTS.md, Platforms).
    target_link_options(tide_flags INTERFACE -Wl,-z,stack-size=1048576)
endif()
