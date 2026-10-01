#include "build.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "compile.h"
#include "folders.h"
#include "libraries.h"
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

static file_list game_files(const char *folder)
{
    file_list list = {0};
    char *dir = path_join(folder, "");
    folder_find(dir, ".tide", add_file, &list);
    free(dir);
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
    folder_find(dir, "", add_own_file, &o); // Every file, once
    free(dir);
    return o.list;
}

static const char *const c_extensions[] = {".c", NULL};
static const char *const header_extensions[] = {".h", NULL};
static const char *const library_extensions[] = {".a", ".lib", ".so", ".dll", ".dylib", NULL};
// Whatever a build of the game reads, besides its .tide files: tide run builds
// again when one changes.
static const char *const c_side_extensions[] = {".c", ".h", ".a", ".lib", ".so", ".dll", ".dylib", NULL};

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
static const char *const web_link_flags[] = {"-Wl,--allow-undefined", "-Wl,--export=malloc", "-Wl,--export=free",
                                             "-Wl,-z,stack-size=1048576", "-Wl,--import-memory", "-Wl,--export-memory",
                                             "-Wl,--max-memory=4294967296", NULL};

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
    char *folder;
    const char *name;
    char *include;
    char *compiler;
    char *wasm_ld; // Web builds with an installed clang
    char *cache;   // <folder>/.tide/<configuration>
    bool engine_built;
    file_list engine; // The engine's objects
} build;

static void config_flags(args *a, const build *b)
{
    if (build_target.flag) {
        arg(a, build_target.flag);
        arg(a, build_target.sysroot_flag);
    }
    arg_list(a, common_flags);
    arg_list(a, b->opts->release ? release_flags : debug_flags);
#if !defined(_WIN32) && !defined(__APPLE__)
    // A library's code works wherever it's loaded.
    if (b->library) arg(a, "-fPIC");
#endif
#ifdef _WIN32
    // Debug info Visual Studio's debugger reads, in a .pdb next to the game.
    if (!b->opts->web && !b->opts->release) arg(a, "-gcodeview");
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
    snprintf(config, sizeof config, "%s-%s", opts->web ? "web" : "native", opts->release ? "release" : "debug");
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
} c_side;

static void free_game_c(c_side *c)
{
    free_files(&c->objects);
    free_files(&c->link);
    free_files(&c->dynamic);
}

static lib_platform target_platform(const build *b)
{
    if (b->opts->web) return LIB_WEB;
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

// Compiles the game's .c files into `dir`, each one again only when it, a
// header in the game's folder, or the flags changed: each object has a stamp
// of what made it. Objects for a library are kept apart on Linux, as the
// engine's are.
static bool compile_game_c(const build *b, c_side *out)
{
    file_list sources = own_files_of(b->folder, c_extensions);
    if (sources.count == 0) return true;
#if !defined(_WIN32) && !defined(__APPLE__)
    char *dir = path_join(b->cache, b->library ? "c-pic" : "c");
#else
    char *dir = path_join(b->cache, "c");
#endif
    sys_mkdirs(dir);
    char *flags = stamp_of(b);
    uint64_t common = hash_text(0xCBF29CE484222325ull, flags);
    free(flags);
    file_list headers = own_files_of(b->folder, header_extensions);
    for (int i = 0; i < headers.count; i++) common = hash_file(common, headers.items[i]);
    free_files(&headers);
    char *folder = path_join(b->folder, "");
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

// The game's libraries built for the target: static ones (and Windows import
// libraries) are linked in, and dynamic ones are linked to and go next to the
// program, which finds them there. A library tide can't read is left out,
// saying so; one for another platform or CPU is left out quietly.
static void find_libraries(const build *b, c_side *out)
{
    file_list libraries = own_files_of(b->folder, library_extensions);
    const lib_platform platform = target_platform(b);
    const unsigned cpu = target_cpu(b);
    bool dynamic = false;
    for (int i = 0; i < libraries.count; i++) {
        const char *path = libraries.items[i];
        const lib_info info = lib_identify_file(path);
        if (info.platform == LIB_UNKNOWN) {
            fprintf(stderr, "tide: left out %s: tide can't tell which platform it was built for\n", path);
            continue;
        }
        if (info.platform != platform || !(info.cpus & cpu)) continue;
        if (!info.dynamic) {
            add_file(&out->link, path);
            continue;
        }
        add_file(&out->dynamic, path);
        dynamic = true;
#ifdef _WIN32
        // Linked through its import library (a .lib, static above): the .dll only goes next to the game
#elif defined(__APPLE__)
        add_file(&out->link, path);
#else
        // By its name, so the program looks for it by name: next to itself
        char *dir = path_dir(path);
        char *search = format("-L%s", dir, NULL);
        char *name = format("-l:%s", path_base(path), NULL);
        add_file(&out->link, search);
        add_file(&out->link, name);
        free(dir);
        free(search);
        free(name);
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

// With `layout`, the generated code describes the data layout too, for hot
// reloading. `externs` gets the C function of each extern function, a line
// each, until the next build.
static bool generate(const char *folder, const char *gen, const bool layout, sb *externs)
{
    file_list files = game_files(folder);
    if (files.count == 0) {
        fprintf(stderr, "tide: there are no .tide files in %s\n", folder);
        fprintf(stderr, "  = note: a game is every .tide file in its folder; add one, like game.tide\n");
        return false;
    }
    arena_reset(); // What an earlier build compiled, when a run rebuilds the game
    *externs = (sb){0};
    const codegen_options codegen = {"game", gen, true, layout, externs};
    const bool ok = compile_program((const char **)files.items, files.count, &codegen, NULL);
    free_files(&files);
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

char *tide_build(const char *root, const build_options *opts)
{
    build b = {0};
    if (!build_setup(&b, root, opts, false)) return NULL;
    char *gen = path_join(b.cache, "gen");
    sys_mkdirs(gen);

    sb externs;
    if (!generate(b.folder, gen, false, &externs)) return NULL;
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
    if (!compile_game_c(&b, &c)) return NULL;
    find_libraries(&b, &c);

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
    if (!generate(r->b.folder, r->gen, true, &externs) || !build_engine(&r->b)) return false;
    const char *suffix = r->web ? ".wasm" : LIBRARY_SUFFIX;
    char *game_c = path_join(r->gen, "game.c");
    char *library_c = path_join(r->gen, r->web ? "main.c" : "library.c");
    file_list objects = {0};
    add_file(&objects, path_join(r->dir, "game.o"));
    add_file(&objects, path_join(r->dir, r->web ? "main.o" : "library.o"));
    c_side c = {0};
    bool ok = compile_c(&r->b, game_c, objects.items[0], r->gen)
           && compile_c(&r->b, library_c, objects.items[1], r->gen) && compile_game_c(&r->b, &c);
    if (ok) {
        find_libraries(&r->b, &c);
        copy_dynamic(&c, r->dir); // Next to the host, which loads the game's library
    }
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

// Changes whenever one of the game's .tide files does, or its C, headers and
// libraries, or one comes or goes.
static uint64_t game_stamp(const char *folder)
{
    uint64_t h = 0xCBF29CE484222325ull;
    char *dir = path_join(folder, "");
    folder_find(dir, ".tide", stamp_file, &h);
    free(dir);
    file_list others = own_files_of(folder, c_side_extensions);
    for (int i = 0; i < others.count; i++) stamp_file(&h, others.items[i]);
    free_files(&others);
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

    uint64_t stamp = game_stamp(r.b.folder);
    int64_t changed_at = -1;
    int code = 0;
    while (!sys_wait(game, 250, &code)) {
        if (typed_restart()) {
            char *restart = path_join(r.dir, "restart");
            sys_write_text(restart, "");
            free(restart);
        }
        const uint64_t now = game_stamp(r.b.folder);
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

    uint64_t stamp = game_stamp(r->b.folder);
    int64_t changed_at = -1;
    for (;;) {
        serve_poll(server, 250, answer_web, &w);
        if (typed_restart()) w.restarts++;
        const uint64_t now = game_stamp(r->b.folder);
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

bool tide_schedule(const char *folder_arg)
{
    char *folder = path_absolute(folder_arg);
    file_list files = game_files(folder);
    if (files.count == 0) {
        fprintf(stderr, "tide: there are no .tide files in %s\n", folder);
        return false;
    }
    const codegen_options codegen = {game_name(folder), NULL, true, false, NULL};
    sb text = {0};
    if (!compile_program((const char **)files.items, files.count, &codegen, &text)) return false;
    fputs(text.data, stdout);
    return true;
}
