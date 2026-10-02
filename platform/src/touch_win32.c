// Touches on Windows: the window's pointer messages, which GLFW leaves to the
// system, which turns them into the mouse's. Taken here first, they're only
// the touchscreen's, as Unity's are.

#ifdef _WIN32

// Windows 8, where touches come as pointer messages. MinGW's headers start
// older.
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0602
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "native.h"

#define QUEUE 256 // Events between two polls; more are dropped, as a finger that touches while every slot is taken

static WNDPROC glfw_proc; // The window's own, which everything else goes to
static tide_touch_report queue[QUEUE];
static unsigned queue_start, queue_count;

static void report(const HWND window, const tide_touch_phase phase, const UINT32 pointer)
{
    if (queue_count == QUEUE) return;
    tide_touch_report r = {phase, pointer, 0.0f, 0.0f};
    POINTER_INFO info;
    if (phase != TIDE_TOUCH_CANCELED && GetPointerInfo(pointer, &info)) {
        POINT at = info.ptPixelLocation;
        RECT inside;
        if (ScreenToClient(window, &at) && GetClientRect(window, &inside) && inside.right > 0 && inside.bottom > 0) {
            r.x = (float)at.x / (float)inside.right;
            r.y = (float)at.y / (float)inside.bottom;
        }
    }
    queue[(queue_start + queue_count++) % QUEUE] = r;
}

static LRESULT CALLBACK touch_proc(const HWND window, const UINT message, const WPARAM w, const LPARAM l)
{
    if (message == WM_POINTERDOWN || message == WM_POINTERUPDATE || message == WM_POINTERUP
        || message == WM_POINTERCAPTURECHANGED) {
        const UINT32 pointer = GET_POINTERID_WPARAM(w);
        POINTER_INPUT_TYPE type;
        if (GetPointerType(pointer, &type) && type == PT_TOUCH) {
            if (message == WM_POINTERDOWN) report(window, TIDE_TOUCH_BEGAN, pointer);
            else if (message == WM_POINTERUPDATE && IS_POINTER_INCONTACT_WPARAM(w)) report(window, TIDE_TOUCH_MOVED, pointer);
            else if (message == WM_POINTERUP) {
                report(window, IS_POINTER_CANCELED_WPARAM(w) ? TIDE_TOUCH_CANCELED : TIDE_TOUCH_ENDED, pointer);
            } else if (message == WM_POINTERCAPTURECHANGED) {
                report(window, TIDE_TOUCH_CANCELED, pointer);
            }
            return 0; // Handled: the system makes no mouse of it
        }
    }
    return CallWindowProcW(glfw_proc, window, message, w, l);
}

void tide_win32_touch_attach(void *window)
{
    if (!window || glfw_proc) return;
    glfw_proc = (WNDPROC)SetWindowLongPtrW((HWND)window, GWLP_WNDPROC, (LONG_PTR)touch_proc);
}

bool tide_native_touchscreen(void)
{
    return GetSystemMetrics(SM_MAXIMUMTOUCHES) > 0;
}

bool tide_native_take_touch(tide_touch_report *r)
{
    if (queue_count == 0) return false;
    *r = queue[queue_start];
    queue_start = (queue_start + 1) % QUEUE;
    queue_count--;
    return true;
}

#else
typedef int tide_no_win32_touch; // ISO C wants something in every file
#endif
