// Events in C: a turret's hit is a write straight into its target's health,
// in an array by target.

#include <stdbool.h>
#include <stdlib.h>

#include "tide/math.h" // tide_hash_i2: the random numbers Tide's Math.Hash gives
#include "tide/page.h" // tide_alloc: memory every thread can use on the web
#include "versus.h"

typedef struct events {
    uint32_t targets, turrets;
    bool ready;
    int32_t *health; // By target
    uint32_t *aim;   // By turret: its target
    int32_t *damage;
} events;

static void *make(const versus_flag *flags)
{
    events *e = tide_alloc_zeroed(1, sizeof *e);
    e->targets = flags[0].value;
    e->turrets = flags[1].value;
    return e;
}

static void setup(events *e)
{
    e->health = tide_alloc(e->targets * sizeof *e->health);
    e->aim = tide_alloc(e->turrets * sizeof *e->aim);
    e->damage = tide_alloc(e->turrets * sizeof *e->damage);
    for (uint32_t i = 0; i < e->targets; i++) e->health[i] = 1000;
    for (uint32_t i = 0; i < e->turrets; i++) {
        const int32_t h = tide_hash_i2((tide_int2){(int32_t)i, 7});
        e->aim[i] = (uint32_t)(h % (int32_t)e->targets);
        e->damage[i] = 1 + h / (int32_t)e->targets % 50;
    }
}

static void tick(void *state, const tide_jobs *jobs)
{
    (void)jobs; // Turrets share targets, and one thread goes faster than any way of sharing them
    events *e = state;
    if (!e->ready) { // Tide's first tick spawns them, and they shoot from the next
        setup(e);
        e->ready = true;
        return;
    }
    int32_t *restrict health = e->health;
    const uint32_t *restrict aim = e->aim;
    const int32_t *restrict damage = e->damage;
    for (uint32_t i = 0; i < e->turrets; i++) {
        int32_t *const h = &health[aim[i]];
        *h -= damage[i];
        if (*h <= 0) *h += 1000;
    }
}

static uint64_t digest(const void *state)
{
    const events *e = state;
    uint64_t sum = 0;
    for (uint32_t i = 0; i < e->targets; i++) {
        const int32_t item[2] = {(int32_t)i, e->health[i]};
        sum += versus_item(item, sizeof item);
    }
    return sum;
}

static void destroy(void *state)
{
    events *e = state;
    free(e->health);
    free(e->aim);
    free(e->damage);
    free(e);
}

const versus_way events_c = {"C", make, tick, digest, destroy};
