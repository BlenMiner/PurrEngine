#include "purr/draw.h"

#include <string.h>

void purr_draw_reset(purr_draw_list *d)
{
    d->count = 0;
    d->text_used = 0;
    d->dropped = 0;
}

// A full list drops commands instead of failing: drawing never stops the game.
static purr_draw_command *push(purr_draw_list *d, const purr_draw_kind kind, const purr_float2 a, const purr_float2 b,
                               const purr_color color)
{
    if (d->count == PURR_DRAW_MAX_COMMANDS) {
        d->dropped++;
        return NULL;
    }
    purr_draw_command *c = &d->commands[d->count++];
    *c = (purr_draw_command){(uint32_t)kind, 0, a, b, color};
    return c;
}

void purr_draw_clear(purr_draw_list *d, const purr_color color)
{
    push(d, PURR_DRAW_CLEAR, purr_f2(0.0f, 0.0f), purr_f2(0.0f, 0.0f), color);
}

void purr_draw_camera(purr_draw_list *d, const purr_float2 center, const float size)
{
    push(d, PURR_DRAW_CAMERA, center, purr_f2(size, 0.0f), PURR_COLOR_CLEAR);
}

void purr_draw_circle(purr_draw_list *d, const purr_float2 center, const float radius, const purr_color color)
{
    push(d, PURR_DRAW_CIRCLE, center, purr_f2(radius, 0.0f), color);
}

void purr_draw_wire_circle(purr_draw_list *d, const purr_float2 center, const float radius, const purr_color color)
{
    push(d, PURR_DRAW_WIRE_CIRCLE, center, purr_f2(radius, 0.0f), color);
}

void purr_draw_rect(purr_draw_list *d, const purr_float2 center, const purr_float2 size, const purr_color color)
{
    push(d, PURR_DRAW_RECT, center, size, color);
}

void purr_draw_wire_rect(purr_draw_list *d, const purr_float2 center, const purr_float2 size, const purr_color color)
{
    push(d, PURR_DRAW_WIRE_RECT, center, size, color);
}

void purr_draw_line(purr_draw_list *d, const purr_float2 from, const purr_float2 to, const purr_color color)
{
    push(d, PURR_DRAW_LINE, from, to, color);
}

void purr_draw_text(purr_draw_list *d, const char *text, const purr_float2 position, const float size,
                    const purr_color color)
{
    const size_t bytes = strlen(text) + 1;
    if (bytes > PURR_DRAW_TEXT_BYTES - d->text_used) {
        d->dropped++;
        return;
    }
    purr_draw_command *c = push(d, PURR_DRAW_TEXT, position, purr_f2(size, 0.0f), color);
    if (!c) return;
    c->text = d->text_used;
    memcpy(d->text + d->text_used, text, bytes);
    d->text_used += (uint32_t)bytes;
}
