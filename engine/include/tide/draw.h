#pragma once

#include <stdint.h>

#include "tide/color.h"
#include "tide/math.h"

// Immediate-mode drawing: Tide's Draw API records commands into a draw
// list, and a renderer plays them back (tide_platform_draw, on raylib). The list
// knows nothing about any renderer, so replacing one doesn't touch game code.
//
// Temporary implementation written by Claude; the project owner takes it over
// later. Generated code calls the tide_draw_* functions below (and
// tide/mesh.h's), and so does a game's C that a view hands its list (a
// `DrawList` parameter).
//
// Positions and sizes are in world units, with y up. The camera decides where
// they land on screen. Commands draw in the order they were recorded, on a
// frame that starts black. After a TIDE_DRAW_SCREEN command, they're in the
// screen's pixels instead, from the top left with y down, as the GUI's are.

typedef enum tide_draw_kind {
    TIDE_DRAW_CLEAR,       // color
    TIDE_DRAW_CAMERA,      // a = center, b.x = size
    TIDE_DRAW_CIRCLE,      // a = center, b.x = radius, color
    TIDE_DRAW_WIRE_CIRCLE, // a = center, b.x = radius, color
    TIDE_DRAW_RECT,        // a = center, b = size, color
    TIDE_DRAW_WIRE_RECT,   // a = center, b = size, color
    TIDE_DRAW_LINE,        // a = from, b = to, color
    TIDE_DRAW_TEXT,        // a = top left corner, b.x = height, color, text
    TIDE_DRAW_SCREEN,      // The commands after it are in the screen's pixels
    TIDE_DRAW_MESH,        // mesh: its triangles, and the texture they sample
    TIDE_DRAW_CLIP,        // a = the corner where x and y are lowest, b = size
    TIDE_DRAW_NO_CLIP,
} tide_draw_kind;

typedef struct tide_draw_command {
    uint32_t kind; // tide_draw_kind
    uint32_t text; // TIDE_DRAW_TEXT: offset of the text in tide_draw_list.text
    union {
        struct {
            tide_float2 a;
            tide_float2 b;
        };
        struct {              // TIDE_DRAW_MESH
            uint32_t first;   // Its indices in tide_draw_list.indices, three a triangle...
            uint32_t count;   // ...and how many
            uint32_t texture; // 1 + its texture's place in tide_draw_list.textures, or 0 for none
            uint32_t filter;  // tide_filter
        } mesh;
    };
    tide_color color;
} tide_draw_command;

// A rectangle: its corner and its size. The GUI's are from the top left.
typedef struct tide_rect {
    float x, y, width, height;
} tide_rect;

// A mesh's corner: where it is, where in the texture it samples ((0, 0) is the
// first pixel's outer corner, (1, 1) the last one's), and its color, which
// multiplies the texture's. Tide's `Vertex` is the same.
typedef struct tide_vertex {
    tide_float2 position;
    tide_float2 uv;
    tide_color color;
} tide_vertex;

// How a mesh samples its texture between pixels. Tide's `Filter` has the same
// values, which are Unity's FilterMode's.
typedef enum tide_filter {
    TIDE_FILTER_POINT,    // The nearest pixel: hard edges
    TIDE_FILTER_BILINEAR, // Blended with its neighbors
} tide_filter;

// Pixels to draw with, where the game keeps them. Nothing holds a texture on
// the GPU: a mesh names its pixels, the list copies them when it hasn't got
// them as they are, and the renderer uploads from that copy. So C can keep
// its pixels wherever it likes, and say when they changed.
typedef struct tide_texture {
    const void *pixels; // width * height of them: red, green, blue, alpha, a byte each, rows from the first
    int32_t width, height;
    uint64_t version; // Any number that's another whenever these pixels are (see tide_texture_version)
} tide_texture;

// A texture the list has, for the commands that draw with it: a copy of the
// pixels as they were when a mesh last named them. One no command of a frame
// named is let go when the next frame starts.
typedef struct tide_draw_texture {
    uint64_t id;      // Whose pixels: C's address of them, or a grid's place in its world
    uint64_t version; // ...as they were then
    uint32_t space;   // What `id` is: TIDE_PIXELS_*
    int32_t width, height;
    uint32_t frame;    // The frame that last drew with it
    uint32_t epoch;    // The list's when it was copied (see tide_draw_forget)
    uint32_t capacity; // Bytes `pixels` has room for
    uint8_t *pixels;   // width * height * 4 bytes, rows from the first
} tide_draw_texture;

enum { TIDE_PIXELS_MEMORY, TIDE_PIXELS_GRID };

// One frame's commands. Not simulation state: it lives outside the world. A
// zeroed list is ready to use, and it grows as it needs, with no limit but
// memory: running out ends the program, as it does for worlds.
typedef struct tide_draw_list {
    tide_draw_command *commands; // `count` of them, with room for `capacity`
    uint32_t count;
    uint32_t capacity;
    char *text; // The commands' text, `text_used` bytes, with room for `text_capacity`
    uint32_t text_used;
    uint32_t text_capacity;
    tide_vertex *vertices; // The meshes' corners
    uint32_t vertex_count;
    uint32_t vertex_capacity;
    uint32_t *indices; // ...and their triangles, each index a place in `vertices`
    uint32_t index_count;
    uint32_t index_capacity;
    tide_draw_texture *textures; // What the meshes draw with, kept from frame to frame while they do
    uint32_t texture_count;
    uint32_t texture_capacity;
    uint32_t frame;   // Counts tide_draw_reset
    uint32_t clipped; // Whether a clip is on for the commands to come
    uint32_t epoch;   // Counts tide_draw_forget
} tide_draw_list;

// Empties the list for a new frame. It keeps its memory for the next, and the
// textures the frame drew with.
void tide_draw_reset(tide_draw_list *d);

// Lets the list's memory go: it's empty, and ready to use again.
void tide_draw_free(tide_draw_list *d);

// Lets the list's textures go, and has it copy again whatever the commands to
// come draw with, and renderers upload it again: for a host whose game was
// swapped for another build (hot reloading), whose C names other pixels than
// the one before, maybe at the same address with the same version.
void tide_draw_forget(tide_draw_list *d);

// Fills the whole screen, or what the clip leaves of it.
void tide_draw_clear(tide_draw_list *d, tide_color color);

// Sets the camera for the commands after it: `center` is the world position
// at the middle of the screen, and `size` is half the visible height, like
// Unity's orthographic size. Each frame starts with the camera at the origin
// and 1 world unit per pixel.
void tide_draw_camera(tide_draw_list *d, tide_float2 center, float size);

// The commands after it are in the screen's pixels: from the top left, with y
// down, until the next camera.
void tide_draw_screen(tide_draw_list *d);

void tide_draw_circle(tide_draw_list *d, tide_float2 center, float radius, tide_color color);
void tide_draw_wire_circle(tide_draw_list *d, tide_float2 center, float radius, tide_color color);
void tide_draw_rect(tide_draw_list *d, tide_float2 center, tide_float2 size, tide_color color);
void tide_draw_wire_rect(tide_draw_list *d, tide_float2 center, tide_float2 size, tide_color color);
void tide_draw_line(tide_draw_list *d, tide_float2 from, tide_float2 to, tide_color color);

// `position` is the text's top left corner and `size` its height. The text is
// copied into the list.
void tide_draw_text(tide_draw_list *d, const char *text, tide_float2 position, float size, tide_color color);

// Triangles: three of `indices` each, places in `vertices`, both copied into
// the list. Each pixel is its corners' colors blended across the triangle,
// times `texture`'s at their uvs (NULL for none: the colors alone), sampled
// with `filter`. Either side of a triangle draws. A triangle with an index
// past the vertices is left out.
void tide_draw_mesh(tide_draw_list *d, const tide_vertex *vertices, uint32_t vertex_count, const uint32_t *indices,
                    uint32_t index_count, const tide_texture *texture, tide_filter filter);

// The same in two steps, for vertices several meshes share, like a GUI
// library's, whose every batch has a texture and a clip of its own:
// tide_draw_vertices copies them into the list and returns where the first
// is, and tide_draw_triangles draws triangles of the list's vertices from
// `base` on.
uint32_t tide_draw_vertices(tide_draw_list *d, const tide_vertex *vertices, uint32_t count);
void tide_draw_triangles(tide_draw_list *d, uint32_t base, const uint32_t *indices, uint32_t count,
                         const tide_texture *texture, tide_filter filter);

// A version for pixels that change rarely: a hash of them. C that knows when
// its pixels change can count instead.
uint64_t tide_texture_version(const void *pixels, int32_t width, int32_t height);

// Only what's inside `rect` draws, for the commands after it, clears too,
// until another clip or tide_draw_no_clip. The rect is in the units the
// commands are in (the world's under a camera, pixels on the screen), from
// (x, y) to (x + width, y + height), and stays where it is on the screen when
// the camera changes. Each frame starts with none.
void tide_draw_clip(tide_draw_list *d, tide_rect rect);
void tide_draw_no_clip(tide_draw_list *d);

// Adds `from`'s commands to the end of `d`, such as the GUI's over the world.
void tide_draw_append(tide_draw_list *d, const tide_draw_list *from);

// The texture `id` of `space` (TIDE_PIXELS_*) at `version`, for a mesh
// command: 1 + its place in the list's textures, or 0 when it has no size.
// When the list hasn't got its pixels as they are (it's new, or its version
// or size changed), *pixels is where to write them: width * height * 4 bytes.
// Otherwise it's NULL.
uint32_t tide_draw_texture_slot(tide_draw_list *d, uint32_t space, uint64_t id, uint64_t version, int32_t width,
                                int32_t height, uint8_t **pixels);

// tide_draw_triangles, with what tide_draw_texture_slot gave as the texture.
void tide_draw_triangles_with(tide_draw_list *d, uint32_t base, const uint32_t *indices, uint32_t count,
                              uint32_t texture, tide_filter filter);
