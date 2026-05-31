/**
 * led_matrix.c — bottom-of-stack WS2812 driver.
 *
 * Wraps espressif/led_strip's RMT backend into the small C API that the
 * Matrix C++ class needs. Everything above this file (Canvas, font
 * rasteriser, drawing primitives) goes through led_matrix_set_pixel /
 * led_matrix_show / led_matrix_set_brightness so the renderer doesn't
 * need to know about RMT channels or pixel order.
 *
 * Hardware:
 *   - 32 × 8 = 256 WS2812 pixels, single GPIO data line (MATRIX_DATA_PIN).
 *   - Physical layout is "serpentine": odd-numbered rows run right-to-left.
 *     led_matrix_xy_to_index() encodes that transform so callers can keep
 *     thinking in (x, y) screen coordinates.
 *   - RMT channel is grabbed once at init; show() blocks ~3 ms for a full
 *     frame, fits comfortably inside the 16 ms tick budget.
 *
 * Gamma / correction / temperature are computed in the Matrix C++ layer
 * (matrix_cpp.cpp::applyGamma / setCorrection / setTemperature) before
 * the pixels reach this file — we just push the final 24-bit values to
 * the LED strip.
 */

#include "led_matrix.h"
#include "led_strip.h"
#include <stdlib.h>
#include <string.h>
#include "math.h"
#include "esp_log.h"

#define TAG TAG_DISPLAY

/* ── WS2812 via espressif/led_strip ────────────────────────── */
static led_strip_handle_t s_strip = NULL;

static esp_err_t awtrix_ws2812_init(int pin, int num_leds) {
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = pin,
        .max_leds = num_leds,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
        .flags.with_dma = false,
    };
    return led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
}

/* ── Matrix implementation ──────────────────────────────────── */
led_matrix_t *awtrix_matrix_create(uint8_t w, uint8_t h, uint8_t pin, uint8_t layout) {
    int total = w * h;
    led_matrix_t *m = (led_matrix_t *)calloc(1, sizeof(led_matrix_t));
    if (!m) return NULL;
    m->framebuffer = (rgb_t *)calloc(total, sizeof(rgb_t));
    m->xy_lut = (int *)malloc(total * sizeof(int));
    if (!m->xy_lut) { free(m->framebuffer); free(m); return NULL; }
    m->width  = w;
    m->height = h;
    m->total  = total;
    m->pin    = pin;
    m->layout = layout;
    m->brightness = 128;
    m->power  = true;

    /* precompute xy → strip-index LUT once at creation time */
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            m->xy_lut[y * w + x] = awtrix_matrix_xy(m, x, y);
        }
    }

    awtrix_ws2812_init(pin, total);
    ESP_LOGI(TAG, "Matrix %dx%d pin=%d layout=%d LUT=%d", w, h, pin, layout, total);
    return m;
}

void awtrix_matrix_destroy(led_matrix_t *m) {
    if (!m) return;
    if (s_strip) { led_strip_del(s_strip); s_strip = NULL; }
    free(m->framebuffer);
    free(m->xy_lut);
    free(m);
}

void awtrix_matrix_clear(led_matrix_t *m) {
    if (!m) return;
    memset(m->framebuffer, 0, m->width * m->height * sizeof(rgb_t));
}

int awtrix_matrix_xy(const led_matrix_t *m, int x, int y) {
    int w = m->width, h = m->height;
    uint8_t layout = m->layout;

    bool vflip = (layout & NEO_MATRIX_BOTTOM) != 0;
    bool hflip = (layout & NEO_MATRIX_RIGHT)  != 0;
    bool cols  = (layout & NEO_MATRIX_COLUMNS) != 0;
    bool progr = (layout & NEO_MATRIX_PROGRESSIVE) != 0;
    bool zig   = (layout & NEO_MATRIX_ZIGZAG) != 0;

    if (vflip) y = h - 1 - y;
    if (hflip) x = w - 1 - x;

    if (cols) {
        /* Column-major (vertical strips) */
        if (progr) {
            return y * w + x;
        }
        if (zig && (y & 1)) {
            return (y + 1) * w - 1 - x;
        }
        return y * w + x;
    } else {
        /* Row-major (horizontal strips) – Ulanzi physical layout */
        if (progr) {
            /* 4×(8×8) progressive: each 8-row zone maps sequentially */
            int panel_h = 8;
            int panel_x = x;             /* x within panel */
            int panel   = y / panel_h;
            int py      = y % panel_h;
            return panel * (w * panel_h) + py * w + panel_x;
        }
        if (zig && (x & 1)) {
            return (x + 1) * h - 1 - y;
        }
        return x * h + y;
    }
}

void awtrix_matrix_draw_pixel(led_matrix_t *m, int x, int y, rgb_t color) {
    if (!m || x < 0 || x >= m->width || y < 0 || y >= m->height) return;
    m->framebuffer[y * m->width + x] = color;
}

rgb_t awtrix_matrix_get_pixel(const led_matrix_t *m, int x, int y) {
    if (!m || x < 0 || x >= m->width || y < 0 || y >= m->height) return (rgb_t){0,0,0};
    return m->framebuffer[y * m->width + x];
}

void awtrix_matrix_fill(led_matrix_t *m, rgb_t color) {
    if (!m) return;
    for (int i = 0; i < m->width * m->height; i++) m->framebuffer[i] = color;
}

void awtrix_matrix_show(led_matrix_t *m) {
    if (!m || !s_strip) return;
    int total = m->total;
    uint8_t bri = m->brightness;

    for (int i = 0; i < total; i++) {
        rgb_t col = m->framebuffer[i];
        int idx = m->xy_lut[i];
        if (idx >= 0 && idx < total) {
            led_strip_set_pixel(s_strip, idx,
                (col.r * bri) >> 8,
                (col.g * bri) >> 8,
                (col.b * bri) >> 8);
        }
    }
    led_strip_refresh(s_strip);
}

void awtrix_matrix_set_brightness(led_matrix_t *m, uint8_t bri) {
    if (m) m->brightness = bri;
}

void awtrix_matrix_draw_rgb_bitmap(led_matrix_t *m, int x, int y,
    const uint32_t *bitmap, int w, int h) {
    if (!m) return;
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            uint32_t c = bitmap[row * w + col];
            awtrix_matrix_draw_pixel(m, x + col, y + row, awtrix_rgb_from_u32(c));
        }
    }
}

void awtrix_matrix_set_rotation(led_matrix_t *m, uint8_t rot) {
    if (m) m->rotation = rot;
}

/* ── Color helpers ──────────────────────────────────────────── */
rgb_t awtrix_rgb_from_u32(uint32_t c) {
    return (rgb_t){ (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c };
}

uint32_t awtrix_rgb_to_u32(rgb_t c) {
    return ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | (uint32_t)c.b;
}

rgb_t awtrix_rgb_hsv_to_rgb(uint8_t h, uint8_t s, uint8_t v) {
    rgb_t out = {0,0,0};
    if (s == 0) { out.r = out.g = out.b = v; return out; }
    uint8_t region = h / 43;
    uint8_t rem    = (h - region * 43) * 6;
    uint8_t p = (v * (255 - s)) >> 8;
    uint8_t q = (v * (255 - ((s * rem) >> 8))) >> 8;
    uint8_t t = (v * (255 - ((s * (255 - rem)) >> 8))) >> 8;
    switch (region) {
        case 0: out.r=v; out.g=t; out.b=p; break;
        case 1: out.r=q; out.g=v; out.b=p; break;
        case 2: out.r=p; out.g=v; out.b=t; break;
        case 3: out.r=p; out.g=q; out.b=v; break;
        case 4: out.r=t; out.g=p; out.b=v; break;
        default:out.r=v; out.g=p; out.b=q; break;
    }
    return out;
}

rgb_t awtrix_rgb_gamma(rgb_t in, float gamma) {
    float inv = 1.0f / gamma;
    return (rgb_t){
        (uint8_t)(powf(in.r / 255.0f, inv) * 255.0f),
        (uint8_t)(powf(in.g / 255.0f, inv) * 255.0f),
        (uint8_t)(powf(in.b / 255.0f, inv) * 255.0f),
    };
}