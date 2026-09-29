#include "types.h"

#include <stdio.h>

static const struct {
    const char *name;
    type_kind kind;
    const char *suffix;
    const char *c_name;
} builtins[] = {
    {"bool", TY_BOOL, "b", "bool"},
    {"int", TY_INT, "i", "int32_t"},
    {"int2", TY_INT2, "i2", "purr_int2"},
    {"int3", TY_INT3, "i3", "purr_int3"},
    {"int4", TY_INT4, "i4", "purr_int4"},
    {"float", TY_FLOAT, "f", "float"},
    {"float2", TY_FLOAT2, "f2", "purr_float2"},
    {"float3", TY_FLOAT3, "f3", "purr_float3"},
    {"float4", TY_FLOAT4, "f4", "purr_float4"},
    {"quaternion", TY_QUATERNION, "q", "purr_quaternion"},
    {"float2x2", TY_FLOAT2X2, "f2x2", "purr_float2x2"},
    {"float3x3", TY_FLOAT3X3, "f3x3", "purr_float3x3"},
    {"float4x4", TY_FLOAT4X4, "f4x4", "purr_float4x4"},
    {"Entity", TY_ENTITY, "e", "purr_entity"},
    {"PlayerID", TY_PLAYER, "p", "purr_player_id"},
    {"Color", TY_COLOR, "c", "purr_color"},
};

#define BUILTIN_COUNT (sizeof builtins / sizeof builtins[0])

bool builtin_type_named(const str name, type *out)
{
    for (size_t i = 0; i < BUILTIN_COUNT; i++) {
        if (str_eq_c(name, builtins[i].name)) {
            *out = (type){builtins[i].kind, NULL};
            return true;
        }
    }
    return false;
}

void suggest_builtin_types(suggestion *s)
{
    for (size_t i = 0; i < BUILTIN_COUNT; i++) suggest_consider_c(s, builtins[i].name);
}

int type_dim(const type t)
{
    switch (t.kind) {
    case TY_INT:
    case TY_FLOAT: return 1;
    case TY_INT2:
    case TY_FLOAT2: return 2;
    case TY_INT3:
    case TY_FLOAT3: return 3;
    case TY_INT4:
    case TY_FLOAT4: return 4;
    default: return 0;
    }
}

bool type_is_float_based(const type t)
{
    return t.kind == TY_FLOAT || t.kind == TY_FLOAT2 || t.kind == TY_FLOAT3 || t.kind == TY_FLOAT4;
}

bool type_is_int_based(const type t)
{
    return t.kind == TY_INT || t.kind == TY_INT2 || t.kind == TY_INT3 || t.kind == TY_INT4;
}

bool type_is_numeric(const type t)
{
    return type_dim(t) > 0;
}

int matrix_dim(const type t)
{
    switch (t.kind) {
    case TY_FLOAT2X2: return 2;
    case TY_FLOAT3X3: return 3;
    case TY_FLOAT4X4: return 4;
    default: return 0;
    }
}

type vector_type(const bool is_float, const int dim)
{
    static const type_kind floats[] = {TY_ERROR, TY_FLOAT, TY_FLOAT2, TY_FLOAT3, TY_FLOAT4};
    static const type_kind ints[] = {TY_ERROR, TY_INT, TY_INT2, TY_INT3, TY_INT4};
    if (dim < 1 || dim > 4) return (type){TY_ERROR, NULL};
    return (type){is_float ? floats[dim] : ints[dim], NULL};
}

type matrix_type(const int dim)
{
    static const type_kind kinds[] = {TY_ERROR, TY_ERROR, TY_FLOAT2X2, TY_FLOAT3X3, TY_FLOAT4X4};
    if (dim < 2 || dim > 4) return (type){TY_ERROR, NULL};
    return (type){kinds[dim], NULL};
}

bool type_assignable(const type to, const type from)
{
    if (to.kind == TY_ERROR || from.kind == TY_ERROR) return true;
    if (to.kind == from.kind && to.decl == from.decl) return true;
    return type_is_float_based(to) && type_is_int_based(from) && type_dim(to) == type_dim(from);
}

const char *type_suffix(const type t)
{
    for (size_t i = 0; i < BUILTIN_COUNT; i++) {
        if (builtins[i].kind == t.kind) return builtins[i].suffix;
    }
    return "?";
}

const char *type_name(const type t)
{
    static char buf[4][128];
    static int next;
    switch (t.kind) {
    case TY_ERROR: return "<error>";
    case TY_VOID: return "nothing";
    case TY_STRING: return "string";
    case TY_COMPONENT:
    case TY_SINGLETON:
    case TY_INPUT:
    case TY_RECORD:
    case TY_STRUCT:
    case TY_EVENT: {
        char *b = buf[next++ % 4];
        const str name = t.decl->qualified.len > 0 ? t.decl->qualified : t.decl->name;
        snprintf(b, sizeof buf[0], STR_FMT, STR_ARG(name));
        return b;
    }
    default:
        for (size_t i = 0; i < BUILTIN_COUNT; i++) {
            if (builtins[i].kind == t.kind) return builtins[i].name;
        }
        return "?";
    }
}

const char *type_c_name(const type t)
{
    if (t.kind == TY_STRING) return "const char *";
    for (size_t i = 0; i < BUILTIN_COUNT; i++) {
        if (builtins[i].kind == t.kind) return builtins[i].c_name;
    }
    return "void";
}
