#pragma once

#include "ast.h"

// Built-in functions and constants reached through a type, `Math` or `Draw`:
// Math.Dot(a, b), quaternion.AxisAngle(axis, angle), Draw.Circle(...), Color.red.

// Is `name` something that can own static members: Math, Draw or a built-in type?
bool builtin_owner(str name);

// Resolves owner.name(args) for a call whose arguments are already checked.
// Fills e->call, e->c_callee and e->arg_want. Reports errors and returns TY_ERROR on failure.
type resolve_builtin_call(str owner, expr *e);

// Resolves owner.member, such as quaternion.identity. Fills e->c_constant.
type resolve_builtin_member(str owner, expr *e);

// The owner of a built-in function named `name` ("Math" for Sin), or NULL.
const char *builtin_function_owner(str name);

// ---------------------------------------------------------------------------
// For editors (the language server)

typedef struct builtin_member {
    const char *name;
    bool is_function;
    const char *detail;  // "Draw.Circle(float2 center, float radius, Color color)", or "Math.PI: float"
    const char *doc;     // One line, or NULL
    const char *snippet; // "Circle(${1:center}, ...)" for functions, NULL for constants
} builtin_member;

// Calls `visit` for every static member of `owner`, overloads once.
void builtin_list_members(str owner, void (*visit)(void *user, const builtin_member *m), void *user);

// Appends every signature of owner.name, one per line, then its doc. False if unknown.
bool builtin_describe(str owner, str name, sb *out);

// Calls `visit` with each signature of owner.name, like
// "Draw.Circle(float2 center, float radius, Color color)", and its doc (or NULL).
void builtin_signatures(str owner, str name, void (*visit)(void *user, const char *label, const char *doc), void *user);
