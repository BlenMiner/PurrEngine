#include "game.h"
#include "tide_test.h"

static tide_world world;

TIDE_TEST(loops_run_as_written)
{
    tide_world_init(&world, 1.0f);
    tide_world_tick(&world);
    TIDE_CHECK(tide_world_entity_count(&world) == 5); // Main, the tally and three sparks
    const Spark *last = tide_get_Spark(&world, (tide_entity){4, 1});
    TIDE_REQUIRE(last != NULL);
    TIDE_CHECK(last->index == 2);

    const Tally *t = tide_get_Tally(&world, (tide_entity){1, 1});
    TIDE_REQUIRE(t != NULL);
    TIDE_CHECK(t->sum == 55);
    TIDE_CHECK(t->countdown == 3);
    TIDE_CHECK(t->evens == 5);
    TIDE_CHECK(t->pairs == 10);
    TIDE_CHECK(t->switched == 3 && t->skipped == 4);
    TIDE_CHECK(t->picked == 6); // Rounds 0 and 2: round 1 continued, round 3 broke out
    TIDE_CHECK(t->forever == 5);
    TIDE_CHECK(t->half == 0.5f);
}
