#include <string.h>

#include "game.h"
#include "tide_test.h"

// Clients stay ahead of the server by how early their inputs reach it. A game
// with no input sends none, so that says nothing, and a client that read it
// ran as far ahead as prediction goes.

TIDE_TEST(net_no_input_single_player_keeps_to_the_server)
{
    tide_session *s = tide_session_create(&(tide_session_desc){.game = &tide_game_api, .tick_rate = 60});
    double t = 0.0;
    tide_session_start(s, NULL, t);
    for (int frame = 0; frame < 180; frame++) {
        t += 1.0 / 60.0;
        tide_session_update(s, t);
        const tide_session_status status = tide_session_status_of(s);
        if (status.client.state != TIDE_SESSION_CONNECTED) continue;
        // This machine's player has no lead: it's where the server is
        const uint32_t server = (uint32_t)((const tide_world *)tide_session_server_world(s))->Time.tick;
        TIDE_CHECK(status.client.predicted_tick <= server + 1u);
    }
    const tide_session_status status = tide_session_status_of(s);
    TIDE_CHECK(status.client.state == TIDE_SESSION_CONNECTED);
    TIDE_CHECK(status.client.resyncs == 0);
    TIDE_CHECK(status.client.verified_tick >= 170u);
    TIDE_CHECK(memcmp(tide_session_world(s), tide_session_server_world(s), sizeof(tide_world)) == 0);
    tide_session_destroy(s);
}

TIDE_TEST(net_no_input_client_keeps_to_the_server)
{
    double now = 0.0;
    tide_loopback *net = tide_loopback_create(7);
    tide_loopback_set_conditions(net, (tide_net_conditions){.latency = 0.04, .jitter = 0.01});
    tide_server *server = tide_server_create(
        &(tide_server_desc){.game = &tide_game_api, .tick_rate = 60, .transports = {tide_loopback_endpoint(net, 1)}},
        now);
    tide_client *client = tide_client_create(&(tide_client_desc){.game = &tide_game_api,
                                                                 .transport = tide_loopback_endpoint(net, 2),
                                                                 .server = tide_loopback_address(1),
                                                                 .lead = 2},
                                             now);
    while (now < 4.0) {
        now += 1.0 / 60.0;
        tide_loopback_set_time(net, now);
        tide_client_update(client, now);
        tide_server_update(server, now);
    }
    const tide_client_status status = tide_client_status_of(client);
    TIDE_CHECK(status.state == TIDE_SESSION_CONNECTED);
    TIDE_CHECK(status.resyncs == 0);
    // Ahead by its lead and the time it takes to hear from the server, as when
    // it joined, and no more: a round trip is about 5 ticks here
    TIDE_CHECK(status.predicted_tick <= tide_server_tick(server) + 2u + 8u);
    TIDE_CHECK(status.verified_tick + 10u >= tide_server_tick(server));
    tide_client_destroy(client);
    tide_server_destroy(server);
    tide_loopback_destroy(net);
}
