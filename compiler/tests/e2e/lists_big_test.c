#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "tide_test.h"

static tide_world world;
static tide_world snapshot;
static tide_world unpacked;

static Big big_in(const tide_world *w)
{
    for (uint32_t i = 0; i < w->entities.next_unused; i++) {
        if (tide_has_Big(w, tide_entity_in_slot(&w->entities, i))) return tide_get_Big(w, tide_entity_in_slot(&w->entities, i));
    }
    return (Big){0};
}

// Whether an entity has the Results, and they
static bool results_in(const tide_world *w, Results *r)
{
    for (uint32_t i = 0; i < w->entities.next_unused; i++) {
        if (!tide_has_Results(w, tide_entity_in_slot(&w->entities, i))) continue;
        *r = tide_get_Results(w, tide_entity_in_slot(&w->entities, i));
        return true;
    }
    return false;
}

static int32_t cell_id(const tide_world *w, const int32_t i)
{
    const Cell *c = tide_list_read(&w->heap, big_in(w).cells, i, sizeof(Cell));
    return c ? c->id : -1;
}

TIDE_TEST(lists_big_all_checks_pass)
{
    tide_world_init(&world, 1.0f);
    tide_world_tick(&world);
    tide_world_tick(&world);
    Results r;
    TIDE_REQUIRE(results_in(&world, &r));
    if (r.firstFailure) printf("    check %d failed\n", (int)r.firstFailure);
    TIDE_CHECK(r.checks == 22);
    TIDE_CHECK(r.passed == r.checks);
    TIDE_CHECK(tide_scratch_mark() == 0);
    tide_world_free(&world);
}

// Changing one element of a big list leaves a snapshot as it was, and copies
// only the chunk the element is in.
TIDE_TEST(lists_big_snapshots_share_chunks)
{
    tide_world_init(&world, 1.0f);
    tide_world_tick(&world); // The lists filled
    tide_world_tick(&world);
    tide_world_copy(&snapshot, &world);
    const uint64_t before = tide_world_hash(&snapshot);
    const int32_t id = cell_id(&world, 1500);
    tide_world_tick(&world); // One cell changes
    TIDE_CHECK(cell_id(&world, 1500) == id + 1);
    TIDE_CHECK(cell_id(&snapshot, 1500) == id);
    TIDE_CHECK(tide_world_hash(&snapshot) == before);
    TIDE_CHECK(tide_world_hash(&world) != before);

    // Of the heap's pages, the snapshot shares all but the changed chunk
    uint32_t different = 0;
    TIDE_REQUIRE(world.heap.pages == snapshot.heap.pages);
    for (uint32_t i = 0; i < world.heap.pages; i++) different += world.heap.page[i] != snapshot.heap.page[i];
    TIDE_CHECK(different == 1);
    tide_world_free(&world);
    tide_world_free(&snapshot);
}

// A world with lists in chunks packs and unpacks into the same world.
TIDE_TEST(lists_big_pack_and_unpack)
{
    tide_world_init(&world, 1.0f);
    tide_world_tick(&world);
    tide_world_tick(&world);
    const uint32_t size = tide_world_pack(&world, NULL, 0);
    uint8_t *bytes = malloc(size);
    TIDE_REQUIRE(bytes != NULL);
    tide_world_pack(&world, bytes, size);
    TIDE_REQUIRE(tide_world_unpack(&unpacked, bytes, size));
    TIDE_CHECK(tide_world_hash(&unpacked) == tide_world_hash(&world));
    TIDE_CHECK(cell_id(&unpacked, 1999) == 1999);
    tide_world_tick(&world);
    tide_world_tick(&unpacked);
    TIDE_CHECK(tide_world_hash(&unpacked) == tide_world_hash(&world));
    free(bytes);
    tide_world_free(&world);
    tide_world_free(&unpacked);
}
