#include <string.h>

#include "game.h"
#include "tide_test.h"

static tide_world world;
static tide_world copy;

static bool reads(const tide_world *w, const tide_text t, const char *expected)
{
    const tide_str s = tide_text_read(&w->heap, t);
    return s.bytes == (int32_t)strlen(expected) && memcmp(s.ptr, expected, strlen(expected)) == 0;
}

TIDE_TEST(text_fields_keep_their_own_text)
{
    tide_world_init(&world, 1.0f);
    tide_world_tick(&world);
    const Named *cat = tide_get_Named(&world, (tide_entity){1, 1});
    const Named *nobody = tide_get_Named(&world, (tide_entity){2, 1});
    TIDE_REQUIRE(cat && nobody);
    TIDE_CHECK(reads(&world, cat->name, "cats"));
    TIDE_CHECK(reads(&world, cat->tag.label, "pet!") && cat->tag.level == 2);
    TIDE_CHECK(reads(&world, nobody->name, "nobodys"));
    TIDE_CHECK(reads(&world, nobody->tag.label, "!"));
    TIDE_CHECK(world.Log.renames == 2);
    TIDE_CHECK(world.Log.kept == 2);
    TIDE_CHECK(world.Log.heard == 2);
    TIDE_CHECK(reads(&world, world.Log.last, "nobodys said hi"));
    TIDE_CHECK(reads(&world, world.Log.status, "tick 0"));
    TIDE_CHECK(world.heap.pending == 0);
    TIDE_CHECK(world.heap.failed == 0);

    tide_world_tick(&world);
    TIDE_CHECK(tide_get_Named(&world, (tide_entity){2, 1}) == NULL); // Destroyed, its text released
    TIDE_CHECK(reads(&world, world.Log.last, "nobodyss said hi"));
    TIDE_CHECK(world.Log.heard == 4);
}

TIDE_TEST(text_fields_snapshot_and_repeat)
{
    tide_world_init(&world, 1.0f);
    tide_world_tick(&world);
    memcpy(&copy, &world, sizeof world); // A snapshot, heap and all
    for (int i = 0; i < 5; i++) {
        tide_world_tick(&world);
        tide_world_tick(&copy);
    }
    TIDE_CHECK(memcmp(&world, &copy, sizeof world) == 0);
}

TIDE_TEST(text_fields_reuse_their_memory)
{
    // Once the names stop growing, every tick's new text reuses released blocks.
    tide_world_init(&world, 1.0f);
    for (int i = 0; i < 50; i++) tide_world_tick(&world);
    const uint32_t used = world.heap.used;
    for (int i = 0; i < 200; i++) tide_world_tick(&world);
    TIDE_CHECK(world.heap.used == used);
    const Named *cat = tide_get_Named(&world, (tide_entity){1, 1});
    TIDE_REQUIRE(cat != NULL);
    TIDE_CHECK(reads(&world, cat->name, "CATSSSSSSSSS"));
    tide_world_print(&world);
}
