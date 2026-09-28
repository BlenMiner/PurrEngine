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
    TY_COLOR,
    TY_STRING,     // Text literals; only Draw.Text takes them for now.
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
// Files and names

// One source file of a program.
typedef struct unit {
    const source *src;
    str ns;           // `namespace Game.Combat;`, or empty for the global namespace
    loc ns_at;
    VEC(str) usings;  // `using Physics;`
    VEC(loc) using_at;
} unit;

// A name that may be qualified by a namespace: Health, Combat.Health.
typedef struct qname {
    str text;    // Without spaces: "Combat.Health"
    loc at;      // The first part
    loc name_at; // The last part: the name itself
    struct decl *decl; // What it names, once checked
} qname;

// [Before(Physics.Integrate)]
typedef struct attribute {
    str name;
    loc at;
    VEC(qname) args;     // On declarations, names: [After(Physics.Gravity)]
    VEC(expr *) values;  // On input fields, constant values: [Clamp(-1, 1)]
} attribute;

// ---------------------------------------------------------------------------
// Declarations

typedef struct field {
    str name;
    str type_name;
    loc at;      // The name
    type type;
    expr *default_value; // Constant expression, or NULL for zero.
    loc type_at; // The type name
    VEC(attribute) attributes; // [Clamp], [Min] and [Max] on input fields
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
    loc at;   // The first token, a modifier or the type
    type type;
    loc type_at;      // The type's name (the last part if it's qualified)
    loc type_qual_at; // Where the type starts: its namespace if it's qualified
    loc name_at;
    bool read;    // The body names it
    bool written; // The body assigns through it
} param;

// Why a system waits for one that runs before it in the tick (see parallel.c).
typedef enum conflict_kind {
    CONFLICT_BOTH_WRITE,     // Both write the data
    CONFLICT_EARLIER_READS,  // The earlier system reads what this one writes
    CONFLICT_EARLIER_WRITES, // The earlier system writes what this one reads
} conflict_kind;

typedef struct conflict {
    struct decl *data; // A component or singleton
    conflict_kind kind;
} conflict;

typedef struct system_wait {
    struct decl *on;         // A system earlier in the tick
    VEC(conflict) conflicts; // The data they share; empty if only an attribute orders them
    bool ordered;            // [After] or [Before] orders them
    struct decl *through;    // Another system it waits for that already waits for `on`, or NULL
} system_wait;

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
    str qualified; // With its namespace, for messages: "Combat.Health"
    loc at;
    loc end; // The closing brace
    const unit *unit; // The file it's in; NULL for built-ins
    VEC(attribute) attributes;
    bool builtin;
    const char *c_name; // Records: the C struct name.

    // Components, singletons, inputs and records
    VEC(field) fields;
    int index; // Component bit / singleton index / system order.

    // Systems, views, and an input's Sample
    VEC(param) params;
    stmt *body;
    loc body_at;
    stmt *sanitize; // An input's Sanitize() { ... }
    loc sanitize_at;
    bool is_view;        // A view: a DECL_SYSTEM that runs once per frame, reads the world and draws.
    bool is_main;
    bool per_entity;     // Runs once per matching entity, not once per tick.
    uint64_t need_mask;  // Components an entity must have (access and `with`).
    uint64_t without_mask;
    VEC(struct decl *) after; // Systems or views that must run first ([After], and [Before] on them)

    // Systems in the tick, from analyze_parallelism
    VEC(system_wait) waits;       // Earlier systems it must wait for
    VEC(struct decl *) alongside; // Systems it can run at the same time as
    int stage;                    // 1 + the deepest stage it waits for; 0 before the analysis
} decl;

// ---------------------------------------------------------------------------
// Expressions

typedef enum expr_kind {
    E_INT,
    E_FLOAT,
    E_BOOL,
    E_STRING,  // "text"; `text` holds it without the quotes, escapes as written
    E_NAME,
    E_MEMBER,
    E_CALL,    // name(args): float3(...), Spawn(...)
    E_METHOD,  // object.name(args): e.Add(...)
    E_BINARY,
    E_UNARY,
    E_LITERAL, // Transform { position = ... }
    E_CONDITIONAL, // cond ? lhs : rhs
} expr_kind;

typedef enum builtin_call {
    CALL_NONE,
    CALL_CONSTRUCT, // float3(...), int(...), quaternion(...), float4x4(...)
    CALL_BUILTIN,   // Math.Dot(...), quaternion.AxisAngle(...): calls c_callee
    CALL_DRAW,      // Draw.Circle(...): calls c_callee with the view's draw list first
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
    CTOR_COLOR,          // Color(r, g, b) or Color(r, g, b, a)
} ctor_form;

typedef enum binding_kind {
    BIND_NONE,
    BIND_PARAM,
    BIND_LOCAL,
    BIND_TYPE,  // A component name used as a value: Spawn(Player), Spawn(Combat.Health).
    BIND_FIELD, // A field of the input, named directly inside its Sample or Sanitize.
    BIND_NAMESPACE, // `Combat` in Combat.Health
} binding_kind;

typedef enum input_edge {
    EDGE_NONE,
    EDGE_DOWN, // input.jump.down: true now, false last tick
    EDGE_UP,   // input.jump.up: false now, true last tick
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
    input_edge edge;       // .down / .up on an input field.
    int swizzle_len;       // Vector swizzle: number of components (0 if not a swizzle).
    int swizzle[4];        // Component indices: 0..3 for x, y, z, w.
    const char *c_constant; // Static member such as quaternion.identity, as C.

    // E_CALL, E_METHOD
    VEC(expr *) args;
    builtin_call call;
    ctor_form ctor;         // CALL_CONSTRUCT
    const char *c_callee;   // CALL_BUILTIN
    VEC(type) arg_want;     // CALL_BUILTIN: the type each argument converts to.
    uint64_t spawn_mask;  // CALL_SPAWN: components the new entity has.
    int spawn_archetype;  // CALL_SPAWN: index into the archetype list.
    const char *hoisted;  // CALL_SPAWN: the temporary codegen ran it into, before the statement.

    // E_BINARY, E_UNARY; E_CONDITIONAL's two sides
    tok_kind op;
    expr *lhs;
    expr *rhs;
    expr *cond; // E_CONDITIONAL

    // E_LITERAL
    VEC(field_init) inits;
    loc qual_at; // Where a qualified name starts (Combat.Health { }); `at` is its last part
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
    loc end; // The closing brace, or where a block cut short by a syntax error ends

    // S_IF
    expr *cond;
    stmt *then_stmt;
    stmt *else_stmt;

    // S_VAR
    bool is_mut;
    str type_name; // Empty for `var`.
    str name;
    type type;
    loc type_at;
    loc name_at;

    // S_VAR initializer, S_ASSIGN value, S_EXPR expression
    expr *value;

    // S_ASSIGN
    expr *target;
    tok_kind op;
};

// ---------------------------------------------------------------------------
// Program

typedef struct program {
    VEC(unit *) units; // Its files, in the order they're parsed: sorted by path
    VEC(decl *) decls; // In source order, file by file, builtins first.

    // Filled by the checker
    VEC(decl *) components;
    VEC(decl *) singletons;
    VEC(decl *) systems; // Excluding Main.
    VEC(decl *) views;
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
    VEC(struct fix) fixes;     // Quick fixes for editors
} program;

// A change that fixes a diagnostic, which editors offer as a quick fix.
typedef enum fix_kind {
    FIX_ADD_MUT,    // Writing through a read-only parameter: declare it mut
    FIX_REMOVE_MUT, // A mut parameter that's never written
    FIX_USE_WITH,   // A component parameter that's never used: filter with `with` instead
} fix_kind;

typedef struct fix {
    fix_kind kind;
    loc at; // Where the diagnostic points
    const param *param;
} fix;

program *program_new(void);

// Works out which systems in the tick can run at the same time, and why the
// others wait (fills decl.waits, alongside and stage). Two systems conflict
// when one writes a component or singleton the other reads or writes, unless
// their entities can never be the same (disjoint archetypes). Conflicting
// systems keep their order in the tick, and [Before]/[After] order them too.
// Needs the archetypes.
void analyze_parallelism(program *prog);

// Why `sys` waits for `w->on`, like "both write Transform". `quote` wraps names
// ("`" for Markdown).
void describe_wait(const decl *sys, const system_wait *w, const char *quote, sb *out);

// A declaration's name as code in `from`'s namespace writes it: short in the
// same namespace, qualified elsewhere, and always qualified without `from`.
void put_decl_name(sb *out, const decl *d, const char *quote, const decl *from);

// The whole tick's schedule as text, for `purrc --schedule`.
void print_schedule(const program *prog, const char *game, sb *out);

// Parses one file into `prog`. Without `recover`, stops at the first syntax
// error and returns false. With it, reports every syntax error it finds and
// keeps what it could read, skipping a broken statement or declaration: for
// editors, which need structure while code is half typed.
bool parse_file(program *prog, const source *src, token *toks, bool recover);

// Whether the attributes starting at toks[i] (a '[') belong to a field: after
// the last ']' come a type name and a field name. Otherwise they belong to a
// declaration. For recovery, which takes a '[' at column 1 as a declaration.
bool attributes_before_field(const token *toks, int i);
bool check(program *prog);
