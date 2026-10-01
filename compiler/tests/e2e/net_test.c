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

// Whether two worlds hold the same: the same bytes (tide_world_pack, which
// leaves out where their pages are), and the same hash.
static bool same(const void *a, const void *b)
{
    const uint32_t size = tide_world_pack(a, NULL, 0);
    if (tide_world_pack(b, NULL, 0) != size) return false;
    uint8_t *x = malloc(size);
    uint8_t *y = malloc(size);
    tide_world_pack(a, x, size);
    tide_world_pack(b, y, size);
    const bool equal = memcmp(x, y, size) == 0;
    free(x);
    free(y);
    return equal && tide_world_hash(a) == tide_world_hash(b);
}

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

// A player whose machine stopped for longer than the server keeps ticks, but
// not so long that it timed out, gets the world again and plays on.
TIDE_TEST(net_a_player_far_behind_gets_the_world_again)
{
    start((tide_net_conditions){.latency = 0.03}, 21);
    join(0);
    join(1);
    run(1.0);
    tide_client *stopped = clients[1];
    clients[1] = NULL; // Not updated
    run(5.5);
    clients[1] = stopped;
    run(8.0);
    for (int i = 0; i < 2; i++) {
        const tide_client_status s = tide_client_status_of(clients[i]);
        TIDE_CHECK(s.state == TIDE_SESSION_CONNECTED);
        TIDE_CHECK(s.verified_tick + 30u > tide_server_tick(server));
    }
    TIDE_CHECK(tide_client_status_of(clients[0]).resyncs == 0);
    TIDE_CHECK(tide_client_status_of(clients[1]).resyncs == 1); // The world, once
    finish();
}

// What a match costs to keep going, through transports that count it: two
// players, 50 ms away, for ten seconds after joining.
static tide_transport counted_inner[3];
static uint64_t counted_bytes[3];

static void counted_send(void *self, const tide_address to, const void *data, const uint32_t size)
{
    const intptr_t i = (intptr_t)self;
    counted_bytes[i] += size;
    counted_inner[i].send(counted_inner[i].self, to, data, size);
}

static uint32_t counted_receive(void *self, tide_address *from, void *data, const uint32_t capacity)
{
    const intptr_t i = (intptr_t)self;
    return counted_inner[i].receive(counted_inner[i].self, from, data, capacity);
}

static void counted_close(void *self)
{
    const intptr_t i = (intptr_t)self;
    counted_inner[i].close(counted_inner[i].self);
}

static tide_transport counted(const intptr_t i, const uint32_t endpoint)
{
    counted_inner[i] = tide_loopback_endpoint(net, endpoint);
    return (tide_transport){(void *)i, counted_send, counted_receive, counted_close, NULL};
}

TIDE_TEST(net_a_match_costs_little_to_keep_going)
{
    now = 0.0;
    net = tide_loopback_create(3);
    tide_loopback_set_conditions(net, (tide_net_conditions){.latency = 0.05});
    const tide_server_desc desc = {.game = &tide_game_api, .tick_rate = 60, .transports = {counted(0, 1)}};
    server = tide_server_create(&desc, now);
    memset(clients, 0, sizeof clients);
    for (int i = 0; i < 2; i++) {
        const tide_client_desc client = {.game = &tide_game_api, .transport = counted(i + 1, (uint32_t)i + 2u),
                                         .server = tide_loopback_address(1), .sample = sample, .user = &who[i], .lead = 2};
        clients[i] = tide_client_create(&client, now);
    }
    run(1.0); // Joined, with the world
    memset(counted_bytes, 0, sizeof counted_bytes);
    run(11.0);
    for (int i = 0; i < 2; i++) TIDE_CHECK(tide_client_status_of(clients[i]).resyncs == 0);
    // About 6.5 KB a second to each player and 2.5 KB a second from each, at
    // 60 ticks a second, most of it the hashes of ticks sent again until a
    // round trip says they came: 316 KB and 45 KB before the frames shrank.
    TIDE_CHECK(counted_bytes[0] < 140000u);
    TIDE_CHECK(counted_bytes[1] < 28000u && counted_bytes[2] < 28000u);
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
    TIDE_CHECK(request.kind == TIDE_REQUEST_START);
    TIDE_CHECK(start.scene == 0 && start.value.Arena.size == 3);
    TIDE_CHECK(!tide_local_take_request(&local, &request, &start)); // Taken

    tide_session_start(s, &start, t);
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
    TIDE_CHECK(!status.open); // No one else joins until it's opened
    TIDE_CHECK(status.client.player.id == 1u);
    // On one machine the server ticks right after this player's input: nothing to predict
    TIDE_CHECK(status.client.predicted_tick == status.client.verified_tick);
    TIDE_CHECK(status.client.verified_tick > 100u);
    const tide_world *w = tide_session_world(s);
    TIDE_REQUIRE(w != NULL);
    TIDE_CHECK(w->Players.joined == 1);
    TIDE_CHECK(same(w, tide_session_server_world(s)));

    // The match started in Arena, not Main.
    uint32_t arenas = 0;
    uint32_t mains = 0;
    for (uint32_t i = 0; i < w->entities.next_unused; i++) {
        const tide_entity e = tide_entity_in_slot(&w->entities, i);
        arenas += tide_read_Arena(w, e) != NULL;
        mains += tide_read_Main(w, e) != NULL;
    }
    TIDE_CHECK(arenas == 1 && mains == 0);

    tide_session_leave(s);
    tide_session_event e;
    TIDE_REQUIRE(tide_session_next_event(s, &e));
    TIDE_CHECK(e.kind == TIDE_SESSION_DISCONNECTED_EVENT && e.reason == TIDE_DISCONNECT_LEFT);
    TIDE_CHECK(tide_session_world(s) == NULL);
    tide_session_destroy(s);
}

// Whether the local text `t` is `expected`.
static bool shows(const tide_text t, const char *expected)
{
    const tide_str s = tide_text_read(&local.heap, t);
    return s.bytes == (int32_t)strlen(expected) && memcmp(s.ptr, expected, (size_t)s.bytes) == 0;
}

static void run_views(void)
{
    tide_devices devices = {0};
    tide_draw_reset(&draw);
    tide_gui_begin(&gui, &devices, tide_f2(1920.0f, 1080.0f), NULL);
    tide_frame(NULL, NULL, 1.0f, &local, &draw, &gui);
    tide_gui_end(&gui, &draw);
}

// Session.Join takes a room's code, Session.Connect an address, Session.Open
// a port, and local code reads the room the match is in from Session.room.
TIDE_TEST(net_local_code_joins_rooms_and_connects_to_addresses)
{
    tide_local_init(&local);
    tide_local_set_session(&local, TIDE_SESSION_CONNECTED, (tide_player_id){1}, 20, true, true, "K7QF2M");
    run_views();
    TIDE_CHECK(local.Menu.inRoom && local.Menu.roomLength == 6);
    TIDE_CHECK(shows(local.Menu.shown, "Session { state = Connected, player = PlayerID(0), ping = 20, server = true, "
                                       "open = true, room = \"K7QF2M\" }"));
    tide_local_set_session(&local, TIDE_SESSION_OFFLINE, (tide_player_id){0}, 0, false, false, "");
    run_views();
    TIDE_CHECK(!local.Menu.inRoom && local.Menu.roomLength == 0);
    TIDE_CHECK(shows(local.Menu.shown, "Session { state = Offline, player = PlayerID(none), ping = 0, server = false, "
                                       "open = false, room = \"\" }"));

    tide_session_request request;
    tide_start start;
    local.Menu.join = true;
    run_views();
    TIDE_REQUIRE(tide_local_take_request(&local, &request, &start));
    TIDE_CHECK(request.kind == TIDE_REQUEST_JOIN && strcmp(request.text, "k7qf2m") == 0);

    local.Menu.connect = true;
    run_views();
    TIDE_REQUIRE(tide_local_take_request(&local, &request, &start));
    TIDE_CHECK(request.kind == TIDE_REQUEST_CONNECT && strcmp(request.text, "192.168.1.5") == 0);
    TIDE_CHECK(request.port == 7000u);

    // Start, then Open, in one frame: the host takes them in that order. The
    // Open before Start was for the match Start leaves.
    local.Menu.host = true;
    run_views();
    TIDE_REQUIRE(tide_local_take_request(&local, &request, &start));
    TIDE_CHECK(request.kind == TIDE_REQUEST_START && start.value.Arena.size == 4);
    TIDE_REQUIRE(tide_local_take_request(&local, &request, &start));
    TIDE_CHECK(request.kind == TIDE_REQUEST_OPEN && request.port == 7000u);
    TIDE_CHECK(!tide_local_take_request(&local, &request, &start));

    local.Menu.close = true;
    run_views();
    TIDE_REQUIRE(tide_local_take_request(&local, &request, &start));
    TIDE_CHECK(request.kind == TIDE_REQUEST_CLOSE);
    TIDE_CHECK(!tide_local_take_request(&local, &request, &start));

    // Kicks, in order, with their messages.
    local.Menu.kick = true;
    run_views();
    TIDE_REQUIRE(tide_local_take_request(&local, &request, &start));
    TIDE_CHECK(request.kind == TIDE_REQUEST_KICK && request.player.id == 2u && strcmp(request.text, "Be nice") == 0);
    TIDE_REQUIRE(tide_local_take_request(&local, &request, &start));
    TIDE_CHECK(request.kind == TIDE_REQUEST_KICK_ALL && request.text[0] == 0);
    TIDE_CHECK(!tide_local_take_request(&local, &request, &start));

    // Disconnected has a kick's message.
    tide_local_disconnected(&local, TIDE_DISCONNECT_KICKED, "Be nice");
    run_views();
    TIDE_CHECK(shows(local.Menu.gone, "Disconnected { reason = Kicked, message = \"Be nice\" }"));
    TIDE_CHECK(shows(local.Menu.why, "Be nice"));
    tide_local_disconnected(&local, TIDE_DISCONNECT_LEFT, NULL);
    run_views();
    TIDE_CHECK(shows(local.Menu.gone, "Disconnected { reason = Left, message = \"\" }"));
}

// This machine stops for a while (a browser tab in the background, a
// breakpoint), and its server with it: for the match, that time didn't pass.
TIDE_TEST(net_a_session_survives_a_pause)
{
    tide_session *s = tide_session_create(&(tide_session_desc){.game = &tide_game_api, .tick_rate = 60, .sample = session_sample});
    double t = 0.0;
    tide_session_start(s, NULL, t);
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
    TIDE_CHECK(same(tide_session_world(s), tide_session_server_world(s)));
    tide_session_destroy(s);
}

// A host whose window nobody sees updates its session only when the match is
// due its next tick (tide_session_until_tick): each update runs one, at any
// rate, though hosts add up their time in floats.
TIDE_TEST(net_a_session_says_when_it_next_ticks)
{
    static const uint32_t rates[] = {1, 30, 60, 144, 1000};
    for (size_t i = 0; i < sizeof rates / sizeof rates[0]; i++) {
        tide_session *s = tide_session_create(&(tide_session_desc){.game = &tide_game_api, .tick_rate = rates[i], .sample = session_sample});
        const double tick = 1.0 / (double)rates[i];
        TIDE_CHECK(tide_session_until_tick(s) == tick); // No match yet: a tick at the rate one would start at
        double t = 1000.0;
        tide_session_start(s, NULL, t);
        for (int frame = 0; frame < 10 && tide_session_status_of(s).client.state != TIDE_SESSION_CONNECTED; frame++) {
            t += (double)(float)tide_session_until_tick(s);
            tide_session_update(s, t);
        }
        TIDE_REQUIRE(tide_session_status_of(s).client.state == TIDE_SESSION_CONNECTED);
        int32_t last = ((const tide_world *)tide_session_server_world(s))->Time.tick;
        bool every_one = true;
        for (int frame = 0; frame < 200; frame++) {
            const double until = tide_session_until_tick(s);
            TIDE_CHECK(until > 0.0 && until <= tick * (1.0 + 1e-6)); // A hair over when floats landed a hair before
            t += (double)(float)until; // A frame's seconds, as the platform gives them
            tide_session_update(s, t);
            const int32_t ticked = ((const tide_world *)tide_session_server_world(s))->Time.tick;
            every_one &= ticked == last + 1;
            last = ticked;
        }
        TIDE_CHECK(every_one);
        TIDE_CHECK(tide_session_status_of(s).client.verified_tick == (uint32_t)last); // Its player kept up
        tide_session_destroy(s);
    }

    // A player in another machine's match goes by its own clock, at the
    // match's rate rather than its own desc's, and keeps up updating once a tick.
    start_at(30, (tide_net_conditions){.latency = 0.03, .jitter = 0.01}, 5);
    tide_session *s = tide_session_create(&(tide_session_desc){.game = &tide_game_api, .tick_rate = 60, .sample = session_sample});
    tide_session_join(s, tide_loopback_endpoint(net, 2), tide_loopback_address(1), now);
    double session_at = now;
    double server_at = now;
    uint32_t updates = 0;
    uint32_t predicted = 0;
    while (now < 4.0) {
        now = session_at < server_at ? session_at : server_at;
        tide_loopback_set_time(net, now);
        if (now == server_at) {
            tide_server_update(server, now);
            server_at += 0.016;
        }
        if (now == session_at) {
            tide_session_update(s, now);
            session_at = now + (double)(float)tide_session_until_tick(s);
            if (now >= 2.0 && !updates++) predicted = tide_session_status_of(s).client.predicted_tick;
        }
    }
    const tide_client_status status = tide_session_status_of(s).client;
    TIDE_CHECK(status.state == TIDE_SESSION_CONNECTED && status.resyncs == 0);
    TIDE_CHECK(updates >= 57u && updates <= 63u); // Two seconds at 30 ticks a second
    TIDE_CHECK(status.predicted_tick - predicted >= 56u && status.predicted_tick - predicted <= 64u);
    TIDE_CHECK(status.predicted_tick > tide_server_tick(server)); // Ahead of the server
    tide_session_destroy(s);
    finish();
}

// Hot reloading (tide/host.h): another build of the same game, new code with
// the same layout, takes over the match where it is.
static int new_build_ticks;

static void new_build_tick(void *w, const tide_jobs *jobs)
{
    new_build_ticks++;
    tide_game_api.tick(w, jobs);
}

TIDE_TEST(net_a_session_takes_a_new_build_of_its_game)
{
    tide_game new_build = tide_game_api;
    new_build.tick = new_build_tick;
    tide_session *s = tide_session_create(&(tide_session_desc){.game = &tide_game_api, .tick_rate = 60, .sample = session_sample});
    double t = 0.0;
    tide_session_start(s, NULL, t);
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
    TIDE_CHECK(same(tide_session_world(s), tide_session_server_world(s)));
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
    tide_session_start(s, NULL, t);
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
    TIDE_CHECK(same(tide_session_world(s), tide_session_server_world(s)));
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

// Whether `s` is in a match, and the reason it last went offline otherwise.
static bool in_match(tide_session *s, tide_disconnect_reason *reason)
{
    tide_session_event e;
    while (tide_session_next_event(s, &e)) {
        if (e.kind == TIDE_SESSION_DISCONNECTED_EVENT) *reason = e.reason;
    }
    return tide_session_status_of(s).client.state == TIDE_SESSION_CONNECTED;
}

// Three machines' sessions on one network, until `until`.
static void run_three(tide_loopback *network, tide_session *const s[3], double *t, const double until)
{
    while (*t < until) {
        *t += 1.0 / 60.0;
        tide_loopback_set_time(network, *t);
        for (int i = 0; i < 3; i++) tide_session_update(s[i], *t);
    }
}

// Other machines join a match once it's opened. Closed, it takes no one new,
// and the players in it stay; opened again, it takes them on the network it had.
TIDE_TEST(net_a_match_takes_players_while_it_is_open)
{
    tide_loopback *network = tide_loopback_create(77);
    tide_session *host = tide_session_create(&(tide_session_desc){.game = &tide_game_api, .tick_rate = 60, .sample = session_sample});
    tide_session *guest = tide_session_create(&(tide_session_desc){.game = &tide_game_api, .tick_rate = 60, .sample = guest_sample});
    tide_session *late = tide_session_create(&(tide_session_desc){.game = &tide_game_api, .tick_rate = 60, .sample = guest_sample});
    tide_session *const all[3] = {host, guest, late};
    tide_disconnect_reason reason = TIDE_DISCONNECT_LEFT;
    double t = 0.0;
    TIDE_CHECK(!tide_session_open(host, (tide_transport){0})); // No match to open
    tide_session_start(host, NULL, t);
    TIDE_CHECK(!tide_session_open(host, (tide_transport){0})); // No network to open it on yet
    TIDE_CHECK(!tide_session_status_of(host).open);

    TIDE_REQUIRE(tide_session_open(host, tide_loopback_endpoint(network, 1)));
    TIDE_CHECK(tide_session_status_of(host).open);
    tide_session_join(guest, tide_loopback_endpoint(network, 2), tide_loopback_address(1), t);
    run_three(network, all, &t, 1.0);
    TIDE_CHECK(in_match(guest, &reason));

    tide_session_close(host);
    TIDE_CHECK(!tide_session_status_of(host).open);
    tide_session_join(late, tide_loopback_endpoint(network, 3), tide_loopback_address(1), t);
    run_three(network, all, &t, 1.5);
    TIDE_CHECK(!in_match(late, &reason) && reason == TIDE_DISCONNECT_REFUSED);
    TIDE_CHECK(in_match(guest, &reason)); // Still in
    TIDE_CHECK(players_in(tide_session_server_world(host))->joined == 2);

    TIDE_CHECK(tide_session_open(host, (tide_transport){0})); // Where it was
    tide_session_join(late, tide_loopback_endpoint(network, 3), tide_loopback_address(1), t);
    run_three(network, all, &t, 2.5);
    TIDE_CHECK(in_match(late, &reason));
    TIDE_CHECK(players_in(tide_session_server_world(host))->joined == 3);
    TIDE_CHECK(!tide_session_open(guest, (tide_transport){0})); // Only the server's machine opens its match

    tide_session_destroy(late);
    tide_session_destroy(guest);
    tide_session_destroy(host);
    tide_loopback_destroy(network);
}

// Whether `s` is in a match; `gone` gets the last Disconnected it reported.
static bool still_in(tide_session *s, tide_session_event *gone)
{
    tide_session_event e;
    while (tide_session_next_event(s, &e)) {
        if (e.kind == TIDE_SESSION_DISCONNECTED_EVENT) *gone = e;
    }
    return tide_session_status_of(s).client.state == TIDE_SESSION_CONNECTED;
}

// The server's machine sends players away, and they hear why even when the
// network loses packets. A kick isn't a ban: they can join again, as the same
// player, while the match is open.
TIDE_TEST(net_a_match_kicks_players)
{
    tide_loopback *network = tide_loopback_create(78);
    tide_loopback_set_conditions(network, (tide_net_conditions){.latency = 0.03, .loss = 0.2});
    tide_session *host = tide_session_create(&(tide_session_desc){.game = &tide_game_api, .tick_rate = 60, .sample = session_sample});
    tide_session *guest = tide_session_create(&(tide_session_desc){.game = &tide_game_api, .tick_rate = 60, .sample = guest_sample});
    tide_session *late = tide_session_create(&(tide_session_desc){.game = &tide_game_api, .tick_rate = 60, .sample = guest_sample});
    tide_session *const all[3] = {host, guest, late};
    tide_session_event gone = {0};
    double t = 0.0;
    tide_session_start(host, NULL, t);
    TIDE_REQUIRE(tide_session_open(host, tide_loopback_endpoint(network, 1)));
    tide_session_join(guest, tide_loopback_endpoint(network, 2), tide_loopback_address(1), t);
    tide_session_join(late, tide_loopback_endpoint(network, 3), tide_loopback_address(1), t);
    run_three(network, all, &t, 2.0);
    TIDE_REQUIRE(still_in(guest, &gone) && still_in(late, &gone));
    const tide_player_id kicked = tide_session_status_of(guest).client.player;

    // Its own player can't be kicked: it leaves instead.
    tide_session_kick(host, tide_session_status_of(host).client.player, "No");

    // 200 é's are 400 bytes: the message is cut to 127 of them.
    char message[401] = {0};
    for (int i = 0; i < 200; i++) memcpy(message + 2 * i, "\xC3\xA9", 2);
    tide_session_kick(host, kicked, message);
    run_three(network, all, &t, 4.0);
    TIDE_CHECK(!still_in(guest, &gone) && gone.reason == TIDE_DISCONNECT_KICKED);
    TIDE_CHECK(strlen(gone.message) == 254 && memcmp(gone.message, message, 254) == 0);
    TIDE_CHECK(still_in(late, &gone) && still_in(host, &gone));
    const Players *players = players_in(tide_session_server_world(host));
    TIDE_CHECK(players->joined == 3 && players->left == 1);

    tide_session_join(guest, tide_loopback_endpoint(network, 2), tide_loopback_address(1), t);
    run_three(network, all, &t, 6.0);
    TIDE_CHECK(still_in(guest, &gone));
    TIDE_CHECK(tide_session_status_of(guest).client.player.id == kicked.id); // The same player

    tide_session_kick_all(host, "The party's over");
    run_three(network, all, &t, 8.0);
    TIDE_CHECK(!still_in(guest, &gone) && gone.reason == TIDE_DISCONNECT_KICKED);
    TIDE_CHECK(strcmp(gone.message, "The party's over") == 0);
    TIDE_CHECK(!still_in(late, &gone) && gone.reason == TIDE_DISCONNECT_KICKED);
    TIDE_CHECK(still_in(host, &gone)); // Its own player plays on, alone
    TIDE_CHECK(players_in(tide_session_server_world(host))->left == 3);

    tide_session_destroy(late);
    tide_session_destroy(guest);
    tide_session_destroy(host);
    tide_loopback_destroy(network);
}

// A player whose network lets it send but hears nothing while `deaf`.
static tide_transport hearing;
static bool deaf;

static void deaf_send(void *self, const tide_address to, const void *data, const uint32_t size)
{
    (void)self;
    hearing.send(hearing.self, to, data, size);
}

static uint32_t deaf_receive(void *self, tide_address *from, void *data, const uint32_t capacity)
{
    (void)self;
    uint32_t n;
    while ((n = hearing.receive(hearing.self, from, data, capacity)) > 0 && deaf) {
    }
    return deaf ? 0u : n;
}

static void deaf_close(void *self)
{
    (void)self;
    if (hearing.close) hearing.close(hearing.self);
}

static tide_transport sometimes_deaf(const tide_transport t)
{
    hearing = t;
    return (tide_transport){NULL, deaf_send, deaf_receive, deaf_close, NULL};
}

// A kicked player who heard nothing of it times out, and hears it the next
// time it joins, before it's let in. Knowing, it can join again.
TIDE_TEST(net_a_kicked_player_who_didnt_hear_it_hears_it_coming_back)
{
    tide_loopback *network = tide_loopback_create(79);
    tide_loopback_set_conditions(network, (tide_net_conditions){.latency = 0.02});
    tide_session *host = tide_session_create(&(tide_session_desc){.game = &tide_game_api, .tick_rate = 60, .sample = session_sample});
    tide_session *guest = tide_session_create(&(tide_session_desc){.game = &tide_game_api, .tick_rate = 60, .sample = guest_sample});
    tide_session_event gone = {0};
    double t = 0.0;
    deaf = false;
    tide_session_start(host, NULL, t);
    TIDE_REQUIRE(tide_session_open(host, tide_loopback_endpoint(network, 1)));
    tide_session_join(guest, sometimes_deaf(tide_loopback_endpoint(network, 2)), tide_loopback_address(1), t);
    run_sessions(network, host, guest, &t, 2.0);
    TIDE_REQUIRE(still_in(guest, &gone));
    const tide_player_id kicked = tide_session_status_of(guest).client.player;

    deaf = true;
    tide_session_kick(host, kicked, "Bye");
    run_sessions(network, host, guest, &t, 8.0);
    TIDE_CHECK(!still_in(guest, &gone) && gone.reason == TIDE_DISCONNECT_TIMED_OUT); // It heard nothing

    deaf = false;
    tide_session_join(guest, sometimes_deaf(tide_loopback_endpoint(network, 2)), tide_loopback_address(1), t);
    run_sessions(network, host, guest, &t, 9.0);
    TIDE_CHECK(!still_in(guest, &gone) && gone.reason == TIDE_DISCONNECT_KICKED);
    TIDE_CHECK(strcmp(gone.message, "Bye") == 0);

    tide_session_join(guest, sometimes_deaf(tide_loopback_endpoint(network, 2)), tide_loopback_address(1), t);
    run_sessions(network, host, guest, &t, 11.0);
    TIDE_CHECK(still_in(guest, &gone));
    TIDE_CHECK(tide_session_status_of(guest).client.player.id == kicked.id); // The same player
    tide_session_destroy(guest);
    tide_session_destroy(host);
    tide_loopback_destroy(network);
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
    tide_session_start(host, NULL, t);
    TIDE_REQUIRE(tide_session_open(host, tide_loopback_endpoint(network, 1)));
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
    tide_session_start(s, NULL, t);
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
    tide_session_start_from(next, left, 1u << player, t);
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
    TIDE_CHECK(same(tide_session_world(next), w));
    tide_session_destroy(next);
    tide_world_free(left);
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

// As what changed from another input, as frames and players send them: a bit
// for each value that didn't, and back exactly.
TIDE_TEST(net_input_packs_as_what_changed)
{
    const Controls was = {.move = {0.25f, 0.0f}, .jump = true};
    uint8_t bytes[16];
    Controls out;
    uint32_t n = tide_game_api.write_input_delta(&was, &was, bytes, sizeof bytes);
    TIDE_CHECK(n == 1u); // Three bits
    TIDE_REQUIRE(tide_game_api.read_input_delta(bytes, n, &was, &out));
    TIDE_CHECK(memcmp(&was, &out, sizeof out) == 0);

    // A button let go, and a value whose bits changed, though -0 == 0
    const Controls now = {.move = {0.25f, -0.0f}, .jump = false};
    n = tide_game_api.write_input_delta(&now, &was, bytes, sizeof bytes);
    TIDE_CHECK(n == 5u); // 1 + 33 + 1 bits
    TIDE_CHECK(n <= tide_game_api.max_input_bytes);
    TIDE_REQUIRE(tide_game_api.read_input_delta(bytes, n, &was, &out));
    TIDE_CHECK(memcmp(&now, &out, sizeof out) == 0);
    TIDE_CHECK(!tide_game_api.read_input_delta(bytes, 0, &was, &out)); // Too short
}
