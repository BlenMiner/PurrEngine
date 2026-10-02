// Web only: two players in a room, through the relay (relay/) and WebRTC.
// web_rooms.mjs runs it twice in one page: `host`, which opens a room and
// says its code, then `join <code>`. The game is written by hand in C, as in
// udp.c. Once the other's inputs reach it, the host's page goes hidden for
// longer than players take to time out, as if its player switched tabs, and
// the match has to go on: the joiner's verified tick keeps up. Each prints
// "ok" then, and keeps playing so the other can finish; anything else ends it
// with "FAIL". With `handover-host` and `handover-join <code> <n>`, three of
// them check host migration instead; with `end-host`, a host says its room's
// code and key and ends its match at once, and `migrate <code> <key>` goes to
// that room again and says what the relay answered.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tide/platform.h"
#include "tide/session.h"
#include "tide_web.h"

typedef struct world {
    uint32_t tick;
    uint32_t joined;
    uint32_t left;
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

static void tick(void *w, const tide_jobs *jobs)
{
    (void)jobs;
    ((world *)w)->tick++;
}

static void joined(void *w, const tide_player_id player)
{
    (void)player;
    ((world *)w)->joined++;
}

static void left(void *w, const tide_player_id player)
{
    (void)player;
    ((world *)w)->left++;
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

// The world as bytes, to send it: plain data, all of it, whatever the
// receiver has.
static uint8_t *pack(const void *w, const void *base, const uint8_t *need, const uint32_t need_size, uint32_t *size)
{
    (void)base;
    (void)need;
    (void)need_size;
    uint8_t *out = malloc(sizeof(world));
    if (!out) abort();
    memcpy(out, w, sizeof(world));
    *size = sizeof(world);
    return out;
}

static bool unpack(void *w, const void *base, const uint8_t *data, const uint32_t size)
{
    (void)base;
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
    .pack_delta = pack,
    .unpack_delta = unpack,
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

// The host's page hidden, as browsers hide it when another tab is in front:
// no animation frames, and timers a second apart at the soonest. The event
// comes after the frame, as browsers send it. Its frames come at the match's
// ticks meanwhile, not the 60 a second frames come at by default, and often
// enough that its match skips next to none of them. A slow machine's frames can
// run two ticks at once, so it may have fewer frames than ticks, and it can
// stall for longer than the ticks a server runs in one update, whose time the
// match skips (macOS CI: up to 0.16 seconds). A page without the worker's
// frames skips 5 seconds of the 7.
#define HIDDEN_SECONDS 7.0 // Past the session's 5 second timeout
#define MOST_SKIPPED 1.0
#define TICK_RATE 30

static double met_at = -1.0;   // When the other's inputs first reached this one
static uint32_t met_verified;  // ...and the verified tick then
static int hidden_frames = -1; // Frames while the host's page was hidden
static double hidden_skipped;  // The match's skipped time as the page was hidden
static bool shown_again;

static void set_hidden(const bool hidden)
{
    tide_web_eval(hidden ? "setTimeout(() => {"
                           " const timer = window.setTimeout, frame = window.requestAnimationFrame;"
                           " window.shown = () => { window.setTimeout = timer; window.requestAnimationFrame = frame;"
                           " delete document.hidden; document.dispatchEvent(new Event('visibilitychange')); };"
                           " window.setTimeout = (f, ms, ...rest) => timer(f, Math.max(ms || 0, 1000), ...rest);"
                           " window.requestAnimationFrame = () => 0;"
                           " Object.defineProperty(document, 'hidden', { configurable: true, get: () => true });"
                           " document.dispatchEvent(new Event('visibilitychange')); })"
                         : "setTimeout(() => window.shown())");
}

// Host migration (see handover in rooms_native.c): the host leaves a second
// after both players joined; they say "ok before" once in, and "ok after"
// once the match changed hands, with their player both times.
static tide_game played;
static bool handover;
static bool before;
static bool moved;
static double all_in;
static double migrating_since;

static int fail(const char *why)
{
    printf("FAIL: %s\n", why);
    return 1;
}

// Drives host migration as tide/host.h does.
static void migrate(void)
{
    char code[TIDE_ROOM_CODE_LENGTH + 1] = "";
    char key[TIDE_ROOM_KEY_LENGTH + 1] = "";
    if (tide_session_status_of(session).server) {
        tide_platform_room_code(code, sizeof code);
        tide_platform_room_key(key, sizeof key);
        tide_session_set_room(session, code, key);
    }
    if (!tide_session_migrating(session, code, key)) {
        migrating_since = 0.0;
        return;
    }
    moved = true;
    if (migrating_since == 0.0) {
        migrating_since = now;
        printf("migrating to room %s\n", code);
        tide_platform_room_migrate(code, key);
        return;
    }
    tide_transport network;
    tide_address server;
    const int answer = tide_platform_room_migrated(&network, &server);
    if (answer == 1) {
        printf("hosting now\n");
        tide_session_take_over(session, network, now);
    } else if (answer == 2) {
        printf("joining the new host\n");
        tide_session_join(session, network, server, now);
    } else if (answer < 0) {
        tide_session_fail(session, TIDE_DISCONNECT_TIMED_OUT);
    }
}

// A host that ends its match as soon as it has a room, which the relay may
// not know yet, and a player who missed the goodbye.
static bool end_host;
static const char *migrate_code;
static const char *migrate_key;

static int frame_ended(void)
{
    if (now > 30.0 && !done) return fail("not done within 30 seconds");
    if (migrate_code) {
        if (now == 0.0 || done) return TIDE_KEEP_RUNNING;
        if (!moved) {
            tide_platform_room_migrate(migrate_code, migrate_key);
            moved = true;
        }
        tide_transport network;
        tide_address server;
        const int answer = tide_platform_room_migrated(&network, &server);
        if (answer != 0) {
            printf("migrated %d\n", answer);
            done = true;
        }
        return TIDE_KEEP_RUNNING;
    }
    tide_session_update(session, now);
    char code[TIDE_ROOM_CODE_LENGTH + 1];
    char key[TIDE_ROOM_KEY_LENGTH + 1];
    tide_platform_room_code(code, sizeof code);
    tide_platform_room_key(key, sizeof key);
    if (code[0] && key[0] && !done) {
        printf("room %s key %s\n", code, key);
        tide_session_end(session);
        printf("ended\n");
        done = true;
    }
    return TIDE_KEEP_RUNNING;
}

// Three players: the host leaves once both others are in, and they go on.
static int frame_handover(void)
{
    if (now > 45.0 && !done) return fail("the match didn't change hands within 45 seconds");
    char code[TIDE_ROOM_CODE_LENGTH + 1];
    char key[TIDE_ROOM_KEY_LENGTH + 1];
    if (tide_platform_room_failed() && !tide_session_migrating(session, code, key)) {
        tide_session_fail(session, TIDE_DISCONNECT_FAILED);
    }
    tide_session_update(session, now);
    migrate();
    tide_session_event event;
    while (tide_session_next_event(session, &event)) {
        if (event.kind == TIDE_SESSION_CONNECTED_EVENT) {
            printf("connected\n");
        } else if (me == 1 && event.reason == TIDE_DISCONNECT_LEFT) {
            printf("left\n");
        } else {
            printf("disconnected, reason %d\n", (int)event.reason);
            return fail("the match ended");
        }
    }
    tide_platform_room_code(code, sizeof code);
    if (me == 1 && !said_code && code[0]) {
        printf("room %s\n", code);
        said_code = true;
    }
    const world *w = tide_session_world(session);
    const tide_session_status status = tide_session_status_of(session);
    const bool in = status.client.state == TIDE_SESSION_CONNECTED;
    if (me == 1 && in && w && w->joined == 3 && all_in == 0.0) all_in = now;
    if (me == 1 && all_in > 0.0 && now - all_in > 1.0 && !done) {
        tide_session_leave(session); // The others carry on without this machine
        done = true;
    }
    if (me != 1 && in && w && w->joined == 3 && !before) {
        printf("ok before: player %u\n", (unsigned)status.client.player.id);
        before = true;
    }
    if (me != 1 && in && w && moved && w->left == 1 && !done) {
        printf("ok after: player %u%s\n", (unsigned)status.client.player.id, status.server ? ", hosting" : "");
        done = true;
    }
    return TIDE_KEEP_RUNNING;
}

static int frame(void *user, const float seconds)
{
    (void)user;
    now += seconds;
    if (end_host || migrate_code) return frame_ended();
    if (handover) return frame_handover();
    if (now > 45.0) return fail(met_at < 0.0 ? "the players didn't meet within 45 seconds" : "the test didn't finish within 45 seconds");
    if (tide_platform_room_failed()) tide_session_fail(session, TIDE_DISCONNECT_FAILED);
    tide_session_update(session, now);
    tide_platform_next_frame(tide_session_until_tick(session));
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
    if (met_at < 0.0 && seen && w->joined == 2 && status.client.state == TIDE_SESSION_CONNECTED && code[0]) {
        printf("met: tick %u, verified %u, room %s\n", (unsigned)w->tick, (unsigned)status.client.verified_tick, code);
        met_at = now;
        met_verified = status.client.verified_tick;
        if (me == 1) {
            set_hidden(true);
            hidden_frames = 0;
            hidden_skipped = status.skipped;
        }
    }
    if (me == 1 && hidden_frames >= 0 && !shown_again) {
        if (tide_web_hidden()) hidden_frames++;
        if (now - met_at > HIDDEN_SECONDS) {
            const double skipped = status.skipped - hidden_skipped;
            printf("shown again: %d frames while hidden, %.3f seconds skipped\n", hidden_frames, skipped);
            if (skipped > MOST_SKIPPED) return fail("the match skipped ticks while the page was hidden");
            if (hidden_frames > (int)(HIDDEN_SECONDS * TICK_RATE * 1.15)) return fail("hidden frames didn't follow the tick rate");
            set_hidden(false);
            shown_again = true;
        }
    }
    // The host's server kept ticking all along: the joiner's verified tick
    // went on through the time the host's page was hidden.
    const uint32_t through_tick = met_verified + (uint32_t)(TICK_RATE * (HIDDEN_SECONDS + 2.0));
    const bool through = me == 1 ? shown_again && !tide_web_hidden()
                                 : met_at >= 0.0 && status.client.verified_tick >= through_tick;
    if (!done && through && status.client.state == TIDE_SESSION_CONNECTED) {
        printf("ok: tick %u, verified %u, %u resyncs, room %s\n", (unsigned)w->tick,
               (unsigned)status.client.verified_tick, (unsigned)status.client.resyncs, code);
        done = true;
    }
    return TIDE_KEEP_RUNNING;
}

int main(const int argc, char **argv)
{
    if (argc > 3 && strcmp(argv[1], "migrate") == 0) {
        migrate_code = argv[2];
        migrate_key = argv[3];
        tide_platform_open(&(tide_window_desc){.title = "web rooms", .width = 64, .height = 64, .hidden = true});
        tide_platform_run(frame, NULL);
    }
    end_host = argc > 1 && strcmp(argv[1], "end-host") == 0;
    handover = end_host || (argc > 1 && strncmp(argv[1], "handover-", 9) == 0);
    const char *mode = end_host ? "host" : argc > 1 ? argv[1] + (handover ? 9 : 0) : "";
    const bool host = strcmp(mode, "host") == 0;
    const bool join = argc > 2 && strcmp(mode, "join") == 0;
    if (!host && !join) return fail("run it with 'host', 'join <code>', 'handover-host' or 'handover-join <code> <n>'");
    me = host ? 1 : handover && argc > 3 ? (int32_t)(argv[3][0] - '0') : 2;
    played = game;
    played.host_migration = handover;
    if (handover) played.hash = 8;
    tide_platform_open(&(tide_window_desc){.title = "web rooms", .width = 64, .height = 64, .hidden = true});
    session = tide_session_create(&(tide_session_desc){.game = &played, .tick_rate = TICK_RATE, .sample = sample});
    tide_transport network;
    tide_address server;
    if (host) {
        if (!tide_platform_room_host(&network)) return fail("no room to host");
        tide_session_start(session, NULL, now);
        tide_session_open(session, network);
    } else {
        if (!tide_platform_room_join(argv[2], &network, &server)) return fail("no room to join");
        tide_session_join(session, network, server, now);
    }
    tide_platform_run(frame, NULL);
}
