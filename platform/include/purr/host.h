#pragma once

// The standard host's loop: a game in a window, in a session, driven through
// a table of the game's functions (purr_host_game).
//
// purr/run.h makes the table for a game compiled into the program, which is
// how games run. Under `purr run` the game is a library instead, and
// purr_host_run_library swaps in each new build of it that purr makes: hot
// reloading. The table is what lets one loop do both.
//
// Temporary implementation written by Claude; the project owner takes it over
// later.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "purr/layout.h"
#include "purr/platform.h"
#include "purr/session.h"

typedef struct purr_run_desc {
    const char *title; // Default "PurrEngine"
    int width;         // Starting window size in pixels; default 960 x 540
    int height;
    int tick_rate;     // Ticks per second; default 60
    bool stats;        // Show the tick, entity count, ping and frame rate in a corner
    int argc;          // The command line, for --host and --join
    char **argv;
} purr_run_desc;

// A game as a host runs it: what sessions run (purr_game), and this machine's
// side of it: its local state, its views and its input. `local`, `world` and
// `start` are the game's purr_local, purr_world and purr_start.
typedef struct purr_host_game {
    const purr_game *game;
    uint32_t local_size; // sizeof(purr_local)
    bool main_is_local;  // PURR_MAIN_IS_LOCAL: the program starts outside any match
    void (*local_init)(void *local);
    // purr_input_sample, into `input`; NULL for a game without an input
    void (*sample_input)(const purr_devices *devices, const void *local, void *input);
    void (*frame)(const void *world, const void *previous, float alpha, void *local, purr_draw_list *draw,
                  purr_gui *gui);
    bool (*take_request)(void *local, purr_session_request *request, void *start);
    void (*set_session)(void *local, uint32_t state, purr_player_id player, uint32_t ping, bool server);
    void (*connected)(void *local);
    void (*disconnected)(void *local, uint32_t reason);
    int32_t (*tick)(const void *world); // Time.tick
    uint32_t (*entity_count)(const void *world);
    // Frees what the game's code keeps for itself (the scratch area), before
    // its library is unloaded.
    void (*unload)(void);
    // Its data layout, when purrc described it (purr run's builds), to carry
    // the game over to a build whose layout changed; NULL otherwise.
    const purr_layout *layout;
} purr_host_game;

// A game's library exports this function, which returns its table.
#define PURR_HOST_LIBRARY "purr_host_library"
#ifdef _WIN32
#define PURR_HOST_EXPORT __declspec(dllexport)
#else
#define PURR_HOST_EXPORT __attribute__((visibility("default")))
#endif

// Runs the game from the libraries purr builds in `dir`: game-1 first (a .dll,
// .so or .dylib), then each newer one that appears there, swapped in between
// two frames. A build with the same data layout as the one running (the same
// purr_game hash) takes over the match and the local state where they are;
// with another, they're carried over to it by name (purr/migrate.h), or when
// that can't be done, the game starts over. A file named `restart` in `dir`
// starts it over too. Native builds only (platform/src/reload.c).
_Noreturn void purr_host_run_library(const purr_run_desc *desc, const char *dir);

// The same on the web (purr run --web), where each new build is a program of
// its own, which the page starts in the running one's place: `game` goes on
// from what the last one left it, carried over by name, or starts as
// purr_host_run would. Web builds only (platform/src/reload.c).
_Noreturn void purr_host_run_web(const purr_run_desc *desc, const purr_host_game *game);

// ---------------------------------------------------------------------------
// The loop, for purr/run.h and platform/src/reload.c

static const purr_host_game *purr_run_game;
static void *purr_run_local;  // The game's purr_local
static void *purr_run_start;  // A purr_start, for what local code asks
static purr_devices purr_run_devices;
static purr_draw_list purr_run_draw;
static purr_gui purr_run_gui;
static purr_run_desc purr_run_settings;
static purr_session *purr_run_session;
static double purr_run_now; // Seconds since the program started
static bool purr_run_dropped; // The match ended or turned this machine away, not because it left or couldn't start

// This machine's input for one tick.
static inline void purr_run_sample(void *user, const uint32_t tick, void *input)
{
    (void)user;
    (void)tick;
    if (!purr_run_game->sample_input) return;
    purr_devices sampled = purr_run_devices;
    purr_gui_hide(&purr_run_gui, &sampled); // A click on a button isn't the game's
    purr_run_game->sample_input(&sampled, purr_run_local, input);
    purr_devices_consume(&purr_run_devices);
}

// Starts, joins or leaves a match, as local code or the command line asked.
// `start` is a purr_start, or NULL for Main.
static inline void purr_run_request(const purr_session_request *request, const void *start)
{
    purr_session *s = purr_run_session;
    purr_transport udp;
    purr_address server;
    switch (request->kind) {
    case PURR_REQUEST_PLAY:
        purr_session_play(s, start, purr_run_now);
        break;
    case PURR_REQUEST_HOST:
        if (request->port > 65535u || !purr_platform_udp_open((uint16_t)request->port, &udp)) {
            fprintf(stderr, "purr: can't take players on port %u\n", (unsigned)request->port);
            purr_session_leave(s);
            purr_session_fail(s, PURR_DISCONNECT_FAILED);
            break;
        }
        purr_session_host(s, start, udp, purr_run_now);
        break;
    case PURR_REQUEST_JOIN:
        if (!purr_platform_resolve(request->address, PURR_DEFAULT_PORT, &server) || !purr_platform_udp_open(0, &udp)) {
            fprintf(stderr, "purr: can't reach '%s'\n", request->address);
            purr_session_leave(s);
            purr_session_fail(s, PURR_DISCONNECT_FAILED);
            break;
        }
        purr_session_join(s, udp, server, purr_run_now);
        break;
    case PURR_REQUEST_LEAVE:
        purr_session_leave(s);
        break;
    default:
        break;
    }
}

// --host [port] or --join address, from the command line.
static inline bool purr_run_arguments(purr_session_request *request)
{
    for (int i = 1; i < purr_run_settings.argc; i++) {
        const char *arg = purr_run_settings.argv[i];
        const char *next = i + 1 < purr_run_settings.argc ? purr_run_settings.argv[i + 1] : NULL;
        if (strcmp(arg, "--host") == 0) {
            *request = (purr_session_request){.kind = PURR_REQUEST_HOST, .port = PURR_DEFAULT_PORT};
            if (next && next[0] >= '0' && next[0] <= '9') request->port = (uint32_t)strtoul(next, NULL, 10);
            return true;
        }
        if (strcmp(arg, "--join") == 0 && next) {
            *request = (purr_session_request){.kind = PURR_REQUEST_JOIN};
            snprintf(request->address, sizeof request->address, "%s", next);
            return true;
        }
    }
    return false;
}

// Opens the window, with the defaults for what `desc` leaves out.
static inline void purr_run_open(const purr_run_desc *desc)
{
    purr_run_settings = *desc;
    if (!purr_run_settings.title) purr_run_settings.title = "PurrEngine";
    if (purr_run_settings.width <= 0) purr_run_settings.width = 960;
    if (purr_run_settings.height <= 0) purr_run_settings.height = 540;
    if (purr_run_settings.tick_rate <= 0) purr_run_settings.tick_rate = 60;

    purr_platform_open(&(purr_window_desc){.title = purr_run_settings.title,
                                           .width = purr_run_settings.width,
                                           .height = purr_run_settings.height});
}

// A session for purr_run_game, in no match yet.
static inline purr_session *purr_run_new_session(void)
{
    return purr_session_create(&(purr_session_desc){
        .game = purr_run_game->game,
        .tick_rate = (uint32_t)purr_run_settings.tick_rate,
        .sample = purr_run_sample,
    });
}

// Starts purr_run_game: its local state and a session, then the match the
// command line asks for, or Main's if it's the match's.
static inline void purr_run_begin(void)
{
    const purr_host_game *game = purr_run_game;
    purr_run_local = calloc(1, game->local_size);
    purr_run_start = calloc(1, game->game->start_size);
    if (!purr_run_local || !purr_run_start) abort();
    game->local_init(purr_run_local);
    purr_run_session = purr_run_new_session();
    purr_session_request request = {0};
    if (purr_run_arguments(&request)) {
        if (game->main_is_local && request.kind == PURR_REQUEST_HOST) {
            fprintf(stderr, "purr: --host starts a match in Main, and Main is local: host from the game instead\n");
            request.kind = PURR_REQUEST_NONE;
        }
        purr_run_request(&request, NULL);
    } else if (!game->main_is_local) {
        // Main is the match's: a match on this machine alone starts right away.
        request.kind = PURR_REQUEST_PLAY;
        purr_run_request(&request, NULL);
    }
}

// Ends what purr_run_begin started: the match, if any, and the local state.
static inline void purr_run_end(void)
{
    purr_session_destroy(purr_run_session);
    purr_run_session = NULL;
    free(purr_run_local);
    free(purr_run_start);
    purr_run_local = NULL;
    purr_run_start = NULL;
    purr_run_dropped = false;
    memset(&purr_run_gui, 0, sizeof purr_run_gui);
}

static inline int purr_run_frame(void *user, const float seconds)
{
    (void)user;
    const purr_host_game *game = purr_run_game;
    purr_platform_poll(&purr_run_devices);
    purr_run_now += seconds;

    purr_session *s = purr_run_session;
    purr_session_update(s, purr_run_now);
    purr_session_event event;
    while (purr_session_next_event(s, &event)) {
        const bool connected = event.kind == PURR_SESSION_CONNECTED_EVENT;
        purr_run_dropped = !connected && event.reason != PURR_DISCONNECT_LEFT && event.reason != PURR_DISCONNECT_FAILED;
        if (connected) game->connected(purr_run_local);
        else game->disconnected(purr_run_local, event.reason);
    }
    const purr_session_status status = purr_session_status_of(s);
    game->set_session(purr_run_local, status.client.state, status.client.player, status.client.ping_ms,
                      status.server);

    // Views draw at the frame rate, the match blended between its last two ticks
    const purr_view_worlds view = purr_session_view(s);
    const void *match = view.current;
    purr_draw_reset(&purr_run_draw);
    purr_gui_begin(&purr_run_gui, &purr_run_devices, purr_platform_screen_size(), purr_platform_measure_text);
    game->frame(match, view.previous, view.alpha, purr_run_local, &purr_run_draw, &purr_run_gui);
    purr_gui_end(&purr_run_gui, &purr_run_draw);
    purr_platform_draw(&purr_run_draw);

    if (purr_run_settings.stats && match) {
        char stats[128];
        snprintf(stats, sizeof stats, "tick %d   entities %u   ping %u ms   %d fps", (int)game->tick(match),
                 (unsigned)game->entity_count(match), (unsigned)status.client.ping_ms, purr_platform_fps());
        purr_platform_draw_overlay(stats);
    }

    purr_session_request request;
    while (game->take_request(purr_run_local, &request, purr_run_start)) purr_run_request(&request, purr_run_start);
    return PURR_KEEP_RUNNING;
}

// Runs `game` in a window until it closes (see purr/run.h).
_Noreturn static inline void purr_host_run(const purr_run_desc *desc, const purr_host_game *game)
{
    purr_run_open(desc);
    purr_run_game = game;
    purr_run_begin();
    purr_platform_run(purr_run_frame, NULL);
}
