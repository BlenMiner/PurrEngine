#include <string.h>

#include "game.h"
#include "purr_test.h"

static purr_world world;
static purr_world world_before;
static purr_local local;
static purr_local local_before;
static purr_draw_list draw;

static void start(void)
{
    purr_world_init(&world, 1.0f);
    purr_local_init(&local);
}

static void frame(const purr_world *w)
{
    purr_draw_reset(&draw);
    purr_frame(w, &local, &draw);
}

PURR_TEST(local_views_change_local_state)
{
    start();
    frame(&world);
    PURR_CHECK(local.Frames.count == 1);
    PURR_CHECK(local.Frames.blinks == 2); // Count's Blink, handled at the end of the frame
    PURR_CHECK(local.Frames.lit == 2);    // Each spark's Spawned
    PURR_CHECK(purr_local_entity_count(&local) == 2);
    PURR_CHECK(purr_world_entity_count(&world) == 3); // The Main scene and two balls
    const Spark *newest = purr_get_Spark(&local, local.Frames.newest);
    PURR_REQUIRE(newest != NULL);
    PURR_CHECK(newest->position.x == 3.0f && newest->position.y == 4.0f);
    PURR_CHECK(newest->lit);
    PURR_CHECK(purr_entity_equal(local.Frames.selected, (purr_entity){2, 1})); // The second ball
}

PURR_TEST(local_entities_destroy_themselves)
{
    start();
    frame(&world);
    frame(&world);
    PURR_CHECK(purr_local_entity_count(&local) == 4);
    frame(&world);
    PURR_CHECK(purr_local_entity_count(&local) == 4); // The first two faded, and two more came
    PURR_CHECK(local.Frames.lit == 6);
}

PURR_TEST(local_frames_leave_the_match_alone)
{
    start();
    purr_world_tick(&world);
    memcpy(&world_before, &world, sizeof world);
    frame(&world);
    frame(&world);
    PURR_CHECK(memcmp(&world_before, &world, sizeof world) == 0);
}

PURR_TEST(local_ticks_leave_local_state_alone)
{
    start();
    frame(&world);
    memcpy(&local_before, &local, sizeof local);
    purr_world_tick(&world);
    PURR_CHECK(memcmp(&local_before, &local, sizeof local) == 0);
}

PURR_TEST(local_views_run_outside_a_match)
{
    start();
    frame(NULL);
    PURR_CHECK(local.Frames.count == 1);
    PURR_CHECK(local.Frames.blinks == 2);
    PURR_CHECK(purr_local_entity_count(&local) == 0); // Trail reads the match, so it didn't run
}
