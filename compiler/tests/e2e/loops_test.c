#include "game.h"
#include "purr_test.h"

static purr_world world;

PURR_TEST(loops_run_as_written)
{
    purr_world_init(&world, 1.0f);
    purr_world_tick(&world);
    PURR_CHECK(purr_world_entity_count(&world) == 5); // Main, the tally and three sparks
    const Spark *last = purr_get_Spark(&world, (purr_entity){4, 1});
    PURR_REQUIRE(last != NULL);
    PURR_CHECK(last->index == 2);

    const Tally *t = purr_get_Tally(&world, (purr_entity){1, 1});
    PURR_REQUIRE(t != NULL);
    PURR_CHECK(t->sum == 55);
    PURR_CHECK(t->countdown == 3);
    PURR_CHECK(t->evens == 5);
    PURR_CHECK(t->pairs == 10);
    PURR_CHECK(t->switched == 3 && t->skipped == 4);
    PURR_CHECK(t->picked == 6); // Rounds 0 and 2: round 1 continued, round 3 broke out
    PURR_CHECK(t->forever == 5);
    PURR_CHECK(t->half == 0.5f);
}
