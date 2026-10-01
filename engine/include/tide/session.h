#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "tide/jobs.h"
#include "tide/net.h"
#include "tide/player.h"

// Sessions: how a machine takes part in a match.
//
// Temporary implementation written by Claude; the project owner takes it over
// later.
//
// A server runs the match. It ticks the one true world with the inputs players
// send, and sends every player each tick: whose input changed and to what,
// players joining and leaving, and a hash of the world after it.
//
// A client predicts. It runs a few ticks ahead of the server, so its inputs
// reach the server in time, with its own input straight away and, for everyone
// else, their last one. It keeps a snapshot of each tick from the last one the
// server confirmed (the verified world) to the one it predicted. A tick that
// arrives as it was predicted only has its hash checked; one that went
// otherwise runs on the verified world, and the ticks after it run again. A
// hash that differs means the client went wrong somewhere, and it asks for the
// whole world again.
//
// A player joining gets the server's world, packed, then plays on from there,
// and a cookie: coming back with it, the player gets their PlayerID back. If
// the server still has them connected, the new connection takes over with no
// events; if they'd left, PlayerJoined comes again, with the same PlayerID.
// Single-player and hosting are the same: a server and its own player on one
// machine, over a loopback transport (tide_session).
//
// Host migration (tide_game.host_migration): the server tells its players
// where to meet when it goes, its room and the room's key, and the digests of
// every player's cookie (SHA-256, so none can be worked back from them). When
// the server leaves or stops answering, its players go to the room: one takes
// the match over from the last tick it verified (tide_session_take_over), and
// the others join it again, as the same players, since it knows their
// cookies' digests. The server's player leaves the match. Nothing else
// changes hands: what the server kept of its own, the others never had.
//
// A match whose last scene unloads, when Main is local, ends: the server stops
// ticking and tells every player, who go offline with TIDE_DISCONNECT_ENDED.

// What a session needs from a game. tidec generates it as tide_game_api in
// <game>.h; games without an input have no input functions.
typedef struct tide_game {
    uint64_t hash;            // Tells builds apart: servers only take players of the same game
    uint32_t world_size;      // sizeof(tide_world): a world starts as that many zeros
    uint32_t input_size;      // sizeof(tide_input), or 0
    uint32_t max_input_bytes; // The most write_input writes
    uint32_t start_size;      // sizeof(tide_start)
    // Clears the world and starts the match: `start` is a tide_start, the scene
    // it starts in, or NULL for Main.
    void (*start)(void *world, float dt, const void *start);
    // Runs a tick, on `jobs`' threads (tide/jobs.h), or NULL for this one's.
    void (*tick)(void *world, const tide_jobs *jobs);
    // A snapshot: `to` becomes `from`, sharing its pages until one of them
    // changes. The hash covers what's in use, and only reads what changed since
    // it was last taken.
    void (*copy_world)(void *to, const void *from);
    uint64_t (*hash_world)(const void *world);
    // Lets go of what a world has, before its memory goes; NULL for worlds of
    // plain data.
    void (*free_world)(void *world);
    // The world as bytes, to send it: written into `out` when they fit in
    // `capacity` (NULL to ask), and how many there are. Unpacking makes a
    // world from them, false if they aren't one.
    uint32_t (*pack_world)(const void *world, uint8_t *out, uint32_t capacity);
    bool (*unpack_world)(void *world, const uint8_t *data, uint32_t size);
    void (*player_joined)(void *world, tide_player_id player);
    void (*player_left)(void *world, tide_player_id player);
    void (*set_input)(void *world, tide_player_id player, const void *input);
    void (*set_server_input)(void *world, const void *input);
    uint32_t (*write_input)(const void *input, uint8_t *out, uint32_t capacity); // Bytes written, 0 if it didn't fit
    bool (*read_input)(const uint8_t *data, uint32_t size, void *input);
    // Whether the match is over: its last scene unloaded, and Main is local,
    // so it can't come back. The server ends it then. NULL: it never is.
    bool (*ended)(const void *world);
    // The game's settings (`settings { ... }`), 0 or NULL where it sets none
    uint32_t tick_rate;  // tickRate: what servers run its matches at, unless their desc says
    const char *title;   // title: the window's, unless the host says
    bool host_migration; // hostMigration: when a room's server goes, another machine takes the match over
} tide_game;

typedef enum tide_session_state {
    TIDE_SESSION_OFFLINE,    // In no match
    TIDE_SESSION_CONNECTING, // Waiting for the server, then for its world
    TIDE_SESSION_CONNECTED,  // Playing
} tide_session_state;

// Why a machine left a match. Tide's DisconnectReason has the same values.
typedef enum tide_disconnect_reason {
    TIDE_DISCONNECT_LEFT,        // This machine left
    TIDE_DISCONNECT_TIMED_OUT,   // The server stopped answering, or never did
    TIDE_DISCONNECT_REFUSED,     // The server turned it away: another build of the game, no room left, or not open
    TIDE_DISCONNECT_SERVER_LEFT, // The server's machine left, which ended the match
    TIDE_DISCONNECT_FAILED,      // It couldn't start: no network, a port in use, an address that isn't one, a room nobody has
    TIDE_DISCONNECT_ENDED,       // The match ended: its last scene unloaded (see tide_game.ended)
    TIDE_DISCONNECT_KICKED,      // The server sent it away (tide_server_kick), with a message
} tide_disconnect_reason;

// A kick's message, with its NUL: up to 255 bytes of UTF-8, cut where a
// character starts.
#define TIDE_MESSAGE_BYTES 256u

// This machine's input for `tick`, into `input` (the game's input_size bytes).
// Clients call it once for each tick they run ahead, in order.
typedef void (*tide_sample_fn)(void *user, uint32_t tick, void *input);

// ---------------------------------------------------------------------------
// Server

typedef struct tide_server tide_server;

typedef struct tide_server_desc {
    const tide_game *game;
    uint32_t tick_rate;          // Ticks per second; 0 for the game's (tide_game.tick_rate), or else 60
    float dt;                    // Time.dt; 1 / tick_rate when 0
    const void *start;           // The game's tide_start, or NULL: Main
    tide_transport transports[2]; // Where players connect from; the ones with no `send` are unused
    bool local_first;            // Players on transports[0] are on this machine: their input is the server's too
    bool wait_for_first;         // Tick once the first player has joined, before anything happens without them
    // Hot reloading and host migration: the match goes on from this world (the
    // game's tide_world) instead of starting from `start`, with `players` in it
    // already, a bit per player. A player joining into one of their slots gets
    // no PlayerJoined; those who haven't come back in 20 seconds get
    // PlayerLeft. `leaving` went with the last server: PlayerLeft at the first
    // tick.
    const void *world;
    uint32_t players;
    uint32_t leaving;
    // Host migration: the digests of the players' cookies (TIDE_MAX_PLAYERS of
    // them, 0 for none), which they come back with. NULL: none.
    const uint64_t *digests;
    bool closed;                 // Takes no one new from the start, only players coming back
    const tide_jobs *jobs;       // Threads to run ticks on (tide/jobs.h), or NULL: this one
} tide_server_desc;

tide_server *tide_server_create(const tide_server_desc *desc, double now);
// Tells every player the match is over, and closes the transports.
void tide_server_destroy(tide_server *s);
// Reads what arrived, runs the ticks that are due, and sends each player what's new.
void tide_server_update(tide_server *s, double now);
const void *tide_server_world(const tide_server *s);
uint32_t tide_server_tick(const tide_server *s); // Ticks run so far
uint32_t tide_server_player_count(const tide_server *s);
// Sends `player` away: they go offline with TIDE_DISCONNECT_KICKED and
// `message` (NULL for none), and PlayerLeft follows at the end of the next
// tick. A goodbye can be lost: one who didn't hear it is told again whenever
// they're in touch, still sending or joining again. It's a kick, not a ban:
// once they know, they can join again while the match takes players. False
// if they aren't in the match, or they're on its machine (local_first).
bool tide_server_kick(tide_server *s, tide_player_id player, const char *message);

// ---------------------------------------------------------------------------
// Client

typedef struct tide_client tide_client;

typedef struct tide_client_desc {
    const tide_game *game;
    tide_transport transport;
    tide_address server;
    tide_sample_fn sample; // NULL for a game without an input
    void *user;
    uint32_t lead;         // Ticks its inputs should reach the server early: 0 on the server's machine, 2 or more over a network
    uint64_t cookie;       // From an earlier connection to this server, to be the same player again; 0 for none
    bool knew_kick;        // ...which kicked it, as it heard: the server lets it in again (see tide_server_kick)
    const tide_jobs *jobs; // Threads to run ticks on (tide/jobs.h), or NULL: this one
} tide_client_desc;

typedef struct tide_client_status {
    tide_session_state state;
    tide_disconnect_reason reason; // Once it's offline again
    tide_player_id player;         // This machine's player, once connected
    uint32_t ping_ms;              // Round trip to the server
    uint32_t verified_tick;        // Ticks the server has confirmed
    uint32_t predicted_tick;       // Ticks run, predicted ones included
    uint32_t resyncs;              // Times its world diverged and the server sent it again
    uint64_t cookie;               // What makes this player this player again, on the next connection
    char message[TIDE_MESSAGE_BYTES]; // With TIDE_DISCONNECT_KICKED: the server's message
} tide_client_status;

// What views draw: the latest tick's world, the one before it, and how far
// this moment is between them (0 to 1), to blend them. Views draw a tick late
// that way, but smoothly at any tick rate. `previous` is NULL right after the
// world arrives.
typedef struct tide_view_worlds {
    const void *current; // NULL outside a match
    const void *previous;
    float alpha;
} tide_view_worlds;

tide_client *tide_client_create(const tide_client_desc *desc, double now);
// Tells the server it's leaving, and closes the transport.
void tide_client_destroy(tide_client *c);
void tide_client_update(tide_client *c, double now);
tide_client_status tide_client_status_of(const tide_client *c);
// What views read: the world as predicted, up to predicted_tick. NULL until connected.
const void *tide_client_world(const tide_client *c);
// The world up to verified_tick, which the server confirmed. NULL until connected.
const void *tide_client_verified_world(const tide_client *c);
// What views draw, as of the client's last update.
tide_view_worlds tide_client_view(const tide_client *c);

// ---------------------------------------------------------------------------
// Session: what a host program uses. Start runs a server with this machine's
// player on it, which Open lets other machines join; Join is a client of
// another machine's server.

typedef struct tide_session tide_session;

typedef struct tide_session_desc {
    const tide_game *game;
    uint32_t tick_rate; // The matches it starts: 0 for the game's at the time (tide_game.tick_rate), or else 60
    tide_sample_fn sample;
    void *user;
    const tide_jobs *jobs; // Threads to run ticks on (tide/jobs.h), or NULL: this one
} tide_session_desc;

typedef enum tide_session_event_kind {
    TIDE_SESSION_CONNECTED_EVENT,
    TIDE_SESSION_DISCONNECTED_EVENT,
} tide_session_event_kind;

typedef struct tide_session_event {
    tide_session_event_kind kind;
    tide_disconnect_reason reason;    // Disconnected
    char message[TIDE_MESSAGE_BYTES]; // ...and a kick's message
} tide_session_event;

typedef struct tide_session_status {
    tide_client_status client;
    bool server; // This machine runs the server
    bool open;   // ...and other machines can join it (tide_session_open)
} tide_session_status;

tide_session *tide_session_create(const tide_session_desc *desc);
void tide_session_destroy(tide_session *s);
// A match on this machine, which runs its server; closed until it's opened.
// Leaves the one it's in first.
void tide_session_start(tide_session *s, const void *start, double now);
// The same, going on from `world` (the game's tide_world), for hot reloading
// where a new build is a new program, as on the web. `players`, a bit per
// player, are in it already: this machine's player is the first of them, with
// no PlayerJoined.
void tide_session_start_from(tide_session *s, const void *world, uint32_t players, double now);
// Lets other machines join the match this machine runs, through `network`
// (the session closes it as the match ends). A match opened before opens
// again on the network it had, so pass a zeroed transport first: false if it
// had none, and also if this machine runs no match (closing `network`).
bool tide_session_open(tide_session *s, tide_transport network);
// No one else joins the match from now on; the players in it stay.
void tide_session_close(tide_session *s);
// Sends a player on another machine out of the match this machine runs (see
// tide_server_kick). Nothing on a client, or for this machine's own player.
void tide_session_kick(tide_session *s, tide_player_id player, const char *message);
// The same for every player on another machine.
void tide_session_kick_all(tide_session *s, const char *message);
// Joining the server it joined last, it's the same player again, if the server
// still has room for them. While migrating, it's the match's next server.
void tide_session_join(tide_session *s, tide_transport network, tide_address server, double now);
// Ends the match this machine runs, for everyone: every player goes offline
// with TIDE_DISCONNECT_ENDED, this machine's too, and no other machine takes
// it over: the server's transports hear it ended (tide_transport.end), which
// a room tells its relay, for players who missed the goodbye. Nothing on a
// client.
void tide_session_end(tide_session *s);

// Host migration (tide_game.host_migration), on the machine that runs the
// match: the room its players can meet in again when it goes, by its code and
// key ("" for none), which the server tells them. Once a frame will do.
void tide_session_set_room(tide_session *s, const char *code, const char *key);
// Whether this machine's match lost its server and waits to change hands: the
// room's `code` and `key` are where to meet. Until this machine takes it over
// (tide_session_take_over) or joins the one that did (tide_session_join),
// local code sees it connecting and views see the last world it had. Hosts
// fail it (tide_session_fail) if the room can't be reached.
bool tide_session_migrating(const tide_session *s, char code[TIDE_ROOM_CODE_LENGTH + 1],
                            char key[TIDE_ROOM_KEY_LENGTH + 1]);
// This machine takes the match over: it runs the server from the last tick it
// verified, at the match's tick rate, taking players on `network` (the room).
// The others join it again as the same players; the last server's player
// leaves. Local code sees no Connected or Disconnected, only that this
// machine runs the server now.
void tide_session_take_over(tide_session *s, tide_transport network, double now);
void tide_session_leave(tide_session *s);
// A session that couldn't start, or whose network gave out: leaves the match,
// if it's in one, and reports Disconnected with the reason.
void tide_session_fail(tide_session *s, tide_disconnect_reason reason);
// Once per frame, with the host's time in seconds. When this machine runs the
// server, a long gap since the last update is a pause (a breakpoint, a
// browser that froze the page): the match goes on from where it stopped.
void tide_session_update(tide_session *s, double now);
// What views read, or NULL outside a match.
const void *tide_session_world(const tide_session *s);
// What views draw, blended between ticks (see tide_view_worlds).
tide_view_worlds tide_session_view(const tide_session *s);
// The server's world, on the machine that runs it; NULL elsewhere.
const void *tide_session_server_world(const tide_session *s);
tide_session_status tide_session_status_of(const tide_session *s);
// The next thing that happened since the last call, oldest first.
bool tide_session_next_event(tide_session *s, tide_session_event *event);
// Runs another build of the same game from now on: new code with the same
// data layout (the same hash), for hot reloading. The match goes on.
void tide_session_set_game(tide_session *s, const tide_game *game);

// Makes `to`, a zeroed world of the new build, from `from`, a world of the old.
typedef bool (*tide_migrate_fn)(void *user, const void *from, void *to);

// The same, for a build with another data layout: `migrate` carries the
// match's world over to it, on the machine that runs the server, and the last
// world the server confirmed on a client of another machine. This machine's
// own player takes its server's world. Players stay where they are. A client's
// ticks ahead of the server run again, and a world that came out different
// from the server's is sent again, as when a client goes wrong. False,
// changing nothing, if `migrate` fails or there isn't the memory.
bool tide_session_migrate(tide_session *s, const tide_game *game, tide_migrate_fn migrate, void *user);

// What local code asked for, with Session.Start, Join, Connect, Leave, Open,
// Close, Kick, KickAll and End.
typedef enum tide_session_request_kind {
    TIDE_REQUEST_NONE,
    TIDE_REQUEST_START,
    TIDE_REQUEST_JOIN,    // A room, by its code
    TIDE_REQUEST_CONNECT, // A machine, by its address
    TIDE_REQUEST_LEAVE,
    TIDE_REQUEST_OPEN,    // Other machines can join the match this machine runs
    TIDE_REQUEST_CLOSE,   // ...and no longer
    TIDE_REQUEST_KICK,    // A player, with a message
    TIDE_REQUEST_KICK_ALL,
    TIDE_REQUEST_END,     // The match this machine runs ends, for everyone
} tide_session_request_kind;

#define TIDE_DEFAULT_PORT 7777u

typedef struct tide_session_request {
    uint32_t kind;
    uint32_t port;          // Open: the port to take players on. Connect: the server's, unless `text` has one
    tide_player_id player;  // Kick: whom
    char text[TIDE_MESSAGE_BYTES]; // Join: the room's code. Connect: the server's address, "host" or "host:port".
                                   // Kick and KickAll: the message
} tide_session_request;
