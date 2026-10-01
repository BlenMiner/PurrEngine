// JSON, as much as the relay's messages need (see rtc.h): reading by looking
// values up where they are in the text, with no tree and no allocation, and
// writing into a buffer.

#include "rtc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *skip_space(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

// Past the value at p: its end, or NULL if it isn't one.
static const char *skip_value(const char *p, const char *end, const int depth)
{
    p = skip_space(p, end);
    if (p >= end || depth > 32) return NULL;
    if (*p == '"') {
        for (p++; p < end; p++) {
            if (*p == '\\') p++;
            else if (*p == '"') return p + 1;
        }
        return NULL;
    }
    if (*p == '{' || *p == '[') {
        const char close = *p == '{' ? '}' : ']';
        const bool object = *p == '{';
        p = skip_space(p + 1, end);
        if (p < end && *p == close) return p + 1;
        for (;;) {
            if (object) {
                p = skip_value(p, end, depth + 1); // The key
                p = p ? skip_space(p, end) : NULL;
                if (!p || p >= end || *p != ':') return NULL;
                p++;
            }
            p = skip_value(p, end, depth + 1);
            p = p ? skip_space(p, end) : NULL;
            if (!p || p >= end) return NULL;
            if (*p == close) return p + 1;
            if (*p != ',') return NULL;
            p++;
        }
    }
    // A number, true, false or null
    const char *start = p;
    while (p < end && (strchr("+-.eE", *p) || (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z'))) p++;
    return p > start ? p : NULL;
}

rtc_json rtc_json_of(const char *text, const size_t size)
{
    const char *end = text + size;
    const char *start = skip_space(text, end);
    const char *after = skip_value(start, end, 0);
    return after ? (rtc_json){start, (size_t)(after - start)} : (rtc_json){NULL, 0};
}

rtc_json rtc_json_get(const rtc_json object, const char *key)
{
    const char *p = object.text;
    const char *end = object.text ? object.text + object.size : NULL;
    if (!p || *p != '{') return (rtc_json){NULL, 0};
    const size_t key_size = strlen(key);
    p = skip_space(p + 1, end);
    while (p < end && *p == '"') {
        const char *key_end = skip_value(p, end, 1);
        if (!key_end) break;
        // Keys are compared as written: ours have no escapes
        const bool match = (size_t)(key_end - p - 2) == key_size && memcmp(p + 1, key, key_size) == 0;
        p = skip_space(key_end, end);
        if (p >= end || *p != ':') break;
        const char *value = skip_space(p + 1, end);
        const char *value_end = skip_value(value, end, 1);
        if (!value_end) break;
        if (match) return (rtc_json){value, (size_t)(value_end - value)};
        p = skip_space(value_end, end);
        if (p < end && *p == ',') p = skip_space(p + 1, end);
    }
    return (rtc_json){NULL, 0};
}

bool rtc_json_next(const rtc_json array, const char **at, rtc_json *item)
{
    if (!array.text || *array.text != '[') return false;
    const char *end = array.text + array.size;
    const char *p = *at ? *at : array.text + 1;
    p = skip_space(p, end);
    if (p < end && *p == ',') p = skip_space(p + 1, end);
    if (p >= end || *p == ']') return false;
    const char *value_end = skip_value(p, end, 1);
    if (!value_end) return false;
    *item = (rtc_json){p, (size_t)(value_end - p)};
    *at = value_end;
    return true;
}

bool rtc_json_is_string(const rtc_json v)
{
    return v.text && *v.text == '"';
}

// UTF-8 for a code point below 0x10000 (the relay's messages never need more).
static size_t utf8(uint32_t c, char *out)
{
    if (c < 0x80) {
        out[0] = (char)c;
        return 1;
    }
    if (c < 0x800) {
        out[0] = (char)(0xc0 | c >> 6);
        out[1] = (char)(0x80 | (c & 0x3f));
        return 2;
    }
    out[0] = (char)(0xe0 | c >> 12);
    out[1] = (char)(0x80 | (c >> 6 & 0x3f));
    out[2] = (char)(0x80 | (c & 0x3f));
    return 3;
}

bool rtc_json_string(const rtc_json v, char *out, const size_t capacity)
{
    if (!rtc_json_is_string(v) || capacity == 0) return false;
    size_t n = 0;
    const char *end = v.text + v.size - 1; // The closing quote
    for (const char *p = v.text + 1; p < end; p++) {
        char buffer[4];
        size_t add = 1;
        buffer[0] = *p;
        if (*p == '\\' && p + 1 < end) {
            p++;
            switch (*p) {
            case 'n': buffer[0] = '\n'; break;
            case 'r': buffer[0] = '\r'; break;
            case 't': buffer[0] = '\t'; break;
            case 'b': buffer[0] = '\b'; break;
            case 'f': buffer[0] = '\f'; break;
            case 'u': {
                char hex[5] = {0};
                if (end - p < 5) return false;
                memcpy(hex, p + 1, 4);
                add = utf8((uint32_t)strtoul(hex, NULL, 16), buffer);
                p += 4;
                break;
            }
            default: buffer[0] = *p; break; // \" \\ \/
            }
        }
        if (n + add >= capacity) return false; // Too long for `out`
        memcpy(out + n, buffer, add);
        n += add;
    }
    out[n] = '\0';
    return true;
}

bool rtc_json_number(const rtc_json v, double *out)
{
    if (!v.text || v.size == 0 || v.size > 40 || !(*v.text == '-' || (*v.text >= '0' && *v.text <= '9'))) return false;
    char text[48];
    memcpy(text, v.text, v.size);
    text[v.size] = '\0';
    char *end;
    *out = strtod(text, &end);
    return end == text + v.size;
}

// ---------------------------------------------------------------------------

void rtc_text_add(rtc_text *t, const char *s, const size_t n)
{
    if (t->overflow || t->size + n + 1 > t->capacity) {
        t->overflow = true;
        return;
    }
    memcpy(t->data + t->size, s, n);
    t->size += n;
    t->data[t->size] = '\0';
}

void rtc_text_put(rtc_text *t, const char *s)
{
    rtc_text_add(t, s, strlen(s));
}

void rtc_text_json_string(rtc_text *t, const char *s)
{
    rtc_text_add(t, "\"", 1);
    for (; *s; s++) {
        const unsigned char c = (unsigned char)*s;
        if (c == '"') rtc_text_add(t, "\\\"", 2);
        else if (c == '\\') rtc_text_add(t, "\\\\", 2);
        else if (c == '\n') rtc_text_add(t, "\\n", 2);
        else if (c == '\r') rtc_text_add(t, "\\r", 2);
        else if (c == '\t') rtc_text_add(t, "\\t", 2);
        else if (c < 0x20) {
            char escape[8];
            snprintf(escape, sizeof escape, "\\u%04x", c);
            rtc_text_add(t, escape, 6);
        } else {
            rtc_text_add(t, s, 1);
        }
    }
    rtc_text_add(t, "\"", 1);
}
