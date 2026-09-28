// The demo's host: runs the simulation at a fixed tick rate, feeds it the local
// player's input, and renders what the game's views draw.
//
// `demo --smoke` (`demo.html?smoke` on the web) replaces the player with a
// script, then checks the simulation's result and a rendered pixel, and exits
// with 0 if both are right. The expected hash is the same on every platform, so
// the check also guards determinism between desktop and the web.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "demo.h"
#include "purr/platform.h"

#define WIDTH 960
#define HEIGHT 540
#define TICK_RATE 60
#define MAX_TICKS_PER_FRAME 8 // After a long stall, drop the time instead of catching up

#define SMOKE_TICKS 240
// Update when the demo's simulation changes on purpose. Every platform must agree.
#define SMOKE_HASH 0x87805726297668B9ull

// Colors demo.purr's views use, as 0xRRGGBBAA: the player's and the background.
#define SMOKE_PLAYER_COLOR 0xFFC43DFFu
#define SMOKE_BACKGROUND 0x14141CFFu

static purr_world world;
static purr_devices devices;
static purr_draw_list draw;
static bool smoke;
static int32_t smoke_ticks;
static double unsimulated; // Seconds of real time not simulated yet

static void tick(void)
{
    const PlayerInput input = purr_input_sample(&devices);
    purr_devices_consume(&devices);
    purr_world_set_input(&world, purr_player_from_index(0), input);
    purr_world_set_server_input(&world, input); // Local play: this machine is also the server
    purr_world_tick(&world);
}

static void render(void)
{
    purr_draw_reset(&draw);
    purr_world_draw(&world, &draw);
    purr_platform_draw(&draw);
}

static int play_frame(const float seconds)
{
    purr_platform_poll(&devices);

    const double dt = 1.0 / TICK_RATE;
    unsimulated += seconds;
    for (int ticks = 0; unsimulated >= dt && ticks < MAX_TICKS_PER_FRAME; ticks++) {
        tick();
        unsimulated -= dt;
    }
    if (unsimulated >= dt) unsimulated = 0.0;

    render();

    char stats[96];
    snprintf(stats, sizeof stats, "tick %d   entities %u   %d fps", (int)world.Time.tick,
             (unsigned)purr_world_entity_count(&world), purr_platform_fps());
    purr_platform_draw_overlay(stats);
    return PURR_KEEP_RUNNING;
}

// The scripted player: right, then up and right, then up, then down and left
// with the stick, firing every 40 ticks.
static void smoke_input(const int32_t t)
{
    purr_button_set(&devices.keyboard.d, t < 90);
    purr_button_set(&devices.keyboard.w, t >= 60 && t < 120);
    purr_button_set(&devices.keyboard.space, t % 40 < 2);
    devices.gamepad.connected = true;
    devices.gamepad.leftStick = t >= 150 && t < 200 ? purr_f2(-0.6f, -0.3f) : purr_f2(0.0f, 0.0f);
}

static const Body *find_player(void)
{
    for (uint32_t i = 0; i < world.entities.next_unused; i++) {
        const purr_entity e = {i, world.entities.slots[i].generation};
        if (purr_get_Owner(&world, e)) return purr_get_Body(&world, e);
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
static uint64_t world_hash(void)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < world.entities.next_unused; i++) {
        const purr_entity e = {i, world.entities.slots[i].generation};
        const Body *body = purr_get_Body(&world, e);
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
static bool smoke_pixels(void)
{
    const purr_float2 points[2] = {purr_platform_world_to_screen(find_player()->position), purr_f2(2.0f, 2.0f)};
    uint32_t rgba[2];
    purr_platform_read_pixels(&draw, points, 2, rgba);

    const bool ok = rgba[0] == SMOKE_PLAYER_COLOR && rgba[1] == SMOKE_BACKGROUND;
    printf("smoke: player pixel 0x%08X, corner pixel 0x%08X%s\n", (unsigned)rgba[0], (unsigned)rgba[1],
           ok ? "" : ", expected the player and background colors");
    return ok;
}

static double smoke_seconds; // Real time the smoke frames took

static int smoke_frame(const float seconds)
{
    smoke_seconds += seconds;
    smoke_input(smoke_ticks);
    tick();
    smoke_ticks++;
    render();
    if (smoke_ticks < SMOKE_TICKS) return PURR_KEEP_RUNNING;

    const uint64_t hash = world_hash();
    printf("smoke: %d ticks, %u entities, hash 0x%016llX (expected 0x%016llX)\n", (int)smoke_ticks,
           (unsigned)purr_world_entity_count(&world), (unsigned long long)hash, SMOKE_HASH);
    // A frame loop that doesn't present frames and poll events (raylib built
    // with SUPPORT_CUSTOM_FRAME_CONTROL) never advances its frame time either.
    const bool frames = smoke_seconds > 0.0;
    printf("smoke: frames took %.3f s%s\n", smoke_seconds, frames ? "" : ", expected time to pass");
    const bool pixels = smoke_pixels();
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
    purr_world_init(&world, 1.0f / TICK_RATE);
    purr_platform_run(frame, NULL);
}
