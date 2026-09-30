#include <stdio.h>
#include <string.h>

#include "game.h"
#include "purr_test.h"

// A server and players on one loopback network, in made-up time: frames of
// uneven length, and a network that loses, delays and reorders datagrams.
// Clients check every tick's hash against the server's, so a client that
// never had to ask for the world again kept the server's world exactly.

static purr_loopback *net;
static purr_server *server;
static purr_client *clients[3];
static int who[3] = {0, 1, 2};
static double now;

// Each player's input, from the tick: it changes every so often, as a player's does.
static void sample(void *user, const uint32_t tick, void *input)
{
    const int player = *(const int *)user;
    Controls *c = input;
    *c = (Controls){0};
    c->move = purr_f2((float)((tick / 20u + (uint32_t)player) % 3u) - 1.0f, (tick / 33u) % 2u ? 1.0f : 0.0f);
    c->jump = (tick + (uint32_t)player * 7u) % 45u < 5u;
}

static void start_at(const uint32_t tick_rate, const purr_net_conditions conditions, const uint64_t seed)
{
    now = 0.0;
    net = purr_loopback_create(seed);
    purr_loopback_set_conditions(net, conditions);
    const purr_server_desc desc = {.game = &purr_game_api, .tick_rate = tick_rate, .transports = {purr_loopback_endpoint(net, 1)}};
    server = purr_server_create(&desc, now);
    memset(clients, 0, sizeof clients);
}

static void start(const purr_net_conditions conditions, const uint64_t seed)
{
    start_at(60, conditions, seed);
}

static purr_client *join(const int number)
{
    const purr_client_desc desc = {
        .game = &purr_game_api,
        .transport = purr_loopback_endpoint(net, (uint32_t)number + 2u),
        .server = purr_loopback_address(1),
        .sample = sample,
        .user = &who[number],
        .lead = 2,
    };
    clients[number] = purr_client_create(&desc, now);
    return clients[number];
}

// Runs everything until `until`, in frames of 11 and 22 milliseconds.
static void run(const double until)
{
    bool odd = false;
    while (now < until) {
        now += (odd = !odd) ? 0.011 : 0.022;
        purr_loopback_set_time(net, now);
        for (int i = 0; i < 3; i++) {
            if (clients[i]) purr_client_update(clients[i], now);
        }
        purr_server_update(server, now);
    }
}

static void finish(void)
{
    for (int i = 0; i < 3; i++) purr_client_destroy(clients[i]);
    purr_server_destroy(server);
    purr_loopback_destroy(net);
}

static const Players *players_in(const void *world)
{
    return &((const purr_world *)world)->Players;
}

PURR_TEST(net_players_keep_the_servers_world)
{
    start((purr_net_conditions){.latency = 0.04, .jitter = 0.02, .loss = 0.1}, 12345);
    join(0);
    run(1.0);
    join(1); // Joins a match already on: gets the world, then plays on
    run(5.0);

    for (int i = 0; i < 2; i++) {
        const purr_client_status s = purr_client_status_of(clients[i]);
        PURR_CHECK(s.state == PURR_SESSION_CONNECTED);
        PURR_CHECK(s.resyncs == 0); // Every tick's hash matched the server's
        PURR_CHECK(s.player.id == (uint32_t)i + 1u);
        // Close behind the server, and a little ahead of it with prediction
        PURR_CHECK(s.verified_tick + 30u > purr_server_tick(server));
        PURR_CHECK(s.predicted_tick > s.verified_tick);
        PURR_CHECK(s.predicted_tick < purr_server_tick(server) + 30u);
    }
    PURR_CHECK(players_in(purr_server_world(server))->joined == 2);

    // One leaves; the other sees it, once the network is good again.
    purr_client_destroy(clients[0]);
    clients[0] = NULL;
    purr_loopback_set_conditions(net, (purr_net_conditions){0});
    run(7.0);
    PURR_CHECK(players_in(purr_server_world(server))->left == 1);
    PURR_CHECK(players_in(purr_client_verified_world(clients[1]))->left == 1);
    PURR_CHECK(purr_client_status_of(clients[1]).resyncs == 0);
    finish();
}

PURR_TEST(net_a_terrible_network)
{
    start((purr_net_conditions){.latency = 0.12, .jitter = 0.08, .loss = 0.3}, 99);
    join(0);
    join(1);
    run(8.0);
    for (int i = 0; i < 2; i++) {
        const purr_client_status s = purr_client_status_of(clients[i]);
        PURR_CHECK(s.state == PURR_SESSION_CONNECTED);
        PURR_CHECK(s.resyncs == 0);
        PURR_CHECK(s.verified_tick + 60u > purr_server_tick(server));
    }
    finish();
}

// How far clients predict, and what's kept to send again, follow the tick rate.
PURR_TEST(net_other_tick_rates)
{
    static const uint32_t rates[] = {20, 240};
    for (int i = 0; i < 2; i++) {
        start_at(rates[i], (purr_net_conditions){.latency = 0.08, .jitter = 0.04, .loss = 0.1}, 11);
        join(0);
        join(1);
        run(4.0);
        for (int k = 0; k < 2; k++) {
            const purr_client_status s = purr_client_status_of(clients[k]);
            PURR_CHECK(s.state == PURR_SESSION_CONNECTED);
            PURR_CHECK(s.resyncs == 0);
            PURR_CHECK(s.verified_tick + rates[i] / 2u > purr_server_tick(server)); // Within half a second
            PURR_CHECK(s.predicted_tick - s.verified_tick <= rates[i]);          // At most a second ahead
        }
        PURR_CHECK(purr_server_tick(server) + rates[i] / 10u > 4u * rates[i]);
        finish();
    }
}

PURR_TEST(net_a_client_that_diverges_gets_the_world_again)
{
    start((purr_net_conditions){.latency = 0.03}, 7);
    join(0);
    run(1.0);
    // Something went wrong on this machine: its world is no longer the server's, and
    // what it predicts from it isn't either.
    ((purr_world *)purr_client_world(clients[0]))->Players.joined = 50;
    run(3.0);
    const purr_client_status s = purr_client_status_of(clients[0]);
    PURR_CHECK(s.resyncs == 1);
    PURR_CHECK(s.state == PURR_SESSION_CONNECTED);
    PURR_CHECK(players_in(purr_client_verified_world(clients[0]))->joined == 1);
    finish();
}

// A player's cookie gets them their PlayerID back.
static purr_client *join_as(const int number, const uint64_t cookie)
{
    purr_client *c = join(number);
    purr_client_destroy(c);
    const purr_client_desc desc = {
        .game = &purr_game_api,
        .transport = purr_loopback_endpoint(net, (uint32_t)number + 2u),
        .server = purr_loopback_address(1),
        .sample = sample,
        .user = &who[number],
        .lead = 2,
        .cookie = cookie,
    };
    clients[number] = purr_client_create(&desc, now);
    return clients[number];
}

PURR_TEST(net_a_player_takes_over_from_a_connection_gone_quiet)
{
    start((purr_net_conditions){.latency = 0.02}, 3);
    join(0);
    run(1.0);
    const uint64_t cookie = purr_client_status_of(clients[0]).cookie;
    PURR_CHECK(cookie != 0);
    // The first connection goes quiet (its machine's address changed, say),
    // and the same player connects again before the server gives up on them.
    purr_client *quiet = clients[0];
    clients[0] = NULL;
    join_as(1, cookie);
    run(2.0);
    const purr_client_status s = purr_client_status_of(clients[1]);
    PURR_CHECK(s.state == PURR_SESSION_CONNECTED && s.player.id == 1u && s.cookie == cookie);
    const Players *players = players_in(purr_server_world(server));
    PURR_CHECK(players->joined == 1 && players->left == 0); // No events: nothing happened, as far as the game knows
    PURR_CHECK(purr_server_player_count(server) == 1);
    purr_client_destroy(quiet);
    finish();
}

PURR_TEST(net_a_player_who_left_comes_back_as_themselves)
{
    start((purr_net_conditions){.latency = 0.02}, 4);
    join(0);
    run(0.5);
    join(1);
    run(1.0);
    const uint64_t cookie = purr_client_status_of(clients[0]).cookie;
    purr_client_destroy(clients[0]); // Leaves
    clients[0] = NULL;
    run(1.5);
    PURR_CHECK(players_in(purr_server_world(server))->left == 1);

    join_as(2, cookie);
    run(2.5);
    const purr_client_status back = purr_client_status_of(clients[2]);
    PURR_CHECK(back.state == PURR_SESSION_CONNECTED && back.player.id == 1u); // Player 0 again
    PURR_CHECK(players_in(purr_server_world(server))->joined == 3); // PlayerJoined again, with the same PlayerID
    PURR_CHECK(purr_client_status_of(clients[1]).player.id == 2u);

    // Without the cookie, a new player.
    join_as(0, 0);
    run(3.5);
    PURR_CHECK(purr_client_status_of(clients[0]).player.id == 3u);
    finish();
}

PURR_TEST(net_a_session_joins_the_same_server_as_the_same_player)
{
    start((purr_net_conditions){.latency = 0.01}, 5);
    purr_session *s = purr_session_create(&(purr_session_desc){.game = &purr_game_api, .tick_rate = 60, .sample = sample, .user = &who[0]});
    join(1); // Someone else is there first: player 0
    run(0.5);
    for (int round = 0; round < 2; round++) {
        purr_session_join(s, purr_loopback_endpoint(net, 10u + (uint32_t)round), purr_loopback_address(1), now);
        const double until = now + 1.0;
        while (now < until) {
            now += 1.0 / 60.0;
            purr_loopback_set_time(net, now);
            purr_session_update(s, now);
            purr_client_update(clients[1], now);
            purr_server_update(server, now);
        }
        PURR_CHECK(purr_session_status_of(s).client.player.id == 2u); // Player 1, both times
        purr_session_leave(s);
    }
    purr_session_destroy(s);
    finish();
}

PURR_TEST(net_the_server_turns_away_other_games)
{
    start((purr_net_conditions){0}, 1);
    purr_game other = purr_game_api;
    other.hash ^= 1u;
    const purr_client_desc desc = {.game = &other, .transport = purr_loopback_endpoint(net, 2), .server = purr_loopback_address(1)};
    clients[0] = purr_client_create(&desc, now);
    run(0.5);
    const purr_client_status s = purr_client_status_of(clients[0]);
    PURR_CHECK(s.state == PURR_SESSION_OFFLINE && s.reason == PURR_DISCONNECT_REFUSED);
    finish();
}

PURR_TEST(net_players_hear_when_the_server_leaves)
{
    start((purr_net_conditions){.latency = 0.02}, 1);
    join(0);
    run(1.0);
    purr_server_destroy(server);
    server = purr_server_create(&(purr_server_desc){.game = &purr_game_api, .tick_rate = 60}, now); // One with no network
    run(1.5);
    const purr_client_status s = purr_client_status_of(clients[0]);
    PURR_CHECK(s.state == PURR_SESSION_OFFLINE && s.reason == PURR_DISCONNECT_SERVER_LEFT);
    finish();
}

PURR_TEST(net_nobody_answers)
{
    start((purr_net_conditions){0}, 1);
    const purr_client_desc desc = {.game = &purr_game_api, .transport = purr_loopback_endpoint(net, 2),
                                   .server = purr_loopback_address(9)};
    clients[0] = purr_client_create(&desc, now);
    run(6.0);
    const purr_client_status s = purr_client_status_of(clients[0]);
    PURR_CHECK(s.state == PURR_SESSION_OFFLINE && s.reason == PURR_DISCONNECT_TIMED_OUT);
    finish();
}

// ---------------------------------------------------------------------------
// A session: this machine's own server and player, as a host program runs it.

static purr_local local;
static purr_draw_list draw;
static purr_gui gui;

static void session_sample(void *user, const uint32_t tick, void *input)
{
    (void)user;
    sample(&who[0], tick, input);
}

PURR_TEST(net_local_code_starts_a_match)
{
    purr_local_init(&local);
    purr_session *s = purr_session_create(&(purr_session_desc){.game = &purr_game_api, .tick_rate = 60, .sample = session_sample});
    double t = 0.0;
    purr_devices devices = {0};

    // A view asks for a match in Arena.
    local.Menu.play = true;
    purr_draw_reset(&draw);
    purr_gui_begin(&gui, &devices, purr_f2(1920.0f, 1080.0f), NULL);
    purr_frame(NULL, NULL, 1.0f, &local, &draw, &gui);
    purr_gui_end(&gui, &draw);
    purr_session_request request;
    purr_start start;
    PURR_REQUIRE(purr_local_take_request(&local, &request, &start));
    PURR_CHECK(request.kind == PURR_REQUEST_PLAY);
    PURR_CHECK(start.scene == 0 && start.value.Arena.size == 3);
    PURR_CHECK(!purr_local_take_request(&local, &request, &start)); // Taken

    purr_session_play(s, &start, t);
    bool connected = false;
    for (int frame = 0; frame < 120; frame++) {
        t += 1.0 / 60.0;
        purr_session_update(s, t);
        purr_session_event e;
        while (purr_session_next_event(s, &e)) connected |= e.kind == PURR_SESSION_CONNECTED_EVENT;
    }
    PURR_CHECK(connected);
    const purr_session_status status = purr_session_status_of(s);
    PURR_CHECK(status.server && status.client.state == PURR_SESSION_CONNECTED);
    PURR_CHECK(status.client.player.id == 1u);
    // On one machine the server ticks right after this player's input: nothing to predict
    PURR_CHECK(status.client.predicted_tick == status.client.verified_tick);
    PURR_CHECK(status.client.verified_tick > 100u);
    const purr_world *w = purr_session_world(s);
    PURR_REQUIRE(w != NULL);
    PURR_CHECK(w->Players.joined == 1);
    PURR_CHECK(memcmp(w, purr_session_server_world(s), sizeof *w) == 0);

    // The match started in Arena, not Main.
    uint32_t arenas = 0;
    uint32_t mains = 0;
    for (uint32_t i = 0; i < w->entities.next_unused; i++) {
        const purr_entity e = {i, w->entities.slots[i].generation};
        arenas += purr_get_Arena((purr_world *)w, e) != NULL;
        mains += purr_get_Main((purr_world *)w, e) != NULL;
    }
    PURR_CHECK(arenas == 1 && mains == 0);

    purr_session_leave(s);
    purr_session_event e;
    PURR_REQUIRE(purr_session_next_event(s, &e));
    PURR_CHECK(e.kind == PURR_SESSION_DISCONNECTED_EVENT && e.reason == PURR_DISCONNECT_LEFT);
    PURR_CHECK(purr_session_world(s) == NULL);
    purr_session_destroy(s);
}

// This machine stops for a while (a browser tab in the background, a
// breakpoint), and its server with it: for the match, that time didn't pass.
PURR_TEST(net_a_session_survives_a_pause)
{
    purr_session *s = purr_session_create(&(purr_session_desc){.game = &purr_game_api, .tick_rate = 60, .sample = session_sample});
    double t = 0.0;
    purr_session_play(s, NULL, t);
    for (int frame = 0; frame < 60; frame++) {
        t += 1.0 / 60.0;
        purr_session_update(s, t);
    }
    purr_session_event e;
    while (purr_session_next_event(s, &e)) {}
    const purr_session_status before = purr_session_status_of(s);
    PURR_REQUIRE(before.client.state == PURR_SESSION_CONNECTED);

    t += 10.0; // Twice the timeout
    for (int frame = 0; frame < 60; frame++) {
        t += 1.0 / 60.0;
        purr_session_update(s, t);
        const purr_session_status status = purr_session_status_of(s);
        PURR_CHECK(status.client.predicted_tick == status.client.verified_tick); // Nothing to catch up
    }
    bool disconnected = false;
    while (purr_session_next_event(s, &e)) disconnected |= e.kind == PURR_SESSION_DISCONNECTED_EVENT;
    PURR_CHECK(!disconnected);
    const purr_session_status after = purr_session_status_of(s);
    PURR_CHECK(after.client.state == PURR_SESSION_CONNECTED);
    // A second of play, and at most one update's ticks for the pause
    const uint32_t ticks = after.client.verified_tick - before.client.verified_tick;
    PURR_CHECK(ticks >= 60u && ticks <= 68u);
    PURR_REQUIRE(purr_session_world(s) != NULL);
    PURR_CHECK(memcmp(purr_session_world(s), purr_session_server_world(s), sizeof(purr_world)) == 0);
    purr_session_destroy(s);
}

// Hot reloading (purr/host.h): another build of the same game, new code with
// the same layout, takes over the match where it is.
static int new_build_ticks;

static void new_build_tick(void *w)
{
    new_build_ticks++;
    purr_game_api.tick(w);
}

PURR_TEST(net_a_session_takes_a_new_build_of_its_game)
{
    purr_game new_build = purr_game_api;
    new_build.tick = new_build_tick;
    purr_session *s = purr_session_create(&(purr_session_desc){.game = &purr_game_api, .tick_rate = 60, .sample = session_sample});
    double t = 0.0;
    purr_session_play(s, NULL, t);
    for (int frame = 0; frame < 60; frame++) {
        t += 1.0 / 60.0;
        purr_session_update(s, t);
    }
    const purr_session_status before = purr_session_status_of(s);
    PURR_REQUIRE(before.client.state == PURR_SESSION_CONNECTED);

    new_build_ticks = 0;
    purr_session_set_game(s, &new_build);
    for (int frame = 0; frame < 60; frame++) {
        t += 1.0 / 60.0;
        purr_session_update(s, t);
    }
    const purr_session_status after = purr_session_status_of(s);
    PURR_CHECK(after.client.state == PURR_SESSION_CONNECTED);
    PURR_CHECK(after.client.resyncs == 0);
    PURR_CHECK(after.client.verified_tick >= before.client.verified_tick + 55u); // It went on from where it was
    // Both the server and this machine's client run the new build's ticks
    PURR_CHECK(new_build_ticks >= 2 * (int)(after.client.verified_tick - before.client.verified_tick));
    PURR_REQUIRE(purr_session_world(s) != NULL);
    PURR_CHECK(memcmp(purr_session_world(s), purr_session_server_world(s), sizeof(purr_world)) == 0);
    purr_session_event e;
    bool disconnected = false;
    while (purr_session_next_event(s, &e)) disconnected |= e.kind == PURR_SESSION_DISCONNECTED_EVENT;
    PURR_CHECK(!disconnected);
    purr_session_destroy(s);
}

PURR_TEST(net_input_packs_and_unpacks)
{
    const Controls in = {.move = {0.25f, -1.0f}, .jump = true};
    uint8_t bytes[16];
    const uint32_t n = purr_game_api.write_input(&in, bytes, sizeof bytes);
    PURR_CHECK(n == purr_game_api.max_input_bytes);
    Controls out;
    PURR_REQUIRE(purr_game_api.read_input(bytes, n, &out));
    PURR_CHECK(memcmp(&in, &out, sizeof in) == 0);
    PURR_CHECK(!purr_game_api.read_input(bytes, n - 1u, &out)); // Too short
}
