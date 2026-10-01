#pragma once

#include <stdbool.h>
#include <stdint.h>

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

typedef struct tide_jobs {
    void *self;
    uint32_t threads; // That can run work: the pool's, and the caller's
    // Runs work(context, thread) on the caller's thread, as thread 0, and on
    // each other thread (1 and up) that's free to join while it runs, and
    // returns once it has returned on every one. The work has to get done on
    // the caller's thread alone if no other joins.
    void (*run)(void *self, void (*work)(void *context, uint32_t thread), void *context);
} tide_jobs;

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
} tide_system_tasks;

// Runs the tick's systems on `world`: on `jobs`' threads when there's enough
// to do, or else (and with no jobs) on this thread, in order. `prepare` runs on
// each thread before its first task. What the tasks record goes into `queue`,
// the world's, of items `size` bytes long.
void tide_run_systems(void *world, const tide_system_tasks *systems, uint32_t count, const tide_jobs *jobs,
                      void (*prepare)(void *world), tide_queue *queue, uint32_t size);

// The queue code records into: the running task's own, on threads, or else
// `world`, the world's.
tide_queue *tide_recording(tide_queue *world);
