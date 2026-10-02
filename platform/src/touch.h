#pragma once

// Touchscreens on desktop, where raylib (GLFW) has no touch of its own. A file
// apart from platform.c, which can't include the system's headers alongside
// raylib's.

#include <stdbool.h>
#include <stdint.h>

#include "tide/devices.h"

// One touch event, as the system reported it: where, from 0 to 1 across the
// window's inside, from the top left.
typedef struct tide_touch_report {
    tide_touch_phase phase;
    uint32_t source; // The system's number for the finger
    float x, y;
} tide_touch_report;

#ifdef _WIN32
// Takes the touches of `window` (an HWND): they're no longer the mouse's.
void tide_win32_touch_attach(void *window);
// Whether this machine has a touchscreen.
bool tide_win32_touchscreen(void);
// The next touch event since the last call, false when there's none.
bool tide_win32_take_touch(tide_touch_report *report);
#endif
