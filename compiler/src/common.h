#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Memory. Everything comes from one arena and is never freed individually.
// tidec runs once and exits; the language server resets the arena before each
// analysis, so nothing the compiler allocates may outlive one.

void *arena_alloc(size_t size); // Zeroed.
void arena_reset(void);         // Frees everything arena_alloc returned.

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
            const int vec_old_cap_ = (v).cap;                                  \
            (v).cap = (v).cap ? (v).cap * 2 : 8;                               \
            (v).items = vec_grow((v).items, (size_t)vec_old_cap_, (size_t)(v).cap, sizeof(*(v).items)); \
        }                                                                      \
        (v).items[(v).count++] = (x);                                          \
    } while (0)

// A bigger copy of `items`, from the arena.
void *vec_grow(void *items, size_t old_cap, size_t new_cap, size_t elem_size);

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
    int file; // Which source file: the index diag_add_source gave it
} loc;

typedef struct source {
    const char *path; // As given on the command line.
    const char *text;
    size_t len;
    int file;         // Set by diag_add_source
    const char *package; // The package the file is in (its name, which is its namespace); NULL for the game's own
} source;

typedef enum diag_severity {
    DIAG_ERROR,
    DIAG_WARNING,
    DIAG_NOTE, // Belongs to the previous error or warning; `at` is zero, or where it points.
} diag_severity;

// Receives diagnostics instead of stderr, for tools such as the language server.
typedef void (*diag_sink)(void *user, diag_severity severity, loc at, const char *message);

// Forgets every source file and error, before compiling a program.
void diag_reset(void);
// Registers a source file and sets src->file: locations in it carry that index.
void diag_add_source(source *src);
const source *diag_source(int file);
int diag_source_count(void);
void diag_set_sink(diag_sink sink, void *user); // NULL prints to stderr again.
void diag_error(loc at, const char *fmt, ...);
void diag_warning(loc at, const char *fmt, ...);
void diag_note(const char *fmt, ...); // Attaches to the previous error or warning.
// A note about another place in the code, which its text names too: a sink
// gets `at`, where diag_note gives it none.
void diag_note_at(loc at, const char *fmt, ...);
int diag_error_count(void);

// "did you mean ...?" for a misspelled name: offer every name that would have
// been right, then suggest_note adds a note naming the closest one, if any is
// close enough (the same letters in another case, or a typo or two away).
typedef struct suggestion {
    str wrong;
    str best;
    int distance;
} suggestion;

suggestion suggest_start(str wrong);
void suggest_consider(suggestion *s, str candidate);
void suggest_consider_c(suggestion *s, const char *candidate);
void suggest_note(const suggestion *s);

// Receives the name each "did you mean" note suggests, after the note, with
// the name written (a slice of the source, where it's the source's): for
// editors, to offer the change. The note belongs to the previous diagnostic.
typedef void (*diag_suggestion_sink)(void *user, str wrong, str best);
void diag_set_suggestion_sink(diag_suggestion_sink sink, void *user); // NULL for none

// The order of a game's files, which decides the default order of its systems:
// byte by byte, with backslashes as forward slashes, so it's the same on every
// platform. Returns like strcmp.
int path_compare(const char *a, const char *b);
