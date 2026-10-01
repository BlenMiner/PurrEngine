#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "tide_test.h"

// A server and players on one loopback network, in made-up time: frames of
// uneven length, and a network that loses, delays and reorders datagrams.
// Clients check every tick's hash against the server's, so a client that
// never had to ask for the world again kept the server's world exactly.

static tide_loopback *net;
static tide_server *server;
static tide_client *clients[3];
static int who[3] = {0, 1, 2};
static double now;

// Each player's input, from the tick: it changes every so often, as a player's does.
static void sample(void *user, const uint32_t tick, void *input)
{
    const int player = *(const int *)user;
    Controls *c = input;
    *c = (Controls){0};
    c->move = tide_f2((float)((tick / 20u + (uint32_t)player) % 3u) - 1.0f, (tick / 33u) % 2u ? 1.0f : 0.0f);
    c->jump = (tick + (uint32_t)player * 7u) % 45u < 5u;
}

static void start_at(const uint32_t tick_rate, const tide_net_conditions conditions, const uint64_t seed)
{
    now = 0.0;
    net = tide_loopback_create(seed);
    tide_loopback_set_conditions(net, conditions);
    const tide_server_desc desc = {.game = &tide_game_api, .tick_rate = tick_rate, .transports = {tide_loopback_endpoint(net, 1)}};
    server = tide_server_create(&desc, now);
    memset(clients, 0, sizeof clients);
}

static void start(const tide_net_conditions conditions, const uint64_t seed)
{
    start_at(60, conditions, seed);
}

static tide_client *join(const int number)
{
    const tide_client_desc desc = {
        .game = &tide_game_api,
        .transport = tide_loopback_endpoint(net, (uint32_t)number + 2u),
        .server = tide_loopback_address(1),
        .sample = sample,
        .user = &who[number],
        .lead = 2,
    };
    clients[number] = tide_client_create(&desc, now);
    return clients[number];
}

// Runs everything until `until`, in frames of 11 and 22 milliseconds.
static void run(const double until)
{
    bool odd = false;
    while (now < until) {
        now += (odd = !odd) ? 0.011 : 0.022;
        tide_loopback_set_time(net, now);
        for (int i = 0; i < 3; i++) {
            if (clients[i]) tide_client_update(clients[i], now);
        }
        tide_server_update(server, now);
    }
}

static void finish(void)
{
    for (int i = 0; i < 3; i++) tide_client_destroy(clients[i]);
    tide_server_destroy(server);
    tide_loopback_destroy(net);
}

static const Players *players_in(const void *world)
{
    return &((const tide_world *)world)->Players;
}

TIDE_TEST(net_players_keep_the_servers_world)
{
    start((tide_net_conditions){.latency = 0.04, .jitter = 0.02, .loss = 0.1}, 12345);
    join(0);
    run(1.0);
    join(1); // Joins a match already on: gets the world, then plays on
    run(5.0);

    for (int i = 0; i < 2; i++) {
        const tide_client_status s = tide_client_status_of(clients[i]);
        TIDE_CHECK(s.state == TIDE_SESSION_CONNECTED);
        TIDE_CHECK(s.resyncs == 0); // Every tick's hash matched the server's
        TIDE_CHECK(s.player.id == (uint32_t)i + 1u);
        // Close behind the server, and a little ahead of it with prediction
        TIDE_CHECK(s.verified_tick + 30u > tide_server_tick(server));
        TIDE_CHECK(s.predicted_tick > s.verified_tick);
        TIDE_CHECK(s.predicted_tick < tide_server_tick(server) + 30u);
    }
    TIDE_CHECK(players_in(tide_server_world(server))->joined == 2);

    // One leaves; the other sees it, once the network is good again.
    tide_client_destroy(clients[0]);
    clients[0] = NULL;
    tide_loopback_set_conditions(net, (tide_net_conditions){0});
    run(7.0);
    TIDE_CHECK(players_in(tide_server_world(server))->left == 1);
    TIDE_CHECK(players_in(tide_client_verified_world(clients[1]))->left == 1);
    TIDE_CHECK(tide_client_status_of(clients[1]).resyncs == 0);
    finish();
}

TIDE_TEST(net_a_terrible_network)
{
    start((tide_net_conditions){.latency = 0.12, .jitter = 0.08, .loss = 0.3}, 99);
    join(0);
    join(1);
    run(8.0);
    for (int i = 0; i < 2; i++) {
        const tide_client_status s = tide_client_status_of(clients[i]);
        TIDE_CHECK(s.state == TIDE_SESSION_CONNECTED);
        TIDE_CHECK(s.resyncs == 0);
        TIDE_CHECK(s.verified_tick + 60u > tide_server_tick(server));
    }
    finish();
}

// How far clients predict, and what's kept to send again, follow the tick rate.
TIDE_TEST(net_other_tick_rates)
{
    static const uint32_t rates[] = {20, 240};
    for (int i = 0; i < 2; i++) {
        start_at(rates[i], (tide_net_conditions){.latency = 0.08, .jitter = 0.04, .loss = 0.1}, 11);
        join(0);
        join(1);
        run(4.0);
        for (int k = 0; k < 2; k++) {
            const tide_client_status s = tide_client_status_of(clients[k]);
            TIDE_CHECK(s.state == TIDE_SESSION_CONNECTED);
            TIDE_CHECK(s.resyncs == 0);
            TIDE_CHECK(s.verified_tick + rates[i] / 2u > tide_server_tick(server)); // Within half a second
            TIDE_CHECK(s.predicted_tick - s.verified_tick <= rates[i]);          // At most a second ahead
        }
        TIDE_CHECK(tide_server_tick(server) + rates[i] / 10u > 4u * rates[i]);
        finish();
    }
}

TIDE_TEST(net_a_client_that_diverges_gets_the_world_again)
{
    start((tide_net_conditions){.latency = 0.03}, 7);
    join(0);
    run(1.0);
    // Something went wrong on this machine: its world is no longer the server's, and
    // what it predicts from it isn't either.
    ((tide_world *)tide_client_world(clients[0]))->Players.joined = 50;
    run(3.0);
    const tide_client_status s = tide_client_status_of(clients[0]);
    TIDE_CHECK(s.resyncs == 1);
    TIDE_CHECK(s.state == TIDE_SESSION_CONNECTED);
    TIDE_CHECK(players_in(tide_client_verified_world(clients[0]))->joined == 1);
    finish();
}

// A player's cookie gets them their PlayerID back.
static tide_client *join_as(const int number, const uint64_t cookie)
{
    tide_client *c = join(number);
    tide_client_destroy(c);
    const tide_client_desc desc = {
        .game = &tide_game_api,
        .transport = tide_loopback_endpoint(net, (uint32_t)number + 2u),
        .server = tide_loopback_address(1),
        .sample = sample,
        .user = &who[number],
        .lead = 2,
        .cookie = cookie,
    };
    clients[number] = tide_client_create(&desc, now);
    return clients[number];
}

TIDE_TEST(net_a_player_takes_over_from_a_connection_gone_quiet)
{
    start((tide_net_conditions){.latency = 0.02}, 3);
    join(0);
    run(1.0);
    const uint64_t cookie = tide_client_status_of(clients[0]).cookie;
    TIDE_CHECK(cookie != 0);
    // The first connection goes quiet (its machine's address changed, say),
    // and the same player connects again before the server gives up on them.
    tide_client *quiet = clients[0];
    clients[0] = NULL;
    join_as(1, cookie);
    run(2.0);
    const tide_client_status s = tide_client_status_of(clients[1]);
    TIDE_CHECK(s.state == TIDE_SESSION_CONNECTED && s.player.id == 1u && s.cookie == cookie);
    const Players *players = players_in(tide_server_world(server));
    TIDE_CHECK(players->joined == 1 && players->left == 0); // No events: nothing happened, as far as the game knows
    TIDE_CHECK(tide_server_player_count(server) == 1);
    tide_client_destroy(quiet);
    finish();
}

TIDE_TEST(net_a_player_who_left_comes_back_as_themselves)
{
    start((tide_net_conditions){.latency = 0.02}, 4);
    join(0);
    run(0.5);
    join(1);
    run(1.0);
    const uint64_t cookie = tide_client_status_of(clients[0]).cookie;
    tide_client_destroy(clients[0]); // Leaves
    clients[0] = NULL;
    run(1.5);
    TIDE_CHECK(players_in(tide_server_world(server))->left == 1);

    join_as(2, cookie);
    run(2.5);
    const tide_client_status back = tide_client_status_of(clients[2]);
    TIDE_CHECK(back.state == TIDE_SESSION_CONNECTED && back.player.id == 1u); // Player 0 again
    TIDE_CHECK(players_in(tide_server_world(server))->joined == 3); // PlayerJoined again, with the same PlayerID
    TIDE_CHECK(tide_client_status_of(clients[1]).player.id == 2u);

    // Without the cookie, a new player.
    join_as(0, 0);
    run(3.5);
    TIDE_CHECK(tide_client_status_of(clients[0]).player.id == 3u);
    finish();
}

TIDE_TEST(net_a_session_joins_the_same_server_as_the_same_player)
{
    start((tide_net_conditions){.latency = 0.01}, 5);
    tide_session *s = tide_session_create(&(tide_session_desc){.game = &tide_game_api, .tick_rate = 60, .sample = sample, .user = &who[0]});
    join(1); // Someone else is there first: player 0
    run(0.5);
    for (int round = 0; round < 2; round++) {
        tide_session_join(s, tide_loopback_endpoint(net, 10u + (uint32_t)round), tide_loopback_address(1), now);
        const double until = now + 1.0;
        while (now < until) {
            now += 1.0 / 60.0;
            tide_loopback_set_time(net, now);
            tide_session_update(s, now);
            tide_client_update(clients[1], now);
            tide_server_update(server, now);
        }
        TIDE_CHECK(tide_session_status_of(s).client.player.id == 2u); // Player 1, both times
        tide_session_leave(s);
    }
    tide_session_destroy(s);
    finish();
}

TIDE_TEST(net_the_server_turns_away_other_games)
{
    start((tide_net_conditions){0}, 1);
    tide_game other = tide_game_api;
    other.hash ^= 1u;
    const tide_client_desc desc = {.game = &other, .transport = tide_loopback_endpoint(net, 2), .server = tide_loopback_address(1)};
    clients[0] = tide_client_create(&desc, now);
    run(0.5);
    const tide_client_status s = tide_client_status_of(clients[0]);
    TIDE_CHECK(s.state == TIDE_SESSION_OFFLINE && s.reason == TIDE_DISCONNECT_REFUSED);
    finish();
}

TIDE_TEST(net_players_hear_when_the_server_leaves)
{
    start((tide_net_conditions){.latency = 0.02}, 1);
    join(0);
    run(1.0);
    tide_server_destroy(server);
    server = tide_server_create(&(tide_server_desc){.game = &tide_game_api, .tick_rate = 60}, now); // One with no network
    run(1.5);
    const tide_client_status s = tide_client_status_of(clients[0]);
    TIDE_CHECK(s.state == TIDE_SESSION_OFFLINE && s.reason == TIDE_DISCONNECT_SERVER_LEFT);
    finish();
}

TIDE_TEST(net_nobody_answers)
{
    start((tide_net_conditions){0}, 1);
    const tide_client_desc desc = {.game = &tide_game_api, .transport = tide_loopback_endpoint(net, 2),
                                   .server = tide_loopback_address(9)};
    clients[0] = tide_client_create(&desc, now);
    run(6.0);
    const tide_client_status s = tide_client_status_of(clients[0]);
    TIDE_CHECK(s.state == TIDE_SESSION_OFFLINE && s.reason == TIDE_DISCONNECT_TIMED_OUT);
    finish();
}

// ---------------------------------------------------------------------------
// A session: this machine's own server and player, as a host program runs it.

static tide_local local;
static tide_draw_list draw;
static tide_gui gui;

static void session_sample(void *user, const uint32_t tick, void *input)
{
    (void)user;
    sample(&who[0], tick, input);
}

TIDE_TEST(net_local_code_starts_a_match)
{
    tide_local_init(&local);
    tide_session *s = tide_session_create(&(tide_session_desc){.game = &tide_game_api, .tick_rate = 60, .sample = session_sample});
    double t = 0.0;
    tide_devices devices = {0};

    // A view asks for a match in Arena.
    local.Menu.play = true;
    tide_draw_reset(&draw);
    tide_gui_begin(&gui, &devices, tide_f2(1920.0f, 1080.0f), NULL);
    tide_frame(NULL, NULL, 1.0f, &local, &draw, &gui);
    tide_gui_end(&gui, &draw);
    tide_session_request request;
    tide_start start;
    TIDE_REQUIRE(tide_local_take_request(&local, &request, &start));
    TIDE_CHECK(request.kind == TIDE_REQUEST_PLAY);
    TIDE_CHECK(start.scene == 0 && start.value.Arena.size == 3);
    TIDE_CHECK(!tide_local_take_request(&local, &request, &start)); // Taken

    tide_session_play(s, &start, t);
    bool connected = false;
    for (int frame = 0; frame < 120; frame++) {
        t += 1.0 / 60.0;
        tide_session_update(s, t);
        tide_session_event e;
        while (tide_session_next_event(s, &e)) connected |= e.kind == TIDE_SESSION_CONNECTED_EVENT;
    }
    TIDE_CHECK(connected);
    const tide_session_status status = tide_session_status_of(s);
    TIDE_CHECK(status.server && status.client.state == TIDE_SESSION_CONNECTED);
    TIDE_CHECK(status.client.player.id == 1u);
    // On one machine the server ticks right after this player's input: nothing to predict
    TIDE_CHECK(status.client.predicted_tick == status.client.verified_tick);
    TIDE_CHECK(status.client.verified_tick > 100u);
    const tide_world *w = tide_session_world(s);
    TIDE_REQUIRE(w != NULL);
    TIDE_CHECK(w->Players.joined == 1);
    TIDE_CHECK(memcmp(w, tide_session_server_world(s), sizeof *w) == 0);

    // The match started in Arena, not Main.
    uint32_t arenas = 0;
    uint32_t mains = 0;
    for (uint32_t i = 0; i < w->entities.next_unused; i++) {
        const tide_entity e = {i, w->entities.slots[i].generation};
        arenas += tide_get_Arena((tide_world *)w, e) != NULL;
        mains += tide_get_Main((tide_world *)w, e) != NULL;
    }
    TIDE_CHECK(arenas == 1 && mains == 0);

    tide_session_leave(s);
    tide_session_event e;
    TIDE_REQUIRE(tide_session_next_event(s, &e));
    TIDE_CHECK(e.kind == TIDE_SESSION_DISCONNECTED_EVENT && e.reason == TIDE_DISCONNECT_LEFT);
    TIDE_CHECK(tide_session_world(s) == NULL);
    tide_session_destroy(s);
}

static void run_views(void)
{
    tide_devices devices = {0};
    tide_draw_reset(&draw);
    tide_gui_begin(&gui, &devices, tide_f2(1920.0f, 1080.0f), NULL);
    tide_frame(NULL, NULL, 1.0f, &local, &draw, &gui);
    tide_gui_end(&gui, &draw);
}

// Session.Join takes a room's code, Session.Connect an address, and local
// code reads the room the match is in from Session.room.
TIDE_TEST(net_local_code_joins_rooms_and_connects_to_addresses)
{
    tide_local_init(&local);
    tide_local_set_session(&local, TIDE_SESSION_CONNECTED, (tide_player_id){1}, 20, true, "K7QF2M");
    run_views();
    TIDE_CHECK(local.Menu.inRoom && local.Menu.roomLength == 6);
    tide_local_set_session(&local, TIDE_SESSION_OFFLINE, (tide_player_id){0}, 0, false, "");
    run_views();
    TIDE_CHECK(!local.Menu.inRoom && local.Menu.roomLength == 0);

    tide_session_request request;
    tide_start start;
    local.Menu.join = true;
    run_views();
    TIDE_REQUIRE(tide_local_take_request(&local, &request, &start));
    TIDE_CHECK(request.kind == TIDE_REQUEST_JOIN && strcmp(request.address, "k7qf2m") == 0);

    local.Menu.connect = true;
    run_views();
    TIDE_REQUIRE(tide_local_take_request(&local, &request, &start));
    TIDE_CHECK(request.kind == TIDE_REQUEST_CONNECT && strcmp(request.address, "192.168.1.5") == 0);
    TIDE_CHECK(request.port == 7000u);
}

// This machine stops for a while (a browser tab in the background, a
// breakpoint), and its server with it: for the match, that time didn't pass.
TIDE_TEST(net_a_session_survives_a_pause)
{
    tide_session *s = tide_session_create(&(tide_session_desc){.game = &tide_game_api, .tick_rate = 60, .sample = session_sample});
    double t = 0.0;
    tide_session_play(s, NULL, t);
    for (int frame = 0; frame < 60; frame++) {
        t += 1.0 / 60.0;
        tide_session_update(s, t);
    }
    tide_session_event e;
    while (tide_session_next_event(s, &e)) {}
    const tide_session_status before = tide_session_status_of(s);
    TIDE_REQUIRE(before.client.state == TIDE_SESSION_CONNECTED);

    t += 10.0; // Twice the timeout
    for (int frame = 0; frame < 60; frame++) {
        t += 1.0 / 60.0;
        tide_session_update(s, t);
        const tide_session_status status = tide_session_status_of(s);
        TIDE_CHECK(status.client.predicted_tick == status.client.verified_tick); // Nothing to catch up
    }
    bool disconnected = false;
    while (tide_session_next_event(s, &e)) disconnected |= e.kind == TIDE_SESSION_DISCONNECTED_EVENT;
    TIDE_CHECK(!disconnected);
    const tide_session_status after = tide_session_status_of(s);
    TIDE_CHECK(after.client.state == TIDE_SESSION_CONNECTED);
    // A second of play, and at most one update's ticks for the pause
    const uint32_t ticks = after.client.verified_tick - before.client.verified_tick;
    TIDE_CHECK(ticks >= 60u && ticks <= 68u);
    TIDE_REQUIRE(tide_session_world(s) != NULL);
    TIDE_CHECK(memcmp(tide_session_world(s), tide_session_server_world(s), sizeof(tide_world)) == 0);
    tide_session_destroy(s);
}

// Hot reloading (tide/host.h): another build of the same game, new code with
// the same layout, takes over the match where it is.
static int new_build_ticks;

static void new_build_tick(void *w)
{
    new_build_ticks++;
    tide_game_api.tick(w);
}

TIDE_TEST(net_a_session_takes_a_new_build_of_its_game)
{
    tide_game new_build = tide_game_api;
    new_build.tick = new_build_tick;
    tide_session *s = tide_session_create(&(tide_session_desc){.game = &tide_game_api, .tick_rate = 60, .sample = session_sample});
    double t = 0.0;
    tide_session_play(s, NULL, t);
    for (int frame = 0; frame < 60; frame++) {
        t += 1.0 / 60.0;
        tide_session_update(s, t);
    }
    const tide_session_status before = tide_session_status_of(s);
    TIDE_REQUIRE(before.client.state == TIDE_SESSION_CONNECTED);

    new_build_ticks = 0;
    tide_session_set_game(s, &new_build);
    for (int frame = 0; frame < 60; frame++) {
        t += 1.0 / 60.0;
        tide_session_update(s, t);
    }
    const tide_session_status after = tide_session_status_of(s);
    TIDE_CHECK(after.client.state == TIDE_SESSION_CONNECTED);
    TIDE_CHECK(after.client.resyncs == 0);
    TIDE_CHECK(after.client.verified_tick >= before.client.verified_tick + 55u); // It went on from where it was
    // Both the server and this machine's client run the new build's ticks
    TIDE_CHECK(new_build_ticks >= 2 * (int)(after.client.verified_tick - before.client.verified_tick));
    TIDE_REQUIRE(tide_session_world(s) != NULL);
    TIDE_CHECK(memcmp(tide_session_world(s), tide_session_server_world(s), sizeof(tide_world)) == 0);
    tide_session_event e;
    bool disconnected = false;
    while (tide_session_next_event(s, &e)) disconnected |= e.kind == TIDE_SESSION_DISCONNECTED_EVENT;
    TIDE_CHECK(!disconnected);
    tide_session_destroy(s);
}

// A build with another data layout, carried over (tide_session_migrate): here
// the same game under another hash, carried over by copying, so the match
// must go on exactly as it would have.
static int migrations;

static bool copy_over(void *user, const void *from, void *to)
{
    (void)user;
    migrations++;
    tide_world_copy(to, from);
    return true;
}

TIDE_TEST(net_a_session_carries_its_match_over_to_another_layout)
{
    tide_game new_build = tide_game_api;
    new_build.hash ^= 1u;
    tide_session *s = tide_session_create(&(tide_session_desc){.game = &tide_game_api, .tick_rate = 60, .sample = session_sample});
    double t = 0.0;
    tide_session_play(s, NULL, t);
    for (int frame = 0; frame < 60; frame++) {
        t += 1.0 / 60.0;
        tide_session_update(s, t);
    }
    const tide_session_status before = tide_session_status_of(s);
    TIDE_REQUIRE(before.client.state == TIDE_SESSION_CONNECTED);

    migrations = 0;
    TIDE_REQUIRE(tide_session_migrate(s, &new_build, copy_over, NULL));
    TIDE_CHECK(migrations == 1); // The server's world: this machine's player takes it
    for (int frame = 0; frame < 60; frame++) {
        t += 1.0 / 60.0;
        tide_session_update(s, t);
    }
    const tide_session_status after = tide_session_status_of(s);
    TIDE_CHECK(after.client.state == TIDE_SESSION_CONNECTED);
    TIDE_CHECK(after.client.resyncs == 0);
    TIDE_CHECK(after.client.verified_tick >= before.client.verified_tick + 55u);
    TIDE_CHECK(memcmp(tide_session_world(s), tide_session_server_world(s), sizeof(tide_world)) == 0);
    tide_session_destroy(s);
}

static void guest_sample(void *user, const uint32_t tick, void *input)
{
    (void)user;
    sample(&who[1], tick, input);
}

// Two machines' sessions on one network, until `until`.
static void run_sessions(tide_loopback *network, tide_session *a, tide_session *b, double *t, const double until)
{
    while (*t < until) {
        *t += 1.0 / 60.0;
        tide_loopback_set_time(network, *t);
        tide_session_update(a, *t);
        tide_session_update(b, *t);
    }
}

// A server and a player on another machine: the server's machine reloads
// first, and the other one a moment later.
TIDE_TEST(net_a_client_carries_its_match_over_to_another_layout)
{
    tide_game new_build = tide_game_api;
    new_build.hash ^= 1u;
    tide_loopback *network = tide_loopback_create(4242);
    tide_loopback_set_conditions(network, (tide_net_conditions){.latency = 0.03, .jitter = 0.01});
    tide_session *host = tide_session_create(&(tide_session_desc){.game = &tide_game_api, .tick_rate = 60, .sample = session_sample});
    tide_session *guest = tide_session_create(&(tide_session_desc){.game = &tide_game_api, .tick_rate = 60, .sample = guest_sample});
    double t = 0.0;
    tide_session_host(host, NULL, tide_loopback_endpoint(network, 1), t);
    tide_session_join(guest, tide_loopback_endpoint(network, 2), tide_loopback_address(1), t);
    run_sessions(network, host, guest, &t, 2.0);
    const tide_session_status before = tide_session_status_of(guest);
    TIDE_REQUIRE(before.client.state == TIDE_SESSION_CONNECTED);

    migrations = 0;
    TIDE_REQUIRE(tide_session_migrate(host, &new_build, copy_over, NULL));
    run_sessions(network, host, guest, &t, 2.3);
    TIDE_REQUIRE(tide_session_migrate(guest, &new_build, copy_over, NULL));
    TIDE_CHECK(migrations == 2); // The server's world, and the other machine's verified one
    run_sessions(network, host, guest, &t, 4.5);

    const tide_session_status after = tide_session_status_of(guest);
    TIDE_CHECK(after.client.state == TIDE_SESSION_CONNECTED);
    TIDE_CHECK(after.client.player.id == before.client.player.id); // Still the same player
    TIDE_CHECK(after.client.resyncs == 0);
    TIDE_CHECK(after.client.verified_tick >= before.client.verified_tick + 120u);
    TIDE_CHECK(tide_session_status_of(host).client.resyncs == 0);
    const Players *players = players_in(tide_session_server_world(host));
    TIDE_CHECK(players->joined == 2 && players->left == 0); // Nobody left and came back
    tide_session_destroy(guest);
    tide_session_destroy(host);
    tide_loopback_destroy(network);
}

// Where each build is a program of its own (the web), the next one's session
// goes on from the world the last one left, with this machine's player in it
// already.
TIDE_TEST(net_a_session_goes_on_from_a_world)
{
    tide_session *s = tide_session_create(&(tide_session_desc){.game = &tide_game_api, .tick_rate = 60, .sample = session_sample});
    double t = 0.0;
    tide_session_play(s, NULL, t);
    for (int frame = 0; frame < 60; frame++) {
        t += 1.0 / 60.0;
        tide_session_update(s, t);
    }
    const tide_session_status before = tide_session_status_of(s);
    TIDE_REQUIRE(before.client.state == TIDE_SESSION_CONNECTED);
    tide_world *left = calloc(1, sizeof *left);
    tide_world_copy(left, tide_session_server_world(s));
    const int32_t player = tide_player_index(before.client.player);
    TIDE_REQUIRE(player >= 0);
    TIDE_REQUIRE(players_in(left)->joined == 1);
    tide_session_destroy(s);

    tide_session *next = tide_session_create(&(tide_session_desc){.game = &tide_game_api, .tick_rate = 60, .sample = session_sample});
    t = 0.0;
    tide_session_play_from(next, left, 1u << player, t);
    for (int frame = 0; frame < 60; frame++) {
        t += 1.0 / 60.0;
        tide_session_update(next, t);
    }
    const tide_session_status after = tide_session_status_of(next);
    TIDE_CHECK(after.client.state == TIDE_SESSION_CONNECTED);
    TIDE_CHECK(after.client.player.id == before.client.player.id);
    const tide_world *w = tide_session_server_world(next);
    TIDE_CHECK(players_in(w)->joined == 1); // Not twice
    TIDE_CHECK(w->Time.tick >= left->Time.tick + 55); // Went on from it
    TIDE_CHECK(memcmp(tide_session_world(next), w, sizeof(tide_world)) == 0);
    tide_session_destroy(next);
    free(left);
}

TIDE_TEST(net_input_packs_and_unpacks)
{
    const Controls in = {.move = {0.25f, -1.0f}, .jump = true};
    uint8_t bytes[16];
    const uint32_t n = tide_game_api.write_input(&in, bytes, sizeof bytes);
    TIDE_CHECK(n == tide_game_api.max_input_bytes);
    Controls out;
    TIDE_REQUIRE(tide_game_api.read_input(bytes, n, &out));
    TIDE_CHECK(memcmp(&in, &out, sizeof in) == 0);
    TIDE_CHECK(!tide_game_api.read_input(bytes, n - 1u, &out)); // Too short
}
