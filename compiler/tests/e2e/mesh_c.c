// The C side of mesh.tide: functions a view hands its draw list, which draw
// into it with tide/draw.h, as a GUI library's renderer would.

#include "tide/draw.h"

// A white pixel beside a clear one. C keeps its own pixels, and says when
// they changed.
static const uint8_t pixels[2 * 1 * 4] = {255, 255, 255, 255, 0, 0, 0, 0};

// `blades` triangles around the origin, which share their vertices, each with
// a clip of its own.
void DrawFan(tide_draw_list *list, const int blades, const tide_color color)
{
    tide_vertex vertices[1 + 8] = {{{0.0f, 0.0f}, {0.5f, 0.5f}, color}};
    const int corners = blades < 7 ? blades + 1 : 8;
    for (int i = 0; i < corners; i++) vertices[1 + i] = (tide_vertex){{(float)i, 1.0f}, {(float)i, 0.0f}, color};
    const uint32_t base = tide_draw_vertices(list, vertices, (uint32_t)(1 + corners));
    const tide_texture texture = {pixels, 2, 1, 1};
    for (int i = 0; i + 1 < corners; i++) {
        const uint32_t triangle[3] = {0, (uint32_t)(1 + i), (uint32_t)(2 + i)};
        tide_draw_clip(list, (tide_rect){(float)i, 0.0f, 1.0f, 1.0f});
        tide_draw_triangles(list, base, triangle, 3, &texture, TIDE_FILTER_POINT);
    }
    tide_draw_no_clip(list);
}

int commands_so_far(tide_draw_list *list)
{
    return (int)list->count;
}
