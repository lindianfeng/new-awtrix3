#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "awtrix_hal.h"

/* ── Initialization modes matching FastLED_NeoMatrix ────────── */
enum
{
    NEO_MATRIX_TOP = 0x00,
    NEO_MATRIX_BOTTOM = 0x01,
    NEO_MATRIX_LEFT = 0x00,
    NEO_MATRIX_RIGHT = 0x02,
    NEO_MATRIX_CORNER = 0x03,
    NEO_MATRIX_ROWS = 0x00,
    NEO_MATRIX_COLUMNS = 0x04,
    NEO_MATRIX_PROGRESSIVE = 0x08,
    NEO_MATRIX_ZIGZAG = 0x10,
};

/* ── Display surface types ──────────────────────────────────── */
typedef struct
{
    rgb_t* framebuffer; /* [width * height], row-major (y*width+x) */
    int* xy_lut; /* [total] precomputed framebuffer[y*w+x] → strip index */
    int total; /* width * height */
    uint8_t width;
    uint8_t height;
    uint8_t pin;
    uint8_t layout;
    uint8_t rotation; /* 0, 1=90°, 2=180°, 3=270° */
    uint8_t brightness; /* 0-255 */
    bool power;
} led_matrix_t;

/* ── Public API ──────────────────────────────────────────────── */
led_matrix_t* awtrix_matrix_create(uint8_t w, uint8_t h, uint8_t pin, uint8_t layout);
void awtrix_matrix_destroy(led_matrix_t* m);
void awtrix_matrix_show(led_matrix_t* m);
void awtrix_matrix_clear(led_matrix_t* m);
void awtrix_matrix_set_brightness(led_matrix_t* m, uint8_t bri);

/* Row-major coordinate → strip index */
int awtrix_matrix_xy(const led_matrix_t* m, int x, int y);

/* Drawing primitives (index-based) */
void awtrix_matrix_draw_pixel(led_matrix_t* m, int x, int y, rgb_t color);
rgb_t awtrix_matrix_get_pixel(const led_matrix_t* m, int x, int y);
void awtrix_matrix_fill(led_matrix_t* m, rgb_t color);
void awtrix_matrix_draw_rgb_bitmap(led_matrix_t* m, int x, int y, const uint32_t* bitmap, int w, int h);

/* Rotation helper */
void awtrix_matrix_set_rotation(led_matrix_t* m, uint8_t rot);

/* ── Color helpers ──────────────────────────────────────────── */
rgb_t awtrix_rgb_from_u32(uint32_t c);
uint32_t awtrix_rgb_to_u32(rgb_t c);
rgb_t awtrix_rgb_hsv_to_rgb(uint8_t h, uint8_t s, uint8_t v);
rgb_t awtrix_rgb_gamma(rgb_t in, float gamma);

#ifdef __cplusplus
}
#endif