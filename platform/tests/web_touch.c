// Web only: fingers on the canvas are the touchscreen's, and never the mouse's.
//
// The page gets pointer events as a touchscreen sends them. Each frame checks
// the events sent at the end of the previous one, the way real events arrive
// between frames, then samples the devices, as a tick would.

#include <stdio.h>

#include "tide/platform.h"
#include "tide_web.h"

static tide_devices devices;
static int failures;

static void check(const bool ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

// A pointer event on the canvas, `x` and `y` CSS pixels from its top left.
static void pointer_event(const char *type, const char *kind, const int id, const int x, const int y)
{
    char script[400];
    snprintf(script, sizeof script,
             "{ const c = document.querySelector('canvas'); const r = c.getBoundingClientRect();"
             " c.dispatchEvent(new PointerEvent('%s', {pointerId: %d, pointerType: '%s', clientX: r.left + %d,"
             " clientY: r.top + %d, bubbles: true, cancelable: true})); }",
             type, id, kind, x, y);
    tide_web_eval(script);
}

static int frame(void *user, const float seconds)
{
    (void)user, (void)seconds;
    static int step;
    tide_platform_poll(&devices);
    const tide_touch *first = &devices.touchscreen.touches.at[0];
    const tide_float2 size = tide_platform_screen_size();
    int code = TIDE_KEEP_RUNNING;

    switch (step++) {
    case 0:
        pointer_event("pointerdown", "touch", 5, 10, 20);
        break;
    case 1:
        check(first->press.held && first->press.down && first->id == 1, "a finger touches in the first slot");
        check(first->position.x == 10.0f && first->position.y == size.y - 20.0f,
              "where it touched, from the bottom left like the mouse");
        check(devices.touchscreen.primaryTouch.id == 1, "and it's the primary touch");
        check(devices.pointer.touch && devices.pointer.press.held && devices.pointer.position.x == 10.0f,
              "the pointer follows it");
        check(!devices.mouse.left.held, "it isn't the mouse");
        pointer_event("pointermove", "touch", 5, 14, 20);
        break;
    case 2:
        check(first->position.x == 14.0f && first->delta.x == 4.0f && first->startPosition.x == 10.0f,
              "it moves, from where it started");
        pointer_event("pointerup", "touch", 5, 14, 20);
        break;
    case 3:
        check(!first->press.held && first->press.up, "it lifts");
        check(!devices.pointer.press.held && devices.pointer.press.up, "and the pointer lets go");
        // A tap shorter than a frame: down and up again before the next one.
        pointer_event("pointerdown", "touch", 6, 30, 30);
        pointer_event("pointerup", "touch", 6, 30, 30);
        break;
    case 4:
        check(first->press.held && first->press.down && first->id == 2, "a tap between frames reads as held for one");
        break;
    case 5:
        check(!first->press.held && first->press.up, "and as lifted the next");
        // The mouse: not a finger, and the pointer goes back to it.
        pointer_event("pointerdown", "mouse", 1, 40, 40);
        tide_web_eval("{ const c = document.querySelector('canvas'); const r = c.getBoundingClientRect();"
                      " c.dispatchEvent(new MouseEvent('mousedown', {button: 0, clientX: r.left + 40,"
                      " clientY: r.top + 40, bubbles: true})); }");
        break;
    default:
        check(first->id == 0 && !devices.touchscreen.primaryTouch.press.held, "the mouse isn't a finger");
        check(devices.mouse.left.held && !devices.pointer.touch && devices.pointer.press.held,
              "the pointer follows the mouse once it's used");
        code = failures == 0 ? 0 : 1;
        break;
    }
    tide_devices_consume(&devices);
    return code;
}

int main(void)
{
    tide_platform_open(&(tide_window_desc){.title = "web touch", .width = 64, .height = 64, .hidden = true});
    tide_platform_run(frame, NULL);
}
