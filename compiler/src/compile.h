#pragma once

#include "codegen.h"

// The whole transpiler, as purrc and the purr command run it: reads the files,
// checks the program and writes the C (see codegen). `inputs` is sorted by
// path first, which decides the default order of systems. With `schedule`, it
// prints the tick's schedule there instead of writing code. Errors go to
// stderr; returns false after reporting them.
bool compile_program(const char **inputs, int count, const codegen_options *opts, sb *schedule);

// "dir/game.purr" -> "game", in the arena.
const char *path_stem(const char *path);
