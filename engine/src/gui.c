#include "tide/gui.h"

#include <stdio.h>

#include "tide/text.h"
#include <stdlib.h>
#include <string.h>

// See tide/gui.h. The GUI is local: it runs on one machine and never touches
// the simulation, so it's free to use the C library (formatting and parsing
// numbers) and to depend on the window's size.

// Sizes, in GUI units (pixels)
#define FONT 16.0f         // Text height
#define LINE 28.0f         // A widget's height
#define PAD 10.0f          // Text inside a button, from its sides
#define SPACING 5.0f       // Between widgets
#define AREA_PADDING 12.0f // Inside an area, around its content
#define AREA_MARGIN 12.0f  // Between an anchored area and the screen's edges
#define LABEL_WIDTH 130.0f // A labelled widget's label, at least
#define SLIDER_WIDTH 180.0f
#define VALUE_WIDTH 56.0f  // A slider's value, on its right
#define FIELD_WIDTH 90.0f
#define PART_WIDTH 68.0f   // Each number of a vector field
#define COLOR_PART_WIDTH 52.0f
#define BOX 16.0f          // A toggle's box
#define BORDER 2.0f        // The focus's outline

// Squeezed, when there isn't room: as narrow as these go
#define LEAST_FIELD 48.0f  // A number field, or half a text field
#define LEAST_TRACK 40.0f  // A slider's track
#define LEAST_PART 36.0f   // Each number of a vector or color field

static const tide_color TEXT = {0.93f, 0.93f, 0.95f, 1.0f};
static const tide_color PANEL = {0.07f, 0.07f, 0.1f, 0.9f};
static const tide_color CONTROL = {0.2f, 0.2f, 0.25f, 1.0f};
static const tide_color CONTROL_HOT = {0.28f, 0.28f, 0.34f, 1.0f};
static const tide_color CONTROL_DOWN = {0.14f, 0.14f, 0.18f, 1.0f};
static const tide_color FIELD = {0.12f, 0.12f, 0.15f, 1.0f};
static const tide_color FIELD_HOT = {0.16f, 0.16f, 0.2f, 1.0f};
static const tide_color ACCENT = {1.0f, 0.77f, 0.24f, 1.0f};
static const tide_color DIM = {0.0f, 0.0f, 0.0f, 0.5f}; // Over the screen, under a modal
static const float FADED = 0.5f; // How opaque a disabled widget is, as in Unity's IMGUI

enum { VERTICAL, HORIZONTAL, AREA };

#define ROOT_ID 1u // The screen's own group, where widgets outside any container go

// Navigation keys, in tide_gui.keys
enum {
    KEY_UP = 1u << 0,
    KEY_DOWN = 1u << 1,
    KEY_LEFT = 1u << 2,
    KEY_RIGHT = 1u << 3,
    PAD_UP = 1u << 4, // The d-pad and the left stick
    PAD_DOWN = 1u << 5,
    PAD_LEFT = 1u << 6,
    PAD_RIGHT = 1u << 7,
    KEY_TAB = 1u << 8,
    KEY_ENTER = 1u << 9,
    KEY_SPACE = 1u << 10,
    PAD_ACCEPT = 1u << 11, // South
    KEY_CANCEL = 1u << 12, // Escape
    PAD_CANCEL = 1u << 13, // East
    KEY_BACKSPACE = 1u << 14,
    KEY_SHIFT = 1u << 15,
};

#define STICK 0.5f // How far the left stick goes before it counts as a d-pad press

static uint32_t nav_keys(const tide_devices *d)
{
    const tide_keyboard *k = &d->keyboard;
    const tide_gamepad *p = &d->gamepad;
    uint32_t keys = 0;
    if (k->upArrow.held) keys |= KEY_UP;
    if (k->downArrow.held) keys |= KEY_DOWN;
    if (k->leftArrow.held) keys |= KEY_LEFT;
    if (k->rightArrow.held) keys |= KEY_RIGHT;
    if (k->tab.held) keys |= KEY_TAB;
    if (k->enter.held || k->numpadEnter.held) keys |= KEY_ENTER;
    if (k->space.held) keys |= KEY_SPACE;
    if (k->escape.held) keys |= KEY_CANCEL;
    if (k->backspace.held) keys |= KEY_BACKSPACE;
    if (k->leftShift.held || k->rightShift.held) keys |= KEY_SHIFT;
    if (p->connected) {
        if (p->dpad.up.held || p->leftStick.y > STICK) keys |= PAD_UP;
        if (p->dpad.down.held || p->leftStick.y < -STICK) keys |= PAD_DOWN;
        if (p->dpad.left.held || p->leftStick.x < -STICK) keys |= PAD_LEFT;
        if (p->dpad.right.held || p->leftStick.x > STICK) keys |= PAD_RIGHT;
        if (p->buttonSouth.held) keys |= PAD_ACCEPT;
        if (p->buttonEast.held) keys |= PAD_CANCEL;
    }
    return keys;
}

static bool pressed(const tide_gui *g, const uint32_t keys)
{
    return (g->keys_pressed & keys) != 0;
}

static float max_f(const float a, const float b)
{
    return a > b ? a : b;
}

static float min_f(const float a, const float b)
{
    return a < b ? a : b;
}

static float clamp_f(const float v, const float lo, const float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

// Without the platform's font: about the width of raylib's default one.
static float guess_width(const char *text, const float size)
{
    return (float)strlen(text) * size * 0.6f;
}

static float text_width(const tide_gui *g, const char *text)
{
    return g->measure(text, FONT);
}

// ---------------------------------------------------------------------------
// Drawing

static tide_rect grow(const tide_rect r, const float by)
{
    return (tide_rect){r.x - by, r.y - by, r.width + 2.0f * by, r.height + 2.0f * by};
}

static bool contains(const tide_rect r, const tide_float2 p)
{
    return p.x >= r.x && p.y >= r.y && p.x < r.x + r.width && p.y < r.y + r.height;
}

// Whether widgets drawn now are in a Disabled block.
static bool grayed(const tide_gui *g)
{
    return g->groups[g->depth].disabled;
}

// A widget's color: faded in a Disabled block.
static tide_color shade(const tide_gui *g, tide_color color)
{
    if (grayed(g)) color.a *= FADED;
    return color;
}

static void paint(tide_gui *g, const tide_rect r, const tide_color color)
{
    tide_draw_rect(&g->list, tide_f2(r.x + r.width * 0.5f, r.y + r.height * 0.5f), tide_f2(r.width, r.height), color);
}

// Part of a widget.
static void fill(tide_gui *g, const tide_rect r, const tide_color color)
{
    paint(g, r, shade(g, color));
}

// Text starting at `x`, centered on `middle` vertically.
static void text_at(tide_gui *g, const char *text, const float x, const float middle, const tide_color color)
{
    if (text[0]) tide_draw_text(&g->list, text, tide_f2(x, middle - FONT * 0.5f), FONT, shade(g, color));
}

static void text_centered(tide_gui *g, const char *text, const tide_rect r, const tide_color color)
{
    text_at(g, text, r.x + (r.width - text_width(g, text)) * 0.5f, r.y + r.height * 0.5f, color);
}

// ---------------------------------------------------------------------------
// IDs and remembered sizes

uint32_t tide_gui_seed(const uint32_t a, const uint32_t b)
{
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u + (a << 6) + (a >> 2));
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return h ? h : 1u;
}

uint32_t tide_gui_id(tide_gui *g, const uint32_t seed, const uint32_t site)
{
    const uint32_t key = tide_gui_seed(seed, site);
    uint32_t slot = key & (TIDE_GUI_MAX_IDS - 1);
    for (uint32_t probe = 0; probe < TIDE_GUI_MAX_IDS; probe++) {
        tide_gui_seen *s = &g->seen[slot];
        if (s->frame != g->frame) { // Free this frame
            *s = (tide_gui_seen){key, g->frame, 1};
            return key;
        }
        if (s->key == key) return tide_gui_seed(key, s->count++);
        slot = (slot + 1) & (TIDE_GUI_MAX_IDS - 1);
    }
    return key; // Full: widgets from one site share an ID
}

static const tide_gui_size *remembered(const tide_gui *g, const uint32_t id)
{
    const tide_gui_size *s = &g->sizes[id & (TIDE_GUI_MAX_SIZES - 1)];
    return s->frame != 0 && s->id == id ? s : NULL;
}

static void remember(tide_gui *g, const uint32_t id, const tide_float2 size, const float natural, const float least)
{
    g->sizes[id & (TIDE_GUI_MAX_SIZES - 1)] = (tide_gui_size){id, g->frame, size, natural, least};
}

// ---------------------------------------------------------------------------
// Frames

// A button as views see it: down or up since last frame.
static tide_button frame_button(const tide_button now, const tide_button last)
{
    return (tide_button){.pressed = now.held, .down = now.held && !last.held, .up = !now.held && last.held, .held = now.held};
}

// A touch as views see it. A finger that lifted shows its last frame with its
// id, and one that came in its slot since shows as new.
static void frame_touch(tide_touch *d, const tide_touch *now, const tide_touch *last)
{
    const bool same = now->id == last->id;
    const tide_button before = same ? last->press : (tide_button){0};
    if (now->id == 0 && last->press.held) { // It lifted, and a sample cleared its slot
        *d = *last;
        d->press = frame_button((tide_button){0}, before);
        d->delta = tide_f2(0.0f, 0.0f);
        return;
    }
    *d = *now;
    d->press = frame_button(now->press, before);
    d->delta = now->poll_delta;
}

// The devices views read this frame, from the platform's.
static void frame_devices(tide_gui *g, const tide_devices *now)
{
    tide_devices *d = &g->devices;
    const tide_devices *last = &g->last;
    memset(d, 0, sizeof *d);
#define FRAME_KEY(name) d->keyboard.name = frame_button(now->keyboard.name, last->keyboard.name);
#define FRAME_MOUSE(name) d->mouse.name = frame_button(now->mouse.name, last->mouse.name);
#define FRAME_PAD(name) d->gamepad.name = frame_button(now->gamepad.name, last->gamepad.name);
#define FRAME_DPAD(name) d->gamepad.dpad.name = frame_button(now->gamepad.dpad.name, last->gamepad.dpad.name);
    TIDE_KEYBOARD_KEYS(FRAME_KEY)
    TIDE_MOUSE_BUTTONS(FRAME_MOUSE)
    TIDE_GAMEPAD_BUTTONS(FRAME_PAD)
    TIDE_DPAD_BUTTONS(FRAME_DPAD)
#undef FRAME_KEY
#undef FRAME_MOUSE
#undef FRAME_PAD
#undef FRAME_DPAD
    d->keyboard.text = now->keyboard.text;
    d->mouse.position = now->mouse.position;
    d->mouse.delta = now->mouse.poll_delta;
    d->mouse.scroll = now->mouse.poll_scroll;
    d->gamepad.connected = now->gamepad.connected;
    d->gamepad.leftStick = now->gamepad.leftStick;
    d->gamepad.rightStick = now->gamepad.rightStick;
    d->gamepad.leftTrigger = now->gamepad.leftTrigger;
    d->gamepad.rightTrigger = now->gamepad.rightTrigger;
    d->touchscreen.connected = now->touchscreen.connected;
    frame_touch(&d->touchscreen.primaryTouch, &now->touchscreen.primaryTouch, &last->touchscreen.primaryTouch);
    for (int i = 0; i < TIDE_TOUCHES; i++) {
        frame_touch(&d->touchscreen.touches.at[i], &now->touchscreen.touches.at[i], &last->touchscreen.touches.at[i]);
    }
    d->pointer.position = now->pointer.position;
    d->pointer.delta = now->pointer.poll_delta;
    d->pointer.press = frame_button(now->pointer.press, last->pointer.press);
    d->pointer.touch = now->pointer.touch;
    g->last = *now;
}

#define RELEASE(b) (b) = (tide_button){0};

// The keys, and what's typed.
static void hide_keyboard(tide_devices *d)
{
#define RELEASE_KEY(name) RELEASE(d->keyboard.name)
    TIDE_KEYBOARD_KEYS(RELEASE_KEY)
#undef RELEASE_KEY
    d->keyboard.text = (tide_typed){0};
}

static void hide_gamepad(tide_devices *d)
{
#define RELEASE_PAD(name) RELEASE(d->gamepad.name)
#define RELEASE_DPAD(name) RELEASE(d->gamepad.dpad.name)
    TIDE_GAMEPAD_BUTTONS(RELEASE_PAD)
    TIDE_DPAD_BUTTONS(RELEASE_DPAD)
#undef RELEASE_PAD
#undef RELEASE_DPAD
    d->gamepad.leftStick = d->gamepad.rightStick = tide_f2(0.0f, 0.0f);
    d->gamepad.leftTrigger = d->gamepad.rightTrigger = 0.0f;
}

// The mouse's buttons and scroll, and the pointer's press.
static void hide_pointer(tide_devices *d)
{
#define RELEASE_MOUSE(name) RELEASE(d->mouse.name)
    TIDE_MOUSE_BUTTONS(RELEASE_MOUSE)
#undef RELEASE_MOUSE
    d->mouse.scroll = tide_f2(0.0f, 0.0f);
    // The finger that's the pointer: the primary touch, in its slot too.
    RELEASE(d->pointer.press)
    tide_touchscreen *s = &d->touchscreen;
    if (d->pointer.touch && s->primaryTouch.id != 0) {
        for (int i = 0; i < TIDE_TOUCHES; i++) {
            if (s->touches.at[i].id == s->primaryTouch.id) RELEASE(s->touches.at[i].press)
        }
        RELEASE(s->primaryTouch.press)
    }
}

#undef RELEASE

// Hides what the GUI uses from devices the game or views read. The GUI works
// out the next frame's from the platform's, so nothing is lost.
static void hide(const tide_gui *g, tide_devices *d)
{
    if (g->focus || g->editing || g->modal) {
        hide_keyboard(d);
        hide_gamepad(d);
    }
    if (g->over || g->active || g->modal) hide_pointer(d);
    if (g->modal) {
        d->mouse.delta = tide_f2(0.0f, 0.0f);
        d->pointer.delta = tide_f2(0.0f, 0.0f);
    }
}

// Hides what views claimed (`what`), as the GUI's own use is.
static void hide_claimed(tide_devices *d, const uint32_t what)
{
    if (what & TIDE_GUI_KEYBOARD) hide_keyboard(d);
    if (what & TIDE_GUI_POINTER) hide_pointer(d);
}

// Moves the focus with Tab, the arrows and the d-pad, in last frame's order.
static void navigate(tide_gui *g)
{
    // A view that claimed the keyboard has its keys until a widget has the
    // focus: Tab and the arrows don't start moving it.
    const bool keyboard = g->focus || !(g->taken & TIDE_GUI_KEYBOARD);
    const bool tab = keyboard && pressed(g, KEY_TAB);
    bool next = tab && !(g->keys & KEY_SHIFT);
    bool back = tab && (g->keys & KEY_SHIFT);
    // The arrows and d-pad move the focus once a widget has it. They only
    // start moving it while the game isn't reading the devices, so they never
    // take a HUD's button away from the game.
    if (g->focus || g->nav_arrows) {
        const uint32_t usable = keyboard ? ~0u : ~(uint32_t)(KEY_DOWN | KEY_UP);
        if (pressed(g, (KEY_DOWN | PAD_DOWN) & usable)) next = true;
        if (pressed(g, (KEY_UP | PAD_UP) & usable)) back = true;
    }
    const int n = (int)g->nav_last_count;
    if ((next || back) && n > 0) {
        int at = -1;
        for (int i = 0; i < n; i++) {
            if (g->nav_last[i] == g->focus) at = i;
        }
        const int to = at < 0 ? (next ? 0 : n - 1) : (at + (next ? 1 : n - 1)) % n;
        g->focus = g->nav_last[to];
    }
    if (pressed(g, KEY_CANCEL | PAD_CANCEL)) {
        if (g->editing) g->editing = 0; // Stops typing, keeping the old value
        else g->focus = 0;
    }
}

void tide_gui_begin(tide_gui *g, const tide_devices *devices, const tide_float2 screen, const tide_measure_fn measure)
{
    g->frame++;
    if (g->frame == 0) g->frame = 1; // Stamps of 0 mean never
    g->width = screen.x;
    g->height = screen.y;
    g->measure = measure ? measure : guess_width;

    // The pointer: the mouse, or a finger. The devices count from the bottom
    // left, y up; the GUI from the top left, y down. A finger that's off the
    // screen is nowhere, once it lifted, so nothing stays hovered.
    const tide_pointer *p = &devices->pointer;
    g->mouse = tide_f2(p->position.x, screen.y - p->position.y);
    g->mouse_held = p->press.held;
    if (p->touch && !g->mouse_held && !g->mouse_was_held) g->mouse = tide_f2(-1e30f, -1e30f);
    g->mouse_pressed = g->mouse_held && !g->mouse_was_held;
    g->mouse_released = !g->mouse_held && g->mouse_was_held;
    g->mouse_was_held = g->mouse_held;
    const uint32_t keys = nav_keys(devices);
    g->keys_pressed = keys & ~g->keys;
    g->keys = keys;
    g->text = devices->keyboard.text;
    g->nav_arrows = !g->game_input;
    g->game_input = false;
    g->back = pressed(g, KEY_CANCEL | PAD_CANCEL) && !g->editing;
    // With what the GUI used last frame, before this frame's keys move the focus.
    frame_devices(g, devices);
    hide(g, &g->devices);
    // ...and what views claimed then, until a view runs (tide_gui_view).
    g->unclaimed = g->devices;
    hide_claimed(&g->devices, g->taken);
    g->view = 0;

    g->claimed = false;
    g->active_seen = false;
    g->focus_seen = false;
    g->editing_seen = false;

    // A click ends keyboard and gamepad navigation; a field it lands on takes the focus back.
    if (g->mouse_pressed) g->focus = 0;
    navigate(g);

    g->nav_count = 0;
    g->depth = 0;
    g->in_modal = 0;
    const tide_gui_size *root = remembered(g, ROOT_ID);
    g->groups[0] = (tide_gui_group){
        .id = ROOT_ID, .kind = VERTICAL, .room = screen.x, .stretch = root ? root->natural : 0.0f, .anchor = -1};
    tide_draw_reset(&g->list);
}

void tide_gui_end(tide_gui *g, tide_draw_list *draw)
{
    tide_gui_close(g, 0);
    remember(g, ROOT_ID, g->groups[0].size, g->groups[0].natural, g->groups[0].least);

    // What wasn't drawn this frame lets go.
    if (!g->focus_seen) g->focus = 0;
    if (!g->editing_seen) g->editing = 0;
    if (!g->active_seen || !g->mouse_held) g->active = 0;
    g->hot = g->hot_next;
    g->hot_rect = g->hot_rect_next;
    g->hot_next = 0;
    g->over = g->over_next;
    g->over_next = false;
    // A modal that just came on top takes the focus next frame.
    g->grab = g->modal_next != g->modal ? g->modal_next : 0;
    g->modal = g->modal_next;
    g->modal_next = 0;
    memcpy(g->nav_last, g->nav, g->nav_count * sizeof g->nav[0]);
    g->nav_last_count = g->nav_count;
    // This frame's claims stand until the next frame ends.
    g->taken = g->taken_next;
    g->taken_next = 0;
    memcpy(g->claims, g->claims_next, g->claim_count_next * sizeof g->claims[0]);
    g->claim_count = g->claim_count_next;
    g->claim_count_next = 0;
    g->keyboard_shown = g->keyboard_shown_next;
    g->keyboard_shown_next = false;
    g->view = 0;

    if (g->list.count > 0) {
        tide_draw_gui(draw);
        tide_draw_append(draw, &g->list);
    }
}

bool tide_gui_typing(const tide_gui *g)
{
    return g->editing != 0 || g->keyboard_shown;
}

void tide_gui_hide(tide_gui *g, tide_devices *d)
{
    g->game_input = true;
    hide(g, d);
    hide_claimed(d, g->taken);
    d->keyboard.text = (tide_typed){0}; // What's typed is a frame's: views read it
}

// ---------------------------------------------------------------------------
// Claims

void tide_gui_view(tide_gui *g, const uint32_t view)
{
    g->view = view;
    if (!g->taken) return; // Nothing's claimed: every view reads the same devices
    uint32_t own = 0;
    for (uint32_t i = 0; i < g->claim_count; i++) {
        if (g->claims[i].view == view) own = g->claims[i].what;
    }
    g->devices = g->unclaimed;
    hide_claimed(&g->devices, g->taken & ~own);
}

// The view that's running claims `what`. Past TIDE_GUI_MAX_CLAIMS views, a
// claim still hides it from the game and the other views, and from the view
// itself.
static void claim(tide_gui *g, const uint32_t what)
{
    g->taken_next |= what;
    for (uint32_t i = 0; i < g->claim_count_next; i++) {
        if (g->claims_next[i].view != g->view) continue;
        g->claims_next[i].what |= what;
        return;
    }
    if (g->claim_count_next < TIDE_GUI_MAX_CLAIMS) g->claims_next[g->claim_count_next++] = (tide_gui_claim){g->view, what};
}

void tide_gui_claim_pointer(tide_gui *g)
{
    claim(g, TIDE_GUI_POINTER);
}

void tide_gui_claim_keyboard(tide_gui *g)
{
    claim(g, TIDE_GUI_KEYBOARD);
}

void tide_gui_show_keyboard(tide_gui *g)
{
    g->keyboard_shown_next = true;
}

// ---------------------------------------------------------------------------
// Interaction

// Whether widgets drawn now are under the modal on top, outside it.
static bool under_modal(const tide_gui *g)
{
    return g->modal != 0 && g->in_modal != g->modal;
}

// Whether widgets drawn now work: always, unless they're under a modal or in
// a Disabled block.
static bool live(const tide_gui *g)
{
    return !under_modal(g) && !grayed(g);
}

// Whether the mouse is on the widget. Where widgets overlap, the one drawn
// last is on top: it was under the mouse last frame, and hides the others
// while the mouse stays on it. A disabled widget does too, and keeps clicks on
// it from the game, but it's never hovered itself.
static bool hovered(tide_gui *g, const uint32_t id, const tide_rect r)
{
    if (under_modal(g) || !contains(r, g->mouse)) return false;
    g->hot_next = id;
    g->hot_rect_next = r;
    g->over_next = true;
    return !grayed(g) && (g->hot == id || g->hot == 0 || !contains(g->hot_rect, g->mouse));
}

// Whether the mouse is pressing the widget: from a press on it until it's let
// go, or until it's disabled.
static bool held_down(tide_gui *g, const uint32_t id, const bool hover)
{
    if (hover && g->mouse_pressed && !g->claimed) {
        g->active = id;
        g->claimed = true;
    }
    if (g->active != id || grayed(g)) return false;
    g->active_seen = true;
    return true;
}

// Adds the widget to the ones the focus moves between; true if it has it.
static bool focusable(tide_gui *g, const uint32_t id)
{
    if (!live(g)) return false;
    if (g->grab && g->grab == g->in_modal) { // The first widget of a modal that just came on top
        g->grab = 0;
        g->focus = id;
        g->editing = 0;
    }
    if (g->nav_count < TIDE_GUI_MAX_NAV) g->nav[g->nav_count++] = id;
    if (g->focus != id) return false;
    g->focus_seen = true;
    return true;
}

// A button's press: a click that starts and ends on it, or Enter, Space or
// the south button while it has the focus.
static bool clicked(tide_gui *g, const uint32_t id, const tide_rect r, bool *hover, bool *down, bool *focused)
{
    *hover = hovered(g, id, r);
    *down = held_down(g, id, *hover);
    *focused = focusable(g, id);
    return (*down && g->mouse_released && *hover) || (*focused && pressed(g, KEY_ENTER | KEY_SPACE | PAD_ACCEPT));
}

// ---------------------------------------------------------------------------
// Layout

static tide_gui_group *top(tide_gui *g)
{
    return &g->groups[g->depth];
}

// How far a row's widgets shrink, from their natural widths (0) to their
// least (1), for the row to fit its room.
static float squeeze(const float natural, const float least, const float room)
{
    if (natural <= room) return 0.0f;
    if (natural <= least) return 1.0f;
    return min_f((natural - room) / (natural - least), 1.0f);
}

// How wide a widget wants to be: `natural`, or in a vertical container, as
// wide as its widest widget was last frame if it `stretch`es.
static float wanted(const tide_gui_group *grp, const float natural, const bool stretch)
{
    return stretch && grp->kind != HORIZONTAL && grp->stretch > natural ? grp->stretch : natural;
}

// How wide it gets: what it wants if there's room, and down to `least` if
// there isn't. A row shrinks each widget by as much as it can give.
static float fit(const tide_gui_group *grp, const float want, const float least)
{
    const float floor = min_f(least, want);
    if (grp->kind == HORIZONTAL) return want - (want - floor) * grp->squeeze;
    return max_f(min_f(want, grp->room), floor);
}

// Puts something `width` wide at the next place in the current container.
// `natural` and `least` are the widths it would take with room and squeezed.
static tide_rect take(tide_gui *g, const float width, const float height, const float natural, const float least)
{
    tide_gui_group *grp = top(g);
    const tide_rect r = {grp->cursor.x, grp->cursor.y, width, height};
    if (grp->kind == HORIZONTAL) {
        grp->cursor.x += width + SPACING;
        grp->size = tide_f2(r.x + width - grp->origin.x, max_f(grp->size.y, r.y + height - grp->origin.y));
        grp->natural += natural + SPACING; // The last one's spacing comes off when it closes
        grp->least += least + SPACING;
        return r;
    }
    grp->cursor.y += height + SPACING;
    grp->size = tide_f2(max_f(grp->size.x, r.x + width - grp->origin.x), r.y + height - grp->origin.y);
    grp->natural = max_f(grp->natural, natural);
    grp->least = max_f(grp->least, least);
    return r;
}

// Takes the next place in the current container for a widget `natural` wide,
// which can shrink to `least`. In a vertical one, widgets that `stretch` are
// as wide as its widest one was last frame.
static tide_rect reserve(tide_gui *g, const float natural, const float least, const bool stretch)
{
    const tide_gui_group *grp = top(g);
    return take(g, fit(grp, wanted(grp, natural, stretch), least), LINE, natural, min_f(least, natural));
}

static int open_group(tide_gui *g, const uint32_t id, const uint32_t kind, const tide_float2 origin)
{
    const int before = g->depth;
    if (g->depth == TIDE_GUI_MAX_DEPTH) return before; // Too deep: its content goes in the container around it
    const tide_gui_group *around = top(g);
    const tide_gui_size *last = remembered(g, id);
    // Its room: in a row, its share of the row's, from last frame's size; in a
    // vertical container, all of it.
    float room = around->room;
    if (around->kind == HORIZONTAL) {
        room = last ? fit(around, last->natural, last->least)
                    : max_f(around->room - (around->cursor.x - around->origin.x), 0.0f);
    }
    g->depth++;
    tide_gui_group *grp = top(g);
    *grp = (tide_gui_group){.id = id, .kind = kind, .origin = origin, .cursor = origin, .room = room, .anchor = -1,
                            .panel = UINT32_MAX, .in_modal_before = g->in_modal, .disabled = around->disabled};
    if (last && kind != HORIZONTAL) grp->stretch = last->natural;
    if (last && kind == HORIZONTAL) grp->squeeze = squeeze(last->natural, last->least, room);
    return before;
}

int tide_gui_begin_vertical(tide_gui *g, const uint32_t id)
{
    return open_group(g, id, VERTICAL, top(g)->cursor);
}

int tide_gui_begin_horizontal(tide_gui *g, const uint32_t id)
{
    return open_group(g, id, HORIZONTAL, top(g)->cursor);
}

// Where an anchored area of `size` goes on the screen. One too big for it
// starts at the top left margin, so what doesn't fit is what comes last.
static tide_float2 place(const tide_gui *g, int32_t anchor, const tide_float2 size)
{
    if (anchor < 0 || anchor > TIDE_ANCHOR_LOWER_RIGHT) anchor = TIDE_ANCHOR_UPPER_LEFT;
    const int column = anchor % 3;
    const int row = anchor / 3;
    const float x = column == 0 ? AREA_MARGIN : column == 1 ? (g->width - size.x) * 0.5f : g->width - AREA_MARGIN - size.x;
    const float y = row == 0 ? AREA_MARGIN : row == 1 ? (g->height - size.y) * 0.5f : g->height - AREA_MARGIN - size.y;
    return tide_f2(max_f(x, AREA_MARGIN), max_f(y, AREA_MARGIN));
}

// An area's background, sized when it closes. It isn't a widget: in a
// Disabled block, only its widgets fade.
static uint32_t panel(tide_gui *g, const tide_rect r)
{
    const uint32_t index = g->list.count;
    paint(g, r, PANEL);
    return g->list.count > index ? index : UINT32_MAX;
}

int tide_gui_begin_area(tide_gui *g, const uint32_t id, const tide_rect rect)
{
    const uint32_t background = panel(g, rect);
    const int before = open_group(g, id, AREA, tide_f2(rect.x + AREA_PADDING, rect.y + AREA_PADDING));
    if (g->depth == before) return before;
    top(g)->room = max_f(rect.width - 2.0f * AREA_PADDING, 0.0f);
    top(g)->rect = rect;
    top(g)->panel = background;
    top(g)->hot_before = g->hot_next;
    return before;
}

int tide_gui_begin_area_at(tide_gui *g, const uint32_t id, const int32_t anchor)
{
    // Placed with last frame's size, so widgets know where they are as
    // they're drawn. If the size changes, it moves at the end.
    const tide_gui_size *last = remembered(g, id);
    const tide_float2 size = last ? last->size : tide_f2(0.0f, 0.0f);
    const tide_float2 at = place(g, anchor, size);
    const uint32_t background = panel(g, (tide_rect){at.x, at.y, size.x, size.y});
    const int before = open_group(g, id, AREA, tide_f2(at.x + AREA_PADDING, at.y + AREA_PADDING));
    if (g->depth == before) return before;
    top(g)->room = max_f(g->width - 2.0f * (AREA_MARGIN + AREA_PADDING), 0.0f);
    top(g)->guess = at;
    top(g)->anchor = anchor;
    top(g)->panel = background;
    top(g)->hot_before = g->hot_next;
    return before;
}

int tide_gui_begin_modal(tide_gui *g, const uint32_t id, const int32_t anchor, bool *open)
{
    if (!*open) return -1;
    // Back closes the modal on top. Not the one that came up this frame: the
    // press that opened it isn't for it.
    if (g->modal == id && g->back) {
        g->back = false;
        *open = false;
        return -1;
    }
    paint(g, (tide_rect){0.0f, 0.0f, g->width, g->height}, DIM);
    g->over_next = true; // The whole screen is the modal's
    g->modal_next = id;  // The last one drawn is on top
    const int before = tide_gui_begin_area_at(g, id, anchor);
    if (g->depth > before) g->in_modal = id;
    return before;
}

// A copy of the container around it, which its widgets go on laying out, with
// `disabled` added.
int tide_gui_begin_disabled(tide_gui *g, const bool disabled)
{
    const int before = g->depth;
    if (g->depth == TIDE_GUI_MAX_DEPTH) { // Too deep: the rest of the container around it is disabled too
        top(g)->disabled = top(g)->disabled || disabled;
        return before;
    }
    g->groups[g->depth + 1] = *top(g);
    g->depth++;
    top(g)->disabled = top(g)->disabled || disabled;
    top(g)->scope = true;
    return before;
}

static void close_area(tide_gui *g, const tide_gui_group *grp)
{
    const tide_float2 size = tide_f2(grp->size.x + 2.0f * AREA_PADDING, grp->size.y + 2.0f * AREA_PADDING);
    tide_rect r = grp->rect;
    if (grp->anchor >= 0) {
        const tide_float2 at = place(g, grp->anchor, size);
        const float dx = at.x - grp->guess.x;
        const float dy = at.y - grp->guess.y;
        if ((dx != 0.0f || dy != 0.0f) && grp->panel != UINT32_MAX) {
            for (uint32_t i = grp->panel; i < g->list.count; i++) {
                tide_draw_command *c = &g->list.commands[i];
                c->a = tide_f2(c->a.x + dx, c->a.y + dy);
                if (c->kind == TIDE_DRAW_LINE) c->b = tide_f2(c->b.x + dx, c->b.y + dy);
            }
        }
        r = (tide_rect){at.x, at.y, size.x, size.y};
    }
    if (grp->panel != UINT32_MAX) {
        tide_draw_command *c = &g->list.commands[grp->panel];
        c->a = tide_f2(r.x + r.width * 0.5f, r.y + r.height * 0.5f);
        c->b = tide_f2(r.width, r.height);
    }
    remember(g, grp->id, size, grp->natural, grp->least);
    if (!contains(r, g->mouse)) return;
    g->over_next = true;
    // Under the mouse, but none of its widgets is: the panel hides what's below it.
    if (g->hot_next == grp->hot_before) {
        g->hot_next = grp->id;
        g->hot_rect_next = r;
    }
}

void tide_gui_close(tide_gui *g, const int depth)
{
    while (g->depth > depth && g->depth > 0) {
        const tide_gui_group grp = *top(g);
        g->depth--;
        if (grp.scope) { // A Disabled block hands the layout back to the container around it
            tide_gui_group *around = top(g);
            const bool disabled = around->disabled;
            const bool scope = around->scope;
            *around = grp;
            around->disabled = disabled;
            around->scope = scope;
            continue;
        }
        g->in_modal = grp.in_modal_before;
        if (grp.kind == AREA) {
            close_area(g, &grp); // Areas are on the screen, not in the container around them
            continue;
        }
        float natural = grp.natural;
        float least = grp.least;
        if (grp.kind == HORIZONTAL) { // Less the spacing after its last widget
            natural = max_f(natural - SPACING, 0.0f);
            least = max_f(least - SPACING, 0.0f);
        }
        remember(g, grp.id, grp.size, natural, least);
        take(g, grp.size.x, grp.size.y, natural, least);
    }
}

void tide_gui_layout_space(tide_gui *g, const float size)
{
    tide_gui_group *grp = top(g);
    if (grp->kind == HORIZONTAL) {
        grp->cursor.x += size;
        grp->size.x = max_f(grp->size.x, grp->cursor.x - SPACING - grp->origin.x);
        grp->natural += size;
        grp->least += size;
    } else {
        grp->cursor.y += size;
        grp->size.y = max_f(grp->size.y, grp->cursor.y - SPACING - grp->origin.y);
    }
}

// A labelled widget's label column, as narrow as it goes: the label and a gap.
static float label_least(const tide_gui *g, const char *label)
{
    return label[0] ? text_width(g, label) + 2.0f * SPACING : 0.0f;
}

// The label column with room: at least LABEL_WIDTH, so a column of them lines up.
static float label_width(const tide_gui *g, const char *label)
{
    return label[0] ? max_f(LABEL_WIDTH, label_least(g, label)) : 0.0f;
}

// Takes the next place for a labelled widget whose control is `natural` wide
// and can shrink to `least`. Draws the label and returns the control's rect.
// Squeezed, the label's column gives up its room first, down to the label: by
// what the widget lost from the width it wanted, so the labelled widgets of a
// vertical container, which want the same width, keep their columns lined up.
static tide_rect labelled(tide_gui *g, const char *label, const float natural, const float least)
{
    const tide_gui_group *grp = top(g);
    const float column = label_width(g, label);
    const float tight = label_least(g, label);
    const float want = wanted(grp, column + natural, true);
    const float width = fit(grp, want, tight + least);
    const tide_rect r = take(g, width, LINE, column + natural, tight + least);
    const float w = min_f(max_f(column - (want - width), tight), r.width);
    text_at(g, label, r.x, r.y + r.height * 0.5f, TEXT);
    return (tide_rect){r.x + w, r.y, r.width - w, r.height};
}

// Draws the label in its column and returns the rest of the rect.
static tide_rect after_label(tide_gui *g, const tide_rect r, const char *label)
{
    if (!label[0]) return r;
    const float w = min_f(label_width(g, label), r.width);
    text_at(g, label, r.x, r.y + r.height * 0.5f, TEXT);
    return (tide_rect){r.x + w, r.y, r.width - w, r.height};
}

// ---------------------------------------------------------------------------
// Widgets

void tide_gui_label(tide_gui *g, const tide_rect rect, const char *text)
{
    text_at(g, text, rect.x, rect.y + rect.height * 0.5f, TEXT);
}

void tide_gui_layout_label(tide_gui *g, const char *text)
{
    const float width = text_width(g, text);
    tide_gui_label(g, reserve(g, width, width, false), text);
}

bool tide_gui_button(tide_gui *g, const uint32_t id, const tide_rect rect, const char *text)
{
    bool hover, down, focused;
    const bool pressed_now = clicked(g, id, rect, &hover, &down, &focused);
    if (focused) fill(g, grow(rect, BORDER), ACCENT);
    fill(g, rect, down && hover ? CONTROL_DOWN : hover ? CONTROL_HOT : CONTROL);
    text_centered(g, text, rect, TEXT);
    return pressed_now;
}

bool tide_gui_layout_button(tide_gui *g, const uint32_t id, const char *text)
{
    const float width = text_width(g, text);
    return tide_gui_button(g, id, reserve(g, width + 2.0f * PAD, width + PAD, true), text);
}

bool tide_gui_toggle(tide_gui *g, const uint32_t id, const tide_rect rect, const char *text, bool *value)
{
    bool hover, down, focused;
    const bool changed = clicked(g, id, rect, &hover, &down, &focused);
    if (changed) *value = !*value;
    const tide_rect box = {rect.x, rect.y + (rect.height - BOX) * 0.5f, BOX, BOX};
    if (focused) fill(g, grow(box, BORDER), ACCENT);
    fill(g, box, hover ? CONTROL_HOT : CONTROL);
    if (*value) fill(g, grow(box, -4.0f), ACCENT);
    text_at(g, text, rect.x + BOX + 8.0f, rect.y + rect.height * 0.5f, TEXT);
    return changed;
}

bool tide_gui_layout_toggle(tide_gui *g, const uint32_t id, const char *text, bool *value)
{
    const float width = BOX + 8.0f + text_width(g, text);
    return tide_gui_toggle(g, id, reserve(g, width, width, true), text, value);
}

// A slider's track and thumb. `t` is where the value is, 0 to 1; returns
// where the player moved it, or -1.
static float slider(tide_gui *g, const uint32_t id, const tide_rect r, const float t, const float steps)
{
    const bool hover = hovered(g, id, r);
    const bool down = held_down(g, id, hover);
    const bool focused = focusable(g, id);
    float moved = -1.0f;
    if (down && g->mouse_held && r.width > 0.0f) moved = clamp_f((g->mouse.x - r.x) / r.width, 0.0f, 1.0f);
    if (focused && pressed(g, KEY_LEFT | PAD_LEFT)) moved = clamp_f(t - 1.0f / steps, 0.0f, 1.0f);
    if (focused && pressed(g, KEY_RIGHT | PAD_RIGHT)) moved = clamp_f(t + 1.0f / steps, 0.0f, 1.0f);

    const float at = moved >= 0.0f ? moved : t;
    const float middle = r.y + r.height * 0.5f;
    const tide_rect rail = {r.x, middle - 3.0f, r.width, 6.0f};
    if (focused) fill(g, grow(rail, BORDER), ACCENT);
    fill(g, rail, hover || down ? CONTROL_HOT : CONTROL);
    fill(g, (tide_rect){rail.x, rail.y, rail.width * at, rail.height}, ACCENT);
    fill(g, (tide_rect){r.x + r.width * at - 6.0f, middle - 12.0f, 12.0f, 24.0f}, down ? ACCENT : TEXT);
    return moved;
}

// Where `v` is between `min` and `max`, 0 to 1; a reversed range works too.
static float fraction(const float v, const float min, const float max)
{
    if (max == min) return 0.0f;
    const float t = (v - min) / (max - min);
    return t >= 0.0f ? min_f(t, 1.0f) : 0.0f; // Also NaN
}

// The track's rect: the rest of the widget after its label, minus the value on the right.
static tide_rect track(tide_gui *g, const tide_rect rect, const char *label, const char *value)
{
    const tide_rect r = after_label(g, rect, label);
    const float w = max_f(r.width - VALUE_WIDTH, 0.0f);
    text_at(g, value, r.x + w + 2.0f * SPACING, r.y + r.height * 0.5f, TEXT);
    return (tide_rect){r.x + 6.0f, r.y, max_f(w - 12.0f, 0.0f), r.height}; // Room for the thumb at both ends
}

bool tide_gui_slider(tide_gui *g, const uint32_t id, const tide_rect rect, const char *label, float *value,
                     const float min, const float max)
{
    char text[32];
    snprintf(text, sizeof text, "%.2f", (double)*value);
    const float moved = slider(g, id, track(g, rect, label, text), fraction(*value, min, max), 20.0f);
    if (moved < 0.0f) return false;
    const float v = moved >= 1.0f ? max : min + (max - min) * moved;
    if (v == *value) return false;
    *value = v;
    return true;
}

bool tide_gui_layout_slider(tide_gui *g, const uint32_t id, const char *label, float *value, const float min,
                            const float max)
{
    const tide_rect r = labelled(g, label, SLIDER_WIDTH + VALUE_WIDTH, LEAST_TRACK + VALUE_WIDTH);
    return tide_gui_slider(g, id, r, "", value, min, max);
}

bool tide_gui_int_slider(tide_gui *g, const uint32_t id, const tide_rect rect, const char *label, int32_t *value,
                         const int32_t min, const int32_t max)
{
    char text[32];
    snprintf(text, sizeof text, "%d", (int)*value);
    const float range = (float)max - (float)min;
    const float steps = range < 0.0f ? -range : range;
    const float moved =
        slider(g, id, track(g, rect, label, text), fraction((float)*value, (float)min, (float)max), steps > 0.0f ? steps : 1.0f);
    if (moved < 0.0f) return false;
    const double exact = (double)min + (double)range * (double)moved;
    const int32_t v = (int32_t)(exact < 0.0 ? exact - 0.5 : exact + 0.5);
    if (v == *value) return false;
    *value = v;
    return true;
}

bool tide_gui_layout_int_slider(tide_gui *g, const uint32_t id, const char *label, int32_t *value, const int32_t min,
                                const int32_t max)
{
    const tide_rect r = labelled(g, label, SLIDER_WIDTH + VALUE_WIDTH, LEAST_TRACK + VALUE_WIDTH);
    return tide_gui_int_slider(g, id, r, "", value, min, max);
}

// ---------------------------------------------------------------------------
// Number fields: click or press Enter to type, Enter or leaving to keep it,
// Escape to go back. With the focus, left and right step the value.

typedef enum number_kind {
    NUMBER_INT,
    NUMBER_FLOAT,
    NUMBER_UNIT, // A float from 0 to 1, like a color's
} number_kind;

static void format_number(char *out, const size_t size, const double v, const number_kind kind)
{
    if (kind == NUMBER_INT) snprintf(out, size, "%d", (int)v);
    else snprintf(out, size, "%g", v);
}

// Starts typing into a field. Until something is typed, the whole value is
// selected: typing replaces it.
static void start_typing(tide_gui *g, const uint32_t id, const double value, const number_kind kind)
{
    g->editing = id;
    g->editing_seen = true;
    format_number(g->edit, sizeof g->edit, value, kind);
    g->edit_len = (uint32_t)strlen(g->edit);
    g->edit_fresh = true;
}

static bool number_char(const uint32_t c, const number_kind kind)
{
    if ((c >= '0' && c <= '9') || c == '-' || c == '+') return true;
    return kind != NUMBER_INT && (c == '.' || c == ',' || c == 'e' || c == 'E');
}

// Whether any of this frame's typed characters fit a number.
static bool typed_number(const tide_gui *g, const number_kind kind)
{
    for (uint32_t i = 0; i < g->text.count && i < TIDE_TEXT_MAX; i++) {
        if (number_char(g->text.chars[i], kind)) return true;
    }
    return false;
}

static void type_into(tide_gui *g, const number_kind kind)
{
    if (pressed(g, KEY_BACKSPACE)) {
        if (g->edit_fresh) g->edit_len = 0;
        else if (g->edit_len > 0) g->edit_len--;
        g->edit_fresh = false;
    }
    for (uint32_t i = 0; i < g->text.count && i < TIDE_TEXT_MAX; i++) {
        const uint32_t c = g->text.chars[i];
        if (!number_char(c, kind)) continue;
        if (g->edit_fresh) g->edit_len = 0;
        g->edit_fresh = false;
        if (g->edit_len + 1 < sizeof g->edit) g->edit[g->edit_len++] = c == ',' ? '.' : (char)c;
    }
    g->edit[g->edit_len] = '\0';
}

// What was typed, if it's a number.
static bool parse_number(tide_gui *g, const number_kind kind, double *out)
{
    g->edit[g->edit_len] = '\0';
    char *end = NULL;
    double v;
    if (kind == NUMBER_INT) {
        const long long n = strtoll(g->edit, &end, 10);
        v = n < INT32_MIN ? INT32_MIN : n > INT32_MAX ? INT32_MAX : (double)n;
    } else {
        v = strtod(g->edit, &end);
    }
    if (end == g->edit || *end != '\0' || v - v != 0.0) return false; // Empty, junk after it, infinite or NaN
    if (kind == NUMBER_FLOAT && (v > 3.4028234663852886e38 || v < -3.4028234663852886e38)) return false;
    if (kind == NUMBER_UNIT) v = v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v;
    *out = v;
    return true;
}

// One box of a number field. Returns whether the player changed `value`.
static bool number_box(tide_gui *g, const uint32_t id, const tide_rect box, double *value, const number_kind kind)
{
    const bool hover = hovered(g, id, box);
    if (hover && g->mouse_pressed && !g->claimed) {
        g->claimed = true;
        g->focus = id;
        if (g->editing != id) start_typing(g, id, *value, kind);
    }
    const bool focused = focusable(g, id);
    bool changed = false;
    if (g->editing == id) {
        g->editing_seen = true;
        if (grayed(g)) {
            g->editing = 0; // Disabled while typing: keeps the old value, as Escape does
        } else if (!focused || pressed(g, KEY_ENTER | PAD_ACCEPT)) {
            double v;
            if (parse_number(g, kind, &v) && v != *value) {
                *value = v;
                changed = true;
            }
            g->editing = 0;
        } else {
            type_into(g, kind);
        }
    } else if (focused) {
        if (pressed(g, KEY_ENTER)) {
            start_typing(g, id, *value, kind);
        } else if (typed_number(g, kind)) {
            start_typing(g, id, *value, kind);
            type_into(g, kind);
        } else {
            const double step = kind == NUMBER_INT ? 1.0 : kind == NUMBER_UNIT ? 0.05 : 0.1;
            double v = *value;
            if (pressed(g, KEY_LEFT | PAD_LEFT)) v -= step;
            if (pressed(g, KEY_RIGHT | PAD_RIGHT)) v += step;
            if (kind == NUMBER_UNIT) v = v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v;
            if (kind == NUMBER_INT) v = v < INT32_MIN ? INT32_MIN : v > INT32_MAX ? INT32_MAX : v;
            if (v != *value) {
                *value = v;
                changed = true;
            }
        }
    }

    if (focused) fill(g, grow(box, BORDER), ACCENT);
    fill(g, box, hover ? FIELD_HOT : FIELD);
    char text[48];
    if (g->editing == id) snprintf(text, sizeof text, "%s_", g->edit);
    else format_number(text, sizeof text, *value, kind);
    text_at(g, text, box.x + 10.0f, box.y + box.height * 0.5f, TEXT);
    return changed;
}

// A field of `n` floats side by side.
static bool float_parts(tide_gui *g, const uint32_t id, const tide_rect r, float *parts, const int n,
                        const number_kind kind)
{
    const float w = max_f((r.width - SPACING * (float)(n - 1)) / (float)n, 0.0f);
    bool changed = false;
    for (int i = 0; i < n; i++) {
        const tide_rect box = {r.x + (float)i * (w + SPACING), r.y, w, r.height};
        double v = parts[i];
        if (number_box(g, tide_gui_seed(id, (uint32_t)i + 1u), box, &v, kind) && (float)v != parts[i]) {
            parts[i] = (float)v;
            changed = true;
        }
    }
    return changed;
}

bool tide_gui_int_field(tide_gui *g, const uint32_t id, const tide_rect rect, const char *label, int32_t *value)
{
    double v = *value;
    if (!number_box(g, id, after_label(g, rect, label), &v, NUMBER_INT) || (int32_t)v == *value) return false;
    *value = (int32_t)v;
    return true;
}

bool tide_gui_float_field(tide_gui *g, const uint32_t id, const tide_rect rect, const char *label, float *value)
{
    float parts[1] = {*value};
    if (!float_parts(g, id, after_label(g, rect, label), parts, 1, NUMBER_FLOAT)) return false;
    *value = parts[0];
    return true;
}

bool tide_gui_float2_field(tide_gui *g, const uint32_t id, const tide_rect rect, const char *label, tide_float2 *value)
{
    float parts[2] = {value->x, value->y};
    if (!float_parts(g, id, after_label(g, rect, label), parts, 2, NUMBER_FLOAT)) return false;
    *value = tide_f2(parts[0], parts[1]);
    return true;
}

bool tide_gui_float3_field(tide_gui *g, const uint32_t id, const tide_rect rect, const char *label, tide_float3 *value)
{
    float parts[3] = {value->x, value->y, value->z};
    if (!float_parts(g, id, after_label(g, rect, label), parts, 3, NUMBER_FLOAT)) return false;
    *value = tide_f3(parts[0], parts[1], parts[2]);
    return true;
}

bool tide_gui_float4_field(tide_gui *g, const uint32_t id, const tide_rect rect, const char *label, tide_float4 *value)
{
    float parts[4] = {value->x, value->y, value->z, value->w};
    if (!float_parts(g, id, after_label(g, rect, label), parts, 4, NUMBER_FLOAT)) return false;
    *value = tide_f4(parts[0], parts[1], parts[2], parts[3]);
    return true;
}

bool tide_gui_color_field(tide_gui *g, const uint32_t id, const tide_rect rect, const char *label, tide_color *value)
{
    const tide_rect r = after_label(g, rect, label);
    const tide_rect swatch = {r.x, r.y, min_f(r.height, r.width), r.height};
    fill(g, swatch, CONTROL);
    fill(g, grow(swatch, -3.0f), *value);
    float parts[4] = {value->r, value->g, value->b, value->a};
    const float skip = swatch.width + SPACING;
    const tide_rect rest = {r.x + skip, r.y, max_f(r.width - skip, 0.0f), r.height};
    if (!float_parts(g, id, rest, parts, 4, NUMBER_UNIT)) return false;
    *value = (tide_color){parts[0], parts[1], parts[2], parts[3]};
    return true;
}

// ---------------------------------------------------------------------------
// Text fields: click, or press Enter or type with the focus, to type; the
// value changes as you type. Enter or Escape stop typing.

static void start_text(tide_gui *g, const uint32_t id, const tide_str value)
{
    g->editing = id;
    g->editing_seen = true;
    int32_t n = value.bytes < (int32_t)sizeof g->edit - 1 ? value.bytes : (int32_t)sizeof g->edit - 1;
    while (n > 0 && n < value.bytes && ((unsigned char)value.ptr[n] & 0xC0u) == 0x80u) n--; // Whole characters
    memcpy(g->edit, value.ptr, (size_t)n);
    g->edit_len = (uint32_t)n;
    g->edit[n] = '\0';
    g->edit_fresh = false;
}

// Typed characters and Backspace into the edit buffer; whether it changed.
static bool type_text(tide_gui *g)
{
    bool changed = false;
    if (pressed(g, KEY_BACKSPACE) && g->edit_len > 0) {
        do g->edit_len--; // A whole character: its continuation bytes, then its first
        while (g->edit_len > 0 && ((unsigned char)g->edit[g->edit_len] & 0xC0u) == 0x80u);
        changed = true;
    }
    for (uint32_t i = 0; i < g->text.count && i < TIDE_TEXT_MAX; i++) {
        char bytes[4];
        const int n = tide_utf8_encode(g->text.chars[i], bytes);
        if (n == 0 || g->edit_len + (uint32_t)n + 1 > sizeof g->edit) continue;
        memcpy(g->edit + g->edit_len, bytes, (size_t)n);
        g->edit_len += (uint32_t)n;
        changed = true;
    }
    g->edit[g->edit_len] = '\0';
    return changed;
}

bool tide_gui_text_field(tide_gui *g, const uint32_t id, const tide_rect rect, const char *label, const tide_textref value)
{
    const tide_rect box = after_label(g, rect, label);
    const bool hover = hovered(g, id, box);
    if (hover && g->mouse_pressed && !g->claimed) {
        g->claimed = true;
        g->focus = id;
        if (g->editing != id) start_text(g, id, tide_textref_get(value));
    }
    const bool focused = focusable(g, id);
    bool changed = false;
    if (g->editing == id) {
        g->editing_seen = true;
        if (!focused || pressed(g, KEY_ENTER)) g->editing = 0;
        else changed = type_text(g);
    } else if (focused && (pressed(g, KEY_ENTER) || g->text.count > 0)) {
        start_text(g, id, tide_textref_get(value));
        changed = type_text(g);
    }
    if (changed) {
        // A copy of the buffer, which keeps changing as the player types.
        const tide_str typed = {g->edit, (int32_t)g->edit_len, tide_utf8_chars(g->edit, (int32_t)g->edit_len)};
        tide_textref_set(value, tide_str_add(TIDE_STR_EMPTY, typed));
    }

    if (focused) fill(g, grow(box, BORDER), ACCENT);
    fill(g, box, hover ? FIELD_HOT : FIELD);
    const tide_str shown = tide_textref_get(value);
    char text[300];
    snprintf(text, sizeof text, g->editing == id ? "%.*s_" : "%.*s", (int)shown.bytes, shown.ptr);
    text_at(g, text, box.x + 10.0f, box.y + box.height * 0.5f, TEXT);
    return changed;
}

bool tide_gui_layout_text_field(tide_gui *g, const uint32_t id, const char *label, const tide_textref value)
{
    return tide_gui_text_field(g, id, labelled(g, label, 2.0f * FIELD_WIDTH, 2.0f * LEAST_FIELD), "", value);
}

bool tide_gui_layout_int_field(tide_gui *g, const uint32_t id, const char *label, int32_t *value)
{
    return tide_gui_int_field(g, id, labelled(g, label, FIELD_WIDTH, LEAST_FIELD), "", value);
}

bool tide_gui_layout_float_field(tide_gui *g, const uint32_t id, const char *label, float *value)
{
    return tide_gui_float_field(g, id, labelled(g, label, FIELD_WIDTH, LEAST_FIELD), "", value);
}

static float parts_width(const int n, const float each)
{
    return (float)n * each + (float)(n - 1) * SPACING;
}

bool tide_gui_layout_float2_field(tide_gui *g, const uint32_t id, const char *label, tide_float2 *value)
{
    const tide_rect r = labelled(g, label, parts_width(2, PART_WIDTH), parts_width(2, LEAST_PART));
    return tide_gui_float2_field(g, id, r, "", value);
}

bool tide_gui_layout_float3_field(tide_gui *g, const uint32_t id, const char *label, tide_float3 *value)
{
    const tide_rect r = labelled(g, label, parts_width(3, PART_WIDTH), parts_width(3, LEAST_PART));
    return tide_gui_float3_field(g, id, r, "", value);
}

bool tide_gui_layout_float4_field(tide_gui *g, const uint32_t id, const char *label, tide_float4 *value)
{
    const tide_rect r = labelled(g, label, parts_width(4, PART_WIDTH), parts_width(4, LEAST_PART));
    return tide_gui_float4_field(g, id, r, "", value);
}

bool tide_gui_layout_color_field(tide_gui *g, const uint32_t id, const char *label, tide_color *value)
{
    const float swatch = LINE + SPACING;
    const tide_rect r =
        labelled(g, label, swatch + parts_width(4, COLOR_PART_WIDTH), swatch + parts_width(4, LEAST_PART));
    return tide_gui_color_field(g, id, r, "", value);
}
