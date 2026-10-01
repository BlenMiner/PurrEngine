#include "tide/jobs.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "tide/page.h"

// See tide/jobs.h.

#if defined(__wasm__) && !defined(__wasm_atomics__)
#define TIDE_THREAD_LOCAL // Web builds are single-threaded
#else
#define TIDE_THREAD_LOCAL _Thread_local
#endif

// Below this much work in all (see tide_system_tasks.count), a tick runs on
// one thread: waking the others would cost more than they'd save. It's about a
// tenth of a millisecond's work.
#define PARALLEL_WORK 2000000u

static TIDE_THREAD_LOCAL tide_queue *recording;

tide_queue *tide_recording(tide_queue *world)
{
    return recording ? recording : world;
}

typedef struct system_state {
    atomic_uint next;    // Its next task to start
    atomic_uint done;    // Its tasks finished
    atomic_uint waiting; // Systems it waits for that aren't done
    uint32_t tasks;
    uint32_t first; // Its first task's place among all the tasks
} system_state;

typedef struct tick_run {
    void *world;
    const tide_system_tasks *systems;
    uint32_t count;
    system_state *state;
    uint32_t *dependents;      // The systems that wait for each: system s's from first_dependent[s] to first_dependent[s + 1]
    uint32_t *first_dependent;
    tide_queue *queues;        // Each task's
    void (*prepare)(void *world);
    atomic_uint finished; // Systems done
} tick_run;

// Waiting for another thread to finish what this one needs: a hint to the CPU.
static void relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#endif
}

static void finish_task(tick_run *r, const uint32_t s)
{
    system_state *st = &r->state[s];
    if (atomic_fetch_add(&st->done, 1u) + 1u != st->tasks) return;
    // The system is done: the ones waiting for it are one closer to starting
    for (uint32_t i = r->first_dependent[s]; i < r->first_dependent[s + 1u]; i++) {
        atomic_fetch_sub(&r->state[r->dependents[i]].waiting, 1u);
    }
    atomic_fetch_add(&r->finished, 1u);
}

// Each thread: the first task it can start, earliest system first, until every
// system is done.
static void work(void *context, const uint32_t thread)
{
    (void)thread;
    tick_run *r = context;
    if (r->prepare) r->prepare(r->world);
    while (atomic_load(&r->finished) < r->count) {
        bool ran = false;
        for (uint32_t s = 0; s < r->count && !ran; s++) {
            system_state *st = &r->state[s];
            if (atomic_load(&st->waiting) || atomic_load(&st->next) >= st->tasks) continue;
            const uint32_t task = atomic_fetch_add(&st->next, 1u);
            if (task >= st->tasks) continue;
            recording = &r->queues[st->first + task];
            r->systems[s].run(r->world, task);
            recording = NULL;
            finish_task(r, s);
            ran = true;
        }
        if (!ran) relax();
    }
}

static void *zeroed(const size_t count, const size_t size)
{
    void *p = calloc(count ? count : 1u, size);
    if (!p) tide_out_of_memory();
    return p;
}

void tide_run_systems(void *world, const tide_system_tasks *systems, const uint32_t count, const tide_jobs *jobs,
                      void (*prepare)(void *world), tide_queue *queue, const uint32_t size)
{
    uint64_t work_in_all = 0;
    for (uint32_t s = 0; s < count && jobs && jobs->threads > 1 && work_in_all < PARALLEL_WORK; s++) {
        uint64_t n = 0;
        systems[s].count(world, &n);
        work_in_all += n;
    }
    if (!jobs || jobs->threads < 2 || work_in_all < PARALLEL_WORK) {
        // One thread, in order, recording straight into the world's queue
        if (prepare) prepare(world);
        for (uint32_t s = 0; s < count; s++) {
            uint64_t n;
            const uint32_t tasks = systems[s].count(world, &n);
            for (uint32_t task = 0; task < tasks; task++) systems[s].run(world, task);
        }
        return;
    }

    tick_run r = {world, systems, count, zeroed(count, sizeof(system_state)), NULL, zeroed(count + 1u, sizeof(uint32_t)),
                  NULL, prepare, 0};
    uint32_t tasks = 0;
    uint32_t edges = 0;
    for (uint32_t s = 0; s < count; s++) {
        uint64_t n;
        system_state *st = &r.state[s];
        st->tasks = systems[s].count(world, &n);
        st->first = tasks;
        tasks += st->tasks;
        atomic_init(&st->next, 0u);
        atomic_init(&st->done, 0u);
        atomic_init(&st->waiting, systems[s].wait_count);
        edges += systems[s].wait_count;
        for (uint32_t i = 0; i < systems[s].wait_count; i++) r.first_dependent[systems[s].waits[i] + 1u]++;
    }
    for (uint32_t s = 0; s < count; s++) r.first_dependent[s + 1u] += r.first_dependent[s];
    r.dependents = zeroed(edges, sizeof(uint32_t));
    uint32_t *filled = zeroed(count, sizeof(uint32_t));
    for (uint32_t s = 0; s < count; s++) {
        for (uint32_t i = 0; i < systems[s].wait_count; i++) {
            const uint32_t on = systems[s].waits[i];
            r.dependents[r.first_dependent[on] + filled[on]++] = s;
        }
    }
    free(filled);
    r.queues = zeroed(tasks, sizeof(tide_queue));
    atomic_init(&r.finished, 0u);

    jobs->run(jobs->self, work, &r);

    // What they recorded, in the order one thread would have
    for (uint32_t t = 0; t < tasks; t++) {
        const tide_queue *q = &r.queues[t];
        for (uint32_t i = 0; i < q->count; i++) memcpy(tide_queue_push(queue, size), tide_queue_at(q, i, size), size);
        tide_queue_free(&r.queues[t]);
    }
    free(r.queues);
    free(r.dependents);
    free(r.first_dependent);
    free(r.state);
}
