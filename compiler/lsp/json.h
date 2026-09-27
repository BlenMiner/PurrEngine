#pragma once

#include <stdbool.h>
#include <stddef.h>

// Just enough JSON for the language server protocol: a reader that builds a
// tree, and a writer that appends to a growable buffer.
//
// Both use malloc, never the compiler's arena: the arena is reset before each
// analysis, in the middle of handling a message.

typedef enum json_kind {
    JSON_NULL,
    JSON_FALSE,
    JSON_TRUE,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT,
} json_kind;

typedef struct json {
    json_kind kind;
    double number;
    const char *string; // JSON_STRING: UTF-8, NUL-terminated
    size_t string_len;
    int count;          // JSON_ARRAY and JSON_OBJECT
    struct json **items;
    const char **keys;  // JSON_OBJECT
} json;

// Parses one JSON value. NULL if the text isn't valid JSON. The tree lives
// until json_release.
json *json_parse(const char *text, size_t len);
void json_release(void);

// NULL if `object` isn't an object or has no such key.
const json *json_get(const json *object, const char *key);
// Follows a path of keys: json_path(msg, "params", "textDocument", "uri", NULL).
const json *json_path(const json *root, ...);
// NULL if `v` isn't a string.
const char *json_str(const json *v);
int json_int(const json *v, int fallback);

// ---------------------------------------------------------------------------

typedef struct jbuf {
    char *data;
    size_t len;
    size_t cap;
} jbuf;

void jb_putn(jbuf *b, const char *s, size_t n);
void jb_put(jbuf *b, const char *s);
void jb_printf(jbuf *b, const char *fmt, ...);
// A JSON string literal, with quotes and escapes.
void jb_string(jbuf *b, const char *s);
void jb_string_n(jbuf *b, const char *s, size_t n);
// Writes `v` back out as JSON (for echoing request ids).
void jb_json(jbuf *b, const json *v);
void jb_free(jbuf *b);
