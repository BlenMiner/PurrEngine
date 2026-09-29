#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "purr/math.h"

// Input devices, as PurrLang's `Devices` sees them.
//
// Temporary implementation written by Claude; the project owner takes it over
// later. purrc reads the member lists below (the X-macros), so PurrLang and C
// always agree on the names.
//
// The platform layer (raylib, for example) updates the devices every frame with
// purr_button_set and the mouse fields. Once per tick, the host builds the local
// player's input from them (purr_input_sample in generated code) and then calls
// purr_devices_consume to start the next sample window. Views read them once
// per frame, through the GUI (purr/gui.h), which works out what changed since
// the last frame.

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
    purr_float2 poll_delta;  // Platform state: this poll's movement and scroll alone, for what views read
    purr_float2 poll_scroll;
} purr_mouse;

typedef struct purr_dpad {
    PURR_DPAD_BUTTONS(PURR_DEVICES_MEMBER_BUTTON)
} purr_dpad;

typedef struct purr_gamepad {
    PURR_GAMEPAD_STICKS(PURR_DEVICES_MEMBER_FLOAT2)   // -1 to 1 on each axis, y positive up
    PURR_GAMEPAD_TRIGGERS(PURR_DEVICES_MEMBER_FLOAT)  // 0 to 1
    PURR_GAMEPAD_BUTTONS(PURR_DEVICES_MEMBER_BUTTON)
    purr_dpad dpad;
    bool connected;
    uint8_t purr_pad[3]; // Written out: no padding the compiler adds (see below)
} purr_gamepad;

#ifndef PURR_TEXT_MAX
#define PURR_TEXT_MAX 32u
#endif

// Characters typed since the last poll, as Unicode code points. Unlike keys,
// they follow the keyboard layout: what's printed on the key, with Shift and
// friends applied. The GUI types them into fields; the input never sees them.
typedef struct purr_typed {
    uint32_t count;
    uint32_t chars[PURR_TEXT_MAX];
} purr_typed;

typedef struct purr_devices {
    purr_keyboard keyboard;
    purr_mouse mouse;
    purr_gamepad gamepad;
    purr_typed text; // Platform state, not visible to PurrLang
} purr_devices;

#undef PURR_DEVICES_MEMBER_BUTTON
#undef PURR_DEVICES_MEMBER_FLOAT2
#undef PURR_DEVICES_MEMBER_FLOAT

// What a player's devices send is part of the match's input, which snapshots
// and state hashes compare byte by byte, so the devices have no padding the
// compiler adds, and the same size on every platform.
_Static_assert(sizeof(purr_button) == 4, "a button is four bools");
_Static_assert(sizeof(purr_gamepad) % 4 == 0 && sizeof(purr_devices) % 4 == 0, "devices have no padding the compiler adds");

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
