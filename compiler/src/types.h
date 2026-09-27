#pragma once

#include "ast.h"

// Helpers for PurrLang's built-in types.

// Looks up a built-in type by name: bool, int, float, float3, quaternion, ...
bool builtin_type_named(str name, type *out);

// Offers every built-in type name to a "did you mean" suggestion.
void suggest_builtin_types(suggestion *s);

// Number of components: 1 for int and float, 2-4 for vectors, 0 for anything else.
int type_dim(type t);

// float or a float vector / int or an int vector.
bool type_is_float_based(type t);
bool type_is_int_based(type t);

// int or float (dim 1) and their vectors (dim 2-4).
bool type_is_numeric(type t);

// 2, 3 or 4 for float2x2, float3x3, float4x4; 0 otherwise.
int matrix_dim(type t);

// The int/float scalar or vector with `dim` components.
type vector_type(bool is_float, int dim);
type matrix_type(int dim);

// Can a value of type `from` be stored where `to` is expected? Exact matches,
// plus int -> float and intN -> floatN widening. Errors are always accepted so
// they don't cascade.
bool type_assignable(type to, type from);

// Suffix of the purr/math.h functions for a type: f3, i2, q, f4x4, ...
const char *type_suffix(type t);

// Name for error messages.
const char *type_name(type t);

// C type for generated code.
const char *type_c_name(type t);
