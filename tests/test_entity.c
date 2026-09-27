#include <stdlib.h>

#include "purr/entity.h"
#include "purr_test.h"

// The table is large, so tests allocate it zeroed on the heap.
static purr_entities *new_table(void)
{
    return calloc(1, sizeof(purr_entities));
}

PURR_TEST(entity_null_is_never_alive)
{
    purr_entities *t = new_table();
    const purr_entity null = {0};
    PURR_CHECK(purr_entity_is_null(null));
    PURR_CHECK(!purr_entity_alive(t, null));
    free(t);
}

PURR_TEST(entity_create_is_alive_and_pending)
{
    purr_entities *t = new_table();
    const purr_entity e = purr_entity_create(t);
    PURR_CHECK(!purr_entity_is_null(e));
    PURR_CHECK(purr_entity_alive(t, e));
    PURR_CHECK(purr_entity_location(t, e).archetype == PURR_ARCHETYPE_NONE);
    free(t);
}

PURR_TEST(entity_location_round_trips)
{
    purr_entities *t = new_table();
    const purr_entity e = purr_entity_create(t);
    purr_entity_set_location(t, e, (purr_location){3, 42});
    const purr_location loc = purr_entity_location(t, e);
    PURR_CHECK(loc.archetype == 3);
    PURR_CHECK(loc.row == 42);
    free(t);
}

PURR_TEST(entity_destroy_invalidates_handle)
{
    purr_entities *t = new_table();
    const purr_entity e = purr_entity_create(t);
    PURR_CHECK(purr_entity_destroy(t, e));
    PURR_CHECK(!purr_entity_alive(t, e));
    PURR_CHECK(!purr_entity_destroy(t, e));
    PURR_CHECK(purr_entity_location(t, e).archetype == PURR_ARCHETYPE_NONE);
    free(t);
}

PURR_TEST(entity_reuse_bumps_generation)
{
    purr_entities *t = new_table();
    const purr_entity a = purr_entity_create(t);
    purr_entity_destroy(t, a);
    const purr_entity b = purr_entity_create(t);
    PURR_CHECK(b.index == a.index);
    PURR_CHECK(b.generation != a.generation);
    PURR_CHECK(purr_entity_alive(t, b));
    PURR_CHECK(!purr_entity_alive(t, a));
    free(t);
}

PURR_TEST(entity_reuse_order_is_lifo)
{
    purr_entities *t = new_table();
    const purr_entity a = purr_entity_create(t);
    const purr_entity b = purr_entity_create(t);
    purr_entity_destroy(t, a);
    purr_entity_destroy(t, b);
    PURR_CHECK(purr_entity_create(t).index == b.index);
    PURR_CHECK(purr_entity_create(t).index == a.index);
    free(t);
}

PURR_TEST(entity_create_fails_when_full)
{
    purr_entities *t = new_table();
    for (uint32_t i = 0; i < PURR_MAX_ENTITIES; i++) {
        PURR_REQUIRE(!purr_entity_is_null(purr_entity_create(t)));
    }
    PURR_CHECK(purr_entity_is_null(purr_entity_create(t)));
    free(t);
}
