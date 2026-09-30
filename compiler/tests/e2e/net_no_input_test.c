#include <string.h>

#include "game.h"
#include "purr_test.h"

// Clients stay ahead of the server by how early their inputs reach it. A game
// with no input sends none, so that says nothing, and a client that read it
// ran as far ahead as prediction goes.

PURR_TEST(net_no_input_single_player_keeps_to_the_server)
{
    purr_session *s = purr_session_create(&(purr_session_desc){.game = &purr_game_api, .tick_rate = 60});
    double t = 0.0;
    purr_session_play(s, NULL, t);
    for (int frame = 0; frame < 180; frame++) {
        t += 1.0 / 60.0;
        purr_session_update(s, t);
        const purr_session_status status = purr_session_status_of(s);
        if (status.client.state != PURR_SESSION_CONNECTED) continue;
        // This machine's player has no lead: it's where the server is
        const uint32_t server = (uint32_t)((const purr_world *)purr_session_server_world(s))->Time.tick;
        PURR_CHECK(status.client.predicted_tick <= server + 1u);
    }
    const purr_session_status status = purr_session_status_of(s);
    PURR_CHECK(status.client.state == PURR_SESSION_CONNECTED);
    PURR_CHECK(status.client.resyncs == 0);
    PURR_CHECK(status.client.verified_tick >= 170u);
    PURR_CHECK(memcmp(purr_session_world(s), purr_session_server_world(s), sizeof(purr_world)) == 0);
    purr_session_destroy(s);
}

PURR_TEST(net_no_input_client_keeps_to_the_server)
{
    double now = 0.0;
    purr_loopback *net = purr_loopback_create(7);
    purr_loopback_set_conditions(net, (purr_net_conditions){.latency = 0.04, .jitter = 0.01});
    purr_server *server = purr_server_create(
        &(purr_server_desc){.game = &purr_game_api, .tick_rate = 60, .transports = {purr_loopback_endpoint(net, 1)}},
        now);
    purr_client *client = purr_client_create(&(purr_client_desc){.game = &purr_game_api,
                                                                 .transport = purr_loopback_endpoint(net, 2),
                                                                 .server = purr_loopback_address(1),
                                                                 .lead = 2},
                                             now);
    while (now < 4.0) {
        now += 1.0 / 60.0;
        purr_loopback_set_time(net, now);
        purr_client_update(client, now);
        purr_server_update(server, now);
    }
    const purr_client_status status = purr_client_status_of(client);
    PURR_CHECK(status.state == PURR_SESSION_CONNECTED);
    PURR_CHECK(status.resyncs == 0);
    // Ahead by its lead and the time it takes to hear from the server, as when
    // it joined, and no more: a round trip is about 5 ticks here
    PURR_CHECK(status.predicted_tick <= purr_server_tick(server) + 2u + 8u);
    PURR_CHECK(status.verified_tick + 10u >= purr_server_tick(server));
    purr_client_destroy(client);
    purr_server_destroy(server);
    purr_loopback_destroy(net);
}
