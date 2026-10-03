#include "build.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "android.h"
#include "apk.h"
#include "common.h"
#include "compile.h"
#include "fetch.h"
#include "folders.h"
#include "libraries.h"
#include "packages.h"
#include "serve.h"
#include "sys.h"
#include "toolchain.h"

#ifndef TIDE_VERSION
#define TIDE_VERSION "0.0.0-dev"
#endif

// A growable argv for sys_run, ending with NULL.
typedef struct args {
    const char **items;
    int count;
    int cap;
} args;

static void arg(args *a, const char *s)
{
    if (a->count + 2 > a->cap) {
        a->cap = a->cap * 2 + 16;
        a->items = realloc(a->items, sizeof(char *) * (size_t)a->cap);
        if (!a->items) abort();
    }
    a->items[a->count++] = s;
    a->items[a->count] = NULL;
}

static void arg_list(args *a, const char *const *list)
{
    for (int i = 0; list[i]; i++) arg(a, list[i]);
}

static char *format(const char *fmt, const char *a, const char *b)
{
    const size_t n = strlen(fmt) + strlen(a) + (b ? strlen(b) : 0) + 1;
    char *out = malloc(n);
    if (!out) abort();
    snprintf(out, n, fmt, a, b ? b : "");
    return out;
}

// ---------------------------------------------------------------------------
// The game's files

typedef struct file_list {
    char **items;
    int count;
} file_list;

static void add_file(void *user, const char *path)
{
    file_list *list = user;
    list->items = realloc(list->items, sizeof(char *) * (size_t)(list->count + 1));
    if (!list->items) abort();
    const size_t n = strlen(path) + 1;
    list->items[list->count] = malloc(n);
    if (!list->items[list->count]) abort();
    memcpy(list->items[list->count++], path, n);
}

static void free_files(file_list *list)
{
    for (int i = 0; i < list->count; i++) free(list->items[i]);
    free(list->items);
    *list = (file_list){0};
}

static int path_order(const void *a, const void *b)
{
    return path_compare(*(const char *const *)a, *(const char *const *)b);
}

// The .tide files in `folder` and its subfolders, but those of games and
// packages of their own (subfolders with a tide.packages), by path.
static file_list tide_files(const char *folder)
{
    file_list list = {0};
    char *dir = path_join(folder, "");
    folder_find_own(dir, ".tide", add_file, &list);
    free(dir);
    if (list.count > 0) qsort(list.items, (size_t)list.count, sizeof(char *), path_order);
    return list;
}

// The game's own files of other kinds (its C, its headers, its libraries):
// not in a hidden folder, like .tide/, where tide keeps what it makes, or in
// build/, where builds go.
typedef struct own_files {
    size_t prefix; // The folder's length, with its '/'
    const char *const *extensions;
    file_list list;
} own_files;

static bool has_extension(const char *path, const char *extension)
{
    const size_t n = strlen(path);
    const size_t e = strlen(extension);
    if (n <= e) return false;
    for (size_t i = 0; i < e; i++) {
        char ch = path[n - e + i];
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a'); // STEAM_API64.LIB
        if (ch != extension[i]) return false;
    }
    return true;
}

static void add_own_file(void *user, const char *path)
{
    own_files *o = user;
    bool wanted = false;
    for (int i = 0; o->extensions[i] && !wanted; i++) wanted = has_extension(path, o->extensions[i]);
    if (!wanted) return;
    const char *relative = path + o->prefix;
    if (strncmp(relative, "build/", 6) == 0) return;
    for (const char *p = relative; *p; p++) {
        if (*p == '.' && (p == relative || p[-1] == '/') && strchr(p, '/')) return; // A hidden folder
    }
    add_file(&o->list, path);
}

static file_list own_files_of(const char *folder, const char *const *extensions)
{
    char *dir = path_join(folder, "");
    own_files o = {strlen(dir), extensions, {0}};
    folder_find_own(dir, "", add_own_file, &o); // Every file, once
    free(dir);
    return o.list;
}

static const char *const c_extensions[] = {".c", NULL};
static const char *const header_extensions[] = {".h", NULL};
static const char *const library_extensions[] = {".a", ".lib", ".so", ".dll", ".dylib", NULL};
// Whatever a build of the game reads, besides its .tide files: tide run builds
// again when one changes.
static const char *const c_side_extensions[] = {".c", ".h", ".a", ".lib", ".so", ".dll", ".dylib", NULL};

// ---------------------------------------------------------------------------
// Packages (packages.h): those from git are downloaded the first time a build
// needs them.

// Says what's wrong with a tide.packages, as tidec says what's wrong with code.
static void report_package(void *user, const char *where, const int line, const char *message, const char *note)
{
    (void)user;
    if (line > 0) fprintf(stderr, "%s:%d: error: %s\n", where, line, message);
    else fprintf(stderr, "%s: error: %s\n", where, message);
    if (note) fprintf(stderr, "  = note: %s\n", note);
}

// The packages of the game in `folder`, into `out`. False after saying
// what's wrong, which includes `folder` being a package rather than a game.
static bool find_packages(const char *folder, game_packages *out)
{
    packages_free(out);
    if (!packages_resolve(folder, out, fetch_package, report_package, NULL)) return false;
    if (out->file.name) {
        fprintf(stderr, "tide: %s is package %s, not a game\n", folder, out->file.name);
        fprintf(stderr, "  = note: to try it, make a game in a folder of its own whose tide.packages lists this "
                        "folder, like `..` from a folder in it\n");
        return false;
    }
    return true;
}

// Every .tide file of the game, in the order they compile: each package's,
// in the packages' order, then the game's own, each by path. `paths` holds
// the paths. NULL after saying the game has no files of its own.
static compile_input *game_inputs(const char *folder, const game_packages *packages, file_list *paths, int *count)
{
    *paths = (file_list){0};
    const char **names = malloc(sizeof(char *) * 1); // Each path's package
    if (!names) abort();
    int game_start = 0;
    for (int p = 0; p <= packages->count; p++) {
        const char *name = p < packages->count ? packages->items[p].name : NULL;
        if (!name) game_start = paths->count;
        file_list files = tide_files(name ? packages->items[p].folder : folder);
        names = realloc(names, sizeof(char *) * (size_t)(paths->count + files.count + 1));
        if (!names) abort();
        for (int i = 0; i < files.count; i++) {
            names[paths->count] = name;
            add_file(paths, files.items[i]);
        }
        free_files(&files);
    }
    if (game_start == paths->count) {
        fprintf(stderr, "tide: there are no .tide files in %s\n", folder);
        fprintf(stderr, "  = note: a game is every .tide file in its folder; add one, like game.tide\n");
        free_files(paths);
        free(names);
        return NULL;
    }
    compile_input *inputs = malloc(sizeof(compile_input) * (size_t)paths->count);
    if (!inputs) abort();
    for (int i = 0; i < paths->count; i++) inputs[i] = (compile_input){paths->items[i], names[i]};
    free(names);
    *count = paths->count;
    return inputs;
}

// The folder's name, as a window title and a file name.
static const char *game_name(const char *folder)
{
    const char *name = path_base(folder);
    return name[0] ? name : "game";
}

// `text` in a C string literal.
static void c_string(char *out, const size_t size, const char *text)
{
    size_t n = 0;
    for (const char *p = text; *p && n + 3 < size; p++) {
        if (*p == '"' || *p == '\\') out[n++] = '\\';
        out[n++] = *p;
    }
    out[n] = '\0';
}

// The window's title in a tide_run_desc: `.title = "..."` when --title gave
// one, which goes over the game's title setting, or else the game's name,
// which only stands in when the game has none.
static void title_field(char *out, const size_t size, const build_options *opts, const char *name)
{
    char escaped[512];
    c_string(escaped, sizeof escaped, opts->title ? opts->title : name);
    snprintf(out, size, "%s = \"%s\"", opts->title ? ".title" : ".game_name", escaped);
}

// The generated host: the game in a window, through tide/run.h. The same as
// tide_add_game's (cmake/Tide.cmake).
static bool write_main(const char *path, const build_options *opts, const char *name)
{
    char title[600];
    title_field(title, sizeof title, opts, name);
    char text[1024];
    snprintf(text, sizeof text,
             "// Generated by tide. Do not edit.\n"
             "#include \"game.h\"\n"
             "#include \"tide/run.h\"\n"
             "\n"
             "int main(int argc, char **argv)\n"
             "{\n"
             "    tide_run(&(tide_run_desc){%s, .stats = %s, .argc = argc, .argv = argv});\n"
             "}\n",
             title, opts->stats ? "true" : "false");
    return sys_write_text(path, text);
}

// ---------------------------------------------------------------------------
// Compiler flags

// Determinism (see AGENTS.md): no fast-math and no contraction, in this order.
static const char *const common_flags[] = {"-std=c17", "-fno-fast-math", "-ffp-contract=off", "-w", NULL};
static const char *const debug_flags[] = {"-O0", "-g", NULL};
static const char *const release_flags[] = {"-O2", "-DNDEBUG", NULL};

#ifdef _WIN32
// Windows games use the MinGW-w64 target, whose C runtime comes with tide (as
// in cmake/mingw-toolchain.cmake): Microsoft's can't be redistributed. Its C
// library is the UCRT, part of Windows, so games need no DLLs of their own.
// The package holds exactly these libraries (platform/CMakeLists.txt).
#define NATIVE_TARGET "--target=x86_64-w64-windows-gnu"
#define NATIVE_RUNTIME "mingw"
static const char *const native_libs[] = {"-lmingw32", "-lmingwex", "-lmoldname", "-lmsvcrt", "-lkernel32",
                                          "-luser32", "-lgdi32", "-lshell32", "-ladvapi32", "-lopengl32",
                                          "-lwinmm", "-lws2_32", "-lsecur32", NULL};
#define EXE_SUFFIX ".exe"
#define LIBRARY_SUFFIX ".dll"
#elif defined(__APPLE__)
// What raylib and its GLFW link on macOS, and Security, for TLS to the relay.
static const char *const native_libs[] = {"-framework", "Cocoa", "-framework", "IOKit", "-framework", "CoreFoundation",
                                          "-framework", "CoreVideo", "-framework", "OpenGL", "-framework", "CoreAudio",
                                          "-framework", "AudioToolbox", "-framework", "Security", NULL};
#define EXE_SUFFIX ""
#define LIBRARY_SUFFIX ".dylib"
#else
// raylib calls Xlib directly (the rest of X11 and OpenGL it loads at run time).
// The library itself, not -lX11: every desktop has it, but not every desktop
// has the development package that provides libX11.so.
static const char *const native_libs[] = {"-lm", "-lpthread", "-ldl", "-lrt", "-l:libX11.so.6", NULL};
#define EXE_SUFFIX ""
#define LIBRARY_SUFFIX ".so"
#endif

// Web builds: clang's own wasm target, with threads, and the package's
// wasi-libc (as in cmake/wasi-toolchain.cmake and TideFlags.cmake). The page's
// JavaScript implements the GL functions the platform imports, allocates with
// malloc, and makes the memory, which it shares with workers when it can.
// Threads are spelled out (-pthread, --shared-memory) for an installed clang
// older than tide's, which doesn't take them from the target's name.
static const char *const web_link_flags[] = {"-Wl,--allow-undefined", "-Wl,--export=malloc", "-Wl,--export=free",
                                             "-Wl,-z,stack-size=1048576", "-Wl,--import-memory", "-Wl,--export-memory",
                                             "-Wl,--shared-memory", "-Wl,--max-memory=4294967296", NULL};

// The target, when it isn't the system's: its C library comes with tide, in
// <root>/<runtime>/sysroot, and its compiler runtime is passed by path, since
// clang looks for it in its own installation.
typedef struct target {
    const char *flag;   // --target=..., or NULL for the system's
    char *sysroot_flag; // --sysroot=...
    char *builtins;     // The compiler runtime
} target;

static target build_target;

static bool find_target(const char *root, const build_options *opts)
{
    const char *runtime = NULL;
    if (opts->android) return true; // Each of its CPUs, as it's built (build_android)
    if (opts->web) {
        build_target.flag = "--target=wasm32-wasip1-threads";
        runtime = "wasi";
    }
#ifdef NATIVE_TARGET
    else {
        build_target.flag = NATIVE_TARGET;
        runtime = NATIVE_RUNTIME;
    }
#endif
    if (!runtime) return true;
    char *dir = path_join(root, runtime);
    build_target.sysroot_flag = format("--sysroot=%s/sysroot", dir, NULL);
    build_target.builtins = opts->web ? path_join(dir, "libclang_rt.builtins.a")
                                      : path_join(dir, "libclang_rt.builtins-x86_64.a");
    const bool found = sys_exists(build_target.builtins);
    if (!found) {
        fprintf(stderr, "tide: this installation can't build %s games: %s is missing\n", opts->web ? "web" : "native",
                dir);
        fprintf(stderr, "  = note: reinstall tide, or for a build of this repo package the %s preset too\n",
                opts->web ? "web-package" : "mingw-release");
    }
    free(dir);
    return found;
}

// ---------------------------------------------------------------------------
// A build: what it needs, found once

typedef struct build {
    const char *root;
    const build_options *opts;
    bool library; // For `tide run`: the game is a library, which a host reloads (see tide/host.h)
    int abi;      // For Android: which of its CPUs is being built (android_abis)
    char *folder;
    const char *name;
    char *include;
    char *compiler;
    char *wasm_ld; // Web builds with an installed clang
    char *cache;   // <folder>/.tide/<configuration>
    bool engine_built;
    game_info info; // Its settings, from its last generate
    file_list engine;       // The engine's objects
    game_packages packages; // Found again by each build of the game
} build;

static void config_flags(args *a, const build *b)
{
    if (build_target.flag) {
        arg(a, build_target.flag);
        arg(a, build_target.sysroot_flag);
    }
    if (b->opts->web) {
        arg(a, "-pthread");
        arg(a, "-msimd128"); // WebAssembly's SIMD, the same on every machine (not relaxed SIMD)
    }
    arg_list(a, common_flags);
    arg_list(a, b->opts->release ? release_flags : debug_flags);
    // A library's code works wherever it's loaded, and Android's games are libraries.
#if !defined(_WIN32) && !defined(__APPLE__)
    if (b->library || b->opts->android) arg(a, "-fPIC");
#else
    if (b->opts->android) arg(a, "-fPIC");
#endif
#ifdef _WIN32
    // Debug info Visual Studio's debugger reads, in a .pdb next to the game.
    if (!b->opts->web && !b->opts->android && !b->opts->release) arg(a, "-gcodeview");
#endif
}

// The C compiler: an installed clang, or tide itself as `tide cc` when clang
// is built in (compiler/cli/llvm).
static void arg_compiler(args *a, const char *compiler)
{
    arg(a, compiler);
#ifdef TIDE_EMBEDDED_CLANG
    arg(a, "cc");
#endif
}

// One line with everything that decides the engine objects, so a change in
// any of it rebuilds them.
static char *stamp_of(const build *b)
{
    args a = {0};
    config_flags(&a, b);
    size_t n = strlen(b->compiler) + strlen(TIDE_VERSION) + 4;
    for (int i = 0; i < a.count; i++) n += strlen(a.items[i]) + 1;
    char *stamp = malloc(n);
    if (!stamp) abort();
    snprintf(stamp, n, "%s %s", TIDE_VERSION, b->compiler);
    for (int i = 0; i < a.count; i++) {
        strcat(stamp, " ");
        strcat(stamp, a.items[i]);
    }
    free(a.items);
    return stamp;
}

// Compiles one C file; `gen`, the generated files' folder, may be NULL.
static bool compile_c(const build *b, const char *source, const char *object, const char *gen)
{
    args a = {0};
    arg_compiler(&a, b->compiler);
    arg(&a, "-c");
    arg(&a, source);
    arg(&a, "-o");
    arg(&a, object);
    char *include_flag = format("-I%s", b->include, NULL);
    char *gen_flag = gen ? format("-I%s", gen, NULL) : NULL;
    arg(&a, include_flag);
    if (gen_flag) arg(&a, gen_flag);
    config_flags(&a, b);
    const int code = sys_run(a.items, NULL, false);
    free(include_flag);
    free(gen_flag);
    free(a.items);
    if (code == -1) fprintf(stderr, "tide: couldn't start %s\n", b->compiler);
    return code == 0;
}

// Finds the compiler and makes the game's .tide folder. False after saying
// what's wrong.
static bool build_setup(build *b, const char *root, const build_options *opts, const bool library)
{
    b->root = root;
    b->opts = opts;
    b->library = library;
    b->folder = path_absolute(opts->folder);
    b->name = game_name(b->folder);
    b->include = path_join(root, "include");
    char *run_h = path_join(b->include, "tide/run.h");
    if (!sys_exists(run_h)) {
        fprintf(stderr, "tide: the engine files aren't next to tide, in %s\n", root);
        fprintf(stderr, "  = note: reinstall tide, or for a build of this repo run `cmake --install` first\n");
        return false;
    }
    free(run_h);

#ifdef TIDE_EMBEDDED_CLANG
    char *bin = sys_exe_dir();
    b->compiler = path_join(bin, "tide" EXE_SUFFIX);
    free(bin);
#ifdef __APPLE__
    // macOS's headers and libraries come with Apple's command line tools,
    // which clang finds through SDKROOT.
    if (!opts->web && !find_macos_sdk()) {
        fprintf(stderr, "tide: building for macOS needs Apple's command line tools\n");
        fprintf(stderr, "  = note: install them with `xcode-select --install`\n");
        return false;
    }
#endif
#else
    b->compiler = opts->web ? find_web_clang() : find_clang();
    if (!b->compiler) {
        fprintf(stderr, "tide: clang isn't installed, and tide needs it to build games\n");
        fprintf(stderr, "  = note: install LLVM (https://github.com/llvm/llvm-project/releases), add it to PATH, "
                        "or set LLVM_ROOT\n");
        return false;
    }
    if (opts->web) {
        b->wasm_ld = find_wasm_ld(b->compiler);
        if (!b->wasm_ld) {
            fprintf(stderr, "tide: web builds need wasm-ld, the WebAssembly linker, and it isn't next to clang\n");
            fprintf(stderr, "  = note: LLVM's releases include it; on Linux install your distribution's lld, and on "
                            "macOS `brew install llvm lld`\n");
            return false;
        }
    }
#endif
    if (!find_target(root, opts)) return false;

    // Everything tide makes goes in <folder>/.tide/<configuration>, hidden
    // like .git.
    char *tide_dir = path_join(b->folder, ".tide");
    sys_mkdirs(tide_dir);
    sys_hide(tide_dir);
    char *ignore = path_join(tide_dir, ".gitignore");
    if (!sys_exists(ignore)) sys_write_text(ignore, "# Made by tide; safe to delete.\n*\n");
    free(ignore);
    char config[32];
    snprintf(config, sizeof config, "%s-%s", opts->web ? "web" : opts->android ? "android" : "native",
             opts->release ? "release" : "debug");
    b->cache = path_join(tide_dir, config);
    free(tide_dir);
    sys_mkdirs(b->cache);
    return true;
}

// ---------------------------------------------------------------------------
// Engine objects, compiled once per configuration and kept

typedef struct engine_build {
    const build *b;
    const char *out_dir;
    bool rebuild;
    bool ok;
    file_list objects;
} engine_build;

static void compile_engine_file(void *user, const char *source)
{
    engine_build *e = user;
    char name[512];
    snprintf(name, sizeof name, "%s.o", path_base(source));
    char *object = path_join(e->out_dir, name);
    if (e->ok && (e->rebuild || sys_mtime(object) < sys_mtime(source))) {
        e->ok = compile_c(e->b, source, object, NULL);
    }
    add_file(&e->objects, object);
    free(object);
}

// Rebuilt when tide, the compiler or the flags change.
static bool build_engine(build *b)
{
    if (b->engine_built) return true;
#if !defined(_WIN32) && !defined(__APPLE__)
    // Objects for a library have flags of their own (config_flags): kept apart,
    // so running and building don't rebuild each other's.
    char *engine_dir = path_join(b->cache, b->library ? "engine-pic" : "engine");
#else
    char *engine_dir = path_join(b->cache, "engine");
#endif
    sys_mkdirs(engine_dir);
    char *stamp_path = path_join(engine_dir, "stamp");
    char *stamp = stamp_of(b);
    char *old_stamp = sys_read_file(stamp_path, NULL);
    engine_build engine = {b, engine_dir, !old_stamp || strcmp(old_stamp, stamp) != 0, true, {0}};
    char *engine_src = path_join(b->root, "src/engine/");
    folder_find(engine_src, ".c", compile_engine_file, &engine);
    free(engine_src);
    if (engine.ok) sys_write_text(stamp_path, stamp);
    free(stamp_path);
    free(stamp);
    free(old_stamp);
    free(engine_dir);
    b->engine = engine.objects;
    b->engine_built = engine.ok;
    return engine.ok;
}

// ---------------------------------------------------------------------------
// The game's C: the .c files in its folder, which define its extern functions,
// and the prebuilt libraries there that were built for the target (see
// libraries.h).

typedef struct c_side {
    file_list objects; // Its .c files, compiled
    file_list link;    // Its libraries for the target, as the linker takes them
    file_list dynamic; // ...those the program loads as it starts, which go next to it
    file_list names;   // On Android, the name each of those goes in the app by
    file_list unsure;  // On Android, ELF libraries for its CPU left out as Linux's
} c_side;

static void free_game_c(c_side *c)
{
    free_files(&c->objects);
    free_files(&c->link);
    free_files(&c->dynamic);
    free_files(&c->names);
    free_files(&c->unsure);
}

// Android's CPUs, in android_abis' order
static const unsigned android_cpus[ANDROID_ABIS] = {LIB_ARM64, LIB_X64};

static lib_platform target_platform(const build *b)
{
    if (b->opts->web) return LIB_WEB;
    if (b->opts->android) return LIB_ANDROID;
#ifdef _WIN32
    return LIB_WINDOWS;
#elif defined(__APPLE__)
    return LIB_MACOS;
#else
    return LIB_LINUX;
#endif
}

static unsigned target_cpu(const build *b)
{
    if (b->opts->web) return LIB_WASM32;
    if (b->opts->android) return android_cpus[b->abi];
#if defined(_WIN32) || defined(__x86_64__)
    return LIB_X64; // Windows games are always x86-64 (NATIVE_TARGET)
#elif defined(__aarch64__) || defined(__arm64__)
    return LIB_ARM64;
#else
    return LIB_OTHER_CPU;
#endif
}

static uint64_t hash_text(uint64_t h, const char *text)
{
    for (const char *p = text; *p; p++) h = (h ^ (uint8_t)*p) * 0x100000001B3ull;
    return h;
}

// A file's path and its stamp, which changes whenever it does.
static uint64_t hash_file(const uint64_t h, const char *path)
{
    return (hash_text(h, path) ^ sys_file_stamp(path)) * 0x100000001B3ull;
}

// Compiles the .c files of the game, or of one of its packages (`package`,
// its name), in `folder_path` into a folder of the cache, each one again only
// when it, a header in that folder, or the flags changed: each object has a
// stamp of what made it. Objects for a library are kept apart on Linux, as
// the engine's are.
static bool compile_c_files(const build *b, const char *folder_path, const char *package, c_side *out)
{
    file_list sources = own_files_of(folder_path, c_extensions);
    if (sources.count == 0) return true;
#if !defined(_WIN32) && !defined(__APPLE__)
    char *dir = path_join(b->cache, b->library ? "c-pic" : "c");
#else
    char *dir = path_join(b->cache, "c");
#endif
    if (package) {
        char sub[512];
        snprintf(sub, sizeof sub, "packages/%s", package);
        char *base = dir;
        dir = path_join(base, sub);
        free(base);
    }
    sys_mkdirs(dir);
    char *flags = stamp_of(b);
    uint64_t common = hash_text(0xCBF29CE484222325ull, flags);
    free(flags);
    file_list headers = own_files_of(folder_path, header_extensions);
    for (int i = 0; i < headers.count; i++) common = hash_file(common, headers.items[i]);
    free_files(&headers);
    char *folder = path_join(folder_path, "");
    const size_t prefix = strlen(folder);
    free(folder);
    bool ok = true;
    for (int i = 0; i < sources.count && ok; i++) {
        char name[512]; // lib/noise.c: lib_noise.c.o
        snprintf(name, sizeof name, "%s.o", sources.items[i] + prefix);
        for (char *p = name; *p; p++) {
            if (*p == '/') *p = '_';
        }
        char *object = path_join(dir, name);
        char *stamp_path = format("%s.stamp", object, NULL);
        char stamp[32];
        snprintf(stamp, sizeof stamp, "%016llx", (unsigned long long)hash_file(common, sources.items[i]));
        char *old = sys_read_file(stamp_path, NULL);
        if (!old || strcmp(old, stamp) != 0 || !sys_exists(object)) {
            ok = compile_c(b, sources.items[i], object, NULL);
            if (ok) sys_write_text(stamp_path, stamp);
            else sys_remove(stamp_path);
        }
        add_file(&out->objects, object);
        free(old);
        free(stamp_path);
        free(object);
    }
    free(dir);
    free_files(&sources);
    return ok;
}

// The game's C, and its packages'.
static bool compile_game_c(const build *b, c_side *out)
{
    bool ok = compile_c_files(b, b->folder, NULL, out);
    for (int p = 0; p < b->packages.count && ok; p++) {
        ok = compile_c_files(b, b->packages.items[p].folder, b->packages.items[p].name, out);
    }
    return ok;
}

// Links to the ELF shared library at `path` by its name, so the program asks
// for it by name, wherever the two are put.
static void link_by_name(c_side *out, const char *path)
{
    char *dir = path_dir(path);
    char *search = format("-L%s", dir, NULL);
    char *name = format("-l:%s", path_base(path), NULL);
    add_file(&out->link, search);
    add_file(&out->link, name);
    free(dir);
    free(search);
    free(name);
}

// The pages of Android's newer phones: a library loads there when its
// segments are aligned to them (the game's own is: link_android).
#define ANDROID_PAGE_SIZE 16384

// A shared library for the Android app: linked to, and put in the app beside
// the game's library under the name that one asks for it by, which is its
// soname, or its file's name when it has none. An app's libraries are named
// lib<name>.so, one of a name for each CPU. False after saying why it can't
// go in.
static bool add_android_library(const build *b, const char *path, const lib_info *info, c_side *out)
{
    const char *name = info->name[0] ? info->name : path_base(path);
    const size_t n = strlen(name);
    if (n < 7 || strncmp(name, "lib", 3) != 0 || strcmp(name + n - 3, ".so") != 0) {
        fprintf(stderr, "tide: %s can't go in an Android app: its libraries are named lib<name>.so, and this one's "
                        "name is %s\n", path, name);
        fprintf(stderr, info->name[0] ? "  = note: that's the name it was linked with, which the game asks for it by; "
                                        "link it again with -Wl,-soname,lib<name>.so\n"
                                      : "  = note: rename the file\n");
        return false;
    }
    if (strcmp(name, "libgame.so") == 0) {
        fprintf(stderr, "tide: %s can't go in an Android app: libgame.so is the game's own library there\n", path);
        return false;
    }
    for (int i = 0; i < out->names.count; i++) {
        if (strcmp(out->names.items[i], name) != 0) continue;
        fprintf(stderr, "tide: two libraries for %s are named %s: %s and %s\n", android_abis[b->abi], name,
                out->dynamic.items[i], path);
        fprintf(stderr, "  = note: an Android app holds one library of a name for each CPU\n");
        return false;
    }
    if (info->page_size && info->page_size < ANDROID_PAGE_SIZE) {
        fprintf(stderr, "tide: warning: %s is aligned for %u KiB pages, and phones with 16 KiB pages may not load it\n",
                path, (unsigned)(info->page_size / 1024));
        fprintf(stderr, "  = note: Google Play requires apps to run on those phones; build the library with NDK r28 or "
                        "newer, or link it with -Wl,-z,max-page-size=16384\n");
    }
    add_file(&out->dynamic, path);
    add_file(&out->names, name);
    link_by_name(out, path);
    return true;
}

// The libraries in `folder` (the game's or a package's) built for the target:
// static ones (and Windows import libraries) are linked in, and dynamic ones
// are linked to and go next to the program, which finds them there (on
// Android, in the app). A library tide can't read is left out, saying so; one
// for another platform or CPU is left out quietly. False after saying why a
// library for the target can't be used.
static bool find_libraries_in(const build *b, const char *folder, c_side *out)
{
    file_list libraries = own_files_of(folder, library_extensions);
    const lib_platform platform = target_platform(b);
    const unsigned cpu = target_cpu(b);
    bool dynamic = false;
    bool ok = true;
    for (int i = 0; i < libraries.count && ok; i++) {
        const char *path = libraries.items[i];
        const lib_info info = lib_identify_file(path);
        if (info.platform == LIB_UNKNOWN) {
            fprintf(stderr, "tide: left out %s: tide can't tell which platform it was built for\n", path);
            continue;
        }
        // Linux's and Android's are both ELF (see libraries.h): if the link
        // fails, one taken for Linux's may be why
        if (platform == LIB_ANDROID && info.platform == LIB_LINUX && (info.cpus & cpu)) add_file(&out->unsure, path);
        if (info.platform != platform || !(info.cpus & cpu)) continue;
        if (!info.dynamic) {
            add_file(&out->link, path);
            continue;
        }
        if (platform == LIB_ANDROID) {
            ok = add_android_library(b, path, &info, out);
            continue;
        }
        add_file(&out->dynamic, path);
        dynamic = true;
#ifdef _WIN32
        // Linked through its import library (a .lib, static above): the .dll only goes next to the game
#elif defined(__APPLE__)
        add_file(&out->link, path);
#else
        link_by_name(out, path); // So the program looks for it by name: next to itself
#endif
    }
#ifdef __APPLE__
    if (dynamic) add_file(&out->link, "-Wl,-rpath,@loader_path");
#elif !defined(_WIN32)
    if (dynamic) add_file(&out->link, "-Wl,-rpath,$ORIGIN");
#else
    (void)dynamic;
#endif
    free_files(&libraries);
    return ok;
}

// The game's libraries, and its packages'.
static bool find_libraries(const build *b, c_side *out)
{
    bool ok = find_libraries_in(b, b->folder, out);
    for (int p = 0; p < b->packages.count && ok; p++) ok = find_libraries_in(b, b->packages.items[p].folder, out);
    return ok;
}

// Copies the game's dynamic libraries into `dir`, next to its program. Ones
// already there and up to date stay: the running game may have them open.
static void copy_dynamic(const c_side *c, const char *dir)
{
    for (int i = 0; i < c->dynamic.count; i++) {
        const char *from = c->dynamic.items[i];
        char *to = path_join(dir, path_base(from));
        const bool current = sys_file_size(to) == sys_file_size(from) && sys_mtime(to) >= sys_mtime(from);
        if (strcmp(from, to) != 0 && !current) {
            size_t len = 0;
            char *data = sys_read_file(from, &len);
            if (!data || !sys_write_file(to, data, len)) {
                fprintf(stderr, "tide: can't copy %s next to the game, to %s\n", from, to);
            }
            free(data);
        }
        free(to);
    }
}

// Links `objects`, the game's C (`c`, NULL for tide run's host) and the
// engine's objects into `output`: a program, with the platform layer, or with
// `shared`, a library of the game for a host to load (see tide/host.h), whose
// debug info on Windows goes in `pdb`.
static bool link_objects(const build *b, const file_list *objects, const c_side *c, const char *output,
                         const bool shared, const char *pdb)
{
    const build_options *opts = b->opts;
    args a = {0};
    arg_compiler(&a, b->compiler);
    if (build_target.flag) {
        arg(&a, build_target.flag);
        arg(&a, build_target.sysroot_flag);
    }
    if (shared) arg(&a, "-shared");
    for (int i = 0; i < objects->count; i++) arg(&a, objects->items[i]);
    for (int i = 0; c && i < c->objects.count; i++) arg(&a, c->objects.items[i]);
    for (int i = 0; c && i < c->link.count; i++) arg(&a, c->link.items[i]);
    for (int i = 0; i < b->engine.count; i++) arg(&a, b->engine.items[i]);
    // The prebuilt platform layer, raylib inside (see platform/CMakeLists.txt).
    char *lib_dir = path_join(b->root, opts->web ? "lib/web" : "lib/native");
    char *platform_lib = format("%s/lib%s.a", lib_dir, "tide_platform");
    char *raylib_lib = format("%s/lib%s.a", lib_dir, "raylib");
    if (!shared) {
        arg(&a, platform_lib);
        arg(&a, raylib_lib);
    }
    arg(&a, "-o");
    arg(&a, output);
    arg_list(&a, opts->release ? release_flags : debug_flags);
    char *pdb_flag = NULL;
    char *ld_flag = NULL;
    if (opts->web) {
        arg_list(&a, web_link_flags);
#ifndef TIDE_EMBEDDED_CLANG
        ld_flag = format("-fuse-ld=%s", b->wasm_ld, NULL); // It may be elsewhere than clang, as with Homebrew
        arg(&a, ld_flag);
#endif
    } else {
#if defined(TIDE_EMBEDDED_CLANG) && !defined(_WIN32)
        arg(&a, "-fuse-ld=lld"); // The built-in linker
#endif
#ifdef _WIN32
        arg(&a, "-fuse-ld=lld");
        if (shared) {
            if (pdb) {
                pdb_flag = format("-Wl,--pdb=%s", pdb, NULL);
                arg(&a, pdb_flag);
            }
        } else if (!opts->release) {
            arg(&a, "-Wl,--pdb="); // A debug game gets its debug info in a .pdb
        } else if (!b->library) {
            // A release game opens its window without a console next to it. tide
            // run's host keeps tide's, to say what each reload did.
            arg(&a, "-Wl,--subsystem,windows");
        }
#else
        (void)pdb;
#endif
        arg_list(&a, native_libs);
    }
    // A target's C library, when it comes with tide: its libraries by name
    // (on the web, just the C library) and the compiler runtime by path.
    if (build_target.flag) {
        arg(&a, "-nodefaultlibs");
        if (opts->web) arg(&a, "-lc");
        arg(&a, build_target.builtins);
    }
    const int code = sys_run(a.items, NULL, false);
    free(a.items);
    free(lib_dir);
    free(platform_lib);
    free(raylib_lib);
    free(pdb_flag);
    free(ld_flag);
    if (code == -1) fprintf(stderr, "tide: couldn't start %s\n", b->compiler);
    return code == 0;
}

// ---------------------------------------------------------------------------
// The web page: the package's shell, with a <script> holding the program as
// base64 and tide.js, which runs it. The same as cmake/web_page.mjs.

static char *base64(const unsigned char *data, const size_t len)
{
    static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *out = malloc((len + 2) / 3 * 4 + 1);
    if (!out) abort();
    size_t n = 0;
    for (size_t i = 0; i < len; i += 3) {
        const unsigned v = (unsigned)data[i] << 16 | (i + 1 < len ? (unsigned)data[i + 1] << 8 : 0)
                         | (i + 2 < len ? data[i + 2] : 0);
        out[n++] = digits[v >> 18 & 63];
        out[n++] = digits[v >> 12 & 63];
        out[n++] = i + 1 < len ? digits[v >> 6 & 63] : '=';
        out[n++] = i + 2 < len ? digits[v & 63] : '=';
    }
    out[n] = '\0';
    return out;
}

// The first line from `from` that's `marker` alone (indented or not), or NULL.
// `after` gets the start of the line after it.
static char *marker_line(char *from, const char *marker, char **after)
{
    const size_t n = strlen(marker);
    for (char *line = from; line && *line;) {
        char *text = line;
        while (*text == ' ' || *text == '\t') text++;
        char *end = text + n;
        if (strncmp(text, marker, n) == 0 && (*end == '\n' || (*end == '\r' && end[1] == '\n'))) {
            *after = end + (*end == '\r' ? 2 : 1);
            return line;
        }
        line = strchr(line, '\n');
        if (line) line++;
    }
    return NULL;
}

// Leaves out the lines tide.js has for tide run's hot reloading, from a
// `// <tide run>` line to a `// </tide run>` one: pages made to ship don't
// have them.
static void strip_reloading(char *script)
{
    char *after_start = NULL;
    char *after_end = NULL;
    for (char *start; (start = marker_line(script, "// <tide run>", &after_start));) {
        if (!marker_line(after_start, "// </tide run>", &after_end)) return;
        memmove(start, after_end, strlen(after_end) + 1);
    }
}

static bool make_page(const char *root, const char *program, const char *page)
{
    char *shell_path = path_join(root, "web/shell.html");
    char *script_path = path_join(root, "web/tide.js");
    char *shell = sys_read_file(shell_path, NULL);
    char *script = sys_read_file(script_path, NULL);
    size_t wasm_len = 0;
    char *wasm = sys_read_file(program, &wasm_len);
    const char *slot = shell ? strstr(shell, "{{{ SCRIPT }}}") : NULL;
    if (!script || !wasm || !slot) {
        fprintf(stderr, "tide: the web page's files are missing from %s/web; reinstall tide\n", root);
        return false;
    }
    strip_reloading(script);
    char *encoded = base64((const unsigned char *)wasm, wasm_len);
    FILE *f = fopen(page, "wb");
    if (!f) {
        fprintf(stderr, "tide: can't write %s\n", page);
        return false;
    }
    fwrite(shell, 1, (size_t)(slot - shell), f);
    fprintf(f, "<script>\nconst TIDE_PROGRAM = \"%s\";\n%s</script>", encoded, script);
    fputs(slot + strlen("{{{ SCRIPT }}}"), f);
    const bool ok = fclose(f) == 0;
    free(encoded);
    free(wasm);
    return ok;
}

// ---------------------------------------------------------------------------

// Finds the game's packages and generates its C. With `layout`, the generated
// code describes the data layout too, for hot reloading. `externs` gets the C
// function of each extern function, a line each, until the next build.
static bool generate(build *b, const char *gen, const bool layout, sb *externs)
{
    if (!find_packages(b->folder, &b->packages)) return false;
    file_list paths;
    int count = 0;
    compile_input *inputs = game_inputs(b->folder, &b->packages, &paths, &count);
    if (!inputs) return false;
    arena_reset(); // What an earlier build compiled, when a run rebuilds the game
    *externs = (sb){0};
    const codegen_options codegen = {"game", gen, true, layout, externs, &b->info};
    const bool ok = compile_inputs(inputs, count, &codegen, NULL);
    free(inputs);
    free_files(&paths);
    return ok;
}

typedef struct web_imports {
    const char *externs; // A C function a line
    int missing;
} web_imports;

static void check_import(void *user, const char *name, const size_t len)
{
    web_imports *w = user;
    for (const char *line = w->externs; *line;) {
        const char *end = strchr(line, '\n');
        if ((size_t)(end - line) == len && memcmp(line, name, len) == 0) {
            fprintf(stderr, "tide: nothing defines the C function %.*s for the web\n", (int)len, name);
            w->missing++;
        }
        line = end + 1;
    }
}

// On the web, a C function nothing defines isn't a link error: the program
// imports it from the page, which fails when it's called. So the game's extern
// functions among its imports are errors. False after saying so.
static bool check_web_externs(const char *program, const sb *externs)
{
    if (!externs->data || externs->len == 0) return true;
    size_t size = 0;
    char *wasm = sys_read_file(program, &size);
    web_imports w = {externs->data, 0};
    if (wasm) wasm_function_imports((const unsigned char *)wasm, size, "env", check_import, &w);
    free(wasm);
    if (w.missing > 0) {
        fprintf(stderr, "  = note: the game's C files and WebAssembly libraries (.a) define its C functions on the web; "
                        "for one that only exists natively, write a stand-in inside '#ifdef __wasm__'\n");
    }
    return w.missing == 0;
}

static char *build_android(const char *root, const build_options *opts, char *package, size_t package_size);

char *tide_build(const char *root, const build_options *opts)
{
    if (opts->android) return build_android(root, opts, NULL, 0);
    build b = {0};
    if (!build_setup(&b, root, opts, false)) return NULL;
    char *gen = path_join(b.cache, "gen");
    sys_mkdirs(gen);

    sb externs;
    if (!generate(&b, gen, false, &externs)) return NULL;
    char *main_c = path_join(gen, "main.c");
    write_main(main_c, opts, b.name);
    if (!build_engine(&b)) return NULL;

    char *game_c = path_join(gen, "game.c");
    file_list objects = {0};
    add_file(&objects, path_join(b.cache, "game.o"));
    add_file(&objects, path_join(b.cache, "main.o"));
    if (!compile_c(&b, game_c, objects.items[0], gen)) return NULL;
    if (!compile_c(&b, main_c, objects.items[1], gen)) return NULL;
    c_side c = {0};
    if (!compile_game_c(&b, &c) || !find_libraries(&b, &c)) return NULL;

    char *output;
    const char *suffix = opts->web ? ".html" : EXE_SUFFIX;
    if (opts->output) {
        output = path_absolute(opts->output);
    } else {
        char file[512];
        snprintf(file, sizeof file, "%s%s", b.name, suffix);
        char *dir = path_join(b.folder, "build");
        output = path_join(dir, file);
        free(dir);
    }
    char *output_dir = path_dir(output);
    sys_mkdirs(output_dir);
    // Web builds link a .wasm, which then goes inside the page.
    char *program = output;
    if (opts->web) {
        char file[512];
        snprintf(file, sizeof file, "%s.wasm", b.name);
        program = path_join(b.cache, file);
    }
    if (!link_objects(&b, &objects, &c, program, false, NULL)) return NULL;
    if (opts->web && (!check_web_externs(program, &externs) || !make_page(root, program, output))) return NULL;
    copy_dynamic(&c, output_dir);
    free_game_c(&c);
    return output;
}

// ---------------------------------------------------------------------------
// `tide run`: the game as a library in a small host program, which swaps in
// each new build of it (see tide/host.h), and a new build whenever one of the
// game's files changes. Each run has a folder of its own, .tide/<configuration>/run/
// <tide's process ID>, so two runs of one game (a server and a client) never
// build over each other.

typedef struct run {
    build b;
    bool web;        // Each build is a web program the page starts, not a library
    char *dir;       // This run's folder
    char *gen;       // ...and its generated files
    uint32_t builds; // Made so far, game-1 to game-<builds>
} run;

static char *build_path(const run *r, const uint32_t n, const char *suffix)
{
    char name[64];
    snprintf(name, sizeof name, "game-%u%s", (unsigned)n, suffix);
    return path_join(r->dir, name);
}

// Builds the game's next library (or web program), game-<builds + 1>, linked
// to another name and renamed once it's whole: the host (or the page) takes
// it as soon as it's there, and says what it did with it. False after saying
// what's wrong.
static bool build_library(run *r)
{
    sb externs;
    if (!generate(&r->b, r->gen, true, &externs) || !build_engine(&r->b)) return false;
    const char *suffix = r->web ? ".wasm" : LIBRARY_SUFFIX;
    char *game_c = path_join(r->gen, "game.c");
    char *library_c = path_join(r->gen, r->web ? "main.c" : "library.c");
    file_list objects = {0};
    add_file(&objects, path_join(r->dir, "game.o"));
    add_file(&objects, path_join(r->dir, r->web ? "main.o" : "library.o"));
    c_side c = {0};
    bool ok = compile_c(&r->b, game_c, objects.items[0], r->gen)
           && compile_c(&r->b, library_c, objects.items[1], r->gen) && compile_game_c(&r->b, &c)
           && find_libraries(&r->b, &c);
    if (ok) copy_dynamic(&c, r->dir); // Next to the host, which loads the game's library
    const uint32_t n = r->builds + 1u;
    char *linked = build_path(r, n, ".tmp");
    char *pdb = build_path(r, n, ".pdb");
    char *library = build_path(r, n, suffix);
    ok = ok && link_objects(&r->b, &objects, &c, linked, !r->web, r->b.opts->release || r->web ? NULL : pdb);
    ok = ok && (!r->web || check_web_externs(linked, &externs));
    free_game_c(&c);
    if (ok && !sys_rename(linked, library)) {
        fprintf(stderr, "tide: can't write %s\n", library);
        ok = false;
    }
    if (ok) {
        r->builds = n;
        // The one before the last is done with; if the host still has it, it
        // stays until the run ends.
        if (n > 2) {
            char *old = build_path(r, n - 2u, suffix);
            char *old_pdb = build_path(r, n - 2u, ".pdb");
            sys_remove(old);
            sys_remove(old_pdb);
            free(old);
            free(old_pdb);
        }
    }
    free(game_c);
    free(library_c);
    free_files(&objects);
    free(linked);
    free(pdb);
    free(library);
    return ok;
}

// The host: tide/host.h's loop, reloading the libraries in the run's folder.
static bool build_host(run *r, const char *program)
{
    char title[600];
    char dir[2048];
    title_field(title, sizeof title, r->b.opts, r->b.name);
    c_string(dir, sizeof dir, r->dir);
    char text[4096];
    snprintf(text, sizeof text,
             "// Generated by tide. Do not edit.\n"
             "#include \"tide/host.h\"\n"
             "\n"
             "int main(int argc, char **argv)\n"
             "{\n"
             "    tide_host_run_library(&(tide_run_desc){%s, .stats = %s, .argc = argc, .argv = argv},\n"
             "                          \"%s\");\n"
             "}\n",
             title, r->b.opts->stats ? "true" : "false", dir);
    char *host_c = path_join(r->gen, "host.c");
    file_list objects = {0};
    add_file(&objects, path_join(r->dir, "host.o"));
    const bool ok = sys_write_text(host_c, text) && compile_c(&r->b, host_c, objects.items[0], NULL)
                 && link_objects(&r->b, &objects, NULL, program, false, NULL);
    free(host_c);
    free_files(&objects);
    return ok;
}

// What the host loads from each library (see tide/host.h): the game, with its
// data layout.
static bool write_library(const run *r)
{
    char *path = path_join(r->gen, "library.c");
    const bool ok = sys_write_text(path, "// Generated by tide. Do not edit.\n"
                                         "#include \"game.h\"\n"
                                         "#include \"tide/run.h\"\n"
                                         "\n"
                                         "extern const tide_layout tide_game_layout;\n"
                                         "\n"
                                         "TIDE_HOST_EXPORT const tide_host_game *tide_host_library(void)\n"
                                         "{\n"
                                         "    static tide_host_game game;\n"
                                         "    game = tide_host_game_api;\n"
                                         "    game.layout = &tide_game_layout;\n"
                                         "    return &game;\n"
                                         "}\n");
    free(path);
    return ok;
}

// Removes the folders of runs whose tide has ended without cleaning up.
static void remove_ended_run(void *user, const char *name, const bool is_dir)
{
    const char *runs = user;
    char *end = NULL;
    const unsigned long pid = strtoul(name, &end, 10);
    if (!is_dir || end == name || *end || sys_process_alive((uint32_t)pid)) return;
    char *path = path_join(runs, name);
    sys_remove_tree(path);
    free(path);
}

static void stamp_file(void *user, const char *path)
{
    uint64_t *h = user;
    for (const char *p = path; *p; p++) *h = (*h ^ (uint8_t)*p) * 0x100000001B3ull;
    *h = (*h ^ sys_file_stamp(path)) * 0x100000001B3ull;
}

// Stamps the .tide files in `folder`, its C, headers and libraries, and its
// tide.packages.
static void stamp_folder(uint64_t *h, const char *folder)
{
    char *dir = path_join(folder, "");
    folder_find_own(dir, ".tide", stamp_file, h);
    char *packages = path_join(dir, PACKAGES_FILE);
    stamp_file(h, packages);
    free(packages);
    free(dir);
    file_list others = own_files_of(folder, c_side_extensions);
    for (int i = 0; i < others.count; i++) stamp_file(h, others.items[i]);
    free_files(&others);
}

// Changes whenever one of the game's .tide files does, or its C, headers and
// libraries, or one comes or goes; or its tide.packages, or the files of a
// package in a folder on this machine. Those from git, at their commits,
// never change.
static uint64_t game_stamp(const build *b)
{
    uint64_t h = 0xCBF29CE484222325ull;
    stamp_folder(&h, b->folder);
    for (int p = 0; p < b->packages.count; p++) {
        if (b->packages.items[p].line->local) stamp_folder(&h, b->packages.items[p].folder);
    }
    return h;
}

// A change is built once the files have stayed the same this long, as
// editors can save in steps.
#define SETTLE_MS 200

// This run's folder, made anew, after removing those of runs that ended.
static void open_run(run *r)
{
    char *runs = path_join(r->b.cache, "run");
    sys_mkdirs(runs);
    sys_list(runs, remove_ended_run, (void *)runs);
    char pid[32];
    snprintf(pid, sizeof pid, "%u", (unsigned)sys_pid());
    r->dir = path_join(runs, pid);
    r->gen = path_join(r->dir, "gen");
    sys_remove_tree(r->dir); // A run that ended with the same process ID
    sys_mkdirs(r->gen);
    free(runs);
}

// Whether a line typed into tide's terminal asks to start the game over,
// saying so either way.
static bool typed_restart(void)
{
    char line[64];
    if (!sys_typed_line(line, sizeof line)) return false;
    const bool restart = strcmp(line, "r") == 0;
    if (restart) printf("tide: started over\n");
    else if (line[0]) printf("tide: type r and press Enter to start the game over\n");
    fflush(stdout);
    return restart;
}

int tide_run_reloading(const char *root, const build_options *opts, const char *const *game_args)
{
    run r = {0};
    if (!build_setup(&r.b, root, opts, true)) return 1;
    open_run(&r);

    char file[512];
    snprintf(file, sizeof file, "%s%s", r.b.name, EXE_SUFFIX);
    char *program = path_join(r.dir, file);
    if (!write_library(&r) || !build_library(&r) || !build_host(&r, program)) return 1;

    args a = {0};
    arg(&a, program);
    for (int i = 0; game_args[i]; i++) arg(&a, game_args[i]);
    sys_process *game = sys_start(a.items, r.b.folder);
    free(a.items);
    if (!game) {
        fprintf(stderr, "tide: couldn't start %s\n", program);
        return 1;
    }
    printf("tide: saving a .tide or C file reloads the game; type r and press Enter to start it over\n");
    fflush(stdout);
    sys_read_lines();

    uint64_t stamp = game_stamp(&r.b);
    int64_t changed_at = -1;
    int code = 0;
    while (!sys_wait(game, 250, &code)) {
        if (typed_restart()) {
            char *restart = path_join(r.dir, "restart");
            sys_write_text(restart, "");
            free(restart);
        }
        const uint64_t now = game_stamp(&r.b);
        if (now != stamp) {
            stamp = now;
            changed_at = sys_now_ms();
            continue;
        }
        if (changed_at < 0 || sys_now_ms() - changed_at < SETTLE_MS) continue;
        changed_at = -1;
        if (!build_library(&r)) fprintf(stderr, "tide: the game keeps running its last build\n");
    }
    sys_remove_tree(r.dir);
    return code;
}

// ---------------------------------------------------------------------------
// `tide run --web`: tide serves the game's page on this machine, which asks
// it for the newest build four times a second and starts each new one in the
// running one's place (platform/web/tide.js).

typedef struct web_run {
    run r;
    char *page;
    uint32_t restarts; // Times `r` was typed: the page starts the game over when it changes
    char status[32];
    char *program; // The newest build, read when first asked for
    size_t program_size;
    uint32_t program_build;
} web_run;

// The game's program for the page: tide/host.h's loop, going on from where
// the last build left it.
static bool write_web_main(const run *r)
{
    char title[600];
    title_field(title, sizeof title, r->b.opts, r->b.name);
    char text[2048];
    snprintf(text, sizeof text,
             "// Generated by tide. Do not edit.\n"
             "#include \"game.h\"\n"
             "#include \"tide/run.h\"\n"
             "\n"
             "extern const tide_layout tide_game_layout;\n"
             "\n"
             "int main(int argc, char **argv)\n"
             "{\n"
             "    static tide_host_game game;\n"
             "    game = tide_host_game_api;\n"
             "    game.layout = &tide_game_layout;\n"
             "    tide_host_run_web(&(tide_run_desc){%s, .stats = %s, .argc = argc, .argv = argv}, &game);\n"
             "}\n",
             title, r->b.opts->stats ? "true" : "false");
    char *path = path_join(r->gen, "main.c");
    const bool ok = sys_write_text(path, text);
    free(path);
    return ok;
}

// The page: the package's shell, and tide.js as it is, hot reloading and all,
// which loads the program from tide. `args` (--host, --join, --connect, ending with NULL)
// go to the game.
static char *web_page(const char *root, const char *const *args)
{
    char *shell_path = path_join(root, "web/shell.html");
    char *script_path = path_join(root, "web/tide.js");
    char *shell = sys_read_file(shell_path, NULL);
    char *script = sys_read_file(script_path, NULL);
    free(shell_path);
    free(script_path);
    const char *slot = shell ? strstr(shell, "{{{ SCRIPT }}}") : NULL;
    if (!script || !slot) {
        fprintf(stderr, "tide: the web page's files are missing from %s/web; reinstall tide\n", root);
        return NULL;
    }
    // Tide.arguments = ["--join", "K7QF2M"]: letters, digits and dots, so
    // anything else is left out rather than escaped.
    char before[512] = "<script>\nTide.reload = true;\nTide.arguments = [";
    for (int i = 0; args && args[i]; i++) {
        size_t at = strlen(before);
        if (at + strlen(args[i]) + 8 >= sizeof before) break;
        before[at++] = i ? ',' : ' ';
        before[at++] = '"';
        for (const char *c = args[i]; *c; c++) {
            if (*c != '"' && *c != '\\' && *c != '<' && (unsigned char)*c >= ' ') before[at++] = *c;
        }
        before[at++] = '"';
        before[at] = '\0';
    }
    strcat(before, "];\n");
    const char *after = "</script>";
    const size_t n = (size_t)(slot - shell) + strlen(before) + strlen(script) + strlen(after)
                   + strlen(slot + strlen("{{{ SCRIPT }}}")) + 1;
    char *page = malloc(n);
    if (!page) abort();
    snprintf(page, n, "%.*s%s%s%s%s", (int)(slot - shell), shell, before, script, after, slot + strlen("{{{ SCRIPT }}}"));
    free(shell);
    free(script);
    return page;
}

static void answer_web(void *user, const char *path, serve_reply *reply)
{
    web_run *w = user;
    char newest[64];
    snprintf(newest, sizeof newest, "/game-%u.wasm", (unsigned)w->r.builds);
    if (strcmp(path, "/") == 0) {
        *reply = (serve_reply){200, "text/html; charset=utf-8", w->page, strlen(w->page)};
    } else if (strcmp(path, "/build") == 0) {
        snprintf(w->status, sizeof w->status, "%u %u", (unsigned)w->r.builds, (unsigned)w->restarts);
        *reply = (serve_reply){200, "text/plain; charset=utf-8", w->status, strlen(w->status)};
    } else if (strcmp(path, newest) == 0) {
        if (w->program_build != w->r.builds) {
            char *file = build_path(&w->r, w->r.builds, ".wasm");
            free(w->program);
            w->program = sys_read_file(file, &w->program_size);
            w->program_build = w->program ? w->r.builds : 0;
            free(file);
        }
        if (w->program) *reply = (serve_reply){200, "application/wasm", w->program, w->program_size};
    }
}

int tide_run_web(const char *root, const build_options *opts, const bool open_page, const char *const *args)
{
    web_run w = {0};
    run *r = &w.r;
    r->web = true;
    if (!build_setup(&r->b, root, opts, false)) return 1;
    open_run(r);
    w.page = web_page(root, args);
    if (!w.page || !write_web_main(r) || !build_library(r)) return 1;

    uint16_t port = 0;
    serve *server = serve_open(&port);
    if (!server) {
        fprintf(stderr, "tide: can't serve the game's page on this machine\n");
        return 1;
    }
    char url[64];
    snprintf(url, sizeof url, "http://127.0.0.1:%u/", (unsigned)port);
    printf("tide: the game is at %s\n", url); // The VS Code extension reads the address from this line
    printf("tide: saving a .tide or C file reloads it; type r and press Enter to start it over, and Ctrl+C to stop\n");
    fflush(stdout);
    if (open_page && !sys_open_in_browser(url)) fprintf(stderr, "tide: couldn't open a browser; open %s in one\n", url);
    sys_read_lines();

    uint64_t stamp = game_stamp(&r->b);
    int64_t changed_at = -1;
    for (;;) {
        serve_poll(server, 250, answer_web, &w);
        if (typed_restart()) w.restarts++;
        const uint64_t now = game_stamp(&r->b);
        if (now != stamp) {
            stamp = now;
            changed_at = sys_now_ms();
            continue;
        }
        if (changed_at < 0 || sys_now_ms() - changed_at < SETTLE_MS) continue;
        changed_at = -1;
        if (build_library(r)) printf("tide: built it again; the page reloads it\n");
        else fprintf(stderr, "tide: the page keeps running its last build\n");
        fflush(stdout);
    }
}

// ---------------------------------------------------------------------------
// Android: the game as a library for each of Android's CPUs, which its
// NativeActivity loads (see platform/android), in an app tide signs itself
// (apk.h).

// The app's ID: its appId setting, or dev.tide.<its name> to test with, since
// an app store would take that one from whoever got there first.
static bool app_id(const build *b, char *out, const size_t size)
{
    if (b->info.app_id[0]) {
        snprintf(out, size, "%s", b->info.app_id);
        return true;
    }
    char name[128];
    size_t n = 0;
    for (const char *c = b->name; *c && n < sizeof name - 1; c++) {
        if (isalnum((unsigned char)*c)) name[n++] = (char)tolower((unsigned char)*c);
    }
    name[n] = '\0';
    if (b->opts->release) {
        fprintf(stderr, "tide: an app made to ship needs an ID of its own, which phones and app stores know it by\n");
        fprintf(stderr, "  = note: give the game one in its settings, like 'settings { appId = \"com.studio.%s\"; }'\n",
                n ? name : "game");
        return false;
    }
    snprintf(out, size, "dev.tide.%s%s", n && isalpha((unsigned char)name[0]) ? "" : "game", name);
    return true;
}

// The app's version code, which Android and Google Play only take updates
// with higher ones of: the minutes since 2020 began, so each build is newer
// than the last with no number to keep. SOURCE_DATE_EPOCH stands in for now
// where it's set, for builds that come out the same each time.
static int android_version_code(void)
{
    const char *epoch = getenv("SOURCE_DATE_EPOCH");
    const int64_t now = epoch && *epoch ? strtoll(epoch, NULL, 10) : sys_now();
    const int64_t minutes = (now - 1577836800) / 60; // 2020-01-01
    return minutes > 1 ? (int)minutes : 1;
}

// Links the game for one CPU: a library with the platform layer, built for
// Android in the package (lib/android/<abi>), the game's libraries for that
// CPU, and Android's own.
static bool link_android(const build *b, const file_list *objects, const c_side *c, const char *output)
{
    char *lib_dir = path_join(b->root, "lib/android");
    char *abi_dir = path_join(lib_dir, android_abis[b->abi]);
    char *platform_lib = path_join(abi_dir, "libtide_platform.a");
    char *raylib_lib = path_join(abi_dir, "libraylib.a");
    free(lib_dir);
    if (!sys_exists(platform_lib) || !sys_exists(raylib_lib)) {
        fprintf(stderr, "tide: this installation can't build Android games: %s is missing\n", abi_dir);
        fprintf(stderr, "  = note: reinstall tide, or for a build of this repo package the android-package presets too\n");
        free(abi_dir);
        free(platform_lib);
        free(raylib_lib);
        return false;
    }
    args a = {0};
    arg_compiler(&a, b->compiler);
    arg(&a, build_target.flag);
    arg(&a, build_target.sysroot_flag);
    arg(&a, "-shared");
    for (int i = 0; i < objects->count; i++) arg(&a, objects->items[i]);
    for (int i = 0; i < c->objects.count; i++) arg(&a, c->objects.items[i]);
    for (int i = 0; i < c->link.count; i++) arg(&a, c->link.items[i]);
    for (int i = 0; i < b->engine.count; i++) arg(&a, b->engine.items[i]);
    arg(&a, platform_lib);
    arg(&a, raylib_lib);
    arg(&a, "-o");
    arg(&a, output);
    arg_list(&a, b->opts->release ? release_flags : debug_flags);
    // The activity's way in, which nothing in the game calls; a C function
    // nothing defines fails here, not when the app starts; pages of 16 KiB,
    // as Android 15's devices can have.
    static const char *const flags[] = {"-fuse-ld=lld", "-Wl,-u,ANativeActivity_onCreate", "-Wl,--no-undefined",
                                        "-Wl,-z,max-page-size=16384", "-nodefaultlibs", "-lc", "-lm", "-ldl",
                                        "-landroid", "-llog", "-lEGL", "-lGLESv3", NULL};
    arg_list(&a, flags);
    arg(&a, build_target.builtins);
    const int code = sys_run(a.items, NULL, false);
    free(a.items);
    free(abi_dir);
    free(platform_lib);
    free(raylib_lib);
    if (code == -1) fprintf(stderr, "tide: couldn't start %s\n", b->compiler);
    // What the link may have lacked: a library that doesn't say it's Android's
    for (int i = 0; code > 0 && i < c->unsure.count; i++) {
        const char *path = c->unsure.items[i];
        if (has_extension(path, ".so")) {
            fprintf(stderr, "  = note: left out %s, which tide took for Linux's: it has no .note.android.ident, the "
                            "note the NDK puts in the libraries it links for Android\n", path);
        } else {
            fprintf(stderr, "  = note: left out %s, which tide took for Linux's: a static library doesn't say which "
                            "of the two it's for, so build it as a shared one (.so) for Android\n", path);
        }
    }
    return code == 0;
}

// Builds the app, and gives its ID in `package` if it's there. Returns the
// app's path, or NULL after saying what went wrong.
static char *build_android(const char *root, const build_options *opts, char *package, const size_t package_size)
{
    build b = {0};
    if (!build_setup(&b, root, opts, false)) return NULL;
    android_ndk ndk;
    if (!android_find_ndk(root, &ndk)) return NULL;
    char *gen = path_join(b.cache, "gen");
    sys_mkdirs(gen);
    sb externs;
    if (!generate(&b, gen, false, &externs)) return NULL;
    char id[256];
    if (!app_id(&b, id, sizeof id)) return NULL;
    char *main_c = path_join(gen, "main.c");
    write_main(main_c, opts, b.name);
    char *game_c = path_join(gen, "game.c");

    // Each CPU: objects of its own, in <cache>/<abi>, and the game's libraries
    // for it, of which the shared ones go in the app with the game's. The app
    // is made for Android 16, as Google Play wants.
    char *cache = b.cache;
    apk_desc desc = {.package = id, .lib_name = "game", .version_code = android_version_code(),
                     .version_name = b.info.version[0] ? b.info.version : "1.0",
                     .min_sdk = atoi(ANDROID_API), .target_sdk = 36, .debuggable = !opts->release};
    c_side c[ANDROID_ABIS] = {0};
    apk_lib *needed = NULL;
    for (int abi = 0; abi < ANDROID_ABIS; abi++) {
        char target_flag[64];
        snprintf(target_flag, sizeof target_flag, "--target=%s-linux-android" ANDROID_API, android_arches[abi]);
        build_target.flag = target_flag;
        build_target.sysroot_flag = format("--sysroot=%s", ndk.sysroot, NULL);
        build_target.builtins = ndk.builtins[abi];
        b.abi = abi;
        b.cache = path_join(cache, android_abis[abi]);
        sys_mkdirs(b.cache);
        b.engine_built = false;
        b.engine = (file_list){0};
        if (!build_engine(&b)) return NULL;
        file_list objects = {0};
        add_file(&objects, path_join(b.cache, "game.o"));
        add_file(&objects, path_join(b.cache, "main.o"));
        if (!compile_c(&b, game_c, objects.items[0], gen) || !compile_c(&b, main_c, objects.items[1], gen)) return NULL;
        if (!compile_game_c(&b, &c[abi]) || !find_libraries(&b, &c[abi])) return NULL;
        char *lib = path_join(b.cache, "libgame.so");
        if (!link_android(&b, &objects, &c[abi], lib)) return NULL;
        desc.abis[desc.lib_count] = android_abis[abi];
        desc.libs[desc.lib_count++] = lib;
        needed = realloc(needed, sizeof(apk_lib) * (size_t)(desc.needed_count + c[abi].dynamic.count + 1));
        if (!needed) abort();
        for (int i = 0; i < c[abi].dynamic.count; i++) {
            needed[desc.needed_count++] = (apk_lib){android_abis[abi], c[abi].names.items[i], c[abi].dynamic.items[i]};
        }
    }
    desc.needed = needed;
    build_target = (target){0};

    char *output;
    if (opts->output) {
        output = path_absolute(opts->output);
    } else {
        char file[512];
        snprintf(file, sizeof file, "%s.apk", b.name);
        char *dir = path_join(b.folder, "build");
        output = path_join(dir, file);
        free(dir);
    }
    char *output_dir = path_dir(output);
    sys_mkdirs(output_dir);
    desc.label = opts->title ? opts->title : b.info.title[0] ? b.info.title : b.name;
    // Its icon: the game's icon.png, or Tide's
    char *icon = path_join(b.folder, "icon.png");
    if (!sys_exists(icon)) {
        free(icon);
        icon = path_join(root, "lib/android/icon.png");
    }
    desc.icon = sys_exists(icon) ? icon : NULL;
    char error[512];
    apk_key key;
    bool made;
    char *key_path = android_key_path();
    if (!apk_key_load(key_path, &key, &made, error, sizeof error)) {
        fprintf(stderr, "tide: %s\n", error);
        return NULL;
    }
    if (made) {
        printf("Made the key that signs your Android apps: %s\n"
               "  Keep a copy somewhere safe. An app's updates have to be signed with the key the app was, and on\n"
               "  Google Play it's your upload key.\n",
               key_path);
    }
    if (!apk_write(output, &desc, &key, error, sizeof error)) {
        fprintf(stderr, "tide: %s\n", error);
        return NULL;
    }
    // Made to ship: an App Bundle too, which Google Play takes
    if (opts->release) {
        const size_t n = strlen(output);
        char *bundle = format("%s%s", output, ".aab");
        if (n > 4 && strcmp(output + n - 4, ".apk") == 0) strcpy(bundle + n - 4, ".aab");
        if (!apk_bundle_write(bundle, &desc, &key, error, sizeof error)) {
            fprintf(stderr, "tide: %s\n", error);
            return NULL;
        }
        printf("Built %s for Google Play\n", bundle);
    }
    for (int abi = 0; abi < ANDROID_ABIS; abi++) free_game_c(&c[abi]);
    free(needed);
    if (package) snprintf(package, package_size, "%s", id);
    return output;
}

// adb's devices, as `adb devices` lists them: how many are ready.
static int android_devices(const char *adb)
{
    char out[4096];
    const char *const argv[] = {adb, "devices", NULL};
    if (sys_capture(argv, out, sizeof out) != 0) return 0;
    int count = 0;
    for (const char *line = strchr(out, '\n'); line; line = strchr(line + 1, '\n')) {
        const char *tab = strchr(line + 1, '\t');
        const char *end = strchr(line + 1, '\n');
        if (tab && (!end || tab < end) && strncmp(tab + 1, "device", 6) == 0) count++;
    }
    return count;
}

// The app's process on the device, or 0 when it isn't running.
static long android_pid(const char *adb, const char *package)
{
    char out[256];
    const char *const argv[] = {adb, "shell", "pidof", package, NULL};
    if (sys_capture(argv, out, sizeof out) != 0) return 0;
    return strtol(out, NULL, 10);
}

// Installs the app. One installed before with another key (another
// machine's) can't be updated: it goes, and this one comes in its place.
static bool install_app(const char *adb, const char *apk, const char *package)
{
    printf("Installing %s...\n", path_base(apk));
    fflush(stdout);
    const char *const install[] = {adb, "install", "-r", apk, NULL};
    char out[4096];
    if (sys_capture(install, out, sizeof out) == 0) return true;
    if (strstr(out, "INSTALL_FAILED_UPDATE_INCOMPATIBLE") || strstr(out, "signatures do not match")) {
        printf("The app on the device was signed with another key: replacing it.\n");
        const char *const uninstall[] = {adb, "uninstall", package, NULL};
        sys_run(uninstall, NULL, true);
    }
    if (sys_run(install, NULL, false) == 0) return true;
    fprintf(stderr, "tide: the device didn't take the app\n");
    return false;
}

// Starts the app, or starts it over, with what goes to its main
// (platform/android): the session's flags.
static bool start_app(const char *adb, const char *package, const char *const *args)
{
    char start[1024];
    int at = snprintf(start, sizeof start, "am start -S -n %s/android.app.NativeActivity", package);
    if (args[0]) {
        at += snprintf(start + at, sizeof start - (size_t)at, " --es tide.args '");
        for (int i = 0; args[i]; i++) {
            for (const char *c = args[i]; *c && at < (int)sizeof start - 8; c++) {
                if (*c != '\'') start[at++] = *c; // Codes and addresses never have quotes
            }
            if (args[i + 1]) start[at++] = ' ';
        }
        at += snprintf(start + at, sizeof start - (size_t)at, "'");
    }
    const char *const launch[] = {adb, "shell", start, NULL};
    if (sys_run(launch, NULL, true) == 0) return true;
    fprintf(stderr, "tide: the app didn't start\n");
    return false;
}

int tide_run_android(const char *root, const build_options *opts, const char *const *args)
{
    char *adb = android_find_adb(root);
    if (!adb) return 1;
    if (android_devices(adb) == 0) {
        fprintf(stderr, "tide: no Android phone or emulator is connected\n");
        fprintf(stderr, "  = note: on the phone, turn on USB debugging (Settings > About phone: tap Build number seven "
                        "times; then System > Developer options > USB debugging), connect it, and allow this computer\n");
        return 1;
    }
    char package[256];
    char *apk = build_android(root, opts, package, sizeof package);
    if (!apk || !install_app(adb, apk, package)) return 1;
    const char *const clear[] = {adb, "logcat", "-c", NULL};
    sys_run(clear, NULL, true);
    if (!start_app(adb, package, args)) return 1;

    // What it prints, until it ends; saving a file makes the app again and
    // starts it over, as the device can't swap code into a running app.
    printf("Running %s on the device; its output follows. Saving a .tide or C file builds it again and starts it "
           "over; type r and press Enter to start it over, or close it to stop.\n", package);
    fflush(stdout);
    sys_read_lines();
    const char *const logs[] = {adb, "logcat", "-v", "raw", "-s", "tide:V", NULL};
    sys_process *log = sys_start(logs, NULL);
    build watch = {0}; // The game's files and its packages', for their stamp
    watch.folder = path_absolute(opts->folder);
    find_packages(watch.folder, &watch.packages);
    uint64_t stamp = game_stamp(&watch);
    int64_t changed_at = -1;
    bool seen = false;
    int waits = 0; // Half seconds without the app, since it was started
    for (;;) {
        int code;
        if (log && sys_wait(log, 500, &code)) log = NULL;
        bool restart = typed_restart();
        const uint64_t now = game_stamp(&watch);
        if (now != stamp) {
            stamp = now;
            changed_at = sys_now_ms();
        } else if (changed_at >= 0 && sys_now_ms() - changed_at >= SETTLE_MS) {
            changed_at = -1;
            char *rebuilt = build_android(root, opts, package, sizeof package);
            if (rebuilt && install_app(adb, rebuilt, package)) restart = true;
            else fprintf(stderr, "tide: the device keeps running the last build\n");
        }
        if (restart) {
            if (!start_app(adb, package, args)) break;
            seen = false;
            waits = 0;
            continue;
        }
        const long pid = android_pid(adb, package);
        if (pid) seen = true;
        else waits++;
        if ((seen && !pid) || (!seen && waits > 40) || !log) break;
    }
    if (log) sys_kill(log);
    printf(seen ? "The app ended.\n" : "The app didn't start.\n");
    return seen ? 0 : 1;
}

bool tide_schedule(const char *folder_arg)
{
    char *folder = path_absolute(folder_arg);
    game_packages packages = {0};
    file_list paths;
    int count = 0;
    compile_input *inputs = find_packages(folder, &packages) ? game_inputs(folder, &packages, &paths, &count) : NULL;
    if (!inputs) return false;
    const codegen_options codegen = {game_name(folder), NULL, true, false, NULL, NULL};
    sb text = {0};
    if (!compile_inputs(inputs, count, &codegen, &text)) return false;
    fputs(text.data, stdout);
    return true;
}
