#include <stdio.h>

#include "game.h"
#include "tide_test.h"

static tide_world world;

static int32_t cell(const tide_grid g, const int32_t x, const int32_t y)
{
    const int32_t *c = tide_grid_read(&world.heap, g, x, y, 0, &tide_shape_Grid2_int);
    return c ? *c : 0;
}

static int32_t count(const tide_grid g, const int32_t value)
{
    int32_t n = 0;
    for (int32_t y = 0; y < world.Heat.size; y++) {
        for (int32_t x = 0; x < world.Heat.size; x++) n += cell(g, x, y) == value;
    }
    return n;
}

// Parallel loops give what the same rules give by hand: every cell from the
// grid as it was, and blocks that never overlap.
TIDE_TEST(parallel_loops_match_their_rules_by_hand)
{
    tide_world_init(&world, 1.0f);
    tide_world_tick(&world);
    TIDE_CHECK(world.Heat.mismatches == 0);
    TIDE_CHECK(cell(world.Heat.doubled, 3, 4) == 14); // (3 + 4) * 2: its own cell, at once
    TIDE_CHECK(cell(world.Heat.totals, 5, 2) == 6);   // In order: each sees the one before it changed
    int32_t sum = 0;
    for (int32_t y = 0; y < world.Heat.size; y++) {
        for (int32_t x = 0; x < world.Heat.size; x++) sum += cell(world.Heat.doubled, x, y);
    }
    TIDE_CHECK(world.Heat.sum == sum); // In order too: it changes a total outside its cell
    const int32_t grains = count(world.Heat.grains, 1);
    const int32_t warm = world.Heat.size * world.Heat.size - count(world.Heat.cells, 0);
    for (int i = 0; i < 60; i++) tide_world_tick(&world);
    if (world.Heat.mismatches) printf("    %d cells differ\n", (int)world.Heat.mismatches);
    TIDE_CHECK(world.Heat.mismatches == 0);
    TIDE_CHECK(count(world.Heat.grains, 1) == grains); // None lost, none made
    TIDE_CHECK(cell(world.Heat.grains, 0, 0) == 1 || cell(world.Heat.grains, 1, 0) == 1); // They reached the floor
    TIDE_CHECK(world.Heat.size * world.Heat.size - count(world.Heat.cells, 0) > warm); // The heat spread
    TIDE_CHECK(tide_scratch_mark() == 0);
    tide_world_free(&world);
}

static int32_t value(const tide_list l, const int32_t i)
{
    return *(const int32_t *)tide_list_read(&world.heap, l, i, sizeof(int32_t));
}

// Parallel loops over lists give what the same rules give by hand: each element
// from the list as it was, and blocks of two that never overlap.
TIDE_TEST(parallel_list_loops_match_their_rules_by_hand)
{
    tide_world_init(&world, 1.0f);
    tide_world_tick(&world);
    const int32_t first = value(world.Wave.values, 1); // Smoothed once already
    for (int i = 0; i < 40; i++) tide_world_tick(&world);
    if (world.Wave.mismatches) printf("    %d elements differ\n", (int)world.Wave.mismatches);
    TIDE_CHECK(world.Wave.mismatches == 0);
    TIDE_CHECK(value(world.Wave.values, 1) != first); // They went on changing
    const tide_int2 *a = tide_list_read(&world.heap, world.Wave.pairs, 0, sizeof(tide_int2));
    const tide_int2 *b = tide_list_read(&world.heap, world.Wave.pairs, 1, sizeof(tide_int2));
    TIDE_CHECK(a->x <= b->x); // In order, the pair at the start
    TIDE_CHECK(tide_scratch_mark() == 0);
    tide_world_free(&world);
}
