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
