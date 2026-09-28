// raylib's platform backend for PurrEngine's web builds: a canvas with WebGL 2
// through our own JavaScript (platform/web/purr.js) instead of Emscripten.
// rcore.c includes it in place of its own backends (see cmake/Raylib.cmake),
// so it sees raylib's internal CORE state. Based on raylib's
// platforms/rcore_template.c.
//
// Keys and gamepads bypass raylib: the platform layer reads them from the page
// directly, by physical position (platform/src/platform.c).

#include <time.h>

#include "purr_web.h"

extern CoreData CORE;

int InitPlatform(void);

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
Vector2 GetWindowScaleDPI(void) { return (Vector2){1, 1}; }
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
    const int buttons = purr_web_mouse_buttons();
    for (int i = 0; i < MAX_MOUSE_BUTTONS && i < 5; i++) CORE.Input.Mouse.currentButtonState[i] = (buttons >> i) & 1;
    CORE.Input.Mouse.previousWheelMove = CORE.Input.Mouse.currentWheelMove;
    CORE.Input.Mouse.currentWheelMove = (Vector2){purr_web_take_wheel_x(), purr_web_take_wheel_y()};
    CORE.Input.Mouse.previousPosition = CORE.Input.Mouse.currentPosition;
    CORE.Input.Mouse.currentPosition = (Vector2){purr_web_mouse_x(), purr_web_mouse_y()};

    // A page-filling canvas follows the browser window.
    const unsigned width = (unsigned)purr_web_canvas_width();
    const unsigned height = (unsigned)purr_web_canvas_height();
    CORE.Window.resizedLastFrame = width != CORE.Window.screen.width || height != CORE.Window.screen.height;
    if (CORE.Window.resizedLastFrame) {
        CORE.Window.screen = CORE.Window.render = CORE.Window.currentFbo = CORE.Window.display = (Size){width, height};
        SetupViewport((int)width, (int)height);
    }
}

// ---------------------------------------------------------------------------

int InitPlatform(void)
{
    const bool resizable = FLAG_IS_SET(CORE.Window.flags, FLAG_WINDOW_RESIZABLE);
    if (!purr_web_init_canvas((int)CORE.Window.screen.width, (int)CORE.Window.screen.height, resizable)) {
        TRACELOG(LOG_FATAL, "PLATFORM: this browser has no WebGL 2");
        return -1;
    }
    const Size size = {(unsigned)purr_web_canvas_width(), (unsigned)purr_web_canvas_height()};
    CORE.Window.screen = CORE.Window.render = CORE.Window.currentFbo = CORE.Window.display = size;
    CORE.Window.ready = true;
    InitTimer();
    CORE.Storage.basePath = GetWorkingDirectory();
    return 0;
}

void ClosePlatform(void) { }
