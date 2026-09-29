#pragma once

// Runs a PurrLang game in a window. Include it after the game's generated
// header, in the file with main:
//
//     #include "game.h"
//     #include "purr/run.h"
//
//     int main(int argc, char **argv)
//     {
//         purr_run(&(purr_run_desc){.title = "Sandbox", .argc = argc, .argv = argv});
//     }
//
// The game runs in a session (purr/session.h). If its Main scene is the
// match's, a match starts right away: on this machine alone, or with others:
//
//     game --host [port]   a match others can join (port 7777 by default)
//     game --join address  the match at "192.168.1.5", "localhost:7777" and the like
//
// If Main is local, the program starts in it, with no match, and local code
// starts one with Session.Play, Host or Join (--join works too). This machine's
// player joins before the match's first tick. If the game has an input, the
// devices are sampled once per tick this machine runs, minus what the GUI is
// using. The views draw every frame, and their GUI over the world. Close the
// window to quit.
//
// Temporary implementation written by Claude; the project owner takes it over
// later.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static purr_local purr_run_local;
static purr_devices purr_run_devices;
static purr_draw_list purr_run_draw;
static purr_gui purr_run_gui;
static purr_run_desc purr_run_settings;
static purr_session *purr_run_session;
static double purr_run_now; // Seconds since the program started

// This machine's input for one tick.
static inline void purr_run_sample(void *user, const uint32_t tick, void *input)
{
    (void)user;
    (void)tick;
#ifdef PURR_HAS_INPUT
    purr_devices sampled = purr_run_devices;
    purr_gui_hide(&purr_run_gui, &sampled); // A click on a button isn't the game's
    *(purr_input *)input = purr_input_sample(&sampled, &purr_run_local);
    purr_devices_consume(&purr_run_devices);
#else
    (void)input;
#endif
}

// Starts, joins or leaves a match, as local code or the command line asked.
static inline void purr_run_request(const purr_session_request *request, const purr_start *start)
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

static inline int purr_run_frame(void *user, const float seconds)
{
    (void)user;
    purr_platform_poll(&purr_run_devices);
    purr_run_now += seconds;

    purr_session *s = purr_run_session;
    purr_session_update(s, purr_run_now);
    purr_session_event event;
    while (purr_session_next_event(s, &event)) {
        if (event.kind == PURR_SESSION_CONNECTED_EVENT) purr_local_connected(&purr_run_local);
        else purr_local_disconnected(&purr_run_local, event.reason);
    }
    const purr_session_status status = purr_session_status_of(s);
    purr_local_set_session(&purr_run_local, status.client.state, status.client.player, status.client.ping_ms, status.server);

    // Views draw at the frame rate, the match blended between its last two ticks
    const purr_view_worlds view = purr_session_view(s);
    const purr_world *match = view.current;
    purr_draw_reset(&purr_run_draw);
    purr_gui_begin(&purr_run_gui, &purr_run_devices, purr_platform_screen_size(), purr_platform_measure_text);
    purr_frame(match, view.previous, view.alpha, &purr_run_local, &purr_run_draw, &purr_run_gui);
    purr_gui_end(&purr_run_gui, &purr_run_draw);
    purr_platform_draw(&purr_run_draw);

    if (purr_run_settings.stats && match) {
        char stats[128];
        snprintf(stats, sizeof stats, "tick %d   entities %u   ping %u ms   %d fps", (int)match->Time.tick,
                 (unsigned)purr_world_entity_count(match), (unsigned)status.client.ping_ms, purr_platform_fps());
        purr_platform_draw_overlay(stats);
    }

    purr_session_request request;
    purr_start start;
    while (purr_local_take_request(&purr_run_local, &request, &start)) purr_run_request(&request, &start);
    return PURR_KEEP_RUNNING;
}

_Noreturn static inline void purr_run(const purr_run_desc *desc)
{
    purr_run_settings = *desc;
    if (!purr_run_settings.title) purr_run_settings.title = "PurrEngine";
    if (purr_run_settings.width <= 0) purr_run_settings.width = 960;
    if (purr_run_settings.height <= 0) purr_run_settings.height = 540;
    if (purr_run_settings.tick_rate <= 0) purr_run_settings.tick_rate = 60;

    purr_platform_open(&(purr_window_desc){.title = purr_run_settings.title,
                                           .width = purr_run_settings.width,
                                           .height = purr_run_settings.height});
    purr_local_init(&purr_run_local);
    purr_run_session = purr_session_create(&(purr_session_desc){
        .game = &purr_game_api,
        .tick_rate = (uint32_t)purr_run_settings.tick_rate,
        .sample = purr_run_sample,
    });
    purr_session_request request = {0};
    const purr_start main_scene = {.scene = -1};
    if (purr_run_arguments(&request)) {
#ifdef PURR_MAIN_IS_LOCAL
        if (request.kind == PURR_REQUEST_HOST) {
            fprintf(stderr, "purr: --host starts a match in Main, and Main is local: host from the game instead\n");
            request.kind = PURR_REQUEST_NONE;
        }
#endif
        purr_run_request(&request, &main_scene);
    } else {
#ifndef PURR_MAIN_IS_LOCAL
        // Main is the match's: a match on this machine alone starts right away.
        request.kind = PURR_REQUEST_PLAY;
        purr_run_request(&request, &main_scene);
#endif
    }
    purr_platform_run(purr_run_frame, NULL);
}
