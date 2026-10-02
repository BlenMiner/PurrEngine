// Particles, Tide against C (see bench/versus/versus.h).

#include <stdlib.h>

#include "particles.h"
#include "tide/page.h"
#include "versus.h"

extern const versus_way particles_c;

static versus_flag flags[] = {
    {"count", "particles", 1000000, 10000, 0},
};

static void *make(const versus_flag *f)
{
    tide_world *w = tide_alloc_zeroed(1, sizeof *w);
    tide_world_init(w, VERSUS_DT);
    w->Swarm.count = (int32_t)f[0].value;
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
    for (uint32_t row = 0; row < w->arch0_Particle.count; row++) {
        const Particle p = TIDE_AT(w, arch0_Particle, Particle, row);
        sum += versus_item(&p, sizeof p);
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
    const versus_way ways[] = {{"Tide", make, tick, digest, destroy}, particles_c};
    const versus_desc desc = {
        .name = "versus_particles",
        .about = "particles fall and bounce off the floor and the walls",
        .flags = flags,
        .flag_count = sizeof flags / sizeof flags[0],
        .warmup = 10,
        .ticks = 200,
        .quick_warmup = 2,
        .quick_ticks = 20,
        .ways = ways,
        .way_count = sizeof ways / sizeof ways[0],
    };
    return versus_main(&desc, argc, argv);
}
