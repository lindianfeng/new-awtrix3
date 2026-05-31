#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

/* Pack K + L: load an 8x8 icon from SPIFFS into a 64-pixel RGB565 buffer.
 *
 * Lookup order (mirrors the original AWTRIX3 icon-bank behaviour):
 *   /spiffs/ICONS/<name>.rgb565   ← Pack K: raw 128-byte 8x8 bitmap (fastest path)
 *   /spiffs/ICONS/<name>.jpg      ← Pack L: decoded via esp_new_jpeg
 *   /spiffs/ICONS/<name>          ← any of the above without extension
 *
 * Returns true and fills `out_rgb565[64]` on success. Returns false if no
 * matching file exists, the SPIFFS read fails, or the decoded image isn't
 * 8x8. Caller passes either a bare name ("alarm") or a name+extension
 * ("alarm.jpg") — the loader tries both.
 *
 * GIF support is deliberately omitted in this round (would require the
 * full LZW decoder from src/GifPlayer.h; tracked as future work). Users
 * who need animated icons should send a customApp draw-instructions
 * payload instead. */
bool awtrix_icon_load_rgb565(const char *name, uint16_t out_rgb565[64]);

#ifdef __cplusplus
}
#endif
