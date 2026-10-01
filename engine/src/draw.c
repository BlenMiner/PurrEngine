#include "tide/draw.h"

#include <string.h>

void tide_draw_reset(tide_draw_list *d)
{
    d->count = 0;
    d->text_used = 0;
    d->dropped = 0;
}

// A full list drops commands instead of failing: drawing never stops the game.
static tide_draw_command *push(tide_draw_list *d, const tide_draw_kind kind, const tide_float2 a, const tide_float2 b,
                               const tide_color color)
{
    if (d->count == TIDE_DRAW_MAX_COMMANDS) {
        d->dropped++;
        return NULL;
    }
    tide_draw_command *c = &d->commands[d->count++];
    *c = (tide_draw_command){(uint32_t)kind, 0, a, b, color};
    return c;
}

void tide_draw_clear(tide_draw_list *d, const tide_color color)
{
    push(d, TIDE_DRAW_CLEAR, tide_f2(0.0f, 0.0f), tide_f2(0.0f, 0.0f), color);
}

void tide_draw_camera(tide_draw_list *d, const tide_float2 center, const float size)
{
    push(d, TIDE_DRAW_CAMERA, center, tide_f2(size, 0.0f), TIDE_COLOR_CLEAR);
}

void tide_draw_circle(tide_draw_list *d, const tide_float2 center, const float radius, const tide_color color)
{
    push(d, TIDE_DRAW_CIRCLE, center, tide_f2(radius, 0.0f), color);
}

void tide_draw_wire_circle(tide_draw_list *d, const tide_float2 center, const float radius, const tide_color color)
{
    push(d, TIDE_DRAW_WIRE_CIRCLE, center, tide_f2(radius, 0.0f), color);
}

void tide_draw_rect(tide_draw_list *d, const tide_float2 center, const tide_float2 size, const tide_color color)
{
    push(d, TIDE_DRAW_RECT, center, size, color);
}

void tide_draw_wire_rect(tide_draw_list *d, const tide_float2 center, const tide_float2 size, const tide_color color)
{
    push(d, TIDE_DRAW_WIRE_RECT, center, size, color);
}

void tide_draw_line(tide_draw_list *d, const tide_float2 from, const tide_float2 to, const tide_color color)
{
    push(d, TIDE_DRAW_LINE, from, to, color);
}

void tide_draw_gui(tide_draw_list *d)
{
    push(d, TIDE_DRAW_GUI, tide_f2(0.0f, 0.0f), tide_f2(0.0f, 0.0f), TIDE_COLOR_CLEAR);
}

void tide_draw_append(tide_draw_list *d, const tide_draw_list *from)
{
    for (uint32_t i = 0; i < from->count; i++) {
        const tide_draw_command *c = &from->commands[i];
        if (c->kind == TIDE_DRAW_TEXT) {
            tide_draw_text(d, from->text + c->text, c->a, c->b.x, c->color);
        } else {
            tide_draw_command *copy = push(d, (tide_draw_kind)c->kind, c->a, c->b, c->color);
            if (copy) copy->text = 0;
        }
    }
    d->dropped += from->dropped;
}

void tide_draw_text(tide_draw_list *d, const char *text, const tide_float2 position, const float size,
                    const tide_color color)
{
    const size_t bytes = strlen(text) + 1;
    if (bytes > TIDE_DRAW_TEXT_BYTES - d->text_used) {
        d->dropped++;
        return;
    }
    tide_draw_command *c = push(d, TIDE_DRAW_TEXT, position, tide_f2(size, 0.0f), color);
    if (!c) return;
    c->text = d->text_used;
    memcpy(d->text + d->text_used, text, bytes);
    d->text_used += (uint32_t)bytes;
}
