#pragma once

// What the page's JavaScript (platform/web/purr.js) gives WebAssembly in web
// builds: the canvas and its WebGL 2 context, input, and the frame loop. The
// GL functions themselves are imported by their C names (see purr.js).

#include <stdbool.h>

#define PURR_WEB_IMPORT(name) __attribute__((import_module("purr"), import_name(#name)))

// Sizes the canvas and creates its WebGL 2 context: `width` x `height` pixels,
// or the whole page when `resizable`. False if the browser has no WebGL 2.
PURR_WEB_IMPORT(init_canvas) bool purr_web_init_canvas(int width, int height, bool resizable);
PURR_WEB_IMPORT(canvas_width) int purr_web_canvas_width(void);
PURR_WEB_IMPORT(canvas_height) int purr_web_canvas_height(void);

// The mouse over the canvas, in canvas pixels from its top left. Buttons are a
// bit mask in raylib's order: left, right, middle, back, forward. A button
// pressed since the last call reads as held, even if it's already up again.
PURR_WEB_IMPORT(mouse_x) float purr_web_mouse_x(void);
PURR_WEB_IMPORT(mouse_y) float purr_web_mouse_y(void);
PURR_WEB_IMPORT(mouse_buttons) int purr_web_mouse_buttons(void);
// Scrolling since the last call, positive away from the user.
PURR_WEB_IMPORT(take_wheel_x) float purr_web_take_wheel_x(void);
PURR_WEB_IMPORT(take_wheel_y) float purr_web_take_wheel_y(void);

// Keys by the DOM's `code`, which names physical positions: `index` is ours.
// Held keys are released when the page loses focus. A key pressed since the
// last call reads as held, even if it's already up again.
PURR_WEB_IMPORT(watch_key) void purr_web_watch_key(int index, const char *code);
PURR_WEB_IMPORT(key_held) bool purr_web_key_held(int index);

// The next character typed, as a Unicode code point, or 0 once there are no
// more. They follow the keyboard layout, unlike keys.
PURR_WEB_IMPORT(take_char) int purr_web_take_char(void);

// Gamepads in the browser's standard mapping: axes -1 to 1 (y down), buttons
// 0 to 1 (triggers are analog buttons 6 and 7).
PURR_WEB_IMPORT(gamepad_connected) bool purr_web_gamepad_connected(int pad);
PURR_WEB_IMPORT(gamepad_axis) float purr_web_gamepad_axis(int pad, int axis);
PURR_WEB_IMPORT(gamepad_button) float purr_web_gamepad_button(int pad, int button);

// Starts calling the exported purr_web_frame, on animation frames or, with
// `timer_frames`, as fast as timers allow (headless pages have no animation
// frames). Doesn't return: it unwinds main's stack back to the browser.
PURR_WEB_IMPORT(run) __attribute__((noreturn)) void purr_web_run(bool timer_frames);
PURR_WEB_IMPORT(stop) void purr_web_stop(void);

// Runs JavaScript, for tests that need to fake browser events.
PURR_WEB_IMPORT(eval) void purr_web_eval(const char *script);
