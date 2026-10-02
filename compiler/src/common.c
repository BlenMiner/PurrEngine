#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Memory

typedef struct arena_block {
    struct arena_block *next;
    size_t used;
    size_t cap;
    _Alignas(16) char data[];
} arena_block;

static arena_block *arena_head;

void *arena_alloc(size_t size)
{
    size = (size + 15) & ~(size_t)15;
    if (!arena_head || arena_head->used + size > arena_head->cap) {
        const size_t cap = size > 1u << 20 ? size : 1u << 20;
        arena_block *block = malloc(sizeof(arena_block) + cap);
        if (!block) {
            fprintf(stderr, "tidec: out of memory\n");
            exit(1);
        }
        block->next = arena_head;
        block->used = 0;
        block->cap = cap;
        arena_head = block;
    }
    void *p = arena_head->data + arena_head->used;
    arena_head->used += size;
    memset(p, 0, size);
    return p;
}

void arena_reset(void)
{
    while (arena_head) {
        arena_block *next = arena_head->next;
        free(arena_head);
        arena_head = next;
    }
}

void *vec_grow(void *items, const size_t old_cap, const size_t new_cap, const size_t elem_size)
{
    void *p = arena_alloc(new_cap * elem_size);
    if (items) memcpy(p, items, old_cap * elem_size);
    return p;
}

// ---------------------------------------------------------------------------
// Strings

str str_from(const char *cstr)
{
    return (str){cstr, (int)strlen(cstr)};
}

bool str_eq(const str a, const str b)
{
    return a.len == b.len && memcmp(a.ptr, b.ptr, (size_t)a.len) == 0;
}

bool str_eq_c(const str a, const char *cstr)
{
    return str_eq(a, str_from(cstr));
}

bool str_starts_with_c(const str a, const char *prefix)
{
    const size_t n = strlen(prefix);
    return (size_t)a.len >= n && memcmp(a.ptr, prefix, n) == 0;
}

char *str_to_cstr(const str s)
{
    char *p = arena_alloc((size_t)s.len + 1);
    memcpy(p, s.ptr, (size_t)s.len);
    return p;
}

void sb_putn(sb *b, const char *s, const size_t n)
{
    if (b->line == 0) b->line = 1;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        while (cap < b->len + n + 1) cap *= 2;
        b->data = vec_grow(b->data, b->cap, cap, 1);
        b->cap = cap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n') b->line++;
    }
}

void sb_put(sb *b, const char *s)
{
    sb_putn(b, s, strlen(s));
}

void sb_printf(sb *b, const char *fmt, ...)
{
    char stack[1024];
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(stack, sizeof stack, fmt, args);
    va_end(args);
    if (n < 0) return;
    if ((size_t)n < sizeof stack) {
        sb_putn(b, stack, (size_t)n);
        return;
    }
    char *heap = malloc((size_t)n + 1);
    va_start(args, fmt);
    vsnprintf(heap, (size_t)n + 1, fmt, args);
    va_end(args);
    sb_putn(b, heap, (size_t)n);
    free(heap);
}

// ---------------------------------------------------------------------------
// Diagnostics

#define MAX_SOURCES 1024

static const source *sources[MAX_SOURCES];
static int source_count;
static int error_count;
static diag_sink sink;
static void *sink_user;
static diag_suggestion_sink suggestion_sink;
static void *suggestion_user;

void diag_reset(void)
{
    source_count = 0;
    error_count = 0;
}

void diag_add_source(source *src)
{
    if (source_count == MAX_SOURCES) {
        fprintf(stderr, "tidec: more than %d source files\n", MAX_SOURCES);
        exit(1);
    }
    src->file = source_count;
    sources[source_count++] = src;
}

const source *diag_source(const int file)
{
    return file >= 0 && file < source_count ? sources[file] : NULL;
}

int diag_source_count(void)
{
    return source_count;
}

void diag_set_sink(const diag_sink new_sink, void *user)
{
    sink = new_sink;
    sink_user = user;
}

void diag_set_suggestion_sink(const diag_suggestion_sink new_sink, void *user)
{
    suggestion_sink = new_sink;
    suggestion_user = user;
}

static void to_sink(const diag_severity severity, const loc at, const char *fmt, va_list args)
{
    char message[1024];
    vsnprintf(message, sizeof message, fmt, args);
    sink(sink_user, severity, at, message);
}

// Prints the offending source line with a caret under the column.
static void print_excerpt(const source *src, const loc at)
{
    const char *p = src->text;
    const char *end = src->text + src->len;
    if (src->len >= 3 && memcmp(p, "\xEF\xBB\xBF", 3) == 0) p += 3; // Byte-order mark.
    for (int line = 1; line < at.line && p < end; p++) {
        if (*p == '\n') line++;
    }
    const char *line_end = p;
    while (line_end < end && *line_end != '\n' && *line_end != '\r') line_end++;

    fprintf(stderr, "%5d | %.*s\n      | ", at.line, (int)(line_end - p), p);
    for (int i = 0; i < at.col - 1 && p + i < line_end; i++) fputc(p[i] == '\t' ? '\t' : ' ', stderr);
    fputs("^\n", stderr);
}

// Not const: va_list is an array on some targets, and vfprintf takes it as is.
static void report(const loc at, const char *kind, const char *fmt, va_list args)
{
    const source *src = diag_source(at.file);
    if (src) fprintf(stderr, "%s:%d:%d: %s: ", src->path, at.line, at.col, kind);
    else fprintf(stderr, "tidec: %s: ", kind);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    if (src && at.line > 0) print_excerpt(src, at);
}

void diag_error(const loc at, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    if (sink) to_sink(DIAG_ERROR, at, fmt, args);
    else report(at, "error", fmt, args);
    va_end(args);
    error_count++;
}

void diag_warning(const loc at, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    if (sink) to_sink(DIAG_WARNING, at, fmt, args);
    else report(at, "warning", fmt, args);
    va_end(args);
}

// Not const: va_list is an array on some targets, and vfprintf takes it as is.
static void note(const loc at, const char *fmt, va_list args)
{
    if (sink) {
        to_sink(DIAG_NOTE, at, fmt, args);
    } else {
        fputs("      = note: ", stderr);
        vfprintf(stderr, fmt, args);
        fputc('\n', stderr);
    }
}

void diag_note(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    note((loc){0, 0, 0}, fmt, args);
    va_end(args);
}

void diag_note_at(const loc at, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    note(at, fmt, args);
    va_end(args);
}

int diag_error_count(void)
{
    return error_count;
}

// ---------------------------------------------------------------------------
// Suggestions

suggestion suggest_start(const str wrong)
{
    return (suggestion){wrong, {NULL, 0}, 1 << 30};
}

static char lower(const char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

// Edits (insert, delete, replace, swap two neighbors) between two names,
// ignoring case. Names are short, so the table fits on the stack.
static int distance(const str a, const str b)
{
    enum { MAX = 64 };
    if (a.len >= MAX || b.len >= MAX) return 1 << 30;
    int d[MAX][MAX];
    for (int i = 0; i <= a.len; i++) d[i][0] = i;
    for (int j = 0; j <= b.len; j++) d[0][j] = j;
    for (int i = 1; i <= a.len; i++) {
        for (int j = 1; j <= b.len; j++) {
            const int cost = lower(a.ptr[i - 1]) != lower(b.ptr[j - 1]);
            int best = d[i - 1][j - 1] + cost;
            if (d[i - 1][j] + 1 < best) best = d[i - 1][j] + 1;
            if (d[i][j - 1] + 1 < best) best = d[i][j - 1] + 1;
            if (i > 1 && j > 1 && lower(a.ptr[i - 1]) == lower(b.ptr[j - 2]) && lower(a.ptr[i - 2]) == lower(b.ptr[j - 1])
                && d[i - 2][j - 2] + 1 < best) {
                best = d[i - 2][j - 2] + 1;
            }
            d[i][j] = best;
        }
    }
    return d[a.len][b.len];
}

void suggest_consider(suggestion *s, const str candidate)
{
    if (candidate.len == 0 || str_eq(candidate, s->wrong)) return;
    const int d = distance(s->wrong, candidate);
    if (d < s->distance) {
        s->distance = d;
        s->best = candidate;
    }
}

void suggest_consider_c(suggestion *s, const char *candidate)
{
    suggest_consider(s, str_from(candidate));
}

void suggest_note(const suggestion *s)
{
    // One typo in a short name, two in a longer one. Beyond that it's a guess.
    const int allowed = s->wrong.len <= 4 ? 1 : 2;
    if (s->best.len == 0 || s->distance > allowed) return;
    diag_note("did you mean '" STR_FMT "'?", STR_ARG(s->best));
    if (suggestion_sink) suggestion_sink(suggestion_user, s->wrong, s->best);
}

int path_compare(const char *a, const char *b)
{
    for (;; a++, b++) {
        const char x = *a == '\\' ? '/' : *a;
        const char y = *b == '\\' ? '/' : *b;
        if (x != y || x == '\0') return (unsigned char)x - (unsigned char)y;
    }
}
