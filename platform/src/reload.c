// Hot reloading, for `tide run` (see tide/host.h). tide rebuilds the game as a
// library whenever a .tide file changes: game-1, game-2 and so on, each renamed
// into place once it's whole. Between two frames, the host loads the newest
// and runs it from then on. A file of its own, since it includes the operating
// system's headers.
//
// On the web, each build is a program of its own instead. The page asks the
// running one for its state (tide_reload_save), then starts the new one in its
// place (platform/web/tide.js), which carries that state over.

#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__wasm__)
#define _DEFAULT_SOURCE // dlopen under strict C
#endif

#include "tide/host.h"
#include "tide/migrate.h"

static const char *plural(const uint32_t n)
{
    return n == 1 ? "" : "s";
}

// What carrying a game over to a new build did, after "tide: reloaded".
static void say_carried(const tide_layout *from, const tide_layout *to, const uint32_t dropped)
{
    const uint32_t reset = tide_layout_fields_reset(from, to);
    printf("tide: reloaded, and carried the game over to its new data layout");
    if (reset) printf("; %u field%s reset", (unsigned)reset, plural(reset));
    if (dropped) printf("; %u entit%s dropped", (unsigned)dropped, dropped == 1 ? "y" : "ies");
    printf("\n");
}

#if defined(__wasm__)

#include "tide_web.h"

_Noreturn void tide_host_run_library(const tide_run_desc *desc, const char *dir)
{
    (void)desc;
    (void)dir;
    fprintf(stderr, "tide: web builds reload with tide_host_run_web\n");
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
__attribute__((export_name("tide_reload_save"))) uint32_t tide_reload_save(void)
{
    const tide_host_game *game = tide_run_game;
    if (!game || !game->layout || !tide_run_local) return 0;
    uint32_t layout_size = 0;
    void *layout = tide_layout_pack(game->layout, &layout_size);
    if (!layout) return 0;
    const void *world = tide_session_server_world(tide_run_session);
    const tide_session_status status = tide_session_status_of(tide_run_session);
    const int32_t player = tide_player_index(status.client.player);
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
    h.gui_size = sizeof tide_run_gui;
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
    memcpy(block + h.local, tide_run_local, h.local_size);
    memcpy(block + h.gui, &tide_run_gui, h.gui_size);
    free(layout);
    return (uint32_t)(uintptr_t)block;
}

// Goes on from what the last program left, if anything: false to start the
// game as it would anyway, having said why when something was left.
static bool resume(void)
{
    const tide_host_game *game = tide_run_game;
    const uint32_t size = tide_web_resume_size();
    if (size < sizeof(saved) || !game->layout) return false;
    uint8_t *block = malloc(size);
    if (!block) return false;
    tide_web_resume_copy(block);
    saved h;
    memcpy(&h, block, sizeof h);
    const bool whole = h.magic == SAVED_MAGIC && h.size == size && h.layout <= size && h.layout_size <= size - h.layout
                    && h.world <= size && h.world_size <= size - h.world && h.local <= size
                    && h.local_size <= size - h.local && h.gui <= size && h.gui_size <= size - h.gui;
    const tide_layout *old = whole ? tide_layout_unpack(block + h.layout, h.layout_size) : NULL;
    if (!old) {
        printf("tide: reloaded, and started over: the last build's state didn't come through\n");
        free(block);
        return false;
    }

    void *local = calloc(1, game->local_size);
    void *world = h.world_size ? calloc(1, game->game->world_size) : NULL;
    tide_migration local_done = {0};
    tide_migration match_done = {0};
    bool ok = local && (world || !h.world_size);
    if (ok) {
        ok = tide_migrate_world(old, &old->local, block + h.local, game->layout, &game->layout->local, local,
                                &local_done);
    }
    if (ok && world) {
        ok = tide_migrate_world(old, &old->match, block + h.world, game->layout, &game->layout->match, world,
                                &match_done);
    }
    if (!ok) {
        const char *why = local_done.failed[0] ? local_done.failed : match_done.failed;
        printf("tide: reloaded, and started over: %s\n", why[0] ? why : "there wasn't enough memory");
        free(local);
        free(world);
        free(block);
        return false;
    }

    tide_run_local = local;
    tide_run_start = calloc(1, game->game->start_size);
    if (!tide_run_start) abort();
    tide_run_session = tide_run_new_session();
    if (world) tide_session_play_from(tide_run_session, world, h.players, tide_run_now);
    if (h.gui_size == sizeof tide_run_gui) memcpy(&tide_run_gui, block + h.gui, sizeof tide_run_gui);
    if (h.hash == game->game->hash) printf("tide: reloaded\n");
    else say_carried(old, game->layout, local_done.entities_dropped + match_done.entities_dropped);
    free(world);
    free(block);
    return true;
}

_Noreturn void tide_host_run_web(const tide_run_desc *desc, const tide_host_game *game)
{
    tide_run_open(desc);
    tide_run_game = game;
    if (!resume()) tide_run_begin();
    tide_platform_run(tide_run_frame, NULL);
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

typedef const tide_host_game *(*library_fn)(void);

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
static const tide_host_game *load(const uint32_t build, void **library)
{
    char path[4096];
    library_path(build, path, sizeof path);
    void *opened = library_open(path);
    if (!opened) {
        char error[512];
        library_error(error, sizeof error);
        fprintf(stderr, "tide: can't load %s (%s)\n", path, error);
        return NULL;
    }
    const library_fn game = library_find(opened, TIDE_HOST_LIBRARY);
    if (!game) {
        fprintf(stderr, "tide: %s isn't a game's library: it has no %s\n", path, TIDE_HOST_LIBRARY);
        library_close(opened);
        return NULL;
    }
    *library = opened;
    return game();
}

// The match's worlds, carried over for tide_session_migrate. The first one is
// the server's, or the client's when another machine runs it: what's said of
// the carrying over is about that one.
typedef struct match_carry {
    const tide_layout *from;
    const tide_layout *to;
    tide_migration first;
    tide_migration failed;
    int worlds;
} match_carry;

static bool carry_match(void *user, const void *from, void *to)
{
    match_carry *m = user;
    tide_migration done;
    const bool ok = tide_migrate_world(m->from, &m->from->match, from, m->to, &m->to->match, to, &done);
    if (m->worlds++ == 0) m->first = done;
    if (!ok) m->failed = done;
    return ok;
}

// The match and the local state, carried over to `next`'s data layout by
// name. False, having said why and changed nothing, if they can't be.
static bool carry_over(const tide_host_game *next)
{
    const tide_host_game *old = tide_run_game;
    if (!old->layout || !next->layout) {
        printf("tide: reloaded, and started over: a build doesn't describe its data layout\n");
        return false;
    }
    void *local = calloc(1, next->local_size);
    void *start = calloc(1, next->game->start_size);
    tide_migration local_done = {0};
    match_carry match = {old->layout, next->layout, {0}, {0}, 0};
    bool ok = local && start;
    if (ok) {
        ok = tide_migrate_world(old->layout, &old->layout->local, tide_run_local, next->layout, &next->layout->local,
                                local, &local_done);
    }
    if (ok) ok = tide_session_migrate(tide_run_session, next->game, carry_match, &match);
    if (!ok) {
        const char *why = local_done.failed[0] ? local_done.failed : match.failed.failed;
        printf("tide: reloaded, and started over: %s\n", why[0] ? why : "there wasn't enough memory");
        free(local);
        free(start);
        return false;
    }
    free(tide_run_local);
    free(tide_run_start);
    tide_run_local = local;
    tide_run_start = start;
    say_carried(old->layout, next->layout, local_done.entities_dropped + match.first.entities_dropped);
    return true;
}

// Runs `next` from now on. With the same data layout, it takes over the match
// and the local state where they are; with another, they're carried over to
// it, and when they can't be, the game starts over.
static void swap(const tide_host_game *next, void *library)
{
    const tide_host_game *old = tide_run_game;
    if (next->game->hash == old->game->hash && next->local_size == old->local_size) {
        tide_session_set_game(tide_run_session, next->game);
        tide_run_game = next;
        printf("tide: reloaded\n");
    } else if (carry_over(next)) {
        tide_run_game = next;
    } else {
        tide_run_end();
        tide_run_game = next;
        tide_run_begin();
    }
    fflush(stdout);
    old->unload();
    library_close(reload.library);
    reload.library = library;
}

// Swaps in the newest build, if there's a newer one, and starts over if tide
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
        const tide_host_game *next = load(newest, &library);
        if (next) swap(next, library);
    }

    snprintf(path, sizeof path, "%s/restart", reload.dir);
    if (file_exists(path)) {
        remove(path);
        tide_run_end();
        tide_run_begin();
    }
}

// A game started with --join or --connect joins again a second after its
// match ends without it leaving: when the server starts over, has another
// build for a moment, or went away and came back.
static void rejoin(const float seconds)
{
    tide_session_request request;
    if (!tide_run_dropped || !tide_run_arguments(&request)
        || (request.kind != TIDE_REQUEST_JOIN && request.kind != TIDE_REQUEST_CONNECT)) {
        reload.dropped_for = 0.0f;
        return;
    }
    reload.dropped_for += seconds;
    if (reload.dropped_for < 1.0f) return;
    reload.dropped_for = 0.0f;
    tide_run_dropped = false;
    tide_run_request(&request, NULL);
}

static int reload_frame(void *user, const float seconds)
{
    reload.since_check += seconds;
    if (reload.since_check >= 0.1f) {
        reload.since_check = 0.0f;
        check();
    }
    const int code = tide_run_frame(user, seconds);
    rejoin(seconds);
    return code;
}

_Noreturn void tide_host_run_library(const tide_run_desc *desc, const char *dir)
{
    reload.dir = dir;
    reload.running = 1;
    const tide_host_game *game = load(1, &reload.library);
    if (!game) exit(1);
    tide_run_open(desc);
    tide_run_game = game;
    tide_run_begin();
    tide_platform_run(reload_frame, NULL);
}

#endif
