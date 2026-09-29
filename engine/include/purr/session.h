#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "purr/net.h"
#include "purr/player.h"

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
// machine, over a loopback transport (purr_session).

// What a session needs from a game. purrc generates it as purr_game_api in
// <game>.h; games without an input have no input functions.
typedef struct purr_game {
    uint64_t hash;            // Tells builds apart: servers only take players of the same game
    uint32_t world_size;      // sizeof(purr_world)
    uint32_t input_size;      // sizeof(purr_input), or 0
    uint32_t max_input_bytes; // The most write_input writes
    uint32_t start_size;      // sizeof(purr_start)
    // Clears the world and starts the match: `start` is a purr_start, the scene
    // it starts in, or NULL for Main.
    void (*start)(void *world, float dt, const void *start);
    void (*tick)(void *world);
    // A snapshot: `to` becomes `from`, copying only what's in use. The hash
    // covers the same, so its cost follows the world's contents, not its size.
    void (*copy_world)(void *to, const void *from);
    uint64_t (*hash_world)(const void *world);
    void (*player_joined)(void *world, purr_player_id player);
    void (*player_left)(void *world, purr_player_id player);
    void (*set_input)(void *world, purr_player_id player, const void *input);
    void (*set_server_input)(void *world, const void *input);
    uint32_t (*write_input)(const void *input, uint8_t *out, uint32_t capacity); // Bytes written, 0 if it didn't fit
    bool (*read_input)(const uint8_t *data, uint32_t size, void *input);
} purr_game;

typedef enum purr_session_state {
    PURR_SESSION_OFFLINE,    // In no match
    PURR_SESSION_CONNECTING, // Waiting for the server, then for its world
    PURR_SESSION_CONNECTED,  // Playing
} purr_session_state;

// Why a machine left a match. PurrLang's DisconnectReason has the same values.
typedef enum purr_disconnect_reason {
    PURR_DISCONNECT_LEFT,        // This machine left
    PURR_DISCONNECT_TIMED_OUT,   // The server stopped answering, or never did
    PURR_DISCONNECT_REFUSED,     // The server turned it away: another build of the game, or no room
    PURR_DISCONNECT_SERVER_LEFT, // The server ended the match
    PURR_DISCONNECT_FAILED,      // It couldn't start: no network, a port in use, an address that isn't one
} purr_disconnect_reason;

// This machine's input for `tick`, into `input` (the game's input_size bytes).
// Clients call it once for each tick they run ahead, in order.
typedef void (*purr_sample_fn)(void *user, uint32_t tick, void *input);

// ---------------------------------------------------------------------------
// Server

typedef struct purr_server purr_server;

typedef struct purr_server_desc {
    const purr_game *game;
    uint32_t tick_rate;          // Ticks per second
    float dt;                    // Time.dt; 1 / tick_rate when 0
    const void *start;           // The game's purr_start, or NULL: Main
    purr_transport transports[2]; // Where players connect from; the ones with no `send` are unused
    bool local_first;            // Players on transports[0] are on this machine: their input is the server's too
    bool wait_for_first;         // Tick once the first player has joined, before anything happens without them
} purr_server_desc;

purr_server *purr_server_create(const purr_server_desc *desc, double now);
// Tells every player the match is over, and closes the transports.
void purr_server_destroy(purr_server *s);
// Reads what arrived, runs the ticks that are due, and sends each player what's new.
void purr_server_update(purr_server *s, double now);
const void *purr_server_world(const purr_server *s);
uint32_t purr_server_tick(const purr_server *s); // Ticks run so far
uint32_t purr_server_player_count(const purr_server *s);

// ---------------------------------------------------------------------------
// Client

typedef struct purr_client purr_client;

typedef struct purr_client_desc {
    const purr_game *game;
    purr_transport transport;
    purr_address server;
    purr_sample_fn sample; // NULL for a game without an input
    void *user;
    uint32_t lead;         // Ticks its inputs should reach the server early: 0 on the server's machine, 2 or more over a network
    uint64_t cookie;       // From an earlier connection to this server, to be the same player again; 0 for none
} purr_client_desc;

typedef struct purr_client_status {
    purr_session_state state;
    purr_disconnect_reason reason; // Once it's offline again
    purr_player_id player;         // This machine's player, once connected
    uint32_t ping_ms;              // Round trip to the server
    uint32_t verified_tick;        // Ticks the server has confirmed
    uint32_t predicted_tick;       // Ticks run, predicted ones included
    uint32_t resyncs;              // Times its world diverged and the server sent it again
    uint64_t cookie;               // What makes this player this player again, on the next connection
} purr_client_status;

// What views draw: the latest tick's world, the one before it, and how far
// this moment is between them (0 to 1), to blend them. Views draw a tick late
// that way, but smoothly at any tick rate. `previous` is NULL right after the
// world arrives.
typedef struct purr_view_worlds {
    const void *current; // NULL outside a match
    const void *previous;
    float alpha;
} purr_view_worlds;

purr_client *purr_client_create(const purr_client_desc *desc, double now);
// Tells the server it's leaving, and closes the transport.
void purr_client_destroy(purr_client *c);
void purr_client_update(purr_client *c, double now);
purr_client_status purr_client_status_of(const purr_client *c);
// What views read: the world as predicted, up to predicted_tick. NULL until connected.
const void *purr_client_world(const purr_client *c);
// The world up to verified_tick, which the server confirmed. NULL until connected.
const void *purr_client_verified_world(const purr_client *c);
// What views draw, as of the client's last update.
purr_view_worlds purr_client_view(const purr_client *c);

// ---------------------------------------------------------------------------
// Session: what a host program uses. Play and Host run a server with this
// machine's player on it; Join is a client of another machine's server.

typedef struct purr_session purr_session;

typedef struct purr_session_desc {
    const purr_game *game;
    uint32_t tick_rate;
    purr_sample_fn sample;
    void *user;
} purr_session_desc;

typedef enum purr_session_event_kind {
    PURR_SESSION_CONNECTED_EVENT,
    PURR_SESSION_DISCONNECTED_EVENT,
} purr_session_event_kind;

typedef struct purr_session_event {
    purr_session_event_kind kind;
    purr_disconnect_reason reason; // Disconnected
} purr_session_event;

typedef struct purr_session_status {
    purr_client_status client;
    bool server; // This machine runs the server
} purr_session_status;

purr_session *purr_session_create(const purr_session_desc *desc);
void purr_session_destroy(purr_session *s);
// A match on this machine alone. Leaves the one it's in first.
void purr_session_play(purr_session *s, const void *start, double now);
// A match others can join through `network` (its owner closes it on leaving).
void purr_session_host(purr_session *s, const void *start, purr_transport network, double now);
// Joining the server it joined last, it's the same player again, if the server
// still has room for them.
void purr_session_join(purr_session *s, purr_transport network, purr_address server, double now);
void purr_session_leave(purr_session *s);
// A session that couldn't start: reports Disconnected with the reason.
void purr_session_fail(purr_session *s, purr_disconnect_reason reason);
void purr_session_update(purr_session *s, double now);
// What views read, or NULL outside a match.
const void *purr_session_world(const purr_session *s);
// What views draw, blended between ticks (see purr_view_worlds).
purr_view_worlds purr_session_view(const purr_session *s);
// The server's world, on the machine that runs it; NULL elsewhere.
const void *purr_session_server_world(const purr_session *s);
purr_session_status purr_session_status_of(const purr_session *s);
// The next thing that happened since the last call, oldest first.
bool purr_session_next_event(purr_session *s, purr_session_event *event);

// What local code asked for, with Session.Play, Host, Join and Leave.
typedef enum purr_session_request_kind {
    PURR_REQUEST_NONE,
    PURR_REQUEST_PLAY,
    PURR_REQUEST_HOST,
    PURR_REQUEST_JOIN,
    PURR_REQUEST_LEAVE,
} purr_session_request_kind;

#define PURR_DEFAULT_PORT 7777u

typedef struct purr_session_request {
    uint32_t kind;
    uint32_t port;     // Host: the port to take players on
    char address[256]; // Join: the server's, "host" or "host:port"
} purr_session_request;
