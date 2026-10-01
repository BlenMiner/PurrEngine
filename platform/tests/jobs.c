// Ticks on every core (tide_platform_jobs) give the same worlds as ticks on
// one thread, tick after tick (platform/tests/jobs.tide).

#include <stdio.h>
#include <stdlib.h>

#include "game.h"
#include "tide/platform.h"
#include "tide_test.h"

static tide_world one;      // Ticked on this thread
static tide_world many;     // On every core
static tide_world snapshot; // Of `many`, before each tick: it shares its pages

TIDE_TEST(jobs_threads_change_nothing)
{
    const tide_jobs *jobs = tide_platform_jobs();
    if (!jobs) {
        printf("    one core: nothing to compare\n");
        return;
    }
    tide_world_init(&one, 1.0f / 60.0f);
    tide_world_init(&many, 1.0f / 60.0f);
    TIDE_REQUIRE(tide_world_hash(&one) == tide_world_hash(&many));
    for (int t = 0; t < 240; t++) {
        tide_world_copy(&snapshot, &many);
        const uint64_t before = tide_world_hash(&snapshot);
        tide_world_tick(&one);
        tide_world_tick_on(&many, jobs);
        if (tide_world_hash(&one) != tide_world_hash(&many)) printf("    tick %d came out different\n", t);
        TIDE_REQUIRE(tide_world_hash(&one) == tide_world_hash(&many));
        TIDE_REQUIRE(tide_world_hash(&snapshot) == before); // Its pages stayed as they were
    }
    TIDE_CHECK(one.Stats.hits > 100 && one.Stats.spawned == 80);
    TIDE_CHECK(tide_world_entity_count(&one) == tide_world_entity_count(&many));
    tide_world_free(&one);
    tide_world_free(&many);
    tide_world_free(&snapshot);
}
