#include "tide/platform.h"

#include <math.h>
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
// list starts at the origin with 1 unit per pixel. After TIDE_DRAW_SCREEN, it
// maps the screen's pixels instead: from the top left, y down.
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

// A mesh's corner, in window pixels. Meshes go to the GPU as triangles, three
// of these each: a run of them with one texture is one draw call.
typedef struct mesh_vertex {
    float position[2];
    float uv[2];
    uint8_t color[4]; // RGBA
} mesh_vertex;

// What a list comes to, in order: runs of shapes, clears, text, runs of
// triangles, clips and runs of instances of 3D meshes.
enum { STEP_SHAPES, STEP_CLEAR, STEP_TEXT, STEP_MESH, STEP_CLIP, STEP_NO_CLIP, STEP_MESH_3D };

typedef struct draw_step {
    uint32_t kind;    // STEP_*
    uint32_t first;   // STEP_SHAPES: its shapes; STEP_TEXT: its text's offset in the list; STEP_MESH: its vertices;
                      // STEP_MESH_3D: its instances' transforms in the list's matrices
    uint32_t count;
    uint32_t texture; // STEP_MESH and STEP_MESH_3D: 1 + its texture's place in the list's, or 0 for none
    uint32_t filter;  // ...and how it's sampled: tide_filter
    uint32_t mesh;    // STEP_MESH_3D: 1 + its mesh's place in the list's
    uint32_t camera;  // ...and 1 + its camera's matrix's place in the list's, or 0 for the first camera
    uint32_t fit;     // ...which is for a square screen (see TIDE_DRAW_CAMERA_3D)
    Vector2 at; // STEP_TEXT: its top left corner in window pixels, and its height; STEP_CLIP: its top left corner
    Vector2 to; // STEP_CLIP: its bottom right corner
    float size;
    Color color;
} draw_step;

static shape *shapes;
static uint32_t shape_count, shape_capacity;
static mesh_vertex *mesh_vertices;
static uint32_t mesh_vertex_count, mesh_vertex_capacity;
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

// A mesh command's triangles, their corners through the camera, after the
// ones before it when nothing came between them.
static void add_mesh(const tide_draw_list *list, const tide_draw_command *c, const camera *cam)
{
    const uint32_t count = c->mesh.count;
    if (count > mesh_vertex_capacity - mesh_vertex_count) {
        uint64_t capacity = mesh_vertex_capacity ? mesh_vertex_capacity : 4096u;
        while (capacity < (uint64_t)mesh_vertex_count + count) capacity *= 2u;
        if (capacity > UINT32_MAX / sizeof(mesh_vertex)) tide_out_of_memory();
        mesh_vertices = tide_realloc(mesh_vertices, mesh_vertex_capacity * sizeof(mesh_vertex),
                                     (size_t)capacity * sizeof(mesh_vertex));
        mesh_vertex_capacity = (uint32_t)capacity;
    }
    draw_step *s = step_count ? &steps[step_count - 1] : NULL;
    if (!s || s->kind != STEP_MESH || s->texture != c->mesh.texture || s->filter != c->mesh.filter) {
        s = add_step(STEP_MESH);
        s->first = mesh_vertex_count;
        s->texture = c->mesh.texture;
        s->filter = c->mesh.filter;
    }
    s->count += count;
    const uint32_t *indices = list->indices + c->mesh.first;
    mesh_vertex *out = mesh_vertices + mesh_vertex_count;
    for (uint32_t i = 0; i < count; i++) {
        const tide_vertex *v = &list->vertices[indices[i]];
        const Vector2 p = to_screen(cam, v->position);
        const Color color = to_raylib(v->color);
        out[i] = (mesh_vertex){{p.x, p.y}, {v->uv.x, v->uv.y}, {color.r, color.g, color.b, color.a}};
    }
    mesh_vertex_count += count;
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

// A mesh's pixels are its corners' colors, blended across each triangle,
// times its texture's (a white pixel, for a mesh with none).
static const char mesh_vertex_shader[] = GLSL_VERSION
    "in vec2 position;\n" // In window pixels
    "in vec2 uv;\n"
    "in vec4 color;\n"
    "uniform mat4 mvp;\n"
    "out vec2 at;\n"
    "out vec4 tint;\n"
    "void main() {\n"
    "    at = uv;\n"
    "    tint = color;\n"
    "    gl_Position = mvp * vec4(position, 0.0, 1.0);\n"
    "}\n";

static const char mesh_fragment_shader[] = GLSL_VERSION
    "in vec2 at;\n"
    "in vec4 tint;\n"
    "uniform sampler2D pixels;\n"
    "out vec4 pixel;\n"
    "void main() {\n"
    "    pixel = texture(pixels, at) * tint;\n"
    "}\n";

static struct {
    unsigned int shader, vao, vertices;
    uint32_t capacity; // Vertices the buffer holds
    int mvp, pixels, position, uv, color;
} mesh_gpu;

// A 3D mesh's corners go through its instance's transform and the camera,
// with depth; its pixels are as a mesh's.
static const char mesh_3d_vertex_shader[] = GLSL_VERSION
    "in vec3 position;\n"
    "in vec2 uv;\n"
    "in vec4 color;\n"
    "in vec4 model0;\n" // Its instance's transform, column by column
    "in vec4 model1;\n"
    "in vec4 model2;\n"
    "in vec4 model3;\n"
    "uniform mat4 camera;\n" // From the world to clip space
    "out vec2 at;\n"
    "out vec4 tint;\n"
    "void main() {\n"
    "    at = uv;\n"
    "    tint = color;\n"
    "    gl_Position = camera * (mat4(model0, model1, model2, model3) * vec4(position, 1.0));\n"
    "}\n";

// A 3D mesh's corner on the GPU: its triangles go there as three of these
// each, uploaded when a list first draws the mesh, and let go once a frame
// draws without it.
typedef struct mesh_3d_vertex {
    float position[3];
    float uv[2];
    uint8_t color[4]; // RGBA
} mesh_3d_vertex;

static struct {
    unsigned int shader, vao, instances;
    uint32_t capacity; // Transforms the instance buffer holds
    int camera, pixels, position, uv, color, model[4];
} mesh_3d_gpu;

// The GPU's copy of a list's 3D mesh, kept in the place the list keeps it.
typedef struct gpu_mesh {
    uint64_t id, other, version; // Which it is (see tide_draw_mesh_data)
    uint32_t space;
    uint32_t epoch;
    unsigned int buffer;
    uint32_t corners;  // Its triangles' corners in `buffer`, three each
    uint32_t capacity; // Corners `buffer` has room for
    uint32_t frame;    // The frame that last drew it
} gpu_mesh;

static gpu_mesh *gpu_meshes;
static uint32_t gpu_mesh_count; // Places, as the list's
static mesh_3d_vertex *mesh_3d_corners; // A mesh's, on their way to the GPU
static uint32_t mesh_3d_corner_capacity;

// A texture on the GPU: a copy of a draw list's, uploaded when a list first
// draws with it and again when its pixels changed, and let go once a frame
// draws without it. Nothing else keeps it: the pixels are the game's.
typedef struct gpu_texture {
    uint64_t id; // Whose pixels (see tide_draw_texture)
    uint64_t version;
    uint32_t space;
    int32_t width, height;
    uint32_t epoch; // Its list's when it copied them (see tide_draw_forget)
    unsigned int gl;
    int filter;     // The last it was sampled with, or -1
    uint32_t frame; // The frame that last drew with it
} gpu_texture;

static gpu_texture *gpu_textures;
static uint32_t gpu_texture_count, gpu_texture_capacity;
static uint32_t gpu_frame;

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

static void meshes_start(void)
{
    if (mesh_gpu.shader) return;
    mesh_gpu.shader = rlLoadShaderProgram(mesh_vertex_shader, mesh_fragment_shader);
    if (mesh_gpu.shader == 0 || mesh_gpu.shader == rlGetShaderIdDefault()) {
        TraceLog(LOG_FATAL, "DRAW: the meshes' shader didn't compile (see above)");
        abort();
    }
    mesh_gpu.mvp = rlGetLocationUniform(mesh_gpu.shader, "mvp");
    mesh_gpu.pixels = rlGetLocationUniform(mesh_gpu.shader, "pixels");
    mesh_gpu.position = rlGetLocationAttrib(mesh_gpu.shader, "position");
    mesh_gpu.uv = rlGetLocationAttrib(mesh_gpu.shader, "uv");
    mesh_gpu.color = rlGetLocationAttrib(mesh_gpu.shader, "color");
    mesh_gpu.vao = rlLoadVertexArray();
}

static void meshes_3d_start(void)
{
    if (mesh_3d_gpu.shader) return;
    mesh_3d_gpu.shader = rlLoadShaderProgram(mesh_3d_vertex_shader, mesh_fragment_shader);
    if (mesh_3d_gpu.shader == 0 || mesh_3d_gpu.shader == rlGetShaderIdDefault()) {
        TraceLog(LOG_FATAL, "DRAW: the 3D meshes' shader didn't compile (see above)");
        abort();
    }
    mesh_3d_gpu.camera = rlGetLocationUniform(mesh_3d_gpu.shader, "camera");
    mesh_3d_gpu.pixels = rlGetLocationUniform(mesh_3d_gpu.shader, "pixels");
    mesh_3d_gpu.position = rlGetLocationAttrib(mesh_3d_gpu.shader, "position");
    mesh_3d_gpu.uv = rlGetLocationAttrib(mesh_3d_gpu.shader, "uv");
    mesh_3d_gpu.color = rlGetLocationAttrib(mesh_3d_gpu.shader, "color");
    static const char *const model[4] = {"model0", "model1", "model2", "model3"};
    for (int i = 0; i < 4; i++) mesh_3d_gpu.model[i] = rlGetLocationAttrib(mesh_3d_gpu.shader, model[i]);
    mesh_3d_gpu.vao = rlLoadVertexArray();
    rlEnableVertexArray(mesh_3d_gpu.vao);
    const int per_corner[] = {mesh_3d_gpu.position, mesh_3d_gpu.uv, mesh_3d_gpu.color};
    for (int i = 0; i < 3; i++) rlEnableVertexAttribute((unsigned)per_corner[i]);
    for (int i = 0; i < 4; i++) {
        rlEnableVertexAttribute((unsigned)mesh_3d_gpu.model[i]);
        rlSetVertexAttributeDivisor((unsigned)mesh_3d_gpu.model[i], 1);
    }
    rlDisableVertexArray();
}

// The GPU's copy of the list's 3D mesh `mesh`, uploaded if it hasn't got it.
static gpu_mesh *mesh_for(const tide_draw_list *list, const uint32_t mesh)
{
    if (list->mesh_count > gpu_mesh_count) {
        gpu_meshes = tide_realloc(gpu_meshes, gpu_mesh_count * sizeof(gpu_mesh), list->mesh_count * sizeof(gpu_mesh));
        memset(gpu_meshes + gpu_mesh_count, 0, (list->mesh_count - gpu_mesh_count) * sizeof(gpu_mesh));
        gpu_mesh_count = list->mesh_count;
    }
    const tide_draw_mesh_data *m = &list->meshes[mesh - 1u];
    gpu_mesh *g = &gpu_meshes[mesh - 1u];
    g->frame = gpu_frame;
    if (g->buffer && g->id == m->id && g->other == m->other && g->version == m->version && g->space == m->space
        && g->epoch == m->epoch) {
        return g;
    }
    // Each triangle's corners, three each
    if (m->index_count > mesh_3d_corner_capacity) {
        mesh_3d_corners = tide_realloc(mesh_3d_corners, mesh_3d_corner_capacity * sizeof(mesh_3d_vertex),
                                       m->index_count * sizeof(mesh_3d_vertex));
        mesh_3d_corner_capacity = m->index_count;
    }
    for (uint32_t i = 0; i < m->index_count; i++) {
        const tide_vertex3 *v = &m->vertices[m->indices[i]];
        const Color color = to_raylib(v->color);
        mesh_3d_corners[i] = (mesh_3d_vertex){{v->position.x, v->position.y, v->position.z}, {v->uv.x, v->uv.y},
                                              {color.r, color.g, color.b, color.a}};
    }
    const int bytes = (int)(m->index_count * sizeof(mesh_3d_vertex));
    if (g->buffer && m->index_count <= g->capacity) {
        rlUpdateVertexBuffer(g->buffer, mesh_3d_corners, bytes, 0);
    } else {
        if (g->buffer) rlUnloadVertexBuffer(g->buffer);
        g->buffer = rlLoadVertexBuffer(mesh_3d_corners, bytes, false);
        g->capacity = m->index_count;
    }
    g->id = m->id;
    g->other = m->other;
    g->version = m->version;
    g->space = m->space;
    g->epoch = m->epoch;
    g->corners = m->index_count;
    return g;
}

// The list's matrices into the instance buffer, which grows as they need,
// and its 3D meshes onto the GPU, before anything draws.
static void upload_meshes_3d(const tide_draw_list *list)
{
    bool any = false;
    for (uint32_t i = 0; i < step_count; i++) {
        if (steps[i].kind != STEP_MESH_3D) continue;
        if (!any) meshes_3d_start();
        any = true;
        mesh_for(list, steps[i].mesh);
    }
    if (!any) return;
    if (list->matrix_count > mesh_3d_gpu.capacity) {
        uint32_t capacity = mesh_3d_gpu.capacity ? mesh_3d_gpu.capacity : 1024u;
        while (capacity < list->matrix_count) capacity *= 2u;
        if (mesh_3d_gpu.instances) rlUnloadVertexBuffer(mesh_3d_gpu.instances);
        mesh_3d_gpu.instances = rlLoadVertexBuffer(NULL, (int)(capacity * sizeof(tide_float4x4)), true);
        mesh_3d_gpu.capacity = capacity;
    }
    rlUpdateVertexBuffer(mesh_3d_gpu.instances, list->matrices, (int)(list->matrix_count * sizeof(tide_float4x4)), 0);
}

// Lets go of the 3D meshes the frame didn't draw.
static void forget_meshes_3d(void)
{
    for (uint32_t i = 0; i < gpu_mesh_count; i++) {
        if (gpu_meshes[i].buffer && gpu_meshes[i].frame != gpu_frame) {
            rlUnloadVertexBuffer(gpu_meshes[i].buffer);
            gpu_meshes[i] = (gpu_mesh){0};
        }
    }
}

// This list's triangles into their buffer, which grows as they need.
static void upload_meshes(void)
{
    if (mesh_vertex_count == 0) return;
    meshes_start();
    if (mesh_vertex_count > mesh_gpu.capacity) {
        uint32_t capacity = mesh_gpu.capacity ? mesh_gpu.capacity : 4096u;
        while (capacity < mesh_vertex_count) capacity *= 2u;
        rlEnableVertexArray(mesh_gpu.vao);
        if (mesh_gpu.vertices) rlUnloadVertexBuffer(mesh_gpu.vertices);
        mesh_gpu.vertices = rlLoadVertexBuffer(NULL, (int)(capacity * sizeof(mesh_vertex)), true);
        mesh_gpu.capacity = capacity;
        rlSetVertexAttribute((unsigned)mesh_gpu.position, 2, RL_FLOAT, false, sizeof(mesh_vertex),
                             (int)offsetof(mesh_vertex, position));
        rlSetVertexAttribute((unsigned)mesh_gpu.uv, 2, RL_FLOAT, false, sizeof(mesh_vertex), (int)offsetof(mesh_vertex, uv));
        rlSetVertexAttribute((unsigned)mesh_gpu.color, 4, RL_UNSIGNED_BYTE, true, sizeof(mesh_vertex),
                             (int)offsetof(mesh_vertex, color));
        const int attributes[] = {mesh_gpu.position, mesh_gpu.uv, mesh_gpu.color};
        for (int i = 0; i < 3; i++) rlEnableVertexAttribute((unsigned)attributes[i]);
        rlDisableVertexArray();
    }
    rlUpdateVertexBuffer(mesh_gpu.vertices, mesh_vertices, (int)(mesh_vertex_count * sizeof(mesh_vertex)), 0);
}

// The GPU's copy of a list's texture, uploaded if it hasn't got these pixels.
static gpu_texture *texture_for(const tide_draw_texture *t)
{
    gpu_texture *g = NULL;
    for (uint32_t i = 0; i < gpu_texture_count && !g; i++) {
        if (gpu_textures[i].id == t->id && gpu_textures[i].space == t->space) g = &gpu_textures[i];
    }
    if (!g) {
        if (gpu_texture_count == gpu_texture_capacity) {
            const uint32_t capacity = gpu_texture_capacity ? gpu_texture_capacity * 2u : 8u;
            gpu_textures = tide_realloc(gpu_textures, gpu_texture_capacity * sizeof(gpu_texture),
                                        capacity * sizeof(gpu_texture));
            gpu_texture_capacity = capacity;
        }
        g = &gpu_textures[gpu_texture_count++];
        *g = (gpu_texture){.id = t->id, .space = t->space};
    }
    g->frame = gpu_frame;
    if (g->gl && g->version == t->version && g->width == t->width && g->height == t->height && g->epoch == t->epoch) {
        return g;
    }
    if (g->gl && g->width == t->width && g->height == t->height) {
        rlUpdateTexture(g->gl, 0, 0, t->width, t->height, RL_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, t->pixels);
    } else {
        if (g->gl) rlUnloadTexture(g->gl);
        g->gl = rlLoadTexture(t->pixels, t->width, t->height, RL_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1);
        rlTextureParameters(g->gl, RL_TEXTURE_WRAP_S, RL_TEXTURE_WRAP_CLAMP);
        rlTextureParameters(g->gl, RL_TEXTURE_WRAP_T, RL_TEXTURE_WRAP_CLAMP);
        g->filter = -1;
    }
    g->version = t->version;
    g->width = t->width;
    g->height = t->height;
    g->epoch = t->epoch;
    return g;
}

// Lets go of the textures the frame didn't draw with.
static void forget_textures(void)
{
    uint32_t kept = 0;
    for (uint32_t i = 0; i < gpu_texture_count; i++) {
        if (gpu_textures[i].frame != gpu_frame) {
            if (gpu_textures[i].gl) rlUnloadTexture(gpu_textures[i].gl);
            continue;
        }
        gpu_textures[kept++] = gpu_textures[i];
    }
    gpu_texture_count = kept;
    gpu_frame++;
}

// The GPU's texture a mesh step samples, set to sample it as the step does.
static unsigned int step_texture(const tide_draw_list *list, const draw_step *s)
{
    unsigned int texture = rlGetTextureIdDefault(); // A white pixel
    if (s->texture) {
        gpu_texture *g = texture_for(&list->textures[s->texture - 1u]);
        if (g->gl && g->filter != (int)s->filter) {
            const int filter = s->filter == TIDE_FILTER_POINT ? RL_TEXTURE_FILTER_NEAREST : RL_TEXTURE_FILTER_LINEAR;
            rlTextureParameters(g->gl, RL_TEXTURE_MIN_FILTER, filter);
            rlTextureParameters(g->gl, RL_TEXTURE_MAG_FILTER, filter);
            g->filter = (int)s->filter;
        }
        if (g->gl) texture = g->gl;
    }
    return texture;
}

static Matrix to_matrix(const tide_float4x4 m)
{
    // raylib's are named in OpenGL's order: m0 to m3 are the first column
    Matrix r;
    r.m0 = m.c0.x, r.m1 = m.c0.y, r.m2 = m.c0.z, r.m3 = m.c0.w;
    r.m4 = m.c1.x, r.m5 = m.c1.y, r.m6 = m.c1.z, r.m7 = m.c1.w;
    r.m8 = m.c2.x, r.m9 = m.c2.y, r.m10 = m.c2.z, r.m11 = m.c2.w;
    r.m12 = m.c3.x, r.m13 = m.c3.y, r.m14 = m.c3.z, r.m15 = m.c3.w;
    return r;
}

static void draw_mesh_3d(const tide_draw_list *list, const draw_step *s)
{
    const unsigned int texture = step_texture(list, s);
    const gpu_mesh *g = &gpu_meshes[s->mesh - 1u];
    // The camera, fitted to the screen's shape: x divided by its width over its height
    tide_float4x4 world_to_clip = s->camera ? list->matrices[s->camera - 1u]
                                            : tide_draw_camera_3d_matrix(tide_f3(0.0f, 0.0f, 0.0f), tide_identity_q(), 60.0f);
    if (!s->camera || s->fit) {
        const float across = (float)GetScreenHeight() / (float)GetScreenWidth();
        world_to_clip.c0.x *= across, world_to_clip.c1.x *= across;
        world_to_clip.c2.x *= across, world_to_clip.c3.x *= across;
    }
    rlDrawRenderBatchActive(); // What raylib batched before it (text) goes first
    rlEnableDepthTest();
    rlDisableBackfaceCulling(); // Either side of a triangle draws
    rlEnableShader(mesh_3d_gpu.shader);
    rlSetUniformMatrix(mesh_3d_gpu.camera, to_matrix(world_to_clip));
    const int unit = 0;
    rlSetUniform(mesh_3d_gpu.pixels, &unit, RL_SHADER_UNIFORM_INT, 1);
    rlActiveTextureSlot(0);
    rlEnableTexture(texture);
    rlEnableVertexArray(mesh_3d_gpu.vao);
    rlEnableVertexBuffer(g->buffer);
    rlSetVertexAttribute((unsigned)mesh_3d_gpu.position, 3, RL_FLOAT, false, sizeof(mesh_3d_vertex),
                         (int)offsetof(mesh_3d_vertex, position));
    rlSetVertexAttribute((unsigned)mesh_3d_gpu.uv, 2, RL_FLOAT, false, sizeof(mesh_3d_vertex),
                         (int)offsetof(mesh_3d_vertex, uv));
    rlSetVertexAttribute((unsigned)mesh_3d_gpu.color, 4, RL_UNSIGNED_BYTE, true, sizeof(mesh_3d_vertex),
                         (int)offsetof(mesh_3d_vertex, color));
    rlEnableVertexBuffer(mesh_3d_gpu.instances);
    for (int i = 0; i < 4; i++) {
        rlSetVertexAttribute((unsigned)mesh_3d_gpu.model[i], 4, RL_FLOAT, false, sizeof(tide_float4x4),
                             (int)(s->first * sizeof(tide_float4x4) + (uint32_t)i * sizeof(tide_float4)));
    }
    rlDrawVertexArrayInstanced(0, (int)g->corners, (int)s->count);
    rlDisableVertexArray();
    rlDisableTexture();
    rlDisableShader();
    rlEnableBackfaceCulling();
    rlDisableDepthTest(); // Nothing else tests depth
}

static void draw_mesh(const tide_draw_list *list, const draw_step *s)
{
    const unsigned int texture = step_texture(list, s);
    rlDrawRenderBatchActive();  // What raylib batched before it (text) goes first
    rlDisableBackfaceCulling(); // Either side of a triangle draws
    rlEnableShader(mesh_gpu.shader);
    rlSetUniformMatrix(mesh_gpu.mvp, shape_mvp());
    const int unit = 0;
    rlSetUniform(mesh_gpu.pixels, &unit, RL_SHADER_UNIFORM_INT, 1);
    rlActiveTextureSlot(0);
    rlEnableTexture(texture);
    rlEnableVertexArray(mesh_gpu.vao);
    rlDrawVertexArray((int)s->first, (int)s->count);
    rlDisableVertexArray();
    rlDisableTexture();
    rlDisableShader();
    rlEnableBackfaceCulling();
}

// Whether the list is drawn into a render texture of the window's size, in
// its pixels, rather than the window, whose are the display's.
static bool offscreen;

static int nearest_pixel(const float v)
{
    return (int)floorf(v + 0.5f);
}

// Only what's inside the step's rect draws from here on, or everything again.
static void clip(const draw_step *s)
{
    rlDrawRenderBatchActive(); // What raylib batched before it isn't clipped by it
    if (s->kind == STEP_NO_CLIP) {
        rlDisableScissorTest();
        return;
    }
    // OpenGL's are the framebuffer's pixels, from the bottom left
    const Vector2 scale = offscreen ? (Vector2){1.0f, 1.0f} : GetWindowScaleDPI();
#ifdef __APPLE__
    const int height = offscreen ? GetScreenHeight() : nearest_pixel((float)GetScreenHeight() * scale.y);
#else
    const int height = offscreen ? GetScreenHeight() : GetRenderHeight();
#endif
    const int left = nearest_pixel(s->at.x * scale.x), right = nearest_pixel(s->to.x * scale.x);
    const int top = nearest_pixel(s->at.y * scale.y), bottom = nearest_pixel(s->to.y * scale.y);
    rlEnableScissorTest();
    rlScissor(left, height - bottom, right > left ? right - left : 0, bottom > top ? bottom - top : 0);
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
    mesh_vertex_count = 0;
    step_count = 0;
    camera cam = {{0.0f, 0.0f}, 1.0f, false};
    camera world = cam; // The last world camera, for tide_platform_world_to_screen
    uint32_t camera_3d = 0, fit = 1; // The 3D meshes' (see draw_step)
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
        case TIDE_DRAW_SCREEN:
            cam.scale = 1.0f;
            cam.gui = true;
            break;
        case TIDE_DRAW_MESH:
            add_mesh(list, c, &cam);
            break;
        case TIDE_DRAW_CAMERA_3D:
            camera_3d = c->camera.matrix + 1u;
            fit = c->camera.fit;
            break;
        case TIDE_DRAW_MESH_3D: {
            if (!c->mesh3.mesh || c->mesh3.mesh > list->mesh_count) break; // Forgotten (see tide_draw_forget)
            draw_step *s = add_step(STEP_MESH_3D);
            s->first = c->mesh3.first;
            s->count = c->mesh3.count;
            s->texture = c->mesh3.texture;
            s->filter = c->mesh3.filter;
            s->mesh = c->mesh3.mesh;
            s->camera = camera_3d;
            s->fit = fit;
            break;
        }
        case TIDE_DRAW_CLIP: {
            // Its corners on the screen, whichever way up the units are
            const Vector2 p = to_screen(&cam, c->a);
            const Vector2 q = to_screen(&cam, tide_f2(c->a.x + c->b.x, c->a.y + c->b.y));
            draw_step *s = add_step(STEP_CLIP);
            s->at = (Vector2){p.x < q.x ? p.x : q.x, p.y < q.y ? p.y : q.y};
            s->to = c->b.x > 0.0f && c->b.y > 0.0f ? (Vector2){p.x < q.x ? q.x : p.x, p.y < q.y ? q.y : p.y} : s->at;
            break;
        }
        case TIDE_DRAW_NO_CLIP:
            add_step(STEP_NO_CLIP);
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
    upload_meshes();
    upload_meshes_3d(list);
    rlDrawRenderBatchActive(); // What came before the list goes under it, and is cleared
    ClearBackground(BLACK);    // Every frame starts black; Draw.Clear picks another color
    bool clipped = false;
    for (uint32_t i = 0; i < step_count; i++) {
        const draw_step *s = &steps[i];
        if (s->kind == STEP_SHAPES) {
            draw_shapes(s->first, s->count);
        } else if (s->kind == STEP_CLEAR) {
            rlDrawRenderBatchActive(); // Text before it is cleared too
            ClearBackground(s->color);
        } else if (s->kind == STEP_MESH) {
            draw_mesh(list, s);
        } else if (s->kind == STEP_MESH_3D) {
            draw_mesh_3d(list, s);
        } else if (s->kind == STEP_CLIP || s->kind == STEP_NO_CLIP) {
            clip(s);
            clipped = s->kind == STEP_CLIP;
        } else {
            DrawTextEx(GetFontDefault(), list->text + s->first, s->at, s->size, s->size / 10.0f, s->color);
        }
    }
    if (clipped) clip(&(draw_step){.kind = STEP_NO_CLIP}); // What's drawn after the list isn't the list's to clip
    forget_meshes_3d();
    forget_textures();
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
    offscreen = true;
    draw_list(list);
    offscreen = false;
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
