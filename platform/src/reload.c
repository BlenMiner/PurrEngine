// Hot reloading, for `purr run` (see purr/host.h). purr rebuilds the game as a
// library whenever a .purr file changes: game-1, game-2 and so on, each renamed
// into place once it's whole. Between two frames, the host loads the newest
// and runs it from then on. A file of its own, since it includes the operating
// system's headers.
//
// On the web, each build is a program of its own instead. The page asks the
// running one for its state (purr_reload_save), then starts the new one in its
// place (platform/web/purr.js), which carries that state over.

#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__wasm__)
#define _DEFAULT_SOURCE // dlopen under strict C
#endif

#include "purr/host.h"
#include "purr/migrate.h"

static const char *plural(const uint32_t n)
{
    return n == 1 ? "" : "s";
}

// What carrying a game over to a new build did, after "purr: reloaded".
static void say_carried(const purr_layout *from, const purr_layout *to, const uint32_t dropped)
{
    const uint32_t reset = purr_layout_fields_reset(from, to);
    printf("purr: reloaded, and carried the game over to its new data layout");
    if (reset) printf("; %u field%s reset", (unsigned)reset, plural(reset));
    if (dropped) printf("; %u entit%s dropped", (unsigned)dropped, dropped == 1 ? "y" : "ies");
    printf("\n");
}

#if defined(__wasm__)

#include "purr_web.h"

_Noreturn void purr_host_run_library(const purr_run_desc *desc, const char *dir)
{
    (void)desc;
    (void)dir;
    fprintf(stderr, "purr: web builds reload with purr_host_run_web\n");
    exit(1);
}

// What a program leaves the next one: this, then its layout (packed), its
// match's world as the server has it, its local state and its GUI, each
// 8-aligned.
#define SAVED_MAGIC 0x53525550u // "PURS"

typedef struct saved {
    uint32_t size; // Of it all, first, for the page
    uint32_t magic;
    uint64_t hash;    // Its game's: the same one has the same layout
    uint32_t players; // In the match, a bit per player
    uint32_t layout, layout_size;
    uint32_t world, world_size; // 0 bytes outside a match
    uint32_t local, local_size;
    uint32_t gui, gui_size;
} saved;

static uint32_t align8(const uint32_t n)
{
    return (n + 7u) & ~7u;
}

// For the page, before it starts the next build: where this program's state
// is, starting with its size, or 0 when there's none to carry over. It stays
// until the program goes.
__attribute__((export_name("purr_reload_save"))) uint32_t purr_reload_save(void)
{
    const purr_host_game *game = purr_run_game;
    if (!game || !game->layout || !purr_run_local) return 0;
    uint32_t layout_size = 0;
    void *layout = purr_layout_pack(game->layout, &layout_size);
    if (!layout) return 0;
    const void *world = purr_session_server_world(purr_run_session);
    const purr_session_status status = purr_session_status_of(purr_run_session);
    const int32_t player = purr_player_index(status.client.player);
    saved h = {0};
    h.magic = SAVED_MAGIC;
    h.hash = game->game->hash;
    h.players = world && player >= 0 ? 1u << player : 0u;
    h.layout = align8(sizeof h);
    h.layout_size = layout_size;
    h.world = align8(h.layout + layout_size);
    h.world_size = world ? game->game->world_size : 0u;
    h.local = align8(h.world + h.world_size);
    h.local_size = game->local_size;
    h.gui = align8(h.local + h.local_size);
    h.gui_size = sizeof purr_run_gui;
    h.size = h.gui + h.gui_size;
    uint8_t *block = malloc(h.size);
    if (!block) {
        free(layout);
        return 0;
    }
    memset(block, 0, h.size);
    memcpy(block, &h, sizeof h);
    memcpy(block + h.layout, layout, layout_size);
    if (world) memcpy(block + h.world, world, h.world_size);
    memcpy(block + h.local, purr_run_local, h.local_size);
    memcpy(block + h.gui, &purr_run_gui, h.gui_size);
    free(layout);
    return (uint32_t)(uintptr_t)block;
}

// Goes on from what the last program left, if anything: false to start the
// game as it would anyway, having said why when something was left.
static bool resume(void)
{
    const purr_host_game *game = purr_run_game;
    const uint32_t size = purr_web_resume_size();
    if (size < sizeof(saved) || !game->layout) return false;
    uint8_t *block = malloc(size);
    if (!block) return false;
    purr_web_resume_copy(block);
    saved h;
    memcpy(&h, block, sizeof h);
    const bool whole = h.magic == SAVED_MAGIC && h.size == size && h.layout <= size && h.layout_size <= size - h.layout
                    && h.world <= size && h.world_size <= size - h.world && h.local <= size
                    && h.local_size <= size - h.local && h.gui <= size && h.gui_size <= size - h.gui;
    const purr_layout *old = whole ? purr_layout_unpack(block + h.layout, h.layout_size) : NULL;
    if (!old) {
        printf("purr: reloaded, and started over: the last build's state didn't come through\n");
        free(block);
        return false;
    }

    void *local = calloc(1, game->local_size);
    void *world = h.world_size ? calloc(1, game->game->world_size) : NULL;
    purr_migration local_done = {0};
    purr_migration match_done = {0};
    bool ok = local && (world || !h.world_size);
    if (ok) {
        ok = purr_migrate_world(old, &old->local, block + h.local, game->layout, &game->layout->local, local,
                                &local_done);
    }
    if (ok && world) {
        ok = purr_migrate_world(old, &old->match, block + h.world, game->layout, &game->layout->match, world,
                                &match_done);
    }
    if (!ok) {
        const char *why = local_done.failed[0] ? local_done.failed : match_done.failed;
        printf("purr: reloaded, and started over: %s\n", why[0] ? why : "there wasn't enough memory");
        free(local);
        free(world);
        free(block);
        return false;
    }

    purr_run_local = local;
    purr_run_start = calloc(1, game->game->start_size);
    if (!purr_run_start) abort();
    purr_run_session = purr_run_new_session();
    if (world) purr_session_play_from(purr_run_session, world, h.players, purr_run_now);
    if (h.gui_size == sizeof purr_run_gui) memcpy(&purr_run_gui, block + h.gui, sizeof purr_run_gui);
    if (h.hash == game->game->hash) printf("purr: reloaded\n");
    else say_carried(old, game->layout, local_done.entities_dropped + match_done.entities_dropped);
    free(world);
    free(block);
    return true;
}

_Noreturn void purr_host_run_web(const purr_run_desc *desc, const purr_host_game *game)
{
    purr_run_open(desc);
    purr_run_game = game;
    if (!resume()) purr_run_begin();
    purr_platform_run(purr_run_frame, NULL);
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

// The match's worlds, carried over for purr_session_migrate. The first one is
// the server's, or the client's when another machine runs it: what's said of
// the carrying over is about that one.
typedef struct match_carry {
    const purr_layout *from;
    const purr_layout *to;
    purr_migration first;
    purr_migration failed;
    int worlds;
} match_carry;

static bool carry_match(void *user, const void *from, void *to)
{
    match_carry *m = user;
    purr_migration done;
    const bool ok = purr_migrate_world(m->from, &m->from->match, from, m->to, &m->to->match, to, &done);
    if (m->worlds++ == 0) m->first = done;
    if (!ok) m->failed = done;
    return ok;
}

// The match and the local state, carried over to `next`'s data layout by
// name. False, having said why and changed nothing, if they can't be.
static bool carry_over(const purr_host_game *next)
{
    const purr_host_game *old = purr_run_game;
    if (!old->layout || !next->layout) {
        printf("purr: reloaded, and started over: a build doesn't describe its data layout\n");
        return false;
    }
    void *local = calloc(1, next->local_size);
    void *start = calloc(1, next->game->start_size);
    purr_migration local_done = {0};
    match_carry match = {old->layout, next->layout, {0}, {0}, 0};
    bool ok = local && start;
    if (ok) {
        ok = purr_migrate_world(old->layout, &old->layout->local, purr_run_local, next->layout, &next->layout->local,
                                local, &local_done);
    }
    if (ok) ok = purr_session_migrate(purr_run_session, next->game, carry_match, &match);
    if (!ok) {
        const char *why = local_done.failed[0] ? local_done.failed : match.failed.failed;
        printf("purr: reloaded, and started over: %s\n", why[0] ? why : "there wasn't enough memory");
        free(local);
        free(start);
        return false;
    }
    free(purr_run_local);
    free(purr_run_start);
    purr_run_local = local;
    purr_run_start = start;
    say_carried(old->layout, next->layout, local_done.entities_dropped + match.first.entities_dropped);
    return true;
}

// Runs `next` from now on. With the same data layout, it takes over the match
// and the local state where they are; with another, they're carried over to
// it, and when they can't be, the game starts over.
static void swap(const purr_host_game *next, void *library)
{
    const purr_host_game *old = purr_run_game;
    if (next->game->hash == old->game->hash && next->local_size == old->local_size) {
        purr_session_set_game(purr_run_session, next->game);
        purr_run_game = next;
        printf("purr: reloaded\n");
    } else if (carry_over(next)) {
        purr_run_game = next;
    } else {
        purr_run_end();
        purr_run_game = next;
        purr_run_begin();
    }
    fflush(stdout);
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

// A game started with --join or --connect joins again a second after its
// match ends without it leaving: when the server starts over, has another
// build for a moment, or went away and came back.
static void rejoin(const float seconds)
{
    purr_session_request request;
    if (!purr_run_dropped || !purr_run_arguments(&request)
        || (request.kind != PURR_REQUEST_JOIN && request.kind != PURR_REQUEST_CONNECT)) {
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
