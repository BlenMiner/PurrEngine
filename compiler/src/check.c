#include <stdio.h>
#include <string.h>

#include "ast.h"
#include "builtins.h"
#include "purr/devices.h"
#include "types.h"

// Semantic analysis: resolves names and types, enforces access and mutability
// rules, and derives the archetype list from spawn, add and remove sites.

#define MAX_COMPONENTS 64
#define MAX_ARCHETYPES 256

typedef struct checker {
    program *prog;
    const unit *unit;         // The file being checked: its namespace and `using`s decide what names mean.
    decl *system;             // Whose parameters are in scope: a system or view, a method, or the input when checking its Sample.
    decl *method;             // The method or function being checked: a method's type's fields are in scope.
    bool in_input;      // Checking the input's constructor, which runs outside the simulation.
    bool in_sanitize;         // Checking the input's Sanitize; in_input is set too, for the fields.
    int short_circuit_depth;  // Inside the right side of && or ||, which may not run.
    int branch_depth;         // Inside a side of ?:, which may not run.
    VEC(stmt *) locals;       // S_VAR statements currently in scope.
    VEC(int) scope_marks;
    VEC(expr *) spawns;       // Spawn calls, patched with archetype indices at the end.
} checker;

static const type T_ERR = {TY_ERROR, NULL};
static const type T_VOID_ = {TY_VOID, NULL};
static const type T_BOOL_ = {TY_BOOL, NULL};
static const type T_INT_ = {TY_INT, NULL};
static const type T_FLOAT_ = {TY_FLOAT, NULL};
static const type T_ENTITY_ = {TY_ENTITY, NULL};

// The type a declaration names: a component, singleton, input, record or struct.
static type decl_type(decl *d)
{
    switch (d->kind) {
    case DECL_COMPONENT: return (type){TY_COMPONENT, d};
    case DECL_SINGLETON: return (type){TY_SINGLETON, d};
    case DECL_INPUT: return (type){TY_INPUT, d};
    case DECL_RECORD: return (type){TY_RECORD, d};
    case DECL_STRUCT: return (type){TY_STRUCT, d};
    default: return T_ERR;
    }
}

// Does this type have fields (components, singletons, inputs, records, structs)?
static bool has_fields(const type t)
{
    return t.kind == TY_COMPONENT || t.kind == TY_SINGLETON || t.kind == TY_INPUT || t.kind == TY_RECORD
        || t.kind == TY_STRUCT;
}

// int or float, not a vector.
static bool is_scalar_number(const type t)
{
    return t.kind == TY_INT || t.kind == TY_FLOAT;
}

// ---------------------------------------------------------------------------
// Names and namespaces

static const str global_ns = {"", 0};

static str decl_ns(const decl *d)
{
    return d->unit ? d->unit->ns : global_ns;
}

// Splits "Game.Combat.Health" into "Game.Combat" and "Health". False if there's no dot.
static bool split_qualified(const str text, str *ns, str *name)
{
    int dot = text.len - 1;
    while (dot >= 0 && text.ptr[dot] != '.') dot--;
    if (dot < 0) return false;
    *ns = (str){text.ptr, dot};
    *name = (str){text.ptr + dot + 1, text.len - dot - 1};
    return true;
}

// Does some file declare this namespace, or one inside it?
static bool is_namespace(const program *prog, const str ns)
{
    for (int i = 0; i < prog->units.count; i++) {
        const str u = prog->units.items[i]->ns;
        if (str_eq(u, ns) || (u.len > ns.len && str_starts_with_c(u, str_to_cstr(ns)) && u.ptr[ns.len] == '.')) return true;
    }
    return false;
}

// What a name can refer to where it's written.
typedef enum name_kind {
    NAME_TYPE,     // Components, singletons, inputs and structs
    NAME_SYSTEM,   // Systems and views, in [Before] and [After]
    NAME_FUNCTION,
} name_kind;

static bool is_kind(const decl *d, const name_kind kind)
{
    if (kind == NAME_SYSTEM) return d->kind == DECL_SYSTEM;
    if (kind == NAME_FUNCTION) return d->kind == DECL_FUNCTION;
    return d->kind != DECL_SYSTEM && d->kind != DECL_FUNCTION;
}

// A declaration of that kind named exactly `name` in `ns`.
static decl *find_in(const program *prog, const str ns, const str name, const name_kind kind)
{
    for (int i = 0; i < prog->decls.count; i++) {
        decl *d = prog->decls.items[i];
        if (is_kind(d, kind) && str_eq(d->name, name) && str_eq(decl_ns(d), ns)) return d;
    }
    return NULL;
}

// What a name means as written in `from`. A qualified name (Combat.Health) is
// exact. A plain one is looked up in the file's namespace, then its parents,
// then the `using` namespaces, then the global namespace. If two `using`
// namespaces both have it, *other gets the second: the name is ambiguous.
static decl *lookup(const program *prog, const unit *from, const str text, const name_kind kind, decl **other)
{
    *other = NULL;
    str ns;
    str name;
    if (split_qualified(text, &ns, &name)) return find_in(prog, ns, name, kind);

    for (ns = from ? from->ns : global_ns; ns.len > 0;) {
        decl *d = find_in(prog, ns, text, kind);
        if (d) return d;
        int dot = ns.len - 1;
        while (dot >= 0 && ns.ptr[dot] != '.') dot--;
        ns.len = dot < 0 ? 0 : dot;
    }
    decl *found = NULL;
    for (int i = 0; from && i < from->usings.count; i++) {
        decl *d = find_in(prog, from->usings.items[i], text, kind);
        if (d && !found) found = d;
        else if (d && d != found && !*other) *other = d;
    }
    return found ? found : find_in(prog, global_ns, text, kind);
}

static decl *find_named(const checker *c, const str name, const loc at, const name_kind kind)
{
    decl *other;
    decl *d = lookup(c->prog, c->unit, name, kind, &other);
    if (other) {
        diag_error(at, "'" STR_FMT "' is ambiguous: both " STR_FMT " and " STR_FMT " have it", STR_ARG(name),
                   STR_ARG(decl_ns(d)), STR_ARG(decl_ns(other)));
        diag_note("write which one you mean, like '" STR_FMT "'", STR_ARG(d->qualified));
    }
    return d;
}

static decl *find_type(const checker *c, const str name, const loc at)
{
    return find_named(c, name, at, NAME_TYPE);
}

// `a.b.c` as the text "a.b.c", if the expression is only names and members.
static bool qualified_text(const expr *e, str *out)
{
    if (e->kind == E_NAME) {
        *out = e->name;
        return true;
    }
    if (e->kind != E_MEMBER) return false;
    str left;
    if (!qualified_text(e->object, &left)) return false;
    char *text = arena_alloc((size_t)left.len + (size_t)e->member.len + 2);
    memcpy(text, left.ptr, (size_t)left.len);
    text[left.len] = '.';
    memcpy(text + left.len + 1, e->member.ptr, (size_t)e->member.len);
    *out = (str){text, left.len + 1 + e->member.len};
    return true;
}

// The innermost name of a.b.c.
static const expr *chain_root(const expr *e)
{
    while (e->kind == E_MEMBER) e = e->object;
    return e;
}

// Marks the namespace parts of Game.Combat.Health, for editors.
static void mark_namespaces(expr *e)
{
    for (; e && (e->kind == E_MEMBER || e->kind == E_NAME); e = e->kind == E_MEMBER ? e->object : NULL) {
        e->bind = BIND_NAMESPACE;
    }
}

// "did you mean" candidates: declared types of the kinds asked for.
static void suggest_decls(suggestion *s, const program *prog, const bool components, const bool singletons,
                          const bool inputs)
{
    for (int i = 0; i < prog->decls.count; i++) {
        const decl *d = prog->decls.items[i];
        if ((components && d->kind == DECL_COMPONENT) || (singletons && d->kind == DECL_SINGLETON)
            || (inputs && d->kind == DECL_INPUT)) {
            suggest_consider(s, d->name);
        }
    }
}

static void suggest_structs(suggestion *s, const program *prog)
{
    for (int i = 0; i < prog->decls.count; i++) {
        if (prog->decls.items[i]->kind == DECL_STRUCT) suggest_consider(s, prog->decls.items[i]->name);
    }
}

static void suggest_fields(suggestion *s, const decl *d)
{
    for (int i = 0; i < d->fields.count; i++) suggest_consider(s, d->fields.items[i].name);
}

static bool check_reserved(const str name, const loc at)
{
    if (str_starts_with_c(name, "purr_")) {
        diag_error(at, "names starting with 'purr_' are reserved for generated code");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Scopes

static void push_scope(checker *c)
{
    vec_push(c->scope_marks, c->locals.count);
}

static void pop_scope(checker *c)
{
    c->locals.count = c->scope_marks.items[--c->scope_marks.count];
}

static stmt *find_local(const checker *c, const str name)
{
    for (int i = c->locals.count - 1; i >= 0; i--) {
        if (str_eq(c->locals.items[i]->name, name)) return c->locals.items[i];
    }
    return NULL;
}

static bool field_named(const decl *d, const str name)
{
    for (int i = 0; d && i < d->fields.count; i++) {
        if (str_eq(d->fields.items[i].name, name)) return true;
    }
    return false;
}

// The fields named directly by name: the input's in its Sample and Sanitize,
// and a type's in its methods.
static field *field_in_scope(const checker *c, const str name)
{
    decl *const d = c->method ? c->method->owner : c->in_input ? c->system : NULL;
    for (int i = 0; d && i < d->fields.count; i++) {
        if (str_eq(d->fields.items[i].name, name)) return &d->fields.items[i];
    }
    return NULL;
}

static decl *find_method(const decl *d, const str name)
{
    for (int i = 0; d && i < d->methods.count; i++) {
        if (str_eq(d->methods.items[i]->name, name)) return d->methods.items[i];
    }
    return NULL;
}

static param *find_param(const checker *c, const str name)
{
    if (!c->system) return NULL; // Field defaults are checked outside any system.
    if (c->in_sanitize) return NULL; // Sample's parameters aren't Sanitize's
    for (int i = 0; i < c->system->params.count; i++) {
        param *p = &c->system->params.items[i];
        if (p->name.len > 0 && str_eq(p->name, name)) return p;
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// Expressions

static type check_expr(checker *c, expr *e);
static bool check_writable(checker *c, expr *target, const decl *called, const param *arg_of);

static uint64_t bit(const decl *component)
{
    return (uint64_t)1 << component->index;
}

// Checks `Transform { position = ... }` and `Stats { armor = 2 }`.
static type check_literal(checker *c, expr *e)
{
    decl *d = find_type(c, e->name, e->at);
    if (!d || (d->kind != DECL_COMPONENT && d->kind != DECL_STRUCT)) {
        if (d) {
            diag_error(e->at, "'" STR_FMT "' isn't a component or struct; only those have values like this",
                       STR_ARG(e->name));
        } else {
            diag_error(e->at, "unknown component or struct '" STR_FMT "'", STR_ARG(e->name));
            suggestion s = suggest_start(e->name);
            suggest_decls(&s, c->prog, true, false, false);
            suggest_structs(&s, c->prog);
            suggest_note(&s);
        }
        for (int i = 0; i < e->inits.count; i++) check_expr(c, e->inits.items[i].value);
        return T_ERR;
    }
    e->type_decl = d;

    for (int i = 0; i < e->inits.count; i++) {
        field_init *init = &e->inits.items[i];
        const type value = check_expr(c, init->value);
        for (int j = 0; j < d->fields.count; j++) {
            if (str_eq(d->fields.items[j].name, init->name)) init->field = &d->fields.items[j];
        }
        if (!init->field) {
            diag_error(init->at, "%s '" STR_FMT "' has no field '" STR_FMT "'",
                       d->kind == DECL_STRUCT ? "struct" : "component", STR_ARG(d->name), STR_ARG(init->name));
            suggestion s = suggest_start(init->name);
            suggest_fields(&s, d);
            suggest_note(&s);
            continue;
        }
        for (int j = 0; j < i; j++) {
            if (str_eq(e->inits.items[j].name, init->name)) {
                diag_error(init->at, "field '" STR_FMT "' is set twice", STR_ARG(init->name));
            }
        }
        if (!type_assignable(init->field->type, value)) {
            diag_error(init->value->at, "field '" STR_FMT "' is %s, not %s", STR_ARG(init->name),
                       type_name(init->field->type), type_name(value));
        }
    }
    return decl_type(d);
}

// A component argument to Spawn or Add: `Player` (defaults) or `Player { ... }`.
// Returns the component, or NULL after reporting an error.
static decl *check_component_arg(checker *c, expr *arg, const char *fn)
{
    if (arg->kind == E_LITERAL) {
        const type t = check_literal(c, arg);
        if (t.kind == TY_STRUCT) {
            diag_error(arg->at, "%s takes components, and '" STR_FMT "' is a struct", fn, STR_ARG(t.decl->name));
            diag_note("put it in a component, like 'component Name { " STR_FMT " value; }'", STR_ARG(t.decl->name));
        }
        return t.kind == TY_COMPONENT ? t.decl : NULL;
    }
    // Player, or Combat.Health
    str name;
    const expr *root = arg->kind == E_NAME || arg->kind == E_MEMBER ? chain_root(arg) : NULL;
    if (root && root->kind == E_NAME && !find_local(c, root->name) && !find_param(c, root->name)
        && qualified_text(arg, &name)) {
        decl *d = find_type(c, name, arg->at);
        if (d && d->kind == DECL_COMPONENT) {
            if (arg->kind == E_MEMBER) mark_namespaces(arg->object);
            arg->bind = BIND_TYPE;
            arg->type_decl = d;
            arg->type = (type){TY_COMPONENT, d};
            return d;
        }
        if (d && d->kind == DECL_STRUCT) {
            if (arg->kind == E_MEMBER) mark_namespaces(arg->object);
            arg->bind = BIND_TYPE;
            arg->type_decl = d;
            diag_error(arg->at, "%s takes components, and '" STR_FMT "' is a struct", fn, STR_ARG(d->name));
            diag_note("put it in a component, like 'component Name { " STR_FMT " value; }'", STR_ARG(d->name));
            return NULL;
        }
    }
    const type t = check_expr(c, arg);
    if (t.kind != TY_ERROR) {
        diag_error(arg->at, "%s takes components, like 'Player' or 'Player { speed = ... }'", fn);
    }
    return NULL;
}

// Component list shared by Spawn and Add. Returns the mask of components.
static uint64_t check_component_list(checker *c, const expr *call, const char *fn)
{
    uint64_t mask = 0;
    for (int i = 0; i < call->args.count; i++) {
        decl *d = check_component_arg(c, call->args.items[i], fn);
        if (!d) continue;
        if (mask & bit(d)) {
            diag_error(call->args.items[i]->at, "'" STR_FMT "' appears twice; an entity has at most one of each component",
                       STR_ARG(d->name));
        }
        mask |= bit(d);
    }
    return mask;
}

// Constructors of built-in types: float(x), int3(1), float4(v.xy, 0, 1),
// quaternion(x, y, z, w), float3x3(c0, c1, c2) and friends.
static type check_construct(checker *c, expr *e, const type target)
{
    bool any_error = false;
    for (int i = 0; i < e->args.count; i++) {
        if (check_expr(c, e->args.items[i]).kind == TY_ERROR) any_error = true;
    }
    if (any_error) return T_ERR;

    e->call = CALL_CONSTRUCT;
    const int argc = e->args.count;
    const char *name = type_name(target);
    const type first = argc > 0 ? e->args.items[0]->type : T_VOID_;

    // PlayerID(0): a player by index, for local play and tests.
    if (target.kind == TY_PLAYER) {
        if (argc == 1 && first.kind == TY_INT) {
            e->ctor = CTOR_PLAYER;
            return target;
        }
        diag_error(e->at, "PlayerID(...) takes a player index, an int");
        return T_ERR;
    }

    // Color(r, g, b) with alpha 1, or Color(r, g, b, a).
    if (target.kind == TY_COLOR) {
        bool scalars = argc == 3 || argc == 4;
        for (int i = 0; i < argc; i++) {
            if (!is_scalar_number(e->args.items[i]->type)) scalars = false;
        }
        if (scalars) {
            e->ctor = CTOR_COLOR;
            return target;
        }
        diag_error(e->at, "Color takes (r, g, b) or (r, g, b, a), numbers from 0 to 1");
        return T_ERR;
    }

    // float(x), int(x): conversions between the two number types.
    if (is_scalar_number(target)) {
        if (argc == 1 && is_scalar_number(first)) {
            e->ctor = CTOR_SCALAR;
            return target;
        }
        diag_error(e->at, "%s(...) converts one number", name);
        return T_ERR;
    }

    const int dim = type_dim(target);
    if (dim >= 2) {
        if (argc == 1 && is_scalar_number(first)) {
            e->ctor = CTOR_SPLAT;
            return target;
        }
        if (argc == 1 && type_dim(first) == dim && first.kind != target.kind) {
            e->ctor = CTOR_CONVERT;
            return target;
        }
        int total = 0;
        for (int i = 0; i < argc; i++) {
            const type t = e->args.items[i]->type;
            if (!type_is_numeric(t)) {
                diag_error(e->args.items[i]->at, "%s takes numbers and vectors, not %s", name, type_name(t));
                return T_ERR;
            }
            if (type_is_int_based(target) && type_is_float_based(t)) {
                diag_error(e->args.items[i]->at, "%s takes ints, not %s", name, type_name(t));
                diag_note("convert explicitly, for example int(x) or %s(v)", name);
                return T_ERR;
            }
            total += type_dim(t);
        }
        if (total != dim) {
            diag_error(e->at, "%s needs %d components, got %d", name, dim, total);
            return T_ERR;
        }
        e->ctor = CTOR_COMPONENTS;
        return target;
    }

    if (target.kind == TY_QUATERNION) {
        bool scalars = argc == 4;
        for (int i = 0; i < argc; i++) {
            if (!is_scalar_number(e->args.items[i]->type)) scalars = false;
        }
        if (scalars) {
            e->ctor = CTOR_COMPONENTS;
            return target;
        }
        if (argc == 1 && first.kind == TY_FLOAT4) {
            e->ctor = CTOR_QUAT_FROM_F4;
            return target;
        }
        if (argc == 1 && first.kind == TY_FLOAT3X3) {
            e->ctor = CTOR_QUAT_FROM_MAT;
            return target;
        }
        diag_error(e->at, "quaternion takes (x, y, z, w), a float4 or a float3x3");
        return T_ERR;
    }

    const int n = matrix_dim(target);
    if (n > 0) {
        const type column = vector_type(true, n);
        bool columns = argc == n;
        bool scalars = argc == n * n;
        for (int i = 0; i < argc; i++) {
            const type t = e->args.items[i]->type;
            if (!type_assignable(column, t)) columns = false;
            if (!is_scalar_number(t)) scalars = false;
        }
        if (columns) {
            e->ctor = CTOR_MAT_COLUMNS;
            return target;
        }
        if (scalars) {
            e->ctor = CTOR_MAT_SCALARS;
            return target;
        }
        if (n == 3 && argc == 1 && first.kind == TY_QUATERNION) {
            e->ctor = CTOR_MAT_FROM_QUAT;
            return target;
        }
        if (n == 4 && argc == 2 && first.kind == TY_FLOAT3X3 && type_assignable((type){TY_FLOAT3, NULL}, e->args.items[1]->type)) {
            e->ctor = CTOR_MAT_FROM_ROT_T;
            return target;
        }
        diag_error(e->at, "%s takes %d float%d columns or %d numbers row by row", name, n, n, n * n);
        if (n == 3) diag_note("float3x3(quaternion) builds a rotation matrix");
        if (n == 4) diag_note("float4x4(float3x3 rotation, float3 translation) builds a transform");
        return T_ERR;
    }

    diag_error(e->at, "%s has no constructor", name);
    return T_ERR;
}

// The input code being checked, for messages.
static const char *input_code(const checker *c)
{
    return c->in_sanitize ? "Sanitize" : "Sample";
}

static bool in_view(const checker *c)
{
    return c->system && c->system->is_view;
}

// The arguments of a call of method or function `m`, against its parameters.
// A mut parameter takes the caller's variable itself, which it changes.
static void check_method_args(checker *c, expr *e, decl *m)
{
    e->call = m->kind == DECL_FUNCTION ? CALL_FUNCTION : CALL_METHOD;
    e->method = m;
    for (int i = 0; i < e->args.count; i++) check_expr(c, e->args.items[i]);
    for (int i = 0; i < m->params.count; i++) vec_push(e->arg_want, m->params.items[i].type);
    if (e->args.count != m->params.count) {
        diag_error(e->at, "'" STR_FMT "' takes %d argument%s, not %d", STR_ARG(m->name), m->params.count,
                   m->params.count == 1 ? "" : "s", e->args.count);
        return;
    }
    for (int i = 0; i < e->args.count; i++) {
        const param *p = &m->params.items[i];
        expr *arg = e->args.items[i];
        const bool exact = p->type.kind == arg->type.kind && p->type.decl == arg->type.decl;
        if (p->mode == PARAM_MUT && arg->type.kind != TY_ERROR && p->type.kind != TY_ERROR && !exact) {
            diag_error(arg->at, "'" STR_FMT "' changes its '" STR_FMT "', which must be %s, not %s", STR_ARG(m->name),
                       STR_ARG(p->name), type_name(p->type), type_name(arg->type));
        } else if (!type_assignable(p->type, arg->type)) {
            diag_error(arg->at, "'" STR_FMT "' takes %s for '" STR_FMT "', not %s", STR_ARG(m->name), type_name(p->type),
                       STR_ARG(p->name), type_name(arg->type));
        } else if (p->mode == PARAM_MUT) {
            check_writable(c, arg, m, p);
        }
    }
}

// Heal(unit.stats, 5), or Combat.Heal(...) from elsewhere.
static type check_function_call(checker *c, expr *e, decl *fn)
{
    check_method_args(c, e, fn);
    return fn->return_type;
}

// "methods" or "functions", for messages about the code being checked.
static const char *routines(const checker *c)
{
    return c->method->kind == DECL_FUNCTION ? "functions" : "methods";
}

// IsDead() inside another of the type's methods: the same value.
static type check_self_call(checker *c, expr *e, decl *m)
{
    if (m->is_mut_method && !c->method->is_mut_method) {
        diag_error(e->at, "'" STR_FMT "' changes the fields, so only a mut method can call it", STR_ARG(m->name));
        diag_note("declare '" STR_FMT "' as 'mut' too", STR_ARG(c->method->name));
    }
    check_method_args(c, e, m);
    return m->return_type;
}

static type check_call(checker *c, expr *e)
{
    decl *const own = c->method ? find_method(c->method->owner, e->name) : NULL;
    if (own) return check_self_call(c, e, own);

    type builtin;
    if (builtin_type_named(e->name, &builtin)) return check_construct(c, e, builtin);

    if (str_eq_c(e->name, "Spawn")) {
        if (c->method) {
            diag_error(e->at, "%s can't spawn entities; systems do", routines(c));
            return T_ERR;
        }
        if (c->in_input) {
            diag_error(e->at, "%s runs outside the simulation, so it can't spawn entities", input_code(c));
            return T_ERR;
        }
        if (in_view(c)) {
            diag_error(e->at, "views only read the world, so they can't spawn entities");
            return T_ERR;
        }
        // Expressions evaluate left to right, so codegen runs a statement's spawns
        // first, in order. On the right of && or ||, or in a side of ?:, that would
        // spawn even when that part is skipped.
        if (c->branch_depth > 0) {
            diag_error(e->at, "Spawn can't be inside '?:'");
            diag_note("only one side runs; spawn in an if/else instead");
        } else if (c->short_circuit_depth > 0) {
            diag_error(e->at, "Spawn can't be on the right side of && or ||");
            diag_note("that side only runs sometimes; spawn into a local before the condition");
        }
        e->call = CALL_SPAWN;
        e->spawn_mask = check_component_list(c, e, "Spawn");
        c->prog->spawned_mask |= e->spawn_mask;
        vec_push(c->spawns, e);
        return T_ENTITY_;
    }

    decl *const fn = find_named(c, e->name, e->at, NAME_FUNCTION);
    if (fn) return check_function_call(c, e, fn);

    for (int i = 0; i < e->args.count; i++) check_expr(c, e->args.items[i]);

    const decl *d = find_type(c, e->name, e->at);
    if (d && (d->kind == DECL_COMPONENT || d->kind == DECL_STRUCT)) {
        diag_error(e->at, "write '" STR_FMT " { ... }' to make a %s value", STR_ARG(e->name),
                   d->kind == DECL_STRUCT ? "struct" : "component");
        return T_ERR;
    }
    diag_error(e->at, "unknown function '" STR_FMT "'", STR_ARG(e->name));
    const char *owner = builtin_function_owner(e->name);
    if (owner) {
        diag_note("it's '%s." STR_FMT "'", owner, STR_ARG(e->name)); // Sin(x) for Math.Sin(x)
    } else {
        suggestion s = suggest_start(e->name);
        suggest_consider_c(&s, "Spawn");
        suggest_builtin_types(&s);
        suggest_note(&s);
    }
    return T_ERR;
}

// Does `e` name a built-in owner like Math or quaternion, rather than a variable?
static bool names_builtin_owner(const checker *c, const expr *e)
{
    return e->kind == E_NAME && builtin_owner(e->name) && !find_local(c, e->name) && !find_param(c, e->name);
}

static type check_method(checker *c, expr *e)
{
    // Math.Dot(a, b), quaternion.AxisAngle(axis, angle), Draw.Circle(center, radius, color)
    if (names_builtin_owner(c, e->object)) {
        for (int i = 0; i < e->args.count; i++) check_expr(c, e->args.items[i]);
        const type result = resolve_builtin_call(e->object->name, e);
        if (str_eq_c(e->object->name, "Draw")) {
            if (result.kind != TY_ERROR) e->call = CALL_DRAW;
            if (!in_view(c)) {
                diag_error(e->at, "Draw can only be used in views for now");
                diag_note("views run once per frame and only read the world: 'view Name(...) { ... }'");
                return T_ERR;
            }
        }
        return result;
    }

    // Combat.Heal(...): a function in a namespace.
    const expr *root = chain_root(e->object);
    str ns;
    if (root->kind == E_NAME && !find_local(c, root->name) && !find_param(c, root->name)
        && !field_in_scope(c, root->name) && qualified_text(e->object, &ns) && is_namespace(c->prog, ns)) {
        char *text = arena_alloc((size_t)ns.len + (size_t)e->name.len + 2);
        memcpy(text, ns.ptr, (size_t)ns.len);
        text[ns.len] = '.';
        memcpy(text + ns.len + 1, e->name.ptr, (size_t)e->name.len);
        decl *const fn = find_named(c, (str){text, ns.len + 1 + e->name.len}, e->at, NAME_FUNCTION);
        if (fn) {
            mark_namespaces(e->object);
            return check_function_call(c, e, fn);
        }
    }

    const type obj = check_expr(c, e->object);
    if (obj.kind == TY_ERROR) {
        for (int i = 0; i < e->args.count; i++) check_expr(c, e->args.items[i]);
        return T_ERR;
    }
    // stats.IsDead(), unit.Heal(5): a struct's or component's method.
    if (obj.kind == TY_STRUCT || obj.kind == TY_COMPONENT) {
        decl *const m = find_method(obj.decl, e->name);
        if (!m) {
            for (int i = 0; i < e->args.count; i++) check_expr(c, e->args.items[i]);
            diag_error(e->at, "%s has no method '" STR_FMT "'", type_name(obj), STR_ARG(e->name));
            suggestion s = suggest_start(e->name);
            for (int i = 0; i < obj.decl->methods.count; i++) suggest_consider(&s, obj.decl->methods.items[i]->name);
            suggest_note(&s);
            return T_ERR;
        }
        if (m->is_mut_method) check_writable(c, e->object, m, NULL);
        check_method_args(c, e, m);
        return m->return_type;
    }
    if (obj.kind != TY_ENTITY) {
        diag_error(e->at, "%s has no method '" STR_FMT "'", type_name(obj), STR_ARG(e->name));
        return T_ERR;
    }
    if (c->method) {
        diag_error(e->at, "%s can't change entities; systems do", routines(c));
        return T_ERR;
    }
    if (c->in_input) {
        diag_error(e->at, "%s runs outside the simulation, so it can't change entities", input_code(c));
        return T_ERR;
    }
    if (in_view(c)) {
        diag_error(e->at, "views only read the world, so they can't change entities");
        return T_ERR;
    }

    if (str_eq_c(e->name, "Add")) {
        e->call = CALL_ADD;
        if (e->args.count == 0) diag_error(e->at, "Add needs at least one component");
        c->prog->added_mask |= check_component_list(c, e, "Add");
        return T_VOID_;
    }

    if (str_eq_c(e->name, "Remove")) {
        e->call = CALL_REMOVE;
        if (e->args.count == 0) diag_error(e->at, "Remove needs at least one component");
        uint64_t mask = 0;
        for (int i = 0; i < e->args.count; i++) {
            expr *arg = e->args.items[i];
            if (arg->kind == E_LITERAL) {
                diag_error(arg->at, "Remove takes component types, like 'Stunned', not values");
                continue;
            }
            decl *d = check_component_arg(c, arg, "Remove");
            if (!d) continue;
            if (mask & bit(d)) diag_error(arg->at, "'" STR_FMT "' appears twice", STR_ARG(d->name));
            mask |= bit(d);
        }
        c->prog->removed_mask |= mask;
        return T_VOID_;
    }

    if (str_eq_c(e->name, "Destroy")) {
        e->call = CALL_DESTROY;
        if (e->args.count != 0) diag_error(e->at, "Destroy takes no arguments");
        c->prog->uses_destroy = true;
        return T_VOID_;
    }

    diag_error(e->at, "Entity has no method '" STR_FMT "'; it has Add, Remove and Destroy", STR_ARG(e->name));
    suggestion s = suggest_start(e->name);
    suggest_consider_c(&s, "Add");
    suggest_consider_c(&s, "Remove");
    suggest_consider_c(&s, "Destroy");
    suggest_note(&s);
    return T_ERR;
}

static const char *op_str(const tok_kind op)
{
    return tok_kind_name(op);
}

// Result type of `lhs op rhs`, or TY_ERROR after reporting.
static type binary_result(const tok_kind op, const type l, const type r, const loc at)
{
    if (l.kind == TY_ERROR || r.kind == TY_ERROR) return T_ERR;

    switch (op) {
    case T_PLUS:
    case T_MINUS:
    case T_STAR:
    case T_SLASH:
    case T_PERCENT: {
        if (l.kind == TY_QUATERNION || r.kind == TY_QUATERNION) {
            diag_error(at, "operator %s can't be used with quaternions", op_str(op));
            diag_note("combine rotations with Math.Mul(a, b) and rotate vectors with Math.Rotate(q, v)");
            return T_ERR;
        }
        // Matrices: + and - with the same type, * and / by a number.
        const int ml = matrix_dim(l);
        const int mr = matrix_dim(r);
        if (ml || mr) {
            if ((op == T_PLUS || op == T_MINUS) && l.kind == r.kind) return l;
            if ((op == T_STAR || op == T_SLASH) && ml && is_scalar_number(r)) return l;
            if (op == T_STAR && mr && is_scalar_number(l)) return r;
            if (op == T_STAR) {
                diag_error(at, "operator '*' doesn't multiply matrices");
                diag_note("use Math.Mul(a, b) for a matrix product, or with a vector");
                return T_ERR;
            }
            break;
        }
        // Numbers and vectors, component-wise. A scalar widens to the vector's
        // size, and int widens to float.
        if (type_is_numeric(l) && type_is_numeric(r)) {
            const int dl = type_dim(l);
            const int dr = type_dim(r);
            const bool is_float = type_is_float_based(l) || type_is_float_based(r);
            if (dl > 1 && dr > 1 && dl != dr) break;
            if (op == T_PERCENT && is_float) break;
            return vector_type(is_float, dl > dr ? dl : dr);
        }
        break;
    }
    case T_SHL:
    case T_SHR:
    case T_AMP:
    case T_PIPE:
    case T_CARET:
        if (l.kind == TY_INT && r.kind == TY_INT) return T_INT_;
        break;
    case T_LT:
    case T_LE:
    case T_GT:
    case T_GE:
        if (is_scalar_number(l) && is_scalar_number(r)) return T_BOOL_;
        break;
    case T_EQ:
    case T_NE:
        if (is_scalar_number(l) && is_scalar_number(r)) return T_BOOL_;
        if (l.kind == TY_BOOL && r.kind == TY_BOOL) return T_BOOL_;
        if (l.kind == TY_ENTITY && r.kind == TY_ENTITY) return T_BOOL_;
        if (l.kind == TY_PLAYER && r.kind == TY_PLAYER) return T_BOOL_;
        break;
    case T_AND:
    case T_OR:
        if (l.kind == TY_BOOL && r.kind == TY_BOOL) return T_BOOL_;
        break;
    default:
        break;
    }
    diag_error(at, "operator %s can't be used with %s and %s", op_str(op), type_name(l), type_name(r));
    if ((op == T_EQ || op == T_NE) && l.kind == TY_STRUCT && r.kind == TY_STRUCT) diag_note("compare their fields instead");
    return T_ERR;
}

// Reads a swizzle such as `xz` or `wzyx` into e->swizzle. False if a letter
// isn't one of the vector's components.
static bool parse_swizzle(expr *e, const int dim)
{
    if (e->member.len < 1 || e->member.len > 4) return false;
    for (int i = 0; i < e->member.len; i++) {
        const char ch = e->member.ptr[i];
        const int index = ch == 'x' ? 0 : ch == 'y' ? 1 : ch == 'z' ? 2 : ch == 'w' ? 3 : -1;
        if (index < 0 || index >= dim) return false;
        e->swizzle[i] = index;
    }
    e->swizzle_len = e->member.len;
    return true;
}

// Combat.Health or Game.Combat where a value belongs: says what the name is.
static type check_namespace_member(checker *c, expr *e)
{
    str text;
    qualified_text(e, &text);
    mark_namespaces(e->object);
    decl *d = find_type(c, text, e->at);
    if (d) {
        e->bind = BIND_TYPE;
        e->type_decl = d;
        diag_error(e->at, "'" STR_FMT "' is a type, not a value", STR_ARG(text));
        return T_ERR;
    }
    if (is_namespace(c->prog, text)) {
        diag_error(e->at, "'" STR_FMT "' is a namespace, not a value", STR_ARG(text));
        return T_ERR;
    }
    str ns;
    str name;
    split_qualified(text, &ns, &name);
    diag_error(e->at, "namespace '" STR_FMT "' has no '" STR_FMT "'", STR_ARG(ns), STR_ARG(name));
    suggestion s = suggest_start(name);
    for (int i = 0; i < c->prog->decls.count; i++) {
        const decl *other = c->prog->decls.items[i];
        if (other->kind != DECL_SYSTEM && str_eq(decl_ns(other), ns)) suggest_consider(&s, other->name);
    }
    suggest_note(&s);
    return T_ERR;
}

// input.jump or input.buttons.jump: a field of an input parameter, maybe
// inside structs.
static bool is_input_field(const expr *e)
{
    if (e->kind != E_MEMBER || !e->field) return false;
    for (e = e->object; e->kind == E_MEMBER; e = e->object) {
        if (!e->field) return false;
    }
    return e->kind == E_NAME && e->bind == BIND_PARAM && e->param->type.kind == TY_INPUT;
}

static type check_member(checker *c, expr *e)
{
    // quaternion.identity, Math.PI
    if (names_builtin_owner(c, e->object)) return resolve_builtin_member(e->object->name, e);

    // Combat.Health: a namespace, not a variable, on the left
    const expr *root = chain_root(e);
    str text;
    if (root->kind == E_NAME && !find_local(c, root->name) && !find_param(c, root->name)
        && !field_in_scope(c, root->name) && is_namespace(c->prog, root->name)
        && qualified_text(e, &text)) {
        return check_namespace_member(c, e);
    }

    const type obj = check_expr(c, e->object);
    if (obj.kind == TY_ERROR) return T_ERR;

    const int dim = type_dim(obj);
    if (dim >= 2) {
        static const char *components[] = {"", "", "x, y", "x, y, z", "x, y, z, w"};
        if (!parse_swizzle(e, dim)) {
            diag_error(e->at, "'" STR_FMT "' doesn't match %s's components (%s)", STR_ARG(e->member), type_name(obj),
                       components[dim]);
            return T_ERR;
        }
        return vector_type(type_is_float_based(obj), e->swizzle_len);
    }
    if (obj.kind == TY_QUATERNION) {
        if (str_eq_c(e->member, "value")) return (type){TY_FLOAT4, NULL};
        diag_error(e->at, "quaternion has one member, 'value' (a float4), not '" STR_FMT "'", STR_ARG(e->member));
        return T_ERR;
    }
    if (obj.kind == TY_COLOR) {
        if (str_eq_c(e->member, "r") || str_eq_c(e->member, "g") || str_eq_c(e->member, "b") || str_eq_c(e->member, "a")) {
            return T_FLOAT_;
        }
        diag_error(e->at, "Color has r, g, b and a, not '" STR_FMT "'", STR_ARG(e->member));
        return T_ERR;
    }
    const int n = matrix_dim(obj);
    if (n > 0) {
        if (e->member.len == 2 && e->member.ptr[0] == 'c' && e->member.ptr[1] >= '0' && e->member.ptr[1] < '0' + n) {
            return vector_type(true, n);
        }
        diag_error(e->at, "%s has columns c0 to c%d, not '" STR_FMT "'", type_name(obj), n - 1, STR_ARG(e->member));
        return T_ERR;
    }

    // input.jump.down: a bool field of an input parameter, compared with last tick.
    if (obj.kind == TY_BOOL) {
        const bool on_input_field = is_input_field(e->object);
        const bool edge = str_eq_c(e->member, "down") || str_eq_c(e->member, "up");
        if (edge && on_input_field) {
            e->edge = str_eq_c(e->member, "down") ? EDGE_DOWN : EDGE_UP;
            return T_BOOL_;
        }
        if (edge) {
            diag_error(e->at, "only input fields have '." STR_FMT "', like 'input.jump." STR_FMT "'",
                       STR_ARG(e->member), STR_ARG(e->member));
            if (c->in_input && !c->in_sanitize) {
                diag_note("in Sample, read the device instead, like 'keys.space.down'");
            }
            return T_ERR;
        }
        if (on_input_field && (str_eq_c(e->member, "pressed") || str_eq_c(e->member, "released"))) {
            diag_error(e->at, "input fields have '.down' and '.up', not '." STR_FMT "'", STR_ARG(e->member));
            diag_note(str_eq_c(e->member, "pressed")
                          ? "the field is true the whole time it's held; '.down' is true on the tick it went down"
                          : "'.up' is true on the tick it went up");
            return T_ERR;
        }
    }

    if (has_fields(obj)) {
        for (int i = 0; i < obj.decl->fields.count; i++) {
            field *f = &obj.decl->fields.items[i];
            if (str_eq(f->name, e->member)) {
                e->field = f;
                return f->type;
            }
        }
        diag_error(e->at, "%s has no field '" STR_FMT "'", type_name(obj), STR_ARG(e->member));
        if (obj.kind == TY_RECORD && str_eq_c(obj.decl->name, "Button") && str_eq_c(e->member, "released")) {
            diag_note("'up' is true on the tick the button went up");
            return T_ERR;
        }
        suggestion s = suggest_start(e->member);
        suggest_fields(&s, obj.decl);
        suggest_note(&s);
        return T_ERR;
    }
    diag_error(e->at, "%s has no members", type_name(obj));
    return T_ERR;
}

static type check_name(const checker *c, expr *e)
{
    stmt *local = find_local(c, e->name);
    if (local) {
        e->bind = BIND_LOCAL;
        e->local = local;
        return local->type;
    }
    param *p = find_param(c, e->name);
    if (p) {
        e->bind = BIND_PARAM;
        e->param = p;
        p->read = true;
        return p->type;
    }
    // Inside the input's Sample and Sanitize, and a type's methods, the fields
    // are in scope by name.
    field *f = field_in_scope(c, e->name);
    if (f) {
        e->bind = BIND_FIELD;
        e->field = f;
        return f->type;
    }
    const decl *d = find_type(c, e->name, e->at);
    if (d) {
        diag_error(e->at, "'" STR_FMT "' is a type, not a value", STR_ARG(e->name));
        return T_ERR;
    }
    if (is_namespace(c->prog, e->name)) {
        e->bind = BIND_NAMESPACE;
        diag_error(e->at, "'" STR_FMT "' is a namespace, not a value", STR_ARG(e->name));
        return T_ERR;
    }
    diag_error(e->at, "unknown name '" STR_FMT "'", STR_ARG(e->name));
    // What's in scope, and the built-in names that start expressions (math. for Math.)
    suggestion s = suggest_start(e->name);
    for (int i = 0; i < c->locals.count; i++) suggest_consider(&s, c->locals.items[i]->name);
    for (int i = 0; c->system && i < c->system->params.count; i++) suggest_consider(&s, c->system->params.items[i].name);
    if (c->in_input) suggest_fields(&s, c->system);
    if (c->method && c->method->owner) suggest_fields(&s, c->method->owner);
    suggest_consider_c(&s, "Math");
    suggest_consider_c(&s, "Draw");
    suggest_builtin_types(&s);
    suggest_note(&s);
    return T_ERR;
}

// A side of cond ? a : b that has to be a value.
static bool check_side(const expr *side, const type t)
{
    if (t.kind == TY_VOID) {
        diag_error(side->at, "this side of '?:' doesn't produce a value");
        return false;
    }
    if (t.kind == TY_STRING) {
        diag_error(side->at, "text can only be passed straight to Draw.Text for now");
        return false;
    }
    return t.kind != TY_ERROR;
}

// cond ? a : b. As in C#, the sides need the same type, or one that converts
// to the other's (int to float).
static type check_conditional(checker *c, expr *e)
{
    const type cond = check_expr(c, e->cond);
    if (cond.kind != TY_ERROR && cond.kind != TY_BOOL) {
        diag_error(e->cond->at, "the condition of '?:' must be bool, not %s", type_name(cond));
        if (type_is_numeric(cond) && type_dim(cond) == 1) diag_note("compare it, for example 'x != 0 ? a : b'");
    }
    c->branch_depth++;
    const type a = check_expr(c, e->lhs);
    const type b = check_expr(c, e->rhs);
    c->branch_depth--;
    const bool a_ok = check_side(e->lhs, a);
    const bool b_ok = check_side(e->rhs, b);
    if (!a_ok || !b_ok || cond.kind != TY_BOOL) return T_ERR;
    if (type_assignable(a, b)) return a;
    if (type_assignable(b, a)) return b;
    diag_error(e->at, "the two sides of '?:' have different types: %s and %s", type_name(a), type_name(b));
    diag_note("convert one side so both have the same type");
    return T_ERR;
}

static type check_expr(checker *c, expr *e)
{
    type t = T_ERR;
    switch (e->kind) {
    case E_INT: t = T_INT_; break;
    case E_FLOAT: t = T_FLOAT_; break;
    case E_BOOL: t = T_BOOL_; break;
    case E_STRING: t = (type){TY_STRING, NULL}; break;
    case E_NAME: t = check_name(c, e); break;
    case E_MEMBER: t = check_member(c, e); break;
    case E_CALL: t = check_call(c, e); break;
    case E_METHOD: t = check_method(c, e); break;
    case E_LITERAL: t = check_literal(c, e); break;
    case E_BINARY: {
        const bool short_circuit = e->op == T_AND || e->op == T_OR;
        const type l = check_expr(c, e->lhs);
        if (short_circuit) c->short_circuit_depth++;
        const type r = check_expr(c, e->rhs);
        if (short_circuit) c->short_circuit_depth--;
        t = binary_result(e->op, l, r, e->at);
        break;
    }
    case E_CONDITIONAL: t = check_conditional(c, e); break;
    case E_UNARY: {
        const type operand = check_expr(c, e->lhs);
        if (operand.kind == TY_ERROR) break;
        if (e->op == T_NOT && operand.kind == TY_BOOL) t = T_BOOL_;
        else if (e->op == T_TILDE && operand.kind == TY_INT) t = T_INT_;
        else if (e->op == T_MINUS && (type_is_numeric(operand) || matrix_dim(operand))) t = operand;
        else diag_error(e->at, "operator %s can't be used with %s", op_str(e->op), type_name(operand));
        break;
    }
    }
    e->type = t;
    return t;
}

// ---------------------------------------------------------------------------
// Statements

// Finds the variable an assignment target writes through, or NULL if the
// target isn't something that can be assigned.
static expr *assign_root(expr *target)
{
    while (target->kind == E_MEMBER) target = target->object;
    return target->kind == E_NAME ? target : NULL;
}

// Whether `target` can be changed: by an assignment, by calling mut method
// `called` on it, or passed to `called`'s mut parameter `arg_of`. Reports why not.
static bool check_writable(checker *c, expr *target, const decl *called, const param *arg_of)
{
    expr *root = assign_root(target);
    if (!root || root->bind == BIND_NONE || root->bind == BIND_TYPE || root->bind == BIND_NAMESPACE) {
        if (arg_of) {
            diag_error(target->at, "'" STR_FMT "' changes its '" STR_FMT "', so pass it a variable or field",
                       STR_ARG(called->name), STR_ARG(arg_of->name));
            diag_note("store the value in a 'mut var' first");
        } else if (called) {
            diag_error(target->at, "'" STR_FMT "' changes what it's called on, so that needs to be a variable or field",
                       STR_ARG(called->name));
            diag_note("store the value in a 'mut var' first");
        } else {
            diag_error(target->at, "can't assign to this expression");
        }
        return false;
    }
    if (root->bind == BIND_PARAM) root->param->written = true;

    if (root->bind == BIND_PARAM && root->param->mode != PARAM_MUT) {
        diag_error(root->at, "'" STR_FMT "' is read-only", STR_ARG(root->name));
        if (arg_of) diag_note("'" STR_FMT "' changes its '" STR_FMT "'", STR_ARG(called->name), STR_ARG(arg_of->name));
        else if (called) diag_note("'" STR_FMT "' is a mut method: it changes what it's called on", STR_ARG(called->name));
        if (root->param->function_param) {
            diag_note("declare the parameter as 'mut " STR_FMT " " STR_FMT "' to change the caller's variable, or copy "
                      "it into a 'mut var'", STR_ARG(root->param->type_name), STR_ARG(root->name));
        } else if (root->param->type.kind == TY_ENTITY) {
            diag_note("entity handles can't be reassigned");
        } else if (root->param->type.kind == TY_INPUT) {
            diag_note("input comes from the players; the simulation can only read it");
        } else if (root->param->type.kind == TY_RECORD) {
            diag_note("devices can only be read");
        } else if (root->param->type.kind == TY_SINGLETON && root->param->type.decl->builtin) {
            diag_note("'" STR_FMT "' is managed by the engine", STR_ARG(root->param->type_name));
        } else if (in_view(c)) {
            diag_note("views only read the world; systems change it");
        } else {
            diag_note("declare the parameter as 'mut " STR_FMT " " STR_FMT "' to write to it",
                      STR_ARG(root->param->type_name), STR_ARG(root->name));
            const fix f = {FIX_ADD_MUT, root->at, root->param};
            vec_push(c->prog->fixes, f);
        }
        return false;
    }
    if (root->bind == BIND_LOCAL && !root->local->is_mut) {
        const stmt *local = root->local;
        diag_error(root->at, "'" STR_FMT "' is read-only", STR_ARG(root->name));
        if (arg_of) diag_note("'" STR_FMT "' changes its '" STR_FMT "'", STR_ARG(called->name), STR_ARG(arg_of->name));
        else if (called) diag_note("'" STR_FMT "' is a mut method: it changes what it's called on", STR_ARG(called->name));
        if (local->type_name.len > 0) {
            diag_note("declare it as 'mut " STR_FMT " " STR_FMT " = ...' to change it", STR_ARG(local->type_name), STR_ARG(local->name));
        } else {
            diag_note("declare it as 'mut var " STR_FMT " = ...' to change it", STR_ARG(local->name));
        }
        return false;
    }
    if (root->bind == BIND_FIELD && c->method && !c->method->is_mut_method) {
        diag_error(root->at, "'" STR_FMT "' is read-only in '" STR_FMT "'", STR_ARG(root->name), STR_ARG(c->method->name));
        diag_note("declare the method as 'mut " STR_FMT " " STR_FMT "(...)' to change the fields",
                  STR_ARG(c->method->return_type_name), STR_ARG(c->method->name));
        return false;
    }
    return true;
}

static void check_assign(checker *c, const stmt *s)
{
    const type target = check_expr(c, s->target);
    const type value = check_expr(c, s->value);
    if (target.kind == TY_ERROR) return;
    if (!check_writable(c, s->target, NULL, NULL)) return;

    // Swizzles can be written (v.xz = ...) as long as no component repeats, but
    // only as the last step: in v.xy.x the swizzle is a temporary copy.
    for (const expr *m = s->target->object; s->target->kind == E_MEMBER && m->kind == E_MEMBER; m = m->object) {
        if (m->swizzle_len > 1) {
            diag_error(m->at, "can't assign through the swizzle '" STR_FMT "'", STR_ARG(m->member));
            diag_note("assign the components directly instead");
            return;
        }
    }
    if (s->target->kind == E_MEMBER && s->target->swizzle_len > 1) {
        for (int i = 0; i < s->target->swizzle_len; i++) {
            for (int j = 0; j < i; j++) {
                if (s->target->swizzle[i] == s->target->swizzle[j]) {
                    diag_error(s->target->at, "'" STR_FMT "' repeats a component, so it can't be assigned",
                               STR_ARG(s->target->member));
                    return;
                }
            }
        }
    }

    type result = value;
    if (s->op != T_ASSIGN) {
        result = binary_result(compound_op(s->op), target, value, s->at);
        if (result.kind == TY_ERROR) return;
    }
    if (!type_assignable(target, result)) {
        diag_error(s->value->at, "can't assign %s to %s", type_name(result), type_name(target));
    }
}

static void check_stmt(checker *c, stmt *s);

// `return;` everywhere, and `return value;` in a method that returns one.
static void check_return(checker *c, const stmt *s)
{
    const decl *m = c->method;
    const type value = s->value ? check_expr(c, s->value) : T_VOID_;
    if (!m) {
        if (s->value) {
            if (c->in_input) diag_error(s->value->at, "%s doesn't return a value; use 'return;'", input_code(c));
            else diag_error(s->value->at, "systems don't return values; use 'return;'");
        }
        return;
    }
    if (m->return_type.kind == TY_VOID) {
        if (s->value) diag_error(s->value->at, "'" STR_FMT "' returns nothing; use 'return;'", STR_ARG(m->name));
        return;
    }
    if (!s->value) {
        diag_error(s->at, "'" STR_FMT "' returns %s; write 'return value;'", STR_ARG(m->name), type_name(m->return_type));
        return;
    }
    if (!type_assignable(m->return_type, value)) {
        diag_error(s->value->at, "'" STR_FMT "' returns %s, not %s", STR_ARG(m->name), type_name(m->return_type),
                   type_name(value));
    }
}

// Whether every path through `s` ends in a return.
static bool always_returns(const stmt *s)
{
    if (!s) return false;
    switch (s->kind) {
    case S_RETURN: return true;
    case S_BLOCK:
        for (int i = 0; i < s->stmts.count; i++) {
            if (always_returns(s->stmts.items[i])) return true;
        }
        return false;
    case S_IF: return always_returns(s->then_stmt) && always_returns(s->else_stmt);
    default: return false;
    }
}

static void check_var(checker *c, stmt *s)
{
    type value = check_expr(c, s->value);
    if (s->value->kind == E_NAME && s->value->bind == BIND_TYPE) value = T_ERR;

    if (value.kind == TY_STRING) {
        diag_error(s->value->at, "text can only be passed straight to Draw.Text for now");
        value = T_ERR;
    }
    if (s->type_name.len == 0) {
        if (value.kind == TY_VOID) {
            diag_error(s->value->at, "this expression doesn't produce a value");
            value = T_ERR;
        }
        s->type = value;
    } else {
        decl *d = find_type(c, s->type_name, s->type_at);
        if (builtin_type_named(s->type_name, &s->type)) {
        } else if (d) {
            s->type = decl_type(d);
        } else {
            diag_error(s->type_at.line ? s->type_at : s->at, "unknown type '" STR_FMT "'", STR_ARG(s->type_name));
            suggestion sg = suggest_start(s->type_name);
            suggest_builtin_types(&sg);
            suggest_decls(&sg, c->prog, true, true, true);
            suggest_structs(&sg, c->prog);
            suggest_note(&sg);
            s->type = T_ERR;
        }
        if (!type_assignable(s->type, value)) {
            diag_error(s->value->at, "can't initialize %s with %s", type_name(s->type), type_name(value));
        }
    }

    check_reserved(s->name, s->at);
    if (find_local(c, s->name) || find_param(c, s->name)) {
        diag_error(s->at, "'" STR_FMT "' is already declared", STR_ARG(s->name));
    }
    vec_push(c->locals, s);
}

static void check_stmt(checker *c, stmt *s)
{
    switch (s->kind) {
    case S_BLOCK:
        push_scope(c);
        for (int i = 0; i < s->stmts.count; i++) check_stmt(c, s->stmts.items[i]);
        pop_scope(c);
        break;
    case S_IF: {
        const type cond = check_expr(c, s->cond);
        if (cond.kind != TY_ERROR && cond.kind != TY_BOOL) {
            diag_error(s->cond->at, "condition must be bool, not %s", type_name(cond));
        }
        push_scope(c);
        check_stmt(c, s->then_stmt);
        pop_scope(c);
        if (s->else_stmt) {
            push_scope(c);
            check_stmt(c, s->else_stmt);
            pop_scope(c);
        }
        break;
    }
    case S_RETURN:
        check_return(c, s);
        break;
    case S_VAR:
        check_var(c, s);
        break;
    case S_ASSIGN:
        check_assign(c, s);
        break;
    case S_EXPR: {
        check_expr(c, s->value);
        const builtin_call call = s->value->call;
        const bool effect = (s->value->kind == E_METHOD
                             && (call == CALL_ADD || call == CALL_REMOVE || call == CALL_DESTROY || call == CALL_DRAW))
                         || (s->value->kind == E_CALL && call == CALL_SPAWN) || call == CALL_METHOD || call == CALL_FUNCTION;
        if (!effect && s->value->type.kind != TY_ERROR) diag_error(s->value->at, "this expression does nothing on its own");
        break;
    }
    }
}

// ---------------------------------------------------------------------------
// Declarations

static bool all_constant(const expr *e);

// Constant expressions: literals, constructors of built-in types, Math
// functions, built-in constants like quaternion.identity, members of any of
// these, and operators on them. Nothing that reads fields, singletons or Time,
// so a default never depends on other state.
static bool is_constant(const expr *e)
{
    type ignored;
    switch (e->kind) {
    case E_INT:
    case E_FLOAT:
    case E_BOOL:
        return true;
    case E_UNARY:
        return is_constant(e->lhs);
    case E_BINARY:
        return is_constant(e->lhs) && is_constant(e->rhs);
    case E_CONDITIONAL:
        return is_constant(e->cond) && is_constant(e->lhs) && is_constant(e->rhs);
    case E_CALL:
        return builtin_type_named(e->name, &ignored) && all_constant(e);
    case E_METHOD:
        return e->object->kind == E_NAME && builtin_owner(e->object->name) && all_constant(e);
    case E_MEMBER:
        return (e->object->kind == E_NAME && builtin_owner(e->object->name)) || is_constant(e->object);
    case E_LITERAL:
        for (int i = 0; i < e->inits.count; i++) {
            if (!is_constant(e->inits.items[i].value)) return false;
        }
        return true;
    default:
        return false;
    }
}

static bool all_constant(const expr *e)
{
    for (int i = 0; i < e->args.count; i++) {
        if (!is_constant(e->args.items[i])) return false;
    }
    return true;
}

static void check_default(checker *c, const field *f)
{
    expr *value = f->default_value;
    if (f->type.kind == TY_ENTITY) {
        diag_error(value->at, "Entity fields always start as the null entity; they can't have a default value");
        return;
    }
    if (!is_constant(value)) {
        diag_error(value->at, "default values must be constant expressions");
        diag_note("use literals, constructors like float3(...), struct values of constants, Math functions and "
                  "operators; defaults can't read fields, singletons or Time");
        return;
    }
    const type t = check_expr(c, value);
    if (f->type.kind != TY_ERROR && !type_assignable(f->type, t)) {
        diag_error(value->at, "field '" STR_FMT "' is %s, not %s", STR_ARG(f->name), type_name(f->type), type_name(t));
    }
}

// A bound of [Clamp], [Min] or [Max] on field `f`: a constant of the field's
// type, or a number for every component of a vector.
static void check_bound(checker *c, const field *f, expr *value)
{
    if (!is_constant(value)) {
        diag_error(value->at, "attribute bounds must be constants");
        diag_note("use literals, constructors like float2(...), Math constants and operators");
        return;
    }
    const type t = check_expr(c, value);
    if (t.kind == TY_ERROR || f->type.kind == TY_ERROR) return;
    const bool splat = type_dim(t) == 1 && type_is_numeric(t) && type_dim(f->type) > 1
                    && (type_is_float_based(f->type) || type_is_int_based(t));
    if (!type_assignable(f->type, t) && !splat) {
        diag_error(value->at, "the bound is %s, but field '" STR_FMT "' is %s", type_name(t), STR_ARG(f->name),
                   type_name(f->type));
    }
}

// [Clamp(lo, hi)], [Min(x)] and [Max(x)] on input and struct fields. The
// engine applies them to every input before Sanitize, the fields of structs in
// it included, so they're a quick way to bound what players send. Elsewhere,
// a struct field's bounds only describe it.
static void check_field_attributes(checker *c, const decl *d, field *f)
{
    if (f->attributes.count == 0) return;
    if (d->kind != DECL_INPUT && d->kind != DECL_STRUCT) {
        diag_error(f->attributes.items[0].at, "field attributes only work on input and struct fields for now");
        diag_note("in an input, they bound what players send: [Clamp(lo, hi)], [Min(x)] and [Max(x)]");
        return;
    }
    bool has_clamp = false;
    bool has_min = false;
    bool has_max = false;
    for (int i = 0; i < f->attributes.count; i++) {
        attribute *a = &f->attributes.items[i];
        const bool clamp = str_eq_c(a->name, "Clamp");
        const bool min = str_eq_c(a->name, "Min");
        const bool max = str_eq_c(a->name, "Max");
        if (!clamp && !min && !max) {
            diag_error(a->at, "unknown field attribute '" STR_FMT "'", STR_ARG(a->name));
            suggestion s = suggest_start(a->name);
            suggest_consider_c(&s, "Clamp");
            suggest_consider_c(&s, "Min");
            suggest_consider_c(&s, "Max");
            suggest_note(&s);
            continue;
        }
        const int want = clamp ? 2 : 1;
        if (a->values.count != want) {
            diag_error(a->at, clamp ? "[Clamp] takes two bounds: [Clamp(lo, hi)]"
                                    : min ? "[Min] takes one bound: [Min(x)]" : "[Max] takes one bound: [Max(x)]");
            continue;
        }
        if ((clamp && has_clamp) || (min && has_min) || (max && has_max)) {
            diag_error(a->at, "field '" STR_FMT "' already has [" STR_FMT "]", STR_ARG(f->name), STR_ARG(a->name));
            continue;
        }
        if ((clamp && (has_min || has_max)) || ((min || max) && has_clamp)) {
            diag_error(a->at, "[Clamp] already sets both bounds; use it, or [Min] and [Max]");
            continue;
        }
        has_clamp |= clamp;
        has_min |= min;
        has_max |= max;
        if (f->type.kind != TY_ERROR && !type_is_numeric(f->type)) {
            diag_error(a->at, "[" STR_FMT "] works on numbers and vectors, not %s", STR_ARG(a->name), type_name(f->type));
            continue;
        }
        for (int k = 0; k < a->values.count; k++) check_bound(c, f, a->values.items[k]);
    }
}

// A field's type: a built-in type or a struct.
static void resolve_field_types(const checker *c, const decl *d)
{
    for (int i = 0; i < d->fields.count; i++) {
        field *f = &d->fields.items[i];
        const loc at = f->type_qual_at.line ? f->type_qual_at : f->at;
        if (builtin_type_named(f->type_name, &f->type)) continue;
        f->type = T_ERR;
        decl *const t = find_type(c, f->type_name, at);
        if (t && t->kind == DECL_STRUCT) {
            f->type = (type){TY_STRUCT, t};
        } else if (t) {
            const char *what = t->kind == DECL_COMPONENT ? "components" : t->kind == DECL_SINGLETON ? "singletons" : "inputs";
            diag_error(at, "fields can't hold %s", what);
            diag_note("to share fields between types, declare a struct, like 'struct Name { ... }', and use it in both");
        } else {
            diag_error(at, "unknown type '" STR_FMT "'", STR_ARG(f->type_name));
            suggestion s = suggest_start(f->type_name);
            suggest_builtin_types(&s);
            suggest_structs(&s, c->prog);
            suggest_note(&s);
        }
    }
}

// Puts `d` in prog->structs after the structs it contains. A struct can't
// contain itself, even through others: it would be infinitely big. `index`
// tracks the visit: 0 not yet, 1 on the way down, 2 done.
static void order_struct(program *prog, decl *d)
{
    if (d->index == 2) return;
    d->index = 1;
    for (int i = 0; i < d->fields.count; i++) {
        field *f = &d->fields.items[i];
        if (f->type.kind != TY_STRUCT) continue;
        decl *const inner = f->type.decl;
        if (inner->index == 1) {
            if (inner == d) {
                diag_error(f->type_at, "struct '" STR_FMT "' can't contain itself", STR_ARG(d->name));
            } else {
                diag_error(f->type_at, "struct '" STR_FMT "' can't contain '" STR_FMT "': '" STR_FMT "' already contains '"
                           STR_FMT "'", STR_ARG(d->name), STR_ARG(inner->name), STR_ARG(inner->name), STR_ARG(d->name));
            }
            diag_note("it would be infinitely big");
            f->type = T_ERR; // Breaks the loop for everything that walks fields
            continue;
        }
        order_struct(prog, inner);
    }
    d->index = 2;
    vec_push(prog->structs, d);
}

static void check_fields(checker *c, const decl *d)
{
    for (int i = 0; i < d->fields.count; i++) {
        field *f = &d->fields.items[i];
        check_reserved(f->name, f->at);
        for (int j = 0; j < i; j++) {
            if (str_eq(d->fields.items[j].name, f->name)) {
                diag_error(f->at, "field '" STR_FMT "' is declared twice", STR_ARG(f->name));
            }
        }
        if (f->default_value) check_default(c, f);
        check_field_attributes(c, d, f);
    }
}

// A method's parameter or return type: a built-in type, a struct or a
// component, or `void` for what it returns.
static type method_type(const checker *c, const str name, const loc at, const bool is_return)
{
    type t;
    if (is_return && str_eq_c(name, "void")) return T_VOID_;
    if (builtin_type_named(name, &t)) return t;
    decl *const d = find_type(c, name, at);
    if (d && (d->kind == DECL_STRUCT || d->kind == DECL_COMPONENT)) return decl_type(d);
    if (d) {
        diag_error(at, "methods take and return built-in types, structs and components, not %s",
                   d->kind == DECL_SINGLETON ? "singletons" : "inputs");
    } else if (str_eq_c(name, "void")) {
        diag_error(at, "'void' only goes before a method that returns nothing");
    } else {
        diag_error(at, "unknown type '" STR_FMT "'", STR_ARG(name));
        suggestion s = suggest_start(name);
        suggest_builtin_types(&s);
        suggest_structs(&s, c->prog);
        if (is_return) suggest_consider_c(&s, "void");
        suggest_note(&s);
    }
    return T_ERR;
}

// The types in a method's or function's signature.
static void check_signature(const checker *c, decl *m)
{
    m->return_type = method_type(c, m->return_type_name, m->return_type_qual_at, true);
    for (int k = 0; k < m->params.count; k++) {
        param *p = &m->params.items[k];
        check_reserved(p->name, p->at);
        for (int j = 0; j < k; j++) {
            if (str_eq(m->params.items[j].name, p->name)) {
                diag_error(p->name_at, "parameter '" STR_FMT "' is declared twice", STR_ARG(p->name));
            }
        }
        p->type = method_type(c, p->type_name, p->type_qual_at, false);
    }
}

// Where methods go (structs and components), their names, and their signatures.
static void check_method_decls(const checker *c, const decl *d)
{
    for (int i = 0; i < d->methods.count; i++) {
        decl *m = d->methods.items[i];
        if (d->kind != DECL_STRUCT && d->kind != DECL_COMPONENT) {
            diag_error(m->at, "methods work on structs and components");
            if (d->kind == DECL_INPUT) diag_note("an input has Sample and Sanitize");
            else diag_note("put the data and its methods in a struct, and the struct in the singleton");
        }
        check_reserved(m->name, m->at);
        if (field_named(d, m->name)) {
            diag_error(m->at, "'" STR_FMT "' already has a field '" STR_FMT "'", STR_ARG(d->name), STR_ARG(m->name));
        }
        for (int j = 0; j < i; j++) {
            if (str_eq(d->methods.items[j]->name, m->name)) {
                diag_error(m->at, "'" STR_FMT "' already has a method '" STR_FMT "'", STR_ARG(d->name), STR_ARG(m->name));
                diag_note("methods can't share a name, even with different parameters");
            }
        }
        check_signature(c, m);
    }
}

static void check_function_decl(const checker *c, decl *fn)
{
    if (fn->is_mut_method) {
        diag_error(fn->at, "only methods are 'mut': they change their struct's fields");
        diag_note("a function changes what's passed to its 'mut' parameters, like 'void Heal(mut Stats stats)'");
    }
    check_signature(c, fn);
}

static void check_method_body(checker *c, decl *m)
{
    c->method = m;
    c->system = m; // For its parameters
    c->unit = m->unit;
    check_stmt(c, m->body);
    const type_kind ret = m->return_type.kind;
    if (ret != TY_VOID && ret != TY_ERROR && !always_returns(m->body)) {
        diag_error(m->at, "'" STR_FMT "' doesn't return a value on every path", STR_ARG(m->name));
        diag_note("end every path with 'return value;', as the method returns %s", type_name(m->return_type));
    }
    c->method = NULL;
    c->system = NULL;
}

static void check_params(const checker *c, decl *sys)
{
    const program *prog = c->prog;
    bool has_entity = false;
    bool has_input = false;
    uint64_t seen = 0;

    for (int i = 0; i < sys->params.count; i++) {
        param *p = &sys->params.items[i];
        decl *d = find_type(c, p->type_name, p->type_at);
        bool is_entity = str_eq_c(p->type_name, "Entity");

        if (p->name.len > 0) {
            check_reserved(p->name, p->at);
            for (int j = 0; j < i; j++) {
                if (str_eq(sys->params.items[j].name, p->name)) {
                    diag_error(p->at, "parameter '" STR_FMT "' is declared twice", STR_ARG(p->name));
                }
            }
        }

        if (sys->is_view && p->mode == PARAM_MUT) {
            diag_error(p->at, "views only read the world, so their parameters can't be 'mut'");
            diag_note("views run once per frame, outside the simulation");
            p->mode = PARAM_READ;
        }

        if (is_entity) {
            if (p->mode != PARAM_READ) {
                diag_error(p->at, "Entity parameters can't be 'mut', 'with' or 'without'");
            }
            if (has_entity) diag_error(p->at, "a system can only have one Entity parameter");
            has_entity = true;
            p->type = T_ENTITY_;
            continue;
        }

        if (str_eq_c(p->type_name, "Devices")) {
            diag_error(p->at, "Devices can only be read in the input's Sample");
            diag_note("systems read the players' input instead, through an input parameter");
            p->type = T_ERR;
            continue;
        }

        if (!d) {
            diag_error(p->type_at.line ? p->type_at : p->at, "unknown component or singleton '" STR_FMT "'",
                       STR_ARG(p->type_name));
            suggestion s = suggest_start(p->type_name);
            suggest_decls(&s, prog, true, p->mode != PARAM_WITH && p->mode != PARAM_WITHOUT, true);
            suggest_consider_c(&s, "Entity");
            suggest_note(&s);
            p->type = T_ERR;
            continue;
        }

        if (d->kind == DECL_STRUCT) {
            diag_error(p->type_at.line ? p->type_at : p->at, "'" STR_FMT "' is a struct; systems take components and singletons",
                       STR_ARG(d->name));
            diag_note("keep it in a component or singleton, like 'component Name { " STR_FMT " value; }'",
                      STR_ARG(d->name));
            p->type = T_ERR;
            continue;
        }

        // An input parameter gives the input of the player who owns the entity,
        // or the server's input when no player does. It doesn't filter entities.
        if (d->kind == DECL_INPUT) {
            if (sys->is_view) {
                diag_error(p->at, "views can't read input yet");
                diag_note("read the state the input changed instead, like a component the systems update");
            }
            if (p->mode != PARAM_READ) {
                diag_error(p->at, "input can't be 'mut', 'with' or 'without'; the simulation can only read it");
            }
            if (has_input) diag_error(p->at, "a system can only have one input parameter");
            has_input = true;
            p->type = (type){TY_INPUT, d};
            continue;
        }

        if (d->kind == DECL_SINGLETON) {
            if (p->mode == PARAM_WITH || p->mode == PARAM_WITHOUT) {
                diag_error(p->at, "'with' and 'without' filter entities by component; '" STR_FMT "' is a singleton",
                           STR_ARG(d->name));
            }
            if (p->mode == PARAM_MUT && d->builtin) {
                diag_error(p->at, "'" STR_FMT "' is managed by the engine and is read-only", STR_ARG(d->name));
            }
            for (int j = 0; j < i; j++) {
                if (sys->params.items[j].type.decl == d) {
                    diag_error(p->at, "'" STR_FMT "' appears twice", STR_ARG(d->name));
                }
            }
            p->type = (type){TY_SINGLETON, d};
            continue;
        }

        p->type = (type){TY_COMPONENT, d};
        if (seen & bit(d)) diag_error(p->at, "'" STR_FMT "' appears twice", STR_ARG(d->name));
        seen |= bit(d);
        if (p->mode == PARAM_WITHOUT) sys->without_mask |= bit(d);
        else sys->need_mask |= bit(d);
    }

    if (sys->need_mask & sys->without_mask) {
        diag_error(sys->at, "system '" STR_FMT "' both requires and excludes the same component", STR_ARG(sys->name));
    }
    sys->per_entity = has_entity || seen != 0;

    if (sys->is_main && has_input) {
        diag_error(sys->at, "Main runs once when the world is created, before any input arrives");
        diag_note("read the input in a system; it runs every tick");
    } else if (sys->is_main && sys->per_entity) {
        diag_error(sys->at, "Main runs once when the world is created, so it can only take singletons");
    }
}

// Access a system declares but doesn't use makes other systems wait for
// nothing, so it's worth a warning.
static void warn_unused_params(checker *c, const decl *sys)
{
    for (int i = 0; i < sys->params.count; i++) {
        const param *p = &sys->params.items[i];
        const type_kind kind = p->type.kind;
        if (p->name.len == 0 || (kind != TY_COMPONENT && kind != TY_SINGLETON)) continue;
        const str type = p->type_name;
        if (!p->read && kind == TY_COMPONENT) {
            diag_warning(p->name_at, "'" STR_FMT "' is never used", STR_ARG(p->name));
            diag_note("to only require the component, write 'with " STR_FMT "': a filter doesn't make other "
                      "systems wait",
                      STR_ARG(type));
            const fix f = {FIX_USE_WITH, p->name_at, p};
            vec_push(c->prog->fixes, f);
        } else if (!p->read) {
            diag_warning(p->name_at, "'" STR_FMT "' is never used", STR_ARG(p->name));
            diag_note("remove it: systems that write " STR_FMT " wait for this one while it's declared", STR_ARG(type));
        } else if (p->mode == PARAM_MUT && !p->written && !sys->is_view) {
            diag_warning(p->at, "'" STR_FMT "' is declared mut but never written", STR_ARG(p->name));
            diag_note("without 'mut', systems that read " STR_FMT " can run alongside this one", STR_ARG(type));
            const fix f = {FIX_REMOVE_MUT, p->at, p};
            vec_push(c->prog->fixes, f);
        }
    }
}

static void add_builtins(program *prog)
{
    // singleton Time { float dt; int tick; }
    decl *time = NEW(decl);
    time->kind = DECL_SINGLETON;
    time->name = str_from("Time");
    time->builtin = true;
    const field dt = {str_from("dt"), str_from("float"), {0, 0, 0}, {0}, NULL, {0, 0, 0}, {0}, {0, 0, 0}};
    const field tick = {str_from("tick"), str_from("int"), {0, 0, 0}, {0}, NULL, {0, 0, 0}, {0}, {0, 0, 0}};
    vec_push(time->fields, dt);
    vec_push(time->fields, tick);

    // component Owner { PlayerID player; }: ties an entity to a player.
    decl *owner = NEW(decl);
    owner->kind = DECL_COMPONENT;
    owner->name = str_from("Owner");
    owner->builtin = true;
    const field player = {str_from("player"), str_from("PlayerID"), {0, 0, 0}, {0}, NULL, {0, 0, 0}, {0}, {0, 0, 0}};
    vec_push(owner->fields, player);
    prog->owner = owner;

    VEC(decl *) decls = {0};
    vec_push(decls, time);
    vec_push(decls, owner);
    for (int i = 0; i < prog->decls.count; i++) vec_push(decls, prog->decls.items[i]);
    prog->decls.items = decls.items;
    prog->decls.count = decls.count;
    prog->decls.cap = decls.cap;
}

static decl *new_record(program *prog, const char *name, const char *c_name)
{
    decl *d = NEW(decl);
    d->kind = DECL_RECORD;
    d->name = str_from(name);
    d->c_name = c_name;
    d->builtin = true;
    vec_push(prog->records, d);
    return d;
}

static void record_field(decl *d, const char *name, const type t)
{
    const field f = {str_from(name), str_from(""), {0, 0, 0}, t, NULL, {0, 0, 0}, {0}, {0, 0, 0}};
    vec_push(d->fields, f);
}

// The device records an input's Sample reads. Their members come from the
// X-macros in purr/devices.h, so PurrLang and the C structs always match.
static void add_device_records(program *prog)
{
    const type t_bool = {TY_BOOL, NULL};
    const type t_float = {TY_FLOAT, NULL};
    const type t_float2 = {TY_FLOAT2, NULL};

    decl *button = new_record(prog, "Button", "purr_button");
    record_field(button, "pressed", t_bool);
    record_field(button, "down", t_bool);
    record_field(button, "up", t_bool);
    const type t_button = {TY_RECORD, button};

    decl *dpad = new_record(prog, "Dpad", "purr_dpad");
    decl *keyboard = new_record(prog, "Keyboard", "purr_keyboard");
    decl *mouse = new_record(prog, "Mouse", "purr_mouse");
    decl *gamepad = new_record(prog, "Gamepad", "purr_gamepad");

#define KEY(name) record_field(keyboard, #name, t_button);
#define MOUSE_AXIS(name) record_field(mouse, #name, t_float2);
#define MOUSE_BUTTON(name) record_field(mouse, #name, t_button);
#define DPAD_BUTTON(name) record_field(dpad, #name, t_button);
#define STICK(name) record_field(gamepad, #name, t_float2);
#define TRIGGER(name) record_field(gamepad, #name, t_float);
#define GAMEPAD_BUTTON(name) record_field(gamepad, #name, t_button);
    PURR_KEYBOARD_KEYS(KEY)
    PURR_MOUSE_AXES(MOUSE_AXIS)
    PURR_MOUSE_BUTTONS(MOUSE_BUTTON)
    PURR_DPAD_BUTTONS(DPAD_BUTTON)
    record_field(gamepad, "connected", t_bool);
    PURR_GAMEPAD_STICKS(STICK)
    PURR_GAMEPAD_TRIGGERS(TRIGGER)
    PURR_GAMEPAD_BUTTONS(GAMEPAD_BUTTON)
    record_field(gamepad, "dpad", (type){TY_RECORD, dpad});
#undef KEY
#undef MOUSE_AXIS
#undef MOUSE_BUTTON
#undef DPAD_BUTTON
#undef STICK
#undef TRIGGER
#undef GAMEPAD_BUTTON

    decl *devices = new_record(prog, "Devices", "purr_devices");
    record_field(devices, "keyboard", (type){TY_RECORD, keyboard});
    record_field(devices, "mouse", (type){TY_RECORD, mouse});
    record_field(devices, "gamepad", (type){TY_RECORD, gamepad});
    prog->devices = devices;
}

// Names the language reserves: built-in types and function groups.
static bool is_builtin_name(const str name)
{
    type dummy;
    return builtin_type_named(name, &dummy) || str_eq_c(name, "Math") || str_eq_c(name, "Draw")
        || str_eq_c(name, "Devices") || str_eq_c(name, "Spawn");
}

// What a declaration is called in generated C: Combat_Health for Combat.Health.
static void c_name_of(const decl *d, sb *out)
{
    const str ns = decl_ns(d);
    for (int i = 0; i < ns.len; i++) sb_putn(out, ns.ptr[i] == '.' ? "_" : &ns.ptr[i], 1);
    if (ns.len > 0) sb_put(out, "_");
    sb_putn(out, d->name.ptr, (size_t)d->name.len);
}

static void collect_decls(program *prog)
{
    for (int i = 0; i < prog->decls.count; i++) {
        decl *d = prog->decls.items[i];
        const str ns = decl_ns(d);
        if (ns.len > 0) {
            sb qualified = {0};
            sb_printf(&qualified, STR_FMT "." STR_FMT, STR_ARG(ns), STR_ARG(d->name));
            d->qualified = (str){qualified.data, (int)qualified.len};
        } else {
            d->qualified = d->name;
        }

        if (!d->builtin) {
            check_reserved(d->name, d->at);
            if (is_builtin_name(d->name)) {
                diag_error(d->at, "'" STR_FMT "' is built into the language", STR_ARG(d->name));
            }
            for (int j = 0; j < i; j++) {
                const decl *other = prog->decls.items[j];
                const bool both_types = is_kind(d, NAME_TYPE) && is_kind(other, NAME_TYPE);
                const bool both_systems = d->kind == DECL_SYSTEM && other->kind == DECL_SYSTEM;
                const bool function = d->kind == DECL_FUNCTION || other->kind == DECL_FUNCTION;
                if (!both_types && !both_systems && !function) continue;
                if (str_eq(d->name, other->name) && str_eq(ns, decl_ns(other))) {
                    if (other->builtin) {
                        diag_error(d->at, "'" STR_FMT "' is built into the engine", STR_ARG(d->name));
                    } else {
                        diag_error(d->at, "'" STR_FMT "' is already declared", STR_ARG(d->qualified));
                        const source *src = diag_source(other->at.file);
                        if (src && other->at.file != d->at.file) diag_note("the other one is in %s", src->path);
                    }
                    continue;
                }
                // Different names that generated C would spell the same: Combat.Health and Combat_Health.
                sb mine = {0};
                sb theirs = {0};
                c_name_of(d, &mine);
                c_name_of(other, &theirs);
                if (strcmp(mine.data, theirs.data) == 0) {
                    diag_error(d->at, "'" STR_FMT "' and '" STR_FMT "' would have the same name in generated C, %s",
                               STR_ARG(d->qualified), STR_ARG(other->qualified), mine.data);
                    diag_note("rename one of them");
                }
            }
        }

        switch (d->kind) {
        case DECL_COMPONENT:
            d->index = prog->components.count;
            if (d->index == MAX_COMPONENTS) {
                diag_error(d->at, "too many components; the limit is %d for now", MAX_COMPONENTS);
            }
            vec_push(prog->components, d);
            break;
        case DECL_SINGLETON:
            d->index = prog->singletons.count;
            vec_push(prog->singletons, d);
            break;
        case DECL_INPUT:
            if (prog->input) diag_error(d->at, "a game has one input declaration; '" STR_FMT "' is already it",
                                        STR_ARG(prog->input->name));
            else prog->input = d;
            break;
        case DECL_RECORD:
        case DECL_STRUCT: // Ordered once their fields are known; see order_struct
        case DECL_METHOD: // Not in prog->decls
        case DECL_FUNCTION:
            break;
        case DECL_SYSTEM:
            if (d->is_view) {
                d->index = prog->views.count;
                vec_push(prog->views, d);
            } else if (str_eq_c(d->name, "Main")) {
                d->is_main = true;
                if (prog->main) {
                    diag_error(d->at, "there can only be one 'system Main()'");
                    const source *src = diag_source(prog->main->at.file);
                    if (src) diag_note("the other one is in %s", src->path);
                } else {
                    prog->main = d;
                }
            } else {
                d->index = prog->systems.count;
                vec_push(prog->systems, d);
            }
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Archetypes

static int find_archetype(const program *prog, const uint64_t mask)
{
    for (int i = 0; i < prog->archetypes.count; i++) {
        if (prog->archetypes.items[i] == mask) return i;
    }
    return -1;
}

static bool add_archetype(program *prog, const uint64_t mask)
{
    if (find_archetype(prog, mask) >= 0) return true;
    if (prog->archetypes.count == MAX_ARCHETYPES) return false;
    vec_push(prog->archetypes, mask);
    return true;
}

static void append_component_names(const program *prog, const uint64_t mask, char *buf, const size_t size)
{
    size_t len = strlen(buf);
    bool first = true;
    for (int i = 0; i < prog->components.count && len + 1 < size; i++) {
        if (!(mask & ((uint64_t)1 << i))) continue;
        const int n = snprintf(buf + len, size - len, "%s" STR_FMT, first ? "" : ", ", STR_ARG(prog->components.items[i]->name));
        if (n > 0) len += (size_t)n;
        first = false;
    }
}

// Every spawn creates an archetype; every Add and Remove can move an entity from
// any archetype to another, so keep applying them until nothing new appears.
static void derive_archetypes(const checker *c)
{
    program *prog = c->prog;
    for (int i = 0; i < c->spawns.count; i++) {
        if (!add_archetype(prog, c->spawns.items[i]->spawn_mask)) goto too_many;
    }
    for (int i = 0; i < prog->archetypes.count; i++) {
        for (int bit_index = 0; bit_index < prog->components.count; bit_index++) {
            const uint64_t b = (uint64_t)1 << bit_index;
            const uint64_t current = prog->archetypes.items[i];
            if ((prog->added_mask & b) && !add_archetype(prog, current | b)) goto too_many;
            if ((prog->removed_mask & b) && !add_archetype(prog, current & ~b)) goto too_many;
        }
    }
    for (int i = 0; i < prog->archetypes.count; i++) vec_push(prog->spawn_target, false);
    for (int i = 0; i < c->spawns.count; i++) {
        const int a = find_archetype(prog, c->spawns.items[i]->spawn_mask);
        c->spawns.items[i]->spawn_archetype = a;
        prog->spawn_target.items[a] = true;
    }
    return;

too_many:
    diag_error((loc){1, 1, 0}, "the program can create more than %d different component combinations", MAX_ARCHETYPES);
    diag_note("every Add and Remove can apply to any entity, so combinations multiply");
}

static void warn_unmatched(const program *prog, const decl *const *list, const int count)
{
    for (int i = 0; i < count; i++) {
        const decl *sys = list[i];
        if (!sys->per_entity) continue;
        bool matched = false;
        for (int a = 0; a < prog->archetypes.count; a++) {
            const uint64_t mask = prog->archetypes.items[a];
            if ((mask & sys->need_mask) == sys->need_mask && !(mask & sys->without_mask)) matched = true;
        }
        if (!matched) {
            diag_warning(sys->at, "%s '" STR_FMT "' never runs: no entity matches its parameters",
                         sys->is_view ? "view" : "system", STR_ARG(sys->name));
            if (sys->need_mask) {
                char names[512] = "";
                append_component_names(prog, sys->need_mask, names, sizeof names);
                diag_note("nothing spawns or adds an entity with %s", names);
            }
        }
    }
}

// input PlayerInput { ...; Sample(Devices devices) { ... } }: Sample runs on
// the client, outside the simulation. It reads devices and assigns the input's
// fields, which start at their defaults.
static void check_sample(checker *c, decl *input)
{
    if (!input->body) return; // Without Sample, sampling gives the defaults.

    if (input->params.count != 1 || !str_eq_c(input->params.items[0].type_name, "Devices")) {
        diag_error(input->body_at, "Sample takes the devices: 'Sample(Devices devices)'");
    }
    for (int i = 0; i < input->params.count; i++) {
        param *p = &input->params.items[i];
        check_reserved(p->name, p->at);
        p->mode = PARAM_READ;
        p->type = str_eq_c(p->type_name, "Devices") ? (type){TY_RECORD, c->prog->devices} : T_ERR;
    }

    c->system = input;
    c->in_input = true;
    check_stmt(c, input->body);
    c->in_input = false;
    c->system = NULL;
}

// Sanitize() { ... }: runs on every input before the simulation reads it, so
// systems can rely on what it guarantees. It assigns the input's fields by
// name, like Sample, and reads nothing else.
static void check_sanitize(checker *c, decl *input)
{
    if (!input->sanitize) return;
    c->system = input;
    c->in_input = true;
    c->in_sanitize = true;
    check_stmt(c, input->sanitize);
    c->in_sanitize = false;
    c->in_input = false;
    c->system = NULL;
}

// `namespace` and `using` at the top of each file.
static void check_units(const program *prog)
{
    for (int i = 0; i < prog->units.count; i++) {
        const unit *u = prog->units.items[i];
        if (u->ns.len > 0) {
            const char *dot = memchr(u->ns.ptr, '.', (size_t)u->ns.len);
            const str root = {u->ns.ptr, dot ? (int)(dot - u->ns.ptr) : u->ns.len};
            if (is_builtin_name(root)) {
                diag_error(u->ns_at, "'" STR_FMT "' is built into the language, so it can't name a namespace", STR_ARG(root));
            }
            check_reserved(u->ns, u->ns_at);
        }
        for (int k = 0; k < u->usings.count; k++) {
            const str used = u->usings.items[k];
            if (is_namespace(prog, used)) continue;
            diag_error(u->using_at.items[k], "no file declares the namespace '" STR_FMT "'", STR_ARG(used));
            suggestion s = suggest_start(used);
            for (int j = 0; j < prog->units.count; j++) suggest_consider(&s, prog->units.items[j]->ns);
            suggest_note(&s);
        }
    }
}

// [Before(X)] and [After(X)] order systems, and views among views.
static void check_attributes(checker *c)
{
    for (int i = 0; i < c->prog->decls.count; i++) {
        decl *d = c->prog->decls.items[i];
        for (int a = 0; a < d->attributes.count; a++) {
            attribute *attr = &d->attributes.items[a];
            const bool before = str_eq_c(attr->name, "Before");
            if (!before && !str_eq_c(attr->name, "After")) {
                diag_error(attr->at, "unknown attribute '" STR_FMT "'", STR_ARG(attr->name));
                suggestion s = suggest_start(attr->name);
                suggest_consider_c(&s, "Before");
                suggest_consider_c(&s, "After");
                suggest_note(&s);
                diag_note("the attributes are Before and After, which order systems: [After(Physics.Integrate)]");
                continue;
            }
            if (d->kind != DECL_SYSTEM) {
                diag_error(attr->at, "'" STR_FMT "' orders systems and views; '" STR_FMT "' is neither",
                           STR_ARG(attr->name), STR_ARG(d->name));
                continue;
            }
            if (d->is_main) {
                diag_error(attr->at, "Main runs once when the world is created, before any system, so it isn't ordered");
                continue;
            }
            if (attr->args.count == 0) {
                diag_error(attr->at, "'" STR_FMT "' needs the %s it runs %s, like [" STR_FMT "(Movement)]",
                           STR_ARG(attr->name), d->is_view ? "views" : "systems", before ? "before" : "after",
                           STR_ARG(attr->name));
                continue;
            }
            const char *kind = d->is_view ? "view" : "system";
            for (int k = 0; k < attr->args.count; k++) {
                qname *q = &attr->args.items[k];
                decl *other;
                decl *target = lookup(c->prog, d->unit, q->text, NAME_SYSTEM, &other);
                if (!target) {
                    diag_error(q->name_at, "unknown %s '" STR_FMT "'", kind, STR_ARG(q->text));
                    suggestion s = suggest_start(q->text);
                    for (int j = 0; j < c->prog->decls.count; j++) {
                        const decl *candidate = c->prog->decls.items[j];
                        if (candidate->kind == DECL_SYSTEM && candidate->is_view == d->is_view && !candidate->is_main) {
                            suggest_consider(&s, candidate->name);
                        }
                    }
                    suggest_note(&s);
                    continue;
                }
                if (other) {
                    diag_error(q->name_at, "'" STR_FMT "' is ambiguous: both " STR_FMT " and " STR_FMT " have it",
                               STR_ARG(q->text), STR_ARG(decl_ns(target)), STR_ARG(decl_ns(other)));
                    diag_note("write which one you mean, like '" STR_FMT "'", STR_ARG(target->qualified));
                    continue;
                }
                q->decl = target;
                if (target == d) {
                    diag_error(q->name_at, "a %s can't run %s itself", kind, before ? "before" : "after");
                } else if (target->is_main) {
                    diag_error(q->name_at, "Main runs once when the world is created, before every system");
                } else if (target->is_view != d->is_view) {
                    diag_error(q->name_at, "systems and views are ordered separately: views draw once per frame, "
                                           "after the ticks");
                } else if (before) {
                    vec_push(target->after, d);
                } else {
                    vec_push(d->after, target);
                }
            }
        }
    }
}

static bool placed_all(const decl *d, const bool *placed, const decl *const *list, const int n)
{
    for (int i = 0; i < d->after.count; i++) {
        for (int k = 0; k < n; k++) {
            if (list[k] == d->after.items[i] && !placed[k]) return false;
        }
    }
    return true;
}

// Orders systems (or views): each after the ones it must follow, and otherwise
// in the order they're written, file by file with files sorted by path. The
// order is part of the program, the same on every platform.
static void schedule(decl **list, const int n)
{
    if (n == 0) return;
    bool *placed = arena_alloc(sizeof(bool) * (size_t)n);
    decl **order = arena_alloc(sizeof(decl *) * (size_t)n);
    int count = 0;
    while (count < n) {
        int next = -1;
        for (int i = 0; i < n && next < 0; i++) {
            if (!placed[i] && placed_all(list[i], placed, (const decl *const *)list, n)) next = i;
        }
        if (next < 0) break; // A cycle
        placed[next] = true;
        order[count++] = list[next];
    }

    if (count < n) {
        // Follow "must run after" links among the unplaced until one repeats.
        const decl *path[64];
        int length = 0;
        const decl *d = NULL;
        for (int i = 0; i < n && !d; i++) {
            if (!placed[i]) d = list[i];
        }
        for (;;) {
            int seen = -1;
            for (int i = 0; i < length; i++) {
                if (path[i] == d) seen = i;
            }
            if (seen >= 0 || length == 64) {
                // Each system in the path runs after the next one.
                sb chain = {0};
                for (int i = seen >= 0 ? seen : 0; i < length; i++) {
                    sb_printf(&chain, STR_FMT " runs after ", STR_ARG(path[i]->qualified));
                }
                sb_printf(&chain, STR_FMT, STR_ARG(d->qualified));
                diag_error(d->at, "these %s must each run after the next, which can't happen: %s",
                           d->is_view ? "views" : "systems", chain.data);
                diag_note("remove one of the Before or After attributes that form the loop");
                break;
            }
            path[length++] = d;
            const decl *next = NULL;
            for (int i = 0; i < d->after.count && !next; i++) {
                for (int k = 0; k < n; k++) {
                    if (list[k] == d->after.items[i] && !placed[k]) next = list[k];
                }
            }
            d = next;
        }
        return;
    }
    for (int i = 0; i < n; i++) {
        list[i] = order[i];
        list[i]->index = i;
    }
}

bool check(program *prog)
{
    checker c = {0};
    c.prog = prog;

    add_builtins(prog);
    add_device_records(prog);
    collect_decls(prog);
    check_units(prog);

    for (int i = 0; i < prog->decls.count; i++) {
        const decl *d = prog->decls.items[i];
        c.unit = d->unit;
        if (d->kind != DECL_SYSTEM) resolve_field_types(&c, d);
    }
    for (int i = 0; i < prog->decls.count; i++) {
        if (prog->decls.items[i]->kind == DECL_STRUCT) order_struct(prog, prog->decls.items[i]);
    }
    for (int i = 0; i < prog->decls.count; i++) {
        const decl *d = prog->decls.items[i];
        c.unit = d->unit;
        if (d->kind != DECL_SYSTEM) check_fields(&c, d);
    }
    // Every signature first, so bodies can call any method or function.
    for (int i = 0; i < prog->decls.count; i++) {
        decl *d = prog->decls.items[i];
        c.unit = d->unit;
        if (d->kind == DECL_FUNCTION) check_function_decl(&c, d);
        else if (d->kind != DECL_SYSTEM) check_method_decls(&c, d);
    }
    for (int i = 0; i < prog->decls.count; i++) {
        decl *d = prog->decls.items[i];
        if (d->kind == DECL_FUNCTION) check_method_body(&c, d);
        for (int k = 0; k < d->methods.count; k++) check_method_body(&c, d->methods.items[k]);
    }
    if (prog->input) {
        c.unit = prog->input->unit;
        check_sample(&c, prog->input);
        check_sanitize(&c, prog->input);
    }

    for (int i = 0; i < prog->decls.count; i++) {
        decl *d = prog->decls.items[i];
        if (d->kind != DECL_SYSTEM) continue;
        c.unit = d->unit;
        check_params(&c, d);
        c.system = d;
        const int errors = diag_error_count();
        check_stmt(&c, d->body);
        if (diag_error_count() == errors) warn_unused_params(&c, d); // Errors hide uses
    }

    check_attributes(&c);
    if (diag_error_count() == 0) {
        schedule(prog->systems.items, prog->systems.count);
        schedule(prog->views.items, prog->views.count);
    }

    if (!prog->main) {
        const decl *misspelled = NULL;
        for (int i = 0; i < prog->systems.count; i++) {
            const str name = prog->systems.items[i]->name;
            if (name.len == 4 && (name.ptr[0] == 'm' || name.ptr[0] == 'M') && memcmp(name.ptr + 1, "ain", 3) == 0) {
                misspelled = prog->systems.items[i];
            }
        }
        if (misspelled) {
            diag_error(misspelled->at, "the program has no entry point");
            diag_note("the entry point is spelled 'Main', with a capital M");
        } else {
            diag_error((loc){1, 1, 0}, "the program has no entry point");
            diag_note("add 'system Main() { ... }' to create the starting entities");
        }
    }

    if (diag_error_count() > 0) return false;

    derive_archetypes(&c);
    if (diag_error_count() > 0) return false;

    warn_unmatched(prog, (const decl *const *)prog->systems.items, prog->systems.count);
    warn_unmatched(prog, (const decl *const *)prog->views.items, prog->views.count);
    analyze_parallelism(prog);
    return true;
}
