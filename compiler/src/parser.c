#include <setjmp.h>
#include <stdio.h>
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
    VEC(attribute) field_pending; // Attributes waiting for the next field
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

// A type: a name, maybe qualified, or List<T>, whose text is "List<T>".
static qname parse_type(parser *p, const char *what)
{
    qname q = parse_qname(p, what);
    if (!at(p, T_LT) || !str_eq_c(q.text, "List")) return q;
    advance(p);
    const qname element = parse_type(p, "the list's element type, like 'List<int>'");
    if (at(p, T_SHR)) {
        diag_error(peek(p)->at, "a list can't hold lists yet");
        diag_note("hold structs that have the lists instead");
        longjmp(p->fail, 1);
    }
    expect(p, T_GT, "'>' after the list's element type");
    char *text = arena_alloc((size_t)element.text.len + 7);
    snprintf(text, (size_t)element.text.len + 7, "List<" STR_FMT ">", STR_ARG(element.text));
    q.text = (str){text, element.text.len + 6};
    return q;
}

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
    case T_THIS:
        advance(p);
        return new_expr(E_THIS, t->at);
    case T_DEFAULT:
        advance(p);
        return new_expr(E_DEFAULT, t->at);
    case T_INTERP: {
        // $"a {x} b {y:F2} c": the tokens are `$"a {`, x, `} b {`, y, `:F2`, `} c"`.
        advance(p);
        expr *e = new_expr(E_INTERP, t->at);
        vec_push(e->parts, ((str){t->text.ptr + 2, t->text.len - 3}));
        bool open = t->text.ptr[t->text.len - 1] == '{';
        while (open) {
            vec_push(e->args, parse_expr(p));
            str format = {"", 0};
            if (at(p, T_INTERP_FORMAT)) {
                const token *f = advance(p);
                format = (str){f->text.ptr + 1, f->text.len - 1};
            }
            vec_push(e->formats, format);
            const token *part = expect(p, T_INTERP_PART, "'}' to end the value in the text");
            vec_push(e->parts, ((str){part->text.ptr + 1, part->text.len - 2}));
            open = part->text.ptr[part->text.len - 1] == '{';
        }
        return e;
    }
    case T_LPAREN: {
        advance(p);
        expr *e = parse_expr(p);
        expect(p, T_RPAREN, "')'");
        return e;
    }
    case T_LBRACKET: { // [a, b, c]: a list
        advance(p);
        expr *e = new_expr(E_LIST, t->at);
        if (!at(p, T_RBRACKET)) {
            do {
                vec_push(e->args, parse_expr(p));
            } while (accept(p, T_COMMA) && !at(p, T_RBRACKET));
        }
        expect(p, T_RBRACKET, "']' after the list's elements");
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
    while (at(p, T_DOT) || at(p, T_LBRACKET)) {
        if (at(p, T_LBRACKET)) { // items[i]
            const token *open = advance(p);
            expr *index = new_expr(E_INDEX, open->at);
            index->object = e;
            index->lhs = parse_expr(p);
            expect(p, T_RBRACKET, "']' after the index");
            e = index;
            continue;
        }
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

// cond ? a : b binds looser than every binary operator and groups to the
// right, as in C#: a ? b : c ? d : e is a ? b : (c ? d : e).
static expr *parse_expr(parser *p)
{
    expr *cond = parse_binary(p, 1);
    if (!at(p, T_QUESTION)) return cond;
    const token *question = advance(p);
    expr *e = new_expr(E_CONDITIONAL, question->at);
    e->cond = cond;
    e->lhs = parse_expr(p);
    if (at(p, T_INTERP_FORMAT)) { // $"{a ? b : c}": the ':' started a format
        diag_error(peek(p)->at, "in text, '?:' goes in parentheses: '{(a ? b : c)}'");
        diag_note("a ':' after a value in text starts its format, like '{x:F2}'");
        longjmp(p->fail, 1);
    }
    expect(p, T_COLON, "':' and the value for when the condition is false");
    e->rhs = parse_expr(p);
    return e;
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

// Contextual keywords: special only where a declaration or a file header can
// start, so they can still name parameters and locals.
static bool is_decl_word(const str text)
{
    return str_eq_c(text, "input") || str_eq_c(text, "view") || str_eq_c(text, "struct") || str_eq_c(text, "event")
        || str_eq_c(text, "enum") || str_eq_c(text, "scene") || str_eq_c(text, "local") || str_eq_c(text, "namespace")
        || str_eq_c(text, "using") || str_eq_c(text, "extern");
}

// Keywords that only start declarations, and the contextual ones at the start
// of a line: where recovery can safely pick up again.
bool attributes_before_field(const token *toks, int i)
{
    do { // [A(...)] [B] ...: skip to after the last ']'
        int depth = 0;
        for (; toks[i].kind != T_EOF; i++) {
            if (toks[i].kind == T_LBRACKET) depth++;
            else if (toks[i].kind == T_RBRACKET && --depth == 0) break;
        }
        if (toks[i].kind == T_EOF) return false;
        i++;
    } while (toks[i].kind == T_LBRACKET);
    const token *t = &toks[i];
    return t->kind == T_IDENT && (toks[i + 1].kind == T_IDENT || toks[i + 1].kind == T_DOT) && !is_decl_word(t->text);
}

// [mut] Type Name(: a method or function, rather than a field or a local.
static bool at_method(const parser *p)
{
    int i = at(p, T_MUT) ? 1 : 0;
    if (peek_at(p, i)->kind != T_IDENT || is_decl_word(peek_at(p, i)->text)) return false; // view Name(...)
    i++;
    while (peek_at(p, i)->kind == T_DOT && peek_at(p, i + 1)->kind == T_IDENT) i += 2;
    if (peek_at(p, i)->kind == T_LT) { // List<Item> Name(...)
        i++;
        while (peek_at(p, i)->kind == T_IDENT || peek_at(p, i)->kind == T_DOT) i++;
        if (peek_at(p, i)->kind != T_GT) return false;
        i++;
    }
    return peek_at(p, i)->kind == T_IDENT && peek_at(p, i + 1)->kind == T_LPAREN;
}

// Where recovery can pick up again: a declaration's start. `Type Name(` at
// column 1 starts a function, except in a type's body (`functions` false),
// where it's a method.
static bool at_decl_start_or_function(const parser *p, const bool functions)
{
    const token *t = peek(p);
    if (t->kind == T_COMPONENT || t->kind == T_SINGLETON || t->kind == T_SYSTEM) return true;
    if (t->kind == T_LBRACKET && t->at.col == 1) return !attributes_before_field(p->toks, p->pos);
    if (t->kind == T_IDENT && t->at.col == 1 && peek_at(p, 1)->kind == T_IDENT && is_decl_word(t->text)) return true;
    if (t->kind == T_IDENT && t->at.col == 1 && peek_at(p, 1)->kind == T_LPAREN && str_eq_c(t->text, "event")) return true;
    if (t->kind == T_IDENT && t->at.col == 1 && str_eq_c(t->text, "local")
        && (peek_at(p, 1)->kind == T_COMPONENT || peek_at(p, 1)->kind == T_SINGLETON || peek_at(p, 1)->kind == T_SYSTEM)) {
        return true;
    }
    return functions && (t->kind == T_IDENT || t->kind == T_MUT) && t->at.col == 1 && at_method(p);
}

static bool at_decl_start(const parser *p)
{
    return at_decl_start_or_function(p, true);
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
        const qname type = parse_type(p, "'var' or a type");
        s->type_name = type.text;
        s->type_at = type.name_at;
        s->type_qual_at = type.at;
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

// switch (value) { case A: ... break; case B: case C: ... break; default: ... break; }
// Labels in a row share one section.
static stmt *parse_switch(parser *p)
{
    const token *keyword = advance(p);
    stmt *s = new_stmt(S_SWITCH, keyword->at);
    expect(p, T_LPAREN, "'(' after 'switch'");
    s->cond = parse_expr(p);
    expect(p, T_RPAREN, "')' after the switch's value");
    const token *open = expect(p, T_LBRACE, "'{' and the switch's cases");
    while (!at(p, T_RBRACE)) {
        if (at(p, T_EOF) || (p->recover && at_decl_start(p))) {
            if (!p->recover) fail_at(p, peek(p), "'}'");
            diag_error(peek(p)->at, "expected '}' to close the switch on line %d", open->at.line);
            s->end = peek(p)->at;
            return s;
        }
        if (!at(p, T_CASE) && !at(p, T_DEFAULT)) fail_at(p, peek(p), "'case' or 'default'");
        switch_case section = {0};
        while (at(p, T_CASE) || at(p, T_DEFAULT)) {
            const token *label = advance(p);
            vec_push(section.label_at, label->at);
            expr *value = label->kind == T_CASE ? parse_expr(p) : NULL;
            vec_push(section.labels, value);
            expect(p, T_COLON, label->kind == T_CASE ? "':' after the case's value" : "':' after 'default'");
        }
        while (!at(p, T_CASE) && !at(p, T_DEFAULT) && !at(p, T_RBRACE) && !at(p, T_EOF)
               && !(p->recover && at_decl_start(p))) {
            if (p->recover) RECOVERING(p, vec_push(section.body, parse_stmt(p)));
            else vec_push(section.body, parse_stmt(p));
        }
        vec_push(s->cases, section);
    }
    s->end = peek(p)->at;
    advance(p);
    return s;
}

// i++, --i, x = y, x += y or a call: a statement without its ';', as a for's
// step and as the start of most statements.
static stmt *parse_simple(parser *p)
{
    const token *t = peek(p);
    if (t->kind == T_PLUS_PLUS || t->kind == T_MINUS_MINUS) { // ++i
        advance(p);
        stmt *s = new_stmt(S_ASSIGN, t->at);
        s->target = parse_expr(p);
        s->op = t->kind;
        s->value = new_expr(E_INT, t->at);
        s->value->int_value = 1;
        return s;
    }
    expr *e = parse_expr(p);
    if (is_assign_op(peek(p)->kind) || at(p, T_PLUS_PLUS) || at(p, T_MINUS_MINUS)) {
        const token *op = advance(p);
        stmt *s = new_stmt(S_ASSIGN, op->at);
        s->target = e;
        s->op = op->kind;
        if (op->kind == T_PLUS_PLUS || op->kind == T_MINUS_MINUS) {
            s->value = new_expr(E_INT, op->at);
            s->value->int_value = 1;
        } else {
            s->value = parse_expr(p);
        }
        return s;
    }
    stmt *s = new_stmt(S_EXPR, e->at);
    s->value = e;
    return s;
}

// for (init; cond; step) body, each part optional: for (var i = 0; i < n; i++)
static stmt *parse_for(parser *p)
{
    const token *keyword = advance(p);
    stmt *s = new_stmt(S_FOR, keyword->at);
    expect(p, T_LPAREN, "'(' after 'for'");
    if (!accept(p, T_SEMI)) {
        const token *t = peek(p);
        int next = 1;
        while (t->kind == T_IDENT && peek_at(p, next)->kind == T_DOT && peek_at(p, next + 1)->kind == T_IDENT) next += 2;
        const bool list = t->kind == T_IDENT && str_eq_c(t->text, "List") && peek_at(p, 1)->kind == T_LT;
        if (t->kind == T_MUT || t->kind == T_VAR || list || (t->kind == T_IDENT && peek_at(p, next)->kind == T_IDENT)) {
            s->init = parse_var(p); // Takes the ';'
            s->init->loop_var = true;
        } else {
            s->init = parse_simple(p);
            expect(p, T_SEMI, "';' after the for's start");
        }
    }
    if (!at(p, T_SEMI)) s->cond = parse_expr(p);
    expect(p, T_SEMI, "';' after the for's condition");
    if (!at(p, T_RPAREN)) s->step = parse_simple(p);
    expect(p, T_RPAREN, "')' after the for's step");
    s->then_stmt = parse_stmt(p);
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
        if (!at(p, T_SEMI)) s->value = parse_expr(p); // Only methods return values; the checker says so
        expect(p, T_SEMI, "';'");
        return s;
    }

    case T_MUT:
    case T_VAR:
        return parse_var(p);

    case T_SWITCH:
        return parse_switch(p);

    case T_BREAK:
    case T_CONTINUE: {
        advance(p);
        stmt *s = new_stmt(t->kind == T_BREAK ? S_BREAK : S_CONTINUE, t->at);
        expect(p, T_SEMI, "';'");
        return s;
    }

    case T_WHILE: {
        advance(p);
        stmt *s = new_stmt(S_WHILE, t->at);
        expect(p, T_LPAREN, "'(' after 'while'");
        s->cond = parse_expr(p);
        expect(p, T_RPAREN, "')' after the condition");
        s->then_stmt = parse_stmt(p);
        return s;
    }

    case T_FOR:
        return parse_for(p);

    case T_FOREACH: {
        // foreach (var name in list) or foreach (Type name in list)
        advance(p);
        stmt *s = new_stmt(S_FOREACH, t->at);
        expect(p, T_LPAREN, "'(' after 'foreach'");
        if (!accept(p, T_VAR)) {
            const qname type = parse_type(p, "'var' or the elements' type");
            s->type_name = type.text;
            s->type_at = type.name_at;
            s->type_qual_at = type.at;
        }
        const token *name = expect_ident(p, "the variable for each element");
        s->name = name->text;
        s->name_at = name->at;
        if (!at(p, T_IDENT) || !str_eq_c(peek(p)->text, "in")) fail_at(p, peek(p), "'in' and the list");
        advance(p);
        s->value = parse_expr(p);
        expect(p, T_RPAREN, "')' after the list");
        s->then_stmt = parse_stmt(p);
        return s;
    }

    default: {
        // `Type name = ...` declares a local: two identifiers in a row, the
        // first maybe qualified, as in `Combat.Stats stats = ...`.
        int next = 1;
        while (t->kind == T_IDENT && peek_at(p, next)->kind == T_DOT && peek_at(p, next + 1)->kind == T_IDENT) next += 2;
        if (t->kind == T_IDENT && peek_at(p, next)->kind == T_IDENT) return parse_var(p);
        if (t->kind == T_IDENT && str_eq_c(t->text, "List") && peek_at(p, 1)->kind == T_LT) return parse_var(p);

        stmt *s = parse_simple(p);
        // A call's block: Foldout("Audio") { ... }, GUILayout.Horizontal() { ... }
        if (s->kind == S_EXPR && at(p, T_LBRACE) && (s->value->kind == E_CALL || s->value->kind == E_METHOD)) {
            s->value->block = parse_block(p);
            return s;
        }
        expect(p, T_SEMI, "';'");
        return s;
    }
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

// Sample() { ... } inside an input declaration, maybe taking local singletons.
// The input's own name instead of Sample is the constructor it used to be.
static void parse_sample(parser *p, decl *d, const token *name)
{
    const bool spelled_sample = str_eq_c(name->text, "Sample") || str_eq_c(name->text, "sample");
    if (d->kind != DECL_INPUT) {
        if (spelled_sample) diag_error(name->at, "only inputs have Sample");
        else diag_error(name->at, "only inputs have methods; give fields defaults instead of a constructor");
        longjmp(p->fail, 1);
    }
    if (d->body) {
        diag_error(name->at, "an input has one Sample");
        longjmp(p->fail, 1);
    }
    if (str_eq_c(name->text, "sample")) {
        diag_error(name->at, "methods use PascalCase: 'Sample()'");
    } else if (!spelled_sample) {
        diag_error(name->at, "inputs read the devices in a method: 'Sample()'");
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

// Sanitize() { ... } inside an input declaration.
static void parse_sanitize(parser *p, decl *d, const token *name)
{
    if (d->kind != DECL_INPUT) {
        diag_error(name->at, "only inputs have Sanitize");
        longjmp(p->fail, 1);
    }
    if (d->sanitize) {
        diag_error(name->at, "an input has one Sanitize");
        longjmp(p->fail, 1);
    }
    if (name->text.ptr[0] == 's') diag_error(name->at, "methods use PascalCase: 'Sanitize()'");
    d->sanitize_at = name->at;
    expect(p, T_LPAREN, "'('");
    expect(p, T_RPAREN, "')': Sanitize takes no parameters, it works on the input's fields");
    d->sanitize = parse_block(p);
}

// [Clamp(-1, 1)] before a field. Unlike a declaration's attributes, the
// arguments are values.
static void parse_field_attributes(parser *p)
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
                    vec_push(a.values, parse_expr(p));
                } while (accept(p, T_COMMA));
            }
            expect(p, T_RPAREN, "')' after the attribute's arguments");
        }
        vec_push(p->field_pending, a);
    } while (accept(p, T_COMMA));
    expect(p, T_RBRACKET, "']' after attributes");
}

// Type name; [= default];
static void parse_field(parser *p, decl *d)
{
    const qname type = parse_type(p, "field type or '}'");
    const token *field_name = expect_ident(p, "field name");
    field f = {0};
    f.name = field_name->text;
    f.at = field_name->at;
    f.type_name = type.text;
    f.type_at = type.name_at;
    f.type_qual_at = type.at;
    f.attributes.items = p->field_pending.items;
    f.attributes.count = p->field_pending.count;
    f.attributes.cap = p->field_pending.cap;
    p->field_pending.items = NULL;
    p->field_pending.count = p->field_pending.cap = 0;
    if (accept(p, T_ASSIGN)) f.default_value = parse_expr(p);
    expect(p, T_SEMI, "';' after field");
    vec_push(d->fields, f);
}

// (Type name, ...): a method's, function's or operator's parameters.
static void parse_routine_params(parser *p, decl *m)
{
    expect(p, T_LPAREN, "'('");
    if (!at(p, T_RPAREN)) {
        do {
            param prm = {0};
            prm.at = peek(p)->at;
            prm.mode = accept(p, T_MUT) ? PARAM_MUT : PARAM_READ;
            // `in Stats stats`: `in` before a type and a name (the checker says it's only for extern functions)
            const tok_kind after = peek_at(p, 2)->kind;
            if (prm.mode == PARAM_READ && at(p, T_IDENT) && str_eq_c(peek(p)->text, "in") && peek_at(p, 1)->kind == T_IDENT
                && (after == T_IDENT || after == T_DOT || after == T_LT)) {
                advance(p);
                prm.mode = PARAM_IN;
            }
            prm.function_param = true;
            const qname type = parse_type(p, "parameter type");
            prm.type_name = type.text;
            prm.type_qual_at = type.at;
            prm.type_at = type.name_at;
            prm.name_at = peek(p)->at;
            prm.name = expect_ident(p, "parameter name")->text;
            vec_push(m->params, prm);
        } while (accept(p, T_COMMA));
    }
    expect(p, T_RPAREN, "')' after parameters");
}

// (Type name, ...) { ... }: the rest of a method, function or operator.
static void parse_routine_rest(parser *p, decl *m, const token *name)
{
    parse_routine_params(p, m);
    m->body_at = name->at;
    m->body = parse_block(p);
    m->end = m->body->end;
}

// extern float Noise(float x, float y);: a function written in C, which the
// game's C files or libraries define.
static decl *parse_extern(parser *p)
{
    const qname ret = parse_type(p, "return type");
    const token *name = expect_ident(p, "function name");
    decl *m = new_decl(DECL_FUNCTION, name);
    m->unit = p->unit;
    m->is_extern = true;
    m->return_type_name = ret.text;
    m->return_type_at = ret.name_at;
    m->return_type_qual_at = ret.at;
    parse_routine_params(p, m);
    m->body_at = name->at;
    if (at(p, T_LBRACE)) {
        diag_error(peek(p)->at, "an extern function has no body: its code is in C");
        diag_note("end it with ';', like 'extern float Noise(float x);', or remove 'extern' to write it in PurrLang");
        longjmp(p->fail, 1);
    }
    m->end = expect(p, T_SEMI, "';' after the extern function: its code is in C")->at;
    return m;
}

// [mut] ReturnType Name(Type name, ...) { ... }: a method of `owner`, or with
// no owner, a function. The checker says where methods are allowed.
static decl *parse_method(parser *p, decl *owner)
{
    const bool is_mut = accept(p, T_MUT);
    const qname ret = parse_type(p, "return type");
    const token *name = expect_ident(p, owner ? "method name" : "function name");
    decl *m = new_decl(owner ? DECL_METHOD : DECL_FUNCTION, name);
    m->unit = p->unit;
    m->owner = owner;
    m->is_mut_method = is_mut;
    m->return_type_name = ret.text;
    m->return_type_at = ret.name_at;
    m->return_type_qual_at = ret.at;
    parse_routine_rest(p, m, name);
    return m;
}

// Type operator: an operator of a struct, rather than a field or method.
static bool at_operator(const parser *p)
{
    if (peek(p)->kind != T_IDENT) return false;
    int i = 1;
    while (peek_at(p, i)->kind == T_DOT && peek_at(p, i + 1)->kind == T_IDENT) i += 2;
    return peek_at(p, i)->kind == T_IDENT && str_eq_c(peek_at(p, i)->text, "operator");
}

// The operators a struct can declare, as in C#.
static bool is_overloadable(const tok_kind kind)
{
    switch (kind) {
    case T_PLUS: case T_MINUS: case T_STAR: case T_SLASH: case T_PERCENT: case T_AMP: case T_PIPE: case T_CARET:
    case T_SHL: case T_SHR: case T_EQ: case T_NE: case T_LT: case T_LE: case T_GT: case T_GE: case T_NOT: case T_TILDE:
        return true;
    default:
        return false;
    }
}

// ReturnType operator +(Type a, Type b) { ... } in a struct.
static decl *parse_operator(parser *p, decl *owner)
{
    const qname ret = parse_type(p, "return type");
    const token *keyword = advance(p);
    const token *op = advance(p);
    if (!is_overloadable(op->kind)) {
        diag_error(op->at, "expected an operator a struct can declare after 'operator'");
        diag_note("these can: + - * / %% & | ^ << >> == != < <= > >= ! ~");
        longjmp(p->fail, 1);
    }
    decl *m = new_decl(DECL_METHOD, keyword);
    sb name = {0};
    sb_printf(&name, "operator " STR_FMT, STR_ARG(op->text));
    m->name = (str){name.data, (int)name.len};
    m->unit = p->unit;
    m->owner = owner;
    m->is_operator = true;
    m->op = op->kind;
    m->return_type_name = ret.text;
    m->return_type_at = ret.name_at;
    m->return_type_qual_at = ret.at;
    parse_routine_rest(p, m, keyword);
    return m;
}

// component Name { Type field; ... }, and the same for singletons, inputs and structs.
static decl *parse_data_decl(parser *p, const decl_kind kind)
{
    const char *what = kind == DECL_COMPONENT   ? "component or scene name"
                       : kind == DECL_SINGLETON ? "singleton name"
                       : kind == DECL_STRUCT    ? "struct name"
                       : kind == DECL_EVENT     ? "event name"
                                                : "input name";
    const token *name = expect_ident(p, what);
    decl *d = new_decl(kind, name);
    expect(p, T_LBRACE, "'{'");
    while (!at(p, T_RBRACE)) {
        if (p->recover && (at(p, T_EOF) || at_decl_start_or_function(p, false))) {
            diag_error(peek(p)->at, "expected '}' to close '" STR_FMT "'", STR_ARG(name->text));
            d->end = peek(p)->at;
            return d;
        }
        if (at(p, T_LBRACKET)) {
            if (p->recover) RECOVERING(p, parse_field_attributes(p));
            else parse_field_attributes(p);
            continue;
        }
        if (p->field_pending.count > 0 && ((at(p, T_IDENT) && peek_at(p, 1)->kind == T_LPAREN) || at_method(p))) {
            diag_error(p->field_pending.items[0].at, "field attributes go right before a field");
            p->field_pending.count = 0;
        }
        // Sample(...), or a constructor: the type's own name followed by '('.
        if (at(p, T_IDENT) && peek_at(p, 1)->kind == T_LPAREN
            && (str_eq_c(peek(p)->text, "Sample") || str_eq_c(peek(p)->text, "sample")
                || str_eq(peek(p)->text, name->text))) {
            parse_sample(p, d, advance(p));
            continue;
        }
        if (at(p, T_IDENT) && peek_at(p, 1)->kind == T_LPAREN
            && (str_eq_c(peek(p)->text, "Sanitize") || str_eq_c(peek(p)->text, "sanitize"))) {
            parse_sanitize(p, d, advance(p));
            continue;
        }
        if (at(p, T_IDENT) && str_eq_c(peek(p)->text, "extern") && peek_at(p, 1)->kind == T_IDENT) {
            diag_error(peek(p)->at, "extern functions go at the top of a file, outside '" STR_FMT "'", STR_ARG(name->text));
            if (!p->recover) longjmp(p->fail, 1);
            skip_statement(p);
            continue;
        }
        if (at_operator(p)) {
            if (p->recover) RECOVERING(p, vec_push(d->methods, parse_operator(p, d)));
            else vec_push(d->methods, parse_operator(p, d));
            continue;
        }
        if (at_method(p)) {
            if (p->recover) RECOVERING(p, vec_push(d->methods, parse_method(p, d)));
            else vec_push(d->methods, parse_method(p, d));
            continue;
        }
        if (p->recover) RECOVERING(p, parse_field(p, d));
        else parse_field(p, d);
    }
    d->end = peek(p)->at;
    advance(p);
    return d;
}

// (Time time, mut Transform trs, with Player, without Dead) { ... }: the rest
// of a system, view or event handler.
static void parse_query_rest(parser *p, decl *d)
{
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
            const qname type = parse_type(p, what);
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
}

// enum Page { Title, Options = 3, Credits }, with an optional ',' after the last.
static decl *parse_enum(parser *p)
{
    const token *name = expect_ident(p, "enum name");
    decl *d = new_decl(DECL_ENUM, name);
    expect(p, T_LBRACE, "'{'");
    while (!at(p, T_RBRACE)) {
        const token *member = expect_ident(p, "a member name or '}'");
        enum_member m = {member->text, member->at, NULL, 0};
        if (accept(p, T_ASSIGN)) m.value = parse_expr(p);
        vec_push(d->members, m);
        if (!accept(p, T_COMMA)) break;
    }
    d->end = peek(p)->at;
    expect(p, T_RBRACE, "',' or '}' after the member");
    return d;
}

// system Name(Time time, mut Transform trs, with Player, without Dead) { ... }
// Views have the same shape: view Name(Transform trs, with Player) { ... }
static decl *parse_system(parser *p, const bool is_view)
{
    const token *name = expect_ident(p, is_view ? "view name" : "system name");
    decl *d = new_decl(DECL_SYSTEM, name);
    d->is_view = is_view;
    parse_query_rest(p, d);
    return d;
}

// event(Hit hit) TakeHit(mut Health health) { ... }: runs when a Hit is sent.
// The trigger is the first parameter; an event without fields needs no name.
static decl *parse_handler(parser *p)
{
    expect(p, T_LPAREN, "'(' and the event it handles");
    param trigger = {0};
    trigger.mode = PARAM_EVENT;
    trigger.at = peek(p)->at;
    const qname type = parse_qname(p, "the event it handles, like 'event(Hit hit)'");
    trigger.type_name = type.text;
    trigger.type_qual_at = type.at;
    trigger.type_at = type.name_at;
    if (at(p, T_IDENT)) {
        trigger.name_at = peek(p)->at;
        trigger.name = advance(p)->text;
    }
    expect(p, T_RPAREN, "')' after the event");
    const token *name = expect_ident(p, "handler name, like 'event(Hit hit) TakeHit(...)'");
    decl *d = new_decl(DECL_SYSTEM, name);
    d->is_handler = true;
    vec_push(d->params, trigger);
    parse_query_rest(p, d);
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
                    if (at(p, T_STRING)) vec_push(a.values, parse_expr(p)); // [NativeName("stb_perlin_noise3")]
                    else vec_push(a.args, parse_qname(p, "a name"));
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
    parser p = {toks, 0, {0}, recover, NULL, {0}, {0}, false};
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
        // `local` before a declaration: it belongs to this machine, not the match.
        const token *local = NULL;
        const tok_kind after = peek_at(&p, 1)->kind;
        if (t->kind == T_IDENT && str_eq_c(t->text, "local")
            && (after == T_IDENT || after == T_COMPONENT || after == T_SINGLETON || after == T_SYSTEM)) {
            local = advance(&p);
            t = peek(&p);
        }
        // Contextual keywords: only special at the start of a declaration or a
        // file, so they can still name parameters and locals.
        const bool followed_by_name = peek_at(&p, 1)->kind == T_IDENT;
        if (t->kind == T_IDENT && followed_by_name && (str_eq_c(t->text, "namespace") || str_eq_c(t->text, "using"))) {
            advance(&p);
            if (local) diag_error(local->at, "'local' goes before a declaration, like 'local component Spark { ... }'");
            parse_file_header(&p, t);
            continue;
        }
        if (t->kind == T_LBRACKET) {
            parse_attributes(&p);
            continue;
        }
        decl *d;
        const bool function = at_method(&p);
        if (!function) advance(&p);
        if (function) d = parse_method(&p, NULL);
        else if (t->kind == T_COMPONENT) d = parse_data_decl(&p, DECL_COMPONENT);
        else if (t->kind == T_SINGLETON) d = parse_data_decl(&p, DECL_SINGLETON);
        else if (t->kind == T_SYSTEM) d = parse_system(&p, false);
        else if (t->kind == T_IDENT && followed_by_name && str_eq_c(t->text, "input")) d = parse_data_decl(&p, DECL_INPUT);
        else if (t->kind == T_IDENT && followed_by_name && str_eq_c(t->text, "view")) d = parse_system(&p, true);
        else if (t->kind == T_IDENT && followed_by_name && str_eq_c(t->text, "struct")) d = parse_data_decl(&p, DECL_STRUCT);
        else if (t->kind == T_IDENT && followed_by_name && str_eq_c(t->text, "event")) d = parse_data_decl(&p, DECL_EVENT);
        else if (t->kind == T_IDENT && followed_by_name && str_eq_c(t->text, "enum")) d = parse_enum(&p);
        else if (t->kind == T_IDENT && followed_by_name && str_eq_c(t->text, "scene")) {
            d = parse_data_decl(&p, DECL_COMPONENT);
            d->is_scene = true;
        }
        else if (t->kind == T_IDENT && at(&p, T_LPAREN) && str_eq_c(t->text, "event")) d = parse_handler(&p);
        else if (t->kind == T_IDENT && followed_by_name && str_eq_c(t->text, "extern")) d = parse_extern(&p);
        else if (t->kind == T_IDENT && followed_by_name && str_eq_c(t->text, "external")) {
            diag_error(t->at, "did you mean 'extern'? It declares a function written in C: 'extern float Noise(float x);'");
            longjmp(p.fail, 1);
        }
        else fail_at(&p, t, "'component', 'scene', 'singleton', 'struct', 'enum', 'event', 'input', 'system', 'view', 'extern' or a function"); // Consumed, so recovery skips it
        d->unit = p.unit;
        if (local) {
            d->is_local = true;
            d->local_at = local->at;
        }
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
