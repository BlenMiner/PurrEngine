#include "tide/platform.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tide/page.h"
#include "tide/time.h"
#include "font.h"
#include "gpu.h"
#include "shaders.h"
#include "window.h"

#ifdef __wasm__
#include "tide_web.h" // The page's frame loop (platform/web/tide.js)
#endif

#define COUNT_OF(array) (sizeof(array) / sizeof((array)[0]))

// Buttons are found by their offset in the device struct, in the order
// window.h has them, which is devices.h's.
#define BUTTON_AT(device, offset) ((tide_button *)((char *)(device) + (offset)))

#define KEY_OFFSET(name) offsetof(tide_keyboard, name),
#define MOUSE_OFFSET(name) offsetof(tide_mouse, name),
#define PAD_OFFSET(name) offsetof(tide_gamepad, name),
#define DPAD_OFFSET(name) offsetof(tide_gamepad, dpad.name),

static const size_t key_offsets[] = {TIDE_KEYBOARD_KEYS(KEY_OFFSET)};
static const size_t mouse_offsets[] = {TIDE_MOUSE_BUTTONS(MOUSE_OFFSET)};
static const size_t pad_offsets[] = {TIDE_GAMEPAD_BUTTONS(PAD_OFFSET) TIDE_DPAD_BUTTONS(DPAD_OFFSET)};

_Static_assert(COUNT_OF(key_offsets) == TIDE_KEY_COUNT, "a tide_key for every key");
_Static_assert(COUNT_OF(pad_offsets) == TIDE_PAD_COUNT, "a tide_pad_button for every gamepad button");

#ifdef __wasm__
static bool web_timer_frames; // Frames on timers instead of animation frames
#endif

static tide_frame_fn run_frame;
static void *run_user;
static uint64_t opened;    // When the window opened (tide_time_now_ns)
static double frame_start; // When the frame running, or the last one, started (now())
static double frame_due;   // When the next one comes while nobody sees the window
static bool due_asked;     // ...as the frame function asked (tide_platform_next_frame)

// The window's size as of the frame's start, in logical pixels, and in the
// pixels it renders.
static int screen_width = 1, screen_height = 1;
static int pixel_width = 1, pixel_height = 1;

// What draws in the window (see gpu.h), which it says once it's open.
static const tide_gpu *gpu;

// The last frames' lengths, for tide_platform_fps.
static float frame_seconds[32];
static unsigned frames_counted;

// Seconds since the window opened.
static double now(void)
{
    return (double)(tide_time_now_ns() - opened) * 1e-9;
}

static void follow_window(void)
{
    tide_window_size(&screen_width, &screen_height);
    tide_window_pixel_size(&pixel_width, &pixel_height);
}

void tide_platform_open(const tide_window_desc *desc)
{
    if (!tide_window_open(desc)) exit(1); // It said why
    gpu = tide_window_gpu();
    opened = tide_time_now_ns();
    follow_window();
#ifdef __wasm__
    web_timer_frames = desc->hidden;
#endif
}

_Noreturn static void finish(const int code)
{
#ifdef __wasm__
    tide_web_stop();
#endif
    tide_window_close();
    exit(code);
}

void tide_platform_next_frame(const double seconds)
{
    const double due = frame_start + (seconds > 0.0 ? seconds : 0.0);
    if (!due_asked || due < frame_due) frame_due = due;
    due_asked = true;
}

static int step(void)
{
    const double start = now();
    const float seconds = (float)(start - frame_start);
    frame_start = start;
    frame_due = start + 1.0 / 60.0;
    due_asked = false;
    frame_seconds[frames_counted++ % COUNT_OF(frame_seconds)] = seconds;
    follow_window();
    const int code = run_frame(run_user, seconds);
    tide_window_present(); // Also takes the system's input
#ifndef __wasm__
    // A window nobody sees (minimized, or an app in the background) has no
    // display's pace to wait for: the next frame comes when the frame function
    // asked, and draws nothing (see tide_platform_draw). It waits on the
    // system, so a window restored or closed meanwhile goes on at once.
    while (tide_window_unseen() && !tide_window_should_close()) {
        const double left = frame_due - now();
        if (left <= 0.0) break;
        tide_window_wait(left);
    }
#endif
    return code;
}

#ifdef __wasm__
// The page calls it for every frame, once tide_platform_run starts the loop:
// seconds until the next one, which the page waits while it's hidden.
__attribute__((export_name("tide_web_frame"))) double tide_web_frame(void)
{
    const int code = step();
    if (code != TIDE_KEEP_RUNNING) finish(code);
    return frame_due - now();
}
#endif

void tide_platform_run(const tide_frame_fn frame, void *user)
{
    run_frame = frame;
    run_user = user;
    frame_start = now();
#ifdef __wasm__
    // Frames on requestAnimationFrame, or as fast as timers allow when hidden.
    // Doesn't return: it unwinds main's stack back to the browser.
    tide_web_run(web_timer_frames);
#else
    for (;;) {
        if (tide_window_should_close()) finish(0);
        const int code = step();
        if (code != TIDE_KEEP_RUNNING) finish(code);
    }
#endif
}

int tide_platform_fps(void)
{
    const unsigned count = frames_counted < COUNT_OF(frame_seconds) ? frames_counted : (unsigned)COUNT_OF(frame_seconds);
    float total = 0.0f;
    for (unsigned i = 0; i < count; i++) total += frame_seconds[i];
    return total > 0.0f ? (int)((float)count / total + 0.5f) : 0;
}

// ---------------------------------------------------------------------------
// Devices

static void poll_keyboard(tide_keyboard *k)
{
    for (size_t i = 0; i < COUNT_OF(key_offsets); i++) {
        tide_button_set(BUTTON_AT(k, key_offsets[i]), tide_window_key_held((tide_key)i));
    }
}

// The mouse; true if it was used: it moved or scrolled, or a button went down
// or up.
static bool poll_mouse(tide_mouse *m)
{
    static float last_x, last_y;
    static bool polled;
    tide_window_mouse mouse;
    tide_window_mouse_state(&mouse);
    const float dx = polled ? mouse.x - last_x : 0.0f, dy = polled ? mouse.y - last_y : 0.0f;
    last_x = mouse.x, last_y = mouse.y;
    polled = true;
    // The window counts from the top left, y down; Devices follows Unity: from
    // the bottom left, y up.
    m->position = tide_f2(mouse.x, (float)screen_height - mouse.y);
    // Delta and scroll add up until the input is sampled.
    m->delta = tide_f2(m->delta.x + dx, m->delta.y - dy);
    m->scroll = tide_f2(m->scroll.x + mouse.wheel_x, m->scroll.y + mouse.wheel_y);
    m->poll_delta = tide_f2(dx, -dy);
    m->poll_scroll = tide_f2(mouse.wheel_x, mouse.wheel_y);
    bool used = dx != 0.0f || dy != 0.0f || mouse.wheel_x != 0.0f || mouse.wheel_y != 0.0f;
    for (size_t i = 0; i < COUNT_OF(mouse_offsets); i++) {
        tide_button *b = BUTTON_AT(m, mouse_offsets[i]);
        const bool held = (mouse.buttons >> i & 1u) != 0;
        used |= held != b->held;
        tide_button_set(b, held);
    }
    return used;
}

// Fingers on a touchscreen: the page's on the web, the window's on Windows,
// the activity's on Android. Other desktops have none yet.
static void poll_touches(tide_touchscreen *s)
{
    tide_touches_poll(s);
    s->connected = tide_window_touchscreen();
    tide_touch_report r;
    while (tide_window_take_touch(&r)) {
        tide_touch_event(s, r.phase, r.source, tide_f2(r.x, (float)screen_height - r.y));
    }
}

static void poll_gamepad(tide_gamepad *g)
{
    tide_window_gamepad pad;
    tide_window_gamepad_state(&pad);
    g->connected = pad.connected;
    if (pad.connected) {
        g->leftStick = tide_f2(pad.left_x, -pad.left_y);
        g->rightStick = tide_f2(pad.right_x, -pad.right_y);
        g->leftTrigger = pad.left_trigger;
        g->rightTrigger = pad.right_trigger;
    } else {
        g->leftStick = g->rightStick = tide_f2(0.0f, 0.0f);
        g->leftTrigger = g->rightTrigger = 0.0f;
    }
    for (size_t i = 0; i < COUNT_OF(pad_offsets); i++) {
        tide_button_set(BUTTON_AT(g, pad_offsets[i]), pad.connected && (pad.buttons >> i & 1u) != 0);
    }
}

// What Ctrl+V (Cmd+V on macOS) pasted, as characters typed: the next to type
// is pasted[pasted_at]. On the web, the page does it (tide.js).
static uint32_t pasted[1024];
static uint32_t pasted_count, pasted_at;

static void paste(void)
{
    const char *text = tide_window_take_paste();
    if (!text) return;
    for (uint32_t c; pasted_count < COUNT_OF(pasted) && (c = tide_utf8_next(&text)) != 0;) {
        if (c >= 32 && c != 127) pasted[pasted_count++] = c; // Not newlines or tabs
    }
}

// Characters typed since the last poll, which follow the keyboard layout. A
// poll takes as many as it holds, and leaves the rest for the next.
static void poll_text(tide_typed *text)
{
    text->count = 0;
    paste();
    while (pasted_at < pasted_count && text->count < TIDE_TEXT_MAX) text->chars[text->count++] = pasted[pasted_at++];
    if (pasted_at == pasted_count) pasted_at = pasted_count = 0;
    while (text->count < TIDE_TEXT_MAX) {
        const uint32_t c = tide_window_take_char();
        if (c == 0) break;
        text->chars[text->count++] = c;
    }
}

void tide_platform_typing(const bool typing)
{
    static bool was;
    if (typing == was) return;
    was = typing;
    tide_window_typing(typing);
}

void tide_platform_copy(const char *text)
{
    tide_window_copy(text);
}

void tide_platform_poll(tide_devices *devices)
{
    poll_keyboard(&devices->keyboard);
    const bool mouse_used = poll_mouse(&devices->mouse);
    poll_gamepad(&devices->gamepad);
    poll_touches(&devices->touchscreen);
    tide_pointer_poll(devices, mouse_used);
    poll_text(&devices->keyboard.text);
}

// ---------------------------------------------------------------------------
// The renderer: a draw list's commands, on OpenGL.

// The camera maps world units (y up) to window pixels (y down). Each frame's
// list starts at the origin with 1 unit per pixel. After TIDE_DRAW_SCREEN, it
// maps the screen's pixels instead: from the top left, y down.
typedef struct camera {
    tide_float2 center;
    float scale; // Pixels per world unit
    bool gui;
} camera;

static camera last_camera = {{0.0f, 0.0f}, 1.0f, false};

static tide_float2 to_screen(const camera *cam, const tide_float2 p)
{
    if (cam->gui) return tide_f2(p.x * cam->scale, p.y * cam->scale);
    return tide_f2((float)screen_width * 0.5f + (p.x - cam->center.x) * cam->scale,
                   (float)screen_height * 0.5f - (p.y - cam->center.y) * cam->scale);
}

// A rect's top left corner on screen: y goes up in the world, down in the GUI.
static tide_float2 rect_corner(const camera *cam, const tide_draw_command *c)
{
    const float half_height = cam->gui ? -c->b.y * 0.5f : c->b.y * 0.5f;
    return to_screen(cam, tide_f2(c->a.x - c->b.x * 0.5f, c->a.y + half_height));
}

// A color as the GPU takes it: a byte each.
typedef struct rgba {
    uint8_t r, g, b, a;
} rgba;

static uint8_t color_channel(const float v)
{
    if (!(v > 0.0f)) return 0; // Also NaN
    if (v >= 1.0f) return 255;
    return (uint8_t)(v * 255.0f + 0.5f);
}

static rgba to_rgba(const tide_color c)
{
    return (rgba){color_channel(c.r), color_channel(c.g), color_channel(c.b), color_channel(c.a)};
}

// Shapes (rects, circles, their outlines and lines) go to the GPU as instances
// of one quad, each its shape in window pixels, so a run of them between
// other commands is one draw call, however many there are.

enum { SHAPE_RECT, SHAPE_WIRE_RECT, SHAPE_CIRCLE, SHAPE_WIRE_CIRCLE, SHAPE_LINE };

typedef struct shape {
    float a[2];       // Its middle, or a line's start
    float b[2];       // Half its size (a circle's radius, twice), or a line's end
    uint8_t color[4]; // RGBA
    float kind;       // SHAPE_*
} shape;

// A mesh's corner, in window pixels. Meshes go to the GPU as triangles, three
// of these each: a run of them with one texture is one draw call. Text goes
// the same way, two triangles a glyph, its uv in the font atlas's pixels.
typedef struct mesh_vertex {
    float position[2];
    float uv[2];
    uint8_t color[4]; // RGBA
} mesh_vertex;

// What a list comes to, in order: runs of shapes, clears, runs of text, runs
// of triangles, clips and runs of instances of 3D meshes.
enum { STEP_SHAPES, STEP_CLEAR, STEP_TEXT, STEP_MESH, STEP_CLIP, STEP_NO_CLIP, STEP_MESH_3D };

typedef struct draw_step {
    uint32_t kind;    // STEP_*
    uint32_t first;   // STEP_SHAPES: its shapes; STEP_TEXT and STEP_MESH: its vertices;
                      // STEP_MESH_3D: its instances' transforms in the list's matrices
    uint32_t count;
    uint32_t texture; // STEP_MESH and STEP_MESH_3D: 1 + its texture's place in the list's, or 0 for none
    uint32_t filter;  // ...and how it's sampled: tide_filter
    uint32_t mesh;    // STEP_MESH_3D: 1 + its mesh's place in the list's
    uint32_t camera;  // ...and 1 + its camera's matrix's place in the list's, or 0 for the first camera
    uint32_t fit;     // ...which is for a square screen (see TIDE_DRAW_CAMERA_3D)
    tide_float2 at;   // STEP_CLIP: its top left corner in window pixels
    tide_float2 to;   // ...and its bottom right one
    rgba color;       // STEP_CLEAR
} draw_step;

static shape *shapes;
static uint32_t shape_count, shape_capacity;
static mesh_vertex *mesh_vertices;
static uint32_t mesh_vertex_count, mesh_vertex_capacity;
static draw_step *steps;
static uint32_t step_count, step_capacity;

// What's being drawn into: the window, or tide_platform_read_pixels' own
// pixels, as many as the window's logical ones.
static bool offscreen;

// The target's pixels per logical pixel.
static tide_float2 pixel_scale(void)
{
    if (offscreen) return tide_f2(1.0f, 1.0f);
    return tide_f2((float)pixel_width / (float)screen_width, (float)pixel_height / (float)screen_height);
}

static draw_step *add_step(const uint32_t kind)
{
    if (step_count == step_capacity) {
        const uint32_t capacity = step_capacity ? step_capacity * 2u : 64u;
        steps = tide_realloc(steps, step_capacity * sizeof(draw_step), capacity * sizeof(draw_step));
        step_capacity = capacity;
    }
    draw_step *s = &steps[step_count++];
    *s = (draw_step){.kind = kind};
    return s;
}

static void add_shape(const int kind, const float ax, const float ay, const float bx, const float by, const rgba color)
{
    if (shape_count == shape_capacity) {
        const uint32_t capacity = shape_capacity ? shape_capacity * 2u : 1024u;
        if (capacity > UINT32_MAX / sizeof(shape)) tide_out_of_memory();
        shapes = tide_realloc(shapes, shape_capacity * sizeof(shape), capacity * sizeof(shape));
        shape_capacity = capacity;
    }
    if (step_count == 0 || steps[step_count - 1].kind != STEP_SHAPES) add_step(STEP_SHAPES)->first = shape_count;
    steps[step_count - 1].count++;
    shapes[shape_count++] = (shape){{ax, ay}, {bx, by}, {color.r, color.g, color.b, color.a}, (float)kind};
}

// Room for `count` more vertices after the ones there.
static mesh_vertex *mesh_room(const uint32_t count)
{
    if (count > mesh_vertex_capacity - mesh_vertex_count) {
        uint64_t capacity = mesh_vertex_capacity ? mesh_vertex_capacity : 4096u;
        while (capacity < (uint64_t)mesh_vertex_count + count) capacity *= 2u;
        if (capacity > UINT32_MAX / sizeof(mesh_vertex)) tide_out_of_memory();
        mesh_vertices = tide_realloc(mesh_vertices, mesh_vertex_capacity * sizeof(mesh_vertex),
                                     (size_t)capacity * sizeof(mesh_vertex));
        mesh_vertex_capacity = (uint32_t)capacity;
    }
    return mesh_vertices + mesh_vertex_count;
}

// A mesh command's triangles, their corners through the camera, after the
// ones before it when nothing came between them.
static void add_mesh(const tide_draw_list *list, const tide_draw_command *c, const camera *cam)
{
    const uint32_t count = c->mesh.count;
    mesh_vertex *out = mesh_room(count);
    draw_step *s = step_count ? &steps[step_count - 1] : NULL;
    if (!s || s->kind != STEP_MESH || s->texture != c->mesh.texture || s->filter != c->mesh.filter) {
        s = add_step(STEP_MESH);
        s->first = mesh_vertex_count;
        s->texture = c->mesh.texture;
        s->filter = c->mesh.filter;
    }
    s->count += count;
    const uint32_t *indices = list->indices + c->mesh.first;
    for (uint32_t i = 0; i < count; i++) {
        const tide_vertex *v = &list->vertices[indices[i]];
        const tide_float2 p = to_screen(cam, v->position);
        const rgba color = to_rgba(v->color);
        out[i] = (mesh_vertex){{p.x, p.y}, {v->uv.x, v->uv.y}, {color.r, color.g, color.b, color.a}};
    }
    mesh_vertex_count += count;
}

// Text of `size` with its top left corner at `at`, in window pixels: a quad a
// glyph, after the text before it when nothing came between them. It's drawn
// on the target's own pixels (see font.h): its lines start on one, and each
// glyph is a whole number of them from the last, so none is ever blended
// between two.
static void add_text(const char *text, const tide_float2 at, const float size, const rgba color)
{
    if (!(size > 0.0f)) return;
    const tide_float2 scale = pixel_scale();
    float stretch;
    const int pixels = tide_font_pixels(size, scale.y, &stretch);
    // In the target's pixels, as the pen is
    const float left = floorf(at.x * scale.x + 0.5f);
    const float pitch = floorf(tide_font_line() * (float)pixels * stretch + 0.5f);
    float pen_x = left, pen_y = floorf((at.y + tide_font_baseline() * size) * scale.y + 0.5f); // On the baseline

    if (step_count == 0 || steps[step_count - 1].kind != STEP_TEXT) add_step(STEP_TEXT)->first = mesh_vertex_count;
    for (uint32_t c; (c = tide_utf8_next(&text)) != 0;) {
        if (c == '\n') {
            pen_x = left;
            pen_y += pitch;
            continue;
        }
        tide_font_glyph g;
        if (!tide_font_glyph_for(c, pixels, &g)) continue; // No room for it this frame
        if (g.width) {
            const float x0 = (pen_x + (float)g.left * stretch) / scale.x;
            const float y0 = (pen_y + (float)g.top * stretch) / scale.y;
            const float x1 = x0 + (float)g.width * stretch / scale.x, y1 = y0 + (float)g.height * stretch / scale.y;
            const float u0 = (float)g.x, v0 = (float)g.y, u1 = u0 + (float)g.width, v1 = v0 + (float)g.height;
            const mesh_vertex corners[4] = {
                {{x0, y0}, {u0, v0}, {color.r, color.g, color.b, color.a}},
                {{x1, y0}, {u1, v0}, {color.r, color.g, color.b, color.a}},
                {{x1, y1}, {u1, v1}, {color.r, color.g, color.b, color.a}},
                {{x0, y1}, {u0, v1}, {color.r, color.g, color.b, color.a}},
            };
            mesh_vertex *out = mesh_room(6); // After add_step: the steps before it keep their vertices' places
            out[0] = corners[0], out[1] = corners[1], out[2] = corners[2];
            out[3] = corners[0], out[4] = corners[2], out[5] = corners[3];
            mesh_vertex_count += 6;
            steps[step_count - 1].count += 6;
        }
        pen_x += (float)g.advance * stretch;
    }
}

// What the renderer keeps on the GPU (see gpu.h): its pipelines, whose
// shaders are in shaders.h, and the buffers and textures a list's steps draw
// from, made the first time they're needed.
static struct {
    tide_gpu_id shapes, meshes, text, meshes_3d, clear; // Pipelines
    tide_gpu_id corners;                                // The shapes' quad
    tide_gpu_id shape_instances;
    uint32_t shape_capacity; // Shapes the instance buffer holds
    tide_gpu_id mesh_vertices;
    uint32_t mesh_capacity; // Vertices the buffer holds
    tide_gpu_id matrices;
    uint32_t matrix_capacity; // Transforms the 3D meshes' instance buffer holds
    tide_gpu_id white;        // A white pixel, which a mesh with no texture samples
    tide_gpu_id atlas;        // The font atlas, as of `atlas_version`
    int atlas_width, atlas_height;
    uint32_t atlas_version;
    tide_gpu_id target; // What tide_platform_read_pixels draws into, kept while the window keeps its size
    int target_width, target_height;
    uint8_t *target_pixels; // ...read back: RGBA, rows from the top
} gpu_state;

// The shapes', meshes' and text's uniforms, as their shaders have them: from
// the target's logical pixels (from its top left, y down) to clip space, as
// OpenGL's matrices go, column by column, and for text, 1 over the atlas's size.
typedef struct screen_uniforms {
    float screen[16];
    float texel[2];
    float unused[2];
} screen_uniforms;

// A 3D mesh's corner on the GPU. A mesh goes there as its corners, each
// once, and its triangles as indices into them, uploaded when a list first
// draws it, and let go once a frame draws without it.
typedef struct mesh_3d_vertex {
    float position[3];
    float uv[2];
    uint8_t color[4]; // RGBA
} mesh_3d_vertex;

// The GPU's copy of a list's 3D mesh, kept in the place the list keeps it.
typedef struct gpu_mesh {
    uint64_t id, other, version; // Which it is (see tide_draw_mesh_data)
    uint32_t space;
    uint32_t epoch;
    tide_gpu_id vertices, indices; // Its buffers
    uint32_t vertex_capacity;      // Corners `vertices` has room for
    uint32_t index_capacity;       // Bytes `indices` has room for
    uint32_t index_count;          // Three a triangle...
    bool wide;                     // ...of 16 bits where they fit, or 32
    uint32_t frame;                // The frame that last drew it
} gpu_mesh;

static gpu_mesh *gpu_meshes;
static uint32_t gpu_mesh_count; // Places, as the list's
static void *mesh_3d_upload;    // A mesh's corners or indices, on their way to the GPU
static size_t mesh_3d_upload_capacity;

// Room for `bytes` in mesh_3d_upload.
static void *upload_room(const size_t bytes)
{
    if (bytes > mesh_3d_upload_capacity) {
        mesh_3d_upload = tide_realloc(mesh_3d_upload, mesh_3d_upload_capacity, bytes);
        mesh_3d_upload_capacity = bytes;
    }
    return mesh_3d_upload;
}

// A texture on the GPU: a copy of a draw list's, uploaded when a list first
// draws with it and again when its pixels changed, and let go once a frame
// draws without it. Nothing else keeps it: the pixels are the game's.
typedef struct gpu_texture {
    uint64_t id; // Whose pixels (see tide_draw_texture)
    uint64_t version;
    uint32_t space;
    int32_t width, height;
    uint32_t epoch; // Its list's when it copied them (see tide_draw_forget)
    tide_gpu_id texture;
    uint32_t frame; // The frame that last drew with it
} gpu_texture;

static gpu_texture *gpu_textures;
static uint32_t gpu_texture_count, gpu_texture_capacity;
static uint32_t gpu_frame;
static uint32_t gpu_epoch; // The device all of the above was made on (tide_gpu.epoch)

// Whether there's a device to draw with. When it's another than last time,
// what the renderer had on the last one went with it: it forgets it, and
// makes it again as lists draw. It can: the lists keep their textures' and
// meshes' copies, and the font its atlas.
static bool gpu_ready(void)
{
    const uint32_t epoch = gpu->epoch();
    if (epoch == 0 || epoch == gpu_epoch) return epoch != 0;
    free(gpu_state.target_pixels);
    memset(&gpu_state, 0, sizeof gpu_state);
    if (gpu_meshes) memset(gpu_meshes, 0, gpu_mesh_count * sizeof(gpu_mesh));
    gpu_texture_count = 0;
    gpu_epoch = epoch;
    return true;
}

// The pipelines, the quad and the white pixel, the first time a list is drawn.
static void renderer_start(void)
{
    if (gpu_state.shapes) return;
    gpu_state.shapes = gpu->pipeline(&(tide_gpu_pipeline_desc){
        .name = "shapes",
        .glsl_vertex = shape_glsl_vertex,
        .glsl_fragment = shape_glsl_fragment,
        .wgsl = shape_wgsl,
        .slots = {{2 * sizeof(float), false}, {sizeof(shape), true}}, // The quad's corners, and an instance a shape
        .slot_count = 2,
        .attributes = {{0, TIDE_GPU_FLOAT2, 0},
                       {1, TIDE_GPU_FLOAT4, offsetof(shape, a)},
                       {1, TIDE_GPU_BYTES4, offsetof(shape, color)},
                       {1, TIDE_GPU_FLOAT, offsetof(shape, kind)}},
        .attribute_count = 4,
        .uniform_size = 16 * sizeof(float),
        .blend = TIDE_GPU_BLEND_ALPHA,
        .depth = TIDE_GPU_DEPTH_NONE,
    });
    tide_gpu_pipeline_desc mesh = {
        .name = "meshes",
        .glsl_vertex = mesh_glsl_vertex,
        .glsl_fragment = mesh_glsl_fragment,
        .wgsl = mesh_wgsl,
        .slots = {{sizeof(mesh_vertex), false}},
        .slot_count = 1,
        .attributes = {{0, TIDE_GPU_FLOAT2, offsetof(mesh_vertex, position)},
                       {0, TIDE_GPU_FLOAT2, offsetof(mesh_vertex, uv)},
                       {0, TIDE_GPU_BYTES4, offsetof(mesh_vertex, color)}},
        .attribute_count = 3,
        .uniform_size = sizeof(screen_uniforms),
        .texture = true,
        .blend = TIDE_GPU_BLEND_ALPHA,
        .depth = TIDE_GPU_DEPTH_NONE,
    };
    gpu_state.meshes = gpu->pipeline(&mesh);
    mesh.name = "text"; // A mesh's corners, with pixels of its own
    mesh.glsl_fragment = text_glsl_fragment;
    mesh.wgsl = text_wgsl;
    gpu_state.text = gpu->pipeline(&mesh);
    gpu_state.meshes_3d = gpu->pipeline(&(tide_gpu_pipeline_desc){
        .name = "3D meshes",
        .glsl_vertex = mesh_3d_glsl_vertex,
        .glsl_fragment = mesh_glsl_fragment,
        .wgsl = mesh_3d_wgsl,
        .slots = {{sizeof(mesh_3d_vertex), false}, {sizeof(tide_float4x4), true}}, // Its corners, and an instance a transform
        .slot_count = 2,
        .attributes = {{0, TIDE_GPU_FLOAT3, offsetof(mesh_3d_vertex, position)},
                       {0, TIDE_GPU_FLOAT2, offsetof(mesh_3d_vertex, uv)},
                       {0, TIDE_GPU_BYTES4, offsetof(mesh_3d_vertex, color)},
                       {1, TIDE_GPU_FLOAT4, 0 * sizeof(tide_float4)},
                       {1, TIDE_GPU_FLOAT4, 1 * sizeof(tide_float4)},
                       {1, TIDE_GPU_FLOAT4, 2 * sizeof(tide_float4)},
                       {1, TIDE_GPU_FLOAT4, 3 * sizeof(tide_float4)}},
        .attribute_count = 7,
        .uniform_size = sizeof(tide_float4x4),
        .texture = true,
        .blend = TIDE_GPU_BLEND_ALPHA,
        .depth = TIDE_GPU_DEPTH_TEST,
    });
    gpu_state.clear = gpu->pipeline(&(tide_gpu_pipeline_desc){
        .name = "clears",
        .glsl_vertex = clear_glsl_vertex,
        .glsl_fragment = clear_glsl_fragment,
        .wgsl = clear_wgsl,
        .uniform_size = 4 * sizeof(float),
        .blend = TIDE_GPU_BLEND_REPLACE,
        .depth = TIDE_GPU_DEPTH_RESET,
    });
    static const float corners[12] = {0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0};
    gpu_state.corners = gpu->buffer(TIDE_GPU_VERTICES, sizeof corners, corners);
    gpu_state.white = gpu->texture(TIDE_GPU_RGBA8, 1, 1, (const uint8_t[4]){255, 255, 255, 255});
}

// The GPU's copy of the list's 3D mesh `mesh`, uploaded if it hasn't got it.
static gpu_mesh *mesh_for(const tide_draw_list *list, const uint32_t mesh)
{
    if (list->mesh_count > gpu_mesh_count) {
        gpu_meshes = tide_realloc(gpu_meshes, gpu_mesh_count * sizeof(gpu_mesh), list->mesh_count * sizeof(gpu_mesh));
        memset(gpu_meshes + gpu_mesh_count, 0, (list->mesh_count - gpu_mesh_count) * sizeof(gpu_mesh));
        gpu_mesh_count = list->mesh_count;
    }
    const tide_draw_mesh_data *m = &list->meshes[mesh - 1u];
    gpu_mesh *g = &gpu_meshes[mesh - 1u];
    g->frame = gpu_frame;
    if (g->vertices && g->id == m->id && g->other == m->other && g->version == m->version && g->space == m->space
        && g->epoch == m->epoch) {
        return g;
    }
    // Its corners, each once
    const size_t vertex_bytes = (size_t)m->vertex_count * sizeof(mesh_3d_vertex);
    mesh_3d_vertex *corners = upload_room(vertex_bytes);
    for (uint32_t i = 0; i < m->vertex_count; i++) {
        const tide_vertex3 *v = &m->vertices[i];
        const rgba color = to_rgba(v->color);
        corners[i] = (mesh_3d_vertex){{v->position.x, v->position.y, v->position.z}, {v->uv.x, v->uv.y},
                                      {color.r, color.g, color.b, color.a}};
    }
    if (g->vertices && m->vertex_count <= g->vertex_capacity) {
        gpu->buffer_write(g->vertices, 0, corners, vertex_bytes);
    } else {
        if (g->vertices) gpu->buffer_free(g->vertices);
        g->vertices = gpu->buffer(TIDE_GPU_VERTICES, vertex_bytes, corners);
        g->vertex_capacity = m->vertex_count;
    }
    // ...and its triangles, in 16 bits where they fit, to a multiple of 4 bytes, as buffers take them
    const bool wide = m->vertex_count > 65536u;
    const size_t index_bytes = ((size_t)m->index_count * (wide ? sizeof(uint32_t) : sizeof(uint16_t)) + 3u) & ~(size_t)3u;
    const void *indices = m->indices;
    if (!wide) {
        uint16_t *narrow = upload_room(index_bytes);
        memset(narrow, 0, index_bytes);
        for (uint32_t i = 0; i < m->index_count; i++) narrow[i] = (uint16_t)m->indices[i];
        indices = narrow;
    }
    if (g->indices && index_bytes <= g->index_capacity) {
        gpu->buffer_write(g->indices, 0, indices, index_bytes);
    } else {
        if (g->indices) gpu->buffer_free(g->indices);
        g->indices = gpu->buffer(TIDE_GPU_INDICES, index_bytes, indices);
        g->index_capacity = (uint32_t)index_bytes;
    }
    g->id = m->id;
    g->other = m->other;
    g->version = m->version;
    g->space = m->space;
    g->epoch = m->epoch;
    g->index_count = m->index_count;
    g->wide = wide;
    return g;
}

// A buffer for `count` elements of `size` bytes, in place of `*buffer` when
// it has no room for them: its capacity doubles until it does.
static void buffer_room(tide_gpu_id *buffer, uint32_t *capacity, const uint32_t count, const size_t size,
                        const uint32_t at_least)
{
    if (count <= *capacity) return;
    uint32_t grown = *capacity ? *capacity : at_least;
    while (grown < count) grown *= 2u;
    if (*buffer) gpu->buffer_free(*buffer);
    *buffer = gpu->buffer(TIDE_GPU_VERTICES, (size_t)grown * size, NULL);
    *capacity = grown;
}

// The list's matrices into the instance buffer, which grows as they need,
// and its 3D meshes onto the GPU, before anything draws.
static void upload_meshes_3d(const tide_draw_list *list)
{
    bool any = false;
    for (uint32_t i = 0; i < step_count; i++) {
        if (steps[i].kind != STEP_MESH_3D) continue;
        any = true;
        mesh_for(list, steps[i].mesh);
    }
    if (!any) return;
    buffer_room(&gpu_state.matrices, &gpu_state.matrix_capacity, list->matrix_count, sizeof(tide_float4x4), 1024u);
    gpu->buffer_write(gpu_state.matrices, 0, list->matrices, (size_t)list->matrix_count * sizeof(tide_float4x4));
}

// Lets go of the 3D meshes the frame didn't draw.
static void forget_meshes_3d(void)
{
    for (uint32_t i = 0; i < gpu_mesh_count; i++) {
        if (gpu_meshes[i].vertices && gpu_meshes[i].frame != gpu_frame) {
            gpu->buffer_free(gpu_meshes[i].vertices);
            gpu->buffer_free(gpu_meshes[i].indices);
            gpu_meshes[i] = (gpu_mesh){0};
        }
    }
}

// This list's shapes into the instance buffer, and its triangles, text's too,
// into theirs: both grow as they need.
static void upload_shapes_and_meshes(void)
{
    if (shape_count) {
        buffer_room(&gpu_state.shape_instances, &gpu_state.shape_capacity, shape_count, sizeof(shape), 1024u);
        gpu->buffer_write(gpu_state.shape_instances, 0, shapes, (size_t)shape_count * sizeof(shape));
    }
    if (mesh_vertex_count) {
        buffer_room(&gpu_state.mesh_vertices, &gpu_state.mesh_capacity, mesh_vertex_count, sizeof(mesh_vertex), 4096u);
        gpu->buffer_write(gpu_state.mesh_vertices, 0, mesh_vertices, (size_t)mesh_vertex_count * sizeof(mesh_vertex));
    }
}

// The font atlas onto the GPU, as the list's text left it.
static void upload_atlas(void)
{
    int width = 0, height = 0;
    uint32_t version = 0;
    const uint8_t *pixels = tide_font_atlas(&width, &height, &version);
    if (!pixels || (gpu_state.atlas && gpu_state.atlas_version == version)) return;
    if (gpu_state.atlas && width == gpu_state.atlas_width && height == gpu_state.atlas_height) {
        gpu->texture_write(gpu_state.atlas, pixels);
    } else {
        if (gpu_state.atlas) gpu->texture_free(gpu_state.atlas);
        gpu_state.atlas = gpu->texture(TIDE_GPU_R8, width, height, pixels);
    }
    gpu_state.atlas_width = width;
    gpu_state.atlas_height = height;
    gpu_state.atlas_version = version;
}

// The GPU's copy of a list's texture, uploaded if it hasn't got these pixels.
static gpu_texture *texture_for(const tide_draw_texture *t)
{
    gpu_texture *g = NULL;
    for (uint32_t i = 0; i < gpu_texture_count && !g; i++) {
        if (gpu_textures[i].id == t->id && gpu_textures[i].space == t->space) g = &gpu_textures[i];
    }
    if (!g) {
        if (gpu_texture_count == gpu_texture_capacity) {
            const uint32_t capacity = gpu_texture_capacity ? gpu_texture_capacity * 2u : 8u;
            gpu_textures = tide_realloc(gpu_textures, gpu_texture_capacity * sizeof(gpu_texture),
                                        capacity * sizeof(gpu_texture));
            gpu_texture_capacity = capacity;
        }
        g = &gpu_textures[gpu_texture_count++];
        *g = (gpu_texture){.id = t->id, .space = t->space};
    }
    g->frame = gpu_frame;
    if (g->texture && g->version == t->version && g->width == t->width && g->height == t->height
        && g->epoch == t->epoch) {
        return g;
    }
    if (g->texture && g->width == t->width && g->height == t->height) {
        gpu->texture_write(g->texture, t->pixels);
    } else {
        if (g->texture) gpu->texture_free(g->texture);
        g->texture = gpu->texture(TIDE_GPU_RGBA8, t->width, t->height, t->pixels);
    }
    g->version = t->version;
    g->width = t->width;
    g->height = t->height;
    g->epoch = t->epoch;
    return g;
}

// Lets go of the textures the frame didn't draw with.
static void forget_textures(void)
{
    uint32_t kept = 0;
    for (uint32_t i = 0; i < gpu_texture_count; i++) {
        if (gpu_textures[i].frame != gpu_frame) {
            if (gpu_textures[i].texture) gpu->texture_free(gpu_textures[i].texture);
            continue;
        }
        gpu_textures[kept++] = gpu_textures[i];
    }
    gpu_texture_count = kept;
    gpu_frame++;
}

// The texture a mesh step samples, sampled as the step says.
static void sample_step_texture(const tide_draw_list *list, const draw_step *s)
{
    tide_gpu_id texture = gpu_state.white;
    if (s->texture) {
        const gpu_texture *g = texture_for(&list->textures[s->texture - 1u]);
        if (g->texture) texture = g->texture;
    }
    gpu->sample(texture, s->filter != TIDE_FILTER_POINT);
}

static void draw_mesh_3d(const tide_draw_list *list, const draw_step *s)
{
    const gpu_mesh *g = &gpu_meshes[s->mesh - 1u];
    // The camera, fitted to the screen's shape: x divided by its width over its height
    tide_float4x4 world_to_clip = s->camera ? list->matrices[s->camera - 1u]
                                            : tide_draw_camera_3d_matrix(tide_f3(0.0f, 0.0f, 0.0f), tide_identity_q(), 60.0f);
    if (!s->camera || s->fit) {
        const float across = (float)screen_height / (float)screen_width;
        world_to_clip.c0.x *= across, world_to_clip.c1.x *= across;
        world_to_clip.c2.x *= across, world_to_clip.c3.x *= across;
    }
    gpu->use(gpu_state.meshes_3d);
    gpu->uniforms(&world_to_clip, sizeof world_to_clip);
    sample_step_texture(list, s);
    gpu->vertices(0, g->vertices, 0);
    gpu->vertices(1, gpu_state.matrices, (size_t)s->first * sizeof(tide_float4x4));
    gpu->indices(g->indices, g->wide);
    gpu->draw_indexed(g->index_count, s->count);
}

static int nearest_pixel(const float v)
{
    return (int)floorf(v + 0.5f);
}

static int clamp_pixel(const int v, const int most)
{
    return v < 0 ? 0 : v > most ? most : v;
}

// Only what's inside the step's rect draws from here on, or everything again.
static void clip(const draw_step *s)
{
    const int width = offscreen ? screen_width : pixel_width, height = offscreen ? screen_height : pixel_height;
    if (s->kind == STEP_NO_CLIP) {
        gpu->scissor(0, 0, width, height);
        return;
    }
    // In the target's pixels, and inside it
    const tide_float2 scale = pixel_scale();
    const int left = clamp_pixel(nearest_pixel(s->at.x * scale.x), width);
    const int right = clamp_pixel(nearest_pixel(s->to.x * scale.x), width);
    const int top = clamp_pixel(nearest_pixel(s->at.y * scale.y), height);
    const int bottom = clamp_pixel(nearest_pixel(s->to.y * scale.y), height);
    gpu->scissor(left, top, right > left ? right - left : 0, bottom > top ? bottom - top : 0);
}

// Draws the list into `target`: the window for 0, or tide_platform_read_pixels'
// own pixels. With `over`, it goes over what's there (the overlay), and leaves
// the frame's camera, textures and meshes as they are.
static void draw_list(const tide_draw_list *list, const tide_gpu_id target, const bool over)
{
    if (!gpu_ready()) return; // Between a device that was lost and the next
    offscreen = target != 0;
    tide_font_frame();
    shape_count = 0;
    mesh_vertex_count = 0;
    step_count = 0;
    camera cam = {{0.0f, 0.0f}, 1.0f, false};
    camera world = cam; // The last world camera, for tide_platform_world_to_screen
    uint32_t camera_3d = 0, fit = 1; // The 3D meshes' (see draw_step)
    for (uint32_t i = 0; i < list->count; i++) {
        const tide_draw_command *c = &list->commands[i];
        const rgba color = to_rgba(c->color);
        switch ((tide_draw_kind)c->kind) {
        case TIDE_DRAW_CLEAR:
            add_step(STEP_CLEAR)->color = color;
            break;
        case TIDE_DRAW_CAMERA:
            cam.center = c->a;
            cam.scale = c->b.x > 0.0f ? (float)screen_height / (2.0f * c->b.x) : 1.0f;
            cam.gui = false;
            world = cam;
            break;
        case TIDE_DRAW_SCREEN:
            cam.scale = 1.0f;
            cam.gui = true;
            break;
        case TIDE_DRAW_MESH:
            add_mesh(list, c, &cam);
            break;
        case TIDE_DRAW_CAMERA_3D:
            camera_3d = c->camera.matrix + 1u;
            fit = c->camera.fit;
            break;
        case TIDE_DRAW_MESH_3D: {
            if (!c->mesh3.mesh || c->mesh3.mesh > list->mesh_count) break; // Forgotten (see tide_draw_forget)
            draw_step *s = add_step(STEP_MESH_3D);
            s->first = c->mesh3.first;
            s->count = c->mesh3.count;
            s->texture = c->mesh3.texture;
            s->filter = c->mesh3.filter;
            s->mesh = c->mesh3.mesh;
            s->camera = camera_3d;
            s->fit = fit;
            break;
        }
        case TIDE_DRAW_CLIP: {
            // Its corners on the screen, whichever way up the units are
            const tide_float2 p = to_screen(&cam, c->a);
            const tide_float2 q = to_screen(&cam, tide_f2(c->a.x + c->b.x, c->a.y + c->b.y));
            draw_step *s = add_step(STEP_CLIP);
            s->at = tide_f2(p.x < q.x ? p.x : q.x, p.y < q.y ? p.y : q.y);
            s->to = c->b.x > 0.0f && c->b.y > 0.0f ? tide_f2(p.x < q.x ? q.x : p.x, p.y < q.y ? q.y : p.y) : s->at;
            break;
        }
        case TIDE_DRAW_NO_CLIP:
            add_step(STEP_NO_CLIP);
            break;
        case TIDE_DRAW_CIRCLE:
        case TIDE_DRAW_WIRE_CIRCLE: {
            const tide_float2 p = to_screen(&cam, c->a);
            const float r = c->b.x * cam.scale;
            add_shape(c->kind == TIDE_DRAW_CIRCLE ? SHAPE_CIRCLE : SHAPE_WIRE_CIRCLE, p.x, p.y, r, r, color);
            break;
        }
        case TIDE_DRAW_RECT:
        case TIDE_DRAW_WIRE_RECT: {
            const tide_float2 top_left = rect_corner(&cam, c);
            const float hw = c->b.x * cam.scale * 0.5f;
            const float hh = c->b.y * cam.scale * 0.5f;
            add_shape(c->kind == TIDE_DRAW_RECT ? SHAPE_RECT : SHAPE_WIRE_RECT, top_left.x + hw, top_left.y + hh, hw,
                      hh, color);
            break;
        }
        case TIDE_DRAW_LINE: {
            const tide_float2 from = to_screen(&cam, c->a);
            const tide_float2 to = to_screen(&cam, c->b);
            add_shape(SHAPE_LINE, from.x, from.y, to.x, to.y, color);
            break;
        }
        case TIDE_DRAW_TEXT:
            add_text(list->text + c->text, to_screen(&cam, c->a), c->b.x * cam.scale, color);
            break;
        }
    }
    if (!over) last_camera = world;

    // What the steps draw from goes to the GPU first
    renderer_start();
    upload_shapes_and_meshes();
    upload_meshes_3d(list);
    upload_atlas();
    screen_uniforms uniforms = {.screen = {[0] = 2.0f / (float)screen_width, [5] = -2.0f / (float)screen_height,
                                           [10] = 1.0f, [12] = -1.0f, [13] = 1.0f, [15] = 1.0f}};
    if (gpu_state.atlas) {
        uniforms.texel[0] = 1.0f / (float)gpu_state.atlas_width;
        uniforms.texel[1] = 1.0f / (float)gpu_state.atlas_height;
    }

    gpu->begin(target, !over); // Every frame starts black; Draw.Clear picks another color
    for (uint32_t i = 0; i < step_count; i++) {
        const draw_step *s = &steps[i];
        switch (s->kind) {
        case STEP_SHAPES:
            gpu->use(gpu_state.shapes);
            gpu->uniforms(uniforms.screen, sizeof uniforms.screen);
            gpu->vertices(0, gpu_state.corners, 0);
            gpu->vertices(1, gpu_state.shape_instances, (size_t)s->first * sizeof(shape));
            gpu->draw(0, 6, s->count);
            break;
        case STEP_CLEAR: {
            // A triangle over the target, which the clip cuts, and which sets depth back too
            const float color[4] = {(float)s->color.r / 255.0f, (float)s->color.g / 255.0f, (float)s->color.b / 255.0f,
                                    (float)s->color.a / 255.0f};
            gpu->use(gpu_state.clear);
            gpu->uniforms(color, sizeof color);
            gpu->draw(0, 3, 1);
            break;
        }
        case STEP_MESH:
            if (s->count == 0) break;
            gpu->use(gpu_state.meshes);
            gpu->uniforms(&uniforms, sizeof uniforms);
            sample_step_texture(list, s);
            gpu->vertices(0, gpu_state.mesh_vertices, 0);
            gpu->draw(s->first, s->count, 1);
            break;
        case STEP_MESH_3D:
            draw_mesh_3d(list, s);
            break;
        case STEP_CLIP:
        case STEP_NO_CLIP:
            clip(s);
            break;
        default: // Text
            if (s->count == 0 || !gpu_state.atlas) break;
            gpu->use(gpu_state.text);
            gpu->uniforms(&uniforms, sizeof uniforms);
            gpu->sample(gpu_state.atlas, true); // Stretched glyphs blend; the others land on the target's pixels
            gpu->vertices(0, gpu_state.mesh_vertices, 0);
            gpu->draw(s->first, s->count, 1);
            break;
        }
    }
    gpu->end();
    offscreen = false;
    if (!over) {
        forget_meshes_3d();
        forget_textures();
    }
}

void tide_platform_draw(const tide_draw_list *list)
{
    // Nobody sees it while the window is minimized or, on the web, the page
    // is hidden, and frames go on meanwhile.
    if (!tide_window_unseen()) draw_list(list, 0, false);
}

tide_float2 tide_platform_world_to_screen(const tide_float2 world)
{
    return to_screen(&last_camera, world);
}

tide_float2 tide_platform_screen_size(void)
{
    return tide_f2((float)screen_width, (float)screen_height);
}

// The window's pixels in a logical one, which text is fitted to (see font.h).
static float window_scale(void)
{
    return (float)pixel_height / (float)screen_height;
}

float tide_platform_measure_text(const char *text, const float size)
{
    return tide_font_measure(text, size, window_scale());
}

const char *tide_platform_renderer(void)
{
    return gpu ? gpu->name : "";
}

void tide_platform_draw_overlay(const char *text)
{
    if (tide_window_unseen()) return;
    static tide_draw_list overlay;
    const float size = 16.0f;
    const float pitch = 20.0f; // From one line to the next
    char lines[16][128];
    float widths[16];
    int count = 0;
    float widest = 0.0f;
    for (const char *line = text; line && count < 16; count++) {
        const char *end = strchr(line, '\n');
        size_t n = end ? (size_t)(end - line) : strlen(line);
        if (n >= sizeof lines[0]) n = sizeof lines[0] - 1;
        memcpy(lines[count], line, n);
        lines[count][n] = '\0';
        widths[count] = ceilf(tide_font_measure(lines[count], size, window_scale()));
        if (widths[count] > widest) widest = widths[count];
        line = end ? end + 1 : NULL;
    }
    // A panel in the bottom right corner, each line against its right edge
    const float right = (float)screen_width - 12.0f;
    const float top = (float)screen_height - 12.0f - (float)count * pitch;
    const tide_float2 panel = tide_f2(widest + 16.0f, (float)count * pitch + 8.0f);
    tide_draw_reset(&overlay);
    tide_draw_screen(&overlay);
    tide_draw_rect(&overlay, tide_f2(right + 8.0f - panel.x * 0.5f, top - 6.0f + panel.y * 0.5f), panel,
                   (tide_color){0.0f, 0.0f, 0.0f, 140.0f / 255.0f});
    for (int i = 0; i < count; i++) {
        tide_draw_text(&overlay, lines[i], tide_f2(right - widths[i], top + (float)i * pitch), size,
                       (tide_color){200.0f / 255.0f, 200.0f / 255.0f, 200.0f / 255.0f, 1.0f});
    }
    draw_list(&overlay, 0, true);
}

void tide_platform_read_pixels(const tide_draw_list *list, const tide_float2 *points, const int count, uint32_t *out)
{
    const int width = screen_width, height = screen_height;
    if (!gpu_ready()) { // Nothing to draw with for now: no pixels
        memset(out, 0, (size_t)(count > 0 ? count : 0) * sizeof *out);
        return;
    }
    if (!gpu_state.target || gpu_state.target_width != width || gpu_state.target_height != height) {
        if (gpu_state.target) gpu->target_free(gpu_state.target);
        gpu_state.target = gpu->target(width, height);
        gpu_state.target_pixels =
            tide_realloc(gpu_state.target_pixels, (size_t)gpu_state.target_width * (size_t)gpu_state.target_height * 4u,
                         (size_t)width * (size_t)height * 4u);
        gpu_state.target_width = width;
        gpu_state.target_height = height;
    }
    draw_list(list, gpu_state.target, false);
    gpu->target_read(gpu_state.target, gpu_state.target_pixels);
    for (int i = 0; i < count; i++) {
        const int x = (int)points[i].x, y = (int)points[i].y;
        uint32_t pixel = 0;
        if (x >= 0 && x < width && y >= 0 && y < height) {
            const uint8_t *p = gpu_state.target_pixels + ((size_t)y * (size_t)width + (size_t)x) * 4u;
            pixel = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
        }
        out[i] = pixel;
    }
}
