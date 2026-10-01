#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "tide/entity.h"
#include "tide/table.h"

// Running a tick's systems on several threads, with the same results as on
// one.
//
// Temporary implementation written by Claude; the project owner takes it over
// later.
//
// The threads are the platform layer's (tide_platform_jobs): all this needs
// is to run a function on each of them. A tick's systems become tasks: a
// system that splits its entities across threads has a task per chunk of
// them, any other one task. A system's tasks start once the systems it waits
// for are done (the schedule: see analyze_parallelism in tidec), with no
// barriers between stages. Each task records its structural changes and
// events in a queue of its own, and those go into the world's queue in the
// order one thread would have recorded them: system by system, entity by
// entity.
//
// Spawns hand out entity IDs, in order. A system that runs on one thread
// hands them out as it spawns, so systems that spawn that way wait for each
// other. One that splits its entities gets temporary handles instead
// (tide_entity_is_temporary), made from its task, never its thread, and once
// all its tasks are done it settles them: its entities get their IDs, in the
// order one thread would have given them, and its own data and what it
// recorded get the real handles. That step alone keeps the tick's order among
// systems that spawn, and systems waiting for it start after it, so they
// never see a temporary handle.

typedef struct tide_jobs {
    void *self;
    uint32_t threads; // That can run work: the pool's, and the caller's
    // Runs work(context, thread) on the caller's thread, as thread 0, and on
    // each other thread (1 and up) that's free to join while it runs, and
    // returns once it has returned on every one. The work has to get done on
    // the caller's thread alone if no other joins.
    void (*run)(void *self, void (*work)(void *context, uint32_t thread), void *context);
} tide_jobs;

// The entities a system's spawns got as it settled them: what a temporary
// handle of its tasks is now.
typedef struct tide_new_entities {
    uint32_t first_task;   // Its tasks, among the tick's
    uint32_t task_count;
    const uint32_t *start; // Each task's first entity in `entities`
    const tide_entity *entities;
} tide_new_entities;

// The real handle for `e` if it's one of these temporary ones, or else `e`.
tide_entity tide_settled(const tide_new_entities *settled, tide_entity e);

// A system, as the tick runs it: generated code has a table of them, in the
// tick's order.
typedef struct tide_system_tasks {
    // Its tasks this tick (at least 1), and roughly how much work they are:
    // the rows they go through, times what tidec reckons one costs. That
    // tells whether there's enough to do for threads to be worth it.
    uint32_t (*count)(const void *world, uint64_t *work);
    void (*run)(void *world, uint32_t task);
    uint32_t wait_count;
    const uint32_t *waits; // Systems before it in the tick that it waits for
    bool spawns;           // It hands out entity IDs...
    bool settles;          // ...through temporary handles, settled once its tasks are done
    // A task's own data, with the real handles for the temporary ones it
    // stored: its mut components' entity fields. NULL when it has none.
    void (*settle)(void *world, uint32_t task, const tide_new_entities *settled);
} tide_system_tasks;

// What the tick runs its systems with.
typedef struct tide_tick_systems {
    const tide_system_tasks *systems;
    uint32_t count;
    void (*prepare)(void *world); // On each thread, before its first task
    tide_queue *queue;            // The world's
    uint32_t item_size;           // Its items'
    tide_entities *entities;      // The world's
    // A recorded change or event, with the real handles for temporary ones
    void (*settle_item)(void *item, const tide_new_entities *settled);
} tide_tick_systems;

// Runs the tick's systems on `world`: on `jobs`' threads when there's enough
// to do, or else (and with no jobs) on this thread, in order. Either way, with
// the same results.
void tide_run_systems(void *world, const tide_tick_systems *tick, const tide_jobs *jobs);

// The queue code records into: the running task's own, on threads, or else
// `world`, the world's.
tide_queue *tide_recording(tide_queue *world);

// The entity a Spawn makes: a temporary handle in a task of a system that
// settles its spawns, or else a new entity in `t`.
tide_entity tide_new_entity(tide_entities *t);
