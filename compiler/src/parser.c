#include <setjmp.h>
#include <string.h>

#include "ast.h"

// Recursive descent. For the compiler, stops at the first syntax error: later
// errors after a bad parse are usually noise. For editors (`recover`), skips the
// broken statement, field or declaration and carries on, so a half-typed file
// still gives most of its structure.

typedef struct parser {
    token *toks;
    int pos;
    jmp_buf fail; // Where a syntax error goes: the innermost place that can recover.
    bool recover;
    // File level. In the struct rather than locals: they change between setjmp and longjmp.
    unit *unit;
    VEC(attribute) pending; // Attributes waiting for the next declaration
    bool seen_decl;
} parser;

static token *peek(const parser *p)
{
    return &p->toks[p->pos];
}

static token *peek_at(const parser *p, const int offset)
{
    token *t = &p->toks[p->pos];
    for (int i = 0; i < offset && t->kind != T_EOF; i++) t++;
    return t;
}

static bool at(const parser *p, const tok_kind kind)
{
    return peek(p)->kind == kind;
}

static token *advance(parser *p)
{
    token *t = peek(p);
    if (t->kind != T_EOF) p->pos++;
    return t;
}

static bool accept(parser *p, const tok_kind kind)
{
    if (!at(p, kind)) return false;
    advance(p);
    return true;
}

static _Noreturn void fail_at(parser *p, const token *t, const char *what)
{
    if (t->kind == T_EOF) {
        diag_error(t->at, "expected %s, found end of file", what);
    } else {
        diag_error(t->at, "expected %s, found '" STR_FMT "'", what, STR_ARG(t->text));
    }
    longjmp(p->fail, 1);
}

static token *expect(parser *p, const tok_kind kind, const char *what)
{
    if (!at(p, kind)) fail_at(p, peek(p), what);
    return advance(p);
}

static token *expect_ident(parser *p, const char *what)
{
    return expect(p, T_IDENT, what);
}

// ---------------------------------------------------------------------------
// Names

// "a" and "b" as "a.b", in the arena.
static str join_names(const str a, const str b)
{
    char *text = arena_alloc((size_t)a.len + (size_t)b.len + 2);
    memcpy(text, a.ptr, (size_t)a.len);
    text[a.len] = '.';
    memcpy(text + a.len + 1, b.ptr, (size_t)b.len);
    return (str){text, a.len + 1 + b.len};
}

// Name or Namespace.Name, as in `mut Combat.Health health` or `[After(Physics.Integrate)]`.
static qname parse_qname(parser *p, const char *what)
{
    const token *first = expect_ident(p, what);
    qname q = {first->text, first->at, first->at, NULL};
    while (at(p, T_DOT) && peek_at(p, 1)->kind == T_IDENT) {
        advance(p);
        const token *part = advance(p);
        q.text = join_names(q.text, part->text);
        q.name_at = part->at;
    }
    return q;
}

// ---------------------------------------------------------------------------
// Expressions

static expr *new_expr(const expr_kind kind, const loc at)
{
    expr *e = NEW(expr);
    e->kind = kind;
    e->at = at;
    e->spawn_archetype = -1;
    return e;
}

static expr *parse_expr(parser *p);

static void parse_args(parser *p, expr *call)
{
    expect(p, T_LPAREN, "'('");
    if (!at(p, T_RPAREN)) {
        do {
            vec_push(call->args, parse_expr(p));
        } while (accept(p, T_COMMA));
    }
    expect(p, T_RPAREN, "')' after arguments");
}

// Transform { position = float3(0, 1, 0), scale = float3(1, 1, 1) }
static void parse_literal_inits(parser *p, expr *lit)
{
    expect(p, T_LBRACE, "'{'");
    while (!at(p, T_RBRACE)) {
        const token *name = expect_ident(p, "field name");
        expect(p, T_ASSIGN, "'=' after field name");
        const field_init init = {name->text, name->at, parse_expr(p), NULL};
        vec_push(lit->inits, init);
        if (!accept(p, T_COMMA)) break;
    }
    expect(p, T_RBRACE, "'}' after fields");
}

static expr *parse_primary(parser *p)
{
    const token *t = peek(p);
    switch (t->kind) {
    case T_INT: {
        advance(p);
        expr *e = new_expr(E_INT, t->at);
        e->text = t->text;
        str digits = t->text;
        int base = 10;
        if (digits.len > 2 && digits.ptr[0] == '0' && (digits.ptr[1] == 'x' || digits.ptr[1] == 'X')) base = 16;
        if (digits.len > 2 && digits.ptr[0] == '0' && (digits.ptr[1] == 'b' || digits.ptr[1] == 'B')) base = 2;
        if (base != 10) {
            digits.ptr += 2;
            digits.len -= 2;
        }
        // Decimal literals go up to INT32_MAX. Hex and binary literals can use
        // all 32 bits and are read as the int's bit pattern: 0xFFFFFFFF is -1.
        const int64_t limit = base == 10 ? 2147483647LL : 4294967295LL;
        int64_t v = 0;
        for (int i = 0; i < digits.len; i++) {
            const char ch = digits.ptr[i];
            if (ch == '_') continue; // Digit separator.
            const int d = ch <= '9' ? ch - '0' : (ch | 0x20) - 'a' + 10;
            v = v * base + d;
            if (v > limit) {
                diag_error(t->at, "integer '" STR_FMT "' is too large for int", STR_ARG(t->text));
                longjmp(p->fail, 1);
            }
        }
        e->int_value = base == 10 ? v : (int64_t)(int32_t)(uint32_t)v;
        return e;
    }
    case T_FLOAT: {
        advance(p);
        expr *e = new_expr(E_FLOAT, t->at);
        e->text = t->text;
        return e;
    }
    case T_TRUE:
    case T_FALSE: {
        advance(p);
        expr *e = new_expr(E_BOOL, t->at);
        e->bool_value = t->kind == T_TRUE;
        return e;
    }
    case T_STRING: {
        advance(p);
        expr *e = new_expr(E_STRING, t->at);
        e->text = t->text;
        return e;
    }
    case T_LPAREN: {
        advance(p);
        expr *e = parse_expr(p);
        expect(p, T_RPAREN, "')'");
        return e;
    }
    case T_IDENT: {
        advance(p);
        if (at(p, T_LPAREN)) {
            expr *e = new_expr(E_CALL, t->at);
            e->name = t->text;
            parse_args(p, e);
            return e;
        }
        if (at(p, T_LBRACE)) {
            expr *e = new_expr(E_LITERAL, t->at);
            e->name = t->text;
            e->qual_at = t->at;
            parse_literal_inits(p, e);
            return e;
        }
        expr *e = new_expr(E_NAME, t->at);
        e->name = t->text;
        return e;
    }
    default:
        fail_at(p, t, "an expression");
    }
}

static expr *parse_postfix(parser *p)
{
    expr *e = parse_primary(p);
    while (at(p, T_DOT)) {
        advance(p);
        const token *name = expect_ident(p, "member name after '.'");
        if (at(p, T_LPAREN)) {
            expr *m = new_expr(E_METHOD, name->at);
            m->object = e;
            m->name = name->text;
            parse_args(p, m);
            e = m;
        } else {
            expr *m = new_expr(E_MEMBER, name->at);
            m->object = e;
            m->member = name->text;
            e = m;
        }
    }
    // Combat.Health { ... }: a component literal with a qualified name.
    if (at(p, T_LBRACE) && e->kind == E_MEMBER) {
        str text = {NULL, 0};
        const expr *first = e;
        bool names_only = true;
        for (const expr *part = e; part; part = part->kind == E_MEMBER ? part->object : NULL) {
            if (part->kind != E_MEMBER && part->kind != E_NAME) names_only = false;
            first = part;
        }
        if (names_only) {
            text = first->name;
            // Rebuild "A.B.C" from the chain, innermost first.
            const expr *chain[16];
            int n = 0;
            for (const expr *part = e; part->kind == E_MEMBER && n < 16; part = part->object) chain[n++] = part;
            for (int i = n - 1; i >= 0; i--) text = join_names(text, chain[i]->member);
            expr *lit = new_expr(E_LITERAL, e->at);
            lit->name = text;
            lit->qual_at = first->at;
            parse_literal_inits(p, lit);
            return lit;
        }
    }
    return e;
}

static expr *parse_unary(parser *p)
{
    if (at(p, T_NOT) || at(p, T_MINUS) || at(p, T_TILDE)) {
        const token *op = advance(p);
        expr *e = new_expr(E_UNARY, op->at);
        e->op = op->kind;
        e->lhs = parse_unary(p);
        return e;
    }
    return parse_postfix(p);
}

// C# precedence, lowest first.
static int binary_precedence(const tok_kind kind)
{
    switch (kind) {
    case T_OR: return 1;
    case T_AND: return 2;
    case T_PIPE: return 3;
    case T_CARET: return 4;
    case T_AMP: return 5;
    case T_EQ:
    case T_NE: return 6;
    case T_LT:
    case T_LE:
    case T_GT:
    case T_GE: return 7;
    case T_SHL:
    case T_SHR: return 8;
    case T_PLUS:
    case T_MINUS: return 9;
    case T_STAR:
    case T_SLASH:
    case T_PERCENT: return 10;
    default: return 0;
    }
}

// Precedence climbing; all binary operators are left-associative.
static expr *parse_binary(parser *p, const int min_prec)
{
    expr *lhs = parse_unary(p);
    for (;;) {
        const int prec = binary_precedence(peek(p)->kind);
        if (prec == 0 || prec < min_prec) return lhs;
        const token *op = advance(p);
        expr *e = new_expr(E_BINARY, op->at);
        e->op = op->kind;
        e->lhs = lhs;
        e->rhs = parse_binary(p, prec + 1);
        lhs = e;
    }
}

static expr *parse_expr(parser *p)
{
    return parse_binary(p, 1);
}

// ---------------------------------------------------------------------------
// Statements

static stmt *new_stmt(const stmt_kind kind, const loc at)
{
    stmt *s = NEW(stmt);
    s->kind = kind;
    s->at = at;
    return s;
}

static stmt *parse_stmt(parser *p);

// Keywords that only start declarations, and `input` or `view` at the start of a
// line: where recovery can safely pick up again.
static bool at_decl_start(const parser *p)
{
    const token *t = peek(p);
    if (t->kind == T_COMPONENT || t->kind == T_SINGLETON || t->kind == T_SYSTEM) return true;
    if (t->kind == T_LBRACKET && t->at.col == 1) return true; // Attributes
    return t->kind == T_IDENT && t->at.col == 1 && peek_at(p, 1)->kind == T_IDENT
        && (str_eq_c(t->text, "input") || str_eq_c(t->text, "view") || str_eq_c(t->text, "namespace")
            || str_eq_c(t->text, "using"));
}

// After a syntax error in a statement or field: skips to the end of it (a ';'
// at this level), stopping before the '}' that closes the enclosing block and
// before the next declaration.
static void skip_statement(parser *p)
{
    int depth = 0;
    while (!at(p, T_EOF) && !at_decl_start(p)) {
        const tok_kind kind = peek(p)->kind;
        if (kind == T_RBRACE && depth == 0) return;
        advance(p);
        if (kind == T_LBRACE) depth++;
        else if (kind == T_RBRACE) depth--;
        else if (kind == T_SEMI && depth == 0) return;
    }
}

// Runs one statement or field parser, recovering from a syntax error in it.
// Returns false if it failed; the parser then stands after the broken part.
#define RECOVERING(p, action)                                                  \
    do {                                                                       \
        jmp_buf outer_;                                                        \
        memcpy(outer_, (p)->fail, sizeof outer_);                              \
        if (setjmp((p)->fail) == 0) {                                          \
            action;                                                            \
        } else {                                                               \
            skip_statement(p);                                                 \
        }                                                                      \
        memcpy((p)->fail, outer_, sizeof outer_);                              \
    } while (0)

static stmt *parse_block(parser *p)
{
    const token *open = expect(p, T_LBRACE, "'{'");
    stmt *s = new_stmt(S_BLOCK, open->at);
    while (!at(p, T_RBRACE)) {
        if (at(p, T_EOF) || (p->recover && at_decl_start(p))) {
            if (!p->recover) fail_at(p, peek(p), "'}'");
            diag_error(peek(p)->at, "expected '}' to close the block on line %d", open->at.line);
            s->end = peek(p)->at;
            return s;
        }
        if (p->recover) RECOVERING(p, vec_push(s->stmts, parse_stmt(p)));
        else vec_push(s->stmts, parse_stmt(p));
    }
    s->end = peek(p)->at;
    advance(p);
    return s;
}

static bool is_assign_op(const tok_kind kind)
{
    return kind == T_ASSIGN || kind == T_PLUS_ASSIGN || kind == T_MINUS_ASSIGN
        || kind == T_STAR_ASSIGN || kind == T_SLASH_ASSIGN || kind == T_PERCENT_ASSIGN
        || kind == T_SHL_ASSIGN || kind == T_SHR_ASSIGN || kind == T_AMP_ASSIGN
        || kind == T_PIPE_ASSIGN || kind == T_CARET_ASSIGN;
}

// [mut] (var | Type) name = value;
static stmt *parse_var(parser *p)
{
    const token *first = peek(p);
    stmt *s = new_stmt(S_VAR, first->at);
    s->is_mut = accept(p, T_MUT);
    if (!accept(p, T_VAR)) {
        const token *type_tok = expect_ident(p, "'var' or a type");
        s->type_name = type_tok->text;
        s->type_at = type_tok->at;
    }
    const token *name = expect_ident(p, "variable name");
    s->name = name->text;
    s->name_at = name->at;
    if (!at(p, T_ASSIGN)) {
        diag_error(name->at, "local variable '" STR_FMT "' needs an initial value", STR_ARG(s->name));
        longjmp(p->fail, 1);
    }
    advance(p);
    s->value = parse_expr(p);
    expect(p, T_SEMI, "';'");
    return s;
}

static stmt *parse_stmt(parser *p)
{
    const token *t = peek(p);
    switch (t->kind) {
    case T_LBRACE:
        return parse_block(p);

    case T_IF: {
        advance(p);
        stmt *s = new_stmt(S_IF, t->at);
        expect(p, T_LPAREN, "'(' after 'if'");
        s->cond = parse_expr(p);
        expect(p, T_RPAREN, "')' after condition");
        s->then_stmt = parse_stmt(p);
        if (accept(p, T_ELSE)) s->else_stmt = parse_stmt(p);
        return s;
    }

    case T_RETURN: {
        advance(p);
        stmt *s = new_stmt(S_RETURN, t->at);
        if (!at(p, T_SEMI)) {
            diag_error(peek(p)->at, "systems don't return values; use 'return;'");
            longjmp(p->fail, 1);
        }
        advance(p);
        return s;
    }

    case T_MUT:
    case T_VAR:
        return parse_var(p);

    default:
        // `Type name = ...` declares a local: two identifiers in a row.
        if (t->kind == T_IDENT && peek_at(p, 1)->kind == T_IDENT) return parse_var(p);

        expr *e = parse_expr(p);
        if (is_assign_op(peek(p)->kind)) {
            const token *op = advance(p);
            stmt *s = new_stmt(S_ASSIGN, op->at);
            s->target = e;
            s->op = op->kind;
            s->value = parse_expr(p);
            expect(p, T_SEMI, "';'");
            return s;
        }
        stmt *s = new_stmt(S_EXPR, e->at);
        s->value = e;
        expect(p, T_SEMI, "';'");
        return s;
    }
}

// ---------------------------------------------------------------------------
// Declarations

static decl *new_decl(const decl_kind kind, const token *name)
{
    decl *d = NEW(decl);
    d->kind = kind;
    d->name = name->text;
    d->at = name->at;
    return d;
}

// Name(Type name, ...) { ... } inside an input declaration.
static void parse_constructor(parser *p, decl *d, const token *name)
{
    if (d->kind != DECL_INPUT) {
        diag_error(name->at, "only inputs have constructors for now");
        longjmp(p->fail, 1);
    }
    if (d->body) {
        diag_error(name->at, "an input has one constructor");
        longjmp(p->fail, 1);
    }
    d->body_at = name->at;
    expect(p, T_LPAREN, "'('");
    if (!at(p, T_RPAREN)) {
        do {
            param prm = {0};
            prm.at = peek(p)->at;
            prm.type_at = peek(p)->at;
            prm.type_name = expect_ident(p, "parameter type")->text;
            prm.name_at = peek(p)->at;
            prm.name = expect_ident(p, "parameter name")->text;
            vec_push(d->params, prm);
        } while (accept(p, T_COMMA));
    }
    expect(p, T_RPAREN, "')' after parameters");
    d->body = parse_block(p);
}

// Type name; [= default];
static void parse_field(parser *p, decl *d)
{
    const token *type_tok = expect_ident(p, "field type or '}'");
    const token *field_name = expect_ident(p, "field name");
    field f = {field_name->text, type_tok->text, field_name->at, {0}, NULL, type_tok->at};
    if (accept(p, T_ASSIGN)) f.default_value = parse_expr(p);
    expect(p, T_SEMI, "';' after field");
    vec_push(d->fields, f);
}

// component Name { Type field; ... }, and the same for singletons and inputs.
static decl *parse_data_decl(parser *p, const decl_kind kind)
{
    const char *what = kind == DECL_COMPONENT ? "component name" : kind == DECL_SINGLETON ? "singleton name" : "input name";
    const token *name = expect_ident(p, what);
    decl *d = new_decl(kind, name);
    expect(p, T_LBRACE, "'{'");
    while (!at(p, T_RBRACE)) {
        if (p->recover && (at(p, T_EOF) || at_decl_start(p))) {
            diag_error(peek(p)->at, "expected '}' to close '" STR_FMT "'", STR_ARG(name->text));
            d->end = peek(p)->at;
            return d;
        }
        // A constructor is the type's own name followed by '('.
        if (at(p, T_IDENT) && str_eq(peek(p)->text, name->text) && peek_at(p, 1)->kind == T_LPAREN) {
            parse_constructor(p, d, advance(p));
            continue;
        }
        if (p->recover) RECOVERING(p, parse_field(p, d));
        else parse_field(p, d);
    }
    d->end = peek(p)->at;
    advance(p);
    return d;
}

// system Name(Time time, mut Transform trs, with Player, without Dead) { ... }
// Views have the same shape: view Name(Transform trs, with Player) { ... }
static decl *parse_system(parser *p, const bool is_view)
{
    const token *name = expect_ident(p, is_view ? "view name" : "system name");
    decl *d = new_decl(DECL_SYSTEM, name);
    d->is_view = is_view;
    expect(p, T_LPAREN, "'('");
    if (!at(p, T_RPAREN)) {
        do {
            param prm = {0};
            prm.at = peek(p)->at;
            const char *what = "parameter type";
            if (accept(p, T_WITH)) {
                prm.mode = PARAM_WITH;
                what = "component name after 'with'";
            } else if (accept(p, T_WITHOUT)) {
                prm.mode = PARAM_WITHOUT;
                what = "component name after 'without'";
            } else {
                prm.mode = accept(p, T_MUT) ? PARAM_MUT : PARAM_READ;
            }
            const qname type = parse_qname(p, what);
            prm.type_name = type.text;
            prm.type_qual_at = type.at;
            prm.type_at = type.name_at;
            if (prm.mode != PARAM_WITH && prm.mode != PARAM_WITHOUT) {
                prm.name_at = peek(p)->at;
                prm.name = expect_ident(p, "parameter name")->text;
            }
            vec_push(d->params, prm);
        } while (accept(p, T_COMMA));
    }
    expect(p, T_RPAREN, "')' after parameters");
    d->body = parse_block(p);
    d->end = d->body->end;
    return d;
}

// After a syntax error outside any body: skips to the next declaration. A
// failed declaration always consumed its first token, so this makes progress.
static void skip_declaration(parser *p)
{
    while (!at(p, T_EOF) && !at_decl_start(p)) advance(p);
}

// [Before(A.B), After(C)], one or more groups before a declaration.
static void parse_attributes(parser *p)
{
    expect(p, T_LBRACKET, "'['");
    do {
        attribute a = {0};
        const token *name = expect_ident(p, "attribute name");
        a.name = name->text;
        a.at = name->at;
        if (accept(p, T_LPAREN)) {
            if (!at(p, T_RPAREN)) {
                do {
                    vec_push(a.args, parse_qname(p, "a name"));
                } while (accept(p, T_COMMA));
            }
            expect(p, T_RPAREN, "')' after the attribute's arguments");
        }
        vec_push(p->pending, a);
    } while (accept(p, T_COMMA));
    expect(p, T_RBRACKET, "']' after attributes");
}

// `namespace Game.Combat;` and `using Physics;` come before a file's declarations.
static void parse_file_header(parser *p, const token *keyword)
{
    const bool is_namespace = str_eq_c(keyword->text, "namespace");
    if (p->seen_decl) {
        diag_error(keyword->at, "'" STR_FMT "' goes at the top of the file, before any declaration", STR_ARG(keyword->text));
        longjmp(p->fail, 1);
    }
    const qname name = parse_qname(p, "a namespace name");
    expect(p, T_SEMI, "';'");
    if (is_namespace) {
        if (p->unit->ns.len > 0) {
            diag_error(keyword->at, "a file has one namespace; it's already '" STR_FMT "'", STR_ARG(p->unit->ns));
            diag_note("put the declarations of another namespace in their own file");
            return;
        }
        p->unit->ns = name.text;
        p->unit->ns_at = name.at;
    } else {
        vec_push(p->unit->usings, name.text);
        vec_push(p->unit->using_at, name.at);
    }
}

program *program_new(void)
{
    return NEW(program);
}

bool parse_file(program *prog, const source *src, token *toks, const bool recover)
{
    parser p = {toks, 0, {0}, recover, NULL, {0}, false};
    p.unit = NEW(unit);
    p.unit->src = src;
    vec_push(prog->units, p.unit);

    if (!recover && setjmp(p.fail)) return false;

    while (!at(&p, T_EOF)) {
        if (recover && setjmp(p.fail)) {
            p.pending.count = 0;
            skip_declaration(&p);
            continue;
        }
        const token *t = peek(&p);
        // Contextual keywords: only special at the start of a declaration or a
        // file, so they can still name parameters and locals.
        const bool followed_by_name = peek_at(&p, 1)->kind == T_IDENT;
        if (t->kind == T_IDENT && followed_by_name && (str_eq_c(t->text, "namespace") || str_eq_c(t->text, "using"))) {
            advance(&p);
            parse_file_header(&p, t);
            continue;
        }
        if (t->kind == T_LBRACKET) {
            parse_attributes(&p);
            continue;
        }
        advance(&p);
        decl *d;
        if (t->kind == T_COMPONENT) d = parse_data_decl(&p, DECL_COMPONENT);
        else if (t->kind == T_SINGLETON) d = parse_data_decl(&p, DECL_SINGLETON);
        else if (t->kind == T_SYSTEM) d = parse_system(&p, false);
        else if (t->kind == T_IDENT && followed_by_name && str_eq_c(t->text, "input")) d = parse_data_decl(&p, DECL_INPUT);
        else if (t->kind == T_IDENT && followed_by_name && str_eq_c(t->text, "view")) d = parse_system(&p, true);
        else fail_at(&p, t, "'component', 'singleton', 'input', 'system' or 'view'"); // Consumed, so recovery skips it
        d->unit = p.unit;
        d->attributes.items = p.pending.items;
        d->attributes.count = p.pending.count;
        d->attributes.cap = p.pending.cap;
        p.pending.items = NULL;
        p.pending.count = p.pending.cap = 0;
        p.seen_decl = true;
        vec_push(prog->decls, d);
    }
    if (p.pending.count > 0) {
        diag_error(p.pending.items[0].at, "attributes go right before a declaration");
        if (!recover) return false;
    }
    return true;
}
