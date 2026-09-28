#include "lexer.h"

#include <ctype.h>
#include <string.h>

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

static token *lex_impl(const source *src, const bool tolerant)
{
    lexer lx = {src->text, src->text + src->len, 1, src->text, src->file};
    VEC(token) toks = {0};
    bool ok = true;

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
            t.kind = T_EOF;
            t.text = (str){start, 0};
            vec_push(toks, t);
            break;
        }

        char c = *lx.p;
        char n = lx.p + 1 < lx.end ? lx.p[1] : '\0';

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

        // "text": printable ASCII on one line, with the escapes \" \\ and \n.
        if (c == '"') {
            lx.p++;
            bool closed = false;
            while (lx.p < lx.end && *lx.p != '\n') {
                const char ch = *lx.p;
                if (ch == '"') {
                    closed = true;
                    break;
                }
                if (ch == '\\') {
                    const char esc = lx.p + 1 < lx.end ? lx.p[1] : '\0';
                    if (esc == '\n' || esc == '\0') break;
                    if (esc != '"' && esc != '\\' && esc != 'n') {
                        diag_error(here(&lx), "unknown escape '\\%c'; text can use \\\", \\\\ and \\n", esc);
                        ok = false;
                    }
                    lx.p += 2;
                    continue;
                }
                if ((unsigned char)ch >= 0x80 || ch < ' ') {
                    diag_error(here(&lx), "text can only use printable ASCII characters for now");
                    ok = false;
                }
                lx.p++;
            }
            if (!closed) {
                diag_error(t.at, "text is missing its closing '\"'");
                ok = false;
                continue;
            }
            lx.p++;
            t.kind = T_STRING;
            t.text = (str){start + 1, (int)(lx.p - start) - 2};
            vec_push(toks, t);
            continue;
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
        case '?': t.kind = T_QUESTION; break;
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
        case '+': t.kind = n == '=' ? (len = 2, T_PLUS_ASSIGN) : T_PLUS; break;
        case '-': t.kind = n == '=' ? (len = 2, T_MINUS_ASSIGN) : T_MINUS; break;
        case '*': t.kind = n == '=' ? (len = 2, T_STAR_ASSIGN) : T_STAR; break;
        case '/': t.kind = n == '=' ? (len = 2, T_SLASH_ASSIGN) : T_SLASH; break;
        case '%': t.kind = n == '=' ? (len = 2, T_PERCENT_ASSIGN) : T_PERCENT; break;
        case '^': t.kind = n == '=' ? (len = 2, T_CARET_ASSIGN) : T_CARET; break;
        case '~': t.kind = T_TILDE; break;
        case '&': t.kind = n == '&' ? (len = 2, T_AND) : n == '=' ? (len = 2, T_AMP_ASSIGN) : T_AMP; break;
        case '|': t.kind = n == '|' ? (len = 2, T_OR) : n == '=' ? (len = 2, T_PIPE_ASSIGN) : T_PIPE; break;
        default:
            if ((unsigned char)c >= 0x80) {
                diag_error(t.at, "non-ASCII characters are only allowed in comments");
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
    return lex_impl(src, false);
}

token *lex_all(const source *src)
{
    return lex_impl(src, true);
}

tok_kind compound_op(const tok_kind assign)
{
    switch (assign) {
    case T_PLUS_ASSIGN: return T_PLUS;
    case T_MINUS_ASSIGN: return T_MINUS;
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
