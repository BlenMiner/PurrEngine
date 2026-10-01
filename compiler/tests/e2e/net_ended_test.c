#include "game.h"
#include "tide_test.h"

// The arena counts down 30 ticks, then unloads. Main is local, so it can't
// come back in the match: the match ends.

static const tide_start arena = {.scene = 0, .value.Arena = {.rounds = 30}};

TIDE_TEST(net_ended_when_the_last_scene_unloads)
{
    static tide_world w;
    tide_world_start(&w, 1.0f / 60.0f, &arena);
    for (int i = 0; i < 29; i++) {
        tide_world_tick(&w);
        TIDE_CHECK(!tide_world_ended(&w));
    }
    tide_world_tick(&w);
    TIDE_CHECK(tide_world_ended(&w));
    TIDE_CHECK(tide_world_entity_count(&w) == 0);
}

TIDE_TEST(net_ended_a_session_goes_back_to_main)
{
    static tide_local local;
    static tide_draw_list draw;
    static tide_gui gui;
    tide_local_init(&local);
    tide_session *s = tide_session_create(&(tide_session_desc){.game = &tide_game_api, .tick_rate = 60});
    double t = 0.0;

    // The menu starts a match in the arena.
    local.Menu.play = true;
    tide_draw_reset(&draw);
    tide_frame(NULL, NULL, 1.0f, &local, &draw, &gui);
    tide_session_request request;
    tide_start start;
    TIDE_REQUIRE(tide_local_take_request(&local, &request, &start));
    TIDE_CHECK(request.kind == TIDE_REQUEST_START && start.value.Arena.rounds == 30);
    tide_session_start(s, &start, t);
    bool connected = false;
    bool ended = false;
    for (int frame = 0; frame < 120; frame++) {
        t += 1.0 / 60.0;
        tide_session_update(s, t);
        tide_session_event e;
        while (tide_session_next_event(s, &e)) {
            if (e.kind == TIDE_SESSION_CONNECTED_EVENT) {
                connected = true;
                continue;
            }
            TIDE_CHECK(e.reason == TIDE_DISCONNECT_ENDED);
            ended = true;
            tide_local_disconnected(&local, e.reason);
        }
    }
    TIDE_CHECK(connected && ended);
    TIDE_CHECK(tide_session_status_of(s).client.state == TIDE_SESSION_OFFLINE);
    TIDE_CHECK(tide_session_world(s) == NULL);

    // Local code hears why, and it's in Main, as it was before the match.
    tide_draw_reset(&draw);
    tide_frame(NULL, NULL, 1.0f, &local, &draw, &gui);
    TIDE_CHECK(local.Menu.ended == 1);
    TIDE_CHECK(tide_local_entity_count(&local) == 1);
    tide_session_destroy(s);
}

static tide_client *join(tide_loopback *net, const uint32_t endpoint, const double now)
{
    return tide_client_create(&(tide_client_desc){.game = &tide_game_api,
                                                  .transport = tide_loopback_endpoint(net, endpoint),
                                                  .server = tide_loopback_address(1),
                                                  .lead = 2},
                              now);
}

TIDE_TEST(net_ended_players_hear_it)
{
    double now = 0.0;
    tide_loopback *net = tide_loopback_create(3);
    tide_loopback_set_conditions(net, (tide_net_conditions){.latency = 0.03, .jitter = 0.01, .loss = 0.1});
    tide_server *server = tide_server_create(&(tide_server_desc){.game = &tide_game_api,
                                                                 .tick_rate = 60,
                                                                 .start = &arena,
                                                                 .transports = {tide_loopback_endpoint(net, 1)}},
                                             now);
    tide_client *clients[2] = {join(net, 2, now), NULL};
    while (now < 3.0) {
        now += 1.0 / 60.0;
        if (!clients[1] && now >= 2.0) clients[1] = join(net, 3, now); // Once it's over
        tide_loopback_set_time(net, now);
        for (int i = 0; i < 2; i++) {
            if (clients[i]) tide_client_update(clients[i], now);
        }
        tide_server_update(server, now);
    }
    TIDE_CHECK(tide_server_tick(server) == 30u); // No more ticks once it ended
    for (int i = 0; i < 2; i++) {
        const tide_client_status status = tide_client_status_of(clients[i]);
        TIDE_CHECK(status.state == TIDE_SESSION_OFFLINE && status.reason == TIDE_DISCONNECT_ENDED);
        tide_client_destroy(clients[i]);
    }
    tide_server_destroy(server);
    tide_loopback_destroy(net);
}
