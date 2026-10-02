#include "bench.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tide/net.h"
#include "tide/platform.h"
#include "tide/time.h"

#define FRAME (1.0 / 60.0) // Made-up seconds between updates
#define JOIN_TIMEOUT 30.0  // Seconds a bot gets to join before the run fails
// A client has caught up when the ticks it has from the server are this close
// behind the server's (in ticks: half a second at 60 a second), and falls
// behind, failing the run, when they're further than twice that.
#define CAUGHT_UP 30u

// Wall-clock times, in milliseconds, to take percentiles of.
typedef struct samples {
    float *ms;
    uint32_t count;
    uint32_t capacity;
} samples;

typedef struct machine machine;

// A transport that counts what its machine sends.
typedef struct counted {
    tide_transport inner;
    machine *owner;
} counted;

struct machine {
    tide_client *client; // NULL for the server
    uint32_t bot;
    double created;   // Made-up seconds when it started joining
    double connected; // ...when it had the world; 0 until then
    double caught_up; // ...and when it caught up with the match: it's in
    uint32_t max_lag; // Ticks it was behind the server at most, once in
    bool failed;
    uint32_t first_tick; // Its predicted tick once connected
    samples steady;      // Its updates once connected, while nobody joins...
    samples joining;     // ...and while somebody does
    // What its updates spent in the game's functions
    double tick_ms, copy_ms, hash_ms, pack_ms, unpack_ms;
    uint64_t ticks, copies, hashes, packs, unpacks;
    uint64_t bytes; // Sent
    counted transport;
};

// The game, with its functions timed for the machine updating now. Machines
// update one at a time, so one at a time is `current`.
static const tide_game *real;
static tide_game timed;
static machine *current;

static double since(const uint64_t start)
{
    return (double)(tide_time_now_ns() - start) / 1e6;
}

static void timed_tick(void *world, const tide_jobs *jobs)
{
    const uint64_t start = tide_time_now_ns();
    real->tick(world, jobs);
    current->tick_ms += since(start);
    current->ticks++;
}

static void timed_copy(void *to, const void *from)
{
    const uint64_t start = tide_time_now_ns();
    real->copy_world(to, from);
    current->copy_ms += since(start);
    current->copies++;
}

static uint64_t timed_hash(const void *world)
{
    const uint64_t start = tide_time_now_ns();
    const uint64_t hash = real->hash_world(world);
    current->hash_ms += since(start);
    current->hashes++;
    return hash;
}

static uint32_t timed_pack(const void *world, uint8_t *out, const uint32_t capacity)
{
    const uint64_t start = tide_time_now_ns();
    const uint32_t size = real->pack_world(world, out, capacity);
    current->pack_ms += since(start);
    if (out) current->packs++; // Not when it only asks the size
    return size;
}

static bool timed_unpack(void *world, const uint8_t *data, const uint32_t size)
{
    const uint64_t start = tide_time_now_ns();
    const bool ok = real->unpack_world(world, data, size);
    current->unpack_ms += since(start);
    current->unpacks++;
    return ok;
}

static void counted_send(void *self, const tide_address to, const void *data, const uint32_t size)
{
    counted *c = self;
    c->owner->bytes += size;
    c->inner.send(c->inner.self, to, data, size);
}

static uint32_t counted_receive(void *self, tide_address *from, void *data, const uint32_t capacity)
{
    const counted *c = self;
    return c->inner.receive(c->inner.self, from, data, capacity);
}

static void counted_close(void *self)
{
    const counted *c = self;
    if (c->inner.close) c->inner.close(c->inner.self);
}

static void counted_end(void *self)
{
    const counted *c = self;
    if (c->inner.end) c->inner.end(c->inner.self);
}

static tide_transport count(machine *m, const tide_transport inner)
{
    m->transport = (counted){inner, m};
    return (tide_transport){&m->transport, counted_send, counted_receive, counted_close, inner.end ? counted_end : NULL};
}

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

// Threads to tick on: NULL for this one alone.
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

static void add_sample(samples *s, const double ms)
{
    if (s->count == s->capacity) {
        s->capacity = s->capacity ? s->capacity * 2u : 1024u;
        s->ms = realloc(s->ms, s->capacity * sizeof *s->ms);
        if (!s->ms) {
            fprintf(stderr, "bench: out of memory\n");
            exit(1);
        }
    }
    s->ms[s->count++] = (float)ms;
}

static void add_samples(samples *to, const samples *from)
{
    for (uint32_t i = 0; i < from->count; i++) add_sample(to, from->ms[i]);
}

static int compare_floats(const void *a, const void *b)
{
    const float x = *(const float *)a;
    const float y = *(const float *)b;
    return (x > y) - (x < y);
}

// Of a set of samples, sorting them.
typedef struct timing {
    double median, p99, max;
    uint32_t count;
} timing;

static timing timing_of(samples *s)
{
    if (!s->count) return (timing){0};
    qsort(s->ms, s->count, sizeof *s->ms, compare_floats);
    return (timing){s->ms[s->count / 2u], s->ms[(uint32_t)((uint64_t)s->count * 99u / 100u)], s->ms[s->count - 1u], s->count};
}

// The server, or a client on average: its work per second of play.
typedef struct side {
    timing update, update_joining; // ms, while nobody joins and while one does
    double simulate_ms, hash_ms, snapshot_ms, pack_ms, unpack_ms;
    double ticks, hashes, snapshots, packs, unpacks; // Calls
    double sent;                                     // Bytes
} side;

// What a run found, as it's printed and saved.
typedef struct results {
    double played; // Made-up seconds
    side server, client;
    double replays;     // Ticks clients ran for each one played
    timing world;       // Seconds until a bot had the world...
    timing caught_up;   // ...and until it caught up with the match
    uint32_t max_lag;   // Ticks a client was behind the server at most, once in
    double pack_ms;     // Packing the world, each time
    uint32_t sent_size; // The world at the end, as sent...
    uint32_t size;      // ...and in memory
    uint32_t tick;
    uint64_t hash;
    uint32_t resyncs;
    bool joined; // Every bot joined
    bool ok;
} results;

static side side_of(const machine *m, const double seconds)
{
    const double s = seconds > 0.0 ? seconds : 1.0;
    return (side){
        .simulate_ms = m->tick_ms / s,
        .hash_ms = m->hash_ms / s,
        .snapshot_ms = m->copy_ms / s,
        .pack_ms = m->pack_ms / s,
        .unpack_ms = m->unpack_ms / s,
        .ticks = (double)m->ticks / s,
        .hashes = (double)m->hashes / s,
        .snapshots = (double)m->copies / s,
        .packs = (double)m->packs / s,
        .unpacks = (double)m->unpacks / s,
        .sent = (double)m->bytes / s,
    };
}

static void collect(results *r, machine *machines, const uint32_t bots, const tide_server *server)
{
    machine *server_machine = &machines[0];
    r->server = side_of(server_machine, r->played);
    r->server.update = timing_of(&server_machine->steady);
    r->server.update_joining = timing_of(&server_machine->joining);
    r->pack_ms = server_machine->packs ? server_machine->pack_ms / (double)server_machine->packs : 0.0;

    // Clients add up, over the time each was in the match
    machine sum = {0};
    double seconds = 0.0;
    uint64_t advanced = 0; // Ticks clients moved on from when they joined
    samples steady = {0}, joining = {0}, worlds = {0}, joins = {0};
    for (uint32_t i = 1; i <= bots; i++) {
        const machine *m = &machines[i];
        sum.tick_ms += m->tick_ms;
        sum.hash_ms += m->hash_ms;
        sum.copy_ms += m->copy_ms;
        sum.unpack_ms += m->unpack_ms;
        sum.ticks += m->ticks;
        sum.hashes += m->hashes;
        sum.copies += m->copies;
        sum.unpacks += m->unpacks;
        sum.bytes += m->bytes;
        seconds += r->played - m->created;
        const tide_client_status s = tide_client_status_of(m->client);
        if (m->connected > 0.0) {
            advanced += s.predicted_tick - m->first_tick;
            add_sample(&worlds, m->connected - m->created);
        }
        if (m->caught_up > 0.0) add_sample(&joins, m->caught_up - m->created);
        if (m->max_lag > r->max_lag) r->max_lag = m->max_lag;
        r->resyncs += s.resyncs;
        add_samples(&steady, &m->steady);
        add_samples(&joining, &m->joining);
    }
    r->client = side_of(&sum, seconds);
    r->client.update = timing_of(&steady);
    r->client.update_joining = timing_of(&joining);
    r->replays = advanced ? (double)sum.ticks / (double)advanced : 0.0;
    r->world = timing_of(&worlds);
    r->caught_up = timing_of(&joins);
    free(steady.ms);
    free(joining.ms);
    free(worlds.ms);
    free(joins.ms);

    // The world at the end, in memory and as it's sent whole (a delta from nothing)
    const void *world = tide_server_world(server);
    r->size = real->pack_world(world, NULL, 0);
    uint32_t sent = r->size;
    uint8_t *whole = real->pack_delta ? real->pack_delta(world, NULL, NULL, 0, &sent) : NULL;
    r->sent_size = whole ? sent : r->size;
    free(whole);
    r->tick = tide_server_tick(server);
    r->hash = real->hash_world(world);
}

static void print_timing(const char *label, const timing *t)
{
    if (!t->count) printf("  %-26s %8s\n", label, "-");
    else printf("  %-26s %8.2f %8.2f %8.2f %8u\n", label, t->median, t->p99, t->max, t->count);
}

// "12.3 (45)": ms per second of play, and calls.
static void print_work(const double ms, const double calls)
{
    char cell[32];
    if (calls > 0.0) snprintf(cell, sizeof cell, calls >= 100.0 ? "%.2f (%.0f)" : "%.2f (%.3g)", ms, calls);
    else snprintf(cell, sizeof cell, "-");
    printf(" %16s", cell);
}

static void print_bytes(const char *label, const double bytes)
{
    if (bytes >= 1e6) printf("%s%.2f MB", label, bytes / 1e6);
    else printf("%s%.1f KB", label, bytes / 1e3);
}

static void print_side(const char *label, const side *s)
{
    printf("  %-26s", label);
    print_work(s->simulate_ms, s->ticks);
    print_work(s->hash_ms, s->hashes);
    print_work(s->snapshot_ms, s->snapshots);
    print_work(s->pack_ms, s->packs);
    print_work(s->unpack_ms, s->unpacks);
    printf("\n");
}

static void print_results(const results *r)
{
    printf("\n  %-26s %8s %8s %8s %8s\n", "update (ms)", "median", "p99", "max", "updates");
    print_timing("server", &r->server.update);
    print_timing("server, while one joins", &r->server.update_joining);
    print_timing("clients", &r->client.update);
    print_timing("clients, while one joins", &r->client.update_joining);

    printf("\n  %-26s %16s %16s %16s %16s %16s\n", "ms per second (calls)", "simulate", "hash", "snapshot", "pack", "unpack");
    print_side("server", &r->server);
    print_side("a client (mean)", &r->client);
    if (r->replays > 0.0) {
        printf("  clients ran %.1f ticks for each one played: the rest ran again, after a guess went wrong\n", r->replays);
    }

    printf("\n");
    print_bytes("  sent per second: server ", r->server.sent);
    print_bytes(", a client ", r->client.sent);
    printf("\n");
    if (r->world.count) {
        printf("  joining: the world took %.2f s median, %.2f s at most", r->world.median, r->world.max);
        if (r->caught_up.count) printf("; catching up, %.2f s median, %.2f s at most", r->caught_up.median, r->caught_up.max);
        printf("\n");
    }
    if (r->pack_ms > 0.0) {
        printf("  the server packed the world %.0f times, %.2f ms each", r->server.packs * r->played, r->pack_ms);
        if (r->caught_up.count) printf("; clients were %u ticks behind it at most, once in", r->max_lag);
        printf("\n");
    }
    print_bytes("  the world: ", (double)r->sent_size);
    print_bytes(" as sent, ", (double)r->size);
    printf(" in memory\n");
}

static void print_verdict(const results *r, const uint64_t expect)
{
    printf("\n  tick %u, hash 0x%016llx", r->tick, (unsigned long long)r->hash);
    if (expect) printf(r->hash == expect ? " as expected" : ", but 0x%016llx was expected", (unsigned long long)expect);
    printf("\n");
    if (r->resyncs) printf("  FAILED: clients' worlds went wrong %u times, and the server sent them again\n", r->resyncs);
    if (!r->joined) printf("  FAILED: not every bot joined\n");
    else if (!r->ok) printf("  FAILED\n");
}

// Saved results go here, one file per benchmark (see bench/compare.mjs).
#ifndef TIDE_BENCH_RESULTS
#define TIDE_BENCH_RESULTS "results"
#endif

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

static void json_timing(FILE *f, const char *key, const timing *t)
{
    fprintf(f, ",\"%s\":{\"median\":%.6g,\"p99\":%.6g,\"max\":%.6g,\"count\":%u}", key, t->median, t->p99, t->max, t->count);
}

static void json_side(FILE *f, const char *key, const side *s)
{
    fprintf(f, ",\"%s\":{\"simulate_ms\":%.6g,\"hash_ms\":%.6g,\"snapshot_ms\":%.6g,\"pack_ms\":%.6g,\"unpack_ms\":%.6g", key,
            s->simulate_ms, s->hash_ms, s->snapshot_ms, s->pack_ms, s->unpack_ms);
    fprintf(f, ",\"ticks\":%.6g,\"hashes\":%.6g,\"snapshots\":%.6g,\"packs\":%.6g,\"unpacks\":%.6g,\"sent_bytes\":%.6g", s->ticks,
            s->hashes, s->snapshots, s->packs, s->unpacks, s->sent);
    json_timing(f, "update_ms", &s->update);
    json_timing(f, "update_ms_joining", &s->update_joining);
    fputc('}', f);
}

// One line of JSON: what ran, where, and what it found. Work and bytes are
// per second of play.
static void write_record(FILE *f, const bench_desc *d, const bench_options *o, const uint32_t threads, const char *label,
                         const results *r)
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
    fprintf(f, ",\"date\":\"%s\",\"os\":\"%s\",\"optimized\":%s,\"threads\":%u", date, os, optimized ? "true" : "false",
            threads);
    fprintf(f, ",\"options\":{\"players\":%u,\"join_every\":%.6g,\"seconds\":%.6g,\"latency_ms\":%.6g,\"jitter_ms\":%.6g,"
               "\"loss_percent\":%.6g",
            o->players, o->join_every, o->seconds, o->latency * 1e3, o->jitter * 1e3, o->loss * 1e2);
    for (uint32_t i = 0; i < d->flag_count; i++) fprintf(f, ",\"%s\":%u", d->flags[i].name, d->flags[i].value);
    fprintf(f, "},\"ok\":%s,\"joined\":%s,\"resyncs\":%u,\"tick\":%u,\"hash\":\"0x%016llx\",\"played_s\":%.6g",
            r->ok ? "true" : "false", r->joined ? "true" : "false", r->resyncs, r->tick, (unsigned long long)r->hash,
            r->played);
    json_side(f, "server", &r->server);
    json_side(f, "client", &r->client);
    fprintf(f, ",\"replays\":%.6g,\"pack_ms_each\":%.6g,\"max_lag_ticks\":%u", r->replays, r->pack_ms, r->max_lag);
    json_timing(f, "world_s", &r->world);
    json_timing(f, "caught_up_s", &r->caught_up);
    fprintf(f, ",\"world_sent_bytes\":%u,\"world_bytes\":%u}\n", r->sent_size, r->size);
}

// Adds the run to TIDE_BENCH_RESULTS/<name>.jsonl, or prints its record where
// that can't be written (as on the web, which only sees its own folder).
static void save_results(const bench_desc *d, const bench_options *o, const uint32_t threads, const char *label,
                         const results *r)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.jsonl", TIDE_BENCH_RESULTS, d->name);
    FILE *f = fopen(path, "ab");
    if (!f) {
        printf("\n  couldn't add to %s; here's the record instead:\n", path);
        write_record(stdout, d, o, threads, label, r);
        return;
    }
    write_record(f, d, o, threads, label, r);
    fclose(f);
    printf("\n  saved as '%s' in %s\n", label, path);
}

static void usage(const bench_desc *d)
{
    printf("usage: %s [--quick] [options]\n\n", d->name);
    printf("  --quick             a short, small run, as tests make\n");
    printf("  --players <n>       bots that join, up to %u (%u)\n", TIDE_MAX_PLAYERS, d->full.players);
    printf("  --join-every <s>    seconds between joins (%g)\n", d->full.join_every);
    printf("  --seconds <s>       how long to run once everyone's in (%g)\n", d->full.seconds);
    printf("  --threads <n>       threads to tick on, 0 for every core (%u)\n", d->full.threads);
    printf("  --latency <ms>      network delay each way (%g)\n", d->full.latency * 1e3);
    printf("  --jitter <ms>       up to this much more, at random (%g)\n", d->full.jitter * 1e3);
    printf("  --loss <percent>    datagrams lost (%g)\n", d->full.loss * 1e2);
    printf("  --expect <hash>     the server's world's hash at the end; fails on another\n");
    printf("  --save <label>      adds the results to bench/results/%s.jsonl (see bench/compare.mjs)\n", d->name);
    for (uint32_t i = 0; i < d->flag_count; i++) {
        printf("  --%-17s %s (%u)\n", d->flags[i].name, d->flags[i].help, d->flags[i].full);
    }
}

// The options from the command line, over a full run's or --quick's.
static bool parse(const bench_desc *d, const int argc, char **argv, bench_options *o, const char **save)
{
    bool quick = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--quick") == 0) quick = true;
    }
    *o = quick ? d->quick : d->full;
    for (uint32_t i = 0; i < d->flag_count; i++) d->flags[i].value = quick ? d->flags[i].quick : d->flags[i].full;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--quick") == 0) continue;
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) return false;
        if (i + 1 >= argc || strncmp(arg, "--", 2) != 0) {
            fprintf(stderr, "%s: '%s' isn't an option, or it needs a value\n", d->name, arg);
            return false;
        }
        const char *value = argv[++i];
        char *end = NULL;
        const double number = strtod(value, &end);
        const bool numeric = end != value && *end == '\0' && number >= 0.0;
        bool known = true;
        if (strcmp(arg, "--save") == 0) {
            *save = value;
        } else if (strcmp(arg, "--expect") == 0) {
            o->expect = strtoull(value, &end, 0);
            if (end == value || *end != '\0') known = false;
        } else if (!numeric) {
            known = false;
        } else if (strcmp(arg, "--players") == 0) {
            o->players = (uint32_t)number;
        } else if (strcmp(arg, "--join-every") == 0) {
            o->join_every = number;
        } else if (strcmp(arg, "--seconds") == 0) {
            o->seconds = number;
        } else if (strcmp(arg, "--threads") == 0) {
            o->threads = (uint32_t)number;
        } else if (strcmp(arg, "--latency") == 0) {
            o->latency = number / 1e3;
        } else if (strcmp(arg, "--jitter") == 0) {
            o->jitter = number / 1e3;
        } else if (strcmp(arg, "--loss") == 0) {
            o->loss = number / 1e2;
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
    if (o->players < 1 || o->players > TIDE_MAX_PLAYERS) {
        fprintf(stderr, "%s: --players goes from 1 to %u\n", d->name, TIDE_MAX_PLAYERS);
        return false;
    }
    return true;
}

// A bot's input: `user` is its machine.
static const bench_desc *the_desc;

static void sample(void *user, const uint32_t tick, void *input)
{
    const machine *m = user;
    memset(input, 0, real->input_size);
    the_desc->sample(the_desc->flags, m->bot, tick, input);
}

int bench_main(const bench_desc *d, const int argc, char **argv)
{
    bench_options o;
    const char *save = NULL; // The label to save the results under
    if (!parse(d, argc, argv, &o, &save)) {
        usage(d);
        return 2;
    }
    the_desc = d;
    real = d->game;
    timed = *d->game;
    timed.tick = timed_tick;
    timed.copy_world = timed_copy;
    timed.hash_world = timed_hash;
    timed.pack_world = timed_pack;
    timed.unpack_world = timed_unpack;

    const tide_jobs *jobs = jobs_for(o.threads);

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
    printf("%s: %u bots, joining %g s apart, then %g s; %g ms each way (+%g), %g%% lost; %u thread%s; %s %s build\n", d->name,
           o.players, o.join_every, o.seconds, o.latency * 1e3, o.jitter * 1e3, o.loss * 1e2, thread_count(jobs),
           thread_count(jobs) == 1 ? "" : "s", build, platform);
    if (d->flag_count) {
        printf("  ");
        for (uint32_t i = 0; i < d->flag_count; i++) printf("%s%s %u", i ? ", " : "", d->flags[i].name, d->flags[i].value);
        printf("\n");
    }
    fflush(stdout);

    tide_loopback *net = tide_loopback_create(0x7469646562656e63ull);
    tide_loopback_set_conditions(net, (tide_net_conditions){.latency = o.latency, .jitter = o.jitter, .loss = o.loss});

    // The server is machine 0, and bot n machine n + 1.
    static machine machines[TIDE_MAX_PLAYERS + 1u];
    memset(machines, 0, sizeof machines);
    machine *server_machine = &machines[0];
    current = server_machine;
    void *first = calloc(1, real->world_size);
    if (!first) {
        fprintf(stderr, "%s: out of memory\n", d->name);
        tide_loopback_destroy(net);
        return 1;
    }
    d->make_world(d->flags, first, (float)FRAME);
    const tide_server_desc server_desc = {
        .game = &timed,
        .dt = (float)FRAME,
        .transports = {count(server_machine, tide_loopback_endpoint(net, 1))},
        .jobs = jobs,
        .world = first,
    };
    tide_server *server = tide_server_create(&server_desc, 0.0);
    if (real->free_world) real->free_world(first); // The server has its own copy
    free(first);

    double now = 0.0;
    uint32_t bots = 0;     // Made so far
    double all_in = -1.0;  // When the last one caught up
    bool failed = false;
    while (true) {
        if (bots < o.players && now >= (double)bots * o.join_every) {
            machine *m = &machines[bots + 1u];
            m->bot = bots;
            m->created = now;
            current = m;
            const tide_client_desc client_desc = {
                .game = &timed,
                .transport = count(m, tide_loopback_endpoint(net, bots + 2u)),
                .server = tide_loopback_address(1),
                .sample = real->input_size ? sample : NULL,
                .user = m,
                .lead = 2,
                .jobs = jobs,
            };
            m->client = tide_client_create(&client_desc, now);
            bots++;
        }

        now += FRAME;
        tide_loopback_set_time(net, now);
        bool joining = false; // Somebody is, as this frame starts
        for (uint32_t i = 1; i <= bots; i++) joining |= machines[i].caught_up == 0.0 && !machines[i].failed;

        for (uint32_t i = 1; i <= bots; i++) {
            machine *m = &machines[i];
            if (m->failed) continue;
            current = m;
            const uint64_t t = tide_time_now_ns();
            tide_client_update(m->client, now);
            if (m->connected > 0.0) add_sample(joining ? &m->joining : &m->steady, since(t));
        }
        current = server_machine;
        const uint64_t t = tide_time_now_ns();
        tide_server_update(server, now);
        add_sample(joining ? &server_machine->joining : &server_machine->steady, since(t));

        bool everyone = bots == o.players;
        const uint32_t server_tick = tide_server_tick(server);
        for (uint32_t i = 1; i <= bots; i++) {
            machine *m = &machines[i];
            if (m->failed) continue;
            const tide_client_status s = tide_client_status_of(m->client);
            const bool in = s.state == TIDE_SESSION_CONNECTED;
            if (in && m->connected == 0.0) {
                m->connected = now;
                m->first_tick = s.predicted_tick;
            }
            const uint32_t lag = in && server_tick > s.verified_tick ? server_tick - s.verified_tick : 0u;
            if (in && m->caught_up == 0.0 && lag <= CAUGHT_UP) m->caught_up = now;
            if (m->caught_up > 0.0 && lag > m->max_lag) m->max_lag = lag;
            if (s.state == TIDE_SESSION_OFFLINE) {
                printf("  bot %u went offline (reason %d) at %.2f s; stopping\n", m->bot, (int)s.reason, now);
                m->failed = failed = true;
            } else if (m->caught_up == 0.0 && now - m->created > JOIN_TIMEOUT) {
                if (m->connected > 0.0) {
                    printf("  bot %u got the world after %.2f s, but in %g s it never caught up with the match; stopping\n",
                           m->bot, m->connected - m->created, JOIN_TIMEOUT);
                } else {
                    printf("  bot %u didn't get the world in %g s; stopping\n", m->bot, JOIN_TIMEOUT);
                }
                m->failed = failed = true;
            } else if (m->max_lag > 2u * CAUGHT_UP) {
                printf("  bot %u fell %u ticks behind the server at %.2f s; stopping\n", m->bot, m->max_lag, now);
                m->failed = failed = true;
            }
            everyone &= m->caught_up > 0.0;
        }
        if (failed) break;
        if (everyone && all_in < 0.0) all_in = now;
        if (all_in >= 0.0 && now - all_in >= o.seconds) break;
    }

    results r = {.played = now, .joined = !failed};
    collect(&r, machines, bots, server);
    r.ok = r.joined && r.resyncs == 0 && (!o.expect || r.hash == o.expect);
    print_results(&r);
    if (d->describe) d->describe(tide_server_world(server));
    print_verdict(&r, o.expect);
    if (save) save_results(d, &o, thread_count(jobs), save, &r);

    for (uint32_t i = 1; i <= bots; i++) {
        current = &machines[i];
        tide_client_destroy(machines[i].client);
        free(machines[i].steady.ms);
        free(machines[i].joining.ms);
    }
    current = server_machine;
    tide_server_destroy(server);
    free(server_machine->steady.ms);
    free(server_machine->joining.ms);
    tide_loopback_destroy(net);
    return r.ok ? 0 : 1;
}
