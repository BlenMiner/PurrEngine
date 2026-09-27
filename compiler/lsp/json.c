#include "json.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Memory: a pool of blocks, released all at once after each message.

typedef struct pool_block {
    struct pool_block *next;
    size_t used;
    size_t cap;
    _Alignas(16) char data[];
} pool_block;

static pool_block *pool;

static void *pool_alloc(size_t size)
{
    size = (size + 15) & ~(size_t)15;
    if (!pool || pool->used + size > pool->cap) {
        const size_t cap = size > 65536 ? size : 65536;
        pool_block *block = malloc(sizeof(pool_block) + cap);
        if (!block) {
            fprintf(stderr, "purrls: out of memory\n");
            exit(1);
        }
        block->next = pool;
        block->used = 0;
        block->cap = cap;
        pool = block;
    }
    void *p = pool->data + pool->used;
    pool->used += size;
    memset(p, 0, size);
    return p;
}

void json_release(void)
{
    while (pool) {
        pool_block *next = pool->next;
        free(pool);
        pool = next;
    }
}

// ---------------------------------------------------------------------------
// Reader

typedef struct reader {
    const char *p;
    const char *end;
    int depth;
} reader;

static void skip_space(reader *r)
{
    while (r->p < r->end && (*r->p == ' ' || *r->p == '\t' || *r->p == '\n' || *r->p == '\r')) r->p++;
}

static bool literal(reader *r, const char *word)
{
    const size_t n = strlen(word);
    if ((size_t)(r->end - r->p) < n || memcmp(r->p, word, n) != 0) return false;
    r->p += n;
    return true;
}

static int hex_digit(const char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool read_hex4(reader *r, uint32_t *out)
{
    if (r->end - r->p < 4) return false;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        const int d = hex_digit(r->p[i]);
        if (d < 0) return false;
        v = v * 16 + (uint32_t)d;
    }
    r->p += 4;
    *out = v;
    return true;
}

static size_t put_utf8(char *out, const uint32_t cp)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

// After the opening quote. The decoded text is never longer than the escaped one.
static bool read_string(reader *r, const char **out, size_t *out_len)
{
    const char *start = r->p;
    while (r->p < r->end && *r->p != '"') r->p += *r->p == '\\' ? 2 : 1;
    if (r->p >= r->end) return false;
    char *s = pool_alloc((size_t)(r->p - start) + 1);
    size_t n = 0;
    reader in = {start, r->p, 0};
    while (in.p < in.end) {
        const char c = *in.p++;
        if (c != '\\') {
            s[n++] = c;
            continue;
        }
        if (in.p >= in.end) return false;
        const char e = *in.p++;
        switch (e) {
        case '"': s[n++] = '"'; break;
        case '\\': s[n++] = '\\'; break;
        case '/': s[n++] = '/'; break;
        case 'b': s[n++] = '\b'; break;
        case 'f': s[n++] = '\f'; break;
        case 'n': s[n++] = '\n'; break;
        case 'r': s[n++] = '\r'; break;
        case 't': s[n++] = '\t'; break;
        case 'u': {
            uint32_t cp;
            if (!read_hex4(&in, &cp)) return false;
            if (cp >= 0xD800 && cp < 0xDC00 && in.end - in.p >= 6 && in.p[0] == '\\' && in.p[1] == 'u') {
                in.p += 2;
                uint32_t low;
                if (!read_hex4(&in, &low)) return false;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            }
            n += put_utf8(s + n, cp);
            break;
        }
        default: return false;
        }
    }
    s[n] = '\0';
    r->p++; // Closing quote
    *out = s;
    *out_len = n;
    return true;
}

static json *read_value(reader *r);

static json *read_container(reader *r, const bool object)
{
    json *v = pool_alloc(sizeof(json));
    v->kind = object ? JSON_OBJECT : JSON_ARRAY;
    int cap = 0;
    const char close = object ? '}' : ']';
    skip_space(r);
    if (r->p < r->end && *r->p == close) {
        r->p++;
        return v;
    }
    for (;;) {
        const char *key = NULL;
        if (object) {
            skip_space(r);
            size_t key_len;
            if (r->p >= r->end || *r->p != '"') return NULL;
            r->p++;
            if (!read_string(r, &key, &key_len)) return NULL;
            skip_space(r);
            if (r->p >= r->end || *r->p != ':') return NULL;
            r->p++;
        }
        json *item = read_value(r);
        if (!item) return NULL;
        if (v->count == cap) {
            cap = cap ? cap * 2 : 8;
            json **items = pool_alloc(sizeof(json *) * (size_t)cap);
            if (v->count) memcpy(items, v->items, sizeof(json *) * (size_t)v->count);
            v->items = items;
            if (object) {
                const char **keys = pool_alloc(sizeof(char *) * (size_t)cap);
                if (v->count) memcpy(keys, v->keys, sizeof(char *) * (size_t)v->count);
                v->keys = keys;
            }
        }
        if (object) v->keys[v->count] = key;
        v->items[v->count++] = item;
        skip_space(r);
        if (r->p < r->end && *r->p == ',') {
            r->p++;
            continue;
        }
        if (r->p < r->end && *r->p == close) {
            r->p++;
            return v;
        }
        return NULL;
    }
}

static json *read_value(reader *r)
{
    if (++r->depth > 200) return NULL;
    skip_space(r);
    if (r->p >= r->end) return NULL;
    json *v = NULL;
    const char c = *r->p;
    if (c == '{' || c == '[') {
        r->p++;
        v = read_container(r, c == '{');
    } else if (c == '"') {
        r->p++;
        v = pool_alloc(sizeof(json));
        v->kind = JSON_STRING;
        if (!read_string(r, &v->string, &v->string_len)) v = NULL;
    } else if (literal(r, "true")) {
        v = pool_alloc(sizeof(json));
        v->kind = JSON_TRUE;
    } else if (literal(r, "false")) {
        v = pool_alloc(sizeof(json));
        v->kind = JSON_FALSE;
    } else if (literal(r, "null")) {
        v = pool_alloc(sizeof(json));
        v->kind = JSON_NULL;
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        char buf[64];
        size_t n = 0;
        while (r->p < r->end && n + 1 < sizeof buf && strchr("+-0123456789.eE", *r->p)) buf[n++] = *r->p++;
        buf[n] = '\0';
        char *end;
        const double d = strtod(buf, &end);
        if (end == buf) return NULL;
        v = pool_alloc(sizeof(json));
        v->kind = JSON_NUMBER;
        v->number = d;
    }
    r->depth--;
    return v;
}

json *json_parse(const char *text, const size_t len)
{
    reader r = {text, text + len, 0};
    json *v = read_value(&r);
    if (!v) return NULL;
    skip_space(&r);
    return r.p == r.end ? v : NULL;
}

const json *json_get(const json *object, const char *key)
{
    if (!object || object->kind != JSON_OBJECT) return NULL;
    for (int i = 0; i < object->count; i++) {
        if (strcmp(object->keys[i], key) == 0) return object->items[i];
    }
    return NULL;
}

const json *json_path(const json *root, ...)
{
    va_list args;
    va_start(args, root);
    const json *v = root;
    for (const char *key = va_arg(args, const char *); key && v; key = va_arg(args, const char *)) {
        v = json_get(v, key);
    }
    va_end(args);
    return v;
}

const char *json_str(const json *v)
{
    return v && v->kind == JSON_STRING ? v->string : NULL;
}

int json_int(const json *v, const int fallback)
{
    return v && v->kind == JSON_NUMBER ? (int)v->number : fallback;
}

// ---------------------------------------------------------------------------
// Writer

void jb_putn(jbuf *b, const char *s, const size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 1024;
        while (cap < b->len + n + 1) cap *= 2;
        char *data = realloc(b->data, cap);
        if (!data) {
            fprintf(stderr, "purrls: out of memory\n");
            exit(1);
        }
        b->data = data;
        b->cap = cap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void jb_put(jbuf *b, const char *s)
{
    jb_putn(b, s, strlen(s));
}

void jb_printf(jbuf *b, const char *fmt, ...)
{
    char stack[512];
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(stack, sizeof stack, fmt, args);
    va_end(args);
    if (n < 0) return;
    if ((size_t)n < sizeof stack) {
        jb_putn(b, stack, (size_t)n);
        return;
    }
    char *heap = malloc((size_t)n + 1);
    if (!heap) return;
    va_start(args, fmt);
    vsnprintf(heap, (size_t)n + 1, fmt, args);
    va_end(args);
    jb_putn(b, heap, (size_t)n);
    free(heap);
}

void jb_string_n(jbuf *b, const char *s, const size_t n)
{
    jb_put(b, "\"");
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': jb_put(b, "\\\""); break;
        case '\\': jb_put(b, "\\\\"); break;
        case '\n': jb_put(b, "\\n"); break;
        case '\r': jb_put(b, "\\r"); break;
        case '\t': jb_put(b, "\\t"); break;
        default:
            if (c < 0x20) jb_printf(b, "\\u%04x", c);
            else jb_putn(b, (const char *)&s[i], 1);
        }
    }
    jb_put(b, "\"");
}

void jb_string(jbuf *b, const char *s)
{
    jb_string_n(b, s, strlen(s));
}

void jb_json(jbuf *b, const json *v)
{
    if (!v) {
        jb_put(b, "null");
        return;
    }
    switch (v->kind) {
    case JSON_NULL: jb_put(b, "null"); break;
    case JSON_FALSE: jb_put(b, "false"); break;
    case JSON_TRUE: jb_put(b, "true"); break;
    case JSON_NUMBER: jb_printf(b, "%.17g", v->number); break;
    case JSON_STRING: jb_string_n(b, v->string, v->string_len); break;
    case JSON_ARRAY:
    case JSON_OBJECT:
        jb_put(b, v->kind == JSON_ARRAY ? "[" : "{");
        for (int i = 0; i < v->count; i++) {
            if (i) jb_put(b, ",");
            if (v->kind == JSON_OBJECT) {
                jb_string(b, v->keys[i]);
                jb_put(b, ":");
            }
            jb_json(b, v->items[i]);
        }
        jb_put(b, v->kind == JSON_ARRAY ? "]" : "}");
        break;
    }
}

void jb_free(jbuf *b)
{
    free(b->data);
    *b = (jbuf){0};
}
