// The demo's host: runs the game in a session (purr/session.h), feeds it this
// machine's input, and renders what the game's views draw.
//
//     demo                  on this machine alone
//     demo --host [port]    a match others can join (port 7777 by default)
//     demo --join address   the match at "192.168.1.5", "localhost:7777" and the like
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
#include "purr/platform.h"

#define WIDTH 960
#define HEIGHT 540
#define TICK_RATE 60

#define SMOKE_TICKS 240
// Update when the demo's simulation changes on purpose. Every platform must agree.
#define SMOKE_HASH 0x1D9D22B70969DF47ull

// Colors demo.purr's views use, as 0xRRGGBBAA: the player's and the background.
#define SMOKE_PLAYER_COLOR 0xFFC43DFFu
#define SMOKE_BACKGROUND 0x14141CFFu

static purr_session *session;
static double now; // Seconds since the start; made up in the smoke test, one tick per frame
static purr_local local;
static purr_devices devices;
static purr_draw_list draw;
static purr_gui gui;
static bool smoke;

static void smoke_input(uint32_t t);

// This machine's input for one tick: the scripted player's in the smoke test.
static void sample(void *user, const uint32_t tick, void *input)
{
    (void)user;
    if (smoke) smoke_input(tick);
    purr_devices sampled = devices;
    purr_gui_hide(&gui, &sampled); // What the GUI is using isn't the player's
    *(PlayerInput *)input = purr_input_sample(&sampled, &local);
    purr_devices_consume(&devices);
}

static purr_view_worlds update(void)
{
    purr_session_update(session, now);
    purr_session_event event;
    while (purr_session_next_event(session, &event)) {
        if (event.kind == PURR_SESSION_CONNECTED_EVENT) purr_local_connected(&local);
        else purr_local_disconnected(&local, event.reason);
    }
    const purr_session_status status = purr_session_status_of(session);
    purr_local_set_session(&local, status.client.state, status.client.player, status.client.ping_ms, status.server);
    return purr_session_view(session);
}

// The views draw the match blended between its last two ticks, smooth at any tick rate.
static void render(const purr_view_worlds view)
{
    purr_draw_reset(&draw);
    purr_gui_begin(&gui, &devices, purr_platform_screen_size(), purr_platform_measure_text);
    purr_frame(view.current, view.previous, view.alpha, &local, &draw, &gui);
    purr_gui_end(&gui, &draw);
    purr_platform_draw(&draw);
}

static int play_frame(const float seconds)
{
    purr_platform_poll(&devices);
    now += seconds;
    const purr_view_worlds view = update();
    const purr_world *match = view.current;
    render(view);

    char stats[128];
    const purr_session_status status = purr_session_status_of(session);
    snprintf(stats, sizeof stats, "tick %d   entities %u   ping %u ms   %d fps", match ? (int)match->Time.tick : 0,
             match ? (unsigned)purr_world_entity_count(match) : 0u, (unsigned)status.client.ping_ms, purr_platform_fps());
    purr_platform_draw_overlay(stats);
    return PURR_KEEP_RUNNING;
}

// The scripted player: right, then up and right, then up, then down and left
// with the stick, firing every 40 ticks.
static void smoke_input(const uint32_t t)
{
    purr_button_set(&devices.keyboard.d, t < 90);
    purr_button_set(&devices.keyboard.w, t >= 60 && t < 120);
    purr_button_set(&devices.keyboard.space, t % 40 < 2);
    devices.gamepad.connected = true;
    devices.gamepad.leftStick = t >= 150 && t < 200 ? purr_f2(-0.6f, -0.3f) : purr_f2(0.0f, 0.0f);
}

static const Body *find_player(const purr_world *w)
{
    for (uint32_t i = 0; i < w->entities.next_unused; i++) {
        const purr_entity e = {i, w->entities.slots[i].generation};
        if (purr_get_Owner((purr_world *)w, e)) return purr_get_Body((purr_world *)w, e);
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
static uint64_t world_hash(const purr_world *w)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < w->entities.next_unused; i++) {
        const purr_entity e = {i, w->entities.slots[i].generation};
        const Body *body = purr_get_Body((purr_world *)w, e);
        if (!body) continue;
        h = fnv1a(h, &e, sizeof e);
        h = fnv1a(h, body, sizeof *body);
    }
    return h;
}

// Renders the views offscreen and reads pixels back, which proves the whole
// path works (PurrLang views, the draw list, raylib, WebGL on the web) without
// needing a visible window. The frame's render() set the camera for
// purr_platform_world_to_screen.
static bool smoke_pixels(const purr_world *w)
{
    const Body *player = find_player(w);
    if (!player) {
        printf("smoke: no player\n");
        return false;
    }
    const purr_float2 points[2] = {purr_platform_world_to_screen(player->position), purr_f2(2.0f, 2.0f)};
    uint32_t rgba[2];
    purr_platform_read_pixels(&draw, points, 2, rgba);

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
    purr_view_worlds view = update();
    const purr_world *match = view.current;
    view.previous = NULL; // Drawn as it is, for the pixels the check reads
    render(view);
    const purr_world *server = purr_session_server_world(session);
    if (!server || server->Time.tick < SMOKE_TICKS) return PURR_KEEP_RUNNING;

    const uint64_t hash = world_hash(server);
    printf("smoke: %d ticks, %u entities, hash 0x%016llX (expected 0x%016llX)\n", (int)server->Time.tick,
           (unsigned)purr_world_entity_count(server), (unsigned long long)hash, SMOKE_HASH);
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
    purr_platform_open(&(purr_window_desc){.title = "PurrEngine demo", .width = WIDTH, .height = HEIGHT, .hidden = smoke});
    purr_local_init(&local);
    session = purr_session_create(&(purr_session_desc){.game = &purr_game_api, .tick_rate = TICK_RATE, .sample = sample});

    const char *host = NULL;
    const char *join = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--host") == 0) host = i + 1 < argc ? argv[i + 1] : "";
        if (strcmp(argv[i], "--join") == 0 && i + 1 < argc) join = argv[i + 1];
    }
    purr_transport udp;
    purr_address server;
    if (host) {
        const unsigned long port = host[0] >= '0' && host[0] <= '9' ? strtoul(host, NULL, 10) : PURR_DEFAULT_PORT;
        if (port > 65535u || !purr_platform_udp_open((uint16_t)port, &udp)) {
            fprintf(stderr, "demo: can't take players on port %lu\n", port);
            return 1;
        }
        purr_session_host(session, NULL, udp, now);
        printf("demo: hosting on port %lu\n", port);
    } else if (join) {
        if (!purr_platform_resolve(join, PURR_DEFAULT_PORT, &server) || !purr_platform_udp_open(0, &udp)) {
            fprintf(stderr, "demo: can't reach '%s'\n", join);
            return 1;
        }
        purr_session_join(session, udp, server, now);
    } else {
        purr_session_play(session, NULL, now);
    }
    purr_platform_run(frame, NULL);
}
