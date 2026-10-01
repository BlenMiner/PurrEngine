#include <stdlib.h>

#include "tide/entity.h"
#include "tide_test.h"

TIDE_TEST(entity_null_is_never_alive)
{
    tide_entities t = {0};
    const tide_entity null = {0};
    TIDE_CHECK(tide_entity_is_null(null));
    TIDE_CHECK(!tide_entity_alive(&t, null));
    tide_entities_free(&t);
}

TIDE_TEST(entity_create_is_alive_and_pending)
{
    tide_entities t = {0};
    const tide_entity e = tide_entity_create(&t);
    TIDE_CHECK(!tide_entity_is_null(e));
    TIDE_CHECK(tide_entity_alive(&t, e));
    TIDE_CHECK(tide_entity_location(&t, e).archetype == TIDE_ARCHETYPE_NONE);
    tide_entities_free(&t);
}

TIDE_TEST(entity_location_round_trips)
{
    tide_entities t = {0};
    const tide_entity e = tide_entity_create(&t);
    tide_entity_set_location(&t, e, (tide_location){3, 42});
    const tide_location loc = tide_entity_location(&t, e);
    TIDE_CHECK(loc.archetype == 3);
    TIDE_CHECK(loc.row == 42);
    tide_entities_free(&t);
}

TIDE_TEST(entity_destroy_invalidates_handle)
{
    tide_entities t = {0};
    const tide_entity e = tide_entity_create(&t);
    TIDE_CHECK(tide_entity_destroy(&t, e));
    TIDE_CHECK(!tide_entity_alive(&t, e));
    TIDE_CHECK(!tide_entity_destroy(&t, e));
    TIDE_CHECK(tide_entity_location(&t, e).archetype == TIDE_ARCHETYPE_NONE);
    tide_entities_free(&t);
}

TIDE_TEST(entity_reuse_bumps_generation)
{
    tide_entities t = {0};
    const tide_entity a = tide_entity_create(&t);
    tide_entity_destroy(&t, a);
    const tide_entity b = tide_entity_create(&t);
    TIDE_CHECK(b.index == a.index);
    TIDE_CHECK(b.generation != a.generation);
    TIDE_CHECK(tide_entity_alive(&t, b));
    TIDE_CHECK(!tide_entity_alive(&t, a));
    tide_entities_free(&t);
}

TIDE_TEST(entity_reuse_order_is_lifo)
{
    tide_entities t = {0};
    const tide_entity a = tide_entity_create(&t);
    const tide_entity b = tide_entity_create(&t);
    tide_entity_destroy(&t, a);
    tide_entity_destroy(&t, b);
    TIDE_CHECK(tide_entity_create(&t).index == b.index);
    TIDE_CHECK(tide_entity_create(&t).index == a.index);
    tide_entities_free(&t);
}

// It grows as entities are made, page after page, with no limit but memory.
TIDE_TEST(entity_table_grows)
{
    tide_entities t = {0};
    const uint32_t n = 5u << TIDE_ENTITY_PAGE_SHIFT;
    for (uint32_t i = 0; i < n; i++) {
        const tide_entity e = tide_entity_create(&t);
        TIDE_REQUIRE(e.index == i);
        tide_entity_set_location(&t, e, (tide_location){1, i});
    }
    TIDE_CHECK(t.pages == 5);
    TIDE_CHECK(tide_entity_location(&t, (tide_entity){n - 1u, 1}).row == n - 1u);
    tide_entities_free(&t);
    TIDE_CHECK(t.pages == 0 && t.next_unused == 0);
}

// A snapshot shares the table's pages, and changing either leaves the other
// as it was.
TIDE_TEST(entity_snapshots_share_until_changed)
{
    tide_entities t = {0};
    tide_entities snapshot = {0};
    const tide_entity a = tide_entity_create(&t);
    const tide_entity b = tide_entity_create(&t);
    tide_entities_copy(&snapshot, &t);
    TIDE_CHECK(snapshot.page[0] == t.page[0]);
    TIDE_CHECK(tide_entities_hash(1, &snapshot) == tide_entities_hash(1, &t));

    tide_entity_destroy(&t, a);
    tide_entity_set_location(&t, b, (tide_location){2, 7});
    TIDE_CHECK(snapshot.page[0] != t.page[0]);
    TIDE_CHECK(tide_entity_alive(&snapshot, a) && !tide_entity_alive(&t, a));
    TIDE_CHECK(tide_entity_location(&snapshot, b).archetype == TIDE_ARCHETYPE_NONE);
    TIDE_CHECK(tide_entities_hash(1, &snapshot) != tide_entities_hash(1, &t));

    // Back to the snapshot: the same entities, and the same hash
    tide_entities_copy(&t, &snapshot);
    TIDE_CHECK(tide_entity_alive(&t, a));
    TIDE_CHECK(tide_entities_hash(1, &snapshot) == tide_entities_hash(1, &t));
    tide_entities_free(&t);
    tide_entities_free(&snapshot);
}

TIDE_TEST(entity_table_packs_and_unpacks)
{
    tide_entities t = {0};
    for (uint32_t i = 0; i < 1500; i++) tide_entity_create(&t);
    tide_entity_destroy(&t, (tide_entity){7, 1});
    tide_entity_destroy(&t, (tide_entity){1200, 1});
    const uint32_t size = tide_entities_packed_size(&t);
    uint8_t *bytes = malloc(size);
    tide_writer w = {bytes, size, 0, false, false};
    tide_entities_pack(&t, &w);
    TIDE_CHECK(!w.overflow && w.size == size);

    tide_entities back = {0};
    tide_reader r = {bytes, size, 0, false};
    TIDE_REQUIRE(tide_entities_unpack(&back, &r));
    TIDE_CHECK(tide_entities_hash(1, &back) == tide_entities_hash(1, &t));
    TIDE_CHECK(tide_entity_create(&back).index == 1200); // Free slots come back in the same order
    TIDE_CHECK(tide_entity_create(&back).index == 7);

    // A free list that goes round isn't a table
    tide_entities broken = {0};
    tide_entity_slot *slot = (tide_entity_slot *)(bytes + 8u + 7u * sizeof(tide_entity_slot));
    slot->row = 1201; // Slot 7 leads back to 1200
    tide_reader again = {bytes, size, 0, false};
    TIDE_CHECK(!tide_entities_unpack(&broken, &again));
    tide_entities_free(&broken);
    free(bytes);
    tide_entities_free(&t);
    tide_entities_free(&back);
}
