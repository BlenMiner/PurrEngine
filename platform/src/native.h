#pragma once

// What the platform layer gets from code that talks to the system itself,
// apart from platform.c, which can't include the system's headers alongside
// raylib's: touchscreens on Windows (touch_win32.c), where GLFW has no touch,
// and Android's activity (platform/android/raylib/rcore_android_tide.c, built
// into raylib).

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

#if defined(_WIN32) || defined(__ANDROID__)
// Whether this machine has a touchscreen.
bool tide_native_touchscreen(void);
// The next touch event since the last call, false when there's none.
bool tide_native_take_touch(tide_touch_report *report);
#endif

#ifdef _WIN32
// Takes the touches of `window` (an HWND): they're no longer the mouse's.
void tide_win32_touch_attach(void *window);
#endif

#ifdef __ANDROID__
// Waits up to `seconds` for something to change while the app has no window
// (it's in the background): input, or the activity giving it one again.
void tide_android_wait(double seconds);
#endif
