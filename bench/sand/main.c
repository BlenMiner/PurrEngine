// Sand's bots, and its run (see bench/bench.h).

#include <stdio.h>

#include "bench.h"
#include "sand.h"

static bench_flag flags[] = {
    {"size", "the grid's width and height, in cells", 256, 96, 0},
};

static void make_world(const bench_flag *f, void *world, const float dt)
{
    tide_world *w = world;
    tide_world_init(w, dt);
    w->Field.size = (int32_t)f[0].value;
}

// From 0 up to `span` and back, over `period` ticks.
static int32_t wave(const uint32_t tick, const uint32_t period, const int32_t span)
{
    const uint32_t half = period / 2u;
    const uint32_t at = tick % period;
    const uint32_t up = at < half ? at : period - at;
    return (int32_t)((int64_t)up * span / (int64_t)half);
}

// Each bot's brush sweeps the grid on a path of its own, changes material
// every two seconds and paints three quarters of the time: an input that
// changes every tick, so every other machine's guess at it is wrong.
static void sample(const bench_flag *f, const uint32_t bot, const uint32_t tick, void *input)
{
    static const Material materials[] = {Material_Sand, Material_Water, Material_Sand, Material_Empty, Material_Wall};
    Brush *b = input;
    const int32_t last = (int32_t)f[0].value - 1;
    b->x = wave(tick + bot * 97u, 400u + bot * 46u, last);
    b->y = wave(tick + bot * 61u, 520u + bot * 34u, last);
    b->material = materials[(tick / 120u + bot) % 5u];
    b->down = (tick / 45u + bot) % 4u != 0u;
}

static void describe(const void *world)
{
    const tide_world *w = world;
    int32_t counts[4] = {0};
    for (int32_t y = 0; y < w->Field.size; y++) {
        for (int32_t x = 0; x < w->Field.size; x++) {
            const Material *m = tide_grid_read(&w->heap, w->Field.cells, x, y, 0, &tide_shape_Grid2_Material);
            counts[m ? *m & 3 : Material_Empty]++;
        }
    }
    printf("  cells: %d sand, %d water, %d wall, %d empty\n", (int)counts[Material_Sand], (int)counts[Material_Water],
           (int)counts[Material_Wall], (int)counts[Material_Empty]);
}

int main(int argc, char **argv)
{
    const bench_desc desc = {
        .name = "sand",
        .game = &tide_game_api,
        .full = {.players = 8, .join_every = 0.25, .seconds = 10.0, .latency = 0.05, .jitter = 0.01, .loss = 0.01},
        .quick = {.players = 3, .join_every = 0.25, .seconds = 2.0, .latency = 0.05, .jitter = 0.01, .loss = 0.01},
        .flags = flags,
        .flag_count = sizeof flags / sizeof flags[0],
        .make_world = make_world,
        .sample = sample,
        .describe = describe,
    };
    return bench_main(&desc, argc, argv);
}
