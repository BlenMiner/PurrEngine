#pragma once

#include "codegen.h"

// The whole transpiler, as tidec and the tide command run it: reads the files,
// checks the program and writes the C (see codegen). `inputs` is sorted by
// path first, which decides the default order of systems. With `schedule`, it
// prints the tick's schedule there instead of writing code. Errors go to
// stderr; returns false after reporting them.
bool compile_program(const char **inputs, int count, const codegen_options *opts, sb *schedule);

// One file of a game, as tide gives them.
typedef struct compile_input {
    const char *path;
    const char *package; // The package it's in, by name; NULL for the game's own files
} compile_input;

// compile_program for files in the order given: tide's, which is each
// package's files, then the game's (see docs/spec.md, Packages).
bool compile_inputs(const compile_input *inputs, int count, const codegen_options *opts, sb *schedule);

// "dir/game.tide" -> "game", in the arena.
const char *path_stem(const char *path);
