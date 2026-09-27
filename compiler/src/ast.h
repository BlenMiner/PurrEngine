#pragma once

#include "common.h"
#include "lexer.h"

typedef struct decl decl;
typedef struct expr expr;
typedef struct stmt stmt;

// ---------------------------------------------------------------------------
// Types

typedef enum type_kind {
    TY_ERROR, // Already reported; suppresses follow-up errors.
    TY_VOID,
    TY_BOOL,
    TY_INT,
    TY_INT2,
    TY_INT3,
    TY_INT4,
    TY_FLOAT,
    TY_FLOAT2,
    TY_FLOAT3,
    TY_FLOAT4,
    TY_QUATERNION,
    TY_FLOAT2X2,
    TY_FLOAT3X3,
    TY_FLOAT4X4,
    TY_ENTITY,
    TY_PLAYER,     // PlayerID
    TY_COMPONENT,
    TY_SINGLETON,
    TY_INPUT,      // The game's input declaration
    TY_RECORD,     // Built-in read-only data: Devices, Keyboard, Button, ...
} type_kind;

typedef struct type {
    type_kind kind;
    decl *decl; // For components and singletons.
} type;

// ---------------------------------------------------------------------------
// Declarations

typedef struct field {
    str name;
    str type_name;
    loc at;
    type type;
    expr *default_value; // Constant expression, or NULL for zero.
} field;

typedef enum param_mode {
    PARAM_READ,
    PARAM_MUT,
    PARAM_WITH,
    PARAM_WITHOUT,
} param_mode;

typedef struct param {
    param_mode mode;
    str type_name;
    str name; // Empty for with/without.
    loc at;
    type type;
} param;

typedef enum decl_kind {
    DECL_COMPONENT,
    DECL_SINGLETON,
    DECL_SYSTEM,
    DECL_INPUT,  // input PlayerInput { fields; PlayerInput(Devices devices) { ... } }
    DECL_RECORD, // Built-in device data; not in program.decls
} decl_kind;

typedef struct decl {
    decl_kind kind;
    str name;
    loc at;
    bool builtin;
    const char *c_name; // Records: the C struct name.

    // Components, singletons, inputs and records
    VEC(field) fields;
    int index; // Component bit / singleton index / system order.

    // Systems, and an input's constructor
    VEC(param) params;
    stmt *body;
    loc body_at;
    bool is_main;
    bool per_entity;     // Runs once per matching entity, not once per tick.
    uint64_t need_mask;  // Components an entity must have (access and `with`).
    uint64_t without_mask;
} decl;

// ---------------------------------------------------------------------------
// Expressions

typedef enum expr_kind {
    E_INT,
    E_FLOAT,
    E_BOOL,
    E_NAME,
    E_MEMBER,
    E_CALL,    // name(args): float3(...), Spawn(...)
    E_METHOD,  // object.name(args): e.Add(...)
    E_BINARY,
    E_UNARY,
    E_LITERAL, // Transform { position = ... }
} expr_kind;

typedef enum builtin_call {
    CALL_NONE,
    CALL_CONSTRUCT, // float3(...), int(...), quaternion(...), float4x4(...)
    CALL_BUILTIN,   // Math.Dot(...), quaternion.AxisAngle(...): calls c_callee
    CALL_SPAWN,
    CALL_ADD,
    CALL_REMOVE,
    CALL_DESTROY,
} builtin_call;

// How a constructor call builds its value.
typedef enum ctor_form {
    CTOR_SCALAR,         // float(x), int(x)
    CTOR_SPLAT,          // float3(1): every component the same
    CTOR_CONVERT,        // float3(int3), int3(float3)
    CTOR_COMPONENTS,     // float4(v.xy, 1, 2): components from scalars and vectors
    CTOR_QUAT_FROM_F4,   // quaternion(float4)
    CTOR_QUAT_FROM_MAT,  // quaternion(float3x3)
    CTOR_MAT_COLUMNS,    // float3x3(c0, c1, c2)
    CTOR_MAT_SCALARS,    // float2x2(m00, m01, m10, m11), row by row
    CTOR_MAT_FROM_QUAT,  // float3x3(quaternion)
    CTOR_MAT_FROM_ROT_T, // float4x4(float3x3 rotation, float3 translation)
    CTOR_PLAYER,         // PlayerID(index)
} ctor_form;

typedef enum binding_kind {
    BIND_NONE,
    BIND_PARAM,
    BIND_LOCAL,
    BIND_TYPE,  // A component name used as a value: Spawn(Player).
    BIND_FIELD, // A field of the input, named directly inside its constructor.
} binding_kind;

typedef enum input_edge {
    EDGE_NONE,
    EDGE_PRESSED,  // input.jump.pressed: true now, false last tick
    EDGE_RELEASED, // input.jump.released: false now, true last tick
} input_edge;

typedef struct field_init {
    str name;
    loc at;
    expr *value;
    field *field;
} field_init;

struct expr {
    expr_kind kind;
    loc at;
    type type;

    // E_INT, E_FLOAT
    str text;
    int64_t int_value;

    // E_BOOL
    bool bool_value;

    // E_NAME, and the callee/method name for E_CALL/E_METHOD
    str name;
    binding_kind bind;
    param *param;       // BIND_PARAM
    stmt *local;        // BIND_LOCAL: the S_VAR declaring it
    decl *type_decl;    // BIND_TYPE, and E_LITERAL's component

    // E_MEMBER, E_METHOD
    expr *object;
    str member;
    field *field;          // Field of a component, singleton, input or record; BIND_FIELD's field.
    input_edge edge;       // .pressed / .released on an input field.
    int swizzle_len;       // Vector swizzle: number of components (0 if not a swizzle).
    int swizzle[4];        // Component indices: 0..3 for x, y, z, w.
    const char *c_constant; // Static member such as quaternion.Identity, as C.

    // E_CALL, E_METHOD
    VEC(expr *) args;
    builtin_call call;
    ctor_form ctor;         // CALL_CONSTRUCT
    const char *c_callee;   // CALL_BUILTIN
    VEC(type) arg_want;     // CALL_BUILTIN: the type each argument converts to.
    uint64_t spawn_mask;  // CALL_SPAWN: components the new entity has.
    int spawn_archetype;  // CALL_SPAWN: index into the archetype list.
    const char *hoisted;  // CALL_SPAWN: the temporary codegen ran it into, before the statement.

    // E_BINARY, E_UNARY
    tok_kind op;
    expr *lhs;
    expr *rhs;

    // E_LITERAL
    VEC(field_init) inits;
};

// ---------------------------------------------------------------------------
// Statements

typedef enum stmt_kind {
    S_BLOCK,
    S_IF,
    S_RETURN,
    S_VAR,
    S_ASSIGN,
    S_EXPR,
} stmt_kind;

struct stmt {
    stmt_kind kind;
    loc at;

    // S_BLOCK
    VEC(stmt *) stmts;

    // S_IF
    expr *cond;
    stmt *then_stmt;
    stmt *else_stmt;

    // S_VAR
    bool is_mut;
    str type_name; // Empty for `var`.
    str name;
    type type;

    // S_VAR initializer, S_ASSIGN value, S_EXPR expression
    expr *value;

    // S_ASSIGN
    expr *target;
    tok_kind op;
};

// ---------------------------------------------------------------------------
// Program

typedef struct program {
    const source *src;
    VEC(decl *) decls; // In source order, builtins first.

    // Filled by the checker
    VEC(decl *) components;
    VEC(decl *) singletons;
    VEC(decl *) systems; // Excluding Main.
    decl *main;
    decl *input;         // The input declaration, if any.
    decl *owner;         // The built-in Owner component.
    decl *devices;       // The built-in Devices record.
    VEC(decl *) records; // Built-in records: Devices, Keyboard, Mouse, Gamepad, Dpad, Button.

    VEC(uint64_t) archetypes;  // Component masks, in derivation order.
    VEC(bool) spawn_target;    // Per archetype: does some Spawn create it directly?
    uint64_t spawned_mask;     // Union of components that appear in spawns.
    uint64_t added_mask;       // Components that appear in Add.
    uint64_t removed_mask;     // Components that appear in Remove.
    bool uses_destroy;
} program;

program *parse(const source *src, token *toks);
bool check(program *prog);
