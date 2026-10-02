#include "lexer.h"

#include <ctype.h>
#include <string.h>

#include "version.h"

static const struct {
    const char *text;
    tok_kind kind;
} keywords[] = {
    {"component", T_COMPONENT},
    {"singleton", T_SINGLETON},
    {"system", T_SYSTEM},
    {"mut", T_MUT},
    {"var", T_VAR},
    {"with", T_WITH},
    {"without", T_WITHOUT},
    {"if", T_IF},
    {"else", T_ELSE},
    {"return", T_RETURN},
    {"true", T_TRUE},
    {"false", T_FALSE},
    {"switch", T_SWITCH},
    {"case", T_CASE},
    {"default", T_DEFAULT},
    {"break", T_BREAK},
    {"while", T_WHILE},
    {"for", T_FOR},
    {"foreach", T_FOREACH},
    {"parallel", T_PARALLEL},
    {"continue", T_CONTINUE},
    {"this", T_THIS},
    {"fail", T_FAIL},
    {"try", T_TRY},
    {"await", T_AWAIT},
    {"is", T_IS},
    {"null", T_NULL},
};

typedef struct lexer {
    const char *p;
    const char *end;
    int line;
    const char *line_start;
    int file;
} lexer;

static loc here(const lexer *lx)
{
    return (loc){lx->line, (int)(lx->p - lx->line_start) + 1, lx->file};
}

// A line ends at "\n", or "\r\n" in files saved on Windows.
static bool is_line_end(const char *p, const char *end)
{
    return *p == '\n' || (*p == '\r' && p + 1 < end && p[1] == '\n');
}

static bool is_ident_start(const char c)
{
    return isalpha((unsigned char)c) || c == '_';
}

static bool is_ident_char(const char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

// Skips whitespace and comments. Returns false on an unterminated block comment.
static bool skip_trivia(lexer *lx)
{
    while (lx->p < lx->end) {
        const char c = *lx->p;
        if (c == '\n') {
            lx->p++;
            lx->line++;
            lx->line_start = lx->p;
        } else if (c == ' ' || c == '\t' || c == '\r') {
            lx->p++;
        } else if (c == '/' && lx->p + 1 < lx->end && lx->p[1] == '/') {
            while (lx->p < lx->end && *lx->p != '\n') lx->p++;
        } else if (c == '/' && lx->p + 1 < lx->end && lx->p[1] == '*') {
            const loc start = here(lx);
            lx->p += 2;
            while (lx->p < lx->end && !(*lx->p == '*' && lx->p + 1 < lx->end && lx->p[1] == '/')) {
                if (*lx->p == '\n') {
                    lx->line++;
                    lx->line_start = lx->p + 1;
                }
                lx->p++;
            }
            if (lx->p >= lx->end) {
                diag_error(start, "unterminated comment");
                return false;
            }
            lx->p += 2;
        } else {
            break;
        }
    }
    return true;
}

static bool is_digit_in_base(const char c, const int base)
{
    if (base == 2) return c == '0' || c == '1';
    if (base == 16) return isxdigit((unsigned char)c);
    return isdigit((unsigned char)c);
}

// Digits with optional `_` separators, as in C#: a run of underscores is only
// consumed when a digit follows it, so `1_` stops before the underscore and is
// then rejected as an invalid number. `leading` allows `_` before the first
// digit, which C# permits right after a 0x or 0b prefix.
static void lex_digits(lexer *lx, const int base, const bool leading)
{
    bool any = false;
    while (lx->p < lx->end) {
        if (is_digit_in_base(*lx->p, base)) {
            lx->p++;
            any = true;
            continue;
        }
        if (*lx->p != '_' || (!any && !leading)) break;
        const char *q = lx->p;
        while (q < lx->end && *q == '_') q++;
        if (q >= lx->end || !is_digit_in_base(*q, base)) break;
        lx->p = q;
    }
}

// Numbers: 12, 1.5, 1e3, 2.5e-3, with an optional f suffix meaning float.
// Integers can also be hex (0x1F) or binary (0b1010). `_` separates digits.
static tok_kind lex_number(lexer *lx)
{
    if (*lx->p == '0' && lx->p + 1 < lx->end) {
        const char prefix = lx->p[1];
        const bool hex = prefix == 'x' || prefix == 'X';
        const bool binary = prefix == 'b' || prefix == 'B';
        if (hex || binary) {
            lx->p += 2;
            lex_digits(lx, hex ? 16 : 2, true);
            return T_INT;
        }
    }

    bool is_float = false;
    lex_digits(lx, 10, false);
    if (lx->p + 1 < lx->end && *lx->p == '.' && isdigit((unsigned char)lx->p[1])) {
        is_float = true;
        lx->p++;
        lex_digits(lx, 10, false);
    }
    if (lx->p < lx->end && (*lx->p == 'e' || *lx->p == 'E')) {
        const char *save = lx->p;
        lx->p++;
        if (lx->p < lx->end && (*lx->p == '+' || *lx->p == '-')) lx->p++;
        if (lx->p < lx->end && isdigit((unsigned char)*lx->p)) {
            is_float = true;
            lex_digits(lx, 10, false);
        } else {
            lx->p = save;
        }
    }
    if (lx->p < lx->end && (*lx->p == 'f' || *lx->p == 'F')) {
        is_float = true;
        lx->p++;
    }
    return is_float ? T_FLOAT : T_INT;
}

// Reads text up to its closing quote or, in text with values, the `{` that
// starts one, and stands after it. Returns '"' or '{', or 0 after an error.
// `start` is where the token starts, for the error about a missing quote.
static char lex_text(lexer *lx, const bool values, const loc start)
{
    while (lx->p < lx->end && !is_line_end(lx->p, lx->end)) {
        const unsigned char ch = (unsigned char)*lx->p;
        if (ch == '"') {
            lx->p++;
            return '"';
        }
        if (values && ch == '{') {
            if (lx->p + 1 < lx->end && lx->p[1] == '{') { // {{ is a brace
                lx->p += 2;
                continue;
            }
            lx->p++;
            return '{';
        }
        if (values && ch == '}') {
            if (lx->p + 1 < lx->end && lx->p[1] == '}') {
                lx->p += 2;
                continue;
            }
            diag_error(here(lx), "a '}' in text is written '}}'");
            lx->p++;
            return 0;
        }
        if (ch == '\\') {
            const char esc = lx->p + 1 < lx->end ? lx->p[1] : '\0';
            if (esc == '\0' || is_line_end(lx->p + 1, lx->end)) break;
            if (esc != '"' && esc != '\\' && esc != 'n') {
                diag_error(here(lx), "unknown escape '\\%c'; text can use \\\", \\\\ and \\n", esc);
                lx->p += 2;
                return 0;
            }
            lx->p += 2;
            continue;
        }
        if (ch < ' ' || ch == 0x7F) {
            diag_error(here(lx), "text can't hold control characters; write a new line as \\n");
            lx->p++;
            return 0;
        }
        if (ch >= 0x80) { // UTF-8: a lead byte and its continuation bytes
            const int more = ch >= 0xF0 && ch < 0xF5 ? 3 : ch >= 0xE0 ? 2 : ch >= 0xC2 && ch < 0xE0 ? 1 : -1;
            bool valid = more > 0 && ch < 0xF5;
            for (int i = 1; valid && i <= more; i++) {
                valid = lx->p + i < lx->end && ((unsigned char)lx->p[i] & 0xC0u) == 0x80u;
            }
            if (!valid) {
                diag_error(here(lx), "text must be UTF-8, and this isn't");
                lx->p++;
                return 0;
            }
            lx->p += 1 + more;
            continue;
        }
        lx->p++;
    }
    diag_error(start, "text is missing its closing '\"'");
    return 0;
}

// The values open in text with values: their braces and parentheses, so a
// `}` of their own doesn't end them and a `:` inside parentheses isn't a format.
typedef struct text_value {
    int braces;
    int parens;
} text_value;

// ---------------------------------------------------------------------------
// Conditional compilation: #if, #elif, #else and #endif, as in C#. A
// directive is a line of its own, and the lines it leaves out aren't read as
// code, only looked through for the directives that end them, so they can
// hold anything (code for another version of tide).

// An #if whose #endif hasn't come yet.
typedef struct cond {
    loc at;       // The #if
    bool taken;   // One of its branches was read, so the rest are left out
    bool in_else; // Past its #else
} cond;

typedef VEC(cond) cond_stack;

static void add_span(lex_spans *spans, const int first, const int last, const bool directive)
{
    if (!spans || last < first) return;
    const lex_span span = {first, last, directive};
    vec_push(*spans, span);
}

// Whether only spaces come before lx->p on its line.
static bool starts_line(const lexer *lx)
{
    for (const char *p = lx->line_start; p < lx->p; p++) {
        if (*p != ' ' && *p != '\t') return false;
    }
    return true;
}

// A directive's condition, read from `p` to `end` on the lexer's line.
typedef struct cond_reader {
    const char *p;
    const char *end;
    const lexer *lx;
    bool ok;
} cond_reader;

static loc cond_here(const cond_reader *c)
{
    return (loc){c->lx->line, (int)(c->p - c->lx->line_start) + 1, c->lx->file};
}

static void cond_space(cond_reader *c)
{
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\r')) c->p++;
}

// TIDE_0_3_OR_NEWER, or TIDE_0_3_1_OR_NEWER: true in that version of tide and
// newer ones. Its numbers into `v`.
static bool version_symbol(const str name, int v[3])
{
    const char *p = name.ptr + 5;
    const char *end = name.ptr + name.len;
    if (name.len < 5 || memcmp(name.ptr, "TIDE_", 5) != 0) return false;
    v[0] = v[1] = v[2] = 0;
    for (int part = 0; part < 3; part++) {
        if (p >= end || !isdigit((unsigned char)*p)) return part >= 2;
        int n = 0;
        while (p < end && isdigit((unsigned char)*p) && n < 100000) n = n * 10 + (*p++ - '0');
        v[part] = n;
        if (end - p == 9 && memcmp(p, "_OR_NEWER", 9) == 0) return part >= 1;
        if (p >= end || *p != '_') return false;
        p++;
    }
    return false;
}

static bool cond_or(cond_reader *c);

static bool cond_primary(cond_reader *c)
{
    cond_space(c);
    if (c->p >= c->end) {
        if (c->ok) {
            diag_error(cond_here(c), "a condition is missing here");
            diag_note("conditions are tide's versions, like '#if TIDE_0_3_OR_NEWER', with !, && and ||");
        }
        c->ok = false;
        return false;
    }
    if (*c->p == '!' && !(c->p + 1 < c->end && c->p[1] == '=')) {
        c->p++;
        return !cond_primary(c);
    }
    if (*c->p == '(') {
        const loc open = cond_here(c);
        c->p++;
        const bool value = cond_or(c);
        cond_space(c);
        if (c->p < c->end && *c->p == ')') {
            c->p++;
        } else if (c->ok) {
            diag_error(open, "this '(' is missing its ')'");
            c->ok = false;
        }
        return value;
    }
    if (is_ident_start(*c->p)) {
        const loc at = cond_here(c);
        const char *start = c->p;
        while (c->p < c->end && is_ident_char(*c->p)) c->p++;
        const str name = {start, (int)(c->p - start)};
        if (str_eq_c(name, "true")) return true;
        if (str_eq_c(name, "false")) return false;
        int v[3];
        if (version_symbol(name, v)) return tide_version_at_least(v[0], v[1], v[2]);
        if (c->ok) {
            diag_error(at, "'" STR_FMT "' isn't a symbol Tide knows", STR_ARG(name));
            if (name.len > 5 && memcmp(name.ptr, "TIDE_", 5) == 0) {
                diag_note("a version of tide is written like TIDE_0_3_OR_NEWER, true in tide 0.3 and newer");
            } else {
                diag_note("conditions are tide's versions, like TIDE_0_3_OR_NEWER, true in tide 0.3 and newer");
            }
        }
        c->ok = false;
        return false;
    }
    if (c->ok) {
        diag_error(cond_here(c), "'%c' can't go in a condition", *c->p);
        diag_note("conditions are tide's versions, like '#if TIDE_0_3_OR_NEWER', with !, &&, ||, == and !=");
    }
    c->ok = false;
    c->p = c->end;
    return false;
}

// == and !=, both sides read whatever the first says, as with && and ||.
static bool cond_equality(cond_reader *c)
{
    bool value = cond_primary(c);
    for (;;) {
        cond_space(c);
        const bool eq = c->p + 1 < c->end && c->p[0] == '=' && c->p[1] == '=';
        const bool ne = c->p + 1 < c->end && c->p[0] == '!' && c->p[1] == '=';
        if (!eq && !ne) return value;
        c->p += 2;
        const bool other = cond_primary(c);
        value = eq ? value == other : value != other;
    }
}

static bool cond_and(cond_reader *c)
{
    bool value = cond_equality(c);
    for (;;) {
        cond_space(c);
        if (!(c->p + 1 < c->end && c->p[0] == '&' && c->p[1] == '&')) return value;
        c->p += 2;
        const bool other = cond_equality(c);
        value = value && other;
    }
}

static bool cond_or(cond_reader *c)
{
    bool value = cond_and(c);
    for (;;) {
        cond_space(c);
        if (!(c->p + 1 < c->end && c->p[0] == '|' && c->p[1] == '|')) return value;
        c->p += 2;
        const bool other = cond_and(c);
        value = value || other;
    }
}

// The condition of #if or #elif, which is all that's left of its line.
static bool read_condition(cond_reader *c)
{
    const bool value = cond_or(c);
    cond_space(c);
    if (c->p < c->end && c->ok) {
        diag_error(cond_here(c), "the condition ends before '%.*s'", (int)(c->end - c->p), c->p);
        c->ok = false;
    }
    return value;
}

// The directive word after the '#' at `p`, and where what follows it starts.
static str directive_word(const char *p, const char *end, const char **after)
{
    p++;
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    const char *word = p;
    while (p < end && is_ident_char(*p)) p++;
    *after = p;
    return (str){word, (int)(p - word)};
}

// Leaves out the lines after a directive, up to the #elif, #else or #endif
// of its #if, and stands at that directive's '#'; or at the end of the file,
// for lex_impl to say the #if never ended.
static void skip_branch(lexer *lx, lex_spans *spans)
{
    while (lx->p < lx->end && *lx->p != '\n') lx->p++;
    if (lx->p < lx->end) {
        lx->p++;
        lx->line++;
        lx->line_start = lx->p;
    }
    const int first = lx->line;
    int depth = 0; // #ifs inside what's left out
    while (lx->p < lx->end) {
        const char *q = lx->p;
        while (q < lx->end && (*q == ' ' || *q == '\t')) q++;
        if (q < lx->end && *q == '#') {
            const char *after;
            const str word = directive_word(q, lx->end, &after);
            const bool ends = str_eq_c(word, "endif");
            if (str_eq_c(word, "if")) {
                depth++;
            } else if ((ends || str_eq_c(word, "elif") || str_eq_c(word, "else")) && depth == 0) {
                add_span(spans, first, lx->line - 1, false);
                lx->p = q;
                return;
            } else if (ends) {
                depth--;
            }
        }
        while (lx->p < lx->end && *lx->p != '\n') lx->p++;
        if (lx->p < lx->end) {
            lx->p++;
            lx->line++;
            lx->line_start = lx->p;
        }
    }
    add_span(spans, first, lx->line, false);
}

// Reads the directive at lx->p (its '#'), to the end of its line, and leaves
// out what it says to. False after reporting an error.
static bool directive(lexer *lx, cond_stack *conds, lex_spans *spans)
{
    const loc at = here(lx);
    const char *line_end = lx->p;
    while (line_end < lx->end && *line_end != '\n') line_end++;
    // A comment can follow it.
    const char *text_end = line_end;
    for (const char *q = lx->p; q + 1 < line_end; q++) {
        if (q[0] == '/' && q[1] == '/') {
            text_end = q;
            break;
        }
    }
    const char *after;
    const str word = directive_word(lx->p, text_end, &after);
    add_span(spans, at.line, at.line, true);
    cond_reader c = {after, text_end, lx, true};
    cond *top = conds->count > 0 ? &conds->items[conds->count - 1] : NULL;
    bool skip = false;
    bool ok = true;
    if (str_eq_c(word, "if")) {
        const bool value = read_condition(&c);
        const cond pushed = {at, value, false};
        vec_push(*conds, pushed);
        skip = !value;
    } else if (str_eq_c(word, "elif") || str_eq_c(word, "else")) {
        const bool is_else = str_eq_c(word, "else");
        if (!top) {
            diag_error(at, "'#" STR_FMT "' has no #if before it", STR_ARG(word));
            lx->p = line_end;
            return false;
        }
        if (top->in_else) {
            diag_error(at, "'#" STR_FMT "' after #else: the #else comes last, before #endif", STR_ARG(word));
            diag_note_at(top->at, "the #if it belongs to");
            ok = false;
        }
        bool value = true;
        if (is_else) {
            top->in_else = true;
            cond_space(&c);
            if (c.p < c.end) {
                diag_error(cond_here(&c), "#else takes no condition; for one, write #elif");
                ok = false;
            }
        } else {
            value = read_condition(&c);
        }
        skip = top->taken || !value;
        top->taken = top->taken || value;
    } else if (str_eq_c(word, "endif")) {
        if (!top) {
            diag_error(at, "'#endif' has no #if before it");
            lx->p = line_end;
            return false;
        }
        cond_space(&c);
        if (c.p < c.end) {
            diag_error(cond_here(&c), "#endif takes nothing after it");
            ok = false;
        }
        conds->count--;
    } else {
        diag_error(at, "'#" STR_FMT "' isn't a directive", STR_ARG(word));
        diag_note("Tide's directives are #if, #elif, #else and #endif, as in '#if TIDE_0_3_OR_NEWER'");
        lx->p = line_end;
        return false;
    }
    lx->p = line_end;
    if (skip) skip_branch(lx, spans);
    return ok && c.ok;
}

static token *lex_impl(const source *src, const bool tolerant, lex_spans *spans)
{
    lexer lx = {src->text, src->text + src->len, 1, src->text, src->file};
    VEC(token) toks = {0};
    bool ok = true;
    VEC(text_value) values = {0}; // Innermost last
    cond_stack conds = {0};       // #ifs not ended yet, innermost last

    // Editors on Windows often start UTF-8 files with a byte-order mark.
    if (src->len >= 3 && memcmp(src->text, "\xEF\xBB\xBF", 3) == 0) {
        lx.p += 3;
        lx.line_start = lx.p;
    }

    for (;;) {
        if (!skip_trivia(&lx)) {
            ok = false;
            const token eof = {T_EOF, {lx.end, 0}, here(&lx)};
            vec_push(toks, eof);
            break;
        }
        token t = {0};
        t.at = here(&lx);
        const char *start = lx.p;

        if (lx.p >= lx.end) {
            if (values.count > 0) {
                diag_error(t.at, "text with a value in it is missing its closing '}' and quote");
                ok = false;
            }
            for (int i = 0; i < conds.count; i++) {
                diag_error(conds.items[i].at, "this #if is missing its #endif");
                ok = false;
            }
            t.kind = T_EOF;
            t.text = (str){start, 0};
            vec_push(toks, t);
            break;
        }

        char c = *lx.p;
        char n = lx.p + 1 < lx.end ? lx.p[1] : '\0';

        if (c == '#' && values.count == 0 && starts_line(&lx)) {
            if (!directive(&lx, &conds, spans)) ok = false;
            continue;
        }

        if (is_ident_start(c)) {
            while (lx.p < lx.end && is_ident_char(*lx.p)) lx.p++;
            t.kind = T_IDENT;
            t.text = (str){start, (int)(lx.p - start)};
            for (size_t i = 0; i < sizeof keywords / sizeof keywords[0]; i++) {
                if (str_eq_c(t.text, keywords[i].text)) t.kind = keywords[i].kind;
            }
            vec_push(toks, t);
            continue;
        }

        if (isdigit((unsigned char)c)) {
            t.kind = lex_number(&lx);
            t.text = (str){start, (int)(lx.p - start)};
            // "0x" or "0b" with no digits after it.
            if (t.text.len == 2 && t.text.ptr[0] == '0' && strchr("xXbB", t.text.ptr[1])) {
                diag_error(t.at, "'" STR_FMT "' needs digits after it", STR_ARG(t.text));
                ok = false;
                continue;
            }
            if (lx.p < lx.end && is_ident_char(*lx.p)) {
                diag_error(t.at, "invalid number '%.*s'", (int)(lx.p - start) + 1, start);
                ok = false;
                lx.p++;
                continue;
            }
            vec_push(toks, t);
            continue;
        }

        // "text": UTF-8 on one line, with the escapes \" \\ and \n.
        if (c == '"') {
            lx.p++;
            if (lex_text(&lx, false, t.at) != '"') {
                ok = false;
                continue;
            }
            t.kind = T_STRING;
            t.text = (str){start + 1, (int)(lx.p - start) - 2};
            vec_push(toks, t);
            continue;
        }

        // $"text with {values}": the text up to the first value, whose tokens follow.
        if (c == '$' && n == '"') {
            lx.p += 2;
            const char ended = lex_text(&lx, true, t.at);
            if (!ended) {
                ok = false;
                continue;
            }
            t.kind = T_INTERP;
            t.text = (str){start, (int)(lx.p - start)};
            vec_push(toks, t);
            if (ended == '{') {
                const text_value v = {0, 0};
                vec_push(values, v);
            }
            continue;
        }

        // In a value in text: `}` ends it, and `:` starts its format.
        if (values.count > 0) {
            text_value *v = &values.items[values.count - 1];
            if (c == '}' && v->braces == 0) {
                lx.p++;
                const char ended = lex_text(&lx, true, t.at);
                if (!ended) {
                    ok = false;
                    values.count--;
                    continue;
                }
                t.kind = T_INTERP_PART;
                t.text = (str){start, (int)(lx.p - start)};
                vec_push(toks, t);
                if (ended == '"') values.count--;
                continue;
            }
            if (c == ':' && v->braces == 0 && v->parens == 0) {
                while (lx.p < lx.end && *lx.p != '}' && *lx.p != '"' && !is_line_end(lx.p, lx.end)) lx.p++;
                t.kind = T_INTERP_FORMAT;
                t.text = (str){start, (int)(lx.p - start)};
                vec_push(toks, t);
                continue;
            }
            if (c == '{') v->braces++;
            if (c == '}') v->braces--;
            if (c == '(' || c == '[') v->parens++;
            if (c == ')' || c == ']') v->parens--;
        }

        int len = 1;
        switch (c) {
        case '{': t.kind = T_LBRACE; break;
        case '}': t.kind = T_RBRACE; break;
        case '(': t.kind = T_LPAREN; break;
        case ')': t.kind = T_RPAREN; break;
        case '[': t.kind = T_LBRACKET; break;
        case ']': t.kind = T_RBRACKET; break;
        case ';': t.kind = T_SEMI; break;
        case ',': t.kind = T_COMMA; break;
        case '.': t.kind = T_DOT; break;
        case '?': t.kind = n == '?' ? (len = 2, T_COALESCE) : T_QUESTION; break;
        case ':': t.kind = T_COLON; break;
        case '=': t.kind = n == '=' ? (len = 2, T_EQ) : T_ASSIGN; break;
        case '!': t.kind = n == '=' ? (len = 2, T_NE) : T_NOT; break;
        case '<':
        case '>': {
            bool less = c == '<';
            char nn = lx.p + 2 < lx.end ? lx.p[2] : '\0';
            if (n == c && nn == '=') {
                t.kind = less ? T_SHL_ASSIGN : T_SHR_ASSIGN;
                len = 3;
            } else if (n == c) {
                t.kind = less ? T_SHL : T_SHR;
                len = 2;
            } else if (n == '=') {
                t.kind = less ? T_LE : T_GE;
                len = 2;
            } else {
                t.kind = less ? T_LT : T_GT;
            }
            break;
        }
        case '+': t.kind = n == '=' ? (len = 2, T_PLUS_ASSIGN) : n == '+' ? (len = 2, T_PLUS_PLUS) : T_PLUS; break;
        case '-': t.kind = n == '=' ? (len = 2, T_MINUS_ASSIGN) : n == '-' ? (len = 2, T_MINUS_MINUS) : T_MINUS; break;
        case '*': t.kind = n == '=' ? (len = 2, T_STAR_ASSIGN) : T_STAR; break;
        case '/': t.kind = n == '=' ? (len = 2, T_SLASH_ASSIGN) : T_SLASH; break;
        case '%': t.kind = n == '=' ? (len = 2, T_PERCENT_ASSIGN) : T_PERCENT; break;
        case '^': t.kind = n == '=' ? (len = 2, T_CARET_ASSIGN) : T_CARET; break;
        case '~': t.kind = T_TILDE; break;
        case '&': t.kind = n == '&' ? (len = 2, T_AND) : n == '=' ? (len = 2, T_AMP_ASSIGN) : T_AMP; break;
        case '|': t.kind = n == '|' ? (len = 2, T_OR) : n == '=' ? (len = 2, T_PIPE_ASSIGN) : T_PIPE; break;
        default:
            if ((unsigned char)c >= 0x80) {
                diag_error(t.at, "non-ASCII characters are only allowed in comments and text");
                // Skip the rest of the UTF-8 sequence so one character is one error.
                lx.p++;
                while (lx.p < lx.end && ((unsigned char)*lx.p & 0xC0) == 0x80) lx.p++;
            } else {
                diag_error(t.at, "unexpected character '%c'", c);
                lx.p++;
            }
            ok = false;
            continue;
        }
        lx.p += len;
        t.text = (str){start, len};
        vec_push(toks, t);
    }

    return ok || tolerant ? toks.items : NULL;
}

token *lex(const source *src)
{
    return lex_impl(src, false, NULL);
}

token *lex_all(const source *src)
{
    return lex_impl(src, true, NULL);
}

token *lex_all_spans(const source *src, lex_spans *spans)
{
    return lex_impl(src, true, spans);
}

tok_kind compound_op(const tok_kind assign)
{
    switch (assign) {
    case T_PLUS_ASSIGN:
    case T_PLUS_PLUS: return T_PLUS;
    case T_MINUS_ASSIGN:
    case T_MINUS_MINUS: return T_MINUS;
    case T_STAR_ASSIGN: return T_STAR;
    case T_SLASH_ASSIGN: return T_SLASH;
    case T_PERCENT_ASSIGN: return T_PERCENT;
    case T_SHL_ASSIGN: return T_SHL;
    case T_SHR_ASSIGN: return T_SHR;
    case T_AMP_ASSIGN: return T_AMP;
    case T_PIPE_ASSIGN: return T_PIPE;
    case T_CARET_ASSIGN: return T_CARET;
    default: return assign;
    }
}

const char *tok_kind_name(const tok_kind kind)
{
    switch (kind) {
    case T_EOF: return "end of file";
    case T_IDENT: return "identifier";
    case T_INT: return "integer";
    case T_FLOAT: return "number";
    case T_STRING: return "text";
    case T_COMPONENT: return "'component'";
    case T_SINGLETON: return "'singleton'";
    case T_SYSTEM: return "'system'";
    case T_MUT: return "'mut'";
    case T_VAR: return "'var'";
    case T_WITH: return "'with'";
    case T_WITHOUT: return "'without'";
    case T_IF: return "'if'";
    case T_ELSE: return "'else'";
    case T_RETURN: return "'return'";
    case T_TRUE: return "'true'";
    case T_FALSE: return "'false'";
    case T_SWITCH: return "'switch'";
    case T_CASE: return "'case'";
    case T_DEFAULT: return "'default'";
    case T_BREAK: return "'break'";
    case T_WHILE: return "'while'";
    case T_FOR: return "'for'";
    case T_FOREACH: return "'foreach'";
    case T_PARALLEL: return "'parallel'";
    case T_CONTINUE: return "'continue'";
    case T_THIS: return "'this'";
    case T_FAIL: return "'fail'";
    case T_TRY: return "'try'";
    case T_AWAIT: return "'await'";
    case T_IS: return "'is'";
    case T_NULL: return "'null'";
    case T_COALESCE: return "'?\?'";
    case T_PLUS_PLUS: return "'++'";
    case T_INTERP: return "text";
    case T_INTERP_PART: return "'}' after a value in text";
    case T_INTERP_FORMAT: return "a format";
    case T_MINUS_MINUS: return "'--'";
    case T_LBRACE: return "'{'";
    case T_RBRACE: return "'}'";
    case T_LPAREN: return "'('";
    case T_RPAREN: return "')'";
    case T_LBRACKET: return "'['";
    case T_RBRACKET: return "']'";
    case T_SEMI: return "';'";
    case T_COMMA: return "','";
    case T_DOT: return "'.'";
    case T_QUESTION: return "'?'";
    case T_COLON: return "':'";
    case T_ASSIGN: return "'='";
    case T_PLUS_ASSIGN: return "'+='";
    case T_MINUS_ASSIGN: return "'-='";
    case T_STAR_ASSIGN: return "'*='";
    case T_SLASH_ASSIGN: return "'/='";
    case T_PERCENT_ASSIGN: return "'%='";
    case T_SHL_ASSIGN: return "'<<='";
    case T_SHR_ASSIGN: return "'>>='";
    case T_AMP_ASSIGN: return "'&='";
    case T_PIPE_ASSIGN: return "'|='";
    case T_CARET_ASSIGN: return "'^='";
    case T_EQ: return "'=='";
    case T_NE: return "'!='";
    case T_LT: return "'<'";
    case T_LE: return "'<='";
    case T_GT: return "'>'";
    case T_GE: return "'>='";
    case T_PLUS: return "'+'";
    case T_MINUS: return "'-'";
    case T_STAR: return "'*'";
    case T_SLASH: return "'/'";
    case T_PERCENT: return "'%'";
    case T_SHL: return "'<<'";
    case T_SHR: return "'>>'";
    case T_AMP: return "'&'";
    case T_PIPE: return "'|'";
    case T_CARET: return "'^'";
    case T_TILDE: return "'~'";
    case T_NOT: return "'!'";
    case T_AND: return "'&&'";
    case T_OR: return "'||'";
    }
    return "token";
}
