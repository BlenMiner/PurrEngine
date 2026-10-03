#pragma once

// Text, in the platform's font (font/Tuffy.ttf, built into the library),
// through stb_truetype: how wide it is, and its glyphs' pixels, drawn the
// first time each is needed at a size into an atlas the renderer draws from
// (platform.c). Text `size` tall has its lines that far from top to bottom,
// from the font's ascent to its descent.

#include <stdbool.h>
#include <stdint.h>

// The next code point of UTF-8 `*text`, moving it past it; 0 at its end.
// Bytes that aren't UTF-8 read as U+FFFD, one each.
uint32_t tide_utf8_next(const char **text);

// How far a code point moves the pen, for text 1 tall.
float tide_font_advance(uint32_t codepoint);

// From one line's top to the next's, for text 1 tall.
float tide_font_line(void);

// The width of `text` drawn `size` tall, in the units `size` is in: its
// widest line's.
float tide_font_measure(const char *text, float size);

// The tallest a glyph is drawn into the atlas, in pixels: taller text
// stretches these.
#define TIDE_FONT_MAX_PIXELS 192

// A glyph drawn for lines `pixels` tall: where its pixels are in the atlas,
// and where they go from the pen at the line's top left. `width` is 0 for one
// with nothing to draw, like a space.
typedef struct tide_font_glyph {
    uint32_t codepoint;
    uint16_t pixels; // 0 for a place in the table nothing is in
    uint16_t x, y, width, height;
    int16_t left, top;
} tide_font_glyph;

// False when the atlas has no room left for it: it's drawn from the next
// frame on, once tide_font_frame made room.
bool tide_font_glyph_for(uint32_t codepoint, int pixels, tide_font_glyph *out);

// The atlas: a byte a pixel, how much of it its glyph covers, `*width` a row.
// `*version` changes whenever its pixels or its size do.
const uint8_t *tide_font_atlas(int *width, int *height, uint32_t *version);

// Before a frame's text: lets every glyph go, if the last frame needed one
// the atlas had no room for. No glyph a frame asked for goes before it ends.
void tide_font_frame(void);
