#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* ── DFPlayer Mini MP3 player (UART2 @ 9600 8N1)
 * Port of lib/DFMiniMp3-1.0.7-noChecksums and its usage in
 * src/PeripheryManager.cpp. We re-implement only the protocol bytes
 * (no library dependency) so it runs natively on ESP-IDF v6.
 *
 * Wiring: TX=GPIO 6, RX=GPIO 7  (Ulanzi TC001 default — change in
 * board header if your board differs).
 */

#include <stdint.h>
#include <stdbool.h>

bool awtrix_dfp_init(void);                /* uart driver + 1s warm-up */
void awtrix_dfp_set_volume(uint8_t vol);   /* 0..30 */
void awtrix_dfp_play_file_number(uint16_t n);
void awtrix_dfp_play_folder(uint8_t folder, uint8_t file);
void awtrix_dfp_stop(void);
bool awtrix_dfp_is_playing(void);

#ifdef __cplusplus
}
#endif
