#include "tide/delta.h"

#include <stdlib.h>
#include <string.h>

// See tide/delta.h.
//
// A delta: its flags (1: regions are XOR'd with the base's), the world's
// hash, then each part's numbers and regions. A part's regions go as tokens:
// how many are the same as the base's, then the bytes of the one after them,
// and at its end, how many more are the same.

#define FLAG_XOR 1u

// ---------------------------------------------------------------------------
// Bytes, as runs of zeros and literals

// Byte `i` of `now` XOR'd with `base`, which is zeros past `base_size`.
static inline uint8_t byte_at(const uint8_t *now, const uint8_t *base, const uint32_t base_size, const uint32_t i)
{
    return i < base_size ? (uint8_t)(now[i] ^ base[i]) : now[i];
}

static void put_bytes(tide_writer *w, const uint8_t *now, const uint32_t size, const uint8_t *base, uint32_t base_size)
{
    if (base_size > size) base_size = size;
    uint32_t i = 0;
    while (i < size) {
        const uint32_t zeros_from = i;
        // Eight at a time while they're the same, then one at a time
        while (i + 8u <= size && (i + 8u <= base_size || i >= base_size)) {
            uint64_t a;
            uint64_t b = 0;
            memcpy(&a, now + i, 8);
            if (i < base_size) memcpy(&b, base + i, 8);
            if (a != b) break;
            i += 8u;
        }
        while (i < size && byte_at(now, base, base_size, i) == 0) i++;
        const uint32_t literal_from = i;
        while (i < size
               && !(byte_at(now, base, base_size, i) == 0
                    && (i + 1u >= size || byte_at(now, base, base_size, i + 1u) == 0))) {
            i++;
        }
        tide_write_varint(w, literal_from - zeros_from);
        tide_write_varint(w, i - literal_from);
        if (i == literal_from) continue;
        uint8_t *out = tide_write_space(w, i - literal_from);
        if (!out) return;
        for (uint32_t k = literal_from; k < i; k++) out[k - literal_from] = byte_at(now, base, base_size, k);
    }
}

// Into `out`, `size` bytes that are zeros, or the base's bytes when XOR'd.
static bool get_bytes(tide_reader *r, uint8_t *out, const uint32_t size, const uint8_t *base, const uint32_t base_size)
{
    if (base && base_size) memcpy(out, base, base_size < size ? base_size : size);
    uint32_t at = 0;
    while (at < size) {
        const uint32_t zeros = tide_read_varint(r);
        const uint32_t literal = tide_read_varint(r);
        if (r->failed || (zeros == 0 && literal == 0) || zeros > size - at || literal > size - at - zeros) return false;
        at += zeros;
        const uint8_t *bytes = tide_read_bytes(r, literal);
        if (!bytes) return false;
        for (uint32_t k = 0; k < literal; k++) out[at + k] ^= bytes[k];
        at += literal;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Writing

void tide_delta_begin(tide_delta_writer *d, const bool base, const uint8_t *need, const uint32_t need_size,
                      const uint64_t hash)
{
    *d = (tide_delta_writer){.bytes = {.grows = true}};
    d->by_need = need != NULL;
    d->xor = base && !d->by_need;
    d->need = (tide_reader){need, need_size, 0, false};
    d->lacking = true; // Its first run is of regions it has
    tide_write_varint(&d->bytes, d->xor ? FLAG_XOR : 0u);
    tide_write_u64(&d->bytes, hash);
}

void tide_delta_number(tide_delta_writer *d, const uint32_t v)
{
    tide_write_varint(&d->bytes, v);
}

// Whether the receiver lacks the next region, by what it said.
static bool lacks_next(tide_delta_writer *d)
{
    while (d->left == 0) {
        if (d->need.at >= d->need.size || d->need.failed) { // Past what it said: everything
            d->lacking = true;
            d->left = UINT32_MAX;
            break;
        }
        d->left = tide_read_varint(&d->need);
        d->lacking = !d->lacking;
        if (d->need.failed) d->left = 0;
    }
    if (d->left != UINT32_MAX) d->left--;
    return d->lacking;
}

void tide_delta_region(tide_delta_writer *d, const void *now, const uint32_t size, const void *base,
                       const uint32_t base_size, const bool same)
{
    bool send;
    if (d->by_need) send = lacks_next(d);
    else send = !(d->xor && base && base_size == size && (same || memcmp(now, base, size) == 0));
    if (!send) {
        d->same++;
        return;
    }
    tide_write_varint(&d->bytes, d->same);
    d->same = 0;
    put_bytes(&d->bytes, now, size, d->xor ? base : NULL, d->xor && base ? base_size : 0u);
}

void tide_delta_close(tide_delta_writer *d)
{
    tide_write_varint(&d->bytes, d->same);
    d->same = 0;
}

uint8_t *tide_delta_end(tide_delta_writer *d, uint32_t *size)
{
    if (d->bytes.overflow) tide_out_of_memory();
    *size = d->bytes.size;
    return d->bytes.data;
}

// ---------------------------------------------------------------------------
// Reading

bool tide_delta_open(tide_delta_reader *d, const uint8_t *data, const uint32_t size, const bool base, uint64_t *hash)
{
    *d = (tide_delta_reader){.bytes = {data, size, 0, false}};
    const uint32_t flags = tide_read_varint(&d->bytes);
    *hash = tide_read_u64(&d->bytes);
    d->xor = (flags & FLAG_XOR) != 0;
    return !d->bytes.failed && (flags & ~FLAG_XOR) == 0 && (!d->xor || base);
}

uint32_t tide_delta_get_number(tide_delta_reader *d)
{
    return tide_read_varint(&d->bytes);
}

// Whether the next region is the same as the base's; else its bytes are next.
static bool next_same(tide_delta_reader *d)
{
    if (!d->token) {
        d->same = tide_read_varint(&d->bytes);
        d->token = true;
    }
    if (d->same) {
        d->same--;
        return true;
    }
    d->token = false;
    return false;
}

bool tide_delta_get(tide_delta_reader *d, void *out, const uint32_t size, const void *base, const uint32_t base_size)
{
    if (d->bytes.failed) return false;
    if (next_same(d)) {
        if (!base || base_size != size || d->bytes.failed) return false;
        if (size) memcpy(out, base, size);
        return true;
    }
    return get_bytes(&d->bytes, out, size, d->xor ? base : NULL, d->xor && base ? base_size : 0u);
}

tide_page *tide_delta_page(tide_delta_reader *d, const uint32_t size, const uint32_t page_size, const uint32_t refs,
                           tide_page *base, const uint32_t base_size, const bool share)
{
    if (d->bytes.failed || size > page_size) return NULL;
    const void *from = base ? tide_page_data(base) : NULL;
    if (next_same(d)) {
        if (!base || base_size != size || d->bytes.failed) return NULL;
        if (share) {
            base->refs += refs;
            return base;
        }
        tide_page *p = tide_page_new(page_size);
        p->refs = refs;
        if (size) memcpy(tide_page_data(p), from, size);
        return p;
    }
    tide_page *p = tide_page_new(page_size);
    p->refs = refs;
    if (!get_bytes(&d->bytes, tide_page_data(p), size, d->xor ? from : NULL, d->xor && base ? base_size : 0u)) {
        tide_page_release(p, refs);
        return NULL;
    }
    return p;
}

bool tide_delta_closed(tide_delta_reader *d)
{
    if (!d->token) {
        d->same = tide_read_varint(&d->bytes);
        d->token = true;
    }
    const bool ok = !d->bytes.failed && d->same == 0;
    d->token = false;
    return ok;
}

// ---------------------------------------------------------------------------
// What a base lacks

void tide_needs_begin(tide_needs *n, const uint8_t *hashes, const uint32_t size, uint8_t *out, const uint32_t capacity)
{
    *n = (tide_needs){.list = {hashes, size, 0, false}, .runs = {out, capacity, 0, false, false}};
}

// Ends the run being counted.
static void end_run(tide_needs *n)
{
    if (n->full) return;
    const uint32_t before = n->runs.size;
    tide_write_varint(&n->runs, n->run);
    if (n->runs.overflow) { // What didn't fit, it lacks
        n->runs.size = before;
        n->full = true;
    }
}

static void add(tide_needs *n, const bool lacking)
{
    if (lacking != n->lacking) {
        end_run(n);
        n->lacking = lacking;
        n->run = 0;
    }
    n->run++;
}

uint32_t tide_needs_count(tide_needs *n)
{
    const uint32_t count = tide_read_varint(&n->list);
    // Each has a hash: no more than the list has room for
    if (count > (n->list.size - n->list.at) / 8u) n->list.failed = true;
    return n->list.failed ? 0u : count;
}

void tide_needs_put(tide_needs *n, const bool have, const uint64_t hash)
{
    const uint64_t theirs = tide_read_u64(&n->list);
    add(n, n->list.failed || !have || hash != theirs);
}

void tide_needs_one(tide_needs *n, const void *data, const uint32_t size)
{
    const uint32_t count = tide_needs_count(n);
    for (uint32_t i = 0; i < count; i++) tide_needs_put(n, i == 0, i == 0 ? tide_hash(data, size) : 0u);
}

uint32_t tide_needs_end(tide_needs *n)
{
    if (n->list.failed || n->list.at != n->list.size) return 0;
    if (!n->lacking) end_run(n); // A last run of lacking ones goes without saying
    return n->runs.size;
}
