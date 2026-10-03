// Android's window (see src/window.h): Android's NativeActivity, with OpenGL
// ES 3 through EGL, and no Java of our own.
//
// Android starts the app by loading its library and calling
// ANativeActivity_onCreate on its main thread, which mustn't block. The program
// runs main() on a thread of its own, as on desktop. The activity's callbacks
// say what changed (a window to draw in, the input queue, the activity going)
// under `app.lock`, and wake the program's looper; one that takes something
// away (the window, the input queue) waits until the program let it go.
//
// Keys are read by physical position (Android's key codes are positions), and
// fingers are the touchscreen's, never the mouse's. A key, mouse button or
// gamepad button pressed and released between two frames reads as held for
// one, as on the web.
//
// The screen is the window in the display's logical pixels, as a browser's CSS
// pixels are: its own pixels over (dpi / 160). It renders the window's own
// pixels, so it's sharp.

#include "window.h"

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
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define COUNT_OF(array) (sizeof(array) / sizeof((array)[0]))

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
    float scale;                    // The window's pixels per logical pixel
    int width, height;              // The window's size in logical pixels, as of when it last had one...
    int pixel_width, pixel_height;  // ...and in its own

    // Input: what's held, and what was pressed since it was last asked about.
    bool keys_held[TIDE_KEY_COUNT], keys_tapped[TIDE_KEY_COUNT];
    uint32_t mouse_held, mouse_tapped; // Bits in tide_mouse's order: left, right, middle, back, forward
    uint32_t pad_held, pad_tapped;     // The first gamepad's buttons: bits, 1 << tide_pad_button
    float pad_sticks[4];               // ...its sticks: the left one's x and y, then the right one's
    float pad_triggers[2];             // ...and its triggers, 0 to 1
    bool pad_seen;                     // A gamepad sent something
    float mouse_x, mouse_y, wheel_x, wheel_y;
    bool paste_asked; // Ctrl+V went down
    tide_touch_report touches[256]; // Where, from 0 to 1 across the window
    unsigned touch_start, touch_count;
    uint32_t typed[256]; // Characters typed and not yet taken: more than fit are dropped
    unsigned typed_start, typed_count;
} app = {.lock = PTHREAD_MUTEX_INITIALIZER, .taken = PTHREAD_COND_INITIALIZER, .wake = {-1, -1},
         .width = 1, .height = 1, .pixel_width = 1, .pixel_height = 1};

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
    wake(); // The program looks at the window's size every frame anyway
}

static JavaVM *java_vm; // The app's, for code that calls Java (rooms' TLS: platform/src/rtc/tls.c)
static int ui_pipe[2] = {-1, -1}; // The program's asks of the main thread: the keyboard, and copying
static int on_ui_asked(int fd, int events, void *data);

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
    // main returned without closing a window (see tide_window_close): the app ends
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
    // What the program asks of the main thread, which only it may do: the keyboard
    if (pipe(ui_pipe) == 0) {
        fcntl(ui_pipe[0], F_SETFL, O_NONBLOCK);
        ALooper_addFd(ALooper_forThread(), ui_pipe[0], ALOOPER_POLL_CALLBACK, ALOOPER_EVENT_INPUT, on_ui_asked, NULL);
    }
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
// own. It follows the screen's turns and the system's bars.
static void fit_window(void)
{
    if (!app.window) return;
    const int width = ANativeWindow_getWidth(app.window), height = ANativeWindow_getHeight(app.window);
    if (width <= 0 || height <= 0) return;
    app.pixel_width = width;
    app.pixel_height = height;
    app.width = (int)((float)width / app.scale + 0.5f);
    app.height = (int)((float)height / app.scale + 0.5f);
    if (app.width < 1) app.width = 1;
    if (app.height < 1) app.height = 1;
}

static void drop_surface(void)
{
    if (app.surface != EGL_NO_SURFACE) {
        eglMakeCurrent(app.display, EGL_NO_SURFACE, EGL_NO_SURFACE, app.context); // Keeps the context and what's in it
        eglDestroySurface(app.display, app.surface);
        app.surface = EGL_NO_SURFACE;
    }
}

static void make_surface(void)
{
    if (!app.window || app.display == EGL_NO_DISPLAY || app.surface != EGL_NO_SURFACE) return;
    EGLint format = 0;
    eglGetConfigAttrib(app.display, app.config, EGL_NATIVE_VISUAL_ID, &format);
    ANativeWindow_setBuffersGeometry(app.window, 0, 0, format);
    app.surface = eglCreateWindowSurface(app.display, app.config, app.window, NULL);
    if (app.surface == EGL_NO_SURFACE || !eglMakeCurrent(app.display, app.surface, app.surface, app.context)) {
        fprintf(stderr, "tide: no surface to draw in (EGL error 0x%x)\n", (unsigned)eglGetError());
        drop_surface();
        return;
    }
    eglSwapInterval(app.display, 1); // Frames at the display's pace
    fit_window();
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

// ---------------------------------------------------------------------------
// Input

// Android's key codes (positions) for Tide's keys; the back button is Escape,
// as in Unity.
static const struct {
    int32_t android;
    tide_key key;
} keys[] = {
    {AKEYCODE_A, TIDE_KEY_a}, {AKEYCODE_B, TIDE_KEY_b}, {AKEYCODE_C, TIDE_KEY_c}, {AKEYCODE_D, TIDE_KEY_d},
    {AKEYCODE_E, TIDE_KEY_e}, {AKEYCODE_F, TIDE_KEY_f}, {AKEYCODE_G, TIDE_KEY_g}, {AKEYCODE_H, TIDE_KEY_h},
    {AKEYCODE_I, TIDE_KEY_i}, {AKEYCODE_J, TIDE_KEY_j}, {AKEYCODE_K, TIDE_KEY_k}, {AKEYCODE_L, TIDE_KEY_l},
    {AKEYCODE_M, TIDE_KEY_m}, {AKEYCODE_N, TIDE_KEY_n}, {AKEYCODE_O, TIDE_KEY_o}, {AKEYCODE_P, TIDE_KEY_p},
    {AKEYCODE_Q, TIDE_KEY_q}, {AKEYCODE_R, TIDE_KEY_r}, {AKEYCODE_S, TIDE_KEY_s}, {AKEYCODE_T, TIDE_KEY_t},
    {AKEYCODE_U, TIDE_KEY_u}, {AKEYCODE_V, TIDE_KEY_v}, {AKEYCODE_W, TIDE_KEY_w}, {AKEYCODE_X, TIDE_KEY_x},
    {AKEYCODE_Y, TIDE_KEY_y}, {AKEYCODE_Z, TIDE_KEY_z},
    {AKEYCODE_0, TIDE_KEY_digit0}, {AKEYCODE_1, TIDE_KEY_digit1}, {AKEYCODE_2, TIDE_KEY_digit2},
    {AKEYCODE_3, TIDE_KEY_digit3}, {AKEYCODE_4, TIDE_KEY_digit4}, {AKEYCODE_5, TIDE_KEY_digit5},
    {AKEYCODE_6, TIDE_KEY_digit6}, {AKEYCODE_7, TIDE_KEY_digit7}, {AKEYCODE_8, TIDE_KEY_digit8},
    {AKEYCODE_9, TIDE_KEY_digit9},
    {AKEYCODE_SPACE, TIDE_KEY_space}, {AKEYCODE_ENTER, TIDE_KEY_enter}, {AKEYCODE_ESCAPE, TIDE_KEY_escape},
    {AKEYCODE_BACK, TIDE_KEY_escape}, {AKEYCODE_TAB, TIDE_KEY_tab}, {AKEYCODE_DEL, TIDE_KEY_backspace},
    {AKEYCODE_INSERT, TIDE_KEY_insert}, {AKEYCODE_FORWARD_DEL, TIDE_KEY_delete}, {AKEYCODE_MOVE_HOME, TIDE_KEY_home},
    {AKEYCODE_MOVE_END, TIDE_KEY_end}, {AKEYCODE_PAGE_UP, TIDE_KEY_pageUp}, {AKEYCODE_PAGE_DOWN, TIDE_KEY_pageDown},
    {AKEYCODE_DPAD_UP, TIDE_KEY_upArrow}, {AKEYCODE_DPAD_DOWN, TIDE_KEY_downArrow},
    {AKEYCODE_DPAD_LEFT, TIDE_KEY_leftArrow}, {AKEYCODE_DPAD_RIGHT, TIDE_KEY_rightArrow},
    {AKEYCODE_SHIFT_LEFT, TIDE_KEY_leftShift}, {AKEYCODE_SHIFT_RIGHT, TIDE_KEY_rightShift},
    {AKEYCODE_CTRL_LEFT, TIDE_KEY_leftCtrl}, {AKEYCODE_CTRL_RIGHT, TIDE_KEY_rightCtrl},
    {AKEYCODE_ALT_LEFT, TIDE_KEY_leftAlt}, {AKEYCODE_ALT_RIGHT, TIDE_KEY_rightAlt},
    {AKEYCODE_CAPS_LOCK, TIDE_KEY_capsLock},
    {AKEYCODE_F1, TIDE_KEY_f1}, {AKEYCODE_F2, TIDE_KEY_f2}, {AKEYCODE_F3, TIDE_KEY_f3}, {AKEYCODE_F4, TIDE_KEY_f4},
    {AKEYCODE_F5, TIDE_KEY_f5}, {AKEYCODE_F6, TIDE_KEY_f6}, {AKEYCODE_F7, TIDE_KEY_f7}, {AKEYCODE_F8, TIDE_KEY_f8},
    {AKEYCODE_F9, TIDE_KEY_f9}, {AKEYCODE_F10, TIDE_KEY_f10}, {AKEYCODE_F11, TIDE_KEY_f11},
    {AKEYCODE_F12, TIDE_KEY_f12},
    {AKEYCODE_MINUS, TIDE_KEY_minus}, {AKEYCODE_EQUALS, TIDE_KEY_equals},
    {AKEYCODE_LEFT_BRACKET, TIDE_KEY_leftBracket}, {AKEYCODE_RIGHT_BRACKET, TIDE_KEY_rightBracket},
    {AKEYCODE_BACKSLASH, TIDE_KEY_backslash}, {AKEYCODE_SEMICOLON, TIDE_KEY_semicolon},
    {AKEYCODE_APOSTROPHE, TIDE_KEY_quote}, {AKEYCODE_COMMA, TIDE_KEY_comma}, {AKEYCODE_PERIOD, TIDE_KEY_period},
    {AKEYCODE_SLASH, TIDE_KEY_slash}, {AKEYCODE_GRAVE, TIDE_KEY_backquote},
    {AKEYCODE_NUMPAD_0, TIDE_KEY_numpad0}, {AKEYCODE_NUMPAD_1, TIDE_KEY_numpad1}, {AKEYCODE_NUMPAD_2, TIDE_KEY_numpad2},
    {AKEYCODE_NUMPAD_3, TIDE_KEY_numpad3}, {AKEYCODE_NUMPAD_4, TIDE_KEY_numpad4}, {AKEYCODE_NUMPAD_5, TIDE_KEY_numpad5},
    {AKEYCODE_NUMPAD_6, TIDE_KEY_numpad6}, {AKEYCODE_NUMPAD_7, TIDE_KEY_numpad7}, {AKEYCODE_NUMPAD_8, TIDE_KEY_numpad8},
    {AKEYCODE_NUMPAD_9, TIDE_KEY_numpad9},
    {AKEYCODE_NUMPAD_ENTER, TIDE_KEY_numpadEnter}, {AKEYCODE_NUMPAD_ADD, TIDE_KEY_numpadPlus},
    {AKEYCODE_NUMPAD_SUBTRACT, TIDE_KEY_numpadMinus}, {AKEYCODE_NUMPAD_MULTIPLY, TIDE_KEY_numpadMultiply},
    {AKEYCODE_NUMPAD_DIVIDE, TIDE_KEY_numpadDivide}, {AKEYCODE_NUMPAD_DOT, TIDE_KEY_numpadPeriod},
};

_Static_assert(COUNT_OF(keys) == TIDE_KEY_COUNT + 1, "every key in devices.h needs Android's code, and the back button");

// The character a key types, with what's held (Shift and the like) and the
// keyboard's layout: Java's, as the NDK has no way to it. 0 for none.
static int typed_char(const AInputEvent *event)
{
    static jclass key_class;
    static jmethodID make, unicode;
    JNIEnv *env = NULL;
    if (!java_vm || (*java_vm)->AttachCurrentThread(java_vm, &env, NULL) != JNI_OK) return 0;
    if (!key_class) {
        jclass local = (*env)->FindClass(env, "android/view/KeyEvent");
        if (!local) {
            (*env)->ExceptionClear(env);
            return 0;
        }
        key_class = (*env)->NewGlobalRef(env, local);
        (*env)->DeleteLocalRef(env, local);
        make = (*env)->GetMethodID(env, key_class, "<init>", "(JJIIII)V");
        unicode = (*env)->GetMethodID(env, key_class, "getUnicodeChar", "(I)I");
    }
    if (!make || !unicode) return 0;
    const int32_t meta = AKeyEvent_getMetaState(event);
    jobject key = (*env)->NewObject(env, key_class, make, (jlong)(AKeyEvent_getDownTime(event) / 1000000),
                                    (jlong)(AKeyEvent_getEventTime(event) / 1000000), (jint)AKeyEvent_getAction(event),
                                    (jint)AKeyEvent_getKeyCode(event), (jint)AKeyEvent_getRepeatCount(event), (jint)meta);
    const int c = key ? (*env)->CallIntMethod(env, key, unicode, (jint)meta) : 0;
    if (key) (*env)->DeleteLocalRef(env, key);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return 0;
    }
    return c;
}

// A gamepad's buttons (Android's key codes) as Tide's: by position, as
// Unity's, so A is south.
static const struct {
    int32_t android;
    tide_pad_button button;
} pad_buttons[] = {
    {AKEYCODE_BUTTON_A, TIDE_PAD_buttonSouth},          {AKEYCODE_BUTTON_B, TIDE_PAD_buttonEast},
    {AKEYCODE_BUTTON_X, TIDE_PAD_buttonWest},           {AKEYCODE_BUTTON_Y, TIDE_PAD_buttonNorth},
    {AKEYCODE_BUTTON_L1, TIDE_PAD_leftShoulder},        {AKEYCODE_BUTTON_R1, TIDE_PAD_rightShoulder},
    {AKEYCODE_BUTTON_THUMBL, TIDE_PAD_leftStickButton}, {AKEYCODE_BUTTON_THUMBR, TIDE_PAD_rightStickButton},
    {AKEYCODE_BUTTON_START, TIDE_PAD_start},            {AKEYCODE_BUTTON_SELECT, TIDE_PAD_select},
    {AKEYCODE_DPAD_UP, TIDE_PAD_dpad_up},               {AKEYCODE_DPAD_DOWN, TIDE_PAD_dpad_down},
    {AKEYCODE_DPAD_LEFT, TIDE_PAD_dpad_left},           {AKEYCODE_DPAD_RIGHT, TIDE_PAD_dpad_right},
};

_Static_assert(COUNT_OF(pad_buttons) == TIDE_PAD_COUNT, "every gamepad button needs Android's code");

static bool from_gamepad(const AInputEvent *event)
{
    const int32_t source = AInputEvent_getSource(event);
    return (source & AINPUT_SOURCE_GAMEPAD) == AINPUT_SOURCE_GAMEPAD || (source & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK;
}

static void pad_button(const tide_pad_button button, const bool down)
{
    app.pad_seen = true;
    if (down) app.pad_held |= 1u << button, app.pad_tapped |= 1u << button;
    else app.pad_held &= ~(1u << button);
}

// True if it's a key of ours, or types a character; others (volume, power)
// are the system's.
static bool handle_key(const AInputEvent *event)
{
    const int32_t code = AKeyEvent_getKeyCode(event);
    const int32_t action = AKeyEvent_getAction(event);
    if (from_gamepad(event) && action != AKEY_EVENT_ACTION_MULTIPLE) {
        for (size_t i = 0; i < COUNT_OF(pad_buttons); i++) {
            if (pad_buttons[i].android != code) continue;
            pad_button(pad_buttons[i].button, action == AKEY_EVENT_ACTION_DOWN);
            return true;
        }
    }
    bool ours = false;
    if (action == AKEY_EVENT_ACTION_DOWN) {
        const int c = typed_char(event);
        if (c >= 32 && c != 127 && app.typed_count < COUNT_OF(app.typed)) {
            app.typed[(app.typed_start + app.typed_count++) % COUNT_OF(app.typed)] = (uint32_t)c;
            ours = true;
        }
        // Ctrl+V on a keyboard pastes
        if (code == AKEYCODE_V && (AKeyEvent_getMetaState(event) & AMETA_CTRL_ON)) app.paste_asked = true;
    }
    for (size_t i = 0; i < COUNT_OF(keys); i++) {
        if (keys[i].android != code) continue;
        const tide_key key = keys[i].key;
        if (action == AKEY_EVENT_ACTION_DOWN) {
            app.keys_held[key] = true;
            app.keys_tapped[key] = true;
        } else if (action == AKEY_EVENT_ACTION_UP) {
            app.keys_held[key] = false;
        }
        return true;
    }
    return ours;
}

// The system's keyboard, which Android shows for the view with the focus,
// from its main thread: NativeActivity's own can't take it, so the window's
// does, and its keys still come to the input queue, which has them all.

static jobject call(JNIEnv *env, jobject on, const char *name, const char *signature, ...)
{
    jclass c = (*env)->GetObjectClass(env, on);
    jmethodID m = (*env)->GetMethodID(env, c, name, signature);
    (*env)->DeleteLocalRef(env, c);
    if (!m) return NULL;
    // Java checks a call is made as what the method returns: nothing, a bool, or an object
    const char returns = signature[strlen(signature) - 1];
    va_list args;
    va_start(args, signature);
    jobject result = NULL;
    if (returns == 'V') (*env)->CallVoidMethodV(env, on, m, args);
    else if (returns == 'Z') (*env)->CallBooleanMethodV(env, on, m, args);
    else result = (*env)->CallObjectMethodV(env, on, m, args);
    va_end(args);
    return result;
}

static void keyboard_on_ui(const bool show)
{
    if (!app.activity) return;
    JNIEnv *env = app.activity->env; // The main thread's
    (*env)->PushLocalFrame(env, 16);
    jobject activity = app.activity->clazz;
    jobject window = call(env, activity, "getWindow", "()Landroid/view/Window;");
    jobject view = window ? call(env, window, "getDecorView", "()Landroid/view/View;") : NULL;
    jstring service = (*env)->NewStringUTF(env, "input_method");
    jobject keyboards = call(env, activity, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;", service);
    if (view && keyboards) {
        if (show) {
            call(env, view, "setFocusable", "(Z)V", JNI_TRUE);
            call(env, view, "setFocusableInTouchMode", "(Z)V", JNI_TRUE);
            call(env, view, "requestFocus", "()Z");
            call(env, keyboards, "showSoftInput", "(Landroid/view/View;I)Z", view, 0);
        } else {
            jobject token = call(env, view, "getWindowToken", "()Landroid/os/IBinder;");
            call(env, keyboards, "hideSoftInputFromWindow", "(Landroid/os/IBinder;I)Z", token, 0);
        }
    }
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
    }
    (*env)->PopLocalFrame(env, NULL);
}

// What to put on the clipboard, which the main thread takes ('c').
static char copy_text[4096];

static void copy_on_ui(void)
{
    if (!app.activity) return;
    JNIEnv *env = app.activity->env;
    (*env)->PushLocalFrame(env, 16);
    pthread_mutex_lock(&app.lock);
    jstring text = (*env)->NewStringUTF(env, copy_text);
    pthread_mutex_unlock(&app.lock);
    jstring service = (*env)->NewStringUTF(env, "clipboard");
    jobject clipboard = call(env, app.activity->clazz, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;", service);
    jclass clip_class = (*env)->FindClass(env, "android/content/ClipData");
    jmethodID plain = clip_class ? (*env)->GetStaticMethodID(env, clip_class, "newPlainText",
                                                             "(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Landroid/content/ClipData;")
                                 : NULL;
    jstring label = (*env)->NewStringUTF(env, "tide");
    jobject clip = plain ? (*env)->CallStaticObjectMethod(env, clip_class, plain, label, text) : NULL;
    if (clipboard && clip) call(env, clipboard, "setPrimaryClip", "(Landroid/content/ClipData;)V", clip);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
    }
    (*env)->PopLocalFrame(env, NULL);
}

// The main thread's looper calls it when the program asked: '1' shows the
// keyboard and '0' hides it (the last word goes), 'c' copies.
static int on_ui_asked(int fd, int events, void *data)
{
    (void)events, (void)data;
    char asked[64];
    const ssize_t n = read(fd, asked, sizeof asked);
    char keyboard = 0;
    for (ssize_t i = 0; i < n; i++) {
        if (asked[i] == 'c') copy_on_ui();
        else keyboard = asked[i];
    }
    if (keyboard) keyboard_on_ui(keyboard == '1');
    return 1;
}

static void ask_ui(const char asked)
{
    if (ui_pipe[1] >= 0 && write(ui_pipe[1], &asked, 1) < 0) { } // The main thread is gone
}

static void report_touch(const AInputEvent *event, const size_t index, const tide_touch_phase phase)
{
    if (app.touch_count == COUNT_OF(app.touches)) return; // Dropped, as a finger past the slots
    const float width = app.window ? (float)ANativeWindow_getWidth(app.window) : 1.0f;
    const float height = app.window ? (float)ANativeWindow_getHeight(app.window) : 1.0f;
    const tide_touch_report r = {phase, (uint32_t)AMotionEvent_getPointerId(event, index),
                                 AMotionEvent_getX(event, index) / (width > 0.0f ? width : 1.0f),
                                 AMotionEvent_getY(event, index) / (height > 0.0f ? height : 1.0f)};
    app.touches[(app.touch_start + app.touch_count++) % COUNT_OF(app.touches)] = r;
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

    if (from_gamepad(event) && kind == AMOTION_EVENT_ACTION_MOVE) { // Its sticks, triggers and d-pad
        static const int32_t sticks[4] = {AMOTION_EVENT_AXIS_X, AMOTION_EVENT_AXIS_Y, AMOTION_EVENT_AXIS_Z,
                                          AMOTION_EVENT_AXIS_RZ};
        app.pad_seen = true;
        for (size_t i = 0; i < 4; i++) app.pad_sticks[i] = AMotionEvent_getAxisValue(event, sticks[i], 0);
        app.pad_triggers[0] = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_LTRIGGER, 0);
        app.pad_triggers[1] = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RTRIGGER, 0);
        // Some pads give their triggers as the brake and the gas
        const float brake = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_BRAKE, 0);
        const float gas = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_GAS, 0);
        if (brake > 0.0f) app.pad_triggers[0] = brake;
        if (gas > 0.0f) app.pad_triggers[1] = gas;
        // A d-pad that's a hat: -1 to 1 on each axis
        const float hat_x = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HAT_X, 0);
        const float hat_y = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HAT_Y, 0);
        pad_button(TIDE_PAD_dpad_left, hat_x < -0.5f);
        pad_button(TIDE_PAD_dpad_right, hat_x > 0.5f);
        pad_button(TIDE_PAD_dpad_up, hat_y < -0.5f);
        pad_button(TIDE_PAD_dpad_down, hat_y > 0.5f);
        return true;
    }

    if (is_mouse(event, 0)) { // A mouse, on a Chromebook or a phone it's plugged into
        app.mouse_x = AMotionEvent_getX(event, 0) / app.scale;
        app.mouse_y = AMotionEvent_getY(event, 0) / app.scale;
        if (kind == AMOTION_EVENT_ACTION_SCROLL) {
            app.wheel_x += AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HSCROLL, 0);
            app.wheel_y += AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_VSCROLL, 0);
        }
        const int32_t buttons = AMotionEvent_getButtonState(event);
        const uint32_t held = ((buttons & AMOTION_EVENT_BUTTON_PRIMARY) ? 1u : 0u) | ((buttons & AMOTION_EVENT_BUTTON_SECONDARY) ? 2u : 0u)
                            | ((buttons & AMOTION_EVENT_BUTTON_TERTIARY) ? 4u : 0u) | ((buttons & AMOTION_EVENT_BUTTON_BACK) ? 8u : 0u)
                            | ((buttons & AMOTION_EVENT_BUTTON_FORWARD) ? 16u : 0u);
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

bool tide_window_key_held(const tide_key key)
{
    const bool held = app.keys_held[key] || app.keys_tapped[key];
    app.keys_tapped[key] = false;
    return held;
}

void tide_window_mouse_state(tide_window_mouse *out)
{
    *out = (tide_window_mouse){app.mouse_x, app.mouse_y, app.wheel_x, app.wheel_y, app.mouse_held | app.mouse_tapped};
    app.wheel_x = app.wheel_y = 0.0f;
    app.mouse_tapped = 0;
}

void tide_window_gamepad_state(tide_window_gamepad *out)
{
    // The first gamepad, once it said something (Android tells programs of
    // pads coming and going through Java only)
    *out = (tide_window_gamepad){.connected = app.pad_seen};
    if (!app.pad_seen) return;
    out->left_x = app.pad_sticks[0];
    out->left_y = app.pad_sticks[1];
    out->right_x = app.pad_sticks[2];
    out->right_y = app.pad_sticks[3];
    out->left_trigger = app.pad_triggers[0];
    out->right_trigger = app.pad_triggers[1];
    out->buttons = app.pad_held | app.pad_tapped;
    app.pad_tapped = 0;
}

bool tide_window_touchscreen(void)
{
    return true; // Android's devices have one, or emulate one
}

bool tide_window_take_touch(tide_touch_report *r)
{
    if (app.touch_count == 0) return false;
    *r = app.touches[app.touch_start];
    r->x *= (float)app.width;
    r->y *= (float)app.height;
    app.touch_start = (app.touch_start + 1) % COUNT_OF(app.touches);
    app.touch_count--;
    return true;
}

uint32_t tide_window_take_char(void)
{
    if (app.typed_count == 0) return 0;
    const uint32_t c = app.typed[app.typed_start];
    app.typed_start = (app.typed_start + 1) % COUNT_OF(app.typed);
    app.typed_count--;
    return c;
}

// The clipboard is Java's: what's copied goes from the main thread, and what's
// pasted comes on the program's (Ctrl+V on a keyboard).
void tide_window_copy(const char *text)
{
    pthread_mutex_lock(&app.lock);
    snprintf(copy_text, sizeof copy_text, "%s", text ? text : "");
    pthread_mutex_unlock(&app.lock);
    ask_ui('c');
}

const char *tide_window_take_paste(void)
{
    static char pasted[4096];
    if (!app.paste_asked) return NULL;
    app.paste_asked = false;
    pasted[0] = '\0';
    JNIEnv *env = NULL;
    if (!java_vm || !app.activity || (*java_vm)->AttachCurrentThread(java_vm, &env, NULL) != JNI_OK) return pasted;
    (*env)->PushLocalFrame(env, 16);
    jstring service = (*env)->NewStringUTF(env, "clipboard");
    jobject clipboard = call(env, app.activity->clazz, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;", service);
    jobject clip = clipboard ? call(env, clipboard, "getPrimaryClip", "()Landroid/content/ClipData;") : NULL;
    jobject item = clip ? call(env, clip, "getItemAt", "(I)Landroid/content/ClipData$Item;", 0) : NULL;
    jobject text = item ? call(env, item, "coerceToText", "(Landroid/content/Context;)Ljava/lang/CharSequence;",
                               app.activity->clazz) : NULL;
    jstring string = text ? (jstring)call(env, text, "toString", "()Ljava/lang/String;") : NULL;
    if (string) {
        const char *chars = (*env)->GetStringUTFChars(env, string, NULL);
        if (chars) {
            snprintf(pasted, sizeof pasted, "%s", chars);
            (*env)->ReleaseStringUTFChars(env, string, chars);
        }
    }
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->PopLocalFrame(env, NULL);
    return pasted;
}

void tide_window_typing(const bool typing)
{
    ask_ui(typing ? '1' : '0');
}

// ---------------------------------------------------------------------------
// Window: the activity's, which the system moves and sizes

bool tide_window_should_close(void)
{
    pthread_mutex_lock(&app.lock);
    const bool destroyed = app.destroyed;
    pthread_mutex_unlock(&app.lock);
    return destroyed;
}

// An app in the background has no window: frames come when the frame
// function asked, and draw nothing, until the system freezes it.
bool tide_window_unseen(void)
{
    return app.surface == EGL_NO_SURFACE;
}

void tide_window_size(int *width, int *height)
{
    fit_window();
    *width = app.width;
    *height = app.height;
}

void tide_window_pixel_size(int *width, int *height)
{
    fit_window();
    *width = app.pixel_width;
    *height = app.pixel_height;
}

void tide_window_present(void)
{
    if (app.surface != EGL_NO_SURFACE && !eglSwapBuffers(app.display, app.surface)) {
        const EGLint error = eglGetError();
        if (error == EGL_BAD_SURFACE || error == EGL_BAD_NATIVE_WINDOW) drop_surface(); // Gone under us
    }
    poll_looper(0);
}

// It waits on the activity, so a window given back goes on at once.
void tide_window_wait(const double seconds)
{
    poll_looper(seconds > 0.0 ? (int)(seconds * 1000.0) + 1 : 0);
}

bool tide_window_open(const tide_window_desc *desc)
{
    (void)desc; // The system sizes the activity's window, and names it after the app
    app.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (app.display == EGL_NO_DISPLAY || !eglInitialize(app.display, NULL, NULL)) {
        fprintf(stderr, "tide: no EGL display\n");
        return false;
    }
    const EGLint wanted[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RED_SIZE, 8,
                             EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_NONE};
    EGLint found = 0;
    if (!eglChooseConfig(app.display, wanted, &app.config, 1, &found) || found < 1) {
        fprintf(stderr, "tide: this device has no OpenGL ES 3\n");
        return false;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint version[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    app.context = eglCreateContext(app.display, app.config, EGL_NO_CONTEXT, version);
    if (app.context == EGL_NO_CONTEXT) {
        fprintf(stderr, "tide: no OpenGL ES 3 context (EGL error 0x%x)\n", (unsigned)eglGetError());
        return false;
    }
    app.surface = EGL_NO_SURFACE;

    // The first window: the renderer needs a context that's current to start.
    while (!tide_window_should_close()) {
        poll_looper(-1);
        if (app.surface != EGL_NO_SURFACE) break;
    }
    return app.surface != EGL_NO_SURFACE; // Or the activity went before it had a window
}

void tide_window_close(void)
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
