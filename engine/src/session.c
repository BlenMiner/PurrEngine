#include "tide/session.h"

#include <stdlib.h>
#include <string.h>

// See tide/session.h.
//
// Every datagram starts with a header: "PU", the protocol version and a type.
//
//   HELLO    client: the game's hash, a number for this attempt, its time,
//            and its cookie from before, if any
//   WELCOME  server: the attempt's number, the player, the tick rate, the
//            world being sent: its tick, size and number of chunks, and the
//            player's cookie
//   REFUSE   server: why
//   CHUNK    server: a piece of the packed world
//   CLIENT   client: times, the chunks it has of which world, the ticks it
//            has, whether it needs the world again, and its inputs from the
//            first the server lacks
//   SERVER   server: times, its tick, how early the player's inputs arrive, the
//            newest input it has, and pieces of the ticks the player lacks
//   BYE      either: leaving; the server's says whether the match ended
//
// Nothing is sent reliably as such: each side says what it has, and the other
// sends what's missing again until it does.
//
// A tick, as the server sends it (a "frame"): the tick, the hash of the world
// after it, the players who joined or left before it, and the inputs that
// changed for it: a bit per slot (players, then the server) and each input in
// slot order. A slot without one keeps its last input, on every machine,
// whether it didn't change or didn't arrive in time; a second set of bits says
// which ones were late, so a client knows whether its prediction held.

#define MAGIC 0x5449u // "TI"
#define PROTOCOL 1u

enum { MSG_HELLO = 1, MSG_WELCOME, MSG_REFUSE, MSG_CHUNK, MSG_CLIENT, MSG_SERVER, MSG_BYE };
enum { REFUSE_OTHER_GAME = 1, REFUSE_FULL = 2 };
enum { EVENT_JOIN = 1, EVENT_LEAVE = 2 };
enum { BYE_LEFT = 0, BYE_ENDED = 1 };

#define SERVER_SLOT TIDE_MAX_PLAYERS // The server's input, after the players'
#define PREDICTION_SECONDS 1.0 // How far a client runs ahead of the last tick it knows, at most
#define HISTORY_SECONDS 4.0    // How long the server keeps ticks to send again, and a client keeps ticks ahead
#define FIRST_RING 8u          // Snapshots a client starts with; more as it runs further ahead
#define PIECE 1000u                  // Bytes of a tick per piece
#define MAX_PIECES 32u
#define CHUNK 1000u                  // Bytes of a packed world per chunk
#define CHUNKS_PER_UPDATE 64u
#define PACKETS_PER_UPDATE 8u
#define MAX_EVENTS (4u * TIDE_MAX_PLAYERS)
#define TIMEOUT 5.0     // Seconds of silence before giving up on the other side
#define ROOM_TIMEOUT 15.0 // ...or on a room's host before it first answers: WebRTC can take a while to connect
#define HELLO_EVERY 0.2 // Seconds between HELLOs until the server answers
#define MAX_TICKS 8u    // Ticks a server runs in one update at most: after a stall it drops the time instead

// How far each side looks, in ticks at the match's tick rate.
typedef struct windows {
    uint32_t prediction; // Ticks a client runs ahead of the last one it knows at most
    uint32_t inputs;     // Inputs kept by tick: the prediction, and a little more
    uint32_t history;    // Ticks the server keeps to send again, and a client keeps ahead
} windows;

static windows windows_for(const uint32_t rate)
{
    const uint32_t prediction = (uint32_t)(PREDICTION_SECONDS * rate) > 4u ? (uint32_t)(PREDICTION_SECONDS * rate) : 4u;
    const uint32_t history = (uint32_t)(HISTORY_SECONDS * rate) > 64u ? (uint32_t)(HISTORY_SECONDS * rate) : 64u;
    return (windows){prediction, prediction + 8u, history};
}

static uint32_t millis(const double t)
{
    return t > 0.0 ? (uint32_t)(uint64_t)(t * 1000.0) : 0u;
}

// Whole ticks from `seconds` at `rate`.
static uint64_t ticks_in(const double seconds, const uint32_t rate)
{
    const double n = seconds * (double)rate + 1e-7;
    return n > 0.0 ? (uint64_t)n : 0u;
}

static void header(tide_writer *w, const uint8_t type)
{
    tide_write_u16(w, MAGIC);
    tide_write_u8(w, PROTOCOL);
    tide_write_u8(w, type);
}

// The datagram's type, or 0 if it isn't ours.
static uint8_t read_header(tide_reader *r)
{
    if (tide_read_u16(r) != MAGIC || tide_read_u8(r) != PROTOCOL) return 0;
    const uint8_t type = tide_read_u8(r);
    return r->failed ? 0 : type;
}

static void send_packet(const tide_transport *t, const tide_address to, const tide_writer *w)
{
    if (!w->overflow && t->send) t->send(t->self, to, w->data, w->size);
}

static void patch_u32(uint8_t *at, const uint32_t v)
{
    for (int i = 0; i < 4; i++) at[i] = (uint8_t)(v >> (8 * i));
}

static void patch_u64(uint8_t *at, const uint64_t v)
{
    for (int i = 0; i < 8; i++) at[i] = (uint8_t)(v >> (8 * i));
}

// The most a frame can take: its header, the events, and every slot's input.
static uint32_t frame_capacity(const tide_game *g)
{
    return 4u + 8u + 1u + 2u * MAX_EVENTS + 8u + (TIDE_MAX_PLAYERS + 1u) * (2u + g->max_input_bytes);
}

// An input from outside, as every machine will read it: unpacked, packed again
// the one way it packs, and unpacked from that. Returns the packed size, or 0.
static uint32_t canonical_input(const tide_game *g, const uint8_t *data, const uint32_t size, void *input,
                                uint8_t *packed)
{
    if (!g->read_input(data, size, input)) return 0;
    const uint32_t n = g->write_input(input, packed, g->max_input_bytes);
    if (n == 0 || !g->read_input(packed, n, input)) return 0;
    return n;
}

// ---------------------------------------------------------------------------
// Server

typedef struct stored_frame {
    uint32_t tick;
    uint32_t size;
    uint8_t *data;
} stored_frame;

typedef struct sent_mark {
    uint32_t tick;
    double at;
} sent_mark;

typedef struct connection {
    bool used;
    uint32_t transport;
    tide_address address;
    uint32_t nonce;
    bool local; // On the server's machine: its input is the server's too
    double last_heard;
    uint32_t their_time;
    uint32_t rtt_ms;

    // The world it's being sent
    bool sending;
    uint32_t snapshot_tick;
    uint32_t snapshot_size;
    uint32_t chunk_count;
    uint8_t *snapshot;
    double *chunk_sent;
    uint32_t chunk_ack;  // It has every chunk before this one
    uint64_t chunk_mask; // ...and these after it

    // Ticks
    uint32_t frame_ack;  // It has every tick before this one
    uint32_t frame_mask; // ...and these after it
    sent_mark *frame_sent; // history

    // Its inputs, by tick
    uint32_t newest_input; // The last tick it sent an input for, plus one
    uint32_t *input_tick; // inputs
    uint16_t *input_size;
    uint8_t *inputs; // `inputs` of max_input_bytes, then the last one set
    uint32_t last_size;
} connection;

// splitmix64
static uint64_t next_random(uint64_t *state)
{
    uint64_t z = (*state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

struct tide_server {
    tide_server_desc desc;
    const tide_game *game;
    void *world;
    uint32_t tick;
    bool started;
    bool ended; // The match ran out of scenes (tide_game.ended): no more ticks
    double clock_start;
    uint32_t clock_base;
    double now;
    connection connections[TIDE_MAX_PLAYERS]; // Player n is connections[n]
    uint8_t events[MAX_EVENTS][2];            // Joins and leaves before the next tick
    uint32_t event_count;
    windows w;
    stored_frame *frames; // history
    uint8_t *frame;  // The tick being made
    uint8_t *packet; // TIDE_NET_MTU
    uint8_t *input;  // An input: input_size, then its packed bytes
    uint8_t *packed; // max_input_bytes
    uint8_t *server_last; // The server's input last set, packed
    uint32_t server_last_size;
    // Each player's cookie, kept after they leave so they can come back, and
    // since when they're away (0: here, or never was)
    uint64_t cookies[TIDE_MAX_PLAYERS];
    double away_since[TIDE_MAX_PLAYERS];
    uint64_t random;
    uint32_t present; // Players in the world it went on from, who haven't joined it yet (tide_server_desc.players)
};

static double resend_after(const uint32_t rtt_ms)
{
    const double rtt = (double)rtt_ms / 1000.0;
    return rtt * 1.5 > 0.05 ? rtt * 1.5 : 0.05;
}

static void add_event(tide_server *s, const uint8_t kind, const uint32_t player)
{
    if (s->event_count == MAX_EVENTS) return;
    s->events[s->event_count][0] = kind;
    s->events[s->event_count][1] = (uint8_t)player;
    s->event_count++;
}

static void stop_sending(connection *c)
{
    free(c->snapshot);
    free(c->chunk_sent);
    c->snapshot = NULL;
    c->chunk_sent = NULL;
    c->sending = false;
}

// Packs the world as it is now, before the next tick, to send it.
static void start_snapshot(tide_server *s, connection *c)
{
    stop_sending(c);
    const uint32_t bound = tide_zeros_bound(s->game->world_size);
    c->snapshot = malloc(bound);
    if (!c->snapshot) return;
    c->snapshot_size = tide_zeros_pack(s->world, s->game->world_size, c->snapshot, bound);
    c->chunk_count = (c->snapshot_size + CHUNK - 1u) / CHUNK;
    c->chunk_sent = calloc(c->chunk_count ? c->chunk_count : 1u, sizeof *c->chunk_sent);
    c->snapshot_tick = s->tick;
    c->chunk_ack = 0;
    c->chunk_mask = 0;
    c->sending = true;
    c->frame_ack = s->tick;
    c->frame_mask = 0;
    memset(c->frame_sent, 0, s->w.history * sizeof *c->frame_sent);
}

static void free_connection(connection *c)
{
    stop_sending(c);
    free(c->inputs);
    free(c->input_tick);
    free(c->input_size);
    free(c->frame_sent);
    memset(c, 0, sizeof *c);
}

static void drop_connection(tide_server *s, connection *c)
{
    const uint32_t player = (uint32_t)(c - s->connections);
    add_event(s, EVENT_LEAVE, player);
    s->away_since[player] = s->now > 0.0 ? s->now : 1e-9;
    free_connection(c);
}

static void send_bye(const tide_server *s, const uint32_t transport, const tide_address to)
{
    uint8_t data[8];
    tide_writer w = {data, sizeof data, 0, false};
    header(&w, MSG_BYE);
    tide_write_u8(&w, s->ended ? BYE_ENDED : BYE_LEFT);
    send_packet(&s->desc.transports[transport], to, &w);
}

static void refuse(const tide_server *s, const uint32_t transport, const tide_address to, const uint8_t reason)
{
    uint8_t data[8];
    tide_writer w = {data, sizeof data, 0, false};
    header(&w, MSG_REFUSE);
    tide_write_u8(&w, reason);
    send_packet(&s->desc.transports[transport], to, &w);
}

static connection *find_connection(tide_server *s, const uint32_t transport, const tide_address from)
{
    for (uint32_t i = 0; i < TIDE_MAX_PLAYERS; i++) {
        connection *c = &s->connections[i];
        if (c->used && c->transport == transport && tide_address_equal(c->address, from)) return c;
    }
    return NULL;
}

static void on_hello(tide_server *s, const uint32_t transport, const tide_address from, tide_reader *r)
{
    const uint64_t game = tide_read_u64(r);
    const uint32_t nonce = tide_read_u32(r);
    const uint32_t their_time = tide_read_u32(r);
    const uint64_t cookie = tide_read_u64(r);
    if (r->failed) return;
    connection *c = find_connection(s, transport, from);
    if (c && c->nonce == nonce) return; // It's being welcomed already
    if (game != s->game->hash) {
        if (c) drop_connection(s, c);
        refuse(s, transport, from, REFUSE_OTHER_GAME);
        return;
    }
    int32_t player = -1;
    for (uint32_t i = 0; cookie && i < TIDE_MAX_PLAYERS; i++) {
        if (s->cookies[i] == cookie) player = (int32_t)i;
    }
    if (c && (int32_t)(c - s->connections) != player) drop_connection(s, c); // The same machine, starting over

    // The player is still here, on another connection: this one takes over,
    // as if nothing happened, from the world as it is now.
    if (player >= 0 && s->connections[player].used) {
        c = &s->connections[player];
        c->transport = transport;
        c->address = from;
        c->nonce = nonce;
        c->last_heard = s->now;
        c->their_time = their_time;
        c->local = s->desc.local_first && transport == 0;
        for (uint32_t i = 0; i < s->w.inputs; i++) c->input_tick[i] = UINT32_MAX;
        c->newest_input = s->tick;
        start_snapshot(s, c);
        return;
    }
    // A player coming back gets their slot; a new one a slot never used, or
    // failing that, the one away the longest, whose cookie stops working.
    for (uint32_t i = 0; player < 0 && i < TIDE_MAX_PLAYERS; i++) {
        if (!s->connections[i].used && s->cookies[i] == 0) player = (int32_t)i;
    }
    if (player < 0) {
        for (uint32_t i = 0; i < TIDE_MAX_PLAYERS; i++) {
            if (!s->connections[i].used && (player < 0 || s->away_since[i] < s->away_since[player])) player = (int32_t)i;
        }
        if (player >= 0) s->cookies[player] = 0; // Given away
    }
    if (player < 0) {
        refuse(s, transport, from, REFUSE_FULL);
        return;
    }
    if (s->cookies[player] == 0) {
        s->random ^= (uint64_t)from.host << 16 ^ from.port ^ (uint64_t)nonce << 32;
        do s->cookies[player] = next_random(&s->random);
        while (s->cookies[player] == 0);
    }
    s->away_since[player] = 0.0;
    c = &s->connections[player];
    const uint32_t bytes = s->game->max_input_bytes ? s->game->max_input_bytes : 1u;
    *c = (connection){.used = true, .transport = transport, .address = from, .nonce = nonce, .last_heard = s->now,
                      .their_time = their_time};
    c->local = s->desc.local_first && transport == 0;
    c->inputs = malloc((size_t)(s->w.inputs + 1u) * bytes);
    c->input_tick = malloc(s->w.inputs * sizeof *c->input_tick);
    c->input_size = calloc(s->w.inputs, sizeof *c->input_size);
    c->frame_sent = calloc(s->w.history, sizeof *c->frame_sent);
    if (!c->inputs || !c->input_tick || !c->input_size || !c->frame_sent) {
        free_connection(c);
        refuse(s, transport, from, REFUSE_FULL);
        return;
    }
    for (uint32_t i = 0; i < s->w.inputs; i++) c->input_tick[i] = UINT32_MAX;
    c->newest_input = s->tick;
    if (!(s->present >> player & 1u)) add_event(s, EVENT_JOIN, (uint32_t)player); // Else it's in the world already
    s->present &= ~(1u << player);
    start_snapshot(s, c);
    if (!s->started) { // The first player: the match starts with them
        s->started = true;
        s->clock_start = s->now;
        s->clock_base = s->tick;
    }
}

static void on_client(tide_server *s, connection *c, tide_reader *r)
{
    const uint32_t their_time = tide_read_u32(r);
    const uint32_t echo = tide_read_u32(r);
    const uint8_t flags = tide_read_u8(r);
    const uint32_t chunk_tick = tide_read_u32(r);
    const uint32_t chunk_ack = tide_read_u32(r);
    const uint64_t chunk_mask = tide_read_u64(r);
    const uint32_t frame_ack = tide_read_u32(r);
    const uint32_t frame_mask = tide_read_u32(r);
    const uint32_t first_input = tide_read_u32(r);
    const uint8_t input_count = tide_read_u8(r);
    if (r->failed) return;
    c->last_heard = s->now;
    c->their_time = their_time;
    if (echo) c->rtt_ms = millis(s->now) - echo;

    if (c->sending) {
        if (chunk_tick == c->snapshot_tick) { // About the world being sent, not an older one
            if (chunk_ack > c->chunk_ack) c->chunk_ack = chunk_ack < c->chunk_count ? chunk_ack : c->chunk_count;
            if (chunk_ack == c->chunk_ack) c->chunk_mask = chunk_mask;
        }
        if (c->chunk_ack >= c->chunk_count) stop_sending(c); // It has the world: ticks from here
    } else if (flags & 1u) {
        start_snapshot(s, c); // Its world went wrong: send it again
    } else {
        if (frame_ack > c->frame_ack && frame_ack <= s->tick) c->frame_ack = frame_ack;
        if (frame_ack == c->frame_ack) c->frame_mask = frame_mask;
    }

    const uint32_t max = s->game->max_input_bytes;
    for (uint32_t i = 0; i < input_count; i++) {
        const uint16_t size = tide_read_u16(r);
        const uint8_t *bytes = tide_read_bytes(r, size);
        if (!bytes) return;
        const uint32_t t = first_input + i;
        if (t < s->tick || t - s->tick >= s->w.inputs || size > max || size == 0) continue; // Late, or much too early
        const uint32_t slot = t % s->w.inputs;
        c->input_tick[slot] = t;
        c->input_size[slot] = size;
        memcpy(c->inputs + (size_t)slot * max, bytes, size);
        if (t + 1u > c->newest_input) c->newest_input = t + 1u;
    }
}

static void server_receive(tide_server *s, const uint32_t transport)
{
    const tide_transport *t = &s->desc.transports[transport];
    if (!t->receive) return;
    uint8_t data[TIDE_NET_MTU];
    tide_address from;
    uint32_t size;
    while ((size = t->receive(t->self, &from, data, sizeof data)) > 0) {
        tide_reader r = {data, size, 0, false};
        const uint8_t type = read_header(&r);
        if (type == MSG_HELLO) {
            if (s->ended) send_bye(s, transport, from); // Too late to join
            else on_hello(s, transport, from, &r);
            continue;
        }
        connection *c = find_connection(s, transport, from);
        if (!c) continue;
        if (type == MSG_CLIENT) on_client(s, c, &r);
        else if (type == MSG_BYE) drop_connection(s, c);
    }
}

// Runs one tick: joins and leaves, then the inputs that arrived for it, in
// slot order, then the systems. Clients do exactly the same with the frame.
static void server_tick(tide_server *s)
{
    const tide_game *g = s->game;
    const uint32_t tick = s->tick;
    tide_writer w = {s->frame, frame_capacity(g), 0, false};
    tide_write_u32(&w, tick);
    tide_write_u64(&w, 0); // The hash, once the tick has run
    tide_write_u8(&w, (uint8_t)s->event_count);
    for (uint32_t i = 0; i < s->event_count; i++) {
        const tide_player_id player = tide_player_from_index(s->events[i][1]);
        if (s->events[i][0] == EVENT_JOIN) g->player_joined(s->world, player);
        else g->player_left(s->world, player);
        tide_write_u8(&w, s->events[i][0]);
        tide_write_u8(&w, s->events[i][1]);
    }
    s->event_count = 0;

    // Each input that changed. One that's the same as the slot's last is left
    // out: keeping the last input is the same as setting it again.
    const uint32_t mask_at = w.size;
    tide_write_u32(&w, 0);
    tide_write_u32(&w, 0);
    uint32_t mask = 0;
    uint32_t late = 0;
    const uint32_t max = g->max_input_bytes;
    uint8_t *input = s->input;
    uint32_t server_size = 0; // The server's input: its own player's, on the server's machine
    for (uint32_t p = 0; g->set_input && p < TIDE_MAX_PLAYERS; p++) {
        connection *c = &s->connections[p];
        if (!c->used) continue;
        const uint32_t slot = tick % s->w.inputs;
        const uint32_t size = c->input_tick[slot] != tick
                                ? 0u
                                : canonical_input(g, c->inputs + (size_t)slot * max, c->input_size[slot], input, s->packed);
        if (size == 0) {
            late |= 1u << p | (c->local ? 1u << SERVER_SLOT : 0u);
            continue;
        }
        if (c->local) {
            server_size = size;
            memcpy(input + g->input_size, s->packed, size);
        }
        uint8_t *last = c->inputs + (size_t)s->w.inputs * max;
        if (size == c->last_size && memcmp(last, s->packed, size) == 0) continue;
        memcpy(last, s->packed, size);
        c->last_size = size;
        g->set_input(s->world, tide_player_from_index((int32_t)p), input);
        mask |= 1u << p;
        tide_write_u16(&w, (uint16_t)size);
        tide_write_bytes(&w, s->packed, size);
    }
    const uint8_t *server_packed = input + g->input_size;
    if (server_size && !(server_size == s->server_last_size && memcmp(s->server_last, server_packed, server_size) == 0)) {
        memcpy(s->server_last, server_packed, server_size);
        s->server_last_size = server_size;
        g->read_input(server_packed, server_size, input);
        g->set_server_input(s->world, input);
        mask |= 1u << SERVER_SLOT;
        tide_write_u16(&w, (uint16_t)server_size);
        tide_write_bytes(&w, server_packed, server_size);
    }
    patch_u32(s->frame + mask_at, mask);
    patch_u32(s->frame + mask_at + 4u, late);

    g->tick(s->world);
    s->tick++;
    s->ended = g->ended && g->ended(s->world);
    patch_u64(s->frame + 4, g->hash_world(s->world));

    stored_frame *f = &s->frames[tick % s->w.history];
    uint8_t *data = realloc(f->data, w.size);
    if (!data) return;
    memcpy(data, s->frame, w.size);
    *f = (stored_frame){tick, w.size, data};
}

static void send_welcome(const tide_server *s, const connection *c)
{
    uint8_t data[64];
    tide_writer w = {data, sizeof data, 0, false};
    header(&w, MSG_WELCOME);
    tide_write_u32(&w, c->nonce);
    tide_write_u8(&w, (uint8_t)(c - s->connections));
    tide_write_u8(&w, c->local ? 1u : 0u); // Its input is the server's too
    tide_write_u32(&w, s->desc.tick_rate);
    tide_write_u32(&w, c->snapshot_tick);
    tide_write_u32(&w, c->snapshot_size);
    tide_write_u32(&w, c->chunk_count);
    tide_write_u32(&w, millis(s->now));
    tide_write_u32(&w, c->their_time);
    tide_write_u64(&w, s->cookies[c - s->connections]);
    send_packet(&s->desc.transports[c->transport], c->address, &w);
}

static void send_chunks(tide_server *s, connection *c)
{
    const double again = resend_after(c->rtt_ms);
    uint32_t sent = 0;
    for (uint32_t i = c->chunk_ack; i < c->chunk_count && sent < CHUNKS_PER_UPDATE; i++) {
        if (i > c->chunk_ack && i - c->chunk_ack <= 64u && (c->chunk_mask >> (i - c->chunk_ack - 1u) & 1u)) continue;
        if (c->chunk_sent[i] > 0.0 && s->now - c->chunk_sent[i] < again) continue;
        const uint32_t from = i * CHUNK;
        const uint32_t size = c->snapshot_size - from < CHUNK ? c->snapshot_size - from : CHUNK;
        tide_writer w = {s->packet, TIDE_NET_MTU, 0, false};
        header(&w, MSG_CHUNK);
        tide_write_u32(&w, c->snapshot_tick);
        tide_write_u32(&w, i);
        tide_write_u16(&w, (uint16_t)size);
        tide_write_bytes(&w, c->snapshot + from, size);
        send_packet(&s->desc.transports[c->transport], c->address, &w);
        c->chunk_sent[i] = s->now > 0.0 ? s->now : 1e-9;
        sent++;
    }
}

static void start_server_packet(const tide_server *s, const connection *c, tide_writer *w, uint32_t *count_at)
{
    *w = (tide_writer){s->packet, TIDE_NET_MTU, 0, false};
    header(w, MSG_SERVER);
    tide_write_u32(w, millis(s->now));
    tide_write_u32(w, c->their_time);
    tide_write_u32(w, s->tick);
    int32_t margin = (int32_t)(c->newest_input - s->tick);
    if (margin < -30000) margin = -30000;
    if (margin > 30000) margin = 30000;
    tide_write_u16(w, (uint16_t)(int16_t)margin);
    tide_write_u32(w, c->newest_input);
    *count_at = w->size;
    tide_write_u8(w, 0);
}

// The ticks it lacks, in pieces, as many packets as it takes (up to a limit).
static void send_frames(tide_server *s, connection *c)
{
    if (s->tick - c->frame_ack >= s->w.history) { // Too far behind for the ticks kept: the whole world again
        start_snapshot(s, c);
        return;
    }
    // While what it lacks fits in one datagram, each one carries all of it:
    // a lost one costs a frame, not a round trip. Beyond that, what was sent
    // goes again once it should have been acknowledged.
    uint32_t lacking = 0;
    for (uint32_t tick = c->frame_ack; tick < s->tick && lacking <= TIDE_NET_MTU; tick++) {
        const stored_frame *f = &s->frames[tick % s->w.history];
        if (f->tick == tick) lacking += f->size + 11u * ((f->size + PIECE - 1u) / PIECE);
    }
    const double again = lacking + 32u <= TIDE_NET_MTU ? 0.0 : resend_after(c->rtt_ms);
    const tide_transport *t = &s->desc.transports[c->transport];
    tide_writer w;
    uint32_t count_at;
    uint32_t pieces = 0;
    uint32_t packets = 0;
    start_server_packet(s, c, &w, &count_at);
    for (uint32_t tick = c->frame_ack; tick < s->tick && packets < PACKETS_PER_UPDATE; tick++) {
        if (tick > c->frame_ack && tick - c->frame_ack <= 32u && (c->frame_mask >> (tick - c->frame_ack - 1u) & 1u)) continue;
        sent_mark *mark = &c->frame_sent[tick % s->w.history];
        if (mark->tick == tick && mark->at > 0.0 && s->now - mark->at < again) continue;
        const stored_frame *f = &s->frames[tick % s->w.history];
        if (f->tick != tick || !f->data) continue;
        const uint32_t count = (f->size + PIECE - 1u) / PIECE;
        for (uint32_t k = 0; k < count && packets < PACKETS_PER_UPDATE; k++) {
            const uint32_t from = k * PIECE;
            const uint32_t size = f->size - from < PIECE ? f->size - from : PIECE;
            if (w.size + 11u + size > TIDE_NET_MTU || pieces == 255u) {
                s->packet[count_at] = (uint8_t)pieces;
                send_packet(t, c->address, &w);
                packets++;
                start_server_packet(s, c, &w, &count_at);
                pieces = 0;
            }
            tide_write_u32(&w, tick);
            tide_write_u16(&w, (uint16_t)f->size);
            tide_write_u8(&w, (uint8_t)k);
            tide_write_u16(&w, (uint16_t)size);
            tide_write_bytes(&w, f->data + from, size);
            pieces++;
        }
        *mark = (sent_mark){tick, s->now > 0.0 ? s->now : 1e-9};
    }
    s->packet[count_at] = (uint8_t)pieces;
    if (packets < PACKETS_PER_UPDATE) send_packet(t, c->address, &w); // Always one, for the times and margin
}

tide_server *tide_server_create(const tide_server_desc *desc, const double now)
{
    tide_server *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->desc = *desc;
    s->game = desc->game;
    if (s->desc.tick_rate == 0) s->desc.tick_rate = 60;
    const tide_game *g = s->game;
    s->w = windows_for(s->desc.tick_rate);
    s->frames = calloc(s->w.history, sizeof *s->frames);
    s->world = calloc(1, g->world_size);
    s->frame = malloc(frame_capacity(g));
    s->packet = malloc(TIDE_NET_MTU);
    s->input = calloc(1, (size_t)g->input_size + g->max_input_bytes + 1u);
    s->packed = malloc((size_t)g->max_input_bytes + 1u);
    s->server_last = malloc((size_t)g->max_input_bytes + 1u);
    if (!s->frames || !s->world || !s->frame || !s->packet || !s->input || !s->packed || !s->server_last) {
        tide_server_destroy(s);
        return NULL;
    }
    const float dt = desc->dt > 0.0f ? desc->dt : 1.0f / (float)s->desc.tick_rate;
    if (desc->world) g->copy_world(s->world, desc->world);
    else g->start(s->world, dt, desc->start);
    s->present = desc->world ? desc->players : 0u;
    s->now = now;
    s->started = !desc->wait_for_first;
    s->clock_start = now;
    s->random = tide_hash(&s, sizeof s) ^ tide_hash(&now, sizeof now);
    return s;
}

void tide_server_destroy(tide_server *s)
{
    if (!s) return;
    for (uint32_t i = 0; i < TIDE_MAX_PLAYERS; i++) {
        connection *c = &s->connections[i];
        if (!c->used) continue;
        send_bye(s, c->transport, c->address);
        free_connection(c);
    }
    for (uint32_t i = 0; i < 2; i++) {
        if (s->desc.transports[i].close) s->desc.transports[i].close(s->desc.transports[i].self);
    }
    for (uint32_t i = 0; s->frames && i < s->w.history; i++) free(s->frames[i].data);
    free(s->frames);
    free(s->world);
    free(s->frame);
    free(s->packet);
    free(s->input);
    free(s->packed);
    free(s->server_last);
    free(s);
}

void tide_server_update(tide_server *s, const double now)
{
    s->now = now;
    for (uint32_t i = 0; i < 2; i++) server_receive(s, i);
    for (uint32_t i = 0; i < TIDE_MAX_PLAYERS; i++) {
        connection *c = &s->connections[i];
        if (c->used && now - c->last_heard > TIMEOUT) drop_connection(s, c);
    }
    if (s->started && !s->ended) {
        const uint64_t due = (uint64_t)s->clock_base + ticks_in(now - s->clock_start, s->desc.tick_rate);
        for (uint32_t n = 0; s->tick < due && n < MAX_TICKS && !s->ended; n++) server_tick(s);
        if (s->tick < due && !s->ended) { // Stalled: drop the time instead of catching up
            s->clock_base = s->tick;
            s->clock_start = now;
        }
    }
    for (uint32_t i = 0; i < TIDE_MAX_PLAYERS; i++) {
        connection *c = &s->connections[i];
        if (!c->used) continue;
        if (s->ended) { // Every update, until they've all gone
            send_bye(s, c->transport, c->address);
        } else if (c->sending) {
            send_welcome(s, c);
            send_chunks(s, c);
        } else {
            send_frames(s, c);
        }
    }
}

const void *tide_server_world(const tide_server *s)
{
    return s->world;
}

uint32_t tide_server_tick(const tide_server *s)
{
    return s->tick;
}

uint32_t tide_server_player_count(const tide_server *s)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < TIDE_MAX_PLAYERS; i++) n += s->connections[i].used ? 1u : 0u;
    return n;
}

// ---------------------------------------------------------------------------
// Client

typedef struct pending_frame {
    uint32_t tick;
    uint32_t size;
    uint32_t have;   // A bit per piece
    uint32_t pieces;
    uint8_t *data;
} pending_frame;

struct tide_client {
    tide_client_desc desc;
    const tide_game *game;
    tide_session_state state;
    tide_disconnect_reason reason;
    uint32_t nonce;
    double created;
    double last_hello;
    double last_heard;
    double now;
    bool welcomed;
    int32_t player;
    bool server_input_mine; // On the server's machine: its input is the server's too
    uint32_t tick_rate;
    uint32_t their_time;
    uint32_t rtt_ms;
    uint32_t input_ack; // The server has every input before this tick

    // The world being received
    bool receiving;
    uint32_t snapshot_tick;
    uint32_t snapshot_size;
    uint32_t chunk_count;
    uint32_t chunks_had;
    uint8_t *snapshot;
    uint8_t *chunk_have;
    bool need_snapshot;
    uint32_t resyncs;
    uint32_t loaded_tick; // The tick of the world it last loaded
    uint64_t cookie;

    // Snapshots: the world at each tick from `verified`, the last the server
    // confirmed, to `ahead`, the last it predicted, at worlds[tick % ring], and
    // the one before them, which views blend from
    bool loaded;
    uint32_t first_tick; // The tick of the world that arrived last: nothing before it
    void **worlds;
    uint32_t ring;
    uint32_t verified;
    uint32_t ahead;
    windows w;              // From the match's tick rate, once welcomed
    pending_frame *frames;  // history

    // Its own inputs, packed, by tick
    uint32_t *input_tick;   // inputs
    uint16_t *input_size;
    uint8_t *inputs;

    // Its clock: the tick it should have reached is clock_base plus the ticks since clock_start
    double clock_start;
    int64_t clock_base;
    int32_t margin; // How early its inputs reach the server, in ticks, as the server last said
    bool margin_known;
    double hold_until; // No clock changes until then: the last one takes a round trip to show

    uint8_t *packet;
    uint8_t *input; // input_size
};

static void go_offline(tide_client *c, const tide_disconnect_reason reason)
{
    if (c->state == TIDE_SESSION_OFFLINE) return;
    c->state = TIDE_SESSION_OFFLINE;
    c->reason = reason;
}

static void stop_receiving(tide_client *c)
{
    free(c->snapshot);
    free(c->chunk_have);
    c->snapshot = NULL;
    c->chunk_have = NULL;
    c->receiving = false;
}

static void on_welcome(tide_client *c, tide_reader *r)
{
    const uint32_t nonce = tide_read_u32(r);
    const uint8_t player = tide_read_u8(r);
    const uint8_t flags = tide_read_u8(r);
    const uint32_t tick_rate = tide_read_u32(r);
    const uint32_t tick = tide_read_u32(r);
    const uint32_t size = tide_read_u32(r);
    const uint32_t chunks = tide_read_u32(r);
    const uint32_t their_time = tide_read_u32(r);
    const uint32_t echo = tide_read_u32(r);
    const uint64_t cookie = tide_read_u64(r);
    if (r->failed || nonce != c->nonce || player >= TIDE_MAX_PLAYERS || tick_rate == 0) return;
    if (!c->frames) { // Its windows, now that it knows the tick rate
        c->w = windows_for(tick_rate);
        c->frames = calloc(c->w.history, sizeof *c->frames);
        c->input_tick = malloc(c->w.inputs * sizeof *c->input_tick);
        c->input_size = calloc(c->w.inputs, sizeof *c->input_size);
        c->inputs = malloc((size_t)c->w.inputs * (c->game->max_input_bytes ? c->game->max_input_bytes : 1u));
        if (!c->frames || !c->input_tick || !c->input_size || !c->inputs) {
            go_offline(c, TIDE_DISCONNECT_FAILED);
            return;
        }
        for (uint32_t i = 0; i < c->w.inputs; i++) c->input_tick[i] = UINT32_MAX;
    }
    c->welcomed = true;
    c->cookie = cookie;
    c->player = player;
    c->server_input_mine = (flags & 1u) != 0;
    c->tick_rate = tick_rate;
    c->their_time = their_time;
    if (echo) c->rtt_ms = millis(c->now) - echo;
    // A world it doesn't have yet: the first, or a newer one after it went wrong
    const bool wanted = !c->loaded || (c->need_snapshot && tick >= c->verified);
    if (!wanted || (c->receiving && c->snapshot_tick == tick)) return;
    stop_receiving(c);
    c->snapshot = malloc(size ? size : 1u);
    c->chunk_have = calloc(chunks ? chunks : 1u, 1);
    if (!c->snapshot || !c->chunk_have) {
        stop_receiving(c);
        return;
    }
    c->receiving = true;
    c->snapshot_tick = tick;
    c->snapshot_size = size;
    c->chunk_count = chunks;
    c->chunks_had = 0;
}

static void load_snapshot(tide_client *c);

static void on_chunk(tide_client *c, tide_reader *r)
{
    const uint32_t tick = tide_read_u32(r);
    const uint32_t index = tide_read_u32(r);
    const uint16_t size = tide_read_u16(r);
    const uint8_t *bytes = tide_read_bytes(r, size);
    if (!bytes || !c->receiving || tick != c->snapshot_tick || index >= c->chunk_count || c->chunk_have[index]) return;
    const uint32_t from = index * CHUNK;
    if (from + size > c->snapshot_size) return;
    memcpy(c->snapshot + from, bytes, size);
    c->chunk_have[index] = 1;
    if (++c->chunks_had == c->chunk_count) load_snapshot(c);
}

static void on_server(tide_client *c, tide_reader *r)
{
    const uint32_t their_time = tide_read_u32(r);
    const uint32_t echo = tide_read_u32(r);
    tide_read_u32(r); // The server's tick
    const int16_t margin = (int16_t)tide_read_u16(r);
    const uint32_t input_ack = tide_read_u32(r);
    const uint8_t pieces = tide_read_u8(r);
    if (r->failed) return;
    c->their_time = their_time;
    if (echo) c->rtt_ms = millis(c->now) - echo;
    c->margin = margin;
    c->margin_known = true;
    if (input_ack > c->input_ack) c->input_ack = input_ack;
    if (!c->loaded) return;
    for (uint32_t i = 0; i < pieces; i++) {
        const uint32_t tick = tide_read_u32(r);
        const uint16_t total = tide_read_u16(r);
        const uint8_t index = tide_read_u8(r);
        const uint16_t size = tide_read_u16(r);
        const uint8_t *bytes = tide_read_bytes(r, size);
        if (!bytes) return;
        if (tick < c->verified || tick - c->verified >= c->w.history || total == 0) continue;
        pending_frame *f = &c->frames[tick % c->w.history];
        if (f->tick != tick || !f->data) {
            free(f->data);
            const uint32_t count = (total + PIECE - 1u) / PIECE;
            if (count > MAX_PIECES) continue;
            *f = (pending_frame){tick, total, 0, count, malloc(total)};
            if (!f->data) continue;
        }
        if (f->size != total || index >= f->pieces || (f->have >> index & 1u)) continue;
        const uint32_t from = (uint32_t)index * PIECE;
        const uint32_t expected = total - from < PIECE ? total - from : PIECE;
        if (size != expected) continue;
        memcpy(f->data + from, bytes, size);
        f->have |= 1u << index;
    }
}

static void client_receive(tide_client *c)
{
    uint8_t data[TIDE_NET_MTU];
    tide_address from;
    uint32_t size;
    const tide_transport *t = &c->desc.transport;
    while (t->receive && (size = t->receive(t->self, &from, data, sizeof data)) > 0) {
        if (!tide_address_equal(from, c->desc.server)) continue;
        tide_reader r = {data, size, 0, false};
        const uint8_t type = read_header(&r);
        if (type != 0 && type != MSG_HELLO) c->last_heard = c->now;
        switch (type) {
        case MSG_WELCOME: on_welcome(c, &r); break;
        case MSG_CHUNK: on_chunk(c, &r); break;
        case MSG_SERVER: on_server(c, &r); break;
        case MSG_REFUSE: go_offline(c, TIDE_DISCONNECT_REFUSED); break;
        case MSG_BYE: {
            const bool ended = tide_read_u8(&r) == BYE_ENDED && !r.failed;
            go_offline(c, ended ? TIDE_DISCONNECT_ENDED : TIDE_DISCONNECT_SERVER_LEFT);
            break;
        }
        default: break;
        }
        if (c->state == TIDE_SESSION_OFFLINE) return;
    }
}

static bool frame_complete(const pending_frame *f, const uint32_t tick)
{
    return f->tick == tick && f->data && f->have == (f->pieces >= 32u ? UINT32_MAX : (1u << f->pieces) - 1u);
}

static const uint8_t *own_input(const tide_client *c, const uint32_t tick, uint32_t *size)
{
    const uint32_t slot = tick % c->w.inputs;
    if (c->input_tick[slot] != tick) return NULL;
    *size = c->input_size[slot];
    return c->inputs + (size_t)slot * c->game->max_input_bytes;
}

// Whether a tick went as it was predicted: no one joined or left, no one
// else's input changed, and this machine's arrived in time, as it was sent (and
// the server's, when that's this machine's too).
static bool as_predicted(const tide_client *c, const pending_frame *f)
{
    tide_reader r = {f->data, f->size, 0, false};
    tide_read_u32(&r);
    tide_read_u64(&r);
    if (tide_read_u8(&r) != 0) return false;
    const uint32_t mask = tide_read_u32(&r);
    const uint32_t late = tide_read_u32(&r);
    if (!c->game->set_input) return mask == 0;
    const uint32_t mine = 1u << c->player | (c->server_input_mine ? 1u << SERVER_SLOT : 0u);
    if ((mask & ~mine) || (late & mine)) return false;
    uint32_t own_size = 0;
    const uint8_t *own = own_input(c, f->tick, &own_size);
    if (!own) return mask == 0;
    for (uint32_t bits = mask; bits; bits &= bits - 1u) {
        const uint16_t size = tide_read_u16(&r);
        const uint8_t *bytes = tide_read_bytes(&r, size);
        if (!bytes || size != own_size || memcmp(bytes, own, size) != 0) return false;
    }
    return !r.failed;
}

static void *world_at(const tide_client *c, const uint32_t tick)
{
    return c->worlds[tick % c->ring];
}

// Room for one more snapshot. The ones in use keep their ticks; the others,
// and new ones, fill the rest.
static bool room_ahead(tide_client *c)
{
    // From the one before `verified` to the one after `ahead`
    if (c->ahead + 2u - c->verified < c->ring) return true;
    const uint32_t most = c->w.prediction + 2u;
    if (c->ring >= most) return false;
    const uint32_t ring = c->ring * 2u > most ? most : c->ring * 2u;
    void **worlds = calloc(ring, sizeof *worlds);
    bool *moved = calloc(c->ring, sizeof *moved);
    if (!worlds || !moved) {
        free(worlds);
        free(moved);
        return false;
    }
    for (uint32_t t = c->verified > c->first_tick ? c->verified - 1u : c->verified; t <= c->ahead; t++) {
        worlds[t % ring] = c->worlds[t % c->ring];
        moved[t % c->ring] = true;
    }
    uint32_t spare = 0;
    for (uint32_t i = 0; i < ring; i++) {
        if (worlds[i]) continue;
        while (spare < c->ring && moved[spare]) spare++;
        worlds[i] = spare < c->ring ? c->worlds[spare++] : calloc(1, c->game->world_size);
        if (!worlds[i]) { // Out of memory: keep what there was
            for (uint32_t k = 0; k < ring; k++) {
                bool old = false;
                for (uint32_t j = 0; j < c->ring && !old; j++) old = worlds[k] == c->worlds[j];
                if (!old) free(worlds[k]);
            }
            free(worlds);
            free(moved);
            return false;
        }
    }
    free(moved);
    free(c->worlds);
    c->worlds = worlds;
    c->ring = ring;
    return true;
}

// The hash a frame says the world has after its tick.
static uint64_t frame_hash(const pending_frame *f)
{
    tide_reader r = {f->data, f->size, 0, false};
    tide_read_u32(&r);
    return tide_read_u64(&r);
}

// Runs a tick the server sent on `world`, the world before it. False if it's
// broken or the world came out different from the server's.
static bool apply_frame(tide_client *c, void *world, const pending_frame *f)
{
    const tide_game *g = c->game;
    tide_reader r = {f->data, f->size, 0, false};
    tide_read_u32(&r);
    const uint64_t hash = tide_read_u64(&r);
    const uint8_t events = tide_read_u8(&r);
    for (uint32_t i = 0; i < events; i++) {
        const uint8_t kind = tide_read_u8(&r);
        const uint8_t player = tide_read_u8(&r);
        if (r.failed || player >= TIDE_MAX_PLAYERS) return false;
        if (kind == EVENT_JOIN) g->player_joined(world, tide_player_from_index(player));
        else g->player_left(world, tide_player_from_index(player));
    }
    const uint32_t mask = tide_read_u32(&r);
    tide_read_u32(&r); // Late ones
    for (uint32_t slot = 0; slot <= SERVER_SLOT; slot++) {
        if (!(mask >> slot & 1u)) continue;
        const uint16_t size = tide_read_u16(&r);
        const uint8_t *bytes = tide_read_bytes(&r, size);
        if (!bytes || !g->read_input || !g->read_input(bytes, size, c->input)) return false;
        if (slot == SERVER_SLOT) g->set_server_input(world, c->input);
        else g->set_input(world, tide_player_from_index((int32_t)slot), c->input);
    }
    if (r.failed) return false;
    g->tick(world);
    return g->hash_world(world) == hash;
}

// Runs predicted tick `tick`: the snapshot after it is the one before it, run
// with this machine's input.
static void run_predicted(tide_client *c, const uint32_t tick)
{
    const tide_game *g = c->game;
    void *world = world_at(c, tick + 1u);
    g->copy_world(world, world_at(c, tick));
    uint32_t size = 0;
    const uint8_t *own = g->set_input ? own_input(c, tick, &size) : NULL;
    if (own && g->read_input(own, size, c->input)) {
        const tide_player_id me = tide_player_from_index(c->player);
        g->set_input(world, me, c->input);
        if (c->server_input_mine) g->set_server_input(world, c->input);
    }
    g->tick(world);
}

// From the verified world, the predicted ticks again.
static void predict_again(tide_client *c)
{
    for (uint32_t t = c->verified; t < c->ahead; t++) run_predicted(c, t);
}

static void load_snapshot(tide_client *c)
{
    // Ahead of the world that came, it keeps its predicted ticks, which it runs again
    const uint32_t tick = c->snapshot_tick;
    if (!c->loaded || c->ahead < tick) c->verified = c->ahead = tick;
    const bool ok = tide_zeros_unpack(c->snapshot, c->snapshot_size, world_at(c, tick), c->game->world_size);
    stop_receiving(c);
    if (!ok) {
        c->need_snapshot = true;
        return;
    }
    const bool first = !c->loaded;
    c->loaded = true;
    c->loaded_tick = tick;
    c->first_tick = tick;
    c->need_snapshot = false;
    c->verified = tick;
    for (uint32_t i = 0; i < c->w.history; i++) {
        if (c->frames[i].tick < tick) {
            free(c->frames[i].data);
            c->frames[i] = (pending_frame){0};
        }
    }
    if (first) {
        c->state = TIDE_SESSION_CONNECTED;
        c->ahead = tick;
        // Ahead of the server by its lead, and the round trip it takes to get there
        const uint32_t trip = (uint32_t)ticks_in((double)c->rtt_ms / 1000.0, c->tick_rate);
        c->clock_start = c->now;
        c->clock_base = (int64_t)tick + c->desc.lead + (c->desc.lead ? trip : 0u);
        c->hold_until = c->now + (double)c->rtt_ms / 1000.0 + 3.0 / c->tick_rate;
    } else {
        c->resyncs++;
        if (c->ahead > tick) predict_again(c);
    }
}

// Runs one tick ahead: this machine's input goes in, and out to the server.
static void predict(tide_client *c)
{
    const tide_game *g = c->game;
    const uint32_t tick = c->ahead;
    if (g->set_input && c->desc.sample) {
        c->desc.sample(c->desc.user, tick, c->input);
        const uint32_t slot = tick % c->w.inputs;
        uint8_t *packed = c->inputs + (size_t)slot * g->max_input_bytes;
        const uint32_t size = g->write_input(c->input, packed, g->max_input_bytes);
        c->input_tick[slot] = size ? tick : UINT32_MAX;
        c->input_size[slot] = (uint16_t)size;
    }
    run_predicted(c, tick);
    c->ahead++;
}

static void play(tide_client *c)
{
    // Ticks the server confirmed. One that went as predicted is already in its
    // snapshot, which only needs its hash checked. One that didn't runs on the
    // snapshot before it, and the predicted ones after it are stale.
    bool stale = false;
    while (!c->need_snapshot && !c->receiving) {
        const uint32_t tick = c->verified;
        pending_frame *f = &c->frames[tick % c->w.history];
        if (!frame_complete(f, tick)) break;
        bool ok;
        if (tick < c->ahead && !stale && as_predicted(c, f)) {
            ok = c->game->hash_world(world_at(c, tick + 1u)) == frame_hash(f);
        } else {
            c->game->copy_world(world_at(c, tick + 1u), world_at(c, tick));
            ok = apply_frame(c, world_at(c, tick + 1u), f);
            stale |= tick + 1u < c->ahead;
        }
        free(f->data);
        *f = (pending_frame){0};
        if (!ok) { // Diverged: ask for the world again, and keep predicting meanwhile
            c->need_snapshot = true;
            break;
        }
        c->verified++;
        if (c->ahead < c->verified) c->ahead = c->verified;
    }
    if (stale && c->ahead > c->verified) predict_again(c);

    // Keep its inputs the right number of ticks early: early enough to arrive
    // in time, and no earlier, since further ahead is more to predict. A client
    // that sends none (a game without an input) has no margin to keep: its
    // clock stays as the world's arrival set it.
    const int32_t low = (int32_t)c->desc.lead;
    const int32_t high = low + 2;
    const bool sends_inputs = c->game->set_input && c->desc.sample;
    if (sends_inputs && c->margin_known && c->now >= c->hold_until && (c->margin < low || c->margin > high + 2)) {
        c->clock_base += c->margin < low ? low - c->margin : high - c->margin;
        c->hold_until = c->now + (double)c->rtt_ms / 1000.0 + 3.0 / c->tick_rate;
    }
    const int64_t due = c->clock_base + (int64_t)ticks_in(c->now - c->clock_start, c->tick_rate);
    for (uint32_t n = 0; (int64_t)c->ahead < due && c->ahead - c->verified < c->w.prediction && n < 16u && room_ahead(c); n++) {
        predict(c);
    }
}

static void send_hello(tide_client *c)
{
    uint8_t data[32];
    tide_writer w = {data, sizeof data, 0, false};
    header(&w, MSG_HELLO);
    tide_write_u64(&w, c->game->hash);
    tide_write_u32(&w, c->nonce);
    tide_write_u32(&w, millis(c->now));
    tide_write_u64(&w, c->desc.cookie);
    send_packet(&c->desc.transport, c->desc.server, &w);
    c->last_hello = c->now;
}

static void send_client(tide_client *c)
{
    tide_writer w = {c->packet, TIDE_NET_MTU, 0, false};
    header(&w, MSG_CLIENT);
    tide_write_u32(&w, millis(c->now));
    tide_write_u32(&w, c->their_time);
    tide_write_u8(&w, c->need_snapshot && !c->receiving ? 1u : 0u);
    // The chunks it has of the world being received, or that it has all of the last one
    uint32_t chunk_tick = 0;
    uint32_t chunk_ack = 0;
    uint64_t chunk_mask = 0;
    if (c->receiving) {
        chunk_tick = c->snapshot_tick;
        while (chunk_ack < c->chunk_count && c->chunk_have[chunk_ack]) chunk_ack++;
        for (uint32_t i = 0; i < 64u && chunk_ack + 1u + i < c->chunk_count; i++) {
            if (c->chunk_have[chunk_ack + 1u + i]) chunk_mask |= (uint64_t)1 << i;
        }
    } else if (c->loaded) {
        chunk_tick = c->loaded_tick;
        chunk_ack = UINT32_MAX;
    }
    tide_write_u32(&w, chunk_tick);
    tide_write_u32(&w, chunk_ack);
    tide_write_u64(&w, chunk_mask);
    uint32_t frame_mask = 0;
    for (uint32_t i = 0; i < 32u; i++) {
        const uint32_t tick = c->verified + 1u + i;
        if (frame_complete(&c->frames[tick % c->w.history], tick)) frame_mask |= 1u << i;
    }
    tide_write_u32(&w, c->verified);
    tide_write_u32(&w, frame_mask);

    // Inputs from the first the server lacks, as many as fit
    uint32_t first = c->input_ack > c->verified ? c->input_ack : c->verified;
    if (first > c->ahead) first = c->ahead;
    tide_write_u32(&w, first);
    const uint32_t count_at = w.size;
    tide_write_u8(&w, 0);
    uint32_t count = 0;
    for (uint32_t t = first; t < c->ahead && count < 255u; t++) {
        uint32_t size = 0;
        const uint8_t *own = own_input(c, t, &size);
        if (!own || w.size + 2u + size > TIDE_NET_MTU) break;
        tide_write_u16(&w, (uint16_t)size);
        tide_write_bytes(&w, own, size);
        count++;
    }
    c->packet[count_at] = (uint8_t)count;
    send_packet(&c->desc.transport, c->desc.server, &w);
}

tide_client *tide_client_create(const tide_client_desc *desc, const double now)
{
    tide_client *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->desc = *desc;
    c->game = desc->game;
    const tide_game *g = c->game;
    c->state = TIDE_SESSION_CONNECTING;
    c->created = now;
    c->now = now;
    c->last_heard = now;
    c->last_hello = -1.0;
    c->nonce = (uint32_t)(tide_hash(&c, sizeof c) ^ tide_hash(&now, sizeof now)) | 1u;
    c->ring = FIRST_RING;
    c->worlds = calloc(c->ring, sizeof *c->worlds);
    for (uint32_t i = 0; c->worlds && i < c->ring; i++) c->worlds[i] = calloc(1, g->world_size);
    c->input = calloc(1, (size_t)g->input_size + 1u);
    c->packet = malloc(TIDE_NET_MTU);
    bool worlds = c->worlds != NULL;
    for (uint32_t i = 0; worlds && i < c->ring; i++) worlds = c->worlds[i] != NULL;
    if (!worlds || !c->input || !c->packet) {
        tide_client_destroy(c);
        return NULL;
    }
    return c;
}

void tide_client_destroy(tide_client *c)
{
    if (!c) return;
    if (c->state != TIDE_SESSION_OFFLINE && c->desc.transport.send) {
        uint8_t data[8];
        tide_writer w = {data, sizeof data, 0, false};
        header(&w, MSG_BYE);
        send_packet(&c->desc.transport, c->desc.server, &w);
    }
    if (c->desc.transport.close) c->desc.transport.close(c->desc.transport.self);
    stop_receiving(c);
    for (uint32_t i = 0; c->frames && i < c->w.history; i++) free(c->frames[i].data);
    free(c->frames);
    free(c->input_tick);
    free(c->input_size);
    for (uint32_t i = 0; c->worlds && i < c->ring; i++) free(c->worlds[i]);
    free(c->worlds);
    free(c->inputs);
    free(c->input);
    free(c->packet);
    free(c);
}

void tide_client_update(tide_client *c, const double now)
{
    c->now = now;
    if (c->state == TIDE_SESSION_OFFLINE) return;
    client_receive(c);
    if (c->state == TIDE_SESSION_OFFLINE) return;
    const double patience = !c->welcomed && c->desc.server.kind == TIDE_ADDRESS_ROOM ? ROOM_TIMEOUT : TIMEOUT;
    if (now - c->last_heard > patience) {
        go_offline(c, TIDE_DISCONNECT_TIMED_OUT);
        return;
    }
    if (!c->welcomed) {
        if (now - c->last_hello >= HELLO_EVERY) send_hello(c);
        return;
    }
    if (c->loaded) play(c);
    send_client(c);
}

tide_client_status tide_client_status_of(const tide_client *c)
{
    return (tide_client_status){
        .state = c->state,
        .reason = c->reason,
        .player = c->welcomed ? tide_player_from_index(c->player) : (tide_player_id){0},
        .ping_ms = c->rtt_ms,
        .verified_tick = c->verified,
        .predicted_tick = c->ahead,
        .resyncs = c->resyncs,
        .cookie = c->cookie,
    };
}

const void *tide_client_world(const tide_client *c)
{
    return c->loaded ? world_at(c, c->ahead) : NULL;
}

const void *tide_client_verified_world(const tide_client *c)
{
    return c->loaded ? world_at(c, c->verified) : NULL;
}

tide_view_worlds tide_client_view(const tide_client *c)
{
    if (!c->loaded) return (tide_view_worlds){0};
    // How many ticks into the match this moment is, by the client's clock:
    // the latest tick it ran is at most one of them behind.
    const double at = (double)c->clock_base + (c->now - c->clock_start) * (double)c->tick_rate;
    double alpha = at - (double)c->ahead;
    if (alpha < 0.0) alpha = 0.0;
    if (alpha > 1.0) alpha = 1.0;
    return (tide_view_worlds){world_at(c, c->ahead), c->ahead > c->first_tick ? world_at(c, c->ahead - 1u) : NULL,
                              (float)alpha};
}

// ---------------------------------------------------------------------------
// Session

#define SESSION_EVENTS 8u

struct tide_session {
    tide_session_desc desc;
    tide_loopback *loopback;
    tide_server *server;
    tide_client *client;
    tide_session_state last_state;
    tide_address joined;  // The server it last joined, and the cookie that makes it the same player there
    uint64_t cookie;
    tide_session_event events[SESSION_EVENTS];
    uint32_t event_count;
    double last_now; // The host's time at the last update
    double paused;   // Host time that didn't pass for this machine's own match (see tide_session_update)
};

// The session's own time starts over with each match.
static void start_clock(tide_session *s, const double now)
{
    s->last_now = now;
    s->paused = 0.0;
}

static void push_event(tide_session *s, const tide_session_event e)
{
    if (s->event_count < SESSION_EVENTS) s->events[s->event_count++] = e;
}

// Ends whatever it's in, without a word to local code.
static void tear_down(tide_session *s)
{
    tide_client_destroy(s->client);
    tide_server_destroy(s->server);
    tide_loopback_destroy(s->loopback);
    s->client = NULL;
    s->server = NULL;
    s->loopback = NULL;
    s->last_state = TIDE_SESSION_OFFLINE;
}

tide_session *tide_session_create(const tide_session_desc *desc)
{
    tide_session *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->desc = *desc;
    if (s->desc.tick_rate == 0) s->desc.tick_rate = 60;
    return s;
}

void tide_session_destroy(tide_session *s)
{
    if (!s) return;
    tear_down(s);
    free(s);
}

void tide_session_fail(tide_session *s, const tide_disconnect_reason reason)
{
    tear_down(s);
    push_event(s, (tide_session_event){TIDE_SESSION_DISCONNECTED_EVENT, reason});
}

void tide_session_leave(tide_session *s)
{
    if (!s->client && !s->server) return;
    tear_down(s);
    push_event(s, (tide_session_event){TIDE_SESSION_DISCONNECTED_EVENT, TIDE_DISCONNECT_LEFT});
}

// A server with this machine's player on it, over loopback; `network` takes others.
static void start_server(tide_session *s, const void *start, const void *world, const uint32_t players,
                         const tide_transport network, const double now)
{
    tide_session_leave(s);
    start_clock(s, now);
    s->loopback = tide_loopback_create(1);
    if (!s->loopback) {
        if (network.close) network.close(network.self);
        tide_session_fail(s, TIDE_DISCONNECT_FAILED);
        return;
    }
    tide_loopback_set_time(s->loopback, now);
    const tide_server_desc server = {
        .game = s->desc.game,
        .tick_rate = s->desc.tick_rate,
        .start = start,
        .transports = {tide_loopback_endpoint(s->loopback, 1), network},
        .local_first = true,
        .wait_for_first = true,
        .world = world,
        .players = players,
    };
    s->server = tide_server_create(&server, now);
    const tide_client_desc client = {
        .game = s->desc.game,
        .transport = tide_loopback_endpoint(s->loopback, 2),
        .server = tide_loopback_address(1),
        .sample = s->desc.sample,
        .user = s->desc.user,
        .lead = 0, // Its inputs go straight in: the server ticks right after it
    };
    s->client = s->server ? tide_client_create(&client, now) : NULL;
    if (!s->client) {
        tear_down(s);
        tide_session_fail(s, TIDE_DISCONNECT_FAILED);
        return;
    }
    s->last_state = TIDE_SESSION_CONNECTING;
}

void tide_session_play(tide_session *s, const void *start, const double now)
{
    start_server(s, start, NULL, 0, (tide_transport){0}, now);
}

void tide_session_play_from(tide_session *s, const void *world, const uint32_t players, const double now)
{
    start_server(s, NULL, world, players, (tide_transport){0}, now);
}

void tide_session_host(tide_session *s, const void *start, const tide_transport network, const double now)
{
    start_server(s, start, NULL, 0, network, now);
}

void tide_session_join(tide_session *s, const tide_transport network, const tide_address server, const double now)
{
    tide_session_leave(s);
    start_clock(s, now);
    if (!tide_address_equal(server, s->joined)) s->cookie = 0;
    s->joined = server;
    const tide_client_desc client = {
        .game = s->desc.game,
        .transport = network,
        .server = server,
        .sample = s->desc.sample,
        .user = s->desc.user,
        .lead = 2,
        .cookie = s->cookie,
    };
    s->client = tide_client_create(&client, now);
    if (!s->client) {
        if (network.close) network.close(network.self);
        tide_session_fail(s, TIDE_DISCONNECT_FAILED);
        return;
    }
    s->last_state = TIDE_SESSION_CONNECTING;
}

void tide_session_update(tide_session *s, const double now)
{
    if (!s->client) return;
    // This machine's server stops whenever the machine does (a browser tab in
    // the background, a breakpoint), and its player with it. Beyond the ticks
    // the server can run in one update, which it would drop anyway, that time
    // didn't pass for the match: neither side went quiet, and there's nothing
    // to catch up. A client of another machine keeps to the real time.
    const double most = (double)MAX_TICKS / (double)s->desc.tick_rate;
    if (s->server && now - s->last_now > most) s->paused += now - s->last_now - most;
    s->last_now = now;
    const double t = now - s->paused;

    if (s->loopback) tide_loopback_set_time(s->loopback, t);
    tide_client_update(s->client, t);
    if (s->server) tide_server_update(s->server, t);
    tide_client_update(s->client, t); // What the server just sent: this machine's ticks, at once

    const tide_client_status status = tide_client_status_of(s->client);
    if (!s->server && status.cookie) s->cookie = status.cookie;
    if (status.state == TIDE_SESSION_CONNECTED && s->last_state != TIDE_SESSION_CONNECTED) {
        push_event(s, (tide_session_event){TIDE_SESSION_CONNECTED_EVENT, TIDE_DISCONNECT_LEFT});
    }
    if (status.state == TIDE_SESSION_OFFLINE) {
        tear_down(s);
        push_event(s, (tide_session_event){TIDE_SESSION_DISCONNECTED_EVENT, status.reason});
        return;
    }
    s->last_state = status.state;
}

const void *tide_session_world(const tide_session *s)
{
    return s->client ? tide_client_world(s->client) : NULL;
}

tide_view_worlds tide_session_view(const tide_session *s)
{
    return s->client ? tide_client_view(s->client) : (tide_view_worlds){0};
}

const void *tide_session_server_world(const tide_session *s)
{
    return s->server ? tide_server_world(s->server) : NULL;
}

tide_session_status tide_session_status_of(const tide_session *s)
{
    tide_session_status status = {0};
    if (s->client) status.client = tide_client_status_of(s->client);
    status.server = s->server != NULL;
    return status;
}

bool tide_session_next_event(tide_session *s, tide_session_event *event)
{
    if (s->event_count == 0) return false;
    *event = s->events[0];
    memmove(s->events, s->events + 1, (s->event_count - 1u) * sizeof s->events[0]);
    s->event_count--;
    return true;
}

void tide_session_set_game(tide_session *s, const tide_game *game)
{
    s->desc.game = game;
    if (s->server) {
        s->server->desc.game = game;
        s->server->game = game;
    }
    if (s->client) {
        s->client->desc.game = game;
        s->client->game = game;
    }
}

// What a new layout needs, made before anything changes, so running out of
// memory or a world that can't be carried over leaves the session as it was.
typedef struct server_migration {
    void *world;
    uint8_t *frame;
    uint8_t *input;
    uint8_t *packed;
    uint8_t *server_last;
    uint8_t *inputs[TIDE_MAX_PLAYERS];
} server_migration;

typedef struct client_migration {
    void **worlds;
    uint8_t *input;
    uint8_t *inputs;
} client_migration;

static void discard_server_migration(server_migration *m)
{
    free(m->world);
    free(m->frame);
    free(m->input);
    free(m->packed);
    free(m->server_last);
    for (uint32_t i = 0; i < TIDE_MAX_PLAYERS; i++) free(m->inputs[i]);
}

static void discard_client_migration(const tide_client *c, client_migration *m)
{
    for (uint32_t i = 0; m->worlds && i < c->ring; i++) free(m->worlds[i]);
    free(m->worlds);
    free(m->input);
    free(m->inputs);
}

static bool prepare_server(const tide_server *s, const tide_game *g, const tide_migrate_fn migrate, void *user,
                           server_migration *m)
{
    const uint32_t bytes = g->max_input_bytes ? g->max_input_bytes : 1u;
    m->world = calloc(1, g->world_size);
    m->frame = malloc(frame_capacity(g));
    m->input = calloc(1, (size_t)g->input_size + g->max_input_bytes + 1u);
    m->packed = malloc((size_t)g->max_input_bytes + 1u);
    m->server_last = malloc((size_t)g->max_input_bytes + 1u);
    bool ok = m->world && m->frame && m->input && m->packed && m->server_last;
    for (uint32_t i = 0; ok && i < TIDE_MAX_PLAYERS; i++) {
        if (!s->connections[i].used) continue;
        m->inputs[i] = malloc((size_t)(s->w.inputs + 1u) * bytes);
        ok = m->inputs[i] != NULL;
    }
    return ok && migrate(user, s->world, m->world);
}

// Inputs sent for ticks to come were packed for the old layout: the players'
// last ones stand for them until new ones arrive.
static void commit_server(tide_server *s, const tide_game *g, server_migration *m)
{
    free(s->world);
    free(s->frame);
    free(s->input);
    free(s->packed);
    free(s->server_last);
    s->world = m->world;
    s->frame = m->frame;
    s->input = m->input;
    s->packed = m->packed;
    s->server_last = m->server_last;
    s->server_last_size = 0;
    s->game = g;
    s->desc.game = g;
    for (uint32_t i = 0; i < TIDE_MAX_PLAYERS; i++) {
        connection *c = &s->connections[i];
        if (!c->used) continue;
        free(c->inputs);
        c->inputs = m->inputs[i];
        for (uint32_t k = 0; k < s->w.inputs; k++) c->input_tick[k] = UINT32_MAX;
        c->last_size = 0;
        c->newest_input = s->tick;
        if (c->sending) start_snapshot(s, c); // The world being sent was the old one
    }
}

static bool prepare_client(const tide_client *c, const tide_game *g, const tide_migrate_fn migrate, void *user,
                           client_migration *m)
{
    const uint32_t bytes = g->max_input_bytes ? g->max_input_bytes : 1u;
    m->worlds = calloc(c->ring, sizeof *m->worlds);
    bool ok = m->worlds != NULL;
    for (uint32_t i = 0; ok && i < c->ring; i++) {
        m->worlds[i] = calloc(1, g->world_size);
        ok = m->worlds[i] != NULL;
    }
    m->input = calloc(1, (size_t)g->input_size + 1u);
    ok = ok && m->input;
    if (ok && c->frames) {
        m->inputs = malloc((size_t)c->w.inputs * bytes);
        ok = m->inputs != NULL;
    }
    return ok && (!c->loaded || !migrate || migrate(user, world_at(c, c->verified), m->worlds[c->verified % c->ring]));
}

// It goes on from the verified world: the ticks it predicted, and its inputs
// for them, were the old build's.
static void commit_client(tide_client *c, const tide_game *g, client_migration *m)
{
    for (uint32_t i = 0; i < c->ring; i++) free(c->worlds[i]);
    free(c->worlds);
    c->worlds = m->worlds;
    free(c->input);
    c->input = m->input;
    if (c->frames) {
        free(c->inputs);
        c->inputs = m->inputs;
        for (uint32_t i = 0; i < c->w.inputs; i++) c->input_tick[i] = UINT32_MAX;
    }
    stop_receiving(c); // A world it was receiving is the old build's: the server sends it again
    c->game = g;
    c->desc.game = g;
    if (c->loaded) {
        c->ahead = c->verified;
        c->first_tick = c->verified; // Views don't blend from the old build's world
    }
}

// This machine's own player takes its server's world as it is now, which is
// what it would come to.
static void take_server_world(tide_client *c, const tide_server *s)
{
    c->game->copy_world(world_at(c, s->tick), s->world);
    c->verified = s->tick;
    c->ahead = s->tick;
    c->first_tick = s->tick;
    c->loaded_tick = s->tick;
}

bool tide_session_migrate(tide_session *s, const tide_game *game, const tide_migrate_fn migrate, void *user)
{
    const bool own = s->server && s->client; // This machine's player, on its own server
    server_migration sm = {0};
    client_migration cm = {0};
    if (s->server && !prepare_server(s->server, game, migrate, user, &sm)) {
        discard_server_migration(&sm);
        return false;
    }
    if (s->client && !prepare_client(s->client, game, own ? NULL : migrate, user, &cm)) {
        discard_server_migration(&sm);
        discard_client_migration(s->client, &cm);
        return false;
    }
    if (s->server) commit_server(s->server, game, &sm);
    if (s->client) commit_client(s->client, game, &cm);
    if (own && s->client->loaded) take_server_world(s->client, s->server);
    s->desc.game = game;
    return true;
}
