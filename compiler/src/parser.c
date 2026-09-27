#include <setjmp.h>

#include "ast.h"

// Recursive descent. Stops at the first syntax error: later errors after a bad
// parse are usually noise.

typedef struct parser {
    token *toks;
    int pos;
    jmp_buf fail;
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

static stmt *parse_block(parser *p)
{
    const token *open = expect(p, T_LBRACE, "'{'");
    stmt *s = new_stmt(S_BLOCK, open->at);
    while (!at(p, T_RBRACE)) {
        if (at(p, T_EOF)) fail_at(p, peek(p), "'}'");
        vec_push(s->stmts, parse_stmt(p));
    }
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
    if (!accept(p, T_VAR)) s->type_name = expect_ident(p, "'var' or a type")->text;
    const token *name = expect_ident(p, "variable name");
    s->name = name->text;
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
            prm.type_name = expect_ident(p, "parameter type")->text;
            prm.name = expect_ident(p, "parameter name")->text;
            vec_push(d->params, prm);
        } while (accept(p, T_COMMA));
    }
    expect(p, T_RPAREN, "')' after parameters");
    d->body = parse_block(p);
}

// component Name { Type field; ... }, and the same for singletons and inputs.
static decl *parse_data_decl(parser *p, const decl_kind kind)
{
    const char *what = kind == DECL_COMPONENT ? "component name" : kind == DECL_SINGLETON ? "singleton name" : "input name";
    const token *name = expect_ident(p, what);
    decl *d = new_decl(kind, name);
    expect(p, T_LBRACE, "'{'");
    while (!at(p, T_RBRACE)) {
        // A constructor is the type's own name followed by '('.
        if (at(p, T_IDENT) && str_eq(peek(p)->text, name->text) && peek_at(p, 1)->kind == T_LPAREN) {
            parse_constructor(p, d, advance(p));
            continue;
        }
        const token *type_tok = expect_ident(p, "field type or '}'");
        const token *field_name = expect_ident(p, "field name");
        field f = {field_name->text, type_tok->text, field_name->at, {0}, NULL};
        if (accept(p, T_ASSIGN)) f.default_value = parse_expr(p);
        expect(p, T_SEMI, "';' after field");
        vec_push(d->fields, f);
    }
    advance(p);
    return d;
}

// system Name(Time time, mut Transform trs, with Player, without Dead) { ... }
static decl *parse_system(parser *p)
{
    const token *name = expect_ident(p, "system name");
    decl *d = new_decl(DECL_SYSTEM, name);
    expect(p, T_LPAREN, "'('");
    if (!at(p, T_RPAREN)) {
        do {
            param prm = {0};
            prm.at = peek(p)->at;
            if (accept(p, T_WITH)) {
                prm.mode = PARAM_WITH;
                prm.type_name = expect_ident(p, "component name after 'with'")->text;
            } else if (accept(p, T_WITHOUT)) {
                prm.mode = PARAM_WITHOUT;
                prm.type_name = expect_ident(p, "component name after 'without'")->text;
            } else {
                prm.mode = accept(p, T_MUT) ? PARAM_MUT : PARAM_READ;
                prm.type_name = expect_ident(p, "parameter type")->text;
                prm.name = expect_ident(p, "parameter name")->text;
            }
            vec_push(d->params, prm);
        } while (accept(p, T_COMMA));
    }
    expect(p, T_RPAREN, "')' after parameters");
    d->body = parse_block(p);
    return d;
}

program *parse(const source *src, token *toks)
{
    parser p = {toks, 0, {0}};
    program *prog = NEW(program);
    prog->src = src;

    if (setjmp(p.fail)) return NULL;

    while (!at(&p, T_EOF)) {
        const token *t = advance(&p);
        switch (t->kind) {
        case T_COMPONENT: vec_push(prog->decls, parse_data_decl(&p, DECL_COMPONENT)); break;
        case T_SINGLETON: vec_push(prog->decls, parse_data_decl(&p, DECL_SINGLETON)); break;
        case T_SYSTEM: vec_push(prog->decls, parse_system(&p)); break;
        case T_LBRACKET:
            diag_error(t->at, "attributes aren't supported yet");
            return NULL;
        default:
            // `input` is only a keyword at the start of a declaration, so it can
            // still name parameters and locals.
            if (t->kind == T_IDENT && str_eq_c(t->text, "input") && at(&p, T_IDENT)) {
                vec_push(prog->decls, parse_data_decl(&p, DECL_INPUT));
                break;
            }
            p.pos--;
            fail_at(&p, t, "'component', 'singleton', 'input' or 'system'");
        }
    }
    return prog;
}
