#include "tide/devices.h"

static void consume_button(tide_button *b)
{
    b->pressed = b->held;
    b->down = false;
    b->up = false;
}

void tide_devices_consume(tide_devices *d)
{
#define CONSUME_KEY(name) consume_button(&d->keyboard.name);
#define CONSUME_MOUSE(name) consume_button(&d->mouse.name);
#define CONSUME_GAMEPAD(name) consume_button(&d->gamepad.name);
#define CONSUME_DPAD(name) consume_button(&d->gamepad.dpad.name);
    TIDE_KEYBOARD_KEYS(CONSUME_KEY)
    TIDE_MOUSE_BUTTONS(CONSUME_MOUSE)
    TIDE_GAMEPAD_BUTTONS(CONSUME_GAMEPAD)
    TIDE_DPAD_BUTTONS(CONSUME_DPAD)
#undef CONSUME_KEY
#undef CONSUME_MOUSE
#undef CONSUME_GAMEPAD
#undef CONSUME_DPAD

    d->mouse.delta = tide_f2(0.0f, 0.0f);
    d->mouse.scroll = tide_f2(0.0f, 0.0f);
}
