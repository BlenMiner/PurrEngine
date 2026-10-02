// C against Tide's harness (see versus.h).

#include "versus.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tide/page.h"
#include "tide/platform.h"
#include "tide/time.h"

// Saved results go here, every concept's in one file.
#ifndef TIDE_BENCH_RESULTS
#define TIDE_BENCH_RESULTS "results"
#endif

enum { MAX_WAYS = 8, MAX_RUNS = 2 };

// The platform's pool, with only its first `threads` threads taking part.
typedef struct limited {
    tide_jobs jobs;
    const tide_jobs *pool;
} limited;

typedef struct limited_round {
    void (*work)(void *context, uint32_t thread);
    void *context;
    uint32_t threads;
} limited_round;

static void limited_work(void *context, const uint32_t thread)
{
    const limited_round *r = context;
    if (thread < r->threads) r->work(r->context, thread);
}

static void limited_run(void *self, void (*work)(void *context, uint32_t thread), void *context)
{
    const limited *l = self;
    limited_round r = {work, context, l->jobs.threads};
    l->pool->run(l->pool->self, limited_work, &r);
}

static limited the_limited;

// Threads to tick on: NULL for this one alone, 0 for every core.
static const tide_jobs *jobs_for(const uint32_t threads)
{
    const tide_jobs *pool = tide_platform_jobs();
    if (!pool || threads == 1) return NULL;
    if (threads == 0 || threads >= pool->threads) return pool;
    the_limited = (limited){{&the_limited, threads, limited_run}, pool};
    return &the_limited.jobs;
}

static uint32_t thread_count(const tide_jobs *jobs)
{
    return jobs ? jobs->threads : 1u;
}

typedef struct pieces_round {
    void (*work)(void *context, uint32_t piece);
    void *context;
    uint32_t count;
    atomic_uint next;
} pieces_round;

// On each thread that joins: pieces until there are none left. Threads take
// them as they come, as a pool can start with fewer threads than it has.
static void pieces_work(void *context, const uint32_t thread)
{
    (void)thread;
    pieces_round *r = context;
    tide_memory_sync(); // What the thread that started it made (see tide/page.h)
    for (;;) {
        const uint32_t piece = atomic_fetch_add(&r->next, 1u);
        if (piece >= r->count) return;
        r->work(r->context, piece);
    }
}

void versus_for(const tide_jobs *jobs, const uint32_t count, void (*work)(void *context, uint32_t piece), void *context)
{
    if (!jobs || count <= 1u) {
        for (uint32_t piece = 0; piece < count; piece++) work(context, piece);
        return;
    }
    pieces_round r = {work, context, count, 0u};
    jobs->run(jobs->self, pieces_work, &r);
    tide_memory_sync(); // What the other threads made
}

// Split work however little there is (--split, --quick).
static bool always_split;

uint32_t versus_pieces(const tide_jobs *jobs, const uint32_t count, const uint32_t enough)
{
    if (!jobs || (count < enough && !always_split)) return 1u;
    const uint32_t pieces = jobs->threads * 8u;
    return count < pieces ? (count ? count : 1u) : pieces;
}

uint64_t versus_item(const void *data, const size_t size)
{
    // FNV-1a over the bytes, then SplitMix64's finish, so that sums of items
    // that differ a little differ a lot
    uint64_t h = 0xcbf29ce484222325ull;
    const unsigned char *bytes = data;
    for (size_t i = 0; i < size; i++) h = (h ^ bytes[i]) * 0x100000001b3ull;
    h = (h ^ (h >> 30)) * 0xbf58476d1ce4e5b9ull;
    h = (h ^ (h >> 27)) * 0x94d049bb133111ebull;
    return h ^ (h >> 31);
}

// A way's run on some number of threads.
typedef struct timing {
    double median_ms; // Of a tick
    double mean_ms;
    uint64_t digest;
} timing;

typedef struct options {
    uint32_t warmup, ticks;
    uint32_t threads; // Only this many (0 for every core), when `one_count`
    bool one_count;
} options;

static int compare_doubles(const void *a, const void *b)
{
    const double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

static timing run_way(const versus_way *way, const versus_flag *flags, const options *o, const tide_jobs *jobs, double *ms)
{
    void *state = way->make(flags);
    for (uint32_t i = 0; i < o->warmup; i++) way->tick(state, jobs);
    double total = 0.0;
    for (uint32_t i = 0; i < o->ticks; i++) {
        const uint64_t start = tide_time_now_ns();
        way->tick(state, jobs);
        ms[i] = (double)(tide_time_now_ns() - start) / 1e6;
        total += ms[i];
    }
    timing t = {.digest = way->digest(state)};
    way->free(state);
    if (o->ticks) {
        qsort(ms, o->ticks, sizeof *ms, compare_doubles);
        t.median_ms = o->ticks % 2u ? ms[o->ticks / 2u] : (ms[o->ticks / 2u - 1u] + ms[o->ticks / 2u]) / 2.0;
        t.mean_ms = total / (double)o->ticks;
    }
    return t;
}

static void json_text(FILE *f, const char *text)
{
    fputc('"', f);
    for (const unsigned char *c = (const unsigned char *)text; *c; c++) {
        if (*c == '"' || *c == '\\') fprintf(f, "\\%c", *c);
        else if (*c < 0x20) fprintf(f, "\\u%04x", *c);
        else fputc(*c, f);
    }
    fputc('"', f);
}

// One line of JSON: what ran, where, and how long each way's ticks took.
static void write_record(FILE *f, const versus_desc *d, const options *o, const char *label, const uint32_t runs,
                         const uint32_t *threads, const timing (*t)[MAX_WAYS], const bool ok)
{
    char date[32] = "";
    const time_t now = time(NULL);
    const struct tm *utc = gmtime(&now);
    if (utc) strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%SZ", utc);
#if defined(__wasm__)
    const char *os = "web";
#elif defined(_WIN32)
    const char *os = "windows";
#elif defined(__APPLE__)
    const char *os = "macos";
#else
    const char *os = "linux";
#endif
#if defined(__OPTIMIZE__)
    const bool optimized = true;
#else
    const bool optimized = false;
#endif
    fprintf(f, "{\"benchmark\":");
    json_text(f, d->name);
    fprintf(f, ",\"label\":");
    json_text(f, label);
    fprintf(f, ",\"date\":\"%s\",\"os\":\"%s\",\"optimized\":%s", date, os, optimized ? "true" : "false");
    fprintf(f, ",\"options\":{\"warmup\":%u,\"ticks\":%u", o->warmup, o->ticks);
    for (uint32_t i = 0; i < d->flag_count; i++) fprintf(f, ",\"%s\":%u", d->flags[i].name, d->flags[i].value);
    fprintf(f, "},\"ok\":%s,\"digest\":\"0x%016llx\",\"runs\":[", ok ? "true" : "false",
            (unsigned long long)t[0][0].digest);
    for (uint32_t r = 0; r < runs; r++) {
        fprintf(f, "%s{\"threads\":%u,\"ways\":{", r ? "," : "", threads[r]);
        for (uint32_t w = 0; w < d->way_count; w++) {
            fprintf(f, "%s", w ? "," : "");
            json_text(f, d->ways[w].name);
            fprintf(f, ":{\"median_ms\":%.6g,\"mean_ms\":%.6g}", t[r][w].median_ms, t[r][w].mean_ms);
        }
        fprintf(f, "}}");
    }
    fprintf(f, "]}\n");
}

// Adds the run to TIDE_BENCH_RESULTS/versus.jsonl, or prints its record where
// that can't be written (as on the web, which only sees its own folder).
static void save_results(const versus_desc *d, const options *o, const char *label, const uint32_t runs,
                         const uint32_t *threads, const timing (*t)[MAX_WAYS], const bool ok)
{
    const char *path = TIDE_BENCH_RESULTS "/versus.jsonl";
    FILE *f = fopen(path, "ab");
    if (!f) {
        printf("\n  couldn't add to %s; here's the record instead:\n", path);
        write_record(stdout, d, o, label, runs, threads, t, ok);
        return;
    }
    write_record(f, d, o, label, runs, threads, t, ok);
    fclose(f);
    printf("\n  saved as '%s' in %s\n", label, path);
}

static void usage(const versus_desc *d)
{
    printf("usage: %s [--quick] [options]\n\n", d->name);
    printf("  --quick             a short, small run, as tests make\n");
    printf("  --ticks <n>         ticks to time (%u)\n", d->ticks);
    printf("  --warmup <n>        ticks to run first, untimed, at least 1 (%u)\n", d->warmup);
    printf("  --threads <n>       only this many threads, 0 for every core (one, then every core)\n");
    printf("  --split             C splits its work across threads however little there is\n");
    printf("  --save <label>      adds the results to bench/results/versus.jsonl\n");
    for (uint32_t i = 0; i < d->flag_count; i++) {
        printf("  --%-17s %s (%u)\n", d->flags[i].name, d->flags[i].help, d->flags[i].full);
    }
}

// The options from the command line, over a full run's or --quick's.
static bool parse(const versus_desc *d, const int argc, char **argv, options *o, const char **save)
{
    bool quick = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--quick") == 0) quick = true;
    }
    always_split = quick;
    *o = (options){quick ? d->quick_warmup : d->warmup, quick ? d->quick_ticks : d->ticks, 0, false};
    for (uint32_t i = 0; i < d->flag_count; i++) d->flags[i].value = quick ? d->flags[i].quick : d->flags[i].full;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--quick") == 0) continue;
        if (strcmp(arg, "--split") == 0) {
            always_split = true;
            continue;
        }
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) return false;
        if (i + 1 >= argc || strncmp(arg, "--", 2) != 0) {
            fprintf(stderr, "%s: '%s' isn't an option, or it needs a value\n", d->name, arg);
            return false;
        }
        const char *value = argv[++i];
        char *end = NULL;
        const unsigned long number = strtoul(value, &end, 10);
        const bool numeric = end != value && *end == '\0' && number <= UINT32_MAX;
        bool known = true;
        if (strcmp(arg, "--save") == 0) {
            *save = value;
        } else if (!numeric) {
            known = false;
        } else if (strcmp(arg, "--ticks") == 0) {
            o->ticks = (uint32_t)number;
        } else if (strcmp(arg, "--warmup") == 0) {
            o->warmup = (uint32_t)number;
        } else if (strcmp(arg, "--threads") == 0) {
            o->threads = (uint32_t)number;
            o->one_count = true;
        } else {
            known = false;
            for (uint32_t k = 0; k < d->flag_count; k++) {
                if (strcmp(arg + 2, d->flags[k].name) == 0) {
                    d->flags[k].value = (uint32_t)number;
                    known = true;
                }
            }
        }
        if (!known) {
            fprintf(stderr, "%s: '%s %s' isn't an option it knows\n", d->name, arg, value);
            return false;
        }
    }
    if (o->warmup < 1u) {
        fprintf(stderr, "%s: --warmup is at least 1, the tick that sets every way up\n", d->name);
        return false;
    }
    return true;
}

int versus_main(const versus_desc *d, const int argc, char **argv)
{
    options o;
    const char *save = NULL; // The label to save the results under
    if (!parse(d, argc, argv, &o, &save) || d->way_count < 1u || d->way_count > MAX_WAYS) {
        usage(d);
        return 2;
    }

    // One thread, then every core; or the threads asked for
    const tide_jobs *jobs[MAX_RUNS];
    uint32_t threads[MAX_RUNS];
    uint32_t runs = 0;
    if (o.one_count) {
        jobs[runs++] = jobs_for(o.threads);
    } else {
        jobs[runs++] = NULL;
        const tide_jobs *every = jobs_for(0);
        if (thread_count(every) > 1u) jobs[runs++] = every;
    }
    for (uint32_t r = 0; r < runs; r++) threads[r] = thread_count(jobs[r]);

#if defined(__OPTIMIZE__)
    const char *build = "optimized";
#else
    const char *build = "unoptimized (the times mean little)";
#endif
#if defined(__wasm__)
    const char *platform = "web";
#else
    const char *platform = "native";
#endif
    printf("%s: %s\n  ", d->name, d->about);
    for (uint32_t i = 0; i < d->flag_count; i++) printf("%s %u; ", d->flags[i].name, d->flags[i].value);
    printf("%u ticks timed after %u; %s %s build\n", o.ticks, o.warmup, build, platform);
    fflush(stdout);

    double *ms = malloc((o.ticks ? o.ticks : 1u) * sizeof *ms);
    if (!ms) {
        fprintf(stderr, "%s: out of memory\n", d->name);
        return 1;
    }
    timing t[MAX_RUNS][MAX_WAYS];
    bool ok = true;
    for (uint32_t r = 0; r < runs; r++) {
        for (uint32_t w = 0; w < d->way_count; w++) {
            t[r][w] = run_way(&d->ways[w], d->flags, &o, jobs[r], ms);
            ok &= t[r][w].digest == t[0][0].digest;
        }
    }
    free(ms);

    // Ways down, threads across: each one's milliseconds a tick, and beside
    // the other ways', how many times as long Tide's took
    printf("\n  %-14s", "ms a tick");
    for (uint32_t r = 0; r < runs; r++) {
        char head[32];
        snprintf(head, sizeof head, "%u thread%s", threads[r], threads[r] == 1u ? "" : "s");
        printf("  %-20s", head);
    }
    printf("\n");
    for (uint32_t w = 0; w < d->way_count; w++) {
        printf("  %-14s", d->ways[w].name);
        for (uint32_t r = 0; r < runs; r++) {
            char cell[32] = "";
            if (w) snprintf(cell, sizeof cell, "%.2fx", t[r][0].median_ms / t[r][w].median_ms);
            printf("  %9.3f %-10s", t[r][w].median_ms, cell);
        }
        printf("\n");
    }
    printf("\n  medians; beside C, how many times as long Tide takes\n");

    if (ok) {
        printf("  every way ended the same: 0x%016llx\n", (unsigned long long)t[0][0].digest);
    } else {
        printf("  FAILED: the ways ended differently:\n");
        for (uint32_t r = 0; r < runs; r++) {
            for (uint32_t w = 0; w < d->way_count; w++) {
                printf("    %-14s on %u thread%s: 0x%016llx\n", d->ways[w].name, threads[r], threads[r] == 1u ? "" : "s",
                       (unsigned long long)t[r][w].digest);
            }
        }
    }
    if (save) save_results(d, &o, save, runs, threads, (const timing(*)[MAX_WAYS])t, ok);
    return ok ? 0 : 1;
}
