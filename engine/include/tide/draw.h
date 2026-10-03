#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "tide/color.h"
#include "tide/math.h"

// Immediate-mode drawing: Tide's Draw API records commands into a draw
// list, and a renderer plays them back (tide_platform_draw, on OpenGL). The list
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
//
// 3D meshes (TIDE_DRAW_MESH_3D) go through a camera of their own
// (TIDE_DRAW_CAMERA_3D), and are the only commands that test and write depth:
// they hide each other by how far they are, whatever their order, and
// everything else draws over what's there, in order. A clear clears depth
// too. Each frame starts with a 3D camera at the origin, looking along +z
// with a field of view of 60 degrees.

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
    TIDE_DRAW_CAMERA_3D, // camera: the 3D camera, for the 3D meshes after it
    TIDE_DRAW_MESH_3D,   // mesh3: a run of instances of a 3D mesh
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
        struct {             // TIDE_DRAW_CAMERA_3D
            uint32_t matrix; // Its place in tide_draw_list.matrices: from the world to clip space, as OpenGL's
            uint32_t fit;    // 1: it's for a square screen, and renderers divide x by the screen's width over its height
        } camera;
        struct {              // TIDE_DRAW_MESH_3D
            uint32_t mesh;    // 1 + its place in tide_draw_list.meshes, or 0 for none (see tide_draw_forget)
            uint32_t first;   // Its instances: transforms in tide_draw_list.matrices, from the mesh to the world...
            uint32_t count;   // ...and how many
            uint32_t texture; // As a mesh's
            uint32_t filter;
        } mesh3;
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

// A 3D mesh's corner, as a mesh's but in 3D. Tide's `Vertex3` is the same.
typedef struct tide_vertex3 {
    tide_float3 position;
    tide_float2 uv;
    tide_color color;
} tide_vertex3;

// Triangles in 3D, where the game keeps them: three of `indices` each, places
// in `vertices`. As with a texture, nothing holds them on the GPU: the list
// copies them when it hasn't got them as they are, and the renderer uploads
// from that copy, so a mesh that doesn't change costs neither again.
typedef struct tide_mesh {
    const tide_vertex3 *vertices;
    uint32_t vertex_count;
    const uint32_t *indices;
    uint32_t index_count;
    uint64_t version; // Any number that's another whenever these are (see tide_mesh_version)
} tide_mesh;

// A 3D mesh the list has, for the commands that draw it: a copy of its
// vertices and triangles at a version. It never changes while it has its
// place: another version of the mesh takes a place of its own, so what a
// frame drew before it changed still shows. One no command of a frame named
// is let go when the next frame starts, and one that frames go on drawing
// keeps its place, so renderers can keep theirs by its place.
typedef struct tide_draw_mesh_data {
    uint64_t id, other; // Whose: C's addresses of its vertices and indices, or Tide's lists
    uint64_t version;
    uint32_t space;     // What the ids are: TIDE_MESH_*, or TIDE_MESH_NONE for a free place
    uint32_t frame;     // The frame that last drew it
    uint32_t epoch;     // The list's when it was copied (see tide_draw_forget)
    uint32_t next_free; // A free place's: 1 + the next free one, or 0
    tide_vertex3 *vertices;
    uint32_t vertex_count;
    uint32_t vertex_capacity;
    uint32_t *indices; // Three a triangle, each a place in `vertices`: what's past them is left out
    uint32_t index_count;
    uint32_t index_capacity;
} tide_draw_mesh_data;

enum { TIDE_MESH_NONE, TIDE_MESH_MEMORY, TIDE_MESH_LISTS };

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
    tide_float4x4 *matrices; // The 3D cameras' and the instances' of 3D meshes
    uint32_t matrix_count;
    uint32_t matrix_capacity;
    tide_draw_mesh_data *meshes; // What 3D meshes draw, kept from frame to frame while they do
    uint32_t mesh_count;         // Places, free ones too
    uint32_t mesh_capacity;
    uint32_t mesh_free;           // 1 + the first free place, or 0
    uint32_t *mesh_index;         // The meshes by whose they are: 1 + a place, or 0 for none
    uint32_t mesh_index_capacity; // A power of two, or 0
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

// ---------------------------------------------------------------------------
// 3D

// Sets the 3D camera for the 3D meshes after it: where it is, which way it
// looks (along its rotation's +z, its up its +y), and how much it sees up
// and down, in degrees, like Unity's Camera.fieldOfView. It sees from 0.3
// units in front of it on, however far, and as much across as the screen's
// shape gives. Other commands keep the camera they have.
void tide_draw_camera_3d(tide_draw_list *d, tide_float3 position, tide_quaternion rotation, float field_of_view);

// What tide_draw_camera_3d records: from the world to clip space, for a
// square screen (TIDE_DRAW_CAMERA_3D's `fit`).
tide_float4x4 tide_draw_camera_3d_matrix(tide_float3 position, tide_quaternion rotation, float field_of_view);

// The same with any projection: `transform` places the camera in the world
// (it looks along its +z), and `projection` takes what it sees, looking down
// -z as OpenGL's and Unity's views do, to clip space (see
// tide_perspectivefov_f4x4 and tide_ortho_f4x4), the screen's shape included.
void tide_draw_camera_matrices(tide_draw_list *d, tide_float4x4 transform, tide_float4x4 projection);

// A 3D mesh, placed in the world by `transform`: each pixel is its corners'
// colors blended across the triangle, times `texture`'s at their uvs (NULL
// for none), sampled with `filter`. Either side of a triangle draws.
// Instances of one mesh drawn one after another, with one texture and filter,
// are one command, which renderers draw at once.
void tide_draw_mesh_3d(tide_draw_list *d, const tide_mesh *mesh, tide_float4x4 transform, const tide_texture *texture,
                       tide_filter filter);

// A version for vertices and indices that change rarely: a hash of them.
uint64_t tide_mesh_version(const tide_mesh *mesh);

// The same in steps, for meshes kept elsewhere, like Tide's lists: the mesh
// `id` and `other` of `space` (TIDE_MESH_*) at `version`, for a 3D mesh
// command: 1 + its place in the list's meshes. When the list hasn't got it
// at that version, *fill is true, and tide_draw_mesh_fill gives it its
// vertices and indices before anything draws it.
uint32_t tide_draw_mesh_slot(tide_draw_list *d, uint32_t space, uint64_t id, uint64_t other, uint64_t version,
                             bool *fill);
void tide_draw_mesh_fill(tide_draw_list *d, uint32_t mesh, const tide_vertex3 *vertices, uint32_t vertex_count,
                         const uint32_t *indices, uint32_t index_count);

// An instance of what tide_draw_mesh_slot gave, with what
// tide_draw_texture_slot gave as the texture.
void tide_draw_instance(tide_draw_list *d, uint32_t mesh, tide_float4x4 transform, uint32_t texture, tide_filter filter);
