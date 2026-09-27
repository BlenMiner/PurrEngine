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
            fprintf(stderr, "purrc: out of memory\n");
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

void *vec_grow(void *items, const size_t cap, const size_t elem_size)
{
    void *p = realloc(items, cap * elem_size);
    if (!p) {
        fprintf(stderr, "purrc: out of memory\n");
        exit(1);
    }
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
        b->data = vec_grow(b->data, cap, 1);
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

static const source *diag_src;
static int error_count;

void diag_init(const source *src)
{
    diag_src = src;
    error_count = 0;
}

// Prints the offending source line with a caret under the column.
static void print_excerpt(const loc at)
{
    const char *p = diag_src->text;
    const char *end = diag_src->text + diag_src->len;
    if (diag_src->len >= 3 && memcmp(p, "\xEF\xBB\xBF", 3) == 0) p += 3; // Byte-order mark.
    for (int line = 1; line < at.line && p < end; p++) {
        if (*p == '\n') line++;
    }
    const char *line_end = p;
    while (line_end < end && *line_end != '\n' && *line_end != '\r') line_end++;

    fprintf(stderr, "%5d | %.*s\n      | ", at.line, (int)(line_end - p), p);
    for (int i = 0; i < at.col - 1 && p + i < line_end; i++) fputc(p[i] == '\t' ? '\t' : ' ', stderr);
    fputs("^\n", stderr);
}

static void report(const loc at, const char *kind, const char *fmt, const va_list args)
{
    fprintf(stderr, "%s:%d:%d: %s: ", diag_src->path, at.line, at.col, kind);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    print_excerpt(at);
}

void diag_error(const loc at, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    report(at, "error", fmt, args);
    va_end(args);
    error_count++;
}

void diag_warning(const loc at, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    report(at, "warning", fmt, args);
    va_end(args);
}

void diag_note(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    fputs("      = note: ", stderr);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
}

int diag_error_count(void)
{
    return error_count;
}
