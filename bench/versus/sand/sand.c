// Sand in C: the rule of bench/sand/sand.tide on a plain array of bytes,
// stepped in place, block row by block row on as many threads as there are.
// Then the same with the rule worked out ahead for each of a block's 256
// states, so a block is a lookup.

#include <stdbool.h>
#include <stdlib.h>

#include "tide/math.h" // tide_hash_i2: the start Tide's Math.Hash gives
#include "tide/page.h" // tide_alloc: memory every thread can use on the web
#include "versus.h"

enum { EMPTY, SAND, WATER, WALL };

typedef struct sand {
    int32_t size;
    uint32_t tick;
    bool table; // Blocks by lookup
    uint8_t *cells; // Row by row from the bottom: (x, y) is cells[y * size + x]
} sand;

static void *make(const versus_flag *flags, const bool table)
{
    sand *s = tide_alloc_zeroed(1, sizeof *s);
    s->size = (int32_t)flags[0].value;
    s->table = table;
    return s;
}

static void *make_plain(const versus_flag *flags)
{
    return make(flags, false);
}

static void *make_table(const versus_flag *flags)
{
    return make(flags, true);
}

// A box of wall with ledges in it, and sand scattered over its top half.
static void setup(sand *s)
{
    const int32_t size = s->size;
    s->cells = tide_alloc_zeroed((size_t)size * (size_t)size, 1u);
    const int32_t ledges = size / 4 > 1 ? size / 4 : 1;
    const int32_t span = size / 8 > 1 ? size / 8 : 1;
    for (int32_t y = 0; y < size; y++) {
        for (int32_t x = 0; x < size; x++) {
            uint8_t *cell = &s->cells[(size_t)y * (size_t)size + (size_t)x];
            if (x == 0 || y == 0 || x == size - 1 || y == size - 1) *cell = WALL;
            else if (y % ledges == ledges / 2 && (x / span) % 2 == 1) *cell = WALL;
            else if (y > size / 2 && tide_hash_i2((tide_int2){x, y}) % 4 == 0) *cell = SAND;
        }
    }
}

static bool takes(const uint8_t there, const uint8_t mover)
{
    if (mover == SAND) return there == EMPTY || there == WATER;
    return mover == WATER && there == EMPTY;
}

static bool flows(const uint8_t left, const uint8_t right)
{
    return (left == WATER && right == EMPTY) || (left == EMPTY && right == WATER);
}

static void swap(uint8_t *a, uint8_t *b)
{
    const uint8_t was = *a;
    *a = *b;
    *b = was;
}

// A block: a and b on top, c and d below.
static void rule(uint8_t *a, uint8_t *b, uint8_t *c, uint8_t *d)
{
    if (takes(*c, *a) || takes(*d, *b) || takes(*d, *a) || takes(*c, *b)) {
        if (takes(*c, *a)) swap(a, c);
        if (takes(*d, *b)) swap(b, d);
        if (takes(*d, *a)) swap(a, d);
        if (takes(*c, *b)) swap(b, c);
    } else {
        if (flows(*c, *d)) swap(c, d);
        if (flows(*a, *b)) swap(a, b);
    }
}

// What a block becomes, by its state: c, d, a and b, two bits each from the
// lowest. Every material fits in two bits.
static uint8_t blocks[256];
static bool blocks_made;

static void make_blocks(void)
{
    blocks_made = true;
    for (uint32_t k = 0; k < 256u; k++) {
        uint8_t c = k & 3u, d = k >> 2 & 3u, a = k >> 4 & 3u, b = k >> 6 & 3u;
        rule(&a, &b, &c, &d);
        blocks[k] = (uint8_t)(c | d << 2 | a << 4 | b << 6);
    }
}

typedef struct step_job {
    sand *s;
    int32_t offset; // Of the blocks: 0 or 1
    uint32_t rows;  // Of blocks
    uint32_t pieces;
} step_job;

static void step(void *context, const uint32_t piece)
{
    const step_job *j = context;
    const int32_t size = j->s->size;
    const uint32_t from = (uint32_t)((uint64_t)j->rows * piece / j->pieces);
    const uint32_t to = (uint32_t)((uint64_t)j->rows * (piece + 1u) / j->pieces);
    for (uint32_t r = from; r < to; r++) {
        const int32_t y = j->offset + 2 * (int32_t)r;
        uint8_t *restrict low = &j->s->cells[(size_t)y * (size_t)size];
        uint8_t *restrict high = low + size;
        if (j->s->table) {
            for (int32_t x = j->offset; x + 1 < size; x += 2) {
                const uint8_t k = (uint8_t)(low[x] | low[x + 1] << 2 | high[x] << 4 | high[x + 1] << 6);
                const uint8_t n = blocks[k];
                if (n == k) continue;
                low[x] = n & 3u;
                low[x + 1] = n >> 2 & 3u;
                high[x] = n >> 4 & 3u;
                high[x + 1] = n >> 6;
            }
        } else {
            for (int32_t x = j->offset; x + 1 < size; x += 2) {
                uint8_t a = high[x], b = high[x + 1], c = low[x], d = low[x + 1];
                if (a == b && b == c && c == d) continue;
                rule(&a, &b, &c, &d);
                low[x] = c;
                low[x + 1] = d;
                high[x] = a;
                high[x + 1] = b;
            }
        }
    }
}

static void tick(void *state, const tide_jobs *jobs)
{
    sand *s = state;
    if (!s->cells) {
        setup(s);
        if (s->table && !blocks_made) make_blocks();
    }
    const int32_t offset = (int32_t)(s->tick % 2u);
    const uint32_t rows = s->size >= 2 ? (uint32_t)(s->size - offset) / 2u : 0u;
    // Threads pay from about 200 rows of blocks, or 400 by lookup (on a
    // 12-core Ryzen 9 7900X)
    step_job j = {s, offset, rows, versus_pieces(jobs, rows, s->table ? 384u : 192u)};
    versus_for(jobs, j.pieces, step, &j);
    s->tick++;
}

static uint64_t digest(const void *state)
{
    const sand *s = state;
    uint64_t sum = 0;
    for (int32_t y = 0; y < s->size; y++) {
        for (int32_t x = 0; x < s->size; x++) {
            const int32_t item[3] = {x, y, s->cells[(size_t)y * (size_t)s->size + (size_t)x]};
            sum += versus_item(item, sizeof item);
        }
    }
    return sum;
}

static void destroy(void *state)
{
    sand *s = state;
    free(s->cells);
    free(s);
}

const versus_way sand_c = {"C", make_plain, tick, digest, destroy};
const versus_way sand_c_table = {"C, lookup", make_table, tick, digest, destroy};
