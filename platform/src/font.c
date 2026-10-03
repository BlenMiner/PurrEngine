#include "font.h"

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
static float unit;           // Font units to text 1 tall: 1 over its ascent less its descent
static float ascent, line;   // For text 1 tall
static float advances[256];  // The first code points', which most text is

static float advance_of(const uint32_t codepoint)
{
    int advance = 0, bearing = 0;
    stbtt_GetCodepointHMetrics(&font, (int)codepoint, &advance, &bearing);
    return (float)advance * unit;
}

static void start(void)
{
    if (started) return;
    started = true;
    if (!stbtt_InitFont(&font, tide_font_ttf, stbtt_GetFontOffsetForIndex(tide_font_ttf, 0))) {
        fprintf(stderr, "tide: the platform's font isn't one\n");
        abort();
    }
    int up = 0, down = 0, gap = 0;
    stbtt_GetFontVMetrics(&font, &up, &down, &gap);
    unit = 1.0f / (float)(up - down);
    ascent = (float)up * unit;
    line = (float)(up - down + gap) * unit;
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

float tide_font_advance(const uint32_t codepoint)
{
    start();
    if (codepoint < 256u) return advances[codepoint]; // Nothing for control characters
    return advance_of(codepoint);
}

float tide_font_line(void)
{
    start();
    return line;
}

float tide_font_measure(const char *text, const float size)
{
    float widest = 0.0f, width = 0.0f;
    for (uint32_t c; (c = tide_utf8_next(&text)) != 0;) {
        if (c == '\n') {
            if (width > widest) widest = width;
            width = 0.0f;
        } else {
            width += tide_font_advance(c);
        }
    }
    return (width > widest ? width : widest) * size;
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
    const float scale = (float)pixels * unit;
    const int index = stbtt_FindGlyphIndex(&font, (int)codepoint);
    int left = 0, top = 0, right = 0, bottom = 0;
    stbtt_GetGlyphBitmapBox(&font, index, scale, scale, &left, &top, &right, &bottom);
    const int width = right - left, height = bottom - top;
    if (width > 0 && height > 0) {
        if (!place(width, height, &g.x, &g.y)) {
            atlas_full = true;
            return false;
        }
        stbtt_MakeGlyphBitmap(&font, atlas + (size_t)g.y * ATLAS_WIDTH + g.x, width, height, ATLAS_WIDTH, scale, scale, index);
        g.width = (uint16_t)width;
        g.height = (uint16_t)height;
        g.left = (int16_t)left;
        g.top = (int16_t)((int)(ascent * (float)pixels + 0.5f) + top); // The box is from the baseline
        atlas_version++;
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
