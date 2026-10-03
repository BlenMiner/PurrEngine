#include "game.h"
#include "tide_test.h"

static tide_world world;

// The result after one tick. C++ keeps its counters between ticks, so the
// world ticks once, whichever test runs first.
static Result result(void)
{
    static bool ticked;
    if (!ticked) {
        tide_world_init(&world, 1.0f); // Loads Main, which spawns the result
        tide_world_tick(&world);
        ticked = true;
    }
    return tide_get_Result(&world, (tide_entity){1, 1});
}

TIDE_TEST(cpp_a_global_is_constructed_before_the_game_runs)
{
    TIDE_CHECK(result().started == 10);
}

TIDE_TEST(cpp_classes_templates_and_virtual_functions)
{
    const Result r = result();
    TIDE_CHECK(r.area == 9.0f);
    TIDE_CHECK(r.largest == 12);
    TIDE_CHECK(r.totals.count == 2); // A method, on the game's struct by address
    TIDE_CHECK(r.totals.sum == 4.0f);
}

// new and delete are the code's own, over malloc and free: no C++ runtime
// defines them. Two bodies weighing 50, both alive, then neither.
TIDE_TEST(cpp_new_and_delete_are_the_codes_own)
{
    TIDE_CHECK(result().made == 5020);
}

TIDE_TEST(cpp_a_functions_static_is_constructed_the_first_time_through)
{
    const Result r = result();
    TIDE_CHECK(r.firstCall == 11);
    TIDE_CHECK(r.secondCall == 12);
}
