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
    TY_FLOAT,
    TY_FLOAT3,
    TY_ENTITY,
    TY_COMPONENT,
    TY_SINGLETON,
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
} decl_kind;

typedef struct decl {
    decl_kind kind;
    str name;
    loc at;
    bool builtin;

    // Components and singletons
    VEC(field) fields;
    int index; // Component bit / singleton index / system order.

    // Systems
    VEC(param) params;
    stmt *body;
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
    CALL_FLOAT3,
    CALL_SPAWN,
    CALL_ADD,
    CALL_REMOVE,
    CALL_DESTROY,
} builtin_call;

typedef enum binding_kind {
    BIND_NONE,
    BIND_PARAM,
    BIND_LOCAL,
    BIND_TYPE, // A component name used as a value: Spawn(Player).
} binding_kind;

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
    field *field;       // Component or singleton field.
    int swizzle;        // float3 member: 0, 1, 2 for x, y, z; -1 otherwise.

    // E_CALL, E_METHOD
    VEC(expr *) args;
    builtin_call call;
    uint64_t spawn_mask;  // CALL_SPAWN: components the new entity has.
    int spawn_archetype;  // CALL_SPAWN: index into the archetype list.

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

    VEC(uint64_t) archetypes;  // Component masks, in derivation order.
    VEC(bool) spawn_target;    // Per archetype: does some Spawn create it directly?
    uint64_t spawned_mask;     // Union of components that appear in spawns.
    uint64_t added_mask;       // Components that appear in Add.
    uint64_t removed_mask;     // Components that appear in Remove.
    bool uses_destroy;
} program;

program *parse(const source *src, token *toks);
bool check(program *prog);
