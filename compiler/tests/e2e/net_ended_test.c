#include "game.h"
#include "purr_test.h"

// The arena counts down 30 ticks, then unloads. Main is local, so it can't
// come back in the match: the match ends.

static const purr_start arena = {.scene = 0, .value.Arena = {.rounds = 30}};

PURR_TEST(net_ended_when_the_last_scene_unloads)
{
    static purr_world w;
    purr_world_start(&w, 1.0f / 60.0f, &arena);
    for (int i = 0; i < 29; i++) {
        purr_world_tick(&w);
        PURR_CHECK(!purr_world_ended(&w));
    }
    purr_world_tick(&w);
    PURR_CHECK(purr_world_ended(&w));
    PURR_CHECK(purr_world_entity_count(&w) == 0);
}

PURR_TEST(net_ended_a_session_goes_back_to_main)
{
    static purr_local local;
    static purr_draw_list draw;
    static purr_gui gui;
    purr_local_init(&local);
    purr_session *s = purr_session_create(&(purr_session_desc){.game = &purr_game_api, .tick_rate = 60});
    double t = 0.0;

    // The menu starts a match in the arena.
    local.Menu.play = true;
    purr_draw_reset(&draw);
    purr_frame(NULL, NULL, 1.0f, &local, &draw, &gui);
    purr_session_request request;
    purr_start start;
    PURR_REQUIRE(purr_local_take_request(&local, &request, &start));
    PURR_CHECK(request.kind == PURR_REQUEST_PLAY && start.value.Arena.rounds == 30);
    purr_session_play(s, &start, t);
    bool connected = false;
    bool ended = false;
    for (int frame = 0; frame < 120; frame++) {
        t += 1.0 / 60.0;
        purr_session_update(s, t);
        purr_session_event e;
        while (purr_session_next_event(s, &e)) {
            if (e.kind == PURR_SESSION_CONNECTED_EVENT) {
                connected = true;
                continue;
            }
            PURR_CHECK(e.reason == PURR_DISCONNECT_ENDED);
            ended = true;
            purr_local_disconnected(&local, e.reason);
        }
    }
    PURR_CHECK(connected && ended);
    PURR_CHECK(purr_session_status_of(s).client.state == PURR_SESSION_OFFLINE);
    PURR_CHECK(purr_session_world(s) == NULL);

    // Local code hears why, and it's in Main, as it was before the match.
    purr_draw_reset(&draw);
    purr_frame(NULL, NULL, 1.0f, &local, &draw, &gui);
    PURR_CHECK(local.Menu.ended == 1);
    PURR_CHECK(purr_local_entity_count(&local) == 1);
    purr_session_destroy(s);
}

static purr_client *join(purr_loopback *net, const uint32_t endpoint, const double now)
{
    return purr_client_create(&(purr_client_desc){.game = &purr_game_api,
                                                  .transport = purr_loopback_endpoint(net, endpoint),
                                                  .server = purr_loopback_address(1),
                                                  .lead = 2},
                              now);
}

PURR_TEST(net_ended_players_hear_it)
{
    double now = 0.0;
    purr_loopback *net = purr_loopback_create(3);
    purr_loopback_set_conditions(net, (purr_net_conditions){.latency = 0.03, .jitter = 0.01, .loss = 0.1});
    purr_server *server = purr_server_create(&(purr_server_desc){.game = &purr_game_api,
                                                                 .tick_rate = 60,
                                                                 .start = &arena,
                                                                 .transports = {purr_loopback_endpoint(net, 1)}},
                                             now);
    purr_client *clients[2] = {join(net, 2, now), NULL};
    while (now < 3.0) {
        now += 1.0 / 60.0;
        if (!clients[1] && now >= 2.0) clients[1] = join(net, 3, now); // Once it's over
        purr_loopback_set_time(net, now);
        for (int i = 0; i < 2; i++) {
            if (clients[i]) purr_client_update(clients[i], now);
        }
        purr_server_update(server, now);
    }
    PURR_CHECK(purr_server_tick(server) == 30u); // No more ticks once it ended
    for (int i = 0; i < 2; i++) {
        const purr_client_status status = purr_client_status_of(clients[i]);
        PURR_CHECK(status.state == PURR_SESSION_OFFLINE && status.reason == PURR_DISCONNECT_ENDED);
        purr_client_destroy(clients[i]);
    }
    purr_server_destroy(server);
    purr_loopback_destroy(net);
}
