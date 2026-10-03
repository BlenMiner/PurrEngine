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
#include "gl.h"
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

// Text `size` tall with its top left corner at `at`, in window pixels: a quad
// a glyph, after the text before it when nothing came between them. Glyphs
// are drawn into the atlas as tall as they are on the target, to the nearest
// pixel, and land on its pixels, so they're as sharp as the display.
static void add_text(const char *text, const tide_float2 at, const float size, const rgba color)
{
    if (!(size > 0.0f)) return;
    const tide_float2 scale = pixel_scale();
    const float tall = size * scale.y; // In the target's pixels, as the pen below
    int pixels = (int)(tall + 0.5f);
    if (pixels < 1) pixels = 1;
    if (pixels > TIDE_FONT_MAX_PIXELS) pixels = TIDE_FONT_MAX_PIXELS;
    const float stretch = tall / (float)pixels;
    const float left = at.x * scale.x;
    float pen_x = left, pen_y = at.y * scale.y;

    if (step_count == 0 || steps[step_count - 1].kind != STEP_TEXT) add_step(STEP_TEXT)->first = mesh_vertex_count;
    for (uint32_t c; (c = tide_utf8_next(&text)) != 0;) {
        if (c == '\n') {
            pen_x = left;
            pen_y += tide_font_line() * tall;
            continue;
        }
        tide_font_glyph g;
        if (tide_font_glyph_for(c, pixels, &g) && g.width) {
            const float x0 = floorf(pen_x + (float)g.left * stretch + 0.5f) / scale.x;
            const float y0 = floorf(pen_y + (float)g.top * stretch + 0.5f) / scale.y;
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
        pen_x += tide_font_advance(c) * tall * (scale.x / scale.y);
    }
}

// OpenGL ES 3 on the web and Android, OpenGL 3.3 on desktop.
#if defined(__wasm__) || defined(__ANDROID__)
#define GLSL_VERSION "#version 300 es\nprecision highp float;\n"
#else
#define GLSL_VERSION "#version 330\n"
#endif

// Each instance's quad covers its shape in window pixels, which the camera
// already applied, and `screen` takes to the target.
static const char shape_vertex[] = GLSL_VERSION
    "layout(location = 0) in vec2 corner;\n" // A corner of the quad, 0 to 1
    "layout(location = 1) in vec4 shape;\n"  // a, then b
    "layout(location = 2) in vec4 color;\n"
    "layout(location = 3) in float kind;\n"
    "uniform mat4 screen;\n"
    "out vec2 local;\n"  // Pixels from its middle; for a line, along and across it
    "out vec2 extent;\n" // Half its size in pixels
    "out vec4 tint;\n"
    "out float form;\n"
    "void main() {\n"
    "    vec2 c = corner * 2.0 - 1.0;\n"
    "    vec2 p;\n"
    "    if (kind > 3.5) {\n" // A line: a pixel wide, reaching half a pixel past its ends
    "        vec2 d = shape.zw - shape.xy;\n"
    "        float len = length(d);\n"
    "        vec2 along = len > 0.0 ? d / len : vec2(1.0, 0.0);\n"
    "        extent = vec2(len * 0.5 + 0.5, 0.5);\n"
    "        local = c * extent;\n"
    "        p = (shape.xy + shape.zw) * 0.5 + along * local.x + vec2(-along.y, along.x) * local.y;\n"
    "    } else {\n"
    "        extent = abs(shape.zw);\n"
    "        local = c * (extent + (kind > 1.5 ? 1.0 : 0.0));\n" // Room for a circle's smooth edge
    "        p = shape.xy + local;\n"
    "    }\n"
    "    tint = color;\n"
    "    form = kind;\n"
    "    gl_Position = screen * vec4(p, 0.0, 1.0);\n"
    "}\n";

static const char shape_fragment[] = GLSL_VERSION
    "in vec2 local;\n"
    "in vec2 extent;\n"
    "in vec4 tint;\n"
    "in float form;\n"
    "out vec4 pixel;\n"
    "void main() {\n"
    "    float cover = 1.0;\n"
    "    if (form > 0.5 && form < 1.5) {\n" // A rect's outline: the pixel inside its edge
    "        if (all(lessThan(abs(local), extent - 1.0))) discard;\n"
    "    } else if (form > 1.5 && form < 3.5) {\n" // Circles, with a smooth edge
    "        float d = length(local);\n"
    "        cover = clamp(extent.x - d + 0.5, 0.0, 1.0);\n"
    "        if (form > 2.5) cover *= clamp(d - extent.x + 1.5, 0.0, 1.0);\n" // An outline: the pixel inside it
    "        if (cover <= 0.0) discard;\n"
    "    }\n"
    "    pixel = vec4(tint.rgb, tint.a * cover);\n"
    "}\n";

enum { SHAPE_CORNER, SHAPE_SHAPE, SHAPE_COLOR, SHAPE_KIND }; // Its attributes' locations

static struct {
    GLuint program, vao, corners, instances;
    uint32_t capacity; // Shapes the instance buffer holds
    GLint screen;
} shape_gpu;

// A mesh's pixels are its corners' colors, blended across each triangle,
// times its texture's (a white pixel, for a mesh with none).
static const char mesh_vertex_shader[] = GLSL_VERSION
    "layout(location = 0) in vec2 position;\n" // In window pixels
    "layout(location = 1) in vec2 uv;\n"
    "layout(location = 2) in vec4 color;\n"
    "uniform mat4 screen;\n"
    "out vec2 at;\n"
    "out vec4 tint;\n"
    "void main() {\n"
    "    at = uv;\n"
    "    tint = color;\n"
    "    gl_Position = screen * vec4(position, 0.0, 1.0);\n"
    "}\n";

static const char mesh_fragment_shader[] = GLSL_VERSION
    "in vec2 at;\n"
    "in vec4 tint;\n"
    "uniform sampler2D pixels;\n"
    "out vec4 pixel;\n"
    "void main() {\n"
    "    pixel = texture(pixels, at) * tint;\n"
    "}\n";

// Text's pixels are its color, as much as the font atlas says its glyph
// covers each. Its corners say where in the atlas's pixels, so the atlas can
// grow under them.
static const char text_fragment_shader[] = GLSL_VERSION
    "in vec2 at;\n"
    "in vec4 tint;\n"
    "uniform sampler2D pixels;\n"
    "uniform vec2 texel;\n" // 1 over the atlas's size
    "out vec4 pixel;\n"
    "void main() {\n"
    "    pixel = vec4(tint.rgb, tint.a * texture(pixels, at * texel).r);\n"
    "}\n";

enum { MESH_POSITION, MESH_UV, MESH_COLOR }; // Its attributes' locations, and text's

static struct {
    GLuint program, vao, vertices;
    uint32_t capacity; // Vertices the buffer holds
    GLint screen, pixels;
} mesh_gpu;

static struct {
    GLuint program, atlas;
    GLint screen, pixels, texel;
    int width, height; // The atlas's on the GPU, and its version there
    uint32_t version;
} text_gpu;

// A 3D mesh's corners go through its instance's transform and the camera,
// with depth; its pixels are as a mesh's.
static const char mesh_3d_vertex_shader[] = GLSL_VERSION
    "layout(location = 0) in vec3 position;\n"
    "layout(location = 1) in vec2 uv;\n"
    "layout(location = 2) in vec4 color;\n"
    "layout(location = 3) in vec4 model0;\n" // Its instance's transform, column by column
    "layout(location = 4) in vec4 model1;\n"
    "layout(location = 5) in vec4 model2;\n"
    "layout(location = 6) in vec4 model3;\n"
    "uniform mat4 camera;\n" // From the world to clip space
    "out vec2 at;\n"
    "out vec4 tint;\n"
    "void main() {\n"
    "    at = uv;\n"
    "    tint = color;\n"
    "    gl_Position = camera * (mat4(model0, model1, model2, model3) * vec4(position, 1.0));\n"
    "}\n";

enum { MESH_3D_POSITION, MESH_3D_UV, MESH_3D_COLOR, MESH_3D_MODEL }; // Its attributes' locations: the model's four

// A 3D mesh's corner on the GPU. A mesh goes there as its corners, each
// once, and its triangles as indices into them, uploaded when a list first
// draws it, and let go once a frame draws without it.
typedef struct mesh_3d_vertex {
    float position[3];
    float uv[2];
    uint8_t color[4]; // RGBA
} mesh_3d_vertex;

static struct {
    GLuint program, vao, instances;
    uint32_t capacity; // Transforms the instance buffer holds
    GLint camera, pixels;
} mesh_3d_gpu;

// The GPU's copy of a list's 3D mesh, kept in the place the list keeps it.
typedef struct gpu_mesh {
    uint64_t id, other, version; // Which it is (see tide_draw_mesh_data)
    uint32_t space;
    uint32_t epoch;
    GLuint vertices, indices; // Its buffers
    uint32_t vertex_capacity; // Corners `vertices` has room for
    uint32_t index_capacity;  // Bytes `indices` has room for
    uint32_t index_count;     // Three a triangle...
    GLenum index_type;        // ...of 16 bits where they fit, or 32 (GL_UNSIGNED_SHORT or GL_UNSIGNED_INT)
    uint32_t frame;           // The frame that last drew it
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
    GLuint gl;
    int filter;     // The last it was sampled with, or -1
    uint32_t frame; // The frame that last drew with it
} gpu_texture;

static gpu_texture *gpu_textures;
static uint32_t gpu_texture_count, gpu_texture_capacity;
static uint32_t gpu_frame;
static GLuint white_texture; // A white pixel, which a mesh with no texture samples

static GLuint compile(const char *what, const GLenum kind, const char *source)
{
    const GLuint shader = glCreateShader(kind);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = "";
        glGetShaderInfoLog(shader, sizeof log, NULL, log);
        fprintf(stderr, "tide: the %s' %s shader didn't compile:\n%s\n", what,
                kind == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        abort();
    }
    return shader;
}

// A program of the two shaders.
static GLuint make_program(const char *what, const char *vertex, const char *fragment)
{
    const GLuint vertex_shader = compile(what, GL_VERTEX_SHADER, vertex);
    const GLuint fragment_shader = compile(what, GL_FRAGMENT_SHADER, fragment);
    const GLuint program = glCreateProgram();
    glAttachShader(program, vertex_shader);
    glAttachShader(program, fragment_shader);
    glLinkProgram(program);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = "";
        glGetProgramInfoLog(program, sizeof log, NULL, log);
        fprintf(stderr, "tide: the %s' shaders didn't link:\n%s\n", what, log);
        abort();
    }
    glDeleteShader(vertex_shader); // The program keeps them
    glDeleteShader(fragment_shader);
    return program;
}

static GLuint make_buffer(const GLenum target, const void *data, const size_t bytes, const bool changes)
{
    GLuint buffer = 0;
    glGenBuffers(1, &buffer);
    glBindBuffer(target, buffer);
    glBufferData(target, (GLsizeiptr)bytes, data, changes ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
    return buffer;
}

// An attribute of `count` floats, or of 4 bytes read as 0 to 1, in the array
// buffer that's bound.
static void attribute(const int index, const int count, const bool bytes, const size_t stride, const size_t offset)
{
    glVertexAttribPointer((GLuint)index, count, bytes ? GL_UNSIGNED_BYTE : GL_FLOAT, bytes, (GLsizei)stride,
                          (const void *)offset);
}

// The shader and the quad, the first time they're needed.
static void shapes_start(void)
{
    if (shape_gpu.program) return;
    shape_gpu.program = make_program("shapes", shape_vertex, shape_fragment);
    shape_gpu.screen = glGetUniformLocation(shape_gpu.program, "screen");
    glGenVertexArrays(1, &shape_gpu.vao);
    glBindVertexArray(shape_gpu.vao);
    static const float corners[12] = {0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0};
    shape_gpu.corners = make_buffer(GL_ARRAY_BUFFER, corners, sizeof corners, false);
    attribute(SHAPE_CORNER, 2, false, 0, 0);
    glEnableVertexAttribArray(SHAPE_CORNER);
    for (int i = SHAPE_SHAPE; i <= SHAPE_KIND; i++) {
        glEnableVertexAttribArray((GLuint)i);
        glVertexAttribDivisor((GLuint)i, 1);
    }
    glBindVertexArray(0);
}

static void meshes_start(void)
{
    if (mesh_gpu.program) return;
    mesh_gpu.program = make_program("meshes", mesh_vertex_shader, mesh_fragment_shader);
    mesh_gpu.screen = glGetUniformLocation(mesh_gpu.program, "screen");
    mesh_gpu.pixels = glGetUniformLocation(mesh_gpu.program, "pixels");
    text_gpu.program = make_program("text", mesh_vertex_shader, text_fragment_shader);
    text_gpu.screen = glGetUniformLocation(text_gpu.program, "screen");
    text_gpu.pixels = glGetUniformLocation(text_gpu.program, "pixels");
    text_gpu.texel = glGetUniformLocation(text_gpu.program, "texel");
    glGenVertexArrays(1, &mesh_gpu.vao);
    glBindVertexArray(mesh_gpu.vao);
    for (int i = MESH_POSITION; i <= MESH_COLOR; i++) glEnableVertexAttribArray((GLuint)i);
    glBindVertexArray(0);
}

static void meshes_3d_start(void)
{
    if (mesh_3d_gpu.program) return;
    mesh_3d_gpu.program = make_program("3D meshes", mesh_3d_vertex_shader, mesh_fragment_shader);
    mesh_3d_gpu.camera = glGetUniformLocation(mesh_3d_gpu.program, "camera");
    mesh_3d_gpu.pixels = glGetUniformLocation(mesh_3d_gpu.program, "pixels");
    glGenVertexArrays(1, &mesh_3d_gpu.vao);
    glBindVertexArray(mesh_3d_gpu.vao);
    for (int i = MESH_3D_POSITION; i < MESH_3D_MODEL + 4; i++) glEnableVertexAttribArray((GLuint)i);
    for (int i = 0; i < 4; i++) glVertexAttribDivisor((GLuint)(MESH_3D_MODEL + i), 1);
    glBindVertexArray(0);
}

// The GPU's copy of the list's 3D mesh `mesh`, uploaded if it hasn't got it.
// Its index buffer goes with the 3D meshes' vertex array, which is bound.
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
    mesh_3d_vertex *corners = upload_room((size_t)m->vertex_count * sizeof(mesh_3d_vertex));
    for (uint32_t i = 0; i < m->vertex_count; i++) {
        const tide_vertex3 *v = &m->vertices[i];
        const rgba color = to_rgba(v->color);
        corners[i] = (mesh_3d_vertex){{v->position.x, v->position.y, v->position.z}, {v->uv.x, v->uv.y},
                                      {color.r, color.g, color.b, color.a}};
    }
    const size_t vertex_bytes = (size_t)m->vertex_count * sizeof(mesh_3d_vertex);
    if (g->vertices && m->vertex_count <= g->vertex_capacity) {
        glBindBuffer(GL_ARRAY_BUFFER, g->vertices);
        glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)vertex_bytes, corners);
    } else {
        if (g->vertices) glDeleteBuffers(1, &g->vertices);
        g->vertices = make_buffer(GL_ARRAY_BUFFER, corners, vertex_bytes, false);
        g->vertex_capacity = m->vertex_count;
    }
    // ...and its triangles, in 16 bits where they fit
    const bool small = m->vertex_count <= 65536u;
    const size_t index_bytes = (size_t)m->index_count * (small ? sizeof(uint16_t) : sizeof(uint32_t));
    const void *indices = m->indices;
    if (small) {
        uint16_t *narrow = upload_room(index_bytes);
        for (uint32_t i = 0; i < m->index_count; i++) narrow[i] = (uint16_t)m->indices[i];
        indices = narrow;
    }
    if (g->indices && index_bytes <= g->index_capacity) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g->indices);
        glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, (GLsizeiptr)index_bytes, indices);
    } else {
        if (g->indices) glDeleteBuffers(1, &g->indices);
        g->indices = make_buffer(GL_ELEMENT_ARRAY_BUFFER, indices, index_bytes, false);
        g->index_capacity = (uint32_t)index_bytes;
    }
    g->id = m->id;
    g->other = m->other;
    g->version = m->version;
    g->space = m->space;
    g->epoch = m->epoch;
    g->index_count = m->index_count;
    g->index_type = small ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
    return g;
}

// The list's matrices into the instance buffer, which grows as they need,
// and its 3D meshes onto the GPU, before anything draws.
static void upload_meshes_3d(const tide_draw_list *list)
{
    bool any = false;
    for (uint32_t i = 0; i < step_count; i++) {
        if (steps[i].kind != STEP_MESH_3D) continue;
        if (!any) {
            meshes_3d_start();
            glBindVertexArray(mesh_3d_gpu.vao);
        }
        any = true;
        mesh_for(list, steps[i].mesh);
    }
    if (!any) return;
    glBindVertexArray(0);
    if (list->matrix_count > mesh_3d_gpu.capacity) {
        uint32_t capacity = mesh_3d_gpu.capacity ? mesh_3d_gpu.capacity : 1024u;
        while (capacity < list->matrix_count) capacity *= 2u;
        if (mesh_3d_gpu.instances) glDeleteBuffers(1, &mesh_3d_gpu.instances);
        mesh_3d_gpu.instances = make_buffer(GL_ARRAY_BUFFER, NULL, (size_t)capacity * sizeof(tide_float4x4), true);
        mesh_3d_gpu.capacity = capacity;
    }
    glBindBuffer(GL_ARRAY_BUFFER, mesh_3d_gpu.instances);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)((size_t)list->matrix_count * sizeof(tide_float4x4)), list->matrices);
}

// Lets go of the 3D meshes the frame didn't draw.
static void forget_meshes_3d(void)
{
    for (uint32_t i = 0; i < gpu_mesh_count; i++) {
        if (gpu_meshes[i].vertices && gpu_meshes[i].frame != gpu_frame) {
            glDeleteBuffers(1, &gpu_meshes[i].vertices);
            glDeleteBuffers(1, &gpu_meshes[i].indices);
            gpu_meshes[i] = (gpu_mesh){0};
        }
    }
}

// This list's triangles, text's too, into their buffer, which grows as they need.
static void upload_meshes(void)
{
    if (mesh_vertex_count == 0) return;
    meshes_start();
    if (mesh_vertex_count > mesh_gpu.capacity) {
        uint32_t capacity = mesh_gpu.capacity ? mesh_gpu.capacity : 4096u;
        while (capacity < mesh_vertex_count) capacity *= 2u;
        glBindVertexArray(mesh_gpu.vao);
        if (mesh_gpu.vertices) glDeleteBuffers(1, &mesh_gpu.vertices);
        mesh_gpu.vertices = make_buffer(GL_ARRAY_BUFFER, NULL, (size_t)capacity * sizeof(mesh_vertex), true);
        mesh_gpu.capacity = capacity;
        attribute(MESH_POSITION, 2, false, sizeof(mesh_vertex), offsetof(mesh_vertex, position));
        attribute(MESH_UV, 2, false, sizeof(mesh_vertex), offsetof(mesh_vertex, uv));
        attribute(MESH_COLOR, 4, true, sizeof(mesh_vertex), offsetof(mesh_vertex, color));
        glBindVertexArray(0);
    }
    glBindBuffer(GL_ARRAY_BUFFER, mesh_gpu.vertices);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)((size_t)mesh_vertex_count * sizeof(mesh_vertex)), mesh_vertices);
}

// The font atlas onto the GPU, as the list's text left it.
static void upload_atlas(void)
{
    int width = 0, height = 0;
    uint32_t version = 0;
    const uint8_t *pixels = tide_font_atlas(&width, &height, &version);
    if (!pixels || (text_gpu.atlas && text_gpu.version == version)) return;
    if (!text_gpu.atlas) {
        glGenTextures(1, &text_gpu.atlas);
        glBindTexture(GL_TEXTURE_2D, text_gpu.atlas);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        // Stretched glyphs blend; the others land on the target's pixels
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    glBindTexture(GL_TEXTURE_2D, text_gpu.atlas);
    if (width == text_gpu.width && height == text_gpu.height) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RED, GL_UNSIGNED_BYTE, pixels);
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, pixels);
    }
    text_gpu.width = width;
    text_gpu.height = height;
    text_gpu.version = version;
}

static GLuint make_texture(const void *pixels, const int width, const int height)
{
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return texture;
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
    if (g->gl && g->version == t->version && g->width == t->width && g->height == t->height && g->epoch == t->epoch) {
        return g;
    }
    if (g->gl && g->width == t->width && g->height == t->height) {
        glBindTexture(GL_TEXTURE_2D, g->gl);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, t->width, t->height, GL_RGBA, GL_UNSIGNED_BYTE, t->pixels);
    } else {
        if (g->gl) glDeleteTextures(1, &g->gl);
        g->gl = make_texture(t->pixels, t->width, t->height);
        g->filter = -1;
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
            if (gpu_textures[i].gl) glDeleteTextures(1, &gpu_textures[i].gl);
            continue;
        }
        gpu_textures[kept++] = gpu_textures[i];
    }
    gpu_texture_count = kept;
    gpu_frame++;
}

// Binds the GPU's texture a mesh step samples, set to sample it as the step does.
static void bind_step_texture(const tide_draw_list *list, const draw_step *s)
{
    GLuint texture = white_texture;
    if (s->texture) {
        gpu_texture *g = texture_for(&list->textures[s->texture - 1u]);
        if (g->gl) {
            glBindTexture(GL_TEXTURE_2D, g->gl);
            if (g->filter != (int)s->filter) {
                const GLint filter = s->filter == TIDE_FILTER_POINT ? GL_NEAREST : GL_LINEAR;
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
                g->filter = (int)s->filter;
            }
            texture = g->gl;
        }
    }
    glBindTexture(GL_TEXTURE_2D, texture);
}

// From the target's logical pixels, from its top left with y down, to clip
// space, as OpenGL's matrices go: column by column.
static void screen_matrix(float m[16])
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = 2.0f / (float)screen_width;
    m[5] = -2.0f / (float)screen_height;
    m[10] = 1.0f;
    m[12] = -1.0f;
    m[13] = 1.0f;
    m[15] = 1.0f;
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
    glEnable(GL_DEPTH_TEST);
    glUseProgram(mesh_3d_gpu.program);
    glUniformMatrix4fv(mesh_3d_gpu.camera, 1, GL_FALSE, &world_to_clip.c0.x);
    glUniform1i(mesh_3d_gpu.pixels, 0);
    bind_step_texture(list, s);
    glBindVertexArray(mesh_3d_gpu.vao);
    glBindBuffer(GL_ARRAY_BUFFER, g->vertices);
    attribute(MESH_3D_POSITION, 3, false, sizeof(mesh_3d_vertex), offsetof(mesh_3d_vertex, position));
    attribute(MESH_3D_UV, 2, false, sizeof(mesh_3d_vertex), offsetof(mesh_3d_vertex, uv));
    attribute(MESH_3D_COLOR, 4, true, sizeof(mesh_3d_vertex), offsetof(mesh_3d_vertex, color));
    glBindBuffer(GL_ARRAY_BUFFER, mesh_3d_gpu.instances);
    for (int i = 0; i < 4; i++) {
        attribute(MESH_3D_MODEL + i, 4, false, sizeof(tide_float4x4),
                  (size_t)s->first * sizeof(tide_float4x4) + (size_t)i * sizeof(tide_float4));
    }
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g->indices);
    glDrawElementsInstanced(GL_TRIANGLES, (GLsizei)g->index_count, g->index_type, NULL, (GLsizei)s->count);
    glBindVertexArray(0);
    glDisable(GL_DEPTH_TEST); // Nothing else tests depth
}

static void draw_mesh(const tide_draw_list *list, const draw_step *s, const float *screen)
{
    if (s->count == 0) return;
    glUseProgram(mesh_gpu.program);
    glUniformMatrix4fv(mesh_gpu.screen, 1, GL_FALSE, screen);
    glUniform1i(mesh_gpu.pixels, 0);
    bind_step_texture(list, s);
    glBindVertexArray(mesh_gpu.vao);
    glDrawArrays(GL_TRIANGLES, (GLint)s->first, (GLsizei)s->count);
    glBindVertexArray(0);
}

static void draw_text(const draw_step *s, const float *screen)
{
    if (s->count == 0 || !text_gpu.atlas) return;
    glUseProgram(text_gpu.program);
    glUniformMatrix4fv(text_gpu.screen, 1, GL_FALSE, screen);
    glUniform1i(text_gpu.pixels, 0);
    glUniform2f(text_gpu.texel, 1.0f / (float)text_gpu.width, 1.0f / (float)text_gpu.height);
    glBindTexture(GL_TEXTURE_2D, text_gpu.atlas);
    glBindVertexArray(mesh_gpu.vao);
    glDrawArrays(GL_TRIANGLES, (GLint)s->first, (GLsizei)s->count);
    glBindVertexArray(0);
}

static int nearest_pixel(const float v)
{
    return (int)floorf(v + 0.5f);
}

// Only what's inside the step's rect draws from here on, or everything again.
static void clip(const draw_step *s)
{
    if (s->kind == STEP_NO_CLIP) {
        glDisable(GL_SCISSOR_TEST);
        return;
    }
    // OpenGL's are the target's pixels, from the bottom left
    const tide_float2 scale = pixel_scale();
    const int height = offscreen ? screen_height : pixel_height;
    const int left = nearest_pixel(s->at.x * scale.x), right = nearest_pixel(s->to.x * scale.x);
    const int top = nearest_pixel(s->at.y * scale.y), bottom = nearest_pixel(s->to.y * scale.y);
    glEnable(GL_SCISSOR_TEST);
    glScissor(left, height - bottom, right > left ? right - left : 0, bottom > top ? bottom - top : 0);
}

// The instance buffer's attributes, from shape `first` on.
static void point_at(const uint32_t first)
{
    const size_t at = (size_t)first * sizeof(shape);
    glBindBuffer(GL_ARRAY_BUFFER, shape_gpu.instances);
    attribute(SHAPE_SHAPE, 4, false, sizeof(shape), at + offsetof(shape, a));
    attribute(SHAPE_COLOR, 4, true, sizeof(shape), at + offsetof(shape, color));
    attribute(SHAPE_KIND, 1, false, sizeof(shape), at + offsetof(shape, kind));
}

// This list's shapes into the instance buffer, which grows as they need.
static void upload_shapes(void)
{
    if (shape_count == 0) return;
    shapes_start();
    if (shape_count > shape_gpu.capacity) {
        uint32_t capacity = shape_gpu.capacity ? shape_gpu.capacity : 1024u;
        while (capacity < shape_count) capacity *= 2u;
        if (shape_gpu.instances) glDeleteBuffers(1, &shape_gpu.instances);
        shape_gpu.instances = make_buffer(GL_ARRAY_BUFFER, NULL, (size_t)capacity * sizeof(shape), true);
        shape_gpu.capacity = capacity;
    }
    glBindBuffer(GL_ARRAY_BUFFER, shape_gpu.instances);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)((size_t)shape_count * sizeof(shape)), shapes);
}

static void draw_shapes(const uint32_t first, const uint32_t count, const float *screen)
{
    glUseProgram(shape_gpu.program);
    glUniformMatrix4fv(shape_gpu.screen, 1, GL_FALSE, screen);
    glBindVertexArray(shape_gpu.vao);
    point_at(first);
    glDrawArraysInstanced(GL_TRIANGLES, 0, 6, (GLsizei)count);
    glBindVertexArray(0);
}

static void clear(const rgba color)
{
    glClearColor((float)color.r / 255.0f, (float)color.g / 255.0f, (float)color.b / 255.0f, (float)color.a / 255.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

// Draws the list into the target that's bound: the window, or with
// `offscreen` set, pixels of its own. With `over`, it goes over what's there
// (the overlay), and leaves the frame's camera, textures and meshes as they are.
static void draw_list(const tide_draw_list *list, const bool over)
{
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

    // What the steps draw with is set here, whatever drew before: a list of
    // another target, or on the web, the program this one took over from.
    glViewport(0, 0, offscreen ? screen_width : pixel_width, offscreen ? screen_height : pixel_height);
    glDisable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_CULL_FACE); // Either side of a triangle draws
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glActiveTexture(GL_TEXTURE0);
    if (!white_texture) white_texture = make_texture((const uint8_t[4]){255, 255, 255, 255}, 1, 1);

    upload_shapes();
    upload_meshes();
    upload_meshes_3d(list);
    upload_atlas();
    float screen[16];
    screen_matrix(screen);
    if (!over) clear((rgba){0, 0, 0, 255}); // Every frame starts black; Draw.Clear picks another color
    bool clipped = false;
    for (uint32_t i = 0; i < step_count; i++) {
        const draw_step *s = &steps[i];
        switch (s->kind) {
        case STEP_SHAPES:
            draw_shapes(s->first, s->count, screen);
            break;
        case STEP_CLEAR:
            clear(s->color);
            break;
        case STEP_MESH:
            draw_mesh(list, s, screen);
            break;
        case STEP_MESH_3D:
            draw_mesh_3d(list, s);
            break;
        case STEP_CLIP:
        case STEP_NO_CLIP:
            clip(s);
            clipped = s->kind == STEP_CLIP;
            break;
        default:
            draw_text(s, screen);
            break;
        }
    }
    if (clipped) clip(&(draw_step){.kind = STEP_NO_CLIP}); // What's drawn after the list isn't the list's to clip
    if (!over) {
        forget_meshes_3d();
        forget_textures();
    }
}

void tide_platform_draw(const tide_draw_list *list)
{
    // Nobody sees it while the window is minimized or, on the web, the page
    // is hidden, and frames go on meanwhile.
    if (!tide_window_unseen()) draw_list(list, false);
}

tide_float2 tide_platform_world_to_screen(const tide_float2 world)
{
    return to_screen(&last_camera, world);
}

tide_float2 tide_platform_screen_size(void)
{
    return tide_f2((float)screen_width, (float)screen_height);
}

float tide_platform_measure_text(const char *text, const float size)
{
    return tide_font_measure(text, size);
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
        widths[count] = ceilf(tide_font_measure(lines[count], size));
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
    draw_list(&overlay, true);
}

// What tide_platform_read_pixels draws into: pixels of its own, as many as
// the window's logical ones, with depth. Kept while the window keeps its size.
static struct {
    GLuint framebuffer, color, depth;
    int width, height;
    uint8_t *pixels; // Read back: RGBA, rows from the bottom
} target;

void tide_platform_read_pixels(const tide_draw_list *list, const tide_float2 *points, const int count, uint32_t *out)
{
    const int width = screen_width, height = screen_height;
    if (!target.framebuffer || target.width != width || target.height != height) {
        if (target.framebuffer) {
            glDeleteFramebuffers(1, &target.framebuffer);
            glDeleteRenderbuffers(1, &target.color);
            glDeleteRenderbuffers(1, &target.depth);
        }
        glGenFramebuffers(1, &target.framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
        glGenRenderbuffers(1, &target.color);
        glBindRenderbuffer(GL_RENDERBUFFER, target.color);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, target.color);
        glGenRenderbuffers(1, &target.depth);
        glBindRenderbuffer(GL_RENDERBUFFER, target.depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, target.depth);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            fprintf(stderr, "tide: no framebuffer to render offscreen in\n");
            abort();
        }
        target.pixels = tide_realloc(target.pixels, (size_t)target.width * (size_t)target.height * 4u,
                                     (size_t)width * (size_t)height * 4u);
        target.width = width;
        target.height = height;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
    offscreen = true;
    draw_list(list, false);
    offscreen = false;
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, target.pixels);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    for (int i = 0; i < count; i++) {
        const int x = (int)points[i].x, y = height - 1 - (int)points[i].y;
        uint32_t pixel = 0;
        if (x >= 0 && x < width && y >= 0 && y < height) {
            const uint8_t *p = target.pixels + ((size_t)y * (size_t)width + (size_t)x) * 4u;
            pixel = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
        }
        out[i] = pixel;
    }
}
