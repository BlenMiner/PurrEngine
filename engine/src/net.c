#include "purr/net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// See purr/net.h.

// ---------------------------------------------------------------------------
// Addresses

bool purr_address_equal(const purr_address a, const purr_address b)
{
    return a.kind == b.kind && a.host == b.host && a.port == b.port;
}

static bool parse_number(const char **p, const uint32_t max, uint32_t *out)
{
    uint32_t v = 0;
    const char *s = *p;
    if (*s < '0' || *s > '9') return false;
    while (*s >= '0' && *s <= '9') {
        v = v * 10u + (uint32_t)(*s - '0');
        if (v > max) return false;
        s++;
    }
    *p = s;
    *out = v;
    return true;
}

bool purr_address_parse(const char *text, const uint16_t default_port, purr_address *out)
{
    const char *p = text;
    uint32_t host = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t part;
        if (!parse_number(&p, 255, &part)) return false;
        host = host << 8 | part;
        if (i < 3 && *p++ != '.') return false;
    }
    uint32_t port = default_port;
    if (*p == ':') {
        p++;
        if (!parse_number(&p, 65535, &port)) return false;
    }
    if (*p != '\0') return false;
    *out = (purr_address){PURR_ADDRESS_IPV4, host, (uint16_t)port};
    return true;
}

void purr_address_format(const purr_address a, char *out, const size_t size)
{
    if (a.kind == PURR_ADDRESS_IPV4) {
        snprintf(out, size, "%u.%u.%u.%u:%u", (unsigned)(a.host >> 24), (unsigned)(a.host >> 16 & 255u),
                 (unsigned)(a.host >> 8 & 255u), (unsigned)(a.host & 255u), (unsigned)a.port);
    } else if (a.kind == PURR_ADDRESS_LOOPBACK) {
        snprintf(out, size, "loopback %u", (unsigned)a.host);
    } else if (a.kind == PURR_ADDRESS_ROOM && a.host & PURR_ROOM_CODE_BIT) {
        char code[PURR_ROOM_CODE_LENGTH + 1];
        for (int i = 0; i < PURR_ROOM_CODE_LENGTH; i++) {
            code[i] = PURR_ROOM_CODE_LETTERS[a.host >> (5 * (PURR_ROOM_CODE_LENGTH - 1 - i)) & 31u];
        }
        code[PURR_ROOM_CODE_LENGTH] = '\0';
        snprintf(out, size, "room %s, player %u", code, (unsigned)a.port);
    } else if (a.kind == PURR_ADDRESS_ROOM) {
        snprintf(out, size, "room player %u", (unsigned)a.port);
    } else {
        snprintf(out, size, "nowhere");
    }
}

// ---------------------------------------------------------------------------
// Loopback

#define LOOPBACK_ENDPOINTS 64u

typedef struct datagram {
    uint32_t to;
    purr_address from;
    double at;      // When it arrives
    uint64_t order; // Sent before the ones with higher numbers
    uint32_t size;
    uint8_t *data;
} datagram;

typedef struct endpoint {
    purr_loopback *net;
    uint32_t number;
    bool open;
} endpoint;

struct purr_loopback {
    double now;
    purr_net_conditions conditions;
    uint64_t random;
    datagram *queue;
    uint32_t count;
    uint32_t capacity;
    uint64_t sent;
    endpoint endpoints[LOOPBACK_ENDPOINTS];
};

// splitmix64: a number from 0 to 1.
static double chance(purr_loopback *net)
{
    uint64_t z = (net->random += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return (double)(z >> 11) * (1.0 / 9007199254740992.0);
}

purr_loopback *purr_loopback_create(const uint64_t seed)
{
    purr_loopback *net = calloc(1, sizeof *net);
    if (!net) return NULL;
    net->random = seed;
    for (uint32_t i = 0; i < LOOPBACK_ENDPOINTS; i++) net->endpoints[i] = (endpoint){net, i + 1u, false};
    return net;
}

void purr_loopback_destroy(purr_loopback *net)
{
    if (!net) return;
    for (uint32_t i = 0; i < net->count; i++) free(net->queue[i].data);
    free(net->queue);
    free(net);
}

void purr_loopback_set_time(purr_loopback *net, const double now)
{
    net->now = now;
}

void purr_loopback_set_conditions(purr_loopback *net, const purr_net_conditions conditions)
{
    net->conditions = conditions;
}

purr_address purr_loopback_address(const uint32_t number)
{
    return (purr_address){PURR_ADDRESS_LOOPBACK, number, 0};
}

static void remove_at(purr_loopback *net, const uint32_t i)
{
    free(net->queue[i].data);
    net->queue[i] = net->queue[--net->count];
}

static void loopback_send(void *self, const purr_address to, const void *data, const uint32_t size)
{
    const endpoint *from = self;
    purr_loopback *net = from->net;
    if (to.kind != PURR_ADDRESS_LOOPBACK || to.host < 1 || to.host > LOOPBACK_ENDPOINTS) return;
    if (!net->endpoints[to.host - 1u].open) return; // Nobody there
    if (chance(net) < net->conditions.loss) return;
    if (net->count == net->capacity) {
        const uint32_t capacity = net->capacity ? net->capacity * 2u : 64u;
        datagram *queue = realloc(net->queue, capacity * sizeof *queue);
        if (!queue) return;
        net->queue = queue;
        net->capacity = capacity;
    }
    uint8_t *copy = malloc(size ? size : 1u);
    if (!copy) return;
    memcpy(copy, data, size);
    const double delay = net->conditions.latency + net->conditions.jitter * chance(net);
    net->queue[net->count++] = (datagram){to.host, purr_loopback_address(from->number), net->now + delay, net->sent++, size, copy};
}

static uint32_t loopback_receive(void *self, purr_address *from, void *data, const uint32_t capacity)
{
    const endpoint *e = self;
    purr_loopback *net = e->net;
    for (;;) {
        int32_t next = -1;
        for (uint32_t i = 0; i < net->count; i++) {
            const datagram *d = &net->queue[i];
            if (d->to != e->number || d->at > net->now) continue;
            const datagram *best = next >= 0 ? &net->queue[next] : NULL;
            if (!best || d->at < best->at || (d->at == best->at && d->order < best->order)) next = (int32_t)i;
        }
        if (next < 0) return 0;
        const datagram d = net->queue[next];
        if (d.size > capacity || d.size == 0) {
            remove_at(net, (uint32_t)next);
            continue;
        }
        memcpy(data, d.data, d.size);
        *from = d.from;
        remove_at(net, (uint32_t)next);
        return d.size;
    }
}

static void loopback_close(void *self)
{
    endpoint *e = self;
    purr_loopback *net = e->net;
    e->open = false;
    for (uint32_t i = 0; i < net->count;) {
        if (net->queue[i].to == e->number) remove_at(net, i);
        else i++;
    }
}

purr_transport purr_loopback_endpoint(purr_loopback *net, const uint32_t number)
{
    if (number < 1 || number > LOOPBACK_ENDPOINTS) return (purr_transport){0};
    endpoint *e = &net->endpoints[number - 1u];
    e->open = true;
    return (purr_transport){e, loopback_send, loopback_receive, loopback_close};
}

// ---------------------------------------------------------------------------
// Bytes

static uint8_t *room(purr_writer *w, const uint32_t size)
{
    if (w->overflow || w->capacity - w->size < size) {
        w->overflow = true;
        return NULL;
    }
    uint8_t *at = w->data + w->size;
    w->size += size;
    return at;
}

void purr_write_u8(purr_writer *w, const uint8_t v)
{
    uint8_t *at = room(w, 1);
    if (at) at[0] = v;
}

void purr_write_u16(purr_writer *w, const uint16_t v)
{
    uint8_t *at = room(w, 2);
    if (!at) return;
    at[0] = (uint8_t)v;
    at[1] = (uint8_t)(v >> 8);
}

void purr_write_u32(purr_writer *w, const uint32_t v)
{
    uint8_t *at = room(w, 4);
    if (!at) return;
    for (int i = 0; i < 4; i++) at[i] = (uint8_t)(v >> (8 * i));
}

void purr_write_u64(purr_writer *w, const uint64_t v)
{
    uint8_t *at = room(w, 8);
    if (!at) return;
    for (int i = 0; i < 8; i++) at[i] = (uint8_t)(v >> (8 * i));
}

void purr_write_bytes(purr_writer *w, const void *data, const uint32_t size)
{
    uint8_t *at = room(w, size);
    if (at && size) memcpy(at, data, size);
}

static const uint8_t *take(purr_reader *r, const uint32_t size)
{
    if (r->failed || r->size - r->at < size) {
        r->failed = true;
        return NULL;
    }
    const uint8_t *at = r->data + r->at;
    r->at += size;
    return at;
}

uint8_t purr_read_u8(purr_reader *r)
{
    const uint8_t *at = take(r, 1);
    return at ? at[0] : 0;
}

uint16_t purr_read_u16(purr_reader *r)
{
    const uint8_t *at = take(r, 2);
    return at ? (uint16_t)(at[0] | at[1] << 8) : 0;
}

uint32_t purr_read_u32(purr_reader *r)
{
    const uint8_t *at = take(r, 4);
    if (!at) return 0;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= (uint32_t)at[i] << (8 * i);
    return v;
}

uint64_t purr_read_u64(purr_reader *r)
{
    const uint8_t *at = take(r, 8);
    if (!at) return 0;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)at[i] << (8 * i);
    return v;
}

const uint8_t *purr_read_bytes(purr_reader *r, const uint32_t size)
{
    return take(r, size);
}

// ---------------------------------------------------------------------------
// Bits: the first bit is the lowest of the first byte.

void purr_bits_put(purr_bits *b, const uint32_t value, const uint32_t count)
{
    if (b->overflow) return;
    if (b->bit + count > b->capacity * 8u) {
        b->overflow = true;
        return;
    }
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t at = b->bit + i;
        const uint8_t mask = (uint8_t)(1u << (at % 8u));
        if (value >> i & 1u) b->data[at / 8u] |= mask;
        else b->data[at / 8u] &= (uint8_t)~mask;
    }
    b->bit += count;
}

uint32_t purr_bits_get(purr_bits *b, const uint32_t count)
{
    if (b->overflow || b->bit + count > b->capacity * 8u) {
        b->overflow = true;
        return 0;
    }
    uint32_t v = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t at = b->bit + i;
        v |= (uint32_t)(b->data[at / 8u] >> (at % 8u) & 1u) << i;
    }
    b->bit += count;
    return v;
}

uint32_t purr_bits_end(purr_bits *b)
{
    if (b->overflow) return 0;
    if (b->bit % 8u) b->data[b->bit / 8u] &= (uint8_t)((1u << (b->bit % 8u)) - 1u);
    return (b->bit + 7u) / 8u;
}

void purr_bits_put_f32(purr_bits *b, const float v)
{
    uint32_t bits;
    memcpy(&bits, &v, 4);
    purr_bits_put(b, bits, 32);
}

float purr_bits_get_f32(purr_bits *b)
{
    const uint32_t bits = purr_bits_get(b, 32);
    float v;
    memcpy(&v, &bits, 4);
    return v;
}

// ---------------------------------------------------------------------------
// Snapshots: (zeros, literal count, literal bytes) repeated, the counts as
// varints. A literal ends at two zeros in a row.

static void put_varint(purr_writer *w, uint32_t v)
{
    while (v >= 0x80u) {
        purr_write_u8(w, (uint8_t)(v | 0x80u));
        v >>= 7;
    }
    purr_write_u8(w, (uint8_t)v);
}

static uint32_t get_varint(purr_reader *r)
{
    uint32_t v = 0;
    for (uint32_t shift = 0; shift < 35; shift += 7) {
        const uint8_t byte = purr_read_u8(r);
        v |= (uint32_t)(byte & 0x7Fu) << shift;
        if (!(byte & 0x80u)) return v;
    }
    r->failed = true;
    return 0;
}

uint32_t purr_zeros_bound(const uint32_t size)
{
    return size + size / 16u + 16u;
}

uint32_t purr_zeros_pack(const void *data, const uint32_t size, uint8_t *out, const uint32_t capacity)
{
    const uint8_t *p = data;
    purr_writer w = {out, capacity, 0, false};
    uint32_t i = 0;
    while (i < size) {
        const uint32_t zeros_from = i;
        while (i < size && p[i] == 0) i++;
        const uint32_t literal_from = i;
        while (i < size && !(p[i] == 0 && (i + 1u >= size || p[i + 1u] == 0))) i++;
        put_varint(&w, literal_from - zeros_from);
        put_varint(&w, i - literal_from);
        purr_write_bytes(&w, p + literal_from, i - literal_from);
    }
    return w.overflow ? 0 : w.size;
}

bool purr_zeros_unpack(const uint8_t *packed, const uint32_t packed_size, void *out, const uint32_t size)
{
    uint8_t *p = out;
    purr_reader r = {packed, packed_size, 0, false};
    uint32_t at = 0;
    while (r.at < r.size) {
        const uint32_t zeros = get_varint(&r);
        if (r.failed || zeros > size - at) return false;
        memset(p + at, 0, zeros);
        at += zeros;
        const uint32_t literal = get_varint(&r);
        if (r.failed || literal > size - at) return false;
        const uint8_t *bytes = purr_read_bytes(&r, literal);
        if (!bytes) return false;
        memcpy(p + at, bytes, literal);
        at += literal;
    }
    return at == size;
}

// ---------------------------------------------------------------------------
// Hash: 8 bytes at a time, read little-endian, as every platform PurrEngine
// supports stores them.

static uint64_t mix(uint64_t h)
{
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 33;
    h *= 0xC4CEB9FE1A85EC53ull;
    h ^= h >> 33;
    return h;
}

static uint64_t add_word(uint64_t h, const uint64_t word)
{
    h ^= word * 0xBF58476D1CE4E5B9ull;
    return (h << 27 | h >> 37) * 0x94D049BB133111EBull;
}

uint64_t purr_hash_more(uint64_t h, const void *data, const size_t size)
{
    const uint8_t *p = data;
    h = add_word(h, (uint64_t)size);
    size_t i = 0;
    for (; i + 8u <= size; i += 8u) {
        uint64_t word;
        memcpy(&word, p + i, 8);
        h = add_word(h, word);
    }
    if (i < size) {
        uint64_t tail = 0;
        for (size_t k = 0; i + k < size; k++) tail |= (uint64_t)p[i + k] << (8u * k);
        h = add_word(h, tail);
    }
    return h;
}

uint64_t purr_hash_end(const uint64_t h)
{
    return mix(h);
}

uint64_t purr_hash(const void *data, const size_t size)
{
    return purr_hash_end(purr_hash_more(PURR_HASH_START, data, size));
}
