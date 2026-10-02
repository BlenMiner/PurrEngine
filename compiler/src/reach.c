#include <stdint.h>
#include <string.h>

#include "ast.h"
#include "lexer.h"

// See infer_reaches in ast.h.
//
// How far a chunk system reaches comes from the cells its body indexes through
// its chunk parameter. Each index is worked out axis by axis as an interval
// relative to the chunk's first cell (cells.min), or else it's unknown:
// cells.min is [min, min], cells.max is [min + 1, min + span] (less at the
// grid's edge), constants are themselves, and a loop variable that only its
// step changes goes from where it starts toward the bound its condition sets.
// Every other local holds all the values it's given, joined, worked out again
// until nothing changes; one that keeps growing grows without a bound. So the
// interval holds every cell the code could index, and sometimes more: it
// doesn't follow `if`s.
//
// Each cell it indexes gets into the chunks around its own that its interval
// spans, and together they're the chunks a task touches. Its phases are as
// few as keep two tasks that run together from touching the same chunk
// (find_phases).

#define INF ((int64_t)1 << 40)
#define NO_AXIS (-1)

typedef struct bound {
    int axis;       // NO_AXIS: a plain number; otherwise relative to the chunk's min on that axis
    int64_t lo, hi; // -INF and INF where nothing bounds it
} bound;

typedef struct value {
    int n; // -1: not known yet; 0: not an int; 1: an int; 2 or 3: an int vector
    bound c[3];
} value;

typedef struct local_value {
    const stmt *local;
    value v;
    int changes;
} local_value;

typedef struct reach {
    const decl *sys;
    const param *chunk;
    int dims;
    int span[3];
    VEC(local_value) locals;
    bool changed;
    int depth; // Constants within constants

    // What the body indexes: on each axis, the furthest before its chunk and
    // after it, and where; the chunks around its own it gets into (a bit each,
    // by tide_grid_around); and the first cell it can't tell, on each axis
    int64_t lo[3], hi[3];
    loc lo_at[3], hi_at[3];
    unsigned chunks;
    bool unknown[3];
    loc unknown_at[3];

    // With [Reach]: the chunks it gets into, and the first cell tidec can
    // tell is in a chunk it doesn't, and which
    unsigned declared_chunks;
    bool outside;
    loc outside_at;
    unsigned outside_chunk;
} reach;

static const bound UNKNOWN = {NO_AXIS, -INF, INF};

static int64_t clamped(const int64_t v)
{
    return v <= -INF ? -INF : v >= INF ? INF : v;
}

static int64_t sum_lo(const int64_t a, const int64_t b)
{
    return a <= -INF || b <= -INF ? -INF : clamped(a + b);
}

static int64_t sum_hi(const int64_t a, const int64_t b)
{
    return a >= INF || b >= INF ? INF : clamped(a + b);
}

static bool finite(const bound b)
{
    return b.lo > -INF && b.hi < INF;
}

static bound number(const int64_t v)
{
    return (bound){NO_AXIS, clamped(v), clamped(v)};
}

static bound add(const bound a, const bound b)
{
    if (a.axis != NO_AXIS && b.axis != NO_AXIS) return UNKNOWN; // Twice the min
    return (bound){a.axis != NO_AXIS ? a.axis : b.axis, sum_lo(a.lo, b.lo), sum_hi(a.hi, b.hi)};
}

static bound negate(const bound a)
{
    if (a.axis != NO_AXIS) return UNKNOWN;
    return (bound){NO_AXIS, -a.hi, -a.lo};
}

static bound subtract(const bound a, const bound b)
{
    if (b.axis == NO_AXIS) return add(a, negate(b));
    if (a.axis != b.axis) return UNKNOWN;
    // The min cancels out: a plain number
    return add((bound){NO_AXIS, a.lo, a.hi}, negate((bound){NO_AXIS, b.lo, b.hi}));
}

static bound multiply(const bound a, const bound b)
{
    if (a.axis != NO_AXIS || b.axis != NO_AXIS || !finite(a) || !finite(b)) return UNKNOWN;
    const int64_t p[4] = {a.lo * b.lo, a.lo * b.hi, a.hi * b.lo, a.hi * b.hi};
    bound r = {NO_AXIS, p[0], p[0]};
    for (int i = 1; i < 4; i++) {
        if (p[i] < r.lo) r.lo = p[i];
        if (p[i] > r.hi) r.hi = p[i];
    }
    return (bound){NO_AXIS, clamped(r.lo), clamped(r.hi)};
}

static bound divide(const bound a, const bound b)
{
    if (a.axis != NO_AXIS || b.axis != NO_AXIS || !finite(a) || !finite(b) || (b.lo <= 0 && b.hi >= 0)) return UNKNOWN;
    const int64_t q[4] = {a.lo / b.lo, a.lo / b.hi, a.hi / b.lo, a.hi / b.hi};
    bound r = {NO_AXIS, q[0], q[0]};
    for (int i = 1; i < 4; i++) {
        if (q[i] < r.lo) r.lo = q[i];
        if (q[i] > r.hi) r.hi = q[i];
    }
    return r;
}

// a % b: smaller than b either way, and not negative when a isn't.
static bound remainder_of(const bound a, const bound b)
{
    if (b.axis != NO_AXIS || !finite(b) || (b.lo <= 0 && b.hi >= 0)) return UNKNOWN;
    const int64_t m = (b.lo < 0 ? -b.lo : b.lo) > (b.hi < 0 ? -b.hi : b.hi) ? (b.lo < 0 ? -b.lo : b.lo) - 1
                                                                            : (b.hi < 0 ? -b.hi : b.hi) - 1;
    if (a.axis == NO_AXIS && a.lo >= 0) return (bound){NO_AXIS, 0, a.hi < m ? a.hi : m};
    return (bound){NO_AXIS, -m, m};
}

// a & b: between 0 and a mask that isn't negative.
static bound and_of(const bound a, const bound b)
{
    if (b.axis == NO_AXIS && b.lo >= 0 && b.hi < INF) return (bound){NO_AXIS, 0, b.hi};
    if (a.axis == NO_AXIS && a.lo >= 0 && a.hi < INF) return (bound){NO_AXIS, 0, a.hi};
    return UNKNOWN;
}

static bound join(const bound a, const bound b)
{
    if (a.axis != b.axis) return UNKNOWN;
    return (bound){a.axis, a.lo < b.lo ? a.lo : b.lo, a.hi > b.hi ? a.hi : b.hi};
}

// Math.Max: no less than either. Of a cell and a number, only how low it goes is known.
static bound larger(const bound a, const bound b)
{
    if (a.axis == b.axis) return (bound){a.axis, a.lo > b.lo ? a.lo : b.lo, a.hi > b.hi ? a.hi : b.hi};
    if (a.axis != NO_AXIS && b.axis != NO_AXIS) return UNKNOWN;
    const bound cell = a.axis != NO_AXIS ? a : b;
    return (bound){cell.axis, cell.lo, INF};
}

static bound smaller(const bound a, const bound b)
{
    if (a.axis == b.axis) return (bound){a.axis, a.lo < b.lo ? a.lo : b.lo, a.hi < b.hi ? a.hi : b.hi};
    if (a.axis != NO_AXIS && b.axis != NO_AXIS) return UNKNOWN;
    const bound cell = a.axis != NO_AXIS ? a : b;
    return (bound){cell.axis, -INF, cell.hi};
}

static bound absolute(const bound a)
{
    if (a.axis != NO_AXIS) return UNKNOWN;
    if (a.lo >= 0) return a;
    if (a.hi <= 0) return negate(a);
    return (bound){NO_AXIS, 0, -a.lo > a.hi ? -a.lo : a.hi};
}

// From a value no lower than `low` to one no higher than `high`.
static bound between(const bound low, const bound high)
{
    if (low.axis == high.axis) return (bound){low.axis, low.lo, high.hi};
    if (low.axis != NO_AXIS && high.axis != NO_AXIS) return UNKNOWN;
    if (high.axis != NO_AXIS) return (bound){high.axis, -INF, high.hi};
    return (bound){low.axis, low.lo, INF};
}

// ---------------------------------------------------------------------------
// Values

static int int_width(const type t)
{
    switch (t.kind) {
    case TY_INT: return 1;
    case TY_INT2: return 2;
    case TY_INT3: return 3;
    default: return 0;
    }
}

static value of(const int n, const bound b)
{
    value v;
    memset(&v, 0, sizeof v);
    v.n = n;
    for (int i = 0; i < (n > 0 ? n : 0); i++) v.c[i] = b;
    return v;
}

static value unknown_value(const int n)
{
    return of(n, UNKNOWN);
}

static value not_known_yet(void)
{
    return of(-1, UNKNOWN);
}

// `v` as an int vector of `n` components: a scalar goes to every one.
static value widened(const value v, const int n)
{
    if (v.n != 1 || n <= 1) return v;
    return of(n, v.c[0]);
}

typedef bound (*binary_fn)(bound, bound);

static value componentwise(value a, value b, const int n, const binary_fn fn)
{
    if (a.n < 0 || b.n < 0) return not_known_yet();
    a = widened(a, n);
    b = widened(b, n);
    if (a.n != n || b.n != n) return unknown_value(n);
    value r = of(n, UNKNOWN);
    for (int i = 0; i < n; i++) r.c[i] = fn(a.c[i], b.c[i]);
    return r;
}

static value join_values(const value a, const value b)
{
    if (a.n < 0) return b;
    if (b.n < 0) return a;
    if (a.n != b.n) return unknown_value(a.n > b.n ? a.n : b.n);
    value r = a;
    for (int i = 0; i < a.n; i++) r.c[i] = join(a.c[i], b.c[i]);
    return r;
}

static bool same_value(const value a, const value b)
{
    if (a.n != b.n) return false;
    for (int i = 0; i < (a.n > 0 ? a.n : 0); i++) {
        if (a.c[i].axis != b.c[i].axis || a.c[i].lo != b.c[i].lo || a.c[i].hi != b.c[i].hi) return false;
    }
    return true;
}

static bool is_chunk(const reach *r, const expr *e)
{
    return e && e->kind == E_NAME && e->bind == BIND_PARAM && e->param == r->chunk;
}

static local_value *local_of(reach *r, const stmt *local)
{
    for (int i = 0; i < r->locals.count; i++) {
        if (r->locals.items[i].local == local) return &r->locals.items[i];
    }
    return NULL;
}

static value eval(reach *r, const expr *e);

static value eval_constant(reach *r, const decl *k, const int n)
{
    if (!k || !k->value || r->depth > 16) return unknown_value(n);
    r->depth++;
    const value v = eval(r, k->value);
    r->depth--;
    return v.n == n ? v : unknown_value(n);
}

// int2(x, y), int3(v, z), int2(a): an int vector's components.
static value eval_construct(reach *r, const expr *e, const int n)
{
    if (n == 1 && e->ctor == CTOR_SCALAR && e->args.count == 1) {
        const value v = eval(r, e->args.items[0]);
        return v.n == 1 || v.n < 0 ? v : unknown_value(1);
    }
    if (e->ctor == CTOR_SPLAT && e->args.count == 1) return widened(eval(r, e->args.items[0]), n);
    if (e->ctor != CTOR_COMPONENTS) return unknown_value(n);
    value out = of(n, UNKNOWN);
    int k = 0;
    for (int i = 0; i < e->args.count && k < n; i++) {
        const value v = eval(r, e->args.items[i]);
        if (v.n < 0) return not_known_yet();
        const int parts = v.n > 0 ? v.n : int_width(e->args.items[i]->type);
        for (int j = 0; j < parts && k < n; j++) out.c[k++] = v.n > 0 ? v.c[j] : UNKNOWN;
        if (parts == 0) k++; // Not an int: that component's unknown
    }
    return out;
}

// Math.Max, Min, Clamp and Abs on ints: the rest are unknown.
static value eval_math(reach *r, const expr *e, const int n)
{
    const char *f = e->c_callee ? e->c_callee : "";
    value a[3];
    for (int i = 0; i < e->args.count && i < 3; i++) {
        a[i] = widened(eval(r, e->args.items[i]), n);
        if (a[i].n < 0) return not_known_yet();
    }
    if (!strncmp(f, "tide_max_i", 10) && e->args.count == 2) return componentwise(a[0], a[1], n, larger);
    if (!strncmp(f, "tide_min_i", 10) && e->args.count == 2) return componentwise(a[0], a[1], n, smaller);
    if (!strncmp(f, "tide_clamp_i", 12) && e->args.count == 3) {
        return componentwise(componentwise(a[0], a[1], n, larger), a[2], n, smaller);
    }
    if (!strncmp(f, "tide_abs_i", 10) && e->args.count == 1 && a[0].n == n) {
        value out = a[0];
        for (int i = 0; i < n; i++) out.c[i] = absolute(a[0].c[i]);
        return out;
    }
    return unknown_value(n);
}

static value eval(reach *r, const expr *e)
{
    if (!e) return unknown_value(0);
    const int n = int_width(e->type);
    if (n == 0) return unknown_value(0);
    switch (e->kind) {
    case E_INT: return of(1, number(e->int_value));
    case E_NAME:
        if (e->bind == BIND_LOCAL) {
            const local_value *l = local_of(r, e->local);
            return l ? l->v : not_known_yet();
        }
        if (e->bind == BIND_CONST) return eval_constant(r, e->constant, n);
        return unknown_value(n);
    case E_MEMBER: {
        if (e->constant) return eval_constant(r, e->constant, n); // Combat.MAX_RANGE
        if (is_chunk(r, e->object)) { // cells.min, cells.max
            if (str_eq_c(e->member, "min")) {
                value v = of(n, UNKNOWN);
                for (int i = 0; i < n; i++) v.c[i] = (bound){i, 0, 0};
                return v;
            }
            if (str_eq_c(e->member, "max")) {
                value v = of(n, UNKNOWN);
                for (int i = 0; i < n; i++) v.c[i] = (bound){i, 1, r->span[i]};
                return v;
            }
            return unknown_value(n); // Its size: the grid's
        }
        if (e->swizzle_len > 0) {
            const value o = eval(r, e->object);
            if (o.n < 0) return not_known_yet();
            value v = of(n, UNKNOWN);
            for (int i = 0; i < e->swizzle_len && i < n; i++) v.c[i] = o.n > e->swizzle[i] ? o.c[e->swizzle[i]] : UNKNOWN;
            return v;
        }
        return unknown_value(n);
    }
    case E_UNARY: {
        const value v = eval(r, e->lhs ? e->lhs : e->rhs);
        if (v.n < 0) return v;
        if (e->op == T_PLUS) return v;
        if (e->op != T_MINUS || v.n != n) return unknown_value(n);
        value out = v;
        for (int i = 0; i < n; i++) out.c[i] = negate(v.c[i]);
        return out;
    }
    case E_BINARY: {
        if (e->method) return unknown_value(n); // A struct's operator
        const value a = eval(r, e->lhs);
        const value b = eval(r, e->rhs);
        switch (e->op) {
        case T_PLUS: return componentwise(a, b, n, add);
        case T_MINUS: return componentwise(a, b, n, subtract);
        case T_STAR: return componentwise(a, b, n, multiply);
        case T_SLASH: return componentwise(a, b, n, divide);
        case T_PERCENT: return componentwise(a, b, n, remainder_of);
        case T_AMP: return componentwise(a, b, n, and_of);
        default: return a.n < 0 || b.n < 0 ? not_known_yet() : unknown_value(n);
        }
    }
    case E_CONDITIONAL: {
        const value a = widened(eval(r, e->lhs), n);
        const value b = widened(eval(r, e->rhs), n);
        return join_values(a, b);
    }
    case E_CALL:
    case E_METHOD:
        if (e->call == CALL_CONSTRUCT) return eval_construct(r, e, n);
        if (e->call == CALL_BUILTIN) return eval_math(r, e, n);
        return unknown_value(n);
    default: return unknown_value(n);
    }
}

// ---------------------------------------------------------------------------
// The body

// A local's value is every value it's given. One that keeps changing grows
// without a bound wherever it still grows.
static void define(reach *r, const stmt *local, const value v)
{
    if (v.n < 0) return; // Not known yet: the next pass
    local_value *l = local_of(r, local);
    if (!l) {
        const local_value fresh = {local, v, 0};
        vec_push(r->locals, fresh);
        r->changed = true;
        return;
    }
    value joined = join_values(l->v, v);
    if (same_value(joined, l->v)) return;
    if (++l->changes > 3) {
        for (int i = 0; i < joined.n && joined.n == l->v.n; i++) {
            if (joined.c[i].lo < l->v.c[i].lo) joined.c[i].lo = -INF;
            if (joined.c[i].hi > l->v.c[i].hi) joined.c[i].hi = INF;
        }
    }
    l->v = joined;
    r->changed = true;
}

static bool names_local(const expr *e, const stmt *local)
{
    return e && e->kind == E_NAME && e->bind == BIND_LOCAL && e->local == local;
}

// Which way a for's step moves its variable: 1 up, -1 down, 0 neither or not known.
static int step_way(reach *r, const stmt *step, const stmt *var)
{
    if (!step || step->kind != S_ASSIGN || !names_local(step->target, var)) return 0;
    if (step->op == T_PLUS_PLUS) return 1;
    if (step->op == T_MINUS_MINUS) return -1;
    if (step->op != T_PLUS_ASSIGN && step->op != T_MINUS_ASSIGN) return 0;
    const value by = eval(r, step->value);
    if (by.n != 1 || by.c[0].axis != NO_AXIS) return 0;
    const int up = step->op == T_PLUS_ASSIGN ? 1 : -1;
    if (by.c[0].lo >= 1) return up;
    if (by.c[0].hi <= -1) return -up;
    return 0;
}

// The bound a loop's condition sets on its variable, as `var op value`: the
// first comparison of it, among those joined by &&.
static bool loop_bound(const expr *cond, const stmt *var, tok_kind *op, const expr **other)
{
    if (!cond || cond->kind != E_BINARY) return false;
    if (cond->op == T_AND) return loop_bound(cond->lhs, var, op, other) || loop_bound(cond->rhs, var, op, other);
    if (cond->op != T_LT && cond->op != T_LE && cond->op != T_GT && cond->op != T_GE) return false;
    if (names_local(cond->lhs, var)) {
        *op = cond->op;
        *other = cond->rhs;
        return true;
    }
    if (names_local(cond->rhs, var)) { // 10 > i: i < 10
        *op = cond->op == T_LT ? T_GT : cond->op == T_LE ? T_GE : cond->op == T_GT ? T_LT : T_LE;
        *other = cond->lhs;
        return true;
    }
    return false;
}

// A for's variable that only its step changes, as an int: from where it starts
// toward the bound its condition sets.
static value loop_range(reach *r, const stmt *s)
{
    const stmt *var = s->init;
    const value start = eval(r, var->value);
    if (start.n < 0) return start;
    if (start.n != 1) return unknown_value(1);
    const int way = step_way(r, s->step, var);
    if (way == 0) return unknown_value(1);
    tok_kind op;
    const expr *other = NULL;
    const bool bounded = loop_bound(s->cond, var, &op, &other) && (way > 0 ? op == T_LT || op == T_LE : op == T_GT || op == T_GE);
    if (!bounded) {
        return of(1, way > 0 ? (bound){start.c[0].axis, start.c[0].lo, INF} : (bound){start.c[0].axis, -INF, start.c[0].hi});
    }
    const value limit = eval(r, other);
    if (limit.n < 0) return limit;
    if (limit.n != 1) return unknown_value(1);
    bound end = limit.c[0];
    if (op == T_LT) end.hi = sum_hi(end.hi, -1);
    if (op == T_GT) end.lo = sum_lo(end.lo, 1);
    return of(1, way > 0 ? between(start.c[0], end) : between(end, start.c[0]));
}

static bool is_loop_counter(const stmt *s)
{
    return s->init && s->init->kind == S_VAR && s->init->loop_var && !s->init->is_mut && int_width(s->init->type) == 1
        && s->init->value;
}

typedef enum pass {
    DEFINE, // Locals' values
    INDEX,  // The cells it indexes
} pass;

static void walk_stmt(reach *r, const stmt *s, pass p);

// Which chunk, before its own (-1), its own (0) or after (1), a cell `cells`
// from the chunk's first is in on an axis of `span` cells.
static int chunk_along(const int64_t cells, const int span)
{
    const int64_t c = cells >= 0 ? cells / span : -((-cells + span - 1) / span);
    return c < -1 ? -1 : c > 1 ? 1 : (int)c;
}

// The chunks around a chunk, a bit each by tide_grid_around, that a box of
// chunks spans: from `from` to `to` on each axis, -1 to 1.
static unsigned chunk_box(const int from[3], const int to[3])
{
    unsigned bits = 0;
    for (int z = from[2]; z <= to[2]; z++) {
        for (int y = from[1]; y <= to[1]; y++) {
            for (int x = from[0]; x <= to[0]; x++) bits |= 1u << ((x + 1) + 3 * (y + 1) + 9 * (z + 1));
        }
    }
    return bits;
}

static void note_index(reach *r, const expr *e)
{
    value at = eval(r, e->lhs);
    if (at.n < 0) at = unknown_value(r->dims);
    int from[3] = {0, 0, 0};
    int to[3] = {0, 0, 0};
    bool known = true;
    for (int i = 0; i < r->dims; i++) {
        const bound b = at.n == r->dims ? at.c[i] : UNKNOWN;
        if (b.axis != i || !finite(b)) {
            known = false;
            if (!r->unknown[i]) {
                r->unknown[i] = true;
                r->unknown_at[i] = e->at;
            }
            continue;
        }
        from[i] = chunk_along(b.lo, r->span[i]);
        to[i] = chunk_along(b.hi, r->span[i]);
        if (b.lo < r->lo[i]) {
            r->lo[i] = b.lo;
            r->lo_at[i] = e->at;
        }
        if (b.hi > r->hi[i]) {
            r->hi[i] = b.hi;
            r->hi_at[i] = e->at;
        }
    }
    if (!known) return;
    const unsigned chunks = chunk_box(from, to);
    r->chunks |= chunks;
    const unsigned outside = chunks & ~(r->declared_chunks | 1u << 13);
    if (outside && !r->outside) {
        r->outside = true;
        r->outside_at = e->at;
        r->outside_chunk = (unsigned)__builtin_ctz(outside);
    }
}

static void walk_expr(reach *r, const expr *e, const pass p)
{
    if (!e) return;
    if (p == INDEX && e->kind == E_INDEX && is_chunk(r, e->object)) note_index(r, e);
    if (p == DEFINE && e->kind == E_IS && e->binding) define(r, e->binding, unknown_value(int_width(e->binding->type)));
    walk_expr(r, e->object, p);
    walk_expr(r, e->lhs, p);
    walk_expr(r, e->rhs, p);
    walk_expr(r, e->cond, p);
    for (int i = 0; i < e->args.count; i++) walk_expr(r, e->args.items[i], p);
    for (int i = 0; i < e->inits.count; i++) walk_expr(r, e->inits.items[i].value, p);
    if (e->block) walk_stmt(r, e->block, p);
}

static void walk_assign(reach *r, const stmt *s)
{
    const expr *t = s->target;
    if (!t) return;
    const stmt *local = t->kind == E_NAME && t->bind == BIND_LOCAL ? t->local : NULL;
    if (t->kind == E_MEMBER && t->swizzle_len > 0 && t->object && t->object->kind == E_NAME && t->object->bind == BIND_LOCAL) {
        local = t->object->local; // to.x = ...: that component
    }
    if (!local) return;
    const int n = int_width(local->type);
    if (n == 0) return;
    value now;
    if (s->op == T_ASSIGN) {
        now = eval(r, s->value);
    } else {
        const value before = eval(r, t);
        const value by = s->op == T_PLUS_PLUS || s->op == T_MINUS_MINUS ? of(1, number(1)) : eval(r, s->value);
        const tok_kind op = compound_op(s->op);
        const int w = int_width(t->type);
        now = op == T_PLUS ? componentwise(before, by, w, add)
            : op == T_MINUS ? componentwise(before, by, w, subtract)
            : op == T_STAR ? componentwise(before, by, w, multiply)
            : op == T_SLASH ? componentwise(before, by, w, divide)
            : op == T_PERCENT ? componentwise(before, by, w, remainder_of)
            : op == T_AMP ? componentwise(before, by, w, and_of)
                          : unknown_value(w);
    }
    if (now.n < 0) return;
    if (t->kind == E_MEMBER) { // One component: the others as they are
        const local_value *l = local_of(r, local);
        value whole = l ? l->v : of(n, UNKNOWN);
        if (whole.n != n) whole = of(n, UNKNOWN);
        const value part = widened(now, t->swizzle_len);
        for (int i = 0; i < t->swizzle_len; i++) whole.c[t->swizzle[i]] = part.n > i ? part.c[i] : UNKNOWN;
        now = whole;
    }
    define(r, local, now.n == n ? now : widened(now, n));
}

static void walk_stmt(reach *r, const stmt *s, const pass p)
{
    if (!s) return;
    switch (s->kind) {
    case S_BLOCK:
        for (int i = 0; i < s->stmts.count; i++) walk_stmt(r, s->stmts.items[i], p);
        break;
    case S_IF:
    case S_WHILE:
        walk_expr(r, s->cond, p);
        walk_stmt(r, s->then_stmt, p);
        walk_stmt(r, s->else_stmt, p);
        break;
    case S_FOR: {
        const bool counter = is_loop_counter(s);
        if (counter) {
            walk_expr(r, s->init->value, p);
            if (p == DEFINE) define(r, s->init, loop_range(r, s));
        } else {
            walk_stmt(r, s->init, p);
        }
        walk_expr(r, s->cond, p);
        if (counter && s->step && s->step->kind == S_ASSIGN && names_local(s->step->target, s->init)) {
            walk_expr(r, s->step->value, p); // Its own change is in its range
        } else {
            walk_stmt(r, s->step, p);
        }
        walk_stmt(r, s->then_stmt, p);
        break;
    }
    case S_FOREACH:
        walk_expr(r, s->value, p);
        if (p == DEFINE) define(r, s, unknown_value(int_width(s->type)));
        walk_stmt(r, s->then_stmt, p);
        break;
    case S_VAR:
        walk_expr(r, s->value, p);
        if (p == DEFINE && int_width(s->type)) define(r, s, s->value ? eval(r, s->value) : unknown_value(int_width(s->type)));
        break;
    case S_ASSIGN:
        walk_expr(r, s->target, p);
        walk_expr(r, s->value, p);
        if (p == DEFINE) walk_assign(r, s);
        break;
    case S_SWITCH:
        walk_expr(r, s->cond, p);
        for (int i = 0; i < s->cases.count; i++) {
            for (int k = 0; k < s->cases.items[i].body.count; k++) walk_stmt(r, s->cases.items[i].body.items[k], p);
        }
        break;
    case S_RETURN:
    case S_EXPR:
    case S_FAIL:
        walk_expr(r, s->value, p);
        break;
    default: break;
    }
}

// ---------------------------------------------------------------------------
// Each chunk system

static const char AXES[] = "xyz";

static int touched(const unsigned chunks)
{
    return __builtin_popcount(chunks | 1u << 13);
}

static int spanned(const decl *sys, const int dims)
{
    int chunks = 1;
    for (int i = 0; i < dims; i++) chunks *= 1 + (sys->reach_before[i] > 0) + (sys->reach_after[i] > 0);
    return chunks;
}

// How far it reaches, like "1 cell before and after its chunk on x, and 1 before it on y".
static void describe(sb *out, const decl *sys, const int dims)
{
    int parts = 0;
    for (int i = 0; i < dims; i++) {
        const int before = sys->reach_before[i];
        const int after = sys->reach_after[i];
        if (!before && !after) continue;
        sb_put(out, parts++ ? ", and " : "");
        if (before && after && before == after) sb_printf(out, "%d cell%s before and after its chunk", before, before == 1 ? "" : "s");
        else if (before && after) sb_printf(out, "%d cell%s before its chunk and %d after", before, before == 1 ? "" : "s", after);
        else sb_printf(out, "%d cell%s %s it", before + after, before + after == 1 ? "" : "s", before ? "before" : "after");
        sb_printf(out, " on %c", AXES[i]);
    }
    if (!parts) sb_put(out, "it stays in its chunk");
    else if (touched(sys->reach_chunks) < spanned(sys, dims)) {
        sb_printf(out, ", into %d of those %d chunks", touched(sys->reach_chunks), spanned(sys, dims));
    }
}

// The chunk around its own at `around` (tide_grid_around), like "x -1, y +1".
static void put_chunk(sb *out, const unsigned around, const int dims)
{
    const int offset[3] = {(int)(around % 3) - 1, (int)(around / 3 % 3) - 1, (int)(around / 9) - 1};
    int parts = 0;
    for (int i = 0; i < dims; i++) {
        if (offset[i]) sb_printf(out, "%s%c %+d", parts++ ? ", " : "", AXES[i], offset[i]);
    }
}

// As few phases as keep tasks that run together from touching the same chunk.
// A chunk's phase is `weight` times its position, axis by axis, added up,
// modulo the phases: two chunks share one when that's a multiple of the
// phases for the difference between them. So for every difference between
// two chunks a task touches, it mustn't be, and the first number of phases
// with weights that work, counting up, is the one. 27 always does, with
// weights 1, 3 and 9: the chunks of a 3 by 3 by 3 block, each in its own.
static void find_phases(const unsigned chunks, const int dims, int weight[3], int *phases)
{
    int diff[125][3];
    int count = 0;
    const unsigned all = chunks | 1u << 13;
    for (int a = 0; a < 27; a++) {
        for (int b = 0; b < 27; b++) {
            if (!(all >> a & 1u) || !(all >> b & 1u) || a == b) continue;
            const int d[3] = {a % 3 - b % 3, a / 3 % 3 - b / 3 % 3, a / 9 - b / 9};
            bool seen = false;
            for (int k = 0; k < count && !seen; k++) seen = diff[k][0] == d[0] && diff[k][1] == d[1] && diff[k][2] == d[2];
            if (seen) continue;
            memcpy(diff[count++], d, sizeof d);
        }
    }
    for (int k = 1; k <= 27; k++) {
        for (int w0 = 0; w0 < k; w0++) {
            for (int w1 = 0; w1 < (dims > 1 ? k : 1); w1++) {
                for (int w2 = 0; w2 < (dims > 2 ? k : 1); w2++) {
                    bool apart = true;
                    for (int d = 0; d < count && apart; d++) apart = (w0 * diff[d][0] + w1 * diff[d][1] + w2 * diff[d][2]) % k != 0;
                    if (!apart) continue;
                    weight[0] = w0;
                    weight[1] = w1;
                    weight[2] = w2;
                    *phases = k;
                    return;
                }
            }
        }
    }
}

// [Reach]'s reach: n cells every way, or the cells around each cell it
// touches. False, reported, when it's past a chunk.
static bool written_reach(reach *r, decl *sys, const decl *grid)
{
    const int all[3] = {-1, -1, -1};
    const int none[3] = {1, 1, 1};
    if (sys->reach >= 0) {
        int shortest = r->span[0];
        for (int i = 1; i < r->dims; i++) {
            if (r->span[i] < shortest) shortest = r->span[i];
        }
        if (sys->reach > shortest) {
            diag_error(sys->reach_at, "'" STR_FMT "' reaches %d cells past its chunk, and " STR_FMT "'s chunks are %d cells across",
                       STR_ARG(sys->name), sys->reach, STR_ARG(grid->name), shortest);
            diag_note("a chunk system reaches at most one chunk past its own: at most %d here", shortest);
            return false;
        }
        int from[3] = {0, 0, 0};
        int to[3] = {0, 0, 0};
        for (int i = 0; i < r->dims; i++) {
            sys->reach_before[i] = sys->reach_after[i] = sys->reach;
            from[i] = sys->reach ? all[i] : 0;
            to[i] = sys->reach ? none[i] : 0;
        }
        sys->reach_chunks = chunk_box(from, to);
        return true;
    }
    for (int k = 0; k + 2 < sys->reach_cells.count; k += 3) {
        int from[3] = {0, 0, 0};
        int to[3] = {0, 0, 0};
        for (int i = 0; i < r->dims; i++) {
            const int cell = sys->reach_cells.items[k + i];
            if (cell < -r->span[i] || cell > r->span[i]) {
                diag_error(sys->reach_at, "'" STR_FMT "' reaches %d cells past its chunk on %c, and " STR_FMT "'s chunks are %d cells across",
                           STR_ARG(sys->name), cell < 0 ? -cell : cell, AXES[i], STR_ARG(grid->name), r->span[i]);
                diag_note("a chunk system reaches at most one chunk past its own");
                return false;
            }
            if (-cell > sys->reach_before[i]) sys->reach_before[i] = -cell;
            if (cell > sys->reach_after[i]) sys->reach_after[i] = cell;
            from[i] = cell < 0 ? -1 : 0;
            to[i] = cell > 0 ? 1 : 0;
        }
        sys->reach_chunks |= chunk_box(from, to);
    }
    return true;
}

static void infer_one(program *prog, decl *sys)
{
    reach r;
    memset(&r, 0, sizeof r);
    r.sys = sys;
    r.chunk = &sys->params.items[sys->chunk_param - 1];
    const decl *grid = r.chunk->type.decl;
    r.dims = grid->dims;
    int cell, shift[3];
    grid_shape(grid, &cell, shift);
    for (int i = 0; i < 3; i++) {
        r.span[i] = 1 << shift[i];
        r.lo[i] = 0;
        r.hi[i] = r.span[i] - 1;
    }
    if (sys->has_reach) {
        if (!written_reach(&r, sys, grid)) return;
        r.declared_chunks = sys->reach_chunks;
    }
    for (int pass_count = 0; pass_count < 64; pass_count++) {
        r.changed = false;
        walk_stmt(&r, sys->body, DEFINE);
        if (!r.changed) break;
    }
    walk_stmt(&r, sys->body, INDEX);

    const field *f = &r.chunk->chunk_of->fields.items[r.chunk->chunk_field];
    bool known = true;
    for (int i = 0; i < r.dims; i++) known &= !r.unknown[i];
    if (!known && !sys->has_reach) {
        int axis = 0;
        while (!r.unknown[axis]) axis++;
        diag_error(r.unknown_at[axis], "can't work out how far this cell is from the chunk on %c", AXES[axis]);
        diag_note("index the cells from '" STR_FMT ".min' and '" STR_FMT ".max', constants, and loop variables that go "
                  "between them, and tidec works out how far '" STR_FMT "' reaches; or say it above the system, with "
                  "[Reach(n)] for n cells past its chunk every way, or [Reach(%s, ...)] for the cells around "
                  "each cell it touches",
                  STR_ARG(r.chunk->name), STR_ARG(r.chunk->name), STR_ARG(sys->name), r.dims == 3 ? "int3(0, -1, 0)" : "int2(0, -1)");
        return;
    }
    if (known) {
        for (int i = 0; i < r.dims; i++) {
            const int64_t before = -r.lo[i];
            const int64_t after = r.hi[i] - (r.span[i] - 1);
            if (before > r.span[i] || after > r.span[i]) {
                const bool back = before > r.span[i];
                diag_error(back ? r.lo_at[i] : r.hi_at[i], "this cell can be %lld cells %s the chunk on %c, and " STR_FMT "." STR_FMT
                           "'s chunks are %d cells across", (long long)(back ? before : after), back ? "before" : "after",
                           AXES[i], STR_ARG(r.chunk->chunk_of->name), STR_ARG(f->name), r.span[i]);
                diag_note("a chunk system reaches at most one chunk past its own");
                return;
            }
        }
        if (sys->has_reach) {
            diag_warning(sys->reach_at, "tidec works out how far '" STR_FMT "' reaches, so [Reach] isn't needed", STR_ARG(sys->name));
            const fix remove = {FIX_REMOVE_REACH, sys->reach_at, NULL, NULL, sys, NULL, {0}};
            vec_push(prog->fixes, remove);
        }
        for (int i = 0; i < 3; i++) {
            sys->reach_before[i] = i < r.dims ? (int)-r.lo[i] : 0;
            sys->reach_after[i] = i < r.dims ? (int)(r.hi[i] - (r.span[i] - 1)) : 0;
        }
        sys->reach_chunks = r.chunks & ~(1u << 13);
        if (sys->has_reach) {
            sb what = {0};
            describe(&what, sys, r.dims);
            diag_note("it reaches %s", what.data);
        }
    } else {
        // [Reach], where tidec can't work it out. Cells it can tell are past it read 0, and writes there do nothing.
        bool warned = false;
        for (int i = 0; i < r.dims && !warned; i++) {
            const int64_t before = -r.lo[i];
            const int64_t after = r.hi[i] - (r.span[i] - 1);
            if (before <= sys->reach_before[i] && after <= sys->reach_after[i]) continue;
            const bool back = before > sys->reach_before[i];
            const int64_t far = back ? before : after;
            diag_warning(back ? r.lo_at[i] : r.hi_at[i], "this cell can be %lld cells %s the chunk on %c, past its [Reach]",
                         (long long)far, back ? "before" : "after", AXES[i]);
            diag_note("past its reach, reads give 0 and writes do nothing; reach as far in [Reach]");
            warned = true;
        }
        if (!warned && r.outside) {
            sb chunk = {0};
            put_chunk(&chunk, r.outside_chunk, r.dims);
            diag_warning(r.outside_at, "this cell can be in the chunk at %s from its own, which its [Reach] doesn't get into", chunk.data);
            diag_note("past its reach, reads give 0 and writes do nothing; add a cell that gets there to [Reach]");
        }
    }
    find_phases(sys->reach_chunks, r.dims, sys->reach_weight, &sys->reach_phases);
}

void infer_reaches(program *prog)
{
    for (int i = 0; i < prog->systems.count; i++) {
        decl *sys = prog->systems.items[i];
        if (sys->chunk_param && sys->body) infer_one(prog, sys);
    }
}
