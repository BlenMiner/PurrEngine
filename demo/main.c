// The demo's host: runs the game in a session (tide/session.h), feeds it this
// machine's input, and renders what the game's views draw.
//
//     demo                     on this machine alone
//     demo --host [port]       a match others can join: on a port (7777 by default) and in a room
//     demo --join code         the match in the room with this code, like K7QF2M
//     demo --connect address   the match at "192.168.1.5", "localhost:7777" and the like
//
// `demo --smoke` (`demo.html?smoke` on the web) replaces the player with a
// script, then checks the simulation's result and a rendered pixel, and exits
// with 0 if both are right. The expected hash is the same on every platform, so
// the check also guards determinism between desktop and the web.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "demo.h"
#include "tide/platform.h"

#define WIDTH 960
#define HEIGHT 540
#define TICK_RATE 60

#define SMOKE_TICKS 240
// Update when the demo's simulation changes on purpose. Every platform must agree.
#define SMOKE_HASH 0x1D9D22B70969DF47ull

// Colors demo.tide's views use, as 0xRRGGBBAA: the player's and the background.
#define SMOKE_PLAYER_COLOR 0xFFC43DFFu
#define SMOKE_BACKGROUND 0x14141CFFu

static tide_session *session;
static double now; // Seconds since the start; made up in the smoke test, one tick per frame
static tide_local local;
static tide_devices devices;
static tide_draw_list draw;
static tide_gui gui;
static bool smoke;

static void smoke_input(uint32_t t);

// This machine's input for one tick: the scripted player's in the smoke test.
static void sample(void *user, const uint32_t tick, void *input)
{
    (void)user;
    if (smoke) smoke_input(tick);
    tide_devices sampled = devices;
    tide_gui_hide(&gui, &sampled); // What the GUI is using isn't the player's
    *(PlayerInput *)input = tide_input_sample(&sampled, &local);
    tide_devices_consume(&devices);
}

static tide_view_worlds update(void)
{
    tide_session_update(session, now);
    tide_session_event event;
    while (tide_session_next_event(session, &event)) {
        if (event.kind == TIDE_SESSION_CONNECTED_EVENT) tide_local_connected(&local);
        else tide_local_disconnected(&local, event.reason, event.message);
    }
    const tide_session_status status = tide_session_status_of(session);
    char room[TIDE_ROOM_CODE_LENGTH + 1] = "";
    if (status.open || !status.server) tide_platform_room_code(room, sizeof room);
    tide_local_set_session(&local, status.client.state, status.client.player, status.client.ping_ms, status.server,
                           status.open, room);
    return tide_session_view(session);
}

// The views draw the match blended between its last two ticks, smooth at any tick rate.
static void render(const tide_view_worlds view)
{
    tide_draw_reset(&draw);
    tide_gui_begin(&gui, &devices, tide_platform_screen_size(), tide_platform_measure_text);
    tide_frame(view.current, view.previous, view.alpha, &local, &draw, &gui);
    tide_gui_end(&gui, &draw);
    tide_platform_draw(&draw);
}

static int play_frame(const float seconds)
{
    tide_platform_poll(&devices);
    now += seconds;
    const tide_view_worlds view = update();
    const tide_world *match = view.current;
    render(view);

    char stats[128];
    const tide_session_status status = tide_session_status_of(session);
    snprintf(stats, sizeof stats, "tick %d   entities %u   ping %u ms   %d fps", match ? (int)match->Time.tick : 0,
             match ? (unsigned)tide_world_entity_count(match) : 0u, (unsigned)status.client.ping_ms, tide_platform_fps());
    tide_platform_draw_overlay(stats);
    tide_platform_next_frame(tide_session_until_tick(session)); // While minimized: the next tick
    return TIDE_KEEP_RUNNING;
}

// The scripted player: right, then up and right, then up, then down and left
// with the stick, firing every 40 ticks.
static void smoke_input(const uint32_t t)
{
    tide_button_set(&devices.keyboard.d, t < 90);
    tide_button_set(&devices.keyboard.w, t >= 60 && t < 120);
    tide_button_set(&devices.keyboard.space, t % 40 < 2);
    devices.gamepad.connected = true;
    devices.gamepad.leftStick = t >= 150 && t < 200 ? tide_f2(-0.6f, -0.3f) : tide_f2(0.0f, 0.0f);
}

static const Body *find_player(const tide_world *w)
{
    for (uint32_t i = 0; i < w->entities.next_unused; i++) {
        const tide_entity e = tide_entity_in_slot(&w->entities, i);
        if (tide_read_Owner(w, e)) return tide_read_Body(w, e);
    }
    return NULL;
}

static uint64_t fnv1a(uint64_t h, const void *data, const size_t size)
{
    const unsigned char *bytes = data;
    for (size_t i = 0; i < size; i++) h = (h ^ bytes[i]) * 0x100000001b3ull;
    return h;
}

// Every Body's exact bits, in entity order.
static uint64_t world_hash(const tide_world *w)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < w->entities.next_unused; i++) {
        const tide_entity e = tide_entity_in_slot(&w->entities, i);
        const Body *body = tide_read_Body(w, e);
        if (!body) continue;
        h = fnv1a(h, &e, sizeof e);
        h = fnv1a(h, body, sizeof *body);
    }
    return h;
}

// Renders the views offscreen and reads pixels back, which proves the whole
// path works (Tide views, the draw list, raylib, WebGL on the web) without
// needing a visible window. The frame's render() set the camera for
// tide_platform_world_to_screen.
static bool smoke_pixels(const tide_world *w)
{
    const Body *player = find_player(w);
    if (!player) {
        printf("smoke: no player\n");
        return false;
    }
    const tide_float2 points[2] = {tide_platform_world_to_screen(player->position), tide_f2(2.0f, 2.0f)};
    uint32_t rgba[2];
    tide_platform_read_pixels(&draw, points, 2, rgba);

    const bool ok = rgba[0] == SMOKE_PLAYER_COLOR && rgba[1] == SMOKE_BACKGROUND;
    printf("smoke: player pixel 0x%08X, corner pixel 0x%08X%s\n", (unsigned)rgba[0], (unsigned)rgba[1],
           ok ? "" : ", expected the player and background colors");
    return ok;
}

static double smoke_seconds; // Real time the smoke frames took

// One tick per frame, in made-up time, so the run is the same everywhere.
static int smoke_frame(const float seconds)
{
    smoke_seconds += seconds;
    now += 1.0 / TICK_RATE;
    tide_view_worlds view = update();
    const tide_world *match = view.current;
    view.previous = NULL; // Drawn as it is, for the pixels the check reads
    render(view);
    const tide_world *server = tide_session_server_world(session);
    if (!server || server->Time.tick < SMOKE_TICKS) return TIDE_KEEP_RUNNING;

    const uint64_t hash = world_hash(server);
    printf("smoke: %d ticks, %u entities, hash 0x%016llX (expected 0x%016llX)\n", (int)server->Time.tick,
           (unsigned)tide_world_entity_count(server), (unsigned long long)hash, SMOKE_HASH);
    // A frame loop that doesn't present frames and poll events (raylib built
    // with SUPPORT_CUSTOM_FRAME_CONTROL) never advances its frame time either.
    const bool frames = smoke_seconds > 0.0;
    printf("smoke: frames took %.3f s%s\n", smoke_seconds, frames ? "" : ", expected time to pass");
    const bool pixels = smoke_pixels(match);
    const bool ok = hash == SMOKE_HASH && pixels && frames;
    printf("smoke: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

static int frame(void *user, const float seconds)
{
    (void)user;
    return smoke ? smoke_frame(seconds) : play_frame(seconds);
}

int main(const int argc, char **argv)
{
    smoke = argc > 1 && strcmp(argv[1], "--smoke") == 0;
    tide_platform_open(&(tide_window_desc){.title = "Tide demo", .width = WIDTH, .height = HEIGHT, .hidden = smoke});
    tide_local_init(&local);
    session = tide_session_create(&(tide_session_desc){.game = &tide_game_api, .tick_rate = TICK_RATE, .sample = sample});

    const char *host = NULL;
    const char *join = NULL;
    const char *connect = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--host") == 0) host = i + 1 < argc ? argv[i + 1] : "";
        if (strcmp(argv[i], "--join") == 0 && i + 1 < argc) join = argv[i + 1];
        if (strcmp(argv[i], "--connect") == 0 && i + 1 < argc) connect = argv[i + 1];
    }
    tide_transport network;
    tide_address server;
    if (host) {
        const unsigned long port = host[0] >= '0' && host[0] <= '9' ? strtoul(host, NULL, 10) : TIDE_DEFAULT_PORT;
        if (port > 65535u || !tide_platform_host_open((uint16_t)port, &network)) {
            fprintf(stderr, "demo: can't take players on port %lu\n", port);
            return 1;
        }
        char room[TIDE_ROOM_CODE_LENGTH + 1];
        tide_platform_room_code(room, sizeof room);
        printf("demo: hosting on port %lu (not on the web), and in room %s\n", port, room);
        tide_session_start(session, NULL, now);
        tide_session_open(session, network);
    } else if (join) {
        if (!tide_platform_room_join(join, &network, &server)) {
            fprintf(stderr, "demo: can't join room '%s'\n", join);
            return 1;
        }
        tide_session_join(session, network, server, now);
    } else if (connect) {
        if (!tide_platform_resolve(connect, TIDE_DEFAULT_PORT, &server) || !tide_platform_udp_open(0, &network)) {
            fprintf(stderr, "demo: can't reach '%s'\n", connect);
            return 1;
        }
        tide_session_join(session, network, server, now);
    } else {
        tide_session_start(session, NULL, now);
    }
    tide_platform_run(frame, NULL);
}
