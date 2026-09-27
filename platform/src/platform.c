#include "purr/platform.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <raylib.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#endif

#define COUNT_OF(array) (sizeof(array) / sizeof((array)[0]))
#define PLUS_ONE(name) +1

// Buttons are found by their offset in the device struct, so one table covers
// every key and a missing one fails to compile.
#define BUTTON_AT(device, offset) ((purr_button *)((char *)(device) + (offset)))

// Every PurrLang key with its raylib key (desktop) and DOM `code` (web). Both
// name physical positions after the US layout, as PurrLang does.
typedef struct key_binding {
    size_t offset; // In purr_keyboard
    int raylib;
    const char *dom;
} key_binding;

#define KEY(name, raylib, dom) {offsetof(purr_keyboard, name), raylib, dom}

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

_Static_assert(COUNT_OF(keys) == 0 PURR_KEYBOARD_KEYS(PLUS_ONE), "every key in devices.h needs a binding");

typedef struct button_binding {
    size_t offset;
    int raylib;
} button_binding;

static const button_binding mouse_buttons[] = {
    {offsetof(purr_mouse, left), MOUSE_BUTTON_LEFT},
    {offsetof(purr_mouse, right), MOUSE_BUTTON_RIGHT},
    {offsetof(purr_mouse, middle), MOUSE_BUTTON_MIDDLE},
    {offsetof(purr_mouse, back), MOUSE_BUTTON_SIDE},     // GLFW button 4, what mice send for back
    {offsetof(purr_mouse, forward), MOUSE_BUTTON_EXTRA}, // GLFW button 5
};

_Static_assert(COUNT_OF(mouse_buttons) == 0 PURR_MOUSE_BUTTONS(PLUS_ONE), "every mouse button needs a binding");

static const button_binding gamepad_buttons[] = {
    {offsetof(purr_gamepad, buttonSouth), GAMEPAD_BUTTON_RIGHT_FACE_DOWN},
    {offsetof(purr_gamepad, buttonEast), GAMEPAD_BUTTON_RIGHT_FACE_RIGHT},
    {offsetof(purr_gamepad, buttonWest), GAMEPAD_BUTTON_RIGHT_FACE_LEFT},
    {offsetof(purr_gamepad, buttonNorth), GAMEPAD_BUTTON_RIGHT_FACE_UP},
    {offsetof(purr_gamepad, leftShoulder), GAMEPAD_BUTTON_LEFT_TRIGGER_1},
    {offsetof(purr_gamepad, rightShoulder), GAMEPAD_BUTTON_RIGHT_TRIGGER_1},
    {offsetof(purr_gamepad, leftStickButton), GAMEPAD_BUTTON_LEFT_THUMB},
    {offsetof(purr_gamepad, rightStickButton), GAMEPAD_BUTTON_RIGHT_THUMB},
    {offsetof(purr_gamepad, start), GAMEPAD_BUTTON_MIDDLE_RIGHT},
    {offsetof(purr_gamepad, select), GAMEPAD_BUTTON_MIDDLE_LEFT},
    {offsetof(purr_gamepad, dpad.up), GAMEPAD_BUTTON_LEFT_FACE_UP},
    {offsetof(purr_gamepad, dpad.down), GAMEPAD_BUTTON_LEFT_FACE_DOWN},
    {offsetof(purr_gamepad, dpad.left), GAMEPAD_BUTTON_LEFT_FACE_LEFT},
    {offsetof(purr_gamepad, dpad.right), GAMEPAD_BUTTON_LEFT_FACE_RIGHT},
};

_Static_assert(COUNT_OF(gamepad_buttons) == 0 PURR_GAMEPAD_BUTTONS(PLUS_ONE) PURR_DPAD_BUTTONS(PLUS_ONE),
               "every gamepad button needs a binding");

#ifdef __EMSCRIPTEN__
// raylib reads web keys by the character they type, which depends on the
// keyboard layout. Physical positions come from the DOM's `code` instead.
static bool web_key_held[COUNT_OF(keys)];

// Keys whose browser default (scrolling, moving focus) would fight the game.
static bool browser_default_interferes(const int key)
{
    switch (key) {
    case KEY_SPACE: case KEY_TAB: case KEY_BACKSPACE:
    case KEY_UP: case KEY_DOWN: case KEY_LEFT: case KEY_RIGHT:
    case KEY_PAGE_UP: case KEY_PAGE_DOWN: case KEY_HOME: case KEY_END:
        return true;
    default:
        return false;
    }
}

static EM_BOOL on_web_key(const int type, const EmscriptenKeyboardEvent *event, void *user)
{
    (void)user;
    for (size_t i = 0; i < COUNT_OF(keys); i++) {
        if (strcmp(event->code, keys[i].dom) == 0) {
            web_key_held[i] = type == EMSCRIPTEN_EVENT_KEYDOWN;
            return browser_default_interferes(keys[i].raylib);
        }
    }
    return false;
}

// Keys released while the page is in the background never send keyup.
static EM_BOOL on_web_blur(const int type, const EmscriptenFocusEvent *event, void *user)
{
    (void)type, (void)event, (void)user;
    memset(web_key_held, 0, sizeof web_key_held);
    return false;
}

static bool web_timer_frames; // Frames on timers instead of animation frames
#endif

static purr_frame_fn run_frame;
static void *run_user;

void purr_platform_open(const purr_window_desc *desc)
{
    unsigned int flags = 0;
    // On the web, a resizable window is a canvas that fills the page. Tests
    // keep the size they asked for.
    if (!desc->hidden) flags |= FLAG_WINDOW_RESIZABLE;
#ifndef __EMSCRIPTEN__
    // The browser paces web frames itself.
    flags |= desc->hidden ? FLAG_WINDOW_HIDDEN : FLAG_VSYNC_HINT;
#endif
    SetConfigFlags(flags);
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(desc->width, desc->height, desc->title);
    SetExitKey(KEY_NULL); // Escape belongs to the game

#ifdef __EMSCRIPTEN__
    web_timer_frames = desc->hidden;
    const char *target = EMSCRIPTEN_EVENT_TARGET_WINDOW;
    emscripten_set_keydown_callback(target, NULL, false, on_web_key);
    emscripten_set_keyup_callback(target, NULL, false, on_web_key);
    emscripten_set_blur_callback(target, NULL, false, on_web_blur);
#endif
}

_Noreturn static void finish(const int code)
{
#ifdef __EMSCRIPTEN__
    emscripten_cancel_main_loop();
#endif
    CloseWindow();
    exit(code);
}

static int step(void)
{
    BeginDrawing();
    const int code = run_frame(run_user, GetFrameTime());
    EndDrawing(); // Also polls the OS for input
    return code;
}

#ifdef __EMSCRIPTEN__
static void web_step(void)
{
    const int code = step();
    if (code != PURR_KEEP_RUNNING) finish(code);
}
#endif

void purr_platform_run(const purr_frame_fn frame, void *user)
{
    run_frame = frame;
    run_user = user;
#ifdef __EMSCRIPTEN__
    // Frames on requestAnimationFrame, or as fast as timers allow when hidden.
    // Doesn't return: it unwinds main's stack.
    emscripten_set_main_loop(web_step, web_timer_frames ? 1000 : 0, true);
    __builtin_unreachable();
#else
    for (;;) {
        if (WindowShouldClose()) finish(0);
        const int code = step();
        if (code != PURR_KEEP_RUNNING) finish(code);
    }
#endif
}

static void poll_keyboard(purr_keyboard *k)
{
    for (size_t i = 0; i < COUNT_OF(keys); i++) {
#ifdef __EMSCRIPTEN__
        const bool held = web_key_held[i];
#else
        const bool held = IsKeyDown(keys[i].raylib);
#endif
        purr_button_set(BUTTON_AT(k, keys[i].offset), held);
    }
}

static void poll_mouse(purr_mouse *m)
{
    const Vector2 position = GetMousePosition();
    const Vector2 delta = GetMouseDelta();
    const Vector2 scroll = GetMouseWheelMoveV();
    // raylib counts from the top left, y down; Devices follows Unity: from the
    // bottom left, y up.
    m->position = purr_f2(position.x, (float)GetScreenHeight() - position.y);
    // Delta and scroll add up until the input is sampled.
    m->delta = purr_f2(m->delta.x + delta.x, m->delta.y - delta.y);
    m->scroll = purr_f2(m->scroll.x + scroll.x, m->scroll.y + scroll.y);
    for (size_t i = 0; i < COUNT_OF(mouse_buttons); i++)
        purr_button_set(BUTTON_AT(m, mouse_buttons[i].offset), IsMouseButtonDown(mouse_buttons[i].raylib));
}

// 0 to 1. `web_button` is the trigger in the browser's standard gamepad mapping.
static float gamepad_trigger(const int pad, const int axis, const int web_button)
{
#ifdef __EMSCRIPTEN__
    // Browsers report triggers as analog buttons, not axes.
    (void)axis;
    EmscriptenGamepadEvent state;
    if (emscripten_get_gamepad_status(pad, &state) == EMSCRIPTEN_RESULT_SUCCESS && web_button < state.numButtons)
        return (float)state.analogButton[web_button];
    return 0.0f;
#else
    (void)web_button;
    return (GetGamepadAxisMovement(pad, axis) + 1.0f) * 0.5f; // raylib: -1 released, 1 fully pressed
#endif
}

static void poll_gamepad(purr_gamepad *g)
{
    const int pad = 0;
    g->connected = IsGamepadAvailable(pad);
    if (g->connected) {
        g->leftStick = purr_f2(GetGamepadAxisMovement(pad, GAMEPAD_AXIS_LEFT_X),
                               -GetGamepadAxisMovement(pad, GAMEPAD_AXIS_LEFT_Y));
        g->rightStick = purr_f2(GetGamepadAxisMovement(pad, GAMEPAD_AXIS_RIGHT_X),
                                -GetGamepadAxisMovement(pad, GAMEPAD_AXIS_RIGHT_Y));
        g->leftTrigger = gamepad_trigger(pad, GAMEPAD_AXIS_LEFT_TRIGGER, 6);
        g->rightTrigger = gamepad_trigger(pad, GAMEPAD_AXIS_RIGHT_TRIGGER, 7);
    } else {
        g->leftStick = g->rightStick = purr_f2(0.0f, 0.0f);
        g->leftTrigger = g->rightTrigger = 0.0f;
    }
    for (size_t i = 0; i < COUNT_OF(gamepad_buttons); i++) {
        const bool held = g->connected && IsGamepadButtonDown(pad, gamepad_buttons[i].raylib);
        purr_button_set(BUTTON_AT(g, gamepad_buttons[i].offset), held);
    }
}

void purr_platform_poll(purr_devices *devices)
{
    poll_keyboard(&devices->keyboard);
    poll_mouse(&devices->mouse);
    poll_gamepad(&devices->gamepad);
}

// The camera maps world units (y up) to window pixels (y down). Each frame's
// list starts at the origin with 1 unit per pixel.
typedef struct camera {
    purr_float2 center;
    float scale; // Pixels per world unit
} camera;

static camera last_camera = {{0.0f, 0.0f}, 1.0f};

static Vector2 to_screen(const camera *cam, const purr_float2 p)
{
    return (Vector2){(float)GetScreenWidth() * 0.5f + (p.x - cam->center.x) * cam->scale,
                     (float)GetScreenHeight() * 0.5f - (p.y - cam->center.y) * cam->scale};
}

static unsigned char color_channel(const float v)
{
    if (!(v > 0.0f)) return 0; // Also NaN
    if (v >= 1.0f) return 255;
    return (unsigned char)(v * 255.0f + 0.5f);
}

static Color to_raylib(const purr_color c)
{
    return (Color){color_channel(c.r), color_channel(c.g), color_channel(c.b), color_channel(c.a)};
}

void purr_platform_draw(const purr_draw_list *list)
{
    ClearBackground(BLACK); // Every frame starts black; Draw.Clear picks another color
    camera cam = {{0.0f, 0.0f}, 1.0f};
    for (uint32_t i = 0; i < list->count; i++) {
        const purr_draw_command *c = &list->commands[i];
        const Color color = to_raylib(c->color);
        switch ((purr_draw_kind)c->kind) {
        case PURR_DRAW_CLEAR:
            ClearBackground(color);
            break;
        case PURR_DRAW_CAMERA:
            cam.center = c->a;
            cam.scale = c->b.x > 0.0f ? (float)GetScreenHeight() / (2.0f * c->b.x) : 1.0f;
            break;
        case PURR_DRAW_CIRCLE:
            DrawCircleV(to_screen(&cam, c->a), c->b.x * cam.scale, color);
            break;
        case PURR_DRAW_WIRE_CIRCLE:
            DrawCircleLinesV(to_screen(&cam, c->a), c->b.x * cam.scale, color);
            break;
        case PURR_DRAW_RECT:
        case PURR_DRAW_WIRE_RECT: {
            const Vector2 top_left = to_screen(&cam, purr_f2(c->a.x - c->b.x * 0.5f, c->a.y + c->b.y * 0.5f));
            const Rectangle r = {top_left.x, top_left.y, c->b.x * cam.scale, c->b.y * cam.scale};
            if (c->kind == PURR_DRAW_RECT) DrawRectangleRec(r, color);
            else DrawRectangleLinesEx(r, 1.0f, color);
            break;
        }
        case PURR_DRAW_LINE:
            DrawLineV(to_screen(&cam, c->a), to_screen(&cam, c->b), color);
            break;
        case PURR_DRAW_TEXT: {
            const float size = c->b.x * cam.scale;
            DrawTextEx(GetFontDefault(), list->text + c->text, to_screen(&cam, c->a), size, size / 10.0f, color);
            break;
        }
        }
    }
    last_camera = cam;

    static bool warned;
    if (list->dropped > 0 && !warned) {
        TraceLog(LOG_WARNING, "DRAW: %u commands didn't fit in the draw list (raise PURR_DRAW_MAX_COMMANDS)",
                 (unsigned)list->dropped);
        warned = true;
    }
}

purr_float2 purr_platform_world_to_screen(const purr_float2 world)
{
    const Vector2 p = to_screen(&last_camera, world);
    return purr_f2(p.x, p.y);
}

void purr_platform_draw_overlay(const char *text)
{
    const int size = 16;
    DrawText(text, GetScreenWidth() - MeasureText(text, size) - 12, 8, size, GRAY);
}

int purr_platform_fps(void)
{
    return GetFPS();
}

void purr_platform_read_pixels(const purr_draw_list *list, const purr_float2 *points, const int count, uint32_t *rgba)
{
    const int height = GetScreenHeight();
    const RenderTexture2D target = LoadRenderTexture(GetScreenWidth(), height);
    BeginTextureMode(target);
    purr_platform_draw(list);
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
