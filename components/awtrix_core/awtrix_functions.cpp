/**
 * Port of original AWTRIX3 src/Functions.cpp. Uses ESP-IDF + cJSON instead
 * of Arduino + FastLED + ArduinoJson. The text-width table mirrors the
 * original CharMap (5x3 font + UTF-8 substitutes).
 */
#include "awtrix_functions.h"
#include "awtrix_globals.h"
#include "esp_timer.h"
#include <cJSON.h>
#include <cmath>
#include <cstring>
#include <cctype>

static uint64_t _now_ms() { return esp_timer_get_time() / 1000; }

/* ── Char-width table (ported from Functions.cpp::CharMap) ───── */
static int8_t s_charWidth[256] = {0};
static bool s_charWidthInited = false;

static void init_charwidth()
{
    if (s_charWidthInited) return;
    /* Default = 4 px wide for printable ASCII; punctuation = 2-3 px */
    for (int i = 0; i < 256; i++) s_charWidth[i] = 4;
    s_charWidth[' '] = 2;
    s_charWidth['!'] = 2;
    s_charWidth[0x27] = 2;
    s_charWidth['('] = 3;
    s_charWidth[')'] = 3;
    s_charWidth[','] = 3;
    s_charWidth['.'] = 2;
    s_charWidth[':'] = 2;
    s_charWidth[';'] = 3;
    s_charWidth['I'] = 2;
    s_charWidth['M'] = 6;
    s_charWidth['N'] = 5;
    s_charWidth['Q'] = 5;
    s_charWidth['W'] = 6;
    s_charWidth['i'] = 2;
    s_charWidth['l'] = 2;
    s_charWidth['|'] = 2;
    s_charWidthInited = true;
}

CRGB kelvinToRGB(int kelvin)
{
    float t = kelvin / 100.0f;
    float r, g, b;
    if (t <= 66)
    {
        r = 255;
        g = 99.4708025861f * logf(t) - 161.1195681661f;
    }
    else
    {
        r = 329.698727446f * powf(t - 60, -0.1332047592f);
        g = 288.1221695283f * powf(t - 60, -0.0755148492f);
    }
    if (t >= 66) b = 255;
    else if (t <= 19) b = 0;
    else b = 138.5177312231f * logf(t - 10) - 305.0447927307f;
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    return CRGB((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

uint32_t hsvToRgb(uint8_t h, uint8_t s, uint8_t v)
{
    if (s == 0) return ((uint32_t)v << 16) | ((uint32_t)v << 8) | v;
    uint8_t region = h / 43;
    uint8_t rem = (h - region * 43) * 6;
    uint8_t p = (v * (255 - s)) >> 8;
    uint8_t q = (v * (255 - ((s * rem) >> 8))) >> 8;
    uint8_t t = (v * (255 - ((s * (255 - rem)) >> 8))) >> 8;
    uint8_t R = 0, G = 0, B = 0;
    switch (region)
    {
    case 0: R = v;
        G = t;
        B = p;
        break;
    case 1: R = q;
        G = v;
        B = p;
        break;
    case 2: R = p;
        G = v;
        B = t;
        break;
    case 3: R = p;
        G = q;
        B = v;
        break;
    case 4: R = t;
        G = p;
        B = v;
        break;
    default: R = v;
        G = p;
        B = q;
        break;
    }
    return ((uint32_t)R << 16) | ((uint32_t)G << 8) | B;
}

uint32_t hexToUint32(const char* hexString)
{
    if (!hexString) return 0;
    if (*hexString == '#') hexString++;
    return (uint32_t)strtoul(hexString, nullptr, 16);
}

uint32_t getColorFromJsonVariant(cJSON* v, uint32_t defaultColor)
{
    if (!v) return defaultColor;
    if (cJSON_IsNumber(v)) return (uint32_t)v->valuedouble;
    if (cJSON_IsString(v) && v->valuestring) return hexToUint32(v->valuestring);
    if (cJSON_IsArray(v))
    {
        int sz = cJSON_GetArraySize(v);
        if (sz == 3)
        {
            int r = cJSON_GetArrayItem(v, 0)->valueint;
            int g = cJSON_GetArrayItem(v, 1)->valueint;
            int b = cJSON_GetArrayItem(v, 2)->valueint;
            return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
        if (sz == 4)
        {
            cJSON* tag = cJSON_GetArrayItem(v, 0);
            if (tag && cJSON_IsString(tag) && strcmp(tag->valuestring, "HSV") == 0)
            {
                int h = cJSON_GetArrayItem(v, 1)->valueint;
                int s = cJSON_GetArrayItem(v, 2)->valueint;
                int va = cJSON_GetArrayItem(v, 3)->valueint;
                return hsvToRgb((uint8_t)h, (uint8_t)s, (uint8_t)va);
            }
        }
    }
    return defaultColor;
}

double roundToDecimalPlaces(double value, int places)
{
    double factor = pow(10.0, places);
    return round(value * factor) / factor;
}

float getTextWidth(const char* text, uint8_t textCase)
{
    init_charwidth();
    if (!text) return 0;
    float w = 0;
    for (const char* c = text; *c; c++)
    {
        unsigned char cur = (unsigned char)*c;
        if ((AwtrixConfig::get().uppercase_letters && textCase == 0) || textCase == 1)
            cur = (unsigned char)toupper((int)cur);
        w += s_charWidth[cur];
    }
    return w;
}

/* UTF-8 → ASCII state machine (port of utf8ascii from original) */
static uint8_t s_utf8_c1 = 0;

uint8_t utf8ascii(uint8_t ascii)
{
    if (ascii < 128)
    {
        s_utf8_c1 = 0;
        return ascii;
    }
    uint8_t last = s_utf8_c1;
    s_utf8_c1 = ascii;
    switch (last)
    {
    case 0xC2: return ascii;
    case 0xC3:
        if (ascii == 0xB3) return 0x6F;
        if (ascii == 0x93) return 0x4F;
        return ascii | 0xC0;
    case 0xC4:
        if (ascii == 0x85) return 0x61;
        if (ascii == 0x84) return 0x41;
        if (ascii == 0x87) return 0x63;
        if (ascii == 0x86) return 0x43;
        if (ascii == 0x99) return 0x65;
        if (ascii == 0x98) return 0x45;
        break;
    case 0xC5:
        if (ascii == 0x82) return 0x6C;
        if (ascii == 0x81) return 0x4C;
        if (ascii == 0x84) return 0x6E;
        if (ascii == 0x83) return 0x4E;
        if (ascii == 0x9A) return 0x53;
        if (ascii == 0xBC) return 0x7A;
        if (ascii == 0xBB) return 0x5A;
        break;
    }
    return 0;
}

void utf8ascii_str(const char* src, char* dst, size_t dstSize)
{
    if (!src || !dst || dstSize == 0) return;
    size_t j = 0;
    s_utf8_c1 = 0;
    for (const unsigned char* p = (const unsigned char*)src; *p && j + 1 < dstSize; p++)
    {
        uint8_t out = utf8ascii(*p);
        if (out) dst[j++] = (char)out;
    }
    dst[j] = '\0';
}

uint32_t TextEffect(uint32_t color, uint32_t fadeMs, uint32_t blinkMs)
{
    if (blinkMs > 0)
    {
        uint32_t phase = (uint32_t)(_now_ms() / blinkMs);
        if (phase & 1) return 0;
    }
    if (fadeMs > 0) return fadeColor(color, fadeMs);
    return color;
}

uint32_t fadeColor(uint32_t color, uint32_t intervalMs)
{
    if (intervalMs == 0) return color;
    uint32_t phase = (uint32_t)(_now_ms() % (intervalMs * 2));
    float k = (phase < intervalMs)
                  ? (float)phase / (float)intervalMs
                  : 1.0f - (float)(phase - intervalMs) / (float)intervalMs;
    uint8_t r = (uint8_t)(((color >> 16) & 0xFF) * k);
    uint8_t g = (uint8_t)(((color >> 8) & 0xFF) * k);
    uint8_t b = (uint8_t)((color & 0xFF) * k);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}
