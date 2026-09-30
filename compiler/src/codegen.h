#pragma once

#include "ast.h"

typedef struct codegen_options {
    const char *name;        // Base name of the generated files.
    const char *out_dir;
    bool line_directives;    // Map generated code back to .purr lines for debuggers.
    bool layout;             // Describe the data layout as purr_game_layout (purr/layout.h), for hot reloading.
} codegen_options;

// Writes <out_dir>/<name>.h and <out_dir>/<name>.c. Returns false on I/O errors.
bool codegen(program *prog, const codegen_options *opts);
