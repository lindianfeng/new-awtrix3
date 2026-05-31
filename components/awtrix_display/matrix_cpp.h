#pragma once

#ifdef __cplusplus

#include <stdint.h>
#include <stddef.h>
#include "awtrix_hal.h"
#include "awtrix_color.h"   /* CRGB — extracted from this file so AwtrixConfig
                              and other lightweight clients don't need to pull
                              the entire Matrix / led_matrix dependency tree. */
#include "led_matrix.h"

/* ── Matrix – FastLED_NeoMatrix-compatible C++ wrapper ───────────
 * Owns the framebuffer (CRGB array) + the low-level led_matrix_t HAL.
 */
class Matrix {
public:
    Matrix(int w, int h, int panelsW, int panelsH, uint8_t layout);
    ~Matrix();

    /* display */
    void show();
    void clear();
    void setBrightness(uint8_t bri);
    uint8_t getBrightness() const;

    /* pixel access (bounds-checked) */
    void  drawPixel(int16_t x, int16_t y, uint32_t color);
    void  drawPixel(int16_t x, int16_t y, const CRGB &color);
    CRGB  getPixel(int16_t x, int16_t y) const;

    /* geometry helpers */
    void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint32_t color);
    void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint32_t color);
    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint32_t color);
    void drawCircle(int16_t x0, int16_t y0, int16_t r, uint32_t color);
    void fillCircle(int16_t x0, int16_t y0, int16_t r, uint32_t color);
    void drawFastVLine(int16_t x, int16_t y, int16_t h, uint32_t color);
    void drawFastHLine(int16_t x, int16_t y, int16_t w, uint32_t color);

    /* bitmap blit */
    void drawRGBBitmap(int16_t x, int16_t y, const uint16_t *bitmap, int16_t w, int16_t h);
    void drawRGBBitmap(int16_t x, int16_t y, const uint32_t *bitmap, int16_t w, int16_t h);

    /* text (uses AwtrixFont by default) */
    void setCursor(int16_t x, int16_t y);
    void setTextColor(uint32_t color);
    void setTextColor(const CRGB &color);
    void setFont(const uint8_t *(*fontData)());
    void print(const char *str);
    void print(char c);
    void print(double number, uint8_t digits);
    void print(int number);

    /* properties */
    int  width()  const { return m_width; }
    int  height() const { return m_height; }
    int  XY(int x, int y) const;
    void setRotation(int rot);
    void setLayout(int layout);

    /* raw framebuffer access */
    CRGB *getLeds() { return m_leds; }
    const CRGB *getLeds() const { return m_leds; }

    /* color helpers (static) */
    static uint32_t Color(uint8_t r, uint8_t g, uint8_t b) {
        return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
    static uint32_t Color(const CRGB &c) {
        return ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | (uint32_t)c.b;
    }

    /* gamma / correction wrappers (delegated to led_matrix HAL) */
    void applyGamma(float gamma);
    void setCorrection(const CRGB &corr);
    void setTemperature(const CRGB &temp);

private:
    void charBounds(unsigned char c, int16_t *x, int16_t *y,
                    int16_t *minx, int16_t *miny,
                    int16_t *maxx, int16_t *maxy);
    void drawChar(int16_t x, int16_t y, unsigned char c,
                  uint32_t color, uint8_t size);

    CRGB         *m_leds;
    led_matrix_t *m_hal;
    int           m_width, m_height;
    int           m_panelsW, m_panelsH;
    uint8_t       m_layout;
    int16_t       m_cursorX, m_cursorY;
    uint32_t      m_textColor;
    const uint8_t *(*m_getFont)();  /* function pointer returning font data */
    CRGB          m_correction;
    CRGB          m_temperature;
    bool          m_hasCorrection;
    bool          m_hasTemperature;
};

#endif /* __cplusplus */
