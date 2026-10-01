// Web only: two players in a room, through the relay (relay/) and WebRTC.
// web_rooms.mjs runs it twice in one page: `host`, which opens a room and
// says its code, then `join <code>`. The game is written by hand in C, as in
// udp.c. Each prints "ok" once the other's inputs reach it, and keeps playing
// so the other can finish; anything else ends it with "FAIL".

#include <stdio.h>
#include <string.h>

#include "tide/platform.h"
#include "tide/session.h"

typedef struct world {
    uint32_t tick;
    uint32_t joined;
    int32_t inputs[TIDE_MAX_PLAYERS + 1];
} world;

static void start(void *w, const float dt, const void *s)
{
    (void)dt;
    (void)s;
    memset(w, 0, sizeof(world));
}

static void copy(void *to, const void *from)
{
    memcpy(to, from, sizeof(world));
}

static uint64_t hash(const void *w)
{
    return tide_hash(w, sizeof(world));
}

static void tick(void *w)
{
    ((world *)w)->tick++;
}

static void joined(void *w, const tide_player_id player)
{
    (void)player;
    ((world *)w)->joined++;
}

static void left(void *w, const tide_player_id player)
{
    (void)w;
    (void)player;
}

static void set_input(void *w, const tide_player_id player, const void *input)
{
    const int32_t index = tide_player_index(player);
    if (index >= 0) memcpy(&((world *)w)->inputs[index], input, 4);
}

static void set_server_input(void *w, const void *input)
{
    memcpy(&((world *)w)->inputs[TIDE_MAX_PLAYERS], input, 4);
}

static uint32_t write_input(const void *input, uint8_t *out, const uint32_t capacity)
{
    if (capacity < 4) return 0;
    memcpy(out, input, 4);
    return 4;
}

static bool read_input(const uint8_t *data, const uint32_t size, void *input)
{
    if (size < 4) return false;
    memcpy(input, data, 4);
    return true;
}

static const tide_game game = {
    .hash = 7,
    .world_size = sizeof(world),
    .input_size = 4,
    .max_input_bytes = 4,
    .start = start,
    .tick = tick,
    .copy_world = copy,
    .hash_world = hash,
    .player_joined = joined,
    .player_left = left,
    .set_input = set_input,
    .set_server_input = set_server_input,
    .write_input = write_input,
    .read_input = read_input,
};

// Each player's input is its own number: the host's 1, the other's 2.
static int32_t me;

static void sample(void *user, const uint32_t t, void *input)
{
    (void)user;
    (void)t;
    memcpy(input, &me, 4);
}

static tide_session *session;
static double now;
static bool said_code;
static bool done;

static int fail(const char *why)
{
    printf("FAIL: %s\n", why);
    return 1;
}

static int frame(void *user, const float seconds)
{
    (void)user;
    now += seconds;
    if (now > 45.0) return fail("the players didn't meet within 45 seconds");
    if (tide_platform_room_failed()) tide_session_fail(session, TIDE_DISCONNECT_FAILED);
    tide_session_update(session, now);
    tide_session_event event;
    while (tide_session_next_event(session, &event)) {
        if (event.kind == TIDE_SESSION_CONNECTED_EVENT) {
            printf("connected\n");
        } else {
            printf("disconnected, reason %d\n", (int)event.reason);
            return fail("the match ended");
        }
    }
    char code[TIDE_ROOM_CODE_LENGTH + 1];
    tide_platform_room_code(code, sizeof code);
    if (me == 1 && !said_code && code[0]) {
        printf("room %s\n", code);
        said_code = true;
    }

    // Each sees the other's input in the match: it went through the room
    const world *w = tide_session_world(session);
    const int32_t other = me == 1 ? 2 : 1;
    bool seen = false;
    for (int i = 0; w && i < (int)TIDE_MAX_PLAYERS; i++) seen |= w->inputs[i] == other;
    const tide_session_status status = tide_session_status_of(session);
    if (!done && seen && w->joined == 2 && status.client.state == TIDE_SESSION_CONNECTED && code[0]) {
        printf("ok: tick %u, verified %u, %u resyncs, room %s\n", (unsigned)w->tick,
               (unsigned)status.client.verified_tick, (unsigned)status.client.resyncs, code);
        done = true;
    }
    return TIDE_KEEP_RUNNING;
}

int main(const int argc, char **argv)
{
    const bool host = argc > 1 && strcmp(argv[1], "host") == 0;
    const bool join = argc > 2 && strcmp(argv[1], "join") == 0;
    if (!host && !join) return fail("run it with 'host' or 'join <code>'");
    me = host ? 1 : 2;
    tide_platform_open(&(tide_window_desc){.title = "web rooms", .width = 64, .height = 64, .hidden = true});
    session = tide_session_create(&(tide_session_desc){.game = &game, .tick_rate = 60, .sample = sample});
    tide_transport network;
    tide_address server;
    if (host) {
        if (!tide_platform_room_host(&network)) return fail("no room to host");
        tide_session_host(session, NULL, network, now);
    } else {
        if (!tide_platform_room_join(argv[2], &network, &server)) return fail("no room to join");
        tide_session_join(session, network, server, now);
    }
    tide_platform_run(frame, NULL);
}
