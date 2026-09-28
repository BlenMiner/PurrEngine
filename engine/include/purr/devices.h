#pragma once

#include <stdbool.h>

#include "purr/math.h"

// Input devices, as PurrLang's `Devices` sees them in an input's constructor.
//
// Temporary implementation written by Claude; the project owner takes it over
// later. purrc reads the member lists below (the X-macros), so PurrLang and C
// always agree on the names.
//
// The platform layer (raylib, for example) updates the devices every frame with
// purr_button_set and the mouse fields. Once per tick, the host builds the local
// player's input from them (purr_input_sample in generated code) and then calls
// purr_devices_consume to start the next sample window.

// A button between two samples, named as in Unity. `pressed` is true if the
// button was down at any point since the last sample, so a quick tap between
// two ticks is never lost.
typedef struct purr_button {
    bool pressed; // Down at any point since the last sample
    bool down;    // Went down since the last sample
    bool up;      // Went up since the last sample
    bool held;    // Down right now; platform state, not visible to PurrLang
} purr_button;

// Keys by physical position, named after the US layout: `w` is the key in the W
// position, whatever is printed on it. WASD works on AZERTY without remapping.
#define PURR_KEYBOARD_KEYS(X)                                                                     \
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

#define PURR_MOUSE_BUTTONS(X) X(left) X(right) X(middle) X(back) X(forward)
#define PURR_MOUSE_AXES(X) X(position) X(delta) X(scroll)

#define PURR_DPAD_BUTTONS(X) X(up) X(down) X(left) X(right)

// Face buttons by position, as in Unity: buttonSouth is A on Xbox, Cross on
// PlayStation and B on Switch.
#define PURR_GAMEPAD_BUTTONS(X)                                                                   \
    X(buttonSouth) X(buttonEast) X(buttonWest) X(buttonNorth)                                      \
    X(leftShoulder) X(rightShoulder) X(leftStickButton) X(rightStickButton)                        \
    X(start) X(select)
#define PURR_GAMEPAD_STICKS(X) X(leftStick) X(rightStick)
#define PURR_GAMEPAD_TRIGGERS(X) X(leftTrigger) X(rightTrigger)

#define PURR_DEVICES_MEMBER_BUTTON(name) purr_button name;
#define PURR_DEVICES_MEMBER_FLOAT2(name) purr_float2 name;
#define PURR_DEVICES_MEMBER_FLOAT(name) float name;

typedef struct purr_keyboard {
    PURR_KEYBOARD_KEYS(PURR_DEVICES_MEMBER_BUTTON)
} purr_keyboard;

typedef struct purr_mouse {
    // Position in window pixels from the bottom left, y up, as in Unity. Delta
    // and scroll add up since the last sample; scroll y is positive away from
    // the user.
    PURR_MOUSE_AXES(PURR_DEVICES_MEMBER_FLOAT2)
    PURR_MOUSE_BUTTONS(PURR_DEVICES_MEMBER_BUTTON)
} purr_mouse;

typedef struct purr_dpad {
    PURR_DPAD_BUTTONS(PURR_DEVICES_MEMBER_BUTTON)
} purr_dpad;

typedef struct purr_gamepad {
    bool connected;
    PURR_GAMEPAD_STICKS(PURR_DEVICES_MEMBER_FLOAT2)   // -1 to 1 on each axis, y positive up
    PURR_GAMEPAD_TRIGGERS(PURR_DEVICES_MEMBER_FLOAT)  // 0 to 1
    PURR_GAMEPAD_BUTTONS(PURR_DEVICES_MEMBER_BUTTON)
    purr_dpad dpad;
} purr_gamepad;

typedef struct purr_devices {
    purr_keyboard keyboard;
    purr_mouse mouse;
    purr_gamepad gamepad;
} purr_devices;

#undef PURR_DEVICES_MEMBER_BUTTON
#undef PURR_DEVICES_MEMBER_FLOAT2
#undef PURR_DEVICES_MEMBER_FLOAT

// Platform layer: report a button's current state. Call every frame for every
// button; presses and releases latch until the next purr_devices_consume.
static inline void purr_button_set(purr_button *b, const bool down_now)
{
    if (down_now && !b->held) b->down = true;
    if (!down_now && b->held) b->up = true;
    if (down_now) b->pressed = true;
    b->held = down_now;
}

// After sampling input: buttons restart from their current state, and mouse
// delta and scroll reset to zero.
void purr_devices_consume(purr_devices *d);
