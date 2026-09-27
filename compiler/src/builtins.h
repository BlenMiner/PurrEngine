#pragma once

#include "ast.h"

// Built-in functions and constants reached through a type or `Math`:
// Math.Dot(a, b), quaternion.AxisAngle(axis, angle), quaternion.Identity, Math.PI.

// Is `name` something that can own static members: Math or a built-in type?
bool builtin_owner(str name);

// Resolves owner.name(args) for a call whose arguments are already checked.
// Fills e->call, e->c_callee and e->arg_want. Reports errors and returns TY_ERROR on failure.
type resolve_builtin_call(str owner, expr *e);

// Resolves owner.member, such as quaternion.Identity. Fills e->c_constant.
type resolve_builtin_member(str owner, expr *e);
