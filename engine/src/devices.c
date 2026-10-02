#include "tide/devices.h"

#include <stdint.h>

static void consume_button(tide_button *b)
{
    b->pressed = b->held;
    b->down = false;
    b->up = false;
}

// A lifted finger leaves its slot once a sample saw it lift.
static void consume_touch(tide_touch *t)
{
    consume_button(&t->press);
    t->delta = tide_f2(0.0f, 0.0f);
    if (!t->press.held) *t = (tide_touch){0};
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

    consume_touch(&d->touchscreen.primaryTouch);
    for (int i = 0; i < TIDE_TOUCHES; i++) consume_touch(&d->touchscreen.touches.at[i]);
    consume_button(&d->pointer.press);
    d->pointer.delta = tide_f2(0.0f, 0.0f);
}

// ---------------------------------------------------------------------------
// Touches

// The primary touch is a copy of its finger's slot, with a press of its own:
// one finger lifting and the next touching in one sample is an up and a down.
static void follow(tide_touchscreen *s, const tide_touch *slot)
{
    if (slot->id == 0 || s->primaryTouch.id != slot->id) return;
    const tide_button press = s->primaryTouch.press;
    s->primaryTouch = *slot;
    s->primaryTouch.press = press;
    tide_button_set(&s->primaryTouch.press, slot->press.held);
    s->used = 1;
}

void tide_touches_poll(tide_touchscreen *s)
{
    s->used = 0;
    tide_touch *primary = &s->primaryTouch;
    primary->poll_delta = tide_f2(0.0f, 0.0f);
    primary->began = primary->lifted = 0;
    for (int i = 0; i < TIDE_TOUCHES; i++) {
        tide_touch *t = &s->touches.at[i];
        t->poll_delta = tide_f2(0.0f, 0.0f);
        t->began = 0;
        if (t->lifted) {
            t->lifted = 0;
            tide_button_set(&t->press, false);
            follow(s, t);
        }
    }
}

// The slot of the finger the platform calls `source`, while it touches.
static tide_touch *finger(tide_touchscreen *s, const uint32_t source)
{
    for (int i = 0; i < TIDE_TOUCHES; i++) {
        tide_touch *t = &s->touches.at[i];
        if (t->press.held && !t->lifted && t->source == source) return t;
    }
    return NULL;
}

// A slot no finger has, and no sample has yet to see lift.
static tide_touch *free_slot(tide_touchscreen *s)
{
    for (int i = 0; i < TIDE_TOUCHES; i++) {
        tide_touch *t = &s->touches.at[i];
        if (!t->press.held && !t->press.pressed) return t;
    }
    return NULL;
}

static void move_to(tide_touch *t, const tide_float2 position)
{
    const tide_float2 by = tide_f2(position.x - t->position.x, position.y - t->position.y);
    t->delta = tide_f2(t->delta.x + by.x, t->delta.y + by.y);
    t->poll_delta = tide_f2(t->poll_delta.x + by.x, t->poll_delta.y + by.y);
    t->position = position;
}

void tide_touch_event(tide_touchscreen *s, const tide_touch_phase phase, const uint32_t source, const tide_float2 position)
{
    tide_touch *t = finger(s, source);
    if (phase == TIDE_TOUCH_BEGAN && !t) {
        t = free_slot(s);
        if (!t) return;
        *t = (tide_touch){0};
        t->source = source;
        s->last_id = s->last_id == INT32_MAX ? 1 : s->last_id + 1;
        t->id = s->last_id;
        t->position = t->startPosition = position;
        t->began = 1;
        tide_button_set(&t->press, true);
        if (!s->primaryTouch.press.held) { // It's the primary touch from now on
            const tide_button press = s->primaryTouch.press;
            s->primaryTouch = *t;
            s->primaryTouch.press = press;
            tide_button_set(&s->primaryTouch.press, true);
            s->used = 1;
        }
        return;
    }
    if (!t) return;
    if (phase != TIDE_TOUCH_CANCELED) move_to(t, position);
    // A finger that touched this poll lifts at the next, so it reads as held
    // for one: a tap between two frames isn't lost.
    if (phase == TIDE_TOUCH_ENDED || phase == TIDE_TOUCH_CANCELED) {
        if (t->began) t->lifted = 1;
        else tide_button_set(&t->press, false);
    }
    follow(s, t);
}

// ---------------------------------------------------------------------------
// The pointer

void tide_pointer_poll(tide_devices *d, const bool mouse_used)
{
    tide_pointer *p = &d->pointer;
    const tide_touch *primary = &d->touchscreen.primaryTouch;
    if (d->touchscreen.used) p->touch = true;
    else if (mouse_used) p->touch = false;
    const tide_float2 position = p->touch ? primary->position : d->mouse.position;
    const tide_float2 moved = p->touch ? primary->poll_delta : d->mouse.poll_delta;
    p->position = position;
    p->poll_delta = moved;
    p->delta = tide_f2(p->delta.x + moved.x, p->delta.y + moved.y);
    tide_button_set(&p->press, p->touch ? primary->press.held : d->mouse.left.held);
}
