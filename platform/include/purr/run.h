#pragma once

// Runs a PurrLang game in a window. Include it after the game's generated
// header, in the file with main:
//
//     #include "game.h"
//     #include "purr/run.h"
//
//     int main(void)
//     {
//         purr_run(&(purr_run_desc){.title = "Sandbox"});
//     }
//
// The simulation ticks at a fixed rate. Player 0 joins before the first tick.
// If the game declares an input, the devices are sampled every tick as player
// 0's input and, since this machine is also the server, as the server's. The
// views draw every frame. Close the window to quit.
//
// Temporary implementation written by Claude; the project owner takes it over
// later.

#include <stdio.h>

#include "purr/platform.h"

typedef struct purr_run_desc {
    const char *title; // Default "PurrEngine"
    int width;         // Starting window size in pixels; default 960 x 540
    int height;
    int tick_rate;     // Ticks per second; default 60
    bool stats;        // Show the tick, entity count and frame rate in a corner
} purr_run_desc;

// After a long stall (a breakpoint, a dragged window), drop the time instead of
// catching up tick by tick.
#define PURR_RUN_MAX_TICKS_PER_FRAME 8

static purr_world purr_run_world;
static purr_local purr_run_local;
static purr_devices purr_run_devices;
static purr_draw_list purr_run_draw;
static purr_run_desc purr_run_settings;
static double purr_run_unsimulated; // Seconds of real time not simulated yet

static inline void purr_run_tick(void)
{
#ifdef PURR_HAS_INPUT
    const purr_input input = purr_input_sample(&purr_run_devices);
    purr_devices_consume(&purr_run_devices);
    purr_world_set_input(&purr_run_world, purr_player_from_index(0), input);
    purr_world_set_server_input(&purr_run_world, input); // Local play: this machine is also the server
#endif
    purr_world_tick(&purr_run_world);
}

static inline int purr_run_frame(void *user, const float seconds)
{
    (void)user;
    purr_platform_poll(&purr_run_devices);

    const double dt = 1.0 / purr_run_settings.tick_rate;
    purr_run_unsimulated += seconds;
    for (int ticks = 0; purr_run_unsimulated >= dt && ticks < PURR_RUN_MAX_TICKS_PER_FRAME; ticks++) {
        purr_run_tick();
        purr_run_unsimulated -= dt;
    }
    if (purr_run_unsimulated >= dt) purr_run_unsimulated = 0.0;

    purr_draw_reset(&purr_run_draw);
    purr_frame(&purr_run_world, &purr_run_local, &purr_run_draw);
    purr_platform_draw(&purr_run_draw);

    if (purr_run_settings.stats) {
        char stats[96];
        snprintf(stats, sizeof stats, "tick %d   entities %u   %d fps", (int)purr_run_world.Time.tick,
                 (unsigned)purr_world_entity_count(&purr_run_world), purr_platform_fps());
        purr_platform_draw_overlay(stats);
    }
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
    purr_world_init(&purr_run_world, 1.0f / (float)purr_run_settings.tick_rate);
    purr_world_player_joined(&purr_run_world, purr_player_from_index(0)); // Local play: player 0 is here from the start
    purr_local_init(&purr_run_local);
    purr_platform_run(purr_run_frame, NULL);
}
