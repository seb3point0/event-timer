#include "canvas.h"

#include <cstring>

namespace {

inline bool InBounds(int x, int y) {
    return x >= 0 && y >= 0 && x < Canvas::kWidth && y < Canvas::kHeight;
}

}  // namespace

void Canvas::Clear(bool black) {
    std::memset(pixels_, black ? 0x00 : 0xFF, sizeof(pixels_));
}

void Canvas::Pixel(int x, int y, bool black) {
    if (!InBounds(x, y)) return;
    uint8_t& byte = pixels_[y * kStride + (x >> 3)];
    const uint8_t mask = static_cast<uint8_t>(0x80 >> (x & 7));
    if (black) {
        byte &= static_cast<uint8_t>(~mask);
    } else {
        byte |= mask;
    }
}

void Canvas::FillRect(int x, int y, int w, int h, bool black) {
    for (int row = y; row < y + h; ++row) {
        for (int col = x; col < x + w; ++col) Pixel(col, row, black);
    }
}

void Canvas::FillGray(int x, int y, int w, int h, Gray level) {
    for (int row = y; row < y + h; ++row) {
        for (int col = x; col < x + w; ++col) {
            const bool ink = level == Gray::kMid
                                 ? ((col ^ row) & 1) == 0
                                 : (col & 1) == 0 && (row & 1) == 0;
            if (ink) Pixel(col, row, true);
        }
    }
}

void Canvas::StrokeRect(int x, int y, int w, int h, int thickness, bool black) {
    FillRect(x, y, w, thickness, black);
    FillRect(x, y + h - thickness, w, thickness, black);
    FillRect(x, y, thickness, h, black);
    FillRect(x + w - thickness, y, thickness, h, black);
}

void Canvas::InvertRect(int x, int y, int w, int h) {
    for (int row = y; row < y + h; ++row) {
        if (row < 0 || row >= kHeight) continue;
        for (int col = x; col < x + w; ++col) {
            if (col < 0 || col >= kWidth) continue;
            pixels_[row * kStride + (col >> 3)] ^=
                static_cast<uint8_t>(0x80 >> (col & 7));
        }
    }
}

int Canvas::TextWidth(const zt_font_t& font, const char* text) const {
    int width = 0;
    for (const char* p = text; *p != '\0'; ++p) {
        const uint8_t code = static_cast<uint8_t>(*p);
        if (code < font.first || code > font.last) continue;
        width += font.glyphs[code - font.first].advance;
    }
    return width;
}

void Canvas::Text(int x, int baseline, const zt_font_t& font, const char* text,
                  bool black) {
    int pen = x;
    for (const char* p = text; *p != '\0'; ++p) {
        const uint8_t code = static_cast<uint8_t>(*p);
        if (code < font.first || code > font.last) continue;
        const zt_glyph_t& glyph = font.glyphs[code - font.first];
        const int stride = (glyph.w + 7) / 8;
        const uint8_t* rows = font.bitmap + glyph.off;

        for (int row = 0; row < glyph.h; ++row) {
            const int py = baseline - glyph.dy + row;
            for (int col = 0; col < glyph.w; ++col) {
                if (rows[row * stride + (col >> 3)] & (0x80 >> (col & 7))) {
                    Pixel(pen + glyph.dx + col, py, black);
                }
            }
        }
        pen += glyph.advance;
    }
}

void Canvas::TextCentered(int center_x, int baseline, const zt_font_t& font,
                          const char* text, bool black) {
    Text(center_x - TextWidth(font, text) / 2, baseline, font, text, black);
}

void Canvas::ExtractRect(int x, int y, int w, int h, uint8_t* out) const {
    const int stride = (w + 7) / 8;
    std::memset(out, 0, static_cast<size_t>(stride) * h);
    for (int row = 0; row < h; ++row) {
        for (int col = 0; col < w; ++col) {
            const int sx = x + col;
            const int sy = y + row;
            if (!InBounds(sx, sy)) continue;
            if (pixels_[sy * kStride + (sx >> 3)] & (0x80 >> (sx & 7))) {
                out[row * stride + (col >> 3)] |=
                    static_cast<uint8_t>(0x80 >> (col & 7));
            }
        }
    }
}
