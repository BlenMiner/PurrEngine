#pragma once

#include "ast.h"

// What tide needs of the game's settings to build it, beyond its C.
typedef struct game_info {
    char title[256];  // The title setting, or "" without one
    char app_id[256]; // appId, likewise
    char version[64]; // version, likewise
} game_info;

typedef struct codegen_options {
    const char *name;        // Base name of the generated files.
    const char *out_dir;
    bool line_directives;    // Map generated code back to .tide lines for debuggers.
    bool layout;             // Describe the data layout as tide_game_layout (tide/layout.h), for hot reloading.
    sb *externs;             // If set, gets the C function of each extern function, a line each (for tide's checks).
    game_info *info;         // If set, gets what tide needs of the game's settings.
} codegen_options;

// Writes <out_dir>/<name>.h and <out_dir>/<name>.c. Returns false on I/O errors.
bool codegen(program *prog, const codegen_options *opts);
