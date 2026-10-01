#pragma once

#include <stdint.h>

#include "tide/color.h"
#include "tide/math.h"

// Immediate-mode drawing: Tide's Draw API records commands into a draw
// list, and a renderer plays them back (tide_platform_draw, on raylib). The list
// knows nothing about any renderer, so replacing one doesn't touch game code.
//
// Temporary implementation written by Claude; the project owner takes it over
// later. Generated code calls the tide_draw_* functions below.
//
// Positions and sizes are in world units, with y up. The camera decides where
// they land on screen. Commands draw in the order they were recorded, on a
// frame that starts black. After a TIDE_DRAW_GUI command, they're the GUI's
// instead: in pixels, from the top left with y down.

#ifndef TIDE_DRAW_MAX_COMMANDS
#define TIDE_DRAW_MAX_COMMANDS 16384u
#endif

#ifndef TIDE_DRAW_TEXT_BYTES
#define TIDE_DRAW_TEXT_BYTES 65536u
#endif

typedef enum tide_draw_kind {
    TIDE_DRAW_CLEAR,       // color
    TIDE_DRAW_CAMERA,      // a = center, b.x = size
    TIDE_DRAW_CIRCLE,      // a = center, b.x = radius, color
    TIDE_DRAW_WIRE_CIRCLE, // a = center, b.x = radius, color
    TIDE_DRAW_RECT,        // a = center, b = size, color
    TIDE_DRAW_WIRE_RECT,   // a = center, b = size, color
    TIDE_DRAW_LINE,        // a = from, b = to, color
    TIDE_DRAW_TEXT,        // a = top left corner, b.x = height, color, text
    TIDE_DRAW_GUI,         // The commands after it are the GUI's (see tide/gui.h)
} tide_draw_kind;

typedef struct tide_draw_command {
    uint32_t kind; // tide_draw_kind
    uint32_t text; // TIDE_DRAW_TEXT: offset of the text in tide_draw_list.text
    tide_float2 a;
    tide_float2 b;
    tide_color color;
} tide_draw_command;

// One frame's commands. Not simulation state: it lives outside the world.
typedef struct tide_draw_list {
    uint32_t count;
    uint32_t text_used;
    uint32_t dropped; // Commands that didn't fit this frame
    tide_draw_command commands[TIDE_DRAW_MAX_COMMANDS];
    char text[TIDE_DRAW_TEXT_BYTES];
} tide_draw_list;

// Empties the list for a new frame.
void tide_draw_reset(tide_draw_list *d);

// Fills the whole screen.
void tide_draw_clear(tide_draw_list *d, tide_color color);

// Sets the camera for the commands after it: `center` is the world position
// at the middle of the screen, and `size` is half the visible height, like
// Unity's orthographic size. Each frame starts with the camera at the origin
// and 1 world unit per pixel.
void tide_draw_camera(tide_draw_list *d, tide_float2 center, float size);

void tide_draw_circle(tide_draw_list *d, tide_float2 center, float radius, tide_color color);
void tide_draw_wire_circle(tide_draw_list *d, tide_float2 center, float radius, tide_color color);
void tide_draw_rect(tide_draw_list *d, tide_float2 center, tide_float2 size, tide_color color);
void tide_draw_wire_rect(tide_draw_list *d, tide_float2 center, tide_float2 size, tide_color color);
void tide_draw_line(tide_draw_list *d, tide_float2 from, tide_float2 to, tide_color color);

// `position` is the text's top left corner and `size` its height. The text is
// copied into the list.
void tide_draw_text(tide_draw_list *d, const char *text, tide_float2 position, float size, tide_color color);

// The commands after it are the GUI's: in pixels, from the top left with y
// down.
void tide_draw_gui(tide_draw_list *d);

// Adds `from`'s commands to the end of `d`, such as the GUI's over the world.
void tide_draw_append(tide_draw_list *d, const tide_draw_list *from);
