#ifndef ZT_CANVAS_H_
#define ZT_CANVAS_H_

#include <stddef.h>
#include <stdint.h>

#include "font.h"
#include "zectrix_epd.h"

// A 400x300 one-bit-per-pixel framebuffer in the exact layout the panel wants:
// row-major, MSB first, 1 = white and 0 = black.
class Canvas {
public:
    static constexpr int kWidth = ZECTRIX_EPD_PANEL_WIDTH;
    static constexpr int kHeight = ZECTRIX_EPD_PANEL_HEIGHT;
    static constexpr int kStride = kWidth / 8;

    // The panel only does true greyscale in a 4bpp full refresh, which then
    // forbids partial refresh until a 1bpp frame re-establishes the baseline.
    // A countdown needs partial refreshes, so grey here is a dither instead.
    enum class Gray {
        kLight,  // 25% ink
        kMid,    // 50% ink
    };

    void Clear(bool black = false);
    void Pixel(int x, int y, bool black);
    void FillRect(int x, int y, int w, int h, bool black);
    void FillGray(int x, int y, int w, int h, Gray level);
    void StrokeRect(int x, int y, int w, int h, int thickness, bool black);
    void InvertRect(int x, int y, int w, int h);

    int TextWidth(const zt_font_t& font, const char* text) const;
    // Draws with `x` as the pen origin and `baseline` as the text baseline.
    void Text(int x, int baseline, const zt_font_t& font, const char* text,
              bool black);
    void TextCentered(int center_x, int baseline, const zt_font_t& font,
                      const char* text, bool black);

    // Copies a sub-rectangle out as a tightly packed patch for partial refresh.
    // `out` must hold ((w + 7) / 8) * h bytes.
    void ExtractRect(int x, int y, int w, int h, uint8_t* out) const;

    uint8_t* data() { return pixels_; }
    const uint8_t* data() const { return pixels_; }
    static constexpr size_t size() { return ZECTRIX_EPD_1BPP_FRAME_BYTES; }

private:
    uint8_t pixels_[ZECTRIX_EPD_1BPP_FRAME_BYTES];
};

#endif  // ZT_CANVAS_H_
