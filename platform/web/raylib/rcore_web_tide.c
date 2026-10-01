// raylib's platform backend for Tide's web builds: a canvas with WebGL 2
// through our own JavaScript (platform/web/tide.js) instead of Emscripten.
// rcore.c includes it in place of its own backends (see cmake/Raylib.cmake),
// so it sees raylib's internal CORE state. Based on raylib's
// platforms/rcore_template.c.
//
// Keys and gamepads bypass raylib: the platform layer reads them from the page
// directly, by physical position (platform/src/platform.c).
//
// The screen is the canvas in CSS pixels, which the program draws and reads the
// mouse in, as desktop builds do in the display's logical pixels. raylib scales
// its drawing up to the canvas's own pixels, devicePixelRatio times as many, so
// it's sharp.

#include <time.h>

#include "tide_web.h"

extern CoreData CORE;

int InitPlatform(void);

// Takes the canvas's sizes (see tide_web.h), without the viewport, which
// needs rlgl. Returns whether they changed.
static bool FitCanvas(void)
{
    const Size screen = {(unsigned)tide_web_canvas_width(), (unsigned)tide_web_canvas_height()};
    const Size render = {(unsigned)tide_web_canvas_pixel_width(), (unsigned)tide_web_canvas_pixel_height()};
    if (screen.width == CORE.Window.screen.width && screen.height == CORE.Window.screen.height &&
        render.width == CORE.Window.render.width && render.height == CORE.Window.render.height)
        return false;
    CORE.Window.screen = CORE.Window.display = screen;
    CORE.Window.render = CORE.Window.currentFbo = render;
    const Vector2 scale = GetWindowScaleDPI();
    CORE.Window.screenScale = MatrixScale(scale.x, scale.y, 1.0f);
    return true;
}

// ---------------------------------------------------------------------------
// Window: the canvas, which can't move, close or change mode

bool WindowShouldClose(void) { return false; }
void ToggleFullscreen(void) { }
void ToggleBorderlessWindowed(void) { }
void MaximizeWindow(void) { }
void MinimizeWindow(void) { }
void RestoreWindow(void) { }
void SetWindowState(unsigned int flags) { (void)flags; }
void ClearWindowState(unsigned int flags) { (void)flags; }
void SetWindowIcon(Image image) { (void)image; }
void SetWindowIcons(Image *images, int count) { (void)images, (void)count; }
void SetWindowTitle(const char *title) { CORE.Window.title = title; }
void SetWindowPosition(int x, int y) { (void)x, (void)y; }
void SetWindowMonitor(int monitor) { (void)monitor; }
void SetWindowMinSize(int width, int height) { CORE.Window.screenMin = (Size){(unsigned)width, (unsigned)height}; }
void SetWindowMaxSize(int width, int height) { CORE.Window.screenMax = (Size){(unsigned)width, (unsigned)height}; }
void SetWindowSize(int width, int height) { (void)width, (void)height; }
void SetWindowOpacity(float opacity) { (void)opacity; }
void SetWindowFocused(void) { }
void *GetWindowHandle(void) { return NULL; }
int GetMonitorCount(void) { return 1; }
int GetCurrentMonitor(void) { return 0; }
Vector2 GetMonitorPosition(int monitor) { (void)monitor; return (Vector2){0, 0}; }
int GetMonitorWidth(int monitor) { (void)monitor; return (int)CORE.Window.display.width; }
int GetMonitorHeight(int monitor) { (void)monitor; return (int)CORE.Window.display.height; }
int GetMonitorPhysicalWidth(int monitor) { (void)monitor; return 0; }
int GetMonitorPhysicalHeight(int monitor) { (void)monitor; return 0; }
int GetMonitorRefreshRate(int monitor) { (void)monitor; return 60; }
const char *GetMonitorName(int monitor) { (void)monitor; return "canvas"; }
Vector2 GetWindowPosition(void) { return (Vector2){0, 0}; }
Vector2 GetWindowScaleDPI(void)
{
    const Size s = CORE.Window.screen, r = CORE.Window.render;
    return (Vector2){s.width ? (float)r.width / (float)s.width : 1.0f, s.height ? (float)r.height / (float)s.height : 1.0f};
}
void SetClipboardText(const char *text) { (void)text; }
const char *GetClipboardText(void) { return ""; }
Image GetClipboardImage(void) { return (Image){0}; }
void ShowCursor(void) { CORE.Input.Mouse.cursorHidden = false; }
void HideCursor(void) { CORE.Input.Mouse.cursorHidden = true; }
void EnableCursor(void) { CORE.Input.Mouse.cursorLocked = false; }
void DisableCursor(void) { CORE.Input.Mouse.cursorLocked = true; }

// The browser shows the frame when the animation frame callback returns.
void SwapScreenBuffer(void) { }

// ---------------------------------------------------------------------------
// Misc

// Seconds since InitTimer (WASI's clock is the page's performance.now).
double GetTime(void)
{
    struct timespec ts = {0};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const unsigned long long ns = (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
    return (double)(ns - CORE.Time.base) * 1e-9;
}

void OpenURL(const char *url) { (void)url; }

// ---------------------------------------------------------------------------
// Input

int SetGamepadMappings(const char *mappings) { (void)mappings; return 0; }
void SetGamepadVibration(int gamepad, float left, float right, float duration)
{
    (void)gamepad, (void)left, (void)right, (void)duration;
}
void SetMousePosition(int x, int y)
{
    CORE.Input.Mouse.currentPosition = (Vector2){(float)x, (float)y};
    CORE.Input.Mouse.previousPosition = CORE.Input.Mouse.currentPosition;
}
void SetMouseCursor(int cursor) { CORE.Input.Mouse.cursor = cursor; }
const char *GetKeyName(int key) { (void)key; return ""; }

void PollInputEvents(void)
{
    CORE.Input.Keyboard.keyPressedQueueCount = 0;
    CORE.Input.Keyboard.charPressedQueueCount = 0;
    for (int i = 0; i < MAX_KEYBOARD_KEYS; i++) {
        CORE.Input.Keyboard.previousKeyState[i] = CORE.Input.Keyboard.currentKeyState[i];
        CORE.Input.Keyboard.keyRepeatInFrame[i] = 0;
    }

    for (int i = 0; i < MAX_MOUSE_BUTTONS; i++) {
        CORE.Input.Mouse.previousButtonState[i] = CORE.Input.Mouse.currentButtonState[i];
    }
    const int buttons = tide_web_mouse_buttons();
    for (int i = 0; i < MAX_MOUSE_BUTTONS && i < 5; i++) CORE.Input.Mouse.currentButtonState[i] = (buttons >> i) & 1;
    CORE.Input.Mouse.previousWheelMove = CORE.Input.Mouse.currentWheelMove;
    CORE.Input.Mouse.currentWheelMove = (Vector2){tide_web_take_wheel_x(), tide_web_take_wheel_y()};
    CORE.Input.Mouse.previousPosition = CORE.Input.Mouse.currentPosition;
    CORE.Input.Mouse.currentPosition = (Vector2){tide_web_mouse_x(), tide_web_mouse_y()};

    // A page-filling canvas follows the browser window, and every canvas the
    // page's zoom and screen.
    CORE.Window.resizedLastFrame = FitCanvas();
    if (CORE.Window.resizedLastFrame) SetupViewport((int)CORE.Window.render.width, (int)CORE.Window.render.height);
}

// ---------------------------------------------------------------------------

// The page's JavaScript provides every GL function as an import, so there's
// nothing to look up.
static void *GetProcAddress(const char *name)
{
    (void)name;
    return NULL;
}

int InitPlatform(void)
{
    const bool resizable = FLAG_IS_SET(CORE.Window.flags, FLAG_WINDOW_RESIZABLE);
    if (!tide_web_init_canvas((int)CORE.Window.screen.width, (int)CORE.Window.screen.height, resizable)) {
        TRACELOG(LOG_FATAL, "PLATFORM: this browser has no WebGL 2");
        return -1;
    }
    // What the GL supports, VAOs among it: without them, rlgl binds its
    // attributes on every draw, the default shader's missing normal included.
    rlLoadExtensions((void *)GetProcAddress);
    FitCanvas();
    CORE.Window.ready = true;
    InitTimer();
    CORE.Storage.basePath = GetWorkingDirectory();
    return 0;
}

void ClosePlatform(void) { }
