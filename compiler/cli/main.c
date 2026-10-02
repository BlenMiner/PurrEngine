// tide: builds and runs Tide games, and keeps itself up to date.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "build.h"
#include "editors.h"
#include "sys.h"
#include "toolchain.h"
#include "upgrade.h"

#ifdef TIDE_EMBEDDED_CLANG
#include "cc.h"
#endif

#ifndef TIDE_VERSION
#define TIDE_VERSION "0.0.0-dev"
#endif

static void usage(void)
{
    printf("tide %s: builds and runs Tide games.\n"
           "\n"
           "usage: tide <command> [folder] [options]\n"
           "\n"
           "  run [folder]       build the game in folder (default: here) and play it\n"
           "  build [folder]     build the game into <folder>/build\n"
           "  schedule [folder]  show which systems can run at the same time, and why the others wait\n"
           "  editors            add Tide to VS Code, Cursor, VSCodium and Windsurf\n"
           "  upgrade            update tide to the newest version\n"
           "  version            show tide's version, and which compilers it found\n"
           "\n"
           "A game is every .tide file in its folder and its subfolders.\n"
           "\n"
           "run and build:\n"
           "  --release          optimized, the way players get it\n"
           "  --web              a web page (WebGL 2), with clang's WebAssembly target\n"
           "  --title <title>    the window's title (default: the game's title setting, or the folder's name)\n"
           "  --stats            show the frame rate, ping, bandwidth, tick and entity count\n"
           "  -o <path>          build: where the program goes\n"
           "  --no-open          run --web: serve the page without opening a browser\n"
           "\n"
           "run, in a match with others (the game's Main must be the match's for --host):\n"
           "  --host [port]        a match others can join: in a room, and on a port (7777 by default; not on the web)\n"
           "  --join <code>        the match in the room with this code, like K7QF2M\n"
           "  --connect <address>  the match at an address, like 192.168.1.5 or localhost:7777\n"
           "\n"
           "upgrade:\n"
           "  --nightly          follow nightly versions from now on\n"
           "  --stable           follow stable versions from now on\n"
           "  --version <v>      install exactly this version\n",
           TIDE_VERSION);
}

// The installation: tide is in <root>/bin.
static char *install_root(void)
{
    char *bin = sys_exe_dir();
    char *root = path_dir(bin);
    free(bin);
    return root;
}

static int version(const char *root)
{
    printf("tide %s (%s)\ninstalled in %s\n", TIDE_VERSION, tide_channel(root), root);
#ifdef TIDE_EMBEDDED_CLANG
    printf("clang:   built in (LLVM %s)\n", TIDE_LLVM_VERSION);
#else
    char *clang = find_clang();
    char *web_clang = find_web_clang();
    char *wasm_ld = web_clang ? find_wasm_ld(web_clang) : NULL;
    printf("clang:   %s\n", clang ? clang : "not found (tide needs it to build games)");
    if (web_clang && clang && strcmp(web_clang, clang) != 0) printf("web:     %s\n", web_clang);
    printf("wasm-ld: %s\n", wasm_ld ? wasm_ld : "not found (needed for --web)");
#endif
    return 0;
}

int main(const int argc, char **argv)
{
#ifdef TIDE_EMBEDDED_CLANG
    // tide is its own C compiler: `tide cc` is clang, which runs itself again
    // as `tide -cc1` when it compiles in another process.
    if (argc > 1 && (strcmp(argv[1], "cc") == 0 || strcmp(argv[1], "-cc1") == 0)) {
        return tide_cc(argc, (const char **)argv);
    }
#endif
    if (argc < 2 || strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage();
        return argc < 2 ? 2 : 0;
    }
    const char *command = argv[1];
    char *root = install_root();
    tide_cleanup(root);

    if (strcmp(command, "version") == 0 || strcmp(command, "--version") == 0) return version(root);

    if (strcmp(command, "editors") == 0) {
        bool only_updates = false;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--update") == 0) only_updates = true; // Used by tide upgrade
            else {
                fprintf(stderr, "tide: editors doesn't take '%s'\n", argv[i]);
                return 2;
            }
        }
        return tide_editors(root, only_updates);
    }

    if (strcmp(command, "upgrade") == 0) {
        const char *channel = NULL;
        const char *exact = NULL;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--nightly") == 0) channel = "nightly";
            else if (strcmp(argv[i], "--stable") == 0) channel = "stable";
            else if (strcmp(argv[i], "--version") == 0 && i + 1 < argc) exact = argv[++i];
            else {
                fprintf(stderr, "tide: upgrade doesn't take '%s'\n", argv[i]);
                return 2;
            }
        }
        return tide_upgrade(root, channel, exact);
    }

    const bool run = strcmp(command, "run") == 0;
    const bool build = strcmp(command, "build") == 0;
    const bool schedule = strcmp(command, "schedule") == 0;
    if (!run && !build && !schedule) {
        fprintf(stderr, "tide: unknown command '%s'\n", command);
        fprintf(stderr, "  = note: the commands are run, build, schedule, editors, upgrade and version; see `tide help`\n");
        return 2;
    }

    build_options opts = {.folder = "."};
    const char *session[3] = {NULL, NULL, NULL}; // --host [port], --join <code> or --connect <address>, for the game
    bool open_page = true;                        // run --web opens the page in a browser
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (run && strcmp(a, "--host") == 0) {
            session[0] = a;
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') session[1] = argv[++i];
        } else if (run && (strcmp(a, "--join") == 0 || strcmp(a, "--connect") == 0) && i + 1 < argc) {
            session[0] = a;
            session[1] = argv[++i];
        } else if (run && strcmp(a, "--no-open") == 0) open_page = false;
        else if (strcmp(a, "--release") == 0) opts.release = true;
        else if (strcmp(a, "--web") == 0) opts.web = true;
        else if (strcmp(a, "--stats") == 0) opts.stats = true;
        else if (strcmp(a, "--title") == 0 && i + 1 < argc) opts.title = argv[++i];
        else if (strcmp(a, "-o") == 0 && i + 1 < argc && build) opts.output = argv[++i];
        else if (a[0] != '-') opts.folder = a;
        else {
            fprintf(stderr, "tide: %s doesn't take '%s'; see `tide help`\n", command, a);
            return 2;
        }
    }

    int code = 0;
    if (schedule) {
        code = tide_schedule(opts.folder) ? 0 : 1;
    } else if (run && !opts.web) {
        code = tide_run_reloading(root, &opts, session);
    } else if (run) {
        code = tide_run_web(root, &opts, open_page, session);
    } else {
        char *program = tide_build(root, &opts);
        if (!program) code = 1;
        else printf("Built %s\n", program);
    }
    tide_check_for_update(root);
    return code;
}
