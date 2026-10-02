// N-body, Tide against C (see bench/versus/versus.h).

#include <stdlib.h>

#include "nbody.h"
#include "tide/page.h"
#include "versus.h"

extern const versus_way nbody_c, nbody_c_simd;

static versus_flag flags[] = {
    {"count", "bodies, each pulled by every other", 2000, 100, 0},
};

static void *make(const versus_flag *f)
{
    tide_world *w = tide_alloc_zeroed(1, sizeof *w);
    tide_world_init(w, VERSUS_DT);
    w->Space.count = (int32_t)f[0].value;
    return w;
}

static void tick(void *world, const tide_jobs *jobs)
{
    tide_world_tick_on(world, jobs);
}

static uint64_t digest(const void *world)
{
    const tide_world *w = world;
    uint64_t sum = 0;
    for (int32_t i = 0; i < tide_list_read_count(&w->heap, w->Space.bodies); i++) {
        sum += versus_item(tide_list_read(&w->heap, w->Space.bodies, i, sizeof(Body)), sizeof(Body));
    }
    return sum;
}

static void destroy(void *world)
{
    tide_world_free(world);
    free(world);
}

int main(int argc, char **argv)
{
    const versus_way ways[] = {{"Tide", make, tick, digest, destroy}, nbody_c, nbody_c_simd};
    const versus_desc desc = {
        .name = "versus_nbody",
        .about = "every body pulls on every other, through a list in a singleton",
        .flags = flags,
        .flag_count = sizeof flags / sizeof flags[0],
        .warmup = 2,
        .ticks = 30,
        .quick_warmup = 2,
        .quick_ticks = 10,
        .ways = ways,
        .way_count = sizeof ways / sizeof ways[0],
    };
    return versus_main(&desc, argc, argv);
}
