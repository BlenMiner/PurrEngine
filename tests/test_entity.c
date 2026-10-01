#include <stdlib.h>

#include "tide/entity.h"
#include "tide_test.h"

// The table is large, so tests allocate it zeroed on the heap.
static tide_entities *new_table(void)
{
    return calloc(1, sizeof(tide_entities));
}

TIDE_TEST(entity_null_is_never_alive)
{
    tide_entities *t = new_table();
    const tide_entity null = {0};
    TIDE_CHECK(tide_entity_is_null(null));
    TIDE_CHECK(!tide_entity_alive(t, null));
    free(t);
}

TIDE_TEST(entity_create_is_alive_and_pending)
{
    tide_entities *t = new_table();
    const tide_entity e = tide_entity_create(t);
    TIDE_CHECK(!tide_entity_is_null(e));
    TIDE_CHECK(tide_entity_alive(t, e));
    TIDE_CHECK(tide_entity_location(t, e).archetype == TIDE_ARCHETYPE_NONE);
    free(t);
}

TIDE_TEST(entity_location_round_trips)
{
    tide_entities *t = new_table();
    const tide_entity e = tide_entity_create(t);
    tide_entity_set_location(t, e, (tide_location){3, 42});
    const tide_location loc = tide_entity_location(t, e);
    TIDE_CHECK(loc.archetype == 3);
    TIDE_CHECK(loc.row == 42);
    free(t);
}

TIDE_TEST(entity_destroy_invalidates_handle)
{
    tide_entities *t = new_table();
    const tide_entity e = tide_entity_create(t);
    TIDE_CHECK(tide_entity_destroy(t, e));
    TIDE_CHECK(!tide_entity_alive(t, e));
    TIDE_CHECK(!tide_entity_destroy(t, e));
    TIDE_CHECK(tide_entity_location(t, e).archetype == TIDE_ARCHETYPE_NONE);
    free(t);
}

TIDE_TEST(entity_reuse_bumps_generation)
{
    tide_entities *t = new_table();
    const tide_entity a = tide_entity_create(t);
    tide_entity_destroy(t, a);
    const tide_entity b = tide_entity_create(t);
    TIDE_CHECK(b.index == a.index);
    TIDE_CHECK(b.generation != a.generation);
    TIDE_CHECK(tide_entity_alive(t, b));
    TIDE_CHECK(!tide_entity_alive(t, a));
    free(t);
}

TIDE_TEST(entity_reuse_order_is_lifo)
{
    tide_entities *t = new_table();
    const tide_entity a = tide_entity_create(t);
    const tide_entity b = tide_entity_create(t);
    tide_entity_destroy(t, a);
    tide_entity_destroy(t, b);
    TIDE_CHECK(tide_entity_create(t).index == b.index);
    TIDE_CHECK(tide_entity_create(t).index == a.index);
    free(t);
}

TIDE_TEST(entity_create_fails_when_full)
{
    tide_entities *t = new_table();
    for (uint32_t i = 0; i < TIDE_MAX_ENTITIES; i++) {
        TIDE_REQUIRE(!tide_entity_is_null(tide_entity_create(t)));
    }
    TIDE_CHECK(tide_entity_is_null(tide_entity_create(t)));
    free(t);
}
