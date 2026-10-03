#pragma once

// Touchscreens on Windows (touch_win32.c), where GLFW has no touch: the
// window's pointer messages, taken before GLFW's. It's apart from
// desktop/glfw.c for the system's headers, which that file does without.

#ifdef _WIN32

#include <stdbool.h>

#include "window.h"

// Takes the touches of `window` (an HWND): they're no longer the mouse's.
void tide_win32_touch_attach(void *window);
// Whether this machine has a touchscreen.
bool tide_win32_touchscreen(void);
// The next touch event since the last call, false when there's none: where,
// from 0 to 1 across the window's inside, from the top left.
bool tide_win32_take_touch(tide_touch_report *report);

#endif
