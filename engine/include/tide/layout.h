#pragma once

#include <stdbool.h>
#include <stdint.h>

// A game's data layout, for hot reloading under `tide run` (see tide/host.h):
// its types, and where each world keeps what, by name. With it, a world can be
// carried over to a build of the game whose layout changed. tidec only writes
// it when asked (codegen_options.layout), so games tide builds don't have it.
//
// Temporary implementation written by Claude; the project owner takes it over
// later.

// How a field carries over to a field of the same name.
typedef enum tide_layout_kind {
    TIDE_LAYOUT_PLAIN,  // As it is, to a field of the same type
    TIDE_LAYOUT_INT,    // int and int vectors (`dim` numbers): also to floats, as ints convert
    TIDE_LAYOUT_FLOAT,  // float and float vectors
    TIDE_LAYOUT_ENUM,   // An enum's member (`decl`), by its name
    TIDE_LAYOUT_STRUCT, // Another type (`decl`), field by field
} tide_layout_kind;

typedef struct tide_layout_field {
    const char *name;
    const char *type; // Its type's name
    uint32_t kind;    // tide_layout_kind
    uint32_t dim;     // TIDE_LAYOUT_INT and TIDE_LAYOUT_FLOAT: how many numbers
    int32_t decl;     // TIDE_LAYOUT_STRUCT: a type; TIDE_LAYOUT_ENUM: an enum
    uint32_t offset;
    uint32_t size;
} tide_layout_field;

// A component, singleton, input or struct.
typedef struct tide_layout_type {
    const char *name;
    uint32_t size;
    bool scene; // A scene's component: its entity is the scene
    uint32_t field_count;
    const tide_layout_field *fields;
    void (*defaults)(void *value); // A value with every field's declared default; text and lists empty
} tide_layout_type;

typedef struct tide_layout_member {
    const char *name;
    int32_t value;
} tide_layout_member;

typedef struct tide_layout_enum {
    const char *name;
    uint32_t member_count;
    const tide_layout_member *members;
} tide_layout_enum;

// Where a singleton is in its world's struct, or which column of an
// archetype a component is.
typedef struct tide_layout_place {
    int32_t type;
    uint32_t offset;
} tide_layout_place;

// An archetype's columns (tide/table.h): the entity, the scene each is in
// (`scenes`), then its components.
typedef struct tide_layout_archetype {
    bool scenes;
    uint32_t component_count;
    const tide_layout_place *components; // Their columns, in order
} tide_layout_archetype;

// The match (tide_world) or the local world (tide_local), as its bytes are
// (tide_world_pack): its struct with the storage zeroed, then its entity
// table, its archetypes' tables in this order, its queue, and its heap.
typedef struct tide_layout_world {
    uint32_t size; // Of its struct
    uint32_t singleton_count;
    const tide_layout_place *singletons;
    uint32_t archetype_count;
    const tide_layout_archetype *archetypes;
    int32_t input;         // The match: the input's type, or -1
    uint32_t inputs;       // ...its players' and the server's inputs, in the struct,
    uint32_t previous;     // ...and last tick's
    uint32_t input_count;
    bool heap;             // It has a heap
} tide_layout_world;

typedef struct tide_layout {
    uint32_t type_count;
    const tide_layout_type *types;
    uint32_t enum_count;
    const tide_layout_enum *enums;
    tide_layout_world match;
    tide_layout_world local;
} tide_layout;
