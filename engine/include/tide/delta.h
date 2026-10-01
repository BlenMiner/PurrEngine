#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "tide/net.h"
#include "tide/page.h"

// Deltas: a world as what changed from another world, its base, page by page.
//
// Temporary implementation written by Claude; the project owner takes it over
// later.
//
// A world's data is in regions: its struct (without its storage), then each
// page of its entity table, of each archetype's table (each chunk's column) and
// of its heap, as far as it's in use. A delta (tide_world_pack_delta) has each
// part's own numbers (how many entities, rows, bytes of heap), then, for each
// region, either that it's the same as the base's region in its place, or its
// bytes. Unpacking takes the same regions from the base, sharing its pages,
// and checks the world's hash.
//
// The base is one of two kinds:
// - A world both sides have, bit for bit: a region that changed goes as its
//   bytes XOR'd with the base's, so what's the same in it is zeros.
// - A world only the receiver has: the sender lists its regions' hashes
//   (tide_world_hash_pages), the receiver says which of them its world lacks
//   (tide_world_need_pages), and the delta has those, whole.
// With no base, every region goes whole: the world as a whole.
//
// Bytes go as runs: (zeros, literal count, literal bytes) repeated, the counts
// as varints, a literal ending at two zeros in a row. A world is mostly zeros,
// and a region XOR'd with its base even more.
//
// Generated code calls these for each part of a world, in a fixed order, and
// each part (tide/entity.h, tide/table.h, tide/heap.h) its regions.

// Writing a delta. Its bytes grow as they need.
typedef struct tide_delta_writer {
    tide_writer bytes;
    bool xor;     // Regions are XOR'd with the base's
    bool by_need; // Which regions to send is what the receiver said it needs
    tide_reader need;
    bool lacking;  // The run of `need` being read: regions it needs, or has
    uint32_t left; // ...and how many more of them
    uint32_t same; // Regions the same as the base's since the last one sent
} tide_delta_writer;

// A delta from `base` (a world both have), or from what the receiver said
// it needs (`need`, from tide_needs_end), or from nothing (neither), of a
// world whose hash is `hash`.
void tide_delta_begin(tide_delta_writer *d, bool base, const uint8_t *need, uint32_t need_size, uint64_t hash);

// A part's number.
void tide_delta_number(tide_delta_writer *d, uint32_t v);

// The next region: `size` bytes of `now`, and the base's region in its place
// (NULL when the base has none there), `same` when it's known to be the same
// (its page).
void tide_delta_region(tide_delta_writer *d, const void *now, uint32_t size, const void *base, uint32_t base_size,
                       bool same);

// The end of a part's regions.
void tide_delta_close(tide_delta_writer *d);

// The delta, to free(), and its size.
uint8_t *tide_delta_end(tide_delta_writer *d, uint32_t *size);

// Reading one into an empty world: false at anything that isn't a delta of
// that base.
typedef struct tide_delta_reader {
    tide_reader bytes;
    bool xor;
    bool token;    // `same` was read, and the region after those hasn't been
    uint32_t same; // Regions left that are the same as the base's
} tide_delta_reader;

// Its start, and the hash of the world it makes. `base`: whether there's one.
bool tide_delta_open(tide_delta_reader *d, const uint8_t *data, uint32_t size, bool base, uint64_t *hash);

uint32_t tide_delta_get_number(tide_delta_reader *d);

// The next region, `size` bytes into `out` (zeros), from the base's region in
// its place (NULL for none).
bool tide_delta_get(tide_delta_reader *d, void *out, uint32_t size, const void *base, uint32_t base_size);

// The same as a page of `page_size` bytes, with `refs` references: the base's
// own page when it's the same and `share` (it's the page the part would make),
// else a new one. NULL if the delta is broken.
tide_page *tide_delta_page(tide_delta_reader *d, uint32_t size, uint32_t page_size, uint32_t refs, tide_page *base,
                           uint32_t base_size, bool share);

// The end of a part's regions: false if the delta says there are more.
bool tide_delta_closed(tide_delta_reader *d);

// ---------------------------------------------------------------------------
// What a base lacks: a world's regions' hashes, read part by part, against the
// base's regions in the same places. The answer is runs, alternately of
// regions it has and lacks, starting with ones it has, as varints, as many as
// fit: past them, it lacks everything.

typedef struct tide_needs {
    tide_reader list; // The hashes: each part's count of regions, then theirs
    tide_writer runs;
    bool lacking; // The run being counted
    uint32_t run;
    bool full; // Out of room: the rest is lacking
} tide_needs;

void tide_needs_begin(tide_needs *n, const uint8_t *hashes, uint32_t size, uint8_t *out, uint32_t capacity);

// The next part's count of regions.
uint32_t tide_needs_count(tide_needs *n);

// The next region: whether the base has one in its place, and its hash.
void tide_needs_put(tide_needs *n, bool have, uint64_t hash);

// The next part, a single region of `size` bytes at `data`.
void tide_needs_one(tide_needs *n, const void *data, uint32_t size);

// The runs' size; 0 (lacking everything) if the hashes weren't a list.
uint32_t tide_needs_end(tide_needs *n);
