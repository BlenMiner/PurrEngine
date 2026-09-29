// Web only: the platform layer must read keys by physical position.
//
// The events imitate an AZERTY keyboard, where the key in the US W position
// types 'z'. Anything reading the typed key would report Z; Devices must report
// w, as on desktop. The characters typed are the other way around: they follow
// the layout, so the GUI's fields get what's printed on the keys.

#include <stdio.h>

#include "purr/platform.h"
#include "purr_web.h"

static purr_devices devices;
static int failures;

static void check(const bool ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

// `key_code` is the legacy code for the typed key, which follows the layout.
static void key_event(const char *type, const char *code, const char *key, const int key_code)
{
    char script[200];
    snprintf(script, sizeof script, "dispatchEvent(new KeyboardEvent('%s', {code: '%s', key: '%s', keyCode: %d}))",
             type, code, key, key_code);
    purr_web_eval(script);
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
        check(devices.text.count == 1 && devices.text.chars[0] == 'z', "but the character it types is z");
        key_event("keyup", "KeyW", "z", 90);
        return PURR_KEEP_RUNNING;
    case 2:
        check(!devices.keyboard.w.held && devices.keyboard.w.up, "releasing it reads as a release");
        check(devices.text.count == 0, "characters are only typed once");
        // A tap shorter than a frame: down and up again before the next one.
        key_event("keydown", "KeyA", "q", 81);
        key_event("keyup", "KeyA", "q", 81);
        return PURR_KEEP_RUNNING;
    case 3:
        check(devices.keyboard.a.held && devices.keyboard.a.down, "a tap between frames reads as held for one frame");
        return PURR_KEEP_RUNNING;
    case 4:
        check(!devices.keyboard.a.held && devices.keyboard.a.up, "and as released the next");
        key_event("keydown", "Space", " ", 32);
        purr_web_eval("dispatchEvent(new FocusEvent('blur'))");
        return PURR_KEEP_RUNNING;
    default:
        check(!devices.keyboard.space.held, "losing focus releases held keys");
        check(devices.text.count == 1 && devices.text.chars[0] == ' ', "space types a space");
        return failures == 0 ? 0 : 1;
    }
}

int main(void)
{
    purr_platform_open(&(purr_window_desc){.title = "web keys", .width = 64, .height = 64, .hidden = true});
    purr_platform_run(frame, NULL);
}
