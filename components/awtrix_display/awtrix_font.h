/**
 * Awtrix 3x5 Font – data extracted from original AWTRIX3 firmware.
 * License: 3-clause BSD (see original AwtrixFont.h comments).
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GFXglyph – compatible with Adafruit GFX font format */
typedef struct {
    uint16_t bitmapOffset;
    uint8_t  width;
    uint8_t  height;
    uint8_t  xAdvance;
    int8_t   xOffset;
    int8_t   yOffset;
} GFXglyph;

typedef struct {
    uint8_t  *bitmap;
    GFXglyph *glyph;
    uint8_t   first;
    uint8_t   last;
    uint8_t   yAdvance;
} GFXfont;

/* The raw bitmap data (from AwtrixBitmaps[] in the original header) */
extern const uint8_t AwtrixBitmaps[];

/* Glyph table (from AwtrixFontGlyphs[] in the original header) */
extern const GFXglyph AwtrixFontGlyphs[];

/* The assembled font */
extern const GFXfont AwtrixFont;

/* Per-character glyph bitmap-offset table (used by Matrix::drawChar) */
extern const uint16_t AwtrixGlyphs[];

/* Per-character bitmap pixel-width table (used by Matrix::drawChar) */
extern const uint8_t  AwtrixWidths[];

/* Convenience: returns glyph offset (bitmapOffset) for character c */
static inline int awtrix_glyph_offset(char c) {
    if (c < 0x20 || c >= 0xFF) c = '?';
    return (int)AwtrixFontGlyphs[c - 0x20].bitmapOffset;
}

/* Convenience: returns character width (xAdvance) for character c */
static inline int awtrix_char_width(char c) {
    if (c < 0x20 || c >= 0xFF) c = '?';
    return (int)AwtrixFontGlyphs[c - 0x20].xAdvance;
}

/* Convenience: returns bitmap width (actual pixel width) for character c */
static inline int awtrix_bitmap_width(char c) {
    if (c < 0x20 || c >= 0xFF) c = '?';
    return (int)AwtrixFontGlyphs[c - 0x20].width;
}

#ifdef __cplusplus
}
#endif