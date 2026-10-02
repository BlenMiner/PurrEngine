#pragma once

#include <stddef.h>
#include <stdint.h>

#include "tide/jobs.h"

// C against Tide: a concept written both ways, each as fast as its language
// allows, timed on the same work (see AGENTS.md, Benchmarks).
//
// Every way of writing it ("Tide", "C", and maybe more C, with a trick Tide
// can't do) starts from nothing and runs the same ticks, the first of which
// sets it up. The run times each tick after the warm-up ones, on one thread
// and then on every core, and checks that every way ends with the same
// result, bit for bit: they did the same work, so their times compare.
//
// The C is built with the same flags as the code tidec generates (no fast
// math, no fused multiply-add: see AGENTS.md, Determinism), so what differs is
// the language and the engine under it, not the compiler's options.

// A tick's length, as every way takes it: Time.dt in Tide.
#define VERSUS_DT (1.0f / 60.0f)

// A concept's own option, beside the common ones: --name <value>.
typedef struct versus_flag {
    const char *name;
    const char *help;
    uint32_t full;  // Its value in a full run...
    uint32_t quick; // ...and with --quick
    uint32_t value; // What the run uses
} versus_flag;

// One way of writing the concept.
typedef struct versus_way {
    const char *name;
    // Its state before the first tick, from the flags' values
    void *(*make)(const versus_flag *flags);
    // A tick, on `jobs`' threads, or NULL for this one's alone
    void (*tick)(void *state, const tide_jobs *jobs);
    // What it ended with, which every way has to agree on: versus_item's
    // of what it holds, added up
    uint64_t (*digest)(const void *state);
    void (*free)(void *state);
} versus_way;

typedef struct versus_desc {
    const char *name;  // Of the concept, as reports say it
    const char *about; // What every way does, in a line
    versus_flag *flags;
    uint32_t flag_count;
    uint32_t warmup, ticks;             // A full run's: untimed, then timed
    uint32_t quick_warmup, quick_ticks; // --quick's, for tests
    const versus_way *ways;             // The first is Tide's, which the rest are compared with
    uint32_t way_count;
} versus_desc;

// Reads the command line, runs every way and reports. Returns the program's
// exit code: 0 when every way ended the same, on every number of threads.
int versus_main(const versus_desc *desc, int argc, char **argv);

// Runs work(context, piece) for every piece from 0 to count - 1 on `jobs`'
// threads, each piece once, and returns when they're all done; with no jobs,
// in order on this thread. Whatever the threads, the pieces have to give the
// same result.
void versus_for(const tide_jobs *jobs, uint32_t count, void (*work)(void *context, uint32_t piece), void *context);

// How many pieces to split `count` things into on `jobs`' threads: 8 for each
// thread, so a thread that wakes late holds nobody up, or fewer when there
// are fewer things. 1 with no jobs, or fewer than `enough` things: too little
// work for threads to pay for waking (--split ignores it, and --quick, so
// tests check that splitting gives the same result).
uint32_t versus_pieces(const tide_jobs *jobs, uint32_t count, uint32_t enough);

// A thing a way holds (a particle, a cell and where it is), as its digest
// adds it up: the sum doesn't depend on the order, so ways can keep things in
// any order.
uint64_t versus_item(const void *data, size_t size);
