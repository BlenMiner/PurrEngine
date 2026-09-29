#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <stddef.h>

#include "purr/color.h"
#include "purr/entity.h"
#include "purr/gui.h"
#include "purr/heap.h"
#include "purr/math.h"
#include "purr/player.h"

// PurrLang's `string`: text as a value. Generated code builds, joins, formats
// and compares it with the functions below.
//
// Temporary implementation written by Claude; the project owner takes it over
// later.
//
// A purr_str is a view: where the text is, its length in bytes (UTF-8) and in
// characters (code points), which is what PurrLang's Length counts. Text that
// code makes (joining, formatting, Substring) goes in the scratch area, which
// the code that ran it clears once it's done: a system for each entity, a
// view, a handler. The byte after the text can always be read, and it's
// usually a NUL; purr_str_c gives C text either way.
//
// Everything here is deterministic: numbers become text through exact integer
// arithmetic, never printf, so the same value gives the same bytes on every
// platform. Nothing fails: when the scratch area is full, text stops growing.

typedef struct purr_str {
    const char *ptr;
    int32_t bytes;
    int32_t chars;
} purr_str;

#define PURR_STR_EMPTY ((purr_str){"", 0, 0})

#ifndef PURR_SCRATCH_BYTES
#define PURR_SCRATCH_BYTES (1u << 20)
#endif

// The scratch area, one per thread: where it is now, and going back there,
// which frees everything made since.
uint32_t purr_scratch_mark(void);
void purr_scratch_reset(uint32_t mark);

// How numbers are written, as in C# format strings: PURR_FORMAT('F', 2) is
// "F2", two decimals. 0 is the default: the shortest text that reads back as
// the same float, or an int's digits.
#define PURR_FORMAT(letter, digits) ((int32_t)(letter) << 8 | (int32_t)(digits))

// Joining: `a` followed by the value. When `a` is the newest text in the
// scratch area, it grows in place, so a chain of joins copies once.
purr_str purr_str_add(purr_str a, purr_str b);
purr_str purr_str_add_cstr(purr_str a, const char *s);
purr_str purr_str_add_int(purr_str a, int32_t v, int32_t format);
purr_str purr_str_add_float(purr_str a, float v, int32_t format);
purr_str purr_str_add_bool(purr_str a, bool v);
purr_str purr_str_add_entity(purr_str a, purr_entity e, bool local);
purr_str purr_str_add_player(purr_str a, purr_player_id p);
purr_str purr_str_add_i2(purr_str a, purr_int2 v, int32_t format);
purr_str purr_str_add_i3(purr_str a, purr_int3 v, int32_t format);
purr_str purr_str_add_i4(purr_str a, purr_int4 v, int32_t format);
purr_str purr_str_add_f2(purr_str a, purr_float2 v, int32_t format);
purr_str purr_str_add_f3(purr_str a, purr_float3 v, int32_t format);
purr_str purr_str_add_f4(purr_str a, purr_float4 v, int32_t format);
purr_str purr_str_add_q(purr_str a, purr_quaternion v, int32_t format);
purr_str purr_str_add_color(purr_str a, purr_color v, int32_t format);
purr_str purr_str_add_rect(purr_str a, purr_rect v, int32_t format);

// Text read from C: `s` measured, not copied.
purr_str purr_str_from_cstr(const char *s);

// Text for C: `s` itself when a NUL follows it, or a copy that ends in one.
const char *purr_str_c(purr_str s);

// Comparing and searching, byte by byte. Positions and lengths count
// characters; out of range, they're clamped, as PurrLang is forgiving.
bool purr_str_eq(purr_str a, purr_str b);
bool purr_str_contains(purr_str a, purr_str b);
bool purr_str_starts_with(purr_str a, purr_str b);
bool purr_str_ends_with(purr_str a, purr_str b);
int32_t purr_str_index_of(purr_str a, purr_str b); // -1 if it's not there
purr_str purr_str_substring(purr_str a, int32_t start, int32_t length);
purr_str purr_str_substring_from(purr_str a, int32_t start);
purr_str purr_str_to_upper(purr_str a); // ASCII letters only
purr_str purr_str_to_lower(purr_str a);
purr_str purr_str_trim(purr_str a);
purr_str purr_str_replace(purr_str a, purr_str from, purr_str to);

// Counts the characters of UTF-8 text.
int32_t purr_utf8_chars(const char *bytes, int32_t count);

// ---------------------------------------------------------------------------
// Text in fields: of components, singletons, structs and events.
//
// A purr_text is where a field's text is: in a world's heap, or in the
// scratch area, which the top two bits say. It holds no pointer, so a world
// is the same bytes on every machine.
//
// Text in memory that's part of a world (its components, singletons and
// command queue) is the world's own: it has a block in the world's heap,
// which goes back when the text changes or leaves the world. Text anywhere
// else, like a struct copied into a local, only borrows: it points at the
// heap text it was copied from, which stays until the running code is done,
// or at a copy in the scratch area. Which is which comes from the address
// being written, so a function changing a `mut` struct works the same on a
// component's field and on a local.
//
// Heap text never changes once it's written: changing a field writes new text
// and releases the old, so a copy made before still reads the old text.

typedef struct purr_text {
    uint32_t at; // 0: empty. Otherwise the block's offset, and where in the top two bits
} purr_text;

// The worlds generated code runs with, so text knows whose memory it's in and
// can find its heap: the match (or NULL), and the local world (or NULL).
void purr_text_use(purr_heap *match_heap, const void *match, size_t match_size, purr_heap *local_heap,
                   const void *local, size_t local_size);

// Reading: a view of the text, which lasts until the running code is done.
purr_str purr_text_view(purr_text t);

// A world's own text, read from outside, like a host reading a component.
purr_str purr_text_read(const purr_heap *heap, purr_text t);

// Writing a field: its own copy of `value` if the field is part of a world
// (the old text is released), or a borrowed one.
void purr_text_set(purr_text *field, purr_str value);

// A value just copied into a world, such as a spawn's components going into
// the command queue: its borrowed text becomes the world's own copy.
void purr_text_own(purr_text *field);

// A world's own text leaving it, as its entity goes: freed once the running
// code is done.
void purr_text_release(purr_text *field);

// Text for a value being built, like a struct literal's field: a borrowed
// copy in the scratch area.
purr_text purr_text_temp(purr_str value);

// For purr/list.h, which keeps its blocks the way text does. Where a block is
// goes in the top two bits of its offset.
#define PURR_IN_MATCH 0u
#define PURR_IN_LOCAL 1u
#define PURR_IN_SCRATCH 2u

// The heap of the world whose memory `p` is in, and PURR_IN_MATCH or
// PURR_IN_LOCAL in `where`; NULL for memory that's in neither.
purr_heap *purr_heap_of(const void *p, uint32_t *where);

// A block in the scratch area with room for `bytes` after its header, 16-byte
// aligned: its header, or NULL when the area is full. `at` gets its tagged
// offset.
purr_block *purr_scratch_block(uint32_t bytes, uint32_t *at);

// A tagged offset's block, or NULL for 0.
purr_block *purr_block_at(uint32_t at);

// A `mut string` parameter: the caller's text, a field's or a local's, which
// the function reads and changes.
typedef struct purr_textref {
    purr_text *field; // A field's text, or NULL
    purr_str *local;  // Or a local's
} purr_textref;

static inline purr_str purr_textref_get(const purr_textref r)
{
    return r.field ? purr_text_view(*r.field) : *r.local;
}

static inline void purr_textref_set(const purr_textref r, const purr_str value)
{
    if (r.field) purr_text_set(r.field, value);
    else *r.local = value;
}
