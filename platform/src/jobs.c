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

#if defined(__wasm__)

const tide_jobs *tide_platform_jobs(void)
{
    return NULL; // Web builds are single-threaded
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
