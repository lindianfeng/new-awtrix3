#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Art-Net (DMX-over-Wi-Fi) receiver — port of src/ArtnetWifi.cpp ──
 * Listens on UDP/6454. When ARTNET_MODE is enabled, incoming DMX
 * universes 0..7 (×170 channels/universe) update a latest-frame cache.
 * DisplayManager consumes that cache from the main/display task so Matrix
 * remains single-writer during normal runtime.
 */

void awtrix_artnet_start(void);
void awtrix_artnet_stop(void);
bool awtrix_artnet_take_frame(uint32_t *rgb888, int max_pixels, int *out_pixels);

#ifdef __cplusplus
}
#endif
