// A pool of threads for ticks (tide/jobs.h): one for each of the CPU's cores,
// the caller's among them. A file of its own, since it includes the operating
// system's headers.

#if !defined(_WIN32) && !defined(__wasm__)
#define _DEFAULT_SOURCE // sysconf under strict C
#endif

#include "tide/platform.h"

#ifndef MOST_THREADS
#define MOST_THREADS 32u
#endif

#if defined(__wasm__) && !defined(__wasm_atomics__)

const tide_jobs *tide_platform_jobs(void)
{
    return NULL; // Built without threads
}

#elif defined(__wasm__)

// On the web, threads are workers that share the program's memory, which a
// page can only give them when it's cross-origin isolated (platform/web/tide.js
// says how many it has). The page's main thread can never wait, for a lock or
// a signal, only spin: so workers wait for a round on its count, and the
// caller, which only spins, opens and closes rounds with atomics alone. As on
// desktop, the caller never waits for a worker to wake (a worker only starts
// once the page's main thread is back in the browser's hands), only for the
// ones that joined.

#include <pthread.h>

#include "tide_web.h"

#define OPEN 0x80000000u // In `gate`: the round takes workers. Below it: the workers in it

typedef struct pool {
    tide_jobs jobs;
    uint32_t round; // Rounds started, which workers wait on
    uint32_t gate;
    void (*work)(void *context, uint32_t thread);
    void *context;
    bool started; // Its workers are made
} pool;

static pool the_pool;
static bool made;

static void *worker(void *thread);

// The workers, thread 1 and up, with the stack the program's own thread has:
// the caller is thread 0. Made the first time there's work for them, since a
// worker costs a page far more than a thread costs a program. Fewer, if the
// page won't make them all: rounds only ever take the workers that join.
static void start_workers(pool *p)
{
    p->started = true;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1u << 20);
    for (uint32_t thread = 1; thread < p->jobs.threads; thread++) {
        pthread_t t;
        if (pthread_create(&t, &attr, worker, (void *)(uintptr_t)thread) != 0) break;
        pthread_detach(t);
    }
    pthread_attr_destroy(&attr);
}

static void *worker(void *thread)
{
    pool *p = &the_pool;
    uint32_t seen = 0;
    for (;;) {
        uint32_t round;
        while ((round = __atomic_load_n(&p->round, __ATOMIC_ACQUIRE)) == seen) {
            __builtin_wasm_memory_atomic_wait32((int32_t *)&p->round, (int32_t)seen, -1);
        }
        seen = round;
        uint32_t gate = __atomic_load_n(&p->gate, __ATOMIC_ACQUIRE);
        bool joined = false;
        while ((gate & OPEN) && !joined) {
            joined = __atomic_compare_exchange_n(&p->gate, &gate, gate + 1u, true, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        }
        if (!joined) continue; // Over before it woke
        p->work(p->context, (uint32_t)(uintptr_t)thread);
        __atomic_fetch_sub(&p->gate, 1u, __ATOMIC_RELEASE);
    }
    return NULL;
}

static void run(void *self, void (*work)(void *context, uint32_t thread), void *context)
{
    pool *p = self;
    if (!p->started) start_workers(p);
    p->work = work;
    p->context = context;
    __atomic_store_n(&p->gate, OPEN, __ATOMIC_RELEASE);
    __atomic_fetch_add(&p->round, 1u, __ATOMIC_RELEASE);
    __builtin_wasm_memory_atomic_notify((int32_t *)&p->round, UINT32_MAX);
    work(context, 0);
    __atomic_fetch_and(&p->gate, ~OPEN, __ATOMIC_ACQ_REL);
    while (__atomic_load_n(&p->gate, __ATOMIC_ACQUIRE) != 0) {
    }
}

const tide_jobs *tide_platform_jobs(void)
{
    if (made) return the_pool.jobs.threads > 1 ? &the_pool.jobs : NULL;
    made = true;
    pool *p = &the_pool;
    const uint32_t n = tide_web_threads();
    p->jobs = (tide_jobs){p, n < MOST_THREADS ? n : MOST_THREADS, run};
    return p->jobs.threads > 1 ? &p->jobs : NULL;
}

#else

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef SRWLOCK pool_lock;
typedef CONDITION_VARIABLE pool_signal;
#define LOCK(l) AcquireSRWLockExclusive(l)
#define UNLOCK(l) ReleaseSRWLockExclusive(l)
#define WAIT(s, l) SleepConditionVariableSRW(s, l, INFINITE, 0)
#define WAKE_ONE(s) WakeConditionVariable(s)
#define WAKE_ALL(s) WakeAllConditionVariable(s)
#else
#include <pthread.h>
#include <unistd.h>
typedef pthread_mutex_t pool_lock;
typedef pthread_cond_t pool_signal;
#define LOCK(l) pthread_mutex_lock(l)
#define UNLOCK(l) pthread_mutex_unlock(l)
#define WAIT(s, l) pthread_cond_wait(s, l)
#define WAKE_ONE(s) pthread_cond_signal(s)
#define WAKE_ALL(s) pthread_cond_broadcast(s)
#endif

// Each round of work, the workers that wake in time join the caller. A round
// is open until the caller's own run of the work returns, which is once all
// of it is done: a worker that wakes later has nothing left to do, so the
// caller never waits for a worker to wake, only for the ones that joined.
typedef struct pool {
    tide_jobs jobs;
    pool_lock lock;
    pool_signal start; // A round has started
    pool_signal done;  // The last worker that joined it is done
    uint64_t round;    // Rounds started
    bool open;         // The round takes workers
    uint32_t inside;   // Workers running the round's work
    void (*work)(void *context, uint32_t thread);
    void *context;
} pool;

static pool the_pool;
static bool made;

static void worker_loop(const uint32_t thread)
{
    pool *p = &the_pool;
    uint64_t seen = 0;
    for (;;) {
        LOCK(&p->lock);
        while (p->round == seen) WAIT(&p->start, &p->lock);
        seen = p->round;
        if (!p->open) { // Over before it woke
            UNLOCK(&p->lock);
            continue;
        }
        p->inside++;
        void (*const work)(void *, uint32_t) = p->work;
        void *const context = p->context;
        UNLOCK(&p->lock);
        work(context, thread);
        LOCK(&p->lock);
        if (--p->inside == 0 && !p->open) WAKE_ONE(&p->done);
        UNLOCK(&p->lock);
    }
}

#ifdef _WIN32
static DWORD WINAPI worker(LPVOID thread)
{
    worker_loop((uint32_t)(uintptr_t)thread);
    return 0;
}
#else
static void *worker(void *thread)
{
    worker_loop((uint32_t)(uintptr_t)thread);
    return NULL;
}
#endif

static void run(void *self, void (*work)(void *context, uint32_t thread), void *context)
{
    pool *p = self;
    LOCK(&p->lock);
    p->work = work;
    p->context = context;
    p->open = true;
    p->inside = 0;
    p->round++;
    WAKE_ALL(&p->start);
    UNLOCK(&p->lock);
    work(context, 0);
    LOCK(&p->lock);
    p->open = false;
    while (p->inside) WAIT(&p->done, &p->lock);
    UNLOCK(&p->lock);
}

static uint32_t cores(void)
{
#ifdef _WIN32
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return (uint32_t)info.dwNumberOfProcessors;
#else
    const long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (uint32_t)n : 1u;
#endif
}

const tide_jobs *tide_platform_jobs(void)
{
    if (made) return the_pool.jobs.threads > 1 ? &the_pool.jobs : NULL;
    made = true;
    pool *p = &the_pool;
    const uint32_t n = cores();
    p->jobs = (tide_jobs){p, n < MOST_THREADS ? n : MOST_THREADS, run};
#ifdef _WIN32
    InitializeSRWLock(&p->lock);
    InitializeConditionVariable(&p->start);
    InitializeConditionVariable(&p->done);
#else
    pthread_mutex_init(&p->lock, NULL);
    pthread_cond_init(&p->start, NULL);
    pthread_cond_init(&p->done, NULL);
#endif
    // The workers, thread 1 and up: the caller is thread 0. Fewer, if the
    // system won't make them all.
    uint32_t threads = 1;
    for (; threads < p->jobs.threads; threads++) {
#ifdef _WIN32
        HANDLE h = CreateThread(NULL, 0, worker, (LPVOID)(uintptr_t)threads, 0, NULL);
        if (!h) break;
        CloseHandle(h);
#else
        pthread_t t;
        if (pthread_create(&t, NULL, worker, (void *)(uintptr_t)threads) != 0) break;
        pthread_detach(t);
#endif
    }
    p->jobs.threads = threads;
    return threads > 1 ? &p->jobs : NULL;
}

#endif
