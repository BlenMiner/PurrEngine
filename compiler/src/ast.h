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
    TY_LOCAL_ENTITY, // LocalEntity: an entity of the local world, which only local code can hold
    TY_PLAYER,     // PlayerID
    TY_COLOR,
    TY_STRING,     // Text: literals, and function parameters and locals that hold them
    TY_RECT,       // Rect: a GUI rectangle, x and y from the top left, width and height
    TY_BLOCK,      // Block: a function's last parameter, the code its caller writes in braces after the call
    TY_LIST,       // List<T>: decl is the list type, whose one field is its element
    TY_COMPONENT,
    TY_SINGLETON,
    TY_INPUT,      // The game's input declaration
    TY_RECORD,     // Built-in read-only data: Devices, Keyboard, Button, ...
    TY_STRUCT,     // A struct: plain data, copied like any value
    TY_EVENT,      // An event's value: what `Send` sends and a handler receives
    TY_ENUM,       // A value of an enum: one of its members, an int underneath
} type_kind;

typedef struct type {
    type_kind kind;
    decl *decl; // For components, singletons, inputs, records, structs and events.
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
    VEC(expr *) values;  // On input fields, constant values: [Clamp(-1, 1)]; on declarations, text: [NativeName("f")]
} attribute;

// ---------------------------------------------------------------------------
// Declarations

typedef struct field {
    str name;
    str type_name;
    loc at;      // The name
    type type;
    expr *default_value; // Constant expression, or NULL for zero.
    loc type_at; // The type's name (the last part if it's qualified)
    VEC(attribute) attributes; // [Clamp], [Min] and [Max] on input and struct fields
    loc type_qual_at; // Where the type starts: its namespace if it's qualified
    bool hidden; // The engine's own, in generated C only: a scene's visibility and players
    int leaf;    // In the device records: 1 + its index in program.device_leaves, if it's a value the devices send
} field;

typedef enum param_mode {
    PARAM_READ,
    PARAM_MUT,
    PARAM_WITH,
    PARAM_WITHOUT,
    PARAM_EVENT, // An event handler's trigger: `event(Hit hit)`, always its first parameter
    PARAM_IN,    // `in Stats stats`, an extern function's: C gets a read-only pointer to the value
} param_mode;

// One value the devices send, like keyboard.space or gamepad.leftStick.
typedef struct device_leaf {
    const char *path;          // Its C access from tide_devices: "keyboard.space"
    const struct field *field; // Its field in the device records
} device_leaf;

#define DEVICE_WORDS 4 // Bits for up to 256 device leaves

typedef struct param {
    param_mode mode;
    str type_name;
    str name; // Empty for with/without, and for a trigger without a name: `event(Spawned)`.
    loc at;   // The first token, a modifier or the type
    type type;
    loc type_at;      // The type's name (the last part if it's qualified)
    loc type_qual_at; // Where the type starts: its namespace if it's qualified
    loc name_at;
    bool read;    // The body names it
    bool written; // The body assigns through it
    bool function_param; // A method's or function's: a copy, or with mut, the caller's variable itself
} param;

// Why a system waits for one that runs before it in the tick (see parallel.c).
typedef enum conflict_kind {
    CONFLICT_BOTH_WRITE,     // Both write the data
    CONFLICT_EARLIER_READS,  // The earlier system reads what this one writes
    CONFLICT_EARLIER_WRITES, // The earlier system writes what this one reads
    CONFLICT_TEXT,           // Both write text or lists, which the match keeps in one heap
    CONFLICT_SPAWN,          // Both spawn: entities get their IDs in order
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
    DECL_STRUCT, // struct Stats { fields }: a value type for fields and locals
    DECL_METHOD, // bool IsDead() { ... } in a struct or component; in its `methods`, not program.decls
    DECL_FUNCTION, // void Heal(mut Stats stats, float amount) { ... }: code other code calls; `extern` ones are C's
    DECL_EVENT,    // event Hit { fields }: something that happened, sent with Send
    DECL_ENUM,     // enum Page { Title, Options }: a type with named values
    DECL_LIST,     // List<T>, one per element type: its one field is the element; in program.lists
} decl_kind;

// One of an enum's members: `Options`, or `Options = 3`.
typedef struct enum_member {
    str name;
    loc at;
    struct expr *value; // The value written after '=', or NULL for the one after the previous member's
    int64_t number;     // Its value, once checked
} enum_member;

typedef struct decl {
    decl_kind kind;
    str name;
    str qualified; // With its namespace, for messages: "Combat.Health"
    loc at;
    loc end; // The closing brace
    const unit *unit; // The file it's in; NULL for built-ins
    VEC(attribute) attributes;
    bool builtin;
    bool is_local;    // `local`: belongs to this machine, not the match. Views are always local code.
    loc local_at;     // The `local` keyword
    bool is_scene;    // `scene Arena { ... }`: a DECL_COMPONENT whose entity is a loaded scene
    bool is_extern;   // `extern float Noise(float x);`: a DECL_FUNCTION written in C, with no body
    const char *c_name; // Records: the C struct name. Extern functions: the C function, from [NativeName] or the name.
    bool device_group;  // Devices, Keyboard, Mouse, Gamepad and Dpad: records made of device values
    int leaves_first;   // ...which are program.device_leaves from this one
    int leaves_count;

    // Components, singletons, inputs, records, structs and events
    VEC(field) fields;
    int index; // Component bit / singleton index / event index / system order.

    // Events
    bool world_event;        // Built-in events the engine sends to the world, never to an entity
    const struct decl *needs_target; // A handler that takes data from the entity the event is sent to, or NULL
    VEC(struct decl *) handlers;     // Its handlers, in the order they run

    // Structs and components: their methods
    VEC(struct decl *) methods;

    // Enums
    VEC(enum_member) members;

    // Methods and functions
    struct decl *owner;       // A method's struct or component; NULL for a function
    bool is_mut_method;       // `mut void Damage(...)`: it may change the fields
    bool is_operator;         // `Money operator +(Money a, Money b)`: in a struct, with no value of its own
    bool is_interpolate;      // `Angle Interpolate(Angle from, Angle to, float t)`: how views blend its type, no value of its own
    bool uses_this;           // A component's method that reads `this`, itself or through its other methods: it's given the entity
    loc this_at;              // ...where it first does
    struct decl *interpolate; // Structs and components: their Interpolate, if they have one
    bool snapped;             // Singletons: something calls .Snap() on it, so it counts its snaps
    bool shown;               // Values with fields, and lists: text shows one somewhere, so it has a text helper
    tok_kind op;              // An operator's: T_PLUS, T_EQ, ...; T_MINUS is negation with one parameter
    str return_type_name;     // "void" if it returns nothing
    loc return_type_at;       // Its last part, if it's qualified
    loc return_type_qual_at;
    type return_type;

    // Systems, views, methods, functions, and an input's Sample
    VEC(param) params;
    stmt *body;
    loc body_at;
    stmt *sanitize; // An input's Sanitize() { ... }
    loc sanitize_at;
    bool is_view;        // A view: a DECL_SYSTEM that runs once per frame, reads the world and draws.
    bool is_handler;     // An event handler: a DECL_SYSTEM that runs when its event is sent, `event(Hit hit) Name(...)`.
    struct decl *event;  // A handler's event
    bool per_entity;     // Runs once per matching entity, not once per tick.
    bool entity_local;   // Its entities are the local world's (views of local components)
    bool draws;          // A function that draws or uses the GUI, itself or through the functions it calls
    loc draws_at;        // ...where it first does
    bool frame_devices;  // ...and whether that's reading this frame's Devices, not drawing
    bool takes_block;    // A function whose last parameter is a Block: inlined where it's called
    bool calls_c;        // Code that calls an extern function, itself or through others: its calls run in order
    bool writes_text;    // A system that writes text into its world: its heap, which one system changes at a time
    bool spawns;         // A system that spawns or loads scenes: entity IDs are handed out in order
    VEC(struct decl *) callees; // Functions it calls, once each
    VEC(loc) callee_at;         // ...and where it first calls each
    uint64_t device_uses[DEVICE_WORDS]; // Device values it reads through parameters, a bit per device leaf
    loc position_at;     // ...where it first reads the mouse's position that way
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
    E_INTERP,  // $"score {score}": `parts` around the values in `args`, each with its format
    E_INDEX,   // object[lhs]: a list's element
    E_LIST,    // [a, b, c]: a list of `args`, whose type comes from where it goes
    E_THIS,    // this: the entity the code runs for
    E_DEFAULT, // `default`: the default value of the type where it goes
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
    CALL_METHOD,    // stats.IsDead(), or IsDead() inside another of Stats' methods
    CALL_FUNCTION,  // Heal(unit.stats, 5), or Combat.Heal(...)
    CALL_SEND,      // Send(RoundOver { ... }), or target.Send(Hit { ... }): type_decl is the event
    CALL_LOAD,      // Scene.Load(Arena { ... }): a spawn whose entity is its own scene; type_decl is the scene
    CALL_UNLOAD,    // Scene.Unload(scene)
    CALL_SCENE_PLAYER, // Scene.AddPlayer(scene, player) and Scene.RemovePlayer(scene, player)
    CALL_SESSION,   // Session.Start, Open, Close, Join, Connect and Leave: `name` says which; type_decl is Start's scene
    CALL_SNAP,      // entity.Snap() or singleton.Snap(): views draw it as it is this tick; type_decl is a singleton's
    CALL_GUI,       // GUI.Button(...), GUILayout.Horizontal() { ... }: calls c_callee with the GUI first
    CALL_BLOCK,     // content(): runs the Block its function was given
    CALL_TEXT,      // name.Contains(...), text's methods: calls c_callee with the text first
    CALL_LIST,      // items.Add(...), a list's methods: `name` says which
} builtin_call;

// What a GUI call needs besides its arguments (expr.gui).
enum {
    GUI_ID = 1,        // A widget ID, from where it's called
    GUI_CONTAINER = 2, // A block after the call: GUILayout.Vertical() { ... }
    GUI_SKIPS = 4,     // A container whose block doesn't always run: it returns -1 then
};

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
    CTOR_RECT,           // Rect(x, y, width, height)
} ctor_form;

typedef enum binding_kind {
    BIND_NONE,
    BIND_PARAM,
    BIND_LOCAL,
    BIND_TYPE,  // A component or event name used as a value: Spawn(Player), Send(RoundOver).
    BIND_FIELD, // A field of the input, named directly inside its Sample or Sanitize.
    BIND_NAMESPACE, // `Combat` in Combat.Health
    BIND_DEVICES,   // `Devices`: this machine's devices, in views and the input's Sample
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
    decl *type_decl;    // BIND_TYPE, E_LITERAL's type, and CALL_SEND's event

    // E_MEMBER, E_METHOD
    expr *object;
    str member;
    field *field;          // Field of a component, singleton, input or record; BIND_FIELD's field.
    input_edge edge;       // .down / .up on an input field.
    int swizzle_len;       // Vector swizzle: number of components (0 if not a swizzle).
    int swizzle[4];        // Component indices: 0..3 for x, y, z, w.
    const char *c_constant; // Static member such as quaternion.identity, as C.
    const enum_member *enum_member; // Page.Title: the member (type_decl is its enum)

    // E_CALL, E_METHOD
    VEC(expr *) args;
    builtin_call call;
    ctor_form ctor;         // CALL_CONSTRUCT
    const char *c_callee;   // CALL_BUILTIN
    VEC(type) arg_want;     // CALL_BUILTIN, CALL_METHOD and CALL_FUNCTION: the type each argument converts to.
    struct decl *method;    // CALL_METHOD and CALL_FUNCTION: the method or function called
    uint64_t spawn_mask;  // CALL_SPAWN: components the new entity has.
    int spawn_archetype;  // CALL_SPAWN: index into the archetype list.
    bool local_world;     // CALL_SPAWN, CALL_ADD, CALL_REMOVE, CALL_DESTROY and CALL_SEND: in the local world
    const char *hoisted;  // Spawns, and calls, units and operators that keep their order: the temporary codegen ran it into, before the statement
    unsigned arg_mut;     // CALL_GUI: a bit per argument the call changes, which it takes by address
    int gui;              // CALL_GUI: GUI_ID and GUI_CONTAINER
    struct stmt *block;   // A call's block, written in braces after it: Foldout("Audio") { ... }

    // E_BINARY, E_UNARY; E_CONDITIONAL's two sides. `method` is the struct's
    // operator, if one is used.
    tok_kind op;
    expr *lhs;
    expr *rhs;
    expr *cond; // E_CONDITIONAL

    // E_INTERP: the text before, between and after the values (escapes as written),
    // and each value's format as written ("F2") and as tide/text.h takes it
    VEC(str) parts;
    VEC(str) formats;
    VEC(int32_t) format_codes;

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
    S_SWITCH,
    S_BREAK,
    S_WHILE,    // while (cond) then_stmt
    S_FOR,      // for (init; cond; step) then_stmt: each part optional
    S_CONTINUE,
    S_FOREACH,  // foreach (var name in value) then_stmt: `type` is the element's, and it's the variable's declaration
} stmt_kind;

// A switch's section: its labels, then the statements they run.
typedef struct switch_case {
    VEC(struct expr *) labels; // `case` values; NULL for `default`
    VEC(loc) label_at;         // Each label's `case` or `default`
    VEC(struct stmt *) body;
} switch_case;

struct stmt {
    stmt_kind kind;
    loc at;

    // S_BLOCK
    VEC(stmt *) stmts;
    loc end; // The closing brace, or where a block cut short by a syntax error ends

    // S_IF, and S_SWITCH's value; S_WHILE and S_FOR, with then_stmt their body
    expr *cond;
    stmt *then_stmt;
    stmt *else_stmt;

    // S_FOR
    stmt *init; // A local, an assignment or a call, or NULL
    stmt *step; // An assignment, i++ or a call, or NULL

    // S_SWITCH
    VEC(switch_case) cases;

    // S_VAR
    bool is_mut;
    bool loop_var; // Declared by a for: only its step changes it, unless it's mut
    str type_name; // Empty for `var`. With its namespace if it's qualified.
    str name;
    type type;
    loc type_at;      // The type's name (the last part if it's qualified)
    loc name_at;
    loc type_qual_at; // Where the type starts: its namespace if it's qualified

    // S_VAR initializer, S_ASSIGN value, S_EXPR expression, S_RETURN value (or NULL)
    expr *value;

    // S_ASSIGN
    expr *target;
    tok_kind op; // T_ASSIGN, a compound one like T_PLUS_ASSIGN, or T_PLUS_PLUS and T_MINUS_MINUS (value 1)
    struct decl *operator_decl; // A compound assignment's struct operator: `money += tip` uses `+`
};

// ---------------------------------------------------------------------------
// Program

typedef struct program {
    VEC(unit *) units; // Its files, in the order they're parsed: sorted by path
    VEC(decl *) decls; // In source order, file by file, builtins first.

    // Filled by the checker
    VEC(decl *) components;
    VEC(decl *) singletons;
    VEC(decl *) systems;
    VEC(decl *) views;
    VEC(decl *) handlers; // Every event handler, in the order they run
    VEC(decl *) events;   // Built-in ones first
    decl *spawned;        // Built-in events
    decl *destroyed;
    decl *player_joined;
    decl *player_left;
    decl *main;          // The scene named Main, where the program starts
    int main_archetype;  // Main's archetype
    decl *input;         // The input declaration, if any.
    decl *scene_visibility; // The built-in enum SceneVisibility
    decl *anchor;        // The built-in enum Anchor, where GUILayout.Area goes
    decl *session;       // The built-in local singleton Session: this machine's part in a match
    decl *connected;     // Built-in local events: this machine joined a match, and left it
    decl *disconnected;
    VEC(decl *) start_scenes; // Scenes Session.Start starts matches in
    bool uses_text;      // Some code makes text, in the scratch area the run functions clear
    bool uses_heap;      // Some field holds text or a list: the worlds have a heap
    VEC(decl *) lists;   // Every List<T> type the program uses, one per element type
    decl *owner;         // The built-in Owner component.
    decl *devices;       // The built-in Devices record.
    VEC(decl *) records; // Built-in records: Devices, Keyboard, Mouse, Gamepad, Dpad, Button.
    VEC(device_leaf) device_leaves; // Each value the devices send: a button, stick, trigger, axis or bool
    int position_leaf;   // The mouse's position, which the match can't read
    bool match_devices;  // Systems or handlers take Devices: the input sends the devices too
    uint64_t device_uses[DEVICE_WORDS]; // The leaves match code reads: all the input sends of them
    VEC(decl *) structs; // In an order where each comes after the structs it contains.

    VEC(uint64_t) archetypes;  // Component masks, in derivation order.
    VEC(bool) archetype_local; // Per archetype: in the local world rather than the match
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
    FIX_MUT_LOCAL,  // Writing a read-only local: declare it mut
    FIX_MUT_METHOD, // A read-only method that changes a field or calls a mut method: declare it mut
    FIX_CREATE_FUNCTION,  // A call of an unknown function: declare it, taking the arguments' types
    FIX_CREATE_STRUCT,    // An unknown type where a struct fits: declare one
    FIX_CREATE_COMPONENT, // An unknown type where a component fits: declare one
    FIX_USE_THIS,   // An Entity or LocalEntity parameter: remove it, and name the entity `this`
} fix_kind;

typedef struct fix {
    fix_kind kind;
    loc at; // Where the diagnostic points
    const param *param;
    const stmt *local;   // FIX_MUT_LOCAL
    const decl *method;  // FIX_MUT_METHOD
    const expr *call;    // FIX_CREATE_FUNCTION
    str name;            // FIX_CREATE_STRUCT and FIX_CREATE_COMPONENT
} fix;

program *program_new(void);

// Works out which systems in the tick can run at the same time, and why the
// others wait (fills decl.waits, alongside and stage). Two systems conflict
// when one writes a component or singleton the other reads or writes, unless
// their entities can never be the same (disjoint archetypes). Conflicting
// systems keep their order in the tick, and [Before]/[After] order them too.
// Needs the archetypes.
void analyze_parallelism(program *prog);

// Whether a system's entities are split across threads, a task per chunk of
// them: it runs per entity, and what it changes is its entities' own, in no
// order across them. One system at a time changes the heap, and a singleton is
// everyone's. Its spawns get temporary handles until it's done (tide/jobs.h).
// C is trusted: what it does on several threads at once is the game's to get
// right.
bool system_splits(const decl *sys);

// Why `sys` waits for `w->on`, like "both write Transform". `quote` wraps names
// ("`" for Markdown).
void describe_wait(const decl *sys, const system_wait *w, const char *quote, sb *out);

// A declaration's name as code in `from`'s namespace writes it: short in the
// same namespace, qualified elsewhere, and always qualified without `from`.
void put_decl_name(sb *out, const decl *d, const char *quote, const decl *from);

// The whole tick's schedule as text, for `tidec --schedule`.
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
