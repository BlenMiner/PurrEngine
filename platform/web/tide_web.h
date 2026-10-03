#pragma once

// What the page's JavaScript (platform/web/tide.js) gives WebAssembly in web
// builds: the canvas and its WebGL 2 context, input, and the frame loop. The
// GL functions themselves are imported by their C names (see tide.js).

#include <stdbool.h>
#include <stdint.h>

#define TIDE_WEB_IMPORT(name) __attribute__((import_module("tide"), import_name(#name)))

// Sizes the canvas and creates its WebGL 2 context: `width` x `height` CSS
// pixels, or the whole page when `resizable`. False if the browser has no
// WebGL 2.
TIDE_WEB_IMPORT(init_canvas) bool tide_web_init_canvas(int width, int height, bool resizable);
// Its size in CSS pixels, which the program counts in...
TIDE_WEB_IMPORT(canvas_width) int tide_web_canvas_width(void);
TIDE_WEB_IMPORT(canvas_height) int tide_web_canvas_height(void);
// ...and in the pixels it renders: devicePixelRatio times as many.
TIDE_WEB_IMPORT(canvas_pixel_width) int tide_web_canvas_pixel_width(void);
TIDE_WEB_IMPORT(canvas_pixel_height) int tide_web_canvas_pixel_height(void);

// The mouse over the canvas, in CSS pixels from its top left. Buttons are a
// bit mask in tide_mouse's order: left, right, middle, back, forward. A button
// pressed since the last call reads as held, even if it's already up again.
TIDE_WEB_IMPORT(mouse_x) float tide_web_mouse_x(void);
TIDE_WEB_IMPORT(mouse_y) float tide_web_mouse_y(void);
TIDE_WEB_IMPORT(mouse_buttons) int tide_web_mouse_buttons(void);
// Scrolling since the last call, positive away from the user.
TIDE_WEB_IMPORT(take_wheel_x) float tide_web_take_wheel_x(void);
TIDE_WEB_IMPORT(take_wheel_y) float tide_web_take_wheel_y(void);

// Fingers on the canvas, from its pointer events, in the order they happened:
// take_touch moves to the next one since the last call (false once there are
// none), and the others read it. Phases are tide_touch_phase's; positions are
// in CSS pixels from the canvas's top left, as the mouse's. A finger is never
// the mouse.
TIDE_WEB_IMPORT(touchscreen) bool tide_web_touchscreen(void);
TIDE_WEB_IMPORT(take_touch) bool tide_web_take_touch(void);
TIDE_WEB_IMPORT(touch_phase) int tide_web_touch_phase(void);
TIDE_WEB_IMPORT(touch_source) uint32_t tide_web_touch_source(void);
TIDE_WEB_IMPORT(touch_x) float tide_web_touch_x(void);
TIDE_WEB_IMPORT(touch_y) float tide_web_touch_y(void);

// Keys by the DOM's `code`, which names physical positions: `index` is ours.
// Held keys are released when the page loses focus. A key pressed since the
// last call reads as held, even if it's already up again.
TIDE_WEB_IMPORT(watch_key) void tide_web_watch_key(int index, const char *code);
TIDE_WEB_IMPORT(key_held) bool tide_web_key_held(int index);

// The next character typed, as a Unicode code point, or 0 once there are no
// more. They follow the keyboard layout, unlike keys. What's pasted comes as
// characters typed too.
TIDE_WEB_IMPORT(take_char) int tide_web_take_char(void);

// Whether the player is typing into the GUI: the page focuses a field of its
// own, hidden, so phones show their keyboard, and what's typed there comes as
// characters typed (see tide_platform_typing).
TIDE_WEB_IMPORT(typing) void tide_web_typing(bool typing);

// Puts text on the clipboard (see tide_platform_copy).
TIDE_WEB_IMPORT(copy) void tide_web_copy(const char *text);

// Gamepads in the browser's standard mapping: axes -1 to 1 (y down), buttons
// 0 to 1 (triggers are analog buttons 6 and 7).
TIDE_WEB_IMPORT(gamepad_connected) bool tide_web_gamepad_connected(int pad);
TIDE_WEB_IMPORT(gamepad_axis) float tide_web_gamepad_axis(int pad, int axis);
TIDE_WEB_IMPORT(gamepad_button) float tide_web_gamepad_button(int pad, int button);

// Starts calling the exported tide_web_frame, on animation frames or, with
// `timer_frames`, as fast as timers allow (headless pages have no animation
// frames). While the page is hidden, frames come from a worker's timers, since
// browsers slow the page's own, each when tide_web_frame said the next is
// needed (tide_platform_next_frame). Doesn't return: it unwinds main's stack
// back to the browser.
TIDE_WEB_IMPORT(run) __attribute__((noreturn)) void tide_web_run(bool timer_frames);
TIDE_WEB_IMPORT(stop) void tide_web_stop(void);

// Threads the program can run on: the CPU's cores when the page can share the
// program's memory with workers (it's cross-origin isolated), or else 1. Each
// thread pthread_create makes is a worker (wasi-threads' thread-spawn).
TIDE_WEB_IMPORT(threads) uint32_t tide_web_threads(void);

// Whether the page is hidden: another tab in front, or the window minimized.
// Frames go on, but nobody sees what they draw.
TIDE_WEB_IMPORT(hidden) bool tide_web_hidden(void);

// Runs JavaScript, for tests that need to fake browser events or ask the page
// something: whether its value is truthy.
TIDE_WEB_IMPORT(eval) bool tide_web_eval(const char *script);

// Rooms (platform/src/rooms.c): matches players find by a code, through the
// relay, with WebRTC data channels between them. One at a time: opening one
// closes the last. Each has a number, which the calls about it take; the host
// is player 0 to those who join, and they're 1 and up to it.
TIDE_WEB_IMPORT(room_host) uint32_t tide_web_room_host(void);
TIDE_WEB_IMPORT(room_join) uint32_t tide_web_room_join(const char *code);
// Host migration: room `code` again, with its `key` (see tide_platform_room_migrate)
TIDE_WEB_IMPORT(room_migrate) uint32_t tide_web_room_migrate(const char *code, const char *key);
// 1 hosting it, 2 joining its host, -1 failed, 0 not known yet
TIDE_WEB_IMPORT(room_migrated) int32_t tide_web_room_migrated(uint32_t number);
// The key of the room this program hosts, into `out` (TIDE_ROOM_KEY_LENGTH + 1 bytes)
TIDE_WEB_IMPORT(room_key) void tide_web_room_key(char *out);
// The match in room `number`, which this program hosts, ended: the relay keeps it as ended a while
TIDE_WEB_IMPORT(room_end) void tide_web_room_end(uint32_t number);
TIDE_WEB_IMPORT(room_close) void tide_web_room_close(uint32_t room);
// The room's code into `out` (7 bytes), "" while it has none.
TIDE_WEB_IMPORT(room_code) void tide_web_room_code(char *out);
// The room joined isn't one, or it couldn't be reached.
TIDE_WEB_IMPORT(room_failed) bool tide_web_room_failed(void);
TIDE_WEB_IMPORT(room_send) void tide_web_room_send(uint32_t room, uint32_t to, const void *data, uint32_t size);
// The next datagram that arrived: its size, or 0 when there's none.
TIDE_WEB_IMPORT(room_receive) uint32_t tide_web_room_receive(uint32_t room, uint32_t *from, void *data,
                                                             uint32_t capacity);

// Hot reloading, under tide run --web: what the page's last program left this
// one (tide_reload_save in platform/src/reload.c), 0 bytes if nothing, and a
// copy of it.
TIDE_WEB_IMPORT(resume_size) uint32_t tide_web_resume_size(void);
TIDE_WEB_IMPORT(resume_copy) void tide_web_resume_copy(void *to);
