// Churn, Tide against C (see bench/versus/versus.h).

#include <stdlib.h>

#include "churn.h"
#include "tide/page.h"
#include "versus.h"

extern const versus_way churn_c;

static versus_flag flags[] = {
    {"rate", "bullets fired a tick, each living 20 to 179 ticks", 2000, 100, 0},
};

static void *make(const versus_flag *f)
{
    tide_world *w = tide_alloc_zeroed(1, sizeof *w);
    tide_world_init(w, VERSUS_DT);
    w->Gun.rate = (int32_t)f[0].value;
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
    for (uint32_t row = 0; row < w->arch0_Bullet.count; row++) {
        const Bullet b = TIDE_AT(w, arch0_Bullet, Bullet, row);
        sum += versus_item(&b, sizeof b);
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
    const versus_way ways[] = {{"Tide", make, tick, digest, destroy}, churn_c};
    const versus_desc desc = {
        .name = "versus_churn",
        .about = "bullets are fired, fly and are destroyed, each at its own time",
        .flags = flags,
        .flag_count = sizeof flags / sizeof flags[0],
        .warmup = 200, // Past the longest life: as many go as come
        .ticks = 300,
        .quick_warmup = 2,
        .quick_ticks = 60,
        .ways = ways,
        .way_count = sizeof ways / sizeof ways[0],
    };
    return versus_main(&desc, argc, argv);
}
