// Hot reloading, for `purr run` (see purr/host.h). purr rebuilds the game as a
// library whenever a .purr file changes: game-1, game-2 and so on, each renamed
// into place once it's whole. Between two frames, the host loads the newest
// and runs it from then on. A file of its own, since it includes the operating
// system's headers.

#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__wasm__)
#define _DEFAULT_SOURCE // dlopen under strict C
#endif

#include "purr/host.h"

#if defined(__wasm__)

// Web pages reload another way, which doesn't exist yet.
_Noreturn void purr_host_run_library(const purr_run_desc *desc, const char *dir)
{
    (void)desc;
    (void)dir;
    fprintf(stderr, "purr: web builds can't reload libraries\n");
    exit(1);
}

#else

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#define LIBRARY_SUFFIX ".dll"
#else
#include <dlfcn.h>
#include <sys/stat.h>
#ifdef __APPLE__
#define LIBRARY_SUFFIX ".dylib"
#else
#define LIBRARY_SUFFIX ".so"
#endif
#endif

typedef const purr_host_game *(*library_fn)(void);

#ifdef _WIN32

static bool file_exists(const char *path)
{
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

static void *library_open(const char *path)
{
    char native[4096];
    snprintf(native, sizeof native, "%s", path);
    for (char *c = native; *c; c++) {
        if (*c == '/') *c = '\\'; // LoadLibrary wants backslashes
    }
    return (void *)LoadLibraryA(native);
}

static library_fn library_find(void *library, const char *name)
{
    const FARPROC found = GetProcAddress((HMODULE)library, name);
    library_fn fn = NULL;
    if (found) memcpy(&fn, &found, sizeof fn);
    return fn;
}

static void library_close(void *library)
{
    FreeLibrary((HMODULE)library);
}

static void library_error(char *out, const size_t size)
{
    snprintf(out, size, "error %lu", (unsigned long)GetLastError());
}

#else

static bool file_exists(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0;
}

static void *library_open(const char *path)
{
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}

static library_fn library_find(void *library, const char *name)
{
    void *found = dlsym(library, name);
    library_fn fn = NULL;
    if (found) memcpy(&fn, &found, sizeof fn);
    return fn;
}

static void library_close(void *library)
{
    dlclose(library);
}

static void library_error(char *out, const size_t size)
{
    const char *error = dlerror();
    snprintf(out, size, "%s", error ? error : "unknown error");
}

#endif

static struct {
    const char *dir;
    uint32_t running;  // The build running: game-<running>
    void *library;     // ...and its library
    float since_check; // Seconds since `dir` was last looked at
    float dropped_for; // Seconds since the match ended without this machine leaving it
} reload;

static void library_path(const uint32_t build, char *out, const size_t size)
{
    snprintf(out, size, "%s/game-%u" LIBRARY_SUFFIX, reload.dir, (unsigned)build);
}

// Loads a build: its game, or NULL after saying why.
static const purr_host_game *load(const uint32_t build, void **library)
{
    char path[4096];
    library_path(build, path, sizeof path);
    void *opened = library_open(path);
    if (!opened) {
        char error[512];
        library_error(error, sizeof error);
        fprintf(stderr, "purr: can't load %s (%s)\n", path, error);
        return NULL;
    }
    const library_fn game = library_find(opened, PURR_HOST_LIBRARY);
    if (!game) {
        fprintf(stderr, "purr: %s isn't a game's library: it has no %s\n", path, PURR_HOST_LIBRARY);
        library_close(opened);
        return NULL;
    }
    *library = opened;
    return game();
}

// Runs `next` from now on. With the same data layout, it takes over the match
// and the local state where they are; with another, the game starts over.
static void swap(const purr_host_game *next, void *library)
{
    const purr_host_game *old = purr_run_game;
    if (next->game->hash == old->game->hash && next->local_size == old->local_size) {
        purr_session_set_game(purr_run_session, next->game);
        purr_run_game = next;
    } else {
        purr_run_end();
        purr_run_game = next;
        purr_run_begin();
    }
    old->unload();
    library_close(reload.library);
    reload.library = library;
}

// Swaps in the newest build, if there's a newer one, and starts over if purr
// was asked to.
static void check(void)
{
    char path[4096];
    uint32_t newest = reload.running;
    for (;;) {
        library_path(newest + 1u, path, sizeof path);
        if (!file_exists(path)) break;
        newest++;
    }
    if (newest != reload.running) {
        reload.running = newest; // A build that doesn't load isn't tried again
        void *library = NULL;
        const purr_host_game *next = load(newest, &library);
        if (next) swap(next, library);
    }

    snprintf(path, sizeof path, "%s/restart", reload.dir);
    if (file_exists(path)) {
        remove(path);
        purr_run_end();
        purr_run_begin();
    }
}

// A game started with --join joins again a second after its match ends
// without it leaving: when the server starts over, has another build for a
// moment, or went away and came back.
static void rejoin(const float seconds)
{
    purr_session_request request;
    if (!purr_run_dropped || !purr_run_arguments(&request) || request.kind != PURR_REQUEST_JOIN) {
        reload.dropped_for = 0.0f;
        return;
    }
    reload.dropped_for += seconds;
    if (reload.dropped_for < 1.0f) return;
    reload.dropped_for = 0.0f;
    purr_run_dropped = false;
    purr_run_request(&request, NULL);
}

static int reload_frame(void *user, const float seconds)
{
    reload.since_check += seconds;
    if (reload.since_check >= 0.1f) {
        reload.since_check = 0.0f;
        check();
    }
    const int code = purr_run_frame(user, seconds);
    rejoin(seconds);
    return code;
}

_Noreturn void purr_host_run_library(const purr_run_desc *desc, const char *dir)
{
    reload.dir = dir;
    reload.running = 1;
    const purr_host_game *game = load(1, &reload.library);
    if (!game) exit(1);
    purr_run_open(desc);
    purr_run_game = game;
    purr_run_begin();
    purr_platform_run(reload_frame, NULL);
}

#endif
