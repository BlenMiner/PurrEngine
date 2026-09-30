#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compile.h"

static void usage(void)
{
    fprintf(stderr,
            "usage: purrc <file.purr>... -o <output-dir> [--name <name>] [--no-line] [--layout]\n"
            "       purrc <file.purr>... --schedule [--name <name>]\n"
            "\n"
            "Transpiles a PurrLang program, made of one or more files, to\n"
            "<output-dir>/<name>.h and <name>.c.\n"
            "  --name <name>  base name of the generated files (default: the first file's name)\n"
            "  --no-line      don't map generated code back to .purr lines for debuggers\n"
            "  --layout       also describe the data layout, as purr run does for hot reloading\n"
            "  --schedule     print which systems can run at the same time and why the others\n"
            "                 wait, instead of generating code\n");
}

int main(const int argc, char **argv)
{
    const char **inputs = calloc((size_t)argc, sizeof(char *));
    int input_count = 0;
    codegen_options opts = {NULL, NULL, true, false, NULL};
    bool schedule = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            opts.out_dir = argv[++i];
        } else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            opts.name = argv[++i];
        } else if (strcmp(argv[i], "--no-line") == 0) {
            opts.line_directives = false;
        } else if (strcmp(argv[i], "--layout") == 0) {
            opts.layout = true;
        } else if (strcmp(argv[i], "--schedule") == 0) {
            schedule = true;
        } else if (argv[i][0] == '-') {
            usage();
            return 2;
        } else {
            inputs[input_count++] = argv[i];
        }
    }
    if (input_count == 0 || (!opts.out_dir && !schedule)) {
        usage();
        return 2;
    }

    if (!schedule) return compile_program(inputs, input_count, &opts, NULL) ? 0 : 1;
    sb text = {0};
    if (!compile_program(inputs, input_count, &opts, &text)) return 1;
    fputs(text.data, stdout);
    return 0;
}
