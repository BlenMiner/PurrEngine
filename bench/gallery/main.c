// Gallery's bots, and its run (see bench/bench.h).

#include <stdio.h>

#include "bench.h"
#include "gallery.h"

static bench_flag flags[] = {
    {"size", "each canvas's width and height, in pixels", 512, 128, 0},
    {"archived", "canvases already painted when the match starts", 4, 2, 0},
};

static void make_world(const bench_flag *f, void *world, const float dt)
{
    tide_world *w = world;
    tide_world_init(w, dt);
    w->Gallery.size = (int32_t)f[0].value;
    w->Gallery.archived = (int32_t)f[1].value;
}

// From 0 up to `span` and back, over `period` ticks.
static int32_t wave(const uint32_t tick, const uint32_t period, const int32_t span)
{
    const uint32_t half = period / 2u;
    const uint32_t at = tick % period;
    const uint32_t up = at < half ? at : period - at;
    return (int32_t)((int64_t)up * span / (int64_t)half);
}

// Each bot's pen moves over its canvas on a path of its own, changes color
// every half second and is down four fifths of the time.
static void sample(const bench_flag *f, const uint32_t bot, const uint32_t tick, void *input)
{
    Pen *p = input;
    const int32_t last = (int32_t)f[0].value - 1;
    p->x = wave(tick + bot * 97u, 300u + bot * 46u, last);
    p->y = wave(tick + bot * 61u, 380u + bot * 34u, last);
    p->color = (int32_t)(((tick / 30u + bot) * 2654435761u) | 0xFFu);
    p->down = (tick / 40u + bot) % 5u != 0u;
}

static void describe(const void *world)
{
    const tide_world *w = world;
    const uint64_t pixels = (uint64_t)w->Gallery.made * (uint64_t)w->Gallery.size * (uint64_t)w->Gallery.size;
    printf("  %d canvases, %d of them painted before the match: %.2f MB of pixels\n", (int)w->Gallery.made,
           (int)w->Gallery.archived, (double)pixels * 4.0 / 1e6);
}

int main(int argc, char **argv)
{
    const bench_desc desc = {
        .name = "gallery",
        .game = &tide_game_api,
        .full = {.players = 8, .join_every = 3.0, .seconds = 5.0, .latency = 0.05, .jitter = 0.01, .loss = 0.01},
        .quick = {.players = 3, .join_every = 1.0, .seconds = 2.0, .latency = 0.05, .jitter = 0.01, .loss = 0.01},
        .flags = flags,
        .flag_count = sizeof flags / sizeof flags[0],
        .make_world = make_world,
        .sample = sample,
        .describe = describe,
    };
    return bench_main(&desc, argc, argv);
}
