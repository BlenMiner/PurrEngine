#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// What sessions (purr/session.h) send datagrams with: addresses, transports,
// an in-process loopback network, and writing and reading bytes and bits.
//
// Temporary implementation written by Claude; the project owner takes it over
// later. Real networks come from the platform layer (purr_platform_udp_open);
// this file never touches the operating system.

#define PURR_NET_MTU 1200u // The largest datagram sessions send

typedef enum purr_address_kind {
    PURR_ADDRESS_NONE,
    PURR_ADDRESS_LOOPBACK, // An endpoint of a purr_loopback network: `host` is its number
    PURR_ADDRESS_IPV4,     // `host` in host byte order
    // A player in a room (purr_platform_room_host): `port` is its number there,
    // 0 for the room's host. To the players who join, `host` is also the room's
    // code, packed (PURR_ROOM_CODE_BIT and 5 bits a character), so each room's
    // host is a server of its own.
    PURR_ADDRESS_ROOM,
} purr_address_kind;

#define PURR_ROOM_CODE_BIT 0x80000000u
// A room's code: 6 of these characters, which leave out look-alikes (0 and O,
// 1 and I).
#define PURR_ROOM_CODE_LETTERS "23456789ABCDEFGHJKLMNPQRSTUVWXYZ"
#define PURR_ROOM_CODE_LENGTH 6

typedef struct purr_address {
    uint32_t kind;
    uint32_t host;
    uint16_t port;
} purr_address;

bool purr_address_equal(purr_address a, purr_address b);

// "192.168.1.5" or "192.168.1.5:7777". Names like "localhost" need the platform
// layer (purr_platform_resolve).
bool purr_address_parse(const char *text, uint16_t default_port, purr_address *out);

// Writes "192.168.1.5:7777", "loopback 2" or "room K7QF2M, player 0" into `out`.
void purr_address_format(purr_address a, char *out, size_t size);

// Carries datagrams, unreliably: they can be lost, duplicated or come out of
// order, and sessions make up for all of it.
typedef struct purr_transport {
    void *self;
    void (*send)(void *self, purr_address to, const void *data, uint32_t size);
    // The next datagram that arrived, copied into `data`: its size, or 0 when
    // there's none. Longer ones are dropped.
    uint32_t (*receive)(void *self, purr_address *from, void *data, uint32_t capacity);
    void (*close)(void *self);
} purr_transport;

// ---------------------------------------------------------------------------
// Loopback: endpoints in one process that send each other datagrams, for
// single-player (a server and its player on one machine) and for tests, which
// can make it as bad as a real network.

typedef struct purr_net_conditions {
    double latency; // Seconds each way
    double jitter;  // Up to this many seconds more, at random, so datagrams come out of order
    double loss;    // The share of datagrams lost, 0 to 1
} purr_net_conditions;

typedef struct purr_loopback purr_loopback;

// `seed` decides which datagrams are lost and late, so tests repeat exactly.
purr_loopback *purr_loopback_create(uint64_t seed);
void purr_loopback_destroy(purr_loopback *net);
// Datagrams arrive once the network's time reaches theirs. Its owner sets it.
void purr_loopback_set_time(purr_loopback *net, double now);
void purr_loopback_set_conditions(purr_loopback *net, purr_net_conditions conditions);
// Endpoint `number` (1 and up), at address {PURR_ADDRESS_LOOPBACK, number}.
// Closing it drops what's on its way to it.
purr_transport purr_loopback_endpoint(purr_loopback *net, uint32_t number);
purr_address purr_loopback_address(uint32_t number);

// ---------------------------------------------------------------------------
// Bytes, little-endian. A writer that runs out of room sets `overflow` and
// writes nothing more; a reader that runs past the end sets `failed` and reads
// zeros.

typedef struct purr_writer {
    uint8_t *data;
    uint32_t capacity;
    uint32_t size;
    bool overflow;
} purr_writer;

typedef struct purr_reader {
    const uint8_t *data;
    uint32_t size;
    uint32_t at;
    bool failed;
} purr_reader;

void purr_write_u8(purr_writer *w, uint8_t v);
void purr_write_u16(purr_writer *w, uint16_t v);
void purr_write_u32(purr_writer *w, uint32_t v);
void purr_write_u64(purr_writer *w, uint64_t v);
void purr_write_bytes(purr_writer *w, const void *data, uint32_t size);

uint8_t purr_read_u8(purr_reader *r);
uint16_t purr_read_u16(purr_reader *r);
uint32_t purr_read_u32(purr_reader *r);
uint64_t purr_read_u64(purr_reader *r);
// Points at the next `size` bytes, or NULL if there aren't that many.
const uint8_t *purr_read_bytes(purr_reader *r, uint32_t size);

// Bits, for inputs: generated code writes each field in as few as it needs.
typedef struct purr_bits {
    uint8_t *data;
    uint32_t capacity; // Bytes
    uint32_t bit;      // Bits written or read so far
    bool overflow;     // Writing: out of room. Reading: past the end.
} purr_bits;

void purr_bits_put(purr_bits *b, uint32_t value, uint32_t count); // The low `count` bits, up to 32
uint32_t purr_bits_get(purr_bits *b, uint32_t count);

static inline void purr_bits_put_bool(purr_bits *b, const bool v)
{
    purr_bits_put(b, v ? 1u : 0u, 1);
}

static inline bool purr_bits_get_bool(purr_bits *b)
{
    return purr_bits_get(b, 1) != 0;
}

// Floats go as their exact bits.
void purr_bits_put_f32(purr_bits *b, float v);
float purr_bits_get_f32(purr_bits *b);

// Done writing: clears the rest of the last byte, so the same value always
// packs to the same bytes, and returns how many there are (0 if out of room).
uint32_t purr_bits_end(purr_bits *b);

// ---------------------------------------------------------------------------
// Snapshots: a world is mostly zeros (empty rows, unused entities), so runs of
// zeros shrink to a few bytes. Any other byte is kept as it is.

// The most packing `size` bytes can take.
uint32_t purr_zeros_bound(uint32_t size);
// Packs `size` bytes into `out`: its size, or 0 if it doesn't fit.
uint32_t purr_zeros_pack(const void *data, uint32_t size, uint8_t *out, uint32_t capacity);
// Unpacks into exactly `size` bytes; false if the packed bytes don't make that.
bool purr_zeros_unpack(const uint8_t *packed, uint32_t packed_size, void *out, uint32_t size);

// A 64-bit hash of `size` bytes, the same on every platform. Sessions compare
// worlds by it to find divergence.
uint64_t purr_hash(const void *data, size_t size);

// The same over several pieces: start from PURR_HASH_START, add each piece,
// and end. What a world hashes is its live parts (see purr_world_hash).
#define PURR_HASH_START 0x9E3779B97F4A7C15ull
uint64_t purr_hash_more(uint64_t h, const void *data, size_t size);
uint64_t purr_hash_end(uint64_t h);
