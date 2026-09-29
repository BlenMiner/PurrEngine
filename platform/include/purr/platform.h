#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "purr/devices.h"
#include "purr/draw.h"

// The platform layer: a window, the frame loop and input devices, on raylib for
// now. Hosts use it; the simulation never does (see AGENTS.md, Rendering and
// platform). This header doesn't include raylib, and a file that includes a
// game's generated header must not either: raylib's names (Transform, Camera,
// Rectangle...) would clash with the game's components.
//
// Temporary implementation written by Claude; the project owner takes it over
// later.

typedef struct purr_window_desc {
    const char *title;
    // The starting size in pixels. The user can resize and maximize the window;
    // on the web, the canvas fills the page instead.
    int width;
    int height;
    // For automated tests. On desktop, no visible window. On the web, frames
    // run on timers, because browsers stop animation frames in headless and
    // background pages.
    bool hidden;
} purr_window_desc;

// Opens the window and its graphics context.
void purr_platform_open(const purr_window_desc *desc);

// Returned by a frame function to keep going. Any other value ends the program
// with that exit code.
#define PURR_KEEP_RUNNING (-1)

// Runs once per display frame, between raylib's BeginDrawing and EndDrawing.
// `seconds` is the time since the previous frame.
typedef int (*purr_frame_fn)(void *user, float seconds);

// Runs frames until one returns an exit code or the window is closed (exit
// code 0), then closes the window and ends the program. Never returns, because
// on the web the browser drives the frames: main can't wait for the loop.
_Noreturn void purr_platform_run(purr_frame_fn frame, void *user);

// Updates the devices with the input since the previous frame. Call once per
// frame, before sampling input.
//
// Keys are read by physical position on every platform. Axes follow Unity: y
// is positive up for sticks and the mouse, and mouse position is in window
// pixels from the bottom left.
void purr_platform_poll(purr_devices *devices);

// Renders a frame's draw list (see purr/draw.h), such as the one
// purr_frame fills. Call from the frame function.
void purr_platform_draw(const purr_draw_list *list);

// Where a world position lands on screen, in raylib's window pixels (from the
// top left), through the camera the last purr_platform_draw ended with. For
// tools and tests.
purr_float2 purr_platform_world_to_screen(purr_float2 world);

// Draws `text` in the window's top right corner, over the frame. For debug
// overlays such as frame rates.
void purr_platform_draw_overlay(const char *text);

// Frames per second, averaged over the last frames.
int purr_platform_fps(void);

// Renders `list` offscreen at the window's size, then reads the pixels at
// `points` (window pixels from the top left) as 0xRRGGBBAA. For tests: proves
// the whole drawing path works without a visible window.
void purr_platform_read_pixels(const purr_draw_list *list, const purr_float2 *points, int count, uint32_t *rgba);
