// Sand, Tide against C (see bench/versus/versus.h): bench/sand/sand.tide, the
// game itself, with nobody painting.

#include <stdlib.h>

#include "sand.h"
#include "tide/page.h"
#include "versus.h"

extern const versus_way sand_c, sand_c_table;

static versus_flag flags[] = {
    {"size", "the grid's width and height, in cells", 1024, 128, 0},
};

static void *make(const versus_flag *f)
{
    tide_world *w = tide_alloc_zeroed(1, sizeof *w);
    tide_world_init(w, VERSUS_DT);
    w->Field.size = (int32_t)f[0].value;
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
    for (int32_t y = 0; y < w->Field.size; y++) {
        for (int32_t x = 0; x < w->Field.size; x++) {
            const Material *m = tide_grid_read(&w->heap, w->Field.cells, x, y, 0, &tide_shape_Grid2_Material);
            const int32_t item[3] = {x, y, m ? *m : Material_Empty};
            sum += versus_item(item, sizeof item);
        }
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
    const versus_way ways[] = {{"Tide", make, tick, digest, destroy}, sand_c, sand_c_table};
    const versus_desc desc = {
        .name = "versus_sand",
        .about = "falling sand and water, in 2x2 blocks every cell of a grid is in",
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
