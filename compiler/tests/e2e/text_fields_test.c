#include <string.h>

#include "game.h"
#include "purr_test.h"

static purr_world world;
static purr_world copy;

static bool reads(const purr_world *w, const purr_text t, const char *expected)
{
    const purr_str s = purr_text_read(&w->heap, t);
    return s.bytes == (int32_t)strlen(expected) && memcmp(s.ptr, expected, strlen(expected)) == 0;
}

PURR_TEST(text_fields_keep_their_own_text)
{
    purr_world_init(&world, 1.0f);
    purr_world_tick(&world);
    const Named *cat = purr_get_Named(&world, (purr_entity){1, 1});
    const Named *nobody = purr_get_Named(&world, (purr_entity){2, 1});
    PURR_REQUIRE(cat && nobody);
    PURR_CHECK(reads(&world, cat->name, "cats"));
    PURR_CHECK(reads(&world, cat->tag.label, "pet!") && cat->tag.level == 2);
    PURR_CHECK(reads(&world, nobody->name, "nobodys"));
    PURR_CHECK(reads(&world, nobody->tag.label, "!"));
    PURR_CHECK(world.Log.renames == 2);
    PURR_CHECK(world.Log.kept == 2);
    PURR_CHECK(world.Log.heard == 2);
    PURR_CHECK(reads(&world, world.Log.last, "nobodys said hi"));
    PURR_CHECK(reads(&world, world.Log.status, "tick 0"));
    PURR_CHECK(world.heap.pending == 0);
    PURR_CHECK(world.heap.failed == 0);

    purr_world_tick(&world);
    PURR_CHECK(purr_get_Named(&world, (purr_entity){2, 1}) == NULL); // Destroyed, its text released
    PURR_CHECK(reads(&world, world.Log.last, "nobodyss said hi"));
    PURR_CHECK(world.Log.heard == 4);
}

PURR_TEST(text_fields_snapshot_and_repeat)
{
    purr_world_init(&world, 1.0f);
    purr_world_tick(&world);
    memcpy(&copy, &world, sizeof world); // A snapshot, heap and all
    for (int i = 0; i < 5; i++) {
        purr_world_tick(&world);
        purr_world_tick(&copy);
    }
    PURR_CHECK(memcmp(&world, &copy, sizeof world) == 0);
}

PURR_TEST(text_fields_reuse_their_memory)
{
    // Once the names stop growing, every tick's new text reuses released blocks.
    purr_world_init(&world, 1.0f);
    for (int i = 0; i < 50; i++) purr_world_tick(&world);
    const uint32_t used = world.heap.used;
    for (int i = 0; i < 200; i++) purr_world_tick(&world);
    PURR_CHECK(world.heap.used == used);
    const Named *cat = purr_get_Named(&world, (purr_entity){1, 1});
    PURR_REQUIRE(cat != NULL);
    PURR_CHECK(reads(&world, cat->name, "CATSSSSSSSSS"));
    purr_world_print(&world);
}
