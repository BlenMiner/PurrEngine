#pragma once

// The standard host's loop: a game in a window, in a session, driven through
// a table of the game's functions (tide_host_game).
//
// tide/run.h makes the table for a game compiled into the program, which is
// how games run. Under `tide run` the game is a library instead, and
// tide_host_run_library swaps in each new build of it that tide makes: hot
// reloading. The table is what lets one loop do both.
//
// Temporary implementation written by Claude; the project owner takes it over
// later.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tide/layout.h"
#include "tide/platform.h"
#include "tide/session.h"

typedef struct tide_run_desc {
    const char *title; // Default "Tide"
    int width;         // Starting window size in pixels; default 960 x 540
    int height;
    int tick_rate;     // Ticks per second; default 60
    bool stats;        // Show the tick, entity count, ping and frame rate in a corner
    int argc;          // The command line, for --host, --join and --connect
    char **argv;
} tide_run_desc;

// A game as a host runs it: what sessions run (tide_game), and this machine's
// side of it: its local state, its views and its input. `local`, `world` and
// `start` are the game's tide_local, tide_world and tide_start.
typedef struct tide_host_game {
    const tide_game *game;
    uint32_t local_size; // sizeof(tide_local)
    bool main_is_local;  // TIDE_MAIN_IS_LOCAL: the program starts outside any match
    void (*local_init)(void *local);
    // tide_input_sample, into `input`; NULL for a game without an input
    void (*sample_input)(const tide_devices *devices, const void *local, void *input);
    void (*frame)(const void *world, const void *previous, float alpha, void *local, tide_draw_list *draw,
                  tide_gui *gui);
    bool (*take_request)(void *local, tide_session_request *request, void *start);
    // `room`: the code of the room the match is in, "" if none
    void (*set_session)(void *local, uint32_t state, tide_player_id player, uint32_t ping, bool server,
                        const char *room);
    void (*connected)(void *local);
    void (*disconnected)(void *local, uint32_t reason);
    int32_t (*tick)(const void *world); // Time.tick
    uint32_t (*entity_count)(const void *world);
    // Frees what the game's code keeps for itself (the scratch area), before
    // its library is unloaded.
    void (*unload)(void);
    // Its data layout, when tidec described it (tide run's builds), to carry
    // the game over to a build whose layout changed; NULL otherwise.
    const tide_layout *layout;
} tide_host_game;

// A game's library exports this function, which returns its table.
#define TIDE_HOST_LIBRARY "tide_host_library"
#ifdef _WIN32
#define TIDE_HOST_EXPORT __declspec(dllexport)
#else
#define TIDE_HOST_EXPORT __attribute__((visibility("default")))
#endif

// Runs the game from the libraries tide builds in `dir`: game-1 first (a .dll,
// .so or .dylib), then each newer one that appears there, swapped in between
// two frames. A build with the same data layout as the one running (the same
// tide_game hash) takes over the match and the local state where they are;
// with another, they're carried over to it by name (tide/migrate.h), or when
// that can't be done, the game starts over. A file named `restart` in `dir`
// starts it over too. Native builds only (platform/src/reload.c).
_Noreturn void tide_host_run_library(const tide_run_desc *desc, const char *dir);

// The same on the web (tide run --web), where each new build is a program of
// its own, which the page starts in the running one's place: `game` goes on
// from what the last one left it, carried over by name, or starts as
// tide_host_run would. Web builds only (platform/src/reload.c).
_Noreturn void tide_host_run_web(const tide_run_desc *desc, const tide_host_game *game);

// ---------------------------------------------------------------------------
// The loop, for tide/run.h and platform/src/reload.c

static const tide_host_game *tide_run_game;
static void *tide_run_local;  // The game's tide_local
static void *tide_run_start;  // A tide_start, for what local code asks
static tide_devices tide_run_devices;
static tide_draw_list tide_run_draw;
static tide_gui tide_run_gui;
static tide_run_desc tide_run_settings;
static tide_session *tide_run_session;
static double tide_run_now; // Seconds since the program started
// The server went away or turned this machine away: not because it left,
// couldn't start, or the match ran out of scenes
static bool tide_run_dropped;

// This machine's input for one tick.
static inline void tide_run_sample(void *user, const uint32_t tick, void *input)
{
    (void)user;
    (void)tick;
    if (!tide_run_game->sample_input) return;
    tide_devices sampled = tide_run_devices;
    tide_gui_hide(&tide_run_gui, &sampled); // A click on a button isn't the game's
    tide_run_game->sample_input(&sampled, tide_run_local, input);
    tide_devices_consume(&tide_run_devices);
}

// Starts, joins or leaves a match, as local code or the command line asked.
// `start` is a tide_start, or NULL for Main.
static inline void tide_run_request(const tide_session_request *request, const void *start)
{
    tide_session *s = tide_run_session;
    tide_transport network;
    tide_address server;
    const bool port_ok = request->port <= 65535u;
    const uint16_t port = port_ok ? (uint16_t)request->port : 0u;
    switch (request->kind) {
    case TIDE_REQUEST_PLAY:
        tide_session_play(s, start, tide_run_now);
        break;
    case TIDE_REQUEST_HOST:
        // Players join on a UDP port (not on the web), and in a room
        if (port_ok && tide_platform_host_open(port, &network)) {
            tide_session_host(s, start, network, tide_run_now);
            break;
        }
        fprintf(stderr, "tide: can't take players on port %u\n", (unsigned)request->port);
        tide_session_leave(s);
        tide_session_fail(s, TIDE_DISCONNECT_FAILED);
        break;
    case TIDE_REQUEST_JOIN:
        if (!tide_platform_room_join(request->address, &network, &server)) {
            fprintf(stderr, "tide: can't join room '%s'\n", request->address);
            tide_session_leave(s);
            tide_session_fail(s, TIDE_DISCONNECT_FAILED);
            break;
        }
        tide_session_join(s, network, server, tide_run_now);
        break;
    case TIDE_REQUEST_CONNECT:
        if (!port || !tide_platform_resolve(request->address, port, &server) || !tide_platform_udp_open(0, &network)) {
            fprintf(stderr, "tide: can't reach '%s'\n", request->address);
            tide_session_leave(s);
            tide_session_fail(s, TIDE_DISCONNECT_FAILED);
            break;
        }
        tide_session_join(s, network, server, tide_run_now);
        break;
    case TIDE_REQUEST_LEAVE:
        tide_session_leave(s);
        break;
    default:
        break;
    }
}

// --host [port], --join code or --connect address, from the command line.
static inline bool tide_run_arguments(tide_session_request *request)
{
    for (int i = 1; i < tide_run_settings.argc; i++) {
        const char *arg = tide_run_settings.argv[i];
        const char *next = i + 1 < tide_run_settings.argc ? tide_run_settings.argv[i + 1] : NULL;
        if (strcmp(arg, "--host") == 0) {
            *request = (tide_session_request){.kind = TIDE_REQUEST_HOST, .port = TIDE_DEFAULT_PORT};
            if (next && next[0] >= '0' && next[0] <= '9') request->port = (uint32_t)strtoul(next, NULL, 10);
            return true;
        }
        const bool join = strcmp(arg, "--join") == 0;
        if ((join || strcmp(arg, "--connect") == 0) && next) {
            *request = (tide_session_request){.kind = join ? TIDE_REQUEST_JOIN : TIDE_REQUEST_CONNECT,
                                              .port = TIDE_DEFAULT_PORT};
            snprintf(request->address, sizeof request->address, "%s", next);
            return true;
        }
    }
    return false;
}

// Opens the window, with the defaults for what `desc` leaves out.
static inline void tide_run_open(const tide_run_desc *desc)
{
    tide_run_settings = *desc;
    if (!tide_run_settings.title) tide_run_settings.title = "Tide";
    if (tide_run_settings.width <= 0) tide_run_settings.width = 960;
    if (tide_run_settings.height <= 0) tide_run_settings.height = 540;
    if (tide_run_settings.tick_rate <= 0) tide_run_settings.tick_rate = 60;

    tide_platform_open(&(tide_window_desc){.title = tide_run_settings.title,
                                           .width = tide_run_settings.width,
                                           .height = tide_run_settings.height});
}

// A session for tide_run_game, in no match yet.
static inline tide_session *tide_run_new_session(void)
{
    return tide_session_create(&(tide_session_desc){
        .game = tide_run_game->game,
        .tick_rate = (uint32_t)tide_run_settings.tick_rate,
        .sample = tide_run_sample,
    });
}

// Starts tide_run_game: its local state and a session, then the match the
// command line asks for, or Main's if it's the match's.
static inline void tide_run_begin(void)
{
    const tide_host_game *game = tide_run_game;
    tide_run_local = calloc(1, game->local_size);
    tide_run_start = calloc(1, game->game->start_size);
    if (!tide_run_local || !tide_run_start) abort();
    game->local_init(tide_run_local);
    tide_run_session = tide_run_new_session();
    tide_session_request request = {0};
    if (tide_run_arguments(&request)) {
        if (game->main_is_local && request.kind == TIDE_REQUEST_HOST) {
            fprintf(stderr, "tide: --host starts a match in Main, and Main is local: host from the game instead\n");
            request.kind = TIDE_REQUEST_NONE;
        }
        tide_run_request(&request, NULL);
    } else if (!game->main_is_local) {
        // Main is the match's: a match on this machine alone starts right away.
        request.kind = TIDE_REQUEST_PLAY;
        tide_run_request(&request, NULL);
    }
}

// Ends what tide_run_begin started: the match, if any, and the local state.
static inline void tide_run_end(void)
{
    tide_session_destroy(tide_run_session);
    tide_run_session = NULL;
    free(tide_run_local);
    free(tide_run_start);
    tide_run_local = NULL;
    tide_run_start = NULL;
    tide_run_dropped = false;
    memset(&tide_run_gui, 0, sizeof tide_run_gui);
}

static inline int tide_run_frame(void *user, const float seconds)
{
    (void)user;
    const tide_host_game *game = tide_run_game;
    tide_platform_poll(&tide_run_devices);
    tide_run_now += seconds;

    tide_session *s = tide_run_session;
    // A room joined that isn't one, or whose host can't be reached, fails now
    // rather than timing out.
    if (tide_platform_room_failed()) tide_session_fail(s, TIDE_DISCONNECT_FAILED);
    tide_session_update(s, tide_run_now);
    tide_session_event event;
    while (tide_session_next_event(s, &event)) {
        const bool connected = event.kind == TIDE_SESSION_CONNECTED_EVENT;
        tide_run_dropped = !connected && event.reason != TIDE_DISCONNECT_LEFT && event.reason != TIDE_DISCONNECT_FAILED &&
                           event.reason != TIDE_DISCONNECT_ENDED;
        if (connected) game->connected(tide_run_local);
        else game->disconnected(tide_run_local, event.reason);
    }
    const tide_session_status status = tide_session_status_of(s);
    char room[TIDE_ROOM_CODE_LENGTH + 1];
    tide_platform_room_code(room, sizeof room);
    game->set_session(tide_run_local, status.client.state, status.client.player, status.client.ping_ms,
                      status.server, room);

    // Views draw at the frame rate, the match blended between its last two ticks
    const tide_view_worlds view = tide_session_view(s);
    const void *match = view.current;
    tide_draw_reset(&tide_run_draw);
    tide_gui_begin(&tide_run_gui, &tide_run_devices, tide_platform_screen_size(), tide_platform_measure_text);
    game->frame(match, view.previous, view.alpha, tide_run_local, &tide_run_draw, &tide_run_gui);
    tide_gui_end(&tide_run_gui, &tide_run_draw);
    tide_platform_draw(&tide_run_draw);

    if (tide_run_settings.stats && match) {
        char stats[128];
        snprintf(stats, sizeof stats, "tick %d   entities %u   ping %u ms   %d fps", (int)game->tick(match),
                 (unsigned)game->entity_count(match), (unsigned)status.client.ping_ms, tide_platform_fps());
        tide_platform_draw_overlay(stats);
    }

    tide_session_request request;
    while (game->take_request(tide_run_local, &request, tide_run_start)) tide_run_request(&request, tide_run_start);
    return TIDE_KEEP_RUNNING;
}

// Runs `game` in a window until it closes (see tide/run.h).
_Noreturn static inline void tide_host_run(const tide_run_desc *desc, const tide_host_game *game)
{
    tide_run_open(desc);
    tide_run_game = game;
    tide_run_begin();
    tide_platform_run(tide_run_frame, NULL);
}
