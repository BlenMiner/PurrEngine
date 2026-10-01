// Desktop only: rooms through the relay, run by rooms.mjs, which starts a
// relay on this machine and sets TIDE_RELAY to it.
//
//     tide_platform_rooms host              hosts a match in a room, says its code
//     tide_platform_rooms join <code>       joins it
//     tide_platform_rooms echo-host         hosts a room, for a browser that echoes
//     tide_platform_rooms echo-join <code>  joins a browser's room that echoes
//
// The first two play a match, like web_rooms.c: each says "ok" once the
// other's input reaches it, and keeps playing so the other can finish. The
// echo ones send datagrams of every size up to the biggest sessions send,
// through a browser's WebRTC and back, and end with 0 once all came back.

#if !defined(_WIN32) && !defined(__APPLE__)
#define _DEFAULT_SOURCE // nanosleep and clock_gettime under strict C
#endif

#include <stdio.h>
#include <string.h>

#include "tide/platform.h"
#include "tide/session.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static void nap(void)
{
    Sleep(2);
}
#else
#include <time.h>
static void nap(void)
{
    const struct timespec t = {0, 2000000};
    nanosleep(&t, NULL);
}
#endif

static double clock_seconds(void)
{
#ifdef _WIN32
    LARGE_INTEGER frequency, now;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
#endif
}

// ---------------------------------------------------------------------------
// A match: the game from web_rooms.c

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

// The world as bytes, to send it: plain data, as it is.
static uint32_t pack(const void *w, uint8_t *out, const uint32_t capacity)
{
    if (out && capacity >= sizeof(world)) memcpy(out, w, sizeof(world));
    return sizeof(world);
}

static bool unpack(void *w, const uint8_t *data, const uint32_t size)
{
    if (size != sizeof(world)) return false;
    memcpy(w, data, size);
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
    .pack_world = pack,
    .unpack_world = unpack,
    .player_joined = joined,
    .player_left = left,
    .set_input = set_input,
    .set_server_input = set_server_input,
    .write_input = write_input,
    .read_input = read_input,
};

static int32_t me;

static void sample(void *user, const uint32_t t, void *input)
{
    (void)user;
    (void)t;
    memcpy(input, &me, 4);
}

static int play(const bool host, const char *code)
{
    me = host ? 1 : 2;
    tide_session *session = tide_session_create(&(tide_session_desc){.game = &game, .tick_rate = 60, .sample = sample});
    const double begin = clock_seconds();
    tide_transport network;
    tide_address server;
    if (host) {
        if (!tide_platform_host_open(0, &network)) return printf("FAIL: no room to host\n"), 1;
        tide_session_start(session, NULL, 0.0);
        tide_session_open(session, network);
    } else {
        if (!tide_platform_room_join(code, &network, &server)) return printf("FAIL: no room to join\n"), 1;
        tide_session_join(session, network, server, 0.0);
    }
    bool said_code = false, done = false;
    for (;;) {
        const double now = clock_seconds() - begin;
        if (now > 45.0) return printf("FAIL: the players didn't meet within 45 seconds\n"), 1;
        if (tide_platform_room_failed()) tide_session_fail(session, TIDE_DISCONNECT_FAILED);
        tide_session_update(session, now);
        tide_session_event event;
        while (tide_session_next_event(session, &event)) {
            if (event.kind == TIDE_SESSION_DISCONNECTED_EVENT) {
                printf("FAIL: the match ended, reason %d\n", (int)event.reason);
                return 1;
            }
            printf("connected\n");
        }
        char room[TIDE_ROOM_CODE_LENGTH + 1];
        tide_platform_room_code(room, sizeof room);
        if (host && !said_code && room[0]) {
            printf("room %s\n", room);
            said_code = true;
        }
        const world *w = tide_session_world(session);
        const int32_t other = me == 1 ? 2 : 1;
        bool seen = false;
        for (int i = 0; w && i < (int)TIDE_MAX_PLAYERS; i++) seen |= w->inputs[i] == other;
        if (!done && seen && w->joined == 2) {
            const tide_session_status status = tide_session_status_of(session);
            if (status.open != host) return printf("FAIL: the match is%s open\n", status.open ? "" : "n't"), 1;
            printf("ok: tick %u, verified %u, %u resyncs\n", (unsigned)w->tick, (unsigned)status.client.verified_tick,
                   (unsigned)status.client.resyncs);
            done = true;
        }
        fflush(stdout);
        nap();
    }
}

// ---------------------------------------------------------------------------
// Echoes: straight on the room's transport

static int echo(const bool host, const char *code)
{
    tide_transport t;
    tide_address other = {0};
    if (host ? !tide_platform_room_host(&t) : !tide_platform_room_join(code, &t, &other)) {
        return printf("FAIL: no room\n"), 1;
    }
    const double begin = clock_seconds();
    bool said_code = false, found = !host; // A host learns the browser's address from its first datagram
    static bool back[1201];
    int sizes_back = 0;
    uint8_t datagram[1500];
    double last_round = -1.0;
    for (;;) {
        const double now = clock_seconds() - begin;
        if (now > 45.0) {
            printf("FAIL: %d of 120 sizes came back within 45 seconds\n", sizes_back);
            return 1;
        }
        if (tide_platform_room_failed()) return printf("FAIL: the room failed\n"), 1;
        char room[TIDE_ROOM_CODE_LENGTH + 1];
        tide_platform_room_code(room, sizeof room);
        if (host && !said_code && room[0]) {
            printf("room %s\n", room);
            said_code = true;
        }
        tide_address from;
        uint32_t n;
        while ((n = t.receive(t.self, &from, datagram, sizeof datagram)) != 0) {
            if (!found) {
                other = from;
                found = true;
                printf("connected\n");
            } else if (n >= 2 && datagram[0] == 0xee && n <= 1200 && !back[n]) {
                back[n] = true;
                sizes_back++;
            }
        }
        // Sizes 10, 20, ... 1200, again every half second until they're back:
        // the channel doesn't send anything twice
        if (found && now - last_round > 0.5) {
            last_round = now;
            for (uint32_t size = 10; size <= 1200; size += 10) {
                if (back[size]) continue;
                memset(datagram, (int)(size & 0xff), size);
                datagram[0] = 0xee;
                t.send(t.self, other, datagram, size);
            }
        }
        if (sizes_back == 120) {
            printf("ok: every size came back\n");
            fflush(stdout);
            t.close(t.self);
            return 0;
        }
        fflush(stdout);
        nap();
    }
}

int main(const int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *mode = argc > 1 ? argv[1] : "";
    const char *code = argc > 2 ? argv[2] : "";
    if (strcmp(mode, "host") == 0) return play(true, NULL);
    if (strcmp(mode, "join") == 0 && argc > 2) return play(false, code);
    if (strcmp(mode, "echo-host") == 0) return echo(true, NULL);
    if (strcmp(mode, "echo-join") == 0 && argc > 2) return echo(false, code);
    printf("FAIL: run it with host, join <code>, echo-host or echo-join <code>\n");
    return 1;
}
