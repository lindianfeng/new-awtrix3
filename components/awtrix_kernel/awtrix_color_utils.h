#pragma once

#ifdef __cplusplus

#include <stdint.h>
#include <string>
#include <vector>
#include <cctype>

/* awtrix_color_utils — header-only color math used by every renderer.
 *
 * Before this header existed, every component that drew pixels had its own
 * file-static copy of these four routines:
 *
 *   - hsv_to_rgb     (HSV→RGB888 fast path; 3 copies: notif_*, custom_*,
 *                     awtrix_functions::hsvToRgb)
 *   - lerp_color     (linear interpolation between two 0xRRGGBB; 2 copies)
 *   - apply_fx       (fade/blink multiplicative dim; 2 copies)
 *   - scale_chart    (auto-scale bar/line data into [0..barH]; 2 copies)
 *
 * The duplication was forced by the layering rule: `awtrix_display` cannot
 * include `awtrix_core/awtrix_functions.h`, and vice-versa. Promoting them
 * to `awtrix_kernel` (the leaf component everyone already depends on) lets
 * one definition serve all callers. Keeping them in a header-only form is
 * deliberate — they are tiny, branchy, and the compiler inlines them at
 * the call site, which is faster than the originally factored extern
 * functions ever were.
 *
 * Naming kept inside an `awtrix_color` namespace so existing helpers like
 * `awtrix_functions::hsvToRgb` can keep their old signature without
 * symbol-clashing (the old function becomes a one-line forwarder).
 */
namespace awtrix_color {

/* HSV → RGB888 fast path. h/s/v all uint8_t (0..255); s=0 returns greyscale. */
inline uint32_t hsv_to_rgb(uint8_t h, uint8_t s, uint8_t v) {
    if (s == 0) return ((uint32_t)v << 16) | ((uint32_t)v << 8) | v;
    uint8_t region = h / 43;
    uint8_t rem    = (uint8_t)((h - region * 43) * 6);
    uint8_t p = (uint8_t)((v * (255 - s)) >> 8);
    uint8_t q = (uint8_t)((v * (255 - ((s * rem) >> 8))) >> 8);
    uint8_t t = (uint8_t)((v * (255 - ((s * (255 - rem)) >> 8))) >> 8);
    uint8_t r, g, b;
    switch (region) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* Linear interpolation between two 0xRRGGBB colors at t∈[0..255]. */
inline uint32_t lerp_color(uint32_t a, uint32_t b, uint8_t t) {
    uint8_t ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    uint8_t br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    uint8_t rr = (uint8_t)(ar + (((int)br - ar) * t) / 255);
    uint8_t rg = (uint8_t)(ag + (((int)bg - ag) * t) / 255);
    uint8_t rb = (uint8_t)(ab + (((int)bb - ab) * t) / 255);
    return ((uint32_t)rr << 16) | ((uint32_t)rg << 8) | (uint32_t)rb;
}

/* fadeMs/blinkMs period multiplicative dim on `col`. blink takes
 * precedence when both are set:
 *   blinkMs > 0: full color on even half-period, black on odd.
 *   fadeMs  > 0: triangle wave 0..255..0 of brightness over `fadeMs` ms. */
inline uint32_t apply_fx(uint32_t col, int fadeMs, int blinkMs, long nowMs) {
    if (blinkMs > 0) {
        long phase = nowMs % (long)(blinkMs * 2);
        return phase < blinkMs ? col : 0u;
    }
    if (fadeMs > 0) {
        long phase = nowMs % (long)(fadeMs * 2);
        int  t     = (int)(phase < fadeMs ? phase : (fadeMs * 2 - phase));
        int  bri   = (255 * t) / fadeMs;
        uint8_t r = (uint8_t)((((col >> 16) & 0xFF) * bri) / 255);
        uint8_t g = (uint8_t)((((col >> 8)  & 0xFF) * bri) / 255);
        uint8_t b = (uint8_t)((((col)       & 0xFF) * bri) / 255);
        return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
    return col;
}

/* Apply textCase (0=as-is, 1=upper, 2=lower) into an output buffer.
 * Caller passes the output vector pre-cleared (we use `assign` then mutate
 * in place to avoid reallocations on the typical 32-byte matrix string). */
inline void apply_case(const std::string &in, int textCase, std::string &out) {
    out.assign(in);
    if (textCase == 1) {
        for (auto &c : out) c = (char)std::toupper((unsigned char)c);
    } else if (textCase == 2) {
        for (auto &c : out) c = (char)std::tolower((unsigned char)c);
    }
}

/* Bar/line auto-scale data into [0..barH]. autoscale=true normalises to
 * data range; autoscale=false treats values as percentages (0..100) and
 * maps proportionally. Mirrors the original src/Apps.cpp behaviour. */
inline void scale_chart(const int *src, int n, int barH, bool autoscale,
                        std::vector<int> &dst) {
    dst.clear();
    if (n <= 0) return;
    if (autoscale) {
        int mn = src[0], mx = src[0];
        for (int i = 1; i < n; ++i) { if (src[i] < mn) mn = src[i]; if (src[i] > mx) mx = src[i]; }
        int range = mx - mn;
        if (range == 0) range = 1;
        for (int i = 0; i < n; ++i) dst.push_back(((src[i] - mn) * barH) / range);
    } else {
        for (int i = 0; i < n; ++i) {
            int u = src[i];
            if (u < 0)   u = 0;
            if (u > 100) u = 100;
            dst.push_back((u * barH) / 100);
        }
    }
}

/* Vector overload of the above so the notification overlay (which holds
 * std::vector<int>) doesn't have to pass `.data(), .size()` every time. */
inline void scale_chart(const std::vector<int> &src, std::vector<int> &dst,
                        int barH, bool autoscale) {
    scale_chart(src.data(), (int)src.size(), barH, autoscale, dst);
}

} // namespace awtrix_color

#endif /* __cplusplus */
