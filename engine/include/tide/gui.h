#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "tide/color.h"
#include "tide/devices.h"
#include "tide/draw.h"
#include "tide/math.h"

typedef struct tide_textref tide_textref;

// Immediate-mode GUI, as Tide's GUI and GUILayout see it: widgets are
// function calls made every frame from views, which draw themselves and say
// what the player did with them. Nothing is kept between frames but which
// widget is hovered, pressed, focused or being typed into.
//
// Temporary implementation written by Claude; the project owner takes it over
// later. Generated code calls the tide_gui_* functions below.
//
// Positions and sizes are in the window's pixels, from the top left with y
// down, as in Unity's GUI. Widgets keep their size when the window changes
// size, as a web page's do, until there isn't room for them: then laid out
// widgets shrink, as far as each can, to fit the width they have.
//
// A frame of a host:
//
//     tide_draw_reset(&draw);
//     tide_gui_begin(&gui, &devices, tide_platform_screen_size(), tide_platform_measure_text);
//     tide_frame(match, previous, alpha, &local, &draw, &gui);
//     tide_gui_end(&gui, &draw); // The GUI's commands go last, over the world
//     tide_platform_draw(&draw);
//
// and before sampling input, it hides what the GUI is using from the game:
//
//     tide_devices sampled = devices;
//     tide_gui_hide(&gui, &sampled);
//     const tide_input input = tide_input_sample(&sampled, &local);
//
// Views read the devices once per frame (`Devices` in Tide), through the
// GUI: tide_gui_begin works out what changed since last frame, and hides what
// the GUI is using from them too.
//
// A modal (GUILayout.Modal) takes the whole screen while it's up: the widgets
// outside it stop working, the focus goes to its first widget, the game and
// the views get nothing from the devices, and back (Escape, or the east
// button) closes it.
//
// Widgets in a Disabled block (GUI.Disabled) are drawn faded and don't work,
// as with Unity's GUI.enabled: a menu stays up, grayed out, while it waits.
//
// Widgets have IDs, which generated code derives from where they're called
// (see tide_gui_id). Keyboard and gamepad navigation are built in: Tab, the
// arrows or the d-pad move the focus between widgets, Enter, Space or the south
// button press, Escape or the east button let go.

#ifndef TIDE_GUI_MAX_DEPTH
#define TIDE_GUI_MAX_DEPTH 32 // Containers and Disabled blocks open at once
#endif
#ifndef TIDE_GUI_MAX_NAV
#define TIDE_GUI_MAX_NAV 512 // Widgets the focus can move between, per frame
#endif
#ifndef TIDE_GUI_MAX_IDS
#define TIDE_GUI_MAX_IDS 2048 // Distinct call sites per frame; a power of two
#endif
#ifndef TIDE_GUI_MAX_SIZES
#define TIDE_GUI_MAX_SIZES 256 // Containers whose size is remembered; a power of two
#endif

// Where GUILayout.Area puts an area, named as Unity's TextAnchor. Tide's
// Anchor enum has the same values.
typedef enum tide_anchor {
    TIDE_ANCHOR_UPPER_LEFT,
    TIDE_ANCHOR_UPPER_CENTER,
    TIDE_ANCHOR_UPPER_RIGHT,
    TIDE_ANCHOR_MIDDLE_LEFT,
    TIDE_ANCHOR_MIDDLE_CENTER,
    TIDE_ANCHOR_MIDDLE_RIGHT,
    TIDE_ANCHOR_LOWER_LEFT,
    TIDE_ANCHOR_LOWER_CENTER,
    TIDE_ANCHOR_LOWER_RIGHT,
} tide_anchor;

// The width of `text` drawn `size` tall, in the same units. The platform
// measures with its font (tide_platform_measure_text); NULL guesses.
typedef float (*tide_measure_fn)(const char *text, float size);

typedef struct tide_gui_group {
    uint32_t id;
    uint32_t kind;            // Vertical, horizontal or area
    tide_float2 origin;       // Where its content starts
    tide_float2 cursor;       // Where the next widget goes
    tide_float2 size;         // Its content's size so far
    float natural;            // Its content's width with room: its widest widget, or a row's widgets and spacing
    float least;              // ...and squeezed as far as it goes
    float room;               // The width its content has
    float squeeze;            // A row: how far its widgets shrink from natural to least (0 to 1), from last frame
    float stretch;            // In a vertical group: the width widgets stretch to, from last frame
    tide_float2 guess;        // An anchored area: its top left, from last frame's size
    int32_t anchor;           // An area: its tide_anchor, or -1 when it's at a rect
    uint32_t panel;           // An area: its background's command
    tide_rect rect;           // An area at a rect: the rect
    uint32_t hot_before;      // An area: what was under the mouse before its content
    uint32_t in_modal_before; // The modal it's in, back when it closes
    bool disabled;            // Its widgets are grayed out and don't work: it's in a Disabled block
    bool scope;               // A Disabled block: the container around it, carried on, and handed back when it closes
} tide_gui_group;

// A container's size last frame, which lays out the next.
typedef struct tide_gui_size {
    uint32_t id;
    uint32_t frame;
    tide_float2 size;
    float natural;
    float least;
} tide_gui_size;

// How many widgets came from one call site this frame, which tells apart the
// ones a loop makes.
typedef struct tide_gui_seen {
    uint32_t key;
    uint32_t frame;
    uint32_t count;
} tide_gui_seen;

typedef struct tide_gui {
    // The screen this frame (Screen in Tide).
    float width;
    float height;

    // Input this frame
    tide_measure_fn measure;
    tide_float2 mouse;
    bool mouse_held, mouse_pressed, mouse_released;
    bool mouse_was_held;
    uint32_t keys;         // Navigation keys held (see gui.c)
    uint32_t keys_pressed; // ...that went down this frame
    tide_typed text;
    bool game_input; // tide_gui_hide ran since the last frame: the game reads the devices
    bool nav_arrows; // The arrows and d-pad can move the focus this frame
    bool back;       // Escape or the east button went down, and didn't stop typing
    tide_devices devices; // As views read them: what changed since last frame, less what the GUI uses
    tide_devices last;    // The devices last frame, as the platform had them

    // Kept between frames
    uint32_t frame;
    uint32_t hot, hot_next;           // The widget under the mouse, drawn last of those there
    tide_rect hot_rect, hot_rect_next; // ...and where it is
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
    uint32_t nav[TIDE_GUI_MAX_NAV];   // This frame's widgets, in order
    uint32_t nav_count;
    uint32_t nav_last[TIDE_GUI_MAX_NAV]; // Last frame's
    uint32_t nav_last_count;

    tide_gui_group groups[TIDE_GUI_MAX_DEPTH + 1]; // [0] is the screen's own
    int depth;
    tide_gui_size sizes[TIDE_GUI_MAX_SIZES];
    tide_gui_seen seen[TIDE_GUI_MAX_IDS];
    tide_draw_list list; // This frame's commands, drawn over the world at the end
} tide_gui;

// Starts a frame. `screen` is the window's size in pixels. A zeroed tide_gui
// is ready to use.
void tide_gui_begin(tide_gui *g, const tide_devices *devices, tide_float2 screen, tide_measure_fn measure);

// Ends the frame: closes what's still open and adds the GUI's commands to the
// end of `draw`, over everything else.
void tide_gui_end(tide_gui *g, tide_draw_list *draw);

// Whether the player is typing into a field: phones show their keyboard
// meanwhile (tide_platform_typing).
bool tide_gui_typing(const tide_gui *g);

// Hides what the GUI is using from `devices`, a copy about to be sampled as
// the game's input: the keyboard and gamepad while a widget has the focus,
// the mouse's buttons and the primary touch while the pointer is over the GUI
// or pressing a widget, and everything while a modal is up.
void tide_gui_hide(tide_gui *g, tide_devices *devices);

// ---------------------------------------------------------------------------
// For generated code

// Mixes two numbers into an ID. Never 0, which means no widget.
uint32_t tide_gui_seed(uint32_t a, uint32_t b);

// A widget's ID: `site` is where it's called from, and `seed` tells apart the
// entities a view runs for and the places a function is called from. Calls
// from the same site with the same seed, as in a loop, count up.
uint32_t tide_gui_id(tide_gui *g, uint32_t seed, uint32_t site);

// Widgets at a rect (GUI). The ones with a value change it, and return whether
// they did; a button returns whether it was pressed.
void tide_gui_label(tide_gui *g, tide_rect rect, const char *text);
bool tide_gui_button(tide_gui *g, uint32_t id, tide_rect rect, const char *text);
bool tide_gui_toggle(tide_gui *g, uint32_t id, tide_rect rect, const char *text, bool *value);
bool tide_gui_slider(tide_gui *g, uint32_t id, tide_rect rect, const char *label, float *value, float min, float max);
bool tide_gui_int_slider(tide_gui *g, uint32_t id, tide_rect rect, const char *label, int32_t *value, int32_t min,
                         int32_t max);
bool tide_gui_int_field(tide_gui *g, uint32_t id, tide_rect rect, const char *label, int32_t *value);
bool tide_gui_float_field(tide_gui *g, uint32_t id, tide_rect rect, const char *label, float *value);
bool tide_gui_float2_field(tide_gui *g, uint32_t id, tide_rect rect, const char *label, tide_float2 *value);
bool tide_gui_float3_field(tide_gui *g, uint32_t id, tide_rect rect, const char *label, tide_float3 *value);
bool tide_gui_float4_field(tide_gui *g, uint32_t id, tide_rect rect, const char *label, tide_float4 *value);
bool tide_gui_color_field(tide_gui *g, uint32_t id, tide_rect rect, const char *label, tide_color *value);
bool tide_gui_text_field(tide_gui *g, uint32_t id, tide_rect rect, const char *label, tide_textref value);

// The same, laid out automatically (GUILayout).
void tide_gui_layout_label(tide_gui *g, const char *text);
bool tide_gui_layout_button(tide_gui *g, uint32_t id, const char *text);
bool tide_gui_layout_toggle(tide_gui *g, uint32_t id, const char *text, bool *value);
bool tide_gui_layout_slider(tide_gui *g, uint32_t id, const char *label, float *value, float min, float max);
bool tide_gui_layout_int_slider(tide_gui *g, uint32_t id, const char *label, int32_t *value, int32_t min, int32_t max);
bool tide_gui_layout_int_field(tide_gui *g, uint32_t id, const char *label, int32_t *value);
bool tide_gui_layout_float_field(tide_gui *g, uint32_t id, const char *label, float *value);
bool tide_gui_layout_float2_field(tide_gui *g, uint32_t id, const char *label, tide_float2 *value);
bool tide_gui_layout_float3_field(tide_gui *g, uint32_t id, const char *label, tide_float3 *value);
bool tide_gui_layout_float4_field(tide_gui *g, uint32_t id, const char *label, tide_float4 *value);
bool tide_gui_layout_color_field(tide_gui *g, uint32_t id, const char *label, tide_color *value);
bool tide_gui_layout_text_field(tide_gui *g, uint32_t id, const char *label, tide_textref value);
void tide_gui_layout_space(tide_gui *g, float size);

// Containers. Each returns how many were open before it, which the matching
// tide_gui_close takes: it closes this one and any left open inside it.
int tide_gui_begin_vertical(tide_gui *g, uint32_t id);
int tide_gui_begin_horizontal(tide_gui *g, uint32_t id);
int tide_gui_begin_area(tide_gui *g, uint32_t id, tide_rect rect);
int tide_gui_begin_area_at(tide_gui *g, uint32_t id, int32_t anchor);
// A modal: an area at an anchor over a dimmed screen, while `open` is true.
// Back closes it, setting `open` to false. Returns -1 when it isn't up: then
// its content doesn't run, and there's nothing to close.
int tide_gui_begin_modal(tide_gui *g, uint32_t id, int32_t anchor, bool *open);
// Widgets until the matching close are grayed out and don't work while
// `disabled` is true: they can't be hovered, pressed, focused or typed into,
// and let go of what they had. It lays out nothing: its widgets go on in the
// container around it. Inside another, it stays disabled whatever `disabled` is.
int tide_gui_begin_disabled(tide_gui *g, bool disabled);
void tide_gui_close(tide_gui *g, int depth);
