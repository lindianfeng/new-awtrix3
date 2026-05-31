#pragma once

#include <stdint.h>
#include "matrix_cpp.h"

#ifdef __cplusplus

/* ── Color helpers (port of Functions.cpp from original AWTRIX3) ── */
CRGB kelvinToRGB(int kelvin);
uint32_t hsvToRgb(uint8_t h, uint8_t s, uint8_t v);
uint32_t hexToUint32(const char* hexString);

/* JSON color extraction — accepts hex string "#FF0000"/"FF0000", number,
 * 3-element [r,g,b] array, or 4-element ["HSV",h,s,v] array. */
struct cJSON;
uint32_t getColorFromJsonVariant(struct cJSON* v, uint32_t defaultColor);

double roundToDecimalPlaces(double value, int places);
float getTextWidth(const char* text, uint8_t textCase);

/* UTF-8 → ASCII (single-codepoint state machine) */
uint8_t utf8ascii(uint8_t ascii);
/* In-place UTF-8 → ASCII string conversion. Destination must be pre-allocated. */
void utf8ascii_str(const char* src, char* dst, size_t dstSize);

/* Text style helpers */
uint32_t TextEffect(uint32_t color, uint32_t fadeMs, uint32_t blinkMs);
uint32_t fadeColor(uint32_t color, uint32_t intervalMs);

#endif /* __cplusplus */
