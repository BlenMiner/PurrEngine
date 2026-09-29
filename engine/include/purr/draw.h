#pragma once

#include <stdint.h>

#include "purr/color.h"
#include "purr/math.h"

// Immediate-mode drawing: PurrLang's Draw API records commands into a draw
// list, and a renderer plays them back (purr_platform_draw, on raylib). The list
// knows nothing about any renderer, so replacing one doesn't touch game code.
//
// Temporary implementation written by Claude; the project owner takes it over
// later. Generated code calls the purr_draw_* functions below.
//
// Positions and sizes are in world units, with y up. The camera decides where
// they land on screen. Commands draw in the order they were recorded, on a
// frame that starts black. After a PURR_DRAW_GUI command, they're the GUI's
// instead: in units of a screen 1080 tall, from the top left with y down.

#ifndef PURR_DRAW_MAX_COMMANDS
#define PURR_DRAW_MAX_COMMANDS 16384u
#endif

#ifndef PURR_DRAW_TEXT_BYTES
#define PURR_DRAW_TEXT_BYTES 65536u
#endif

typedef enum purr_draw_kind {
    PURR_DRAW_CLEAR,       // color
    PURR_DRAW_CAMERA,      // a = center, b.x = size
    PURR_DRAW_CIRCLE,      // a = center, b.x = radius, color
    PURR_DRAW_WIRE_CIRCLE, // a = center, b.x = radius, color
    PURR_DRAW_RECT,        // a = center, b = size, color
    PURR_DRAW_WIRE_RECT,   // a = center, b = size, color
    PURR_DRAW_LINE,        // a = from, b = to, color
    PURR_DRAW_TEXT,        // a = top left corner, b.x = height, color, text
    PURR_DRAW_GUI,         // The commands after it are the GUI's (see purr/gui.h)
} purr_draw_kind;

typedef struct purr_draw_command {
    uint32_t kind; // purr_draw_kind
    uint32_t text; // PURR_DRAW_TEXT: offset of the text in purr_draw_list.text
    purr_float2 a;
    purr_float2 b;
    purr_color color;
} purr_draw_command;

// One frame's commands. Not simulation state: it lives outside the world.
typedef struct purr_draw_list {
    uint32_t count;
    uint32_t text_used;
    uint32_t dropped; // Commands that didn't fit this frame
    purr_draw_command commands[PURR_DRAW_MAX_COMMANDS];
    char text[PURR_DRAW_TEXT_BYTES];
} purr_draw_list;

// Empties the list for a new frame.
void purr_draw_reset(purr_draw_list *d);

// Fills the whole screen.
void purr_draw_clear(purr_draw_list *d, purr_color color);

// Sets the camera for the commands after it: `center` is the world position
// at the middle of the screen, and `size` is half the visible height, like
// Unity's orthographic size. Each frame starts with the camera at the origin
// and 1 world unit per pixel.
void purr_draw_camera(purr_draw_list *d, purr_float2 center, float size);

void purr_draw_circle(purr_draw_list *d, purr_float2 center, float radius, purr_color color);
void purr_draw_wire_circle(purr_draw_list *d, purr_float2 center, float radius, purr_color color);
void purr_draw_rect(purr_draw_list *d, purr_float2 center, purr_float2 size, purr_color color);
void purr_draw_wire_rect(purr_draw_list *d, purr_float2 center, purr_float2 size, purr_color color);
void purr_draw_line(purr_draw_list *d, purr_float2 from, purr_float2 to, purr_color color);

// `position` is the text's top left corner and `size` its height. The text is
// copied into the list.
void purr_draw_text(purr_draw_list *d, const char *text, purr_float2 position, float size, purr_color color);

// The commands after it are in GUI units: a screen 1080 units tall, whose
// width follows the window's shape, from the top left with y down.
void purr_draw_gui(purr_draw_list *d);

// Adds `from`'s commands to the end of `d`, such as the GUI's over the world.
void purr_draw_append(purr_draw_list *d, const purr_draw_list *from);
