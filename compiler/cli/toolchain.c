#include "toolchain.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sys.h"

#ifdef _WIN32
#define EXE ".exe"
#else
#define EXE ""
#endif

static char *existing(const char *dir, const char *name)
{
    char *path = path_join(dir, name);
    for (char *c = path; *c; c++) {
        if (*c == '\\') *c = '/';
    }
    if (sys_exists(path)) return path;
    free(path);
    return NULL;
}

#ifdef _WIN32
// Visual Studio's bundled clang: <Program Files>/Microsoft Visual Studio/<version>/<edition>/VC/Tools/Llvm/x64/bin.
typedef struct vs_search {
    const char *base;
    char *found;
    int depth; // 0: versions, 1: editions
} vs_search;

static void visit_vs(void *user, const char *name, const bool is_dir)
{
    vs_search *s = user;
    if (!is_dir || s->found) return;
    char *dir = path_join(s->base, name);
    if (s->depth == 0) {
        vs_search editions = {dir, NULL, 1};
        sys_list(dir, visit_vs, &editions);
        s->found = editions.found;
    } else {
        char *bin = path_join(dir, "VC/Tools/Llvm/x64/bin");
        s->found = existing(bin, "clang.exe");
        free(bin);
    }
    free(dir);
}
#endif

char *find_clang(void)
{
    const char *root = sys_env("LLVM_ROOT");
    if (root) {
        char *bin = path_join(root, "bin");
        char *found = existing(bin, "clang" EXE);
        free(bin);
        if (found) return found;
    }
#ifdef _WIN32
    const char *program_files = sys_env("ProgramFiles");
    if (program_files) {
        char *bin = path_join(program_files, "LLVM/bin");
        char *found = existing(bin, "clang.exe");
        free(bin);
        if (found) return found;
    }
#endif
    char *on_path = sys_which("clang");
    if (on_path) return on_path;
#ifdef _WIN32
    const char *const bases[] = {sys_env("ProgramFiles"), sys_env("ProgramFiles(x86)")};
    for (size_t i = 0; i < sizeof bases / sizeof bases[0]; i++) {
        if (!bases[i]) continue;
        char *vs = path_join(bases[i], "Microsoft Visual Studio");
        vs_search s = {vs, NULL, 0};
        sys_list(vs, visit_vs, &s);
        free(vs);
        if (s.found) return s.found;
    }
#endif
    return NULL;
}

char *find_wasm_ld(const char *clang)
{
    char *dir = path_dir(clang);
    char *found = existing(dir, "wasm-ld" EXE);
    free(dir);
    return found ? found : sys_which("wasm-ld");
}
