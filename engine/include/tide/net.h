#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// What sessions (tide/session.h) send datagrams with: addresses, transports,
// an in-process loopback network, and writing and reading bytes and bits.
//
// Temporary implementation written by Claude; the project owner takes it over
// later. Real networks come from the platform layer (tide_platform_udp_open);
// this file never touches the operating system.

#define TIDE_NET_MTU 1200u // The largest datagram sessions send

typedef enum tide_address_kind {
    TIDE_ADDRESS_NONE,
    TIDE_ADDRESS_LOOPBACK, // An endpoint of a tide_loopback network: `host` is its number
    TIDE_ADDRESS_IPV4,     // `host` in host byte order
    // A player in a room (tide_platform_room_host): `port` is its number there,
    // 0 for the room's host. To the players who join, `host` is also the room's
    // code, packed (TIDE_ROOM_CODE_BIT and 5 bits a character), so each room's
    // host is a server of its own.
    TIDE_ADDRESS_ROOM,
} tide_address_kind;

#define TIDE_ROOM_CODE_BIT 0x80000000u
// A room's code: 6 of these characters, which leave out look-alikes (0 and O,
// 1 and I).
#define TIDE_ROOM_CODE_LETTERS "23456789ABCDEFGHJKLMNPQRSTUVWXYZ"
#define TIDE_ROOM_CODE_LENGTH 6

typedef struct tide_address {
    uint32_t kind;
    uint32_t host;
    uint16_t port;
} tide_address;

bool tide_address_equal(tide_address a, tide_address b);

// "192.168.1.5" or "192.168.1.5:7777". Names like "localhost" need the platform
// layer (tide_platform_resolve).
bool tide_address_parse(const char *text, uint16_t default_port, tide_address *out);

// Writes "192.168.1.5:7777", "loopback 2" or "room K7QF2M, player 0" into `out`.
void tide_address_format(tide_address a, char *out, size_t size);

// Carries datagrams, unreliably: they can be lost, duplicated or come out of
// order, and sessions make up for all of it.
typedef struct tide_transport {
    void *self;
    void (*send)(void *self, tide_address to, const void *data, uint32_t size);
    // The next datagram that arrived, copied into `data`: its size, or 0 when
    // there's none. Longer ones are dropped.
    uint32_t (*receive)(void *self, tide_address *from, void *data, uint32_t capacity);
    void (*close)(void *self);
} tide_transport;

// ---------------------------------------------------------------------------
// Loopback: endpoints in one process that send each other datagrams, for
// single-player (a server and its player on one machine) and for tests, which
// can make it as bad as a real network.

typedef struct tide_net_conditions {
    double latency; // Seconds each way
    double jitter;  // Up to this many seconds more, at random, so datagrams come out of order
    double loss;    // The share of datagrams lost, 0 to 1
} tide_net_conditions;

typedef struct tide_loopback tide_loopback;

// `seed` decides which datagrams are lost and late, so tests repeat exactly.
tide_loopback *tide_loopback_create(uint64_t seed);
void tide_loopback_destroy(tide_loopback *net);
// Datagrams arrive once the network's time reaches theirs. Its owner sets it.
void tide_loopback_set_time(tide_loopback *net, double now);
void tide_loopback_set_conditions(tide_loopback *net, tide_net_conditions conditions);
// Endpoint `number` (1 and up), at address {TIDE_ADDRESS_LOOPBACK, number}.
// Closing it drops what's on its way to it.
tide_transport tide_loopback_endpoint(tide_loopback *net, uint32_t number);
tide_address tide_loopback_address(uint32_t number);

// ---------------------------------------------------------------------------
// Bytes, little-endian. A writer that runs out of room sets `overflow` and
// writes nothing more; a reader that runs past the end sets `failed` and reads
// zeros.

typedef struct tide_writer {
    uint8_t *data;
    uint32_t capacity;
    uint32_t size;
    bool overflow;
} tide_writer;

typedef struct tide_reader {
    const uint8_t *data;
    uint32_t size;
    uint32_t at;
    bool failed;
} tide_reader;

void tide_write_u8(tide_writer *w, uint8_t v);
void tide_write_u16(tide_writer *w, uint16_t v);
void tide_write_u32(tide_writer *w, uint32_t v);
void tide_write_u64(tide_writer *w, uint64_t v);
void tide_write_bytes(tide_writer *w, const void *data, uint32_t size);

uint8_t tide_read_u8(tide_reader *r);
uint16_t tide_read_u16(tide_reader *r);
uint32_t tide_read_u32(tide_reader *r);
uint64_t tide_read_u64(tide_reader *r);
// Points at the next `size` bytes, or NULL if there aren't that many.
const uint8_t *tide_read_bytes(tide_reader *r, uint32_t size);

// Bits, for inputs: generated code writes each field in as few as it needs.
typedef struct tide_bits {
    uint8_t *data;
    uint32_t capacity; // Bytes
    uint32_t bit;      // Bits written or read so far
    bool overflow;     // Writing: out of room. Reading: past the end.
} tide_bits;

void tide_bits_put(tide_bits *b, uint32_t value, uint32_t count); // The low `count` bits, up to 32
uint32_t tide_bits_get(tide_bits *b, uint32_t count);

static inline void tide_bits_put_bool(tide_bits *b, const bool v)
{
    tide_bits_put(b, v ? 1u : 0u, 1);
}

static inline bool tide_bits_get_bool(tide_bits *b)
{
    return tide_bits_get(b, 1) != 0;
}

// Floats go as their exact bits.
void tide_bits_put_f32(tide_bits *b, float v);
float tide_bits_get_f32(tide_bits *b);

// Done writing: clears the rest of the last byte, so the same value always
// packs to the same bytes, and returns how many there are (0 if out of room).
uint32_t tide_bits_end(tide_bits *b);

// ---------------------------------------------------------------------------
// Snapshots: a world is mostly zeros (empty rows, unused entities), so runs of
// zeros shrink to a few bytes. Any other byte is kept as it is.

// The most packing `size` bytes can take.
uint32_t tide_zeros_bound(uint32_t size);
// Packs `size` bytes into `out`: its size, or 0 if it doesn't fit.
uint32_t tide_zeros_pack(const void *data, uint32_t size, uint8_t *out, uint32_t capacity);
// Unpacks into exactly `size` bytes; false if the packed bytes don't make that.
bool tide_zeros_unpack(const uint8_t *packed, uint32_t packed_size, void *out, uint32_t size);

// A 64-bit hash of `size` bytes, the same on every platform. Sessions compare
// worlds by it to find divergence.
uint64_t tide_hash(const void *data, size_t size);

// The same over several pieces: start from TIDE_HASH_START, add each piece,
// and end. What a world hashes is its live parts (see tide_world_hash).
#define TIDE_HASH_START 0x9E3779B97F4A7C15ull
uint64_t tide_hash_more(uint64_t h, const void *data, size_t size);
uint64_t tide_hash_end(uint64_t h);
