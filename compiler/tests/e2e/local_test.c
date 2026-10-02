#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "tide_test.h"

static tide_world world;
static tide_local local;
static tide_draw_list draw;
static tide_gui gui;

// A world's bytes (tide_world_pack), to see later whether it changed.
static uint8_t *world_bytes(const tide_world *w, uint32_t *size)
{
    *size = tide_world_pack(w, NULL, 0);
    uint8_t *bytes = malloc(*size);
    tide_world_pack(w, bytes, *size);
    return bytes;
}

static bool world_is(const tide_world *w, uint8_t *bytes, const uint32_t size)
{
    uint32_t now_size;
    uint8_t *now = world_bytes(w, &now_size);
    const bool same = now_size == size && memcmp(now, bytes, size) == 0;
    free(now);
    free(bytes);
    return same;
}

static uint8_t *local_bytes(const tide_local *l, uint32_t *size)
{
    *size = tide_local_pack(l, NULL, 0);
    uint8_t *bytes = malloc(*size);
    tide_local_pack(l, bytes, *size);
    return bytes;
}

static bool local_is(const tide_local *l, uint8_t *bytes, const uint32_t size)
{
    uint32_t now_size;
    uint8_t *now = local_bytes(l, &now_size);
    const bool same = now_size == size && memcmp(now, bytes, size) == 0;
    free(now);
    free(bytes);
    return same;
}

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
    const Spark newest = tide_get_Spark(&local, local.Frames.newest);
    TIDE_REQUIRE(tide_has_Spark(&local, local.Frames.newest));
    TIDE_CHECK(newest.position.x == 3.0f && newest.position.y == 4.0f);
    TIDE_CHECK(newest.lit);
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
    uint32_t size;
    uint8_t *before = world_bytes(&world, &size);
    frame(&world);
    frame(&world);
    TIDE_CHECK(world_is(&world, before, size));
}

TIDE_TEST(local_ticks_leave_local_state_alone)
{
    start();
    frame(&world);
    uint32_t size;
    uint8_t *before = local_bytes(&local, &size);
    tide_world_tick(&world);
    TIDE_CHECK(local_is(&local, before, size));
}

TIDE_TEST(local_views_run_outside_a_match)
{
    start();
    frame(NULL);
    TIDE_CHECK(local.Frames.count == 1);
    TIDE_CHECK(local.Frames.blinks == 2);
    TIDE_CHECK(tide_local_entity_count(&local) == 0); // Trail reads the match, so it didn't run
}
