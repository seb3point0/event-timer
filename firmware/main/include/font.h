#ifndef ZT_FONT_H_
#define ZT_FONT_H_

#include <stdint.h>

// Bitmap font tables produced by tools/gen_fonts.py. Glyph rows are packed
// MSB-first, one bit per pixel, stride = (w + 7) / 8. A set bit is ink.
typedef struct {
    uint8_t w;         // bitmap width in pixels
    uint8_t h;         // bitmap height in pixels
    int16_t dx;        // left side bearing from the pen position
    int16_t dy;        // baseline to the top row of the bitmap, positive is up
    uint16_t advance;  // pen movement for this glyph
    uint32_t off;      // byte offset into the font's bitmap blob
} zt_glyph_t;

typedef struct {
    uint16_t ascent;
    uint16_t descent;
    uint8_t first;  // first ASCII code in the (contiguous) glyph table
    uint8_t last;
    const zt_glyph_t* glyphs;
    const uint8_t* bitmap;
} zt_font_t;

#endif  // ZT_FONT_H_
