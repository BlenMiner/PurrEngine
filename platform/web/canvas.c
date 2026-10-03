// The web's window (see src/window.h): the page's canvas, with a WebGL 2
// context, and the page's input, both through its JavaScript (tide.js, which
// tide_web.h declares) instead of Emscripten.
//
// The screen is the canvas in CSS pixels, which the program draws and reads
// the mouse in, as desktop builds do in the display's logical pixels. It
// renders the canvas's own pixels, devicePixelRatio times as many, so it's
// sharp.

#include "window.h"

#include <stdio.h>

#include "tide_web.h"

#define COUNT_OF(array) (sizeof(array) / sizeof((array)[0]))

// Every Tide key with the DOM's `code`, which names physical positions after
// the US layout, as Tide does: never the character a key types, which follows
// the keyboard layout.
static const struct {
    tide_key key;
    const char *code;
} keys[] = {
    {TIDE_KEY_a, "KeyA"}, {TIDE_KEY_b, "KeyB"}, {TIDE_KEY_c, "KeyC"}, {TIDE_KEY_d, "KeyD"}, {TIDE_KEY_e, "KeyE"},
    {TIDE_KEY_f, "KeyF"}, {TIDE_KEY_g, "KeyG"}, {TIDE_KEY_h, "KeyH"}, {TIDE_KEY_i, "KeyI"}, {TIDE_KEY_j, "KeyJ"},
    {TIDE_KEY_k, "KeyK"}, {TIDE_KEY_l, "KeyL"}, {TIDE_KEY_m, "KeyM"}, {TIDE_KEY_n, "KeyN"}, {TIDE_KEY_o, "KeyO"},
    {TIDE_KEY_p, "KeyP"}, {TIDE_KEY_q, "KeyQ"}, {TIDE_KEY_r, "KeyR"}, {TIDE_KEY_s, "KeyS"}, {TIDE_KEY_t, "KeyT"},
    {TIDE_KEY_u, "KeyU"}, {TIDE_KEY_v, "KeyV"}, {TIDE_KEY_w, "KeyW"}, {TIDE_KEY_x, "KeyX"}, {TIDE_KEY_y, "KeyY"},
    {TIDE_KEY_z, "KeyZ"},
    {TIDE_KEY_digit0, "Digit0"}, {TIDE_KEY_digit1, "Digit1"}, {TIDE_KEY_digit2, "Digit2"}, {TIDE_KEY_digit3, "Digit3"},
    {TIDE_KEY_digit4, "Digit4"}, {TIDE_KEY_digit5, "Digit5"}, {TIDE_KEY_digit6, "Digit6"}, {TIDE_KEY_digit7, "Digit7"},
    {TIDE_KEY_digit8, "Digit8"}, {TIDE_KEY_digit9, "Digit9"},
    {TIDE_KEY_space, "Space"}, {TIDE_KEY_enter, "Enter"}, {TIDE_KEY_escape, "Escape"}, {TIDE_KEY_tab, "Tab"},
    {TIDE_KEY_backspace, "Backspace"},
    {TIDE_KEY_insert, "Insert"}, {TIDE_KEY_delete, "Delete"}, {TIDE_KEY_home, "Home"}, {TIDE_KEY_end, "End"},
    {TIDE_KEY_pageUp, "PageUp"}, {TIDE_KEY_pageDown, "PageDown"},
    {TIDE_KEY_upArrow, "ArrowUp"}, {TIDE_KEY_downArrow, "ArrowDown"}, {TIDE_KEY_leftArrow, "ArrowLeft"},
    {TIDE_KEY_rightArrow, "ArrowRight"},
    {TIDE_KEY_leftShift, "ShiftLeft"}, {TIDE_KEY_rightShift, "ShiftRight"}, {TIDE_KEY_leftCtrl, "ControlLeft"},
    {TIDE_KEY_rightCtrl, "ControlRight"}, {TIDE_KEY_leftAlt, "AltLeft"}, {TIDE_KEY_rightAlt, "AltRight"},
    {TIDE_KEY_capsLock, "CapsLock"},
    {TIDE_KEY_f1, "F1"}, {TIDE_KEY_f2, "F2"}, {TIDE_KEY_f3, "F3"}, {TIDE_KEY_f4, "F4"}, {TIDE_KEY_f5, "F5"},
    {TIDE_KEY_f6, "F6"}, {TIDE_KEY_f7, "F7"}, {TIDE_KEY_f8, "F8"}, {TIDE_KEY_f9, "F9"}, {TIDE_KEY_f10, "F10"},
    {TIDE_KEY_f11, "F11"}, {TIDE_KEY_f12, "F12"},
    {TIDE_KEY_minus, "Minus"}, {TIDE_KEY_equals, "Equal"}, {TIDE_KEY_leftBracket, "BracketLeft"},
    {TIDE_KEY_rightBracket, "BracketRight"}, {TIDE_KEY_backslash, "Backslash"}, {TIDE_KEY_semicolon, "Semicolon"},
    {TIDE_KEY_quote, "Quote"}, {TIDE_KEY_comma, "Comma"}, {TIDE_KEY_period, "Period"}, {TIDE_KEY_slash, "Slash"},
    {TIDE_KEY_backquote, "Backquote"},
    {TIDE_KEY_numpad0, "Numpad0"}, {TIDE_KEY_numpad1, "Numpad1"}, {TIDE_KEY_numpad2, "Numpad2"},
    {TIDE_KEY_numpad3, "Numpad3"}, {TIDE_KEY_numpad4, "Numpad4"}, {TIDE_KEY_numpad5, "Numpad5"},
    {TIDE_KEY_numpad6, "Numpad6"}, {TIDE_KEY_numpad7, "Numpad7"}, {TIDE_KEY_numpad8, "Numpad8"},
    {TIDE_KEY_numpad9, "Numpad9"},
    {TIDE_KEY_numpadEnter, "NumpadEnter"}, {TIDE_KEY_numpadPlus, "NumpadAdd"}, {TIDE_KEY_numpadMinus, "NumpadSubtract"},
    {TIDE_KEY_numpadMultiply, "NumpadMultiply"}, {TIDE_KEY_numpadDivide, "NumpadDivide"},
    {TIDE_KEY_numpadPeriod, "NumpadDecimal"},
};

_Static_assert(COUNT_OF(keys) == TIDE_KEY_COUNT, "every key in devices.h needs the DOM's code");

// Each button's index in the browser's standard gamepad mapping.
static const struct {
    tide_pad_button button;
    int web;
} pad_buttons[] = {
    {TIDE_PAD_buttonSouth, 0},     {TIDE_PAD_buttonEast, 1},        {TIDE_PAD_buttonWest, 2},
    {TIDE_PAD_buttonNorth, 3},     {TIDE_PAD_leftShoulder, 4},      {TIDE_PAD_rightShoulder, 5},
    {TIDE_PAD_leftStickButton, 10}, {TIDE_PAD_rightStickButton, 11}, {TIDE_PAD_start, 9},
    {TIDE_PAD_select, 8},          {TIDE_PAD_dpad_up, 12},          {TIDE_PAD_dpad_down, 13},
    {TIDE_PAD_dpad_left, 14},      {TIDE_PAD_dpad_right, 15},
};

_Static_assert(COUNT_OF(pad_buttons) == TIDE_PAD_COUNT, "every gamepad button needs the browser's index");

static const tide_gpu *gpu = &tide_gpu_gl;

bool tide_window_open(const tide_window_desc *desc)
{
    // A resizable window is a canvas that fills the page. Tests keep the size
    // they asked for.
    const int canvas = tide_web_init_canvas(desc->width, desc->height, !desc->hidden);
    if (canvas == TIDE_WEB_NO_CANVAS) {
        fprintf(stderr, "tide: this browser has neither WebGPU nor WebGL 2, or not the one the page asked for\n");
        return false;
    }
    gpu = canvas == TIDE_WEB_WEBGPU ? &tide_gpu_webgpu : &tide_gpu_gl;
    for (size_t i = 0; i < COUNT_OF(keys); i++) tide_web_watch_key((int)keys[i].key, keys[i].code);
    return true;
}

const tide_gpu *tide_window_gpu(void)
{
    return gpu;
}

// The canvas can't close: the page goes, and the program with it.
void tide_window_close(void) { }

bool tide_window_should_close(void)
{
    return false;
}

bool tide_window_unseen(void)
{
    return tide_web_hidden();
}

void tide_window_size(int *width, int *height)
{
    *width = tide_web_canvas_width();
    *height = tide_web_canvas_height();
    if (*width < 1) *width = 1;
    if (*height < 1) *height = 1;
}

void tide_window_pixel_size(int *width, int *height)
{
    *width = tide_web_canvas_pixel_width();
    *height = tide_web_canvas_pixel_height();
    if (*width < 1) *width = 1;
    if (*height < 1) *height = 1;
}

// The browser shows the frame when the frame's callback returns, and the
// page keeps its input as it comes.
void tide_window_present(void) { }

// The page waits between frames (tide.js).
void tide_window_wait(const double seconds)
{
    (void)seconds;
}

bool tide_window_key_held(const tide_key key)
{
    return tide_web_key_held((int)key);
}

void tide_window_mouse_state(tide_window_mouse *out)
{
    out->x = tide_web_mouse_x();
    out->y = tide_web_mouse_y();
    out->wheel_x = tide_web_take_wheel_x();
    out->wheel_y = tide_web_take_wheel_y();
    out->buttons = (uint32_t)tide_web_mouse_buttons();
}

void tide_window_gamepad_state(tide_window_gamepad *out)
{
    // The browser's standard mapping: axes 0 to 3 are the sticks, and the
    // triggers are analog buttons 6 and 7.
    const int pad = 0;
    *out = (tide_window_gamepad){.connected = tide_web_gamepad_connected(pad)};
    if (!out->connected) return;
    out->left_x = tide_web_gamepad_axis(pad, 0);
    out->left_y = tide_web_gamepad_axis(pad, 1);
    out->right_x = tide_web_gamepad_axis(pad, 2);
    out->right_y = tide_web_gamepad_axis(pad, 3);
    out->left_trigger = tide_web_gamepad_button(pad, 6);
    out->right_trigger = tide_web_gamepad_button(pad, 7);
    for (size_t i = 0; i < COUNT_OF(pad_buttons); i++) {
        if (tide_web_gamepad_button(pad, pad_buttons[i].web) > 0.5f) out->buttons |= 1u << pad_buttons[i].button;
    }
}

bool tide_window_touchscreen(void)
{
    return tide_web_touchscreen();
}

bool tide_window_take_touch(tide_touch_report *report)
{
    if (!tide_web_take_touch()) return false;
    report->phase = (tide_touch_phase)tide_web_touch_phase();
    report->source = tide_web_touch_source();
    report->x = tide_web_touch_x(); // CSS pixels from the canvas's top left, as the mouse
    report->y = tide_web_touch_y();
    return true;
}

uint32_t tide_window_take_char(void)
{
    const int c = tide_web_take_char();
    return c > 0 ? (uint32_t)c : 0;
}

// The page types what's pasted (its `paste` event, in tide.js).
const char *tide_window_take_paste(void)
{
    return NULL;
}

void tide_window_copy(const char *text)
{
    tide_web_copy(text);
}

void tide_window_typing(const bool typing)
{
    tide_web_typing(typing);
}
