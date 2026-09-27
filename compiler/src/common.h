#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Memory. The compiler is a short-lived process: everything comes from one
// arena and is never freed individually.

void *arena_alloc(size_t size); // Zeroed.

#define NEW(T) ((T *)arena_alloc(sizeof(T)))

// Growable array. Declare with `VEC(T) name;` zero-initialized.
#define VEC(T)                                                                 \
    struct {                                                                   \
        T *items;                                                              \
        int count;                                                             \
        int cap;                                                               \
    }

#define vec_push(v, x)                                                         \
    do {                                                                       \
        if ((v).count == (v).cap) {                                            \
            (v).cap = (v).cap ? (v).cap * 2 : 8;                               \
            (v).items = vec_grow((v).items, (size_t)(v).cap, sizeof(*(v).items)); \
        }                                                                      \
        (v).items[(v).count++] = (x);                                          \
    } while (0)

void *vec_grow(void *items, size_t cap, size_t elem_size);

// ---------------------------------------------------------------------------
// Strings. Slices point into the source buffer or the arena.

typedef struct str {
    const char *ptr;
    int len;
} str;

#define STR_FMT "%.*s"
#define STR_ARG(s) (s).len, (s).ptr

str str_from(const char *cstr);
bool str_eq(str a, str b);
bool str_eq_c(str a, const char *cstr);
bool str_starts_with_c(str a, const char *prefix);
char *str_to_cstr(str s); // Arena copy, NUL-terminated.

// String builder that also counts lines, for #line directives.
typedef struct sb {
    char *data;
    size_t len;
    size_t cap;
    int line; // 1-based line the next character lands on.
} sb;

void sb_put(sb *b, const char *s);
void sb_putn(sb *b, const char *s, size_t n);
void sb_printf(sb *b, const char *fmt, ...);

// ---------------------------------------------------------------------------
// Diagnostics.

typedef struct loc {
    int line;
    int col;
} loc;

typedef struct source {
    const char *path; // As given on the command line.
    const char *text;
    size_t len;
} source;

void diag_init(const source *src);
void diag_error(loc at, const char *fmt, ...);
void diag_warning(loc at, const char *fmt, ...);
void diag_note(const char *fmt, ...); // Attaches to the previous error or warning.
int diag_error_count(void);
