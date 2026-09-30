#pragma once

#include <stdbool.h>
#include <stdint.h>

// A game's data layout, for hot reloading under `purr run` (see purr/host.h):
// its types, and where each world keeps what, by name. With it, a world can be
// carried over to a build of the game whose layout changed. purrc only writes
// it when asked (codegen_options.layout), so games purr builds don't have it.
//
// Temporary implementation written by Claude; the project owner takes it over
// later.

// How a field carries over to a field of the same name.
typedef enum purr_layout_kind {
    PURR_LAYOUT_PLAIN,  // As it is, to a field of the same type
    PURR_LAYOUT_INT,    // int and int vectors (`dim` numbers): also to floats, as ints convert
    PURR_LAYOUT_FLOAT,  // float and float vectors
    PURR_LAYOUT_ENUM,   // An enum's member (`decl`), by its name
    PURR_LAYOUT_STRUCT, // Another type (`decl`), field by field
} purr_layout_kind;

typedef struct purr_layout_field {
    const char *name;
    const char *type; // Its type's name
    uint32_t kind;    // purr_layout_kind
    uint32_t dim;     // PURR_LAYOUT_INT and PURR_LAYOUT_FLOAT: how many numbers
    int32_t decl;     // PURR_LAYOUT_STRUCT: a type; PURR_LAYOUT_ENUM: an enum
    uint32_t offset;
    uint32_t size;
} purr_layout_field;

// A component, singleton, input or struct.
typedef struct purr_layout_type {
    const char *name;
    uint32_t size;
    bool scene; // A scene's component: its entity is the scene
    uint32_t field_count;
    const purr_layout_field *fields;
    void (*defaults)(void *value); // A value with every field's declared default; text and lists empty
} purr_layout_type;

typedef struct purr_layout_member {
    const char *name;
    int32_t value;
} purr_layout_member;

typedef struct purr_layout_enum {
    const char *name;
    uint32_t member_count;
    const purr_layout_member *members;
} purr_layout_enum;

// Where a singleton, or a component's column in an archetype, is.
typedef struct purr_layout_place {
    int32_t type;
    uint32_t offset;
} purr_layout_place;

typedef struct purr_layout_archetype {
    uint32_t offset;       // In the world
    uint32_t count;        // Offsets in the archetype: its row count,
    uint32_t entities;     // ...its entity column,
    uint32_t scenes;       // ...and the scene each row is in, or UINT32_MAX if its world has no scenes
    uint32_t component_count;
    const purr_layout_place *components;
} purr_layout_archetype;

// The match (purr_world) or the local world (purr_local).
typedef struct purr_layout_world {
    uint32_t size;
    uint32_t singleton_count;
    const purr_layout_place *singletons;
    uint32_t entities; // Offset of its purr_entities
    uint32_t archetype_count;
    const purr_layout_archetype *archetypes;
    uint32_t capacity;     // Rows each archetype has room for
    uint32_t commands;     // Offset of the number of changes waiting, 0 between ticks and frames
    int32_t input;         // The match: the input's type, or -1
    uint32_t inputs;       // ...its players' and the server's inputs,
    uint32_t previous;     // ...and last tick's
    uint32_t input_count;
    uint32_t heap;         // Offset of its purr_heap, or UINT32_MAX
} purr_layout_world;

typedef struct purr_layout {
    uint32_t type_count;
    const purr_layout_type *types;
    uint32_t enum_count;
    const purr_layout_enum *enums;
    purr_layout_world match;
    purr_layout_world local;
} purr_layout;
