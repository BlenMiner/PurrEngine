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

// A note about another place in the code, which the editor can show there.
typedef struct related_note {
    loc at;
    const char *message;
} related_note;

typedef struct diagnostic {
    diag_severity severity;
    loc at;
    sb message; // Notes are appended on their own lines, but those about another place where the editor shows them there
    VEC(related_note) related;
    loc fix_at; // A "did you mean": the name written, `fix_len` bytes, which `fix` replaces
    int fix_len;
    str fix;
} diagnostic;

// A name in the source and what it refers to.
typedef enum occ_kind {
    OCC_TYPE,     // Component, singleton, input, record, struct, event or built-in value type
    OCC_SYSTEM,   // System, view or event handler
    OCC_FIELD,
    OCC_PARAM,
    OCC_LOCAL,
    OCC_OWNER,    // Math, Draw
    OCC_FUNCTION, // Math.Dot, Draw.Circle, Spawn, Send, and the game's functions (with `decl`)
    OCC_CONSTANT, // Math.PI, Color.red
    OCC_METHOD,   // e.Add, e.Remove, e.Destroy, e.Send, and methods of structs and components (with `decl`)
    OCC_MEMBER,   // Swizzles, color channels, quaternion.value, matrix columns, input .down/.up
    OCC_NAMESPACE, // Combat in `namespace Combat;` or Combat.Health
    OCC_ATTRIBUTE, // Before, After, Clamp, Min, Max
    OCC_ENUM_MEMBER, // Title in Page.Title, and in `enum Page { Title }` (with `decl`, the enum)
    OCC_THIS,     // `this`, with `decl` the system, view, handler or method it's in
    OCC_DEFAULT,  // `default`, with the type it takes
    OCC_CONST,    // A constant the game declares, MAX_HEALTH, with `decl`
    OCC_SETTING,  // One of the engine's settings, tickRate in settings { ... }
} occ_kind;

typedef struct occurrence {
    loc at;
    int len;
    occ_kind kind;
    bool declaration;
    const decl *decl;   // The type, system, method or function, or a field's owner. NULL for built-in value types.
    const field *field;
    const param *param;
    const decl *param_of;
    const stmt *local;
    str owner;          // Functions and constants: "Math", "Draw", "Color"...
    str name;
    type type;
    type object_type;   // Members, and built-in methods: the type they're read from or called on
    const char *c_name; // Functions and constants: what they are in C, like tide_draw_circle
    builtin_call call;  // Built-in methods: which kind, CALL_LIST for items.Add(...)
    int order;          // When it was found, which decides between two at one position
    bool write;         // What an assignment changes: `x` in `x = 1`, `radius` in `body.radius += 1`
    bool backing;       // An enum's backing type: byte in `enum Voxel : byte`
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
    VEC(const expr *) calls; // Calls of the game's methods and functions, for inlay hints
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

// Whether the editor shows diagnostics' notes about other places there
// (relatedInformation). From its capabilities, at initialization.
static bool related_information;

void analysis_set_related_information(const bool shows)
{
    related_information = shows;
}

static void collect(void *user, const diag_severity severity, const loc at, const char *message)
{
    (void)user;
    if (severity == DIAG_NOTE) {
        if (A.diags.count == 0) return;
        diagnostic *d = &A.diags.items[A.diags.count - 1];
        if (at.line > 0) {
            char *copy = arena_alloc(strlen(message) + 1);
            memcpy(copy, message, strlen(message));
            const related_note note = {at, copy};
            vec_push(d->related, note);
            if (related_information) return;
        }
        sb_printf(&d->message, "\nnote: %s", message);
        return;
    }
    diagnostic d = {severity, at, {0}, {0}, {0, 0, 0}, 0, {NULL, 0}};
    sb_put(&d.message, message);
    vec_push(A.diags, d);
}

static str last_part(str text);

// A "did you mean" for the last diagnostic: the name written, where it's a
// slice of a file's text, and the one meant, which a quick fix puts there.
static void collect_suggestion(void *user, const str wrong, const str best)
{
    (void)user;
    if (A.diags.count == 0 || best.len == 0) return;
    for (int f = 0; f < A.file_count; f++) {
        const afile *file = &A.files[f];
        if (wrong.ptr < file->src.text || wrong.ptr + wrong.len > file->src.text + file->src.len) continue;
        // Combat.Helth for Health: its last part
        const str written = memchr(best.ptr, '.', (size_t)best.len) ? wrong : last_part(wrong);
        int line = 0;
        while (line + 1 < file->lines.count && file->lines.items[line + 1] <= written.ptr) line++;
        diagnostic *d = &A.diags.items[A.diags.count - 1];
        d->fix_at = (loc){line + 1, (int)(written.ptr - file->lines.items[line]) + 1, f};
        d->fix_len = written.len;
        d->fix = best;
        return;
    }
}

static void add_occ(occurrence o)
{
    o.order = A.occs.count;
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

// Each part of the namespace `path` written at `at`: Game and Game.Combat in
// `namespace Game.Combat;`. A part is named by the path up to it, so the Combat
// in Game.Combat is never another namespace's Combat.
static void namespace_parts(const loc at, const str path, const bool declaration)
{
    int t = token_at(at);
    if (t < 0) return;
    const afile *f = &A.files[at.file];
    for (int start = 0; start < path.len && t < f->tok_count; start++) {
        int end = start;
        while (end < path.len && path.ptr[end] != '.') end++;
        const token *part = &f->toks[t];
        if (part->kind != T_IDENT || !str_eq(part->text, (str){path.ptr + start, end - start})) return;
        add_occ((occurrence){.at = part->at, .len = part->text.len, .kind = OCC_NAMESPACE, .declaration = declaration,
                             .name = {path.ptr, end}});
        if (end < path.len && (t + 1 >= f->tok_count || f->toks[t + 1].kind != T_DOT)) return;
        t += 2; // And the dot
        start = end;
    }
}

// The namespace in front of a qualified name written at `qual_at`: Combat in
// Combat.Health. Nothing for a plain name.
static void qualifier_ref(const loc qual_at, const loc name_at, const str text)
{
    const str name = last_part(text);
    if (name.len == text.len || qual_at.line == 0 || loc_cmp(qual_at, name_at) == 0) return;
    namespace_parts(qual_at, (str){text.ptr, text.len - name.len - 1}, false);
}

// The namespace a part of Game.Combat.Health names in code: Game.Combat for Combat.
static str namespace_path(const expr *e)
{
    if (e->kind != E_MEMBER) return e->name;
    const str left = namespace_path(e->object);
    char *text = arena_alloc((size_t)left.len + (size_t)e->member.len + 2);
    memcpy(text, left.ptr, (size_t)left.len);
    text[left.len] = '.';
    memcpy(text + left.len + 1, e->member.ptr, (size_t)e->member.len);
    return (str){text, left.len + 1 + e->member.len};
}

// A type written by name, maybe qualified: `qual_at` is where it starts, `at`
// its last part.
static void type_ref(const loc qual_at, const loc at, const str text, const type t)
{
    if (text.len == 0) return;
    if (text.ptr[text.len - 1] == '?') { // int?: the type it holds
        const type inner = t.kind == TY_OPTIONAL ? t.decl->fields.items[0].type : (type){TY_ERROR, NULL};
        type_ref(qual_at, at, (str){text.ptr, text.len - 1}, inner);
        return;
    }
    // List<T>, Grid2<T> and Grid3<T>: the generic type, then the one it holds
    const int generic = str_starts_with_c(text, "List<") ? 4
                      : str_starts_with_c(text, "Grid2<") || str_starts_with_c(text, "Grid3<") ? 5
                                                                                               : 0;
    if (generic && text.ptr[text.len - 1] == '>') {
        add_occ((occurrence){.at = qual_at, .len = generic, .kind = OCC_TYPE, .name = (str){text.ptr, generic}, .type = t});
        // The held type, as the formatter writes it: right after the '<'
        const str inner = {text.ptr + generic + 1, text.len - generic - 2};
        const loc inner_at = {qual_at.line, qual_at.col + generic + 1, qual_at.file};
        const type element = t.kind == TY_LIST || t.kind == TY_GRID ? t.decl->fields.items[0].type : (type){TY_ERROR, NULL};
        const str last = last_part(inner);
        const loc last_at = {qual_at.line, inner_at.col + (int)(last.ptr - inner.ptr), qual_at.file};
        type_ref(inner_at, last_at, inner, element);
        return;
    }
    qualifier_ref(qual_at, at, text);
    const str name = last_part(text);
    occurrence o = {.at = at, .len = name.len, .kind = OCC_TYPE, .name = name, .type = t};
    if (t.kind == TY_COMPONENT || t.kind == TY_SINGLETON || t.kind == TY_INPUT || t.kind == TY_RECORD
        || t.kind == TY_STRUCT || t.kind == TY_EVENT || t.kind == TY_ENUM) {
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

// Whose fields plain names refer to in the code being walked: the input in its
// Sample and Sanitize, a type in its methods.
static const decl *walk_fields_of;

// The system, view, handler, method or function being walked: whose `this` it is.
static const decl *walk_code;

// What the assignment being walked changes: a name or a member.
static const expr *walk_written;

// The name or member an assignment to `target` changes: cells for cells[x].
static const expr *written_place(const expr *target)
{
    while (target && target->kind == E_INDEX) target = target->object;
    return target;
}

// A game's method, function or operator, which calls and declarations point at.
static bool is_routine(const decl *d)
{
    return d && (d->kind == DECL_METHOD || d->kind == DECL_FUNCTION);
}

static bool is_operator_decl(const decl *d)
{
    return is_routine(d) && d->is_operator;
}

// How many characters an operator token has: 1 for +, 2 for == and +=, 3 for <<=.
static int operator_len(const tok_kind kind)
{
    switch (kind) {
    case T_EQ: case T_NE: case T_LE: case T_GE: case T_SHL: case T_SHR: case T_AND: case T_OR: case T_PLUS_ASSIGN:
    case T_MINUS_ASSIGN: case T_STAR_ASSIGN: case T_SLASH_ASSIGN: case T_PERCENT_ASSIGN: case T_AMP_ASSIGN:
    case T_PIPE_ASSIGN: case T_CARET_ASSIGN:
        return 2;
    case T_SHL_ASSIGN: case T_SHR_ASSIGN:
        return 3;
    default:
        return 1;
    }
}

static void walk_stmt(const stmt *s);

static void walk_expr(const expr *e)
{
    if (!e) return;
    switch (e->kind) {
    case E_INT:
    case E_FLOAT:
    case E_BOOL:
    case E_STRING:
        break;

    case E_THIS:
        if (walk_code) {
            add_occ((occurrence){.at = e->at, .len = 4, .kind = OCC_THIS, .decl = walk_code, .name = str_from("this"),
                                 .type = e->type});
        }
        break;

    case E_DEFAULT:
        if (e->type.kind != TY_ERROR) {
            add_occ((occurrence){.at = e->at, .len = 7, .kind = OCC_DEFAULT, .name = str_from("default"), .type = e->type});
        }
        break;

    case E_NULL:
        if (e->type.kind == TY_OPTIONAL) {
            add_occ((occurrence){.at = e->at, .len = 4, .kind = OCC_DEFAULT, .name = str_from("null"), .type = e->type});
        }
        break;

    case E_COALESCE:
    case E_TRY:
    case E_DEFAULTED:
    case E_AWAIT:
        walk_expr(e->lhs);
        walk_expr(e->rhs);
        break;

    case E_IS: // x is int score, x is ParseError.Empty
        walk_expr(e->lhs);
        if (e->looks_for == IS_MEMBER && e->enum_member) {
            const str type_part = {e->pattern.ptr, e->pattern.len - e->enum_member->name.len - 1};
            const str type_name_ = last_part(type_part);
            const loc type_at = {e->pattern_at.line, e->pattern_at.col - 1 - type_name_.len, e->pattern_at.file};
            type_ref(e->pattern_qual_at, type_at, type_part, (type){TY_ENUM, e->type_decl});
            add_occ((occurrence){.at = e->pattern_at, .len = e->enum_member->name.len, .kind = OCC_ENUM_MEMBER,
                                 .decl = e->type_decl, .name = e->enum_member->name});
        } else {
            type_ref(e->pattern_qual_at, e->pattern_at, e->pattern, e->binding ? e->binding->type : (type){TY_ERROR, NULL});
        }
        if (e->binding) {
            const stmt *b = e->binding;
            add_occ((occurrence){.at = b->name_at, .len = b->name.len, .kind = OCC_LOCAL, .declaration = true, .local = b,
                                 .name = b->name, .type = b->type});
        }
        break;

    case E_NAME: {
        occurrence o = {.at = e->at, .len = e->name.len, .name = e->name, .type = e->type, .write = e == walk_written};
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
            o.decl = walk_fields_of;
            break;
        case BIND_NAMESPACE:
            o.kind = OCC_NAMESPACE;
            break;
        case BIND_DEVICES: // This machine's devices, like Screen
            o.kind = OCC_OWNER;
            o.owner = e->name;
            break;
        case BIND_CONST:
            o.kind = OCC_CONST;
            o.decl = e->constant;
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
                        .object_type = e->object->type, .write = e == walk_written};
        if (e->bind == BIND_TYPE && e->type_decl) { // Combat.Health, in Spawn(Combat.Health)
            o.kind = OCC_TYPE;
            o.decl = e->type_decl;
        } else if (e->enum_member) { // Title in Page.Title
            o.kind = OCC_ENUM_MEMBER;
            o.decl = e->type_decl;
        } else if (e->bind == BIND_NAMESPACE) { // Combat in Game.Combat.Health
            o.kind = OCC_NAMESPACE;
            o.name = namespace_path(e);
        } else if (e->bind == BIND_CONST) { // MAX_HEALTH in Combat.MAX_HEALTH
            o.kind = OCC_CONST;
            o.decl = e->constant;
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
        occurrence o = {.at = e->at, .len = e->name.len, .name = e->name, .type = e->type, .call = e->call};
        // cells[x, y] is cells[int2(x, y)] to the checker: that int2 isn't in the source.
        const int name = token_at(e->at);
        const bool written = name >= 0 && str_eq(A.files[e->at.file].toks[name].text, e->name);
        if (e->call != CALL_NONE && written) vec_push(A.calls, e);
        if ((e->call == CALL_CONSTRUCT || e->call == CALL_NEW_GRID) && written) { // float3(...), Grid2(64, 64)
            o.kind = OCC_TYPE;
            add_occ(o);
        } else if (e->call == CALL_SPAWN || e->call == CALL_SEND) {
            o.kind = OCC_FUNCTION;
            add_occ(o);
        } else if (e->call == CALL_METHOD || e->call == CALL_FUNCTION) { // IsDead() in a method, or Heal(...)
            o.kind = e->call == CALL_METHOD ? OCC_METHOD : OCC_FUNCTION;
            o.decl = e->method;
            add_occ(o);
        } else if (e->call == CALL_ACTION) { // content(): the Action parameter
            o.kind = OCC_PARAM;
            o.param = e->param;
            add_occ(o);
        }
        for (int i = 0; i < e->args.count; i++) walk_expr(e->args.items[i]);
        walk_stmt(e->block);
        break;
    }

    case E_METHOD: {
        walk_expr(e->object);
        occurrence o = {.at = e->at, .len = e->name.len, .name = e->name, .type = e->type, .object_type = e->object->type,
                        .call = e->call};
        if (e->call != CALL_NONE) vec_push(A.calls, e);
        if (e->call == CALL_BUILTIN || e->call == CALL_DRAW || e->call == CALL_GUI || e->call == CALL_TEXT) {
            o.kind = OCC_FUNCTION;
            o.owner = e->call == CALL_TEXT ? str_from("string") : e->object->name; // name.Contains(...): text's
            o.c_name = e->c_callee;
            add_occ(o);
        } else if (e->call == CALL_ADD || e->call == CALL_REMOVE || e->call == CALL_DESTROY || e->call == CALL_SEND
                   || e->call == CALL_LIST || e->call == CALL_GRID || e->call == CALL_SNAP) {
            o.kind = OCC_METHOD; // e.Destroy(), items.Add(x), cells.Clear(), this.Snap()
            add_occ(o);
        } else if (e->call == CALL_LOAD || e->call == CALL_UNLOAD || e->call == CALL_SCENE_PLAYER) {
            add_occ((occurrence){.at = e->object->at, .len = 5, .kind = OCC_OWNER, .owner = str_from("Scene"),
                                 .name = str_from("Scene")});
            o.kind = OCC_FUNCTION;
            o.owner = str_from("Scene");
            add_occ(o);
        } else if (e->call == CALL_SESSION) {
            add_occ((occurrence){.at = e->object->at, .len = 7, .kind = OCC_OWNER, .owner = str_from("Session"),
                                 .name = str_from("Session")});
            o.kind = OCC_FUNCTION;
            o.owner = str_from("Session");
            add_occ(o);
        } else if (e->call == CALL_CLIPBOARD) {
            add_occ((occurrence){.at = e->object->at, .len = 9, .kind = OCC_OWNER, .owner = str_from("Clipboard"),
                                 .name = str_from("Clipboard")});
            o.kind = OCC_FUNCTION;
            o.owner = str_from("Clipboard");
            add_occ(o);
        } else if (e->call == CALL_WAIT) {
            add_occ((occurrence){.at = e->object->at, .len = 4, .kind = OCC_OWNER, .owner = str_from("Wait"),
                                 .name = str_from("Wait")});
            o.kind = OCC_FUNCTION;
            o.owner = str_from("Wait");
            add_occ(o);
        } else if (e->call == CALL_METHOD || e->call == CALL_FUNCTION) { // stats.IsDead(), Combat.Heal(...)
            o.kind = e->call == CALL_METHOD ? OCC_METHOD : OCC_FUNCTION;
            o.decl = e->method;
            add_occ(o);
        }
        for (int i = 0; i < e->args.count; i++) walk_expr(e->args.items[i]);
        walk_stmt(e->block);
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
    case E_UNARY:
        if (e->method) { // A struct's operator
            add_occ((occurrence){.at = e->at, .len = operator_len(e->op), .kind = OCC_METHOD, .decl = e->method,
                                 .name = e->method->name, .type = e->type});
        }
        walk_expr(e->lhs);
        walk_expr(e->rhs);
        break;

    case E_CONDITIONAL:
        walk_expr(e->cond);
        walk_expr(e->lhs);
        walk_expr(e->rhs);
        break;

    case E_INTERP:
    case E_LIST:
        for (int i = 0; i < e->args.count; i++) walk_expr(e->args.items[i]);
        break;

    case E_INDEX:
        walk_expr(e->object);
        walk_expr(e->lhs);
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
    case S_FAIL:
        walk_expr(s->value);
        break;
    case S_VAR:
        type_ref(s->type_qual_at.line ? s->type_qual_at : s->type_at, s->type_at, s->type_name, s->type);
        add_occ((occurrence){.at = s->name_at, .len = s->name.len, .kind = OCC_LOCAL, .declaration = true, .local = s,
                             .name = s->name, .type = s->type});
        walk_expr(s->value);
        break;
    case S_ASSIGN:
        if (s->operator_decl) { // `+=` with a struct's +
            add_occ((occurrence){.at = s->at, .len = operator_len(s->op), .kind = OCC_METHOD, .decl = s->operator_decl,
                                 .name = s->operator_decl->name});
        }
        walk_written = written_place(s->target);
        walk_expr(s->target);
        walk_written = NULL;
        walk_expr(s->value);
        break;
    case S_EXPR:
        walk_expr(s->value);
        break;
    case S_SWITCH:
        walk_expr(s->cond);
        for (int i = 0; i < s->cases.count; i++) {
            const switch_case *section = &s->cases.items[i];
            for (int k = 0; k < section->labels.count; k++) walk_expr(section->labels.items[k]);
            for (int k = 0; k < section->body.count; k++) walk_stmt(section->body.items[k]);
        }
        break;
    case S_BREAK:
    case S_CONTINUE:
        break;
    case S_WHILE:
        walk_expr(s->cond);
        walk_stmt(s->then_stmt);
        break;
    case S_FOREACH:
    case S_PARALLEL:
        type_ref(s->type_qual_at.line ? s->type_qual_at : s->type_at, s->type_at, s->type_name, s->type);
        add_occ((occurrence){.at = s->name_at, .len = s->name.len, .kind = OCC_LOCAL, .declaration = true, .local = s,
                             .name = s->name, .type = s->type});
        walk_expr(s->value);
        walk_expr(s->by);
        walk_expr(s->offset);
        walk_stmt(s->then_stmt);
        break;
    case S_FOR:
        walk_stmt(s->init);
        walk_expr(s->cond);
        walk_stmt(s->step);
        walk_stmt(s->then_stmt);
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

// A method or function: its name, signature and body.
static void walk_routine(const decl *m)
{
    // An operator's `at` is its `operator` keyword.
    add_occ((occurrence){.at = m->at, .len = m->is_operator ? 8 : m->name.len, .kind = m->owner ? OCC_METHOD : OCC_FUNCTION,
                         .declaration = true, .decl = m, .name = m->name});
    if (!str_eq_c(m->return_type_name, "void")) {
        type_ref(m->return_type_qual_at, m->return_type_at, m->return_type_name, m->return_type);
    }
    type_ref(m->fails_type_qual_at, m->fails_type_at, m->fails_name, m->fails);
    walk_params(m);
    walk_code = m;
    walk_stmt(m->body);
    walk_code = NULL;
}

// In source order, and at one position in the order they were found: qsort
// leaves ties in any order.
static int occ_order(const void *a, const void *b)
{
    const occurrence *x = a;
    const occurrence *y = b;
    const int c = loc_cmp(x->at, y->at);
    return c != 0 ? c : x->order - y->order;
}

static void index_program(void)
{
    // `namespace X;` and `using Y;` at the top of each file
    for (int i = 0; i < A.prog->units.count; i++) {
        const unit *u = A.prog->units.items[i];
        if (u->ns.len > 0) namespace_parts(u->ns_at, u->ns, true);
        for (int k = 0; k < u->usings.count; k++) namespace_parts(u->using_at.items[k], u->usings.items[k], false);
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
            walk_code = d;
            walk_stmt(d->body);
            walk_code = NULL;
            continue;
        }
        if (d->kind == DECL_FUNCTION) {
            walk_routine(d);
            continue;
        }
        if (d->kind == DECL_CONST) {
            add_occ((occurrence){.at = d->at, .len = d->name.len, .kind = OCC_CONST, .declaration = true, .decl = d,
                                 .name = d->name, .type = d->return_type});
            type_ref(d->return_type_qual_at, d->return_type_at, d->return_type_name, d->return_type);
            walk_expr(d->value);
            continue;
        }
        add_occ((occurrence){.at = d->at, .len = d->name.len, .kind = OCC_TYPE, .declaration = true, .decl = d,
                             .name = d->name});
        if (d->kind == DECL_ENUM && d->backing.len > 0) { // enum Voxel : byte
            add_occ((occurrence){.at = d->backing_at, .len = d->backing.len, .kind = OCC_TYPE, .name = d->backing,
                                 .type = {TY_INT, NULL}, .backing = true});
        }
        for (int m = 0; m < d->members.count; m++) {
            const enum_member *member = &d->members.items[m];
            add_occ((occurrence){.at = member->at, .len = member->name.len, .kind = OCC_ENUM_MEMBER, .declaration = true,
                                 .decl = d, .name = member->name});
            walk_expr(member->value);
        }
        for (int f = 0; f < d->fields.count; f++) {
            const field *fl = &d->fields.items[f];
            type_ref(fl->type_qual_at.line ? fl->type_qual_at : fl->type_at, fl->type_at, fl->type_name, fl->type);
            add_occ((occurrence){.at = fl->at, .len = fl->name.len, .kind = OCC_FIELD, .declaration = true, .field = fl,
                                 .decl = d, .name = fl->name, .type = fl->type});
            walk_expr(fl->default_value);
            for (int a = 0; a < fl->attributes.count; a++) {
                const attribute *at = &fl->attributes.items[a];
                add_occ((occurrence){.at = at->at, .len = at->name.len, .kind = OCC_ATTRIBUTE, .name = at->name});
                for (int v = 0; v < at->values.count; v++) walk_expr(at->values.items[v]);
            }
        }
        walk_fields_of = d;
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
        for (int k = 0; k < d->methods.count; k++) walk_routine(d->methods.items[k]);
        walk_fields_of = NULL;
    }

    for (int b = 0; b < A.prog->settings.count; b++) {
        const decl *block = A.prog->settings.items[b];
        for (int f = 0; f < block->fields.count; f++) {
            const field *fl = &block->fields.items[f];
            if (setting_named(fl->name)) {
                add_occ((occurrence){.at = fl->at, .len = fl->name.len, .kind = OCC_SETTING, .name = fl->name,
                                     .type = fl->type});
            }
            walk_expr(fl->default_value);
        }
    }

    // In source order, one per position: the first found there.
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
    diag_set_suggestion_sink(collect_suggestion, NULL);

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
    diag_set_suggestion_sink(NULL, NULL);

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

const char *analysis_file_path(const int file)
{
    return A.files[file].src.path;
}

// ---------------------------------------------------------------------------
// Diagnostics

// A range over the token at `at`, or a character where there's none.
static void write_token_range(jbuf *out, const loc at)
{
    const int t = token_at(at);
    write_range(out, at, t >= 0 ? token_len(&A.files[at.file].toks[t]) : 1);
}

// Whether a warning is about access a system declares and doesn't use: a
// parameter never used, or mut and never written. Editors fade it out.
static bool unused_access(const diagnostic *d)
{
    for (int i = 0; d->severity == DIAG_WARNING && i < A.prog->decls.count; i++) {
        const decl *sys = A.prog->decls.items[i];
        for (int k = 0; sys->kind == DECL_SYSTEM && k < sys->params.count; k++) {
            const param *p = &sys->params.items[k];
            if (p->name.len == 0 || (p->type.kind != TY_COMPONENT && p->type.kind != TY_SINGLETON)) continue;
            if (!p->read && loc_cmp(d->at, p->name_at) == 0) return true;
            if (p->read && p->mode == PARAM_MUT && !p->written && loc_cmp(d->at, p->at) == 0) return true;
        }
    }
    return false;
}

static void write_diagnostic(jbuf *out, const diagnostic *d)
{
    const loc at = d->at.line > 0 ? d->at : (loc){1, 1, d->at.file};
    jb_put(out, "{\"range\":");
    write_token_range(out, at);
    jb_printf(out, ",\"severity\":%d,\"source\":\"tidec\",\"message\":", d->severity == DIAG_ERROR ? 1 : 2);
    jb_string(out, d->message.data ? d->message.data : "");
    if (unused_access(d)) jb_put(out, ",\"tags\":[1]"); // Unnecessary
    if (d->related.count > 0) {
        jb_put(out, ",\"relatedInformation\":[");
        for (int i = 0; i < d->related.count; i++) {
            const related_note *note = &d->related.items[i];
            jb_put(out, i ? ",{\"location\":{\"uri\":" : "{\"location\":{\"uri\":");
            jb_string(out, A.files[note->at.file].uri);
            jb_put(out, ",\"range\":");
            write_token_range(out, note->at);
            jb_put(out, "},\"message\":");
            jb_string(out, note->message);
            jb_put(out, "}");
        }
        jb_put(out, "]");
    }
    jb_put(out, "}");
}

void analysis_diagnostics(const int file, jbuf *out)
{
    jb_put(out, "[");
    int written = 0;
    for (int i = 0; i < A.diags.count; i++) {
        if (A.diags.items[i].at.file != file) continue;
        if (written++) jb_put(out, ",");
        write_diagnostic(out, &A.diags.items[i]);
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
    case TY_LOCAL_ENTITY: return "A handle to an entity of the local world: this machine's own, which only local code holds.";
    case TY_PLAYER: return "A player, independent of connections. `PlayerID(0)` names a player by index.";
    case TY_COLOR: return "A color: r, g, b and a from 0 to 1. `Color(r, g, b)` or `Color(r, g, b, a)`.";
    case TY_RECT: return "A rectangle on the screen, for the GUI: x and y from the top left, y down, then width and height.";
    case TY_STRING: return "Text, written in double quotes.";
    case TY_ACTION: return "Code the caller writes in braces after the call, run with `content();`.";
    case TY_LIST: return "A list of values, which grows and shrinks: `Count`, `items[i]`, `Add`, `RemoveAt`, `foreach`.";
    case TY_GRID: return "Cells at positions, kept in chunks only where something's set: `cells[x, y]`, `size`, `Clear()`, "
                         "and `foreach` or `parallel` through its cells. "
                         "Its size is given when it's made, `Grid2(1024, 1024)`; 0 or nothing is open.";
    default: return NULL;
    }
}

static const char *decl_keyword(const decl *d)
{
    switch (d->kind) {
    case DECL_COMPONENT: return d->is_scene ? "scene" : "component";
    case DECL_SINGLETON: return "singleton";
    case DECL_INPUT: return "input";
    case DECL_RECORD: return "record";
    case DECL_STRUCT: return "struct";
    case DECL_METHOD: return "method";
    case DECL_FUNCTION: return "function";
    case DECL_EVENT: return "event";
    case DECL_ENUM: return "enum";
    case DECL_LIST: return "list";
    case DECL_GRID: return "grid";
    case DECL_RESULT: return "result";
    case DECL_CONST: return "const";
    case DECL_SETTINGS: return "settings";
    case DECL_SYSTEM: return d->is_view ? "view" : d->is_handler ? "event" : "system";
    }
    return "";
}

// What a declaration is, in words: "event handler" rather than the keyword,
// and "local component" for what's local.
static const char *decl_what(const decl *d)
{
    const char *what = d->kind == DECL_SYSTEM && d->is_handler ? "event handler" : decl_keyword(d);
    if (!d->is_local) return what;
    sb b = {0};
    sb_printf(&b, "local %s", what);
    return b.data;
}

static void format_param(const param *p, sb *out)
{
    switch (p->mode) {
    case PARAM_MUT: sb_put(out, "mut "); break;
    case PARAM_IN: sb_put(out, "in "); break;
    case PARAM_WITH: sb_put(out, "with "); break;
    case PARAM_WITHOUT: sb_put(out, "without "); break;
    case PARAM_READ: case PARAM_EVENT: break;
    }
    sb_printf(out, STR_FMT, STR_ARG(p->type_name));
    if (p->name.len > 0) sb_printf(out, " " STR_FMT, STR_ARG(p->name));
}

// `system Move(mut Body body)`, or a handler's `event(Hit hit) TakeHit(mut Health health)`.
static void format_header(const decl *d, sb *out)
{
    int first = 0;
    if (d->is_local) sb_put(out, "local ");
    if (d->is_async) sb_put(out, "async ");
    if (d->is_handler && d->params.count > 0) {
        sb_put(out, "event(");
        format_param(&d->params.items[0], out);
        sb_printf(out, ") " STR_FMT "(", STR_ARG(d->name));
        first = 1;
    } else {
        sb_printf(out, "%s " STR_FMT "(", decl_keyword(d), STR_ARG(d->name));
    }
    for (int i = first; i < d->params.count; i++) {
        if (i > first) sb_put(out, ", ");
        format_param(&d->params.items[i], out);
    }
    sb_put(out, ")");
}

// What the engine's own events are for.
static const char *builtin_event_doc(const program *prog, const decl *d)
{
    if (d == prog->spawned) return "Sent to each entity as it's spawned: its handlers set new entities up.";
    if (d == prog->destroyed) return "Sent to each entity as it's destroyed, while its components can still be read.";
    if (d == prog->player_joined) return "Sent to the world when a player joins.";
    if (d == prog->player_left) return "Sent to the world when a player leaves.";
    return NULL;
}

// `mut void Damage(float amount)`: a method's or function's signature.
static void format_routine(const decl *m, sb *out)
{
    sb_printf(out, "%s%s%s" STR_FMT " " STR_FMT "(", m->is_extern ? "extern " : "", m->is_async ? "async " : "",
              m->is_mut_method ? "mut " : "", STR_ARG(m->return_type_name), STR_ARG(m->name));
    for (int i = 0; i < m->params.count; i++) {
        if (i) sb_put(out, ", ");
        format_param(&m->params.items[i], out);
    }
    sb_put(out, ")");
    if (m->fails_name.len > 0) sb_printf(out, " fails " STR_FMT, STR_ARG(m->fails_name));
}

// The source text of a value after a name, from the tokens between '=' and
// ';': a field's default, or a constant's value.
static void format_value_after(const loc name_at, sb *out)
{
    const int name = token_at(name_at);
    if (name < 0) return;
    const token *toks = A.files[name_at.file].toks;
    if (toks[name + 1].kind != T_ASSIGN) return;
    const int first = name + 2;
    int last = first;
    // To the ';', or the '}' that closes the type the field is in: not one of a
    // value's own, like Stats { armor = 2 }.
    for (int depth = 0; toks[last].kind != T_EOF; last++) {
        if (depth == 0 && (toks[last].kind == T_SEMI || toks[last].kind == T_RBRACE)) break;
        if (toks[last].kind == T_LBRACE) depth++;
        else if (toks[last].kind == T_RBRACE) depth--;
    }
    if (last == first) return;
    const char *start = toks[first].text.ptr - (toks[first].kind == T_STRING ? 1 : 0);
    const token *end_tok = &toks[last - 1];
    const char *end = end_tok->text.ptr + token_len(end_tok) - (end_tok->kind == T_STRING ? 1 : 0);
    sb_put(out, " = ");
    sb_putn(out, start, (size_t)(end - start));
}

static void format_default(const field *f, sb *out)
{
    format_value_after(f->at, out);
}

static void format_data_decl(const decl *d, sb *out)
{
    sb_printf(out, "%s%s " STR_FMT, d->is_local ? "local " : "", decl_keyword(d), STR_ARG(d->name));
    if (d->kind == DECL_ENUM && d->backing.len > 0) sb_printf(out, " : " STR_FMT, STR_ARG(d->backing));
    sb_put(out, "\n{\n");
    for (int i = 0; i < d->members.count; i++) {
        sb_printf(out, "    " STR_FMT " = %lld,\n", STR_ARG(d->members.items[i].name), (long long)d->members.items[i].number);
    }
    for (int i = 0; i < d->fields.count; i++) {
        const field *f = &d->fields.items[i];
        if (f->hidden) continue;
        const char *type = f->type_name.len > 0 ? NULL : type_name(f->type);
        if (type) sb_printf(out, "    %s " STR_FMT, type, STR_ARG(f->name));
        else sb_printf(out, "    " STR_FMT " " STR_FMT, STR_ARG(f->type_name), STR_ARG(f->name));
        if (!d->builtin) format_default(f, out);
        sb_put(out, ";\n");
    }
    for (int i = 0; i < d->methods.count; i++) {
        sb_put(out, i == 0 && d->fields.count > 0 ? "\n    " : "    ");
        format_routine(d->methods.items[i], out);
        sb_put(out, ";\n");
    }
    sb_put(out, "}");
}

static void code_block(sb *out, const char *code)
{
    sb_printf(out, "```tide\n%s\n```", code);
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

// A built-in call the checker takes by name, for hovers, completion, signature
// help and inlay hints: its form and what it does.
typedef struct method_form {
    const char *name;
    const char *form; // In a list's and a grid's methods, T is the element or the cell
    const char *doc;
} method_form;

// "Start, Open and Close": the names of the calls in `forms`.
static void put_names(sb *out, const method_form *forms, const size_t count)
{
    for (size_t i = 0; i < count; i++) sb_printf(out, "%s%s", i == 0 ? "" : i + 1 == count ? " and " : ", ", forms[i].name);
}

// Scene's functions.
static const method_form scene_calls[] = {
    {"Load", "Scene.Load(scene, SceneVisibility visibility)",
     "Loads a scene and returns its entity. Its `Spawned` handlers set it up at the end of the tick; it's public unless it's `SceneVisibility.Private`."},
    {"Unload", "Scene.Unload(scene)", "Unloads a scene at the end of the tick, destroying every entity in it."},
    {"AddPlayer", "Scene.AddPlayer(scene, PlayerID player)", "Lets a player see a private scene."},
    {"RemovePlayer", "Scene.RemovePlayer(scene, PlayerID player)", "Stops a player seeing a private scene."},
};

// Session's calls, from views and local handlers.
static const method_form session_calls[] = {
    {"Start", "Session.Start(scene)",
     "Starts a match on this machine, in `scene`: it runs the server, and no one else joins until it's opened. It leaves "
     "the match it's in first."},
    {"Open", "Session.Open(int port)",
     "Lets others join the match this machine runs: in a room, whose code is `Session.room`, and on `port` too (7777 "
     "unless it says), except on the web."},
    {"Close", "Session.Close()", "No one else joins the match this machine runs from now on; the players in it stay."},
    {"Kick", "Session.Kick(PlayerID player, string message)",
     "Sends a player on another machine out of the match this machine runs. They get `Disconnected` with `Kicked` and "
     "`message`, and `PlayerLeft` follows. They can join again while the match is open."},
    {"KickAll", "Session.KickAll(string message)",
     "Sends every player on another machine out of the match this machine runs, with `message`, as `Kick` does."},
    {"Join", "Session.Join(string code)",
     "Joins the match in the room with `code`, like \"K7QF2M\": its host's `Session.room`. It leaves the match it's in first."},
    {"Connect", "Session.Connect(string address, int port)",
     "Joins the match at `address`, like \"192.168.1.5\" or \"localhost\", on `port` (7777 unless it says). It leaves the "
     "match it's in first."},
    {"Leave", "Session.Leave()",
     "Leaves the match: `Disconnected` follows, and views stop seeing it. With host migration, a match this machine "
     "runs goes on, with another machine as its server."},
    {"End", "Session.End()",
     "Ends the match this machine runs, for everyone: every player goes offline with `Ended`, and no other machine "
     "takes it over. On a client, it does nothing."},
};

#define GRID_SIZE_DOC                                                                                                  \
    "A grid of that size, in cells, whose cell type comes from where it goes: 0 or nothing leaves an axis open."
#define GRID_SIZE_FIELD_DOC "The grid's size, 0 on the axes where it's open."
#define LIST_COUNT_DOC "How many elements the list has."
#define TEXT_LENGTH_DOC "How many characters the text has."

#define CLIPBOARD_COPY_FORM "Clipboard.Copy(string text)"
#define CLIPBOARD_COPY_DOC                                                                                             \
    "Puts `text` on this machine's clipboard, after the frame: up to 255 bytes for now. A browser takes it only "     \
    "shortly after a click or a key, as when a button is pressed. Pasting needs no call: Ctrl+V types what's on the " \
    "clipboard into the text field that has the focus."

// What async code waits for, after `await`.
static const method_form wait_calls[] = {
    {"Ticks", "await Wait.Ticks(int ticks)",
     "Waits for the match's ticks: the task goes on `ticks` ticks later, in the task pass at the end of the tick. "
     "Match code only."},
    {"Frames", "await Wait.Frames(int frames)",
     "Waits for this machine's frames: the task goes on `frames` frames later, at the end of the frame. Local code only."},
    {"Seconds", "await Wait.Seconds(float seconds)",
     "Waits for `seconds`: in the match, the nearest whole number of ticks, the same on every machine; in local code, "
     "this machine's time, frame by frame."},
};

// A list's methods, as the checker takes them.
static const method_form list_calls[] = {
    {"Add", "Add(T item)", "Adds an element at the end."},
    {"Insert", "Insert(int index, T item)", "Adds an element at `index`, moving the ones after it along."},
    {"RemoveAt", "RemoveAt(int index)", "Removes the element at `index`, moving the ones after it back."},
    {"Remove", "Remove(T item) -> bool", "Removes the first element equal to `item`, if there is one, and says whether it did."},
    {"Clear", "Clear()", "Removes every element."},
    {"Contains", "Contains(T item) -> bool", "Whether an element is equal to `item`."},
    {"IndexOf", "IndexOf(T item) -> int", "Where the first element equal to `item` is, or -1."},
};

static const method_form grid_calls[] = {
    {"Clear", "Clear()", "Sets every cell back to zero, keeping the size."},
};

// An entity's methods, in match code for an Entity and in local code for a LocalEntity.
static const method_form entity_calls[] = {
    {"Add", "Add(components...)", "Adds components, or replaces their values. Applied at the end of the tick."},
    {"Remove", "Remove(components...)", "Removes components. Applied at the end of the tick."},
    {"Destroy", "Destroy()", "Destroys the entity at the end of the tick."},
    {"Send", "Send(event)",
     "Sends an event to the entity: handlers that take its components run for it, at the end of the tick."},
    {"Snap", "Snap()",
     "It jumped, like a respawn or a portal: views draw it as it is this tick, not blended from where it was."},
};

static const method_form singleton_calls[] = {
    {"Snap", "Snap()", "It jumped, like a camera cut: views draw it as it is this tick, not blended from where it was."},
};

// The built-in methods of what has the type `t`, or NULL: a list's, a grid's,
// an entity's or a singleton's. Text's are in builtins.c, with Math's.
static const method_form *methods_of(const type t, size_t *count)
{
    const method_form *forms = NULL;
    *count = 0;
    switch (t.kind) {
    case TY_LIST: forms = list_calls; *count = sizeof list_calls / sizeof list_calls[0]; break;
    case TY_GRID: forms = grid_calls; *count = sizeof grid_calls / sizeof grid_calls[0]; break;
    case TY_ENTITY: case TY_LOCAL_ENTITY: forms = entity_calls; *count = sizeof entity_calls / sizeof entity_calls[0]; break;
    case TY_SINGLETON: forms = singleton_calls; *count = sizeof singleton_calls / sizeof singleton_calls[0]; break;
    default: break;
    }
    return forms;
}

// "List<int>.Add(int item)" for the method `form` called on a value of type
// `t`; "items.Add(T item)" where its type isn't known.
static void format_method(const method_form *form, const type t, sb *out)
{
    const bool generic = (t.kind == TY_LIST || t.kind == TY_GRID) && t.decl;
    const char *element = generic ? type_name(t.decl->fields.items[0].type) : "T";
    sb_put(out, generic ? type_name(t)
                : t.kind == TY_LIST ? "items"
                : t.kind == TY_GRID ? "cells"
                : t.kind == TY_SINGLETON ? "singleton"
                : "entity");
    sb_put(out, ".");
    for (const char *p = form->form; *p; p++) {
        const bool alone = (p == form->form || !isalnum((unsigned char)p[-1])) && !isalnum((unsigned char)p[1]);
        if (*p == 'T' && alone) sb_put(out, element);
        else sb_putn(out, p, 1);
    }
}

// The form named `name` in `forms`, or NULL.
static const method_form *find_form(const method_form *forms, const size_t count, const str name)
{
    for (size_t i = 0; i < count; i++) {
        if (str_eq_c(name, forms[i].name)) return &forms[i];
    }
    return NULL;
}

// What `default` is for type `t`, in words.
static void describe_default(const type t, sb *out)
{
    switch (t.kind) {
    case TY_BOOL: sb_put(out, "`false`"); break;
    case TY_INT: case TY_FLOAT: sb_put(out, "`0`"); break;
    case TY_ENTITY: case TY_LOCAL_ENTITY: sb_put(out, "The null entity"); break;
    case TY_PLAYER: sb_put(out, "No player"); break;
    case TY_STRING: sb_put(out, "Empty text"); break;
    case TY_LIST: sb_put(out, "An empty list"); break;
    case TY_GRID: sb_put(out, "An empty grid, open every way"); break;
    case TY_ENUM: {
        const enum_member *zero = NULL;
        for (int i = 0; i < t.decl->members.count && !zero; i++) {
            if (t.decl->members.items[i].number == 0) zero = &t.decl->members.items[i];
        }
        if (zero) sb_printf(out, "`" STR_FMT "." STR_FMT "`, the member that's 0", STR_ARG(t.decl->name), STR_ARG(zero->name));
        else sb_put(out, "0, which isn't one of its members");
        break;
    }
    case TY_COMPONENT: case TY_SINGLETON: case TY_INPUT: case TY_STRUCT: case TY_EVENT:
        sb_printf(out, "`" STR_FMT " { }`: each field's default, and zero where it has none", STR_ARG(t.decl->name));
        break;
    case TY_OPTIONAL: sb_put(out, "Nothing"); break;
    default: sb_put(out, "All zeros"); break;
    }
    sb_printf(out, ": the default value of `%s`, the type where it goes.", type_name(t));
}

// Markdown for a hover over `o`.
static void describe(const occurrence *o, sb *out)
{
    sb code = {0};
    switch (o->kind) {
    case OCC_DEFAULT: // `default`, or `null`
        sb_printf(&code, STR_FMT ": %s", STR_ARG(o->name), type_name(o->type));
        code_block(out, code.data);
        sb_put(out, "\n\n");
        describe_default(o->type, out);
        break;
    case OCC_TYPE:
        if (o->backing) {
            code_block(out, str_to_cstr(o->name));
            sb_put(out, str_eq_c(o->name, "byte")     ? "\n\nWhat the enum is stored as: one byte, so its members go from 0 to 255."
                      : str_eq_c(o->name, "ushort") ? "\n\nWhat the enum is stored as: two bytes, so its members go from 0 to 65535."
                                                    : "\n\nWhat the enum is stored as: four bytes, as it is without one.");
        } else if (o->decl) {
            format_data_decl(o->decl, &code);
            code_block(out, code.data);
            if (o->decl->kind == DECL_EVENT && o->decl->builtin) sb_printf(out, "\n\n%s", builtin_event_doc(A.prog, o->decl));
            else if (o->decl->builtin) sb_put(out, "\n\nBuilt into the engine.");
            else if (o->decl->is_scene) {
                sb_put(out, o->decl->is_local ? "\n\nA local scene: loaded with `Scene.Load` from views, on this machine only."
                                              : "\n\nA scene: its entities load and unload together. Load it with `Scene.Load`; "
                                                "systems that take it run once per loaded one.");
            } else if (o->decl->kind == DECL_EVENT && o->decl->is_local) {
                sb_put(out, "\n\nLocal: sent with `Send` from views, and handled by `local event(...)` handlers at the end of the frame.");
            } else if (o->decl->kind == DECL_EVENT) {
                sb_put(out, "\n\nSent with `Send`, and handled by `event(...)` handlers at the end of the tick.");
            } else if (o->decl->is_local) {
                sb_put(out, "\n\nLocal: this machine's own, never sent, rolled back or hashed. Views change it; the match can't see it.");
            }
        } else {
            code_block(out, type_name(o->type));
            const char *doc = builtin_type_doc(o->type.kind);
            if (doc) sb_printf(out, "\n\n%s", doc);
        }
        break;
    case OCC_SYSTEM:
        format_header(o->decl, &code);
        code_block(out, code.data);
        if (o->decl->is_handler && o->decl->event) {
            const str event = o->decl->event->name;
            const char *when = o->decl->is_local ? "frame" : "tick";
            if (o->decl->per_entity) {
                sb_printf(out, "\n\nRuns when a `" STR_FMT "` is sent to an entity that matches its parameters, "
                               "at the end of the %s.", STR_ARG(event), when);
            } else {
                sb_printf(out, "\n\nRuns once for every `" STR_FMT "` sent, at the end of the %s.", STR_ARG(event), when);
            }
        } else {
            sb_put(out, o->decl->is_view ? "\n\nRuns once per frame. It reads the match and changes local state, never the match."
                                         : o->decl->per_entity ? "\n\nRuns once per tick for every matching entity."
                                         : "\n\nRuns once per tick.");
        }
        describe_order(o->decl, out);
        break;
    case OCC_SETTING: {
        const setting *st = setting_named(o->name);
        sb_printf(&code, "%s %s", type_name((type){st->kind, NULL}), st->name);
        code_block(out, code.data);
        sb_printf(out, "\n\nSetting: %s Without it: %s.", st->doc, st->otherwise);
        break;
    }
    case OCC_CONST:
        sb_printf(&code, "const " STR_FMT " " STR_FMT, STR_ARG(o->decl->return_type_name), STR_ARG(o->decl->name));
        format_value_after(o->decl->at, &code);
        code_block(out, code.data);
        sb_put(out, "\n\nConstant: the same on every machine. Any code can read it, and reading it makes no system wait.");
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
        else if (o->param->mode == PARAM_EVENT) sb_put(out, "\n\nThe event being handled, read-only.");
        else if (o->param->mode == PARAM_IN) sb_put(out, "\n\nParameter, read-only: C gets a pointer to the value.");
        else if (o->param->mode == PARAM_MUT) sb_put(out, "\n\nParameter, writable.");
        else sb_put(out, "\n\nParameter, read-only.");
        break;
    case OCC_LOCAL:
        sb_printf(&code, "%s%s " STR_FMT, o->local->is_mut ? "mut " : "", type_name(o->local->type),
                  STR_ARG(o->local->name));
        code_block(out, code.data);
        sb_put(out, o->local->is_mut ? "\n\nLocal variable." : "\n\nLocal variable, read-only.");
        break;
    case OCC_THIS: {
        const decl *d = o->decl;
        if (o->type.kind == TY_ERROR) {
            code_block(out, "this");
            sb_put(out, "\n\nThe entity the code runs for.");
            break;
        }
        sb_printf(&code, "%s this", type_name(o->type));
        code_block(out, code.data);
        if (d->kind == DECL_METHOD) {
            sb_printf(out, "\n\nThe entity whose `" STR_FMT "` this is.", STR_ARG(d->owner->name));
        } else if (d->is_handler && d->event) {
            sb_printf(out, "\n\nThe entity the `" STR_FMT "` was sent to.", STR_ARG(d->event->name));
        } else {
            sb_printf(out, "\n\nThe entity `" STR_FMT "` runs for.", STR_ARG(d->name));
        }
        break;
    }
    case OCC_OWNER:
        code_block(out, str_to_cstr(o->name));
        // The calls of the owners whose calls the checker takes by name, from the tables above
        if (str_eq_c(o->name, "Scene")) {
            sb_put(out, "\n\nLoads and unloads scenes, groups of entities that come and go together: ");
            put_names(out, scene_calls, sizeof scene_calls / sizeof scene_calls[0]);
            sb_put(out, ".");
        } else if (str_eq_c(o->name, "Session")) {
            sb_put(out, "\n\nWhich match this machine is in, and who's in the one it runs: ");
            put_names(out, session_calls, sizeof session_calls / sizeof session_calls[0]);
            sb_put(out, ", from views and local handlers. Take `Session session` to read where it stands.");
        } else if (str_eq_c(o->name, "Wait")) {
            sb_put(out, "\n\nWhat async code waits for, after `await`: ");
            put_names(out, wait_calls, sizeof wait_calls / sizeof wait_calls[0]);
            sb_put(out, ", the match's ticks, this machine's frames, or seconds.");
        } else {
            sb_put(out, str_eq_c(o->name, "Draw")        ? "\n\nImmediate-mode drawing, in views and the functions they call."
                      : str_eq_c(o->name, "GUI")       ? "\n\nThe GUI's widgets, each at a Rect. In views and the functions they call."
                      : str_eq_c(o->name, "GUILayout") ? "\n\nThe GUI's widgets, laid out one after another, and containers "
                                                         "that arrange them. In views and the functions they call."
                      : str_eq_c(o->name, "Screen")    ? "\n\nThe window's size, in pixels."
                      : str_eq_c(o->name, "Devices")   ? "\n\nThis machine's keyboard, mouse and gamepad. Views read them once "
                                                         "per frame, and the input's Sample once per tick. Systems take a "
                                                         "`Devices` parameter instead: the devices of the entity's owner."
                      : str_eq_c(o->name, "Clipboard") ? "\n\nThis machine's clipboard: Copy, from views and local handlers. "
                                                         "Ctrl+V pastes into text fields by itself."
                                                       : "\n\nMath functions and constants, deterministic on every platform.");
        }
        break;
    case OCC_FUNCTION:
    case OCC_CONSTANT:
        if (is_routine(o->decl)) {
            const decl *m = o->decl;
            if (m->is_extern && m->c_name && !str_eq_c(m->name, m->c_name)) sb_printf(&code, "[NativeName(\"%s\")]\n", m->c_name);
            format_routine(m, &code);
            code_block(out, code.data);
            if (m->is_extern) {
                sb_printf(out, "\n\nC function `%s`, which the game's C files or libraries define.",
                          o->decl->c_name ? o->decl->c_name : "?");
            } else if (m->is_async) {
                sb_put(out, "\n\nAsync function: `await` it for its value, or call it as a statement to start it as a "
                            "task, which goes on by itself.");
            } else {
                sb_put(out, "\n\nFunction: runs when it's called.");
            }
            break;
        }
        if (o->kind == OCC_FUNCTION && str_eq_c(o->owner, "Scene")) {
            for (size_t i = 0; i < sizeof scene_calls / sizeof scene_calls[0]; i++) {
                if (!str_eq_c(o->name, scene_calls[i].name)) continue;
                code_block(out, scene_calls[i].form);
                sb_printf(out, "\n\n%s", scene_calls[i].doc);
            }
        } else if (o->kind == OCC_FUNCTION && str_eq_c(o->owner, "Clipboard")) {
            code_block(out, CLIPBOARD_COPY_FORM);
            sb_put(out, "\n\n" CLIPBOARD_COPY_DOC);
        } else if (o->kind == OCC_FUNCTION && str_eq_c(o->owner, "Session")) {
            for (size_t i = 0; i < sizeof session_calls / sizeof session_calls[0]; i++) {
                if (!str_eq_c(o->name, session_calls[i].name)) continue;
                code_block(out, session_calls[i].form);
                sb_printf(out, "\n\n%s", session_calls[i].doc);
            }
        } else if (o->kind == OCC_FUNCTION && str_eq_c(o->owner, "Wait")) {
            for (size_t i = 0; i < sizeof wait_calls / sizeof wait_calls[0]; i++) {
                if (!str_eq_c(o->name, wait_calls[i].name)) continue;
                code_block(out, wait_calls[i].form);
                sb_printf(out, "\n\n%s", wait_calls[i].doc);
            }
        } else if (o->kind == OCC_FUNCTION && o->owner.len == 0 && str_eq_c(o->name, "Send")) {
            code_block(out, "Send(event)");
            sb_put(out, "\n\nSends an event to the whole world. Its handlers run at the end of the tick, in the order "
                        "everything was sent.");
        } else if (o->kind == OCC_FUNCTION && o->owner.len == 0) {
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
        if (is_routine(o->decl)) {
            format_routine(o->decl, &code);
            code_block(out, code.data);
            if (o->decl->is_operator) {
                sb_printf(out, "\n\nOperator of struct `" STR_FMT "`.", STR_ARG(o->decl->owner->name));
                break;
            }
            sb_printf(out, "\n\nMethod of %s `" STR_FMT "`. %s", decl_keyword(o->decl->owner), STR_ARG(o->decl->owner->name),
                      o->decl->is_mut_method ? "It changes the fields, so it's called on something writable."
                                             : "It only reads the fields.");
            break;
        }
        if (o->call == CALL_NONE && str_eq_c(o->name, "Sample")) {
            code_block(out, "Sample()");
            sb_put(out, "\n\nBuilds the player's input from this machine's `Devices`, once per tick on their machine. "
                        "Fields start at their defaults. It runs outside the simulation, so it only sees the devices and "
                        "the local singletons it takes.");
        } else if (o->call == CALL_NONE && str_eq_c(o->name, "Sanitize")) {
            code_block(out, "Sanitize()");
            sb_put(out, "\n\nRuns on every input before the simulation reads it, including input from other "
                        "players, so systems can rely on what it guarantees.");
        } else {
            // What it's called on decides: items.Clear() is a list's, e.Destroy() an entity's.
            size_t count;
            const method_form *forms = methods_of(o->object_type, &count);
            const method_form *form = find_form(forms, count, o->name);
            if (!form) break;
            format_method(form, o->object_type, &code);
            code_block(out, code.data);
            sb_printf(out, "\n\n%s", form->doc);
        }
        break;
    case OCC_MEMBER:
        sb_printf(&code, "%s " STR_FMT, type_name(o->type), STR_ARG(o->name));
        code_block(out, code.data);
        if (str_eq_c(o->name, "down")) sb_put(out, "\n\nTrue on the tick it became true.");
        if (str_eq_c(o->name, "up")) sb_put(out, "\n\nTrue on the tick it became false.");
        if (o->object_type.kind == TY_STRING && str_eq_c(o->name, "Length")) sb_put(out, "\n\n" TEXT_LENGTH_DOC);
        if (o->object_type.kind == TY_LIST && str_eq_c(o->name, "Count")) sb_put(out, "\n\n" LIST_COUNT_DOC);
        if (o->object_type.kind == TY_GRID && str_eq_c(o->name, "size")) sb_put(out, "\n\n" GRID_SIZE_FIELD_DOC);
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
            {"Snap", "[Snap]", "Views see this field as it is at the latest tick, not blended between the last two: "
                               "for angles that wrap and values that jump."},
            {"NativeName", "[NativeName(\"c_function\")]",
             "The C function the extern function after it calls, when its name isn't the function's own."},
        };
        for (size_t i = 0; i < sizeof attributes / sizeof attributes[0]; i++) {
            if (!str_eq_c(o->name, attributes[i].name)) continue;
            code_block(out, attributes[i].form);
            sb_printf(out, "\n\n%s", attributes[i].doc);
        }
        break;
    }
    case OCC_ENUM_MEMBER:
        for (int i = 0; o->decl && i < o->decl->members.count; i++) {
            const enum_member *m = &o->decl->members.items[i];
            if (!str_eq(m->name, o->name)) continue;
            sb_printf(&code, STR_FMT "." STR_FMT " = %lld", STR_ARG(o->decl->name), STR_ARG(m->name), (long long)m->number);
            code_block(out, code.data);
            sb_printf(out, "\n\nMember of enum `" STR_FMT "`.", STR_ARG(o->decl->name));
        }
        break;
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
    if (d->is_handler) {
        if (!d->event || d->event->handlers.count < 2) return;
        int index = 0;
        while (index < d->event->handlers.count && d->event->handlers.items[index] != d) index++;
        sb_printf(out, "\n\nRuns %s of %d handlers of `" STR_FMT "`", ordinal(index + 1), d->event->handlers.count,
                  STR_ARG(d->event->name));
        for (int i = 0; i < d->after.count; i++) {
            sb_printf(out, "%s`" STR_FMT "`", i ? ", " : ", after ", STR_ARG(d->after.items[i]->qualified));
        }
        sb_put(out, ".");
        return;
    }
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

// The text of line `line` (1-based) of the document, without its line break.
static str doc_line(const int line)
{
    if (line < 1 || line > DOC->lines.count) return (str){"", 0};
    const char *start = DOC->lines.items[line - 1];
    const char *end = start;
    const char *text_end = DOC->src.text + DOC->src.len;
    while (end < text_end && *end != '\n') end++;
    if (end > start && end[-1] == '\r') end--;
    return (str){start, (int)(end - start)};
}

static bool blank_line(const int line)
{
    const str text = doc_line(line);
    for (int i = 0; i < text.len; i++) {
        if (!isspace((unsigned char)text.ptr[i])) return false;
    }
    return true;
}

static bool comment_line(const int line)
{
    str text = doc_line(line);
    while (text.len > 0 && isspace((unsigned char)text.ptr[0])) {
        text.ptr++;
        text.len--;
    }
    return text.len >= 2 && text.ptr[0] == '/' && text.ptr[1] == '/';
}

static bool can_create_files = true;

void analysis_set_can_create_files(const bool can)
{
    can_create_files = can;
}

// Refactoring: moves a type or function into a file of its own, named after
// it, next to this one. A game is every .tide file in its folder, so the
// program stays the same. Systems and views stay put: moving one would change
// the order they run in, which follows the files.
static void move_to_file_action(const int line, jbuf *out, int *written)
{
    for (int i = 0; can_create_files && i < A.prog->decls.count; i++) {
        const decl *d = A.prog->decls.items[i];
        if (d->builtin || d->at.file != A.doc || d->kind == DECL_SYSTEM || d->kind == DECL_CONST) continue;
        // Its lines: the attributes and comments just above it, to its closing brace.
        int first = d->at.line;
        for (int a = 0; a < d->attributes.count; a++) {
            if (d->attributes.items[a].at.line < first) first = d->attributes.items[a].at.line;
        }
        while (first > 1 && comment_line(first - 1)) first--;
        const int last = d->end.line > 0 ? d->end.line : d->at.line;
        if (line + 1 < first || line + 1 > last) continue;

        // Beside this file: file:///D:/game/main.tide -> file:///D:/game/Health.tide
        const char *uri = DOC->uri;
        const char *slash = strrchr(uri, '/');
        if (!slash) return;
        sb target = {0};
        sb_printf(&target, "%.*s/" STR_FMT ".tide", (int)(slash - uri), uri, STR_ARG(d->name));
        if (strcmp(target.data, uri) == 0) return; // Already in its own file
        for (int f = 0; f < A.file_count; f++) {
            if (A.files[f].uri && strcmp(A.files[f].uri, target.data) == 0) return; // Taken
        }

        // The new file: this file's namespace and usings, then the declaration.
        sb text = {0};
        const unit *u = d->unit;
        if (u && u->ns.len > 0) sb_printf(&text, "namespace " STR_FMT ";\n", STR_ARG(u->ns));
        for (int k = 0; u && k < u->usings.count; k++) sb_printf(&text, "using " STR_FMT ";\n", STR_ARG(u->usings.items[k]));
        if (text.len > 0) sb_put(&text, "\n");
        for (int l = first; l <= last; l++) {
            const str line_text = doc_line(l);
            sb_putn(&text, line_text.ptr, (size_t)line_text.len);
            sb_put(&text, "\n");
        }
        // What's left here: no double blank line where it was.
        int remove_to = last + 1;
        if (blank_line(remove_to) && (first == 1 || blank_line(first - 1))) remove_to++;

        if ((*written)++) jb_put(out, ",");
        sb title = {0};
        sb_printf(&title, "Move '" STR_FMT "' to " STR_FMT ".tide", STR_ARG(d->name), STR_ARG(d->name));
        jb_put(out, "{\"title\":");
        jb_string(out, title.data);
        jb_put(out, ",\"kind\":\"refactor.move\",\"edit\":{\"documentChanges\":[{\"kind\":\"create\",\"uri\":");
        jb_string(out, target.data);
        jb_put(out, ",\"options\":{\"overwrite\":false,\"ignoreIfExists\":false}},{\"textDocument\":{\"uri\":");
        jb_string(out, target.data);
        jb_put(out, ",\"version\":null},\"edits\":[{\"range\":");
        write_edit_range(out, (loc){1, 1, A.doc}, (loc){1, 1, A.doc});
        jb_put(out, ",\"newText\":");
        jb_string(out, text.data);
        jb_put(out, "}]},{\"textDocument\":{\"uri\":");
        jb_string(out, uri);
        jb_put(out, ",\"version\":null},\"edits\":[{\"range\":");
        write_edit_range(out, (loc){first, 1, A.doc}, (loc){remove_to, 1, A.doc});
        jb_put(out, ",\"newText\":\"\"}]}]}}");
        return;
    }
}

// Where text added at the end of the document goes, and what comes before it
// so a declaration there stands apart from what's above.
static loc doc_end(const char **separator)
{
    const int last = DOC->lines.count > 0 ? DOC->lines.count : 1;
    const bool ends_with_newline = DOC->src.len > 0 && DOC->src.text[DOC->src.len - 1] == '\n';
    *separator = DOC->src.len == 0 ? "" : ends_with_newline ? "\n" : "\n\n";
    return (loc){last, doc_line(last).len + 1, A.doc};
}

// A parameter name for an argument of a function to create: the name of what's
// passed (unit.stats: stats), or its type's (Stats: stats, float: value).
static void argument_name(const expr *arg, sb *out)
{
    if (arg->kind == E_NAME) {
        sb_printf(out, STR_FMT, STR_ARG(arg->name));
    } else if (arg->kind == E_MEMBER && arg->field) {
        sb_printf(out, STR_FMT, STR_ARG(arg->member));
    } else if (arg->type.decl) {
        const str name = arg->type.decl->name;
        sb_printf(out, "%c%.*s", tolower((unsigned char)name.ptr[0]), name.len - 1, name.ptr + 1);
    } else {
        sb_put(out, "value");
    }
}

static loc token_end(const token *t)
{
    return (loc){t->at.line, t->at.col + token_len(t), t->at.file};
}

// An Entity or LocalEntity parameter: removes it, with the comma between it
// and its neighbor, and writes `this` where the code uses it.
static void use_this_action(const param *p, jbuf *out, int *written)
{
    const int first = token_at(p->at);
    const int name = token_at(p->name_at);
    if (first < 0 || name < 0) return;
    const token *toks = DOC->toks;
    loc start = p->at;
    loc end = token_end(&toks[name]);
    if (toks[name + 1].kind == T_COMMA) {
        end = toks[name + 2].at; // Up to the next parameter
    } else if (first > 1 && toks[first - 1].kind == T_COMMA) {
        start = token_end(&toks[first - 2]); // From the end of the one before
    }
    if ((*written)++) jb_put(out, ",");
    sb title = {0};
    sb_printf(&title, "Use 'this' instead of '" STR_FMT "'", STR_ARG(p->name));
    jb_put(out, "{\"title\":");
    jb_string(out, title.data);
    jb_put(out, ",\"kind\":\"quickfix\",\"isPreferred\":true,\"edit\":{\"changes\":{");
    jb_string(out, A.files[A.doc].uri);
    jb_put(out, ":[{\"range\":");
    write_edit_range(out, start, end);
    jb_put(out, ",\"newText\":\"\"}");
    for (int i = 0; i < A.occs.count; i++) {
        const occurrence *o = &A.occs.items[i];
        if (o->kind != OCC_PARAM || o->param != p || o->declaration || o->at.file != A.doc) continue;
        jb_put(out, ",{\"range\":");
        write_range(out, o->at, o->len);
        jb_put(out, ",\"newText\":\"this\"}");
    }
    jb_put(out, "]}}}");
}

// The diagnostic at `at`, or NULL.
static const diagnostic *diagnostic_at(const loc at)
{
    for (int i = 0; i < A.diags.count; i++) {
        if (loc_cmp(A.diags.items[i].at, at) == 0) return &A.diags.items[i];
    }
    return NULL;
}

// A quick fix that replaces `start` to `end` in the document with `text`, and
// fixes `d`, if it says which diagnostic.
static void quick_fix(jbuf *out, int *written, const char *title, const bool preferred, const diagnostic *d,
                      const loc start, const loc end, const char *text)
{
    if ((*written)++) jb_put(out, ",");
    jb_put(out, "{\"title\":");
    jb_string(out, title);
    jb_printf(out, ",\"kind\":\"quickfix\",\"isPreferred\":%s", preferred ? "true" : "false");
    if (d) {
        jb_put(out, ",\"diagnostics\":[");
        write_diagnostic(out, d);
        jb_put(out, "]");
    }
    jb_put(out, ",\"edit\":{\"changes\":{");
    jb_string(out, A.files[A.doc].uri);
    jb_put(out, ":[{\"range\":");
    write_edit_range(out, start, end);
    jb_put(out, ",\"newText\":");
    jb_string(out, text);
    jb_put(out, "}]}}}");
}

static loc expr_start(const expr *e);
static loc expr_end(const expr *e);
static const unit *doc_unit(void);
static bool visible_by_name(const decl *d);

// A failable call's or a T?'s value that nothing handles: carry on with its
// default (`!`), or in a function that fails with the same error, pass it on
// (`try`), as the checker's note says.
static void unwrap_actions(const fix *f, jbuf *out, int *written)
{
    const diagnostic *d = diagnostic_at(f->at);
    const loc end = expr_end(f->call);
    quick_fix(out, written, "Carry on with the default if there's none: add '!'", true, d, end, end, "!");
    if (!f->method) return;
    sb title = {0};
    sb_printf(&title, "Pass the error on to the caller of '" STR_FMT "': add 'try'", STR_ARG(f->method->name));
    const loc start = expr_start(f->call);
    quick_fix(out, written, title.data, false, d, start, start, "try ");
}

// "did you mean" in a diagnostic, as the checker suggested it: the name meant
// in the place of the one written.
static void suggestion_actions(const int start_line, const int end_line, jbuf *out, int *written)
{
    for (int i = 0; i < A.diags.count; i++) {
        const diagnostic *d = &A.diags.items[i];
        if (d->fix_len == 0 || d->fix_at.file != A.doc || d->at.line - 1 < start_line || d->at.line - 1 > end_line) continue;
        sb title = {0};
        sb_printf(&title, "Change to '" STR_FMT "'", STR_ARG(d->fix));
        const loc end = {d->fix_at.line, d->fix_at.col + d->fix_len, d->fix_at.file};
        quick_fix(out, written, title.data, true, d, d->fix_at, end, str_to_cstr(d->fix));
    }
}

// An unknown name that another namespace declares: write it with its
// namespace, or add a `using` for it.
static void namespace_actions(const int start_line, const int end_line, jbuf *out, int *written)
{
    const unit *u = doc_unit();
    for (int i = 0; i < A.diags.count; i++) {
        const diagnostic *d = &A.diags.items[i];
        if (d->severity != DIAG_ERROR || d->at.file != A.doc || d->at.line - 1 < start_line || d->at.line - 1 > end_line) continue;
        const int t = token_at(d->at);
        if (t < 0 || DOC->toks[t].kind != T_IDENT || (t > 0 && DOC->toks[t - 1].kind == T_DOT)) continue;
        bool known = false; // The name means something there already: the error's about something else
        for (int k = 0; k < A.occs.count && !known; k++) known = loc_cmp(A.occs.items[k].at, d->at) == 0;
        const str name = DOC->toks[t].text;
        VEC(str) offered = {0};
        for (int k = 0; !known && k < A.prog->decls.count; k++) {
            const decl *other = A.prog->decls.items[k];
            if (other->builtin || !other->unit || other->unit->ns.len == 0 || !str_eq(other->name, name)) continue;
            if (visible_by_name(other)) continue;
            bool repeated = false;
            for (int j = 0; j < offered.count; j++) repeated |= str_eq(offered.items[j], other->unit->ns);
            if (repeated) continue;
            vec_push(offered, other->unit->ns);
        }
        for (int k = 0; k < offered.count; k++) {
            const str ns = offered.items[k];
            sb title = {0};
            sb text = {0};
            sb_printf(&text, STR_FMT "." STR_FMT, STR_ARG(ns), STR_ARG(name));
            sb_printf(&title, "Write '%s'", text.data);
            const loc end = {d->at.line, d->at.col + name.len, d->at.file};
            quick_fix(out, written, title.data, false, d, d->at, end, text.data);
            // After the file's usings, or its namespace, or at its top
            int line = 1;
            const char *after = "\n";
            for (int j = 0; u && j < u->using_at.count; j++) {
                if (u->using_at.items[j].line >= line) line = u->using_at.items[j].line + 1;
            }
            if (u && u->using_at.count == 0 && u->ns.len > 0) line = u->ns_at.line + 1;
            if (u && (u->using_at.count > 0 || u->ns.len > 0)) after = "";
            sb using_text = {0};
            sb title_using = {0};
            sb_printf(&using_text, "using " STR_FMT ";\n%s", STR_ARG(ns), after);
            sb_printf(&title_using, "Add 'using " STR_FMT ";'", STR_ARG(ns));
            const loc at = {line, 1, A.doc};
            quick_fix(out, written, title_using.data, offered.count == 1, d, at, at, using_text.data);
        }
    }
}

void analysis_code_actions(const int start_line, const int end_line, jbuf *out)
{
    jb_put(out, "[");
    int written = 0;
    suggestion_actions(start_line, end_line, out, &written);
    namespace_actions(start_line, end_line, out, &written);
    for (int i = 0; i < A.prog->fixes.count; i++) {
        const fix *f = &A.prog->fixes.items[i];
        if (f->at.file != A.doc || f->at.line - 1 < start_line || f->at.line - 1 > end_line) continue;
        if (f->kind == FIX_USE_THIS) {
            use_this_action(f->param, out, &written);
            continue;
        }
        if (f->kind == FIX_UNWRAP) {
            unwrap_actions(f, out, &written);
            continue;
        }
        const param *p = f->param;
        sb title = {0};
        sb text = {0};
        loc start = p ? type_start(p) : f->at;
        loc end = start;
        bool preferred = true;
        const char *separator = "";
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
        case FIX_MUT_LOCAL:
            sb_printf(&title, "Declare '" STR_FMT "' as mut", STR_ARG(f->local->name));
            start = end = f->local->at;
            sb_put(&text, "mut ");
            break;
        case FIX_MUT_METHOD:
            if (f->method->at.file != A.doc) continue;
            sb_printf(&title, "Make '" STR_FMT "' mut", STR_ARG(f->method->name));
            start = end = f->method->return_type_qual_at;
            sb_put(&text, "mut ");
            break;
        case FIX_CREATE_FUNCTION: {
            const expr *call = f->call;
            bool known = true;
            for (int k = 0; k < call->args.count; k++) known &= call->args.items[k]->type.kind != TY_ERROR;
            if (!known) continue;
            sb_printf(&title, "Create function '" STR_FMT "'", STR_ARG(call->name));
            start = end = doc_end(&separator);
            sb_printf(&text, "%svoid " STR_FMT "(", separator, STR_ARG(call->name));
            VEC(char *) names = {0};
            for (int k = 0; k < call->args.count; k++) {
                sb name = {0};
                argument_name(call->args.items[k], &name);
                for (int j = 0; j < names.count; j++) {
                    if (strcmp(names.items[j], name.data) == 0) sb_printf(&name, "%d", k + 1); // Two of the same
                }
                vec_push(names, name.data);
                sb_printf(&text, "%s%s %s", k ? ", " : "", type_name(call->args.items[k]->type), name.data);
            }
            sb_put(&text, ")\n{\n}\n");
            preferred = false;
            break;
        }
        case FIX_CREATE_STRUCT:
        case FIX_CREATE_COMPONENT: {
            if (memchr(f->name.ptr, '.', (size_t)f->name.len)) continue; // Another namespace's
            const char *keyword = f->kind == FIX_CREATE_STRUCT ? "struct" : "component";
            sb_printf(&title, "Create %s '" STR_FMT "'", keyword, STR_ARG(f->name));
            start = end = doc_end(&separator);
            sb_printf(&text, "%s%s " STR_FMT "\n{\n}\n", separator, keyword, STR_ARG(f->name));
            preferred = false;
            break;
        }
        case FIX_USE_THIS: case FIX_UNWRAP: continue; // Their own actions, above
        }
        quick_fix(out, &written, title.data, preferred, NULL, start, end, text.data ? text.data : "");
    }
    move_to_file_action(start_line, out, &written);
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
        return strncmp(type_c_name(o->type), "tide_", 5) == 0
            && cdefs_find(type_c_name(o->type), out);
    case OCC_FUNCTION:
    case OCC_CONSTANT: {
        if (!o->c_name) return false;
        char name[128];
        snprintf(name, sizeof name, "%s", o->c_name);
        char *call = strchr(name, '('); // tide_identity_q() for quaternion.identity
        if (call) *call = '\0';
        return cdefs_find(name, out);
    }
    case OCC_OWNER:
        if (str_eq_c(o->owner, "Devices")) return cdefs_find("tide_devices", out);
        return cdefs_find_header(str_eq_c(o->owner, "Draw")                                         ? "draw.h"
                                 : str_eq_c(o->owner, "GUI") || str_eq_c(o->owner, "GUILayout")
                                       || str_eq_c(o->owner, "Screen")                              ? "gui.h"
                                                                                                    : "math.h",
                                 out);
    case OCC_FIELD:
        return o->decl && o->decl->kind == DECL_RECORD && cdefs_find_member(o->decl->c_name, str_to_cstr(o->name), out);
    case OCC_MEMBER: {
        // Real struct members only: x, r, value, c0; not swizzles like xz.
        const type t = o->object_type;
        const bool vector_component = type_dim(t) >= 2 && o->name.len == 1;
        const bool other = (t.kind == TY_COLOR && o->name.len == 1) || t.kind == TY_QUATERNION || t.kind == TY_RECT
                        || matrix_dim(t) > 0;
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
        } else if ((o->kind == OCC_TYPE || o->kind == OCC_SYSTEM || o->kind == OCC_CONST
                    || (is_routine(o->decl) && o->kind != OCC_THIS))
                   && o->decl && !o->decl->builtin) {
            target = o->decl->at;
            len = o->decl->name.len;
        } else if (o->kind == OCC_PARAM) {
            target = o->param->name_at;
            len = o->param->name.len;
        } else if (o->kind == OCC_LOCAL) {
            target = o->local->name_at;
            len = o->local->name.len;
        } else if (o->kind == OCC_ENUM_MEMBER && o->decl && !o->decl->builtin) { // Options in Page.Options
            for (int i = 0; i < o->decl->members.count && target.line == 0; i++) {
                const enum_member *m = &o->decl->members.items[i];
                if (!str_eq(m->name, o->name)) continue;
                target = m->at;
                len = m->name.len;
            }
        } else if (o->kind == OCC_NAMESPACE) {
            // The first file that declares it (or a namespace inside it)
            for (int i = 0; i < A.occs.count && target.line == 0; i++) {
                const occurrence *d = &A.occs.items[i];
                if (d->kind == OCC_NAMESPACE && d->declaration && str_eq(d->name, o->name)) {
                    target = d->at;
                    len = d->len;
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

enum { SYMBOL_CLASS = 5, SYMBOL_METHOD = 6, SYMBOL_PROPERTY = 7, SYMBOL_FIELD = 8, SYMBOL_ENUM = 10, SYMBOL_INTERFACE = 11,
       SYMBOL_FUNCTION = 12, SYMBOL_CONSTANT = 14, SYMBOL_OBJECT = 19, SYMBOL_ENUM_MEMBER = 22, SYMBOL_STRUCT = 23,
       SYMBOL_EVENT = 24 };

static int symbol_kind(const decl *d)
{
    return d->kind == DECL_COMPONENT || d->kind == DECL_STRUCT ? SYMBOL_STRUCT
         : d->kind == DECL_SINGLETON                            ? SYMBOL_CLASS
         : d->kind == DECL_INPUT                                ? SYMBOL_INTERFACE
         : d->kind == DECL_EVENT                                ? SYMBOL_EVENT
         : d->kind == DECL_ENUM                                 ? SYMBOL_ENUM
         : d->kind == DECL_CONST                                ? SYMBOL_CONSTANT
                                                                : SYMBOL_FUNCTION;
}

void analysis_symbols(jbuf *out)
{
    jb_put(out, "[");
    int written = 0;
    for (int i = 0; i < A.prog->decls.count; i++) {
        const decl *d = A.prog->decls.items[i];
        if (d->builtin || d->at.file != A.doc) continue; // This document's declarations
        const int kind = symbol_kind(d);
        const loc start = {d->at.line, 1, d->at.file};
        const loc end = d->end.line > 0 ? d->end : d->at;
        if (written++) jb_put(out, ",");
        jb_put(out, "{\"name\":");
        jb_string_n(out, d->name.ptr, (size_t)d->name.len);
        jb_printf(out, ",\"detail\":\"%s\",\"kind\":%d,\"range\":{\"start\":", decl_what(d), kind);
        write_position(out, start);
        jb_put(out, ",\"end\":");
        write_position(out, (loc){end.line, end.col + 1, end.file});
        jb_put(out, "},\"selectionRange\":");
        write_range(out, d->at, d->name.len);
        jb_put(out, ",\"children\":[");
        for (int m = 0; m < d->members.count; m++) {
            const enum_member *member = &d->members.items[m];
            if (m) jb_put(out, ",");
            jb_put(out, "{\"name\":");
            jb_string_n(out, member->name.ptr, (size_t)member->name.len);
            jb_printf(out, ",\"detail\":\"%lld\",\"kind\":%d,\"range\":", (long long)member->number, SYMBOL_ENUM_MEMBER);
            write_range(out, member->at, member->name.len);
            jb_put(out, ",\"selectionRange\":");
            write_range(out, member->at, member->name.len);
            jb_put(out, "}");
        }
        bool first_child = d->members.count == 0;
        for (int f = 0; f < d->fields.count; f++) {
            const field *fl = &d->fields.items[f];
            if (fl->hidden) continue;
            if (!first_child) jb_put(out, ",");
            first_child = false;
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
        for (int k = 0; k < d->methods.count; k++) {
            const decl *m = d->methods.items[k];
            sb detail = {0};
            format_routine(m, &detail);
            if (!first_child) jb_put(out, ",");
            first_child = false;
            jb_put(out, "{\"name\":");
            jb_string_n(out, m->name.ptr, (size_t)m->name.len);
            jb_put(out, ",\"detail\":");
            jb_string(out, detail.data);
            jb_printf(out, ",\"kind\":%d,\"range\":{\"start\":", SYMBOL_METHOD);
            write_position(out, m->at);
            jb_put(out, ",\"end\":");
            write_position(out, (loc){m->end.line, m->end.col + 1, m->end.file});
            jb_put(out, "},\"selectionRange\":");
            write_range(out, m->at, m->name.len);
            jb_put(out, "}");
        }
        // The input's Sample and Sanitize
        for (int k = 0; d->kind == DECL_INPUT && k < 2; k++) {
            const stmt *body = k == 0 ? d->body : d->sanitize;
            if (!body) continue;
            const loc at = k == 0 ? d->body_at : d->sanitize_at;
            sb detail = {0};
            sb_put(&detail, k == 0 ? "Sample(" : "Sanitize(");
            for (int i = 0; k == 0 && i < d->params.count; i++) {
                if (i) sb_put(&detail, ", ");
                format_param(&d->params.items[i], &detail);
            }
            sb_put(&detail, ")");
            if (!first_child) jb_put(out, ",");
            first_child = false;
            jb_printf(out, "{\"name\":\"%s\",\"detail\":", k == 0 ? "Sample" : "Sanitize");
            jb_string(out, detail.data);
            jb_printf(out, ",\"kind\":%d,\"range\":{\"start\":", SYMBOL_METHOD);
            write_position(out, at);
            jb_put(out, ",\"end\":");
            write_position(out, (loc){body->end.line, body->end.col + 1, body->end.file});
            jb_put(out, "},\"selectionRange\":");
            write_range(out, at, k == 0 ? 6 : 8);
            jb_put(out, "}");
        }
        jb_put(out, "]}");
    }
    // settings { ... }, with what it sets
    for (int b = 0; b < A.prog->settings.count; b++) {
        const decl *block = A.prog->settings.items[b];
        if (block->at.file != A.doc) continue;
        const loc end = block->end.line > 0 ? block->end : block->at;
        if (written++) jb_put(out, ",");
        jb_printf(out, "{\"name\":\"settings\",\"detail\":\"the engine's\",\"kind\":%d,\"range\":{\"start\":", SYMBOL_OBJECT);
        write_position(out, block->at);
        jb_put(out, ",\"end\":");
        write_position(out, (loc){end.line, end.col + 1, end.file});
        jb_put(out, "},\"selectionRange\":");
        write_range(out, block->at, 8);
        jb_put(out, ",\"children\":[");
        for (int f = 0; f < block->fields.count; f++) {
            const field *fl = &block->fields.items[f];
            if (f) jb_put(out, ",");
            jb_put(out, "{\"name\":");
            jb_string_n(out, fl->name.ptr, (size_t)fl->name.len);
            jb_printf(out, ",\"kind\":%d,\"range\":", SYMBOL_PROPERTY);
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
// Type definitions and implementations

static void write_location(jbuf *out, const loc at, const int len)
{
    jb_put(out, "{\"uri\":");
    jb_string(out, A.files[at.file].uri);
    jb_put(out, ",\"range\":");
    write_range(out, at, len);
    jb_put(out, "}");
}

void analysis_type_definition(const int line, const int character, jbuf *out)
{
    const occurrence *o = occurrence_at(from_lsp(line, character));
    type t = {TY_ERROR, NULL};
    if (o) {
        switch (o->kind) {
        case OCC_TYPE: t = o->decl ? (type){TY_COMPONENT, (decl *)o->decl} : o->type; break; // Its own declaration
        case OCC_PARAM: t = o->param->type; break;
        case OCC_LOCAL: t = o->local->type; break;
        case OCC_FIELD: t = o->field->type; break;
        case OCC_FUNCTION: case OCC_METHOD: t = is_routine(o->decl) ? o->decl->return_type : o->type; break;
        case OCC_CONST: t = o->decl->return_type; break;
        default: t = o->type; break;
        }
    }
    // What a list, grid or T? holds, and a failable call's value
    while ((t.kind == TY_LIST || t.kind == TY_GRID || t.kind == TY_OPTIONAL || t.kind == TY_FAILABLE) && t.decl) {
        t = t.decl->fields.items[0].type;
    }
    const decl *d = t.kind == TY_COMPONENT || t.kind == TY_SINGLETON || t.kind == TY_INPUT || t.kind == TY_STRUCT
                         || t.kind == TY_EVENT || t.kind == TY_ENUM || t.kind == TY_RECORD
                     ? t.decl
                     : NULL;
    if (d && !d->builtin) write_location(out, d->at, d->name.len);
    else if (!(d && d->kind == DECL_RECORD && cdefs_find(d->c_name, out))) jb_put(out, "null"); // Devices, in C
}

// An event's handlers, and the input's Sample and Sanitize: the code that
// gives it its behavior.
void analysis_implementation(const int line, const int character, jbuf *out)
{
    const occurrence *o = occurrence_at(from_lsp(line, character));
    const decl *d = o && (o->kind == OCC_TYPE || o->kind == OCC_PARAM) ? o->kind == OCC_TYPE ? o->decl : o->param->type.decl
                                                                       : NULL;
    if (!d || (d->kind != DECL_EVENT && d->kind != DECL_INPUT)) {
        jb_put(out, "null");
        return;
    }
    jb_put(out, "[");
    int written = 0;
    for (int i = 0; d->kind == DECL_EVENT && i < d->handlers.count; i++) {
        if (written++) jb_put(out, ",");
        write_location(out, d->handlers.items[i]->at, d->handlers.items[i]->name.len);
    }
    if (d->kind == DECL_INPUT && d->body) {
        if (written++) jb_put(out, ",");
        write_location(out, d->body_at, 6);
    }
    if (d->kind == DECL_INPUT && d->sanitize) {
        if (written++) jb_put(out, ",");
        write_location(out, d->sanitize_at, 8);
    }
    jb_put(out, "]");
}

// ---------------------------------------------------------------------------
// Call hierarchy: who calls the game's functions and methods, and what they call

static bool block_contains(const stmt *block, loc at);

// Code that calls: a system, view or handler, a function or method, or the
// input's Sample or Sanitize, whose `body` holds the calls.
typedef struct caller {
    const decl *decl;
    const stmt *body;
    str name;
    loc name_at;
} caller;

// The code whose body holds `at`; its decl is NULL where there's none.
static caller caller_at(const loc at)
{
    for (int i = 0; i < A.prog->decls.count; i++) {
        const decl *d = A.prog->decls.items[i];
        if (d->builtin) continue;
        for (int k = 0; k < d->methods.count; k++) {
            const decl *m = d->methods.items[k];
            if (block_contains(m->body, at)) return (caller){m, m->body, m->name, m->at};
        }
        if (d->kind == DECL_INPUT && block_contains(d->body, at)) return (caller){d, d->body, str_from("Sample"), d->body_at};
        if (d->kind == DECL_INPUT && block_contains(d->sanitize, at)) {
            return (caller){d, d->sanitize, str_from("Sanitize"), d->sanitize_at};
        }
        if ((d->kind == DECL_SYSTEM || d->kind == DECL_FUNCTION) && block_contains(d->body, at)) {
            return (caller){d, d->body, d->name, d->at};
        }
    }
    return (caller){NULL, NULL, {"", 0}, {0, 0, 0}};
}

// A CallHierarchyItem: its whole code, and its name, which says which it is.
static void write_call_item(jbuf *out, const caller *c)
{
    const decl *d = c->decl;
    sb detail = {0};
    if (is_routine(d)) format_routine(d, &detail);
    else if (d->kind == DECL_SYSTEM) format_header(d, &detail);
    else sb_printf(&detail, "input " STR_FMT, STR_ARG(d->name));
    const int kind = d->kind == DECL_METHOD || d->kind == DECL_INPUT ? SYMBOL_METHOD : SYMBOL_FUNCTION;
    const loc end = c->body ? c->body->end : d->end;
    jb_put(out, "{\"name\":");
    jb_string_n(out, c->name.ptr, (size_t)c->name.len);
    jb_printf(out, ",\"kind\":%d,\"detail\":", kind);
    jb_string(out, detail.data);
    jb_put(out, ",\"uri\":");
    jb_string(out, A.files[c->name_at.file].uri);
    jb_put(out, ",\"range\":");
    write_edit_range(out, (loc){c->name_at.line, 1, c->name_at.file}, (loc){end.line, end.col + 1, end.file});
    jb_put(out, ",\"selectionRange\":");
    write_range(out, c->name_at, c->name.len);
    jb_put(out, "}");
}

// The code named at the cursor, which the call hierarchy is about: a function
// or method where it's declared or called, or a system where it's declared.
static caller hierarchy_target(const int line, const int character)
{
    const occurrence *o = occurrence_at(from_lsp(line, character));
    if (o && (o->kind == OCC_FUNCTION || o->kind == OCC_METHOD) && is_routine(o->decl) && !o->decl->is_extern) {
        return (caller){o->decl, o->decl->body, o->decl->name, o->decl->at};
    }
    if (o && o->kind == OCC_METHOD && o->declaration && o->decl && o->decl->kind == DECL_INPUT) { // Sample, Sanitize
        return caller_at(o->decl->body && str_eq_c(o->name, "Sample") ? o->decl->body->end : o->decl->sanitize->end);
    }
    if (o && o->kind == OCC_SYSTEM && o->decl) return (caller){o->decl, o->decl->body, o->decl->name, o->decl->at};
    if (o && (o->kind == OCC_FUNCTION || o->kind == OCC_METHOD) && is_routine(o->decl)) { // An extern function: no body
        return (caller){o->decl, NULL, o->decl->name, o->decl->at};
    }
    return (caller){NULL, NULL, {"", 0}, {0, 0, 0}};
}

void analysis_prepare_call_hierarchy(const int line, const int character, jbuf *out)
{
    const caller target = hierarchy_target(line, character);
    if (!target.decl) {
        jb_put(out, "null");
        return;
    }
    jb_put(out, "[");
    write_call_item(out, &target);
    jb_put(out, "]");
}

// A call of a game's function or method, by name or by operator.
static bool is_call(const occurrence *o)
{
    return (o->kind == OCC_FUNCTION || o->kind == OCC_METHOD) && !o->declaration && is_routine(o->decl);
}

// Who calls the item at the cursor: each caller once, with where it does.
void analysis_incoming_calls(const int line, const int character, jbuf *out)
{
    const caller target = hierarchy_target(line, character);
    jb_put(out, "[");
    int written = 0;
    VEC(const stmt *) done = {0};
    for (int i = 0; target.decl && i < A.occs.count; i++) {
        const occurrence *o = &A.occs.items[i];
        if (!is_call(o) || o->decl != target.decl) continue;
        const caller from = caller_at(o->at);
        bool seen = !from.decl;
        for (int k = 0; k < done.count && !seen; k++) seen = done.items[k] == from.body;
        if (seen) continue;
        vec_push(done, from.body);
        if (written++) jb_put(out, ",");
        jb_put(out, "{\"from\":");
        write_call_item(out, &from);
        jb_put(out, ",\"fromRanges\":[");
        for (int k = i, ranges = 0; k < A.occs.count; k++) {
            const occurrence *call = &A.occs.items[k];
            if (!is_call(call) || call->decl != target.decl || caller_at(call->at).body != from.body) continue;
            if (ranges++) jb_put(out, ",");
            write_range(out, call->at, call->len);
        }
        jb_put(out, "]}");
    }
    jb_put(out, "]");
}

// What the item at the cursor calls: each function or method once, with
// where it does.
void analysis_outgoing_calls(const int line, const int character, jbuf *out)
{
    const caller source = hierarchy_target(line, character);
    jb_put(out, "[");
    int written = 0;
    VEC(const decl *) done = {0};
    for (int i = 0; source.body && i < A.occs.count; i++) {
        const occurrence *o = &A.occs.items[i];
        if (!is_call(o) || caller_at(o->at).body != source.body) continue;
        bool seen = false;
        for (int k = 0; k < done.count && !seen; k++) seen = done.items[k] == o->decl;
        if (seen) continue;
        vec_push(done, o->decl);
        const caller to = {o->decl, o->decl->body, o->decl->name, o->decl->at};
        if (written++) jb_put(out, ",");
        jb_put(out, "{\"to\":");
        write_call_item(out, &to);
        jb_put(out, ",\"fromRanges\":[");
        for (int k = i, ranges = 0; k < A.occs.count; k++) {
            const occurrence *call = &A.occs.items[k];
            if (!is_call(call) || call->decl != o->decl || caller_at(call->at).body != source.body) continue;
            if (ranges++) jb_put(out, ",");
            write_range(out, call->at, call->len);
        }
        jb_put(out, "]}");
    }
    jb_put(out, "]");
}

// ---------------------------------------------------------------------------
// Selection ranges: what holds the cursor, from its word out to its declaration

typedef struct span {
    loc start;
    loc end; // Just past it
} span;

typedef VEC(span) span_list;

static loc stmt_start(const stmt *s);
static loc stmt_end(const stmt *s);

static bool around(const loc at, const loc start, const loc end)
{
    return start.file == at.file && loc_cmp(start, at) <= 0 && loc_cmp(at, end) <= 0;
}

// Adds a span around the cursor, inside the last one: once, however many
// things it's the span of.
static void add_span(span_list *spans, const loc at, const loc start, const loc end)
{
    if (!around(at, start, end)) return;
    if (spans->count > 0) {
        const span *outer = &spans->items[spans->count - 1];
        if (loc_cmp(start, outer->start) < 0 || loc_cmp(outer->end, end) < 0) return;
        if (loc_cmp(outer->start, start) == 0 && loc_cmp(outer->end, end) == 0) return;
    }
    const span s = {start, end};
    vec_push(*spans, s);
}

static void select_in_stmt(const stmt *s, loc at, span_list *spans);

static void select_in_expr(const expr *e, const loc at, span_list *spans)
{
    if (!e || !around(at, expr_start(e), expr_end(e))) return;
    add_span(spans, at, expr_start(e), expr_end(e));
    select_in_expr(e->object, at, spans);
    select_in_expr(e->lhs, at, spans);
    select_in_expr(e->rhs, at, spans);
    select_in_expr(e->cond, at, spans);
    for (int i = 0; i < e->args.count; i++) select_in_expr(e->args.items[i], at, spans);
    for (int i = 0; e->kind == E_LITERAL && i < e->inits.count; i++) select_in_expr(e->inits.items[i].value, at, spans);
    if (e->kind == E_CALL || e->kind == E_METHOD) select_in_stmt(e->block, at, spans);
}

static void select_in_stmt(const stmt *s, const loc at, span_list *spans)
{
    if (!s || !around(at, stmt_start(s), stmt_end(s))) return;
    add_span(spans, at, stmt_start(s), stmt_end(s));
    for (int i = 0; s->kind == S_BLOCK && i < s->stmts.count; i++) select_in_stmt(s->stmts.items[i], at, spans);
    select_in_stmt(s->init, at, spans);
    select_in_expr(s->cond, at, spans);
    select_in_stmt(s->step, at, spans);
    select_in_expr(s->target, at, spans);
    select_in_expr(s->value, at, spans);
    select_in_expr(s->by, at, spans);
    select_in_expr(s->offset, at, spans);
    select_in_stmt(s->then_stmt, at, spans);
    select_in_stmt(s->else_stmt, at, spans);
    for (int i = 0; s->kind == S_SWITCH && i < s->cases.count; i++) {
        const switch_case *section = &s->cases.items[i];
        for (int k = 0; k < section->labels.count; k++) select_in_expr(section->labels.items[k], at, spans);
        for (int k = 0; k < section->body.count; k++) select_in_stmt(section->body.items[k], at, spans);
    }
}

// A declaration's lines, from its attributes to its closing brace or ';'.
static void select_decl(const decl *d, const stmt *body, const loc at, span_list *spans)
{
    int first = d->at.line;
    for (int a = 0; a < d->attributes.count; a++) {
        if (d->attributes.items[a].at.line < first) first = d->attributes.items[a].at.line;
    }
    const loc end = body ? body->end : d->end.line > 0 ? d->end : d->at;
    add_span(spans, at, (loc){first, 1, d->at.file}, (loc){end.line, end.col + 1, end.file});
}

// The spans around `at`, outermost first.
static void selection_spans(const loc at, span_list *spans)
{
    for (int i = 0; i < A.prog->decls.count; i++) {
        const decl *d = A.prog->decls.items[i];
        if (d->builtin || d->at.file != at.file) continue;
        const int before = spans->count;
        select_decl(d, d->kind == DECL_SYSTEM || d->kind == DECL_FUNCTION ? d->body : NULL, at, spans);
        if (spans->count == before) continue;
        for (int k = 0; k < d->methods.count; k++) {
            select_decl(d->methods.items[k], d->methods.items[k]->body, at, spans);
            select_in_stmt(d->methods.items[k]->body, at, spans);
        }
        if (d->kind == DECL_INPUT) { // Sample and Sanitize, from their names
            if (d->body) add_span(spans, at, d->body_at, (loc){d->body->end.line, d->body->end.col + 1, d->at.file});
            select_in_stmt(d->body, at, spans);
            if (d->sanitize) add_span(spans, at, d->sanitize_at, (loc){d->sanitize->end.line, d->sanitize->end.col + 1, d->at.file});
            select_in_stmt(d->sanitize, at, spans);
        } else {
            select_in_stmt(d->body, at, spans);
        }
        select_in_expr(d->value, at, spans); // A constant's
        for (int f = 0; f < d->fields.count; f++) select_in_expr(d->fields.items[f].default_value, at, spans);
        break;
    }
    // The word at the cursor
    const afile *f = &A.files[at.file];
    for (int i = 0; i < f->tok_count; i++) {
        const token *t = &f->toks[i];
        if (t->at.line == at.line && t->at.col <= at.col && at.col <= t->at.col + token_len(t)) {
            add_span(spans, at, t->at, token_end(t));
            break;
        }
    }
}

void analysis_selection_ranges(const json *positions, jbuf *out)
{
    jb_put(out, "[");
    for (int p = 0; positions && positions->kind == JSON_ARRAY && p < positions->count; p++) {
        if (p) jb_put(out, ",");
        const loc at = from_lsp(json_int(json_get(positions->items[p], "line"), 0),
                                json_int(json_get(positions->items[p], "character"), 0));
        span_list spans = {0};
        selection_spans(at, &spans);
        if (spans.count == 0) add_span(&spans, at, at, at); // Each position gets one
        // Innermost first, each with the one around it as its parent
        for (int i = spans.count - 1; i >= 0; i--) {
            jb_put(out, "{\"range\":");
            write_edit_range(out, spans.items[i].start, spans.items[i].end);
            if (i > 0) jb_put(out, ",\"parent\":");
        }
        for (int i = 0; i < spans.count; i++) jb_put(out, "}");
    }
    jb_put(out, "]");
}

// ---------------------------------------------------------------------------
// Folding: blocks and literals over several lines, and runs of comment lines

static void folding_range(jbuf *out, int *written, const int first, const int last, const char *kind)
{
    if (last <= first) return;
    if ((*written)++) jb_put(out, ",");
    jb_printf(out, "{\"startLine\":%d,\"endLine\":%d%s%s%s}", first - 1, last - 1, kind ? ",\"kind\":\"" : "",
              kind ? kind : "", kind ? "\"" : "");
}

static const char *token_start(const token *t);
static int line_of(const char *p);
static const unit *doc_unit(void);

void analysis_folding_ranges(jbuf *out)
{
    jb_put(out, "[");
    int written = 0;
    // A block folds from the line that introduces it, even when its `{` is on
    // a line of its own below, and keeps its `}` in sight. So do arguments,
    // parameters and a list's elements over several lines, keeping their `)`
    // or `]` in sight when it starts a line.
    int *open = arena_alloc(sizeof(int) * ((size_t)DOC->tok_count + 1));
    int depth = 0;
    for (int i = 0; i < DOC->tok_count; i++) {
        const token *t = &DOC->toks[i];
        if (t->kind == T_LBRACE || t->kind == T_LPAREN || t->kind == T_LBRACKET) {
            open[depth++] = i;
        } else if ((t->kind == T_RBRACE || t->kind == T_RPAREN || t->kind == T_RBRACKET) && depth > 0) {
            const int o = open[--depth];
            int first = DOC->toks[o].at.line;
            if (t->kind != T_RBRACE) {
                const bool starts_line = DOC->toks[i - 1].at.line != t->at.line;
                folding_range(out, &written, first, starts_line ? t->at.line - 1 : t->at.line, NULL);
                continue;
            }
            if (o > 0 && DOC->toks[o - 1].at.line == first - 1 && DOC->toks[o - 1].kind != T_RBRACE
                && DOC->toks[o - 1].kind != T_SEMI) {
                first--; // component Body\n{
            }
            folding_range(out, &written, first, t->at.line - 1, NULL);
        }
    }
    // Comment lines one after another, three or more, and comments in /* */
    // over several lines: between tokens, where they are.
    for (int line = 1; line <= DOC->lines.count;) {
        int end = line;
        while (end <= DOC->lines.count && comment_line(end)) end++;
        if (end - line >= 3) folding_range(out, &written, line, end - 1, "comment");
        line = end > line ? end : line + 1;
    }
    const char *p = DOC->src.text;
    for (int i = 0; i <= DOC->tok_count; i++) {
        const char *next = i < DOC->tok_count ? token_start(&DOC->toks[i]) : DOC->src.text + DOC->src.len;
        for (; p + 1 < next; p++) {
            if (p[0] == '/' && p[1] == '/') {
                while (p < next && *p != '\n') p++;
            } else if (p[0] == '/' && p[1] == '*') {
                const char *start = p;
                for (p += 2; p + 1 < next && !(p[0] == '*' && p[1] == '/'); p++) {
                }
                folding_range(out, &written, line_of(start), line_of(p), "comment");
            }
        }
        if (i < DOC->tok_count) p = next + token_len(&DOC->toks[i]);
    }
    // using lines one after another
    const unit *u = doc_unit();
    for (int k = 0; u && k < u->using_at.count;) {
        int last = k;
        while (last + 1 < u->using_at.count && u->using_at.items[last + 1].line == u->using_at.items[last].line + 1) last++;
        folding_range(out, &written, u->using_at.items[k].line, u->using_at.items[last].line, "imports");
        k = last + 1;
    }
    jb_put(out, "]");
}

// ---------------------------------------------------------------------------
// Inlay hints: the type a `var` gets, and parameter names at literal arguments

static bool is_literal(const expr *e)
{
    if (e->kind == E_UNARY && e->op == T_MINUS) e = e->lhs;
    return e->kind == E_INT || e->kind == E_FLOAT || e->kind == E_BOOL || e->kind == E_STRING || e->kind == E_DEFAULT;
}

// Where an expression starts: its leftmost token.
static loc expr_start(const expr *e)
{
    for (;;) {
        if ((e->kind == E_MEMBER || e->kind == E_METHOD || e->kind == E_INDEX) && e->object) e = e->object;
        else if (e->kind == E_BINARY || e->kind == E_COALESCE || e->kind == E_IS || e->kind == E_DEFAULTED) e = e->lhs;
        else if (e->kind == E_CONDITIONAL) e = e->cond;
        else break;
    }
    return e->kind == E_LITERAL && e->qual_at.line ? e->qual_at : e->at;
}

// Where a statement starts: an assignment's and a call's `at` is past it.
static loc stmt_start(const stmt *s)
{
    if (s->kind == S_EXPR) return expr_start(s->value);
    if (s->kind == S_ASSIGN && s->target) {
        const loc target = expr_start(s->target);
        return loc_cmp(target, s->at) < 0 ? target : s->at; // i++, or ++i
    }
    return s->at;
}

// The token at or just after `at` in its file.
static int token_from(const loc at)
{
    if (at.file < 0 || at.file >= A.file_count) return -1;
    const afile *f = &A.files[at.file];
    int lo = 0;
    int hi = f->tok_count;
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (loc_cmp(f->toks[mid].at, at) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static loc token_end(const token *t);

// The bracket that closes the one at token `open` of the expression's file:
// its index, or the last token's where a syntax error left it open.
static int closing(const loc at, const int open)
{
    const afile *f = &A.files[at.file];
    int depth = 0;
    for (int i = open; i < f->tok_count; i++) {
        const tok_kind k = f->toks[i].kind;
        if (k == T_LPAREN || k == T_LBRACKET || k == T_LBRACE) depth++;
        else if ((k == T_RPAREN || k == T_RBRACKET || k == T_RBRACE) && --depth == 0) return i;
    }
    return f->tok_count - 1;
}

static loc stmt_end(const stmt *s);

// Just past an expression's last token.
static loc expr_end(const expr *e)
{
    const afile *f = &A.files[e->at.file];
    const int t = token_at(e->at);
    switch (e->kind) {
    case E_NAME: return (loc){e->at.line, e->at.col + e->name.len, e->at.file};
    case E_MEMBER: return (loc){e->at.line, e->at.col + e->member.len, e->at.file};
    case E_BINARY: case E_COALESCE: case E_CONDITIONAL: return expr_end(e->rhs);
    case E_UNARY: case E_TRY: case E_AWAIT: return expr_end(e->lhs);
    case E_IS:
        if (e->binding) return (loc){e->binding->name_at.line, e->binding->name_at.col + e->binding->name.len, e->at.file};
        return (loc){e->pattern_at.line, e->pattern_at.col + last_part(e->pattern).len, e->at.file};
    case E_CALL: case E_METHOD:
        if (e->block) return stmt_end(e->block); // Its block, after the call
        if (t < 0 || t + 1 >= f->tok_count || f->toks[t + 1].kind != T_LPAREN) {
            return e->args.count > 0 ? expr_end(e->args.items[e->args.count - 1]) : e->at; // cells[x, y]'s position
        }
        return token_end(&f->toks[closing(e->at, t + 1)]);
    case E_LITERAL: // Body { ... }: the '{' after its name
        if (t >= 0 && t + 1 < f->tok_count && f->toks[t + 1].kind == T_LBRACE) return token_end(&f->toks[closing(e->at, t + 1)]);
        break;
    case E_INDEX: case E_LIST: // `at` is the '['
        if (t >= 0) return token_end(&f->toks[closing(e->at, t)]);
        break;
    case E_INTERP: // To the part that ends with the quote, past those of text in its values
        for (int i = t, depth = 0; i >= 0 && i < f->tok_count; i++) {
            const token *part = &f->toks[i];
            if (part->kind != T_INTERP && part->kind != T_INTERP_PART) continue;
            if (part->kind == T_INTERP) depth++;
            if (part->text.ptr[part->text.len - 1] == '"' && --depth == 0) return token_end(part);
        }
        break;
    default:
        break;
    }
    return t >= 0 ? token_end(&f->toks[t]) : e->at; // A literal, this, default, null, or a value's `!`
}

// Just past a statement's last token: its ';', or its block's '}'.
static loc stmt_end(const stmt *s)
{
    switch (s->kind) {
    case S_BLOCK:
    case S_SWITCH:
        return (loc){s->end.line, s->end.col + 1, s->end.file};
    case S_IF:
        return stmt_end(s->else_stmt ? s->else_stmt : s->then_stmt);
    case S_WHILE:
    case S_FOR:
    case S_FOREACH:
    case S_PARALLEL:
        if (s->then_stmt) return stmt_end(s->then_stmt);
        break;
    case S_EXPR:
        if (s->value->block) return stmt_end(s->value->block); // A call with a block after it
        break;
    default:
        break;
    }
    // To its ';', or to the '}' around it where a syntax error cut it short.
    const loc start = stmt_start(s);
    const afile *f = &A.files[start.file];
    int depth = 0;
    int i = token_from(start);
    for (; i < f->tok_count; i++) {
        const tok_kind k = f->toks[i].kind;
        if (k == T_LPAREN || k == T_LBRACKET || k == T_LBRACE) depth++;
        else if ((k == T_RPAREN || k == T_RBRACKET || k == T_RBRACE) && --depth < 0) break;
        else if (k == T_SEMI && depth == 0) return token_end(&f->toks[i]);
    }
    return i > 0 ? token_end(&f->toks[i - 1]) : start;
}

static void inlay_hint(jbuf *out, int *written, const loc at, const char *label, const int kind, const bool before)
{
    if ((*written)++) jb_put(out, ",");
    jb_put(out, "{\"position\":");
    write_position(out, at);
    jb_put(out, ",\"label\":");
    jb_string(out, label);
    jb_printf(out, ",\"kind\":%d,\"%s\":true}", kind, before ? "paddingRight" : "paddingLeft");
}

static int call_param_names(const expr *call, str *names, int max);

void analysis_inlay_hints(const int start_line, const int end_line, jbuf *out)
{
    enum { HINT_TYPE = 1, HINT_PARAMETER = 2 };
    jb_put(out, "[");
    int written = 0;
    for (int i = 0; i < A.occs.count; i++) { // `var speed = ...`: speed's type
        const occurrence *o = &A.occs.items[i];
        if (o->at.file != A.doc || o->kind != OCC_LOCAL || !o->declaration || o->local->type_name.len > 0) continue;
        if (o->at.line - 1 < start_line || o->at.line - 1 > end_line || o->type.kind == TY_ERROR) continue;
        sb label = {0};
        sb_printf(&label, ": %s", type_name(o->type));
        inlay_hint(out, &written, (loc){o->at.line, o->at.col + o->len, o->at.file}, label.data, HINT_TYPE, false);
    }
    for (int i = 0; i < A.calls.count; i++) { // Heal(unit.stats, amount: 2), Draw.Circle(p, radius: 3, color)
        const expr *call = A.calls.items[i];
        if (call->at.file != A.doc || call->at.line - 1 < start_line || call->at.line - 1 > end_line) continue;
        str names[16];
        const int count = call_param_names(call, names, 16);
        for (int k = 0; k < call->args.count && k < count; k++) {
            if (!is_literal(call->args.items[k]) || names[k].len == 0) continue;
            sb label = {0};
            sb_printf(&label, STR_FMT ":", STR_ARG(names[k]));
            inlay_hint(out, &written, expr_start(call->args.items[k]), label.data, HINT_PARAMETER, true);
        }
    }
    jb_put(out, "]");
}

// Whether `name` holds the letters of `query` in order, ignoring case, as
// editors match symbols: "stHp" finds "StatsHelp".
static bool fuzzy_match(const str name, const char *query)
{
    int k = 0;
    for (const char *q = query; *q; q++) {
        while (k < name.len && tolower((unsigned char)name.ptr[k]) != tolower((unsigned char)*q)) k++;
        if (k == name.len) return false;
        k++;
    }
    return true;
}

static void workspace_symbol(jbuf *out, int *written, const str name, const int kind, const loc at, const int len,
                             const str container)
{
    if ((*written)++) jb_put(out, ",");
    jb_put(out, "{\"name\":");
    jb_string_n(out, name.ptr, (size_t)name.len);
    jb_printf(out, ",\"kind\":%d,\"location\":{\"uri\":", kind);
    jb_string(out, A.files[at.file].uri);
    jb_put(out, ",\"range\":");
    write_range(out, at, len);
    jb_put(out, "}");
    if (container.len > 0) {
        jb_put(out, ",\"containerName\":");
        jb_string_n(out, container.ptr, (size_t)container.len);
    }
    jb_put(out, "}");
}

void analysis_workspace_symbols(const char *query, jbuf *out, int *written)
{
    for (int i = 0; A.prog && i < A.prog->decls.count; i++) {
        const decl *d = A.prog->decls.items[i];
        if (d->builtin) continue;
        const int kind = symbol_kind(d);
        const str ns = d->unit ? d->unit->ns : (str){"", 0};
        if (fuzzy_match(d->name, query)) workspace_symbol(out, written, d->name, kind, d->at, d->name.len, ns);
        for (int k = 0; k < d->methods.count; k++) {
            const decl *m = d->methods.items[k];
            if (m->is_operator || !fuzzy_match(m->name, query)) continue;
            workspace_symbol(out, written, m->name, SYMBOL_METHOD, m->at, m->name.len, d->qualified);
        }
    }
}

// ---------------------------------------------------------------------------
// Semantic tokens: highlighting from what names mean, not how they look

static const char *const token_types[] = {"namespace", "type",     "struct", "class",   "interface", "parameter",
                                          "variable",  "property", "enumMember", "function", "method", "keyword",
                                          "decorator", "enum"};
enum { ST_NAMESPACE, ST_TYPE, ST_STRUCT, ST_CLASS, ST_INTERFACE, ST_PARAMETER, ST_VARIABLE, ST_PROPERTY,
       ST_ENUM_MEMBER, ST_FUNCTION, ST_METHOD, ST_KEYWORD, ST_DECORATOR, ST_ENUM };

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
        if (o->backing) { // byte, like int
            *type = ST_KEYWORD;
            *mods = 0;
            break;
        }
        if (!o->decl) {
            *type = is_keyword_type(o->type) ? ST_KEYWORD : ST_TYPE; // Color, Entity, PlayerID are types
            *mods = is_keyword_type(o->type) ? 0 : *mods | SM_DEFAULT_LIBRARY;
            break;
        }
        // Components and structs are structs, singletons classes, inputs
        // interfaces, and the device records types: editors can color each kind.
        *type = o->decl->kind == DECL_COMPONENT || o->decl->kind == DECL_STRUCT || o->decl->kind == DECL_EVENT ? ST_STRUCT
              : o->decl->kind == DECL_ENUM      ? ST_ENUM
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
    case OCC_FUNCTION:
        *type = ST_FUNCTION;
        if (!is_routine(o->decl)) *mods |= SM_DEFAULT_LIBRARY | SM_STATIC;
        break;
    case OCC_CONSTANT: *type = ST_ENUM_MEMBER; *mods |= SM_DEFAULT_LIBRARY | SM_STATIC | SM_READONLY; break;
    case OCC_CONST: *type = ST_ENUM_MEMBER; *mods |= SM_STATIC | SM_READONLY; break; // Like Math.PI
    case OCC_SETTING: *type = ST_PROPERTY; *mods |= SM_DEFAULT_LIBRARY; break;
    case OCC_METHOD:
        if (is_operator_decl(o->decl)) {
            *type = ST_KEYWORD;
            *mods = 0;
        } else if (is_routine(o->decl)) {
            *type = ST_METHOD;
        } else if (str_eq_c(o->name, "Sample") || str_eq_c(o->name, "Sanitize")) { // The input's own members
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
    case OCC_ENUM_MEMBER: *type = ST_ENUM_MEMBER; *mods |= SM_READONLY; break;
    case OCC_THIS: *type = ST_KEYWORD; *mods = 0; break;
    case OCC_DEFAULT: *type = ST_KEYWORD; *mods = 0; break;
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
        if (is_operator_decl(o->decl) && !o->declaration) continue; // `+` in code: the editor colors it as an operator
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
    CK_PROPERTY = 10, CK_ENUM = 13, CK_KEYWORD = 14, CK_SNIPPET = 15, CK_ENUM_MEMBER = 20, CK_CONSTANT = 21, CK_STRUCT = 22,
    CK_EVENT = 23,
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
    const decl *decl;           // The system, view, method, function or input whose body holds the cursor, or NULL
    const decl *fields_of;      // Whose fields plain names refer to: the input in Sample, a type in its methods
    bool in_input;
    bool in_sanitize; // Sets in_input too, for the fields
    VEC(const stmt *) locals;   // Locals declared before the cursor, still in scope
} scope;

static bool block_contains(const stmt *block, const loc at)
{
    return block && block->kind == S_BLOCK && block->at.file == at.file && loc_cmp(block->at, at) < 0
        && loc_cmp(at, block->end) <= 0;
}

// Whether the cursor is in a statement's body: a block, or a statement of its
// own without braces, as in `foreach (var p in ps) Use(p);`.
static bool body_contains(const stmt *body, const loc at)
{
    if (!body || body->kind == S_BLOCK) return block_contains(body, at);
    return body->at.file == at.file && loc_cmp(stmt_start(body), at) <= 0 && loc_cmp(at, stmt_end(body)) <= 0;
}

static void collect_list(stmt *const *stmts, int count, loc at, scope *sc);

// The locals of a body the cursor is in: a block's, or a braceless statement's.
static void collect_body(stmt *const *body, const loc at, scope *sc)
{
    if ((*body)->kind == S_BLOCK) collect_list((*body)->stmts.items, (*body)->stmts.count, at, sc);
    else collect_list(body, 1, at, sc);
}

// The names `is` gives in a condition, joined by &&: in scope where it's true.
static void collect_bindings(const expr *cond, scope *sc)
{
    if (!cond) return;
    if (cond->kind == E_IS && cond->binding) vec_push(sc->locals, cond->binding);
    if (cond->kind != E_BINARY || cond->op != T_AND) return;
    collect_bindings(cond->lhs, sc);
    collect_bindings(cond->rhs, sc);
}

static void collect_locals(const stmt *block, const loc at, scope *sc)
{
    collect_list(block->stmts.items, block->stmts.count, at, sc);
}

// The locals declared before the cursor in a list of statements, and in the
// ones around it.
static void collect_list(stmt *const *stmts, const int count, const loc at, scope *sc)
{
    for (int i = 0; i < count; i++) {
        const stmt *s = stmts[i];
        if (loc_cmp(s->at, at) >= 0) break;
        if (s->kind == S_VAR && loc_cmp(s->name_at, at) < 0) vec_push(sc->locals, s);
        if (block_contains(s, at)) collect_locals(s, at, sc);
        if (s->kind == S_IF) {
            if (body_contains(s->then_stmt, at)) {
                collect_bindings(s->cond, sc); // if (Parse(t) is int score)
                collect_body(&s->then_stmt, at, sc);
            }
            if (body_contains(s->else_stmt, at)) collect_body(&s->else_stmt, at, sc); // else if (...) too
        }
        if ((s->kind == S_FOREACH || s->kind == S_PARALLEL) && body_contains(s->then_stmt, at)) {
            vec_push(sc->locals, s); // foreach (var item in ...)
            collect_body(&s->then_stmt, at, sc);
        }
        if ((s->kind == S_WHILE || s->kind == S_FOR) && body_contains(s->then_stmt, at)) {
            if (s->init && s->init->kind == S_VAR) vec_push(sc->locals, s->init); // for (var i = 0; ...)
            collect_bindings(s->cond, sc);
            collect_body(&s->then_stmt, at, sc);
        }
        if (s->kind == S_EXPR && block_contains(s->value->block, at)) collect_locals(s->value->block, at, sc);
        if (s->kind == S_SWITCH && loc_cmp(at, s->end) <= 0) {
            // The section the cursor is in: from its first label to the next section's.
            for (int k = 0; k < s->cases.count; k++) {
                const switch_case *section = &s->cases.items[k];
                const bool started = section->label_at.count > 0 && loc_cmp(section->label_at.items[0], at) < 0;
                const bool ended = k + 1 < s->cases.count && s->cases.items[k + 1].label_at.count > 0
                                && loc_cmp(s->cases.items[k + 1].label_at.items[0], at) < 0;
                if (started && !ended) collect_list(section->body.items, section->body.count, at, sc);
            }
        }
    }
}

static scope scope_at(const loc at)
{
    scope sc = {0};
    for (int i = 0; i < A.prog->decls.count; i++) {
        const decl *d = A.prog->decls.items[i];
        for (int k = 0; k < d->methods.count; k++) {
            const decl *m = d->methods.items[k];
            if (!block_contains(m->body, at)) continue;
            sc.decl = m;
            sc.fields_of = m->is_operator ? NULL : d;
            collect_locals(m->body, at, &sc);
        }
        const bool sanitize = block_contains(d->sanitize, at);
        if (d->builtin || (!block_contains(d->body, at) && !sanitize)) continue;
        sc.decl = d;
        sc.in_input = d->kind == DECL_INPUT;
        sc.fields_of = sc.in_input ? d : NULL;
        sc.in_sanitize = sanitize;
        collect_locals(sanitize ? d->sanitize : d->body, at, &sc);
    }
    return sc;
}

// The type of `this` in scope: the entity the code runs for, or a component
// method's. TY_ERROR where there's none.
static type this_type(const scope *sc)
{
    const decl *d = sc->decl;
    if (!d || sc->in_input) return (type){TY_ERROR, NULL};
    if (d->kind == DECL_METHOD && !d->is_operator && !d->is_interpolate && d->owner && d->owner->kind == DECL_COMPONENT) {
        return (type){d->owner->is_local ? TY_LOCAL_ENTITY : TY_ENTITY, NULL};
    }
    if (d->kind == DECL_SYSTEM && d->per_entity) return (type){d->entity_local ? TY_LOCAL_ENTITY : TY_ENTITY, NULL};
    return (type){TY_ERROR, NULL};
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
    if (sc->fields_of) {
        for (int i = 0; i < sc->fields_of->fields.count; i++) {
            if (str_eq(sc->fields_of->fields.items[i].name, name)) return sc->fields_of->fields.items[i].type;
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
    if (t.decl && (t.kind == TY_COMPONENT || t.kind == TY_SINGLETON || t.kind == TY_INPUT || t.kind == TY_RECORD
                   || t.kind == TY_STRUCT || t.kind == TY_EVENT)) {
        for (int i = 0; i < t.decl->fields.count; i++) {
            if (str_eq(t.decl->fields.items[i].name, member)) return t.decl->fields.items[i].type;
        }
    }
    const int dim = type_dim(t);
    if (dim >= 2 && is_swizzle(member, dim)) return vector_type(type_is_float_based(t), member.len);
    if (t.kind == TY_COLOR && member.len == 1 && strchr("rgba", member.ptr[0])) return (type){TY_FLOAT, NULL};
    if (t.kind == TY_RECT && (str_eq_c(member, "x") || str_eq_c(member, "y") || str_eq_c(member, "width")
                              || str_eq_c(member, "height"))) {
        return (type){TY_FLOAT, NULL};
    }
    if (t.kind == TY_STRING && str_eq_c(member, "Length")) return (type){TY_INT, NULL};
    if (t.kind == TY_LIST && str_eq_c(member, "Count")) return (type){TY_INT, NULL};
    if (t.kind == TY_GRID && (str_eq_c(member, "size") || str_eq_c(member, "min") || str_eq_c(member, "max"))) {
        return (type){t.decl->dims == 3 ? TY_INT3 : TY_INT2, NULL};
    }
    if (t.kind == TY_QUATERNION && str_eq_c(member, "value")) return (type){TY_FLOAT4, NULL};
    const int n = matrix_dim(t);
    if (n > 0 && member.len == 2 && member.ptr[0] == 'c' && member.ptr[1] >= '0' && member.ptr[1] < '0' + n) {
        return vector_type(true, n);
    }
    return (type){TY_ERROR, NULL};
}

static bool loop_variable_named(int i);

// The type of the variable of a loop the cursor is in, named at token `use`,
// read from its header: for a body without braces that doesn't parse yet,
// like `foreach (var p in parts) Use(p.`, which has no loop to look at.
static type loop_variable_type(const scope *sc, const int use)
{
    const str name = DOC->toks[use].text;
    for (int k = use - 1; k >= 2; k--) {
        const token *t = &DOC->toks[k];
        if (t->kind == T_LBRACE || t->kind == T_RBRACE || t->kind == T_SEMI) break; // Out of the statement
        if (t->kind != T_IDENT || !str_eq(t->text, name) || !loop_variable_named(k)) continue;
        if (DOC->toks[k + 1].kind != T_IDENT || !str_eq_c(DOC->toks[k + 1].text, "in")) continue;
        int open = k - 1;
        while (DOC->toks[open].kind != T_LPAREN) open--;
        if (k - open == 2 && DOC->toks[open + 1].kind == T_IDENT) { // foreach (Stats s in
            type written;
            if (builtin_type_named(DOC->toks[open + 1].text, &written)) return written;
            for (int i = 0; i < A.prog->decls.count; i++) {
                const decl *d = A.prog->decls.items[i];
                if ((d->kind != DECL_STRUCT && d->kind != DECL_ENUM) || !str_eq(d->name, DOC->toks[open + 1].text)) continue;
                return (type){d->kind == DECL_STRUCT ? TY_STRUCT : TY_ENUM, (decl *)d};
            }
            return (type){TY_ERROR, NULL};
        }
        // var: an element of what it goes through, written as names: inv.items
        type of = {TY_ERROR, NULL};
        int i = k + 2;
        if (DOC->toks[i].kind == T_IDENT) {
            const param *ignored;
            of = name_type(sc, DOC->toks[i].text, &ignored);
            for (i++; DOC->toks[i].kind == T_DOT && DOC->toks[i + 1].kind == T_IDENT; i += 2) {
                of = member_type(of, DOC->toks[i + 1].text);
            }
        }
        if (of.kind == TY_LIST) return of.decl->fields.items[0].type;
        if (of.kind == TY_GRID) return (type){of.decl->dims == 3 ? TY_INT3 : TY_INT2, NULL}; // A cell's position
        return (type){TY_ERROR, NULL};
    }
    return (type){TY_ERROR, NULL};
}

// A method or function, with its signature, as `name` (its name by default).
static void complete_routine(completion *c, const decl *m, const char *name)
{
    sb detail = {0};
    format_routine(m, &detail);
    sb snippet = {0};
    sb_printf(&snippet, "%s(%s)", name ? name : str_to_cstr(m->name), m->params.count > 0 ? "$1" : "");
    item(c, name ? name : str_to_cstr(m->name), m->owner ? CK_METHOD : CK_FUNCTION, detail.data, NULL, snippet.data);
}

typedef struct builtin_visit {
    completion *c;
} builtin_visit;

static void add_builtin_member(void *user, const builtin_member *m)
{
    completion *c = ((builtin_visit *)user)->c;
    item(c, m->name, m->is_function ? CK_FUNCTION : CK_CONSTANT, m->detail, m->doc, m->snippet);
}

static void list_members(completion *c, const type t, const bool edges, const scope *sc)
{
    if (t.decl && (t.kind == TY_COMPONENT || t.kind == TY_SINGLETON || t.kind == TY_INPUT || t.kind == TY_RECORD
                   || t.kind == TY_STRUCT || t.kind == TY_EVENT)) {
        for (int i = 0; i < t.decl->fields.count; i++) {
            const field *f = &t.decl->fields.items[i];
            if (f->hidden) continue;
            item(c, str_to_cstr(f->name), CK_FIELD, type_name(f->type), button_field_doc(t.decl, f->name), NULL);
        }
        if (t.kind == TY_SINGLETON && t.decl->builtin && str_eq_c(t.decl->name, "Session")) {
            item(c, "room", CK_FIELD, "string",
                 "The code of the room the match is in, like \"K7QF2M\", or \"\" if it's in none. Others join it with "
                 "`Session.Join(code)`.",
                 NULL);
        }
        if (t.kind == TY_EVENT && t.decl->builtin && str_eq_c(t.decl->name, "Disconnected")) {
            item(c, "message", CK_FIELD, "string", "With `Kicked`: why, as the server's `Session.Kick` said. \"\" otherwise.",
                 NULL);
        }
        for (int i = 0; i < t.decl->methods.count; i++) {
            const decl *m = t.decl->methods.items[i];
            if (!m->is_operator && !m->is_interpolate) complete_routine(c, m, NULL);
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
    if (t.kind == TY_LIST) item(c, "Count", CK_PROPERTY, "int", LIST_COUNT_DOC, NULL);
    if (t.kind == TY_GRID) item(c, "size", CK_PROPERTY, t.decl->dims == 3 ? "int3" : "int2", GRID_SIZE_FIELD_DOC, NULL);
    if (t.kind == TY_STRING) {
        item(c, "Length", CK_PROPERTY, "int", TEXT_LENGTH_DOC, NULL);
        builtin_visit v = {c};
        builtin_list_members(str_from("string"), add_builtin_member, &v);
    }
    if (t.kind == TY_RECT) {
        item(c, "x", CK_PROPERTY, "float", "The left side, from the screen's left.", NULL);
        item(c, "y", CK_PROPERTY, "float", "The top side, from the screen's top.", NULL);
        item(c, "width", CK_PROPERTY, "float", NULL, NULL);
        item(c, "height", CK_PROPERTY, "float", NULL, NULL);
    }
    if (t.kind == TY_QUATERNION) item(c, "value", CK_PROPERTY, "float4", "(x, y, z) is the vector part.", NULL);
    const int n = matrix_dim(t);
    for (int i = 0; i < n; i++) {
        static const char *const columns[] = {"c0", "c1", "c2", "c3"};
        item(c, columns[i], CK_PROPERTY, type_name(vector_type(true, n)), "A column.", NULL);
    }
    // Built-in methods: a list's and a grid's, and an entity's or a singleton's
    // where the code can change it.
    const bool local = sc->decl && sc->decl->kind == DECL_SYSTEM && (sc->decl->is_view || sc->decl->is_local);
    const bool match = sc->decl && sc->decl->kind == DECL_SYSTEM && !local;
    const bool changes = (t.kind == TY_ENTITY && match) || (t.kind == TY_LOCAL_ENTITY && local)
                      || (t.kind == TY_SINGLETON && match && !t.decl->is_local);
    if (t.kind == TY_LIST || t.kind == TY_GRID || changes) {
        size_t count;
        const method_form *forms = methods_of(t, &count);
        for (size_t i = 0; i < count; i++) {
            if (t.kind == TY_LOCAL_ENTITY && strcmp(forms[i].name, "Snap") == 0) continue; // Local state isn't blended
            sb detail = {0};
            format_method(&forms[i], t, &detail);
            sb snippet = {0};
            sb_printf(&snippet, "%s(%s)", forms[i].name, strstr(forms[i].form, "()") ? "" : "$1");
            item(c, forms[i].name, CK_METHOD, detail.data, forms[i].doc, snippet.data);
        }
    }
    if (edges && t.kind == TY_BOOL) {
        item(c, "down", CK_PROPERTY, "bool", "True on the tick it became true.", NULL);
        item(c, "up", CK_PROPERTY, "bool", "True on the tick it became false.", NULL);
    }
}

// Whether code here can call Session.Start and the like: views and local
// handlers, as the checker allows.
static bool decides_session(const scope *sc)
{
    return sc->decl && sc->decl->kind == DECL_SYSTEM && (sc->decl->is_view || sc->decl->is_local) && !sc->in_input;
}

// After `a.b.`: resolves the chain of names before the dot.
static bool complete_in_namespace(completion *c, str ns, bool systems);
static const char *name_for(const decl *d);

static void complete_members(completion *c, const int dot, const loc at, const bool systems)
{
    if (dot >= 1 && DOC->toks[dot - 1].kind == T_THIS) { // this.: the entity's methods
        const scope sc = scope_at(at);
        const type t = this_type(&sc);
        if (t.kind != TY_ERROR) list_members(c, t, false, &sc);
        return;
    }
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
    if (t.kind == TY_ERROR) t = loop_variable_type(&sc, ids[n - 1]);
    if (t.kind == TY_ERROR && str_eq_c(base, "Devices")) t = (type){TY_RECORD, A.prog->devices}; // This machine's
    if (t.kind == TY_ERROR) {
        // Session.: its calls, where local code runs
        if (n == 1 && str_eq_c(base, "Session") && decides_session(&sc)) {
            for (size_t i = 0; i < sizeof session_calls / sizeof session_calls[0]; i++) {
                sb snippet = {0};
                sb_printf(&snippet, "%s($1)", session_calls[i].name);
                item(c, session_calls[i].name, CK_FUNCTION, session_calls[i].form, session_calls[i].doc, snippet.data);
            }
            return;
        }
        // Clipboard.: its one call, where local code runs
        if (n == 1 && str_eq_c(base, "Clipboard") && decides_session(&sc)) {
            item(c, "Copy", CK_FUNCTION, CLIPBOARD_COPY_FORM, CLIPBOARD_COPY_DOC, "Copy($1)");
            return;
        }
        // Wait.: what async code waits for
        if (n == 1 && str_eq_c(base, "Wait") && sc.decl && sc.decl->is_async) {
            for (size_t i = 0; i < sizeof wait_calls / sizeof wait_calls[0]; i++) {
                sb snippet = {0};
                sb_printf(&snippet, "%s($1)", wait_calls[i].name);
                item(c, wait_calls[i].name, CK_FUNCTION, wait_calls[i].form, wait_calls[i].doc, snippet.data);
            }
            return;
        }
        if (n == 1 && str_eq_c(base, "Scene") && sc.decl && !sc.in_input) {
            for (size_t i = 0; i < sizeof scene_calls / sizeof scene_calls[0]; i++) {
                sb snippet = {0};
                sb_printf(&snippet, "%s($1)", scene_calls[i].name);
                item(c, scene_calls[i].name, CK_FUNCTION, scene_calls[i].form, scene_calls[i].doc, snippet.data);
            }
            return;
        }
        // Page. or Game.Page.: an enum's members
        sb written = {0};
        for (int k = n - 1; k >= 0; k--) sb_printf(&written, "%s" STR_FMT, k == n - 1 ? "" : ".", STR_ARG(DOC->toks[ids[k]].text));
        for (int i = 0; i < A.prog->decls.count; i++) {
            const decl *d = A.prog->decls.items[i];
            if (d->kind != DECL_ENUM || strcmp(name_for(d), written.data) != 0) continue;
            for (int m = 0; m < d->members.count; m++) {
                char value[32];
                snprintf(value, sizeof value, "%lld", (long long)d->members.items[m].number);
                item(c, str_to_cstr(d->members.items[m].name), CK_ENUM_MEMBER, value, NULL, NULL);
            }
            return;
        }
        // ORIGIN. or Tuning.ORIGIN.: a constant's members
        for (int i = 0; i < A.prog->decls.count; i++) {
            const decl *d = A.prog->decls.items[i];
            if (d->kind != DECL_CONST || (strcmp(name_for(d), written.data) != 0 && !str_eq_c(d->qualified, written.data))) {
                continue;
            }
            list_members(c, d->return_type, false, &sc);
            return;
        }
        if (n == 1 && builtin_owner(base)) {
            const bool frame_owner = str_eq_c(base, "Draw") || str_eq_c(base, "GUI") || str_eq_c(base, "GUILayout")
                                  || str_eq_c(base, "Screen");
            if (frame_owner && !(sc.decl && (sc.decl->is_view || sc.decl->kind == DECL_FUNCTION))) return;
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
        if (components && d->kind == DECL_COMPONENT) item(c, name_for(d), CK_STRUCT, decl_what(d), NULL, NULL);
        if (singletons && d->kind == DECL_SINGLETON) item(c, name_for(d), CK_CLASS, decl_what(d), NULL, NULL);
        if (input && d->kind == DECL_INPUT) item(c, name_for(d), CK_INTERFACE, "input", NULL, NULL);
    }
}

// The components code on one side can spawn and change: local code the local
// world's, match code the match's.
static void complete_components_of(completion *c, const bool local)
{
    for (int i = 0; i < A.prog->decls.count; i++) {
        const decl *d = A.prog->decls.items[i];
        if (d->kind == DECL_COMPONENT && d->is_local == local) item(c, name_for(d), CK_STRUCT, decl_what(d), NULL, NULL);
    }
}

// Events: what handlers handle and Send sends. `builtin` includes the engine's own.
static void complete_events(completion *c, const bool builtin)
{
    for (int i = 0; i < A.prog->decls.count; i++) {
        const decl *d = A.prog->decls.items[i];
        if (d->kind != DECL_EVENT || (d->builtin && !builtin)) continue;
        item(c, name_for(d), CK_EVENT, decl_what(d), d->builtin ? builtin_event_doc(A.prog, d) : NULL, NULL);
    }
}

// The events code on one side can send.
static void complete_events_of(completion *c, const bool local)
{
    for (int i = 0; i < A.prog->decls.count; i++) {
        const decl *d = A.prog->decls.items[i];
        if (d->kind == DECL_EVENT && !d->builtin && d->is_local == local) item(c, name_for(d), CK_EVENT, decl_what(d), NULL, NULL);
    }
}

// Structs: field and local types, and values like Stats { ... }. Enums too.
static void complete_structs(completion *c)
{
    for (int i = 0; i < A.prog->decls.count; i++) {
        const decl *d = A.prog->decls.items[i];
        if (d->kind == DECL_STRUCT) item(c, name_for(d), CK_STRUCT, "struct", NULL, NULL);
        if (d->kind == DECL_ENUM) item(c, name_for(d), CK_ENUM, "enum", NULL, NULL);
    }
}

// The game's constants, where a value goes: of type `only`, or of any with TY_ERROR.
static void complete_constants(completion *c, const type_kind only)
{
    for (int i = 0; i < A.prog->decls.count; i++) {
        const decl *d = A.prog->decls.items[i];
        if (d->kind != DECL_CONST || (only != TY_ERROR && d->return_type.kind != only)) continue;
        sb detail = {0};
        sb_printf(&detail, "const " STR_FMT, STR_ARG(d->return_type_name));
        item(c, name_for(d), CK_CONSTANT, detail.data, NULL, NULL);
    }
}

// In settings { ... } between tokens `open` and `last`: each setting it
// doesn't set yet.
static void complete_settings(completion *c, const int open, const int last)
{
    int count;
    const setting *all = settings_list(&count);
    for (int i = 0; i < count; i++) {
        bool set = false;
        for (int k = open + 1; k <= last && !set; k++) {
            set = DOC->toks[k].kind == T_IDENT && DOC->toks[k + 1].kind == T_ASSIGN && str_eq_c(DOC->toks[k].text, all[i].name);
        }
        if (set) continue;
        sb detail = {0};
        sb_printf(&detail, "%s %s", type_name((type){all[i].kind, NULL}), all[i].name);
        sb snippet = {0};
        sb_printf(&snippet, "%s = $0;", all[i].name);
        item(c, all[i].name, CK_PROPERTY, detail.data, all[i].doc, snippet.data);
    }
}

// Systems or views, for [Before(...)] and [After(...)].
static void complete_systems(completion *c, const bool views)
{
    for (int i = 0; i < A.prog->decls.count; i++) {
        const decl *d = A.prog->decls.items[i];
        if (d->kind == DECL_SYSTEM && d->is_view == views) {
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
        if (!d->unit || !str_eq(d->unit->ns, ns)) continue;
        if (systems != (d->kind == DECL_SYSTEM)) continue;
        const int kind = d->kind == DECL_COMPONENT || d->kind == DECL_STRUCT ? CK_STRUCT : d->kind == DECL_INPUT ? CK_INTERFACE
                       : d->kind == DECL_EVENT ? CK_EVENT : d->kind == DECL_ENUM ? CK_ENUM
                       : d->kind == DECL_SYSTEM ? CK_FUNCTION : d->kind == DECL_CONST ? CK_CONSTANT : CK_CLASS;
        item(c, str_to_cstr(d->name), kind, decl_keyword(d), NULL, NULL);
    }
    return any;
}

static const char *const value_types[] = {
    "bool", "int", "int2", "int3", "int4", "float", "float2", "float3", "float4", "quaternion",
    "float2x2", "float3x3", "float4x4", "Entity", "LocalEntity", "PlayerID", "Color", "Rect",
};

// Which built-in types to offer: each place takes its own.
enum {
    VT_VALUES = 1,       // Numbers, vectors, matrices, PlayerID, Color and Rect: those with constructors
    VT_BOOL = 2,
    VT_ENTITY = 4,
    VT_LOCAL_ENTITY = 8,
    VT_TEXT = 16,        // string
    VT_LIST = 32,
    VT_GRID = 64,        // Fields of components, singletons and scenes only
    VT_LOCALS = VT_VALUES | VT_BOOL | VT_ENTITY | VT_TEXT | VT_LIST, // What a local can be, but a LocalEntity
};

static void complete_value_types(completion *c, const unsigned which)
{
    for (size_t i = 0; i < sizeof value_types / sizeof value_types[0]; i++) {
        type t;
        builtin_type_named(str_from(value_types[i]), &t);
        const unsigned kind = t.kind == TY_BOOL           ? VT_BOOL
                            : t.kind == TY_ENTITY         ? VT_ENTITY
                            : t.kind == TY_LOCAL_ENTITY   ? VT_LOCAL_ENTITY
                                                          : VT_VALUES;
        if (which & kind) item(c, value_types[i], CK_STRUCT, "built-in type", builtin_type_doc(t.kind), NULL);
    }
    if (which & VT_TEXT) item(c, "string", CK_STRUCT, "built-in type", builtin_type_doc(TY_STRING), NULL);
    if (which & VT_LIST) item(c, "List", CK_STRUCT, "List<T>", builtin_type_doc(TY_LIST), "List<$1>");
    if (which & VT_GRID) {
        item(c, "Grid2", CK_STRUCT, "Grid2<T>", builtin_type_doc(TY_GRID), "Grid2<$1>");
        item(c, "Grid3", CK_STRUCT, "Grid3<T>", builtin_type_doc(TY_GRID), "Grid3<$1>");
    }
}

// What a function's or method's parameter can be, as the checker takes it:
// built-in values, text and lists, structs, enums and components; after `mut`,
// what can change. A function's last can be its Action, and an async
// function takes singletons too.
static void complete_routine_param_types(completion *c, const bool after_mut, const bool function, const bool async)
{
    complete_value_types(c, VT_LOCALS | VT_LOCAL_ENTITY);
    complete_structs(c);
    complete_types(c, true, async, false);
    complete_namespaces(c, false);
    if (after_mut) return;
    if (function) item(c, "Action", CK_STRUCT, "Action", builtin_type_doc(TY_ACTION), NULL);
    for (int i = 0; i < A.prog->records.count; i++) { // Devices, Keyboard, ..., Button: read-only
        item(c, str_to_cstr(A.prog->records.items[i]->name), CK_CLASS, "built-in device record", NULL, NULL);
    }
}

// Names and keywords that can start an expression or statement.
static void complete_expression(completion *c, const loc at, const bool statement)
{
    const scope sc = scope_at(at);
    const bool view = sc.decl && sc.decl->is_view;

    if (statement) {
        static const char *const keywords[] = {"if", "else", "return", "var", "mut", "switch", "case", "default", "break",
                                               "while", "for", "foreach", "continue"};
        for (size_t i = 0; i < sizeof keywords / sizeof keywords[0]; i++) item(c, keywords[i], CK_KEYWORD, NULL, NULL, NULL);
        // Every cell of a grid at once, in a system, view or handler that isn't async
        if (sc.decl && sc.decl->kind == DECL_SYSTEM && !sc.decl->is_async) {
            item(c, "parallel", CK_KEYWORD, "parallel (var at in cells) { ... }",
                 "Goes through a grid's cells, or with `by`, its blocks, all at once: each step reads the grid as the loop "
                 "found it and changes only its own cell or block.",
                 NULL);
        }
        // A function that can fail ends with its error
        if (sc.decl && is_routine(sc.decl) && sc.decl->fails.kind != TY_VOID) {
            item(c, "fail", CK_KEYWORD, NULL, "Ends the function with an error.", NULL);
        }
    }
    item(c, "true", CK_KEYWORD, NULL, NULL, NULL);
    item(c, "false", CK_KEYWORD, NULL, NULL, NULL);
    item(c, "null", CK_KEYWORD, NULL, "Nothing, for a T?.", NULL);
    if (sc.decl && is_routine(sc.decl) && sc.decl->fails.kind != TY_VOID) {
        item(c, "try", CK_KEYWORD, NULL, "A call's value, or its error passed on to the caller.", NULL);
    }
    // Async code waits
    if (sc.decl && sc.decl->is_async) {
        item(c, "await", CK_KEYWORD, NULL, "Waits for an async call's value, or for Wait: the task goes on from here.", NULL);
        item(c, "Wait", CK_MODULE, "Ticks, frames and seconds to wait for", NULL, NULL);
    }
    if (!statement) item(c, "default", CK_KEYWORD, NULL, NULL, NULL); // A statement's list has it, for switches
    // this: the entity the code runs for, or whose component a method is called on
    const type entity = this_type(&sc);
    if (entity.kind != TY_ERROR) {
        item(c, "this", CK_KEYWORD, type_name(entity),
             sc.decl->kind == DECL_METHOD ? "The entity whose component this is." : "The entity the code runs for.", NULL);
    }

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
        if (sc.fields_of) {
            for (int i = 0; i < sc.fields_of->fields.count; i++) {
                const field *f = &sc.fields_of->fields.items[i];
                if (!f->hidden) item(c, str_to_cstr(f->name), CK_FIELD, type_name(f->type), NULL, NULL);
            }
            for (int i = 0; i < sc.fields_of->methods.count; i++) {
                const decl *m = sc.fields_of->methods.items[i];
                if (!m->is_operator && !m->is_interpolate) complete_routine(c, m, NULL);
            }
        }
    }
    for (int i = 0; i < A.prog->decls.count; i++) {
        const decl *d = A.prog->decls.items[i];
        if (d->kind == DECL_FUNCTION) complete_routine(c, d, name_for(d));
    }
    complete_constants(c, TY_ERROR);

    const bool routine = sc.decl && (sc.decl->kind == DECL_METHOD || sc.decl->kind == DECL_FUNCTION);
    if (sc.decl && !sc.in_input && !routine) {
        const bool local = view || sc.decl->is_local;
        item(c, "Spawn", CK_FUNCTION, local ? "Spawn(local components...) -> LocalEntity" : "Spawn(components...) -> Entity",
             local ? "Creates a local entity with these components, at the end of the frame."
                   : "Creates an entity with these components.",
             "Spawn($1)");
        item(c, "Send", CK_FUNCTION, "Send(event)",
             local ? "Sends a local event, handled at the end of the frame." : "Sends an event to the whole world, handled at the end of the tick.",
             "Send($1)");
        complete_components_of(c, local);
        complete_events_of(c, local);
        item(c, "Scene", CK_MODULE, "Loads and unloads scenes", NULL, NULL);
    }
    // Session.Start and the like are statements of their own.
    if (statement && decides_session(&sc)) {
        item(c, "Session", CK_MODULE, "Starts, joins and leaves matches", NULL, NULL);
        item(c, "Clipboard", CK_MODULE, "This machine's clipboard", NULL, NULL);
    }
    // Where a statement starts, what a local can be; elsewhere, what makes a value.
    const bool local_code = view || (sc.decl && sc.decl->is_local);
    complete_value_types(c, !statement ? VT_VALUES : local_code ? VT_LOCALS | VT_LOCAL_ENTITY : VT_LOCALS);
    complete_structs(c);
    item(c, "Math", CK_MODULE, "Math functions and constants", NULL, NULL);
    complete_namespaces(c, false);
    // What views draw with, which the functions they call can use too.
    if (view || (routine && sc.decl->kind == DECL_FUNCTION)) {
        item(c, "Draw", CK_MODULE, "Immediate-mode drawing", NULL, NULL);
        item(c, "GUI", CK_MODULE, "Widgets at a Rect", NULL, NULL);
        item(c, "GUILayout", CK_MODULE, "Widgets laid out automatically", NULL, NULL);
        item(c, "Screen", CK_MODULE, "The window's size, in GUI units", NULL, NULL);
    }
    // This machine's devices
    if (view || (routine && sc.decl->kind == DECL_FUNCTION) || (sc.in_input && !sc.in_sanitize)) {
        item(c, "Devices", CK_MODULE, "This machine's keyboard, mouse and gamepad", NULL, NULL);
    }
    // content(): a function's Action
    for (int i = 0; sc.decl && i < sc.decl->params.count; i++) {
        const param *p = &sc.decl->params.items[i];
        if (p->type.kind == TY_ACTION) item(c, str_to_cstr(p->name), CK_FUNCTION, "Action", "Runs the caller's block.", "$0();");
    }
}

static void complete_declarations(completion *c)
{
    item(c, "component", CK_SNIPPET, "component Name { fields }", NULL, "component ${1:Name}\n{\n    $0\n}");
    item(c, "singleton", CK_SNIPPET, "singleton Name { fields }", NULL, "singleton ${1:Name}\n{\n    $0\n}");
    item(c, "struct", CK_SNIPPET, "struct Name { fields }", "A value type for fields and locals.",
         "struct ${1:Name}\n{\n    $0\n}");
    item(c, "enum", CK_SNIPPET, "enum Name { Members }", "A type with named values, like 'enum Page { Title, Options }'.",
         "enum ${1:Name}\n{\n    $0\n}");
    item(c, "system", CK_SNIPPET, "system Name(parameters) { ... }", NULL, "system ${1:Name}($2)\n{\n    $0\n}");
    item(c, "view", CK_SNIPPET, "view Name(parameters) { ... }", "Runs once per frame and draws.",
         "view ${1:Name}($2)\n{\n    $0\n}");
    item(c, "input", CK_SNIPPET, "input Name { fields; Sample }", "What a player sends each tick.",
         "input ${1:Name}\n{\n    $0\n\n    Sample()\n    {\n    }\n}");
    item(c, "event", CK_SNIPPET, "event Name { fields }", "Something that happened, sent with Send.",
         "event ${1:Name}\n{\n    $0\n}");
    item(c, "scene", CK_SNIPPET, "scene Name { fields }", "Entities that load and unload together, like a level.",
         "scene ${1:Name}\n{\n    $0\n}");
    item(c, "local scene", CK_SNIPPET, "local scene Name { fields }", "A scene on this machine only, like a menu.",
         "local scene ${1:Name}\n{\n    $0\n}");
    item(c, "local component", CK_SNIPPET, "local component Name { fields }",
         "This machine's own: views change it, and the match never sees it.", "local component ${1:Name}\n{\n    $0\n}");
    item(c, "local singleton", CK_SNIPPET, "local singleton Name { fields }",
         "This machine's own, like settings or a menu's state.", "local singleton ${1:Name}\n{\n    $0\n}");
    item(c, "local event", CK_SNIPPET, "local event Name { fields }", "Sent from views, handled at the end of the frame.",
         "local event ${1:Name}\n{\n    $0\n}");
    item(c, "event handler", CK_SNIPPET, "event(Event e) Name(parameters) { ... }",
         "Runs when the event is sent, at the end of the tick.", "event(${1:Event} ${2:e}) ${3:Name}($4)\n{\n    $0\n}");
    item(c, "function", CK_SNIPPET, "Type Name(parameters) { ... }", "Code other code calls, like 'float Heal(mut Stats stats)'.",
         "${1:void} ${2:Name}($3)\n{\n    $0\n}");
    item(c, "async function", CK_SNIPPET, "async Type Name(parameters) { ... }",
         "A function that can wait, with 'await': calling it starts a task that goes on by itself.",
         "async ${1:void} ${2:Name}($3)\n{\n    $0\n}");
    item(c, "async event handler", CK_SNIPPET, "async event(Event e) Name(parameters) { ... }",
         "A handler that can wait, with 'await': each event starts a task.",
         "async event(${1:Event} ${2:e}) ${3:Name}($4)\n{\n    $0\n}");
    item(c, "extern", CK_SNIPPET, "extern Type Name(parameters);",
         "A function written in C, which the game's C files or libraries define.", "extern ${1:void} ${2:Name}($3);");
    item(c, "const", CK_SNIPPET, "const Type NAME = value;", "A value code reads by name, the same on every machine.",
         "const ${1:int} ${2:NAME} = $0;");
    item(c, "settings", CK_SNIPPET, "settings { name = value; }", "The engine's settings for the game, like its tickRate.",
         "settings\n{\n    $0\n}");
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

// Whose parameters a header's parentheses hold.
typedef enum header_kind {
    HEADER_NONE,
    HEADER_SYSTEM,  // A system's or view's, or a handler's after its trigger: system Move(mut Body body)
    HEADER_TRIGGER, // A handler's trigger: event(Hit hit)
    HEADER_ROUTINE, // A function's or method's: void Heal(mut Stats stats)
    HEADER_SAMPLE,  // The input's Sample and Sanitize: local singletons
} header_kind;

typedef struct frame {
    context_kind kind;
    int open; // The token that opened it
    header_kind header; // CTX_HEADER's
} frame;

static bool is_word(const token *t);

// The header a '(' at the top level opens, at token `i`.
static header_kind top_header(const int i)
{
    if (i < 1) return HEADER_NONE;
    const token *name = &DOC->toks[i - 1];
    if (name->kind == T_IDENT && str_eq_c(name->text, "event")) return HEADER_TRIGGER;
    if (i < 2) return HEADER_ROUTINE;
    const token *keyword = &DOC->toks[i - 2];
    if (keyword->kind == T_RPAREN || keyword->kind == T_SYSTEM) return HEADER_SYSTEM; // event(Hit hit) TakeHit(
    return keyword->kind == T_IDENT && str_eq_c(keyword->text, "view") ? HEADER_SYSTEM : HEADER_ROUTINE;
}

// The header a '(' in a type's body opens, at token `i`: the input's Sample or
// Sanitize, or a method's (`bool IsDead(`, `Money operator +(`). None for a
// value, like a field's default.
static header_kind member_header(const int i)
{
    if (i < 2) return HEADER_NONE;
    const token *name = &DOC->toks[i - 1];
    const token *before = &DOC->toks[i - 2];
    if (name->kind == T_IDENT && (str_eq_c(name->text, "Sample") || str_eq_c(name->text, "Sanitize"))) return HEADER_SAMPLE;
    if (name->kind == T_IDENT && (before->kind == T_IDENT || before->kind == T_QUESTION || before->kind == T_GT)) {
        return HEADER_ROUTINE;
    }
    return before->kind == T_IDENT && str_eq_c(before->text, "operator") && !is_word(name) ? HEADER_ROUTINE : HEADER_NONE;
}

// Type Name( at column 1, the type maybe qualified: a function, or in a
// type's body, a method.
static bool starts_function(const int i)
{
    const token *t = &DOC->toks[i];
    if (t->kind != T_IDENT || t->at.col != 1) return false;
    int k = i + 1;
    while (DOC->toks[k].kind == T_DOT && DOC->toks[k + 1].kind == T_IDENT) k += 2;
    if (DOC->toks[k].kind == T_QUESTION) k++; // int? Find(
    return DOC->toks[k].kind == T_IDENT && DOC->toks[k + 1].kind == T_LPAREN;
}

// Whether the `{` at token `i` follows `) fails ParseError`: a function's body.
static bool after_fails(const int i)
{
    for (int k = i - 1; k > 0; k--) {
        const token *t = &DOC->toks[k];
        if (t->kind == T_IDENT && str_eq_c(t->text, "fails")) return DOC->toks[k - 1].kind == T_RPAREN;
        if (t->kind != T_IDENT && t->kind != T_DOT && t->kind != T_LT && t->kind != T_GT) return false;
    }
    return false;
}

static bool starts_declaration(const int i)
{
    const token *t = &DOC->toks[i];
    if (t->kind == T_COMPONENT || t->kind == T_SINGLETON || t->kind == T_SYSTEM) return true;
    if (t->kind == T_LBRACKET && t->at.col == 1) return !attributes_before_field(DOC->toks, i); // Attributes
    if (t->kind == T_IDENT && t->at.col == 1 && DOC->toks[i + 1].kind == T_IDENT
        && (str_eq_c(t->text, "input") || str_eq_c(t->text, "view") || str_eq_c(t->text, "struct")
            || str_eq_c(t->text, "event") || str_eq_c(t->text, "enum") || str_eq_c(t->text, "scene")
            || str_eq_c(t->text, "local") || str_eq_c(t->text, "namespace")
            || str_eq_c(t->text, "using") || str_eq_c(t->text, "extern"))) {
        return true;
    }
    if (t->kind == T_IDENT && t->at.col == 1 && str_eq_c(t->text, "local")
        && (DOC->toks[i + 1].kind == T_COMPONENT || DOC->toks[i + 1].kind == T_SINGLETON)) {
        return true;
    }
    if (t->kind == T_IDENT && t->at.col == 1 && DOC->toks[i + 1].kind == T_LPAREN && str_eq_c(t->text, "event")) {
        return true; // event(Hit hit) TakeHit(...)
    }
    if (t->kind == T_IDENT && t->at.col == 1 && DOC->toks[i + 1].kind == T_IDENT && str_eq_c(t->text, "async")) {
        return true; // async void Countdown(), async event(...)
    }
    if (t->kind == T_IDENT && t->at.col == 1 && DOC->toks[i + 1].kind == T_LBRACE && str_eq_c(t->text, "settings")) {
        return true;
    }
    return starts_function(i);
}

static bool is_word(const token *t)
{
    return t->text.len > 0 && (isalpha((unsigned char)t->text.ptr[0]) || t->text.ptr[0] == '_') && t->kind != T_STRING;
}

static bool ends_operand(tok_kind k);

// Whether the name at token `i` is the variable a loop declares, before its
// `in`: foreach (var item, foreach (Stats s, parallel (var at.
static bool loop_variable_named(const int i)
{
    int k = i - 1;
    while (k > 0 && (DOC->toks[k].kind == T_VAR || DOC->toks[k].kind == T_IDENT || DOC->toks[k].kind == T_DOT
                     || DOC->toks[k].kind == T_LT || DOC->toks[k].kind == T_GT || DOC->toks[k].kind == T_QUESTION)) {
        if (DOC->toks[k].kind == T_IDENT && str_eq_c(DOC->toks[k].text, "in")) return false; // Past the name already
        k--;
    }
    return k < i - 1 && k >= 1 && DOC->toks[k].kind == T_LPAREN
        && (DOC->toks[k - 1].kind == T_FOREACH || DOC->toks[k - 1].kind == T_PARALLEL);
}

// The '(' of the parallel loop whose header holds token `last`, or -1.
static int parallel_header(const int last)
{
    int parens = 0;
    for (int k = last; k > 0; k--) {
        const tok_kind kind = DOC->toks[k].kind;
        if (kind == T_RPAREN) parens++;
        else if (kind == T_LPAREN && parens-- == 0) return DOC->toks[k - 1].kind == T_PARALLEL ? k : -1;
        else if (kind == T_SEMI || kind == T_LBRACE || kind == T_RBRACE) return -1;
    }
    return -1;
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
        // In text with values: not in its text, but in its values.
        const bool text_end = at.col == t->at.col + token_len(t) && t->text.ptr[t->text.len - 1] == '"';
        if ((t->kind == T_INTERP || t->kind == T_INTERP_PART) && t->at.line == at.line
            && (at.col < t->at.col + token_len(t) || text_end)) {
            goto done;
        }
        if (inside && t->kind == T_INTERP_FORMAT) goto done;
        if (inside && is_word(t)) last--;
    }

    frame stack[64] = {{CTX_TOP, -1, HEADER_NONE}};
    int depth = 0;
    for (int i = 0; i <= last; i++) {
        if (starts_declaration(i) && !(stack[depth].kind == CTX_DATA && starts_function(i))) {
            depth = 0;
            if (DOC->toks[i].kind != T_LBRACKET) continue;
        }
        const context_kind top = stack[depth].kind;
        const tok_kind kind = DOC->toks[i].kind;
        const tok_kind before = i > 0 ? DOC->toks[i - 1].kind : T_EOF;
        if (kind == T_LBRACKET && top == CTX_TOP && depth < 63) {
            stack[++depth] = (frame){CTX_ATTRIBUTE, i, HEADER_NONE};
        } else if (kind == T_RBRACKET && top == CTX_ATTRIBUTE) {
            depth--;
        } else if (kind == T_LPAREN && top == CTX_ATTRIBUTE && depth < 63) {
            stack[++depth] = (frame){CTX_ATTRIBUTE_ARGS, i, HEADER_NONE};
        } else if (kind == T_RPAREN && top == CTX_ATTRIBUTE_ARGS) {
            depth--;
        } else if (kind == T_LBRACKET && top == CTX_DATA && depth < 63) {
            stack[++depth] = (frame){CTX_FIELD_ATTRIBUTE, i, HEADER_NONE};
        } else if (kind == T_RBRACKET && top == CTX_FIELD_ATTRIBUTE) {
            depth--;
        } else if (kind == T_LPAREN && (top == CTX_FIELD_ATTRIBUTE || top == CTX_FIELD_ATTRIBUTE_ARGS) && depth < 63) {
            stack[++depth] = (frame){CTX_FIELD_ATTRIBUTE_ARGS, i, HEADER_NONE}; // One frame per parenthesis: float2(0, 1)
        } else if (kind == T_RPAREN && top == CTX_FIELD_ATTRIBUTE_ARGS) {
            depth--;
        } else if (kind == T_LBRACE && depth < 63) {
            context_kind next = CTX_CODE;
            if (top == CTX_TOP && before == T_IDENT && !after_fails(i)) next = CTX_DATA;
            else if ((top == CTX_CODE || top == CTX_LITERAL) && before == T_IDENT) next = CTX_LITERAL;
            stack[++depth] = (frame){next, i, HEADER_NONE};
        } else if (kind == T_RBRACE && depth > 0) {
            depth--;
        } else if (kind == T_LPAREN && depth < 63) {
            const header_kind header = top == CTX_TOP ? top_header(i) : top == CTX_DATA ? member_header(i) : HEADER_NONE;
            if (header != HEADER_NONE) stack[++depth] = (frame){CTX_HEADER, i, header};
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
    // int Parse(string text) fails ParseError: the error's type, one a function can return
    if (pk == T_IDENT && str_eq_c(prev->text, "fails") && last >= 1 && DOC->toks[last - 1].kind == T_RPAREN
        && (f.kind == CTX_TOP || f.kind == CTX_DATA)) {
        complete_structs(&c);
        complete_value_types(&c, VT_LOCALS);
        complete_namespaces(&c, false);
        goto done;
    }

    switch (f.kind) {
    case CTX_TOP:
        if (pk == T_EOF || pk == T_RBRACE || pk == T_SEMI || pk == T_RBRACKET) complete_declarations(&c);
        else if (pk == T_IDENT && str_eq_c(prev->text, "using")) complete_namespaces(&c, true);
        else if (pk == T_IDENT && str_eq_c(prev->text, "const")) { // What a constant can be
            complete_value_types(&c, VT_VALUES | VT_BOOL | VT_TEXT);
            complete_structs(&c);
            complete_namespaces(&c, false);
        }
        else if (pk == T_COLON && last >= 2 && DOC->toks[last - 1].kind == T_IDENT && DOC->toks[last - 2].kind == T_IDENT
                 && str_eq_c(DOC->toks[last - 2].text, "enum")) { // What the enum is stored as
            item(&c, "byte", CK_KEYWORD, "One byte: members from 0 to 255", NULL, NULL);
            item(&c, "ushort", CK_KEYWORD, "Two bytes: members from 0 to 65535", NULL, NULL);
            item(&c, "int", CK_KEYWORD, "Four bytes, as an enum is without one", NULL, NULL);
        } else if (pk == T_IDENT && str_eq_c(prev->text, "extern")) { // Its return type
            item(&c, "void", CK_KEYWORD, "Returns nothing", NULL, NULL);
            complete_value_types(&c, VT_LOCALS | VT_LOCAL_ENTITY);
            complete_structs(&c);
            complete_namespaces(&c, false);
        }
        break;

    case CTX_ATTRIBUTE:
        if (pk == T_LBRACKET || pk == T_COMMA) {
            item(&c, "Before", CK_FUNCTION, "[Before(System)]", "This system runs before the ones named.", "Before($1)");
            item(&c, "After", CK_FUNCTION, "[After(System)]", "This system runs after the ones named.", "After($1)");
            item(&c, "NativeName", CK_FUNCTION, "[NativeName(\"c_function\")]",
                 "The C function the extern function after it calls, when its name isn't the function's own.",
                 "NativeName(\"$1\")");
        }
        break;

    case CTX_FIELD_ATTRIBUTE:
        if (pk == T_LBRACKET || pk == T_COMMA) {
            item(&c, "Clamp", CK_FUNCTION, "[Clamp(lo, hi)]", "Keeps the field between lo and hi, before Sanitize runs.",
                 "Clamp($1)");
            item(&c, "Min", CK_FUNCTION, "[Min(x)]", "Keeps the field at least x, before Sanitize runs.", "Min($1)");
            item(&c, "Max", CK_FUNCTION, "[Max(x)]", "Keeps the field at most x, before Sanitize runs.", "Max($1)");
            item(&c, "Snap", CK_FUNCTION, "[Snap]", "Views see the field as it is, not blended between ticks.", "Snap");
        }
        break;

    case CTX_FIELD_ATTRIBUTE_ARGS: // Constant bounds, like a default value
        complete_value_types(&c, VT_VALUES);
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
        // event(Hit hit): the trigger; event(Hit hit) TakeHit(...): a handler's parameters.
        const bool trigger = f.header == HEADER_TRIGGER;
        const bool sample = f.header == HEADER_SAMPLE;
        // extern float Noise(...): what C takes; async void Load(...): a task's
        bool is_extern = false;
        bool is_async = false;
        for (int k = f.open - 1; k >= 0 && DOC->toks[k].at.line == DOC->toks[f.open].at.line; k--) {
            if (DOC->toks[k].kind != T_IDENT || DOC->toks[k].at.col != 1) continue;
            is_extern = str_eq_c(DOC->toks[k].text, "extern");
            is_async = str_eq_c(DOC->toks[k].text, "async");
        }
        if (f.header == HEADER_ROUTINE && !is_extern) {
            // void Heal(mut Stats stats): values, and for a function its Action
            const bool method = depth >= 1 && stack[depth - 1].kind == CTX_DATA;
            const bool operator_ = f.open >= 2 && DOC->toks[f.open - 2].kind == T_IDENT
                                && str_eq_c(DOC->toks[f.open - 2].text, "operator");
            if (pk == T_LPAREN || pk == T_COMMA || pk == T_MUT) {
                if (pk != T_MUT && !operator_) item(&c, "mut", CK_KEYWORD, "The caller's variable itself, which it can change", NULL, NULL);
                complete_routine_param_types(&c, pk == T_MUT, !method, is_async);
            } else if (pk == T_IDENT) {
                complete_param_name(&c, prev->text);
            }
            break;
        }
        if (is_extern) {
            if (pk == T_LPAREN || pk == T_COMMA) {
                item(&c, "in", CK_KEYWORD, "C gets a read-only pointer to the value", NULL, NULL);
                item(&c, "mut", CK_KEYWORD, "C gets a pointer to the caller's variable, which it can change", NULL, NULL);
            }
            if (pk == T_LPAREN || pk == T_COMMA || pk == T_MUT || (pk == T_IDENT && str_eq_c(prev->text, "in"))) {
                complete_value_types(&c, VT_LOCALS | VT_LOCAL_ENTITY);
                complete_structs(&c);
                complete_namespaces(&c, false);
            } else if (pk == T_IDENT) {
                complete_param_name(&c, prev->text);
            }
            break;
        }
        if (sample && str_eq_c(DOC->toks[f.open - 1].text, "Sanitize")) break; // It takes nothing
        if (trigger) {
            if (pk == T_LPAREN) {
                complete_events(&c, true);
                complete_namespaces(&c, false);
            } else if (pk == T_IDENT) {
                complete_param_name(&c, prev->text);
            }
        } else if (sample) { // Sample(Settings settings): local singletons
            if (pk == T_LPAREN || pk == T_COMMA) {
                for (int i = 0; i < A.prog->decls.count; i++) {
                    const decl *d = A.prog->decls.items[i];
                    if (d->kind == DECL_SINGLETON && d->is_local) item(&c, name_for(d), CK_CLASS, decl_what(d), NULL, NULL);
                }
            } else if (pk == T_IDENT) {
                complete_param_name(&c, prev->text);
            }
        } else if (pk == T_WITH || pk == T_WITHOUT) {
            complete_types(&c, true, false, false);
        } else if (pk == T_LPAREN || pk == T_COMMA || pk == T_MUT) {
            if (pk != T_MUT) {
                item(&c, "mut", CK_KEYWORD, "Write access", NULL, NULL);
                item(&c, "with", CK_KEYWORD, "Entities must have this component", NULL, NULL);
                item(&c, "without", CK_KEYWORD, "Entities must not have this component", NULL, NULL);
                item(&c, "Devices", CK_CLASS, "The devices of the entity's owner, or the server's", NULL, NULL);
            }
            complete_types(&c, true, true, pk != T_MUT);
            complete_namespaces(&c, false);
        } else if (pk == T_IDENT) {
            complete_param_name(&c, prev->text);
        }
        break;
    }

    case CTX_DATA: {
        // settings { ... }: the settings it doesn't set yet, and after '=', a constant
        if (f.open >= 1 && DOC->toks[f.open - 1].kind == T_IDENT && str_eq_c(DOC->toks[f.open - 1].text, "settings")) {
            if (pk == T_LBRACE || pk == T_SEMI) {
                complete_settings(&c, f.open, last);
            } else if (pk != T_IDENT || (last >= 1 && DOC->toks[last - 1].kind != T_LBRACE && DOC->toks[last - 1].kind != T_SEMI)) {
                complete_value_types(&c, VT_VALUES);
                complete_constants(&c, TY_ERROR);
                item(&c, "Math", CK_MODULE, "Math functions and constants", NULL, NULL);
            }
            break;
        }
        // What the body belongs to: `struct Name {`, `component Name {`, `enum Name : byte {`, ...
        const token *keyword = f.open >= 2 ? &DOC->toks[f.open - 2] : NULL;
        const bool backed = keyword && keyword->kind == T_COLON && f.open >= 4;
        if (backed) keyword = &DOC->toks[f.open - 4];
        const bool is_struct = keyword && keyword->kind == T_IDENT && str_eq_c(keyword->text, "struct");
        const bool has_methods = is_struct || (keyword && keyword->kind == T_COMPONENT);
        if (keyword && keyword->kind == T_IDENT && str_eq_c(keyword->text, "enum")) {
            // Members are names; after '=', an int, which a constant can give.
            bool value = false;
            for (int k = last; k > f.open && DOC->toks[k].kind != T_COMMA && !value; k--) value = DOC->toks[k].kind == T_ASSIGN;
            if (value) complete_constants(&c, TY_INT);
            break;
        }
        const bool member_start = last >= 1 && (DOC->toks[last - 1].kind == T_LBRACE || DOC->toks[last - 1].kind == T_SEMI
                                                || DOC->toks[last - 1].kind == T_RBRACE);
        if (pk == T_LBRACE || pk == T_SEMI || pk == T_RBRACE) {
            // A field's type: inputs send numbers, and only ECS data keeps grids.
            const bool input = keyword && keyword->kind == T_IDENT && str_eq_c(keyword->text, "input");
            const bool grids = keyword && (keyword->kind == T_COMPONENT || keyword->kind == T_SINGLETON
                                           || (keyword->kind == T_IDENT && str_eq_c(keyword->text, "scene")));
            complete_value_types(&c, (input ? VT_VALUES | VT_BOOL | VT_ENTITY | VT_LOCAL_ENTITY : VT_LOCALS | VT_LOCAL_ENTITY)
                                         | (grids ? VT_GRID : 0));
            complete_structs(&c);
            complete_namespaces(&c, false);
            if (has_methods) {
                item(&c, "void", CK_KEYWORD, "Returns nothing", NULL, NULL);
                item(&c, "mut", CK_KEYWORD, "A method that changes the fields", NULL, NULL);
                item(&c, "method", CK_SNIPPET, "Type Name(parameters) { ... }", "Reads the fields, which are in scope by name.",
                     "${1:void} ${2:Name}($3)\n{\n    $0\n}");
                item(&c, "mut method", CK_SNIPPET, "mut void Name(parameters) { ... }", "Changes the fields.",
                     "mut void ${1:Name}($2)\n{\n    $0\n}");
            }
            if (is_struct) {
                sb snippet = {0};
                const str name = DOC->toks[f.open - 1].text;
                sb_printf(&snippet, STR_FMT " operator ${1:+}(" STR_FMT " a, " STR_FMT " b)\n{\n    return $0;\n}",
                          STR_ARG(name), STR_ARG(name), STR_ARG(name));
                item(&c, "operator", CK_SNIPPET, "Type operator +(Type a, Type b) { ... }", "Lets '+' and the others work on the struct.",
                     snippet.data);
            }
        } else if (pk == T_IDENT && member_start && is_struct) {
            item(&c, "operator", CK_KEYWORD, "Type operator +(Type a, Type b)", NULL, NULL); // After the return type
        } else if (pk != T_IDENT || (last >= 1 && DOC->toks[last - 1].kind != T_LBRACE && DOC->toks[last - 1].kind != T_SEMI
                                   && DOC->toks[last - 1].kind != T_RBRACE)) {
            // A default value: constants only, and a grid field's size.
            complete_value_types(&c, VT_VALUES);
            complete_structs(&c);
            complete_constants(&c, TY_ERROR);
            item(&c, "Math", CK_MODULE, "Math functions and constants", NULL, NULL);
            int first = last; // The field's first token: its type
            while (first > f.open + 1 && DOC->toks[first - 1].kind != T_SEMI && DOC->toks[first - 1].kind != T_LBRACE
                   && DOC->toks[first - 1].kind != T_RBRACE && DOC->toks[first - 1].kind != T_RBRACKET) {
                first--;
            }
            const str type_text = DOC->toks[first].text;
            if (DOC->toks[first].kind == T_IDENT && (str_eq_c(type_text, "Grid2") || str_eq_c(type_text, "Grid3"))) {
                const bool flat = str_eq_c(type_text, "Grid2");
                item(&c, flat ? "Grid2" : "Grid3", CK_FUNCTION, flat ? "Grid2(int width, int height)" : "Grid3(int width, int height, int depth)",
                     GRID_SIZE_DOC, flat ? "Grid2($1)" : "Grid3($1)");
            }
        }
        break;
    }

    case CTX_LITERAL:
        if ((pk == T_LBRACE || pk == T_COMMA) && f.open >= 1) {
            const str name = DOC->toks[f.open - 1].text;
            for (int i = 0; i < A.prog->decls.count; i++) {
                const decl *d = A.prog->decls.items[i];
                if ((d->kind != DECL_COMPONENT && d->kind != DECL_STRUCT && d->kind != DECL_EVENT) || !str_eq(d->name, name)) continue;
                for (int k = 0; k < d->fields.count; k++) {
                    const field *fl = &d->fields.items[k];
                    if (fl->hidden) continue;
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
        if (pk == T_MUT) { // mut var count = 0;, or mut int count = 0;
            const scope sc = scope_at(at);
            const bool local = sc.decl && (sc.decl->is_view || sc.decl->is_local);
            item(&c, "var", CK_KEYWORD, NULL, NULL, NULL);
            complete_value_types(&c, local ? VT_LOCALS | VT_LOCAL_ENTITY : VT_LOCALS);
            complete_structs(&c);
            complete_namespaces(&c, false);
            break;
        }
        // foreach (var item in ...): `in` after the name
        if (pk == T_IDENT && loop_variable_named(last)) {
            item(&c, "in", CK_KEYWORD, NULL, NULL, NULL);
            break;
        }
        // parallel (var at in cells by 2 offset 1): its blocks and where they start
        const int header = parallel_header(last);
        const bool keyword = pk == T_IDENT && (str_eq_c(prev->text, "in") || str_eq_c(prev->text, "by")
                                               || str_eq_c(prev->text, "offset"));
        bool after_in = false;
        for (int k = header + 1; header >= 0 && k < last && !after_in; k++) {
            after_in = DOC->toks[k].kind == T_IDENT && str_eq_c(DOC->toks[k].text, "in");
        }
        if (after_in && !keyword && ends_operand(pk)) {
            bool by = false;
            bool offset = false;
            for (int k = header + 1; k <= last; k++) {
                if (DOC->toks[k].kind != T_IDENT) continue;
                by |= str_eq_c(DOC->toks[k].text, "by");
                offset |= str_eq_c(DOC->toks[k].text, "offset");
            }
            if (!by && !offset) {
                item(&c, "by", CK_KEYWORD, "by 2", "Each step gets a block of cells this size, rather than one cell.", NULL);
            }
            if (!offset) item(&c, "offset", CK_KEYWORD, "offset 1", "Where the blocks start, like 'time.tick % 2'.", NULL);
            break;
        }
        // `Type name` declares a local: no suggestions for the name.
        const tok_kind before = last >= 1 ? DOC->toks[last - 1].kind : T_EOF;
        type ignored;
        const bool statement_start = before == T_LBRACE || before == T_RBRACE || before == T_SEMI || before == T_MUT;
        if (pk == T_IDENT && statement_start && (builtin_type_named(prev->text, &ignored) || str_eq_c(prev->text, "string"))) break;
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
    case OCC_TYPE:
        if (a->backing || b->backing) return a->backing == b->backing && str_eq(a->name, b->name);
        return a->decl ? a->decl == b->decl : !b->decl && a->type.kind == b->type.kind;
    case OCC_SYSTEM: return a->decl == b->decl;
    case OCC_FIELD: return a->field == b->field;
    case OCC_PARAM: return a->param == b->param;
    case OCC_LOCAL: return a->local == b->local;
    case OCC_OWNER: return str_eq(a->owner, b->owner);
    case OCC_FUNCTION:
    case OCC_CONSTANT:
        if (is_routine(a->decl) || is_routine(b->decl)) return a->decl == b->decl;
        return str_eq(a->owner, b->owner) && str_eq(a->name, b->name);
    case OCC_METHOD: // A built-in one is what it's called on's: a list's Add isn't an entity's
        if (is_routine(a->decl) || is_routine(b->decl)) return a->decl == b->decl;
        return str_eq(a->name, b->name) && a->call == b->call && a->object_type.kind == b->object_type.kind;
    case OCC_MEMBER:
    case OCC_NAMESPACE:
    case OCC_ATTRIBUTE:
    case OCC_SETTING: return str_eq(a->name, b->name);
    case OCC_ENUM_MEMBER: return a->decl == b->decl && str_eq(a->name, b->name);
    case OCC_THIS:
    case OCC_CONST: return a->decl == b->decl;
    case OCC_DEFAULT: return false; // A keyword, not a symbol
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
        jb_printf(out, ",\"kind\":%d}", o->declaration || o->write ? HIGHLIGHT_WRITE : HIGHLIGHT_READ);
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
    case OCC_FIELD:
        if (!o->decl || o->decl->builtin) return "Fields of built-in types can't be renamed.";
        return NULL;
    case OCC_ENUM_MEMBER:
        if (!o->decl || o->decl->builtin) return "Members of the engine's enums can't be renamed.";
        return NULL;
    case OCC_SYSTEM:
    case OCC_PARAM:
    case OCC_LOCAL:
    case OCC_NAMESPACE:
    case OCC_CONST:
        return NULL;
    case OCC_METHOD:
    case OCC_FUNCTION:
        if (is_operator_decl(o->decl)) return "Operators are named by their symbol, so they can't be renamed.";
        if (is_routine(o->decl)) return NULL;
        return "Built-in names can't be renamed.";
    case OCC_THIS:
        return "'this' is a keyword, so it can't be renamed.";
    case OCC_SETTING:
        return "Settings are the engine's, so they can't be renamed.";
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
    if (s->kind == S_SWITCH) {
        for (int i = 0; i < s->cases.count; i++) {
            for (int k = 0; k < s->cases.items[i].body.count; k++) {
                if (local_named(s->cases.items[i].body.items[k], name)) return true;
            }
        }
    }
    if (s->kind == S_EXPR) return local_named(s->value->block, name);
    if (s->kind == S_FOREACH || s->kind == S_PARALLEL) return str_eq(s->name, name) || local_named(s->then_stmt, name);
    if (s->kind == S_WHILE || s->kind == S_FOR) return local_named(s->init, name) || local_named(s->then_stmt, name);
    return s->kind == S_IF && (local_named(s->then_stmt, name) || local_named(s->else_stmt, name));
}

// The system, view or input whose body declares `local`.
static const decl *decl_of_local(const stmt *local)
{
    for (int i = 0; i < A.prog->decls.count; i++) {
        const decl *d = A.prog->decls.items[i];
        for (int k = 0; k < d->methods.count; k++) {
            const stmt *body = d->methods.items[k]->body;
            if (body && loc_cmp(body->at, local->at) < 0 && loc_cmp(local->at, body->end) < 0) return d->methods.items[k];
        }
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
        for (int m = 0; m < d->methods.count; m++) {
            const decl *method = d->methods.items[m];
            for (int k = 0; k < method->params.count; k++) {
                if (&method->params.items[k] == p) return method;
            }
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
    // Words the parser reads as keywords where a declaration or a part of one
    // goes, though the lexer reads them as names. The first ones only start
    // declarations, so parameters and locals can still take them.
    static const char *const contextual[] = {"input", "view", "struct", "event", "enum", "scene", "local", "namespace",
                                             "using", "extern", "const", "async", "settings", "operator", "fails", "in",
                                             "by", "offset", "void"};
    enum { DECL_WORDS = 11 };
    static const char *const reserved[] = {"Math", "Draw", "Devices", "Time", "Owner", "Spawn", "Send", "Spawned",
                                           "Destroyed", "PlayerJoined", "PlayerLeft", "Scene", "SceneVisibility",
                                           "GUI", "GUILayout", "Screen", "Anchor", "Action", "Session", "SessionState", "Clipboard",
                                           "DisconnectReason", "Connected", "Disconnected", "Wait", "List", "Grid2", "Grid3",
                                           "string", "Sample", "Sanitize"};
    static char message[160];

    if (name.len == 0 || !(isalpha((unsigned char)name.ptr[0]) || name.ptr[0] == '_')) return "Names start with a letter.";
    for (int i = 0; i < name.len; i++) {
        if (!isalnum((unsigned char)name.ptr[i]) && name.ptr[i] != '_') return "Names use letters, digits and '_' only.";
    }
    // The lexer's own keywords: it reads them as something other than a name.
    const source text = {"", name.ptr, (size_t)name.len, 0};
    const token *word = lex_all(&text);
    if (!word || word[0].kind != T_IDENT) return "That's a keyword.";
    const bool variable = target->kind == OCC_PARAM || target->kind == OCC_LOCAL;
    for (size_t i = variable ? DECL_WORDS : 0; i < sizeof contextual / sizeof contextual[0]; i++) {
        if (str_eq_c(name, contextual[i])) return "That's a keyword.";
    }
    type ignored;
    for (size_t i = 0; i < sizeof reserved / sizeof reserved[0]; i++) {
        if (str_eq_c(name, reserved[i])) return "That name is built into the language.";
    }
    if (builtin_type_named(name, &ignored)) return "That name is built into the language.";
    if (str_starts_with_c(name, "tide_")) return "Names starting with 'tide_' are reserved for generated code.";

    bool clash = false;
    switch (target->kind) {
    case OCC_TYPE:
    case OCC_SYSTEM:
        // Types share one namespace, and systems and views another; a function
        // shares its name with nothing.
        for (int i = 0; i < A.prog->decls.count; i++) {
            const decl *d = A.prog->decls.items[i];
            const bool same_namespace = d->kind == DECL_FUNCTION || (d->kind == DECL_SYSTEM) == (target->kind == OCC_SYSTEM);
            if (d != target->decl && same_namespace && str_eq(d->name, name)) clash = true;
        }
        break;
    case OCC_METHOD:
    case OCC_FUNCTION:
    case OCC_CONST:
        if (target->decl->owner) { // A method: its type's other methods and fields
            const decl *owner = target->decl->owner;
            for (int i = 0; i < owner->methods.count; i++) {
                if (owner->methods.items[i] != target->decl && str_eq(owner->methods.items[i]->name, name)) clash = true;
            }
            for (int i = 0; i < owner->fields.count; i++) {
                if (str_eq(owner->fields.items[i].name, name)) clash = true;
            }
        } else {
            for (int i = 0; i < A.prog->decls.count; i++) {
                const decl *d = A.prog->decls.items[i];
                if (d != target->decl && str_eq(d->name, name)) clash = true;
            }
        }
        break;
    case OCC_FIELD:
        for (int i = 0; i < target->decl->fields.count; i++) {
            const field *f = &target->decl->fields.items[i];
            if (f != target->field && str_eq(f->name, name)) clash = true;
        }
        break;
    case OCC_ENUM_MEMBER:
        for (int i = 0; i < target->decl->members.count; i++) {
            if (str_eq(target->decl->members.items[i].name, name)) clash = true;
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
    case OCC_NAMESPACE: {
        // Another namespace, or a declaration, with the new path: Game.Fight for Game.Combat
        const str old_path = target->name;
        const int parent = old_path.len - last_part(old_path).len;
        sb renamed = {0};
        sb_printf(&renamed, "%.*s" STR_FMT, parent, old_path.ptr, STR_ARG(name));
        const str path = {renamed.data, (int)renamed.len};
        for (int i = 0; i < A.prog->units.count; i++) {
            const str ns = A.prog->units.items[i]->ns;
            if (str_eq(ns, path) || (ns.len > path.len && ns.ptr[path.len] == '.' && memcmp(ns.ptr, path.ptr, (size_t)path.len) == 0)) {
                clash = true;
            }
        }
        for (int i = 0; i < A.prog->decls.count; i++) {
            if (str_eq(A.prog->decls.items[i]->qualified, path)) clash = true;
        }
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
    {"Rect", {"Rect(float x, float y, float width, float height)"}, "From the top left of the screen, y down, in GUI units."},
    {"PlayerID", {"PlayerID(int index)"}, "A player by index, for local play and tests."},
    {"Spawn", {"Spawn(components...)"}, "Creates an entity with these components. It's added at the end of the tick."},
    {"Send", {"Send(event)"}, "Sends an event to the whole world, handled at the end of the tick."},
    {"Grid2", {"Grid2(int width, int height)"}, GRID_SIZE_DOC},
    {"Grid3", {"Grid3(int width, int height, int depth)"}, GRID_SIZE_DOC},
};

// The type of what's before the dot at token `dot`, from what's written
// there: `this`, text, or names like unit.stats. TY_ERROR for anything else.
static type receiver_type(const int dot, const loc at)
{
    if (dot < 1) return (type){TY_ERROR, NULL};
    const token *before = &DOC->toks[dot - 1];
    if (before->kind == T_STRING) return (type){TY_STRING, NULL};
    if ((before->kind == T_INTERP || before->kind == T_INTERP_PART) && before->text.ptr[before->text.len - 1] == '"') {
        return (type){TY_STRING, NULL};
    }
    const scope sc = scope_at(at);
    if (before->kind == T_THIS) return this_type(&sc);
    int first = dot - 1;
    while (first >= 2 && DOC->toks[first - 1].kind == T_DOT && DOC->toks[first - 2].kind == T_IDENT) first -= 2;
    if (DOC->toks[first].kind != T_IDENT) return (type){TY_ERROR, NULL};
    const param *ignored;
    type t = name_type(&sc, DOC->toks[first].text, &ignored);
    if (t.kind == TY_ERROR) t = loop_variable_type(&sc, first);
    for (int k = first + 2; k < dot && t.kind != TY_ERROR; k += 2) t = member_type(t, DOC->toks[k].text);
    return t;
}

// A signature's parameters, from its label as add_signature reads them: each
// a type and a name ("float2 center"), or only a name ("x" in Math.Clamp).
typedef struct label_params {
    str types[16];
    str names[16];
    int count; // -1 for a label that takes any number, like "Spawn(components...)"
} label_params;

static void parse_label(const char *label, label_params *out)
{
    out->count = 0;
    const char *open = strchr(label, '(');
    const char *close = open ? strchr(open, ')') : NULL;
    if (!open || !close) return;
    if (strstr(label, "...")) {
        out->count = -1;
        return;
    }
    for (const char *p = open + 1; p < close && out->count < 16;) {
        const char *end = p;
        while (end < close && *end != ',') end++;
        const char *name = end; // The last word, and the one before it: "mut float value"
        while (name > p && name[-1] != ' ') name--;
        const char *type_end = name > p ? name - 1 : p;
        const char *type_start = type_end;
        while (type_start > p && type_start[-1] != ' ') type_start--;
        out->names[out->count] = (str){name, (int)(end - name)};
        out->types[out->count] = (str){type_start, (int)(type_end - type_start)};
        out->count++;
        p = end < close ? end + 2 : close;
    }
    // A GUI container's block isn't an argument
    if (out->count > 0 && str_eq_c(out->types[out->count - 1], "Block")) out->count--;
}

// Whether a checked call's argument `k` goes to a parameter of type `type_text`:
// the type the call converted it to, if it says, or else one it can take.
static bool param_fits(const str type_text, const expr *call, const int k)
{
    type want;
    if (type_text.len == 0) return true;
    if (str_eq_c(type_text, "string")) want = (type){TY_STRING, NULL};
    else if (!builtin_type_named(type_text, &want)) return true; // Anchor, or a list's element: nothing to tell apart
    if (call->arg_want.count > k) return call->arg_want.items[k].kind == want.kind;
    return type_assignable(want, call->args.items[k]->type);
}

typedef struct label_list {
    VEC(const char *) labels;
} label_list;

static void collect_label(void *user, const char *label, const char *doc)
{
    (void)doc;
    vec_push(((label_list *)user)->labels, label);
}

// The names of the parameters a checked call's arguments go to, in order:
// the game's function's, or the built-in signature the call took. Returns
// how many; 0 for calls with none to show, like Spawn(...).
static int call_param_names(const expr *call, str *names, const int max)
{
    int count = 0;
    if (call->call == CALL_METHOD || call->call == CALL_FUNCTION) {
        for (; count < call->method->params.count && count < max; count++) names[count] = call->method->params.items[count].name;
        return count;
    }
    label_list list = {0};
    switch (call->call) {
    case CALL_BUILTIN: case CALL_DRAW: case CALL_GUI: case CALL_TEXT: {
        const str owner = call->call == CALL_TEXT ? str_from("string") : call->object ? call->object->name : (str){"", 0};
        builtin_signatures(owner, call->name, collect_label, &list);
        break;
    }
    case CALL_CONSTRUCT: case CALL_NEW_GRID:
        for (size_t i = 0; i < sizeof call_forms / sizeof call_forms[0]; i++) {
            if (!str_eq_c(call->name, call_forms[i].name)) continue;
            for (int f = 0; f < 3 && call_forms[i].forms[f]; f++) vec_push(list.labels, call_forms[i].forms[f]);
        }
        break;
    case CALL_SESSION:
        for (size_t i = 0; i < sizeof session_calls / sizeof session_calls[0]; i++) {
            if (str_eq_c(call->name, session_calls[i].name)) vec_push(list.labels, session_calls[i].form);
        }
        break;
    case CALL_LOAD: case CALL_UNLOAD: case CALL_SCENE_PLAYER:
        for (size_t i = 0; i < sizeof scene_calls / sizeof scene_calls[0]; i++) {
            if (str_eq_c(call->name, scene_calls[i].name)) vec_push(list.labels, scene_calls[i].form);
        }
        break;
    case CALL_WAIT:
        for (size_t i = 0; i < sizeof wait_calls / sizeof wait_calls[0]; i++) {
            if (str_eq_c(call->name, wait_calls[i].name)) vec_push(list.labels, wait_calls[i].form);
        }
        break;
    case CALL_CLIPBOARD:
        vec_push(list.labels, CLIPBOARD_COPY_FORM);
        break;
    case CALL_LIST: {
        const method_form *form = find_form(list_calls, sizeof list_calls / sizeof list_calls[0], call->name);
        sb label = {0};
        if (form) format_method(form, call->object->type, &label);
        if (form) vec_push(list.labels, label.data);
        break;
    }
    default:
        break;
    }
    // The signature with as many parameters as the call has arguments, of
    // their types; or one whose last ones it leaves out, like Session.Open().
    for (int exact = 1; exact >= 0; exact--) {
        for (int i = 0; i < list.labels.count; i++) {
            label_params params;
            parse_label(list.labels.items[i], &params);
            bool fits = exact ? params.count == call->args.count : params.count > call->args.count;
            for (int k = 0; fits && k < call->args.count; k++) fits = param_fits(params.types[k], call, k);
            if (!fits) continue;
            for (; count < params.count && count < max; count++) names[count] = params.names[count];
            return count;
        }
    }
    return 0;
}

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
    const bool method = open >= 2 && DOC->toks[open - 2].kind == T_DOT;
    const str owner = method && open >= 3 && DOC->toks[open - 3].kind == T_IDENT ? DOC->toks[open - 3].text : (str){"", 0};

    jbuf list = {0};
    signature_list s = {&list, commas, -1, commas, 0};
    if (owner.len > 0 && builtin_owner(owner)) { // Math.Clamp(, Draw.Circle(, quaternion.Euler(
        builtin_signatures(owner, name, visit_signature, &s);
    } else if (str_eq_c(owner, "Scene")) {
        for (size_t i = 0; i < sizeof scene_calls / sizeof scene_calls[0]; i++) {
            if (str_eq_c(name, scene_calls[i].name)) add_signature(&s, scene_calls[i].form, scene_calls[i].doc);
        }
    } else if (str_eq_c(owner, "Session")) {
        for (size_t i = 0; i < sizeof session_calls / sizeof session_calls[0]; i++) {
            if (str_eq_c(name, session_calls[i].name)) add_signature(&s, session_calls[i].form, session_calls[i].doc);
        }
    } else if (str_eq_c(owner, "Wait")) {
        for (size_t i = 0; i < sizeof wait_calls / sizeof wait_calls[0]; i++) {
            if (str_eq_c(name, wait_calls[i].name)) add_signature(&s, wait_calls[i].form, wait_calls[i].doc);
        }
    } else if (str_eq_c(owner, "Clipboard")) {
        if (str_eq_c(name, "Copy")) add_signature(&s, CLIPBOARD_COPY_FORM, CLIPBOARD_COPY_DOC);
    } else if (method) {
        // What it's called on decides: text's, a list's, a grid's, an entity's or a singleton's.
        // The call's own, once it parses; until then, what the names before the dot are.
        const occurrence *called = occurrence_at(DOC->toks[open - 1].at);
        const type t = called && called->call != CALL_NONE && called->object_type.kind != TY_ERROR
                         ? called->object_type
                         : receiver_type(open - 2, at);
        size_t count;
        const method_form *forms = methods_of(t, &count);
        const method_form *form = find_form(forms, count, name);
        if (t.kind == TY_STRING) {
            builtin_signatures(str_from("string"), name, visit_signature, &s);
        } else if (form) {
            sb label = {0};
            format_method(form, t, &label);
            add_signature(&s, label.data, form->doc);
        }
    } else {
        for (size_t i = 0; i < sizeof call_forms / sizeof call_forms[0]; i++) {
            if (!str_eq_c(name, call_forms[i].name)) continue;
            for (int f = 0; f < 3 && call_forms[i].forms[f]; f++) add_signature(&s, call_forms[i].forms[f], call_forms[i].doc);
        }
    }
    if (s.shown == 0) {
        // The game's methods and functions: the one the call resolved to, or
        // while it's half written, every one with that name.
        const occurrence *o = occurrence_at(DOC->toks[open - 1].at);
        for (int i = 0; i < A.prog->decls.count && !(o && is_routine(o->decl) && i > 0); i++) {
            const decl *d = A.prog->decls.items[i];
            for (int k = -1; k < d->methods.count; k++) {
                const decl *m = k < 0 ? d : d->methods.items[k];
                const bool match = o && is_routine(o->decl) ? m == o->decl
                                                            : is_routine(m) && str_eq(m->name, name) && (m->owner != NULL) == method;
                if (!match) continue;
                sb label = {0};
                format_routine(m, &label);
                add_signature(&s, label.data, NULL);
            }
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
        || k == T_TRUE || k == T_FALSE || k == T_THIS || k == T_DEFAULT || k == T_NULL || k == T_INTERP
        || k == T_INTERP_PART;
}

// Whether the token at `i` ends an operand: `Parse(t)!`'s `!` does too.
static bool ends_operand_at(const int i)
{
    const tok_kind k = DOC->toks[i].kind;
    return ends_operand(k) || (k == T_NOT && i > 0 && ends_operand_at(i - 1));
}

// Whether the `!` at token `i` comes after a value, `Parse(t)!`, rather than
// before one.
static bool is_postfix_bang(const int i)
{
    return DOC->toks[i].kind == T_NOT && i > 0 && ends_operand_at(i - 1);
}

// Whether the `?` at token `i` makes a type a T?, as in `int? best = null;`
// or `int? Find(...)`, rather than starting `?:`.
static bool is_type_question(const int i)
{
    if (DOC->toks[i].kind != T_QUESTION || i == 0 || i + 2 >= DOC->tok_count) return false;
    const tok_kind before = DOC->toks[i - 1].kind;
    if ((before != T_IDENT && before != T_GT) || DOC->toks[i + 1].kind != T_IDENT) return false;
    const tok_kind after = DOC->toks[i + 2].kind;
    if (after == T_ASSIGN || after == T_SEMI || after == T_COMMA || after == T_RPAREN) return true;
    if (after != T_LPAREN) return false;
    // A function's declaration: its type starts the line, maybe after `mut`.
    int k = i - 1;
    while (k >= 2 && DOC->toks[k - 1].kind == T_DOT && DOC->toks[k - 2].kind == T_IDENT) k -= 2;
    return k == 0 || DOC->toks[k - 1].at.line != DOC->toks[k].at.line || DOC->toks[k - 1].kind == T_MUT;
}

// Whether the `{` at token `i` opens an enum's members: `enum Name {` or
// `enum Name : byte {`, whose members go one to a line, none continuing another.
static bool opens_enum(const int i)
{
    int k = i - 1;
    if (k >= 2 && DOC->toks[k].kind == T_IDENT && DOC->toks[k - 1].kind == T_COLON) k -= 2;
    return k >= 1 && DOC->toks[k].kind == T_IDENT && DOC->toks[k - 1].kind == T_IDENT && str_eq_c(DOC->toks[k - 1].text, "enum");
}

// Whether the `default` at token `i` is a switch's label rather than a value:
// `default:`, but not `c ? default : x`.
static bool is_default_label(const int i)
{
    return i + 1 < DOC->tok_count && DOC->toks[i + 1].kind == T_COLON && (i == 0 || DOC->toks[i - 1].kind != T_QUESTION);
}

// Whether the ':' at token `i` ends a switch's label, like `case Page.Title:`,
// rather than being part of `?:`.
static bool is_label_colon(const int i)
{
    for (int k = i - 1; k >= 0; k--) {
        const tok_kind kind = DOC->toks[k].kind;
        if (kind == T_CASE || (kind == T_DEFAULT && is_default_label(k))) return true;
        if (kind == T_QUESTION || kind == T_SEMI || kind == T_LBRACE || kind == T_RBRACE || kind == T_COLON) return false;
    }
    return false;
}

// A type written with another in <>, which the formatter keeps tight: List<int>.
static bool generic_type(const str name)
{
    return str_eq_c(name, "List") || str_eq_c(name, "Grid2") || str_eq_c(name, "Grid3");
}

static bool space_between(const fmt_item *a, const fmt_item *b)
{
    if (a->tok < 0 || b->tok < 0) return true; // Comments
    const tok_kind x = DOC->toks[a->tok].kind;
    const tok_kind y = DOC->toks[b->tok].kind;
    if (y == T_COLON && is_label_colon(b->tok)) return false;
    // $"a {x:F2} b": the values in text sit against its braces
    if (y == T_INTERP_PART || y == T_INTERP_FORMAT) return false;
    if ((x == T_INTERP || x == T_INTERP_PART) && DOC->toks[a->tok].text.ptr[DOC->toks[a->tok].text.len - 1] == '{') return false;
    if (y == T_RPAREN || y == T_RBRACKET || y == T_COMMA || y == T_SEMI || y == T_DOT) return false;
    if (x == T_LPAREN || x == T_LBRACKET || x == T_DOT) return false;
    // Parse(t)! and int?: against what they follow
    if (y == T_NOT && is_postfix_bang(b->tok)) return false;
    if (y == T_QUESTION && is_type_question(b->tok)) return false;
    if (a->unary || (x == T_NOT && !is_postfix_bang(a->tok)) || x == T_TILDE) return false;
    // List<Item>: a type, not a comparison
    if (x == T_IDENT && y == T_LT && generic_type(DOC->toks[a->tok].text)) return false;
    if (x == T_LT && a->tok > 0 && generic_type(DOC->toks[a->tok - 1].text)) return false;
    if (y == T_GT) {
        int k = b->tok - 1;
        while (k > 0 && (DOC->toks[k].kind == T_IDENT || DOC->toks[k].kind == T_DOT)) k--;
        if (k > 0 && DOC->toks[k].kind == T_LT && generic_type(DOC->toks[k - 1].text)) return false;
    }
    // i++ and ++i: against what they change
    if ((y == T_PLUS_PLUS || y == T_MINUS_MINUS) && ends_operand(x)) return false;
    if ((x == T_PLUS_PLUS || x == T_MINUS_MINUS) && !(a->tok > 0 && ends_operand(DOC->toks[a->tok - 1].kind))) return false;
    // `operator +(`: the symbol names the operator, like a method's name.
    const bool operator_name = a->tok > 0 && DOC->toks[a->tok - 1].kind == T_IDENT
                            && str_eq_c(DOC->toks[a->tok - 1].text, "operator");
    if (y == T_LPAREN && operator_name) return false;
    if (y == T_LPAREN || y == T_LBRACKET) return x != T_IDENT; // Calls, but `if (` and `* (`
    return true;
}

// Whether the `{` at token `i` starts a literal, like Body { position = p } or
// Combat.Health { amount = 1 }: it follows a name where an expression goes.
// Declarations have a word before their name (component Body {, input Keys {).
static bool opens_literal(const int i)
{
    int k = i - 1;
    if (k < 0 || DOC->toks[k].kind != T_IDENT || opens_enum(i)) return false; // `enum Voxel : byte {` isn't one
    while (k >= 2 && DOC->toks[k - 1].kind == T_DOT && DOC->toks[k - 2].kind == T_IDENT) k -= 2;
    if (k == 0) return false;
    const tok_kind before = DOC->toks[k - 1].kind;
    return before != T_IDENT && before != T_COMPONENT && before != T_SINGLETON && before != T_SEMI
        && before != T_LBRACE && before != T_RBRACE && before != T_RBRACKET;
}

// Whether the ')' at token `i` ends the header of an if or a loop, whose
// body can be a statement without braces on the next line.
static bool closes_header(const int i)
{
    if (DOC->toks[i].kind != T_RPAREN) return false;
    int parens = 0;
    for (int k = i; k > 0; k--) {
        if (DOC->toks[k].kind == T_RPAREN) {
            parens++;
        } else if (DOC->toks[k].kind == T_LPAREN && --parens == 0) {
            const tok_kind keyword = DOC->toks[k - 1].kind;
            return keyword == T_IF || keyword == T_WHILE || keyword == T_FOR || keyword == T_FOREACH || keyword == T_PARALLEL;
        }
    }
    return false;
}

// How many levels deeper than its braces each token goes for the switch
// sections around it: statements under `case X:` go one level in, the labels
// stay at the switch's level.
static int *case_levels(void)
{
    int *levels = arena_alloc(sizeof(int) * ((size_t)DOC->tok_count + 1));
    // Per open brace: is it a switch's, and has a label started a section in it?
    bool *is_switch = arena_alloc(sizeof(bool) * ((size_t)DOC->tok_count + 1));
    bool *in_section = arena_alloc(sizeof(bool) * ((size_t)DOC->tok_count + 1));
    int depth = 0;
    int sections = 0; // Open braces whose section is running
    for (int i = 0; i < DOC->tok_count; i++) {
        const tok_kind k = DOC->toks[i].kind;
        const bool label = (k == T_CASE || (k == T_DEFAULT && is_default_label(i))) && depth > 0 && is_switch[depth - 1];
        const bool closing = k == T_RBRACE && depth > 0;
        levels[i] = sections - ((label || closing) && depth > 0 && in_section[depth - 1] ? 1 : 0);
        if (label && !in_section[depth - 1]) {
            in_section[depth - 1] = true;
            sections++;
        }
        if (k == T_LBRACE) {
            // switch (...) {: the ')' before it closes the '(' after `switch`.
            bool after_switch = false;
            if (i > 0 && DOC->toks[i - 1].kind == T_RPAREN) {
                int parens = 0;
                for (int j = i - 1; j >= 0; j--) {
                    if (DOC->toks[j].kind == T_RPAREN) parens++;
                    else if (DOC->toks[j].kind == T_LPAREN && --parens == 0) {
                        after_switch = j > 0 && DOC->toks[j - 1].kind == T_SWITCH;
                        break;
                    }
                }
            }
            is_switch[depth] = after_switch;
            in_section[depth] = false;
            depth++;
        } else if (closing) {
            depth--;
            if (in_section[depth]) sections--;
        }
    }
    return levels;
}

// The case_levels of what's at `text`: the first token that starts there or after.
static int case_level_at(const int *levels, const char *text)
{
    for (int i = 0; i < DOC->tok_count; i++) {
        if (token_start(&DOC->toks[i]) >= text) return levels[i];
    }
    return 0;
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

// The formatter's edits, each a line's (or the final line break): those that
// start on lines `first` to `last` (0-based) are written.
typedef struct line_edits {
    jbuf *out;
    int count;
    int first;
    int last;
} line_edits;

static void write_edit(line_edits *edits, const loc start, const loc end, const char *text, const size_t len)
{
    if (start.line - 1 < edits->first || start.line - 1 > edits->last) return;
    if (edits->count++) jb_put(edits->out, ",");
    jb_put(edits->out, "{\"range\":{\"start\":");
    write_position(edits->out, start);
    jb_put(edits->out, ",\"end\":");
    write_position(edits->out, end);
    jb_put(edits->out, "},\"newText\":");
    jb_string_n(edits->out, text, len);
    jb_put(edits->out, "}");
}

const char *analysis_format(int tab_size, const bool insert_spaces, const int first_line, const int last_line, jbuf *out)
{
    if (DOC->lex_errors > 0) return "The file has text Tide can't read. Fix that first.";
    if (tab_size <= 0) tab_size = 4;
    // Each line's indentation depends on the lines before it, so the whole
    // document is formatted, and only the range's lines are changed.
    line_edits edits = {out, 0, first_line, last_line};

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
        const bool unary = t->kind == T_MINUS && (i == 0 || !ends_operand_at(i - 1));
        const fmt_item item = {token_start(t), token_len(t), i, unary};
        vec_push(lines[t->at.line], item);
        p = next + token_len(t);
    }

    int last_content = 0;
    for (int l = 1; l <= line_count; l++) {
        if (lines[l].count > 0 || verbatim[l]) last_content = l;
    }
    const unsigned char *breaks = brace_breaks();
    const int *case_level = case_levels();
    const char *first_break = memchr(DOC->src.text, '\n', DOC->src.len);
    const char *newline = first_break && first_break > DOC->src.text && first_break[-1] == '\r' ? "\r\n" : "\n";

    jb_put(out, "[");
    int depth = 0;
    int parens = 0;
    // `parens` at each brace depth: inside a literal in a call, like
    // Spawn(Body {, only parentheses opened since the brace continue a line.
    int *parens_at = arena_alloc(sizeof(int) * ((size_t)DOC->tok_count + 1));
    bool *enum_at = arena_alloc(sizeof(bool) * ((size_t)DOC->tok_count + 1)); // Each brace depth: an enum's members
    enum_at[0] = false;
    int pending = 0; // Extra indents for the statement after `if (...)` or `else` without braces
    bool blank_before = true; // Drops blank lines at the start of the file
    // The last line with code: did it end a statement or block, or open a braceless if?
    bool statement_open = false;
    bool opened_pending = false;
    bool ended_comma = false; // ...or ended with a comma, which in an enum ends a member
    for (int l = 1; l <= line_count; l++) {
        const str original = line_text(l);
        const fmt_line *items = &lines[l];
        const loc start = {l, 1, A.doc};
        const loc end = {l, original.len + 1, A.doc};

        if (items->count == 0 && !verbatim[l]) {
            // Blank: at most one in a row, none at the start or the end.
            const bool drop = blank_before || l > last_content;
            if (drop && l < DOC->lines.count) {
                write_edit(&edits, start, (loc){l + 1, 1, A.doc}, "", 0);
            } else if (original.len > 0) {
                write_edit(&edits, start, end, "", 0);
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
            for (const fmt_item *item = from; item < to; item++) {
                if (item->tok >= 0) last_tok = item->tok;
            }

            if (!verbatim[l]) {
                if (piece > 0) sb_put(&text, newline);
                const tok_kind first = from->tok >= 0 ? DOC->toks[from->tok].kind : T_EOF;
                int level = depth;
                if (first == T_RBRACE) level--;
                if (first != T_LBRACE) level += pending;
                level += from->tok >= 0 ? case_level[from->tok] : case_level_at(case_level, from->text);
                if (level < 0) level = 0;

                // A line that continues an expression or an argument list goes one
                // level deeper, or keeps its own indentation if that's deeper
                // still: code aligned under an opening parenthesis stays aligned.
                const bool continuation = first != T_LBRACE && first != T_RBRACE && (depth > 0 || parens > 0)
                                       && ((parens > parens_at[depth] && first != T_RPAREN)
                                           || (statement_open && !opened_pending && !(enum_at[depth] && ended_comma)));
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
                case T_LBRACE:
                    parens_at[++depth] = parens;
                    enum_at[depth] = opens_enum(item->tok);
                    break;
                case T_RBRACE: if (depth > 0) depth--; break;
                case T_LPAREN: parens++; break;
                case T_RPAREN: if (parens > 0) parens--; break;
                default: break;
                }
            }
            if (last_tok >= 0) {
                const tok_kind k = DOC->toks[last_tok].kind;
                opened_pending = false;
                const bool label = k == T_COLON && is_label_colon(last_tok);
                if (k == T_SEMI || k == T_LBRACE || k == T_RBRACE || label) {
                    pending = 0;
                } else if ((k == T_RPAREN && parens == 0 && closes_header(last_tok)) || k == T_ELSE) {
                    pending++;
                    opened_pending = true;
                }
                statement_open = k != T_SEMI && k != T_LBRACE && k != T_RBRACE && !label;
                ended_comma = k == T_COMMA;
            }
        }
        if (!verbatim[l] && (text.len != (size_t)original.len || memcmp(text.data, original.ptr, text.len) != 0)) {
            write_edit(&edits, start, end, text.data, text.len);
        }
    }

    // End with exactly one line break.
    if (!final_newline && last_content > 0 && last_content == line_count) {
        const loc end = {line_count, line_text(line_count).len + 1, A.doc};
        write_edit(&edits, end, end, newline, strlen(newline));
    }
    jb_put(out, "]");
    return NULL;
}
