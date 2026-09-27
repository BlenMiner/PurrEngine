#include "purr/devices.h"

static void consume_button(purr_button *b)
{
    b->down = b->held;
    b->pressed = false;
    b->released = false;
}

void purr_devices_consume(purr_devices *d)
{
#define CONSUME_KEY(name) consume_button(&d->keyboard.name);
#define CONSUME_MOUSE(name) consume_button(&d->mouse.name);
#define CONSUME_GAMEPAD(name) consume_button(&d->gamepad.name);
#define CONSUME_DPAD(name) consume_button(&d->gamepad.dpad.name);
    PURR_KEYBOARD_KEYS(CONSUME_KEY)
    PURR_MOUSE_BUTTONS(CONSUME_MOUSE)
    PURR_GAMEPAD_BUTTONS(CONSUME_GAMEPAD)
    PURR_DPAD_BUTTONS(CONSUME_DPAD)
#undef CONSUME_KEY
#undef CONSUME_MOUSE
#undef CONSUME_GAMEPAD
#undef CONSUME_DPAD

    d->mouse.delta = purr_f2(0.0f, 0.0f);
    d->mouse.scroll = purr_f2(0.0f, 0.0f);
}
