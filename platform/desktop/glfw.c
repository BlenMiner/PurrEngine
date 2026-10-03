// Desktop windows (see src/window.h): GLFW's, with an OpenGL 3.3 context, and
// the keyboard, mouse, gamepads and clipboard through it. Touchscreens are
// Windows' own (src/touch_win32.c): GLFW has no touch, so other desktops have
// none yet.

#include "window.h"

#include <stdio.h>

#define GLFW_INCLUDE_NONE // Its OpenGL is ours (src/gl.h)
#include <GLFW/glfw3.h>

#include "gl.h"
#include "touch_win32.h"

#ifdef _WIN32
void *glfwGetWin32Window(GLFWwindow *window); // GLFW's glfw3native.h, without Windows' headers: an HWND
#endif

#define COUNT_OF(array) (sizeof(array) / sizeof((array)[0]))

// Every Tide key with GLFW's, which names physical positions after the US
// layout, as Tide does.
static const struct {
    tide_key key;
    int glfw;
} keys[] = {
    {TIDE_KEY_a, GLFW_KEY_A}, {TIDE_KEY_b, GLFW_KEY_B}, {TIDE_KEY_c, GLFW_KEY_C}, {TIDE_KEY_d, GLFW_KEY_D},
    {TIDE_KEY_e, GLFW_KEY_E}, {TIDE_KEY_f, GLFW_KEY_F}, {TIDE_KEY_g, GLFW_KEY_G}, {TIDE_KEY_h, GLFW_KEY_H},
    {TIDE_KEY_i, GLFW_KEY_I}, {TIDE_KEY_j, GLFW_KEY_J}, {TIDE_KEY_k, GLFW_KEY_K}, {TIDE_KEY_l, GLFW_KEY_L},
    {TIDE_KEY_m, GLFW_KEY_M}, {TIDE_KEY_n, GLFW_KEY_N}, {TIDE_KEY_o, GLFW_KEY_O}, {TIDE_KEY_p, GLFW_KEY_P},
    {TIDE_KEY_q, GLFW_KEY_Q}, {TIDE_KEY_r, GLFW_KEY_R}, {TIDE_KEY_s, GLFW_KEY_S}, {TIDE_KEY_t, GLFW_KEY_T},
    {TIDE_KEY_u, GLFW_KEY_U}, {TIDE_KEY_v, GLFW_KEY_V}, {TIDE_KEY_w, GLFW_KEY_W}, {TIDE_KEY_x, GLFW_KEY_X},
    {TIDE_KEY_y, GLFW_KEY_Y}, {TIDE_KEY_z, GLFW_KEY_Z},
    {TIDE_KEY_digit0, GLFW_KEY_0}, {TIDE_KEY_digit1, GLFW_KEY_1}, {TIDE_KEY_digit2, GLFW_KEY_2},
    {TIDE_KEY_digit3, GLFW_KEY_3}, {TIDE_KEY_digit4, GLFW_KEY_4}, {TIDE_KEY_digit5, GLFW_KEY_5},
    {TIDE_KEY_digit6, GLFW_KEY_6}, {TIDE_KEY_digit7, GLFW_KEY_7}, {TIDE_KEY_digit8, GLFW_KEY_8},
    {TIDE_KEY_digit9, GLFW_KEY_9},
    {TIDE_KEY_space, GLFW_KEY_SPACE}, {TIDE_KEY_enter, GLFW_KEY_ENTER}, {TIDE_KEY_escape, GLFW_KEY_ESCAPE},
    {TIDE_KEY_tab, GLFW_KEY_TAB}, {TIDE_KEY_backspace, GLFW_KEY_BACKSPACE},
    {TIDE_KEY_insert, GLFW_KEY_INSERT}, {TIDE_KEY_delete, GLFW_KEY_DELETE}, {TIDE_KEY_home, GLFW_KEY_HOME},
    {TIDE_KEY_end, GLFW_KEY_END}, {TIDE_KEY_pageUp, GLFW_KEY_PAGE_UP}, {TIDE_KEY_pageDown, GLFW_KEY_PAGE_DOWN},
    {TIDE_KEY_upArrow, GLFW_KEY_UP}, {TIDE_KEY_downArrow, GLFW_KEY_DOWN},
    {TIDE_KEY_leftArrow, GLFW_KEY_LEFT}, {TIDE_KEY_rightArrow, GLFW_KEY_RIGHT},
    {TIDE_KEY_leftShift, GLFW_KEY_LEFT_SHIFT}, {TIDE_KEY_rightShift, GLFW_KEY_RIGHT_SHIFT},
    {TIDE_KEY_leftCtrl, GLFW_KEY_LEFT_CONTROL}, {TIDE_KEY_rightCtrl, GLFW_KEY_RIGHT_CONTROL},
    {TIDE_KEY_leftAlt, GLFW_KEY_LEFT_ALT}, {TIDE_KEY_rightAlt, GLFW_KEY_RIGHT_ALT},
    {TIDE_KEY_capsLock, GLFW_KEY_CAPS_LOCK},
    {TIDE_KEY_f1, GLFW_KEY_F1}, {TIDE_KEY_f2, GLFW_KEY_F2}, {TIDE_KEY_f3, GLFW_KEY_F3}, {TIDE_KEY_f4, GLFW_KEY_F4},
    {TIDE_KEY_f5, GLFW_KEY_F5}, {TIDE_KEY_f6, GLFW_KEY_F6}, {TIDE_KEY_f7, GLFW_KEY_F7}, {TIDE_KEY_f8, GLFW_KEY_F8},
    {TIDE_KEY_f9, GLFW_KEY_F9}, {TIDE_KEY_f10, GLFW_KEY_F10}, {TIDE_KEY_f11, GLFW_KEY_F11},
    {TIDE_KEY_f12, GLFW_KEY_F12},
    {TIDE_KEY_minus, GLFW_KEY_MINUS}, {TIDE_KEY_equals, GLFW_KEY_EQUAL},
    {TIDE_KEY_leftBracket, GLFW_KEY_LEFT_BRACKET}, {TIDE_KEY_rightBracket, GLFW_KEY_RIGHT_BRACKET},
    {TIDE_KEY_backslash, GLFW_KEY_BACKSLASH}, {TIDE_KEY_semicolon, GLFW_KEY_SEMICOLON},
    {TIDE_KEY_quote, GLFW_KEY_APOSTROPHE}, {TIDE_KEY_comma, GLFW_KEY_COMMA}, {TIDE_KEY_period, GLFW_KEY_PERIOD},
    {TIDE_KEY_slash, GLFW_KEY_SLASH}, {TIDE_KEY_backquote, GLFW_KEY_GRAVE_ACCENT},
    {TIDE_KEY_numpad0, GLFW_KEY_KP_0}, {TIDE_KEY_numpad1, GLFW_KEY_KP_1}, {TIDE_KEY_numpad2, GLFW_KEY_KP_2},
    {TIDE_KEY_numpad3, GLFW_KEY_KP_3}, {TIDE_KEY_numpad4, GLFW_KEY_KP_4}, {TIDE_KEY_numpad5, GLFW_KEY_KP_5},
    {TIDE_KEY_numpad6, GLFW_KEY_KP_6}, {TIDE_KEY_numpad7, GLFW_KEY_KP_7}, {TIDE_KEY_numpad8, GLFW_KEY_KP_8},
    {TIDE_KEY_numpad9, GLFW_KEY_KP_9},
    {TIDE_KEY_numpadEnter, GLFW_KEY_KP_ENTER}, {TIDE_KEY_numpadPlus, GLFW_KEY_KP_ADD},
    {TIDE_KEY_numpadMinus, GLFW_KEY_KP_SUBTRACT}, {TIDE_KEY_numpadMultiply, GLFW_KEY_KP_MULTIPLY},
    {TIDE_KEY_numpadDivide, GLFW_KEY_KP_DIVIDE}, {TIDE_KEY_numpadPeriod, GLFW_KEY_KP_DECIMAL},
};

_Static_assert(COUNT_OF(keys) == TIDE_KEY_COUNT, "every key in devices.h needs GLFW's");

// GLFW's gamepad buttons, which its mappings name by position, as an Xbox
// pad's: A is south.
static const struct {
    tide_pad_button button;
    int glfw;
} pad_buttons[] = {
    {TIDE_PAD_buttonSouth, GLFW_GAMEPAD_BUTTON_A},
    {TIDE_PAD_buttonEast, GLFW_GAMEPAD_BUTTON_B},
    {TIDE_PAD_buttonWest, GLFW_GAMEPAD_BUTTON_X},
    {TIDE_PAD_buttonNorth, GLFW_GAMEPAD_BUTTON_Y},
    {TIDE_PAD_leftShoulder, GLFW_GAMEPAD_BUTTON_LEFT_BUMPER},
    {TIDE_PAD_rightShoulder, GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER},
    {TIDE_PAD_leftStickButton, GLFW_GAMEPAD_BUTTON_LEFT_THUMB},
    {TIDE_PAD_rightStickButton, GLFW_GAMEPAD_BUTTON_RIGHT_THUMB},
    {TIDE_PAD_start, GLFW_GAMEPAD_BUTTON_START},
    {TIDE_PAD_select, GLFW_GAMEPAD_BUTTON_BACK},
    {TIDE_PAD_dpad_up, GLFW_GAMEPAD_BUTTON_DPAD_UP},
    {TIDE_PAD_dpad_down, GLFW_GAMEPAD_BUTTON_DPAD_DOWN},
    {TIDE_PAD_dpad_left, GLFW_GAMEPAD_BUTTON_DPAD_LEFT},
    {TIDE_PAD_dpad_right, GLFW_GAMEPAD_BUTTON_DPAD_RIGHT},
};

_Static_assert(COUNT_OF(pad_buttons) == TIDE_PAD_COUNT, "every gamepad button needs GLFW's");

static GLFWwindow *window;
static int pixel_width = 1, pixel_height = 1; // The framebuffer's, as of when it last had a size
static double wheel_x, wheel_y;               // Since the last tide_window_mouse_state
static bool paste_asked;

// What's held, and what was pressed since it was last asked about: a key or
// button pressed and released between two frames reads as held for one, so
// taps aren't lost when frames are slow. (GLFW's sticky keys would also hold
// every key a frame past its release.)
static int16_t key_of[GLFW_KEY_LAST + 1]; // 1 + GLFW's key's tide_key, or 0 for none
static bool keys_held[TIDE_KEY_COUNT], keys_tapped[TIDE_KEY_COUNT];
static uint32_t mouse_held, mouse_tapped; // Bits in tide_mouse's order

// Characters typed and not yet taken: more than fit are dropped.
static uint32_t typed[256];
static unsigned typed_start, typed_count;

static void on_error(const int code, const char *text)
{
    if (code == GLFW_FORMAT_UNAVAILABLE) return; // Pasting while the clipboard holds no text
    fprintf(stderr, "tide: GLFW: %s (0x%X)\n", text, (unsigned)code);
}

static void on_char(GLFWwindow *from, const unsigned int codepoint)
{
    (void)from;
    if (typed_count < COUNT_OF(typed)) typed[(typed_start + typed_count++) % COUNT_OF(typed)] = codepoint;
}

static void on_scroll(GLFWwindow *from, const double x, const double y)
{
    (void)from;
    wheel_x += x;
    wheel_y += y;
}

static void on_key(GLFWwindow *from, const int key, const int scancode, const int action, const int mods)
{
    (void)from, (void)scancode;
    // Ctrl+V, or Cmd+V on macOS
    if (key == GLFW_KEY_V && action == GLFW_PRESS && (mods & (GLFW_MOD_CONTROL | GLFW_MOD_SUPER))) paste_asked = true;
    if (key < 0 || key > GLFW_KEY_LAST || !key_of[key]) return; // A key Tide has no name for
    const int ours = key_of[key] - 1;
    if (action == GLFW_PRESS) keys_held[ours] = keys_tapped[ours] = true;
    else if (action == GLFW_RELEASE) keys_held[ours] = false; // GLFW releases what's held when the window loses focus
}

static void on_mouse_button(GLFWwindow *from, const int button, const int action, const int mods)
{
    (void)from, (void)mods;
    // GLFW's first five are left, right, middle, and the two mice send for back and forward
    if (button < 0 || button > 4) return;
    if (action == GLFW_PRESS) mouse_held |= 1u << button, mouse_tapped |= 1u << button;
    else if (action == GLFW_RELEASE) mouse_held &= ~(1u << button);
}

bool tide_window_open(const tide_window_desc *desc)
{
    glfwSetErrorCallback(on_error);
    if (!glfwInit()) {
        fprintf(stderr, "tide: no desktop to open a window on\n");
        return false;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE); // The only core contexts macOS makes
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);              // Shown once it's in its place
    // Tests keep the size they asked for.
    glfwWindowHint(GLFW_RESIZABLE, desc->hidden ? GLFW_FALSE : GLFW_TRUE);
    // Pixels are the display's logical ones (see tide/platform.h): the size
    // asked for is in those, so the window is as many times bigger as the
    // display is scaled where the system counts windows in its own pixels
    // (Windows, X11). It renders at the display's full resolution either way.
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
    window = glfwCreateWindow(desc->width, desc->height, desc->title ? desc->title : "", NULL, NULL);
    if (!window) {
        fprintf(stderr, "tide: no window with OpenGL 3.3 to be had\n");
        glfwTerminate();
        return false;
    }
    glfwMakeContextCurrent(window);
    if (!tide_gl_load(glfwGetProcAddress)) {
        tide_window_close();
        return false;
    }
    // Frames at the display's pace; tests run as fast as they can.
    glfwSwapInterval(desc->hidden ? 0 : 1);

    for (size_t i = 0; i < COUNT_OF(keys); i++) key_of[keys[i].glfw] = (int16_t)(keys[i].key + 1);
    glfwSetCharCallback(window, on_char);
    glfwSetScrollCallback(window, on_scroll);
    glfwSetKeyCallback(window, on_key);
    glfwSetMouseButtonCallback(window, on_mouse_button);
#ifdef _WIN32
    tide_win32_touch_attach(glfwGetWin32Window(window));
#endif

    if (!desc->hidden) {
        // In the middle of the screen, as far as its work area goes
        GLFWmonitor *monitor = glfwGetPrimaryMonitor();
        if (monitor) {
            int x = 0, y = 0, width = 0, height = 0, across = 0, down = 0;
            glfwGetMonitorWorkarea(monitor, &x, &y, &width, &height);
            glfwGetWindowSize(window, &across, &down);
            if (width > across && height > down) glfwSetWindowPos(window, x + (width - across) / 2, y + (height - down) / 2);
        }
        glfwShowWindow(window);
    }
    return true;
}

void tide_window_close(void)
{
    if (window) glfwDestroyWindow(window);
    window = NULL;
    glfwTerminate();
}

bool tide_window_should_close(void)
{
    return glfwWindowShouldClose(window);
}

bool tide_window_unseen(void)
{
    return glfwGetWindowAttrib(window, GLFW_ICONIFIED);
}

void tide_window_pixel_size(int *width, int *height)
{
    int w = 0, h = 0;
    glfwGetFramebufferSize(window, &w, &h);
    if (w > 0 && h > 0) pixel_width = w, pixel_height = h; // A minimized window has none
    *width = pixel_width;
    *height = pixel_height;
}

// The display's pixels per logical pixel, as the system says the window's
// contents are scaled.
static void content_scale(float *x, float *y)
{
    *x = *y = 1.0f;
    glfwGetWindowContentScale(window, x, y);
    if (!(*x > 0.0f)) *x = 1.0f;
    if (!(*y > 0.0f)) *y = 1.0f;
}

void tide_window_size(int *width, int *height)
{
    int w, h;
    float x, y;
    tide_window_pixel_size(&w, &h);
    content_scale(&x, &y);
    *width = (int)((float)w / x + 0.5f);
    *height = (int)((float)h / y + 0.5f);
    if (*width < 1) *width = 1;
    if (*height < 1) *height = 1;
}

void tide_window_present(void)
{
    glfwSwapBuffers(window);
    glfwPollEvents();
}

void tide_window_wait(const double seconds)
{
    // Less than the system's timers wait (Windows' take milliseconds)
    if (seconds < 0.001) {
        const double until = glfwGetTime() + seconds;
        while (glfwGetTime() < until) { }
        return;
    }
    glfwWaitEventsTimeout(seconds);
}

bool tide_window_key_held(const tide_key key)
{
    const bool held = keys_held[key] || keys_tapped[key];
    keys_tapped[key] = false;
    return held;
}

void tide_window_mouse_state(tide_window_mouse *out)
{
    // The cursor is in the units the system counts windows in: logical pixels
    // on macOS, the display's own elsewhere.
    double x = 0.0, y = 0.0;
    int across = 0, down = 0, width, height;
    glfwGetCursorPos(window, &x, &y);
    glfwGetWindowSize(window, &across, &down);
    tide_window_size(&width, &height);
    out->x = across > 0 ? (float)(x * (double)width / (double)across) : 0.0f;
    out->y = down > 0 ? (float)(y * (double)height / (double)down) : 0.0f;
    out->wheel_x = (float)wheel_x;
    out->wheel_y = (float)wheel_y;
    wheel_x = wheel_y = 0.0;
    out->buttons = mouse_held | mouse_tapped;
    mouse_tapped = 0;
}

void tide_window_gamepad_state(tide_window_gamepad *out)
{
    *out = (tide_window_gamepad){0};
    // The first joystick GLFW has a gamepad's mapping for
    for (int id = GLFW_JOYSTICK_1; id <= GLFW_JOYSTICK_LAST; id++) {
        GLFWgamepadstate state;
        if (!glfwJoystickIsGamepad(id) || !glfwGetGamepadState(id, &state)) continue;
        out->connected = true;
        out->left_x = state.axes[GLFW_GAMEPAD_AXIS_LEFT_X];
        out->left_y = state.axes[GLFW_GAMEPAD_AXIS_LEFT_Y];
        out->right_x = state.axes[GLFW_GAMEPAD_AXIS_RIGHT_X];
        out->right_y = state.axes[GLFW_GAMEPAD_AXIS_RIGHT_Y];
        // GLFW's triggers are -1 released to 1 fully pressed
        out->left_trigger = (state.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] + 1.0f) * 0.5f;
        out->right_trigger = (state.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] + 1.0f) * 0.5f;
        for (size_t i = 0; i < COUNT_OF(pad_buttons); i++) {
            if (state.buttons[pad_buttons[i].glfw] == GLFW_PRESS) out->buttons |= 1u << pad_buttons[i].button;
        }
        return;
    }
}

bool tide_window_touchscreen(void)
{
#ifdef _WIN32
    return tide_win32_touchscreen();
#else
    return false;
#endif
}

bool tide_window_take_touch(tide_touch_report *report)
{
#ifdef _WIN32
    if (!tide_win32_take_touch(report)) return false;
    int width, height;
    tide_window_size(&width, &height);
    report->x *= (float)width;
    report->y *= (float)height;
    return true;
#else
    (void)report;
    return false;
#endif
}

uint32_t tide_window_take_char(void)
{
    if (typed_count == 0) return 0;
    const uint32_t c = typed[typed_start];
    typed_start = (typed_start + 1) % COUNT_OF(typed);
    typed_count--;
    return c;
}

const char *tide_window_take_paste(void)
{
    if (!paste_asked) return NULL;
    paste_asked = false;
    return glfwGetClipboardString(window);
}

void tide_window_copy(const char *text)
{
    glfwSetClipboardString(window, text);
}

void tide_window_typing(const bool typing)
{
    (void)typing; // Desktops have their keyboard out already
}
