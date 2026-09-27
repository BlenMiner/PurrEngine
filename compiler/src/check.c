#include <stdio.h>
#include <string.h>

#include "ast.h"

// Semantic analysis: resolves names and types, enforces access and mutability
// rules, and derives the archetype list from spawn, add and remove sites.

#define MAX_COMPONENTS 64
#define MAX_ARCHETYPES 256

typedef struct checker {
    program *prog;
    decl *system;             // System whose body is being checked.
    VEC(stmt *) locals;       // S_VAR statements currently in scope.
    VEC(int) scope_marks;
    VEC(expr *) spawns;       // Spawn calls, patched with archetype indices at the end.
} checker;

static const type T_ERR = {TY_ERROR, NULL};
static const type T_VOID_ = {TY_VOID, NULL};
static const type T_BOOL_ = {TY_BOOL, NULL};
static const type T_INT_ = {TY_INT, NULL};
static const type T_FLOAT_ = {TY_FLOAT, NULL};
static const type T_FLOAT3_ = {TY_FLOAT3, NULL};
static const type T_ENTITY_ = {TY_ENTITY, NULL};

static const char *type_str(const type t)
{
    static char buf[4][128];
    static int next;
    switch (t.kind) {
    case TY_ERROR: return "<error>";
    case TY_VOID: return "nothing";
    case TY_BOOL: return "bool";
    case TY_INT: return "int";
    case TY_FLOAT: return "float";
    case TY_FLOAT3: return "float3";
    case TY_ENTITY: return "Entity";
    case TY_COMPONENT:
    case TY_SINGLETON: {
        char *b = buf[next++ % 4];
        snprintf(b, sizeof buf[0], STR_FMT, STR_ARG(t.decl->name));
        return b;
    }
    }
    return "?";
}

static bool is_numeric(const type t)
{
    return t.kind == TY_INT || t.kind == TY_FLOAT;
}

static bool same_type(const type a, const type b)
{
    return a.kind == b.kind && a.decl == b.decl;
}

// Can a value of type `from` be stored where `to` is expected?
static bool assignable(const type to, const type from)
{
    if (to.kind == TY_ERROR || from.kind == TY_ERROR) return true;
    if (same_type(to, from)) return true;
    return to.kind == TY_FLOAT && from.kind == TY_INT;
}

static decl *find_type_decl(const program *prog, const str name)
{
    for (int i = 0; i < prog->decls.count; i++) {
        decl *d = prog->decls.items[i];
        if (d->kind != DECL_SYSTEM && str_eq(d->name, name)) return d;
    }
    return NULL;
}

// Built-in value types, usable for fields and locals.
static bool builtin_type(const str name, type *out)
{
    if (str_eq_c(name, "bool")) *out = T_BOOL_;
    else if (str_eq_c(name, "int")) *out = T_INT_;
    else if (str_eq_c(name, "float")) *out = T_FLOAT_;
    else if (str_eq_c(name, "float3")) *out = T_FLOAT3_;
    else if (str_eq_c(name, "Entity")) *out = T_ENTITY_;
    else return false;
    return true;
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
        if (d) diag_error(e->at, "'" STR_FMT "' is a singleton; only components have values like this", STR_ARG(e->name));
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
        if (!assignable(init->field->type, value)) {
            diag_error(init->value->at, "field '" STR_FMT "' is %s, not %s", STR_ARG(init->name),
                       type_str(init->field->type), type_str(value));
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

static type check_call(checker *c, expr *e)
{
    if (str_eq_c(e->name, "float3")) {
        e->call = CALL_FLOAT3;
        if (e->args.count != 3) {
            diag_error(e->at, "float3 takes 3 numbers (x, y, z), not %d", e->args.count);
        }
        for (int i = 0; i < e->args.count; i++) {
            const type t = check_expr(c, e->args.items[i]);
            if (t.kind != TY_ERROR && !is_numeric(t)) {
                diag_error(e->args.items[i]->at, "float3 takes numbers, not %s", type_str(t));
            }
        }
        return T_FLOAT3_;
    }

    if (str_eq_c(e->name, "Spawn")) {
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
    } else if (str_eq_c(e->name, "int") || str_eq_c(e->name, "float") || str_eq_c(e->name, "bool")) {
        diag_error(e->at, "type conversions aren't supported yet");
    } else {
        diag_error(e->at, "unknown function '" STR_FMT "'", STR_ARG(e->name));
    }
    return T_ERR;
}

static type check_method(checker *c, expr *e)
{
    const type obj = check_expr(c, e->object);
    if (obj.kind == TY_ERROR) {
        for (int i = 0; i < e->args.count; i++) check_expr(c, e->args.items[i]);
        return T_ERR;
    }
    if (obj.kind != TY_ENTITY) {
        diag_error(e->at, "%s has no method '" STR_FMT "'", type_str(obj), STR_ARG(e->name));
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
        if (l.kind == TY_INT && r.kind == TY_INT) return T_INT_;
        if (is_numeric(l) && is_numeric(r)) return T_FLOAT_;
        if (l.kind == TY_FLOAT3 && r.kind == TY_FLOAT3) return T_FLOAT3_;
        if ((op == T_STAR || op == T_SLASH) && l.kind == TY_FLOAT3 && is_numeric(r)) return T_FLOAT3_;
        if (op == T_STAR && is_numeric(l) && r.kind == TY_FLOAT3) return T_FLOAT3_;
        break;
    case T_PERCENT:
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
        if (is_numeric(l) && is_numeric(r)) return T_BOOL_;
        break;
    case T_EQ:
    case T_NE:
        if (is_numeric(l) && is_numeric(r)) return T_BOOL_;
        if (l.kind == TY_BOOL && r.kind == TY_BOOL) return T_BOOL_;
        if (l.kind == TY_ENTITY && r.kind == TY_ENTITY) return T_BOOL_;
        break;
    case T_AND:
    case T_OR:
        if (l.kind == TY_BOOL && r.kind == TY_BOOL) return T_BOOL_;
        break;
    default:
        break;
    }
    diag_error(at, "operator %s can't be used with %s and %s", op_str(op), type_str(l), type_str(r));
    return T_ERR;
}

static type check_member(checker *c, expr *e)
{
    const type obj = check_expr(c, e->object);
    switch (obj.kind) {
    case TY_ERROR:
        return T_ERR;
    case TY_FLOAT3:
        if (e->member.len == 1 && (e->member.ptr[0] == 'x' || e->member.ptr[0] == 'y' || e->member.ptr[0] == 'z')) {
            e->swizzle = e->member.ptr[0] - 'x';
            return T_FLOAT_;
        }
        diag_error(e->at, "float3 has members x, y and z, not '" STR_FMT "'", STR_ARG(e->member));
        return T_ERR;
    case TY_COMPONENT:
    case TY_SINGLETON:
        for (int i = 0; i < obj.decl->fields.count; i++) {
            field *f = &obj.decl->fields.items[i];
            if (str_eq(f->name, e->member)) {
                e->field = f;
                return f->type;
            }
        }
        diag_error(e->at, "%s has no field '" STR_FMT "'", type_str(obj), STR_ARG(e->member));
        return T_ERR;
    default:
        diag_error(e->at, "%s has no members", type_str(obj));
        return T_ERR;
    }
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
        const type l = check_expr(c, e->lhs);
        const type r = check_expr(c, e->rhs);
        t = binary_result(e->op, l, r, e->at);
        break;
    }
    case E_UNARY: {
        const type operand = check_expr(c, e->lhs);
        if (operand.kind == TY_ERROR) break;
        if (e->op == T_NOT && operand.kind == TY_BOOL) t = T_BOOL_;
        else if (e->op == T_TILDE && operand.kind == TY_INT) t = T_INT_;
        else if (e->op == T_MINUS && (is_numeric(operand) || operand.kind == TY_FLOAT3)) t = operand;
        else diag_error(e->at, "operator %s can't be used with %s", op_str(e->op), type_str(operand));
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

    if (root->bind == BIND_PARAM && root->param->mode != PARAM_MUT) {
        diag_error(root->at, "'" STR_FMT "' is read-only", STR_ARG(root->name));
        if (root->param->type.kind == TY_ENTITY) {
            diag_note("entity handles can't be reassigned");
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
    if (!assignable(target, result)) {
        diag_error(s->value->at, "can't assign %s to %s", type_str(result), type_str(target));
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
        if (builtin_type(s->type_name, &s->type)) {
        } else if (d) {
            s->type = (type){d->kind == DECL_COMPONENT ? TY_COMPONENT : TY_SINGLETON, d};
        } else {
            diag_error(s->at, "unknown type '" STR_FMT "'", STR_ARG(s->type_name));
            s->type = T_ERR;
        }
        if (!assignable(s->type, value)) {
            diag_error(s->value->at, "can't initialize %s with %s", type_str(s->type), type_str(value));
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
            diag_error(s->cond->at, "condition must be bool, not %s", type_str(cond));
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
    case S_EXPR:
        check_expr(c, s->value);
        if (s->value->kind != E_METHOD && !(s->value->kind == E_CALL && s->value->call == CALL_SPAWN)) {
            if (s->value->type.kind != TY_ERROR) diag_error(s->value->at, "this expression does nothing on its own");
        }
        break;
    }
}

// ---------------------------------------------------------------------------
// Declarations

// Constant expressions: literals, float3(...) of constants, and operators on
// constants. No names, so a default never depends on other state.
static bool is_constant(const expr *e)
{
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
        if (!str_eq_c(e->name, "float3")) return false;
        for (int i = 0; i < e->args.count; i++) {
            if (!is_constant(e->args.items[i])) return false;
        }
        return true;
    default:
        return false;
    }
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
        diag_note("use literals, float3(...) and operators; defaults can't read fields, singletons or Time");
        return;
    }
    const type t = check_expr(c, value);
    if (f->type.kind != TY_ERROR && !assignable(f->type, t)) {
        diag_error(value->at, "field '" STR_FMT "' is %s, not %s", STR_ARG(f->name), type_str(f->type), type_str(t));
    }
}

static void check_fields(checker *c, const decl *d)
{
    for (int i = 0; i < d->fields.count; i++) {
        field *f = &d->fields.items[i];
        check_reserved(f->name, f->at);
        if (!builtin_type(f->type_name, &f->type)) {
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

        if (!d) {
            diag_error(p->at, "unknown component or singleton '" STR_FMT "'", STR_ARG(p->type_name));
            p->type = T_ERR;
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

    VEC(decl *) decls = {0};
    vec_push(decls, time);
    for (int i = 0; i < prog->decls.count; i++) vec_push(decls, prog->decls.items[i]);
    prog->decls.items = decls.items;
    prog->decls.count = decls.count;
    prog->decls.cap = decls.cap;
}

static void collect_decls(program *prog)
{
    for (int i = 0; i < prog->decls.count; i++) {
        decl *d = prog->decls.items[i];
        type dummy;

        if (!d->builtin) {
            check_reserved(d->name, d->at);
            if (builtin_type(d->name, &dummy)) {
                diag_error(d->at, "'" STR_FMT "' is a built-in type", STR_ARG(d->name));
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

bool check(program *prog)
{
    checker c = {0};
    c.prog = prog;

    add_builtins(prog);
    collect_decls(prog);

    for (int i = 0; i < prog->decls.count; i++) {
        const decl *d = prog->decls.items[i];
        if (d->kind != DECL_SYSTEM) check_fields(&c, d);
    }

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
