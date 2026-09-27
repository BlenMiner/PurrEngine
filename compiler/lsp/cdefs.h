#pragma once

#include <stdbool.h>

#include "json.h"

// Where PurrLang's built-ins are defined in C: the engine headers generated
// code includes. Lets "go to definition" land on real code instead of nothing.

// Writes an LSP Location for the C definition of `name`: a function, macro or
// struct, like purr_draw_circle, PURR_COLOR_RED or purr_float3. Vector math that
// macros generate (purr_sin_f3) resolves to the line that generates it. False
// if it isn't found.
bool cdefs_find(const char *name, jbuf *out);

// The same for a member of a C struct, like `x` in purr_float3 or `w` in
// purr_keyboard (members that X-macros declare resolve to their X(...) entry).
bool cdefs_find_member(const char *struct_name, const char *member, jbuf *out);

// The top of an engine header, like "math.h" for `Math`.
bool cdefs_find_header(const char *file, jbuf *out);
