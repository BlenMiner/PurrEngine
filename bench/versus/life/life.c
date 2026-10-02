// Life in C: bytes, with a border of dead cells so no cell needs a check, in
// two arrays that take turns. A row's cells count their neighbours side by
// side, which the compiler does sixteen at a time with SIMD, and rows go to
// as many threads as there are.

#include <stdbool.h>
#include <stdlib.h>

#include "tide/math.h" // tide_hash_i2: the start Tide's Math.Hash gives
#include "tide/page.h" // tide_alloc: memory every thread can use on the web
#include "versus.h"

typedef struct life {
    int32_t size;
    size_t stride;        // A row's bytes, border and all
    uint8_t *now, *next;  // (x, y) is at [(y + 1) * stride + x + 1]
} life;

static void *make(const versus_flag *flags)
{
    life *l = tide_alloc_zeroed(1, sizeof *l);
    l->size = (int32_t)flags[0].value;
    return l;
}

static void setup(life *l)
{
    const int32_t size = l->size;
    l->stride = (size_t)size + 2u;
    const size_t bytes = l->stride * l->stride;
    // Apart by a few cache lines more than their size, so that a row of one
    // and the same row of the other don't share their address's low bits
    // (see particles.c)
    l->now = tide_alloc_zeroed(2u * bytes + 192u, 1u);
    l->next = l->now + bytes + 192u;
    for (int32_t y = 0; y < size; y++) {
        for (int32_t x = 0; x < size; x++) {
            l->now[(size_t)(y + 1) * l->stride + (size_t)x + 1u] = tide_hash_i2((tide_int2){x, y}) % 3 == 0;
        }
    }
}

typedef struct step_job {
    const life *l;
    uint32_t pieces;
} step_job;

static void step(void *context, const uint32_t piece)
{
    const step_job *j = context;
    const life *l = j->l;
    const uint32_t size = (uint32_t)l->size;
    const uint32_t from = (uint32_t)((uint64_t)size * piece / j->pieces);
    const uint32_t to = (uint32_t)((uint64_t)size * (piece + 1u) / j->pieces);
    for (uint32_t y = from + 1u; y < to + 1u; y++) {
        const uint8_t *restrict below = &l->now[(y - 1u) * l->stride];
        const uint8_t *restrict row = below + l->stride;
        const uint8_t *restrict above = row + l->stride;
        uint8_t *restrict out = &l->next[y * l->stride];
        for (uint32_t x = 1; x <= size; x++) {
            const uint8_t around = (uint8_t)(below[x - 1u] + below[x] + below[x + 1u] + row[x - 1u] + row[x + 1u] +
                                             above[x - 1u] + above[x] + above[x + 1u]);
            out[x] = (uint8_t)((around == 3u) | ((around == 2u) & row[x]));
        }
    }
}

static void tick(void *state, const tide_jobs *jobs)
{
    life *l = state;
    if (!l->now) setup(l); // Tide's first tick makes the grid, then steps it
    // Threads pay from about 1500 rows (on a 12-core Ryzen 9 7900X)
    step_job j = {l, versus_pieces(jobs, (uint32_t)l->size, 1536u)};
    versus_for(jobs, j.pieces, step, &j);
    uint8_t *const was = l->now;
    l->now = l->next;
    l->next = was;
}

static uint64_t digest(const void *state)
{
    const life *l = state;
    uint64_t sum = 0;
    for (int32_t y = 0; y < l->size; y++) {
        for (int32_t x = 0; x < l->size; x++) {
            if (!l->now[(size_t)(y + 1) * l->stride + (size_t)x + 1u]) continue;
            const int32_t item[2] = {x, y};
            sum += versus_item(item, sizeof item);
        }
    }
    return sum;
}

static void destroy(void *state)
{
    life *l = state;
    free(l->now < l->next ? l->now : l->next);
    free(l);
}

const versus_way life_c = {"C", make, tick, digest, destroy};
