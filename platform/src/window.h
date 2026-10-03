#pragma once

// What the platform layer (platform.c) gets from the system it runs on: a
// window with an OpenGL context that's current, and what its input devices
// say. Each build has one of them: desktop/glfw.c (GLFW), web/canvas.c (the
// page, through tide.js) or android/activity.c (NativeActivity and EGL).
//
// Pixels are the display's logical ones (see tide/platform.h), from the
// window's top left, y down, but for tide_window_pixel_size.

#include <stdbool.h>
#include <stdint.h>

#include "tide/devices.h"
#include "tide/platform.h"
#include "gpu.h"

// Opens the window, with what draws in it: an OpenGL 3.3, OpenGL ES 3 or
// WebGL 2 context that's current or, on the web, WebGPU where the browser has
// it. False, having said why, if there's none to be had.
bool tide_window_open(const tide_window_desc *desc);
void tide_window_close(void);

// What draws in the window that's open.
const tide_gpu *tide_window_gpu(void);

// Whether the program is to end: the user closed the window, or the system
// took the app away.
bool tide_window_should_close(void);

// Nobody sees it: it's minimized, the app is in the background, or the page
// is hidden. Frames go on, and draw nothing.
bool tide_window_unseen(void);

// Its size, and in the pixels it renders: as many times more as the display
// is scaled.
void tide_window_size(int *width, int *height);
void tide_window_pixel_size(int *width, int *height);

// Shows what the frame drew, at the display's pace where the window is seen,
// then takes what the system says happened since the last call: the calls
// below read the devices as of then.
void tide_window_present(void);

// Waits up to `seconds` for the system to say something, while the window is
// unseen and has no display's pace to wait for.
void tide_window_wait(double seconds);

// Keys, by physical position, in the order tide_keyboard has them.
#define TIDE_WINDOW_KEY(name) TIDE_KEY_##name,
typedef enum tide_key { TIDE_KEYBOARD_KEYS(TIDE_WINDOW_KEY) TIDE_KEY_COUNT } tide_key;
#undef TIDE_WINDOW_KEY

// Whether `key` is held, or was pressed since the last call for it, even if
// it's up again: a tap between two frames reads as held for one.
bool tide_window_key_held(tide_key key);

// The mouse. Buttons are bits in tide_mouse's order: left, right, middle,
// back, forward, each held or pressed since the last call. The wheel is what
// it turned since the last call, positive y away from the user.
typedef struct tide_window_mouse {
    float x, y;
    float wheel_x, wheel_y;
    uint32_t buttons;
} tide_window_mouse;

void tide_window_mouse_state(tide_window_mouse *out);

// The first gamepad's buttons, in tide_gamepad's order, the d-pad's after.
#define TIDE_WINDOW_PAD(name) TIDE_PAD_##name,
#define TIDE_WINDOW_DPAD(name) TIDE_PAD_dpad_##name,
typedef enum tide_pad_button {
    TIDE_GAMEPAD_BUTTONS(TIDE_WINDOW_PAD) TIDE_DPAD_BUTTONS(TIDE_WINDOW_DPAD) TIDE_PAD_COUNT
} tide_pad_button;
#undef TIDE_WINDOW_PAD
#undef TIDE_WINDOW_DPAD

// ...and all it says: sticks from -1 to 1, y down as gamepads report it,
// triggers from 0 to 1, and its buttons as bits (1 << tide_pad_button).
typedef struct tide_window_gamepad {
    bool connected;
    float left_x, left_y, right_x, right_y;
    float left_trigger, right_trigger;
    uint32_t buttons;
} tide_window_gamepad;

void tide_window_gamepad_state(tide_window_gamepad *out);

// One touch event, as the system reported it.
typedef struct tide_touch_report {
    tide_touch_phase phase;
    uint32_t source; // The system's number for the finger
    float x, y;
} tide_touch_report;

// Whether this machine has a touchscreen.
bool tide_window_touchscreen(void);
// The next touch event since the last call, false when there's none.
bool tide_window_take_touch(tide_touch_report *report);

// The next character typed, as a Unicode code point, or 0 once there are no
// more. They follow the keyboard layout, unlike keys.
uint32_t tide_window_take_char(void);

// The clipboard's text if the player pressed the system's shortcut for
// pasting (Ctrl+V, Cmd+V) since the last call, as UTF-8 that stays until the
// next call; else NULL. Always NULL on the web, where the page types what's
// pasted (tide.js).
const char *tide_window_take_paste(void);

// Puts `text` on the clipboard.
void tide_window_copy(const char *text);

// Shows the system's keyboard, where there is one, or hides it.
void tide_window_typing(bool typing);
