// Web only: the platform layer must read keys by physical position.
//
// The events imitate an AZERTY keyboard, where the key in the US W position
// types 'z'. Emscripten's GLFW, which raylib uses on the web, reads the typed
// key and would report Z; Devices must report w, as on desktop.

#include <stdio.h>

#include <emscripten/emscripten.h>

#include "purr/platform.h"

static purr_devices devices;
static int failures;

static void check(const bool ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

// `key_code` is the legacy code for the typed key, which Emscripten's GLFW reads.
static void key_event(const char *type, const char *code, const char *key, const int key_code)
{
    char script[200];
    snprintf(script, sizeof script, "dispatchEvent(new KeyboardEvent('%s', {code: '%s', key: '%s', keyCode: %d}))",
             type, code, key, key_code);
    emscripten_run_script(script);
}

// Each frame checks the events sent at the end of the previous one, the way
// real events arrive between frames.
static int frame(void *user, const float seconds)
{
    (void)user, (void)seconds;
    static int step;
    purr_platform_poll(&devices);

    switch (step++) {
    case 0:
        key_event("keydown", "KeyW", "z", 90);
        return PURR_KEEP_RUNNING;
    case 1:
        check(devices.keyboard.w.held && devices.keyboard.w.down, "the key in the W position reads as w");
        check(!devices.keyboard.z.held, "and not as z, the letter it types on AZERTY");
        key_event("keyup", "KeyW", "z", 90);
        return PURR_KEEP_RUNNING;
    case 2:
        check(!devices.keyboard.w.held && devices.keyboard.w.up, "releasing it reads as a release");
        key_event("keydown", "Space", " ", 32);
        emscripten_run_script("dispatchEvent(new FocusEvent('blur'))");
        return PURR_KEEP_RUNNING;
    default:
        check(!devices.keyboard.space.held, "losing focus releases held keys");
        return failures == 0 ? 0 : 1;
    }
}

int main(void)
{
    purr_platform_open(&(purr_window_desc){.title = "web keys", .width = 64, .height = 64, .hidden = true});
    purr_platform_run(frame, NULL);
}
