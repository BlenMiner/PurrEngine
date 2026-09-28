#include "analysis.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ast.h"
#include "builtins.h"
#include "cdefs.h"
#include "lexer.h"
#include "types.h"

// Everything here points into the compiler's arena, so it all belongs to the
// last analysis and is reset by the next one.

typedef struct diagnostic {
    diag_severity severity;
    loc at;
    sb message; // Notes are appended on their own lines.
} diagnostic;

// A name in the source and what it refers to.
typedef enum occ_kind {
    OCC_TYPE,     // Component, singleton, input, record or built-in value type
    OCC_SYSTEM,   // System or view
    OCC_FIELD,
    OCC_PARAM,
    OCC_LOCAL,
    OCC_OWNER,    // Math, Draw
    OCC_FUNCTION, // Math.Dot, Draw.Circle, Spawn
    OCC_CONSTANT, // Math.PI, Color.red
    OCC_METHOD,   // e.Add, e.Remove, e.Destroy
    OCC_MEMBER,   // Swizzles, color channels, quaternion.value, matrix columns, input .down/.up
    OCC_NAMESPACE, // Combat in `namespace Combat;` or Combat.Health
    OCC_ATTRIBUTE, // Before, After, Clamp, Min, Max
} occ_kind;

typedef struct occurrence {
    loc at;
    int len;
    occ_kind kind;
    bool declaration;
    const decl *decl;   // The type, the system, or a field's owner. NULL for built-in value types.
    const field *field;
    const param *param;
    const decl *param_of;
    const stmt *local;
    str owner;          // Functions and constants: "Math", "Draw", "Color"...
    str name;
    type type;
    type object_type;   // Members: the type they're read from
    const char *c_name; // Functions and constants: what they are in C, like purr_draw_circle
} occurrence;

// One file of the game being analysed.
typedef struct afile {
    source src;
    const char *uri;
    token *toks;
    int tok_count;
    int lex_errors; // Characters the lexer skipped
    VEC(const char *) lines; // Where each line starts
} afile;

static struct {
    afile *files; // Index i is source file i: locations' `file` indexes this
    int file_count;
    int doc;           // The file requests are about
    int syntax_errors; // Lexer and parser errors in any file: code recovery skipped
    program *prog;
    VEC(diagnostic) diags;
    VEC(occurrence) occs;
} A;

// The document requests are about.
#define DOC (&A.files[A.doc])

// ---------------------------------------------------------------------------
// Positions

// Orders positions file by file, then by line and column.
static int loc_cmp(const loc a, const loc b)
{
    if (a.file != b.file) return a.file - b.file;
    return a.line != b.line ? a.line - b.line : a.col - b.col;
}

// 0-based UTF-16 column of a 1-based byte column.
static int utf16_col(const loc at)
{
    if (at.file < 0 || at.file >= A.file_count) return 0;
    const afile *f = &A.files[at.file];
    if (at.line < 1 || at.line > f->lines.count) return 0;
    const char *line = f->lines.items[at.line - 1];
    const char *end = f->src.text + f->src.len;
    int units = 0;
    for (int i = 0; i < at.col - 1 && line + i < end; i++) {
        const unsigned char c = (unsigned char)line[i];
        if ((c & 0xC0) == 0x80) continue; // Continuation byte
        units += c >= 0xF0 ? 2 : 1;       // Outside the BMP: a surrogate pair
    }
    return units;
}

// A position in the current document.
static loc from_lsp(const int line, const int character)
{
    loc at = {line + 1, 1, A.doc};
    if (line < 0 || line >= DOC->lines.count) return at;
    const char *p = DOC->lines.items[line];
    const char *end = DOC->src.text + DOC->src.len;
    int units = 0;
    while (p < end && *p != '\n' && units < character) {
        const unsigned char c = (unsigned char)*p;
        const int len = c < 0xC0 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        units += c >= 0xF0 ? 2 : 1;
        p += len;
        at.col += len;
    }
    return at;
}

static void write_position(jbuf *out, const loc at)
{
    jb_printf(out, "{\"line\":%d,\"character\":%d}", at.line > 0 ? at.line - 1 : 0, utf16_col(at));
}

// A range on one line, `len` bytes long.
static void write_range(jbuf *out, const loc at, const int len)
{
    jb_put(out, "{\"start\":");
    write_position(out, at);
    jb_put(out, ",\"end\":");
    write_position(out, (loc){at.line, at.col + len, at.file});
    jb_put(out, "}");
}

// Index of the token starting at `at` in its file, or -1.
static int token_at(const loc at)
{
    if (at.file < 0 || at.file >= A.file_count) return -1;
    const afile *f = &A.files[at.file];
    int lo = 0;
    int hi = f->tok_count - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const int c = loc_cmp(f->toks[mid].at, at);
        if (c == 0) return mid;
        if (c < 0) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

static int token_len(const token *t)
{
    return t->kind == T_STRING ? t->text.len + 2 : t->text.len;
}

// ---------------------------------------------------------------------------
// Running the compiler

static void collect(void *user, const diag_severity severity, const loc at, const char *message)
{
    (void)user;
    if (severity == DIAG_NOTE) {
        if (A.diags.count > 0) sb_printf(&A.diags.items[A.diags.count - 1].message, "\nnote: %s", message);
        return;
    }
    diagnostic d = {severity, at, {0}};
    sb_put(&d.message, message);
    vec_push(A.diags, d);
}

static void add_occ(const occurrence o)
{
    if (o.at.line > 0 && o.len > 0) vec_push(A.occs, o);
}

// A type written by name: of a field, parameter or local.
// The last part of "Game.Combat.Health".
static str last_part(const str text)
{
    int dot = text.len - 1;
    while (dot >= 0 && text.ptr[dot] != '.') dot--;
    return (str){text.ptr + dot + 1, text.len - dot - 1};
}

// The namespace in front of a qualified name written at `qual_at`: Combat in
// Combat.Health. Nothing for a plain name.
static void qualifier_ref(const loc qual_at, const loc name_at, const str text)
{
    const str name = last_part(text);
    if (name.len == text.len || qual_at.line == 0 || loc_cmp(qual_at, name_at) == 0) return;
    const str ns = {text.ptr, text.len - name.len - 1};
    add_occ((occurrence){.at = qual_at, .len = ns.len, .kind = OCC_NAMESPACE, .name = ns});
}

// A type written by name, maybe qualified: `qual_at` is where it starts, `at`
// its last part.
static void type_ref(const loc qual_at, const loc at, const str text, const type t)
{
    if (text.len == 0) return;
    qualifier_ref(qual_at, at, text);
    const str name = last_part(text);
    occurrence o = {.at = at, .len = name.len, .kind = OCC_TYPE, .name = name, .type = t};
    if (t.kind == TY_COMPONENT || t.kind == TY_SINGLETON || t.kind == TY_INPUT || t.kind == TY_RECORD) {
        o.decl = t.decl;
    } else if (!builtin_type_named(name, &o.type)) {
        // Unresolved, for example a component used as a field type: still a type.
        for (int i = 0; i < A.prog->decls.count; i++) {
            const decl *d = A.prog->decls.items[i];
            if (d->kind != DECL_SYSTEM && str_eq(d->name, name)) o.decl = d;
        }
        if (!o.decl) return;
    }
    add_occ(o);
}

static void walk_expr(const expr *e)
{
    if (!e) return;
    switch (e->kind) {
    case E_INT:
    case E_FLOAT:
    case E_BOOL:
    case E_STRING:
        break;

    case E_NAME: {
        occurrence o = {.at = e->at, .len = e->name.len, .name = e->name, .type = e->type};
        switch (e->bind) {
        case BIND_PARAM:
            o.kind = OCC_PARAM;
            o.param = e->param;
            break;
        case BIND_LOCAL:
            o.kind = OCC_LOCAL;
            o.local = e->local;
            break;
        case BIND_TYPE:
            o.kind = OCC_TYPE;
            o.decl = e->type_decl;
            break;
        case BIND_FIELD:
            o.kind = OCC_FIELD;
            o.field = e->field;
            o.decl = A.prog->input;
            break;
        case BIND_NAMESPACE:
            o.kind = OCC_NAMESPACE;
            break;
        case BIND_NONE:
            // The owner in Math.Dot(...) or quaternion.identity, which the checker doesn't bind.
            if (!builtin_owner(e->name)) return;
            if (builtin_type_named(e->name, &o.type)) {
                o.kind = OCC_TYPE;
            } else {
                o.kind = OCC_OWNER;
                o.owner = e->name;
            }
            break;
        }
        add_occ(o);
        break;
    }

    case E_MEMBER: {
        walk_expr(e->object);
        occurrence o = {.at = e->at, .len = e->member.len, .name = e->member, .type = e->type,
                        .object_type = e->object->type};
        if (e->bind == BIND_TYPE && e->type_decl) { // Combat.Health, in Spawn(Combat.Health)
            o.kind = OCC_TYPE;
            o.decl = e->type_decl;
        } else if (e->bind == BIND_NAMESPACE) { // Combat in Game.Combat.Health
            o.kind = OCC_NAMESPACE;
        } else if (e->c_constant) {
            o.kind = OCC_CONSTANT;
            o.owner = e->object->name;
            o.c_name = e->c_constant;
        } else if (e->field) {
            o.kind = OCC_FIELD;
            o.field = e->field;
            o.decl = e->object->type.decl;
        } else if (e->type.kind != TY_ERROR) {
            o.kind = OCC_MEMBER;
        } else {
            break;
        }
        add_occ(o);
        break;
    }

    case E_CALL: {
        occurrence o = {.at = e->at, .len = e->name.len, .name = e->name, .type = e->type};
        if (e->call == CALL_CONSTRUCT) {
            o.kind = OCC_TYPE;
            add_occ(o);
        } else if (e->call == CALL_SPAWN) {
            o.kind = OCC_FUNCTION;
            add_occ(o);
        }
        for (int i = 0; i < e->args.count; i++) walk_expr(e->args.items[i]);
        break;
    }

    case E_METHOD: {
        walk_expr(e->object);
        occurrence o = {.at = e->at, .len = e->name.len, .name = e->name, .type = e->type};
        if (e->call == CALL_BUILTIN || e->call == CALL_DRAW) {
            o.kind = OCC_FUNCTION;
            o.owner = e->object->name;
            o.c_name = e->c_callee;
            add_occ(o);
        } else if (e->call == CALL_ADD || e->call == CALL_REMOVE || e->call == CALL_DESTROY) {
            o.kind = OCC_METHOD;
            add_occ(o);
        }
        for (int i = 0; i < e->args.count; i++) walk_expr(e->args.items[i]);
        break;
    }

    case E_LITERAL:
        if (e->type_decl) {
            qualifier_ref(e->qual_at, e->at, e->name);
            const str name = last_part(e->name);
            add_occ((occurrence){.at = e->at, .len = name.len, .kind = OCC_TYPE, .decl = e->type_decl, .name = name});
        }
        for (int i = 0; i < e->inits.count; i++) {
            const field_init *init = &e->inits.items[i];
            if (init->field) {
                add_occ((occurrence){.at = init->at, .len = init->name.len, .kind = OCC_FIELD, .field = init->field,
                                     .decl = e->type_decl, .name = init->name, .type = init->field->type});
            }
            walk_expr(init->value);
        }
        break;

    case E_BINARY:
        walk_expr(e->lhs);
        walk_expr(e->rhs);
        break;

    case E_UNARY:
        walk_expr(e->lhs);
        break;

    case E_CONDITIONAL:
        walk_expr(e->cond);
        walk_expr(e->lhs);
        walk_expr(e->rhs);
        break;
    }
}

static void walk_stmt(const stmt *s)
{
    if (!s) return;
    switch (s->kind) {
    case S_BLOCK:
        for (int i = 0; i < s->stmts.count; i++) walk_stmt(s->stmts.items[i]);
        break;
    case S_IF:
        walk_expr(s->cond);
        walk_stmt(s->then_stmt);
        walk_stmt(s->else_stmt);
        break;
    case S_RETURN:
        break;
    case S_VAR:
        type_ref(s->type_at, s->type_at, s->type_name, s->type);
        add_occ((occurrence){.at = s->name_at, .len = s->name.len, .kind = OCC_LOCAL, .declaration = true, .local = s,
                             .name = s->name, .type = s->type});
        walk_expr(s->value);
        break;
    case S_ASSIGN:
        walk_expr(s->target);
        walk_expr(s->value);
        break;
    case S_EXPR:
        walk_expr(s->value);
        break;
    }
}

static void walk_params(const decl *d)
{
    for (int i = 0; i < d->params.count; i++) {
        const param *p = &d->params.items[i];
        type_ref(p->type_qual_at, p->type_at, p->type_name, p->type);
        if (p->name.len > 0) {
            add_occ((occurrence){.at = p->name_at, .len = p->name.len, .kind = OCC_PARAM, .declaration = true, .param = p,
                                 .param_of = d, .name = p->name, .type = p->type});
        }
    }
}

static int occ_order(const void *a, const void *b)
{
    return loc_cmp(((const occurrence *)a)->at, ((const occurrence *)b)->at);
}

static void index_program(void)
{
    // `namespace X;` and `using Y;` at the top of each file
    for (int i = 0; i < A.prog->units.count; i++) {
        const unit *u = A.prog->units.items[i];
        if (u->ns.len > 0) {
            add_occ((occurrence){.at = u->ns_at, .len = u->ns.len, .kind = OCC_NAMESPACE, .declaration = true,
                                 .name = u->ns});
        }
        for (int k = 0; k < u->usings.count; k++) {
            add_occ((occurrence){.at = u->using_at.items[k], .len = u->usings.items[k].len, .kind = OCC_NAMESPACE,
                                 .name = u->usings.items[k]});
        }
    }

    for (int i = 0; i < A.prog->decls.count; i++) {
        const decl *d = A.prog->decls.items[i];
        if (d->builtin) continue;
        // [After(Physics.Integrate)]: the attribute, and the systems it names
        for (int a = 0; a < d->attributes.count; a++) {
            const attribute *at = &d->attributes.items[a];
            add_occ((occurrence){.at = at->at, .len = at->name.len, .kind = OCC_ATTRIBUTE, .name = at->name});
            for (int k = 0; k < d->attributes.items[a].args.count; k++) {
                const qname *q = &d->attributes.items[a].args.items[k];
                if (!q->decl) continue;
                qualifier_ref(q->at, q->name_at, q->text);
                const str name = last_part(q->text);
                add_occ((occurrence){.at = q->name_at, .len = name.len, .kind = OCC_SYSTEM, .decl = q->decl,
                                     .name = name});
            }
        }
        if (d->kind == DECL_SYSTEM) {
            add_occ((occurrence){.at = d->at, .len = d->name.len, .kind = OCC_SYSTEM, .declaration = true, .decl = d,
                                 .name = d->name});
            walk_params(d);
            walk_stmt(d->body);
            continue;
        }
        add_occ((occurrence){.at = d->at, .len = d->name.len, .kind = OCC_TYPE, .declaration = true, .decl = d,
                             .name = d->name});
        for (int f = 0; f < d->fields.count; f++) {
            const field *fl = &d->fields.items[f];
            type_ref(fl->type_at, fl->type_at, fl->type_name, fl->type);
            add_occ((occurrence){.at = fl->at, .len = fl->name.len, .kind = OCC_FIELD, .declaration = true, .field = fl,
                                 .decl = d, .name = fl->name, .type = fl->type});
            walk_expr(fl->default_value);
            for (int a = 0; a < fl->attributes.count; a++) {
                const attribute *at = &fl->attributes.items[a];
                add_occ((occurrence){.at = at->at, .len = at->name.len, .kind = OCC_ATTRIBUTE, .name = at->name});
                for (int v = 0; v < at->values.count; v++) walk_expr(at->values.items[v]);
            }
        }
        if (d->body) { // The input's Sample
            add_occ((occurrence){.at = d->body_at, .len = 6, .kind = OCC_METHOD, .decl = d, .name = str_from("Sample")});
            walk_params(d);
            walk_stmt(d->body);
        }
        if (d->sanitize) {
            add_occ((occurrence){.at = d->sanitize_at, .len = 8, .kind = OCC_METHOD, .decl = d,
                                 .name = str_from("Sanitize")});
            walk_stmt(d->sanitize);
        }
    }

    // In source order, one per position.
    qsort(A.occs.items, (size_t)A.occs.count, sizeof(occurrence), occ_order);
    int n = 0;
    for (int i = 0; i < A.occs.count; i++) {
        if (n > 0 && loc_cmp(A.occs.items[n - 1].at, A.occs.items[i].at) == 0) continue;
        A.occs.items[n++] = A.occs.items[i];
    }
    A.occs.count = n;
}

void analysis_run(const analysis_file *files, const int count)
{
    arena_reset();
    memset(&A, 0, sizeof A);
    diag_reset();
    diag_set_sink(collect, NULL);

    A.file_count = count;
    A.files = arena_alloc(sizeof(afile) * (size_t)(count > 0 ? count : 1));
    A.prog = program_new();
    for (int i = 0; i < count; i++) {
        afile *f = &A.files[i];
        char *copy = arena_alloc(files[i].len + 1);
        memcpy(copy, files[i].text, files[i].len);
        f->src = (source){files[i].path, copy, files[i].len, 0};
        f->uri = files[i].uri;
        diag_add_source(&f->src);

        const char *p = copy;
        if (files[i].len >= 3 && memcmp(p, "\xEF\xBB\xBF", 3) == 0) p += 3; // The lexer counts columns after it.
        vec_push(f->lines, p);
        for (; p < copy + files[i].len; p++) {
            if (*p == '\n') vec_push(f->lines, p + 1);
        }

        const int errors_before = diag_error_count();
        f->toks = lex_all(&f->src);
        f->lex_errors = diag_error_count() - errors_before;
        while (f->toks[f->tok_count].kind != T_EOF) f->tok_count++;
        parse_file(A.prog, &f->src, f->toks, true);
    }
    A.syntax_errors = diag_error_count();
    check(A.prog);
    diag_set_sink(NULL, NULL);

    index_program();
}

bool analysis_select(const char *uri)
{
    for (int i = 0; i < A.file_count; i++) {
        if (strcmp(A.files[i].uri, uri) == 0) {
            A.doc = i;
            return true;
        }
    }
    return false;
}

int analysis_file_count(void)
{
    return A.file_count;
}

const char *analysis_file_uri(const int file)
{
    return A.files[file].uri;
}

// ---------------------------------------------------------------------------
// Diagnostics

void analysis_diagnostics(const int file, jbuf *out)
{
    jb_put(out, "[");
    int written = 0;
    for (int i = 0; i < A.diags.count; i++) {
        const diagnostic *d = &A.diags.items[i];
        if (d->at.file != file) continue;
        const loc at = d->at.line > 0 ? d->at : (loc){1, 1, file};
        const int t = token_at(at);
        if (written++) jb_put(out, ",");
        jb_put(out, "{\"range\":");
        write_range(out, at, t >= 0 ? token_len(&A.files[file].toks[t]) : 1);
        jb_printf(out, ",\"severity\":%d,\"source\":\"purrc\",\"message\":", d->severity == DIAG_ERROR ? 1 : 2);
        jb_string(out, d->message.data ? d->message.data : "");
        jb_put(out, "}");
    }
    jb_put(out, "]");
}

// ---------------------------------------------------------------------------
// Descriptions, for hovers and completion details

static const char *builtin_type_doc(const type_kind kind)
{
    switch (kind) {
    case TY_BOOL: return "true or false.";
    case TY_INT: return "A 32-bit integer. Arithmetic wraps on overflow; dividing by zero gives 0.";
    case TY_FLOAT: return "A 32-bit float. Results are identical on every platform.";
    case TY_INT2: case TY_INT3: case TY_INT4: return "A vector of ints: x, y, z, w.";
    case TY_FLOAT2: case TY_FLOAT3: case TY_FLOAT4: return "A vector of floats: x, y, z, w. Swizzles like `v.xz` work.";
    case TY_QUATERNION: return "A rotation. Combine rotations with Math.Mul and rotate vectors with Math.Rotate.";
    case TY_FLOAT2X2: case TY_FLOAT3X3: case TY_FLOAT4X4: return "A matrix, stored column by column (c0, c1, ...).";
    case TY_ENTITY: return "A handle to an entity. It stays safe to use after the entity is destroyed.";
    case TY_PLAYER: return "A player, independent of connections. `PlayerID(0)` names a player by index.";
    case TY_COLOR: return "A color: r, g, b and a from 0 to 1. `Color(r, g, b)` or `Color(r, g, b, a)`.";
    default: return NULL;
    }
}

static const char *decl_keyword(const decl *d)
{
    switch (d->kind) {
    case DECL_COMPONENT: return "component";
    case DECL_SINGLETON: return "singleton";
    case DECL_INPUT: return "input";
    case DECL_RECORD: return "record";
    case DECL_SYSTEM: return d->is_view ? "view" : "system";
    }
    return "";
}

static void format_param(const param *p, sb *out)
{
    switch (p->mode) {
    case PARAM_MUT: sb_put(out, "mut "); break;
    case PARAM_WITH: sb_put(out, "with "); break;
    case PARAM_WITHOUT: sb_put(out, "without "); break;
    case PARAM_READ: break;
    }
    sb_printf(out, STR_FMT, STR_ARG(p->type_name));
    if (p->name.len > 0) sb_printf(out, " " STR_FMT, STR_ARG(p->name));
}

static void format_header(const decl *d, sb *out)
{
    sb_printf(out, "%s " STR_FMT "(", decl_keyword(d), STR_ARG(d->name));
    for (int i = 0; i < d->params.count; i++) {
        if (i) sb_put(out, ", ");
        format_param(&d->params.items[i], out);
    }
    sb_put(out, ")");
}

// The source text of a field's default value, from the tokens between '=' and ';'.
static void format_default(const field *f, sb *out)
{
    const int name = token_at(f->at);
    if (name < 0) return;
    const token *toks = A.files[f->at.file].toks;
    if (toks[name + 1].kind != T_ASSIGN) return;
    const int first = name + 2;
    int last = first;
    while (toks[last].kind != T_SEMI && toks[last].kind != T_EOF && toks[last].kind != T_RBRACE) last++;
    if (last == first) return;
    const char *start = toks[first].text.ptr - (toks[first].kind == T_STRING ? 1 : 0);
    const token *end_tok = &toks[last - 1];
    const char *end = end_tok->text.ptr + token_len(end_tok) - (end_tok->kind == T_STRING ? 1 : 0);
    sb_put(out, " = ");
    sb_putn(out, start, (size_t)(end - start));
}

static void format_data_decl(const decl *d, sb *out)
{
    sb_printf(out, "%s " STR_FMT "\n{\n", decl_keyword(d), STR_ARG(d->name));
    for (int i = 0; i < d->fields.count; i++) {
        const field *f = &d->fields.items[i];
        const char *type = f->type_name.len > 0 ? NULL : type_name(f->type);
        if (type) sb_printf(out, "    %s " STR_FMT, type, STR_ARG(f->name));
        else sb_printf(out, "    " STR_FMT " " STR_FMT, STR_ARG(f->type_name), STR_ARG(f->name));
        if (!d->builtin) format_default(f, out);
        sb_put(out, ";\n");
    }
    sb_put(out, "}");
}

static void code_block(sb *out, const char *code)
{
    sb_printf(out, "```purrlang\n%s\n```", code);
}

static void describe_order(const decl *d, sb *out);

// What a Button's fields mean. Engines disagree on these names (raylib's
// "down" is Unity's "pressed"), so hovers and completions spell it out.
static const char *button_field_doc(const decl *d, const str name)
{
    if (!d || d->kind != DECL_RECORD || !str_eq_c(d->name, "Button")) return NULL;
    if (str_eq_c(name, "pressed")) {
        return "True while the button is held: down at any point since the last tick, so a quick tap is never missed.";
    }
    if (str_eq_c(name, "down")) return "True on the tick the button went down.";
    if (str_eq_c(name, "up")) return "True on the tick the button went up.";
    return NULL;
}

// Markdown for a hover over `o`.
static void describe(const occurrence *o, sb *out)
{
    sb code = {0};
    switch (o->kind) {
    case OCC_TYPE:
        if (o->decl) {
            format_data_decl(o->decl, &code);
            code_block(out, code.data);
            if (o->decl->builtin) sb_put(out, "\n\nBuilt into the engine.");
        } else {
            code_block(out, type_name(o->type));
            const char *doc = builtin_type_doc(o->type.kind);
            if (doc) sb_printf(out, "\n\n%s", doc);
        }
        break;
    case OCC_SYSTEM:
        format_header(o->decl, &code);
        code_block(out, code.data);
        sb_put(out, o->decl->is_view ? "\n\nRuns once per frame and only reads the world."
                                     : o->decl->is_main ? "\n\nThe entry point: runs once when the world is created."
                                     : o->decl->per_entity ? "\n\nRuns once per tick for every matching entity."
                                     : "\n\nRuns once per tick.");
        describe_order(o->decl, out);
        break;
    case OCC_FIELD:
        sb_printf(&code, "%s " STR_FMT, type_name(o->field->type), STR_ARG(o->field->name));
        if (o->decl && !o->decl->builtin) format_default(o->field, &code);
        code_block(out, code.data);
        if (o->decl) sb_printf(out, "\n\nField of %s `" STR_FMT "`.", decl_keyword(o->decl), STR_ARG(o->decl->name));
        if (button_field_doc(o->decl, o->field->name)) sb_printf(out, " %s", button_field_doc(o->decl, o->field->name));
        break;
    case OCC_PARAM:
        format_param(o->param, &code);
        code_block(out, code.data);
        if (o->param->type.kind == TY_INPUT) sb_put(out, "\n\nThe input of the player who owns the entity.");
        else if (o->param->mode == PARAM_MUT) sb_put(out, "\n\nParameter, writable.");
        else sb_put(out, "\n\nParameter, read-only.");
        break;
    case OCC_LOCAL:
        sb_printf(&code, "%s%s " STR_FMT, o->local->is_mut ? "mut " : "", type_name(o->local->type),
                  STR_ARG(o->local->name));
        code_block(out, code.data);
        sb_put(out, o->local->is_mut ? "\n\nLocal variable." : "\n\nLocal variable, read-only.");
        break;
    case OCC_OWNER:
        code_block(out, str_to_cstr(o->name));
        sb_put(out, str_eq_c(o->name, "Draw") ? "\n\nImmediate-mode drawing. Only in views."
                                              : "\n\nMath functions and constants, deterministic on every platform.");
        break;
    case OCC_FUNCTION:
    case OCC_CONSTANT:
        if (o->kind == OCC_FUNCTION && o->owner.len == 0) {
            code_block(out, "Spawn(components...) -> Entity");
            sb_put(out, "\n\nCreates an entity with these components. It's added at the end of the tick; the "
                        "handle works right away.");
        } else if (builtin_describe(o->owner, o->name, &code)) {
            // The first lines are signatures, the last paragraph the doc.
            char *doc = strstr(code.data, "\n\n");
            if (doc) *doc = '\0';
            code_block(out, code.data);
            if (doc) sb_printf(out, "\n\n%s", doc + 2);
        }
        break;
    case OCC_METHOD:
        if (str_eq_c(o->name, "Sample")) {
            code_block(out, "Sample(Devices devices)");
            sb_put(out, "\n\nBuilds the player's input from the devices, once per tick on their machine. Fields "
                        "start at their defaults. It runs outside the simulation, so it only sees the devices.");
        } else if (str_eq_c(o->name, "Sanitize")) {
            code_block(out, "Sanitize()");
            sb_put(out, "\n\nRuns on every input before the simulation reads it, including input from other "
                        "players, so systems can rely on what it guarantees.");
        } else if (str_eq_c(o->name, "Destroy")) {
            code_block(out, "entity.Destroy()");
        } else {
            sb_printf(&code, "entity." STR_FMT "(components...)", STR_ARG(o->name));
            code_block(out, code.data);
        }
        sb_put(out, str_eq_c(o->name, "Add") ? "\n\nAdds components, or replaces their values. Applied at the end of the tick."
                  : str_eq_c(o->name, "Remove") ? "\n\nRemoves components. Applied at the end of the tick."
                  : "\n\nDestroys the entity at the end of the tick.");
        break;
    case OCC_MEMBER:
        sb_printf(&code, "%s " STR_FMT, type_name(o->type), STR_ARG(o->name));
        code_block(out, code.data);
        if (str_eq_c(o->name, "down")) sb_put(out, "\n\nTrue on the tick it became true.");
        if (str_eq_c(o->name, "up")) sb_put(out, "\n\nTrue on the tick it became false.");
        break;
    case OCC_ATTRIBUTE: {
        static const struct {
            const char *name;
            const char *form;
            const char *doc;
        } attributes[] = {
            {"Before", "[Before(System, ...)]", "This system runs before the ones named, every tick."},
            {"After", "[After(System, ...)]", "This system runs after the ones named, every tick."},
            {"Clamp", "[Clamp(lo, hi)]", "Keeps this input field between lo and hi. The engine applies it to every "
                                         "input, after repairing NaN and before Sanitize."},
            {"Min", "[Min(x)]", "Keeps this input field at least x, before Sanitize runs."},
            {"Max", "[Max(x)]", "Keeps this input field at most x, before Sanitize runs."},
        };
        for (size_t i = 0; i < sizeof attributes / sizeof attributes[0]; i++) {
            if (!str_eq_c(o->name, attributes[i].name)) continue;
            code_block(out, attributes[i].form);
            sb_printf(out, "\n\n%s", attributes[i].doc);
        }
        break;
    }
    case OCC_NAMESPACE: {
        sb_printf(&code, "namespace " STR_FMT, STR_ARG(o->name));
        code_block(out, code.data);
        int count = 0;
        for (int i = 0; i < A.prog->decls.count; i++) {
            const decl *d = A.prog->decls.items[i];
            if (!d->unit || !str_eq(d->unit->ns, o->name)) continue;
            sb_printf(out, "%s`" STR_FMT "`", count++ ? ", " : "\n\nDeclares ", STR_ARG(d->name));
        }
        break;
    }
    }
}

// "3rd"
static const char *ordinal(const int n)
{
    static char buf[16];
    const int tens = n % 100;
    const char *suffix = tens >= 11 && tens <= 13 ? "th" : n % 10 == 1 ? "st" : n % 10 == 2 ? "nd" : n % 10 == 3 ? "rd" : "th";
    snprintf(buf, sizeof buf, "%d%s", n, suffix);
    return buf;
}

// Where a system or view runs, and what it runs after: the schedule, visible.
static void describe_order(const decl *d, sb *out)
{
    if (d->is_main) return;
    const int count = d->is_view ? A.prog->views.count : A.prog->systems.count;
    sb_printf(out, "\n\nRuns %s of %d %s", ordinal(d->index + 1), count,
              d->is_view ? "views each frame (later views draw on top)" : "systems each tick");
    for (int i = 0; i < d->after.count; i++) {
        sb_printf(out, "%s`" STR_FMT "`", i ? ", " : ", after ", STR_ARG(d->after.items[i]->qualified));
    }
    sb_put(out, ".");
    if (d->is_view || d->stage == 0) return;

    sb_printf(out, "\n\n**Stage %d.** ", d->stage);
    if (d->waits.count == 0) sb_put(out, "It doesn't wait for any system.");
    else sb_put(out, "It waits for:");
    for (int i = 0; i < d->waits.count; i++) {
        const system_wait *w = &d->waits.items[i];
        sb_put(out, "\n- ");
        put_decl_name(out, w->on, "`", d);
        sb_put(out, ": ");
        describe_wait(d, w, "`", out);
    }
    if (d->alongside.count == 0) {
        sb_put(out, "\n\nNo other system can run at the same time.");
    } else {
        sb_put(out, "\n\nCan run at the same time as ");
        for (int i = 0; i < d->alongside.count; i++) {
            sb_put(out, i == 0 ? "" : i == d->alongside.count - 1 ? " and " : ", ");
            put_decl_name(out, d->alongside.items[i], "`", d);
        }
        sb_put(out, ".");
    }
}

// ---------------------------------------------------------------------------
// Code lenses: above each system, its stage and why it waits

void analysis_code_lenses(jbuf *out)
{
    jb_put(out, "[");
    int written = 0;
    for (int i = 0; i < A.prog->systems.count; i++) {
        const decl *d = A.prog->systems.items[i];
        if (d->at.file != A.doc || d->stage == 0) continue;
        sb title = {0};
        sb_printf(&title, "stage %d", d->stage);
        for (int k = 0; k < d->waits.count; k++) {
            const system_wait *w = &d->waits.items[k];
            if (w->through) continue; // Shown on the system it goes through
            sb_put(&title, " \u00B7 after ");
            put_decl_name(&title, w->on, "", d);
            sb_put(&title, ": ");
            describe_wait(d, w, "", &title);
        }
        if (d->alongside.count == 0 && A.prog->systems.count > 1) {
            sb_put(&title, " \u00B7 nothing runs alongside");
        } else if (d->alongside.count > 0) {
            sb_put(&title, " \u00B7 alongside ");
            const int shown = d->alongside.count > 3 ? 3 : d->alongside.count;
            for (int k = 0; k < shown; k++) {
                if (k) sb_put(&title, ", ");
                put_decl_name(&title, d->alongside.items[k], "", d);
            }
            if (d->alongside.count > shown) sb_printf(&title, " and %d more", d->alongside.count - shown);
        }
        if (written++) jb_put(out, ",");
        jb_put(out, "{\"range\":");
        write_range(out, d->at, d->name.len);
        jb_put(out, ",\"command\":{\"title\":");
        jb_string(out, title.data);
        jb_put(out, ",\"command\":\"\"}}");
    }
    jb_put(out, "]");
}

// ---------------------------------------------------------------------------
// Quick fixes

static loc type_start(const param *p)
{
    return p->type_qual_at.line > 0 ? p->type_qual_at : p->type_at;
}

static void write_edit_range(jbuf *out, const loc start, const loc end)
{
    jb_put(out, "{\"start\":");
    write_position(out, start);
    jb_put(out, ",\"end\":");
    write_position(out, end);
    jb_put(out, "}");
}

void analysis_code_actions(const int start_line, const int end_line, jbuf *out)
{
    jb_put(out, "[");
    int written = 0;
    for (int i = 0; i < A.prog->fixes.count; i++) {
        const fix *f = &A.prog->fixes.items[i];
        if (f->at.file != A.doc || f->at.line - 1 < start_line || f->at.line - 1 > end_line) continue;
        const param *p = f->param;
        sb title = {0};
        sb text = {0};
        loc start = type_start(p);
        loc end = start;
        switch (f->kind) {
        case FIX_ADD_MUT:
            sb_printf(&title, "Declare '" STR_FMT "' as mut", STR_ARG(p->name));
            sb_put(&text, "mut ");
            break;
        case FIX_REMOVE_MUT:
            sb_printf(&title, "Remove 'mut' from '" STR_FMT "'", STR_ARG(p->name));
            start = p->at;
            sb_put(&text, "");
            break;
        case FIX_USE_WITH:
            sb_printf(&title, "Only require it: 'with " STR_FMT "'", STR_ARG(p->type_name));
            start = p->at;
            end = (loc){p->name_at.line, p->name_at.col + p->name.len, p->name_at.file};
            sb_printf(&text, "with " STR_FMT, STR_ARG(p->type_name));
            break;
        }
        if (written++) jb_put(out, ",");
        jb_put(out, "{\"title\":");
        jb_string(out, title.data);
        jb_put(out, ",\"kind\":\"quickfix\",\"isPreferred\":true,\"edit\":{\"changes\":{");
        jb_string(out, A.files[A.doc].uri);
        jb_put(out, ":[{\"range\":");
        write_edit_range(out, start, end);
        jb_put(out, ",\"newText\":");
        jb_string(out, text.data ? text.data : "");
        jb_put(out, "}]}}}");
    }
    jb_put(out, "]");
}

static const occurrence *occurrence_at(const loc at)
{
    for (int i = 0; i < A.occs.count; i++) {
        const occurrence *o = &A.occs.items[i];
        if (o->at.file == at.file && o->at.line == at.line && o->at.col <= at.col && at.col <= o->at.col + o->len) {
            return o;
        }
    }
    return NULL;
}

void analysis_hover(const int line, const int character, jbuf *out)
{
    const occurrence *o = occurrence_at(from_lsp(line, character));
    if (!o) {
        jb_put(out, "null");
        return;
    }
    sb text = {0};
    describe(o, &text);
    if (!text.data) {
        jb_put(out, "null");
        return;
    }
    jb_put(out, "{\"contents\":{\"kind\":\"markdown\",\"value\":");
    jb_string(out, text.data);
    jb_put(out, "},\"range\":");
    write_range(out, o->at, o->len);
    jb_put(out, "}");
}

// Built-ins defined in C, in the engine headers: their definition is there.
// Things the compiler itself generates (Time, Owner, Spawn) have none.
static bool write_c_definition(const occurrence *o, jbuf *out)
{
    switch (o->kind) {
    case OCC_TYPE:
        if (o->decl) return o->decl->kind == DECL_RECORD && cdefs_find(o->decl->c_name, out);
        return strncmp(type_c_name(o->type), "purr_", 5) == 0
            && cdefs_find(type_c_name(o->type), out);
    case OCC_FUNCTION:
    case OCC_CONSTANT: {
        if (!o->c_name) return false;
        char name[128];
        snprintf(name, sizeof name, "%s", o->c_name);
        char *call = strchr(name, '('); // purr_identity_q() for quaternion.identity
        if (call) *call = '\0';
        return cdefs_find(name, out);
    }
    case OCC_OWNER:
        return cdefs_find_header(str_eq_c(o->owner, "Draw") ? "draw.h" : "math.h", out);
    case OCC_FIELD:
        return o->decl && o->decl->kind == DECL_RECORD && cdefs_find_member(o->decl->c_name, str_to_cstr(o->name), out);
    case OCC_MEMBER: {
        // Real struct members only: x, r, value, c0; not swizzles like xz.
        const type t = o->object_type;
        const bool vector_component = type_dim(t) >= 2 && o->name.len == 1;
        const bool other = (t.kind == TY_COLOR && o->name.len == 1) || t.kind == TY_QUATERNION || matrix_dim(t) > 0;
        return (vector_component || other) && cdefs_find_member(type_c_name(t), str_to_cstr(o->name), out);
    }
    default:
        return false;
    }
}

void analysis_definition(const char *uri, const int line, const int character, jbuf *out)
{
    const occurrence *o = occurrence_at(from_lsp(line, character));
    loc target = {0, 0, 0};
    int len = 0;
    if (o) {
        if (o->kind == OCC_FIELD && o->decl && !o->decl->builtin) {
            target = o->field->at;
            len = o->field->name.len;
        } else if ((o->kind == OCC_TYPE || o->kind == OCC_SYSTEM) && o->decl && !o->decl->builtin) {
            target = o->decl->at;
            len = o->decl->name.len;
        } else if (o->kind == OCC_PARAM) {
            target = o->param->name_at;
            len = o->param->name.len;
        } else if (o->kind == OCC_LOCAL) {
            target = o->local->name_at;
            len = o->local->name.len;
        } else if (o->kind == OCC_NAMESPACE) {
            // The first file that declares it (or a namespace inside it)
            for (int i = 0; i < A.prog->units.count && target.line == 0; i++) {
                const unit *u = A.prog->units.items[i];
                if (u->ns.len >= o->name.len && memcmp(u->ns.ptr, o->name.ptr, (size_t)o->name.len) == 0) {
                    target = u->ns_at;
                    len = u->ns.len;
                }
            }
        }
    }
    if (target.line == 0) {
        if (!o || !write_c_definition(o, out)) jb_put(out, "null");
        return;
    }
    (void)uri;
    jb_put(out, "{\"uri\":");
    jb_string(out, A.files[target.file].uri); // Maybe another file of the game
    jb_put(out, ",\"range\":");
    write_range(out, target, len);
    jb_put(out, "}");
}

// ---------------------------------------------------------------------------
// Document symbols: the outline

enum { SYMBOL_CLASS = 5, SYMBOL_FIELD = 8, SYMBOL_INTERFACE = 11, SYMBOL_FUNCTION = 12, SYMBOL_STRUCT = 23 };

void analysis_symbols(jbuf *out)
{
    jb_put(out, "[");
    int written = 0;
    for (int i = 0; i < A.prog->decls.count; i++) {
        const decl *d = A.prog->decls.items[i];
        if (d->builtin || d->at.file != A.doc) continue; // This document's declarations
        const int kind = d->kind == DECL_COMPONENT ? SYMBOL_STRUCT
                       : d->kind == DECL_SINGLETON ? SYMBOL_CLASS
                       : d->kind == DECL_INPUT ? SYMBOL_INTERFACE
                                               : SYMBOL_FUNCTION;
        const loc start = {d->at.line, 1, d->at.file};
        const loc end = d->end.line > 0 ? d->end : d->at;
        if (written++) jb_put(out, ",");
        jb_put(out, "{\"name\":");
        jb_string_n(out, d->name.ptr, (size_t)d->name.len);
        jb_printf(out, ",\"detail\":\"%s\",\"kind\":%d,\"range\":{\"start\":", decl_keyword(d), kind);
        write_position(out, start);
        jb_put(out, ",\"end\":");
        write_position(out, (loc){end.line, end.col + 1, end.file});
        jb_put(out, "},\"selectionRange\":");
        write_range(out, d->at, d->name.len);
        jb_put(out, ",\"children\":[");
        for (int f = 0; f < d->fields.count; f++) {
            const field *fl = &d->fields.items[f];
            if (f) jb_put(out, ",");
            jb_put(out, "{\"name\":");
            jb_string_n(out, fl->name.ptr, (size_t)fl->name.len);
            jb_put(out, ",\"detail\":");
            jb_string_n(out, fl->type_name.ptr, (size_t)fl->type_name.len);
            jb_printf(out, ",\"kind\":%d,\"range\":", SYMBOL_FIELD);
            write_range(out, fl->at, fl->name.len);
            jb_put(out, ",\"selectionRange\":");
            write_range(out, fl->at, fl->name.len);
            jb_put(out, "}");
        }
        jb_put(out, "]}");
    }
    jb_put(out, "]");
}

// ---------------------------------------------------------------------------
// Semantic tokens: highlighting from what names mean, not how they look

static const char *const token_types[] = {"namespace", "type",     "struct", "class",   "interface", "parameter",
                                          "variable",  "property", "enumMember", "function", "method", "keyword",
                                          "decorator"};
enum { ST_NAMESPACE, ST_TYPE, ST_STRUCT, ST_CLASS, ST_INTERFACE, ST_PARAMETER, ST_VARIABLE, ST_PROPERTY,
       ST_ENUM_MEMBER, ST_FUNCTION, ST_METHOD, ST_KEYWORD, ST_DECORATOR };

// Lowercase built-in value types (float3, int, bool, ...) read as keywords,
// like C#'s float and int.
static bool is_keyword_type(const type t)
{
    switch (t.kind) {
    case TY_BOOL: case TY_INT: case TY_INT2: case TY_INT3: case TY_INT4: case TY_FLOAT: case TY_FLOAT2:
    case TY_FLOAT3: case TY_FLOAT4: case TY_QUATERNION: case TY_FLOAT2X2: case TY_FLOAT3X3: case TY_FLOAT4X4:
        return true;
    default:
        return false;
    }
}

static const char *const token_modifiers[] = {"declaration", "readonly", "static", "defaultLibrary"};
enum { SM_DECLARATION = 1, SM_READONLY = 2, SM_STATIC = 4, SM_DEFAULT_LIBRARY = 8 };

void analysis_semantic_legend(jbuf *out)
{
    jb_put(out, "{\"tokenTypes\":[");
    for (size_t i = 0; i < sizeof token_types / sizeof token_types[0]; i++) {
        if (i) jb_put(out, ",");
        jb_string(out, token_types[i]);
    }
    jb_put(out, "],\"tokenModifiers\":[");
    for (size_t i = 0; i < sizeof token_modifiers / sizeof token_modifiers[0]; i++) {
        if (i) jb_put(out, ",");
        jb_string(out, token_modifiers[i]);
    }
    jb_put(out, "]}");
}

static void classify(const occurrence *o, int *type, int *mods)
{
    *mods = o->declaration ? SM_DECLARATION : 0;
    switch (o->kind) {
    case OCC_TYPE:
        if (!o->decl) {
            *type = is_keyword_type(o->type) ? ST_KEYWORD : ST_TYPE; // Color, Entity, PlayerID are types
            *mods = is_keyword_type(o->type) ? 0 : *mods | SM_DEFAULT_LIBRARY;
            break;
        }
        // Components are structs, singletons classes, inputs interfaces, and the
        // device records types: editors can color each kind.
        *type = o->decl->kind == DECL_COMPONENT ? ST_STRUCT
              : o->decl->kind == DECL_INPUT     ? ST_INTERFACE
              : o->decl->kind == DECL_RECORD    ? ST_TYPE
                                                : ST_CLASS;
        if (o->decl->builtin) *mods |= SM_DEFAULT_LIBRARY;
        break;
    case OCC_SYSTEM: *type = ST_FUNCTION; break;
    case OCC_FIELD:
        *type = ST_PROPERTY;
        if (o->decl && o->decl->builtin) *mods |= SM_DEFAULT_LIBRARY;
        break;
    case OCC_PARAM:
        *type = ST_PARAMETER;
        if (o->param->mode != PARAM_MUT) *mods |= SM_READONLY;
        break;
    case OCC_LOCAL:
        *type = ST_VARIABLE;
        if (!o->local->is_mut) *mods |= SM_READONLY;
        break;
    case OCC_OWNER: *type = ST_NAMESPACE; *mods |= SM_DEFAULT_LIBRARY; break;
    case OCC_FUNCTION: *type = ST_FUNCTION; *mods |= SM_DEFAULT_LIBRARY | SM_STATIC; break;
    case OCC_CONSTANT: *type = ST_ENUM_MEMBER; *mods |= SM_DEFAULT_LIBRARY | SM_STATIC | SM_READONLY; break;
    case OCC_METHOD:
        if (str_eq_c(o->name, "Sample") || str_eq_c(o->name, "Sanitize")) { // The input's own members
            *type = ST_KEYWORD;
            *mods = 0;
        } else {
            *type = ST_METHOD;
            *mods |= SM_DEFAULT_LIBRARY;
        }
        break;
    case OCC_ATTRIBUTE: *type = ST_DECORATOR; *mods = 0; break;
    case OCC_MEMBER: *type = ST_PROPERTY; *mods |= SM_DEFAULT_LIBRARY; break;
    case OCC_NAMESPACE: *type = ST_NAMESPACE; break;
    }
}

void analysis_semantic_tokens(jbuf *out)
{
    jb_put(out, "{\"data\":[");
    int prev_line = 0;
    int prev_col = 0;
    for (int i = 0, written = 0; i < A.occs.count; i++) {
        const occurrence *o = &A.occs.items[i];
        if (o->at.file != A.doc) continue;
        int type;
        int mods;
        classify(o, &type, &mods);
        const int line = o->at.line - 1;
        const int col = utf16_col(o->at);
        const int delta_col = line == prev_line ? col - prev_col : col;
        jb_printf(out, "%s%d,%d,%d,%d,%d", written++ ? "," : "", line - prev_line, delta_col, o->len, type, mods);
        prev_line = line;
        prev_col = col;
    }
    jb_put(out, "]}");
}

// ---------------------------------------------------------------------------
// Completion

enum {
    CK_METHOD = 2, CK_FUNCTION = 3, CK_FIELD = 5, CK_VARIABLE = 6, CK_CLASS = 7, CK_INTERFACE = 8, CK_MODULE = 9,
    CK_PROPERTY = 10, CK_KEYWORD = 14, CK_SNIPPET = 15, CK_CONSTANT = 21, CK_STRUCT = 22,
};

typedef struct completion {
    jbuf *out;
    int count;
    VEC(const char *) seen;
} completion;

static void item(completion *c, const char *label, const int kind, const char *detail, const char *doc,
                 const char *snippet)
{
    for (int i = 0; i < c->seen.count; i++) {
        if (strcmp(c->seen.items[i], label) == 0) return;
    }
    vec_push(c->seen, label);
    if (c->count++) jb_put(c->out, ",");
    jb_put(c->out, "{\"label\":");
    jb_string(c->out, label);
    jb_printf(c->out, ",\"kind\":%d", kind);
    if (detail) {
        jb_put(c->out, ",\"detail\":");
        jb_string(c->out, detail);
    }
    if (doc) {
        jb_put(c->out, ",\"documentation\":{\"kind\":\"markdown\",\"value\":");
        jb_string(c->out, doc);
        jb_put(c->out, "}");
    }
    if (snippet) {
        jb_put(c->out, ",\"insertText\":");
        jb_string(c->out, snippet);
        jb_put(c->out, ",\"insertTextFormat\":2");
    }
    jb_put(c->out, "}");
}

// What surrounds the cursor.
typedef struct scope {
    const decl *decl;           // The system, view or input whose body holds the cursor, or NULL
    bool in_input;
    bool in_sanitize; // Sets in_input too, for the fields
    VEC(const stmt *) locals;   // Locals declared before the cursor, still in scope
} scope;

static bool block_contains(const stmt *block, const loc at)
{
    return block && block->kind == S_BLOCK && block->at.file == at.file && loc_cmp(block->at, at) < 0
        && loc_cmp(at, block->end) <= 0;
}

static void collect_locals(const stmt *block, const loc at, scope *sc)
{
    for (int i = 0; i < block->stmts.count; i++) {
        const stmt *s = block->stmts.items[i];
        if (loc_cmp(s->at, at) >= 0) break;
        if (s->kind == S_VAR && loc_cmp(s->name_at, at) < 0) vec_push(sc->locals, s);
        if (block_contains(s, at)) collect_locals(s, at, sc);
        if (s->kind == S_IF) {
            if (block_contains(s->then_stmt, at)) collect_locals(s->then_stmt, at, sc);
            if (block_contains(s->else_stmt, at)) collect_locals(s->else_stmt, at, sc);
        }
    }
}

static scope scope_at(const loc at)
{
    scope sc = {0};
    for (int i = 0; i < A.prog->decls.count; i++) {
        const decl *d = A.prog->decls.items[i];
        const bool sanitize = block_contains(d->sanitize, at);
        if (d->builtin || (!block_contains(d->body, at) && !sanitize)) continue;
        sc.decl = d;
        sc.in_input = d->kind == DECL_INPUT;
        sc.in_sanitize = sanitize;
        collect_locals(sanitize ? d->sanitize : d->body, at, &sc);
    }
    return sc;
}

// The type of a name in scope, or TY_ERROR.
static type name_type(const scope *sc, const str name, const param **param_out)
{
    *param_out = NULL;
    for (int i = sc->locals.count - 1; i >= 0; i--) {
        if (str_eq(sc->locals.items[i]->name, name)) return sc->locals.items[i]->type;
    }
    if (!sc->decl) return (type){TY_ERROR, NULL};
    for (int i = 0; i < sc->decl->params.count && !sc->in_sanitize; i++) {
        const param *p = &sc->decl->params.items[i];
        if (p->name.len > 0 && str_eq(p->name, name)) {
            *param_out = p;
            return p->type;
        }
    }
    if (sc->in_input) {
        for (int i = 0; i < sc->decl->fields.count; i++) {
            if (str_eq(sc->decl->fields.items[i].name, name)) return sc->decl->fields.items[i].type;
        }
    }
    return (type){TY_ERROR, NULL};
}

static bool is_swizzle(const str member, const int dim)
{
    if (member.len < 1 || member.len > 4) return false;
    for (int i = 0; i < member.len; i++) {
        const char *at = strchr("xyzw", member.ptr[i]);
        if (!at || at - "xyzw" >= dim) return false;
    }
    return true;
}

// The type of `t.member`, or TY_ERROR.
static type member_type(const type t, const str member)
{
    if (t.decl && (t.kind == TY_COMPONENT || t.kind == TY_SINGLETON || t.kind == TY_INPUT || t.kind == TY_RECORD)) {
        for (int i = 0; i < t.decl->fields.count; i++) {
            if (str_eq(t.decl->fields.items[i].name, member)) return t.decl->fields.items[i].type;
        }
    }
    const int dim = type_dim(t);
    if (dim >= 2 && is_swizzle(member, dim)) return vector_type(type_is_float_based(t), member.len);
    if (t.kind == TY_COLOR && member.len == 1 && strchr("rgba", member.ptr[0])) return (type){TY_FLOAT, NULL};
    if (t.kind == TY_QUATERNION && str_eq_c(member, "value")) return (type){TY_FLOAT4, NULL};
    const int n = matrix_dim(t);
    if (n > 0 && member.len == 2 && member.ptr[0] == 'c' && member.ptr[1] >= '0' && member.ptr[1] < '0' + n) {
        return vector_type(true, n);
    }
    return (type){TY_ERROR, NULL};
}

static void list_members(completion *c, const type t, const bool edges, const scope *sc)
{
    if (t.decl && (t.kind == TY_COMPONENT || t.kind == TY_SINGLETON || t.kind == TY_INPUT || t.kind == TY_RECORD)) {
        for (int i = 0; i < t.decl->fields.count; i++) {
            const field *f = &t.decl->fields.items[i];
            item(c, str_to_cstr(f->name), CK_FIELD, type_name(f->type), button_field_doc(t.decl, f->name), NULL);
        }
    }
    const int dim = type_dim(t);
    if (dim >= 2) {
        static const char *const components[] = {"x", "y", "z", "w"};
        static const char *const swizzles[] = {"xy", "xz", "yz", "xyz"};
        const char *scalar = type_name(vector_type(type_is_float_based(t), 1));
        for (int i = 0; i < dim; i++) item(c, components[i], CK_PROPERTY, scalar, NULL, NULL);
        for (size_t i = 0; i < sizeof swizzles / sizeof swizzles[0]; i++) {
            const str s = str_from(swizzles[i]);
            if (is_swizzle(s, dim) && s.len < dim) {
                item(c, swizzles[i], CK_PROPERTY, type_name(vector_type(type_is_float_based(t), s.len)), NULL, NULL);
            }
        }
    }
    if (t.kind == TY_COLOR) {
        item(c, "r", CK_PROPERTY, "float", "Red, 0 to 1.", NULL);
        item(c, "g", CK_PROPERTY, "float", "Green, 0 to 1.", NULL);
        item(c, "b", CK_PROPERTY, "float", "Blue, 0 to 1.", NULL);
        item(c, "a", CK_PROPERTY, "float", "Alpha, 0 to 1.", NULL);
    }
    if (t.kind == TY_QUATERNION) item(c, "value", CK_PROPERTY, "float4", "(x, y, z) is the vector part.", NULL);
    const int n = matrix_dim(t);
    for (int i = 0; i < n; i++) {
        static const char *const columns[] = {"c0", "c1", "c2", "c3"};
        item(c, columns[i], CK_PROPERTY, type_name(vector_type(true, n)), "A column.", NULL);
    }
    if (t.kind == TY_ENTITY && sc->decl && !sc->decl->is_view && !sc->in_input) {
        item(c, "Add", CK_METHOD, "entity.Add(components...)", "Adds components, or replaces their values.", "Add($1)");
        item(c, "Remove", CK_METHOD, "entity.Remove(components...)", "Removes components.", "Remove($1)");
        item(c, "Destroy", CK_METHOD, "entity.Destroy()", "Destroys the entity at the end of the tick.", "Destroy()");
    }
    if (edges && t.kind == TY_BOOL) {
        item(c, "down", CK_PROPERTY, "bool", "True on the tick it became true.", NULL);
        item(c, "up", CK_PROPERTY, "bool", "True on the tick it became false.", NULL);
    }
}

typedef struct builtin_visit {
    completion *c;
} builtin_visit;

static void add_builtin_member(void *user, const builtin_member *m)
{
    completion *c = ((builtin_visit *)user)->c;
    item(c, m->name, m->is_function ? CK_FUNCTION : CK_CONSTANT, m->detail, m->doc, m->snippet);
}

// After `a.b.`: resolves the chain of names before the dot.
static bool complete_in_namespace(completion *c, str ns, bool systems);

static void complete_members(completion *c, const int dot, const loc at, const bool systems)
{
    int ids[16];
    int n = 0;
    for (int i = dot - 1; i >= 0 && DOC->toks[i].kind == T_IDENT && n < 16;) {
        ids[n++] = i;
        if (i >= 2 && DOC->toks[i - 1].kind == T_DOT) i -= 2;
        else break;
    }
    if (n == 0) return;

    const scope sc = scope_at(at);
    const str base = DOC->toks[ids[n - 1]].text;
    const param *p;
    type t = name_type(&sc, base, &p);
    if (t.kind == TY_ERROR) {
        if (n == 1 && builtin_owner(base)) {
            if (str_eq_c(base, "Draw") && !(sc.decl && sc.decl->is_view)) return;
            builtin_visit v = {c};
            builtin_list_members(base, add_builtin_member, &v);
            return;
        }
        // Game.Combat.: a namespace. In [After(...)] it holds systems, elsewhere types.
        sb ns = {0};
        for (int k = n - 1; k >= 0; k--) {
            sb_printf(&ns, "%s" STR_FMT, k == n - 1 ? "" : ".", STR_ARG(DOC->toks[ids[k]].text));
        }
        complete_in_namespace(c, (str){ns.data, (int)ns.len}, systems);
        return;
    }
    for (int k = n - 2; k >= 0 && t.kind != TY_ERROR; k--) t = member_type(t, DOC->toks[ids[k]].text);
    // input.jump.pressed, in systems
    const bool edges = n == 2 && p && p->type.kind == TY_INPUT;
    list_members(c, t, edges, &sc);
}

// The current document's file: its namespace and `using`s.
static const unit *doc_unit(void)
{
    for (int i = 0; i < A.prog->units.count; i++) {
        if (A.prog->units.items[i]->src == &DOC->src) return A.prog->units.items[i];
    }
    return NULL;
}

// Can the current document name `d` without its namespace?
static bool visible_by_name(const decl *d)
{
    const str ns = d->unit ? d->unit->ns : (str){"", 0};
    if (ns.len == 0) return true;
    const unit *u = doc_unit();
    if (!u) return false;
    // Its own namespace or one it's inside of
    if (u->ns.len >= ns.len && memcmp(u->ns.ptr, ns.ptr, (size_t)ns.len) == 0
        && (u->ns.len == ns.len || u->ns.ptr[ns.len] == '.')) {
        return true;
    }
    for (int i = 0; i < u->usings.count; i++) {
        if (str_eq(u->usings.items[i], ns)) return true;
    }
    return false;
}

// How the current document writes `d`: Health, or Combat.Health.
static const char *name_for(const decl *d)
{
    return str_to_cstr(visible_by_name(d) ? d->name : d->qualified);
}

static void complete_types(completion *c, const bool components, const bool singletons, const bool input)
{
    for (int i = 0; i < A.prog->decls.count; i++) {
        const decl *d = A.prog->decls.items[i];
        if (components && d->kind == DECL_COMPONENT) item(c, name_for(d), CK_STRUCT, "component", NULL, NULL);
        if (singletons && d->kind == DECL_SINGLETON) item(c, name_for(d), CK_CLASS, "singleton", NULL, NULL);
        if (input && d->kind == DECL_INPUT) item(c, name_for(d), CK_INTERFACE, "input", NULL, NULL);
    }
}

// Systems or views, for [Before(...)] and [After(...)].
static void complete_systems(completion *c, const bool views)
{
    for (int i = 0; i < A.prog->decls.count; i++) {
        const decl *d = A.prog->decls.items[i];
        if (d->kind == DECL_SYSTEM && d->is_view == views && !d->is_main) {
            item(c, name_for(d), CK_FUNCTION, views ? "view" : "system", NULL, NULL);
        }
    }
}

// Namespaces as written from the current document: every one for `using`, or
// just the outermost names ("Game" for Game.Combat) where code starts.
static void complete_namespaces(completion *c, const bool full)
{
    for (int i = 0; i < A.prog->units.count; i++) {
        str ns = A.prog->units.items[i]->ns;
        if (ns.len == 0) continue;
        if (!full) {
            const char *dot = memchr(ns.ptr, '.', (size_t)ns.len);
            if (dot) ns.len = (int)(dot - ns.ptr);
        }
        item(c, str_to_cstr(ns), CK_MODULE, "namespace", NULL, NULL);
    }
}

// After `Combat.`: what's inside the namespace. `kinds` picks types or systems.
static bool complete_in_namespace(completion *c, const str ns, const bool systems)
{
    bool any = false;
    for (int i = 0; i < A.prog->units.count; i++) {
        const str u = A.prog->units.items[i]->ns;
        // Namespaces inside it: Game. offers Combat for Game.Combat
        if (u.len > ns.len && memcmp(u.ptr, ns.ptr, (size_t)ns.len) == 0 && u.ptr[ns.len] == '.') {
            str next = {u.ptr + ns.len + 1, u.len - ns.len - 1};
            const char *dot = memchr(next.ptr, '.', (size_t)next.len);
            if (dot) next.len = (int)(dot - next.ptr);
            item(c, str_to_cstr(next), CK_MODULE, "namespace", NULL, NULL);
            any = true;
        }
        if (str_eq(u, ns)) any = true;
    }
    for (int i = 0; i < A.prog->decls.count; i++) {
        const decl *d = A.prog->decls.items[i];
        if (!d->unit || !str_eq(d->unit->ns, ns) || d->is_main) continue;
        if (systems != (d->kind == DECL_SYSTEM)) continue;
        const int kind = d->kind == DECL_COMPONENT ? CK_STRUCT : d->kind == DECL_INPUT ? CK_INTERFACE
                       : d->kind == DECL_SYSTEM ? CK_FUNCTION : CK_CLASS;
        item(c, str_to_cstr(d->name), kind, decl_keyword(d), NULL, NULL);
    }
    return any;
}

static const char *const value_types[] = {
    "bool", "int", "int2", "int3", "int4", "float", "float2", "float3", "float4", "quaternion",
    "float2x2", "float3x3", "float4x4", "Entity", "PlayerID", "Color",
};

static void complete_value_types(completion *c, const bool constructors_only)
{
    for (size_t i = 0; i < sizeof value_types / sizeof value_types[0]; i++) {
        type t;
        builtin_type_named(str_from(value_types[i]), &t);
        if (constructors_only && (t.kind == TY_BOOL || t.kind == TY_ENTITY)) continue;
        item(c, value_types[i], CK_STRUCT, "built-in type", builtin_type_doc(t.kind), NULL);
    }
}

// Names and keywords that can start an expression or statement.
static void complete_expression(completion *c, const loc at, const bool statement)
{
    const scope sc = scope_at(at);
    const bool view = sc.decl && sc.decl->is_view;

    if (statement) {
        static const char *const keywords[] = {"if", "else", "return", "var", "mut"};
        for (size_t i = 0; i < sizeof keywords / sizeof keywords[0]; i++) item(c, keywords[i], CK_KEYWORD, NULL, NULL, NULL);
    }
    item(c, "true", CK_KEYWORD, NULL, NULL, NULL);
    item(c, "false", CK_KEYWORD, NULL, NULL, NULL);

    for (int i = sc.locals.count - 1; i >= 0; i--) {
        const stmt *s = sc.locals.items[i];
        item(c, str_to_cstr(s->name), CK_VARIABLE, type_name(s->type), NULL, NULL);
    }
    if (sc.decl) {
        for (int i = 0; i < sc.decl->params.count && !sc.in_sanitize; i++) {
            const param *p = &sc.decl->params.items[i];
            if (p->name.len == 0) continue;
            sb detail = {0};
            format_param(p, &detail);
            item(c, str_to_cstr(p->name), CK_VARIABLE, detail.data, NULL, NULL);
        }
        if (sc.in_input) {
            for (int i = 0; i < sc.decl->fields.count; i++) {
                const field *f = &sc.decl->fields.items[i];
                item(c, str_to_cstr(f->name), CK_FIELD, type_name(f->type), NULL, NULL);
            }
        }
    }

    if (sc.decl && !view && !sc.in_input) {
        item(c, "Spawn", CK_FUNCTION, "Spawn(components...) -> Entity", "Creates an entity with these components.",
             "Spawn($1)");
        complete_types(c, true, false, false);
    }
    complete_value_types(c, true);
    item(c, "Math", CK_MODULE, "Math functions and constants", NULL, NULL);
    complete_namespaces(c, false);
    if (view) item(c, "Draw", CK_MODULE, "Immediate-mode drawing", NULL, NULL);
}

static void complete_declarations(completion *c)
{
    item(c, "component", CK_SNIPPET, "component Name { fields }", NULL, "component ${1:Name}\n{\n    $0\n}");
    item(c, "singleton", CK_SNIPPET, "singleton Name { fields }", NULL, "singleton ${1:Name}\n{\n    $0\n}");
    item(c, "system", CK_SNIPPET, "system Name(parameters) { ... }", NULL, "system ${1:Name}($2)\n{\n    $0\n}");
    item(c, "view", CK_SNIPPET, "view Name(parameters) { ... }", "Runs once per frame and draws.",
         "view ${1:Name}($2)\n{\n    $0\n}");
    item(c, "input", CK_SNIPPET, "input Name { fields; Sample }", "What a player sends each tick.",
         "input ${1:Name}\n{\n    $0\n\n    Sample(Devices devices)\n    {\n    }\n}");
    item(c, "namespace", CK_KEYWORD, "namespace Name;", "The namespace of everything in this file. Goes at the top.",
         "namespace ${1:Name};");
    item(c, "using", CK_KEYWORD, "using Name;", "Names from another namespace, without writing it. Goes at the top.",
         "using ${1:Name};");
}

// "PlayerInput" -> "playerInput", the usual name for a parameter of that type.
static void complete_param_name(completion *c, const str type_name_)
{
    if (type_name_.len == 0 || !isupper((unsigned char)type_name_.ptr[0])) return;
    char *name = str_to_cstr(type_name_);
    name[0] = (char)tolower((unsigned char)name[0]);
    item(c, name, CK_VARIABLE, NULL, NULL, NULL);
}

// Contexts the cursor can be in, found by scanning tokens.
typedef enum context_kind {
    CTX_TOP, CTX_DATA, CTX_HEADER, CTX_CODE, CTX_LITERAL, CTX_ATTRIBUTE, CTX_ATTRIBUTE_ARGS,
    CTX_FIELD_ATTRIBUTE, CTX_FIELD_ATTRIBUTE_ARGS, // [Clamp(-1, 1)] before an input field
} context_kind;

typedef struct frame {
    context_kind kind;
    int open; // The token that opened it
} frame;

static bool starts_declaration(const int i)
{
    const token *t = &DOC->toks[i];
    if (t->kind == T_COMPONENT || t->kind == T_SINGLETON || t->kind == T_SYSTEM) return true;
    if (t->kind == T_LBRACKET && t->at.col == 1) return !attributes_before_field(DOC->toks, i); // Attributes
    return t->kind == T_IDENT && t->at.col == 1 && DOC->toks[i + 1].kind == T_IDENT
        && (str_eq_c(t->text, "input") || str_eq_c(t->text, "view") || str_eq_c(t->text, "namespace")
            || str_eq_c(t->text, "using"));
}

static bool is_word(const token *t)
{
    return t->text.len > 0 && (isalpha((unsigned char)t->text.ptr[0]) || t->text.ptr[0] == '_') && t->kind != T_STRING;
}

void analysis_completion(const int line, const int character, jbuf *out)
{
    const loc at = from_lsp(line, character);
    completion c = {out, 0, {0}};
    jb_put(out, "{\"isIncomplete\":false,\"items\":[");

    // The last token before the cursor, skipping the word being typed.
    int last = -1;
    for (int i = 0; i < DOC->tok_count && loc_cmp(DOC->toks[i].at, at) < 0; i++) last = i;
    if (last >= 0) {
        const token *t = &DOC->toks[last];
        const bool inside = t->at.line == at.line && at.col <= t->at.col + token_len(t);
        if (inside && t->kind == T_STRING) goto done; // No completion in text
        if (inside && is_word(t)) last--;
    }

    frame stack[64] = {{CTX_TOP, -1}};
    int depth = 0;
    for (int i = 0; i <= last; i++) {
        if (starts_declaration(i)) {
            depth = 0;
            if (DOC->toks[i].kind != T_LBRACKET) continue;
        }
        const context_kind top = stack[depth].kind;
        const tok_kind kind = DOC->toks[i].kind;
        const tok_kind before = i > 0 ? DOC->toks[i - 1].kind : T_EOF;
        if (kind == T_LBRACKET && top == CTX_TOP && depth < 63) {
            stack[++depth] = (frame){CTX_ATTRIBUTE, i};
        } else if (kind == T_RBRACKET && top == CTX_ATTRIBUTE) {
            depth--;
        } else if (kind == T_LPAREN && top == CTX_ATTRIBUTE && depth < 63) {
            stack[++depth] = (frame){CTX_ATTRIBUTE_ARGS, i};
        } else if (kind == T_RPAREN && top == CTX_ATTRIBUTE_ARGS) {
            depth--;
        } else if (kind == T_LBRACKET && top == CTX_DATA && depth < 63) {
            stack[++depth] = (frame){CTX_FIELD_ATTRIBUTE, i};
        } else if (kind == T_RBRACKET && top == CTX_FIELD_ATTRIBUTE) {
            depth--;
        } else if (kind == T_LPAREN && (top == CTX_FIELD_ATTRIBUTE || top == CTX_FIELD_ATTRIBUTE_ARGS) && depth < 63) {
            stack[++depth] = (frame){CTX_FIELD_ATTRIBUTE_ARGS, i}; // One frame per parenthesis: float2(0, 1)
        } else if (kind == T_RPAREN && top == CTX_FIELD_ATTRIBUTE_ARGS) {
            depth--;
        } else if (kind == T_LBRACE && depth < 63) {
            context_kind next = CTX_CODE;
            if (top == CTX_TOP && before == T_IDENT) next = CTX_DATA;
            else if ((top == CTX_CODE || top == CTX_LITERAL) && before == T_IDENT) next = CTX_LITERAL;
            stack[++depth] = (frame){next, i};
        } else if (kind == T_RBRACE && depth > 0) {
            depth--;
        } else if (kind == T_LPAREN && depth < 63) {
            const bool header = top == CTX_TOP
                             || (top == CTX_DATA && before == T_IDENT && str_eq_c(DOC->toks[i - 1].text, "Sample"));
            if (header) stack[++depth] = (frame){CTX_HEADER, i};
        } else if (kind == T_RPAREN && top == CTX_HEADER) {
            depth--;
        }
    }

    const token *prev = last >= 0 ? &DOC->toks[last] : NULL;
    const tok_kind pk = prev ? prev->kind : T_EOF;
    const frame f = stack[depth];

    if (pk == T_DOT) {
        complete_members(&c, last, at, f.kind == CTX_ATTRIBUTE_ARGS);
        goto done;
    }

    switch (f.kind) {
    case CTX_TOP:
        if (pk == T_EOF || pk == T_RBRACE || pk == T_SEMI || pk == T_RBRACKET) complete_declarations(&c);
        else if (pk == T_IDENT && str_eq_c(prev->text, "using")) complete_namespaces(&c, true);
        break;

    case CTX_ATTRIBUTE:
        if (pk == T_LBRACKET || pk == T_COMMA) {
            item(&c, "Before", CK_FUNCTION, "[Before(System)]", "This system runs before the ones named.", "Before($1)");
            item(&c, "After", CK_FUNCTION, "[After(System)]", "This system runs after the ones named.", "After($1)");
        }
        break;

    case CTX_FIELD_ATTRIBUTE:
        if (pk == T_LBRACKET || pk == T_COMMA) {
            item(&c, "Clamp", CK_FUNCTION, "[Clamp(lo, hi)]", "Keeps the field between lo and hi, before Sanitize runs.",
                 "Clamp($1)");
            item(&c, "Min", CK_FUNCTION, "[Min(x)]", "Keeps the field at least x, before Sanitize runs.", "Min($1)");
            item(&c, "Max", CK_FUNCTION, "[Max(x)]", "Keeps the field at most x, before Sanitize runs.", "Max($1)");
        }
        break;

    case CTX_FIELD_ATTRIBUTE_ARGS: // Constant bounds, like a default value
        complete_value_types(&c, true);
        item(&c, "Math", CK_MODULE, "Math functions and constants", NULL, NULL);
        break;

    case CTX_ATTRIBUTE_ARGS:
        if (pk == T_LPAREN || pk == T_COMMA) {
            complete_systems(&c, false);
            complete_systems(&c, true);
            complete_namespaces(&c, false);
        }
        break;

    case CTX_HEADER: {
        const bool sample = f.open < 2 || !(DOC->toks[f.open - 2].kind == T_SYSTEM || str_eq_c(DOC->toks[f.open - 2].text, "view"));
        if (sample) {
            if (pk == T_LPAREN) item(&c, "Devices", CK_CLASS, "The keyboard, mouse and gamepad", NULL, NULL);
            else if (pk == T_IDENT) item(&c, "devices", CK_VARIABLE, NULL, NULL, NULL);
        } else if (pk == T_WITH || pk == T_WITHOUT) {
            complete_types(&c, true, false, false);
        } else if (pk == T_LPAREN || pk == T_COMMA || pk == T_MUT) {
            if (pk != T_MUT) {
                item(&c, "mut", CK_KEYWORD, "Write access", NULL, NULL);
                item(&c, "with", CK_KEYWORD, "Entities must have this component", NULL, NULL);
                item(&c, "without", CK_KEYWORD, "Entities must not have this component", NULL, NULL);
                item(&c, "Entity", CK_STRUCT, "The entity being processed", NULL, NULL);
            }
            complete_types(&c, true, true, pk != T_MUT);
            complete_namespaces(&c, false);
        } else if (pk == T_IDENT) {
            complete_param_name(&c, prev->text);
        }
        break;
    }

    case CTX_DATA:
        if (pk == T_LBRACE || pk == T_SEMI || pk == T_RBRACE) complete_value_types(&c, false);
        else if (pk != T_IDENT || (last >= 1 && DOC->toks[last - 1].kind != T_LBRACE && DOC->toks[last - 1].kind != T_SEMI
                                   && DOC->toks[last - 1].kind != T_RBRACE)) {
            // A default value: constants only.
            complete_value_types(&c, true);
            item(&c, "Math", CK_MODULE, "Math functions and constants", NULL, NULL);
        }
        break;

    case CTX_LITERAL:
        if ((pk == T_LBRACE || pk == T_COMMA) && f.open >= 1) {
            const str name = DOC->toks[f.open - 1].text;
            for (int i = 0; i < A.prog->decls.count; i++) {
                const decl *d = A.prog->decls.items[i];
                if (d->kind != DECL_COMPONENT || !str_eq(d->name, name)) continue;
                for (int k = 0; k < d->fields.count; k++) {
                    const field *fl = &d->fields.items[k];
                    sb snippet = {0};
                    sb_printf(&snippet, STR_FMT " = $0", STR_ARG(fl->name));
                    item(&c, str_to_cstr(fl->name), CK_FIELD, type_name(fl->type), NULL, snippet.data);
                }
            }
        } else {
            complete_expression(&c, at, false);
        }
        break;

    case CTX_CODE: {
        if (pk == T_VAR) break; // Naming a local
        // `Type name` declares a local: no suggestions for the name.
        const tok_kind before = last >= 1 ? DOC->toks[last - 1].kind : T_EOF;
        type ignored;
        const bool statement_start = before == T_LBRACE || before == T_RBRACE || before == T_SEMI || before == T_MUT;
        if (pk == T_IDENT && statement_start && builtin_type_named(prev->text, &ignored)) break;
        const bool at_statement = pk == T_LBRACE || pk == T_RBRACE || pk == T_SEMI || pk == T_RPAREN || pk == T_ELSE;
        complete_expression(&c, at, at_statement);
        break;
    }
    }

done:
    jb_put(out, "]}");
}

// ---------------------------------------------------------------------------
// References, highlights and rename: every occurrence of one symbol

// Do `a` and `b` refer to the same thing?
static bool same_symbol(const occurrence *a, const occurrence *b)
{
    if (a->kind != b->kind) return false;
    switch (a->kind) {
    case OCC_TYPE: return a->decl ? a->decl == b->decl : !b->decl && a->type.kind == b->type.kind;
    case OCC_SYSTEM: return a->decl == b->decl;
    case OCC_FIELD: return a->field == b->field;
    case OCC_PARAM: return a->param == b->param;
    case OCC_LOCAL: return a->local == b->local;
    case OCC_OWNER: return str_eq(a->owner, b->owner);
    case OCC_FUNCTION:
    case OCC_CONSTANT: return str_eq(a->owner, b->owner) && str_eq(a->name, b->name);
    case OCC_METHOD:
    case OCC_MEMBER:
    case OCC_NAMESPACE:
    case OCC_ATTRIBUTE: return str_eq(a->name, b->name);
    }
    return false;
}

void analysis_references(const char *uri, const int line, const int character, const bool declaration, jbuf *out)
{
    (void)uri;
    const occurrence *target = occurrence_at(from_lsp(line, character));
    jb_put(out, "[");
    int written = 0;
    for (int i = 0; target && i < A.occs.count; i++) {
        const occurrence *o = &A.occs.items[i];
        if (!same_symbol(target, o) || (o->declaration && !declaration)) continue;
        if (written++) jb_put(out, ",");
        jb_put(out, "{\"uri\":");
        jb_string(out, A.files[o->at.file].uri); // Any file of the game
        jb_put(out, ",\"range\":");
        write_range(out, o->at, o->len);
        jb_put(out, "}");
    }
    jb_put(out, "]");
}

enum { HIGHLIGHT_READ = 2, HIGHLIGHT_WRITE = 3 };

void analysis_highlights(const int line, const int character, jbuf *out)
{
    const occurrence *target = occurrence_at(from_lsp(line, character));
    jb_put(out, "[");
    int written = 0;
    for (int i = 0; target && i < A.occs.count; i++) {
        const occurrence *o = &A.occs.items[i];
        if (!same_symbol(target, o) || o->at.file != A.doc) continue; // Highlights are per document
        if (written++) jb_put(out, ",");
        jb_put(out, "{\"range\":");
        write_range(out, o->at, o->len);
        jb_printf(out, ",\"kind\":%d}", o->declaration ? HIGHLIGHT_WRITE : HIGHLIGHT_READ);
    }
    jb_put(out, "]");
}

// The occurrence at the cursor if it can be renamed, or an error for the user.
static const char *rename_target(const int line, const int character, const occurrence **out)
{
    if (A.syntax_errors > 0) return "Fix the syntax errors first: uses in code that doesn't parse would be missed.";
    const occurrence *o = occurrence_at(from_lsp(line, character));
    *out = o;
    if (!o) return "Nothing to rename here.";
    switch (o->kind) {
    case OCC_TYPE:
        if (!o->decl) return "Built-in types can't be renamed.";
        if (o->decl->builtin) return "Types built into the engine can't be renamed.";
        return NULL;
    case OCC_SYSTEM:
        if (o->decl->is_main) return "Main is the entry point, so it keeps its name.";
        return NULL;
    case OCC_FIELD:
        if (!o->decl || o->decl->builtin) return "Fields of built-in types can't be renamed.";
        return NULL;
    case OCC_PARAM:
    case OCC_LOCAL:
        return NULL;
    case OCC_NAMESPACE:
        return "Renaming namespaces isn't supported yet: change the `namespace` line in each of its files.";
    default:
        return "Built-in names can't be renamed.";
    }
}

const char *analysis_prepare_rename(const int line, const int character, jbuf *out)
{
    const occurrence *o;
    const char *error = rename_target(line, character, &o);
    if (error) return error;
    write_range(out, o->at, o->len);
    return NULL;
}

static bool local_named(const stmt *s, const str name)
{
    if (!s) return false;
    if (s->kind == S_VAR && str_eq(s->name, name)) return true;
    if (s->kind == S_BLOCK) {
        for (int i = 0; i < s->stmts.count; i++) {
            if (local_named(s->stmts.items[i], name)) return true;
        }
    }
    return s->kind == S_IF && (local_named(s->then_stmt, name) || local_named(s->else_stmt, name));
}

// The system, view or input whose body declares `local`.
static const decl *decl_of_local(const stmt *local)
{
    for (int i = 0; i < A.prog->decls.count; i++) {
        const decl *d = A.prog->decls.items[i];
        if (d->body && loc_cmp(d->body->at, local->at) < 0 && loc_cmp(local->at, d->body->end) < 0) return d;
        if (d->sanitize && loc_cmp(d->sanitize->at, local->at) < 0 && loc_cmp(local->at, d->sanitize->end) < 0) {
            return d;
        }
    }
    return NULL;
}

// The system, view or input that declares `p`.
static const decl *decl_of_param(const param *p)
{
    for (int i = 0; i < A.prog->decls.count; i++) {
        const decl *d = A.prog->decls.items[i];
        for (int k = 0; k < d->params.count; k++) {
            if (&d->params.items[k] == p) return d;
        }
    }
    return NULL;
}

static bool param_named(const decl *d, const str name)
{
    for (int i = 0; d && i < d->params.count; i++) {
        if (d->params.items[i].name.len > 0 && str_eq(d->params.items[i].name, name)) return true;
    }
    return false;
}

// Whether `name` can replace the target's name, or why not.
static const char *check_new_name(const occurrence *target, const str name)
{
    static const char *const keywords[] = {"component", "singleton", "system", "mut", "var", "with",
                                           "without", "if", "else", "return", "true", "false"};
    static const char *const reserved[] = {"Math", "Draw", "Devices", "Time", "Owner", "Spawn"};
    static char message[160];

    if (name.len == 0 || !(isalpha((unsigned char)name.ptr[0]) || name.ptr[0] == '_')) return "Names start with a letter.";
    for (int i = 0; i < name.len; i++) {
        if (!isalnum((unsigned char)name.ptr[i]) && name.ptr[i] != '_') return "Names use letters, digits and '_' only.";
    }
    for (size_t i = 0; i < sizeof keywords / sizeof keywords[0]; i++) {
        if (str_eq_c(name, keywords[i])) return "That's a keyword.";
    }
    type ignored;
    for (size_t i = 0; i < sizeof reserved / sizeof reserved[0]; i++) {
        if (str_eq_c(name, reserved[i])) return "That name is built into the language.";
    }
    if (builtin_type_named(name, &ignored)) return "That name is built into the language.";
    if (str_starts_with_c(name, "purr_")) return "Names starting with 'purr_' are reserved for generated code.";

    bool clash = false;
    switch (target->kind) {
    case OCC_TYPE:
    case OCC_SYSTEM:
        // Types share one namespace; systems and views share another.
        for (int i = 0; i < A.prog->decls.count; i++) {
            const decl *d = A.prog->decls.items[i];
            const bool same_namespace = (d->kind == DECL_SYSTEM) == (target->kind == OCC_SYSTEM);
            if (d != target->decl && same_namespace && str_eq(d->name, name)) clash = true;
        }
        break;
    case OCC_FIELD:
        for (int i = 0; i < target->decl->fields.count; i++) {
            const field *f = &target->decl->fields.items[i];
            if (f != target->field && str_eq(f->name, name)) clash = true;
        }
        break;
    case OCC_PARAM: {
        const decl *d = decl_of_param(target->param);
        clash = param_named(d, name) || (d && local_named(d->body, name));
        break;
    }
    case OCC_LOCAL: {
        const decl *d = decl_of_local(target->local);
        clash = param_named(d, name) || (d && local_named(d->body, name));
        break;
    }
    default:
        break;
    }
    if (clash) {
        snprintf(message, sizeof message, "'" STR_FMT "' is already declared there.", STR_ARG(name));
        return message;
    }
    return NULL;
}

const char *analysis_rename(const char *uri, const int line, const int character, const char *new_name, jbuf *out)
{
    const occurrence *target;
    const char *error = rename_target(line, character, &target);
    if (error) return error;
    error = check_new_name(target, str_from(new_name));
    if (error) return error;

    // Edits grouped by file: every file of the game that uses the name.
    (void)uri;
    jb_put(out, "{\"changes\":{");
    int files_written = 0;
    for (int file = 0; file < A.file_count; file++) {
        int written = 0;
        for (int i = 0; i < A.occs.count; i++) {
            const occurrence *o = &A.occs.items[i];
            if (o->at.file != file || !same_symbol(target, o)) continue;
            if (written++ == 0) {
                if (files_written++) jb_put(out, ",");
                jb_string(out, A.files[file].uri);
                jb_put(out, ":[");
            } else {
                jb_put(out, ",");
            }
            jb_put(out, "{\"range\":");
            write_range(out, o->at, o->len);
            jb_put(out, ",\"newText\":");
            jb_string(out, new_name);
            jb_put(out, "}");
        }
        if (written) jb_put(out, "]");
    }
    jb_put(out, "}}");
    return NULL;
}

// ---------------------------------------------------------------------------
// Signature help: the parameters of the call being typed

typedef struct signature_list {
    jbuf *out;
    int arg;          // The argument the cursor is in
    int active;       // The first signature with enough parameters, or -1
    int active_param; // Its parameter under the cursor
    int shown;
} signature_list;

// Adds a signature, finding its parameters in the label: "Name(float2 center, float radius)".
static void add_signature(signature_list *s, const char *label, const char *doc)
{
    const char *open = strchr(label, '(');
    const char *close = open ? strchr(open, ')') : NULL;
    if (!open || !close) return;
    int starts[16];
    int ends[16];
    int params = 0;
    const char *p = open + 1;
    while (p < close && params < 16) {
        const char *end = p;
        while (end < close && *end != ',') end++;
        starts[params] = (int)(p - label);
        ends[params] = (int)(end - label);
        params++;
        p = end < close ? end + 2 : close;
    }
    const bool variadic = strstr(label, "...") != NULL;
    if (s->active < 0 && (variadic || params > s->arg)) {
        s->active = s->shown;
        s->active_param = variadic && s->arg >= params ? params - 1 : s->arg;
    }

    if (s->shown++) jb_put(s->out, ",");
    jb_put(s->out, "{\"label\":");
    jb_string(s->out, label);
    if (doc) {
        jb_put(s->out, ",\"documentation\":{\"kind\":\"markdown\",\"value\":");
        jb_string(s->out, doc);
        jb_put(s->out, "}");
    }
    jb_put(s->out, ",\"parameters\":[");
    for (int i = 0; i < params; i++) jb_printf(s->out, "%s{\"label\":[%d,%d]}", i ? "," : "", starts[i], ends[i]);
    jb_put(s->out, "]}");
}

static void visit_signature(void *user, const char *label, const char *doc)
{
    add_signature(user, label, doc);
}

// Constructors and other calls that aren't in the built-in function table.
static const struct {
    const char *name;
    const char *forms[3];
    const char *doc;
} call_forms[] = {
    {"int", {"int(float value)"}, "Truncates toward zero and saturates at the int range. NaN gives 0."},
    {"float", {"float(int value)"}, NULL},
    {"int2", {"int2(int x, int y)", "int2(int value)", "int2(float2 value)"}, NULL},
    {"int3", {"int3(int x, int y, int z)", "int3(int2 xy, int z)", "int3(int value)"}, NULL},
    {"int4", {"int4(int x, int y, int z, int w)", "int4(int3 xyz, int w)", "int4(int value)"}, NULL},
    {"float2", {"float2(float x, float y)", "float2(float value)", "float2(int2 value)"}, NULL},
    {"float3", {"float3(float x, float y, float z)", "float3(float2 xy, float z)", "float3(float value)"}, NULL},
    {"float4", {"float4(float x, float y, float z, float w)", "float4(float3 xyz, float w)", "float4(float value)"}, NULL},
    {"quaternion", {"quaternion(float x, float y, float z, float w)", "quaternion(float4 value)", "quaternion(float3x3 rotation)"}, NULL},
    {"float2x2", {"float2x2(float2 c0, float2 c1)", "float2x2(float m00, float m01, float m10, float m11)"}, "Columns, or numbers row by row."},
    {"float3x3", {"float3x3(float3 c0, float3 c1, float3 c2)", "float3x3(quaternion rotation)"}, NULL},
    {"float4x4", {"float4x4(float4 c0, float4 c1, float4 c2, float4 c3)", "float4x4(float3x3 rotation, float3 translation)"}, NULL},
    {"Color", {"Color(float r, float g, float b)", "Color(float r, float g, float b, float a)"}, "Channels from 0 to 1."},
    {"PlayerID", {"PlayerID(int index)"}, "A player by index, for local play and tests."},
    {"Spawn", {"Spawn(components...)"}, "Creates an entity with these components. It's added at the end of the tick."},
};

void analysis_signature_help(const int line, const int character, jbuf *out)
{
    const loc at = from_lsp(line, character);
    int last = -1;
    for (int i = 0; i < DOC->tok_count && loc_cmp(DOC->toks[i].at, at) < 0; i++) last = i;

    // The innermost '(' still open at the cursor, counting the commas after it.
    int parens = 0;
    int braces = 0;
    int commas = 0;
    int open = -1;
    for (int i = last; i >= 0 && open < 0; i--) {
        switch (DOC->toks[i].kind) {
        case T_RPAREN: parens++; break;
        case T_RBRACE: braces++; break;
        case T_LPAREN:
            if (parens == 0) open = i;
            else parens--;
            break;
        case T_LBRACE:
            if (braces == 0) i = -1; // A block: not inside a call
            else braces--;
            break;
        case T_SEMI:
            if (parens == 0 && braces == 0) i = -1;
            break;
        case T_COMMA:
            if (parens == 0 && braces == 0) commas++;
            break;
        default:
            break;
        }
    }
    if (open < 1 || DOC->toks[open - 1].kind != T_IDENT) {
        jb_put(out, "null");
        return;
    }
    const str name = DOC->toks[open - 1].text;
    const bool method = open >= 3 && DOC->toks[open - 2].kind == T_DOT && DOC->toks[open - 3].kind == T_IDENT;

    jbuf list = {0};
    signature_list s = {&list, commas, -1, commas, 0};
    if (method && builtin_owner(DOC->toks[open - 3].text)) {
        builtin_signatures(DOC->toks[open - 3].text, name, visit_signature, &s);
    } else if (method && (str_eq_c(name, "Add") || str_eq_c(name, "Remove"))) {
        add_signature(&s, str_eq_c(name, "Add") ? "entity.Add(components...)" : "entity.Remove(components...)", NULL);
    } else if (!method) {
        for (size_t i = 0; i < sizeof call_forms / sizeof call_forms[0]; i++) {
            if (!str_eq_c(name, call_forms[i].name)) continue;
            for (int f = 0; f < 3 && call_forms[i].forms[f]; f++) add_signature(&s, call_forms[i].forms[f], call_forms[i].doc);
        }
    }
    if (s.shown == 0) {
        jb_put(out, "null");
    } else {
        jb_put(out, "{\"signatures\":[");
        jb_putn(out, list.data, list.len);
        jb_printf(out, "],\"activeSignature\":%d,\"activeParameter\":%d}", s.active < 0 ? 0 : s.active, s.active_param);
    }
    jb_free(&list);
}

// ---------------------------------------------------------------------------
// Formatting: indentation, spacing, and C#-style braces: those of a block that
// spans lines go on lines of their own. Otherwise tokens and comments stay on
// their lines, in their order, so formatting can't change what the code means.

typedef struct fmt_item {
    const char *text; // Verbatim: a token (strings with their quotes) or a comment
    int len;
    int tok;          // Token index, or -1 for a comment
    bool unary;       // A '-' that negates
} fmt_item;

typedef VEC(fmt_item) fmt_line;

// 1-based line holding `p`.
static int line_of(const char *p)
{
    int lo = 0;
    int hi = DOC->lines.count - 1;
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        if (DOC->lines.items[mid] <= p) lo = mid;
        else hi = mid - 1;
    }
    return lo + 1;
}

// A line's text, without its line break.
static str line_text(const int line)
{
    const char *start = DOC->lines.items[line - 1];
    const char *end = line < DOC->lines.count ? DOC->lines.items[line] : DOC->src.text + DOC->src.len;
    if (end > start && end[-1] == '\n') end--;
    if (end > start && end[-1] == '\r') end--;
    return (str){start, (int)(end - start)};
}

static const char *token_start(const token *t)
{
    return t->kind == T_STRING ? t->text.ptr - 1 : t->text.ptr;
}

static bool ends_operand(const tok_kind k)
{
    return k == T_IDENT || k == T_INT || k == T_FLOAT || k == T_STRING || k == T_RPAREN || k == T_RBRACKET
        || k == T_TRUE || k == T_FALSE;
}

static bool space_between(const fmt_item *a, const fmt_item *b)
{
    if (a->tok < 0 || b->tok < 0) return true; // Comments
    const tok_kind x = DOC->toks[a->tok].kind;
    const tok_kind y = DOC->toks[b->tok].kind;
    if (y == T_RPAREN || y == T_RBRACKET || y == T_COMMA || y == T_SEMI || y == T_DOT) return false;
    if (x == T_LPAREN || x == T_LBRACKET || x == T_DOT) return false;
    if (a->unary || x == T_NOT || x == T_TILDE) return false;
    if (y == T_LPAREN || y == T_LBRACKET) return x != T_IDENT; // Calls, but `if (` and `* (`
    return true;
}

// Whether the `{` at token `i` starts a literal, like Body { position = p } or
// Combat.Health { amount = 1 }: it follows a name where an expression goes.
// Declarations have a word before their name (component Body {, input Keys {).
static bool opens_literal(const int i)
{
    int k = i - 1;
    if (k < 0 || DOC->toks[k].kind != T_IDENT) return false;
    while (k >= 2 && DOC->toks[k - 1].kind == T_DOT && DOC->toks[k - 2].kind == T_IDENT) k -= 2;
    if (k == 0) return false;
    const tok_kind before = DOC->toks[k - 1].kind;
    return before != T_IDENT && before != T_COMPONENT && before != T_SINGLETON && before != T_SEMI
        && before != T_LBRACE && before != T_RBRACE && before != T_RBRACKET;
}

enum { BREAK_BEFORE = 1, BREAK_AFTER = 2 };

// Where lines break for C#-style braces, per token: around both braces of a
// block that spans lines (or isn't closed yet), and before an `else` that
// follows a block when its own block spans lines. Literals, and blocks on one
// line, stay as they are.
static unsigned char *brace_breaks(void)
{
    unsigned char *breaks = arena_alloc((size_t)DOC->tok_count + 1);
    int *open = arena_alloc(sizeof(int) * ((size_t)DOC->tok_count + 1));
    int depth = 0;
    for (int i = 0; i < DOC->tok_count; i++) {
        const tok_kind k = DOC->toks[i].kind;
        if (k == T_LBRACE) {
            open[depth++] = i;
        } else if (k == T_RBRACE && depth > 0) {
            const int o = open[--depth];
            if (opens_literal(o) || DOC->toks[o].at.line == DOC->toks[i].at.line) continue;
            breaks[o] = BREAK_BEFORE | BREAK_AFTER;
            breaks[i] = BREAK_BEFORE | BREAK_AFTER;
        }
    }
    while (depth > 0) {
        const int o = open[--depth];
        if (!opens_literal(o)) breaks[o] = BREAK_BEFORE | BREAK_AFTER;
    }
    for (int i = 1; i + 1 < DOC->tok_count; i++) {
        if (DOC->toks[i].kind == T_ELSE && DOC->toks[i - 1].kind == T_RBRACE && (breaks[i + 1] & BREAK_BEFORE)) {
            breaks[i] |= BREAK_BEFORE;
        }
    }
    return breaks;
}

static void write_edit(jbuf *out, int *count, const loc start, const loc end, const char *text, const size_t len)
{
    if ((*count)++) jb_put(out, ",");
    jb_put(out, "{\"range\":{\"start\":");
    write_position(out, start);
    jb_put(out, ",\"end\":");
    write_position(out, end);
    jb_put(out, "},\"newText\":");
    jb_string_n(out, text, len);
    jb_put(out, "}");
}

const char *analysis_format(int tab_size, const bool insert_spaces, jbuf *out)
{
    if (DOC->lex_errors > 0) return "The file has text PurrLang can't read. Fix that first.";
    if (tab_size <= 0) tab_size = 4;

    const bool final_newline = DOC->src.len > 0 && DOC->src.text[DOC->src.len - 1] == '\n';
    const int line_count = DOC->lines.count - (final_newline ? 1 : 0); // The empty "line" after a final \n isn't one
    fmt_line *lines = arena_alloc(sizeof(fmt_line) * (size_t)(DOC->lines.count + 1));
    bool *verbatim = arena_alloc(sizeof(bool) * (size_t)(DOC->lines.count + 2));

    // Tokens and comments, line by line, in text order.
    const char *p = DOC->lines.items[0];
    for (int i = 0; i <= DOC->tok_count; i++) {
        const token *t = &DOC->toks[i];
        const char *next = i < DOC->tok_count ? token_start(t) : DOC->src.text + DOC->src.len;
        while (p < next) {
            if (p + 1 < next && p[0] == '/' && (p[1] == '/' || p[1] == '*')) {
                const char *start = p;
                const int first_line = line_of(start);
                if (p[1] == '/') {
                    while (p < next && *p != '\n') p++;
                } else {
                    p += 2;
                    while (p + 1 < next && !(p[0] == '*' && p[1] == '/')) p++;
                    p += 2;
                }
                const int last_line = line_of(p - 1);
                // A comment over several lines keeps its first line's part here;
                // its other lines stay exactly as they are.
                const str first = line_text(first_line);
                const char *end = last_line == first_line ? p : first.ptr + first.len;
                while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) end--;
                const fmt_item item = {start, (int)(end - start), -1, false};
                vec_push(lines[first_line], item);
                for (int l = first_line + 1; l <= last_line; l++) verbatim[l] = true;
            } else {
                p++;
            }
        }
        if (i == DOC->tok_count) break;
        const bool unary = t->kind == T_MINUS && (i == 0 || !ends_operand(DOC->toks[i - 1].kind));
        const fmt_item item = {token_start(t), token_len(t), i, unary};
        vec_push(lines[t->at.line], item);
        p = next + token_len(t);
    }

    int last_content = 0;
    for (int l = 1; l <= line_count; l++) {
        if (lines[l].count > 0 || verbatim[l]) last_content = l;
    }
    const unsigned char *breaks = brace_breaks();
    const char *first_break = memchr(DOC->src.text, '\n', DOC->src.len);
    const char *newline = first_break && first_break > DOC->src.text && first_break[-1] == '\r' ? "\r\n" : "\n";

    jb_put(out, "[");
    int edits = 0;
    int depth = 0;
    int parens = 0;
    // `parens` at each brace depth: inside a literal in a call, like
    // Spawn(Body {, only parentheses opened since the brace continue a line.
    int *parens_at = arena_alloc(sizeof(int) * ((size_t)DOC->tok_count + 1));
    int pending = 0; // Extra indents for the statement after `if (...)` or `else` without braces
    bool blank_before = true; // Drops blank lines at the start of the file
    // The last line with code: did it end a statement or block, or open a braceless if?
    bool statement_open = false;
    bool opened_pending = false;
    for (int l = 1; l <= line_count; l++) {
        const str original = line_text(l);
        const fmt_line *items = &lines[l];
        const loc start = {l, 1, A.doc};
        const loc end = {l, original.len + 1, A.doc};

        if (items->count == 0 && !verbatim[l]) {
            // Blank: at most one in a row, none at the start or the end.
            const bool drop = blank_before || l > last_content;
            if (drop && l < DOC->lines.count) {
                write_edit(out, &edits, start, (loc){l + 1, 1, A.doc}, "", 0);
            } else if (original.len > 0) {
                write_edit(out, &edits, start, end, "", 0);
            }
            blank_before = true;
            continue;
        }
        blank_before = false;

        // The line's pieces: it breaks before a token that must start a line,
        // and after one that must end it (comments stay behind it).
        int *starts = arena_alloc(sizeof(int) * ((size_t)items->count + 1));
        int pieces = 1;
        if (!verbatim[l]) {
            int prev_tok = items->items[0].tok;
            for (int k = 1; k < items->count; k++) {
                const int tok = items->items[k].tok;
                if (tok < 0) continue;
                if ((breaks[tok] & BREAK_BEFORE) || (prev_tok >= 0 && (breaks[prev_tok] & BREAK_AFTER))) starts[pieces++] = k;
                prev_tok = tok;
            }
        }
        starts[pieces] = items->count;

        sb text = {0};
        for (int piece = 0; piece < pieces; piece++) {
            const fmt_item *from = &items->items[starts[piece]];
            const fmt_item *to = &items->items[starts[piece + 1]];

            int last_tok = -1;
            bool has_if = false;
            for (const fmt_item *item = from; item < to; item++) {
                if (item->tok < 0) continue;
                last_tok = item->tok;
                if (DOC->toks[item->tok].kind == T_IF) has_if = true;
            }

            if (!verbatim[l]) {
                if (piece > 0) sb_put(&text, newline);
                const tok_kind first = from->tok >= 0 ? DOC->toks[from->tok].kind : T_EOF;
                int level = depth;
                if (first == T_RBRACE) level--;
                if (first != T_LBRACE) level += pending;
                if (level < 0) level = 0;

                // A line that continues an expression or an argument list goes one
                // level deeper, or keeps its own indentation if that's deeper
                // still: code aligned under an opening parenthesis stays aligned.
                const bool continuation = first != T_LBRACE && first != T_RBRACE && (depth > 0 || parens > 0)
                                       && ((parens > parens_at[depth] && first != T_RPAREN)
                                           || (statement_open && !opened_pending));
                int own_width = 0;
                if (piece == 0) {
                    for (const char *c = original.ptr; c < from->text; c++) own_width += *c == '\t' ? tab_size : 1;
                }
                if (continuation && own_width > (level + 1) * tab_size) {
                    sb_putn(&text, original.ptr, (size_t)(from->text - original.ptr));
                } else {
                    if (continuation) level++;
                    for (int k = 0; k < level; k++) {
                        if (insert_spaces) sb_printf(&text, "%*s", tab_size, "");
                        else sb_put(&text, "\t");
                    }
                }
                for (const fmt_item *item = from; item < to; item++) {
                    if (item > from && item->tok < 0) {
                        // Before a trailing comment, keep the author's spacing (often alignment).
                        const char *gap = item[-1].text + item[-1].len;
                        if (item->text > gap) sb_putn(&text, gap, (size_t)(item->text - gap));
                        else sb_put(&text, " ");
                    } else if (item > from && space_between(item - 1, item)) {
                        sb_put(&text, " ");
                    }
                    sb_putn(&text, item->text, (size_t)item->len);
                }
            }

            for (const fmt_item *item = from; item < to; item++) {
                if (item->tok < 0) continue;
                switch (DOC->toks[item->tok].kind) {
                case T_LBRACE: parens_at[++depth] = parens; break;
                case T_RBRACE: if (depth > 0) depth--; break;
                case T_LPAREN: parens++; break;
                case T_RPAREN: if (parens > 0) parens--; break;
                default: break;
                }
            }
            if (last_tok >= 0) {
                const tok_kind k = DOC->toks[last_tok].kind;
                opened_pending = false;
                if (k == T_SEMI || k == T_LBRACE || k == T_RBRACE) {
                    pending = 0;
                } else if ((k == T_RPAREN && has_if && parens == 0) || k == T_ELSE) {
                    pending++;
                    opened_pending = true;
                }
                statement_open = k != T_SEMI && k != T_LBRACE && k != T_RBRACE;
            }
        }
        if (!verbatim[l] && (text.len != (size_t)original.len || memcmp(text.data, original.ptr, text.len) != 0)) {
            write_edit(out, &edits, start, end, text.data, text.len);
        }
    }

    // End with exactly one line break.
    if (!final_newline && last_content > 0 && last_content == line_count) {
        const loc end = {line_count, line_text(line_count).len + 1, A.doc};
        write_edit(out, &edits, end, end, newline, strlen(newline));
    }
    jb_put(out, "]");
    return NULL;
}
