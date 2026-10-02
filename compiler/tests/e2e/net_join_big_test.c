#include <stdio.h>
#include <string.h>

#include "game.h"
#include "tide_test.h"

// A server and a player on a loopback network, in made-up time, with updates
// a tenth of a second apart: the server sends 64 KB of the world in each, so
// the world's 4 MB take about six seconds, more than the four seconds of
// ticks it keeps. The ticks the player needs once it has the world are kept
// until it catches up, rather than the world being sent again and again.

static void sample(void *user, const uint32_t tick, void *input)
{
    (void)user;
    Controls *c = input;
    *c = (Controls){.value = (int32_t)(tick % 7u)};
}

TIDE_TEST(net_join_big_world_catches_up)
{
    tide_loopback *net = tide_loopback_create(7);
    tide_loopback_set_conditions(net, (tide_net_conditions){.latency = 0.02, .loss = 0.01});
    double now = 0.0;
    const tide_server_desc server_desc = {.game = &tide_game_api, .tick_rate = 60, .transports = {tide_loopback_endpoint(net, 1)}};
    tide_server *server = tide_server_create(&server_desc, now);
    TIDE_REQUIRE(server != NULL);
    const tide_client_desc client_desc = {
        .game = &tide_game_api,
        .transport = tide_loopback_endpoint(net, 2),
        .server = tide_loopback_address(1),
        .sample = sample,
        .lead = 2,
    };
    tide_client *client = tide_client_create(&client_desc, now);
    TIDE_REQUIRE(client != NULL);

    double connected = 0.0;
    while (now < 20.0) {
        now += 0.1;
        tide_loopback_set_time(net, now);
        tide_client_update(client, now);
        tide_server_update(server, now);
        if (!connected && tide_client_status_of(client).state == TIDE_SESSION_CONNECTED) connected = now;
    }

    const tide_client_status s = tide_client_status_of(client);
    printf("    the world arrived after %.1f s\n", connected);
    TIDE_CHECK(connected > 4.0); // Longer than the ticks the server keeps
    TIDE_CHECK(s.state == TIDE_SESSION_CONNECTED);
    TIDE_CHECK(s.resyncs == 0);
    TIDE_CHECK(s.verified_tick + 30u > tide_server_tick(server)); // Caught up
    const tide_world *world = tide_client_verified_world(client);
    TIDE_CHECK(world && world->Archive.total > 0); // Its inputs reached the match

    tide_client_destroy(client);
    tide_server_destroy(server);
    tide_loopback_destroy(net);
}
