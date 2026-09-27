#include "builtins.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "types.h"

// Every built-in function maps to a purr/math.h function named
// purr_<lowercase name>_<type suffix>, for example Math.Dot on float3 is
// purr_dot_f3. Formulas and semantics follow Unity.Mathematics.

static const type T_F = {TY_FLOAT, NULL};
static const type T_F3 = {TY_FLOAT3, NULL};
static const type T_Q = {TY_QUATERNION, NULL};
static const type T_F4X4 = {TY_FLOAT4X4, NULL};
static const type T_NONE = {TY_VOID, NULL};

// ---------------------------------------------------------------------------
// Component-wise functions: take float or int scalars and vectors, mixed
// freely as long as the vectors have one size; scalars widen to that size.

static const struct {
    const char *name;
    int argc;
    bool ints; // Works on ints too; otherwise ints convert to float.
} componentwise[] = {
    {"Abs", 1, true}, {"Sign", 1, true}, {"Min", 2, true}, {"Max", 2, true}, {"Clamp", 3, true},
    {"Floor", 1, false}, {"Ceil", 1, false}, {"Round", 1, false}, {"Trunc", 1, false}, {"Frac", 1, false},
    {"Sqrt", 1, false}, {"Rsqrt", 1, false}, {"Saturate", 1, false}, {"Radians", 1, false}, {"Degrees", 1, false},
    {"Sin", 1, false}, {"Cos", 1, false}, {"Tan", 1, false}, {"Asin", 1, false}, {"Acos", 1, false},
    {"Atan", 1, false}, {"Atan2", 2, false}, {"Exp", 1, false}, {"Exp2", 1, false}, {"Log", 1, false},
    {"Log2", 1, false}, {"Log10", 1, false}, {"Pow", 2, false}, {"Step", 2, false},
    {"Lerp", 3, false}, {"Unlerp", 3, false}, {"SmoothStep", 3, false},
};

// ---------------------------------------------------------------------------
// Fixed signatures, tried in order; the first whose parameters accept the
// arguments (with implicit int -> float widening) wins.

typedef struct signature {
    const char *owner;
    const char *name;
    type result;
    int argc;
    type params[3];
    const char *c_name;
} signature;

static VEC(signature) signatures;

static const char *lowercase(const char *s)
{
    const size_t n = strlen(s);
    char *out = arena_alloc(n + 1);
    for (size_t i = 0; i < n; i++) out[i] = (char)tolower((unsigned char)s[i]);
    return out;
}

// purr_<lowercase name>_<suffix>
static const char *c_function(const char *name, const type t)
{
    char buf[128];
    snprintf(buf, sizeof buf, "purr_%s_%s", lowercase(name), type_suffix(t));
    char *out = arena_alloc(strlen(buf) + 1);
    memcpy(out, buf, strlen(buf));
    return out;
}

static void add(const char *owner, const char *name, const type result, const char *c_name,
                const int argc, const type p0, const type p1, const type p2)
{
    const signature s = {owner, name, result, argc, {p0, p1, p2}, c_name};
    vec_push(signatures, s);
}

static void build_signatures(void)
{
    if (signatures.count > 0) return;

    for (int n = 2; n <= 4; n++) {
        const type v = vector_type(true, n);
        add("Math", "Dot", T_F, c_function("dot", v), 2, v, v, T_NONE);
        add("Math", "Length", T_F, c_function("length", v), 1, v, T_NONE, T_NONE);
        add("Math", "LengthSq", T_F, c_function("lengthsq", v), 1, v, T_NONE, T_NONE);
        add("Math", "Distance", T_F, c_function("distance", v), 2, v, v, T_NONE);
        add("Math", "DistanceSq", T_F, c_function("distancesq", v), 2, v, v, T_NONE);
        add("Math", "Normalize", v, c_function("normalize", v), 1, v, T_NONE, T_NONE);
        add("Math", "NormalizeSafe", v, c_function("normalizesafe", v), 1, v, T_NONE, T_NONE);
        add("Math", "Reflect", v, c_function("reflect", v), 2, v, v, T_NONE);
        add("Math", "Csum", T_F, c_function("csum", v), 1, v, T_NONE, T_NONE);
        add("Math", "Cmin", T_F, c_function("cmin", v), 1, v, T_NONE, T_NONE);
        add("Math", "Cmax", T_F, c_function("cmax", v), 1, v, T_NONE, T_NONE);
    }
    add("Math", "Cross", T_F3, "purr_cross_f3", 2, T_F3, T_F3, T_NONE);

    // Quaternions
    add("Math", "Dot", T_F, "purr_dot_q", 2, T_Q, T_Q, T_NONE);
    add("Math", "Normalize", T_Q, "purr_normalize_q", 1, T_Q, T_NONE, T_NONE);
    add("Math", "NormalizeSafe", T_Q, "purr_normalizesafe_q", 1, T_Q, T_NONE, T_NONE);
    add("Math", "Mul", T_Q, "purr_mul_q", 2, T_Q, T_Q, T_NONE);
    add("Math", "Mul", T_F3, "purr_rotate_q", 2, T_Q, T_F3, T_NONE);
    add("Math", "Rotate", T_F3, "purr_rotate_q", 2, T_Q, T_F3, T_NONE);
    add("Math", "Inverse", T_Q, "purr_inverse_q", 1, T_Q, T_NONE, T_NONE);
    add("Math", "Conjugate", T_Q, "purr_conjugate_q", 1, T_Q, T_NONE, T_NONE);
    add("Math", "Slerp", T_Q, "purr_slerp_q", 3, T_Q, T_Q, T_F);
    add("Math", "Nlerp", T_Q, "purr_nlerp_q", 3, T_Q, T_Q, T_F);
    add("Math", "Forward", T_F3, "purr_forward_q", 1, T_Q, T_NONE, T_NONE);
    add("Math", "Up", T_F3, "purr_up_q", 1, T_Q, T_NONE, T_NONE);
    add("Math", "Right", T_F3, "purr_right_q", 1, T_Q, T_NONE, T_NONE);
    add("Math", "Angle", T_F, "purr_angle_q", 2, T_Q, T_Q, T_NONE);

    // Matrices
    for (int n = 2; n <= 4; n++) {
        const type m = matrix_type(n);
        const type v = vector_type(true, n);
        char mul_vec[64];
        snprintf(mul_vec, sizeof mul_vec, "purr_mul_%s_%s", type_suffix(m), type_suffix(v));
        char *mul_vec_name = arena_alloc(strlen(mul_vec) + 1);
        memcpy(mul_vec_name, mul_vec, strlen(mul_vec));
        add("Math", "Mul", m, c_function("mul", m), 2, m, m, T_NONE);
        add("Math", "Mul", v, mul_vec_name, 2, m, v, T_NONE);
        add("Math", "Transpose", m, c_function("transpose", m), 1, m, T_NONE, T_NONE);
        add("Math", "Inverse", m, c_function("inverse", m), 1, m, T_NONE, T_NONE);
        add("Math", "Determinant", T_F, c_function("determinant", m), 1, m, T_NONE, T_NONE);
    }
    add("Math", "Transform", T_F3, "purr_transform_f4x4", 2, T_F4X4, T_F3, T_NONE);
    add("Math", "Rotate", T_F3, "purr_rotate_f4x4", 2, T_F4X4, T_F3, T_NONE);

    // Ways to build quaternions and matrices
    add("quaternion", "AxisAngle", T_Q, "purr_axisangle_q", 2, T_F3, T_F, T_NONE);
    add("quaternion", "Euler", T_Q, "purr_euler_q", 1, T_F3, T_NONE, T_NONE);
    add("quaternion", "LookRotation", T_Q, "purr_lookrotation_q", 2, T_F3, T_F3, T_NONE);
    add("float4x4", "TRS", T_F4X4, "purr_trs_f4x4", 3, T_F3, T_Q, T_F3);
    add("float4x4", "Translate", T_F4X4, "purr_translate_f4x4", 1, T_F3, T_NONE, T_NONE);
}

// ---------------------------------------------------------------------------

bool builtin_owner(const str name)
{
    type ignored;
    return str_eq_c(name, "Math") || builtin_type_named(name, &ignored);
}

// Widest type of a component-wise call's arguments, or false if they don't fit together.
static bool unify(const expr *e, const bool ints_ok, type *out)
{
    int dim = 1;
    bool is_float = !ints_ok;
    for (int i = 0; i < e->args.count; i++) {
        const type t = e->args.items[i]->type;
        if (!type_is_numeric(t)) return false;
        const int d = type_dim(t);
        if (d > 1) {
            if (dim > 1 && d != dim) return false;
            dim = d;
        }
        if (type_is_float_based(t)) is_float = true;
    }
    *out = vector_type(is_float, dim);
    return true;
}

static void arg_list(const expr *e, char *buf, const size_t size)
{
    size_t len = 0;
    buf[0] = '\0';
    for (int i = 0; i < e->args.count && len + 1 < size; i++) {
        const int n = snprintf(buf + len, size - len, "%s%s", i ? ", " : "", type_name(e->args.items[i]->type));
        if (n > 0) len += (size_t)n;
    }
}

type resolve_builtin_call(const str owner, expr *e)
{
    for (int i = 0; i < e->args.count; i++) {
        if (e->args.items[i]->type.kind == TY_ERROR) return (type){TY_ERROR, NULL};
    }
    e->call = CALL_BUILTIN;
    e->arg_want.count = 0;

    if (str_eq_c(owner, "Math")) {
        for (size_t i = 0; i < sizeof componentwise / sizeof componentwise[0]; i++) {
            if (!str_eq_c(e->name, componentwise[i].name)) continue;
            if (e->args.count != componentwise[i].argc) {
                diag_error(e->at, "Math.%s takes %d argument%s, not %d", componentwise[i].name, componentwise[i].argc,
                           componentwise[i].argc == 1 ? "" : "s", e->args.count);
                return (type){TY_ERROR, NULL};
            }
            type target;
            if (!unify(e, componentwise[i].ints, &target)) {
                char args[256];
                arg_list(e, args, sizeof args);
                diag_error(e->at, "Math.%s can't take (%s)", componentwise[i].name, args);
                diag_note("it takes numbers and vectors; vectors in one call must be the same size");
                return (type){TY_ERROR, NULL};
            }
            for (int a = 0; a < e->args.count; a++) vec_push(e->arg_want, target);
            e->c_callee = c_function(componentwise[i].name, target);
            return target;
        }
    }

    build_signatures();
    bool known = false;
    for (int i = 0; i < signatures.count; i++) {
        const signature *s = &signatures.items[i];
        if (!str_eq_c(owner, s->owner) || !str_eq_c(e->name, s->name)) continue;
        known = true;
        if (s->argc != e->args.count) continue;
        bool fits = true;
        for (int a = 0; a < s->argc; a++) {
            if (!type_assignable(s->params[a], e->args.items[a]->type)) fits = false;
        }
        if (!fits) continue;
        for (int a = 0; a < s->argc; a++) vec_push(e->arg_want, s->params[a]);
        e->c_callee = s->c_name;
        return s->result;
    }

    if (!known) {
        diag_error(e->at, STR_FMT " has no function '" STR_FMT "'", STR_ARG(owner), STR_ARG(e->name));
        return (type){TY_ERROR, NULL};
    }
    char args[256];
    arg_list(e, args, sizeof args);
    diag_error(e->at, "no version of " STR_FMT "." STR_FMT " takes (%s)", STR_ARG(owner), STR_ARG(e->name), args);
    for (int i = 0; i < signatures.count; i++) {
        const signature *s = &signatures.items[i];
        if (!str_eq_c(owner, s->owner) || !str_eq_c(e->name, s->name)) continue;
        char params[256] = "";
        size_t len = 0;
        for (int a = 0; a < s->argc; a++) {
            const int n = snprintf(params + len, sizeof params - len, "%s%s", a ? ", " : "", type_name(s->params[a]));
            if (n > 0) len += (size_t)n;
        }
        diag_note("%s.%s(%s) -> %s", s->owner, s->name, params, type_name(s->result));
    }
    return (type){TY_ERROR, NULL};
}

type resolve_builtin_member(const str owner, expr *e)
{
    static const struct {
        const char *owner;
        const char *member;
        type_kind kind;
        const char *c_constant;
    } members[] = {
        {"Math", "PI", TY_FLOAT, "PURR_PI_F"},
        {"Math", "Tau", TY_FLOAT, "PURR_TAU_F"},
        {"Math", "E", TY_FLOAT, "PURR_E_F"},
        {"quaternion", "Identity", TY_QUATERNION, "purr_identity_q()"},
        {"float2x2", "Identity", TY_FLOAT2X2, "purr_identity_f2x2()"},
        {"float3x3", "Identity", TY_FLOAT3X3, "purr_identity_f3x3()"},
        {"float4x4", "Identity", TY_FLOAT4X4, "purr_identity_f4x4()"},
    };
    for (size_t i = 0; i < sizeof members / sizeof members[0]; i++) {
        if (str_eq_c(owner, members[i].owner) && str_eq_c(e->member, members[i].member)) {
            e->c_constant = members[i].c_constant;
            return (type){members[i].kind, NULL};
        }
    }
    diag_error(e->at, STR_FMT " has no member '" STR_FMT "'", STR_ARG(owner), STR_ARG(e->member));
    return (type){TY_ERROR, NULL};
}
