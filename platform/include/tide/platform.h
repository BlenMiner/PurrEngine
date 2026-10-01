#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "tide/devices.h"
#include "tide/draw.h"
#include "tide/gui.h"
#include "tide/jobs.h"
#include "tide/net.h"

// The platform layer: a window, the frame loop and input devices, on raylib for
// now. Hosts use it; the simulation never does (see AGENTS.md, Rendering and
// platform). This header doesn't include raylib, and a file that includes a
// game's generated header must not either: raylib's names (Transform, Camera,
// Rectangle...) would clash with the game's components.
//
// Temporary implementation written by Claude; the project owner takes it over
// later.
//
// Pixels here are the display's logical pixels, as CSS pixels are on the web:
// on a display scaled to 150%, one is 1.5 of the screen's real pixels wide.

typedef struct tide_window_desc {
    const char *title;
    // The starting size in pixels. The user can resize and maximize the window;
    // on the web, the canvas fills the page instead.
    int width;
    int height;
    // For automated tests. On desktop, no visible window. On the web, frames
    // run on timers, because browsers stop animation frames in headless and
    // background pages.
    bool hidden;
} tide_window_desc;

// Opens the window and its graphics context.
void tide_platform_open(const tide_window_desc *desc);

// Returned by a frame function to keep going. Any other value ends the program
// with that exit code.
#define TIDE_KEEP_RUNNING (-1)

// Runs once per display frame, between raylib's BeginDrawing and EndDrawing.
// `seconds` is the time since the previous frame.
typedef int (*tide_frame_fn)(void *user, float seconds);

// Runs frames until one returns an exit code or the window is closed (exit
// code 0), then closes the window and ends the program. Never returns, because
// on the web the browser drives the frames: main can't wait for the loop.
_Noreturn void tide_platform_run(tide_frame_fn frame, void *user);

// Updates the devices with the input since the previous frame. Call once per
// frame, before sampling input. The characters typed are only the ones since
// this call's previous one.
//
// Keys are read by physical position on every platform. Axes follow Unity: y
// is positive up for sticks and the mouse, and mouse position is in window
// pixels from the bottom left.
void tide_platform_poll(tide_devices *devices);

// Renders a frame's draw list (see tide/draw.h), such as the one
// tide_frame fills. Call from the frame function.
void tide_platform_draw(const tide_draw_list *list);

// Where a world position lands on screen, in raylib's window pixels (from the
// top left), through the camera the last tide_platform_draw ended with. For
// tools and tests.
tide_float2 tide_platform_world_to_screen(tide_float2 world);

// Draws `text` in the window's top right corner, over the frame. For debug
// overlays such as frame rates.
void tide_platform_draw_overlay(const char *text);

// Frames per second, averaged over the last frames.
int tide_platform_fps(void);

// The window's size in pixels, which tide_gui_begin takes.
tide_float2 tide_platform_screen_size(void);

// The width of `text` drawn `size` tall with the platform's font, in the same
// units: the tide_measure_fn that tide_gui_begin takes.
float tide_platform_measure_text(const char *text, float size);

// Renders `list` offscreen at the window's size, then reads the pixels at
// `points` (window pixels from the top left) as 0xRRGGBBAA. For tests: proves
// the whole drawing path works without a visible window.
void tide_platform_read_pixels(const tide_draw_list *list, const tide_float2 *points, int count, uint32_t *rgba);

// Networking for sessions (tide/session.h): a UDP transport, taking datagrams
// on `port` (0: any free one). False where there's no network, like the web
// for now, or when the port is taken.
bool tide_platform_udp_open(uint16_t port, tide_transport *out);
// The same, reachable from this machine only: for tests, which then open no
// port to the network (and on Windows, ask nothing of the firewall).
bool tide_platform_udp_open_local(uint16_t port, tide_transport *out);

// An address from "192.168.1.5", "localhost:7777" or a name: its IPv4 address,
// with `default_port` if it says none. False if there's none.
bool tide_platform_resolve(const char *text, uint16_t default_port, tide_address *out);

// Rooms: matches players find by a code, like "K7QF2M", through the relay
// (relay/). The relay only introduces players; their datagrams go straight
// between them, on WebRTC: the browser's on the web, and our own on desktop
// (platform/src/rtc), so web and desktop players meet in the same rooms. A
// program is in one room at a time: opening one closes the last. Desktop
// games reach the relay at wss://relay.tide-engine.dev, with the system's
// TLS (on Linux, OpenSSL's libssl), or at $TIDE_RELAY.
//
// A room this machine hosts: a transport that takes the players who join it.
// It picks the room's code itself, so it has one at once.
bool tide_platform_room_host(tide_transport *out);
// Joins the room with `code`: a transport to its host, at `server`.
bool tide_platform_room_join(const char *code, tide_transport *out, tide_address *server);
// The room's code into `out`, "" while there's none: the room closed or
// failed, or its host can't reach the relay (it opens again once it can).
// It changes only if another room had it first, before anyone could join.
void tide_platform_room_code(char *out, size_t size);
// The room joined turned out not to be one, or its host couldn't be reached.
bool tide_platform_room_failed(void);
// The key of the room this machine hosts into `out`, "" for none: what lets
// the players of its match meet there again when it goes (see
// tide_session_set_room). Up to TIDE_ROOM_KEY_LENGTH letters and digits.
void tide_platform_room_key(char *out, size_t size);
// Host migration (see tide_session_migrating): goes to room `code` again,
// with its `key`. If its host is gone, the relay makes this machine the
// room's host; if not, or another machine got there first, it introduces this
// one to the host.
void tide_platform_room_migrate(const char *code, const char *key);
// ...and which it was, once the relay says: 1, this machine hosts the room
// now, and `out` takes the players who join it; 2, it joins the room's host,
// whom `out` reaches at `server`; -1, the room can't be reached; 0, not yet.
int tide_platform_room_migrated(tide_transport *out, tide_address *server);

// Takes the players who join a match this machine hosts: on UDP `port` (0 for
// any), where there's UDP, and in a room, in one transport. False only if
// there's neither.
bool tide_platform_host_open(uint16_t port, tide_transport *out);

// Threads to run ticks on (tide/jobs.h): a pool with one for each of the
// CPU's cores, the caller's among them, made on first use. NULL with one core,
// and on the web, which runs everything on one thread.
const tide_jobs *tide_platform_jobs(void);
