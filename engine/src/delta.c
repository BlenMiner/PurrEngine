#include "tide/delta.h"

#include <stdlib.h>
#include <string.h>

// See tide/delta.h.
//
// A delta: its flags (1: regions are XOR'd with the base's), the world's
// hash, then each part's numbers and regions. A part's regions go as tokens:
// how many are the same as the base's, then the stride and bytes of the one
// after them, and at its end, how many more are the same.

#define FLAG_XOR 1u
#define MAX_STRIDE 127u // So it takes a byte

// ---------------------------------------------------------------------------
// Bytes, as runs of zeros and literals

// Byte `i` of `d`, XOR'd with the one `stride` before it (0: none).
static inline uint8_t filtered(const uint8_t *d, const uint32_t stride, const uint32_t i)
{
    return stride && i >= stride ? (uint8_t)(d[i] ^ d[i - stride]) : d[i];
}

// The 8 filtered bytes from `i` (at least `stride`), as a word.
static inline uint64_t filtered8(const uint8_t *d, const uint32_t stride, const uint32_t i)
{
    uint64_t a;
    uint64_t b = 0;
    memcpy(&a, d + i, 8);
    if (stride) memcpy(&b, d + i - stride, 8);
    return a ^ b;
}

// Whether any of a word's bytes is zero.
static inline bool has_zero(const uint64_t v)
{
    return ((v - 0x0101010101010101ull) & ~v & 0x8080808080808080ull) != 0;
}

// `size` bytes of `d`, filtered by `stride`, as runs into `w`.
static void put_runs(tide_writer *w, const uint8_t *d, const uint32_t size, const uint32_t stride)
{
    uint32_t i = 0;
    while (i < size) {
        const uint32_t zeros_from = i;
        // Eight at a time while they're zeros, then one at a time
        if (i >= stride) {
            while (i + 8u <= size && filtered8(d, stride, i) == 0) i += 8u;
        }
        while (i < size && filtered(d, stride, i) == 0) i++;
        // A literal ends at two zeros in a row: none start in 8 bytes with no zeros
        const uint32_t literal_from = i;
        while (i < size) {
            if (i >= stride) {
                while (i + 8u <= size && !has_zero(filtered8(d, stride, i))) i += 8u;
                if (i >= size) break;
            }
            if (filtered(d, stride, i) == 0 && (i + 1u >= size || filtered(d, stride, i + 1u) == 0)) break;
            i++;
        }
        const uint32_t literal = i - literal_from;
        tide_write_varint(w, literal_from - zeros_from);
        tide_write_varint(w, literal);
        if (!literal) continue;
        uint8_t *out = tide_write_space(w, literal);
        if (!out) return;
        for (uint32_t k = literal_from; k < i; k++) out[k - literal_from] = filtered(d, stride, k);
    }
}

// How many of the bytes of `d` from `from` to `to`, filtered by `stride`,
// aren't zero: about what they take as runs, counted 8 at a time.
static uint32_t nonzero_bytes(const uint8_t *d, const uint32_t from, const uint32_t to, const uint32_t stride)
{
    const uint64_t low7 = 0x7F7F7F7F7F7F7F7Full;
    uint32_t count = 0;
    uint32_t i = from;
    for (; i < to && i < stride; i++) count += d[i] != 0;
    for (; i + 8u <= to; i += 8u) {
        const uint64_t v = filtered8(d, stride, i);
        const uint64_t zeros = ~(((v & low7) + low7) | v | low7) >> 7; // 1 in each byte that's zero
        count += 8u - (uint32_t)(zeros * 0x0101010101010101ull >> 56);
    }
    for (; i < to; i++) count += filtered(d, stride, i) != 0;
    return count;
}

// The same for a sample of `size` bytes: all of them when they're few, else 4
// stretches of SAMPLE bytes across them.
#define SAMPLE 128u
static uint32_t sampled_nonzero(const uint8_t *d, const uint32_t size, const uint32_t stride)
{
    if (size <= 8u * SAMPLE) return nonzero_bytes(d, 0, size, stride);
    uint32_t count = 0;
    for (uint32_t k = 0; k < 4u; k++) {
        const uint32_t from = (uint32_t)((uint64_t)size * k / 4u);
        count += nonzero_bytes(d, from, from + SAMPLE, stride);
    }
    return count;
}

// A region: `now` XOR'd with `base`, which is zeros past `base_size`, then
// filtered by whichever stride leaves the fewest bytes that aren't zero in a
// sample of it: none, a few small ones, or `stride`, the size of what's in it.
static void put_bytes(tide_delta_writer *d, const uint8_t *now, const uint32_t size, const uint8_t *base,
                      uint32_t base_size, const uint32_t stride)
{
    if (base_size > size) base_size = size;
    const uint8_t *bytes = now;
    if (base && base_size) {
        if (d->scratch_size < size) {
            free(d->scratch);
            d->scratch = malloc(size);
            if (!d->scratch) tide_out_of_memory();
            d->scratch_size = size;
        }
        for (uint32_t i = 0; i < base_size; i++) d->scratch[i] = (uint8_t)(now[i] ^ base[i]);
        memcpy(d->scratch + base_size, now + base_size, size - base_size);
        bytes = d->scratch;
    }
    const uint32_t strides[] = {1u, 2u, 4u, 8u, stride};
    const uint32_t count = stride == 1u || stride == 2u || stride == 4u || stride == 8u ? 4u : 5u;
    uint32_t best = 0;
    uint32_t fewest = sampled_nonzero(bytes, size, 0);
    for (uint32_t k = 0; k < count && fewest > 0; k++) {
        const uint32_t s = strides[k];
        if (s == 0 || s > MAX_STRIDE || s >= size) continue;
        const uint32_t left = sampled_nonzero(bytes, size, s);
        if (left + left / 8u < fewest) { // Only for a real gain: on noise, any stride is as good
            fewest = left;
            best = s;
        }
    }
    tide_write_varint(&d->bytes, best);
    put_runs(&d->bytes, bytes, size, best);
}

// Into `out`, `size` bytes over zeros, or over the base's bytes when XOR'd.
static bool get_bytes(tide_reader *r, uint8_t *out, const uint32_t size, const uint8_t *base, const uint32_t base_size)
{
    const uint32_t stride = tide_read_varint(r);
    if (r->failed || stride > MAX_STRIDE) return false;
    // Over the base's bytes, or filtered over zeros, then unfiltered and XOR'd with the base's
    if (!stride && base && base_size) memcpy(out, base, base_size < size ? base_size : size);
    else memset(out, 0, size);
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
    if (!stride) return true;
    uint32_t i = stride;
    if (stride >= 8u) { // Eight at a time, from bytes already done
        for (; i + 8u <= size; i += 8u) {
            uint64_t a;
            uint64_t b;
            memcpy(&a, out + i, 8);
            memcpy(&b, out + i - stride, 8);
            a ^= b;
            memcpy(out + i, &a, 8);
        }
    }
    for (; i < size; i++) out[i] ^= out[i - stride];
    if (base) {
        for (uint32_t k = 0; k < base_size && k < size; k++) out[k] ^= base[k];
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
                       const uint32_t base_size, const bool same, const uint32_t stride)
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
    put_bytes(d, now, size, d->xor ? base : NULL, d->xor && base ? base_size : 0u, stride);
}

void tide_delta_close(tide_delta_writer *d)
{
    tide_write_varint(&d->bytes, d->same);
    d->same = 0;
}

uint8_t *tide_delta_end(tide_delta_writer *d, uint32_t *size)
{
    if (d->bytes.overflow) tide_out_of_memory();
    free(d->scratch);
    d->scratch = NULL;
    d->scratch_size = 0;
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
