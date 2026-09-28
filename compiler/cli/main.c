// purr: builds and runs PurrLang games, and keeps itself up to date.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "build.h"
#include "editors.h"
#include "sys.h"
#include "toolchain.h"
#include "upgrade.h"

#ifndef PURR_VERSION
#define PURR_VERSION "0.0.0-dev"
#endif

static void usage(void)
{
    printf("purr %s: builds and runs PurrLang games.\n"
           "\n"
           "usage: purr <command> [folder] [options]\n"
           "\n"
           "  run [folder]       build the game in folder (default: here) and play it\n"
           "  build [folder]     build the game into <folder>/build\n"
           "  schedule [folder]  show which systems can run at the same time, and why the others wait\n"
           "  editors            add PurrLang to VS Code, Cursor, VSCodium and Windsurf\n"
           "  upgrade            update purr to the newest version\n"
           "  version            show purr's version, and which compilers it found\n"
           "\n"
           "A game is every .purr file in its folder and its subfolders.\n"
           "\n"
           "run and build:\n"
           "  --release          optimized, the way players get it\n"
           "  --web              a web page (WebGL 2), with clang's WebAssembly target\n"
           "  --title <title>    the window's title (default: the folder's name)\n"
           "  --stats            show the tick, the entity count and the frame rate\n"
           "  -o <path>          build: where the program goes\n"
           "\n"
           "upgrade:\n"
           "  --nightly          follow nightly versions from now on\n"
           "  --stable           follow stable versions from now on\n"
           "  --version <v>      install exactly this version\n",
           PURR_VERSION);
}

// The installation: purr is in <root>/bin.
static char *install_root(void)
{
    char *bin = sys_exe_dir();
    char *root = path_dir(bin);
    free(bin);
    return root;
}

static int open_in_browser(const char *page)
{
#if defined(_WIN32)
    const char *const argv[] = {"cmd.exe", "/c", "start", "", page, NULL};
#elif defined(__APPLE__)
    const char *const argv[] = {"open", page, NULL};
#else
    const char *const argv[] = {"xdg-open", page, NULL};
#endif
    return sys_run(argv, NULL, true) == 0 ? 0 : 1;
}

static int version(const char *root)
{
    printf("purr %s (%s)\ninstalled in %s\n", PURR_VERSION, purr_channel(root), root);
    char *clang = find_clang();
    char *web_clang = find_web_clang();
    char *wasm_ld = web_clang ? find_wasm_ld(web_clang) : NULL;
    printf("clang:   %s\n", clang ? clang : "not found (purr needs it to build games)");
    if (web_clang && clang && strcmp(web_clang, clang) != 0) printf("web:     %s\n", web_clang);
    printf("wasm-ld: %s\n", wasm_ld ? wasm_ld : "not found (needed for --web)");
    return 0;
}

int main(const int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage();
        return argc < 2 ? 2 : 0;
    }
    const char *command = argv[1];
    char *root = install_root();
    purr_cleanup(root);

    if (strcmp(command, "version") == 0 || strcmp(command, "--version") == 0) return version(root);

    if (strcmp(command, "editors") == 0) {
        bool only_updates = false;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--update") == 0) only_updates = true; // Used by purr upgrade
            else {
                fprintf(stderr, "purr: editors doesn't take '%s'\n", argv[i]);
                return 2;
            }
        }
        return purr_editors(root, only_updates);
    }

    if (strcmp(command, "upgrade") == 0) {
        const char *channel = NULL;
        const char *exact = NULL;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--nightly") == 0) channel = "nightly";
            else if (strcmp(argv[i], "--stable") == 0) channel = "stable";
            else if (strcmp(argv[i], "--version") == 0 && i + 1 < argc) exact = argv[++i];
            else {
                fprintf(stderr, "purr: upgrade doesn't take '%s'\n", argv[i]);
                return 2;
            }
        }
        return purr_upgrade(root, channel, exact);
    }

    const bool run = strcmp(command, "run") == 0;
    const bool build = strcmp(command, "build") == 0;
    const bool schedule = strcmp(command, "schedule") == 0;
    if (!run && !build && !schedule) {
        fprintf(stderr, "purr: unknown command '%s'\n", command);
        fprintf(stderr, "  = note: the commands are run, build, schedule, editors, upgrade and version; see `purr help`\n");
        return 2;
    }

    build_options opts = {.folder = "."};
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--release") == 0) opts.release = true;
        else if (strcmp(a, "--web") == 0) opts.web = true;
        else if (strcmp(a, "--stats") == 0) opts.stats = true;
        else if (strcmp(a, "--title") == 0 && i + 1 < argc) opts.title = argv[++i];
        else if (strcmp(a, "-o") == 0 && i + 1 < argc && build) opts.output = argv[++i];
        else if (a[0] != '-') opts.folder = a;
        else {
            fprintf(stderr, "purr: %s doesn't take '%s'; see `purr help`\n", command, a);
            return 2;
        }
    }

    int code = 0;
    if (schedule) {
        code = purr_schedule(opts.folder) ? 0 : 1;
    } else {
        char *program = purr_build(root, &opts, run);
        if (!program) {
            code = 1;
        } else if (build) {
            printf("Built %s\n", program);
        } else if (opts.web) {
            printf("Opening %s\n", program);
            code = open_in_browser(program);
        } else {
            char *folder = path_absolute(opts.folder);
            const char *const game[] = {program, NULL};
            code = sys_run(game, folder, false);
            if (code == -1) fprintf(stderr, "purr: couldn't start %s\n", program);
        }
    }
    purr_check_for_update(root);
    return code;
}
