#include <string.h>

#include "game.h"
#include "tide_test.h"

static tide_world world;
static tide_world snapshot;

// The Main scene is slot 0, and Setup spawns crowd i in slot i + 1.
static tide_entity crowd(const uint32_t i)
{
    return (tide_entity){i + 1u, 1};
}

static bool labelled(const tide_world *w, const uint32_t i, const char *expected)
{
    const Crowd *c = tide_read_Crowd(w, crowd(i));
    if (!c) return false;
    const tide_str s = tide_text_read(&w->heap, c->label);
    return s.bytes == (int32_t)strlen(expected) && memcmp(s.ptr, expected, strlen(expected)) == 0;
}

static void ticks(tide_world *w, const int n)
{
    for (int i = 0; i < n; i++) tide_world_tick(w);
}

TIDE_TEST(big_world_grows_past_the_old_limits)
{
    tide_world_init(&world, 1.0f);
    TIDE_CHECK(world.Counts.spawned == 6000);
    TIDE_CHECK(tide_world_entity_count(&world) == 6001);
    TIDE_CHECK(labelled(&world, 4321, "crowd 4321"));
    ticks(&world, 200);
    TIDE_CHECK(world.Counts.retired > 1000);
    TIDE_CHECK(tide_world_entity_count(&world) == 6001u - (uint32_t)world.Counts.retired);
    const Crowd *c = tide_read_Crowd(&world, crowd(1));
    TIDE_REQUIRE(c != NULL);
    TIDE_CHECK(c->value == 201.0f);
    TIDE_CHECK(labelled(&world, 5999, "crowd 5999"));
    tide_world_free(&world);
}

// Rollback across thousands of rows: a snapshot keeps its state while the
// world it shares pages with goes on, and going back to it runs the same.
TIDE_TEST(big_world_snapshots_keep_their_state)
{
    tide_world_init(&world, 1.0f);
    ticks(&world, 3);
    tide_world_copy(&snapshot, &world);
    const uint64_t before = tide_world_hash(&snapshot);
    ticks(&world, 40);
    const uint64_t after = tide_world_hash(&world);
    TIDE_CHECK(after != before);
    TIDE_CHECK(tide_world_hash(&snapshot) == before);
    TIDE_CHECK(tide_read_Crowd(&snapshot, crowd(10))->value == 13.0f);
    TIDE_CHECK(labelled(&snapshot, 10, "crowd 10"));

    tide_world_copy(&world, &snapshot);
    TIDE_CHECK(tide_world_hash(&world) == before);
    ticks(&world, 40);
    TIDE_CHECK(tide_world_hash(&world) == after);
    TIDE_CHECK(tide_world_hash(&snapshot) == before);

    // Bytes and back: the same world
    const uint32_t size = tide_world_pack(&world, NULL, 0);
    static uint8_t bytes[1u << 20];
    TIDE_REQUIRE(size <= sizeof bytes);
    TIDE_CHECK(tide_world_pack(&world, bytes, size) == size);
    TIDE_REQUIRE(tide_world_unpack(&snapshot, bytes, size));
    TIDE_CHECK(tide_world_hash(&snapshot) == after);
    TIDE_CHECK(!tide_world_unpack(&snapshot, bytes, size - 1u));
    tide_world_free(&world);
    tide_world_free(&snapshot);
}
