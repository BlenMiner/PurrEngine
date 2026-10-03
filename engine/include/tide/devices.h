#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "tide/math.h"

// Input devices, as Tide's `Devices` sees them.
//
// Temporary implementation written by Claude; the project owner takes it over
// later. tidec reads the member lists below (the X-macros), so Tide and C
// always agree on the names.
//
// The platform layer (raylib, for example) updates the devices every frame with
// tide_button_set, the mouse fields, tide_touch_event and tide_pointer_poll. Once per tick, the host builds the local
// player's input from them (tide_input_sample in generated code) and then calls
// tide_devices_consume to start the next sample window. Views read them once
// per frame, through the GUI (tide/gui.h), which works out what changed since
// the last frame.

// A button between two samples, named as in Unity. `pressed` is true if the
// button was down at any point since the last sample, so a quick tap between
// two ticks is never lost.
typedef struct tide_button {
    bool pressed; // Down at any point since the last sample
    bool down;    // Went down since the last sample
    bool up;      // Went up since the last sample
    bool held;    // Down right now; platform state, not visible to Tide
} tide_button;

// Keys by physical position, named after the US layout: `w` is the key in the W
// position, whatever is printed on it. WASD works on AZERTY without remapping.
#define TIDE_KEYBOARD_KEYS(X)                                                                     \
    X(a) X(b) X(c) X(d) X(e) X(f) X(g) X(h) X(i) X(j) X(k) X(l) X(m)                               \
    X(n) X(o) X(p) X(q) X(r) X(s) X(t) X(u) X(v) X(w) X(x) X(y) X(z)                               \
    X(digit0) X(digit1) X(digit2) X(digit3) X(digit4)                                              \
    X(digit5) X(digit6) X(digit7) X(digit8) X(digit9)                                              \
    X(space) X(enter) X(escape) X(tab) X(backspace)                                                \
    X(insert) X(delete) X(home) X(end) X(pageUp) X(pageDown)                                       \
    X(upArrow) X(downArrow) X(leftArrow) X(rightArrow)                                             \
    X(leftShift) X(rightShift) X(leftCtrl) X(rightCtrl) X(leftAlt) X(rightAlt) X(capsLock)         \
    X(f1) X(f2) X(f3) X(f4) X(f5) X(f6) X(f7) X(f8) X(f9) X(f10) X(f11) X(f12)                     \
    X(minus) X(equals) X(leftBracket) X(rightBracket) X(backslash)                                 \
    X(semicolon) X(quote) X(comma) X(period) X(slash) X(backquote)                                 \
    X(numpad0) X(numpad1) X(numpad2) X(numpad3) X(numpad4)                                         \
    X(numpad5) X(numpad6) X(numpad7) X(numpad8) X(numpad9)                                         \
    X(numpadEnter) X(numpadPlus) X(numpadMinus) X(numpadMultiply) X(numpadDivide) X(numpadPeriod)

#define TIDE_MOUSE_BUTTONS(X) X(left) X(right) X(middle) X(back) X(forward)
#define TIDE_MOUSE_AXES(X) X(position) X(delta) X(scroll)

#define TIDE_DPAD_BUTTONS(X) X(up) X(down) X(left) X(right)

// Face buttons by position, as in Unity: buttonSouth is A on Xbox, Cross on
// PlayStation and B on Switch.
#define TIDE_GAMEPAD_BUTTONS(X)                                                                   \
    X(buttonSouth) X(buttonEast) X(buttonWest) X(buttonNorth)                                      \
    X(leftShoulder) X(rightShoulder) X(leftStickButton) X(rightStickButton)                        \
    X(start) X(select)
#define TIDE_GAMEPAD_STICKS(X) X(leftStick) X(rightStick)
#define TIDE_GAMEPAD_TRIGGERS(X) X(leftTrigger) X(rightTrigger)

// A touch's and the pointer's values. Positions are in this machine's window,
// which the match can't read.
#define TIDE_TOUCH_AXES(X) X(position) X(delta) X(startPosition)
#define TIDE_POINTER_AXES(X) X(position) X(delta)

// Fingers a touchscreen follows at once, as Unity's.
#define TIDE_TOUCHES 10

#define TIDE_DEVICES_MEMBER_BUTTON(name) tide_button name;
#define TIDE_DEVICES_MEMBER_FLOAT2(name) tide_float2 name;
#define TIDE_DEVICES_MEMBER_FLOAT(name) float name;

#ifndef TIDE_TEXT_MAX
#define TIDE_TEXT_MAX 32u
#endif

// Characters typed since the last poll, as Unicode code points. Unlike keys,
// they follow the keyboard layout: what's printed on the key, with Shift and
// friends applied. A poll takes as many as fit, and the platform keeps the rest
// for the next, so a long paste comes over a few. The GUI types them into
// fields, and views read them as text (`Devices.keyboard.text`); the input
// never sees them.
typedef struct tide_typed {
    uint32_t count;
    uint32_t chars[TIDE_TEXT_MAX];
} tide_typed;

typedef struct tide_keyboard {
    TIDE_KEYBOARD_KEYS(TIDE_DEVICES_MEMBER_BUTTON)
    tide_typed text; // This frame's, for views: never in a sample or the input
} tide_keyboard;

typedef struct tide_mouse {
    // Position in window pixels from the bottom left, y up, as in Unity. Delta
    // and scroll add up since the last sample; scroll y is positive away from
    // the user.
    TIDE_MOUSE_AXES(TIDE_DEVICES_MEMBER_FLOAT2)
    TIDE_MOUSE_BUTTONS(TIDE_DEVICES_MEMBER_BUTTON)
    tide_float2 poll_delta;  // Platform state: this poll's movement and scroll alone, for what views read
    tide_float2 poll_scroll;
} tide_mouse;

typedef struct tide_dpad {
    TIDE_DPAD_BUTTONS(TIDE_DEVICES_MEMBER_BUTTON)
} tide_dpad;

typedef struct tide_gamepad {
    TIDE_GAMEPAD_STICKS(TIDE_DEVICES_MEMBER_FLOAT2)   // -1 to 1 on each axis, y positive up
    TIDE_GAMEPAD_TRIGGERS(TIDE_DEVICES_MEMBER_FLOAT)  // 0 to 1
    TIDE_GAMEPAD_BUTTONS(TIDE_DEVICES_MEMBER_BUTTON)
    tide_dpad dpad;
    bool connected;
    uint8_t tide_pad[3]; // Written out: no padding the compiler adds (see below)
} tide_gamepad;

// One finger on a touchscreen, as Unity's TouchControl. `press` went down when
// it touched (Unity's Began) and up when it lifted (Ended). A finger that
// touches and lifts between two polls reads as held for one, so no tap is lost.
typedef struct tide_touch {
    tide_button press;
    int32_t id; // Unity's touchId: the same while the finger stays down, a new one for each touch; 0 for none
    // Window pixels from the bottom left, y up, like the mouse. Delta adds up
    // since the last sample.
    TIDE_TOUCH_AXES(TIDE_DEVICES_MEMBER_FLOAT2)
    tide_float2 poll_delta; // Platform state: this poll's movement alone, for what views read
    uint32_t source;        // ...the platform's own number for the finger
    uint8_t began;          // ...it touched this poll
    uint8_t lifted;         // ...it lifted this poll, after touching in it: it lifts at the next
    uint8_t tide_pad[2];
} tide_touch;

// A touchscreen's fingers: a finger keeps its slot while it touches.
typedef struct tide_touches {
    tide_touch at[TIDE_TOUCHES];
} tide_touches;

typedef struct tide_touchscreen {
    tide_touch primaryTouch; // The finger that touched while no other was the primary one, until it lifts
    tide_touches touches;
    bool connected; // This machine has a touchscreen
    uint8_t used;   // Platform state: the primary touch touched, moved or lifted this poll
    uint8_t tide_pad[2];
    int32_t last_id; // ...the last touch's id
} tide_touchscreen;

// The mouse or the touchscreen, whichever was used last, as Unity's Pointer:
// `press` is the mouse's left button or the primary touch.
typedef struct tide_pointer {
    TIDE_POINTER_AXES(TIDE_DEVICES_MEMBER_FLOAT2)
    tide_button press;
    tide_float2 poll_delta; // Platform state: this poll's movement alone, for what views read
    bool touch;             // ...it's the touchscreen
    uint8_t tide_pad[3];
} tide_pointer;

typedef struct tide_devices {
    tide_keyboard keyboard;
    tide_mouse mouse;
    tide_gamepad gamepad;
    tide_touchscreen touchscreen;
    tide_pointer pointer;
} tide_devices;

#undef TIDE_DEVICES_MEMBER_BUTTON
#undef TIDE_DEVICES_MEMBER_FLOAT2
#undef TIDE_DEVICES_MEMBER_FLOAT

// What a player's devices send is part of the match's input, which snapshots
// and state hashes compare byte by byte, so the devices have no padding the
// compiler adds, and the same size on every platform.
_Static_assert(sizeof(tide_button) == 4, "a button is four bools");
_Static_assert(sizeof(tide_gamepad) % 4 == 0 && sizeof(tide_devices) % 4 == 0, "devices have no padding the compiler adds");
_Static_assert(sizeof(tide_touch) == 48 && sizeof(tide_touchscreen) == 48 * (1 + TIDE_TOUCHES) + 8
                   && sizeof(tide_pointer) == 32,
               "touches and the pointer have no padding the compiler adds");

// Platform layer: report a button's current state. Call every frame for every
// button; presses and releases latch until the next tide_devices_consume.
static inline void tide_button_set(tide_button *b, const bool down_now)
{
    if (down_now && !b->held) b->down = true;
    if (!down_now && b->held) b->up = true;
    if (down_now) b->pressed = true;
    b->held = down_now;
}

typedef enum tide_touch_phase {
    TIDE_TOUCH_BEGAN,
    TIDE_TOUCH_MOVED,
    TIDE_TOUCH_ENDED,
    TIDE_TOUCH_CANCELED, // The system took the finger away (a gesture of its own, the window lost it): it lifts where it was
} tide_touch_phase;

// Platform layer, once per poll, before its touches: lifts the fingers that
// touched and lifted in the last poll, and starts this poll's movement.
void tide_touches_poll(tide_touchscreen *s);

// Platform layer: a finger touched, moved or lifted at `position` (window
// pixels from the bottom left, y up). `source` is the platform's own number for
// the finger while it touches, which it may give the next one. A finger that
// touches while every slot is taken is left out.
void tide_touch_event(tide_touchscreen *s, tide_touch_phase phase, uint32_t source, tide_float2 position);

// Platform layer, once per poll, after the mouse and the touches: the pointer
// follows the touchscreen when the primary touch was used this poll, and the
// mouse when `mouse_used` (it moved or scrolled, or a button went down or up).
void tide_pointer_poll(tide_devices *d, bool mouse_used);

// After sampling input: buttons restart from their current state, mouse
// delta and scroll reset to zero, and lifted fingers leave their slots.
void tide_devices_consume(tide_devices *d);

// touches[i] in generated code: an empty touch past the last.
static inline tide_touch tide_touch_at(const tide_touches touches, const int32_t i)
{
    return i >= 0 && i < TIDE_TOUCHES ? touches.at[i] : (tide_touch){0};
}
