#include "tide/jobs.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "tide/page.h"
#include "tide/time.h"

// See tide/jobs.h.

// Below this much work in all (see tide_system_tasks.count), a tick runs on
// one thread: waking the others, and then reading on this one what they
// changed from their caches, would cost more than they'd save. It's about half
// a millisecond's work, or on the web, where waking a worker takes longer,
// one and a half.
#ifdef __wasm__
#define PARALLEL_WORK 60000000u
#else
#define PARALLEL_WORK 20000000u
#endif

// A parallel loop's steps go to the other threads past this much work: they're
// awake already, running the tick's tasks, so only a loop with next to nothing
// to do stays on its thread. tidec can't tell how long the loops inside a
// step go on, so a step can be far more work than it reckons: low, so a loop
// that's more work than it looks is shared too.
#ifdef __wasm__
#define LOOP_WORK 300000u
#else
#define LOOP_WORK 100000u
#endif

#define NONE UINT32_MAX

// The task running on this thread, while systems run.
typedef struct task_context {
    tide_queue *queue; // Its own, on threads; NULL: the world's
    uint32_t task;     // Among the tick's
    uint32_t spawned;  // Temporary handles it gave
    bool settles;      // Its system's spawns give temporary handles
} task_context;

static TIDE_THREAD_LOCAL task_context *running;
TIDE_THREAD_LOCAL tide_queue *tide_task_queue;

tide_entity tide_new_entity(tide_entities *t)
{
    if (!running || !running->settles) return tide_entity_create(t);
    return (tide_entity){running->spawned++, TIDE_ENTITY_TEMPORARY | running->task};
}

tide_entity tide_settled(const tide_new_entities *settled, const tide_entity e)
{
    if (!tide_entity_is_temporary(e)) return e;
    const uint32_t task = (e.generation & ~TIDE_ENTITY_TEMPORARY) - settled->first_task;
    if (task >= settled->task_count || e.index >= settled->start[task + 1u] - settled->start[task]) return e;
    return settled->entities[settled->start[task] + e.index];
}

static void *zeroed(const size_t count, const size_t size)
{
    return tide_alloc_zeroed(count ? count : 1u, size);
}

static void run_task(void *world, const tide_system_tasks *system, task_context *context, const uint32_t task)
{
    tide_memory_sync(); // Its queue's pages, which another thread may have made in an earlier tick
    running = context;
    tide_task_queue = context->queue;
    system->run(world, task);
    running = NULL;
    tide_task_queue = NULL;
}

// A system's spawns get their IDs, in the order one thread would have given
// them (task by task, spawn by spawn), and its tasks' data and what they
// recorded get the real handles: `queues` holds each task's, or with NULL,
// they're in the world's from item `from` on.
static void settle_system(void *world, const tide_tick_systems *tick, const tide_system_tasks *system,
                          const uint32_t first_task, const uint32_t tasks, const uint32_t *spawned, tide_queue *queues,
                          const uint32_t from)
{
    uint32_t *start = zeroed(tasks + 1u, sizeof *start);
    for (uint32_t k = 0; k < tasks; k++) start[k + 1u] = start[k] + spawned[k];
    if (start[tasks] == 0) {
        free(start);
        return;
    }
    tide_entity *entities = zeroed(start[tasks], sizeof *entities);
    for (uint32_t i = 0; i < start[tasks]; i++) entities[i] = tide_entity_create(tick->entities);
    const tide_new_entities settled = {first_task, tasks, start, entities};
    for (uint32_t k = 0; k < tasks; k++) {
        if (!spawned[k]) continue;
        if (system->settle) system->settle(world, k, &settled);
        if (!queues) continue;
        for (uint32_t i = 0; i < queues[k].count; i++) tick->settle_item(tide_queue_at(&queues[k], i, tick->item_size), &settled);
    }
    for (uint32_t i = from; !queues && i < tick->queue->count; i++) {
        tick->settle_item(tide_queue_at(tick->queue, i, tick->item_size), &settled);
    }
    free(entities);
    free(start);
}

// ---------------------------------------------------------------------------
// On threads

typedef struct system_state {
    atomic_uint next;            // Its next task to start
    atomic_uint done;            // Its tasks finished
    atomic_uint waiting;         // Systems it waits for that aren't done
    atomic_uint settle_waiting;  // Before it settles: its tasks, and the system that spawns before it
    atomic_uint settle_claimed;
    uint32_t tasks;
    uint32_t first;       // Its first task among all the tick's
    uint32_t next_settle; // The system that spawns after it, if it settles its spawns
} system_state;

// A loop a running system shares out (tide_parallel_for), on its stack.
typedef struct loop_job {
    void (*work)(void *context, uint32_t task);
    void *context;
    uint32_t count;
    atomic_uint next; // Its next task to start
    atomic_uint done;
} loop_job;

// Where threads find the loops running systems share out. The job lives on its
// system's thread's stack, so that thread waits for every thread looking at
// it (`users`, counted before they look) to be done before it goes.
#define LOOP_SLOTS 8u
typedef struct loop_slot {
    _Atomic(loop_job *) job;
    atomic_uint users;
} loop_slot;

typedef struct tick_run {
    loop_slot loops[LOOP_SLOTS];
    void *world;
    const tide_tick_systems *tick;
    system_state *state;
    uint32_t *dependents;      // The systems that wait for each: system s's from first_dependent[s] to first_dependent[s + 1]
    uint32_t *first_dependent;
    tide_queue *queues;        // Each task's
    uint32_t *spawned;         // Each task's temporary handles
    atomic_uint finished;      // Systems done
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

// The system is done: those waiting for it are one closer to starting, and
// the next to settle its spawns one closer to it.
static void finish_system(tick_run *r, const uint32_t s)
{
    tide_memory_sync(); // What its tasks made on other threads (see tide/page.h)
    for (uint32_t i = r->first_dependent[s]; i < r->first_dependent[s + 1u]; i++) {
        atomic_fetch_sub(&r->state[r->dependents[i]].waiting, 1u);
    }
    if (r->state[s].next_settle != NONE) atomic_fetch_sub(&r->state[r->state[s].next_settle].settle_waiting, 1u);
    atomic_fetch_add(&r->finished, 1u);
}

static void finish_task(tick_run *r, const uint32_t s)
{
    system_state *st = &r->state[s];
    if (atomic_fetch_add(&st->done, 1u) + 1u != st->tasks) return;
    if (r->tick->systems[s].settles) atomic_fetch_sub(&st->settle_waiting, 1u);
    else finish_system(r, s);
}

// Something this thread can do now: a task of the earliest system that has
// one ready, or a system's settling. False if there's nothing.
static bool work_once(tick_run *r)
{
    const tide_tick_systems *tick = r->tick;
    for (uint32_t s = 0; s < tick->count; s++) {
        system_state *st = &r->state[s];
        const tide_system_tasks *system = &tick->systems[s];
        if (system->settles && atomic_load(&st->settle_waiting) == 0 && !atomic_exchange(&st->settle_claimed, 1u)) {
            tide_memory_sync(); // What its tasks made on other threads (see tide/page.h)
            settle_system(r->world, tick, system, st->first, st->tasks, &r->spawned[st->first], &r->queues[st->first], 0);
            finish_system(r, s);
            return true;
        }
        if (atomic_load(&st->waiting) || atomic_load(&st->next) >= st->tasks) continue;
        const uint32_t task = atomic_fetch_add(&st->next, 1u);
        if (task >= st->tasks) continue;
        tide_memory_sync(); // What the systems before it made on other threads
        task_context context = {&r->queues[st->first + task], st->first + task, 0, system->settles};
        run_task(r->world, system, &context, task);
        r->spawned[st->first + task] = context.spawned;
        finish_task(r, s);
        return true;
    }
    return false;
}

// The tick this thread works on, while systems run on threads: where its
// systems share their loops out.
static TIDE_THREAD_LOCAL tick_run *current;

// A task of a loop a running system shares out, if there's one to do.
static bool help_loop(tick_run *r)
{
    for (uint32_t i = 0; i < LOOP_SLOTS; i++) {
        loop_slot *slot = &r->loops[i];
        if (!atomic_load_explicit(&slot->job, memory_order_relaxed)) continue;
        atomic_fetch_add(&slot->users, 1u);
        loop_job *job = atomic_load(&slot->job);
        bool did = false;
        if (job) {
            const uint32_t task = atomic_fetch_add(&job->next, 1u);
            if (task < job->count) {
                tide_memory_sync(); // What its system made on its own thread
                job->work(job->context, task);
                atomic_fetch_add(&job->done, 1u);
                did = true;
            }
        }
        atomic_fetch_sub(&slot->users, 1u);
        if (did) return true;
    }
    return false;
}

static void work(void *context, const uint32_t thread)
{
    (void)thread;
    tick_run *r = context;
    current = r;
    if (r->tick->prepare) r->tick->prepare(r->world);
    // A thread with nothing to do spins a little, then lets the system run
    // something else on its core: with a thread on every core, one the
    // system pushes aside for a moment would otherwise hold the tick up for
    // a whole time slice, while the rest spin. A loop a running system shares
    // out comes first: what waits for that system waits for it.
    uint32_t idle = 0;
    while (atomic_load(&r->finished) < r->tick->count) {
        if (help_loop(r) || work_once(r)) {
            idle = 0;
        } else if (++idle < 256u) {
            relax();
        } else {
            tide_yield();
            idle = 0;
        }
    }
    current = NULL;
}

void tide_parallel_for(const uint32_t count, void (*work_fn)(void *context, uint32_t task), void *context,
                       const uint64_t cost)
{
    tick_run *r = current;
    loop_job job = {work_fn, context, count, 0u, 0u};
    atomic_init(&job.next, 0u);
    atomic_init(&job.done, 0u);
    loop_slot *slot = NULL;
    for (uint32_t i = 0; r && count > 1u && cost >= LOOP_WORK && i < LOOP_SLOTS && !slot; i++) {
        loop_job *none = NULL;
        if (atomic_compare_exchange_strong(&r->loops[i].job, &none, &job)) slot = &r->loops[i];
    }
    if (!slot) { // On this thread, in order
        for (uint32_t task = 0; task < count; task++) work_fn(context, task);
        return;
    }
    for (;;) {
        const uint32_t task = atomic_fetch_add(&job.next, 1u);
        if (task >= count) break;
        work_fn(context, task);
        atomic_fetch_add(&job.done, 1u);
    }
    while (atomic_load(&job.done) < count) relax();
    atomic_store(&slot->job, NULL);
    while (atomic_load(&slot->users)) relax();
    tide_memory_sync(); // What the other threads made (see tide/page.h)
}

// Each task's queue, kept from tick to tick by the thread that runs ticks, so
// their pages are made once rather than every tick: queues for items of one
// size (a game's changes and events).
static TIDE_THREAD_LOCAL tide_queue *kept_queues;
static TIDE_THREAD_LOCAL uint32_t kept_count;
static TIDE_THREAD_LOCAL uint32_t kept_item_size;

static tide_queue *task_queues(const uint32_t tasks, const uint32_t item_size)
{
    if (item_size != kept_item_size) { // Another game's, as a reloaded build can have
        for (uint32_t t = 0; t < kept_count; t++) tide_queue_free(&kept_queues[t]);
        free(kept_queues);
        kept_queues = NULL;
        kept_count = 0;
        kept_item_size = item_size;
    }
    if (tasks > kept_count) {
        kept_queues = tide_realloc(kept_queues, kept_count * sizeof *kept_queues, tasks * sizeof *kept_queues);
        memset(&kept_queues[kept_count], 0, (tasks - kept_count) * sizeof *kept_queues);
        kept_count = tasks;
    }
    return kept_queues;
}

static void run_on_threads(void *world, const tide_tick_systems *tick, const tide_jobs *jobs)
{
    const uint32_t count = tick->count;
    tick_run r = {.world = world, .tick = tick, .state = zeroed(count, sizeof(system_state)),
                  .first_dependent = zeroed(count + 1u, sizeof(uint32_t))};
    for (uint32_t i = 0; i < LOOP_SLOTS; i++) {
        atomic_init(&r.loops[i].job, NULL);
        atomic_init(&r.loops[i].users, 0u);
    }
    uint32_t tasks = 0;
    uint32_t edges = 0;
    uint32_t last_spawner = NONE;
    for (uint32_t s = 0; s < count; s++) {
        const tide_system_tasks *system = &tick->systems[s];
        system_state *st = &r.state[s];
        uint64_t work;
        st->tasks = system->count(world, &work);
        st->first = tasks;
        st->next_settle = NONE;
        tasks += st->tasks;
        atomic_init(&st->next, 0u);
        atomic_init(&st->done, 0u);
        atomic_init(&st->waiting, system->wait_count);
        atomic_init(&st->settle_claimed, 0u);
        // Spawns hand out IDs in the tick's order: a system that settles its
        // spawns does so once its tasks are done and the one before it is
        // (one that hands them out as it runs waits for it in the schedule)
        atomic_init(&st->settle_waiting, 1u + (last_spawner != NONE ? 1u : 0u));
        if (system->spawns) {
            if (last_spawner != NONE && system->settles) r.state[last_spawner].next_settle = s;
            last_spawner = s;
        }
        edges += system->wait_count;
        for (uint32_t i = 0; i < system->wait_count; i++) r.first_dependent[system->waits[i] + 1u]++;
    }
    for (uint32_t s = 0; s < count; s++) r.first_dependent[s + 1u] += r.first_dependent[s];
    r.dependents = zeroed(edges, sizeof(uint32_t));
    uint32_t *filled = zeroed(count, sizeof(uint32_t));
    for (uint32_t s = 0; s < count; s++) {
        for (uint32_t i = 0; i < tick->systems[s].wait_count; i++) {
            const uint32_t on = tick->systems[s].waits[i];
            r.dependents[r.first_dependent[on] + filled[on]++] = s;
        }
    }
    free(filled);
    r.queues = task_queues(tasks, tick->item_size);
    r.spawned = zeroed(tasks, sizeof(uint32_t));
    atomic_init(&r.finished, 0u);

    jobs->run(jobs->self, work, &r);
    tide_memory_sync(); // What the other threads made (see tide/page.h)

    // What they recorded, in the order one thread would have
    for (uint32_t t = 0; t < tasks; t++) {
        tide_queue_append(tick->queue, &r.queues[t], tick->item_size);
        tide_queue_clear(&r.queues[t], tick->item_size);
    }
    free(r.spawned);
    free(r.dependents);
    free(r.first_dependent);
    free(r.state);
}

// ---------------------------------------------------------------------------

void tide_run_systems(void *world, const tide_tick_systems *tick, const tide_jobs *jobs)
{
    uint64_t work_in_all = 0;
    for (uint32_t s = 0; s < tick->count && jobs && jobs->threads > 1 && work_in_all < PARALLEL_WORK; s++) {
        uint64_t work = 0;
        tick->systems[s].count(world, &work);
        work_in_all += work;
    }
    if (jobs && jobs->threads > 1 && work_in_all >= PARALLEL_WORK) {
        run_on_threads(world, tick, jobs);
        tide_entities_settle(tick->entities);
        return;
    }

    // One thread, in order, recording straight into the world's queue, with
    // the same tasks, temporary handles and settling as on threads
    if (tick->prepare) tick->prepare(world);
    uint32_t first = 0;
    for (uint32_t s = 0; s < tick->count; s++) {
        const tide_system_tasks *system = &tick->systems[s];
        uint64_t work;
        const uint32_t tasks = system->count(world, &work);
        uint32_t *spawned = system->settles ? zeroed(tasks, sizeof(uint32_t)) : NULL;
        const uint32_t from = tick->queue->count;
        for (uint32_t task = 0; task < tasks; task++) {
            task_context context = {NULL, first + task, 0, system->settles};
            run_task(world, system, &context, task);
            if (spawned) spawned[task] = context.spawned;
        }
        if (spawned) settle_system(world, tick, system, first, tasks, spawned, NULL, from);
        free(spawned);
        first += tasks;
    }
    tide_entities_settle(tick->entities);
}
