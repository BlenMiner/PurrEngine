#include <string.h>

#include "game.h"
#include "tide_test.h"

TIDE_TEST(settings_are_in_the_game)
{
    TIDE_CHECK(tide_game_api.tick_rate == 30);
    TIDE_REQUIRE(tide_game_api.title);
    TIDE_CHECK(strcmp(tide_game_api.title, "Tide \"settings\"") == 0);
}

// A session whose desc leaves the tick rate out runs its matches at the game's.
TIDE_TEST(settings_tick_rate_runs_the_match)
{
    tide_session *s = tide_session_create(&(tide_session_desc){.game = &tide_game_api});
    double t = 0.0;
    tide_session_start(s, NULL, t);
    for (int frame = 0; frame < 60; frame++) {
        t += 1.0 / 60.0;
        tide_session_update(s, t);
    }
    const tide_world *w = tide_session_server_world(s);
    TIDE_REQUIRE(w);
    TIDE_CHECK(w->Time.dt == 1.0f / 30.0f);
    TIDE_CHECK(w->Time.tick >= 28 && w->Time.tick <= 31); // A second at 30 ticks a second
    tide_session_destroy(s);

    // The desc's own rate goes over it.
    s = tide_session_create(&(tide_session_desc){.game = &tide_game_api, .tick_rate = 60});
    t = 0.0;
    tide_session_start(s, NULL, t);
    tide_session_update(s, t + 0.1);
    TIDE_CHECK(((const tide_world *)tide_session_server_world(s))->Time.dt == 1.0f / 60.0f);
    tide_session_destroy(s);
}
