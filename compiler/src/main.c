#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ast.h"
#include "codegen.h"
#include "lexer.h"

static void usage(void)
{
    fprintf(stderr,
            "usage: purrc <input.purr> -o <output-dir> [--name <name>] [--no-line]\n"
            "\n"
            "Transpiles a PurrLang program to <output-dir>/<name>.h and <name>.c.\n"
            "  --name <name>  base name of the generated files (default: input file name)\n"
            "  --no-line      don't map generated code back to .purr lines for debuggers\n");
}

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

// "dir/game.purr" -> "game"
static const char *stem(const char *path)
{
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    const char *dot = strrchr(base, '.');
    const size_t n = dot ? (size_t)(dot - base) : strlen(base);
    char *out = arena_alloc(n + 1);
    memcpy(out, base, n);
    return out;
}

int main(const int argc, char **argv)
{
    const char *input = NULL;
    codegen_options opts = {NULL, NULL, true};

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            opts.out_dir = argv[++i];
        } else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            opts.name = argv[++i];
        } else if (strcmp(argv[i], "--no-line") == 0) {
            opts.line_directives = false;
        } else if (argv[i][0] == '-') {
            usage();
            return 2;
        } else if (!input) {
            input = argv[i];
        } else {
            fprintf(stderr, "purrc: only one input file is supported for now\n");
            return 2;
        }
    }
    if (!input || !opts.out_dir) {
        usage();
        return 2;
    }
    if (!opts.name) opts.name = stem(input);

    source src = {input, NULL, 0};
    char *text = read_file(input, &src.len);
    if (!text) {
        fprintf(stderr, "purrc: can't read %s\n", input);
        return 1;
    }
    src.text = text;
    diag_init(&src);

    token *toks = lex(&src);
    if (!toks) return 1;

    program *prog = parse(&src, toks);
    if (!prog) return 1;

    if (!check(prog)) return 1;

    return codegen(prog, &opts) ? 0 : 1;
}
