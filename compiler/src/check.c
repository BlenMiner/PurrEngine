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
    decl *system;             // Whose parameters are in scope: a system, or the input when checking its constructor.
    bool in_constructor;      // Checking the input's constructor, which runs outside the simulation.
    int short_circuit_depth;  // Inside the right side of && or ||, which may not run.
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

// The type a declaration names: a component, singleton, input or record.
static type decl_type(decl *d)
{
    switch (d->kind) {
    case DECL_COMPONENT: return (type){TY_COMPONENT, d};
    case DECL_SINGLETON: return (type){TY_SINGLETON, d};
    case DECL_INPUT: return (type){TY_INPUT, d};
    case DECL_RECORD: return (type){TY_RECORD, d};
    default: return T_ERR;
    }
}

// Does this type have fields (components, singletons, inputs, records)?
static bool has_fields(const type t)
{
    return t.kind == TY_COMPONENT || t.kind == TY_SINGLETON || t.kind == TY_INPUT || t.kind == TY_RECORD;
}

// int or float, not a vector.
static bool is_scalar_number(const type t)
{
    return t.kind == TY_INT || t.kind == TY_FLOAT;
}

static decl *find_type_decl(const program *prog, const str name)
{
    for (int i = 0; i < prog->decls.count; i++) {
        decl *d = prog->decls.items[i];
        if (d->kind != DECL_SYSTEM && str_eq(d->name, name)) return d;
    }
    return NULL;
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

static param *find_param(const checker *c, const str name)
{
    if (!c->system) return NULL; // Field defaults are checked outside any system.
    for (int i = 0; i < c->system->params.count; i++) {
        param *p = &c->system->params.items[i];
        if (p->name.len > 0 && str_eq(p->name, name)) return p;
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// Expressions

static type check_expr(checker *c, expr *e);

static uint64_t bit(const decl *component)
{
    return (uint64_t)1 << component->index;
}

// Checks `Transform { position = ... }`.
static type check_literal(checker *c, expr *e)
{
    decl *d = find_type_decl(c->prog, e->name);
    if (!d || d->kind != DECL_COMPONENT) {
        if (d) diag_error(e->at, "'" STR_FMT "' isn't a component; only components have values like this", STR_ARG(e->name));
        else diag_error(e->at, "unknown component '" STR_FMT "'", STR_ARG(e->name));
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
            diag_error(init->at, "component '" STR_FMT "' has no field '" STR_FMT "'", STR_ARG(d->name), STR_ARG(init->name));
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
    return (type){TY_COMPONENT, d};
}

// A component argument to Spawn or Add: `Player` (defaults) or `Player { ... }`.
// Returns the component, or NULL after reporting an error.
static decl *check_component_arg(checker *c, expr *arg, const char *fn)
{
    if (arg->kind == E_LITERAL) {
        const type t = check_literal(c, arg);
        return t.kind == TY_COMPONENT ? t.decl : NULL;
    }
    if (arg->kind == E_NAME) {
        decl *d = find_type_decl(c->prog, arg->name);
        if (d && d->kind == DECL_COMPONENT && !find_local(c, arg->name) && !find_param(c, arg->name)) {
            arg->bind = BIND_TYPE;
            arg->type_decl = d;
            arg->type = (type){TY_COMPONENT, d};
            return d;
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

static type check_call(checker *c, expr *e)
{
    type builtin;
    if (builtin_type_named(e->name, &builtin)) return check_construct(c, e, builtin);

    if (str_eq_c(e->name, "Spawn")) {
        if (c->in_constructor) {
            diag_error(e->at, "the input's constructor runs outside the simulation, so it can't spawn entities");
            return T_ERR;
        }
        // Expressions evaluate left to right, so codegen runs a statement's spawns
        // first, in order. On the right of && or || that would spawn even when the
        // right side is skipped.
        if (c->short_circuit_depth > 0) {
            diag_error(e->at, "Spawn can't be on the right side of && or ||");
            diag_note("that side only runs sometimes; spawn into a local before the condition");
        }
        e->call = CALL_SPAWN;
        e->spawn_mask = check_component_list(c, e, "Spawn");
        c->prog->spawned_mask |= e->spawn_mask;
        vec_push(c->spawns, e);
        return T_ENTITY_;
    }

    for (int i = 0; i < e->args.count; i++) check_expr(c, e->args.items[i]);

    const decl *d = find_type_decl(c->prog, e->name);
    if (d && d->kind == DECL_COMPONENT) {
        diag_error(e->at, "write '" STR_FMT " { ... }' to make a component value", STR_ARG(e->name));
    } else {
        diag_error(e->at, "unknown function '" STR_FMT "'", STR_ARG(e->name));
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
    // Math.Dot(a, b), quaternion.AxisAngle(axis, angle)
    if (names_builtin_owner(c, e->object)) {
        for (int i = 0; i < e->args.count; i++) check_expr(c, e->args.items[i]);
        return resolve_builtin_call(e->object->name, e);
    }

    const type obj = check_expr(c, e->object);
    if (obj.kind == TY_ERROR) {
        for (int i = 0; i < e->args.count; i++) check_expr(c, e->args.items[i]);
        return T_ERR;
    }
    if (obj.kind != TY_ENTITY) {
        diag_error(e->at, "%s has no method '" STR_FMT "'", type_name(obj), STR_ARG(e->name));
        return T_ERR;
    }
    if (c->in_constructor) {
        diag_error(e->at, "the input's constructor runs outside the simulation, so it can't change entities");
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

static type check_member(checker *c, expr *e)
{
    // quaternion.Identity, Math.PI
    if (names_builtin_owner(c, e->object)) return resolve_builtin_member(e->object->name, e);

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
    const int n = matrix_dim(obj);
    if (n > 0) {
        if (e->member.len == 2 && e->member.ptr[0] == 'c' && e->member.ptr[1] >= '0' && e->member.ptr[1] < '0' + n) {
            return vector_type(true, n);
        }
        diag_error(e->at, "%s has columns c0 to c%d, not '" STR_FMT "'", type_name(obj), n - 1, STR_ARG(e->member));
        return T_ERR;
    }

    // input.jump.pressed: a bool field of an input parameter, compared with last tick.
    if (obj.kind == TY_BOOL && (str_eq_c(e->member, "pressed") || str_eq_c(e->member, "released"))) {
        const expr *field_access = e->object;
        const bool on_input_field = field_access->kind == E_MEMBER && field_access->object->kind == E_NAME
                                 && field_access->object->bind == BIND_PARAM
                                 && field_access->object->param->type.kind == TY_INPUT;
        if (!on_input_field) {
            diag_error(e->at, "only input fields have '." STR_FMT "', like 'input.jump." STR_FMT "'",
                       STR_ARG(e->member), STR_ARG(e->member));
            if (c->in_constructor) diag_note("in the constructor, read the device instead, like 'keys.space.pressed'");
            return T_ERR;
        }
        e->edge = str_eq_c(e->member, "pressed") ? EDGE_PRESSED : EDGE_RELEASED;
        return T_BOOL_;
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
        return p->type;
    }
    // Inside the input's constructor, its fields are in scope by name.
    if (c->in_constructor) {
        for (int i = 0; i < c->system->fields.count; i++) {
            field *f = &c->system->fields.items[i];
            if (str_eq(f->name, e->name)) {
                e->bind = BIND_FIELD;
                e->field = f;
                return f->type;
            }
        }
    }
    const decl *d = find_type_decl(c->prog, e->name);
    if (d) {
        diag_error(e->at, "'" STR_FMT "' is a type, not a value", STR_ARG(e->name));
    } else {
        diag_error(e->at, "unknown name '" STR_FMT "'", STR_ARG(e->name));
    }
    return T_ERR;
}

static type check_expr(checker *c, expr *e)
{
    type t = T_ERR;
    switch (e->kind) {
    case E_INT: t = T_INT_; break;
    case E_FLOAT: t = T_FLOAT_; break;
    case E_BOOL: t = T_BOOL_; break;
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

static void check_assign(checker *c, const stmt *s)
{
    const type target = check_expr(c, s->target);
    const type value = check_expr(c, s->value);
    if (target.kind == TY_ERROR) return;

    expr *root = assign_root(s->target);
    if (!root || root->bind == BIND_NONE || root->bind == BIND_TYPE) {
        diag_error(s->target->at, "can't assign to this expression");
        return;
    }

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

    if (root->bind == BIND_PARAM && root->param->mode != PARAM_MUT) {
        diag_error(root->at, "'" STR_FMT "' is read-only", STR_ARG(root->name));
        if (root->param->type.kind == TY_ENTITY) {
            diag_note("entity handles can't be reassigned");
        } else if (root->param->type.kind == TY_INPUT) {
            diag_note("input comes from the players; the simulation can only read it");
        } else if (root->param->type.kind == TY_RECORD) {
            diag_note("devices can only be read");
        } else if (root->param->type.kind == TY_SINGLETON && root->param->type.decl->builtin) {
            diag_note("'" STR_FMT "' is managed by the engine", STR_ARG(root->param->type_name));
        } else {
            diag_note("declare the parameter as 'mut " STR_FMT " " STR_FMT "' to write to it",
                      STR_ARG(root->param->type_name), STR_ARG(root->name));
        }
        return;
    }
    if (root->bind == BIND_LOCAL && !root->local->is_mut) {
        const stmt *local = root->local;
        diag_error(root->at, "'" STR_FMT "' is read-only", STR_ARG(root->name));
        if (local->type_name.len > 0) {
            diag_note("declare it as 'mut " STR_FMT " " STR_FMT " = ...' to change it", STR_ARG(local->type_name), STR_ARG(local->name));
        } else {
            diag_note("declare it as 'mut var " STR_FMT " = ...' to change it", STR_ARG(local->name));
        }
        return;
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

static void check_var(checker *c, stmt *s)
{
    type value = check_expr(c, s->value);
    if (s->value->kind == E_NAME && s->value->bind == BIND_TYPE) value = T_ERR;

    if (s->type_name.len == 0) {
        if (value.kind == TY_VOID) {
            diag_error(s->value->at, "this expression doesn't produce a value");
            value = T_ERR;
        }
        s->type = value;
    } else {
        decl *d = find_type_decl(c->prog, s->type_name);
        if (builtin_type_named(s->type_name, &s->type)) {
        } else if (d) {
            s->type = decl_type(d);
        } else {
            diag_error(s->at, "unknown type '" STR_FMT "'", STR_ARG(s->type_name));
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
        const bool effect = (s->value->kind == E_METHOD && (call == CALL_ADD || call == CALL_REMOVE || call == CALL_DESTROY))
                         || (s->value->kind == E_CALL && call == CALL_SPAWN);
        if (!effect && s->value->type.kind != TY_ERROR) diag_error(s->value->at, "this expression does nothing on its own");
        break;
    }
    }
}

// ---------------------------------------------------------------------------
// Declarations

static bool all_constant(const expr *e);

// Constant expressions: literals, constructors of built-in types, Math
// functions, built-in constants like quaternion.Identity, members of any of
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
    case E_CALL:
        return builtin_type_named(e->name, &ignored) && all_constant(e);
    case E_METHOD:
        return e->object->kind == E_NAME && builtin_owner(e->object->name) && all_constant(e);
    case E_MEMBER:
        return (e->object->kind == E_NAME && builtin_owner(e->object->name)) || is_constant(e->object);
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
        diag_note("use literals, constructors like float3(...), Math functions and operators; "
                  "defaults can't read fields, singletons or Time");
        return;
    }
    const type t = check_expr(c, value);
    if (f->type.kind != TY_ERROR && !type_assignable(f->type, t)) {
        diag_error(value->at, "field '" STR_FMT "' is %s, not %s", STR_ARG(f->name), type_name(f->type), type_name(t));
    }
}

static void check_fields(checker *c, const decl *d)
{
    for (int i = 0; i < d->fields.count; i++) {
        field *f = &d->fields.items[i];
        check_reserved(f->name, f->at);
        if (!builtin_type_named(f->type_name, &f->type)) {
            if (find_type_decl(c->prog, f->type_name)) {
                diag_error(f->at, "fields can't hold components or singletons yet");
            } else {
                diag_error(f->at, "unknown type '" STR_FMT "'", STR_ARG(f->type_name));
            }
            f->type = T_ERR;
        }
        for (int j = 0; j < i; j++) {
            if (str_eq(d->fields.items[j].name, f->name)) {
                diag_error(f->at, "field '" STR_FMT "' is declared twice", STR_ARG(f->name));
            }
        }
        if (f->default_value) check_default(c, f);
    }
}

static void check_params(const program *prog, decl *sys)
{
    bool has_entity = false;
    bool has_input = false;
    uint64_t seen = 0;

    for (int i = 0; i < sys->params.count; i++) {
        param *p = &sys->params.items[i];
        decl *d = find_type_decl(prog, p->type_name);
        bool is_entity = str_eq_c(p->type_name, "Entity");

        if (p->name.len > 0) {
            check_reserved(p->name, p->at);
            for (int j = 0; j < i; j++) {
                if (str_eq(sys->params.items[j].name, p->name)) {
                    diag_error(p->at, "parameter '" STR_FMT "' is declared twice", STR_ARG(p->name));
                }
            }
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
            diag_error(p->at, "Devices can only be read in the input's constructor");
            diag_note("systems read the players' input instead, through an input parameter");
            p->type = T_ERR;
            continue;
        }

        if (!d) {
            diag_error(p->at, "unknown component or singleton '" STR_FMT "'", STR_ARG(p->type_name));
            p->type = T_ERR;
            continue;
        }

        // An input parameter gives the input of the player who owns the entity,
        // so the system only runs on entities with an Owner.
        if (d->kind == DECL_INPUT) {
            if (p->mode != PARAM_READ) {
                diag_error(p->at, "input can't be 'mut', 'with' or 'without'; the simulation can only read it");
            }
            if (has_input) diag_error(p->at, "a system can only have one input parameter");
            has_input = true;
            p->type = (type){TY_INPUT, d};
            sys->need_mask |= bit(prog->owner);
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
    sys->per_entity = has_entity || has_input || seen != 0;

    if (sys->is_main && sys->per_entity) {
        diag_error(sys->at, "Main runs once when the world is created, so it can only take singletons");
    }
}

static void add_builtins(program *prog)
{
    // singleton Time { float dt; int tick; }
    decl *time = NEW(decl);
    time->kind = DECL_SINGLETON;
    time->name = str_from("Time");
    time->builtin = true;
    const field dt = {str_from("dt"), str_from("float"), {0, 0}, {0}, NULL};
    const field tick = {str_from("tick"), str_from("int"), {0, 0}, {0}, NULL};
    vec_push(time->fields, dt);
    vec_push(time->fields, tick);

    // component Owner { PlayerID player; }: ties an entity to a player.
    decl *owner = NEW(decl);
    owner->kind = DECL_COMPONENT;
    owner->name = str_from("Owner");
    owner->builtin = true;
    const field player = {str_from("player"), str_from("PlayerID"), {0, 0}, {0}, NULL};
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
    const field f = {str_from(name), str_from(""), {0, 0}, t, NULL};
    vec_push(d->fields, f);
}

// The device records an input's constructor reads. Their members come from the
// X-macros in purr/devices.h, so PurrLang and the C structs always match.
static void add_device_records(program *prog)
{
    const type t_bool = {TY_BOOL, NULL};
    const type t_float = {TY_FLOAT, NULL};
    const type t_float2 = {TY_FLOAT2, NULL};

    decl *button = new_record(prog, "Button", "purr_button");
    record_field(button, "down", t_bool);
    record_field(button, "pressed", t_bool);
    record_field(button, "released", t_bool);
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

static void collect_decls(program *prog)
{
    for (int i = 0; i < prog->decls.count; i++) {
        decl *d = prog->decls.items[i];
        type dummy;

        if (!d->builtin) {
            check_reserved(d->name, d->at);
            if (builtin_type_named(d->name, &dummy) || str_eq_c(d->name, "Math") || str_eq_c(d->name, "Devices")) {
                diag_error(d->at, "'" STR_FMT "' is built into the language", STR_ARG(d->name));
            }
            for (int j = 0; j < i; j++) {
                const decl *other = prog->decls.items[j];
                bool both_types = d->kind != DECL_SYSTEM && other->kind != DECL_SYSTEM;
                bool both_systems = d->kind == DECL_SYSTEM && other->kind == DECL_SYSTEM;
                if ((both_types || both_systems) && str_eq(d->name, other->name)) {
                    if (other->builtin) {
                        diag_error(d->at, "'" STR_FMT "' is built into the engine", STR_ARG(d->name));
                    } else {
                        diag_error(d->at, "'" STR_FMT "' is already declared", STR_ARG(d->name));
                    }
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
            break;
        case DECL_SYSTEM:
            if (str_eq_c(d->name, "Main")) {
                d->is_main = true;
                if (prog->main) diag_error(d->at, "there can only be one 'system Main()'");
                else prog->main = d;
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
    diag_error((loc){1, 1}, "the program can create more than %d different component combinations", MAX_ARCHETYPES);
    diag_note("every Add and Remove can apply to any entity, so combinations multiply");
}

static void warn_unmatched_systems(const program *prog)
{
    for (int i = 0; i < prog->systems.count; i++) {
        const decl *sys = prog->systems.items[i];
        if (!sys->per_entity) continue;
        bool matched = false;
        for (int a = 0; a < prog->archetypes.count; a++) {
            const uint64_t mask = prog->archetypes.items[a];
            if ((mask & sys->need_mask) == sys->need_mask && !(mask & sys->without_mask)) matched = true;
        }
        if (!matched) {
            diag_warning(sys->at, "system '" STR_FMT "' never runs: no entity matches its parameters", STR_ARG(sys->name));
            if (sys->need_mask) {
                char names[512] = "";
                append_component_names(prog, sys->need_mask, names, sizeof names);
                diag_note("nothing spawns or adds an entity with %s", names);
            }
        }
    }
}

// input PlayerInput { ...; PlayerInput(Devices devices) { ... } }: the
// constructor runs on the client, outside the simulation. It reads devices and
// assigns the input's fields, which start at their defaults.
static void check_constructor(checker *c, decl *input)
{
    if (!input->body) return; // Without a constructor, sampling gives the defaults.

    if (input->params.count != 1 || !str_eq_c(input->params.items[0].type_name, "Devices")) {
        diag_error(input->body_at, "the input's constructor takes the devices: '" STR_FMT "(Devices devices)'",
                   STR_ARG(input->name));
    }
    for (int i = 0; i < input->params.count; i++) {
        param *p = &input->params.items[i];
        check_reserved(p->name, p->at);
        p->mode = PARAM_READ;
        p->type = str_eq_c(p->type_name, "Devices") ? (type){TY_RECORD, c->prog->devices} : T_ERR;
    }

    c->system = input;
    c->in_constructor = true;
    check_stmt(c, input->body);
    c->in_constructor = false;
    c->system = NULL;
}

bool check(program *prog)
{
    checker c = {0};
    c.prog = prog;

    add_builtins(prog);
    add_device_records(prog);
    collect_decls(prog);

    for (int i = 0; i < prog->decls.count; i++) {
        const decl *d = prog->decls.items[i];
        if (d->kind != DECL_SYSTEM) check_fields(&c, d);
    }
    if (prog->input) check_constructor(&c, prog->input);

    for (int i = 0; i < prog->decls.count; i++) {
        decl *d = prog->decls.items[i];
        if (d->kind != DECL_SYSTEM) continue;
        check_params(prog, d);
        c.system = d;
        check_stmt(&c, d->body);
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
            diag_error((loc){1, 1}, "the program has no entry point");
            diag_note("add 'system Main() { ... }' to create the starting entities");
        }
    }

    if (diag_error_count() > 0) return false;

    derive_archetypes(&c);
    if (diag_error_count() > 0) return false;

    warn_unmatched_systems(prog);
    return true;
}
