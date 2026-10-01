#include <string.h>

#include "game.h"
#include "tide_test.h"

static tide_world world;
static tide_world world_before;
static tide_local local;
static tide_local local_before;
static tide_draw_list draw;
static tide_gui gui;

static void start(void)
{
    tide_world_init(&world, 1.0f);
    tide_local_init(&local);
}

static void frame(const tide_world *w)
{
    tide_draw_reset(&draw);
    tide_frame(w, NULL, 1.0f, &local, &draw, &gui);
}

TIDE_TEST(local_views_change_local_state)
{
    start();
    frame(&world);
    TIDE_CHECK(local.Frames.count == 1);
    TIDE_CHECK(local.Frames.blinks == 2); // Count's Blink, handled at the end of the frame
    TIDE_CHECK(local.Frames.lit == 2);    // Each spark's Spawned
    TIDE_CHECK(tide_local_entity_count(&local) == 2);
    TIDE_CHECK(tide_world_entity_count(&world) == 3); // The Main scene and two balls
    const Spark *newest = tide_get_Spark(&local, local.Frames.newest);
    TIDE_REQUIRE(newest != NULL);
    TIDE_CHECK(newest->position.x == 3.0f && newest->position.y == 4.0f);
    TIDE_CHECK(newest->lit);
    TIDE_CHECK(tide_entity_equal(local.Frames.selected, (tide_entity){2, 1})); // The second ball
}

TIDE_TEST(local_entities_destroy_themselves)
{
    start();
    frame(&world);
    frame(&world);
    TIDE_CHECK(tide_local_entity_count(&local) == 4);
    frame(&world);
    TIDE_CHECK(tide_local_entity_count(&local) == 4); // The first two faded, and two more came
    TIDE_CHECK(local.Frames.lit == 6);
}

TIDE_TEST(local_frames_leave_the_match_alone)
{
    start();
    tide_world_tick(&world);
    memcpy(&world_before, &world, sizeof world);
    frame(&world);
    frame(&world);
    TIDE_CHECK(memcmp(&world_before, &world, sizeof world) == 0);
}

TIDE_TEST(local_ticks_leave_local_state_alone)
{
    start();
    frame(&world);
    memcpy(&local_before, &local, sizeof local);
    tide_world_tick(&world);
    TIDE_CHECK(memcmp(&local_before, &local, sizeof local) == 0);
}

TIDE_TEST(local_views_run_outside_a_match)
{
    start();
    frame(NULL);
    TIDE_CHECK(local.Frames.count == 1);
    TIDE_CHECK(local.Frames.blinks == 2);
    TIDE_CHECK(tide_local_entity_count(&local) == 0); // Trail reads the match, so it didn't run
}
