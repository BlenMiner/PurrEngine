// raylib's platform backend for Tide's Android builds: Android's NativeActivity,
// with OpenGL ES 3 through EGL, and no Java of our own. rcore.c includes it in
// place of its own backends (see cmake/Raylib.cmake), so it sees raylib's
// internal CORE state. Based on raylib's platforms/rcore_template.c, like the
// web's.
//
// Android starts the app by loading its library and calling
// ANativeActivity_onCreate on its main thread, which mustn't block. The program
// runs main() on a thread of its own, as on desktop. The activity's callbacks
// say what changed (a window to draw in, the input queue, the activity going)
// under `app.lock`, and wake the program's looper; one that takes something
// away (the window, the input queue) waits until the program let it go.
//
// Keys and fingers bypass raylib's own handling: keys go to raylib's key state,
// which the platform layer reads by physical position (Android's key codes are
// positions), and fingers go to the platform layer as events (native.h). A key,
// mouse button or finger pressed and released between two polls reads as held
// for one, as on the web.
//
// The screen is the window in the display's logical pixels, as a browser's CSS
// pixels are: its own pixels over (dpi / 160). raylib scales its drawing up to
// the window's own pixels, so it's sharp.

#include <android/configuration.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <EGL/egl.h>
#include <fcntl.h>
#include <jni.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "native.h"

extern CoreData CORE;

int InitPlatform(void);
int main(int argc, char **argv); // The program's

enum { LOOPER_WAKE = 1, LOOPER_INPUT = 2 }; // What woke the program's looper

static struct {
    pthread_mutex_t lock;
    pthread_cond_t taken; // The program took what the activity changed
    int wake[2];          // A pipe: the activity writes a byte when something changed

    // The activity's thread sets these, under the lock.
    ANativeActivity *activity;
    ANativeWindow *window_given; // The window to draw in, NULL when there's none
    AInputQueue *queue_given;
    bool destroyed; // The activity is gone: the program ends
    bool running;   // The program's thread runs: a callback that waits has someone to wait for

    // ...and the program's, under the lock too, for those that wait.
    ANativeWindow *window_taken;
    AInputQueue *queue_taken;

    // The program's own.
    ALooper *looper;
    AInputQueue *queue;
    ANativeWindow *window;
    EGLDisplay display;
    EGLConfig config;
    EGLContext context;
    EGLSurface surface;
    float scale; // The window's pixels per logical pixel

    // Input: what's held, and what was pressed since the last poll.
    bool keys_held[MAX_KEYBOARD_KEYS], keys_tapped[MAX_KEYBOARD_KEYS];
    int mouse_held, mouse_tapped; // Bits in raylib's order: left, right, middle, back, forward
    Vector2 mouse_at, wheel;
    tide_touch_report touches[256];
    unsigned touch_start, touch_count;
} app = {.lock = PTHREAD_MUTEX_INITIALIZER, .taken = PTHREAD_COND_INITIALIZER, .wake = {-1, -1}};

// ---------------------------------------------------------------------------
// The program's output, which Android drops: to the system's log, a line each.

static void *log_lines(void *pipe_read)
{
    const int from = (int)(intptr_t)pipe_read;
    char line[1024];
    size_t used = 0;
    for (;;) {
        const ssize_t got = read(from, line + used, sizeof line - 1 - used);
        if (got <= 0) return NULL;
        used += (size_t)got;
        size_t start = 0;
        for (size_t i = 0; i < used; i++) {
            if (line[i] != '\n') continue;
            line[i] = '\0';
            __android_log_write(ANDROID_LOG_INFO, "tide", line + start);
            start = i + 1;
        }
        if (start == 0 && used == sizeof line - 1) { // A line longer than the buffer: in pieces
            line[used] = '\0';
            __android_log_write(ANDROID_LOG_INFO, "tide", line);
            used = 0;
        } else {
            memmove(line, line + start, used - start);
            used -= start;
        }
    }
}

static void log_output(void)
{
    int fds[2];
    if (pipe(fds) != 0) return;
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);
    pthread_t thread;
    if (pthread_create(&thread, NULL, log_lines, (void *)(intptr_t)fds[0]) == 0) pthread_detach(thread);
}

// ---------------------------------------------------------------------------
// The activity's thread

static void wake(void)
{
    const char byte = 1;
    if (write(app.wake[1], &byte, 1) < 0) { } // Already awake
}

// Gives the program a window or input queue, or takes it away, with NULL:
// then waits until the program let go of the one it had, if it took it. A
// program that opens no window never takes one.
static void give(ANativeWindow *window, AInputQueue *queue, const bool is_window)
{
    pthread_mutex_lock(&app.lock);
    if (is_window) app.window_given = window;
    else app.queue_given = queue;
    wake();
    const bool taking = is_window ? window == NULL : queue == NULL;
    while (taking && app.running && (is_window ? app.window_taken != NULL : app.queue_taken != NULL)) {
        pthread_cond_wait(&app.taken, &app.lock);
    }
    pthread_mutex_unlock(&app.lock);
}

static void on_window_created(ANativeActivity *activity, ANativeWindow *window)
{
    (void)activity;
    give(window, NULL, true);
}

static void on_window_destroyed(ANativeActivity *activity, ANativeWindow *window)
{
    (void)activity, (void)window;
    give(NULL, NULL, true);
}

static void on_queue_created(ANativeActivity *activity, AInputQueue *queue)
{
    (void)activity;
    give(NULL, queue, false);
}

static void on_queue_destroyed(ANativeActivity *activity, AInputQueue *queue)
{
    (void)activity, (void)queue;
    give(NULL, NULL, false);
}

static void on_destroy(ANativeActivity *activity)
{
    (void)activity;
    pthread_mutex_lock(&app.lock);
    app.destroyed = true;
    app.activity = NULL;
    wake();
    pthread_mutex_unlock(&app.lock);
}

static void on_configuration_changed(ANativeActivity *activity)
{
    (void)activity;
    wake(); // The program looks at the window's size every poll anyway
}

static JavaVM *java_vm; // The app's, for code that calls Java (rooms' TLS: platform/src/rtc/tls.c)

void *tide_android_vm(void)
{
    return java_vm;
}

// What the app was started with, for main: the intent's "tide.args", words
// apart (as `tide run --android` gives --host, --join or --connect), into
// `argv` after the program's name. Returns how many arguments that makes.
static int intent_args(char *text, const size_t size, char **argv, const int max)
{
    static char name[] = "tide";
    argv[0] = name;
    int argc = 1;
    JavaVM *vm = app.activity->vm;
    JNIEnv *env = NULL;
    if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK) return argc;
    jobject activity = app.activity->clazz;
    jclass activity_class = (*env)->GetObjectClass(env, activity);
    jmethodID get_intent = (*env)->GetMethodID(env, activity_class, "getIntent", "()Landroid/content/Intent;");
    jobject intent = get_intent ? (*env)->CallObjectMethod(env, activity, get_intent) : NULL;
    if (intent) {
        jclass intent_class = (*env)->GetObjectClass(env, intent);
        jmethodID get_extra =
            (*env)->GetMethodID(env, intent_class, "getStringExtra", "(Ljava/lang/String;)Ljava/lang/String;");
        jstring key = (*env)->NewStringUTF(env, "tide.args");
        jstring value = get_extra ? (jstring)(*env)->CallObjectMethod(env, intent, get_extra, key) : NULL;
        if (value) {
            const char *chars = (*env)->GetStringUTFChars(env, value, NULL);
            snprintf(text, size, "%s", chars ? chars : "");
            if (chars) (*env)->ReleaseStringUTFChars(env, value, chars);
            char *rest = NULL;
            for (char *word = strtok_r(text, " ", &rest); word && argc < max - 1; word = strtok_r(NULL, " ", &rest)) {
                argv[argc++] = word;
            }
        }
    }
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*vm)->DetachCurrentThread(vm);
    argv[argc] = NULL;
    return argc;
}

static void *run_program(void *unused)
{
    (void)unused;
    app.looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS); // Its descriptors are polled, not called back
    ALooper_addFd(app.looper, app.wake[0], LOOPER_WAKE, ALOOPER_EVENT_INPUT, NULL, NULL);
    static char text[1024];
    char *argv[16];
    const int argc = intent_args(text, sizeof text, argv, 16);
    const int code = main(argc, argv);
    // main returned without CloseWindow (see ClosePlatform): the app ends
    pthread_mutex_lock(&app.lock);
    app.running = false;
    if (app.activity) ANativeActivity_finish(app.activity);
    pthread_cond_broadcast(&app.taken);
    pthread_mutex_unlock(&app.lock);
    exit(code);
}

__attribute__((visibility("default"))) void ANativeActivity_onCreate(ANativeActivity *activity, void *saved, size_t size)
{
    (void)saved, (void)size;
    activity->callbacks->onDestroy = on_destroy;
    activity->callbacks->onNativeWindowCreated = on_window_created;
    activity->callbacks->onNativeWindowDestroyed = on_window_destroyed;
    activity->callbacks->onInputQueueCreated = on_queue_created;
    activity->callbacks->onInputQueueDestroyed = on_queue_destroyed;
    activity->callbacks->onConfigurationChanged = on_configuration_changed;

    pthread_mutex_lock(&app.lock);
    app.activity = activity;
    java_vm = activity->vm;
    const bool started = app.running;
    pthread_mutex_unlock(&app.lock);
    if (started) return; // The process outlived an activity: the program goes on in this one

    log_output();
    if (pipe(app.wake) != 0) abort();
    fcntl(app.wake[0], F_SETFL, O_NONBLOCK); // The program reads what's there, and goes on
    // How dense the display is, for logical pixels: Android's medium density is 160 dpi.
    AConfiguration *config = AConfiguration_new();
    AConfiguration_fromAssetManager(config, activity->assetManager);
    const int32_t dpi = AConfiguration_getDensity(config);
    AConfiguration_delete(config);
    app.scale = dpi > 0 && dpi != ACONFIGURATION_DENSITY_NONE && dpi != ACONFIGURATION_DENSITY_ANY ? (float)dpi / 160.0f : 1.0f;

    app.running = true;
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, 8 << 20); // As a desktop program's main thread
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    if (pthread_create(&thread, &attributes, run_program, NULL) != 0) abort();
    pthread_attr_destroy(&attributes);
}

// ---------------------------------------------------------------------------
// The program's thread: the window and EGL

// Takes the window's sizes: the screen in logical pixels, rendering in its
// own. Returns whether they changed.
static bool FitWindow(void)
{
    if (!app.window) return false;
    const int width = ANativeWindow_getWidth(app.window), height = ANativeWindow_getHeight(app.window);
    if (width <= 0 || height <= 0) return false;
    const Size render = {(unsigned)width, (unsigned)height};
    const Size screen = {(unsigned)((float)width / app.scale + 0.5f), (unsigned)((float)height / app.scale + 0.5f)};
    if (screen.width == CORE.Window.screen.width && screen.height == CORE.Window.screen.height &&
        render.width == CORE.Window.render.width && render.height == CORE.Window.render.height)
        return false;
    CORE.Window.screen = CORE.Window.display = screen;
    CORE.Window.render = CORE.Window.currentFbo = render;
    const Vector2 scale = GetWindowScaleDPI();
    CORE.Window.screenScale = MatrixScale(scale.x, scale.y, 1.0f);
    return true;
}

static void drop_surface(void)
{
    if (app.surface != EGL_NO_SURFACE) {
        eglMakeCurrent(app.display, EGL_NO_SURFACE, EGL_NO_SURFACE, app.context); // Keeps the context and what's in it
        eglDestroySurface(app.display, app.surface);
        app.surface = EGL_NO_SURFACE;
    }
    FLAG_SET(CORE.Window.flags, FLAG_WINDOW_MINIMIZED);
}

static void make_surface(void)
{
    if (!app.window || app.display == EGL_NO_DISPLAY || app.surface != EGL_NO_SURFACE) return;
    EGLint format = 0;
    eglGetConfigAttrib(app.display, app.config, EGL_NATIVE_VISUAL_ID, &format);
    ANativeWindow_setBuffersGeometry(app.window, 0, 0, format);
    app.surface = eglCreateWindowSurface(app.display, app.config, app.window, NULL);
    if (app.surface == EGL_NO_SURFACE || !eglMakeCurrent(app.display, app.surface, app.surface, app.context)) {
        TRACELOG(LOG_WARNING, "PLATFORM: no surface to draw in (EGL error 0x%x)", eglGetError());
        drop_surface();
        return;
    }
    eglSwapInterval(app.display, 1); // Frames at the display's pace
    FLAG_CLEAR(CORE.Window.flags, FLAG_WINDOW_MINIMIZED);
    if (FitWindow() && CORE.Window.ready) SetupViewport((int)CORE.Window.render.width, (int)CORE.Window.render.height);
}

// Takes what the activity changed: a window and input queue, or none, and
// whether it's gone.
static void follow_activity(void)
{
    char bytes[64];
    while (read(app.wake[0], bytes, sizeof bytes) > 0) { }
    pthread_mutex_lock(&app.lock);
    if (app.queue_given != app.queue) {
        if (app.queue) AInputQueue_detachLooper(app.queue);
        app.queue = app.queue_given;
        if (app.queue) AInputQueue_attachLooper(app.queue, app.looper, LOOPER_INPUT, NULL, NULL);
    }
    if (app.window_given != app.window) {
        drop_surface();
        app.window = app.window_given;
        make_surface();
    }
    app.window_taken = app.window;
    app.queue_taken = app.queue;
    pthread_cond_broadcast(&app.taken);
    pthread_mutex_unlock(&app.lock);
}

static void handle_input(void);

// Handles what woke the looper, waiting up to `ms` for something (0: only
// what's there, -1: until something comes).
static void poll_looper(const int ms)
{
    int timeout = ms;
    for (;;) {
        const int ident = ALooper_pollOnce(timeout, NULL, NULL, NULL);
        if (ident == LOOPER_WAKE) follow_activity();
        else if (ident == LOOPER_INPUT) handle_input();
        else return; // Timed out, or woken for nothing
        timeout = 0;
    }
}

void tide_android_wait(const double seconds)
{
    poll_looper(seconds > 0.0 ? (int)(seconds * 1000.0) + 1 : 0);
}

// ---------------------------------------------------------------------------
// Input

// Android's key codes (positions) for raylib's keys; the back button is Escape,
// as in Unity.
static const struct {
    int32_t android;
    int raylib;
} keys[] = {
    {AKEYCODE_A, KEY_A}, {AKEYCODE_B, KEY_B}, {AKEYCODE_C, KEY_C}, {AKEYCODE_D, KEY_D}, {AKEYCODE_E, KEY_E},
    {AKEYCODE_F, KEY_F}, {AKEYCODE_G, KEY_G}, {AKEYCODE_H, KEY_H}, {AKEYCODE_I, KEY_I}, {AKEYCODE_J, KEY_J},
    {AKEYCODE_K, KEY_K}, {AKEYCODE_L, KEY_L}, {AKEYCODE_M, KEY_M}, {AKEYCODE_N, KEY_N}, {AKEYCODE_O, KEY_O},
    {AKEYCODE_P, KEY_P}, {AKEYCODE_Q, KEY_Q}, {AKEYCODE_R, KEY_R}, {AKEYCODE_S, KEY_S}, {AKEYCODE_T, KEY_T},
    {AKEYCODE_U, KEY_U}, {AKEYCODE_V, KEY_V}, {AKEYCODE_W, KEY_W}, {AKEYCODE_X, KEY_X}, {AKEYCODE_Y, KEY_Y},
    {AKEYCODE_Z, KEY_Z},
    {AKEYCODE_0, KEY_ZERO}, {AKEYCODE_1, KEY_ONE}, {AKEYCODE_2, KEY_TWO}, {AKEYCODE_3, KEY_THREE},
    {AKEYCODE_4, KEY_FOUR}, {AKEYCODE_5, KEY_FIVE}, {AKEYCODE_6, KEY_SIX}, {AKEYCODE_7, KEY_SEVEN},
    {AKEYCODE_8, KEY_EIGHT}, {AKEYCODE_9, KEY_NINE},
    {AKEYCODE_SPACE, KEY_SPACE}, {AKEYCODE_ENTER, KEY_ENTER}, {AKEYCODE_ESCAPE, KEY_ESCAPE},
    {AKEYCODE_BACK, KEY_ESCAPE}, {AKEYCODE_TAB, KEY_TAB}, {AKEYCODE_DEL, KEY_BACKSPACE},
    {AKEYCODE_INSERT, KEY_INSERT}, {AKEYCODE_FORWARD_DEL, KEY_DELETE}, {AKEYCODE_MOVE_HOME, KEY_HOME},
    {AKEYCODE_MOVE_END, KEY_END}, {AKEYCODE_PAGE_UP, KEY_PAGE_UP}, {AKEYCODE_PAGE_DOWN, KEY_PAGE_DOWN},
    {AKEYCODE_DPAD_UP, KEY_UP}, {AKEYCODE_DPAD_DOWN, KEY_DOWN}, {AKEYCODE_DPAD_LEFT, KEY_LEFT},
    {AKEYCODE_DPAD_RIGHT, KEY_RIGHT},
    {AKEYCODE_SHIFT_LEFT, KEY_LEFT_SHIFT}, {AKEYCODE_SHIFT_RIGHT, KEY_RIGHT_SHIFT},
    {AKEYCODE_CTRL_LEFT, KEY_LEFT_CONTROL}, {AKEYCODE_CTRL_RIGHT, KEY_RIGHT_CONTROL},
    {AKEYCODE_ALT_LEFT, KEY_LEFT_ALT}, {AKEYCODE_ALT_RIGHT, KEY_RIGHT_ALT}, {AKEYCODE_CAPS_LOCK, KEY_CAPS_LOCK},
    {AKEYCODE_F1, KEY_F1}, {AKEYCODE_F2, KEY_F2}, {AKEYCODE_F3, KEY_F3}, {AKEYCODE_F4, KEY_F4},
    {AKEYCODE_F5, KEY_F5}, {AKEYCODE_F6, KEY_F6}, {AKEYCODE_F7, KEY_F7}, {AKEYCODE_F8, KEY_F8},
    {AKEYCODE_F9, KEY_F9}, {AKEYCODE_F10, KEY_F10}, {AKEYCODE_F11, KEY_F11}, {AKEYCODE_F12, KEY_F12},
    {AKEYCODE_MINUS, KEY_MINUS}, {AKEYCODE_EQUALS, KEY_EQUAL}, {AKEYCODE_LEFT_BRACKET, KEY_LEFT_BRACKET},
    {AKEYCODE_RIGHT_BRACKET, KEY_RIGHT_BRACKET}, {AKEYCODE_BACKSLASH, KEY_BACKSLASH},
    {AKEYCODE_SEMICOLON, KEY_SEMICOLON}, {AKEYCODE_APOSTROPHE, KEY_APOSTROPHE}, {AKEYCODE_COMMA, KEY_COMMA},
    {AKEYCODE_PERIOD, KEY_PERIOD}, {AKEYCODE_SLASH, KEY_SLASH}, {AKEYCODE_GRAVE, KEY_GRAVE},
    {AKEYCODE_NUMPAD_0, KEY_KP_0}, {AKEYCODE_NUMPAD_1, KEY_KP_1}, {AKEYCODE_NUMPAD_2, KEY_KP_2},
    {AKEYCODE_NUMPAD_3, KEY_KP_3}, {AKEYCODE_NUMPAD_4, KEY_KP_4}, {AKEYCODE_NUMPAD_5, KEY_KP_5},
    {AKEYCODE_NUMPAD_6, KEY_KP_6}, {AKEYCODE_NUMPAD_7, KEY_KP_7}, {AKEYCODE_NUMPAD_8, KEY_KP_8},
    {AKEYCODE_NUMPAD_9, KEY_KP_9}, {AKEYCODE_NUMPAD_ENTER, KEY_KP_ENTER}, {AKEYCODE_NUMPAD_ADD, KEY_KP_ADD},
    {AKEYCODE_NUMPAD_SUBTRACT, KEY_KP_SUBTRACT}, {AKEYCODE_NUMPAD_MULTIPLY, KEY_KP_MULTIPLY},
    {AKEYCODE_NUMPAD_DIVIDE, KEY_KP_DIVIDE}, {AKEYCODE_NUMPAD_DOT, KEY_KP_DECIMAL},
};

// True if it's a key of ours; others (volume, power) are the system's.
static bool handle_key(const AInputEvent *event)
{
    const int32_t code = AKeyEvent_getKeyCode(event);
    const int32_t action = AKeyEvent_getAction(event);
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        if (keys[i].android != code) continue;
        const int key = keys[i].raylib;
        if (action == AKEY_EVENT_ACTION_DOWN) {
            app.keys_held[key] = true;
            app.keys_tapped[key] = true;
        } else if (action == AKEY_EVENT_ACTION_UP) {
            app.keys_held[key] = false;
        }
        return true;
    }
    return false;
}

static void report_touch(const AInputEvent *event, const size_t index, const tide_touch_phase phase)
{
    if (app.touch_count == sizeof app.touches / sizeof app.touches[0]) return; // Dropped, as a finger past the slots
    const float width = app.window ? (float)ANativeWindow_getWidth(app.window) : 1.0f;
    const float height = app.window ? (float)ANativeWindow_getHeight(app.window) : 1.0f;
    const tide_touch_report r = {phase, (uint32_t)AMotionEvent_getPointerId(event, index),
                                 AMotionEvent_getX(event, index) / (width > 0.0f ? width : 1.0f),
                                 AMotionEvent_getY(event, index) / (height > 0.0f ? height : 1.0f)};
    app.touches[(app.touch_start + app.touch_count++) % (sizeof app.touches / sizeof app.touches[0])] = r;
}

static bool is_mouse(const AInputEvent *event, const size_t index)
{
    return AMotionEvent_getToolType(event, index) == AMOTION_EVENT_TOOL_TYPE_MOUSE;
}

static bool handle_motion(const AInputEvent *event)
{
    const int32_t action = AMotionEvent_getAction(event);
    const int32_t kind = action & AMOTION_EVENT_ACTION_MASK;
    const size_t index = (size_t)((action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
    const size_t count = AMotionEvent_getPointerCount(event);
    if (count == 0) return false;

    if (is_mouse(event, 0)) { // A mouse, on a Chromebook or a phone it's plugged into
        app.mouse_at = (Vector2){AMotionEvent_getX(event, 0) / app.scale, AMotionEvent_getY(event, 0) / app.scale};
        if (kind == AMOTION_EVENT_ACTION_SCROLL) {
            app.wheel.x += AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HSCROLL, 0);
            app.wheel.y += AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_VSCROLL, 0);
        }
        const int32_t buttons = AMotionEvent_getButtonState(event);
        const int held = ((buttons & AMOTION_EVENT_BUTTON_PRIMARY) ? 1 : 0) | ((buttons & AMOTION_EVENT_BUTTON_SECONDARY) ? 2 : 0)
                       | ((buttons & AMOTION_EVENT_BUTTON_TERTIARY) ? 4 : 0) | ((buttons & AMOTION_EVENT_BUTTON_BACK) ? 8 : 0)
                       | ((buttons & AMOTION_EVENT_BUTTON_FORWARD) ? 16 : 0);
        app.mouse_tapped |= held;
        app.mouse_held = held;
        return true;
    }

    switch (kind) {
    case AMOTION_EVENT_ACTION_DOWN:
    case AMOTION_EVENT_ACTION_POINTER_DOWN:
        report_touch(event, kind == AMOTION_EVENT_ACTION_DOWN ? 0 : index, TIDE_TOUCH_BEGAN);
        break;
    case AMOTION_EVENT_ACTION_MOVE:
        for (size_t i = 0; i < count; i++) report_touch(event, i, TIDE_TOUCH_MOVED);
        break;
    case AMOTION_EVENT_ACTION_UP:
    case AMOTION_EVENT_ACTION_POINTER_UP:
        report_touch(event, kind == AMOTION_EVENT_ACTION_UP ? 0 : index, TIDE_TOUCH_ENDED);
        break;
    case AMOTION_EVENT_ACTION_CANCEL:
        for (size_t i = 0; i < count; i++) report_touch(event, i, TIDE_TOUCH_CANCELED);
        break;
    default:
        return false;
    }
    return true;
}

static void handle_input(void)
{
    AInputEvent *event = NULL;
    while (app.queue && AInputQueue_getEvent(app.queue, &event) >= 0) {
        if (AInputQueue_preDispatchEvent(app.queue, event)) continue; // The system's input method took it
        bool handled = false;
        if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY) handled = handle_key(event);
        else if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_MOTION) handled = handle_motion(event);
        AInputQueue_finishEvent(app.queue, event, handled);
    }
}

bool tide_native_touchscreen(void)
{
    return true; // Android's devices have one, or emulate one
}

bool tide_native_take_touch(tide_touch_report *r)
{
    if (app.touch_count == 0) return false;
    *r = app.touches[app.touch_start];
    app.touch_start = (app.touch_start + 1) % (sizeof app.touches / sizeof app.touches[0]);
    app.touch_count--;
    return true;
}

void PollInputEvents(void)
{
    CORE.Input.Keyboard.keyPressedQueueCount = 0;
    CORE.Input.Keyboard.charPressedQueueCount = 0;
    for (int i = 0; i < MAX_KEYBOARD_KEYS; i++) {
        CORE.Input.Keyboard.previousKeyState[i] = CORE.Input.Keyboard.currentKeyState[i];
        CORE.Input.Keyboard.keyRepeatInFrame[i] = 0;
    }
    for (int i = 0; i < MAX_MOUSE_BUTTONS; i++) CORE.Input.Mouse.previousButtonState[i] = CORE.Input.Mouse.currentButtonState[i];
    CORE.Input.Mouse.previousWheelMove = CORE.Input.Mouse.currentWheelMove;
    CORE.Input.Mouse.previousPosition = CORE.Input.Mouse.currentPosition;

    // Keys and buttons pressed since the last poll read as held for this one,
    // even if they're up again.
    memset(app.keys_tapped, 0, sizeof app.keys_tapped);
    app.mouse_tapped = 0;
    app.wheel = (Vector2){0.0f, 0.0f};

    poll_looper(0);

    for (int i = 0; i < MAX_KEYBOARD_KEYS; i++) CORE.Input.Keyboard.currentKeyState[i] = app.keys_held[i] || app.keys_tapped[i];
    const int buttons = app.mouse_held | app.mouse_tapped;
    for (int i = 0; i < MAX_MOUSE_BUTTONS && i < 5; i++) CORE.Input.Mouse.currentButtonState[i] = (buttons >> i) & 1;
    CORE.Input.Mouse.currentWheelMove = app.wheel;
    CORE.Input.Mouse.currentPosition = app.mouse_at;

    // The window follows the screen's turns and the system's bars.
    CORE.Window.resizedLastFrame = FitWindow();
    if (CORE.Window.resizedLastFrame) SetupViewport((int)CORE.Window.render.width, (int)CORE.Window.render.height);
}

// ---------------------------------------------------------------------------
// Window: the activity's, which the system moves and sizes

bool WindowShouldClose(void)
{
    pthread_mutex_lock(&app.lock);
    const bool destroyed = app.destroyed;
    pthread_mutex_unlock(&app.lock);
    return destroyed;
}
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
void *GetWindowHandle(void) { return app.window; }
int GetMonitorCount(void) { return 1; }
int GetCurrentMonitor(void) { return 0; }
Vector2 GetMonitorPosition(int monitor) { (void)monitor; return (Vector2){0, 0}; }
int GetMonitorWidth(int monitor) { (void)monitor; return (int)CORE.Window.display.width; }
int GetMonitorHeight(int monitor) { (void)monitor; return (int)CORE.Window.display.height; }
int GetMonitorPhysicalWidth(int monitor) { (void)monitor; return 0; }
int GetMonitorPhysicalHeight(int monitor) { (void)monitor; return 0; }
int GetMonitorRefreshRate(int monitor) { (void)monitor; return 60; }
const char *GetMonitorName(int monitor) { (void)monitor; return "display"; }
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

void SwapScreenBuffer(void)
{
    if (app.surface != EGL_NO_SURFACE && !eglSwapBuffers(app.display, app.surface)) {
        const EGLint error = eglGetError();
        if (error == EGL_BAD_SURFACE || error == EGL_BAD_NATIVE_WINDOW) drop_surface(); // Gone under us
    }
}

// ---------------------------------------------------------------------------
// Misc

double GetTime(void)
{
    struct timespec ts = {0};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const unsigned long long ns = (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
    return (double)(ns - CORE.Time.base) * 1e-9;
}

void OpenURL(const char *url) { (void)url; }

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

// ---------------------------------------------------------------------------

int InitPlatform(void)
{
    app.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (app.display == EGL_NO_DISPLAY || !eglInitialize(app.display, NULL, NULL)) {
        TRACELOG(LOG_FATAL, "PLATFORM: no EGL display");
        return -1;
    }
    const EGLint wanted[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RED_SIZE, 8,
                             EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_NONE};
    EGLint found = 0;
    if (!eglChooseConfig(app.display, wanted, &app.config, 1, &found) || found < 1) {
        TRACELOG(LOG_FATAL, "PLATFORM: this device has no OpenGL ES 3");
        return -1;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint version[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    app.context = eglCreateContext(app.display, app.config, EGL_NO_CONTEXT, version);
    if (app.context == EGL_NO_CONTEXT) {
        TRACELOG(LOG_FATAL, "PLATFORM: no OpenGL ES 3 context (EGL error 0x%x)", eglGetError());
        return -1;
    }
    app.surface = EGL_NO_SURFACE;

    // The first window: rlgl needs a context that's current to start.
    FLAG_SET(CORE.Window.flags, FLAG_WINDOW_MINIMIZED);
    while (!WindowShouldClose()) {
        poll_looper(-1);
        if (app.surface != EGL_NO_SURFACE) break;
    }
    if (app.surface == EGL_NO_SURFACE) return -1; // The activity went before it had a window

    rlLoadExtensions((void *)eglGetProcAddress);
    FitWindow();
    CORE.Window.ready = true;
    InitTimer();
    CORE.Storage.basePath = GetWorkingDirectory();
    return 0;
}

void ClosePlatform(void)
{
    drop_surface();
    if (app.context != EGL_NO_CONTEXT) eglDestroyContext(app.display, app.context);
    if (app.display != EGL_NO_DISPLAY) eglTerminate(app.display);
    // The program ends with the activity: it says so before the process goes.
    pthread_mutex_lock(&app.lock);
    app.running = false;
    if (app.activity) ANativeActivity_finish(app.activity);
    pthread_cond_broadcast(&app.taken);
    pthread_mutex_unlock(&app.lock);
}
