#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "tide/session.h"

// Benchmarks: games that stress the engine, played by bots (see AGENTS.md,
// Benchmarks).
//
// A run is a match on a dedicated server, which bots join one after another,
// each a client of its own over the loopback network, as bad as the options
// say. Time is made up, a frame every 60th of a second, so a run goes as fast
// as the machine can take it, and repeats exactly: the same options give the
// same match, bit for bit, on every platform and with any number of threads.
//
// What it measures is each machine's own work: how long the server's and each
// client's updates take, and of that, simulating, hashing, snapshotting and
// packing worlds. The machines run one after another on this thread (each
// ticking on the threads it's given), so the run takes about as long as all
// of them together.

// A benchmark's own option, beside the common ones: --name <value>.
typedef struct bench_flag {
    const char *name;
    const char *help;
    uint32_t full;  // Its value in a full run...
    uint32_t quick; // ...and with --quick
    uint32_t value; // What the run uses
} bench_flag;

typedef struct bench_options {
    uint32_t players;  // Bots that join
    double join_every; // Seconds between joins
    double seconds;    // Run on for this long once everyone's in
    uint32_t threads;  // To tick on: 0 for every core
    double latency;    // Seconds each way
    double jitter;     // Up to this many seconds more, at random
    double loss;       // The share of datagrams lost
    // The server's world's hash at the end, which every platform has to agree
    // on: the run fails if it's another. 0 checks nothing.
    uint64_t expect;
} bench_options;

typedef struct bench_desc {
    const char *name;      // Of the benchmark, as reports say it
    const tide_game *game; // tide_game_api
    bench_options full;    // A full run's options, which flags change
    bench_options quick;   // --quick's: small and short, for tests
    bench_flag *flags;     // Its own options
    uint32_t flag_count;
    // Makes the match's first world, a zeroed tide_world, from the flags'
    // values: tide_world_init, and then whatever it changes before the first
    // tick (a singleton's size, say). `dt` is a tick's length.
    void (*make_world)(const bench_flag *flags, void *world, float dt);
    // A bot's input for a tick: `bot` is its number, from 0. It sees the
    // flags, for the game's size.
    void (*sample)(const bench_flag *flags, uint32_t bot, uint32_t tick, void *input);
    // What the run says about the server's world at the end, after the
    // numbers: a line or two. NULL: nothing.
    void (*describe)(const void *world);
} bench_desc;

// Reads the command line, runs the benchmark and reports. Returns the
// program's exit code: 0 when everyone joined, no world went wrong and the
// hash is the one expected.
int bench_main(const bench_desc *desc, int argc, char **argv);
