#include "cdefs.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Set by the build: the engine this server was built with.
#ifndef PURR_ENGINE_INCLUDE_DIR
#define PURR_ENGINE_INCLUDE_DIR "engine/include"
#endif

// The headers generated code includes, so everything PurrLang builds on.
static const char *const header_names[] = {"color.h", "devices.h", "draw.h", "entity.h", "math.h", "player.h"};
#define HEADER_COUNT (sizeof header_names / sizeof header_names[0])

typedef struct header {
    char *path;
    char *text; // Lines are NUL-separated
    char **lines;
    int line_count;
} header;

static header headers[HEADER_COUNT];
static bool loaded;

static void load(void)
{
    if (loaded) return;
    loaded = true;
    for (size_t h = 0; h < HEADER_COUNT; h++) {
        header *hd = &headers[h];
        const size_t path_len = strlen(PURR_ENGINE_INCLUDE_DIR) + strlen(header_names[h]) + 8;
        hd->path = malloc(path_len);
        if (!hd->path) return;
        snprintf(hd->path, path_len, "%s/purr/%s", PURR_ENGINE_INCLUDE_DIR, header_names[h]);

        FILE *f = fopen(hd->path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        const long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        hd->text = malloc((size_t)(size > 0 ? size : 0) + 1);
        const size_t n = hd->text ? fread(hd->text, 1, (size_t)(size > 0 ? size : 0), f) : 0;
        fclose(f);
        if (!hd->text) continue;
        hd->text[n] = '\0';

        int count = 1;
        for (size_t i = 0; i < n; i++) count += hd->text[i] == '\n';
        hd->lines = malloc(sizeof(char *) * (size_t)count);
        if (!hd->lines) continue;
        hd->lines[0] = hd->text;
        hd->line_count = 1;
        for (size_t i = 0; i < n; i++) {
            if (hd->text[i] == '\n') {
                hd->text[i] = '\0';
                if (i > 0 && hd->text[i - 1] == '\r') hd->text[i - 1] = '\0';
                hd->lines[hd->line_count++] = hd->text + i + 1;
            }
        }
    }
}

static bool is_ident(const char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

// Column of `word` in `line` as a whole word (not inside a longer name), from
// `from`, or -1.
static int find_word(const char *line, const char *word, const int from)
{
    const size_t n = strlen(word);
    for (const char *p = strstr(line + from, word); p; p = strstr(p + 1, word)) {
        const bool before = p > line && is_ident(p[-1]);
        const bool after = is_ident(p[n]);
        if (!before && !after) return (int)(p - line);
    }
    return -1;
}

static bool is_comment(const char *line)
{
    while (*line == ' ' || *line == '\t') line++;
    return (line[0] == '/' && (line[1] == '/' || line[1] == '*')) || line[0] == '*';
}

static void write_location(const header *hd, const int line, const int column, const int len, jbuf *out)
{
    jb_put(out, "{\"uri\":\"file://");
    if (hd->path[0] != '/') jb_put(out, "/");
    for (const char *p = hd->path; *p; p++) {
        if (*p == '\\') jb_put(out, "/");
        else if (*p == ' ') jb_put(out, "%20");
        else if (*p == '"' || *p == '%' || *p == '#') jb_printf(out, "%%%02X", (unsigned char)*p);
        else jb_putn(out, p, 1);
    }
    jb_printf(out, "\",\"range\":{\"start\":{\"line\":%d,\"character\":%d},\"end\":{\"line\":%d,\"character\":%d}}}",
              line, column, line, column + len);
}

// Does this occurrence of `name` define it? `#define name`, `typedef struct
// name`, or a declaration: a return type, then the name, then '('.
static bool defines(const char *line, const int column, const char *name)
{
    const char *after = line + column + strlen(name);
    while (*after == ' ') after++;
    const char *start = line;
    while (*start == ' ' || *start == '\t') start++;
    if (strncmp(start, "#define ", 8) == 0) return start + 8 == line + column;
    if (strstr(line, "typedef struct ") && find_word(line, name, 0) == column) return true;
    if (*after != '(') return false;
    if (start == line + column) return false; // A call at the start of a line
    // Before the name, only a return type: `static inline purr_float3 `, `void *`.
    for (const char *p = start; p < line + column; p++) {
        if (!is_ident(*p) && *p != ' ' && *p != '\t' && *p != '*') return false;
    }
    return strncmp(start, "return ", 7) != 0;
}

// The first line in the headers that defines `name`.
static bool search_definition(const char *name, jbuf *out)
{
    for (size_t h = 0; h < HEADER_COUNT; h++) {
        const header *hd = &headers[h];
        for (int l = 0; l < hd->line_count; l++) {
            if (is_comment(hd->lines[l])) continue;
            for (int c = find_word(hd->lines[l], name, 0); c >= 0; c = find_word(hd->lines[l], name, c + 1)) {
                if (defines(hd->lines[l], c, name)) {
                    write_location(hd, l, c, (int)strlen(name), out);
                    return true;
                }
            }
        }
    }
    return false;
}

static bool search_text(const char *text, jbuf *out)
{
    for (size_t h = 0; h < HEADER_COUNT; h++) {
        const header *hd = &headers[h];
        for (int l = 0; l < hd->line_count; l++) {
            const char *p = strstr(hd->lines[l], text);
            if (p && !is_comment(hd->lines[l])) {
                write_location(hd, l, (int)(p - hd->lines[l]), (int)strlen(text), out);
                return true;
            }
        }
    }
    return false;
}

bool cdefs_find(const char *name, jbuf *out)
{
    load();
    if (search_definition(name, out)) return true;

    // Names that macros generate: purr_<base>_<f|i><2|3|4>.
    const size_t n = strlen(name);
    if (n < 8 || strncmp(name, "purr_", 5) != 0 || !isdigit((unsigned char)name[n - 1])) return false;
    const char kind = name[n - 2];
    if (name[n - 3] != '_' || (kind != 'f' && kind != 'i')) return false;

    // A template in a macro body: purr_normalize_f##N.
    char pasted[128];
    snprintf(pasted, sizeof pasted, "%.*s##N", (int)(n - 1), name);
    if (search_text(pasted, out)) return true;

    // A macro invocation: PURR_MAP1(float, f, sin, f).
    char base[64];
    snprintf(base, sizeof base, "%.*s", (int)(n - 8), name + 5);
    const char *element = kind == 'f' ? "(float," : "(int,";
    for (size_t h = 0; h < HEADER_COUNT; h++) {
        const header *hd = &headers[h];
        for (int l = 0; l < hd->line_count; l++) {
            const char *line = hd->lines[l];
            if (strncmp(line, "PURR_", 5) != 0 || !strstr(line, element)) continue;
            const int c = find_word(line, base, 0);
            if (c >= 0) {
                write_location(hd, l, c, (int)strlen(base), out);
                return true;
            }
        }
    }
    return false;
}

// X-macros that declare a device struct's members.
static const struct {
    const char *struct_name;
    const char *macros[3];
} member_macros[] = {
    {"purr_keyboard", {"PURR_KEYBOARD_KEYS"}},
    {"purr_mouse", {"PURR_MOUSE_AXES", "PURR_MOUSE_BUTTONS"}},
    {"purr_dpad", {"PURR_DPAD_BUTTONS"}},
    {"purr_gamepad", {"PURR_GAMEPAD_STICKS", "PURR_GAMEPAD_TRIGGERS", "PURR_GAMEPAD_BUTTONS"}},
};

bool cdefs_find_member(const char *struct_name, const char *member, jbuf *out)
{
    load();
    char opening[128];
    char closing[128];
    snprintf(opening, sizeof opening, "struct %s", struct_name);
    snprintf(closing, sizeof closing, "} %s", struct_name);
    for (size_t h = 0; h < HEADER_COUNT; h++) {
        const header *hd = &headers[h];
        for (int l = 0; l < hd->line_count; l++) {
            const char *open = strstr(hd->lines[l], opening);
            if (!open || !strstr(hd->lines[l], "typedef")) continue;
            // From the opening brace to `} name`, which may be on the same line.
            for (int k = l; k < hd->line_count; k++) {
                const int from = k == l ? (int)(open - hd->lines[l]) + (int)strlen(opening) : 0;
                const char *end = strstr(hd->lines[k] + from, closing);
                const int c = find_word(hd->lines[k], member, from);
                if (c >= 0 && (!end || hd->lines[k] + c < end) && !is_comment(hd->lines[k])) {
                    write_location(hd, k, c, (int)strlen(member), out);
                    return true;
                }
                if (end) break;
            }
        }
    }

    // Members from X-macros: X(member) in the macro's definition.
    char entry[96];
    snprintf(entry, sizeof entry, "X(%s)", member);
    for (size_t i = 0; i < sizeof member_macros / sizeof member_macros[0]; i++) {
        if (strcmp(member_macros[i].struct_name, struct_name) != 0) continue;
        for (int m = 0; m < 3 && member_macros[i].macros[m]; m++) {
            char define[96];
            snprintf(define, sizeof define, "#define %s(X)", member_macros[i].macros[m]);
            for (size_t h = 0; h < HEADER_COUNT; h++) {
                const header *hd = &headers[h];
                for (int l = 0; l < hd->line_count; l++) {
                    if (!strstr(hd->lines[l], define)) continue;
                    // The definition continues while lines end with a backslash.
                    for (int k = l; k < hd->line_count; k++) {
                        const char *p = strstr(hd->lines[k], entry);
                        if (p) {
                            write_location(hd, k, (int)(p - hd->lines[k]) + 2, (int)strlen(member), out);
                            return true;
                        }
                        const size_t len = strlen(hd->lines[k]);
                        if (len == 0 || hd->lines[k][len - 1] != '\\') break;
                    }
                }
            }
        }
    }
    return false;
}

bool cdefs_find_header(const char *file, jbuf *out)
{
    load();
    for (size_t h = 0; h < HEADER_COUNT; h++) {
        if (strcmp(header_names[h], file) == 0 && headers[h].text) {
            write_location(&headers[h], 0, 0, 0, out);
            return true;
        }
    }
    return false;
}
