#include <string.h>

#include "purr/list.h"
#include "purr/text.h"
#include "purr_test.h"

// A stand-in for a generated world: a list that's part of it, and its heap.
typedef struct list_world {
    purr_list scores;
    purr_list other;
    purr_heap heap;
} list_world;

static list_world world;

static void start(void)
{
    memset(&world, 0, sizeof world);
    purr_text_use(&world.heap, &world, sizeof world, NULL, NULL, 0);
}

static void add(purr_list *l, const int32_t v)
{
    int32_t *slot = purr_list_add(l, sizeof v);
    if (slot) *slot = v;
}

static int32_t at(const purr_list l, const int32_t i)
{
    const int32_t *p = purr_list_at(l, i, sizeof(int32_t));
    return p ? *p : -999;
}

PURR_TEST(list_world_lists_grow_in_their_heap)
{
    start();
    for (int32_t i = 0; i < 100; i++) add(&world.scores, i);
    PURR_CHECK(purr_list_count(world.scores) == 100);
    PURR_CHECK(at(world.scores, 0) == 0 && at(world.scores, 99) == 99);
    PURR_CHECK(at(world.scores, 100) == -999 && at(world.scores, -1) == -999);
    PURR_CHECK(world.scores.at >> 30 == PURR_IN_MATCH);

    purr_list_remove_at(&world.scores, 10, sizeof(int32_t));
    PURR_CHECK(purr_list_count(world.scores) == 99 && at(world.scores, 10) == 11);
    purr_list_remove_at(&world.scores, 500, sizeof(int32_t)); // Past the end: nothing
    PURR_CHECK(purr_list_count(world.scores) == 99);
    *(int32_t *)purr_list_insert(&world.scores, -5, sizeof(int32_t)) = 42; // Clamped to the start
    PURR_CHECK(at(world.scores, 0) == 42 && at(world.scores, 1) == 0);

    purr_heap_flush(&world.heap); // The blocks it outgrew
    const uint32_t used = world.heap.used;
    purr_list_clear(&world.scores, sizeof(int32_t));
    for (int32_t i = 0; i < 100; i++) add(&world.scores, i);
    PURR_CHECK(world.heap.used == used); // Its block had room
}

PURR_TEST(list_copies_are_their_own)
{
    start();
    const uint32_t mark = purr_scratch_mark();
    add(&world.scores, 1);
    add(&world.scores, 2);
    purr_list copy = purr_list_copy(world.scores, sizeof(int32_t)); // A local's copy, in the scratch area
    add(&copy, 3);
    *(int32_t *)purr_list_at(copy, 0, sizeof(int32_t)) = 9;
    PURR_CHECK(purr_list_count(world.scores) == 2 && at(world.scores, 0) == 1);
    PURR_CHECK(purr_list_count(copy) == 3 && at(copy, 0) == 9 && copy.at >> 30 == PURR_IN_SCRATCH);

    purr_list_set(&world.other, copy, sizeof(int32_t)); // Into the world: its own copy
    PURR_CHECK(world.other.at >> 30 == PURR_IN_MATCH);
    purr_scratch_reset(mark);
    PURR_CHECK(purr_list_count(world.other) == 3 && at(world.other, 2) == 3);

    const int32_t three[3] = {7, 8, 9};
    world.other = purr_list_from(three, 3, sizeof(int32_t)); // A raw copy into the world, as into the queue
    purr_list_own(&world.other, sizeof(int32_t));
    purr_scratch_reset(mark);
    PURR_CHECK(world.other.at >> 30 == PURR_IN_MATCH && at(world.other, 1) == 8);

    purr_list_release(&world.other);
    PURR_CHECK(world.other.at == 0 && world.heap.pending != 0);
    purr_heap_flush(&world.heap);
}
