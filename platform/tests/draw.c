// The renderer, through the whole drawing path (the draw list, the platform
// layer, OpenGL or WebGL), read back from an offscreen render.
//
// It checks that each shape lands where it should, that commands draw in
// the order they were recorded whatever the renderer batches together, that
// a frame can draw far more than a list used to hold, and that meshes draw
// their triangles with their corners' colors, their textures and the clip.
// `--bench` (or `?bench` on the web) times frames of many shapes instead.

#include <stdio.h>
#include <string.h>

#include "tide/platform.h"
#include "tide/time.h"

#define WIDTH 256
#define HEIGHT 352

static tide_draw_list list;
static int failures;

// The window's size: the one asked for on desktop and the web, and the
// screen's on a phone, where the checks stay in its top left corner.
static tide_float2 screen = {WIDTH, HEIGHT};

// The world position at window pixel (x, y), through the frame's first
// camera: the origin at the middle, a world unit a pixel, y up.
static tide_float2 at(const float x, const float y)
{
    return tide_f2(x - screen.x * 0.5f, screen.y * 0.5f - y);
}

static uint32_t rgba(const tide_color c)
{
    const float channels[4] = {c.r, c.g, c.b, c.a};
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v = v << 8 | (uint32_t)(channels[i] * 255.0f + 0.5f);
    return v;
}

// Whether two colors' red, green and blue are within `slack` of each other.
static bool near(const uint32_t a, const uint32_t b, const int slack)
{
    for (int shift = 8; shift < 32; shift += 8) {
        const int d = (int)(a >> shift & 0xFFu) - (int)(b >> shift & 0xFFu);
        if (d > slack || d < -slack) return false;
    }
    return true;
}

static const tide_color background = {0.1f, 0.1f, 0.1f, 1.0f};
static const tide_color red = {1.0f, 0.0f, 0.0f, 1.0f};
static const tide_color blue = {0.0f, 0.0f, 1.0f, 1.0f};
static const tide_color green = {0.0f, 1.0f, 0.0f, 1.0f};
static const tide_color white = {1.0f, 1.0f, 1.0f, 1.0f};
static const tide_color yellow = {1.0f, 1.0f, 0.0f, 1.0f};
static const tide_color cyan = {0.0f, 1.0f, 1.0f, 1.0f};
static const tide_color orange = {1.0f, 0.5f, 0.0f, 1.0f};
static const tide_color magenta = {1.0f, 0.0f, 1.0f, 1.0f};
static const tide_color purple = {0.5f, 0.0f, 1.0f, 1.0f};

#define BAND 20000u // Far past the 16384 commands a list used to hold
#define BAND_TOP 272

static tide_color band_color(const uint32_t i)
{
    return i % 2u ? white : orange;
}

static void scene(void)
{
    tide_draw_reset(&list);
    tide_draw_clear(&list, background);
    tide_draw_rect(&list, at(128, 28), tide_f2(16, 16), yellow); // Cleared by the next one
    tide_draw_clear(&list, background);

    // A band of pixels, one rect each, along the bottom
    for (uint32_t i = 0; i < BAND; i++) {
        const float x = (float)(i % WIDTH) + 0.5f;
        const float y = (float)(BAND_TOP + i / WIDTH) + 0.5f;
        tide_draw_rect(&list, at(x, y), tide_f2(1, 1), band_color(i));
    }

    tide_draw_rect(&list, at(48, 48), tide_f2(20, 20), red);
    tide_draw_rect(&list, at(53, 48), tide_f2(10, 20), blue); // Over the red one's right half
    tide_draw_circle(&list, at(208, 48), 15, green);

    // Text between shapes: over the orange rect, and under the magenta one
    tide_draw_rect(&list, at(128, 100), tide_f2(60, 30), orange);
    tide_draw_text(&list, "MMMM", at(102, 88), 24, white);
    tide_draw_rect(&list, at(113, 100), tide_f2(30, 30), magenta);

    tide_draw_line(&list, at(28, 127.5f), at(228, 127.5f), cyan);
    tide_draw_wire_rect(&list, at(48, 160), tide_f2(40, 40), white);
    tide_draw_wire_circle(&list, at(208, 160), 20, yellow);

    // Another camera: 2 pixels a world unit
    tide_draw_camera(&list, tide_f2(1000, 1000), screen.y / 4.0f);
    tide_draw_rect(&list, tide_f2(1040, 970), tide_f2(4, 4), purple);

    // The GUI's pixels: from the top left, y down
    tide_draw_screen(&list);
    tide_draw_rect(&list, tide_f2(16, 250), tide_f2(10, 10), green);
}

typedef struct check {
    float x, y;
    tide_color color;
    const char *what;
} check;

static void expect(const bool ok, const char *what)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) failures++;
}

static int run_checks(void)
{
    scene();
    const uint32_t last = BAND - 1u;
    const check checks[] = {
        {128, 28, background, "a clear covers what was drawn before it"},
        {last % WIDTH + 0.5f, BAND_TOP + last / WIDTH + 0.5f, band_color(last), "the band's last pixel is there"},
        {last % WIDTH - 0.5f, BAND_TOP + last / WIDTH + 0.5f, band_color(last - 1u), "and the one before it"},
        {last % WIDTH + 1.5f, BAND_TOP + last / WIDTH + 0.5f, background, "and nothing after it"},
        {42, 48, red, "a rect"},
        {54, 48, blue, "a rect over another"},
        {208, 48, green, "a circle's middle"},
        {208, 68, background, "and outside it"},
        {128, 127.5f, cyan, "a line"},
        {128, 125.5f, background, "and beside it"},
        {28.5f, 160, white, "a wire rect's edge"},
        {48, 160, background, "and its middle"},
        {208.5f, 140.5f, yellow, "a wire circle's edge"},
        {208, 160, background, "and its middle"},
        {screen.x * 0.5f + 80, screen.y * 0.5f + 60, purple, "a rect through another camera"},
        {16, 250, green, "a rect in the GUI's pixels"},
    };
    enum { CHECKS = sizeof checks / sizeof checks[0] };
    // The text test's region: the magenta rect, and the orange one's right half
    enum { TEXT_W = 28, TEXT_H = 28, POINTS = CHECKS + 2 * TEXT_W * TEXT_H };
    static tide_float2 points[POINTS];
    static uint32_t got[POINTS];
    for (int i = 0; i < CHECKS; i++) points[i] = tide_f2(checks[i].x, checks[i].y);
    for (int y = 0; y < TEXT_H; y++) {
        for (int x = 0; x < TEXT_W; x++) {
            points[CHECKS + y * TEXT_W + x] = tide_f2(99.5f + (float)x, 86.5f + (float)y);
            points[CHECKS + TEXT_W * TEXT_H + y * TEXT_W + x] = tide_f2(129.5f + (float)x, 86.5f + (float)y);
        }
    }
    tide_platform_read_pixels(&list, points, POINTS, got);

    for (int i = 0; i < CHECKS; i++) {
        const bool ok = near(got[i], rgba(checks[i].color), 8);
        printf("%s: %s (0x%08X, expected 0x%08X)\n", ok ? "ok" : "FAIL", checks[i].what, (unsigned)got[i],
               (unsigned)rgba(checks[i].color));
        if (!ok) failures++;
    }
    int covered = 0;
    int text = 0;
    for (int i = 0; i < TEXT_W * TEXT_H; i++) {
        if (near(got[CHECKS + i], rgba(magenta), 0)) covered++;
        if (!near(got[CHECKS + TEXT_W * TEXT_H + i], rgba(orange), 0)) text++;
    }
    expect(covered == TEXT_W * TEXT_H, "a rect drawn after text covers it");
    expect(text > 20, "text drawn after a rect shows over it");
    return failures == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Meshes: triangles with a color at each corner, textures and the clip

// A quad from `top_left` to `bottom_right`, as the list's units have them:
// its left corners `left` and its right ones `right`, and the texture's first
// pixel at its top left.
static void quad(const tide_float2 top_left, const tide_float2 bottom_right, const tide_color left, const tide_color right,
                 const tide_texture *texture, const tide_filter filter, const bool clockwise)
{
    const tide_vertex vertices[4] = {
        {top_left, {0.0f, 0.0f}, left},
        {{bottom_right.x, top_left.y}, {1.0f, 0.0f}, right},
        {bottom_right, {1.0f, 1.0f}, right},
        {{top_left.x, bottom_right.y}, {0.0f, 1.0f}, left},
    };
    static const uint32_t one_way[6] = {0, 1, 2, 0, 2, 3}, other_way[6] = {2, 1, 0, 3, 2, 0};
    tide_draw_mesh(&list, vertices, 4, clockwise ? one_way : other_way, 6, texture, filter);
}

// Red, green, blue and white, two by two, and yellow beside a clear pixel
static uint8_t four[2 * 2 * 4] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
static const uint8_t two[2 * 1 * 4] = {255, 255, 0, 255, 0, 0, 0, 0};
static tide_texture four_texture = {four, 2, 2, 1};
static const tide_texture two_texture = {two, 2, 1, 1};

static void mesh_scene(void)
{
    const tide_color gray = {0.5f, 1.0f, 1.0f, 1.0f};
    tide_draw_reset(&list);
    tide_draw_clear(&list, background);

    // Colors blend from corner to corner
    quad(at(16, 16), at(80, 80), red, blue, NULL, TIDE_FILTER_BILINEAR, true);
    // A texture, pixel by pixel and blended, and times its corners' colors
    quad(at(96, 16), at(160, 80), white, white, &four_texture, TIDE_FILTER_POINT, true);
    quad(at(176, 16), at(240, 80), white, white, &four_texture, TIDE_FILTER_BILINEAR, false);
    quad(at(16, 96), at(80, 160), gray, gray, &four_texture, TIDE_FILTER_POINT, true);
    quad(at(96, 96), at(160, 128), white, white, &two_texture, TIDE_FILTER_POINT, true);

    // A clip, in the world's units: what's drawn after it only shows inside
    // it, a clear and shapes too
    const tide_float2 corner = at(176, 136);
    tide_draw_clip(&list, (tide_rect){corner.x, corner.y, 40, 40});
    tide_draw_clear(&list, cyan);
    quad(at(186, 106), at(206, 126), magenta, magenta, NULL, TIDE_FILTER_BILINEAR, true);
    tide_draw_rect(&list, at(226, 131), tide_f2(40, 6), orange);
    tide_draw_no_clip(&list);
    tide_draw_rect(&list, at(236, 150), tide_f2(10, 10), green);

    // In the order they were recorded, among shapes
    tide_draw_rect(&list, at(48, 240), tide_f2(64, 32), yellow);
    quad(at(16, 224), at(48, 256), blue, blue, NULL, TIDE_FILTER_BILINEAR, false);
    tide_draw_rect(&list, at(24, 240), tide_f2(16, 32), red);

    // On the screen: pixels from the top left, y down, the clip too
    tide_draw_screen(&list);
    tide_draw_clip(&list, (tide_rect){16, 176, 48, 32});
    quad(tide_f2(0, 170), tide_f2(100, 220), purple, purple, NULL, TIDE_FILTER_BILINEAR, true);
    tide_draw_no_clip(&list);
    quad(tide_f2(80, 176), tide_f2(112, 208), white, white, &four_texture, TIDE_FILTER_POINT, true);
    // ...and through a camera again
    tide_draw_camera(&list, tide_f2(0, 0), screen.y / 2.0f);
    quad(at(120, 176), at(136, 192), white, white, NULL, TIDE_FILTER_BILINEAR, true);
}

static void check_pixels(const check *checks, const int count)
{
    static tide_float2 points[64];
    static uint32_t got[64];
    for (int i = 0; i < count; i++) points[i] = tide_f2(checks[i].x, checks[i].y);
    tide_platform_read_pixels(&list, points, count, got);
    for (int i = 0; i < count; i++) {
        const bool ok = near(got[i], rgba(checks[i].color), 8);
        printf("%s: %s (0x%08X, expected 0x%08X)\n", ok ? "ok" : "FAIL", checks[i].what, (unsigned)got[i],
               (unsigned)rgba(checks[i].color));
        if (!ok) failures++;
    }
}

static void meshes(void)
{
    const tide_color dim = {0.5f, 0.0f, 0.0f, 1.0f}, pale = {0.5f, 1.0f, 1.0f, 1.0f};
    const tide_color between = {0.5f, 0.0f, 0.5f, 1.0f}, blend = {0.5f, 0.5f, 0.5f, 1.0f};
    mesh_scene();
    const check checks[] = {
        {17, 48, red, "a mesh's left corners' color"},
        {79, 48, blue, "and its right ones'"},
        {48, 48, between, "blended between them"},
        {112, 32, red, "a texture's first pixel, at uv (0, 0)"},
        {144, 32, green, "the one beside it"},
        {112, 64, blue, "the one below it"},
        {144, 64, white, "and its last"},
        {208, 48, blend, "a texture's pixels blended, between them"},
        {178, 18, red, "and not with the far edge's, at its edge"},
        {32, 112, dim, "a texture's pixel times its corners' color"},
        {64, 144, pale, "and another"},
        {112, 112, yellow, "another texture, wider than it's tall"},
        {144, 112, background, "and its clear pixel"},
        {196, 116, magenta, "a mesh inside the clip"},
        {180, 100, cyan, "a clear fills the clip"},
        {170, 116, background, "and nothing outside it"},
        {215, 100, cyan, "the clip's last column"},
        {216, 100, background, "and the one past it"},
        {180, 96, cyan, "the clip's first row"},
        {180, 95, background, "and the one before it"},
        {180, 135, cyan, "the clip's last row"},
        {180, 136, background, "and the one past it"},
        {210, 131, orange, "a rect inside the clip"},
        {230, 131, background, "and the rest of it, outside"},
        {236, 150, green, "a rect after the clip is off"},
        {24, 240, red, "a rect over a mesh"},
        {40, 240, blue, "a mesh over a rect"},
        {64, 240, yellow, "and the rect under it"},
        {40, 192, purple, "a mesh on the screen, inside the screen's clip"},
        {70, 192, background, "and nothing beside the clip"},
        {40, 212, background, "or below it"},
        {88, 184, red, "a texture on the screen: its first pixel at the top left"},
        {104, 200, white, "and its last at the bottom right"},
        {128, 184, white, "a mesh through a camera again"},
    };
    check_pixels(checks, (int)(sizeof checks / sizeof checks[0]));

    // Pixels that changed, at another version
    four[0] = 0, four[1] = 255, four[2] = 255;
    four_texture.version = 2;
    mesh_scene();
    const check changed[] = {
        {112, 32, cyan, "a texture's pixel that changed"},
        {144, 32, green, "and one that didn't"},
    };
    check_pixels(changed, 2);

    // ...and of another size
    four_texture.width = four_texture.height = 1;
    four_texture.version = 3;
    mesh_scene();
    const check resized[] = {{144, 64, cyan, "a texture that changed size"}};
    check_pixels(resized, 1);

    // A frame without it lets it go, and the next draws with it again
    tide_draw_reset(&list);
    tide_draw_clear(&list, background);
    const check none[] = {{144, 64, background, "a frame without meshes"}};
    check_pixels(none, 1);
    four[0] = 255, four[1] = 0, four[2] = 0;
    four_texture = (tide_texture){four, 2, 2, 4};
    mesh_scene();
    const check again[] = {
        {112, 32, red, "a texture drawn again after a frame without it"},
        {144, 64, white, "all of it"},
    };
    check_pixels(again, 2);

    // A host whose game was swapped for another build has the list forget its
    // textures: the new build's pixels show, though it names them as the old
    // one named its own
    four[0] = 255, four[1] = 255, four[2] = 0;
    tide_draw_forget(&list);
    mesh_scene();
    const check forgotten[] = {{112, 32, yellow, "a texture after the list forgot it"}};
    check_pixels(forgotten, 1);
}

// Rects side by side, a cell each as a grid's view draws them, leave no gap
// between them at any scale: not even where their shared edge falls exactly
// on a row or column of pixel centers, as it does every 8 cells at 3.125
// pixels a cell.
static void tiles_leave_no_gaps(void)
{
    static const float scales[] = {3.125f, 2.75f, 1.5f, 7.3f, 2.5f};
    static const tide_float2 centers[] = {{0.0f, 0.0f}, {0.3f, 0.7f}, {0.5f, 0.5f}, {-0.25f, 0.125f}, {0.0f, 0.2f}};
    enum { HALF = 20 }; // Cells each way from the middle
    for (size_t k = 0; k < sizeof scales / sizeof scales[0]; k++) {
        const float s = scales[k];
        tide_draw_reset(&list);
        tide_draw_clear(&list, background);
        tide_draw_camera(&list, centers[k], screen.y / (2.0f * s));
        for (int y = -HALF; y < HALF; y++) {
            for (int x = -HALF; x < HALF; x++) {
                tide_draw_rect(&list, tide_f2((float)x + 0.5f, (float)y + 0.5f), tide_f2(1, 1), orange);
            }
        }
        // Every pixel inside the tiles, but for a pixel's margin
        const float left = screen.x * 0.5f + (-HALF - centers[k].x) * s + 1.0f;
        const float right = screen.x * 0.5f + (HALF - centers[k].x) * s - 1.0f;
        const float top = screen.y * 0.5f - (HALF - centers[k].y) * s + 1.0f;
        const float bottom = screen.y * 0.5f - (-HALF - centers[k].y) * s - 1.0f;
        static tide_float2 points[WIDTH * HEIGHT];
        static uint32_t got[WIDTH * HEIGHT];
        int count = 0;
        for (int py = (int)top; py < (int)bottom && py < HEIGHT; py++) {
            for (int px = (int)left; px < (int)right && px < WIDTH; px++) {
                if (px >= 0 && py >= 0) points[count++] = tide_f2((float)px + 0.5f, (float)py + 0.5f);
            }
        }
        tide_platform_read_pixels(&list, points, count, got);
        int gaps = 0;
        for (int i = 0; i < count; i++) gaps += !near(got[i], rgba(orange), 0);
        char what[96];
        snprintf(what, sizeof what, "rects side by side leave no gaps at %.3f pixels a cell (%d missed)", (double)s, gaps);
        expect(gaps == 0, what);
    }
}

// ---------------------------------------------------------------------------
// --bench: frames of many shapes, each rendered offscreen and a pixel read
// back, which waits for the GPU to finish it, and works in a hidden window or
// page, where tide_platform_draw draws nothing.

#define BENCH_FRAMES 100

// 0: nothing (what reading back costs), 1: rects, 2: circles, 3: rects with text between, 4: quads of one mesh
static int bench_kind;
static int bench_frame;
static double bench_ms;

static void bench_scene(void)
{
    tide_draw_reset(&list);
    for (uint32_t i = 0; bench_kind > 0 && i < WIDTH * 256u; i++) {
        const tide_float2 p = at((float)(i % WIDTH) + 0.5f, (float)(i / WIDTH) + 0.5f);
        const tide_color c = {(float)(i % WIDTH) / WIDTH, (float)(i / WIDTH) / 256.0f, 0.5f, 1.0f};
        if (bench_kind == 4) {
            quad(tide_f2(p.x - 0.5f, p.y + 0.5f), tide_f2(p.x + 0.5f, p.y - 0.5f), c, c, &four_texture, TIDE_FILTER_POINT, true);
            continue;
        }
        if (bench_kind == 2) tide_draw_circle(&list, p, 0.6f, c);
        else tide_draw_rect(&list, p, tide_f2(1, 1), c);
        if (bench_kind == 3 && i % 1024u == 0) tide_draw_text(&list, "x", p, 10, white);
    }
}

static int bench(void)
{
    static const char *names[] = {"nothing", "65536 rects", "65536 circles", "65536 rects, text every 1024",
                                  "65536 textured quads"};
    bench_scene();
    const uint64_t start = tide_time_now_ns();
    const tide_float2 point = tide_f2(1, 1);
    uint32_t pixel;
    tide_platform_read_pixels(&list, &point, 1, &pixel);
    bench_ms += (double)(tide_time_now_ns() - start) / 1e6;
    if (++bench_frame < BENCH_FRAMES) return TIDE_KEEP_RUNNING;
    printf("%s: %.2f ms a frame\n", names[bench_kind], bench_ms / BENCH_FRAMES);
    bench_frame = 0;
    bench_ms = 0.0;
    return ++bench_kind < 5 ? TIDE_KEEP_RUNNING : 0;
}

static bool benching;

static int frame(void *user, const float seconds)
{
    (void)user, (void)seconds;
    screen = tide_platform_screen_size();
    if (benching) return bench();
    meshes();
    tiles_leave_no_gaps();
    return run_checks();
}

int main(const int argc, char **argv)
{
    benching = argc > 1 && strcmp(argv[1], "--bench") == 0;
    tide_platform_open(&(tide_window_desc){.title = "draw", .width = WIDTH, .height = HEIGHT, .hidden = true});
    tide_platform_run(frame, NULL);
}
