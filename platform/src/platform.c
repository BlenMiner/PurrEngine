#include "tide/platform.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <raylib.h>

#ifdef __wasm__
#include "tide_web.h" // The page's JavaScript, which web builds use instead of raylib for input and frames
#else
// GLFW, which raylib's desktop windows run on, built into raylib: waiting on
// the window's events, rather than sleeping past them.
typedef struct GLFWwindow GLFWwindow;
void glfwWaitEventsTimeout(double timeout);
int glfwWindowShouldClose(GLFWwindow *window);
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
#ifndef __wasm__
    // The browser paces web frames itself.
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
#ifndef __wasm__
    // A minimized window has no vsync to wait for: the next frame comes when
    // the frame function asked, and draws nothing (see tide_platform_draw).
    // It waits on the window's events, so a window restored or closed meanwhile
    // goes on at once.
    while (IsWindowMinimized() && !glfwWindowShouldClose(GetWindowHandle())) {
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

static void poll_mouse(tide_mouse *m)
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
    for (size_t i = 0; i < COUNT_OF(mouse_buttons); i++)
        tide_button_set(BUTTON_AT(m, mouse_buttons[i].offset), IsMouseButtonDown(mouse_buttons[i].raylib));
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

// Characters typed since the last poll, which follow the keyboard layout.
static void poll_text(tide_typed *text)
{
    text->count = 0;
    for (;;) {
#ifdef __wasm__
        const int c = tide_web_take_char();
#else
        const int c = GetCharPressed();
#endif
        if (c <= 0) break;
        if (text->count < TIDE_TEXT_MAX) text->chars[text->count++] = (uint32_t)c;
    }
}

void tide_platform_poll(tide_devices *devices)
{
    poll_keyboard(&devices->keyboard);
    poll_mouse(&devices->mouse);
    poll_gamepad(&devices->gamepad);
    poll_text(&devices->text);
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

static void draw_list(const tide_draw_list *list)
{
    ClearBackground(BLACK); // Every frame starts black; Draw.Clear picks another color
    camera cam = {{0.0f, 0.0f}, 1.0f, false};
    camera world = cam; // The last world camera, for tide_platform_world_to_screen
    for (uint32_t i = 0; i < list->count; i++) {
        const tide_draw_command *c = &list->commands[i];
        const Color color = to_raylib(c->color);
        switch ((tide_draw_kind)c->kind) {
        case TIDE_DRAW_CLEAR:
            ClearBackground(color);
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
            DrawCircleV(to_screen(&cam, c->a), c->b.x * cam.scale, color);
            break;
        case TIDE_DRAW_WIRE_CIRCLE:
            DrawCircleLinesV(to_screen(&cam, c->a), c->b.x * cam.scale, color);
            break;
        case TIDE_DRAW_RECT:
        case TIDE_DRAW_WIRE_RECT: {
            const Vector2 top_left = rect_corner(&cam, c);
            const Rectangle r = {top_left.x, top_left.y, c->b.x * cam.scale, c->b.y * cam.scale};
            if (c->kind == TIDE_DRAW_RECT) DrawRectangleRec(r, color);
            else DrawRectangleLinesEx(r, 1.0f, color);
            break;
        }
        case TIDE_DRAW_LINE:
            DrawLineV(to_screen(&cam, c->a), to_screen(&cam, c->b), color);
            break;
        case TIDE_DRAW_TEXT: {
            const float size = c->b.x * cam.scale;
            DrawTextEx(GetFontDefault(), list->text + c->text, to_screen(&cam, c->a), size, size / 10.0f, color);
            break;
        }
        }
    }
    last_camera = world;

    static bool warned;
    if (list->dropped > 0 && !warned) {
        TraceLog(LOG_WARNING, "DRAW: %u commands didn't fit in the draw list (raise TIDE_DRAW_MAX_COMMANDS)",
                 (unsigned)list->dropped);
        warned = true;
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
    DrawText(text, GetScreenWidth() - MeasureText(text, size) - 12, 8, size, GRAY);
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
