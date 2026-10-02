#include "tide/session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tide/page.h"
#include "tide/sha256.h"

// See tide/session.h.
//
// Every datagram starts with a header: "PU", the protocol version and a type.
//
//   HELLO    client: the game's hash, a number for this attempt, its time,
//            its cookie from before, if any, whether it knows the server
//            kicked it last time, and whether it has a world to build the
//            match's from
//   WELCOME  server: the attempt's number, the player, the tick rate, its
//            tick, what's being sent: its tick, number, kind, size and number
//            of chunks, and the player's cookie
//   REFUSE   server: why
//   CHUNK    server: a piece of what's being sent, by its number
//   CLIENT   client: times, whether it needs the world (and has one to build
//            it from), the chunks it has of what's being sent, the ticks it
//            has, what its world lacks of the one whose hashes it was sent,
//            and its inputs from the first the server lacks
//   SERVER   server: times, its tick, the newest input it has of the player's
//            (from its tick: how early they arrive), and pieces of the ticks
//            the player lacks, each by how many ticks back it is
//
// Times are milliseconds, kept to 16 bits: they only measure round trips and
// order a side's datagrams, which are never seconds apart on a connection that
// hasn't timed out. Numbers that are mostly small go as varints.
//   BYE      either: leaving; the server's says whether the match ended, or
//            goes on with another machine as its server
//   HANDOVER server, with host migration: its room's code and key, its own
//            player, whether it's open, the players in the match and their
//            cookies' digests, numbered so a client keeps the newest
//
// Nothing is sent reliably as such: each side says what it has, and the other
// sends what's missing again until it does.
//
// A tick, as the server sends it (a "frame"): the hash of the world after it,
// which of the rest it has (FRAME_*), the players who joined or left before
// it, and the inputs that changed for it: a bit per slot (players, then the
// server) and each input in slot order, as what differs from the slot's input
// as the world keeps it before the tick (tide_game.write_input_delta). A
// CLIENT's inputs go the same way, each but the first from the one before it.
// A slot without one keeps its last input, on every machine, whether it didn't
// change or didn't arrive in time; a second set of bits says which ones were
// late, so a client knows whether its prediction held.
//
// The world goes as a delta (tide/delta.h), in chunks. A player with no world
// gets all of it. One with a world to build it from (its own, gone wrong, or
// the last one it had of a match that changed hands) first gets the hashes of
// the world's pages, says which of them its world lacks, and gets those.

#define MAGIC 0x5449u // "TI"
#define PROTOCOL 5u

enum { MSG_HELLO = 1, MSG_WELCOME, MSG_REFUSE, MSG_CHUNK, MSG_CLIENT, MSG_SERVER, MSG_BYE, MSG_HANDOVER };
enum { REFUSE_OTHER_GAME = 1, REFUSE_FULL = 2, REFUSE_CLOSED = 3 };
enum { EVENT_JOIN = 1, EVENT_LEAVE = 2 };
// KICKED: then the message's length and bytes. HANDOVER: the server left, and
// the match goes on with another machine as its server.
enum { BYE_LEFT = 0, BYE_ENDED = 1, BYE_KICKED = 2, BYE_HANDOVER = 3 };
enum {
    HELLO_KNEW_KICK = 1, // It heard it was kicked from this server: the server takes it again
    HELLO_HAS_WORLD = 2, // It has a world to build the match's from
    HELLO_CAN_START = 4, // ...or can start the match itself to have one (tide_game.pure_start)
};
enum {
    CLIENT_WANTS_WORLD = 1, // It has none, or its own went wrong, or it's taking the one coming
    CLIENT_HAS_WORLD = 2,   // ...and can build it from what it has
    CLIENT_LACKS = 4,       // What its world lacks of the one whose hashes it was sent follows
    CLIENT_CAN_START = 8,   // ...or from a match it starts itself
};
// What a frame has after its hash, most often nothing: the players who joined
// or left, a bit per slot whose input changed, and one per slot whose was late.
enum { FRAME_EVENTS = 1, FRAME_INPUTS = 2, FRAME_LATE = 4 };
// What's being sent: the world, as a delta, or the hashes of its pages, to
// learn which of them the player's world lacks. The hashes come after how the
// match started: one of START_*, Time.dt's bits and, for START_GIVEN, the
// game's tide_start.
enum { SEND_WORLD = 0, SEND_HASHES = 1 };
enum { START_UNKNOWN = 0, START_MAIN = 1, START_GIVEN = 2 };
#define LACKS_BYTES 512u // The most a CLIENT says about what its world lacks: past that, it lacks the rest

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
#define PRESENT_SECONDS 20.0  // How long players of a world a server went on from have to come back
#define HANDOVER_EVERY 1.0    // Seconds between HANDOVERs, once it's been the same that long

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

// Lets go of what a world has, before its memory goes. NULL does nothing.
static void free_world(const tide_game *g, void *world)
{
    if (world && g->free_world) g->free_world(world);
}

// What players share of a cookie for host migration: the first bytes of its
// SHA-256, which nobody can work the cookie back from. 0 for none.
static uint64_t cookie_digest(const uint64_t cookie)
{
    if (!cookie) return 0;
    uint8_t bytes[8];
    for (int i = 0; i < 8; i++) bytes[i] = (uint8_t)(cookie >> (8 * i));
    uint8_t hash[32];
    tide_sha256_of(bytes, sizeof bytes, hash);
    uint64_t digest = 0;
    for (int i = 0; i < 8; i++) digest |= (uint64_t)hash[i] << (8 * i);
    return digest ? digest : 1u;
}

static uint32_t millis(const double t)
{
    return t > 0.0 ? (uint32_t)(uint64_t)(t * 1000.0) : 0u;
}

// Milliseconds as they're sent, in 16 bits, and the time since one of them.
static uint16_t millis16(const double t)
{
    return (uint16_t)millis(t);
}

static uint32_t millis_since(const double now, const uint16_t then)
{
    return (uint16_t)(millis16(now) - then);
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

// A signed number as a varint, its sign in the lowest bit: small ones of
// either sign take a byte.
static void write_signed(tide_writer *w, const int32_t v)
{
    tide_write_varint(w, (uint32_t)v << 1 ^ (v < 0 ? UINT32_MAX : 0u));
}

static int32_t read_signed(tide_reader *r)
{
    const uint32_t u = tide_read_varint(r);
    return (int32_t)(u >> 1 ^ (0u - (u & 1u)));
}

static uint32_t varint_size(uint32_t v)
{
    uint32_t n = 1;
    for (; v >= 0x80u; v >>= 7) n++;
    return n;
}

static void patch_u32(uint8_t *at, const uint32_t v)
{
    for (int i = 0; i < 4; i++) at[i] = (uint8_t)(v >> (8 * i));
}

static void patch_u64(uint8_t *at, const uint64_t v)
{
    for (int i = 0; i < 8; i++) at[i] = (uint8_t)(v >> (8 * i));
}

// The most a frame can take: its hash, the events, which inputs changed and
// were late, and every slot's input, after its size.
static uint32_t frame_capacity(const tide_game *g)
{
    return 8u + 1u + 2u * MAX_EVENTS + 10u + (TIDE_MAX_PLAYERS + 1u) * (5u + g->max_input_bytes);
}

// Room for two inputs and one packed, whole or as a delta (write_input_delta).
static size_t scratch_size(const tide_game *g)
{
    return 2u * (size_t)g->input_size + g->max_input_bytes + 1u;
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
    uint16_t their_time;
    uint32_t rtt_ms;

    // The world it's being sent
    bool sending;
    bool has_world; // It has a world to build it from: it's sent the hashes first
    bool can_start; // ...or can start the match itself to have one
    uint32_t snapshot_tick;
    uint32_t snapshot_number; // Each thing sent has the next number, from 1
    uint8_t snapshot_kind;    // SEND_WORLD or SEND_HASHES
    uint32_t snapshot_size;
    uint32_t chunk_count;
    uint8_t *snapshot;
    void *hashed; // While the hashes are sent: the world they're of, to send once it says what it lacks
    double *chunk_sent;
    uint32_t chunk_ack;  // It has every chunk before this one
    uint64_t chunk_mask; // ...and these after it

    // Ticks
    uint32_t frame_ack;  // It has every tick before this one
    uint32_t frame_mask; // ...and these after it
    sent_mark *frame_sent; // history
    // It was sent the world and needs every tick from it on, however long
    // that took: the server keeps them until it catches up (keep_needed)
    bool catching_up;

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

// A player the server sent away, and why. A goodbye can be lost, so it tells
// them again whenever they're in touch: when they send from where they were,
// and when they join again with their cookie, from anywhere, as they do to
// take a match over. They're let in again once they come back knowing.
typedef struct kicked {
    bool used;
    uint64_t cookie;      // The player's, if it had one by then
    uint32_t transport;   // Where it was last
    tide_address address;
    uint32_t nonce;       // Its HELLOs, if it was still joining; another one is a new try
    char message[TIDE_MESSAGE_BYTES];
} kicked;

// A kick's message into `out`: as much as fits, cut where a character starts,
// and only while it's UTF-8 (it may come from another machine), stopping at a
// NUL or `size` bytes.
static void copy_message(char out[TIDE_MESSAGE_BYTES], const char *message, const size_t size)
{
    size_t n = 0;
    while (message && n < size && message[n]) {
        const unsigned char lead = (unsigned char)message[n];
        const size_t length = lead < 0x80u ? 1u : lead >= 0xC2u && lead < 0xE0u ? 2u : lead >= 0xE0u && lead < 0xF0u ? 3u
                            : lead >= 0xF0u && lead < 0xF5u ? 4u : 0u;
        bool whole = length > 0 && n + length <= size && n + length < TIDE_MESSAGE_BYTES;
        for (size_t i = 1; whole && i < length; i++) whole = ((unsigned char)message[n + i] & 0xC0u) == 0x80u;
        if (!whole) break;
        n += length;
    }
    if (n) memcpy(out, message, n);
    memset(out + n, 0, TIDE_MESSAGE_BYTES - n);
}

struct tide_server {
    tide_server_desc desc;
    const tide_game *game;
    void *world;
    uint32_t tick;
    bool started;
    bool ended; // The match ran out of scenes (tide_game.ended): no more ticks
    bool closed; // Takes no one new, but the players on this machine (tide_session_close)
    double clock_start;
    uint32_t clock_base;
    double now;
    connection connections[TIDE_MAX_PLAYERS]; // Player n is connections[n]
    uint8_t events[MAX_EVENTS][2];            // Joins and leaves before the next tick
    uint32_t event_count;
    windows w;
    stored_frame *frames; // history
    uint8_t *frame;  // The tick being made, then its inputs as they're made (frame_capacity each)
    uint8_t *packet; // TIDE_NET_MTU
    uint8_t *input;  // An input: input_size, then its packed bytes
    uint8_t *packed; // max_input_bytes
    uint8_t *server_last; // The server's input last set, packed
    uint32_t server_last_size;
    uint8_t *scratch; // scratch_size: inputs and their deltas
    // Each player's cookie, kept after they leave so they can come back, and
    // since when they're away (0: here, or never was)
    uint64_t cookies[TIDE_MAX_PLAYERS];
    uint64_t digests[TIDE_MAX_PLAYERS]; // ...and each one's digest: all a server it went on from passed on
    double away_since[TIDE_MAX_PLAYERS];
    uint64_t random;
    // How its match started, for players to start it too (START_*), with
    // Time.dt and, for START_GIVEN, the game's tide_start
    uint8_t start_kind;
    float dt;
    uint8_t *start;
    uint32_t present; // Players in the world it went on from, who haven't joined it yet (tide_server_desc.players)
    double present_until; // ...who leave then
    kicked kicks[TIDE_MAX_PLAYERS]; // Players sent away, told so every update for a while
    // Host migration: the room its players meet in again when it goes, and
    // what it last told them (see send_handover)
    char room_code[TIDE_ROOM_CODE_LENGTH + 1];
    char room_key[TIDE_ROOM_KEY_LENGTH + 1];
    uint64_t handover_hash;
    uint32_t handover_version;
    double handover_changed;
    double handover_sent;
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

// `g`: the game whose world `hashed` is.
static void stop_sending(const tide_game *g, connection *c)
{
    free(c->snapshot);
    free(c->chunk_sent);
    free_world(g, c->hashed);
    free(c->hashed);
    c->snapshot = NULL;
    c->chunk_sent = NULL;
    c->hashed = NULL;
    c->sending = false;
}

// Sends `bytes` (size `size`, to free()) in chunks, from the first.
static void send_in_chunks(connection *c, const uint8_t kind, uint8_t *bytes, const uint32_t size)
{
    free(c->snapshot);
    free(c->chunk_sent);
    c->snapshot = bytes;
    c->snapshot_number++;
    c->snapshot_kind = kind;
    c->snapshot_size = size;
    c->chunk_count = (size + CHUNK - 1u) / CHUNK;
    c->chunk_sent = calloc(c->chunk_count ? c->chunk_count : 1u, sizeof *c->chunk_sent);
    if (!c->chunk_sent) tide_out_of_memory();
    c->chunk_ack = 0;
    c->chunk_mask = 0;
}

// The hashes of the world's pages, after how the match started (see SEND_HASHES).
static uint8_t *hashes_of(const tide_server *s, const void *world, uint32_t *size)
{
    const tide_game *g = s->game;
    uint32_t list_size;
    uint8_t *list = g->hash_pages(world, &list_size);
    const uint32_t start = s->start_kind == START_GIVEN ? g->start_size : 0u;
    tide_writer w = {.grows = true};
    tide_write_u8(&w, s->start_kind);
    uint32_t dt;
    memcpy(&dt, &s->dt, 4);
    tide_write_u32(&w, dt);
    tide_write_bytes(&w, s->start, start);
    tide_write_bytes(&w, list, list_size);
    free(list);
    if (w.overflow) tide_out_of_memory();
    *size = w.size;
    return w.data;
}

// The world as it is now, before the next tick, to send it: whole, or for a
// player with a world to build it from (or that can start the match itself,
// over a network), its pages' hashes first. A world that goes in one update
// anyway goes whole: the hashes would only cost it a round trip.
static void start_snapshot(tide_server *s, connection *c)
{
    const tide_game *g = s->game;
    stop_sending(g, c);
    c->snapshot_tick = s->tick;
    c->sending = true;
    c->frame_ack = s->tick;
    c->frame_mask = 0;
    c->catching_up = true;
    memset(c->frame_sent, 0, s->w.history * sizeof *c->frame_sent);
    const bool base = c->has_world || (c->can_start && s->start_kind != START_UNKNOWN && !c->local);
    const bool big = !g->pack_world || g->pack_world(s->world, NULL, 0) > CHUNK * CHUNKS_PER_UPDATE;
    c->hashed = base && big && g->hash_pages ? calloc(1, g->world_size) : NULL;
    if (c->hashed) g->copy_world(c->hashed, s->world);
    uint32_t size;
    uint8_t *bytes = c->hashed ? hashes_of(s, c->hashed, &size) : g->pack_delta(s->world, NULL, NULL, 0, &size);
    send_in_chunks(c, c->hashed ? SEND_HASHES : SEND_WORLD, bytes, size);
}

// It said which pages of the hashed world its own lacks: those, as a delta.
static void send_lacking(const tide_game *g, connection *c, const uint8_t *lacks, const uint32_t size)
{
    uint32_t delta_size;
    uint8_t *delta = g->pack_delta(c->hashed, NULL, lacks, size, &delta_size);
    free_world(g, c->hashed);
    free(c->hashed);
    c->hashed = NULL;
    send_in_chunks(c, SEND_WORLD, delta, delta_size);
}

static void free_connection(const tide_game *g, connection *c)
{
    stop_sending(g, c);
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
    free_connection(s->game, c);
}

// Whether its players take the match over when it goes (see tide_session_take_over).
static bool hands_over(const tide_server *s)
{
    return s->game->host_migration && s->room_key[0] && s->room_code[0];
}

static void send_bye(const tide_server *s, const uint32_t transport, const tide_address to)
{
    uint8_t data[8];
    tide_writer w = {data, sizeof data, 0, false, false};
    header(&w, MSG_BYE);
    tide_write_u8(&w, s->ended ? BYE_ENDED : hands_over(s) ? BYE_HANDOVER : BYE_LEFT);
    send_packet(&s->desc.transports[transport], to, &w);
}

// Tells a kicked player they were, and why.
static void send_kicked(const tide_server *s, const kicked *k)
{
    uint8_t data[8u + TIDE_MESSAGE_BYTES];
    tide_writer w = {data, sizeof data, 0, false, false};
    header(&w, MSG_BYE);
    tide_write_u8(&w, BYE_KICKED);
    const uint32_t n = (uint32_t)strlen(k->message);
    tide_write_u8(&w, (uint8_t)n);
    tide_write_bytes(&w, k->message, n);
    send_packet(&s->desc.transports[k->transport], k->address, &w);
}

static void refuse(const tide_server *s, const uint32_t transport, const tide_address to, const uint8_t reason)
{
    uint8_t data[8];
    tide_writer w = {data, sizeof data, 0, false, false};
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
    const uint16_t their_time = tide_read_u16(r);
    const uint64_t cookie = tide_read_u64(r);
    const uint8_t flags = tide_read_u8(r);
    if (r->failed) return;
    for (uint32_t i = 0; i < TIDE_MAX_PLAYERS; i++) {
        kicked *k = &s->kicks[i];
        if (!k->used) continue;
        const bool here = k->transport == transport && tide_address_equal(k->address, from);
        const bool same = cookie && k->cookie == cookie;
        if (same && (flags & HELLO_KNEW_KICK)) {
            k->used = false; // Back, knowing: a kick isn't a ban
        } else if (same || (here && k->nonce == nonce)) {
            // It didn't hear: told again, wherever it is now
            k->transport = transport;
            k->address = from;
            k->nonce = nonce;
            send_kicked(s, k);
            return;
        } else if (here) {
            k->used = false; // That machine trying again, without a cookie: someone new
        }
    }
    connection *c = find_connection(s, transport, from);
    if (c && c->nonce == nonce) return; // It's being welcomed already
    if (game != s->game->hash) {
        if (c) drop_connection(s, c);
        refuse(s, transport, from, REFUSE_OTHER_GAME);
        return;
    }
    // Its cookie's digest: the player's, from this server or the one it went on from
    int32_t player = -1;
    const uint64_t digest = cookie_digest(cookie);
    for (uint32_t i = 0; cookie && i < TIDE_MAX_PLAYERS; i++) {
        if (s->digests[i] == digest) player = (int32_t)i;
    }
    if (player >= 0 && !s->cookies[player]) s->cookies[player] = cookie;
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
        c->has_world = (flags & HELLO_HAS_WORLD) != 0;
        c->can_start = (flags & HELLO_CAN_START) != 0;
        start_snapshot(s, c);
        return;
    }
    // A closed match takes only the players in it: those of the world it went on from too
    const bool in_world = player >= 0 && (s->present >> player & 1u);
    if (s->closed && !(s->desc.local_first && transport == 0) && !in_world) {
        refuse(s, transport, from, REFUSE_CLOSED);
        return;
    }
    // A player coming back gets their slot; a new one a slot never used, or
    // failing that, the one away the longest, whose cookie stops working.
    for (uint32_t i = 0; player < 0 && i < TIDE_MAX_PLAYERS; i++) {
        if (!s->connections[i].used && s->digests[i] == 0) player = (int32_t)i;
    }
    if (player < 0) {
        for (uint32_t i = 0; i < TIDE_MAX_PLAYERS; i++) {
            if (s->connections[i].used || (s->present >> i & 1u)) continue; // In the match
            if (player < 0 || s->away_since[i] < s->away_since[player]) player = (int32_t)i;
        }
        if (player >= 0) { // Given away
            s->cookies[player] = s->digests[player] = 0;
            s->kicks[player].used = false;
        }
    }
    if (player < 0) {
        refuse(s, transport, from, REFUSE_FULL);
        return;
    }
    if (s->cookies[player] == 0) {
        s->random ^= (uint64_t)from.host << 16 ^ from.port ^ (uint64_t)nonce << 32;
        do s->cookies[player] = next_random(&s->random);
        while (s->cookies[player] == 0);
        s->digests[player] = cookie_digest(s->cookies[player]);
    }
    s->away_since[player] = 0.0;
    c = &s->connections[player];
    const uint32_t bytes = s->game->max_input_bytes ? s->game->max_input_bytes : 1u;
    *c = (connection){.used = true, .transport = transport, .address = from, .nonce = nonce, .last_heard = s->now,
                      .their_time = their_time, .has_world = (flags & HELLO_HAS_WORLD) != 0,
                      .can_start = (flags & HELLO_CAN_START) != 0};
    c->local = s->desc.local_first && transport == 0;
    c->inputs = malloc((size_t)(s->w.inputs + 1u) * bytes);
    c->input_tick = malloc(s->w.inputs * sizeof *c->input_tick);
    c->input_size = calloc(s->w.inputs, sizeof *c->input_size);
    c->frame_sent = calloc(s->w.history, sizeof *c->frame_sent);
    if (!c->inputs || !c->input_tick || !c->input_size || !c->frame_sent) {
        free_connection(s->game, c);
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
    const uint16_t their_time = tide_read_u16(r);
    const uint16_t echo = tide_read_u16(r);
    const uint8_t flags = tide_read_u8(r);
    // What it has of what's being sent (see send_client)
    const uint32_t chunk_number = tide_read_varint(r);
    const uint32_t have = chunk_number ? tide_read_varint(r) : 0u;
    const uint32_t chunk_ack = !chunk_number ? 0u : have == 0 ? UINT32_MAX : have - 1u;
    const uint64_t chunk_mask = chunk_number && have ? tide_read_u64(r) : 0u;
    const uint32_t frame_ack = tide_read_varint(r);
    const uint32_t frame_mask = tide_read_varint(r);
    uint32_t lacks_number = 0;
    uint32_t lacks_size = 0;
    const uint8_t *lacks = NULL;
    if (flags & CLIENT_LACKS) {
        lacks_number = tide_read_varint(r);
        lacks_size = tide_read_varint(r);
        lacks = tide_read_bytes(r, lacks_size);
    }
    const uint32_t first_input = frame_ack + tide_read_varint(r);
    const uint8_t input_count = tide_read_u8(r);
    if (r->failed) return;
    // Whether it wants the world goes by the newest it sent: an older one can come late
    const bool newest = (int16_t)(uint16_t)(their_time - c->their_time) >= 0;
    const bool wants = (flags & CLIENT_WANTS_WORLD) != 0;
    c->last_heard = s->now;
    if (newest) c->their_time = their_time;
    if (echo) c->rtt_ms = millis_since(s->now, echo);

    if (c->sending) {
        if (chunk_number == c->snapshot_number) { // About what's being sent, not something older
            if (chunk_ack > c->chunk_ack) c->chunk_ack = chunk_ack < c->chunk_count ? chunk_ack : c->chunk_count;
            if (chunk_ack == c->chunk_ack) c->chunk_mask = chunk_mask;
        }
        if (c->snapshot_kind == SEND_HASHES && lacks && lacks_number == c->snapshot_number) {
            send_lacking(s->game, c, lacks, lacks_size);
        } else if ((c->snapshot_kind == SEND_WORLD && c->chunk_ack >= c->chunk_count) || (newest && !wants)) {
            stop_sending(s->game, c); // It has the world, or one that needs nothing: ticks from here
        }
    } else if (newest && wants) {
        c->has_world = (flags & CLIENT_HAS_WORLD) != 0;
        c->can_start = (flags & CLIENT_CAN_START) != 0;
        start_snapshot(s, c); // Its world went wrong, or never came: sent again
    } else {
        if (frame_ack > c->frame_ack && frame_ack <= s->tick) c->frame_ack = frame_ack;
        if (frame_ack == c->frame_ack) c->frame_mask = frame_mask;
    }

    // Its inputs: after the first, each as what differs from the one before,
    // when the game packs them that way
    const tide_game *g = s->game;
    const uint32_t max = g->max_input_bytes;
    const bool deltas = g->read_input && g->read_input_delta;
    uint8_t *previous = s->scratch;
    uint8_t *current = s->scratch + g->input_size;
    uint8_t *whole = current + g->input_size;
    for (uint32_t i = 0; i < input_count; i++) {
        const uint32_t size = tide_read_varint(r);
        const uint8_t *bytes = tide_read_bytes(r, size);
        if (!bytes) return;
        uint32_t n = size;
        if (deltas) { // Each builds on the one before, late or not
            if (!(i == 0 ? g->read_input(bytes, size, current) : g->read_input_delta(bytes, size, previous, current))) return;
            n = g->write_input(current, whole, max);
            bytes = whole;
            uint8_t *swap = previous;
            previous = current;
            current = swap;
        }
        const uint32_t t = first_input + i;
        if (t < s->tick || t - s->tick >= s->w.inputs || n > max || n == 0) continue; // Late, or much too early
        const uint32_t slot = t % s->w.inputs;
        c->input_tick[slot] = t;
        c->input_size[slot] = (uint16_t)n;
        memcpy(c->inputs + (size_t)slot * max, bytes, n);
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
        if (!c && type == MSG_CLIENT) { // A player it kicked, who didn't hear: told again
            for (uint32_t i = 0; i < TIDE_MAX_PLAYERS; i++) {
                const kicked *k = &s->kicks[i];
                if (k->used && k->transport == transport && tide_address_equal(k->address, from)) send_kicked(s, k);
            }
        }
        if (!c) continue;
        if (type == MSG_CLIENT) on_client(s, c, &r);
        else if (type == MSG_BYE) drop_connection(s, c);
    }
}

// A slot's new input in a frame, after its size: as what differs from the
// slot's input as the world keeps it, which every machine has before that
// tick, when the game packs inputs that way, else whole (`packed`).
static void put_input(tide_server *s, tide_writer *w, const uint32_t slot, const void *input, const uint8_t *packed,
                      uint32_t size)
{
    const tide_game *g = s->game;
    if (g->write_input_delta && g->world_input) {
        uint8_t *delta = s->scratch + g->input_size;
        g->world_input(s->world, slot, s->scratch);
        size = g->write_input_delta(input, s->scratch, delta, g->max_input_bytes);
        packed = delta;
    }
    tide_write_varint(w, size);
    tide_write_bytes(w, packed, size);
}

// Twice the ticks kept, each where it goes in the bigger ring, with what each
// player was sent of it. False, changing nothing, without the memory.
static bool grow_history(tide_server *s)
{
    const uint32_t old = s->w.history;
    const uint32_t grown = old * 2u;
    stored_frame *frames = calloc(grown, sizeof *frames);
    sent_mark *marks[TIDE_MAX_PLAYERS] = {0};
    bool ok = frames != NULL;
    for (uint32_t p = 0; ok && p < TIDE_MAX_PLAYERS; p++) {
        if (!s->connections[p].used) continue;
        marks[p] = calloc(grown, sizeof *marks[p]);
        ok = marks[p] != NULL;
    }
    if (!ok) {
        free(frames);
        for (uint32_t p = 0; p < TIDE_MAX_PLAYERS; p++) free(marks[p]);
        return false;
    }
    for (uint32_t i = 0; i < old; i++) {
        if (s->frames[i].data) frames[s->frames[i].tick % grown] = s->frames[i];
    }
    for (uint32_t p = 0; p < TIDE_MAX_PLAYERS; p++) {
        connection *c = &s->connections[p];
        if (!c->used) continue;
        for (uint32_t i = 0; i < old; i++) {
            if (c->frame_sent[i].at > 0.0) marks[p][c->frame_sent[i].tick % grown] = c->frame_sent[i];
        }
        free(c->frame_sent);
        c->frame_sent = marks[p];
    }
    free(s->frames);
    s->frames = frames;
    s->w.history = grown;
    return true;
}

// The ticks kept are a ring, which the tick about to be stored would go round
// onto the oldest. While a player who was sent the world still needs that
// one, the ring grows instead: a world that took longer to send than the
// ticks kept would otherwise be sent again and again, never caught up with.
static void keep_needed(tide_server *s, const uint32_t tick)
{
    const stored_frame *oldest = &s->frames[tick % s->w.history];
    if (!oldest->data) return;
    for (uint32_t p = 0; p < TIDE_MAX_PLAYERS; p++) {
        const connection *c = &s->connections[p];
        if (c->used && c->catching_up && oldest->tick >= c->frame_ack) {
            if (grow_history(s)) return;
            // Without the memory, players catching up get the world again instead
            for (uint32_t q = 0; q < TIDE_MAX_PLAYERS; q++) s->connections[q].catching_up = false;
            return;
        }
    }
}

// Runs one tick: joins and leaves, then the inputs that arrived for it, in
// slot order, then the systems. Clients do exactly the same with the frame.
static void server_tick(tide_server *s)
{
    const tide_game *g = s->game;
    const uint32_t tick = s->tick;
    tide_writer w = {s->frame, frame_capacity(g), 0, false, false};
    tide_write_u64(&w, 0); // The hash, once the tick has run
    const uint32_t parts_at = w.size;
    tide_write_u8(&w, 0); // Which of what follows it has (FRAME_*), once it's known
    if (s->event_count) tide_write_u8(&w, (uint8_t)s->event_count);
    for (uint32_t i = 0; i < s->event_count; i++) {
        const tide_player_id player = tide_player_from_index(s->events[i][1]);
        if (s->events[i][0] == EVENT_JOIN) g->player_joined(s->world, player);
        else g->player_left(s->world, player);
        tide_write_u8(&w, s->events[i][0]);
        tide_write_u8(&w, s->events[i][1]);
    }
    const uint8_t events = s->event_count ? FRAME_EVENTS : 0u;
    s->event_count = 0;

    // Each input that changed. One that's the same as the slot's last is left
    // out: keeping the last input is the same as setting it again. They come
    // after the bits that say which, so they're made apart first.
    tide_writer inputs = {s->frame + frame_capacity(g), frame_capacity(g), 0, false, false};
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
        put_input(s, &inputs, p, input, s->packed, size);
        g->set_input(s->world, tide_player_from_index((int32_t)p), input);
        mask |= 1u << p;
    }
    const uint8_t *server_packed = input + g->input_size;
    if (server_size && !(server_size == s->server_last_size && memcmp(s->server_last, server_packed, server_size) == 0)) {
        memcpy(s->server_last, server_packed, server_size);
        s->server_last_size = server_size;
        g->read_input(server_packed, server_size, input);
        put_input(s, &inputs, SERVER_SLOT, input, server_packed, server_size);
        g->set_server_input(s->world, input);
        mask |= 1u << SERVER_SLOT;
    }
    if (mask) tide_write_varint(&w, mask);
    if (late) tide_write_varint(&w, late);
    tide_write_bytes(&w, inputs.data, inputs.size);
    s->frame[parts_at] = (uint8_t)(events | (mask ? FRAME_INPUTS : 0u) | (late ? FRAME_LATE : 0u));

    g->tick(s->world, s->desc.jobs);
    s->tick++;
    s->ended = g->ended && g->ended(s->world);
    patch_u64(s->frame, g->hash_world(s->world));

    keep_needed(s, tick);
    stored_frame *f = &s->frames[tick % s->w.history];
    uint8_t *data = realloc(f->data, w.size);
    if (!data) return;
    memcpy(data, s->frame, w.size);
    *f = (stored_frame){tick, w.size, data};
}

static void send_welcome(const tide_server *s, const connection *c)
{
    uint8_t data[64];
    tide_writer w = {data, sizeof data, 0, false, false};
    header(&w, MSG_WELCOME);
    tide_write_u32(&w, c->nonce);
    tide_write_u8(&w, (uint8_t)(c - s->connections));
    tide_write_u8(&w, c->local ? 1u : 0u); // Its input is the server's too
    tide_write_u32(&w, s->desc.tick_rate);
    tide_write_u32(&w, s->tick);
    tide_write_u32(&w, c->snapshot_tick);
    tide_write_u32(&w, c->snapshot_number);
    tide_write_u8(&w, c->snapshot_kind);
    tide_write_u32(&w, c->snapshot_size);
    tide_write_u32(&w, c->chunk_count);
    tide_write_u16(&w, millis16(s->now));
    tide_write_u16(&w, c->their_time);
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
        tide_writer w = {s->packet, TIDE_NET_MTU, 0, false, false};
        header(&w, MSG_CHUNK);
        tide_write_u32(&w, c->snapshot_number);
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
    *w = (tide_writer){s->packet, TIDE_NET_MTU, 0, false, false};
    header(w, MSG_SERVER);
    tide_write_u16(w, millis16(s->now));
    tide_write_u16(w, c->their_time);
    tide_write_u32(w, s->tick);
    write_signed(w, (int32_t)(c->newest_input - s->tick)); // The newest input it has, and so how early they arrive
    *count_at = w->size;
    tide_write_u8(w, 0);
}

// A piece's own bytes in a SERVER: how many ticks before the packet's its
// tick is, its frame's size, and which piece of it, when it has several.
static uint32_t piece_header(const tide_server *s, const uint32_t tick, const uint32_t size)
{
    return varint_size(s->tick - tick) + varint_size(size) + (size > PIECE ? 1u : 0u);
}

// The ticks it lacks, in pieces, as many packets as it takes (up to a limit).
static void send_frames(tide_server *s, connection *c)
{
    // Too far behind for the ticks kept: the whole world again. One catching
    // up with the world it was sent has them all kept for it (keep_needed).
    if (!c->catching_up && s->tick - c->frame_ack >= s->w.history) {
        start_snapshot(s, c);
        return;
    }
    // Caught up with the world it was sent: from here, a player that falls
    // behind gets the world again, rather than the server keeping every tick
    if (c->catching_up && s->tick - c->frame_ack <= windows_for(s->desc.tick_rate).history / 2u) c->catching_up = false;
    // While what it lacks fits in one datagram, each one carries all of it:
    // a lost one costs a frame, not a round trip. Beyond that, what was sent
    // goes again once it should have been acknowledged.
    uint32_t lacking = 0;
    for (uint32_t tick = c->frame_ack; tick < s->tick && lacking <= TIDE_NET_MTU; tick++) {
        const stored_frame *f = &s->frames[tick % s->w.history];
        if (f->tick == tick) lacking += f->size + piece_header(s, tick, f->size) * ((f->size + PIECE - 1u) / PIECE);
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
            if (w.size + piece_header(s, tick, f->size) + size > TIDE_NET_MTU || pieces == 255u) {
                s->packet[count_at] = (uint8_t)pieces;
                send_packet(t, c->address, &w);
                packets++;
                start_server_packet(s, c, &w, &count_at);
                pieces = 0;
            }
            tide_write_varint(&w, s->tick - tick);
            tide_write_varint(&w, f->size);
            if (count > 1) tide_write_u8(&w, (uint8_t)k);
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
    const tide_game *g = s->game;
    if (s->desc.tick_rate == 0) s->desc.tick_rate = g->tick_rate ? g->tick_rate : 60u;
    s->w = windows_for(s->desc.tick_rate);
    s->frames = calloc(s->w.history, sizeof *s->frames);
    s->world = calloc(1, g->world_size);
    s->frame = malloc(2u * (size_t)frame_capacity(g));
    s->packet = malloc(TIDE_NET_MTU);
    s->input = calloc(1, (size_t)g->input_size + g->max_input_bytes + 1u);
    s->packed = malloc((size_t)g->max_input_bytes + 1u);
    s->server_last = malloc((size_t)g->max_input_bytes + 1u);
    s->scratch = calloc(1, scratch_size(g));
    if (!s->frames || !s->world || !s->frame || !s->packet || !s->input || !s->packed || !s->server_last || !s->scratch) {
        tide_server_destroy(s);
        return NULL;
    }
    const float dt = desc->dt > 0.0f ? desc->dt : 1.0f / (float)s->desc.tick_rate;
    if (desc->world) g->copy_world(s->world, desc->world);
    else g->start(s->world, dt, desc->start);
    s->dt = dt;
    s->start_kind = desc->world ? START_UNKNOWN : desc->start ? START_GIVEN : START_MAIN;
    if (s->start_kind == START_GIVEN) {
        s->start = malloc(g->start_size ? g->start_size : 1u);
        if (s->start) memcpy(s->start, desc->start, g->start_size);
        else s->start_kind = START_UNKNOWN;
    }
    s->present = desc->world ? desc->players : 0u;
    s->present_until = now + PRESENT_SECONDS;
    for (uint32_t i = 0; desc->world && i < TIDE_MAX_PLAYERS; i++) {
        if (desc->leaving >> i & 1u) { // Gone with the last server
            add_event(s, EVENT_LEAVE, i);
            s->away_since[i] = now > 0.0 ? now : 1e-9;
        }
        if (desc->digests) s->digests[i] = desc->digests[i];
    }
    s->closed = desc->closed;
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
        free_connection(s->game, c);
    }
    // A goodbye can be lost: a match that ended says so where its players
    // would meet to take it over, so that none does (host migration)
    for (uint32_t i = 0; i < 2; i++) {
        const tide_transport *t = &s->desc.transports[i];
        if (s->ended && t->end) t->end(t->self);
        if (t->close) t->close(t->self);
    }
    for (uint32_t i = 0; s->frames && i < s->w.history; i++) free(s->frames[i].data);
    free(s->frames);
    free_world(s->game, s->world);
    free(s->world);
    free(s->frame);
    free(s->packet);
    free(s->input);
    free(s->start);
    free(s->scratch);
    free(s->packed);
    free(s->server_last);
    free(s);
}

// Host migration: what a player needs to take the match over when the server
// goes, or to join the one who did, every update while it's new, and every
// HANDOVER_EVERY after.
static void send_handover(tide_server *s)
{
    if (!hands_over(s)) return;
    uint32_t in_match = s->present;
    uint8_t own = 0xFFu; // This machine's player, who leaves with it
    for (uint32_t i = 0; i < TIDE_MAX_PLAYERS; i++) {
        if (!s->connections[i].used) continue;
        in_match |= 1u << i;
        if (s->connections[i].local) own = (uint8_t)i;
    }
    const bool open = !s->closed && s->desc.transports[1].send;
    uint8_t data[256];
    tide_writer w = {data, sizeof data, 0, false, false};
    header(&w, MSG_HANDOVER);
    const uint32_t version_at = w.size;
    tide_write_u32(&w, 0);
    tide_write_u8(&w, own);
    tide_write_u8(&w, open ? 1u : 0u);
    tide_write_bytes(&w, s->room_code, TIDE_ROOM_CODE_LENGTH);
    const uint32_t key = (uint32_t)strlen(s->room_key);
    tide_write_u8(&w, (uint8_t)key);
    tide_write_bytes(&w, s->room_key, key);
    tide_write_u32(&w, in_match);
    for (uint32_t i = 0; i < TIDE_MAX_PLAYERS; i++) tide_write_u64(&w, in_match >> i & 1u ? s->digests[i] : 0u);
    const uint64_t hash = tide_hash(data + version_at + 4u, w.size - version_at - 4u);
    if (hash != s->handover_hash || !s->handover_version) {
        s->handover_hash = hash;
        s->handover_version++;
        s->handover_changed = s->now;
    }
    if (s->now - s->handover_changed > HANDOVER_EVERY && s->now - s->handover_sent < HANDOVER_EVERY) return;
    patch_u32(data + version_at, s->handover_version);
    s->handover_sent = s->now;
    for (uint32_t i = 0; i < TIDE_MAX_PLAYERS; i++) {
        const connection *c = &s->connections[i];
        if (c->used && !c->local) send_packet(&s->desc.transports[c->transport], c->address, &w);
    }
}

void tide_server_update(tide_server *s, const double now)
{
    s->now = now;
    for (uint32_t i = 0; i < 2; i++) server_receive(s, i);
    for (uint32_t i = 0; i < TIDE_MAX_PLAYERS; i++) {
        connection *c = &s->connections[i];
        if (c->used && now - c->last_heard > TIMEOUT) drop_connection(s, c);
    }
    // Players of the world it went on from who didn't come back in time left
    if (s->present && now >= s->present_until) {
        for (uint32_t i = 0; i < TIDE_MAX_PLAYERS; i++) {
            if (!(s->present >> i & 1u)) continue;
            add_event(s, EVENT_LEAVE, i);
            s->away_since[i] = now > 0.0 ? now : 1e-9;
        }
        s->present = 0;
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
    if (!s->ended) send_handover(s);
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

bool tide_server_kick(tide_server *s, const tide_player_id player, const char *message)
{
    const int32_t index = tide_player_index(player);
    if (index < 0) return false;
    connection *c = &s->connections[index];
    if (!c->used || c->local) return false;
    kicked *k = &s->kicks[index];
    *k = (kicked){.used = true, .cookie = s->cookies[index], .transport = c->transport, .address = c->address,
                  .nonce = c->nonce};
    copy_message(k->message, message, TIDE_MESSAGE_BYTES - 1u);
    send_kicked(s, k);
    drop_connection(s, c);
    return true;
}

// ---------------------------------------------------------------------------
// Client

// Host migration: what the server last said a player needs to take the match
// over when it goes, or to join the one who did (see send_handover).
typedef struct handover {
    uint32_t version; // 0: none yet
    uint8_t own;      // The server's own player, 0xFF for none
    bool open;
    char code[TIDE_ROOM_CODE_LENGTH + 1];
    char key[TIDE_ROOM_KEY_LENGTH + 1];
    uint32_t in_match;
    uint64_t digests[TIDE_MAX_PLAYERS];
} handover;

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
    char message[TIDE_MESSAGE_BYTES]; // A kick's
    uint32_t nonce;
    double created;
    double last_hello;
    double last_heard;
    double now;
    bool welcomed;
    int32_t player;
    bool server_input_mine; // On the server's machine: its input is the server's too
    uint32_t tick_rate;
    uint32_t server_tick; // The server's, as it last said while sending a world
    uint16_t their_time;
    uint32_t rtt_ms;
    uint32_t input_ack; // The server has every input before this tick

    // What's being received: the world, or the hashes of its pages
    bool receiving;
    uint32_t snapshot_tick;
    uint32_t snapshot_number;
    uint8_t snapshot_kind;
    uint32_t snapshot_size;
    uint32_t chunk_count;
    uint32_t chunks_had;
    uint8_t *snapshot;
    uint8_t *chunk_have;
    uint32_t got_number; // The last thing it had whole
    bool need_snapshot;
    uint32_t resyncs;
    uint64_t world_bytes;
    uint64_t cookie;

    // The world it builds the one being received from: the one it was given
    // (tide_client_desc.base) until it has its own, then a snapshot of its
    // own. Once the hashes of the world at answered_tick came, `lacks` says
    // which of their pages it lacks.
    void *base;
    bool base_failed; // A world built from one came out wrong: the next comes whole
    bool answered;
    uint32_t answered_tick;
    uint32_t answered_number;
    uint32_t lacks_size;
    uint8_t lacks[LACKS_BYTES];

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
    uint8_t *input;   // input_size
    uint8_t *scratch; // scratch_size: inputs and their deltas

    handover handover;
    bool handed_over; // The server left saying the match goes on elsewhere
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

static void *world_at(const tide_client *c, uint32_t tick);

static void drop_base(tide_client *c)
{
    free_world(c->game, c->base);
    free(c->base);
    c->base = NULL;
    c->answered = false;
}

// Whether it can start the match itself, to have a world to build the
// server's from.
static bool can_start(const tide_client *c)
{
    return c->game->pure_start && !c->loaded && !c->base && !c->base_failed;
}

// The world to build the one at `tick` from: the one it was given, until it
// has its own, then its own nearest that tick; with neither, the match as it
// started (`kind`, `dt` and `given`, see SEND_HASHES). NULL for none, after
// one built from it came out wrong.
static const void *base_for(tide_client *c, const uint32_t tick, const uint8_t kind, const float dt, const void *given)
{
    const tide_game *g = c->game;
    if (c->base_failed) {
        drop_base(c);
        return NULL;
    }
    if (!c->loaded && (c->base || !can_start(c) || kind == START_UNKNOWN)) return c->base;
    if (!c->base) c->base = calloc(1, g->world_size);
    if (!c->base) return NULL;
    if (c->loaded) {
        const uint32_t t = tick < c->verified ? c->verified : tick > c->ahead ? c->ahead : tick;
        g->copy_world(c->base, world_at(c, t));
    } else {
        g->start(c->base, dt, kind == START_GIVEN ? given : NULL);
    }
    return c->base;
}

// The hashes of the world at snapshot_tick came: which of its pages the world
// it builds it from lacks, which it tells the server until the world comes.
static void answer_hashes(tide_client *c)
{
    const tide_game *g = c->game;
    // How the match started, then the hashes
    tide_reader r = {c->snapshot, c->snapshot_size, 0, false};
    const uint8_t kind = tide_read_u8(&r);
    const uint32_t dt_bits = tide_read_u32(&r);
    const uint8_t *given = kind == START_GIVEN ? tide_read_bytes(&r, g->start_size) : NULL;
    float dt;
    memcpy(&dt, &dt_bits, 4);
    const void *base = r.failed || kind > START_GIVEN ? NULL : base_for(c, c->snapshot_tick, kind, dt, given);
    c->lacks_size = base && g->need_pages
                      ? g->need_pages(base, c->snapshot + r.at, c->snapshot_size - r.at, c->lacks, LACKS_BYTES)
                      : 0u;
    c->answered = true;
    c->answered_tick = c->snapshot_tick;
    c->answered_number = c->snapshot_number;
    c->got_number = c->snapshot_number;
    stop_receiving(c);
}

static void on_welcome(tide_client *c, tide_reader *r)
{
    const uint32_t nonce = tide_read_u32(r);
    const uint8_t player = tide_read_u8(r);
    const uint8_t flags = tide_read_u8(r);
    const uint32_t tick_rate = tide_read_u32(r);
    const uint32_t server_tick = tide_read_u32(r);
    const uint32_t tick = tide_read_u32(r);
    const uint32_t number = tide_read_u32(r);
    const uint8_t kind = tide_read_u8(r);
    const uint32_t size = tide_read_u32(r);
    const uint32_t chunks = tide_read_u32(r);
    const uint16_t their_time = tide_read_u16(r);
    const uint16_t echo = tide_read_u16(r);
    const uint64_t cookie = tide_read_u64(r);
    if (r->failed || nonce != c->nonce || player >= TIDE_MAX_PLAYERS || tick_rate == 0 || kind > SEND_HASHES) return;
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
    if (server_tick > c->server_tick) c->server_tick = server_tick;
    c->their_time = their_time;
    if (echo) c->rtt_ms = millis_since(c->now, echo);
    // A world it doesn't have yet: the first, a newer one after it went wrong,
    // or one past all it ran, when it fell behind further than the server
    // keeps ticks. Something it had whole, it had: what's after it is newer.
    const bool wanted = !c->loaded || (c->need_snapshot && tick >= c->verified) || tick > c->ahead;
    if (!wanted || number <= c->got_number || (c->receiving && c->snapshot_number == number)) return;
    stop_receiving(c);
    c->snapshot = malloc(size ? size : 1u);
    c->chunk_have = calloc(chunks ? chunks : 1u, 1);
    if (!c->snapshot || !c->chunk_have) {
        stop_receiving(c);
        return;
    }
    c->receiving = true;
    c->snapshot_tick = tick;
    c->snapshot_number = number;
    c->snapshot_kind = kind;
    c->snapshot_size = size;
    c->chunk_count = chunks;
    c->chunks_had = 0;
}

static void load_snapshot(tide_client *c);

static void on_chunk(tide_client *c, tide_reader *r)
{
    const uint32_t number = tide_read_u32(r);
    const uint32_t index = tide_read_u32(r);
    const uint16_t size = tide_read_u16(r);
    const uint8_t *bytes = tide_read_bytes(r, size);
    if (!bytes || !c->receiving || number != c->snapshot_number || index >= c->chunk_count || c->chunk_have[index]) return;
    const uint32_t from = index * CHUNK;
    if (from + size > c->snapshot_size) return;
    memcpy(c->snapshot + from, bytes, size);
    c->chunk_have[index] = 1;
    c->world_bytes += size;
    if (++c->chunks_had < c->chunk_count) return;
    if (c->snapshot_kind == SEND_HASHES) answer_hashes(c);
    else load_snapshot(c);
}

static void on_server(tide_client *c, tide_reader *r)
{
    const uint16_t their_time = tide_read_u16(r);
    const uint16_t echo = tide_read_u16(r);
    const uint32_t server_tick = tide_read_u32(r);
    const int32_t early = read_signed(r); // Its newest input, from the server's tick
    const uint8_t pieces = tide_read_u8(r);
    if (r->failed) return;
    c->their_time = their_time;
    if (echo) c->rtt_ms = millis_since(c->now, echo);
    c->margin = early < -30000 ? -30000 : early > 30000 ? 30000 : early;
    c->margin_known = true;
    const uint32_t input_ack = server_tick + (uint32_t)early;
    if (input_ack > c->input_ack) c->input_ack = input_ack;
    if (!c->loaded) return;
    for (uint32_t i = 0; i < pieces; i++) {
        // Ticks back from the server's, its frame's size, and which piece of it, when there are more
        const uint32_t back = tide_read_varint(r);
        const uint32_t total = tide_read_varint(r);
        const uint32_t count = (total + PIECE - 1u) / PIECE;
        if (r->failed || total == 0 || count > MAX_PIECES) return; // Where the next piece starts is lost
        const uint32_t index = count > 1 ? tide_read_u8(r) : 0u;
        if (index >= count) return;
        const uint32_t from = index * PIECE;
        const uint32_t size = total - from < PIECE ? total - from : PIECE;
        const uint8_t *bytes = tide_read_bytes(r, size);
        if (!bytes) return;
        const uint32_t tick = server_tick - back;
        if (back == 0 || back > server_tick || tick < c->verified || tick - c->verified >= c->w.history) continue;
        pending_frame *f = &c->frames[tick % c->w.history];
        if (f->tick != tick || !f->data) {
            free(f->data);
            *f = (pending_frame){tick, total, 0, count, malloc(total)};
            if (!f->data) continue;
        }
        if (f->size != total || (f->have >> index & 1u)) continue;
        memcpy(f->data + from, bytes, size);
        f->have |= 1u << index;
    }
}

static void on_handover(tide_client *c, tide_reader *r)
{
    handover h = {0};
    h.version = tide_read_u32(r);
    h.own = tide_read_u8(r);
    h.open = tide_read_u8(r) != 0;
    const uint8_t *code = tide_read_bytes(r, TIDE_ROOM_CODE_LENGTH);
    const uint8_t key = tide_read_u8(r);
    const uint8_t *key_bytes = key <= TIDE_ROOM_KEY_LENGTH ? tide_read_bytes(r, key) : NULL;
    h.in_match = tide_read_u32(r);
    for (uint32_t i = 0; i < TIDE_MAX_PLAYERS; i++) h.digests[i] = tide_read_u64(r);
    if (r->failed || !code || !key_bytes || h.version <= c->handover.version) return;
    // A room's code and key are letters and digits, as the platform layer makes them
    for (uint32_t i = 0; i < TIDE_ROOM_CODE_LENGTH; i++) {
        if (!strchr(TIDE_ROOM_CODE_LETTERS, code[i]) || !code[i]) return;
        h.code[i] = (char)code[i];
    }
    for (uint32_t i = 0; i < key; i++) {
        const char k = (char)key_bytes[i];
        if (!((k >= '0' && k <= '9') || (k >= 'a' && k <= 'z') || (k >= 'A' && k <= 'Z'))) return;
        h.key[i] = k;
    }
    c->handover = h;
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
        case MSG_HANDOVER: on_handover(c, &r); break;
        case MSG_REFUSE: go_offline(c, TIDE_DISCONNECT_REFUSED); break;
        case MSG_BYE: {
            const uint8_t why = tide_read_u8(&r);
            if (why == BYE_KICKED && !r.failed) {
                const uint8_t n = tide_read_u8(&r);
                const uint8_t *bytes = tide_read_bytes(&r, n);
                if (c->state != TIDE_SESSION_OFFLINE) copy_message(c->message, r.failed ? NULL : (const char *)bytes, n);
                go_offline(c, TIDE_DISCONNECT_KICKED);
                break;
            }
            if (why == BYE_HANDOVER && !r.failed) c->handed_over = c->state != TIDE_SESSION_OFFLINE;
            go_offline(c, why == BYE_ENDED && !r.failed ? TIDE_DISCONNECT_ENDED : TIDE_DISCONNECT_SERVER_LEFT);
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
    tide_read_u64(&r);
    const uint8_t parts = tide_read_u8(&r);
    if (parts & FRAME_EVENTS) return false;
    const uint32_t mask = parts & FRAME_INPUTS ? tide_read_varint(&r) : 0u;
    const uint32_t late = parts & FRAME_LATE ? tide_read_varint(&r) : 0u;
    if (!c->game->set_input) return mask == 0;
    const uint32_t mine = 1u << c->player | (c->server_input_mine ? 1u << SERVER_SLOT : 0u);
    if ((mask & ~mine) || (late & mine)) return false;
    uint32_t own_size = 0;
    const uint8_t *own = own_input(c, f->tick, &own_size);
    if (!own) return mask == 0;
    const tide_game *g = c->game;
    const bool deltas = g->read_input_delta && g->world_input;
    uint8_t *base = c->scratch;
    uint8_t *theirs = c->scratch + g->input_size;
    if (deltas && !g->read_input(own, own_size, c->input)) return false;
    for (uint32_t slot = 0; slot <= SERVER_SLOT; slot++) {
        if (!(mask >> slot & 1u)) continue;
        const uint32_t size = tide_read_varint(&r);
        const uint8_t *bytes = tide_read_bytes(&r, size);
        if (!bytes) return false;
        if (!deltas) {
            if (size != own_size || memcmp(bytes, own, size) != 0) return false;
            continue;
        }
        // As the server sent it: what differs from the slot's input before the tick
        g->world_input(world_at(c, f->tick), slot, base);
        if (!g->read_input_delta(bytes, size, base, theirs) || memcmp(theirs, c->input, g->input_size) != 0) return false;
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
    return tide_read_u64(&r);
}

// Runs a tick the server sent on `world`, the world before it. False if it's
// broken or the world came out different from the server's.
static bool apply_frame(tide_client *c, void *world, const pending_frame *f)
{
    const tide_game *g = c->game;
    tide_reader r = {f->data, f->size, 0, false};
    const uint64_t hash = tide_read_u64(&r);
    const uint8_t parts = tide_read_u8(&r);
    const uint8_t events = parts & FRAME_EVENTS ? tide_read_u8(&r) : 0u;
    for (uint32_t i = 0; i < events; i++) {
        const uint8_t kind = tide_read_u8(&r);
        const uint8_t player = tide_read_u8(&r);
        if (r.failed || player >= TIDE_MAX_PLAYERS) return false;
        if (kind == EVENT_JOIN) g->player_joined(world, tide_player_from_index(player));
        else g->player_left(world, tide_player_from_index(player));
    }
    const uint32_t mask = parts & FRAME_INPUTS ? tide_read_varint(&r) : 0u;
    if (parts & FRAME_LATE) tide_read_varint(&r); // Late ones
    for (uint32_t slot = 0; slot <= SERVER_SLOT; slot++) {
        if (!(mask >> slot & 1u)) continue;
        const uint32_t size = tide_read_varint(&r);
        const uint8_t *bytes = tide_read_bytes(&r, size);
        if (!bytes || !g->read_input) return false;
        if (g->read_input_delta && g->world_input) { // What differs from the slot's input as the world keeps it
            g->world_input(world, slot, c->scratch);
            if (!g->read_input_delta(bytes, size, c->scratch, c->input)) return false;
        } else if (!g->read_input(bytes, size, c->input)) {
            return false;
        }
        if (slot == SERVER_SLOT) g->set_server_input(world, c->input);
        else g->set_input(world, tide_player_from_index((int32_t)slot), c->input);
    }
    if (r.failed) return false;
    g->tick(world, c->desc.jobs);
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
    g->tick(world, c->desc.jobs);
}

// From the verified world, the predicted ticks again.
static void predict_again(tide_client *c)
{
    for (uint32_t t = c->verified; t < c->ahead; t++) run_predicted(c, t);
}

static void load_snapshot(tide_client *c)
{
    const tide_game *g = c->game;
    const uint32_t tick = c->snapshot_tick;
    // Built from the world it answered these pages' hashes with, if it did
    const void *base = c->answered && c->answered_tick == tick ? c->base : NULL;
    void *world = calloc(1, g->world_size);
    const bool ok = world && g->unpack_delta(world, base, c->snapshot, c->snapshot_size);
    c->got_number = c->snapshot_number;
    stop_receiving(c);
    c->answered = false;
    if (!ok) { // Asked for again, whole if it was built from a world of its own
        free(world);
        if (base) c->base_failed = true;
        c->need_snapshot = true;
        return;
    }
    drop_base(c);
    c->base_failed = false;
    // Ahead of the world that came, it keeps its predicted ticks, which it runs again
    if (!c->loaded || c->ahead < tick) c->verified = c->ahead = tick;
    void **slot = &c->worlds[tick % c->ring];
    free_world(g, *slot);
    free(*slot);
    *slot = world;
    const bool first = !c->loaded;
    c->loaded = true;
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
        // Ahead of the server by its lead, and the round trip it takes to get
        // there: from where the server was as it sent the world, which may
        // have taken a while (and a round trip more for its pages' hashes)
        const uint32_t trip = (uint32_t)ticks_in((double)c->rtt_ms / 1000.0, c->tick_rate);
        const uint32_t server = c->server_tick > tick ? c->server_tick : tick;
        c->clock_start = c->now;
        c->clock_base = (int64_t)server + c->desc.lead + (c->desc.lead ? trip : 0u);
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
    tide_writer w = {data, sizeof data, 0, false, false};
    header(&w, MSG_HELLO);
    tide_write_u64(&w, c->game->hash);
    tide_write_u32(&w, c->nonce);
    tide_write_u16(&w, millis16(c->now));
    tide_write_u64(&w, c->desc.cookie);
    tide_write_u8(&w, (c->desc.knew_kick ? HELLO_KNEW_KICK : 0u) | (c->base ? HELLO_HAS_WORLD : 0u)
                          | (can_start(c) ? HELLO_CAN_START : 0u));
    send_packet(&c->desc.transport, c->desc.server, &w);
    c->last_hello = c->now;
}

static void send_client(tide_client *c)
{
    tide_writer w = {c->packet, TIDE_NET_MTU, 0, false, false};
    header(&w, MSG_CLIENT);
    tide_write_u16(&w, millis16(c->now));
    tide_write_u16(&w, c->their_time);
    // Whether it needs the world, or is taking the one coming, and has one to build it from
    const bool wants = c->need_snapshot || !c->loaded || c->receiving;
    const bool has = !c->base_failed && (c->loaded || c->base);
    const bool lacks = c->answered && !c->receiving;
    tide_write_u8(&w, (wants ? CLIENT_WANTS_WORLD : 0u) | (wants && has ? CLIENT_HAS_WORLD : 0u) | (lacks ? CLIENT_LACKS : 0u)
                          | (wants && can_start(c) ? CLIENT_CAN_START : 0u));
    // The chunks it has of what's being received, or that it has all of the last thing
    uint32_t chunk_number = 0;
    uint32_t chunk_ack = 0;
    uint64_t chunk_mask = 0;
    if (c->receiving) {
        chunk_number = c->snapshot_number;
        while (chunk_ack < c->chunk_count && c->chunk_have[chunk_ack]) chunk_ack++;
        for (uint32_t i = 0; i < 64u && chunk_ack + 1u + i < c->chunk_count; i++) {
            if (c->chunk_have[chunk_ack + 1u + i]) chunk_mask |= (uint64_t)1 << i;
        }
    } else if (c->got_number) {
        chunk_number = c->got_number;
        chunk_ack = UINT32_MAX;
    }
    // Its number (0: nothing yet), then 0 for all of it, or the chunks it has
    // from the first, plus one, and which it has after those
    tide_write_varint(&w, chunk_number);
    if (chunk_number) tide_write_varint(&w, chunk_ack == UINT32_MAX ? 0u : chunk_ack + 1u);
    if (chunk_number && chunk_ack != UINT32_MAX) tide_write_u64(&w, chunk_mask);
    uint32_t frame_mask = 0;
    for (uint32_t i = 0; i < 32u; i++) {
        const uint32_t tick = c->verified + 1u + i;
        if (frame_complete(&c->frames[tick % c->w.history], tick)) frame_mask |= 1u << i;
    }
    tide_write_varint(&w, c->verified);
    tide_write_varint(&w, frame_mask);
    if (lacks) { // Until the world comes
        tide_write_varint(&w, c->answered_number);
        tide_write_varint(&w, c->lacks_size);
        tide_write_bytes(&w, c->lacks, c->lacks_size);
    }

    // Inputs from the first the server lacks, as many as fit: after the
    // first, each as what differs from the one before, when the game packs
    // them that way
    const tide_game *g = c->game;
    const bool deltas = g->read_input && g->write_input_delta;
    uint8_t *previous = c->scratch;
    uint8_t *current = c->scratch + g->input_size;
    uint8_t *delta = current + g->input_size;
    uint32_t first = c->input_ack > c->verified ? c->input_ack : c->verified;
    if (first > c->ahead) first = c->ahead;
    tide_write_varint(&w, first - c->verified); // From the ticks it has
    const uint32_t count_at = w.size;
    tide_write_u8(&w, 0);
    uint32_t count = 0;
    for (uint32_t t = first; t < c->ahead && count < 255u; t++) {
        uint32_t size = 0;
        const uint8_t *own = own_input(c, t, &size);
        if (!own) break;
        if (deltas) {
            if (!g->read_input(own, size, current)) break;
            if (count > 0) {
                size = g->write_input_delta(current, previous, delta, g->max_input_bytes);
                own = delta;
            }
            uint8_t *swap = previous;
            previous = current;
            current = swap;
        }
        if (size == 0 || w.size + varint_size(size) + size > TIDE_NET_MTU) break;
        tide_write_varint(&w, size);
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
    c->scratch = calloc(1, scratch_size(g));
    c->packet = malloc(TIDE_NET_MTU);
    bool worlds = c->worlds != NULL;
    for (uint32_t i = 0; worlds && i < c->ring; i++) worlds = c->worlds[i] != NULL;
    if (!worlds || !c->input || !c->scratch || !c->packet) {
        tide_client_destroy(c);
        return NULL;
    }
    if (desc->base) { // Kept as it is now: the server's hashes say what it lacks
        c->base = calloc(1, g->world_size);
        if (c->base) g->copy_world(c->base, desc->base);
    }
    return c;
}

void tide_client_destroy(tide_client *c)
{
    if (!c) return;
    if (c->state != TIDE_SESSION_OFFLINE && c->desc.transport.send) {
        uint8_t data[8];
        tide_writer w = {data, sizeof data, 0, false, false};
        header(&w, MSG_BYE);
        send_packet(&c->desc.transport, c->desc.server, &w);
    }
    if (c->desc.transport.close) c->desc.transport.close(c->desc.transport.self);
    stop_receiving(c);
    drop_base(c);
    for (uint32_t i = 0; c->frames && i < c->w.history; i++) free(c->frames[i].data);
    free(c->frames);
    free(c->input_tick);
    free(c->input_size);
    for (uint32_t i = 0; c->worlds && i < c->ring; i++) {
        free_world(c->game, c->worlds[i]);
        free(c->worlds[i]);
    }
    free(c->worlds);
    free(c->inputs);
    free(c->input);
    free(c->scratch);
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
    tide_client_status status = {
        .state = c->state,
        .reason = c->reason,
        .player = c->welcomed ? tide_player_from_index(c->player) : (tide_player_id){0},
        .ping_ms = c->rtt_ms,
        .verified_tick = c->verified,
        .predicted_tick = c->ahead,
        .resyncs = c->resyncs,
        .world_bytes = c->world_bytes,
        .cookie = c->cookie,
    };
    memcpy(status.message, c->message, sizeof status.message);
    return status;
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
    bool open; // Other machines can join its server (tide_session_open)
    tide_address joined;  // The server it last joined, and the cookie that makes it the same player there
    uint64_t cookie;
    bool kicked;          // ...which kicked it, as it heard, after it last played there
    tide_session_event events[SESSION_EVENTS];
    uint32_t event_count;
    double last_now; // The host's time at the last update
    double paused;   // Host time that didn't pass for this machine's own match (see tide_session_update)

    // Host migration: the client that lost its server, offline, whose last
    // world views see until the match is back. `migrating` while no other
    // machine took it over yet, or this machine joined the one that did.
    tide_client *stale;
    bool migrating;
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

static void drop_stale(tide_session *s)
{
    tide_client_destroy(s->stale);
    s->stale = NULL;
    s->migrating = false;
}

// Ends whatever it's in, without a word to local code.
static void tear_down(tide_session *s)
{
    tide_client_destroy(s->client);
    tide_server_destroy(s->server);
    tide_loopback_destroy(s->loopback);
    drop_stale(s);
    s->client = NULL;
    s->server = NULL;
    s->loopback = NULL;
    s->last_state = TIDE_SESSION_OFFLINE;
    s->open = false;
}



tide_session *tide_session_create(const tide_session_desc *desc)
{
    tide_session *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->desc = *desc; // A tick rate of 0 is the game's when a match starts, as builds can change
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
    push_event(s, (tide_session_event){.kind = TIDE_SESSION_DISCONNECTED_EVENT, .reason = reason});
}

void tide_session_leave(tide_session *s)
{
    if (!s->client && !s->server && !s->migrating) return;
    tear_down(s);
    push_event(s, (tide_session_event){.kind = TIDE_SESSION_DISCONNECTED_EVENT, .reason = TIDE_DISCONNECT_LEFT});
}

// A server with this machine's player on it, over loopback, and no one else
// until it's opened.
static void start_server(tide_session *s, const void *start, const void *world, const uint32_t players,
                         const double now)
{
    tide_session_leave(s);
    start_clock(s, now);
    s->loopback = tide_loopback_create(1);
    if (!s->loopback) {
        tide_session_fail(s, TIDE_DISCONNECT_FAILED);
        return;
    }
    tide_loopback_set_time(s->loopback, now);
    const tide_server_desc server = {
        .game = s->desc.game,
        .tick_rate = s->desc.tick_rate,
        .start = start,
        .transports = {tide_loopback_endpoint(s->loopback, 1)},
        .local_first = true,
        .wait_for_first = true,
        .world = world,
        .players = players,
        .jobs = s->desc.jobs,
    };
    s->server = tide_server_create(&server, now);
    const tide_client_desc client = {
        .game = s->desc.game,
        .transport = tide_loopback_endpoint(s->loopback, 2),
        .server = tide_loopback_address(1),
        .sample = s->desc.sample,
        .user = s->desc.user,
        .lead = 0, // Its inputs go straight in: the server ticks right after it
        .jobs = s->desc.jobs,
        .base = world, // What its server goes on from, if anything: nothing to send
    };
    s->client = s->server ? tide_client_create(&client, now) : NULL;
    if (!s->client) {
        tear_down(s);
        tide_session_fail(s, TIDE_DISCONNECT_FAILED);
        return;
    }
    s->last_state = TIDE_SESSION_CONNECTING;
}

void tide_session_start(tide_session *s, const void *start, const double now)
{
    start_server(s, start, NULL, 0, now);
}

void tide_session_start_from(tide_session *s, const void *world, const uint32_t players, const double now)
{
    start_server(s, NULL, world, players, now);
}

bool tide_session_open(tide_session *s, const tide_transport network)
{
    tide_transport *mine = s->server ? &s->server->desc.transports[1] : NULL;
    const bool spare = network.send && (!mine || mine->send); // No match to take it, or it has one already
    if (spare && network.close) network.close(network.self);
    if (!mine || (!mine->send && !network.send)) return false;
    if (!mine->send) *mine = network;
    s->server->closed = false;
    s->open = true;
    return true;
}

void tide_session_close(tide_session *s)
{
    if (s->server) s->server->closed = true;
    s->open = false;
}

void tide_session_kick(tide_session *s, const tide_player_id player, const char *message)
{
    if (s->server) tide_server_kick(s->server, player, message);
}

void tide_session_kick_all(tide_session *s, const char *message)
{
    for (int32_t i = 0; s->server && i < (int32_t)TIDE_MAX_PLAYERS; i++) {
        tide_server_kick(s->server, tide_player_from_index(i), message);
    }
}

void tide_session_join(tide_session *s, const tide_transport network, const tide_address server, const double now)
{
    // Joining the match's next server, it's the same player, whatever the address
    const bool migrating = s->migrating;
    if (migrating) s->migrating = false; // Views see the last world until this one arrives
    else tide_session_leave(s);
    start_clock(s, now);
    if (!migrating && !tide_address_equal(server, s->joined)) {
        s->cookie = 0;
        s->kicked = false;
    }
    s->joined = server;
    const tide_client_desc client = {
        .game = s->desc.game,
        .transport = network,
        .server = server,
        .sample = s->desc.sample,
        .user = s->desc.user,
        .lead = 2,
        .cookie = s->cookie,
        .knew_kick = s->kicked && !migrating,
        .jobs = s->desc.jobs,
        .base = migrating && s->stale ? tide_client_verified_world(s->stale) : NULL, // Most of it, most likely
    };
    s->client = tide_client_create(&client, now);
    if (!s->client) {
        if (network.close) network.close(network.self);
        tide_session_fail(s, TIDE_DISCONNECT_FAILED);
        return;
    }
    s->last_state = TIDE_SESSION_CONNECTING;
}

// Whether `c`, offline now, lost a server whose match another machine can take
// over: the server said where to meet, and it went, or stopped answering.
static bool can_migrate(const tide_client *c)
{
    return c->loaded && c->handover.version && c->handover.key[0]
        && (c->reason == TIDE_DISCONNECT_TIMED_OUT || (c->reason == TIDE_DISCONNECT_SERVER_LEFT && c->handed_over));
}

// The match waits to change hands: this machine's client keeps its last world
// for views, and its room goes, to be found again (see tide_session_migrating).
static void begin_migration(tide_session *s)
{
    tide_client *c = s->client;
    if (c->desc.transport.close) c->desc.transport.close(c->desc.transport.self);
    c->desc.transport = (tide_transport){0};
    drop_stale(s);
    s->stale = c;
    s->client = NULL;
    s->migrating = true;
}

void tide_session_update(tide_session *s, const double now)
{
    if (!s->client) return;
    // This machine's server stops whenever the machine does (a breakpoint, a
    // browser that froze the page), and its player with it. Beyond the ticks
    // the server can run in one update, which it would drop anyway, that time
    // didn't pass for the match: neither side went quiet, and there's nothing
    // to catch up. A client of another machine keeps to the real time.
    const double most = s->server ? (double)MAX_TICKS / (double)s->server->desc.tick_rate : 0.0;
    if (s->server && now - s->last_now > most) s->paused += now - s->last_now - most;
    s->last_now = now;
    const double t = now - s->paused;

    if (s->loopback) tide_loopback_set_time(s->loopback, t);
    tide_client_update(s->client, t);
    if (s->server) tide_server_update(s->server, t);
    tide_client_update(s->client, t); // What the server just sent: this machine's ticks, at once

    const tide_client_status status = tide_client_status_of(s->client);
    if (!s->server && status.cookie) s->cookie = status.cookie;
    if (status.state == TIDE_SESSION_CONNECTED) s->kicked = false; // Played there since
    if (status.state == TIDE_SESSION_OFFLINE && status.reason == TIDE_DISCONNECT_KICKED && !s->server) s->kicked = true;
    if (status.state == TIDE_SESSION_CONNECTED && s->last_state != TIDE_SESSION_CONNECTED) {
        // Back in a match that changed hands, it was never out of it
        if (!s->stale) {
            push_event(s, (tide_session_event){.kind = TIDE_SESSION_CONNECTED_EVENT, .reason = TIDE_DISCONNECT_LEFT});
        }
        drop_stale(s);
    }
    if (status.state == TIDE_SESSION_OFFLINE && !s->server && !s->stale && can_migrate(s->client)) {
        begin_migration(s);
        return;
    }
    if (status.state == TIDE_SESSION_OFFLINE) {
        tide_session_event gone = {.kind = TIDE_SESSION_DISCONNECTED_EVENT, .reason = status.reason};
        memcpy(gone.message, status.message, sizeof gone.message);
        tear_down(s);
        push_event(s, gone);
        return;
    }
    s->last_state = status.state;
}

// Seconds after `now` until a clock that started at `start`, at `rate`, counts
// its next tick (ticks_in), aimed at the tick itself rather than ticks_in's
// hair before it, so an update then always finds it due.
static double until_next(const double start, const uint32_t rate, const double now)
{
    return start + (double)(ticks_in(now - start, rate) + 1u) / (double)rate - now;
}

double tide_session_until_tick(const tide_session *s)
{
    const double now = s->last_now - s->paused; // The match's time at the last update
    const tide_server *server = s->server;
    const tide_client *c = s->client;
    // This machine's player ticks right after its server: the server's clock
    if (server && server->started && !server->ended) return until_next(server->clock_start, server->desc.tick_rate, now);
    if (!server && c && c->loaded) return until_next(c->clock_start, c->tick_rate, now);
    uint32_t rate = server ? server->desc.tick_rate : c && c->tick_rate ? c->tick_rate : s->desc.tick_rate;
    if (!rate) rate = s->desc.game->tick_rate ? s->desc.game->tick_rate : 60u;
    return 1.0 / (double)rate;
}

const void *tide_session_world(const tide_session *s)
{
    const void *world = s->client ? tide_client_world(s->client) : NULL;
    return world || !s->stale ? world : tide_client_world(s->stale); // The last one, while the match changes hands
}

tide_view_worlds tide_session_view(const tide_session *s)
{
    if (s->client && s->client->loaded) return tide_client_view(s->client);
    if (s->stale) return (tide_view_worlds){tide_client_world(s->stale), NULL, 1.0f};
    return (tide_view_worlds){0};
}

const void *tide_session_server_world(const tide_session *s)
{
    return s->server ? tide_server_world(s->server) : NULL;
}

tide_session_status tide_session_status_of(const tide_session *s)
{
    tide_session_status status = {0};
    if (s->client) status.client = tide_client_status_of(s->client);
    if (s->stale && status.client.state != TIDE_SESSION_CONNECTED) { // Changing hands: the same player, connecting
        status.client.state = TIDE_SESSION_CONNECTING;
        status.client.player = tide_player_from_index(s->stale->player);
    }
    status.server = s->server != NULL;
    status.open = s->open;
    status.skipped = s->paused;
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

void tide_session_end(tide_session *s)
{
    if (!s->server) return;
    // Its goodbyes say the match ended, and so does its transports' `end`
    s->server->ended = true;
    tear_down(s);
    push_event(s, (tide_session_event){.kind = TIDE_SESSION_DISCONNECTED_EVENT, .reason = TIDE_DISCONNECT_ENDED});
}

void tide_session_set_room(tide_session *s, const char *code, const char *key)
{
    if (!s->server) return;
    snprintf(s->server->room_code, sizeof s->server->room_code, "%s", code ? code : "");
    snprintf(s->server->room_key, sizeof s->server->room_key, "%s", key ? key : "");
}

bool tide_session_migrating(const tide_session *s, char code[TIDE_ROOM_CODE_LENGTH + 1], char key[TIDE_ROOM_KEY_LENGTH + 1])
{
    if (!s->migrating) return false;
    memcpy(code, s->stale->handover.code, TIDE_ROOM_CODE_LENGTH + 1);
    memcpy(key, s->stale->handover.key, TIDE_ROOM_KEY_LENGTH + 1);
    return true;
}

void tide_session_take_over(tide_session *s, const tide_transport network, const double now)
{
    if (!s->migrating) {
        if (network.close) network.close(network.self);
        return;
    }
    // The players in the match come back to it, but the last server's own
    const tide_client *old = s->stale;
    const handover *h = &old->handover;
    uint32_t players = h->in_match;
    uint32_t leaving = 0;
    if (h->own < TIDE_MAX_PLAYERS && (players >> h->own & 1u)) {
        players &= ~(1u << h->own);
        leaving = 1u << h->own;
    }
    s->migrating = false; // Views see the last world until the server's arrives
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
        .tick_rate = old->tick_rate, // The match's, whatever this machine's build says
        .transports = {tide_loopback_endpoint(s->loopback, 1), network},
        .local_first = true,
        .wait_for_first = true,
        .world = world_at(old, old->verified),
        .players = players,
        .leaving = leaving,
        .digests = h->digests,
        .closed = !h->open,
        .jobs = s->desc.jobs,
    };
    s->server = tide_server_create(&server, now); // Closes `network` if it can't be made
    if (s->server) tide_session_set_room(s, h->code, h->key);
    const tide_client_desc client = {
        .game = s->desc.game,
        .transport = tide_loopback_endpoint(s->loopback, 2),
        .server = tide_loopback_address(1),
        .sample = s->desc.sample,
        .user = s->desc.user,
        .lead = 0,
        .cookie = s->cookie, // This machine's player, as it was
        .jobs = s->desc.jobs,
        .base = server.world, // What its server goes on from: nothing to send
    };
    s->client = s->server ? tide_client_create(&client, now) : NULL;
    if (!s->client) {
        tear_down(s);
        tide_session_fail(s, TIDE_DISCONNECT_FAILED);
        return;
    }
    s->open = h->open;
    s->last_state = TIDE_SESSION_CONNECTING;
}

void tide_session_set_game(tide_session *s, const tide_game *game)
{
    s->desc.game = game;
    if (s->stale) {
        s->stale->desc.game = game;
        s->stale->game = game;
    }
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
    uint8_t *scratch;
    uint8_t *inputs[TIDE_MAX_PLAYERS];
} server_migration;

typedef struct client_migration {
    void **worlds;
    uint8_t *input;
    uint8_t *scratch;
    uint8_t *inputs;
} client_migration;

static void discard_server_migration(const tide_game *g, server_migration *m)
{
    free_world(g, m->world);
    free(m->world);
    free(m->frame);
    free(m->input);
    free(m->packed);
    free(m->server_last);
    free(m->scratch);
    for (uint32_t i = 0; i < TIDE_MAX_PLAYERS; i++) free(m->inputs[i]);
}

static void discard_client_migration(const tide_client *c, const tide_game *g, client_migration *m)
{
    for (uint32_t i = 0; m->worlds && i < c->ring; i++) {
        free_world(g, m->worlds[i]);
        free(m->worlds[i]);
    }
    free(m->worlds);
    free(m->input);
    free(m->scratch);
    free(m->inputs);
}

static bool prepare_server(const tide_server *s, const tide_game *g, const tide_migrate_fn migrate, void *user,
                           server_migration *m)
{
    const uint32_t bytes = g->max_input_bytes ? g->max_input_bytes : 1u;
    m->world = calloc(1, g->world_size);
    m->frame = malloc(2u * (size_t)frame_capacity(g));
    m->input = calloc(1, (size_t)g->input_size + g->max_input_bytes + 1u);
    m->packed = malloc((size_t)g->max_input_bytes + 1u);
    m->server_last = malloc((size_t)g->max_input_bytes + 1u);
    m->scratch = calloc(1, scratch_size(g));
    bool ok = m->world && m->frame && m->input && m->packed && m->server_last && m->scratch;
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
    bool sending[TIDE_MAX_PLAYERS]; // What it was sending was the old build's: it starts again
    for (uint32_t i = 0; i < TIDE_MAX_PLAYERS; i++) {
        sending[i] = s->connections[i].used && s->connections[i].sending;
        if (sending[i]) stop_sending(s->game, &s->connections[i]);
    }
    free_world(s->game, s->world);
    free(s->world);
    free(s->frame);
    free(s->input);
    free(s->packed);
    free(s->server_last);
    free(s->scratch);
    free(s->start); // The old build's, which starting the new one wouldn't make
    s->start = NULL;
    s->start_kind = START_UNKNOWN;
    s->world = m->world;
    s->frame = m->frame;
    s->input = m->input;
    s->packed = m->packed;
    s->server_last = m->server_last;
    s->scratch = m->scratch;
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
        if (sending[i]) start_snapshot(s, c);
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
    m->scratch = calloc(1, scratch_size(g));
    ok = ok && m->input && m->scratch;
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
    for (uint32_t i = 0; i < c->ring; i++) {
        free_world(c->game, c->worlds[i]);
        free(c->worlds[i]);
    }
    free(c->worlds);
    c->worlds = m->worlds;
    free(c->input);
    c->input = m->input;
    free(c->scratch);
    c->scratch = m->scratch;
    if (c->frames) {
        free(c->inputs);
        c->inputs = m->inputs;
        for (uint32_t i = 0; i < c->w.inputs; i++) c->input_tick[i] = UINT32_MAX;
    }
    stop_receiving(c); // A world it was receiving is the old build's: the server sends it again
    drop_base(c);      // ...and so is the one it would have built it from
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
}

bool tide_session_migrate(tide_session *s, const tide_game *game, const tide_migrate_fn migrate, void *user)
{
    const bool own = s->server && s->client; // This machine's player, on its own server
    server_migration sm = {0};
    client_migration cm = {0};
    if (s->server && !prepare_server(s->server, game, migrate, user, &sm)) {
        discard_server_migration(game, &sm);
        return false;
    }
    if (s->client && !prepare_client(s->client, game, own ? NULL : migrate, user, &cm)) {
        discard_server_migration(game, &sm);
        discard_client_migration(s->client, game, &cm);
        return false;
    }
    if (s->server) commit_server(s->server, game, &sm);
    if (s->client) commit_client(s->client, game, &cm);
    if (own && s->client->loaded) take_server_world(s->client, s->server);
    s->desc.game = game;
    return true;
}
