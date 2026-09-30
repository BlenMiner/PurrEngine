#pragma once

#include <stdbool.h>

typedef struct build_options {
    const char *folder; // The game: every .purr file in it and its subfolders
    bool release;       // Optimized, no console window on Windows
    bool web;           // One self-contained .html, with clang's WebAssembly target
    const char *output; // Where the program goes; NULL for <folder>/build/<name>
    const char *title;  // The window's title; NULL for the folder's name
    bool stats;         // Show the tick, entity count and frame rate
} build_options;

// Builds a game with the engine files installed in `root` (see AGENTS.md,
// Packaging). With `for_run`, the program stays in the game's .purr folder
// instead of going to `output`. Returns the program's path (malloc'd), or NULL
// after reporting what went wrong.
char *purr_build(const char *root, const build_options *opts, bool for_run);

// Runs a game on this machine, as `purr run` does natively: the game is a
// library in a small host program, rebuilt whenever a .purr file changes and
// swapped into the running game (see purr/host.h). `args` go to the game
// (--host, --join), ending with NULL. Returns the game's exit code.
int purr_run_reloading(const char *root, const build_options *opts, const char *const *args);

// Prints the schedule of the game in `folder` (see purrc --schedule).
bool purr_schedule(const char *folder);
