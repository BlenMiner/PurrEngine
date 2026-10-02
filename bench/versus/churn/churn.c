// Churn in C: an array for each field, as long as the most bullets there can
// be. Each tick moves every bullet and moves the living ones down over the
// dead in the same pass, keeping their order, then puts the new ones at the
// end. On threads, the pieces count their living bullets first, and then each
// moves its own into a second set of arrays, right where they go.

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "tide/math.h" // tide_hash_i2: the random numbers Tide's Math.Hash gives
#include "tide/page.h" // tide_alloc: memory every thread can use on the web
#include "versus.h"

enum { LONGEST = 179 }; // A bullet's longest life, in ticks

typedef struct bullets {
    float *x, *y, *vx, *vy;
    int32_t *life;
} bullets;

typedef struct churn {
    uint32_t rate;
    uint32_t tick;
    uint32_t count;
    float *block;       // Every array, one after the other
    bullets now, other; // The bullets, and arrays for the next tick's on threads
    uint32_t *start;    // On threads: where each piece's living bullets go
} churn;

static void *make(const versus_flag *flags)
{
    churn *c = tide_alloc_zeroed(1, sizeof *c);
    c->rate = flags[0].value;
    // Bullets live LONGEST ticks at most, and the new ones come after the
    // old ones are gone. Apart by a few cache lines more than their size
    // (see particles.c).
    const size_t stride = ((size_t)c->rate * LONGEST + 15u) / 16u * 16u + 64u * 3u;
    c->block = tide_alloc(10u * stride * sizeof(float));
    bullets *sets[2] = {&c->now, &c->other};
    for (uint32_t s = 0; s < 2u; s++) {
        float *at = c->block + 5u * stride * s;
        *sets[s] = (bullets){at, at + stride, at + 2u * stride, at + 3u * stride, (int32_t *)(at + 4u * stride)};
    }
    return c;
}

typedef struct fly_job {
    churn *c;
    uint32_t pieces;
} fly_job;

static uint32_t piece_start(const churn *c, const uint32_t piece, const uint32_t pieces)
{
    return (uint32_t)((uint64_t)c->count * piece / pieces);
}

// Moves the bullets and keeps the living ones, in order, down over the dead.
// How many lived.
static uint32_t fly_in_place(const bullets *b, const uint32_t count)
{
    float *restrict x = b->x, *restrict y = b->y, *restrict vx = b->vx, *restrict vy = b->vy;
    int32_t *restrict life = b->life;
    const float dt = VERSUS_DT;
    uint32_t out = 0;
    for (uint32_t i = 0; i < count; i++) {
        const int32_t left = life[i] - 1;
        if (left <= 0) continue;
        x[out] = x[i] + vx[i] * dt;
        y[out] = y[i] + vy[i] * dt;
        vx[out] = vx[i];
        vy[out] = vy[i];
        life[out] = left;
        out++;
    }
    return out;
}

// The same for a piece, into other arrays from `out` on.
static void fly_into(const bullets *from, const bullets *to, const uint32_t first, const uint32_t end, uint32_t out)
{
    const float *restrict x = from->x, *restrict y = from->y, *restrict vx = from->vx, *restrict vy = from->vy;
    const int32_t *restrict life = from->life;
    float *restrict to_x = to->x, *restrict to_y = to->y, *restrict to_vx = to->vx, *restrict to_vy = to->vy;
    int32_t *restrict to_life = to->life;
    const float dt = VERSUS_DT;
    for (uint32_t i = first; i < end; i++) {
        const int32_t left = life[i] - 1;
        if (left <= 0) continue;
        to_x[out] = x[i] + vx[i] * dt;
        to_y[out] = y[i] + vy[i] * dt;
        to_vx[out] = vx[i];
        to_vy[out] = vy[i];
        to_life[out] = left;
        out++;
    }
}

static void count_living(void *context, const uint32_t piece)
{
    const fly_job *j = context;
    const churn *c = j->c;
    uint32_t living = 0;
    for (uint32_t i = piece_start(c, piece, j->pieces); i < piece_start(c, piece + 1u, j->pieces); i++) {
        living += c->now.life[i] > 1;
    }
    c->start[piece + 1u] = living;
}

static void fly(void *context, const uint32_t piece)
{
    const fly_job *j = context;
    const churn *c = j->c;
    fly_into(&c->now, &c->other, piece_start(c, piece, j->pieces), piece_start(c, piece + 1u, j->pieces), c->start[piece]);
}

static void tick(void *state, const tide_jobs *jobs)
{
    churn *c = state;
    // Threads pay from about a million (on a 12-core Ryzen 9 7900X)
    fly_job j = {c, versus_pieces(jobs, c->count, 1000000u)};
    uint32_t count;
    if (j.pieces == 1u) {
        count = fly_in_place(&c->now, c->count);
    } else {
        c->start = realloc(c->start, (j.pieces + 1u) * sizeof *c->start);
        c->start[0] = 0;
        versus_for(jobs, j.pieces, count_living, &j);
        for (uint32_t p = 1; p <= j.pieces; p++) c->start[p] += c->start[p - 1u];
        versus_for(jobs, j.pieces, fly, &j);
        count = c->start[j.pieces];
        const bullets was = c->now;
        c->now = c->other;
        c->other = was;
    }
    // Fired this tick: they fly from the next one, as Tide's spawns do
    const bullets *b = &c->now;
    for (uint32_t i = 0; i < c->rate; i++) {
        const int32_t h = tide_hash_i2((tide_int2){(int32_t)c->tick, (int32_t)i});
        b->x[count] = 0.0f;
        b->y[count] = 0.0f;
        b->vx[count] = (float)(h % 201 - 100);
        b->vy[count] = (float)(h / 201 % 201 - 100);
        b->life[count] = 20 + h % 160;
        count++;
    }
    c->count = count;
    c->tick++;
}

static uint64_t digest(const void *state)
{
    const churn *c = state;
    const bullets *b = &c->now;
    uint64_t sum = 0;
    for (uint32_t i = 0; i < c->count; i++) {
        uint32_t item[5];
        memcpy(&item[0], &b->x[i], 4u);
        memcpy(&item[1], &b->y[i], 4u);
        memcpy(&item[2], &b->vx[i], 4u);
        memcpy(&item[3], &b->vy[i], 4u);
        memcpy(&item[4], &b->life[i], 4u);
        sum += versus_item(item, sizeof item);
    }
    return sum;
}

static void destroy(void *state)
{
    churn *c = state;
    free(c->block);
    free(c->start);
    free(c);
}

const versus_way churn_c = {"C", make, tick, digest, destroy};
