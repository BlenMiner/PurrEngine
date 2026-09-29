#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "purr/color.h"
#include "purr/devices.h"
#include "purr/draw.h"
#include "purr/math.h"

typedef struct purr_textref purr_textref;

// Immediate-mode GUI, as PurrLang's GUI and GUILayout see it: widgets are
// function calls made every frame from views, which draw themselves and say
// what the player did with them. Nothing is kept between frames but which
// widget is hovered, pressed, focused or being typed into.
//
// Temporary implementation written by Claude; the project owner takes it over
// later. Generated code calls the purr_gui_* functions below.
//
// Positions and sizes are in units of a screen 1080 tall, whose width follows
// the window's shape, from the top left with y down, as in Unity's GUI. A GUI
// laid out once fits every window.
//
// A frame of a host:
//
//     purr_draw_reset(&draw);
//     purr_gui_begin(&gui, &devices, purr_platform_screen_size(), purr_platform_measure_text);
//     purr_frame(match, previous, alpha, &local, &draw, &gui);
//     purr_gui_end(&gui, &draw); // The GUI's commands go last, over the world
//     purr_platform_draw(&draw);
//
// and before sampling input, it hides what the GUI is using from the game:
//
//     purr_devices sampled = devices;
//     purr_gui_hide(&gui, &sampled);
//     const purr_input input = purr_input_sample(&sampled, &local);
//
// Views read the devices once per frame (`Devices` in PurrLang), through the
// GUI: purr_gui_begin works out what changed since last frame, and hides what
// the GUI is using from them too.
//
// A modal (GUILayout.Modal) takes the whole screen while it's up: the widgets
// outside it stop working, the focus goes to its first widget, the game and
// the views get nothing from the devices, and back (Escape, or the east
// button) closes it.
// Widgets have IDs, which generated code derives from where they're called
// (see purr_gui_id). Keyboard and gamepad navigation are built in: Tab, the
// arrows or the d-pad move the focus between widgets, Enter, Space or the south
// button press, Escape or the east button let go.

#define PURR_GUI_SCREEN_HEIGHT 1080.0f

#ifndef PURR_GUI_MAX_DEPTH
#define PURR_GUI_MAX_DEPTH 32 // Containers open at once
#endif
#ifndef PURR_GUI_MAX_NAV
#define PURR_GUI_MAX_NAV 512 // Widgets the focus can move between, per frame
#endif
#ifndef PURR_GUI_MAX_IDS
#define PURR_GUI_MAX_IDS 2048 // Distinct call sites per frame; a power of two
#endif
#ifndef PURR_GUI_MAX_SIZES
#define PURR_GUI_MAX_SIZES 256 // Containers whose size is remembered; a power of two
#endif

// A rectangle: its top left corner and its size.
typedef struct purr_rect {
    float x, y, width, height;
} purr_rect;

// Where GUILayout.Area puts an area, named as Unity's TextAnchor. PurrLang's
// Anchor enum has the same values.
typedef enum purr_anchor {
    PURR_ANCHOR_UPPER_LEFT,
    PURR_ANCHOR_UPPER_CENTER,
    PURR_ANCHOR_UPPER_RIGHT,
    PURR_ANCHOR_MIDDLE_LEFT,
    PURR_ANCHOR_MIDDLE_CENTER,
    PURR_ANCHOR_MIDDLE_RIGHT,
    PURR_ANCHOR_LOWER_LEFT,
    PURR_ANCHOR_LOWER_CENTER,
    PURR_ANCHOR_LOWER_RIGHT,
} purr_anchor;

// The width of `text` drawn `size` tall, in the same units. The platform
// measures with its font (purr_platform_measure_text); NULL guesses.
typedef float (*purr_measure_fn)(const char *text, float size);

typedef struct purr_gui_group {
    uint32_t id;
    uint32_t kind;            // Vertical, horizontal or area
    purr_float2 origin;       // Where its content starts
    purr_float2 cursor;       // Where the next widget goes
    purr_float2 size;         // Its content's size so far
    float natural;            // Its widest widget before stretching
    float stretch;            // In a vertical group: the width widgets stretch to, from last frame
    purr_float2 guess;        // An anchored area: its top left, from last frame's size
    int32_t anchor;           // An area: its purr_anchor, or -1 when it's at a rect
    uint32_t panel;           // An area: its background's command
    purr_rect rect;           // An area at a rect: the rect
    uint32_t hot_before;      // An area: what was under the mouse before its content
    uint32_t in_modal_before; // The modal it's in, back when it closes
} purr_gui_group;

// A container's size last frame, which lays out the next.
typedef struct purr_gui_size {
    uint32_t id;
    uint32_t frame;
    purr_float2 size;
    float natural;
} purr_gui_size;

// How many widgets came from one call site this frame, which tells apart the
// ones a loop makes.
typedef struct purr_gui_seen {
    uint32_t key;
    uint32_t frame;
    uint32_t count;
} purr_gui_seen;

typedef struct purr_gui {
    // The screen this frame, in GUI units (Screen in PurrLang).
    float width;
    float height;
    float scale; // Pixels per unit

    // Input this frame
    purr_measure_fn measure;
    purr_float2 mouse;
    bool mouse_held, mouse_pressed, mouse_released;
    bool mouse_was_held;
    uint32_t keys;         // Navigation keys held (see gui.c)
    uint32_t keys_pressed; // ...that went down this frame
    purr_typed text;
    bool game_input; // purr_gui_hide ran since the last frame: the game reads the devices
    bool nav_arrows; // The arrows and d-pad can move the focus this frame
    bool back;       // Escape or the east button went down, and didn't stop typing
    purr_devices devices; // As views read them: what changed since last frame, less what the GUI uses
    purr_devices last;    // The devices last frame, as the platform had them

    // Kept between frames
    uint32_t frame;
    uint32_t hot, hot_next;           // The widget under the mouse, drawn last of those there
    purr_rect hot_rect, hot_rect_next; // ...and where it is
    uint32_t active;                  // The widget the mouse is pressing
    bool active_seen, claimed;
    uint32_t focus;                   // The widget the keyboard and gamepad are on
    bool focus_seen;
    uint32_t editing;                 // The field being typed into
    bool editing_seen;
    uint32_t edit_len;
    char edit[256];                   // What's typed so far
    bool edit_fresh;                  // Nothing typed yet: the first character replaces the value
    bool over, over_next;             // The mouse is over the GUI
    uint32_t modal, modal_next;       // The modal on top, last frame's and this frame's
    uint32_t in_modal;                // The modal whose content is being drawn
    uint32_t grab;                    // A modal just came on top: its first widget takes the focus
    uint32_t nav[PURR_GUI_MAX_NAV];   // This frame's widgets, in order
    uint32_t nav_count;
    uint32_t nav_last[PURR_GUI_MAX_NAV]; // Last frame's
    uint32_t nav_last_count;

    purr_gui_group groups[PURR_GUI_MAX_DEPTH + 1]; // [0] is the screen's own
    int depth;
    purr_gui_size sizes[PURR_GUI_MAX_SIZES];
    purr_gui_seen seen[PURR_GUI_MAX_IDS];
    purr_draw_list list; // This frame's commands, drawn over the world at the end
} purr_gui;

// Starts a frame. `screen` is the window's size in pixels. A zeroed purr_gui
// is ready to use.
void purr_gui_begin(purr_gui *g, const purr_devices *devices, purr_float2 screen, purr_measure_fn measure);

// Ends the frame: closes what's still open and adds the GUI's commands to the
// end of `draw`, over everything else.
void purr_gui_end(purr_gui *g, purr_draw_list *draw);

// Hides what the GUI is using from `devices`, a copy about to be sampled as
// the game's input: the keyboard and gamepad while a widget has the focus,
// the mouse's buttons while it's over the GUI or pressing a widget, and
// everything while a modal is up.
void purr_gui_hide(purr_gui *g, purr_devices *devices);

// ---------------------------------------------------------------------------
// For generated code

// Mixes two numbers into an ID. Never 0, which means no widget.
uint32_t purr_gui_seed(uint32_t a, uint32_t b);

// A widget's ID: `site` is where it's called from, and `seed` tells apart the
// entities a view runs for and the places a function is called from. Calls
// from the same site with the same seed, as in a loop, count up.
uint32_t purr_gui_id(purr_gui *g, uint32_t seed, uint32_t site);

// Widgets at a rect (GUI). The ones with a value change it, and return whether
// they did; a button returns whether it was pressed.
void purr_gui_label(purr_gui *g, purr_rect rect, const char *text);
bool purr_gui_button(purr_gui *g, uint32_t id, purr_rect rect, const char *text);
bool purr_gui_toggle(purr_gui *g, uint32_t id, purr_rect rect, const char *text, bool *value);
bool purr_gui_slider(purr_gui *g, uint32_t id, purr_rect rect, const char *label, float *value, float min, float max);
bool purr_gui_int_slider(purr_gui *g, uint32_t id, purr_rect rect, const char *label, int32_t *value, int32_t min,
                         int32_t max);
bool purr_gui_int_field(purr_gui *g, uint32_t id, purr_rect rect, const char *label, int32_t *value);
bool purr_gui_float_field(purr_gui *g, uint32_t id, purr_rect rect, const char *label, float *value);
bool purr_gui_float2_field(purr_gui *g, uint32_t id, purr_rect rect, const char *label, purr_float2 *value);
bool purr_gui_float3_field(purr_gui *g, uint32_t id, purr_rect rect, const char *label, purr_float3 *value);
bool purr_gui_float4_field(purr_gui *g, uint32_t id, purr_rect rect, const char *label, purr_float4 *value);
bool purr_gui_color_field(purr_gui *g, uint32_t id, purr_rect rect, const char *label, purr_color *value);
bool purr_gui_text_field(purr_gui *g, uint32_t id, purr_rect rect, const char *label, purr_textref value);

// The same, laid out automatically (GUILayout).
void purr_gui_layout_label(purr_gui *g, const char *text);
bool purr_gui_layout_button(purr_gui *g, uint32_t id, const char *text);
bool purr_gui_layout_toggle(purr_gui *g, uint32_t id, const char *text, bool *value);
bool purr_gui_layout_slider(purr_gui *g, uint32_t id, const char *label, float *value, float min, float max);
bool purr_gui_layout_int_slider(purr_gui *g, uint32_t id, const char *label, int32_t *value, int32_t min, int32_t max);
bool purr_gui_layout_int_field(purr_gui *g, uint32_t id, const char *label, int32_t *value);
bool purr_gui_layout_float_field(purr_gui *g, uint32_t id, const char *label, float *value);
bool purr_gui_layout_float2_field(purr_gui *g, uint32_t id, const char *label, purr_float2 *value);
bool purr_gui_layout_float3_field(purr_gui *g, uint32_t id, const char *label, purr_float3 *value);
bool purr_gui_layout_float4_field(purr_gui *g, uint32_t id, const char *label, purr_float4 *value);
bool purr_gui_layout_color_field(purr_gui *g, uint32_t id, const char *label, purr_color *value);
bool purr_gui_layout_text_field(purr_gui *g, uint32_t id, const char *label, purr_textref value);
void purr_gui_layout_space(purr_gui *g, float size);

// Containers. Each returns how many were open before it, which the matching
// purr_gui_close takes: it closes this one and any left open inside it.
int purr_gui_begin_vertical(purr_gui *g, uint32_t id);
int purr_gui_begin_horizontal(purr_gui *g, uint32_t id);
int purr_gui_begin_area(purr_gui *g, uint32_t id, purr_rect rect);
int purr_gui_begin_area_at(purr_gui *g, uint32_t id, int32_t anchor);
// A modal: an area at an anchor over a dimmed screen, while `open` is true.
// Back closes it, setting `open` to false. Returns -1 when it isn't up: then
// its content doesn't run, and there's nothing to close.
int purr_gui_begin_modal(purr_gui *g, uint32_t id, int32_t anchor, bool *open);
void purr_gui_close(purr_gui *g, int depth);
