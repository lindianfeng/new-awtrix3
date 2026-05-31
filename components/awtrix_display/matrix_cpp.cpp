#include "matrix_cpp.h"
#include "awtrix_font.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define TAG TAG_DISPLAY
#define ABS(x) ((x) < 0 ? -(x) : (x))
#define SWAP(a, b) do { typeof(a) _t = a; a = b; b = _t; } while(0)

/* ── font index helpers ───────────────────────────────────────── */
static const uint8_t *default_font() { return AwtrixBitmaps; }

/* Glyph/width tables come from awtrix_font.h (defined in awtrix_font.cpp) */
#define FONT_FIRST 0x20
#define FONT_LAST  0xFF

/* ── Matrix constructor ──────────────────────────────────────── */
Matrix::Matrix(int w, int h, int panelsW, int panelsH, uint8_t layout)
    : m_width(w), m_height(h), m_panelsW(panelsW), m_panelsH(panelsH)
    , m_layout(layout), m_cursorX(0), m_cursorY(0)
    , m_textColor(0xFFFFFF), m_getFont(default_font)
    , m_correction(255,255,255), m_temperature(255,255,255)
    , m_hasCorrection(false), m_hasTemperature(false)
{
    m_leds = (CRGB *)calloc(w * h, sizeof(CRGB));
    /* create HAL; pin number comes from board_config */
    m_hal = awtrix_matrix_create((uint8_t)w, (uint8_t)h, MATRIX_PIN, layout);
}

Matrix::~Matrix() {
    if (m_leds) { free(m_leds); m_leds = nullptr; }
    if (m_hal)  { awtrix_matrix_destroy(m_hal); m_hal = nullptr; }
}

/* ── display ──────────────────────────────────────────────────── */
void Matrix::show() {
    if (!m_hal) return;
    /* copy CRGB framebuffer → HAL rgb_t framebuffer */
    int total = m_width * m_height;
    for (int i = 0; i < total; i++) {
        rgb_t col;
        if (m_hasCorrection) {
            col.r = ((int)m_leds[i].r * (int)m_correction.r) / 255;
            col.g = ((int)m_leds[i].g * (int)m_correction.g) / 255;
            col.b = ((int)m_leds[i].b * (int)m_correction.b) / 255;
        } else {
            col.r = m_leds[i].r;
            col.g = m_leds[i].g;
            col.b = m_leds[i].b;
        }
        m_hal->framebuffer[i] = col;
    }
    awtrix_matrix_show(m_hal);
}

void Matrix::clear() {
    memset(m_leds, 0, m_width * m_height * sizeof(CRGB));
}

void Matrix::setBrightness(uint8_t bri) {
    if (m_hal) awtrix_matrix_set_brightness(m_hal, bri);
}

uint8_t Matrix::getBrightness() const {
    return m_hal ? m_hal->brightness : 128;
}

void Matrix::setRotation(int rot) {
    if (m_hal) awtrix_matrix_set_rotation(m_hal, rot);
}

void Matrix::setLayout(int layout) {
    m_layout = layout;
    if (m_hal) { m_hal->layout = layout; }
}

int Matrix::XY(int x, int y) const {
    return m_hal ? awtrix_matrix_xy(m_hal, x, y) : (y * m_width + x);
}

/* ── pixel access ─────────────────────────────────────────────── */
void Matrix::drawPixel(int16_t x, int16_t y, uint32_t color) {
    if (x < 0 || x >= m_width || y < 0 || y >= m_height) return;
    m_leds[y * m_width + x] = color;
}

void Matrix::drawPixel(int16_t x, int16_t y, const CRGB &color) {
    if (x < 0 || x >= m_width || y < 0 || y >= m_height) return;
    m_leds[y * m_width + x] = color;
}

CRGB Matrix::getPixel(int16_t x, int16_t y) const {
    if (x < 0 || x >= m_width || y < 0 || y >= m_height) return CRGB(0,0,0);
    return m_leds[y * m_width + x];
}

/* ── geometry helpers ─────────────────────────────────────────── */

void Matrix::drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint32_t color) {
    int16_t steep = ABS(y1 - y0) > ABS(x1 - x0);
    if (steep) { SWAP(x0, y0); SWAP(x1, y1); }
    if (x0 > x1) { SWAP(x0, x1); SWAP(y0, y1); }
    int16_t dx = x1 - x0;
    int16_t dy = ABS(y1 - y0);
    int16_t err = dx / 2;
    int16_t ystep = (y0 < y1) ? 1 : -1;
    for (; x0 <= x1; x0++) {
        if (steep) drawPixel(y0, x0, color);
        else       drawPixel(x0, y0, color);
        err -= dy;
        if (err < 0) { y0 += ystep; err += dx; }
    }
}

void Matrix::drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint32_t color) {
    drawFastHLine(x, y, w, color);
    drawFastHLine(x, y + h - 1, w, color);
    drawFastVLine(x, y, h, color);
    drawFastVLine(x + w - 1, y, h, color);
}

void Matrix::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint32_t color) {
    for (int16_t row = y; row < y + h; row++)
        drawFastHLine(x, row, w, color);
}

void Matrix::drawCircle(int16_t x0, int16_t y0, int16_t r, uint32_t color) {
    int16_t f = 1 - r, ddF_x = 0, ddF_y = -2 * r, x = 0, y = r;
    drawPixel(x0, y0 + r, color);
    drawPixel(x0, y0 - r, color);
    drawPixel(x0 + r, y0, color);
    drawPixel(x0 - r, y0, color);
    while (x < y) {
        if (f >= 0) { y--; ddF_y += 2; f += ddF_y; }
        x++; ddF_x += 2; f += ddF_x + 1;
        drawPixel(x0 + x, y0 + y, color); drawPixel(x0 - x, y0 + y, color);
        drawPixel(x0 + x, y0 - y, color); drawPixel(x0 - x, y0 - y, color);
        drawPixel(x0 + y, y0 + x, color); drawPixel(x0 - y, y0 + x, color);
        drawPixel(x0 + y, y0 - x, color); drawPixel(x0 - y, y0 - x, color);
    }
}

void Matrix::fillCircle(int16_t x0, int16_t y0, int16_t r, uint32_t color) {
    drawFastVLine(x0, y0 - r, 2 * r + 1, color);
    int16_t f = 1 - r, ddF_x = 0, ddF_y = -2 * r, x = 0, y = r;
    while (x < y) {
        if (f >= 0) { y--; ddF_y += 2; f += ddF_y; }
        x++; ddF_x += 2; f += ddF_x + 1;
        drawFastVLine(x0 + x, y0 - y, 2 * y + 1, color);
        drawFastVLine(x0 - x, y0 - y, 2 * y + 1, color);
        drawFastVLine(x0 + y, y0 - x, 2 * x + 1, color);
        drawFastVLine(x0 - y, y0 - x, 2 * x + 1, color);
    }
}

void Matrix::drawFastVLine(int16_t x, int16_t y, int16_t h, uint32_t color) {
    for (int16_t i = 0; i < h; i++) drawPixel(x, y + i, color);
}

void Matrix::drawFastHLine(int16_t x, int16_t y, int16_t w, uint32_t color) {
    for (int16_t i = 0; i < w; i++) drawPixel(x + i, y, color);
}

/* ── bitmap blit ──────────────────────────────────────────────── */
void Matrix::drawRGBBitmap(int16_t x, int16_t y, const uint16_t *bitmap, int16_t w, int16_t h) {
    for (int16_t row = 0; row < h; row++) {
        for (int16_t col = 0; col < w; col++) {
            uint16_t c = bitmap[row * w + col];
            /* RGB565 → 888 */
            uint8_t r = ((c & 0xF800) >> 8) | ((c & 0xF800) >> 13);
            uint8_t g = ((c & 0x07E0) >> 3) | ((c & 0x07E0) >> 9);
            uint8_t b = ((c & 0x001F) << 3) | ((c & 0x001F) >> 2);
            drawPixel(x + col, y + row, Color(r, g, b));
        }
    }
}

void Matrix::drawRGBBitmap(int16_t x, int16_t y, const uint32_t *bitmap, int16_t w, int16_t h) {
    for (int16_t row = 0; row < h; row++) {
        for (int16_t col = 0; col < w; col++) {
            uint32_t c = bitmap[row * w + col];
            drawPixel(x + col, y + row, c);
        }
    }
}

/* ── text rendering (Awtrix 3x5 font) ─────────────────────────── */

/* default font data – declared extern, defined in awtrix_font.c */
/* FONT_FIRST / FONT_LAST already defined above */

void Matrix::setCursor(int16_t x, int16_t y) { m_cursorX = x; m_cursorY = y; }
void Matrix::setTextColor(uint32_t color)    { m_textColor = color; }
void Matrix::setTextColor(const CRGB &color) { m_textColor = (uint32_t)color; }
void Matrix::setFont(const uint8_t *(*fn)()) { if (fn) m_getFont = fn; }

void Matrix::charBounds(unsigned char c, int16_t *x, int16_t *y,
                        int16_t *minx, int16_t *miny,
                        int16_t *maxx, int16_t *maxy) {
    if (c < FONT_FIRST || c >= FONT_LAST) c = '?';
    /* charBounds only reports the glyph extent — width comes from the
     * width table and height is fixed at 5px. The glyph offset itself
     * is only needed by drawChar() when walking the bitmap. */
    uint8_t  w     = AwtrixWidths[c - FONT_FIRST];
    uint8_t  h     = 5; /* Awtrix font is 3-5px wide x 5px high */
    *minx = *x; *miny = *y;
    *maxx = *x + w - 1;
    *maxy = *y + h - 1;
}

void Matrix::drawChar(int16_t x, int16_t y, unsigned char c, uint32_t color, uint8_t size) {
    if (c < FONT_FIRST || c >= FONT_LAST) c = '?';
    uint16_t glyph = AwtrixGlyphs[c - FONT_FIRST];
    uint8_t  w     = AwtrixWidths[c - FONT_FIRST];
    const uint8_t *bitmap = m_getFont() + glyph;

    for (uint8_t row = 0; row < 5; row++) {   /* Awtrix is 5px tall */
        uint8_t bits = bitmap[row];
        for (uint8_t col = 0; col < w; col++) {
            if (bits & (0x80 >> col)) {
                if (size == 1) {
                    drawPixel(x + col, y + row, color);
                } else {
                    fillRect(x + col * size, y + row * size, size, size, color);
                }
            }
        }
    }
}

void Matrix::print(const char *str) {
    while (*str) {
        unsigned char c = *str++;
        if (c == '\n') { m_cursorY += 6; m_cursorX = 0; continue; }
        if (c == '\r') { m_cursorX = 0; continue; }
        uint8_t w = (c >= FONT_FIRST && c < FONT_LAST) ? AwtrixWidths[c - FONT_FIRST] : 3;
        drawChar(m_cursorX, m_cursorY, c, m_textColor, 1);
        m_cursorX += w + 1; /* 1px horizontal spacing */
        if (m_cursorX > m_width) { m_cursorX = 0; m_cursorY += 6; }
    }
}

void Matrix::print(char c) {
    uint8_t w = (c >= FONT_FIRST && c < FONT_LAST) ? AwtrixWidths[c - FONT_FIRST] : 3;
    drawChar(m_cursorX, m_cursorY, c, m_textColor, 1);
    m_cursorX += w + 1;
    if (m_cursorX > m_width) { m_cursorX = 0; m_cursorY += 6; }
}

void Matrix::print(double number, uint8_t digits) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%f", number);
    /* strip trailing zeros */
    char *p = buf + strlen(buf) - 1;
    while (p > buf && *p == '0' && digits < 2) { *p = '\0'; p--; }
    if (*p == '.') *p = '\0';
    print(buf);
}

void Matrix::print(int number) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", number);
    print(buf);
}

/* ── gamma / correction ──────────────────────────────────────── */
void Matrix::applyGamma(float gamma) {
    float inv = 1.0f / gamma;
    for (int i = 0; i < m_width * m_height; i++) {
        m_leds[i].r = (uint8_t)(powf(m_leds[i].r / 255.0f, inv) * 255.0f);
        m_leds[i].g = (uint8_t)(powf(m_leds[i].g / 255.0f, inv) * 255.0f);
        m_leds[i].b = (uint8_t)(powf(m_leds[i].b / 255.0f, inv) * 255.0f);
    }
}

void Matrix::setCorrection(const CRGB &corr) { m_correction = corr; m_hasCorrection = true; }
void Matrix::setTemperature(const CRGB &temp) { m_temperature = temp; m_hasTemperature = true; }
