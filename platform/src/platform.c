#include "tide/platform.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <raylib.h>
#include <rlgl.h>

#include "tide/page.h"
#include "native.h"

#ifdef __wasm__
#include "tide_web.h" // The page's JavaScript, which web builds use instead of raylib for input and frames
#elif !defined(__ANDROID__)
// GLFW, which raylib's desktop windows run on, built into raylib: waiting on
// the window's events, rather than sleeping past them. raylib's window is the
// one whose context is current (GetWindowHandle is the system's, an HWND on
// Windows).
typedef struct GLFWwindow GLFWwindow;
void glfwWaitEventsTimeout(double timeout);
int glfwWindowShouldClose(GLFWwindow *window);
GLFWwindow *glfwGetCurrentContext(void);
#endif

#define COUNT_OF(array) (sizeof(array) / sizeof((array)[0]))
#define PLUS_ONE(name) +1

// Buttons are found by their offset in the device struct, so one table covers
// every key and a missing one fails to compile.
#define BUTTON_AT(device, offset) ((tide_button *)((char *)(device) + (offset)))

// Every Tide key with its raylib key (desktop) and DOM `code` (web). Both
// name physical positions after the US layout, as Tide does.
typedef struct key_binding {
    size_t offset; // In tide_keyboard
    int raylib;
    const char *dom;
} key_binding;

#define KEY(name, raylib, dom) {offsetof(tide_keyboard, name), raylib, dom}

static const key_binding keys[] = {
    KEY(a, KEY_A, "KeyA"), KEY(b, KEY_B, "KeyB"), KEY(c, KEY_C, "KeyC"), KEY(d, KEY_D, "KeyD"),
    KEY(e, KEY_E, "KeyE"), KEY(f, KEY_F, "KeyF"), KEY(g, KEY_G, "KeyG"), KEY(h, KEY_H, "KeyH"),
    KEY(i, KEY_I, "KeyI"), KEY(j, KEY_J, "KeyJ"), KEY(k, KEY_K, "KeyK"), KEY(l, KEY_L, "KeyL"),
    KEY(m, KEY_M, "KeyM"), KEY(n, KEY_N, "KeyN"), KEY(o, KEY_O, "KeyO"), KEY(p, KEY_P, "KeyP"),
    KEY(q, KEY_Q, "KeyQ"), KEY(r, KEY_R, "KeyR"), KEY(s, KEY_S, "KeyS"), KEY(t, KEY_T, "KeyT"),
    KEY(u, KEY_U, "KeyU"), KEY(v, KEY_V, "KeyV"), KEY(w, KEY_W, "KeyW"), KEY(x, KEY_X, "KeyX"),
    KEY(y, KEY_Y, "KeyY"), KEY(z, KEY_Z, "KeyZ"),
    KEY(digit0, KEY_ZERO, "Digit0"), KEY(digit1, KEY_ONE, "Digit1"), KEY(digit2, KEY_TWO, "Digit2"),
    KEY(digit3, KEY_THREE, "Digit3"), KEY(digit4, KEY_FOUR, "Digit4"), KEY(digit5, KEY_FIVE, "Digit5"),
    KEY(digit6, KEY_SIX, "Digit6"), KEY(digit7, KEY_SEVEN, "Digit7"), KEY(digit8, KEY_EIGHT, "Digit8"),
    KEY(digit9, KEY_NINE, "Digit9"),
    KEY(space, KEY_SPACE, "Space"), KEY(enter, KEY_ENTER, "Enter"), KEY(escape, KEY_ESCAPE, "Escape"),
    KEY(tab, KEY_TAB, "Tab"), KEY(backspace, KEY_BACKSPACE, "Backspace"),
    KEY(insert, KEY_INSERT, "Insert"), KEY(delete, KEY_DELETE, "Delete"), KEY(home, KEY_HOME, "Home"),
    KEY(end, KEY_END, "End"), KEY(pageUp, KEY_PAGE_UP, "PageUp"), KEY(pageDown, KEY_PAGE_DOWN, "PageDown"),
    KEY(upArrow, KEY_UP, "ArrowUp"), KEY(downArrow, KEY_DOWN, "ArrowDown"),
    KEY(leftArrow, KEY_LEFT, "ArrowLeft"), KEY(rightArrow, KEY_RIGHT, "ArrowRight"),
    KEY(leftShift, KEY_LEFT_SHIFT, "ShiftLeft"), KEY(rightShift, KEY_RIGHT_SHIFT, "ShiftRight"),
    KEY(leftCtrl, KEY_LEFT_CONTROL, "ControlLeft"), KEY(rightCtrl, KEY_RIGHT_CONTROL, "ControlRight"),
    KEY(leftAlt, KEY_LEFT_ALT, "AltLeft"), KEY(rightAlt, KEY_RIGHT_ALT, "AltRight"),
    KEY(capsLock, KEY_CAPS_LOCK, "CapsLock"),
    KEY(f1, KEY_F1, "F1"), KEY(f2, KEY_F2, "F2"), KEY(f3, KEY_F3, "F3"), KEY(f4, KEY_F4, "F4"),
    KEY(f5, KEY_F5, "F5"), KEY(f6, KEY_F6, "F6"), KEY(f7, KEY_F7, "F7"), KEY(f8, KEY_F8, "F8"),
    KEY(f9, KEY_F9, "F9"), KEY(f10, KEY_F10, "F10"), KEY(f11, KEY_F11, "F11"), KEY(f12, KEY_F12, "F12"),
    KEY(minus, KEY_MINUS, "Minus"), KEY(equals, KEY_EQUAL, "Equal"),
    KEY(leftBracket, KEY_LEFT_BRACKET, "BracketLeft"), KEY(rightBracket, KEY_RIGHT_BRACKET, "BracketRight"),
    KEY(backslash, KEY_BACKSLASH, "Backslash"), KEY(semicolon, KEY_SEMICOLON, "Semicolon"),
    KEY(quote, KEY_APOSTROPHE, "Quote"), KEY(comma, KEY_COMMA, "Comma"), KEY(period, KEY_PERIOD, "Period"),
    KEY(slash, KEY_SLASH, "Slash"), KEY(backquote, KEY_GRAVE, "Backquote"),
    KEY(numpad0, KEY_KP_0, "Numpad0"), KEY(numpad1, KEY_KP_1, "Numpad1"), KEY(numpad2, KEY_KP_2, "Numpad2"),
    KEY(numpad3, KEY_KP_3, "Numpad3"), KEY(numpad4, KEY_KP_4, "Numpad4"), KEY(numpad5, KEY_KP_5, "Numpad5"),
    KEY(numpad6, KEY_KP_6, "Numpad6"), KEY(numpad7, KEY_KP_7, "Numpad7"), KEY(numpad8, KEY_KP_8, "Numpad8"),
    KEY(numpad9, KEY_KP_9, "Numpad9"),
    KEY(numpadEnter, KEY_KP_ENTER, "NumpadEnter"), KEY(numpadPlus, KEY_KP_ADD, "NumpadAdd"),
    KEY(numpadMinus, KEY_KP_SUBTRACT, "NumpadSubtract"), KEY(numpadMultiply, KEY_KP_MULTIPLY, "NumpadMultiply"),
    KEY(numpadDivide, KEY_KP_DIVIDE, "NumpadDivide"), KEY(numpadPeriod, KEY_KP_DECIMAL, "NumpadDecimal"),
};

_Static_assert(COUNT_OF(keys) == 0 TIDE_KEYBOARD_KEYS(PLUS_ONE), "every key in devices.h needs a binding");

typedef struct button_binding {
    size_t offset;
    int raylib;
} button_binding;

static const button_binding mouse_buttons[] = {
    {offsetof(tide_mouse, left), MOUSE_BUTTON_LEFT},
    {offsetof(tide_mouse, right), MOUSE_BUTTON_RIGHT},
    {offsetof(tide_mouse, middle), MOUSE_BUTTON_MIDDLE},
    {offsetof(tide_mouse, back), MOUSE_BUTTON_SIDE},     // GLFW button 4, what mice send for back
    {offsetof(tide_mouse, forward), MOUSE_BUTTON_EXTRA}, // GLFW button 5
};

_Static_assert(COUNT_OF(mouse_buttons) == 0 TIDE_MOUSE_BUTTONS(PLUS_ONE), "every mouse button needs a binding");

// raylib's button (desktop), and the button's index in the browser's standard
// gamepad mapping (web).
typedef struct gamepad_binding {
    size_t offset;
    int raylib;
    int web;
} gamepad_binding;

static const gamepad_binding gamepad_buttons[] = {
    {offsetof(tide_gamepad, buttonSouth), GAMEPAD_BUTTON_RIGHT_FACE_DOWN, 0},
    {offsetof(tide_gamepad, buttonEast), GAMEPAD_BUTTON_RIGHT_FACE_RIGHT, 1},
    {offsetof(tide_gamepad, buttonWest), GAMEPAD_BUTTON_RIGHT_FACE_LEFT, 2},
    {offsetof(tide_gamepad, buttonNorth), GAMEPAD_BUTTON_RIGHT_FACE_UP, 3},
    {offsetof(tide_gamepad, leftShoulder), GAMEPAD_BUTTON_LEFT_TRIGGER_1, 4},
    {offsetof(tide_gamepad, rightShoulder), GAMEPAD_BUTTON_RIGHT_TRIGGER_1, 5},
    {offsetof(tide_gamepad, leftStickButton), GAMEPAD_BUTTON_LEFT_THUMB, 10},
    {offsetof(tide_gamepad, rightStickButton), GAMEPAD_BUTTON_RIGHT_THUMB, 11},
    {offsetof(tide_gamepad, start), GAMEPAD_BUTTON_MIDDLE_RIGHT, 9},
    {offsetof(tide_gamepad, select), GAMEPAD_BUTTON_MIDDLE_LEFT, 8},
    {offsetof(tide_gamepad, dpad.up), GAMEPAD_BUTTON_LEFT_FACE_UP, 12},
    {offsetof(tide_gamepad, dpad.down), GAMEPAD_BUTTON_LEFT_FACE_DOWN, 13},
    {offsetof(tide_gamepad, dpad.left), GAMEPAD_BUTTON_LEFT_FACE_LEFT, 14},
    {offsetof(tide_gamepad, dpad.right), GAMEPAD_BUTTON_LEFT_FACE_RIGHT, 15},
};

_Static_assert(COUNT_OF(gamepad_buttons) == 0 TIDE_GAMEPAD_BUTTONS(PLUS_ONE) TIDE_DPAD_BUTTONS(PLUS_ONE),
               "every gamepad button needs a binding");

#ifdef __wasm__
static bool web_timer_frames; // Frames on timers instead of animation frames
#endif

static tide_frame_fn run_frame;
static void *run_user;
static double frame_start; // When the frame running, or the last one, started (GetTime)
static double frame_due;   // When the next one comes while nobody sees the window
static bool due_asked;     // ...as the frame function asked (tide_platform_next_frame)

void tide_platform_open(const tide_window_desc *desc)
{
    unsigned int flags = 0;
    // On the web, a resizable window is a canvas that fills the page. Tests
    // keep the size they asked for.
    if (!desc->hidden) flags |= FLAG_WINDOW_RESIZABLE;
#if !defined(__wasm__) && !defined(__ANDROID__)
    // The browser paces web frames itself, and Android's backend its own.
    flags |= desc->hidden ? FLAG_WINDOW_HIDDEN : FLAG_VSYNC_HINT;
    // raylib stops the loop while the window is minimized, which would stop a
    // match for the other players: it goes on, paced in step().
    flags |= FLAG_WINDOW_ALWAYS_RUN;
    // Pixels are the display's logical ones, as CSS pixels are on the web
    // (see platform.h). raylib still renders at the display's full resolution.
    flags |= FLAG_WINDOW_HIGHDPI;
#endif
    SetConfigFlags(flags);
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(desc->width, desc->height, desc->title);
    SetExitKey(KEY_NULL); // Escape belongs to the game
#ifdef _WIN32
    tide_win32_touch_attach(GetWindowHandle());
#endif

#ifdef __wasm__
    // raylib reads web keys by the character they type, which depends on the
    // keyboard layout. The page reads the DOM's `code`, which names positions.
    web_timer_frames = desc->hidden;
    for (size_t i = 0; i < COUNT_OF(keys); i++) tide_web_watch_key((int)i, keys[i].dom);
#endif
}

_Noreturn static void finish(const int code)
{
#ifdef __wasm__
    tide_web_stop();
#endif
    CloseWindow();
    exit(code);
}

void tide_platform_next_frame(const double seconds)
{
    const double due = frame_start + (seconds > 0.0 ? seconds : 0.0);
    if (!due_asked || due < frame_due) frame_due = due;
    due_asked = true;
}

static int step(void)
{
    const double start = GetTime();
    const float seconds = (float)(start - frame_start);
    frame_start = start;
    frame_due = start + 1.0 / 60.0;
    due_asked = false;
    BeginDrawing();
    const int code = run_frame(run_user, seconds);
    EndDrawing(); // Also polls the OS for input
#ifdef __ANDROID__
    // An app in the background has no window: frames come when the frame
    // function asked, and draw nothing, until the system freezes it. It waits
    // on the activity, so a window given back goes on at once.
    while (IsWindowMinimized() && !WindowShouldClose()) {
        const double left = frame_due - GetTime();
        if (left <= 0.0) break;
        tide_android_wait(left);
    }
#elif !defined(__wasm__)
    // A minimized window has no vsync to wait for: the next frame comes when
    // the frame function asked, and draws nothing (see tide_platform_draw).
    // It waits on the window's events, so a window restored or closed meanwhile
    // goes on at once.
    while (IsWindowMinimized() && !glfwWindowShouldClose(glfwGetCurrentContext())) {
        const double left = frame_due - GetTime();
        if (left <= 0.0) break;
        if (left < 0.001) WaitTime(left); // Less than the system's timers wait (Windows' take milliseconds)
        else glfwWaitEventsTimeout(left);
    }
#endif
    return code;
}

#ifdef __wasm__
// The page calls it for every frame, once tide_platform_run starts the loop:
// seconds until the next one, which the page waits while it's hidden.
__attribute__((export_name("tide_web_frame"))) double tide_web_frame(void)
{
    const int code = step();
    if (code != TIDE_KEEP_RUNNING) finish(code);
    return frame_due - GetTime();
}
#endif

void tide_platform_run(const tide_frame_fn frame, void *user)
{
    run_frame = frame;
    run_user = user;
    frame_start = GetTime();
#ifdef __wasm__
    // Frames on requestAnimationFrame, or as fast as timers allow when hidden.
    // Doesn't return: it unwinds main's stack back to the browser.
    tide_web_run(web_timer_frames);
#else
    for (;;) {
        if (WindowShouldClose()) finish(0);
        const int code = step();
        if (code != TIDE_KEEP_RUNNING) finish(code);
    }
#endif
}

static void poll_keyboard(tide_keyboard *k)
{
    for (size_t i = 0; i < COUNT_OF(keys); i++) {
#ifdef __wasm__
        const bool held = tide_web_key_held((int)i);
#else
        const bool held = IsKeyDown(keys[i].raylib);
#endif
        tide_button_set(BUTTON_AT(k, keys[i].offset), held);
    }
}

// The mouse; true if it was used: it moved or scrolled, or a button went down
// or up.
static bool poll_mouse(tide_mouse *m)
{
    const Vector2 position = GetMousePosition();
    const Vector2 delta = GetMouseDelta();
    const Vector2 scroll = GetMouseWheelMoveV();
    // raylib counts from the top left, y down; Devices follows Unity: from the
    // bottom left, y up.
    m->position = tide_f2(position.x, (float)GetScreenHeight() - position.y);
    // Delta and scroll add up until the input is sampled.
    m->delta = tide_f2(m->delta.x + delta.x, m->delta.y - delta.y);
    m->scroll = tide_f2(m->scroll.x + scroll.x, m->scroll.y + scroll.y);
    m->poll_delta = tide_f2(delta.x, -delta.y);
    m->poll_scroll = tide_f2(scroll.x, scroll.y);
    bool used = delta.x != 0.0f || delta.y != 0.0f || scroll.x != 0.0f || scroll.y != 0.0f;
    for (size_t i = 0; i < COUNT_OF(mouse_buttons); i++) {
        tide_button *b = BUTTON_AT(m, mouse_buttons[i].offset);
        const bool held = IsMouseButtonDown(mouse_buttons[i].raylib);
        used |= held != b->held;
        tide_button_set(b, held);
    }
    return used;
}

// Fingers on a touchscreen: on the web, the page's; on Windows, the window's;
// on Android, the activity's. Elsewhere on desktop, there are none yet: GLFW
// has no touch.
static void poll_touches(tide_touchscreen *s)
{
    tide_touches_poll(s);
    const float width = (float)GetScreenWidth(), height = (float)GetScreenHeight();
#ifdef __wasm__
    s->connected = tide_web_touchscreen();
    while (tide_web_take_touch()) {
        // CSS pixels from the canvas's top left, as the mouse
        const tide_float2 at = tide_f2(tide_web_touch_x(), height - tide_web_touch_y());
        tide_touch_event(s, (tide_touch_phase)tide_web_touch_phase(), tide_web_touch_source(), at);
    }
    (void)width;
#elif defined(_WIN32) || defined(__ANDROID__)
    s->connected = tide_native_touchscreen();
    tide_touch_report r;
    while (tide_native_take_touch(&r)) tide_touch_event(s, r.phase, r.source, tide_f2(r.x * width, height - r.y * height));
#else
    (void)width;
    (void)height;
#endif
}

#ifndef __wasm__
// A trigger as 0 to 1 (raylib reports -1 released to 1 fully pressed).
static float trigger(const int pad, const int axis)
{
    return (GetGamepadAxisMovement(pad, axis) + 1.0f) * 0.5f;
}
#endif

static void poll_gamepad(tide_gamepad *g)
{
    const int pad = 0;
#ifdef __wasm__
    // The browser's standard mapping: axes 0 to 3 are the sticks, and the
    // triggers are analog buttons 6 and 7.
    g->connected = tide_web_gamepad_connected(pad);
    if (g->connected) {
        g->leftStick = tide_f2(tide_web_gamepad_axis(pad, 0), -tide_web_gamepad_axis(pad, 1));
        g->rightStick = tide_f2(tide_web_gamepad_axis(pad, 2), -tide_web_gamepad_axis(pad, 3));
        g->leftTrigger = tide_web_gamepad_button(pad, 6);
        g->rightTrigger = tide_web_gamepad_button(pad, 7);
    }
#else
    g->connected = IsGamepadAvailable(pad);
    if (g->connected) {
        g->leftStick = tide_f2(GetGamepadAxisMovement(pad, GAMEPAD_AXIS_LEFT_X),
                               -GetGamepadAxisMovement(pad, GAMEPAD_AXIS_LEFT_Y));
        g->rightStick = tide_f2(GetGamepadAxisMovement(pad, GAMEPAD_AXIS_RIGHT_X),
                                -GetGamepadAxisMovement(pad, GAMEPAD_AXIS_RIGHT_Y));
        g->leftTrigger = trigger(pad, GAMEPAD_AXIS_LEFT_TRIGGER);
        g->rightTrigger = trigger(pad, GAMEPAD_AXIS_RIGHT_TRIGGER);
    }
#endif
    if (!g->connected) {
        g->leftStick = g->rightStick = tide_f2(0.0f, 0.0f);
        g->leftTrigger = g->rightTrigger = 0.0f;
    }
    for (size_t i = 0; i < COUNT_OF(gamepad_buttons); i++) {
#ifdef __wasm__
        const bool held = g->connected && tide_web_gamepad_button(pad, gamepad_buttons[i].web) > 0.5f;
#else
        const bool held = g->connected && IsGamepadButtonDown(pad, gamepad_buttons[i].raylib);
#endif
        tide_button_set(BUTTON_AT(g, gamepad_buttons[i].offset), held);
    }
}

#ifndef __wasm__
// What Ctrl+V (Cmd+V on macOS) pasted, as characters typed: the next to type
// is pasted[pasted_at]. On the web, the page does it (tide.js).
static uint32_t pasted[1024];
static uint32_t pasted_count, pasted_at;

static void paste(void)
{
    const bool control = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL) || IsKeyDown(KEY_LEFT_SUPER)
                      || IsKeyDown(KEY_RIGHT_SUPER);
    if (!control || !IsKeyPressed(KEY_V)) return;
    const char *text = GetClipboardText();
    if (!text) return;
    int at = 0;
    while (text[at] && pasted_count < COUNT_OF(pasted)) {
        int size = 0;
        const int c = GetCodepointNext(text + at, &size);
        at += size > 0 ? size : 1;
        if (c >= 32 && c != 127) pasted[pasted_count++] = (uint32_t)c; // Not newlines or tabs
    }
}
#endif

// Characters typed since the last poll, which follow the keyboard layout. A
// poll takes as many as it holds, and leaves the rest for the next.
static void poll_text(tide_typed *text)
{
    text->count = 0;
#ifndef __wasm__
    paste();
    while (pasted_at < pasted_count && text->count < TIDE_TEXT_MAX) text->chars[text->count++] = pasted[pasted_at++];
    if (pasted_at == pasted_count) pasted_at = pasted_count = 0;
#endif
    while (text->count < TIDE_TEXT_MAX) {
#ifdef __wasm__
        const int c = tide_web_take_char();
#else
        const int c = GetCharPressed();
#endif
        if (c <= 0) break;
        text->chars[text->count++] = (uint32_t)c;
    }
}

void tide_platform_typing(const bool typing)
{
    static bool was;
    if (typing == was) return;
    was = typing;
#ifdef __wasm__
    tide_web_typing(typing);
#elif defined(__ANDROID__)
    tide_android_typing(typing);
#endif
}

void tide_platform_copy(const char *text)
{
#ifdef __wasm__
    tide_web_copy(text);
#else
    SetClipboardText(text);
#endif
}

void tide_platform_poll(tide_devices *devices)
{
    poll_keyboard(&devices->keyboard);
    const bool mouse_used = poll_mouse(&devices->mouse);
    poll_gamepad(&devices->gamepad);
    poll_touches(&devices->touchscreen);
    tide_pointer_poll(devices, mouse_used);
    poll_text(&devices->keyboard.text);
}

// The camera maps world units (y up) to window pixels (y down). Each frame's
// list starts at the origin with 1 unit per pixel. After TIDE_DRAW_GUI, it
// maps the GUI's pixels instead: from the top left, y down.
typedef struct camera {
    tide_float2 center;
    float scale; // Pixels per world unit
    bool gui;
} camera;

static camera last_camera = {{0.0f, 0.0f}, 1.0f, false};

static Vector2 to_screen(const camera *cam, const tide_float2 p)
{
    if (cam->gui) return (Vector2){p.x * cam->scale, p.y * cam->scale};
    return (Vector2){(float)GetScreenWidth() * 0.5f + (p.x - cam->center.x) * cam->scale,
                     (float)GetScreenHeight() * 0.5f - (p.y - cam->center.y) * cam->scale};
}

// A rect's top left corner on screen: y goes up in the world, down in the GUI.
static Vector2 rect_corner(const camera *cam, const tide_draw_command *c)
{
    const float half_height = cam->gui ? -c->b.y * 0.5f : c->b.y * 0.5f;
    return to_screen(cam, tide_f2(c->a.x - c->b.x * 0.5f, c->a.y + half_height));
}

static unsigned char color_channel(const float v)
{
    if (!(v > 0.0f)) return 0; // Also NaN
    if (v >= 1.0f) return 255;
    return (unsigned char)(v * 255.0f + 0.5f);
}

static Color to_raylib(const tide_color c)
{
    return (Color){color_channel(c.r), color_channel(c.g), color_channel(c.b), color_channel(c.a)};
}

// ---------------------------------------------------------------------------
// Shapes (rects, circles, their outlines and lines) go to the GPU as instances
// of one quad, each its shape in window pixels, so a run of them between
// other commands is one draw call, however many there are. Text goes through
// raylib, which batches it on its own.

enum { SHAPE_RECT, SHAPE_WIRE_RECT, SHAPE_CIRCLE, SHAPE_WIRE_CIRCLE, SHAPE_LINE };

typedef struct shape {
    float a[2];       // Its middle, or a line's start
    float b[2];       // Half its size (a circle's radius, twice), or a line's end
    uint8_t color[4]; // RGBA
    float kind;       // SHAPE_*
} shape;

// What a list comes to, in order: runs of shapes, clears and text.
enum { STEP_SHAPES, STEP_CLEAR, STEP_TEXT };

typedef struct draw_step {
    uint32_t kind;  // STEP_*
    uint32_t first; // STEP_SHAPES: its shapes; STEP_TEXT: its text's offset in the list
    uint32_t count;
    Vector2 at; // STEP_TEXT: its top left corner in window pixels, and its height
    float size;
    Color color;
} draw_step;

static shape *shapes;
static uint32_t shape_count, shape_capacity;
static draw_step *steps;
static uint32_t step_count, step_capacity;

static draw_step *add_step(const uint32_t kind)
{
    if (step_count == step_capacity) {
        const uint32_t capacity = step_capacity ? step_capacity * 2u : 64u;
        steps = tide_realloc(steps, step_capacity * sizeof(draw_step), capacity * sizeof(draw_step));
        step_capacity = capacity;
    }
    draw_step *s = &steps[step_count++];
    *s = (draw_step){.kind = kind};
    return s;
}

static void add_shape(const int kind, const float ax, const float ay, const float bx, const float by,
                      const Color color)
{
    if (shape_count == shape_capacity) {
        const uint32_t capacity = shape_capacity ? shape_capacity * 2u : 1024u;
        if (capacity > UINT32_MAX / sizeof(shape)) tide_out_of_memory();
        shapes = tide_realloc(shapes, shape_capacity * sizeof(shape), capacity * sizeof(shape));
        shape_capacity = capacity;
    }
    if (step_count == 0 || steps[step_count - 1].kind != STEP_SHAPES) add_step(STEP_SHAPES)->first = shape_count;
    steps[step_count - 1].count++;
    shapes[shape_count++] = (shape){{ax, ay}, {bx, by}, {color.r, color.g, color.b, color.a}, (float)kind};
}

// OpenGL ES 3 on the web and Android, OpenGL 3.3 on desktop.
#if defined(__wasm__) || defined(__ANDROID__)
#define GLSL_VERSION "#version 300 es\nprecision highp float;\n"
#else
#define GLSL_VERSION "#version 330\n"
#endif

// Each instance's quad covers its shape in window pixels, which the camera
// already applied, and rlgl's matrices take to the screen (or a render
// texture), as they do for raylib's own drawing.
static const char shape_vertex[] = GLSL_VERSION
    "in vec2 corner;\n" // A corner of the quad, 0 to 1
    "in vec4 shape;\n"  // a, then b
    "in vec4 color;\n"
    "in float kind;\n"
    "uniform mat4 mvp;\n"
    "out vec2 local;\n"  // Pixels from its middle; for a line, along and across it
    "out vec2 extent;\n" // Half its size in pixels
    "out vec4 tint;\n"
    "out float form;\n"
    "void main() {\n"
    "    vec2 c = corner * 2.0 - 1.0;\n"
    "    vec2 p;\n"
    "    if (kind > 3.5) {\n" // A line: a pixel wide, reaching half a pixel past its ends
    "        vec2 d = shape.zw - shape.xy;\n"
    "        float len = length(d);\n"
    "        vec2 along = len > 0.0 ? d / len : vec2(1.0, 0.0);\n"
    "        extent = vec2(len * 0.5 + 0.5, 0.5);\n"
    "        local = c * extent;\n"
    "        p = (shape.xy + shape.zw) * 0.5 + along * local.x + vec2(-along.y, along.x) * local.y;\n"
    "    } else {\n"
    "        extent = abs(shape.zw);\n"
    "        local = c * (extent + (kind > 1.5 ? 1.0 : 0.0));\n" // Room for a circle's smooth edge
    "        p = shape.xy + local;\n"
    "    }\n"
    "    tint = color;\n"
    "    form = kind;\n"
    "    gl_Position = mvp * vec4(p, 0.0, 1.0);\n"
    "}\n";

static const char shape_fragment[] = GLSL_VERSION
    "in vec2 local;\n"
    "in vec2 extent;\n"
    "in vec4 tint;\n"
    "in float form;\n"
    "out vec4 pixel;\n"
    "void main() {\n"
    "    float cover = 1.0;\n"
    "    if (form > 0.5 && form < 1.5) {\n" // A rect's outline: the pixel inside its edge
    "        if (all(lessThan(abs(local), extent - 1.0))) discard;\n"
    "    } else if (form > 1.5 && form < 3.5) {\n" // Circles, with a smooth edge
    "        float d = length(local);\n"
    "        cover = clamp(extent.x - d + 0.5, 0.0, 1.0);\n"
    "        if (form > 2.5) cover *= clamp(d - extent.x + 1.5, 0.0, 1.0);\n" // An outline: the pixel inside it
    "        if (cover <= 0.0) discard;\n"
    "    }\n"
    "    pixel = vec4(tint.rgb, tint.a * cover);\n"
    "}\n";

static struct {
    unsigned int shader, vao, corners, instances;
    uint32_t capacity; // Shapes the instance buffer holds
    int mvp, corner, shape, color, kind;
} gpu;

// rlgl's matrices, as its own drawing uses them (rlMatrixMultiply(modelview,
// projection) in rlgl.h).
static Matrix shape_mvp(void)
{
    const Matrix l = rlGetMatrixModelview();
    const Matrix r = rlGetMatrixProjection();
    Matrix m;
    m.m0 = l.m0 * r.m0 + l.m1 * r.m4 + l.m2 * r.m8 + l.m3 * r.m12;
    m.m1 = l.m0 * r.m1 + l.m1 * r.m5 + l.m2 * r.m9 + l.m3 * r.m13;
    m.m2 = l.m0 * r.m2 + l.m1 * r.m6 + l.m2 * r.m10 + l.m3 * r.m14;
    m.m3 = l.m0 * r.m3 + l.m1 * r.m7 + l.m2 * r.m11 + l.m3 * r.m15;
    m.m4 = l.m4 * r.m0 + l.m5 * r.m4 + l.m6 * r.m8 + l.m7 * r.m12;
    m.m5 = l.m4 * r.m1 + l.m5 * r.m5 + l.m6 * r.m9 + l.m7 * r.m13;
    m.m6 = l.m4 * r.m2 + l.m5 * r.m6 + l.m6 * r.m10 + l.m7 * r.m14;
    m.m7 = l.m4 * r.m3 + l.m5 * r.m7 + l.m6 * r.m11 + l.m7 * r.m15;
    m.m8 = l.m8 * r.m0 + l.m9 * r.m4 + l.m10 * r.m8 + l.m11 * r.m12;
    m.m9 = l.m8 * r.m1 + l.m9 * r.m5 + l.m10 * r.m9 + l.m11 * r.m13;
    m.m10 = l.m8 * r.m2 + l.m9 * r.m6 + l.m10 * r.m10 + l.m11 * r.m14;
    m.m11 = l.m8 * r.m3 + l.m9 * r.m7 + l.m10 * r.m11 + l.m11 * r.m15;
    m.m12 = l.m12 * r.m0 + l.m13 * r.m4 + l.m14 * r.m8 + l.m15 * r.m12;
    m.m13 = l.m12 * r.m1 + l.m13 * r.m5 + l.m14 * r.m9 + l.m15 * r.m13;
    m.m14 = l.m12 * r.m2 + l.m13 * r.m6 + l.m14 * r.m10 + l.m15 * r.m14;
    m.m15 = l.m12 * r.m3 + l.m13 * r.m7 + l.m14 * r.m11 + l.m15 * r.m15;
    return m;
}

// The shader and the quad, the first time they're needed.
static void shapes_start(void)
{
    if (gpu.shader) return;
    gpu.shader = rlLoadShaderProgram(shape_vertex, shape_fragment);
    if (gpu.shader == 0 || gpu.shader == rlGetShaderIdDefault()) {
        TraceLog(LOG_FATAL, "DRAW: the shapes' shader didn't compile (see above)");
        abort();
    }
    gpu.mvp = rlGetLocationUniform(gpu.shader, "mvp");
    gpu.corner = rlGetLocationAttrib(gpu.shader, "corner");
    gpu.shape = rlGetLocationAttrib(gpu.shader, "shape");
    gpu.color = rlGetLocationAttrib(gpu.shader, "color");
    gpu.kind = rlGetLocationAttrib(gpu.shader, "kind");
    gpu.vao = rlLoadVertexArray();
    rlEnableVertexArray(gpu.vao);
    // Counterclockwise on screen, as rlgl culls the back faces of what's drawn
    static const float corners[12] = {0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0};
    gpu.corners = rlLoadVertexBuffer(corners, sizeof corners, false);
    rlSetVertexAttribute((unsigned)gpu.corner, 2, RL_FLOAT, false, 0, 0);
    rlEnableVertexAttribute((unsigned)gpu.corner);
    rlDisableVertexArray();
}

// The instance buffer's attributes, from shape `first` on.
static void point_at(const uint32_t first)
{
    const int at = (int)(first * sizeof(shape));
    rlEnableVertexBuffer(gpu.instances);
    rlSetVertexAttribute((unsigned)gpu.shape, 4, RL_FLOAT, false, sizeof(shape), at + (int)offsetof(shape, a));
    rlSetVertexAttribute((unsigned)gpu.color, 4, RL_UNSIGNED_BYTE, true, sizeof(shape), at + (int)offsetof(shape, color));
    rlSetVertexAttribute((unsigned)gpu.kind, 1, RL_FLOAT, false, sizeof(shape), at + (int)offsetof(shape, kind));
}

// This list's shapes into the instance buffer, which grows as they need.
static void upload_shapes(void)
{
    if (shape_count > gpu.capacity) {
        uint32_t capacity = gpu.capacity ? gpu.capacity : 1024u;
        while (capacity < shape_count) capacity *= 2u;
        rlEnableVertexArray(gpu.vao);
        if (gpu.instances) rlUnloadVertexBuffer(gpu.instances);
        gpu.instances = rlLoadVertexBuffer(NULL, (int)(capacity * sizeof(shape)), true);
        gpu.capacity = capacity;
        point_at(0);
        const int per_instance[] = {gpu.shape, gpu.color, gpu.kind};
        for (int i = 0; i < 3; i++) {
            rlEnableVertexAttribute((unsigned)per_instance[i]);
            rlSetVertexAttributeDivisor((unsigned)per_instance[i], 1);
        }
        rlDisableVertexArray();
    }
    if (shape_count) rlUpdateVertexBuffer(gpu.instances, shapes, (int)(shape_count * sizeof(shape)), 0);
}

static void draw_shapes(const uint32_t first, const uint32_t count)
{
    rlDrawRenderBatchActive(); // What raylib batched before them (text) goes first
    rlEnableShader(gpu.shader);
    rlSetUniformMatrix(gpu.mvp, shape_mvp());
    rlEnableVertexArray(gpu.vao);
    point_at(first);
    rlDrawVertexArrayInstanced(0, 6, (int)count);
    rlDisableVertexArray();
    rlDisableShader();
}

static void draw_list(const tide_draw_list *list)
{
    shapes_start();
    shape_count = 0;
    step_count = 0;
    camera cam = {{0.0f, 0.0f}, 1.0f, false};
    camera world = cam; // The last world camera, for tide_platform_world_to_screen
    for (uint32_t i = 0; i < list->count; i++) {
        const tide_draw_command *c = &list->commands[i];
        const Color color = to_raylib(c->color);
        switch ((tide_draw_kind)c->kind) {
        case TIDE_DRAW_CLEAR:
            add_step(STEP_CLEAR)->color = color;
            break;
        case TIDE_DRAW_CAMERA:
            cam.center = c->a;
            cam.scale = c->b.x > 0.0f ? (float)GetScreenHeight() / (2.0f * c->b.x) : 1.0f;
            cam.gui = false;
            world = cam;
            break;
        case TIDE_DRAW_GUI:
            cam.scale = 1.0f;
            cam.gui = true;
            break;
        case TIDE_DRAW_CIRCLE:
        case TIDE_DRAW_WIRE_CIRCLE: {
            const Vector2 p = to_screen(&cam, c->a);
            const float r = c->b.x * cam.scale;
            add_shape(c->kind == TIDE_DRAW_CIRCLE ? SHAPE_CIRCLE : SHAPE_WIRE_CIRCLE, p.x, p.y, r, r, color);
            break;
        }
        case TIDE_DRAW_RECT:
        case TIDE_DRAW_WIRE_RECT: {
            const Vector2 top_left = rect_corner(&cam, c);
            const float hw = c->b.x * cam.scale * 0.5f;
            const float hh = c->b.y * cam.scale * 0.5f;
            add_shape(c->kind == TIDE_DRAW_RECT ? SHAPE_RECT : SHAPE_WIRE_RECT, top_left.x + hw, top_left.y + hh, hw,
                      hh, color);
            break;
        }
        case TIDE_DRAW_LINE: {
            const Vector2 from = to_screen(&cam, c->a);
            const Vector2 to = to_screen(&cam, c->b);
            add_shape(SHAPE_LINE, from.x, from.y, to.x, to.y, color);
            break;
        }
        case TIDE_DRAW_TEXT: {
            draw_step *s = add_step(STEP_TEXT);
            s->first = c->text;
            s->at = to_screen(&cam, c->a);
            s->size = c->b.x * cam.scale;
            s->color = color;
            break;
        }
        }
    }
    last_camera = world;

    upload_shapes();
    rlDrawRenderBatchActive(); // What came before the list goes under it, and is cleared
    ClearBackground(BLACK);    // Every frame starts black; Draw.Clear picks another color
    for (uint32_t i = 0; i < step_count; i++) {
        const draw_step *s = &steps[i];
        if (s->kind == STEP_SHAPES) {
            draw_shapes(s->first, s->count);
        } else if (s->kind == STEP_CLEAR) {
            rlDrawRenderBatchActive(); // Text before it is cleared too
            ClearBackground(s->color);
        } else {
            DrawTextEx(GetFontDefault(), list->text + s->first, s->at, s->size, s->size / 10.0f, s->color);
        }
    }
}

// Nobody sees it while the window is minimized or, on the web, the page is
// hidden, and frames go on meanwhile.
static bool unseen(void)
{
#ifdef __wasm__
    return tide_web_hidden();
#else
    return IsWindowMinimized();
#endif
}

void tide_platform_draw(const tide_draw_list *list)
{
    if (!unseen()) draw_list(list);
}

tide_float2 tide_platform_world_to_screen(const tide_float2 world)
{
    const Vector2 p = to_screen(&last_camera, world);
    return tide_f2(p.x, p.y);
}

tide_float2 tide_platform_screen_size(void)
{
    return tide_f2((float)GetScreenWidth(), (float)GetScreenHeight());
}

float tide_platform_measure_text(const char *text, const float size)
{
    return MeasureTextEx(GetFontDefault(), text, size, size / 10.0f).x;
}

void tide_platform_draw_overlay(const char *text)
{
    if (unseen()) return;
    const int size = 16;
    const int pitch = 20; // From one line to the next
    char lines[16][128];
    int count = 0;
    int widest = 0;
    for (const char *line = text; line && count < 16; count++) {
        const char *end = strchr(line, '\n');
        size_t n = end ? (size_t)(end - line) : strlen(line);
        if (n >= sizeof lines[0]) n = sizeof lines[0] - 1;
        memcpy(lines[count], line, n);
        lines[count][n] = '\0';
        const int width = MeasureText(lines[count], size);
        if (width > widest) widest = width;
        line = end ? end + 1 : NULL;
    }
    // A panel in the bottom right corner, each line against its right edge
    const int right = GetScreenWidth() - 12;
    const int top = GetScreenHeight() - 12 - count * pitch;
    DrawRectangle(right - widest - 8, top - 6, widest + 16, count * pitch + 8, (Color){0, 0, 0, 140});
    for (int i = 0; i < count; i++) DrawText(lines[i], right - MeasureText(lines[i], size), top + i * pitch, size, LIGHTGRAY);
}

int tide_platform_fps(void)
{
    return GetFPS();
}

void tide_platform_read_pixels(const tide_draw_list *list, const tide_float2 *points, const int count, uint32_t *rgba)
{
    const int height = GetScreenHeight();
    const RenderTexture2D target = LoadRenderTexture(GetScreenWidth(), height);
    BeginTextureMode(target);
    draw_list(list);
    EndTextureMode();
    const Image image = LoadImageFromTexture(target.texture);
    UnloadRenderTexture(target);
    for (int i = 0; i < count; i++) {
        // Render textures are stored bottom row first.
        const Color c = GetImageColor(image, (int)points[i].x, height - 1 - (int)points[i].y);
        rgba[i] = (uint32_t)c.r << 24 | (uint32_t)c.g << 16 | (uint32_t)c.b << 8 | (uint32_t)c.a;
    }
    UnloadImage(image);
}
