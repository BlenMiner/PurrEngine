// Events, Tide against C (see bench/versus/versus.h).

#include <stdlib.h>

#include "events.h"
#include "tide/page.h"
#include "versus.h"

extern const versus_way events_c;

static versus_flag flags[] = {
    {"targets", "entities that turrets hit", 50000, 500, 0},
    {"turrets", "entities that each hit a target every tick", 200000, 2000, 0},
};

static void *make(const versus_flag *f)
{
    tide_world *w = tide_alloc_zeroed(1, sizeof *w);
    tide_world_init(w, VERSUS_DT);
    w->Range.targets = (int32_t)f[0].value;
    w->Range.turrets = (int32_t)f[1].value;
    return w;
}

static void tick(void *world, const tide_jobs *jobs)
{
    tide_world_tick_on(world, jobs);
}

// The targets, in the order they were spawned: by their number in C.
static uint64_t digest(const void *world)
{
    const tide_world *w = world;
    uint64_t sum = 0;
    for (uint32_t row = 0; row < w->arch0_Health.count; row++) {
        const int32_t item[2] = {(int32_t)row, TIDE_AT(w, arch0_Health, Health, row).value};
        sum += versus_item(item, sizeof item);
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
    const versus_way ways[] = {{"Tide", make, tick, digest, destroy}, events_c};
    const versus_desc desc = {
        .name = "versus_events",
        .about = "turrets send their targets a Hit each tick, which a handler takes",
        .flags = flags,
        .flag_count = sizeof flags / sizeof flags[0],
        .warmup = 10,
        .ticks = 200,
        .quick_warmup = 2,
        .quick_ticks = 30,
        .ways = ways,
        .way_count = sizeof ways / sizeof ways[0],
    };
    return versus_main(&desc, argc, argv);
}
