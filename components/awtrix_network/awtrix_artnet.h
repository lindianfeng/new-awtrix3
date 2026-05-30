#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* ── Art-Net (DMX-over-Wi-Fi) receiver — port of src/ArtnetWifi.cpp ──
 * Listens on UDP/6454. When ARTNET_MODE is enabled, incoming DMX
 * universes 0..7 (×170 channels/universe) write directly to the LED
 * buffer (3 channels/LED). Disable via CONFIG.artnetMode = false.
 */

void awtrix_artnet_start(void);
void awtrix_artnet_stop(void);

#ifdef __cplusplus
}
#endif
