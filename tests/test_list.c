#include <string.h>

#include "tide/list.h"
#include "tide/text.h"
#include "tide_test.h"

// A stand-in for a generated world: a list that's part of it, and its heap.
typedef struct list_world {
    tide_list scores;
    tide_list other;
    tide_heap heap;
} list_world;

static list_world world;

static void start(void)
{
    memset(&world, 0, sizeof world);
    tide_text_use(&world.heap, &world, sizeof world, NULL, NULL, 0);
}

static void add(tide_list *l, const int32_t v)
{
    int32_t *slot = tide_list_add(l, sizeof v);
    if (slot) *slot = v;
}

static int32_t at(const tide_list l, const int32_t i)
{
    const int32_t *p = tide_list_at(l, i, sizeof(int32_t));
    return p ? *p : -999;
}

TIDE_TEST(list_world_lists_grow_in_their_heap)
{
    start();
    for (int32_t i = 0; i < 100; i++) add(&world.scores, i);
    TIDE_CHECK(tide_list_count(world.scores) == 100);
    TIDE_CHECK(at(world.scores, 0) == 0 && at(world.scores, 99) == 99);
    TIDE_CHECK(at(world.scores, 100) == -999 && at(world.scores, -1) == -999);
    TIDE_CHECK(world.scores.at >> 30 == TIDE_IN_MATCH);

    tide_list_remove_at(&world.scores, 10, sizeof(int32_t));
    TIDE_CHECK(tide_list_count(world.scores) == 99 && at(world.scores, 10) == 11);
    tide_list_remove_at(&world.scores, 500, sizeof(int32_t)); // Past the end: nothing
    TIDE_CHECK(tide_list_count(world.scores) == 99);
    *(int32_t *)tide_list_insert(&world.scores, -5, sizeof(int32_t)) = 42; // Clamped to the start
    TIDE_CHECK(at(world.scores, 0) == 42 && at(world.scores, 1) == 0);

    tide_heap_flush(&world.heap); // The blocks it outgrew
    const uint32_t used = world.heap.used;
    tide_list_clear(&world.scores, sizeof(int32_t));
    for (int32_t i = 0; i < 100; i++) add(&world.scores, i);
    TIDE_CHECK(world.heap.used == used); // Its block had room
}

TIDE_TEST(list_copies_are_their_own)
{
    start();
    const uint32_t mark = tide_scratch_mark();
    add(&world.scores, 1);
    add(&world.scores, 2);
    tide_list copy = tide_list_copy(world.scores, sizeof(int32_t)); // A local's copy, in the scratch area
    add(&copy, 3);
    *(int32_t *)tide_list_at(copy, 0, sizeof(int32_t)) = 9;
    TIDE_CHECK(tide_list_count(world.scores) == 2 && at(world.scores, 0) == 1);
    TIDE_CHECK(tide_list_count(copy) == 3 && at(copy, 0) == 9 && copy.at >> 30 == TIDE_IN_SCRATCH);

    tide_list_set(&world.other, copy, sizeof(int32_t)); // Into the world: its own copy
    TIDE_CHECK(world.other.at >> 30 == TIDE_IN_MATCH);
    tide_scratch_reset(mark);
    TIDE_CHECK(tide_list_count(world.other) == 3 && at(world.other, 2) == 3);

    const int32_t three[3] = {7, 8, 9};
    world.other = tide_list_from(three, 3, sizeof(int32_t)); // A raw copy into the world, as into the queue
    tide_list_own(&world.other, sizeof(int32_t));
    tide_scratch_reset(mark);
    TIDE_CHECK(world.other.at >> 30 == TIDE_IN_MATCH && at(world.other, 1) == 8);

    tide_list_release(&world.other);
    TIDE_CHECK(world.other.at == 0 && world.heap.pending != 0);
    tide_heap_flush(&world.heap);
}
