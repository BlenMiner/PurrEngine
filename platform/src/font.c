#include "font.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tide/page.h"

// stb_truetype, kept to this file (its functions are static), so a game's C
// can bring a copy of its own. Its warnings aren't ours to fix.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#pragma clang diagnostic pop

// The font's file, which the build makes an array of (cmake/embed.cmake).
extern const unsigned char tide_font_ttf[];

static stbtt_fontinfo font;
static bool started;
static float em;          // Font units in the font's size: what text's size is
static float baseline;    // From a line's top to its baseline, for text of size 1
static float line;        // From one line's top to the next's
static int advances[256]; // The first code points' advances in font units, which most text is

static int advance_of(const uint32_t codepoint)
{
    int advance = 0, bearing = 0;
    stbtt_GetCodepointHMetrics(&font, (int)codepoint, &advance, &bearing);
    return advance;
}

static void start(void)
{
    if (started) return;
    started = true;
    if (!stbtt_InitFont(&font, tide_font_ttf, stbtt_GetFontOffsetForIndex(tide_font_ttf, 0))) {
        fprintf(stderr, "tide: the platform's font isn't one\n");
        abort();
    }
    em = 1.0f / stbtt_ScaleForMappingEmToPixels(&font, 1.0f);
    int up = 0, down = 0, gap = 0;
    stbtt_GetFontVMetrics(&font, &up, &down, &gap);
    // A line is as tall as text's size. The font's own lines are taller, from
    // its ascent to its descent: that's in the size's middle, so capitals are
    // too, and what goes past (accents above, tails below) goes past evenly.
    baseline = 0.5f + (float)(up + down) * 0.5f / em;
    line = (float)(up - down + gap) / em;
    if (line < 1.2f) line = 1.2f;
    for (uint32_t c = 32; c < 256; c++) advances[c] = advance_of(c);
}

uint32_t tide_utf8_next(const char **text)
{
    const unsigned char *s = (const unsigned char *)*text;
    const uint32_t first = s[0];
    if (first == 0) return 0;
    *text += 1;
    if (first < 0x80u) return first;
    const int more = (first & 0xE0u) == 0xC0u ? 1 : (first & 0xF0u) == 0xE0u ? 2 : (first & 0xF8u) == 0xF0u ? 3 : 0;
    if (more == 0) return 0xFFFDu;
    uint32_t c = first & (0x3Fu >> more);
    for (int i = 1; i <= more; i++) {
        if ((s[i] & 0xC0u) != 0x80u) return 0xFFFDu; // Cut short, by the text's end too
        c = c << 6 | (s[i] & 0x3Fu);
    }
    *text += more;
    return c ? c : 0xFFFDu;
}

float tide_font_baseline(void)
{
    start();
    return baseline;
}

float tide_font_line(void)
{
    start();
    return line;
}

int tide_font_pixels(const float size, const float scale, float *stretch)
{
    const float wanted = size * scale;
    int pixels = (int)(wanted + 0.5f);
    if (pixels < 1) pixels = 1;
    *stretch = 1.0f;
    if (pixels > TIDE_FONT_MAX_PIXELS) {
        pixels = TIDE_FONT_MAX_PIXELS;
        *stretch = wanted / (float)TIDE_FONT_MAX_PIXELS;
    }
    return pixels;
}

// A code point's advance at `pixels`: whole pixels, so every glyph of a text
// lands on them. Nothing for control characters.
static int advance_pixels(const uint32_t codepoint, const int pixels)
{
    const int units = codepoint < 256u ? advances[codepoint] : advance_of(codepoint);
    return (int)((float)units * (float)pixels / em + 0.5f);
}

float tide_font_measure(const char *text, const float size, const float scale)
{
    start();
    if (!(size > 0.0f) || !(scale > 0.0f)) return 0.0f;
    float stretch;
    const int pixels = tide_font_pixels(size, scale, &stretch);
    int widest = 0, width = 0;
    for (uint32_t c; (c = tide_utf8_next(&text)) != 0;) {
        if (c == '\n') {
            if (width > widest) widest = width;
            width = 0;
        } else {
            width += advance_pixels(c, pixels);
        }
    }
    return (float)(width > widest ? width : widest) * stretch / scale;
}

// ---------------------------------------------------------------------------
// The atlas: glyphs side by side in rows, each as tall as its tallest, with a
// clear pixel between them, which stretched text blends their edges with. It
// grows downwards, which moves no glyph, up to a size every GPU takes.

enum { ATLAS_WIDTH = 2048, ATLAS_MAX_HEIGHT = 2048 };

static uint8_t *atlas;
static int atlas_height;
static int row_x = 1, row_y = 1, row_height;
static uint32_t atlas_version;
static bool atlas_full;

// The glyphs drawn so far, by code point and size: a hash table, a quarter
// of it free at least.
static tide_font_glyph *glyphs;
static uint32_t glyph_count, glyph_capacity;

static uint32_t slot_of(const tide_font_glyph *table, const uint32_t capacity, const uint32_t codepoint,
                        const uint16_t pixels)
{
    uint32_t i = (codepoint * 0x9E3779B1u ^ pixels * 0x85EBCA6Bu) & (capacity - 1u);
    while (table[i].pixels && (table[i].codepoint != codepoint || table[i].pixels != pixels)) i = (i + 1u) & (capacity - 1u);
    return i;
}

static void grow_glyphs(void)
{
    const uint32_t capacity = glyph_capacity ? glyph_capacity * 2u : 256u;
    tide_font_glyph *table = tide_alloc_zeroed(capacity, sizeof(tide_font_glyph));
    for (uint32_t i = 0; i < glyph_capacity; i++) {
        if (glyphs[i].pixels) table[slot_of(table, capacity, glyphs[i].codepoint, glyphs[i].pixels)] = glyphs[i];
    }
    free(glyphs);
    glyphs = table;
    glyph_capacity = capacity;
}

// A place for `width` by `height` pixels, false if the atlas has none left.
static bool place(const int width, const int height, uint16_t *x, uint16_t *y)
{
    if (width + 2 > ATLAS_WIDTH || height + 2 > ATLAS_MAX_HEIGHT) return false;
    int at_x = row_x, at_y = row_y, tallest = row_height;
    if (at_x + width + 1 > ATLAS_WIDTH) {
        at_x = 1;
        at_y += tallest;
        tallest = 0;
    }
    if (at_y + height + 1 > ATLAS_MAX_HEIGHT) return false;
    if (at_y + height + 1 > atlas_height) {
        int grown = atlas_height ? atlas_height : 256;
        while (at_y + height + 1 > grown) grown *= 2;
        atlas = tide_realloc(atlas, (size_t)atlas_height * ATLAS_WIDTH, (size_t)grown * ATLAS_WIDTH);
        memset(atlas + (size_t)atlas_height * ATLAS_WIDTH, 0, (size_t)(grown - atlas_height) * ATLAS_WIDTH);
        atlas_height = grown;
    }
    *x = (uint16_t)at_x;
    *y = (uint16_t)at_y;
    row_x = at_x + width + 1;
    row_y = at_y;
    row_height = height + 1 > tallest ? height + 1 : tallest;
    return true;
}

// A length of the glyph's outline, in font units, as whole pixels at `scale`:
// one at least, as a stem thinner than a pixel is still to be seen.
static float whole(const float units, const float scale)
{
    const float pixels = floorf(units * scale + 0.5f);
    return pixels < 1.0f ? 1.0f : pixels;
}

bool tide_font_glyph_for(const uint32_t codepoint, const int pixels, tide_font_glyph *out)
{
    start();
    if (glyph_count * 4u >= glyph_capacity * 3u) grow_glyphs();
    const uint32_t slot = slot_of(glyphs, glyph_capacity, codepoint, (uint16_t)pixels);
    if (glyphs[slot].pixels) {
        *out = glyphs[slot];
        return true;
    }
    tide_font_glyph g = {.codepoint = codepoint, .pixels = (uint16_t)pixels};
    const int index = stbtt_FindGlyphIndex(&font, (int)codepoint);
    const float scale = (float)pixels / em;
    g.advance = (int16_t)advance_pixels(codepoint, pixels);

    // The font has no hints of its own, and stb_truetype would read none: an
    // outline drawn where it falls has its edges between pixels, in grays,
    // which at the sizes text is read at is a blur. So each glyph is fitted to
    // the pixels it's drawn on: its box from its left edge to its right one is
    // a whole number of them, and so is its height above the baseline, with
    // the left edge and the baseline on pixel edges. Upright stems at a
    // glyph's sides and flat tops then fill their pixels. Letters as tall as
    // each other in the font stay so, as they round the same way.
    int left = 0, bottom = 0, right = 0, top = 0; // The outline's box, in font units, y up
    if (stbtt_GetGlyphBox(&font, index, &left, &bottom, &right, &top) && right > left && top > bottom) {
        // A hair under a whole pixel, so that what should fill one doesn't spill a sliver into the next
        const float across = (whole((float)(right - left), scale) - 1.0f / 32.0f) / (float)(right - left);
        const float up = top > 0 ? (whole((float)top, scale) - 1.0f / 32.0f) / (float)top : scale;
        // Its left edge a whole number of pixels from the pen, by moving the outline to it
        const float edge = floorf((float)left * scale + 0.5f);
        const float shift = edge - (float)left * across + 1.0f / 64.0f;
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetGlyphBitmapBoxSubpixel(&font, index, across, up, shift, 0.0f, &x0, &y0, &x1, &y1);
        const int width = x1 - x0, height = y1 - y0;
        if (width > 0 && height > 0) {
            if (!place(width, height, &g.x, &g.y)) {
                atlas_full = true;
                return false;
            }
            stbtt_MakeGlyphBitmapSubpixel(&font, atlas + (size_t)g.y * ATLAS_WIDTH + g.x, width, height, ATLAS_WIDTH, across,
                                          up, shift, 0.0f, index);
            g.width = (uint16_t)width;
            g.height = (uint16_t)height;
            g.left = (int16_t)x0;
            g.top = (int16_t)y0; // From the baseline: above it is less than 0
            atlas_version++;
        }
    }
    glyphs[slot] = g;
    glyph_count++;
    *out = g;
    return true;
}

const uint8_t *tide_font_atlas(int *width, int *height, uint32_t *version)
{
    *width = ATLAS_WIDTH;
    *height = atlas_height;
    *version = atlas_version;
    return atlas;
}

void tide_font_frame(void)
{
    if (!atlas_full) return;
    atlas_full = false;
    memset(glyphs, 0, glyph_capacity * sizeof(tide_font_glyph));
    glyph_count = 0;
    memset(atlas, 0, (size_t)atlas_height * ATLAS_WIDTH);
    row_x = row_y = 1;
    row_height = 0;
    atlas_version++;
}
