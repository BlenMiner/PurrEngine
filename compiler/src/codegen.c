#include "codegen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "types.h"

// Emits C for a checked program. Everything specific to the game is generated
// here: component structs, archetype storage, deferred structural changes,
// system bodies, dispatch loops and the tick. See docs/purrlang.md.

typedef struct transition {
    int from;
    int to;
} transition;

typedef struct gen {
    program *prog;
    const codegen_options *opts;
    sb h;
    sb c;
    const char *purr_path; // For #line, with forward slashes.
    const char *c_path;
    VEC(transition) moves;
    int indent;
    bool in_constructor; // Generating the input's constructor: fields live in purr_self.
    int spawn_temps;     // Temporaries for hoisted spawns, numbered per function.
} gen;

// ---------------------------------------------------------------------------
// C names

// Identifiers that would clash with C keywords, macros or library names in
// generated code. User names that hit one of these get a trailing underscore.
static const char *c_reserved[] = {
    "auto", "break", "case", "char", "const", "continue", "default", "do", "double", "else", "enum",
    "extern", "float", "for", "goto", "if", "inline", "int", "long", "register", "restrict", "return",
    "short", "signed", "sizeof", "static", "struct", "switch", "typedef", "union", "unsigned", "void",
    "volatile", "while", "alignas", "alignof", "bool", "constexpr", "false", "nullptr", "static_assert",
    "thread_local", "true", "typeof", "typeof_unqual", "NULL", "EOF", "FILE", "errno", "assert",
    "offsetof", "stdin", "stdout", "stderr", "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t",
    "uint16_t", "uint32_t", "uint64_t", "intptr_t", "uintptr_t", "size_t", "ptrdiff_t", "wchar_t",
    "abort", "memset", "printf", "fprintf", "main",
};

static bool is_c_reserved(const str name)
{
    if (name.len > 0 && name.ptr[0] == '_') return true;
    for (size_t i = 0; i < sizeof c_reserved / sizeof c_reserved[0]; i++) {
        if (str_eq_c(name, c_reserved[i])) return true;
    }
    return false;
}

static const char *mangle(const str name)
{
    char *out = arena_alloc((size_t)name.len + 2);
    memcpy(out, name.ptr, (size_t)name.len);
    if (is_c_reserved(name)) out[name.len] = '_';
    return out;
}

static const char *type_cname(const decl *d)
{
    return mangle(d->name);
}

static const char *field_cname(const field *f)
{
    return mangle(f->name);
}

// Locals and parameters also can't reuse a type's name: in C that would hide
// the typedef inside the function.
static const char *local_cname(const gen *g, const str name)
{
    bool clash = is_c_reserved(name) || str_eq_c(name, "purr_float3");
    for (int i = 0; i < g->prog->decls.count && !clash; i++) {
        const decl *d = g->prog->decls.items[i];
        if (d->kind != DECL_SYSTEM && str_eq(d->name, name)) clash = true;
    }
    char *out = arena_alloc((size_t)name.len + 2);
    memcpy(out, name.ptr, (size_t)name.len);
    if (clash) out[name.len] = '_';
    return out;
}

static const char *c_type(const type t)
{
    if (t.kind == TY_COMPONENT || t.kind == TY_SINGLETON || t.kind == TY_INPUT) return type_cname(t.decl);
    if (t.kind == TY_RECORD) return t.decl->c_name;
    return type_c_name(t);
}

static const char *const xyzw[] = {"x", "y", "z", "w"};

static bool has_component(const uint64_t mask, const int component)
{
    return (mask >> component) & 1;
}

// "arch0_Transform_Player": the world member and, prefixed with purr_, the type.
static const char *arch_name(const gen *g, const int index)
{
    const uint64_t mask = g->prog->archetypes.items[index];
    sb b = {0};
    sb_printf(&b, "arch%d", index);
    if (mask == 0) sb_put(&b, "_empty");
    for (int i = 0; i < g->prog->components.count; i++) {
        if (has_component(mask, i)) sb_printf(&b, "_%s", type_cname(g->prog->components.items[i]));
    }
    return b.data;
}

// "Transform, Player" for comments and error messages.
static const char *arch_label(const gen *g, const int index)
{
    const uint64_t mask = g->prog->archetypes.items[index];
    sb b = {0};
    sb_put(&b, "");
    bool first = true;
    for (int i = 0; i < g->prog->components.count; i++) {
        if (!has_component(mask, i)) continue;
        sb_printf(&b, "%s" STR_FMT, first ? "" : ", ", STR_ARG(g->prog->components.items[i]->name));
        first = false;
    }
    if (first) sb_put(&b, "no components");
    return b.data;
}

// Components, singletons, input and devices are passed by pointer.
static bool is_pointer_param(const param *p)
{
    const type_kind k = p->type.kind;
    return k == TY_COMPONENT || k == TY_SINGLETON || k == TY_INPUT || k == TY_RECORD;
}

// The hidden parameter holding last tick's input, for .pressed and .released.
static const char *prev_input_name(const gen *g, const str name)
{
    sb b = {0};
    sb_printf(&b, "purr_prev_%s", local_cname(g, name));
    return b.data;
}

// ---------------------------------------------------------------------------
// Output helpers

static void line(const gen *g, sb *o, const char *fmt, ...)
{
    for (int i = 0; i < g->indent; i++) sb_put(o, "    ");
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);
    sb_put(o, buf);
    sb_put(o, "\n");
}

static void indent(const gen *g, sb *o)
{
    for (int i = 0; i < g->indent; i++) sb_put(o, "    ");
}

static void line_directive(gen *g, const loc at)
{
    if (g->opts->line_directives) sb_printf(&g->c, "#line %d \"%s\"\n", at.line, g->purr_path);
}

// Points the following lines back at the generated file itself.
static void line_reset(gen *g)
{
    if (g->opts->line_directives) sb_printf(&g->c, "#line %d \"%s\"\n", g->c.line + 1, g->c_path);
}

// ---------------------------------------------------------------------------
// Expressions

static void gen_expr(gen *g, sb *o, const expr *e);

// Generates `e` into a fresh string. PurrLang expressions have no side effects,
// so an expression that's needed several times (a vector's components) can
// simply be repeated.
static const char *expr_text(gen *g, const expr *e)
{
    sb b = {0};
    gen_expr(g, &b, e);
    return b.data ? b.data : "";
}

// `e` converted to `want`: int -> float, scalars splatted to vectors, int
// vectors widened to float vectors.
static void gen_as(gen *g, sb *o, expr *e, const type want)
{
    const type have = e->type;
    if (want.kind == TY_FLOAT && have.kind == TY_INT) {
        if (e->kind == E_INT) {
            // Same rounding as a runtime conversion, but reads as a plain literal.
            sb_printf(o, "%lld.0f", (long long)e->int_value);
        } else if (e->kind == E_UNARY && e->op == T_MINUS && e->lhs->kind == E_INT && e->lhs->int_value >= 0) {
            sb_printf(o, "-%lld.0f", (long long)e->lhs->int_value);
        } else {
            sb_put(o, "(float)(");
            gen_expr(g, o, e);
            sb_put(o, ")");
        }
        return;
    }
    const int dim = type_dim(want);
    if (dim >= 2 && type_dim(have) == 1) {
        sb_printf(o, "purr_%s_splat(", type_suffix(want));
        gen_as(g, o, e, vector_type(type_is_float_based(want), 1));
        sb_put(o, ")");
        return;
    }
    if (dim >= 2 && type_dim(have) == dim && type_is_float_based(want) && type_is_int_based(have)) {
        sb_printf(o, "purr_%s_from_%s(", type_suffix(want), type_suffix(have));
        gen_expr(g, o, e);
        sb_put(o, ")");
        return;
    }
    gen_expr(g, o, e);
}

// "trs->" for component and singleton parameters, which are pointers; "<expr>." otherwise.
static const char *object_access(gen *g, const expr *obj)
{
    sb b = {0};
    if (obj->kind == E_NAME && obj->bind == BIND_PARAM && is_pointer_param(obj->param)) {
        sb_printf(&b, "%s->", local_cname(g, obj->name));
    } else {
        gen_expr(g, &b, obj);
        sb_put(&b, ".");
    }
    return b.data;
}

// Float literals: always single precision, always with a decimal point.
static void gen_float_literal(sb *o, str text)
{
    if (text.len > 0 && (text.ptr[text.len - 1] == 'f' || text.ptr[text.len - 1] == 'F')) text.len--;
    bool has_point = false;
    for (int i = 0; i < text.len; i++) {
        if (text.ptr[i] == '_') continue; // C has no digit separators.
        sb_putn(o, &text.ptr[i], 1);
        if (text.ptr[i] == '.' || text.ptr[i] == 'e' || text.ptr[i] == 'E') has_point = true;
    }
    sb_put(o, has_point ? "f" : ".0f");
}

static bool has_defaults(const decl *d)
{
    for (int i = 0; i < d->fields.count; i++) {
        if (d->fields.items[i].default_value) return true;
    }
    return false;
}

// A component or singleton value: fields set in `inits` (may be NULL) take that
// value, the rest take their declared default, and anything else is zero.
static void gen_value(gen *g, sb *o, const decl *d, const field_init *inits, const int init_count)
{
    sb_printf(o, "(%s){", type_cname(d));
    int written = 0;
    for (int i = 0; i < d->fields.count; i++) {
        const field *f = &d->fields.items[i];
        expr *value = f->default_value;
        for (int j = 0; j < init_count; j++) {
            if (inits[j].field == f) value = inits[j].value;
        }
        if (!value) continue;
        sb_printf(o, "%s.%s = ", written++ ? ", " : "", field_cname(f));
        gen_as(g, o, value, f->type);
    }
    if (written == 0) sb_put(o, "0");
    sb_put(o, "}");
}

static void gen_literal(gen *g, sb *o, const expr *e)
{
    gen_value(g, o, e->type_decl, e->inits.items, e->inits.count);
}

static void gen_spawn(gen *g, sb *o, const expr *e)
{
    const int a = e->spawn_archetype;
    sb_printf(o, "purr_cmd_spawn%d(purr_w, (purr_spawn%d){", a, a);
    int written = 0;
    for (int i = 0; i < e->args.count; i++) {
        const expr *arg = e->args.items[i];
        // A bare component with no declared defaults is all zeros; leave it out.
        if (arg->kind != E_LITERAL && !has_defaults(arg->type_decl)) continue;
        sb_printf(o, "%s.%s = ", written++ ? ", " : "", type_cname(arg->type_decl));
        gen_value(g, o, arg->type_decl, arg->inits.items, arg->inits.count);
    }
    if (written == 0) sb_put(o, "0");
    sb_put(o, "})");
}

static const char *int_op_helper(const tok_kind op)
{
    switch (op) {
    case T_PLUS: return "purr_add_i";
    case T_MINUS: return "purr_sub_i";
    case T_STAR: return "purr_mul_i";
    case T_SLASH: return "purr_div_i";
    case T_PERCENT: return "purr_mod_i";
    case T_SHL: return "purr_shl_i";
    case T_SHR: return "purr_shr_i";
    default: return NULL; // & | ^ are well defined in C as they are.
    }
}

static const char *c_op(const tok_kind op)
{
    switch (op) {
    case T_PLUS: return "+";
    case T_MINUS: return "-";
    case T_STAR: return "*";
    case T_SLASH: return "/";
    case T_LT: return "<";
    case T_LE: return "<=";
    case T_GT: return ">";
    case T_GE: return ">=";
    case T_EQ: return "==";
    case T_NE: return "!=";
    case T_AND: return "&&";
    case T_OR: return "||";
    case T_AMP: return "&";
    case T_PIPE: return "|";
    case T_CARET: return "^";
    default: return "?";
    }
}

static void gen_binary(gen *g, sb *o, const tok_kind op, expr *l, expr *r, const type result)
{
    const type lt = l->type;
    const type rt = r->type;
    const type float_t = {TY_FLOAT, NULL};

    // Vectors: component-wise, with both sides converted to the result type.
    if (type_dim(result) >= 2) {
        const char *name = op == T_PLUS ? "add" : op == T_MINUS ? "sub" : op == T_STAR ? "mul" : op == T_SLASH ? "div" : "mod";
        sb_printf(o, "purr_%s_%s(", name, type_suffix(result));
        gen_as(g, o, l, result);
        sb_put(o, ", ");
        gen_as(g, o, r, result);
        sb_put(o, ")");
        return;
    }

    // Matrices: + and - with a matrix, * and / by a number.
    if (matrix_dim(result)) {
        const char *suffix = type_suffix(result);
        if (matrix_dim(lt) && matrix_dim(rt)) {
            sb_printf(o, "purr_%s_%s(", op == T_PLUS ? "add" : "sub", suffix);
            gen_expr(g, o, l);
            sb_put(o, ", ");
            gen_expr(g, o, r);
        } else {
            expr *matrix = matrix_dim(lt) ? l : r;
            expr *scalar = matrix == l ? r : l;
            sb_printf(o, "purr_%s_%s(", op == T_STAR ? "scale" : "divs", suffix);
            gen_expr(g, o, matrix);
            sb_put(o, ", ");
            gen_as(g, o, scalar, float_t);
        }
        sb_put(o, ")");
        return;
    }

    if (lt.kind == TY_ENTITY || lt.kind == TY_PLAYER) {
        const char *fn = lt.kind == TY_ENTITY ? "purr_entity_equal(" : "purr_player_equal(";
        if (op == T_NE) sb_put(o, "!");
        sb_put(o, fn);
        gen_expr(g, o, l);
        sb_put(o, ", ");
        gen_expr(g, o, r);
        sb_put(o, ")");
        return;
    }

    // Integer arithmetic and shifts go through helpers that wrap on overflow and
    // define division by zero and shift counts, so results never depend on the
    // C compiler.
    if (result.kind == TY_INT && int_op_helper(op)) {
        sb_printf(o, "%s(", int_op_helper(op));
        gen_expr(g, o, l);
        sb_put(o, ", ");
        gen_expr(g, o, r);
        sb_put(o, ")");
        return;
    }

    bool mixed = (lt.kind == TY_FLOAT) != (rt.kind == TY_FLOAT) && lt.kind != TY_BOOL;
    sb_put(o, "(");
    if (mixed) gen_as(g, o, l, float_t);
    else gen_expr(g, o, l);
    sb_printf(o, " %s ", c_op(op));
    if (mixed) gen_as(g, o, r, float_t);
    else gen_expr(g, o, r);
    sb_put(o, ")");
}

// float3(...), int(...), quaternion(...), float4x4(...): see check_construct for the forms.
static void gen_construct(gen *g, sb *o, const expr *e)
{
    const type t = e->type;
    const type float_t = {TY_FLOAT, NULL};
    expr *first = e->args.count > 0 ? e->args.items[0] : NULL;

    switch (e->ctor) {
    case CTOR_SCALAR:
        if (t.kind == TY_FLOAT) {
            gen_as(g, o, first, t);
        } else if (first->type.kind == TY_FLOAT) {
            sb_put(o, "purr_i_from_f(");
            gen_expr(g, o, first);
            sb_put(o, ")");
        } else {
            gen_expr(g, o, first);
        }
        break;

    case CTOR_SPLAT:
        sb_printf(o, "purr_%s_splat(", type_suffix(t));
        if (type_is_int_based(t) && first->type.kind == TY_FLOAT) {
            sb_put(o, "purr_i_from_f(");
            gen_expr(g, o, first);
            sb_put(o, ")");
        } else {
            gen_as(g, o, first, vector_type(type_is_float_based(t), 1));
        }
        sb_put(o, ")");
        break;

    case CTOR_CONVERT:
        sb_printf(o, "purr_%s_from_%s(", type_suffix(t), type_suffix(first->type));
        gen_expr(g, o, first);
        sb_put(o, ")");
        break;

    case CTOR_COMPONENTS: {
        if (t.kind == TY_QUATERNION) {
            sb_put(o, "purr_q(");
            for (int i = 0; i < e->args.count; i++) {
                if (i) sb_put(o, ", ");
                gen_as(g, o, e->args.items[i], float_t);
            }
            sb_put(o, ")");
            break;
        }
        const bool is_float = type_is_float_based(t);
        sb_printf(o, "(%s){", c_type(t));
        int written = 0;
        for (int i = 0; i < e->args.count; i++) {
            expr *arg = e->args.items[i];
            const int dim = type_dim(arg->type);
            if (dim == 1) {
                if (written++) sb_put(o, ", ");
                if (is_float) gen_as(g, o, arg, float_t);
                else gen_expr(g, o, arg);
                continue;
            }
            const bool widen = is_float && type_is_int_based(arg->type);
            if (arg->kind == E_MEMBER && arg->swizzle_len > 1) {
                // A swizzle argument: read its components straight from the source vector.
                const char *access = object_access(g, arg->object);
                for (int k = 0; k < dim; k++) {
                    sb_printf(o, "%s%s%s%s", written++ ? ", " : "", widen ? "(float)" : "", access, xyzw[arg->swizzle[k]]);
                }
                continue;
            }
            const char *text = expr_text(g, arg);
            for (int k = 0; k < dim; k++) {
                sb_printf(o, "%s%s(%s).%s", written++ ? ", " : "", widen ? "(float)" : "", text, xyzw[k]);
            }
        }
        sb_put(o, "}");
        break;
    }

    case CTOR_QUAT_FROM_F4:
        sb_put(o, "(purr_quaternion){");
        gen_expr(g, o, first);
        sb_put(o, "}");
        break;

    case CTOR_QUAT_FROM_MAT:
        sb_put(o, "purr_q_from_f3x3(");
        gen_expr(g, o, first);
        sb_put(o, ")");
        break;

    case CTOR_MAT_COLUMNS: {
        const type column = vector_type(true, matrix_dim(t));
        sb_printf(o, "(%s){", c_type(t));
        for (int i = 0; i < e->args.count; i++) {
            if (i) sb_put(o, ", ");
            gen_as(g, o, e->args.items[i], column);
        }
        sb_put(o, "}");
        break;
    }

    case CTOR_MAT_SCALARS: {
        // Arguments are row by row; storage is column by column.
        const int n = matrix_dim(t);
        sb_printf(o, "(%s){", c_type(t));
        for (int col = 0; col < n; col++) {
            sb_put(o, col ? ", {" : "{");
            for (int row = 0; row < n; row++) {
                if (row) sb_put(o, ", ");
                gen_as(g, o, e->args.items[row * n + col], float_t);
            }
            sb_put(o, "}");
        }
        sb_put(o, "}");
        break;
    }

    case CTOR_MAT_FROM_QUAT:
        sb_put(o, "purr_f3x3_from_q(");
        gen_expr(g, o, first);
        sb_put(o, ")");
        break;

    case CTOR_MAT_FROM_ROT_T:
        sb_put(o, "purr_f4x4_from_f3x3_f3(");
        gen_expr(g, o, first);
        sb_put(o, ", ");
        gen_as(g, o, e->args.items[1], (type){TY_FLOAT3, NULL});
        sb_put(o, ")");
        break;

    case CTOR_PLAYER:
        sb_put(o, "purr_player_from_index(");
        gen_expr(g, o, first);
        sb_put(o, ")");
        break;
    }
}

static void gen_expr(gen *g, sb *o, const expr *e)
{
    switch (e->kind) {
    case E_INT:
        // Hex and binary literals can be negative (0xFFFFFFFF is -1).
        if (e->int_value == INT32_MIN) sb_put(o, "(-2147483647 - 1)");
        else if (e->int_value < 0) sb_printf(o, "(%lld)", (long long)e->int_value);
        else sb_printf(o, "%lld", (long long)e->int_value);
        break;
    case E_FLOAT:
        gen_float_literal(o, e->text);
        break;
    case E_BOOL:
        sb_put(o, e->bool_value ? "true" : "false");
        break;
    case E_NAME:
        if (e->bind == BIND_FIELD) {
            sb_printf(o, "purr_self.%s", field_cname(e->field)); // Inside the input's constructor
        } else if (e->bind == BIND_LOCAL) {
            sb_put(o, local_cname(g, e->name));
        } else if (e->bind == BIND_PARAM && is_pointer_param(e->param)) {
            sb_printf(o, "(*%s)", local_cname(g, e->name));
        } else {
            sb_put(o, local_cname(g, e->name));
        }
        break;
    case E_MEMBER: {
        if (e->c_constant) {
            sb_put(o, e->c_constant); // quaternion.Identity, Math.PI
            break;
        }
        if (e->edge != EDGE_NONE) {
            // input.jump.pressed: this tick's value against last tick's.
            const expr *field_access = e->object;
            const char *now = expr_text(g, field_access);
            sb b = {0};
            sb_printf(&b, "%s->%s", prev_input_name(g, field_access->object->name), field_cname(field_access->field));
            if (e->edge == EDGE_PRESSED) sb_printf(o, "(%s && !%s)", now, b.data);
            else sb_printf(o, "(!%s && %s)", now, b.data);
            break;
        }
        const char *access = object_access(g, e->object);
        if (e->swizzle_len > 1) {
            sb_printf(o, "(%s){", c_type(e->type));
            for (int i = 0; i < e->swizzle_len; i++) sb_printf(o, "%s%s%s", i ? ", " : "", access, xyzw[e->swizzle[i]]);
            sb_put(o, "}");
        } else if (e->swizzle_len == 1) {
            sb_printf(o, "%s%s", access, xyzw[e->swizzle[0]]);
        } else if (e->field) {
            sb_printf(o, "%s%s", access, field_cname(e->field));
        } else {
            sb_printf(o, "%s" STR_FMT, access, STR_ARG(e->member)); // quaternion.value, matrix.c0
        }
        break;
    }
    case E_CALL:
        if (e->call == CALL_CONSTRUCT) {
            gen_construct(g, o, e);
        } else if (e->call == CALL_SPAWN) {
            if (e->hoisted) sb_put(o, e->hoisted); // Already ran, before the statement
            else gen_spawn(g, o, e);
        }
        break;
    case E_LITERAL:
        gen_literal(g, o, e);
        break;
    case E_BINARY:
        gen_binary(g, o, e->op, e->lhs, e->rhs, e->type);
        break;
    case E_UNARY:
        if (e->op == T_NOT || e->op == T_TILDE) {
            sb_put(o, e->op == T_NOT ? "(!" : "(~");
            gen_expr(g, o, e->lhs);
            sb_put(o, ")");
        } else if (type_dim(e->type) >= 2 || matrix_dim(e->type)) {
            sb_printf(o, "purr_neg_%s(", type_suffix(e->type));
            gen_expr(g, o, e->lhs);
            sb_put(o, ")");
        } else if (e->lhs->kind == E_FLOAT || (e->lhs->kind == E_INT && e->lhs->int_value >= 0)) {
            // Negating a non-negative literal can't overflow.
            sb_put(o, "(-");
            gen_expr(g, o, e->lhs);
            sb_put(o, ")");
        } else if (e->type.kind == TY_INT) {
            sb_put(o, "purr_neg_i(");
            gen_expr(g, o, e->lhs);
            sb_put(o, ")");
        } else {
            sb_put(o, "(-");
            gen_expr(g, o, e->lhs);
            sb_put(o, ")");
        }
        break;
    case E_METHOD:
        // Math.Dot(a, b) and friends. Entity methods (Add, Remove, Destroy) are
        // statements; see gen_method.
        if (e->call == CALL_BUILTIN) {
            sb_printf(o, "%s(", e->c_callee);
            for (int i = 0; i < e->args.count; i++) {
                if (i) sb_put(o, ", ");
                gen_as(g, o, e->args.items[i], e->arg_want.items[i]);
            }
            sb_put(o, ")");
        }
        break;
    }
}

// e.Add(...), e.Remove(...), e.Destroy(): the entity expression is evaluated once.
static void gen_method(gen *g, const expr *e)
{
    sb *o = &g->c;
    indent(g, o);
    sb_put(o, "{ purr_entity purr_e = ");
    gen_expr(g, o, e->object);
    sb_put(o, ";");
    for (int i = 0; i < e->args.count && e->call != CALL_DESTROY; i++) {
        const expr *arg = e->args.items[i];
        decl *comp = arg->type_decl;
        if (e->call == CALL_ADD) {
            sb_printf(o, " purr_cmd_add_%s(purr_w, purr_e, ", type_cname(comp));
            gen_value(g, o, comp, arg->inits.items, arg->inits.count);
            sb_put(o, ");");
        } else {
            sb_printf(o, " purr_cmd_remove(purr_w, purr_e, %d);", comp->index);
        }
    }
    if (e->call == CALL_DESTROY) sb_put(o, " purr_cmd_destroy(purr_w, purr_e);");
    sb_put(o, " }\n");
}

// ---------------------------------------------------------------------------
// Statements

// PurrLang evaluates left to right, like C#, but C leaves the order of a call's
// arguments and an initializer's fields unspecified. Spawn is the only
// expression with a side effect (it allocates an entity ID), so each statement's
// spawns run first, into temporaries, in source order: children before the
// expression that uses them, siblings left to right.
typedef VEC(expr *) expr_list;

static void collect_spawns(expr *e, expr_list *out);

static void collect_spawn_list(expr **items, const int count, expr_list *out)
{
    for (int i = 0; i < count; i++) collect_spawns(items[i], out);
}

static void collect_spawns(expr *e, expr_list *out)
{
    if (!e) return;
    switch (e->kind) {
    case E_MEMBER:
        collect_spawns(e->object, out);
        break;
    case E_METHOD:
        collect_spawns(e->object, out);
        collect_spawn_list(e->args.items, e->args.count, out);
        break;
    case E_CALL:
        collect_spawn_list(e->args.items, e->args.count, out);
        if (e->call == CALL_SPAWN) vec_push(*out, e);
        break;
    case E_LITERAL:
        for (int i = 0; i < e->inits.count; i++) collect_spawns(e->inits.items[i].value, out);
        break;
    case E_BINARY:
        collect_spawns(e->lhs, out);
        collect_spawns(e->rhs, out);
        break;
    case E_UNARY:
        collect_spawns(e->lhs, out);
        break;
    default:
        break;
    }
}

// Emits the spawns of a statement's expressions (in the order given) before it.
static void hoist_spawns(gen *g, expr *a, expr *b)
{
    expr_list spawns = {0};
    collect_spawns(a, &spawns);
    collect_spawns(b, &spawns);
    for (int i = 0; i < spawns.count; i++) {
        expr *spawn = spawns.items[i];
        sb name = {0};
        sb_printf(&name, "purr_spawned%d", g->spawn_temps++);
        indent(g, &g->c);
        sb_printf(&g->c, "const purr_entity %s = ", name.data);
        gen_spawn(g, &g->c, spawn);
        sb_put(&g->c, ";\n");
        line(g, &g->c, "(void)%s;", name.data);
        spawn->hoisted = name.data;
    }
}

static void gen_stmt(gen *g, const stmt *s);

// Statements under if/else always get braces.
static void gen_body_stmt(gen *g, const stmt *s)
{
    if (s->kind == S_BLOCK) {
        for (int i = 0; i < s->stmts.count; i++) gen_stmt(g, s->stmts.items[i]);
    } else {
        gen_stmt(g, s);
    }
}

static void gen_stmt(gen *g, const stmt *s)
{
    sb *o = &g->c;
    if (s->kind != S_BLOCK) line_directive(g, s->at);

    switch (s->kind) {
    case S_VAR:
    case S_EXPR: hoist_spawns(g, s->value, NULL); break;
    case S_ASSIGN: hoist_spawns(g, s->target, s->value); break;
    case S_IF: hoist_spawns(g, s->cond, NULL); break;
    default: break;
    }

    switch (s->kind) {
    case S_BLOCK:
        line(g, o, "{");
        g->indent++;
        for (int i = 0; i < s->stmts.count; i++) gen_stmt(g, s->stmts.items[i]);
        g->indent--;
        line(g, o, "}");
        break;

    case S_IF:
        indent(g, o);
        sb_put(o, "if (");
        gen_expr(g, o, s->cond);
        sb_put(o, ") {\n");
        g->indent++;
        gen_body_stmt(g, s->then_stmt);
        g->indent--;
        if (s->else_stmt) {
            line(g, o, "} else {");
            g->indent++;
            gen_body_stmt(g, s->else_stmt);
            g->indent--;
        }
        line(g, o, "}");
        break;

    case S_RETURN:
        line(g, o, g->in_constructor ? "return purr_self;" : "return;");
        break;

    case S_VAR: {
        const char *name = local_cname(g, s->name);
        indent(g, o);
        sb_printf(o, "%s%s %s = ", s->is_mut ? "" : "const ", c_type(s->type), name);
        gen_as(g, o, s->value, s->type);
        sb_printf(o, ";\n");
        line(g, o, "(void)%s;", name);
        break;
    }

    case S_ASSIGN: {
        const expr *target = s->target;
        indent(g, o);
        if (target->kind == E_MEMBER && target->swizzle_len > 1) {
            // v.xz = value: compute into a temporary, then write each component.
            sb_printf(o, "{ const %s purr_t = ", c_type(target->type));
        } else {
            gen_expr(g, o, target);
            sb_put(o, " = ");
        }
        if (s->op == T_ASSIGN) {
            gen_as(g, o, s->value, target->type);
        } else {
            gen_binary(g, o, compound_op(s->op), s->target, s->value, target->type);
        }
        sb_put(o, ";");
        if (target->kind == E_MEMBER && target->swizzle_len > 1) {
            const char *access = object_access(g, target->object);
            for (int i = 0; i < target->swizzle_len; i++) {
                sb_printf(o, " %s%s = purr_t.%s;", access, xyzw[target->swizzle[i]], xyzw[i]);
            }
            sb_put(o, " }");
        }
        sb_put(o, "\n");
        break;
    }

    case S_EXPR:
        if (s->value->kind == E_METHOD) {
            gen_method(g, s->value);
        } else {
            indent(g, o);
            sb_put(o, "(void)");
            gen_expr(g, o, s->value);
            sb_put(o, ";\n");
        }
        break;
    }
}

// ---------------------------------------------------------------------------
// Header: types, world layout, public API

static void gen_fields(const gen *g, sb *o, const decl *d)
{
    if (d->fields.count == 0) sb_put(o, "    uint8_t purr_empty; // C structs can't be empty.\n");
    for (int i = 0; i < d->fields.count; i++) {
        const field *f = &d->fields.items[i];
        sb_printf(o, "    %s %s;\n", c_type(f->type), field_cname(f));
    }
    (void)g;
}

static void gen_header(gen *g)
{
    const program *prog = g->prog;
    sb *o = &g->h;

    sb_printf(o, "// Generated by purrc from %s. Do not edit.\n", g->purr_path);
    sb_put(o, "#pragma once\n\n");
    sb_put(o, "#include <stdbool.h>\n#include <stdint.h>\n\n");
    sb_put(o, "#include \"purr/devices.h\"\n#include \"purr/entity.h\"\n#include \"purr/math.h\"\n#include \"purr/player.h\"\n\n");
    sb_put(o, "#ifndef PURR_ARCHETYPE_CAPACITY\n#define PURR_ARCHETYPE_CAPACITY 1024u\n#endif\n\n");
    sb_put(o, "#ifndef PURR_MAX_COMMANDS\n#define PURR_MAX_COMMANDS 4096u\n#endif\n\n");

    sb_put(o, "// Components\n\n");
    for (int i = 0; i < prog->components.count; i++) {
        const decl *d = prog->components.items[i];
        const char *name = type_cname(d);
        sb_printf(o, "typedef struct %s {\n", name);
        gen_fields(g, o, d);
        sb_printf(o, "} %s;\n\n", name);
    }

    sb_put(o, "// Singletons: one per world\n\n");
    for (int i = 0; i < prog->singletons.count; i++) {
        const decl *d = prog->singletons.items[i];
        const char *name = type_cname(d);
        sb_printf(o, "typedef struct %s {\n", name);
        gen_fields(g, o, d);
        sb_printf(o, "} %s;\n\n", name);
    }

    if (prog->input) {
        const char *name = type_cname(prog->input);
        sb_put(o, "// One player's input for one tick\n\n");
        sb_printf(o, "typedef struct %s {\n", name);
        gen_fields(g, o, prog->input);
        sb_printf(o, "} %s;\n\n", name);
    }

    sb_put(o, "// Archetypes: storage for each component combination the program can create\n\n");
    for (int a = 0; a < prog->archetypes.count; a++) {
        const uint64_t mask = prog->archetypes.items[a];
        sb_printf(o, "// %s\n", arch_label(g, a));
        sb_printf(o, "typedef struct purr_%s {\n", arch_name(g, a));
        sb_put(o, "    uint32_t count;\n    purr_entity entity[PURR_ARCHETYPE_CAPACITY];\n");
        for (int i = 0; i < prog->components.count; i++) {
            if (!has_component(mask, i)) continue;
            const char *comp = type_cname(prog->components.items[i]);
            sb_printf(o, "    %s %s[PURR_ARCHETYPE_CAPACITY];\n", comp, comp);
        }
        sb_printf(o, "} purr_%s;\n\n", arch_name(g, a));
    }

    sb_put(o, "// Values for each Spawn, per archetype\n\n");
    for (int a = 0; a < prog->archetypes.count; a++) {
        if (!g->prog->spawn_target.items[a]) continue;
        const uint64_t mask = prog->archetypes.items[a];
        sb_printf(o, "typedef struct purr_spawn%d {\n", a);
        if (mask == 0) sb_put(o, "    uint8_t purr_empty;\n");
        for (int i = 0; i < prog->components.count; i++) {
            if (!has_component(mask, i)) continue;
            const char *comp = type_cname(prog->components.items[i]);
            sb_printf(o, "    %s %s;\n", comp, comp);
        }
        sb_printf(o, "} purr_spawn%d;\n\n", a);
    }

    sb_put(o, "// Structural changes, deferred to the end of the tick\n\n");
    sb_put(o, "typedef struct purr_command {\n    uint32_t kind;\n    uint32_t id; // Archetype for spawns, component for add and remove.\n");
    sb_put(o, "    purr_entity entity;\n    union {\n        uint8_t purr_none;\n");
    for (int i = 0; i < prog->components.count; i++) {
        const decl *d = prog->components.items[i];
        if (!has_component(prog->added_mask, d->index)) continue;
        sb_printf(o, "        %s %s;\n", type_cname(d), type_cname(d));
    }
    for (int a = 0; a < prog->archetypes.count; a++) {
        if (g->prog->spawn_target.items[a]) sb_printf(o, "        purr_spawn%d spawn%d;\n", a, a);
    }
    sb_put(o, "    } data;\n} purr_command;\n\n");

    sb_put(o, "// The whole simulation state. Plain data: copying it is a snapshot.\n\n");
    sb_put(o, "typedef struct purr_world {\n");
    for (int i = 0; i < prog->singletons.count; i++) {
        const char *name = type_cname(prog->singletons.items[i]);
        sb_printf(o, "    %s %s;\n", name, name);
    }
    sb_put(o, "    purr_entities entities;\n");
    for (int a = 0; a < prog->archetypes.count; a++) {
        const char *name = arch_name(g, a);
        sb_printf(o, "    purr_%s %s;\n", name, name);
    }
    sb_put(o, "    uint32_t command_count;\n    purr_command commands[PURR_MAX_COMMANDS];\n");
    if (prog->input) {
        const char *name = type_cname(prog->input);
        sb_put(o, "    // Each player's input for this tick and the last; last tick's gives .pressed and .released.\n");
        sb_printf(o, "    %s inputs[PURR_MAX_PLAYERS];\n", name);
        sb_printf(o, "    %s previous_inputs[PURR_MAX_PLAYERS];\n", name);
        sb_printf(o, "    %s no_input; // For entities owned by no player: the defaults.\n", name);
    }
    sb_put(o, "} purr_world;\n\n");

    sb_put(o, "// Clears the world, sets Time.dt and singleton defaults, runs Main and applies its spawns.\n");
    sb_put(o, "void purr_world_init(purr_world *w, float dt);\n\n");
    sb_put(o, "// Runs every system once, in declaration order, then applies structural changes.\n");
    sb_put(o, "void purr_world_tick(purr_world *w);\n\n");
    sb_put(o, "uint32_t purr_world_entity_count(const purr_world *w);\n\n");
    sb_put(o, "// Prints every entity and its components, for debugging.\n");
    sb_put(o, "void purr_world_print(const purr_world *w);\n\n");
    sb_put(o, "// Component of an entity, or NULL if the entity is dead or doesn't have it.\n");
    for (int i = 0; i < prog->components.count; i++) {
        const char *name = type_cname(prog->components.items[i]);
        sb_printf(o, "%s *purr_get_%s(purr_world *w, purr_entity e);\n", name, name);
    }

    if (prog->input) {
        const char *name = type_cname(prog->input);
        sb_put(o, "\n// Client side: builds the local player's input from the devices by running the\n");
        sb_put(o, "// input's constructor. Call once per tick, then purr_devices_consume(devices).\n");
        sb_printf(o, "%s purr_input_sample(const purr_devices *devices);\n\n", name);
        sb_put(o, "// Sets a player's input for the next tick. A player whose input isn't set keeps\n");
        sb_put(o, "// their last one, which is also the usual guess for a remote player.\n");
        sb_printf(o, "void purr_world_set_input(purr_world *w, purr_player_id player, %s input);\n", name);
    }
}

// ---------------------------------------------------------------------------
// Source: runtime helpers

static void gen_prelude(gen *g)
{
    sb *o = &g->c;
    sb_printf(o, "// Generated by purrc from %s. Do not edit.\n\n", g->purr_path);
    sb_printf(o, "#include \"%s.h\"\n\n", g->opts->name);
    sb_put(o, "#include <stddef.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n\n");
    sb_put(o, "// Determinism: never fuse a*b + c, whatever flags this file is built with.\n");
    sb_put(o, "#pragma STDC FP_CONTRACT OFF\n\n");
    sb_put(o, "// Helpers are emitted whether or not this program uses them.\n");
    sb_put(o, "#define PURR_HELPER static inline __attribute__((unused))\n\n");

    sb_put(o,
        "static _Noreturn void purr_fatal(const char *message)\n"
        "{\n"
        "    fprintf(stderr, \"purr: %s\\n\", message);\n"
        "    abort();\n"
        "}\n\n"
        "// Arithmetic on ints, vectors, quaternions and matrices comes from purr/math.h.\n\n");
}

// ---------------------------------------------------------------------------
// Source: archetype storage and structural changes

static void gen_remove_rows(gen *g)
{
    const program *prog = g->prog;
    sb *o = &g->c;
    sb_put(o, "// Swap-remove a row, moving the last entity into the gap.\n\n");
    for (int a = 0; a < prog->archetypes.count; a++) {
        const char *name = arch_name(g, a);
        const uint64_t mask = prog->archetypes.items[a];
        sb_printf(o, "PURR_HELPER void purr_remove_row%d(purr_world *w, uint32_t row)\n{\n", a);
        sb_printf(o, "    purr_%s *a = &w->%s;\n", name, name);
        sb_put(o, "    uint32_t last = --a->count;\n    if (row == last) return;\n");
        sb_put(o, "    a->entity[row] = a->entity[last];\n");
        for (int i = 0; i < prog->components.count; i++) {
            if (!has_component(mask, i)) continue;
            const char *comp = type_cname(prog->components.items[i]);
            sb_printf(o, "    a->%s[row] = a->%s[last];\n", comp, comp);
        }
        sb_printf(o, "    purr_entity_set_location(&w->entities, a->entity[row], (purr_location){%d, row});\n}\n\n", a);
    }
}

static int find_arch(const gen *g, const uint64_t mask)
{
    for (int a = 0; a < g->prog->archetypes.count; a++) {
        if (g->prog->archetypes.items[a] == mask) return a;
    }
    return -1;
}

static void need_move(gen *g, const int from, const int to)
{
    for (int i = 0; i < g->moves.count; i++) {
        if (g->moves.items[i].from == from && g->moves.items[i].to == to) return;
    }
    const transition t = {from, to};
    vec_push(g->moves, t);
}

static void gen_moves(gen *g)
{
    const program *prog = g->prog;
    sb *o = &g->c;

    for (int c = 0; c < prog->components.count; c++) {
        for (int a = 0; a < prog->archetypes.count; a++) {
            const uint64_t mask = prog->archetypes.items[a];
            const uint64_t b = (uint64_t)1 << c;
            if (has_component(prog->added_mask, c) && !(mask & b)) need_move(g, a, find_arch(g, mask | b));
            if (has_component(prog->removed_mask, c) && (mask & b)) need_move(g, a, find_arch(g, mask & ~b));
        }
    }

    sb_put(o, "// Move an entity between archetypes. Returns its row in the new one.\n\n");
    for (int i = 0; i < g->moves.count; i++) {
        const int from = g->moves.items[i].from;
        const int to = g->moves.items[i].to;
        const uint64_t from_mask = prog->archetypes.items[from];
        const uint64_t to_mask = prog->archetypes.items[to];
        sb_printf(o, "// %s -> %s\n", arch_label(g, from), arch_label(g, to));
        sb_printf(o, "PURR_HELPER uint32_t purr_move%d_%d(purr_world *w, uint32_t row)\n{\n", from, to);
        sb_printf(o, "    purr_%s *from = &w->%s;\n", arch_name(g, from), arch_name(g, from));
        sb_printf(o, "    purr_%s *to = &w->%s;\n", arch_name(g, to), arch_name(g, to));
        sb_printf(o, "    if (to->count == PURR_ARCHETYPE_CAPACITY) purr_fatal(\"too many entities with %s (raise PURR_ARCHETYPE_CAPACITY)\");\n",
                  arch_label(g, to));
        sb_put(o, "    uint32_t dst = to->count++;\n    to->entity[dst] = from->entity[row];\n");
        for (int c = 0; c < prog->components.count; c++) {
            if (!has_component(to_mask, c)) continue;
            const char *comp = type_cname(prog->components.items[c]);
            if (has_component(from_mask, c)) sb_printf(o, "    to->%s[dst] = from->%s[row];\n", comp, comp);
            else sb_printf(o, "    to->%s[dst] = (%s){0};\n", comp, comp);
        }
        sb_printf(o, "    purr_entity_set_location(&w->entities, to->entity[dst], (purr_location){%d, dst});\n", to);
        sb_printf(o, "    purr_remove_row%d(w, row);\n    return dst;\n}\n\n", from);
    }
}

static void gen_command_recorders(gen *g)
{
    const program *prog = g->prog;
    sb *o = &g->c;

    sb_put(o, "enum { PURR_CMD_SPAWN, PURR_CMD_ADD, PURR_CMD_REMOVE, PURR_CMD_DESTROY };\n\n");
    sb_put(o,
        "PURR_HELPER purr_command *purr_cmd_push(purr_world *w, uint32_t kind, uint32_t id, purr_entity e)\n"
        "{\n"
        "    if (w->command_count == PURR_MAX_COMMANDS) purr_fatal(\"too many structural changes in one tick (raise PURR_MAX_COMMANDS)\");\n"
        "    purr_command *c = &w->commands[w->command_count++];\n"
        "    c->kind = kind;\n"
        "    c->id = id;\n"
        "    c->entity = e;\n"
        "    return c;\n"
        "}\n\n");

    for (int a = 0; a < prog->archetypes.count; a++) {
        if (!g->prog->spawn_target.items[a]) continue;
        sb_printf(o, "// Spawn: %s\n", arch_label(g, a));
        sb_printf(o, "PURR_HELPER purr_entity purr_cmd_spawn%d(purr_world *w, purr_spawn%d values)\n{\n", a, a);
        sb_put(o, "    purr_entity e = purr_entity_create(&w->entities);\n");
        sb_put(o, "    if (purr_entity_is_null(e)) purr_fatal(\"too many entities (raise PURR_MAX_ENTITIES)\");\n");
        sb_printf(o, "    purr_cmd_push(w, PURR_CMD_SPAWN, %d, e)->data.spawn%d = values;\n    return e;\n}\n\n", a, a);
    }

    for (int c = 0; c < prog->components.count; c++) {
        if (!has_component(prog->added_mask, c)) continue;
        const char *comp = type_cname(prog->components.items[c]);
        sb_printf(o, "PURR_HELPER void purr_cmd_add_%s(purr_world *w, purr_entity e, %s value)\n{\n", comp, comp);
        sb_printf(o, "    purr_cmd_push(w, PURR_CMD_ADD, %d, e)->data.%s = value;\n}\n\n", c, comp);
    }

    sb_put(o,
        "PURR_HELPER void purr_cmd_remove(purr_world *w, purr_entity e, uint32_t component)\n"
        "{\n"
        "    purr_cmd_push(w, PURR_CMD_REMOVE, component, e);\n"
        "}\n\n"
        "PURR_HELPER void purr_cmd_destroy(purr_world *w, purr_entity e)\n"
        "{\n"
        "    purr_cmd_push(w, PURR_CMD_DESTROY, 0, e);\n"
        "}\n\n");
}

static void gen_apply(gen *g)
{
    const program *prog = g->prog;
    sb *o = &g->c;

    // Spawn
    // A program may have no spawns at all, leaving `w` unused.
    sb_put(o, "PURR_HELPER void purr_apply_spawn(purr_world *w, const purr_command *c)\n{\n    (void)w;\n    switch (c->id) {\n");
    for (int a = 0; a < prog->archetypes.count; a++) {
        if (!g->prog->spawn_target.items[a]) continue;
        const uint64_t mask = prog->archetypes.items[a];
        const char *name = arch_name(g, a);
        sb_printf(o, "    case %d: { // %s\n", a, arch_label(g, a));
        sb_printf(o, "        purr_%s *a = &w->%s;\n", name, name);
        sb_printf(o, "        if (a->count == PURR_ARCHETYPE_CAPACITY) purr_fatal(\"too many entities with %s (raise PURR_ARCHETYPE_CAPACITY)\");\n",
                  arch_label(g, a));
        sb_put(o, "        uint32_t row = a->count++;\n        a->entity[row] = c->entity;\n");
        for (int i = 0; i < prog->components.count; i++) {
            if (!has_component(mask, i)) continue;
            const char *comp = type_cname(prog->components.items[i]);
            sb_printf(o, "        a->%s[row] = c->data.spawn%d.%s;\n", comp, a, comp);
        }
        sb_printf(o, "        purr_entity_set_location(&w->entities, c->entity, (purr_location){%d, row});\n", a);
        sb_put(o, "        break;\n    }\n");
    }
    sb_put(o, "    default: break;\n    }\n}\n\n");

    // Add: replace the value if the entity already has the component, otherwise move.
    sb_put(o, "PURR_HELPER void purr_apply_add(purr_world *w, const purr_command *c)\n{\n");
    sb_put(o, "    purr_location loc = purr_entity_location(&w->entities, c->entity);\n");
    sb_put(o, "    if (loc.archetype == PURR_ARCHETYPE_NONE) return; // Destroyed earlier.\n");
    sb_put(o, "    switch (c->id) {\n");
    for (int comp_i = 0; comp_i < prog->components.count; comp_i++) {
        if (!has_component(prog->added_mask, comp_i)) continue;
        const char *comp = type_cname(prog->components.items[comp_i]);
        sb_printf(o, "    case %d: // %s\n        switch (loc.archetype) {\n", comp_i, comp);
        for (int a = 0; a < prog->archetypes.count; a++) {
            const uint64_t mask = prog->archetypes.items[a];
            if (has_component(mask, comp_i)) {
                sb_printf(o, "        case %d: w->%s.%s[loc.row] = c->data.%s; break;\n", a, arch_name(g, a), comp, comp);
            } else {
                const int to = find_arch(g, mask | ((uint64_t)1 << comp_i));
                sb_printf(o, "        case %d: w->%s.%s[purr_move%d_%d(w, loc.row)] = c->data.%s; break;\n",
                          a, arch_name(g, to), comp, a, to, comp);
            }
        }
        sb_put(o, "        default: break;\n        }\n        break;\n");
    }
    sb_put(o, "    default: break;\n    }\n}\n\n");

    // Remove: nothing happens if the entity doesn't have the component.
    sb_put(o, "PURR_HELPER void purr_apply_remove(purr_world *w, const purr_command *c)\n{\n");
    sb_put(o, "    purr_location loc = purr_entity_location(&w->entities, c->entity);\n");
    sb_put(o, "    if (loc.archetype == PURR_ARCHETYPE_NONE) return; // Destroyed earlier.\n");
    sb_put(o, "    switch (c->id) {\n");
    for (int comp_i = 0; comp_i < prog->components.count; comp_i++) {
        if (!has_component(prog->removed_mask, comp_i)) continue;
        sb_printf(o, "    case %d: // %s\n        switch (loc.archetype) {\n", comp_i, type_cname(prog->components.items[comp_i]));
        for (int a = 0; a < prog->archetypes.count; a++) {
            const uint64_t mask = prog->archetypes.items[a];
            if (!has_component(mask, comp_i)) continue;
            const int to = find_arch(g, mask & ~((uint64_t)1 << comp_i));
            sb_printf(o, "        case %d: purr_move%d_%d(w, loc.row); break;\n", a, a, to);
        }
        sb_put(o, "        default: break;\n        }\n        break;\n");
    }
    sb_put(o, "    default: break;\n    }\n}\n\n");

    // Destroy
    sb_put(o, "PURR_HELPER void purr_apply_destroy(purr_world *w, const purr_command *c)\n{\n");
    sb_put(o, "    purr_location loc = purr_entity_location(&w->entities, c->entity);\n");
    sb_put(o, "    switch (loc.archetype) {\n");
    for (int a = 0; a < prog->archetypes.count; a++) {
        sb_printf(o, "    case %d: purr_remove_row%d(w, loc.row); break;\n", a, a);
    }
    sb_put(o, "    default: return; // Already destroyed.\n    }\n");
    sb_put(o, "    purr_entity_destroy(&w->entities, c->entity);\n}\n\n");

    // Commands apply in the order they were recorded, which is deterministic:
    // systems run in a fixed order and iterate entities in a fixed order.
    sb_put(o,
        "static void purr_apply_commands(purr_world *w)\n"
        "{\n"
        "    for (uint32_t i = 0; i < w->command_count; i++) {\n"
        "        const purr_command *c = &w->commands[i];\n"
        "        switch (c->kind) {\n"
        "        case PURR_CMD_SPAWN: purr_apply_spawn(w, c); break;\n"
        "        case PURR_CMD_ADD: purr_apply_add(w, c); break;\n"
        "        case PURR_CMD_REMOVE: purr_apply_remove(w, c); break;\n"
        "        case PURR_CMD_DESTROY: purr_apply_destroy(w, c); break;\n"
        "        default: break;\n"
        "        }\n"
        "    }\n"
        "    w->command_count = 0;\n"
        "}\n\n");

    if (prog->input) {
        const char *name = type_cname(prog->input);
        sb_put(o, "// A player's input, this tick's or last tick's. No player gets the defaults.\n");
        sb_printf(o, "PURR_HELPER const %s *purr_input_of(const purr_world *w, purr_player_id player, bool previous)\n{\n", name);
        sb_put(o, "    const int32_t index = purr_player_index(player);\n");
        sb_put(o, "    if (index < 0) return &w->no_input;\n");
        sb_put(o, "    return previous ? &w->previous_inputs[index] : &w->inputs[index];\n}\n\n");
    }
}

// ---------------------------------------------------------------------------
// Source: systems

static void gen_system_body(gen *g, const decl *sys)
{
    sb *o = &g->c;
    sb_printf(o, "// system " STR_FMT "\n", STR_ARG(sys->name));
    // PURR_HELPER: a system that no entity matches is never called.
    sb_printf(o, "PURR_HELPER void purr_system_" STR_FMT "(purr_world *purr_w", STR_ARG(sys->name));
    for (int i = 0; i < sys->params.count; i++) {
        const param *p = &sys->params.items[i];
        if (p->mode == PARAM_WITH || p->mode == PARAM_WITHOUT) continue;
        const char *name = local_cname(g, p->name);
        if (p->type.kind == TY_ENTITY) {
            sb_printf(o, ", purr_entity %s", name);
        } else {
            sb_printf(o, ", %s%s *restrict %s", p->mode == PARAM_MUT ? "" : "const ", c_type(p->type), name);
        }
        if (p->type.kind == TY_INPUT) {
            sb_printf(o, ", const %s *restrict %s", c_type(p->type), prev_input_name(g, p->name));
        }
    }
    sb_put(o, ")\n{\n");
    g->indent = 1;
    g->spawn_temps = 0;
    line(g, o, "(void)purr_w;");
    for (int i = 0; i < sys->params.count; i++) {
        const param *p = &sys->params.items[i];
        if (p->name.len > 0) line(g, o, "(void)%s;", local_cname(g, p->name));
        if (p->type.kind == TY_INPUT) line(g, o, "(void)%s;", prev_input_name(g, p->name));
    }
    for (int i = 0; i < sys->body->stmts.count; i++) gen_stmt(g, sys->body->stmts.items[i]);
    g->indent = 0;
    sb_put(o, "}\n");
    line_reset(g);
    sb_put(o, "\n");
}

// Arguments that bind one entity's data to the system's parameters.
static void gen_system_args(gen *g, const decl *sys, const char *arch_var)
{
    sb *o = &g->c;
    for (int i = 0; i < sys->params.count; i++) {
        const param *p = &sys->params.items[i];
        switch (p->type.kind) {
        case TY_ENTITY:
            sb_printf(o, ", %s->entity[purr_i]", arch_var);
            break;
        case TY_SINGLETON:
            sb_printf(o, ", &purr_w->%s", type_cname(p->type.decl));
            break;
        case TY_COMPONENT:
            if (p->mode == PARAM_READ || p->mode == PARAM_MUT) {
                sb_printf(o, ", &%s->%s[purr_i]", arch_var, type_cname(p->type.decl));
            }
            break;
        case TY_INPUT: {
            // The owner's input this tick and last tick. Input systems always require Owner.
            const char *owner = type_cname(g->prog->owner);
            sb_printf(o, ", purr_input_of(purr_w, %s->%s[purr_i].player, false)", arch_var, owner);
            sb_printf(o, ", purr_input_of(purr_w, %s->%s[purr_i].player, true)", arch_var, owner);
            break;
        }
        default:
            break;
        }
    }
}

// The input's constructor, as purr_input_sample: fields start at their defaults
// and the body assigns them from the devices.
static void gen_constructor(gen *g)
{
    const decl *input = g->prog->input;
    sb *o = &g->c;
    const char *name = type_cname(input);
    const char *devices = input->params.count > 0 ? local_cname(g, input->params.items[0].name) : "devices";

    sb_printf(o, "// " STR_FMT "'s constructor\n", STR_ARG(input->name));
    sb_printf(o, "%s purr_input_sample(const purr_devices *restrict %s)\n{\n", name, devices);
    g->indent = 1;
    indent(g, o);
    sb_printf(o, "%s purr_self = ", name);
    gen_value(g, o, input, NULL, 0);
    sb_put(o, ";\n");
    line(g, o, "(void)%s;", devices);
    g->in_constructor = true;
    if (input->body) {
        for (int i = 0; i < input->body->stmts.count; i++) gen_stmt(g, input->body->stmts.items[i]);
    }
    g->in_constructor = false;
    line(g, o, "return purr_self;");
    g->indent = 0;
    sb_put(o, "}\n");
    line_reset(g);
    sb_put(o, "\n");
}

static void gen_system_run(gen *g, const decl *sys)
{
    const program *prog = g->prog;
    sb *o = &g->c;
    sb_printf(o, "static void purr_run_" STR_FMT "(purr_world *purr_w)\n{\n", STR_ARG(sys->name));

    if (!sys->per_entity) {
        sb_printf(o, "    purr_system_" STR_FMT "(purr_w", STR_ARG(sys->name));
        gen_system_args(g, sys, NULL);
        sb_put(o, ");\n");
    } else {
        bool any = false;
        for (int a = 0; a < prog->archetypes.count; a++) {
            const uint64_t mask = prog->archetypes.items[a];
            if ((mask & sys->need_mask) != sys->need_mask || (mask & sys->without_mask)) continue;
            any = true;
            const char *name = arch_name(g, a);
            sb_printf(o, "    { // %s\n", arch_label(g, a));
            sb_printf(o, "        purr_%s *purr_a = &purr_w->%s;\n", name, name);
            sb_put(o, "        for (uint32_t purr_i = 0; purr_i < purr_a->count; purr_i++) {\n");
            sb_printf(o, "            purr_system_" STR_FMT "(purr_w", STR_ARG(sys->name));
            gen_system_args(g, sys, "purr_a");
            sb_put(o, ");\n        }\n    }\n");
        }
        if (!any) sb_put(o, "    (void)purr_w; // No entity matches this system.\n");
    }
    sb_put(o, "}\n\n");
}

// ---------------------------------------------------------------------------
// Source: public API

static void gen_print_value(sb *o, const type t, const char *access)
{
    char part[300];
    switch (t.kind) {
    case TY_BOOL: sb_printf(o, "        printf(\"%%s\", %s ? \"true\" : \"false\");\n", access); return;
    case TY_INT: sb_printf(o, "        printf(\"%%d\", (int)%s);\n", access); return;
    case TY_FLOAT: sb_printf(o, "        printf(\"%%g\", (double)%s);\n", access); return;
    case TY_ENTITY:
        sb_printf(o, "        printf(\"#%%u.%%u\", (unsigned)%s.index, (unsigned)%s.generation);\n", access, access);
        return;
    case TY_PLAYER:
        sb_printf(o, "        if (purr_player_is_null(%s)) printf(\"no player\"); else printf(\"player %%u\", (unsigned)%s.id - 1u);\n",
                  access, access);
        return;
    case TY_QUATERNION:
        snprintf(part, sizeof part, "%s.value", access);
        gen_print_value(o, (type){TY_FLOAT4, NULL}, part);
        return;
    default:
        break;
    }

    // Vectors as (x, y, z); matrices as [c0 c1 c2], column by column.
    const int dim = type_dim(t);
    const int n = matrix_dim(t);
    if (dim >= 2) {
        sb_put(o, "        printf(\"(\");\n");
        for (int i = 0; i < dim; i++) {
            if (i) sb_put(o, "        printf(\", \");\n");
            snprintf(part, sizeof part, "%s.%s", access, xyzw[i]);
            gen_print_value(o, vector_type(type_is_float_based(t), 1), part);
        }
        sb_put(o, "        printf(\")\");\n");
    } else if (n > 0) {
        sb_put(o, "        printf(\"[\");\n");
        for (int i = 0; i < n; i++) {
            if (i) sb_put(o, "        printf(\" \");\n");
            snprintf(part, sizeof part, "%s.c%d", access, i);
            gen_print_value(o, vector_type(true, n), part);
        }
        sb_put(o, "        printf(\"]\");\n");
    }
}

static void gen_print_fields(sb *o, const decl *d, const char *base)
{
    sb_printf(o, "        printf(\" " STR_FMT " {\");\n", STR_ARG(d->name));
    for (int f = 0; f < d->fields.count; f++) {
        const field *fl = &d->fields.items[f];
        sb_printf(o, "        printf(\"%s" STR_FMT " = \");\n", f ? ", " : " ", STR_ARG(fl->name));
        char access[256];
        snprintf(access, sizeof access, "%s.%s", base, field_cname(fl));
        gen_print_value(o, fl->type, access);
    }
    sb_put(o, "        printf(\" }\");\n");
}

static void gen_api(gen *g)
{
    const program *prog = g->prog;
    sb *o = &g->c;

    sb_put(o, "void purr_world_init(purr_world *w, float dt)\n{\n    memset(w, 0, sizeof *w);\n    w->Time.dt = dt;\n");
    for (int i = 0; i < prog->singletons.count; i++) {
        decl *d = prog->singletons.items[i];
        if (!has_defaults(d)) continue;
        sb_printf(o, "    w->%s = ", type_cname(d));
        gen_value(g, o, d, NULL, 0);
        sb_put(o, ";\n");
    }
    if (prog->input) {
        sb_put(o, "    w->no_input = ");
        gen_value(g, o, prog->input, NULL, 0);
        sb_put(o, ";\n");
        sb_put(o, "    for (uint32_t i = 0; i < PURR_MAX_PLAYERS; i++) {\n");
        sb_put(o, "        w->inputs[i] = w->no_input;\n        w->previous_inputs[i] = w->no_input;\n    }\n");
    }
    sb_put(o, "    purr_system_Main(w");
    for (int i = 0; i < prog->main->params.count; i++) {
        sb_printf(o, ", &w->%s", type_cname(prog->main->params.items[i].type.decl));
    }
    sb_put(o, ");\n    purr_apply_commands(w);\n}\n\n");

    sb_put(o, "void purr_world_tick(purr_world *w)\n{\n");
    for (int i = 0; i < prog->systems.count; i++) {
        sb_printf(o, "    purr_run_" STR_FMT "(w);\n", STR_ARG(prog->systems.items[i]->name));
    }
    sb_put(o, "    purr_apply_commands(w);\n");
    if (prog->input) sb_put(o, "    memcpy(w->previous_inputs, w->inputs, sizeof w->inputs);\n");
    sb_put(o, "    w->Time.tick++;\n}\n\n");

    if (prog->input) {
        sb_printf(o, "void purr_world_set_input(purr_world *w, purr_player_id player, %s input)\n{\n",
                  type_cname(prog->input));
        sb_put(o, "    const int32_t index = purr_player_index(player);\n");
        sb_put(o, "    if (index >= 0) w->inputs[index] = input;\n}\n\n");
    }

    sb_put(o, "uint32_t purr_world_entity_count(const purr_world *w)\n{\n    uint32_t n = 0;\n");
    for (int a = 0; a < prog->archetypes.count; a++) sb_printf(o, "    n += w->%s.count;\n", arch_name(g, a));
    sb_put(o, "    (void)w;\n    return n;\n}\n\n");

    for (int c = 0; c < prog->components.count; c++) {
        const char *comp = type_cname(prog->components.items[c]);
        sb_printf(o, "%s *purr_get_%s(purr_world *w, purr_entity e)\n{\n", comp, comp);
        sb_put(o, "    purr_location loc = purr_entity_location(&w->entities, e);\n    switch (loc.archetype) {\n");
        for (int a = 0; a < prog->archetypes.count; a++) {
            if (!has_component(prog->archetypes.items[a], c)) continue;
            sb_printf(o, "    case %d: return &w->%s.%s[loc.row];\n", a, arch_name(g, a), comp);
        }
        sb_put(o, "    default: return NULL;\n    }\n}\n\n");
    }

    sb_put(o, "void purr_world_print(const purr_world *w)\n{\n");
    sb_put(o, "    printf(\"tick %d, %u entities\\n\", (int)w->Time.tick, (unsigned)purr_world_entity_count(w));\n");
    for (int s = 0; s < prog->singletons.count; s++) {
        const decl *d = prog->singletons.items[s];
        if (d->builtin) continue;
        char base[256];
        snprintf(base, sizeof base, "w->%s", type_cname(d));
        sb_put(o, "    {\n");
        gen_print_fields(o, d, base);
        sb_put(o, "        printf(\"\\n\");\n    }\n");
    }
    for (int a = 0; a < prog->archetypes.count; a++) {
        const uint64_t mask = prog->archetypes.items[a];
        const char *name = arch_name(g, a);
        sb_printf(o, "    for (uint32_t i = 0; i < w->%s.count; i++) {\n", name);
        sb_printf(o, "        printf(\"  #%%u.%%u\", (unsigned)w->%s.entity[i].index, (unsigned)w->%s.entity[i].generation);\n", name, name);
        for (int c = 0; c < prog->components.count; c++) {
            if (!has_component(mask, c)) continue;
            char base[256];
            snprintf(base, sizeof base, "w->%s.%s[i]", name, type_cname(prog->components.items[c]));
            gen_print_fields(o, prog->components.items[c], base);
        }
        sb_put(o, "        printf(\"\\n\");\n    }\n");
    }
    sb_put(o, "}\n");
}

// ---------------------------------------------------------------------------

static char *forward_slashes(const char *path)
{
    char *out = arena_alloc(strlen(path) + 1);
    for (size_t i = 0; path[i]; i++) out[i] = path[i] == '\\' ? '/' : path[i];
    return out;
}

static bool write_file(const char *path, const sb *b)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "purrc: can't write %s\n", path);
        return false;
    }
    const size_t written = fwrite(b->data, 1, b->len, f);
    fclose(f);
    if (written != b->len) {
        fprintf(stderr, "purrc: can't write %s\n", path);
        return false;
    }
    return true;
}

bool codegen(program *prog, const codegen_options *opts)
{
    gen g = {0};
    g.prog = prog;
    g.opts = opts;
    g.purr_path = forward_slashes(prog->src->path);

    sb path = {0};
    sb_printf(&path, "%s/%s.c", opts->out_dir, opts->name);
    g.c_path = forward_slashes(path.data);

    gen_header(&g);

    gen_prelude(&g);
    gen_remove_rows(&g);
    gen_moves(&g);
    gen_command_recorders(&g);
    gen_apply(&g);
    if (prog->input) gen_constructor(&g);
    gen_system_body(&g, prog->main);
    for (int i = 0; i < prog->systems.count; i++) gen_system_body(&g, prog->systems.items[i]);
    for (int i = 0; i < prog->systems.count; i++) gen_system_run(&g, prog->systems.items[i]);
    gen_api(&g);

    sb h_path = {0};
    sb_printf(&h_path, "%s/%s.h", opts->out_dir, opts->name);
    return write_file(h_path.data, &g.h) && write_file(path.data, &g.c);
}
