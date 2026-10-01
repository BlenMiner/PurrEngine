#pragma once

#include <stdbool.h>

typedef struct build_options {
    const char *folder; // The game: every .tide file in it and its subfolders
    bool release;       // Optimized, no console window on Windows
    bool web;           // One self-contained .html, with clang's WebAssembly target
    const char *output; // Where the program goes; NULL for <folder>/build/<name>
    const char *title;  // The window's title; NULL for the folder's name
    bool stats;         // Show the tick, entity count and frame rate
} build_options;

// Builds a game with the engine files installed in `root` (see AGENTS.md,
// Packaging). Returns the program's path (malloc'd), or NULL after reporting
// what went wrong.
char *tide_build(const char *root, const build_options *opts);

// Runs a game on this machine, as `tide run` does natively: the game is a
// library in a small host program, rebuilt whenever one of its files changes and
// swapped into the running game (see tide/host.h). `args` go to the game
// (--host, --join, --connect), ending with NULL. Returns the game's exit code.
int tide_run_reloading(const char *root, const build_options *opts, const char *const *args);

// The same on the web (`tide run --web`): tide serves the game's page on this
// machine, opens it in a browser if `open_page`, and builds the game again
// whenever one of its files changes, which the page starts in the running build's
// place (see platform/web/tide.js). `args` go to the game, as for
// tide_run_reloading. Runs until it's stopped.
int tide_run_web(const char *root, const build_options *opts, bool open_page, const char *const *args);

// Prints the schedule of the game in `folder` (see tidec --schedule).
bool tide_schedule(const char *folder);
