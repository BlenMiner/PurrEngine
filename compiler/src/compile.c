#include "compile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lexer.h"

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) {
        fclose(f);
        return NULL;
    }
    char *text = arena_alloc((size_t)size + 1);
    *len = fread(text, 1, (size_t)size, f);
    fclose(f);
    return text;
}

const char *path_stem(const char *path)
{
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    const char *dot = strrchr(base, '.');
    const size_t n = dot ? (size_t)(dot - base) : strlen(base);
    char *out = arena_alloc(n + 1);
    memcpy(out, base, n);
    out[n] = '\0';
    return out;
}

// Files are compiled in order of their paths (see path_compare).
static int path_order(const void *a, const void *b)
{
    return path_compare(*(const char *const *)a, *(const char *const *)b);
}

bool compile_program(const char **inputs, const int count, const codegen_options *opts, sb *schedule)
{
    qsort(inputs, (size_t)count, sizeof(char *), path_order);

    diag_reset();
    program *prog = program_new();
    for (int i = 0; i < count; i++) {
        source *src = NEW(source);
        src->path = inputs[i];
        char *text = read_file(inputs[i], &src->len);
        if (!text) {
            fprintf(stderr, "tidec: can't read %s\n", inputs[i]);
            return false;
        }
        src->text = text;
        diag_add_source(src);

        token *toks = lex(src);
        if (!toks) return false;
        if (!parse_file(prog, src, toks, false)) return false;
    }

    if (!check(prog)) return false;

    codegen_options named = *opts;
    if (!named.name) named.name = path_stem(inputs[0]);
    if (schedule) {
        print_schedule(prog, named.name, schedule);
        return true;
    }
    return codegen(prog, &named);
}
