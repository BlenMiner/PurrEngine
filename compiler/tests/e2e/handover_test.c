#include <string.h>

#include "game.h"
#include "tide_test.h"

// Machines on one loopback network, each a session as a host program runs it.
// What the relay does in a real room, the test does: the machine that takes
// the match over takes players on another endpoint, which the others join.

#define MACHINES 4

static tide_loopback *net;
static tide_session *machines[MACHINES];
static bool frozen[MACHINES]; // Not updated: a machine that stopped answering
static int connected[MACHINES];
static int disconnected[MACHINES];
static tide_disconnect_reason gone[MACHINES];
static double now;

static void sample(void *user, const uint32_t tick, void *input)
{
    (void)user;
    (void)tick;
    *(Controls *)input = (Controls){.push = 1};
}

static void setup(void)
{
    now = 0.0;
    net = tide_loopback_create(3);
    tide_loopback_set_conditions(net, (tide_net_conditions){.latency = 0.02});
    for (int i = 0; i < MACHINES; i++) {
        machines[i] = tide_session_create(&(tide_session_desc){.game = &tide_game_api, .sample = sample});
        frozen[i] = false;
        connected[i] = disconnected[i] = 0;
    }
}

static void teardown(void)
{
    for (int i = 0; i < MACHINES; i++) tide_session_destroy(machines[i]);
    tide_loopback_destroy(net);
}

static void run(const double seconds)
{
    const double until = now + seconds;
    while (now < until) {
        now += 1.0 / 60.0;
        tide_loopback_set_time(net, now);
        for (int i = 0; i < MACHINES; i++) {
            if (frozen[i]) continue;
            tide_session_update(machines[i], now);
            tide_session_event e;
            while (tide_session_next_event(machines[i], &e)) {
                if (e.kind == TIDE_SESSION_CONNECTED_EVENT) {
                    connected[i]++;
                } else {
                    disconnected[i]++;
                    gone[i] = e.reason;
                }
            }
        }
    }
}

static tide_session_status status(const int i)
{
    return tide_session_status_of(machines[i]);
}

static const tide_world *server_world(const int i)
{
    return tide_session_server_world(machines[i]);
}

// Machine 0 runs a match in room ABCDEF, which machines 1 and 2 join.
static void play_three(void)
{
    tide_session_start(machines[0], NULL, now);
    tide_session_open(machines[0], tide_loopback_endpoint(net, 1));
    tide_session_set_room(machines[0], "ABCDEF", "key1");
    tide_session_join(machines[1], tide_loopback_endpoint(net, 2), tide_loopback_address(1), now);
    tide_session_join(machines[2], tide_loopback_endpoint(net, 3), tide_loopback_address(1), now);
    run(2.0);
}

TIDE_TEST(handover_a_player_takes_the_match_over_when_its_host_leaves)
{
    setup();
    play_three();
    for (int i = 0; i < 3; i++) TIDE_REQUIRE(status(i).client.state == TIDE_SESSION_CONNECTED);
    const uint32_t player1 = status(1).client.player.id;
    const uint32_t player2 = status(2).client.player.id;
    const int32_t ticks = server_world(0)->Match.ticks;
    tide_session_close(machines[0]); // Closed: no one new, but its players come back
    run(0.5);                        // ...which its players hear

    tide_session_leave(machines[0]);
    run(0.2);
    char code[TIDE_ROOM_CODE_LENGTH + 1];
    char key[TIDE_ROOM_KEY_LENGTH + 1];
    TIDE_REQUIRE(tide_session_migrating(machines[2], code, key));
    TIDE_REQUIRE(tide_session_migrating(machines[1], code, key));
    TIDE_CHECK(strcmp(code, "ABCDEF") == 0 && strcmp(key, "key1") == 0);
    // Meanwhile, the same player, connecting, and views see the last world
    TIDE_CHECK(status(1).client.state == TIDE_SESSION_CONNECTING && status(1).client.player.id == player1);
    TIDE_CHECK(tide_session_world(machines[1]) != NULL);

    tide_session_take_over(machines[1], tide_loopback_endpoint(net, 4), now);
    tide_session_join(machines[2], tide_loopback_endpoint(net, 5), tide_loopback_address(4), now);
    run(2.0);
    TIDE_CHECK(status(1).server && status(1).client.state == TIDE_SESSION_CONNECTED);
    TIDE_CHECK(status(1).client.player.id == player1);
    TIDE_CHECK(status(2).client.state == TIDE_SESSION_CONNECTED && status(2).client.player.id == player2);
    const tide_world *w = server_world(1);
    TIDE_REQUIRE(w);
    TIDE_CHECK(w->Match.joined == 3); // No one joined again
    TIDE_CHECK(w->Match.left == 1);   // The last host's player
    TIDE_CHECK(w->Match.ticks > ticks + 100); // The match went on
    // Local code saw it connect once, and never leave
    TIDE_CHECK(connected[1] == 1 && disconnected[1] == 0);
    TIDE_CHECK(connected[2] == 1 && disconnected[2] == 0);
    TIDE_CHECK(disconnected[0] == 1 && gone[0] == TIDE_DISCONNECT_LEFT);

    // The match is still closed: someone new is turned away
    tide_session_join(machines[3], tide_loopback_endpoint(net, 6), tide_loopback_address(4), now);
    run(1.0);
    TIDE_CHECK(disconnected[3] == 1 && gone[3] == TIDE_DISCONNECT_REFUSED);
    teardown();
}

TIDE_TEST(handover_happens_again_and_when_the_host_stops_answering)
{
    setup();
    play_three();
    const uint32_t player2 = status(2).client.player.id;
    frozen[0] = true; // Its machine went away without a word
    run(5.5);
    char code[TIDE_ROOM_CODE_LENGTH + 1];
    char key[TIDE_ROOM_KEY_LENGTH + 1];
    TIDE_REQUIRE(tide_session_migrating(machines[1], code, key));
    TIDE_REQUIRE(tide_session_migrating(machines[2], code, key));
    tide_session_take_over(machines[1], tide_loopback_endpoint(net, 4), now);
    tide_session_join(machines[2], tide_loopback_endpoint(net, 5), tide_loopback_address(4), now);
    run(2.0);
    TIDE_REQUIRE(status(2).client.state == TIDE_SESSION_CONNECTED);

    // The new host tells its players where to meet too: the match changes hands again
    tide_session_leave(machines[1]);
    run(0.2);
    TIDE_REQUIRE(tide_session_migrating(machines[2], code, key));
    TIDE_CHECK(strcmp(code, "ABCDEF") == 0 && strcmp(key, "key1") == 0);
    tide_session_take_over(machines[2], tide_loopback_endpoint(net, 7), now);
    run(1.0);
    TIDE_CHECK(status(2).server && status(2).client.state == TIDE_SESSION_CONNECTED);
    TIDE_CHECK(status(2).client.player.id == player2);
    TIDE_CHECK(server_world(2)->Match.left == 2);
    TIDE_CHECK(connected[2] == 1 && disconnected[2] == 0);
    teardown();
}

TIDE_TEST(handover_players_who_dont_come_back_leave)
{
    setup();
    play_three();
    tide_session_leave(machines[0]);
    run(0.2);
    tide_session_take_over(machines[1], tide_loopback_endpoint(net, 4), now);
    frozen[2] = true; // Never comes back
    run(19.0);
    TIDE_CHECK(server_world(1)->Match.left == 1); // Its player waits a while...
    run(2.0);
    TIDE_CHECK(server_world(1)->Match.left == 2); // ...then leaves
    teardown();
}

TIDE_TEST(handover_not_when_the_host_ends_the_match)
{
    setup();
    play_three();
    tide_session_end(machines[0]);
    run(1.0);
    char code[TIDE_ROOM_CODE_LENGTH + 1];
    char key[TIDE_ROOM_KEY_LENGTH + 1];
    for (int i = 0; i < 3; i++) {
        TIDE_CHECK(!tide_session_migrating(machines[i], code, key));
        TIDE_CHECK(disconnected[i] == 1 && gone[i] == TIDE_DISCONNECT_ENDED);
        TIDE_CHECK(status(i).client.state == TIDE_SESSION_OFFLINE);
    }
    teardown();
}

TIDE_TEST(handover_not_without_a_room)
{
    setup();
    tide_session_start(machines[0], NULL, now);
    tide_session_open(machines[0], tide_loopback_endpoint(net, 1)); // No room: nowhere to meet again
    tide_session_join(machines[1], tide_loopback_endpoint(net, 2), tide_loopback_address(1), now);
    run(2.0);
    tide_session_leave(machines[0]);
    run(0.5);
    TIDE_CHECK(disconnected[1] == 1 && gone[1] == TIDE_DISCONNECT_SERVER_LEFT);
    teardown();
}
