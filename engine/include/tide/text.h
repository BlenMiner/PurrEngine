#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <stddef.h>

#include "tide/color.h"
#include "tide/entity.h"
#include "tide/gui.h"
#include "tide/heap.h"
#include "tide/math.h"
#include "tide/player.h"

// Tide's `string`: text as a value. Generated code builds, joins, formats
// and compares it with the functions below.
//
// Temporary implementation written by Claude; the project owner takes it over
// later.
//
// A tide_str is a view: where the text is, its length in bytes (UTF-8) and in
// characters (code points), which is what Tide's Length counts. Text that
// code makes (joining, formatting, Substring) goes in the scratch area, which
// the code that ran it clears once it's done: a system for each entity, a
// view, a handler. The byte after the text can always be read, and it's
// usually a NUL; tide_str_c gives C text either way.
//
// Everything here is deterministic: numbers become text through exact integer
// arithmetic, never printf, so the same value gives the same bytes on every
// platform. Nothing fails: when the scratch area is full, text stops growing.

typedef struct tide_str {
    const char *ptr;
    int32_t bytes;
    int32_t chars;
} tide_str;

#define TIDE_STR_EMPTY ((tide_str){"", 0, 0})

#ifndef TIDE_SCRATCH_BYTES
#define TIDE_SCRATCH_BYTES (1u << 20)
#endif

// The scratch area, one per thread: where it is now, and going back there,
// which frees everything made since.
uint32_t tide_scratch_mark(void);
void tide_scratch_reset(uint32_t mark);
// Frees this thread's scratch area; the next text made allocates it again.
// For hot reloading, before a game's library is unloaded (tide/host.h).
void tide_scratch_free(void);

// How numbers are written, as in C# format strings: TIDE_FORMAT('F', 2) is
// "F2", two decimals. 0 is the default: the shortest text that reads back as
// the same float, or an int's digits.
#define TIDE_FORMAT(letter, digits) ((int32_t)(letter) << 8 | (int32_t)(digits))

// Joining: `a` followed by the value. When `a` is the newest text in the
// scratch area, it grows in place, so a chain of joins copies once.
tide_str tide_str_add(tide_str a, tide_str b);
tide_str tide_str_add_cstr(tide_str a, const char *s);
tide_str tide_str_add_int(tide_str a, int32_t v, int32_t format);
tide_str tide_str_add_float(tide_str a, float v, int32_t format);
tide_str tide_str_add_bool(tide_str a, bool v);
tide_str tide_str_add_entity(tide_str a, tide_entity e, bool local);
tide_str tide_str_add_player(tide_str a, tide_player_id p);
tide_str tide_str_add_i2(tide_str a, tide_int2 v, int32_t format);
tide_str tide_str_add_i3(tide_str a, tide_int3 v, int32_t format);
tide_str tide_str_add_i4(tide_str a, tide_int4 v, int32_t format);
tide_str tide_str_add_f2(tide_str a, tide_float2 v, int32_t format);
tide_str tide_str_add_f3(tide_str a, tide_float3 v, int32_t format);
tide_str tide_str_add_f4(tide_str a, tide_float4 v, int32_t format);
tide_str tide_str_add_q(tide_str a, tide_quaternion v, int32_t format);
tide_str tide_str_add_color(tide_str a, tide_color v, int32_t format);
tide_str tide_str_add_rect(tide_str a, tide_rect v, int32_t format);

// Text read from C: `s` measured, not copied.
tide_str tide_str_from_cstr(const char *s);

// Text an extern function returned: a copy in the scratch area, since C may
// reuse its memory, and empty for NULL.
tide_str tide_str_copy_cstr(const char *s);

// Text for C: `s` itself when a NUL follows it, or a copy that ends in one.
const char *tide_str_c(tide_str s);

// Comparing and searching, byte by byte. Positions and lengths count
// characters; out of range, they're clamped, as Tide is forgiving.
bool tide_str_eq(tide_str a, tide_str b);
bool tide_str_contains(tide_str a, tide_str b);
bool tide_str_starts_with(tide_str a, tide_str b);
bool tide_str_ends_with(tide_str a, tide_str b);
int32_t tide_str_index_of(tide_str a, tide_str b); // -1 if it's not there
tide_str tide_str_substring(tide_str a, int32_t start, int32_t length);
tide_str tide_str_substring_from(tide_str a, int32_t start);
tide_str tide_str_to_upper(tide_str a); // ASCII letters only
tide_str tide_str_to_lower(tide_str a);
tide_str tide_str_trim(tide_str a);
tide_str tide_str_replace(tide_str a, tide_str from, tide_str to);

// Counts the characters of UTF-8 text.
int32_t tide_utf8_chars(const char *bytes, int32_t count);

// ---------------------------------------------------------------------------
// Text in fields: of components, singletons, structs and events.
//
// A tide_text is where a field's text is: in a world's heap, or in the
// scratch area, which the top two bits say. It holds no pointer, so a world
// is the same bytes on every machine.
//
// Text in memory that's part of a world (its components, singletons and
// command queue) is the world's own: it has a block in the world's heap,
// which goes back when the text changes or leaves the world. Text anywhere
// else, like a struct copied into a local, only borrows: it points at the
// heap text it was copied from, which stays until the running code is done,
// or at a copy in the scratch area. Generated code says which with `where`:
// the world whose memory the field is in (TIDE_IN_MATCH or TIDE_IN_LOCAL), or
// TIDE_IN_SCRATCH for memory that's no world's. A function changing a `mut`
// struct gets its `where` along with it, so it works the same on a
// component's field and on a local.
//
// Heap text never changes once it's written: changing a field writes new text
// and releases the old, so a copy made before still reads the old text.

typedef struct tide_text {
    uint32_t at; // 0: empty. Otherwise the block's offset, and where in the top two bits
} tide_text;

// Where memory is, for `where` arguments, and in the top two bits of a
// block's tagged offset.
#define TIDE_IN_MATCH 0u
#define TIDE_IN_LOCAL 1u
#define TIDE_IN_SCRATCH 2u

// The heaps of the worlds generated code runs with: the match's (or NULL),
// and the local world's (or NULL).
void tide_text_use(tide_heap *match_heap, tide_heap *local_heap);

// This thread's: the heaps tide_text_use set, by `where`, and the scratch
// area (NULL until it's first used). They're here for the functions below,
// which run for every element code reads or writes, so they inline.
extern TIDE_THREAD_LOCAL tide_heap *tide_world_heaps[2];
extern TIDE_THREAD_LOCAL char *tide_scratch_area;

// The heap of TIDE_IN_MATCH or TIDE_IN_LOCAL, as tide_text_use set it; NULL
// for TIDE_IN_SCRATCH.
static inline tide_heap *tide_heap_of(const uint32_t where)
{
    return where == TIDE_IN_MATCH || where == TIDE_IN_LOCAL ? tide_world_heaps[where] : NULL;
}

// Reading: a view of the text, which lasts until the running code is done.
tide_str tide_text_view(tide_text t);

// A world's own text, read from outside, like a host reading a component.
tide_str tide_text_read(const tide_heap *heap, tide_text t);

// Writing a field: its own copy of `value` if the field is part of a world
// (the old text is released), or a borrowed one.
void tide_text_set(tide_text *field, tide_str value, uint32_t where);

// A value just copied into a world, such as a spawn's components going into
// the command queue: its borrowed text becomes the world's own copy.
void tide_text_own(tide_text *field, uint32_t where);

// A world's own text leaving it, as its entity goes: freed once the running
// code is done.
void tide_text_release(tide_text *field, uint32_t where);

// Text for a value being built, like a struct literal's field: a borrowed
// copy in the scratch area.
tide_text tide_text_temp(tide_str value);

// For tide/list.h, which keeps its blocks the way text does.

// Memory for code as it runs, like the scratch area's but of any size,
// 16-byte aligned: in the area when it has room, or else of its own, freed
// once the area goes back to a mark from before it.
void *tide_scratch_memory(size_t bytes);

// A block in the scratch area with room for `bytes` after its header, 16-byte
// aligned: its header, or NULL when the area is full. `at` gets its tagged
// offset.
tide_block *tide_scratch_block(uint32_t bytes, uint32_t *at);

// A tagged offset's block, or NULL for 0: to read, and to change (a world's
// is made its own, apart from its snapshots: see tide/page.h).
static inline tide_block *tide_block_at(const uint32_t at)
{
    if (!at) return NULL;
    const uint32_t offset = at & 0x3FFFFFFFu;
    if (at >> 30 == TIDE_IN_SCRATCH) return (tide_block *)(uintptr_t)(tide_scratch_area + offset);
    const tide_heap *heap = tide_heap_of(at >> 30);
    return heap ? tide_heap_block(heap, offset) : NULL;
}

static inline tide_block *tide_block_write(const uint32_t at)
{
    if (!at) return NULL;
    const uint32_t offset = at & 0x3FFFFFFFu;
    if (at >> 30 == TIDE_IN_SCRATCH) return (tide_block *)(uintptr_t)(tide_scratch_area + offset);
    tide_heap *heap = tide_heap_of(at >> 30);
    return heap ? tide_heap_write(heap, offset) : NULL;
}

// A `mut string` parameter: the caller's text, a field's or a local's, which
// the function reads and changes.
typedef struct tide_textref {
    tide_text *field; // A field's text, or NULL
    tide_str *local;  // Or a local's
    uint32_t where;   // The field's: whose memory it's in
} tide_textref;

static inline tide_str tide_textref_get(const tide_textref r)
{
    return r.field ? tide_text_view(*r.field) : *r.local;
}

static inline void tide_textref_set(const tide_textref r, const tide_str value)
{
    if (r.field) tide_text_set(r.field, value, r.where);
    else *r.local = value;
}
