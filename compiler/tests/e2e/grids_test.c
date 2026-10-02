#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "tide_test.h"

extern int32_t grids_visits; // grids_c.c: cells Glow ran for

static tide_world world;
static tide_world other;

static int32_t cell(const tide_world *w, const int32_t x, const int32_t y)
{
    const int32_t *c = tide_grid_read(&w->heap, w->Field.cells, x, y, 0, &tide_shape_Grid2_int);
    return c ? *c : 0;
}

// How high the column's ones are, all together: less each tick one falls.
static int32_t height(const tide_world *w)
{
    int32_t sum = 0;
    for (int32_t y = 0; y < 100; y++) sum += y * cell(w, 10, y);
    return sum;
}

static void ticks(tide_world *w, const int n)
{
    for (int i = 0; i < n; i++) tide_world_tick(w);
}

TIDE_TEST(grids_all_checks_pass)
{
    tide_world_init(&world, 1.0f);
    ticks(&world, 3);
    const Tally *t = &world.Results.tally;
    if (t->firstFailure) printf("    check %d failed\n", (int)t->firstFailure);
    TIDE_CHECK(t->checks == 14);
    TIDE_CHECK(t->passed == t->checks);
    TIDE_CHECK(tide_scratch_mark() == 0);
    tide_world_free(&world);
}

// A parallel loop's blocks move cells across chunks' edges: the column falls
// through the chunk below and lands on the floor, whole.
TIDE_TEST(grids_parallel_loop_moves_across_chunks)
{
    tide_world_init(&world, 1.0f);
    ticks(&world, 90);
    int ones = 0;
    for (int32_t y = 0; y < 100; y++) {
        for (int32_t x = 0; x < 200; x++) ones += cell(&world, x, y) == 1;
    }
    TIDE_CHECK(ones == 10);
    for (int32_t y = 0; y < 10; y++) TIDE_CHECK(cell(&world, 10, y) == 1);
    TIDE_CHECK(cell(&world, 10, 10) == 0);
    tide_world_free(&world);
}

// A parallel loop goes through every cell within the grid's size, once a tick:
// none before the first tick gives the lamps a size, and 5 by 5 after.
TIDE_TEST(grids_parallel_loop_steps)
{
    grids_visits = 0;
    tide_world_init(&world, 1.0f);
    ticks(&world, 12);
    TIDE_CHECK(grids_visits == 11 * 25);
    const int32_t *lamp = tide_grid_read(&world.heap, world.Field.lamps, 4, 4, 0, &tide_shape_Grid2_int);
    TIDE_CHECK(lamp && *lamp == 3);
    tide_world_free(&world);
}

TIDE_TEST(grids_3d)
{
    tide_world_init(&world, 1.0f);
    ticks(&world, 3);
    const Voxel *v = tide_grid_read(&world.heap, world.Field.blocks, 100, 200, -300, &tide_shape_Grid3_Voxel);
    TIDE_CHECK(v && *v == Voxel_Stone);
    v = tide_grid_read(&world.heap, world.Field.blocks, 1, -2, 3, &tide_shape_Grid3_Voxel);
    TIDE_CHECK(v && *v == Voxel_Stone);
    TIDE_CHECK(!tide_grid_read(&world.heap, world.Field.blocks, 5000, 5000, 5000, &tide_shape_Grid3_Voxel));
    tide_world_free(&world);
}

// A snapshot keeps its cells as they were, and a world packed and unpacked
// goes on the same.
TIDE_TEST(grids_snapshot_and_pack)
{
    tide_world_init(&world, 1.0f);
    ticks(&world, 3);
    tide_world_copy(&other, &world);
    const int32_t before = height(&other);
    ticks(&world, 5);
    TIDE_CHECK(height(&other) == before);
    TIDE_CHECK(height(&world) < before);
    tide_world_free(&other);

    const uint32_t size = tide_world_pack(&world, NULL, 0);
    uint8_t *bytes = malloc(size);
    TIDE_REQUIRE(bytes != NULL);
    tide_world_pack(&world, bytes, size);
    TIDE_REQUIRE(tide_world_unpack(&other, bytes, size));
    TIDE_CHECK(tide_world_hash(&other) == tide_world_hash(&world));
    ticks(&world, 70);
    ticks(&other, 70);
    TIDE_CHECK(tide_world_hash(&other) == tide_world_hash(&world));
    TIDE_CHECK(cell(&other, 10, 0) == 1);
    free(bytes);
    tide_world_free(&world);
    tide_world_free(&other);
}
