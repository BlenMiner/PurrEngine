// Web only: ticks on threads in a browser. web_threads.mjs serves this page
// cross-origin isolated, so the program's memory is shared and its threads are
// workers (platform/web/tide.js). Frame by frame, as a game runs (workers only
// start once the page's thread is back in the browser's hands), it ticks a busy
// match (jobs.tide) on threads and another on one thread: they have to come out
// the same, with the pool's workers taking part. Prints "ok", or "FAIL".

#define _POSIX_C_SOURCE 199309L // clock_gettime under strict C

#include <stdio.h>
#include <time.h>

#include "game.h"
#include "tide/platform.h"

#define TICKS 240

static tide_world one;  // Ticked on the page's thread
static tide_world many; // On threads
static const tide_jobs *jobs;
static int ticks;
static double now;

// The threads that took part in a round of work, a bit each.
static uint32_t threads_seen;

// Time the ticks took once the workers had joined: on one thread, and on all.
static double one_seconds, many_seconds;
static int timed;

static double seconds_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static void note_thread(void *context, const uint32_t thread)
{
    (void)context;
    __atomic_fetch_or(&threads_seen, 1u << (thread & 31u), __ATOMIC_RELAXED);
    for (volatile uint32_t i = 0; i < 100000u; i++) {
    } // Long enough for a waiting worker to wake and join
}

static int fail(const char *why)
{
    printf("FAIL: %s\n", why);
    return 1;
}

static int frame(void *user, const float seconds)
{
    (void)user;
    now += seconds;
    if (now > 45.0) return fail(threads_seen & ~1u ? "the ticks took over 45 seconds" : "no worker joined in 45 seconds");
    for (int i = 0; i < 4 && ticks < TICKS; i++, ticks++) {
        const double start = seconds_now();
        tide_world_tick(&one);
        const double middle = seconds_now();
        tide_world_tick_on(&many, jobs);
        if (threads_seen & ~1u) {
            one_seconds += middle - start;
            many_seconds += seconds_now() - middle;
            timed++;
        }
        if (tide_world_hash(&one) != tide_world_hash(&many)) {
            printf("tick %d came out different\n", ticks);
            return fail("threads changed the match");
        }
    }
    if (!(threads_seen & ~1u)) jobs->run(jobs->self, note_thread, NULL);
    if (ticks < TICKS || !(threads_seen & ~1u)) return TIDE_KEEP_RUNNING;
    printf("ok: %d ticks the same on %u threads, %d of which took part in one round\n", ticks, (unsigned)jobs->threads,
           __builtin_popcount(threads_seen));
    if (timed) {
        printf("a tick took %.2f ms on one thread, %.2f ms on threads\n", one_seconds * 1000.0 / timed,
               many_seconds * 1000.0 / timed);
    }
    return 0;
}

int main(void)
{
    tide_platform_open(&(tide_window_desc){.title = "web threads", .width = 64, .height = 64, .hidden = true});
    jobs = tide_platform_jobs();
    if (!jobs) return fail("no threads: the page isn't cross-origin isolated, or can't make workers");
    tide_world_init(&one, 1.0f / 60.0f);
    tide_world_init(&many, 1.0f / 60.0f);
    tide_platform_run(frame, NULL);
}
