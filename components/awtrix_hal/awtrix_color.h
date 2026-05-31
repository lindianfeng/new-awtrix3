/**
 * awtrix_color.h — shared 24-bit RGB pixel type.
 *
 * Extracted from matrix_cpp.h so that lightweight clients (e.g. AwtrixConfig
 * fields colorCorrection / colorTemperature) can declare CRGB-typed members
 * without having to pull in the entire Matrix / led_matrix / framebuffer
 * dependency tree from awtrix_display.
 *
 * Lives in awtrix_hal because it is the lowest, hardware-agnostic shared
 * value type used across config, display, render and the periphery layer.
 * Anything that wants a "color" without pulling Matrix should include this
 * header alone.
 *
 * Header-only on purpose: zero runtime cost, no static state, fully inlined.
 */

#pragma once

#ifdef __cplusplus

#include <stdint.h>

/* ── CRGB — FastLED-compatible 24-bit pixel type ─────────────────
 * The original AWTRIX firmware used FastLED's CRGB; we keep the same
 * memory layout, conversion operators, and 0xRRGGBB packed-uint32 form so
 * existing call sites (Matrix::setCorrection, config JSON loaders, …) are
 * binary-compatible without any code churn. */
struct CRGB
{
    uint8_t r, g, b;

    CRGB() : r(0), g(0), b(0)
    {
    }

    CRGB(uint8_t _r, uint8_t _g, uint8_t _b) : r(_r), g(_g), b(_b)
    {
    }

    CRGB(uint32_t hex) : r((hex >> 16) & 0xFF), g((hex >> 8) & 0xFF), b(hex & 0xFF)
    {
    }

    void setRGB(uint8_t _r, uint8_t _g, uint8_t _b)
    {
        r = _r;
        g = _g;
        b = _b;
    }

    operator uint32_t() const { return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b; }

    CRGB& operator=(uint32_t hex)
    {
        r = (hex >> 16) & 0xFF;
        g = (hex >> 8) & 0xFF;
        b = hex & 0xFF;
        return *this;
    }

    bool operator==(const CRGB& o) const { return r == o.r && g == o.g && b == o.b; }
    bool operator!=(const CRGB& o) const { return !(*this == o); }
};

/* ── A4: shared 0xRRGGBB constants ────────────────────────────────
 * Named by *usage*, not by literal RGB value, so that reading e.g.
 * `m.drawPixel(0, 0, AwtrixColors::kStatusWiFiDown)` tells the reader
 * what the pixel *means* rather than what shade of orange it happens
 * to be. Anything that needs a raw "pure red" still has kRed/kGreen/
 * kBlue available for ad-hoc test code.
 *
 * Defined as `constexpr uint32_t` so they compile to the exact same
 * immediate operand the old hex literals did (zero size/perf cost).
 *
 * The actual palette is split into three groups:
 *
 *  1. Primary colors      — for one-off / test paint.
 *  2. Indicator defaults  — match MatrixDisplayUi initial states.
 *  3. Semantic states     — battery levels, charge bolt, status
 *                            pixels, calendar header, lifeTimeEnd
 *                            red border.
 */
namespace AwtrixColors
{
    /* Primary colors. */
    constexpr uint32_t kBlack = 0x000000;
    constexpr uint32_t kWhite = 0xFFFFFF;
    constexpr uint32_t kRed = 0xFF0000;
    constexpr uint32_t kGreen = 0x00FF00;
    constexpr uint32_t kBlue = 0x0000FF;
    constexpr uint32_t kYellow = 0xFFFF00;
    constexpr uint32_t kMagenta = 0xFF00FF;
    constexpr uint32_t kCyan = 0x00FFFF;

    /* Default indicator colors (mirrors MatrixDisplayUi::m_ind{1,2,3}Color
 * defaults). Indicators 1=red, 2=green, 3=blue is a long-standing
 * AWTRIX convention; HA Discovery still publishes those names. */
    constexpr uint32_t kIndicator1Default = kRed;
    constexpr uint32_t kIndicator2Default = kGreen;
    constexpr uint32_t kIndicator3Default = kBlue;

    /* Battery & charging semantic colors used by BatApp.
 *  - kBatteryLow:    < 20 %  (red)
 *  - kBatteryMid:    < 50 %  (yellow)
 *  - kBatteryHigh:   >= 50 % (green)
 *  - kChargingBolt:  blinks above the battery body when charging. */
    constexpr uint32_t kBatteryLow = kRed;
    constexpr uint32_t kBatteryMid = kYellow;
    constexpr uint32_t kBatteryHigh = kGreen;
    constexpr uint32_t kChargingBolt = kGreen;

    /* Status pixel (top-right corner of every frame, drawn by
 * awtrix_status_overlay.cpp). */
    constexpr uint32_t kStatusOnline = kGreen; /* WiFi STA up + MQTT (if configured) up */
    constexpr uint32_t kStatusWiFiDown = 0xFF8800; /* orange — connected but no MQTT */

    /* customApp lifetime expiry red border (drawn by renderCustomApp when
 * lifeTimeEnd is true). Same hue as the battery-low warning. */
    constexpr uint32_t kLifetimeExpiredBorder = kRed;

    /* Calendar app accent colors (TimeApp mode 1). */
    constexpr uint32_t kCalendarHeaderDefault = kRed;
    constexpr uint32_t kCalendarBodyDefault = kWhite;

    /* Progress bar defaults for customApp + NotifyOverlay. */
    constexpr uint32_t kProgressFillDefault = kGreen;
    constexpr uint32_t kProgressTrackDefault = 0x202020; /* dim grey track */

    /* Pure-white default text color (matches CONFIG.textColor888 default). */
    constexpr uint32_t kTextDefault = kWhite;

    /* Calendar weekday strip — today vs other days. */
    constexpr uint32_t kWeekdayActiveDefault = kWhite;
    constexpr uint32_t kWeekdayInactiveDefault = 0x666666; /* 40% grey */
} // namespace AwtrixColors

#endif /* __cplusplus */
